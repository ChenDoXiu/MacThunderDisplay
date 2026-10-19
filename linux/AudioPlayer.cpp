#include "AudioPlayer.hpp"
#include "Audio.hpp"
#include "ClockSync.hpp"
#include "Transport.hpp"
#include "PipeWirePlayback.hpp"
#include "UdpRealtime.hpp"
#include <cstdlib>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>

struct AudioPlayer::Impl {
    using Packet = std::array<uint8_t, td::audio::PacketSize>;
    int socket = -1;
    sockaddr_in server;
    ClockSync &clock;
    const int requestedBufferMs;
    SDL_AudioStream *stream = nullptr;
    std::unique_ptr<PipeWirePlayback> native;
    std::thread thread;
    std::atomic<bool> active{true};
    std::atomic<bool> lowLatency{false};
    std::atomic<bool> playbackStarted{false};
    std::atomic<unsigned> packetFrames{td::audio::LegacyFrames};
    std::atomic<uint64_t> session{0}, packets{0}, lost{0}, underruns{0}, resets{0};
    std::atomic<double> queuedMs{0}, receiveMs{0}, softwareMs{0}, estimatedMs{-1}, outputMs{0};

    Impl(const sockaddr_in &peer, ClockSync &sync, int bufferMs)
        : server(peer), clock(sync), requestedBufferMs(bufferMs) {
        try {
            const char *backend = std::getenv("THUNDERDISPLAY_AUDIO_BACKEND");
            if (!backend || strcmp(backend, "sdl")) {
                try {
                    unsigned period = bufferMs > 0 && bufferMs <= 8 ? 64 : 128;
                    native = std::make_unique<PipeWirePlayback>(period);
                    std::cerr << "[Audio] backend=pipewire-native requested_period_frames="
                              << period << " rt_process=1\n";
                } catch (const std::exception &error) {
                    std::cerr << "[Audio] native fallback: " << error.what() << '\n';
                }
            }
            if (!native) {
                SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
                if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                    throw std::runtime_error(SDL_GetError());
                SDL_AudioSpec input{SDL_AUDIO_S16LE, td::audio::Channels, td::audio::Rate};
                stream = SDL_OpenAudioDeviceStream(
                    SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &input,
                    [](void *context, SDL_AudioStream *, int additionalBytes, int) {
                        auto &self = *static_cast<Impl *>(context);
                        if (additionalBytes > 0 &&
                            self.playbackStarted.load(std::memory_order_relaxed))
                            self.underruns.fetch_add(1, std::memory_order_relaxed);
                    },
                    this);
                if (!stream)
                    throw std::runtime_error(SDL_GetError());
                SDL_AudioSpec output{};
                int period = 0;
                SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &output, &period);
                std::cerr << "[Audio] PCM 48000Hz stereo S16LE backend="
                          << SDL_GetCurrentAudioDriver() << " device_rate=" << output.freq
                          << " device_period_frames=" << period << '\n';
            }
            socket = td::bindUdp();
            int bytes = 128 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
            thread = std::thread([this] {
                run();
            });
        } catch (...) {
            td::closeSocket(socket);
            if (stream)
                SDL_DestroyAudioStream(stream);
            throw;
        }
    }

    ~Impl() {
        active = false;
        if (thread.joinable())
            thread.join();
        td::closeSocket(socket);
        if (stream)
            SDL_DestroyAudioStream(stream);
    }

    int queuedBytes() const {
        return native ? native->queuedBytes() : std::max(0, SDL_GetAudioStreamQueued(stream));
    }

    void pausePlayback() {
        if (native)
            native->pause();
        else
            SDL_PauseAudioStreamDevice(stream);
    }

    void clearPlayback() {
        if (native)
            native->clear();
        else
            SDL_ClearAudioStream(stream);
    }

    void setRate(float ratio) {
        if (native)
            native->setRate(ratio);
        else
            SDL_SetAudioStreamFrequencyRatio(stream, ratio);
    }

    bool resumePlayback() {
        return native ? native->resume() : SDL_ResumeAudioStreamDevice(stream);
    }

    uint64_t underrunCount() const {
        return native ? native->timing().underruns : underruns.load();
    }

    void run() {
        using namespace td::audio;
        pthread_setname_np(pthread_self(), "td-audio");
        td::enableReaderRealtime("AudioRealtime");
        // At most 25 packets are held before the overflow branch below runs.
        // Reserve once so the realtime receive thread never allocates per packet.
        std::vector<std::pair<uint64_t, Packet>> pending;
        pending.reserve(25);
        uint64_t currentSession = 0, next = 0, lastPacket = 0, gapAt = 0;
        uint64_t reportAt = td::nowNs(), lastPackets = 0;
        uint64_t lastCapture = 0, lastRateAt = 0;
        double playoutMs = 0;
        float playbackRatio = 1.f;
        bool playing = false;
        unsigned frames = LegacyFrames;
        size_t payloadBytes = frames * Channels * sizeof(int16_t);
        std::array<uint8_t, PayloadSize> silence{};
        auto reset = [&] {
            playbackStarted = false;
            pausePlayback();
            clearPlayback();
            pending.clear();
            next = gapAt = 0;
            lastCapture = lastRateAt = 0;
            playoutMs = 0;
            playbackRatio = 1.f;
            playing = false;
            setRate(1.f);
            queuedMs = softwareMs = outputMs = 0;
            estimatedMs = -1;
        };
        auto feed = [&](const uint8_t *pcm, uint64_t captureNs) {
            int queued = queuedBytes();
            if (queued > int(BytesPerSecond * std::max(100, requestedBufferMs + 50) / 1000)) {
                clearPlayback();
                queued = 0;
                resets++;
            }
            if (!(native ? native->put(pcm, frames, captureNs)
                         : SDL_PutAudioStreamData(stream, pcm, int(payloadBytes))))
                return;
            queued += int(payloadBytes);
            queuedMs = double(queued) * 1000 / BytesPerSecond;
            const unsigned primeMs =
                requestedBufferMs
                    ? std::min(requestedBufferMs, std::max(native ? 8 : 5, requestedBufferMs - 8))
                : lowLatency.load() ? 8
                                    : 30;
            if (!playing && queued >= int(BytesPerSecond * primeMs / 1000)) {
                playbackStarted = true;
                playing = resumePlayback();
                if (!playing)
                    playbackStarted = false;
            }
        };
        while (active) {
            uint64_t wanted = session.load();
            unsigned wantedFrames = packetFrames.load();
            if (wanted != currentSession || wantedFrames != frames) {
                reset();
                currentSession = wanted;
                frames = wantedFrames;
                payloadBytes = frames * Channels * sizeof(int16_t);
                lastPacket = 0;
            }
            pollfd event{socket, POLLIN, 0};
            poll(&event, 1, pending.empty() ? 20 : 2);
            for (unsigned i = 0; i < 64; ++i) {
                std::array<uint8_t, PacketSize + 1> bytes{};
                sockaddr_in peer{};
                socklen_t size = sizeof(peer);
                ssize_t n = recvfrom(socket, bytes.data(), bytes.size(), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr *>(&peer), &size);
                if (n < 0)
                    break;
                Header header;
                if (!currentSession || session.load() != currentSession ||
                    peer.sin_addr.s_addr != server.sin_addr.s_addr ||
                    !decode(bytes.data(), size_t(n), header) || header.session != currentSession ||
                    header.frames != frames)
                    continue;
                uint64_t now = td::nowNs();
                if (lastPacket && now - lastPacket > 100000000)
                    reset();
                lastPacket = now;
                if (!next)
                    next = header.sequence;
                if (header.sequence < next)
                    continue;
                auto estimate = clock.estimate(currentSession, now);
                if (estimate.valid) {
                    int64_t age =
                        int64_t(now) + estimate.serverMinusClient - int64_t(header.captureNs);
                    if (age >= 0 && age < 1000000000)
                        receiveMs = age / 1e6;
                    if (age > 80000000) {
                        resets++;
                        continue;
                    }
                }
                Packet packet{};
                memcpy(packet.data(), bytes.data(), size_t(n));
                auto at = std::lower_bound(pending.begin(), pending.end(), header.sequence,
                                           [](const auto &entry, uint64_t sequence) {
                                               return entry.first < sequence;
                                           });
                if (at == pending.end() || at->first != header.sequence)
                    pending.insert(at, {header.sequence, packet});
                packets++;
                if (pending.size() > 24) {
                    resets++;
                    auto newest = *pending.rbegin();
                    reset();
                    next = newest.first;
                    pending.emplace_back(std::move(newest));
                }
            }
            uint64_t now = td::nowNs();
            while (!pending.empty()) {
                auto it = pending.begin();
                if (it->first > next) {
                    if (!gapAt)
                        gapAt = now;
                    if (pending.size() < 3 && now - gapAt < 3000000)
                        break;
                    uint64_t missing = it->first - next;
                    lost += missing;
                    if (missing <= 8) {
                        for (uint64_t i = 0; i < missing; ++i)
                            feed(silence.data(), 0);
                    } else {
                        clearPlayback();
                        resets++;
                    }
                    next = it->first;
                }
                lastCapture = get(it->second.data() + 24);
                feed(it->second.data() + HeaderSize, lastCapture);
                next = it->first + 1;
                pending.erase(it);
                gapAt = 0;
            }
            if (playing && lastCapture && now - lastRateAt >= 250000000) {
                auto estimate = clock.estimate(currentSession, now);
                if (estimate.valid) {
                    int queued = queuedBytes();
                    double age =
                        (int64_t(now) + estimate.serverMinusClient - int64_t(lastCapture)) / 1e6;
                    // captureNs identifies the first sample of the newest packet.
                    // The queue includes that packet, so subtract its duration.
                    double delay =
                        age + double(queued) * 1000 / BytesPerSecond - double(frames) * 1000 / Rate;
                    if (delay > 0 && delay < 250) {
                        playoutMs = playoutMs ? .8 * playoutMs + .2 * delay : delay;
                        // Correct clock drift and excess latency gently. Updating
                        // once per 250 ms avoids chasing each individual packet.
                        double targetMs = requestedBufferMs   ? requestedBufferMs
                                          : lowLatency.load() ? 16
                                                              : 55;
                        playbackRatio =
                            float(1 + std::clamp((playoutMs - targetMs) / 4000, -.005, .005));
                        setRate(playbackRatio);
                        softwareMs = playoutMs;
                    }
                }
                lastRateAt = now;
            }
            if (native) {
                auto timing = native->timing();
                auto estimate = clock.estimate(currentSession, now);
                outputMs = timing.deviceMs;
                if (estimate.valid && timing.captureNs && timing.presentationNs &&
                    timing.presentationNs + 100000000 > now) {
                    double total = (int64_t(timing.presentationNs) + estimate.serverMinusClient -
                                    int64_t(timing.captureNs)) /
                                   1e6;
                    estimatedMs = total >= 0 && total < 1000 ? total : -1;
                } else {
                    estimatedMs = -1;
                }
            }
            if (lastPacket && now - lastPacket > 100000000) {
                reset();
                lastPacket = 0;
            }
            if (now - reportAt >= 1000000000) {
                uint64_t total = packets.load();
                if (total != lastPackets)
                    std::cerr << "[Audio] packets=" << total << " lost=" << lost.load()
                              << " packet_frames=" << frames << " underruns=" << underrunCount()
                              << " resets=" << resets.load() << " queued_ms=" << queuedMs.load()
                              << " capture_to_receive_ms=" << receiveMs.load()
                              << " software_playout_ms=" << playoutMs
                              << " output_ms=" << outputMs.load()
                              << " estimated_playout_ms=" << estimatedMs.load()
                              << " missing_frames=" << (native ? native->timing().missingFrames : 0)
                              << " period_frames=" << (native ? native->timing().periodFrames : 256)
                              << " rate=" << playbackRatio << '\n';
                lastPackets = total;
                reportAt = now;
            }
        }
        reset();
    }
};

AudioPlayer::AudioPlayer(const sockaddr_in &server, ClockSync &clock, int bufferMs)
    : impl(std::make_unique<Impl>(server, clock, bufferMs)) {
}

AudioPlayer::~AudioPlayer() = default;

uint16_t AudioPlayer::port() const {
    return td::socketPort(impl->socket);
}

void AudioPlayer::setSession(uint64_t session, bool lowLatency, unsigned packetFrames) {
    if (!td::audio::supportedFrames(packetFrames))
        return;
    impl->lowLatency = lowLatency;
    impl->packetFrames = packetFrames;
    impl->session = session;
}

AudioPlayer::Statistics AudioPlayer::statistics() const {
    return {impl->packets.load(),     impl->lost.load(),     impl->underrunCount(),
            impl->resets.load(),      impl->queuedMs.load(), impl->receiveMs.load(),
            impl->estimatedMs.load(), impl->outputMs.load(), impl->native != nullptr};
}
