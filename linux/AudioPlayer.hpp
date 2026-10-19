#pragma once
#include <cstdint>
#include <memory>
#include <netinet/in.h>

class ClockSync;

class AudioPlayer {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    struct Statistics {
        uint64_t packets = 0, lost = 0, underruns = 0, resets = 0;
        double queuedMs = 0, captureToReceiveMs = 0;
        double estimatedPlayoutMs = -1, outputMs = 0;
        bool native = false;
    };

    AudioPlayer(const sockaddr_in &server, ClockSync &clock, int bufferMs = 0);
    ~AudioPlayer();
    uint16_t port() const;
    void setSession(uint64_t session, bool lowLatency = false);
    Statistics statistics() const;
};
