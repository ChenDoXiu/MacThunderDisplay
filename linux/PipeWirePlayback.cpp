#include "PipeWirePlayback.hpp"
#include "Audio.hpp"
#include "Transport.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#ifdef TD_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>

struct PipeWirePlayback::Impl {
    static constexpr size_t Capacity = 32768, Stride = td::audio::Channels * sizeof(int16_t);
    static constexpr size_t BlendFrames = 64;
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::array<std::array<int16_t, 2>, Capacity> pcm{};
    std::array<uint64_t, Capacity> timestamps{};
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> discardBefore{0};
    std::atomic<uint64_t> underruns{0}, missingFrames{0};
    std::atomic<uint64_t> catchups{0}, skippedFrames{0}, startupUntil{0};
    std::atomic<uint64_t> catchupBefore{0}, catchupKept{0};
    std::atomic<unsigned> keepFrames{768}, guardFrames{256};
    std::atomic<bool> fadeIn{true};
    std::atomic<uint64_t> timingSeq{0}, captureNs{0}, presentationNs{0}, deviceNs{0};
    std::atomic<unsigned> periodFrames{128};
    std::atomic<bool> enabled{false};
    std::atomic<double> ratio{1};
    pw_thread_loop *loop = nullptr;
    pw_stream *stream = nullptr;
    pw_stream_events events{};
    bool loopStarted = false;
    std::array<int16_t, 2> lastSample{};
    std::array<std::array<int16_t, 2>, BlendFrames> blendFrom{};
    size_t blendAt = BlendFrames;
    bool recovering = false;
    const unsigned requestedPeriod;

