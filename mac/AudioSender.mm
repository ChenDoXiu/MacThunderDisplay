#include "AudioSender.hpp"
#include "Transport.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/socket.h>

void AudioSender::consume(CMSampleBufferRef sample, int socket, uint64_t sessionID) {
    using namespace td::audio;
    if (!CMSampleBufferDataIsReady(sample))
        return;
    const auto *format = CMAudioFormatDescriptionGetStreamBasicDescription(
        CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mSampleRate != Rate ||
        format->mChannelsPerFrame != Channels)
        return;
    const bool floating = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if ((floating && format->mBitsPerChannel != 32) ||
        (!floating && (format->mBitsPerChannel != 16 ||
                       !(format->mFormatFlags & kAudioFormatFlagIsSignedInteger))) ||
        (format->mFormatFlags & kAudioFormatFlagIsBigEndian))
        return;

    struct {
        UInt32 count;
        AudioBuffer buffers[Channels];
    } storage{};

    auto *list = reinterpret_cast<AudioBufferList *>(&storage);
    CMBlockBufferRef retained = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, list, sizeof(storage), nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained) != noErr)
        return;
    const size_t count = CMSampleBufferGetNumSamples(sample);
    const unsigned bytes = format->mBitsPerChannel / 8;
    bool valid = list->mNumberBuffers == (planar ? Channels : 1);
    for (unsigned i = 0; valid && i < list->mNumberBuffers; ++i)
        valid = list->mBuffers[i].mData &&
                list->mBuffers[i].mDataByteSize >= count * bytes * (planar ? 1 : Channels);
    if (!valid) {
        if (retained)
            CFRelease(retained);
        return;
    }
    const uint64_t now = td::nowNs();
    uint64_t firstNs =
        now > count * 1000000000ULL / Rate ? now - count * 1000000000ULL / Rate : now;
    CMTime delta = CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
                                  CMSampleBufferGetPresentationTimeStamp(sample));
    double age = CMTimeGetSeconds(delta);
    if (std::isfinite(age) && age >= 0 && age < 1 && uint64_t(age * 1e9) < now)
        firstNs = now - uint64_t(age * 1e9);
    consume(*list, *format, count, firstNs, socket, sessionID);
    if (retained)
        CFRelease(retained);
}

void AudioSender::consume(const AudioBufferList &buffers, const AudioStreamBasicDescription &asbd,
                          size_t count, uint64_t firstNs, int socket, uint64_t sessionID) {
    using namespace td::audio;
    const auto *format = &asbd;
    const auto *list = &buffers;
    const bool floating = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    const uint64_t now = td::nowNs();
    if (session != sessionID || (lastCallback && now - lastCallback > 100000000)) {
        session = sessionID;
        filled = 0;
        sequence = samples = 0;
    }
    lastCallback = now;
    if (!reportAt)
        reportAt = now;
    callbacks++;
    largestCallback = std::max(largestCallback, unsigned(count));
    for (size_t i = 0; i < count; ++i) {
        if (!filled)
            packetTime = firstNs + i * 1000000000ULL / Rate;
        for (unsigned channel = 0; channel < Channels; ++channel) {
            const auto &buffer = list->mBuffers[planar ? channel : 0];
            size_t index = planar ? i : i * Channels + channel;
            int16_t value;
            if (floating) {
                float f = static_cast<const float *>(buffer.mData)[index];
                if (!std::isfinite(f))
                    f = 0;
                peak = std::max(peak, std::abs(f));
                value = int16_t(std::lrint(std::clamp(f, -1.f, 32767.f / 32768.f) * 32768.f));
            } else {
                value = static_cast<const int16_t *>(buffer.mData)[index];
                peak = std::max(peak, std::abs(float(value)) / 32768.f);
            }
            size_t offset = HeaderSize + (filled * Channels + channel) * sizeof(int16_t);
            packet[offset] = uint16_t(value) & 255;
            packet[offset + 1] = uint16_t(value) >> 8;
        }
        filled++;
        samples++;
        if (filled == Frames) {
            encode(packet.data(), {session, ++sequence, packetTime, samples - Frames});
            if (::send(socket, packet.data(), packet.size(), MSG_DONTWAIT) ==
                ssize_t(packet.size()))
                sent++;
            else
                dropped++;
            filled = 0;
        }
    }
    if (now - reportAt >= 1000000000) {
        fprintf(
            stderr,
            "[AudioSend] PCM 48000Hz stereo S16LE packet_ms=2.5 packets=%llu dropped=%llu callbacks=%llu largest_callback_frames=%u peak=%.3f\n",
            (unsigned long long)sent, (unsigned long long)dropped, (unsigned long long)callbacks,
            largestCallback, peak);
        sent = dropped = callbacks = 0;
        largestCallback = 0;
        peak = 0;
        reportAt = now;
    }
}
