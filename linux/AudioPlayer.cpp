#include "AudioPlayer.hpp"
#include "Audio.hpp"
#include "ClockSync.hpp"
#include "Transport.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>

struct AudioPlayer::Impl {
    using Packet = std::array<uint8_t, td::audio::PacketSize>;
    int socket = -1;
    sockaddr_in server;
    ClockSync &clock;
    SDL_AudioStream *stream = nullptr;
    std::thread thread;
    std::atomic<bool> active{true};
    std::atomic<bool> lowLatency{false};
    std::atomic<bool> playbackStarted{false};
    std::atomic<uint64_t> session{0}, packets{0}, lost{0}, underruns{0}, resets{0};
    std::atomic<double> queuedMs{0}, receiveMs{0};

    Impl(const sockaddr_in &peer, ClockSync &sync) : server(peer), clock(sync) {
        try {
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                throw std::runtime_error(SDL_GetError());
            SDL_AudioSpec input{SDL_AUDIO_S16LE, td::audio::Channels, td::audio::Rate};
            stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &input,
                [](void *context, SDL_AudioStream *, int additionalBytes, int) {
                    auto &self = *static_cast<Impl *>(context);
                    if (additionalBytes > 0 && self.playbackStarted.load(std::memory_order_relaxed))
                        self.underruns.fetch_add(1, std::memory_order_relaxed);
                },
                this);
            if (!stream)
                throw std::runtime_error(SDL_GetError());
            socket = td::bindUdp();
            int bytes = 128 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
            SDL_AudioSpec output{};
            int period = 0;
            SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &output, &period);
            std::cerr << "[Audio] PCM 48000Hz stereo S16LE packet_ms=2.5 backend="
                      << SDL_GetCurrentAudioDriver() << " device_rate=" << output.freq
                      << " device_period_frames=" << period << '\n';
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
        SDL_DestroyAudioStream(stream);
    }

    void run() {
        using namespace td::audio;
        pthread_setname_np(pthread_self(), "td-audio");
        std::map<uint64_t, Packet> pending;
        uint64_t currentSession = 0, next = 0, lastPacket = 0, gapAt = 0;
        uint64_t reportAt = td::nowNs(), lastPackets = 0;
        uint64_t lastCapture = 0, lastRateAt = 0;
        double playoutMs = 0;
        float playbackRatio = 1.f;
        bool playing = false;
        std::array<uint8_t, PayloadSize> silence{};
        auto reset = [&] {
            playbackStarted = false;
            SDL_PauseAudioStreamDevice(stream);
            SDL_ClearAudioStream(stream);
            pending.clear();
            next = gapAt = 0;
            lastCapture = lastRateAt = 0;
            playoutMs = 0;
            playbackRatio = 1.f;
            playing = false;
            SDL_SetAudioStreamFrequencyRatio(stream, 1.f);
            queuedMs = 0;
        };
        auto feed = [&](const uint8_t *pcm) {
            int queued = std::max(0, SDL_GetAudioStreamQueued(stream));
            if (queued > int(BytesPerSecond * 100 / 1000)) {
                SDL_ClearAudioStream(stream);
                queued = 0;
                resets++;
            }
            if (!SDL_PutAudioStreamData(stream, pcm, PayloadSize))
                return;
            queued += PayloadSize;
            queuedMs = double(queued) * 1000 / BytesPerSecond;
            const unsigned primeMs = lowLatency.load() ? 8 : 30;
            if (!playing && queued >= int(BytesPerSecond * primeMs / 1000)) {
                playbackStarted = true;
                playing = SDL_ResumeAudioStreamDevice(stream);
                if (!playing)
                    playbackStarted = false;
            }
        };
        while (active) {
            uint64_t wanted = session.load();
            if (wanted != currentSession) {
                reset();
                currentSession = wanted;
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
                    !decode(bytes.data(), size_t(n), header) || header.session != currentSession)
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
                Packet packet;
                memcpy(packet.data(), bytes.data(), PacketSize);
                pending.emplace(header.sequence, packet);
                packets++;
                if (pending.size() > 24) {
                    resets++;
                    auto newest = *pending.rbegin();
                    reset();
                    next = newest.first;
                    pending.emplace(std::move(newest));
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
                            feed(silence.data());
                    } else {
                        SDL_ClearAudioStream(stream);
                        resets++;
                    }
                    next = it->first;
                }
                feed(it->second.data() + HeaderSize);
                lastCapture = get(it->second.data() + 24);
                next = it->first + 1;
                pending.erase(it);
                gapAt = 0;
            }
            if (playing && lastCapture && now - lastRateAt >= 250000000) {
                auto estimate = clock.estimate(currentSession, now);
                if (estimate.valid) {
                    int queued = std::max(0, SDL_GetAudioStreamQueued(stream));
                    double age =
                        (int64_t(now) + estimate.serverMinusClient - int64_t(lastCapture)) / 1e6;
                    // A timestamped packet and the remaining PCM describe the same
                    // playback timeline, regardless of the 20 ms capture bursts.
                    double delay = age + double(queued) * 1000 / BytesPerSecond;
                    if (delay > 0 && delay < 250) {
                        playoutMs = playoutMs ? .8 * playoutMs + .2 * delay : delay;
                        // Correct clock drift and excess latency gently. Updating
                        // once per 250 ms avoids chasing each individual packet.
                        double targetMs = lowLatency.load() ? 16 : 55;
                        playbackRatio =
                            float(1 + std::clamp((playoutMs - targetMs) / 4000, -.005, .005));
                        SDL_SetAudioStreamFrequencyRatio(stream, playbackRatio);
                    }
                }
                lastRateAt = now;
            }
            if (lastPacket && now - lastPacket > 100000000) {
                reset();
                lastPacket = 0;
            }
            if (now - reportAt >= 1000000000) {
                uint64_t total = packets.load();
                if (total != lastPackets)
                    std::cerr << "[Audio] packets=" << total << " lost=" << lost.load()
                              << " underruns=" << underruns.load() << " resets=" << resets.load()
                              << " queued_ms=" << queuedMs.load()
                              << " capture_to_receive_ms=" << receiveMs.load()
                              << " software_playout_ms=" << playoutMs << " rate=" << playbackRatio
                              << '\n';
                lastPackets = total;
                reportAt = now;
            }
        }
        reset();
    }
};

AudioPlayer::AudioPlayer(const sockaddr_in &server, ClockSync &clock)
    : impl(std::make_unique<Impl>(server, clock)) {
}

AudioPlayer::~AudioPlayer() = default;

uint16_t AudioPlayer::port() const {
    return td::socketPort(impl->socket);
}

void AudioPlayer::setSession(uint64_t session, bool lowLatency) {
    impl->lowLatency = lowLatency;
    impl->session = session;
}

AudioPlayer::Statistics AudioPlayer::statistics() const {
    return {impl->packets.load(), impl->lost.load(),     impl->underruns.load(),
            impl->resets.load(),  impl->queuedMs.load(), impl->receiveMs.load()};
}
