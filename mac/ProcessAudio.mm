#include "ProcessAudio.hpp"
#include "Audio.hpp"
#include "Transport.hpp"
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
AudioObjectPropertyAddress
property(AudioObjectPropertySelector selector,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
OSStatus readProperty(AudioObjectID object, AudioObjectPropertySelector selector, T &value,
                      AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    auto address = property(selector, scope);
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value);
}

void check(OSStatus result, const char *operation) {
    if (result != noErr)
        throw std::runtime_error(std::string(operation) + " status=" + std::to_string(result));
}
}

struct ProcessAudio::Impl {
    static constexpr size_t Capacity = 32, MaxFrames = 4096;

    struct Chunk {
        std::array<float, MaxFrames * 2> data{};
        uint64_t hostTime = 0, arrived = 0;
        UInt32 frames = 0, buffers = 0;
    };

    std::array<Chunk, Capacity> chunks{};
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> active{false};
    AudioObjectID tap = kAudioObjectUnknown, device = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    AudioStreamBasicDescription format{};
    unsigned actualPeriod = 0;
    semaphore_t ready = SEMAPHORE_NULL;
    mach_timebase_info_data_t timebase{};
    int64_t clockOffset = 0;
    std::thread worker;
    Sink sink;

    ~Impl() {
        stop();
    }

    uint64_t ns(uint64_t ticks) const {
        return uint64_t((__uint128_t(ticks) * timebase.numer) / timebase.denom);
    }

    static OSStatus capture(AudioObjectID, const AudioTimeStamp *now, const AudioBufferList *input,
                            const AudioTimeStamp *inputTime, AudioBufferList *output,
                            const AudioTimeStamp *, void *context) {
        auto &self = *static_cast<Impl *>(context);
        if (output)
            for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
                if (output->mBuffers[i].mData)
                    memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
        if (!self.active.load(std::memory_order_relaxed) || !input)
            return noErr;
        const bool planar = self.format.mFormatFlags & kAudioFormatFlagIsNonInterleaved;
        UInt32 buffers = planar ? 2 : 1;
        if (input->mNumberBuffers != buffers)
            return noErr;
        UInt32 frames = input->mBuffers[0].mDataByteSize / self.format.mBytesPerFrame;
        if (!frames || frames > MaxFrames)
            return noErr;
        uint64_t w = self.written.load(std::memory_order_relaxed);
        if (w - self.read.load(std::memory_order_acquire) >= Capacity) {
            self.dropped.fetch_add(1, std::memory_order_relaxed);
            return noErr;
        }
        auto &chunk = self.chunks[w % Capacity];
        for (UInt32 i = 0; i < buffers; ++i) {
            size_t bytes = frames * self.format.mBytesPerFrame;
            if (!input->mBuffers[i].mData || input->mBuffers[i].mDataByteSize < bytes)
                return noErr;
            memcpy(chunk.data.data() + i * MaxFrames, input->mBuffers[i].mData, bytes);
        }
        chunk.frames = frames;
        chunk.buffers = buffers;
        chunk.arrived = mach_absolute_time();
        chunk.hostTime = inputTime && (inputTime->mFlags & kAudioTimeStampHostTimeValid)
                             ? inputTime->mHostTime
                             : 0;
        if (!chunk.hostTime && now && (now->mFlags & kAudioTimeStampHostTimeValid))
            chunk.hostTime = now->mHostTime;
        self.written.store(w + 1, std::memory_order_release);
        semaphore_signal(self.ready);
        return noErr;
    }

