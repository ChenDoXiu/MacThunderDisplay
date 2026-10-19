#pragma once
#include "Audio.hpp"
#include <CoreMedia/CoreMedia.h>
#include <CoreAudio/CoreAudioTypes.h>

// Confined to the serial audio capture queue, independent of video encoding.
class AudioSender {
    std::array<uint8_t, td::audio::PacketSize> packet{};
    uint64_t session = 0, sequence = 0, samples = 0, packetTime = 0;
    uint64_t lastCallback = 0, reportAt = 0, sent = 0, dropped = 0, callbacks = 0;
    unsigned filled = 0, largestCallback = 0, packetFrames = td::audio::LegacyFrames;
    float peak = 0;

  public:
    void consume(CMSampleBufferRef sample, int socket, uint64_t sessionID, unsigned frames);
    void consume(const AudioBufferList &buffers, const AudioStreamBasicDescription &format,
                 size_t frames, uint64_t firstNs, int socket, uint64_t sessionID,
                 unsigned packetFrames);
};