    explicit Impl(unsigned frames) : requestedPeriod(frames == 64 ? 64 : 128) {
        periodFrames = requestedPeriod;
        pw_init(nullptr, nullptr);
        try {
            loop = pw_thread_loop_new("td-audio-pw", nullptr);
            if (!loop)
                throw std::runtime_error("cannot create PipeWire loop");
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = [](void *data, pw_stream_state, pw_stream_state, const char *) {
                pw_thread_loop_signal(static_cast<Impl *>(data)->loop, false);
            };
            events.process = [](void *data) {
                static_cast<Impl *>(data)->process();
            };
            const std::string latency = std::to_string(requestedPeriod) + "/48000";
            auto *properties = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
                "Game", PW_KEY_NODE_NAME, "thunderdisplay-audio", PW_KEY_APP_NAME, "ThunderDisplay",
                PW_KEY_NODE_LATENCY, latency.c_str(), PW_KEY_NODE_RATE, "1/48000", nullptr);
            stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "ThunderDisplay PCM",
                                          properties, &events, this);
            if (!stream)
                throw std::runtime_error("cannot create PipeWire playback stream");
            if (pw_thread_loop_start(loop) < 0)
                throw std::runtime_error("cannot start PipeWire loop");
            loopStarted = true;
            pw_thread_loop_lock(loop);
            uint8_t storage[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
            spa_audio_info_raw format{};
            format.format = SPA_AUDIO_FORMAT_S16_LE;
            format.rate = td::audio::Rate;
            format.channels = td::audio::Channels;
            format.position[0] = SPA_AUDIO_CHANNEL_FL;
            format.position[1] = SPA_AUDIO_CHANNEL_FR;
            const spa_pod *params[] = {
                spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
                static_cast<spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, 4),
                    SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
                    SPA_POD_CHOICE_RANGE_Int(4096, int(requestedPeriod * Stride), 32768),
                    SPA_PARAM_BUFFERS_stride, SPA_POD_Int(Stride)))};
            int result = pw_stream_connect(
                stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE),
                params, 2);
            timespec deadline{};
            pw_thread_loop_get_time(loop, &deadline, 2000000000);
            if (result >= 0) {
                for (;;) {
                    auto state = pw_stream_get_state(stream, nullptr);
                    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
                        break;
                    if (state == PW_STREAM_STATE_ERROR ||
                        pw_thread_loop_timed_wait_full(loop, &deadline) < 0) {
                        result = -1;
                        break;
                    }
                }
            }
            pw_thread_loop_unlock(loop);
            if (result < 0)
                throw std::runtime_error("PipeWire stream did not become ready");
        } catch (...) {
            close();
            throw;
        }
    }

    ~Impl() {
        close();
    }

    void close() {
        enabled = false;
        if (loopStarted)
            pw_thread_loop_lock(loop);
        if (stream) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (loopStarted) {
            pw_thread_loop_unlock(loop);
            pw_thread_loop_stop(loop);
            loopStarted = false;
        }
        if (loop) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
        }
        pw_deinit();
    }

    void process() {
        auto *buffer = pw_stream_dequeue_buffer(stream);
        if (!buffer)
            return;
        auto *raw = buffer->buffer;
        if (!raw->n_datas || !raw->datas[0].data || !raw->datas[0].chunk) {
            pw_stream_return_buffer(stream, buffer);
            return;
        }
        auto &data = raw->datas[0];
        size_t frames = data.maxsize / Stride;
        frames = std::min(frames,
                          buffer->requested ? size_t(buffer->requested) : size_t(requestedPeriod));
        periodFrames = unsigned(frames);
        auto *out = static_cast<int16_t *>(data.data);
        uint64_t previousRead = read.load(std::memory_order_relaxed);
        uint64_t r = std::max(previousRead, discardBefore.load(std::memory_order_acquire));
        uint64_t w = written.load(std::memory_order_acquire);
        const bool running = enabled.load(std::memory_order_relaxed);
        const uint64_t now = pw_stream_get_nsec(stream);
        const bool resetFade =
            fadeIn.exchange(false, std::memory_order_relaxed) || r > previousRead;
        if (resetFade) {
            blendFrom = {};
            blendAt = 0;
            lastSample = {};
        }
        // Only the consumer advances read. Trim in the first real output
        // callback as well, after asynchronous device activation has finished.
        // A generous steady-state guard leaves ordinary clock correction alone.
        const uint64_t keep = std::max<uint64_t>(keepFrames.load(), 2 * frames);
        const uint64_t guard = std::max<uint64_t>(
            guardFrames.load(), td::audio::Rate * (now < startupUntil.load() ? 4 : 20) / 1000);
        uint64_t skipped = 0;
        if (running && w - r > keep + guard) {
            if (resetFade || recovering)
                blendFrom = {};
            else
                for (size_t i = 0; i < BlendFrames; ++i)
                    blendFrom[i] = pcm[(r + i) % Capacity];
            skipped = w - r - keep;
            r += skipped;
            blendAt = 0;
            recovering = false;
        }
        size_t supplied = running ? std::min<uint64_t>(frames, w - r) : 0;
        pw_time time{};
        double delayNs = 0;
        bool timed = pw_stream_get_time_n(stream, &time, sizeof(time)) >= 0 && time.rate.denom;
        if (timed) {
            delayNs = double(time.buffered + time.queued) * 1e9 / td::audio::Rate +
                      double(time.delay) * 1e9 * time.rate.num / time.rate.denom -
                      double(int64_t(now) - time.now);
            delayNs = std::max(0.0, delayNs);
        }
        if (supplied && timestamps[r % Capacity] && timed) {
            timingSeq.fetch_add(1, std::memory_order_acq_rel);
            captureNs.store(timestamps[r % Capacity], std::memory_order_relaxed);
            presentationNs.store(now + uint64_t(delayNs), std::memory_order_relaxed);
            deviceNs.store(uint64_t(delayNs), std::memory_order_relaxed);
            timingSeq.fetch_add(1, std::memory_order_release);
        }
        for (size_t i = 0; i < supplied; ++i) {
            auto sample = pcm[(r + i) % Capacity];
            for (unsigned channel = 0; channel < 2; ++channel) {
                int value = sample[channel];
                if (blendAt < BlendFrames)
                    value = (int(blendFrom[blendAt][channel]) * int(BlendFrames - blendAt - 1) +
                             value * int(blendAt + 1)) /
                            int(BlendFrames);
                else if (recovering && i < 32)
                    value = value * int(i + 1) / 32;
                out[i * 2 + channel] = int16_t(value);
            }
            if (blendAt < BlendFrames)
                ++blendAt;
        }
        if (supplied) {
            lastSample = {out[(supplied - 1) * 2], out[(supplied - 1) * 2 + 1]};
            recovering = false;
        }
        if (supplied < frames) {
            for (size_t i = supplied; i < frames; ++i)
                for (unsigned channel = 0; channel < 2; ++channel)
                    out[i * 2 + channel] =
                        int16_t(int(lastSample[channel]) *
                                int(32 - std::min<size_t>(32, i - supplied + 1)) / 32);
            if (enabled.load(std::memory_order_relaxed)) {
                underruns.fetch_add(1, std::memory_order_relaxed);
                missingFrames.fetch_add(frames - supplied, std::memory_order_relaxed);
            }
            lastSample = {};
            recovering = true;
            blendAt = BlendFrames;
        }
        read.store(r + supplied, std::memory_order_release);
        if (skipped) {
            catchupBefore = skipped + keep;
            catchupKept = keep;
            skippedFrames.fetch_add(skipped, std::memory_order_relaxed);
            catchups.fetch_add(1, std::memory_order_release);
        }
        data.chunk->offset = 0;
        data.chunk->stride = Stride;
        data.chunk->size = uint32_t(frames * Stride);
        buffer->size = frames;
        // PipeWire's adaptive rate is output/input; our API follows SDL's
        // input-consumption ratio, so the correction is reciprocal.
        pw_stream_set_rate(stream, 1.0 / ratio.load(std::memory_order_relaxed));
        pw_stream_queue_buffer(stream, buffer);
    }
};

