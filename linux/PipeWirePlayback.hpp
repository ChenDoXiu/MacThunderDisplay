#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

class PipeWirePlayback {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    struct Timing {
        uint64_t captureNs = 0, presentationNs = 0;
        uint64_t underruns = 0, missingFrames = 0;
        double deviceMs = 0;
        unsigned periodFrames = 0;
    };

    explicit PipeWirePlayback(unsigned periodFrames = 128);
    ~PipeWirePlayback();
    bool put(const uint8_t *pcm, size_t frames, uint64_t captureNs);
    int queuedBytes() const;
    void clear();
    void pause();
    bool resume();
    void setRate(double ratio);
    Timing timing() const;
};