    bool start(Sink consumer, bool muteLocal, unsigned requestedPeriod) {
        if (@available(macOS 14.2, *)) {
            try {
                sink = std::move(consumer);
                mach_timebase_info(&timebase);
                clockOffset = int64_t(td::nowNs()) - int64_t(ns(mach_absolute_time()));
                check(semaphore_create(mach_task_self(), &ready, SYNC_POLICY_FIFO, 0),
                      "create audio semaphore");
                pid_t pid = getpid();
                AudioObjectID ownProcess = kAudioObjectUnknown;
                auto address = property(kAudioHardwarePropertyTranslatePIDToProcessObject);
                UInt32 size = sizeof(ownProcess);
                AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(pid), &pid,
                                           &size, &ownProcess);
                NSArray *excluded = ownProcess ? @[ @(ownProcess) ] : @[];
                CATapDescription *description =
                    [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded];
                description.name = @"ThunderDisplay Audio";
                description.privateTap = YES;
                description.muteBehavior = muteLocal ? CATapMutedWhenTapped : CATapUnmuted;
                check(AudioHardwareCreateProcessTap(description, &tap), "create process tap");
                check(readProperty(tap, kAudioTapPropertyFormat, format), "read tap format");
                if (format.mFormatID != kAudioFormatLinearPCM ||
                    format.mSampleRate != td::audio::Rate || format.mChannelsPerFrame != 2 ||
                    format.mBitsPerChannel != 32 ||
                    !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
                    (format.mFormatFlags & kAudioFormatFlagIsBigEndian) ||
                    format.mBytesPerFrame !=
                        ((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 4 : 8))
                    throw std::runtime_error("unsupported process tap PCM format");
                NSDictionary *aggregate = @{
                    @kAudioAggregateDeviceNameKey : @"ThunderDisplay Audio Capture",
                    @kAudioAggregateDeviceUIDKey : NSUUID.UUID.UUIDString,
                    @kAudioAggregateDeviceIsPrivateKey : @YES,
                    @kAudioAggregateDeviceTapAutoStartKey : @YES,
                    @kAudioAggregateDeviceTapListKey : @[ @{
                        @kAudioSubTapUIDKey : description.UUID.UUIDString,
                        @kAudioSubTapDriftCompensationKey : @YES
                    } ]
                };
                check(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate,
                                                         &device),
                      "create tap aggregate");
                UInt32 alive = 0;
                for (unsigned attempt = 0; attempt < 50; ++attempt) {
                    if (readProperty(device, kAudioDevicePropertyDeviceIsAlive, alive) == noErr &&
                        alive)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!alive)
                    throw std::runtime_error("tap aggregate is not alive");
                UInt32 frames = requestedPeriod == td::audio::LowLatencyFrames
                                    ? td::audio::LowLatencyFrames
                                    : td::audio::Frames;
                const UInt32 requested = frames;
                address = property(kAudioDevicePropertyBufferFrameSize);
                OSStatus bufferResult = AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                                                   sizeof(frames), &frames);
                UInt32 configured = 0;
                OSStatus configuredResult =
                    readProperty(device, kAudioDevicePropertyBufferFrameSize, configured);
                if (requested == td::audio::LowLatencyFrames &&
                    (bufferResult != noErr || configuredResult != noErr ||
                     configured != requested)) {
                    frames = td::audio::Frames;
                    OSStatus fallbackResult = AudioObjectSetPropertyData(
                        device, &address, 0, nullptr, sizeof(frames), &frames);
                    fprintf(stderr,
                            "[ProcessAudio] 64-frame period unavailable; retrying 128 status=%d\n",
                            int(fallbackResult));
                }
                check(AudioDeviceCreateIOProcID(device, capture, this, &proc), "create tap IOProc");
                active = true;
                worker = std::thread([this] {
                    run();
                });
                check(AudioDeviceStart(device, proc), "start tap IOProc");
                frames = 0;
                OSStatus queryResult =
                    readProperty(device, kAudioDevicePropertyBufferFrameSize, frames);
                actualPeriod = queryResult == noErr ? frames : 0;
                fprintf(
                    stderr,
                    "[ProcessAudio] started rate=%.0f channels=2 requested_period_frames=%u period_frames=%u period_ms=%.3f buffer_request_status=%d buffer_query_status=%d mute_local=%d\n",
                    format.mSampleRate, unsigned(requested), actualPeriod,
                    actualPeriod * 1000.0 / format.mSampleRate, int(bufferResult), int(queryResult),
                    muteLocal);
                return true;
            } catch (const std::exception &error) {
                fprintf(stderr, "[ProcessAudio] fallback=ScreenCaptureKit reason=%s\n",
                        error.what());
                stop();
            }
        }
        return false;
    }

    void run() {
        pthread_setname_np("td-audio-send");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        uint64_t reportAt = td::nowNs(), callbacks = 0;
        double maxWaitMs = 0, ageTotal = 0;
        unsigned minFrames = MaxFrames, maxFrames = 0;
        while (active) {
            kern_return_t result;
            do {
                result = semaphore_wait(ready);
            } while (result == KERN_ABORTED && active);
            if (!active)
                break;
            uint64_t r = read.load(std::memory_order_relaxed);
            uint64_t w = written.load(std::memory_order_acquire);
            if (r == w)
                continue;
            auto &chunk = chunks[r % Capacity];
            uint64_t now = td::nowNs();
            uint64_t arrived = uint64_t(int64_t(ns(chunk.arrived)) + clockOffset);
            uint64_t first = chunk.hostTime
                                 ? uint64_t(int64_t(ns(chunk.hostTime)) + clockOffset)
                                 : arrived - chunk.frames * 1000000000ULL / td::audio::Rate;
            if (now >= arrived && now - arrived <= 30000000) {
                struct {
                    UInt32 count;
                    AudioBuffer buffers[2];
                } storage{};

                auto *list = reinterpret_cast<AudioBufferList *>(&storage);
                list->mNumberBuffers = chunk.buffers;
                for (UInt32 i = 0; i < chunk.buffers; ++i)
                    list->mBuffers[i] = {chunk.buffers == 2 ? 1U : 2U,
                                         chunk.frames * format.mBytesPerFrame,
                                         chunk.data.data() + i * MaxFrames};
                sink(*list, format, chunk.frames, first);
                maxWaitMs = std::max(maxWaitMs, (now - arrived) / 1e6);
                ageTotal += (int64_t(now) - int64_t(first)) / 1e6;
                callbacks++;
                minFrames = std::min(minFrames, unsigned(chunk.frames));
                maxFrames = std::max(maxFrames, unsigned(chunk.frames));
            } else {
                dropped++;
            }
            read.store(r + 1, std::memory_order_release);
            if (now - reportAt >= 1000000000) {
                fprintf(
                    stderr,
                    "[ProcessAudio] callbacks=%llu ring_drop_total=%llu worker_wait_max_ms=%.3f capture_age_avg_ms=%.3f callback_frames_min=%u callback_frames_max=%u\n",
                    (unsigned long long)callbacks, (unsigned long long)dropped.load(), maxWaitMs,
                    callbacks ? ageTotal / callbacks : 0, callbacks ? minFrames : 0, maxFrames);
                reportAt = now;
                callbacks = 0;
                maxWaitMs = ageTotal = 0;
                minFrames = MaxFrames;
                maxFrames = 0;
            }
        }
    }

    void stop() {
        active = false;
        actualPeriod = 0;
        if (proc && device) {
            AudioDeviceStop(device, proc);
            AudioDeviceDestroyIOProcID(device, proc);
            proc = nullptr;
        }
        if (ready != SEMAPHORE_NULL)
            semaphore_signal(ready);
        if (worker.joinable())
            worker.join();
        if (device) {
            AudioHardwareDestroyAggregateDevice(device);
            device = 0;
        }
        if (tap) {
            if (@available(macOS 14.2, *))
                AudioHardwareDestroyProcessTap(tap);
            tap = 0;
        }
        if (ready != SEMAPHORE_NULL) {
            semaphore_destroy(mach_task_self(), ready);
            ready = SEMAPHORE_NULL;
        }
    }
};

ProcessAudio::ProcessAudio() : impl(std::make_unique<Impl>()) {
}

ProcessAudio::~ProcessAudio() = default;

bool ProcessAudio::start(Sink sink, bool muteLocal, unsigned periodFrames) {
    return impl->start(std::move(sink), muteLocal, periodFrames);
}

unsigned ProcessAudio::periodFrames() const {
    return impl->actualPeriod;
}