PipeWirePlayback::PipeWirePlayback(unsigned periodFrames)
    : impl(std::make_unique<Impl>(periodFrames)) {
}

PipeWirePlayback::~PipeWirePlayback() = default;

bool PipeWirePlayback::put(const uint8_t *pcm, size_t frames, uint64_t captureNs) {
    auto &s = *impl;
    uint64_t w = s.written.load(std::memory_order_relaxed);
    uint64_t r = s.read.load(std::memory_order_acquire);
    if (frames > Impl::Capacity || w - r + frames > Impl::Capacity)
        return false;
    for (size_t i = 0; i < frames; ++i) {
        size_t slot = (w + i) % Impl::Capacity;
        memcpy(s.pcm[slot].data(), pcm + i * Impl::Stride, Impl::Stride);
        s.timestamps[slot] = captureNs ? captureNs + i * 1000000000ULL / td::audio::Rate : 0;
    }
    s.written.store(w + frames, std::memory_order_release);
    return true;
}

int PipeWirePlayback::queuedBytes() const {
    auto &s = *impl;
    uint64_t w = s.written.load(std::memory_order_acquire);
    uint64_t r = std::max(s.read.load(std::memory_order_acquire),
                          s.discardBefore.load(std::memory_order_acquire));
    return int((w > r ? w - r : 0) * Impl::Stride);
}

void PipeWirePlayback::clear() {
    impl->discardBefore = impl->written.load(std::memory_order_acquire);
    impl->ratio = 1;
}

void PipeWirePlayback::pause() {
    impl->enabled = false;
    pw_thread_loop_lock(impl->loop);
    pw_stream_set_active(impl->stream, false);
    pw_stream_flush(impl->stream, false);
    pw_thread_loop_unlock(impl->loop);
}

bool PipeWirePlayback::resume() {
    impl->startupUntil = td::nowNs() + 2000000000ULL;
    impl->fadeIn = true;
    impl->enabled = true;
    pw_thread_loop_lock(impl->loop);
    int result = pw_stream_set_active(impl->stream, true);
    pw_thread_loop_unlock(impl->loop);
    if (result < 0)
        impl->enabled = false;
    return result >= 0;
}

void PipeWirePlayback::setRate(double ratio) {
    impl->ratio = ratio;
}

void PipeWirePlayback::setLatencyTarget(double milliseconds, unsigned packetFrames) {
    impl->keepFrames = unsigned(std::clamp(milliseconds, 1.0, 200.0) * td::audio::Rate / 1000);
    impl->guardFrames = 2 * std::max(packetFrames, impl->requestedPeriod);
}

PipeWirePlayback::Timing PipeWirePlayback::timing() const {
    auto &s = *impl;
    Timing out{};
    out.underruns = s.underruns.load();
    out.missingFrames = s.missingFrames.load();
    out.catchups = s.catchups.load();
    out.skippedFrames = s.skippedFrames.load();
    out.catchupBeforeMs = double(s.catchupBefore.load()) * 1000 / td::audio::Rate;
    out.catchupKeptMs = double(s.catchupKept.load()) * 1000 / td::audio::Rate;
    out.periodFrames = s.periodFrames.load();
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        auto before = s.timingSeq.load(std::memory_order_acquire);
        if (before & 1)
            continue;
        out.captureNs = s.captureNs.load(std::memory_order_relaxed);
        out.presentationNs = s.presentationNs.load(std::memory_order_relaxed);
        out.deviceMs = s.deviceNs.load(std::memory_order_relaxed) / 1e6;
        if (before == s.timingSeq.load(std::memory_order_acquire))
            return out;
    }
    out.captureNs = out.presentationNs = 0;
    return out;
}
#else
struct PipeWirePlayback::Impl {};

PipeWirePlayback::PipeWirePlayback(unsigned) {
    throw std::runtime_error("native PipeWire not built");
}

PipeWirePlayback::~PipeWirePlayback() = default;

bool PipeWirePlayback::put(const uint8_t *, size_t, uint64_t) {
    return false;
}

int PipeWirePlayback::queuedBytes() const {
    return 0;
}

void PipeWirePlayback::clear() {
}

void PipeWirePlayback::pause() {
}

bool PipeWirePlayback::resume() {
    return false;
}

void PipeWirePlayback::setRate(double) {
}

void PipeWirePlayback::setLatencyTarget(double, unsigned) {
}

PipeWirePlayback::Timing PipeWirePlayback::timing() const {
    return {};
}
#endif
