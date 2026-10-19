#pragma once
#include <CoreAudio/CoreAudioTypes.h>
#include <functional>
#include <memory>

// The Core Audio IOProc only copies into a preallocated SPSC queue. The sink
// runs on an independent worker and may convert PCM, acquire locks and send UDP.
class ProcessAudio {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    using Sink = std::function<void(const AudioBufferList &, const AudioStreamBasicDescription &,
                                    size_t, uint64_t)>;
    ProcessAudio();
    ~ProcessAudio();
    bool start(Sink sink, bool muteLocal = false, unsigned periodFrames = 128);
    unsigned periodFrames() const;
};
