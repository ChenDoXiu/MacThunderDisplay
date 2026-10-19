#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace td::audio {
constexpr unsigned Rate = 48000, Channels = 2, Frames = 120;
constexpr size_t HeaderSize = 40, PayloadSize = Frames * Channels * sizeof(int16_t);
constexpr size_t PacketSize = HeaderSize + PayloadSize;
constexpr unsigned BytesPerSecond = Rate * Channels * sizeof(int16_t);

struct Header {
    uint64_t session = 0, sequence = 0, captureNs = 0, firstSample = 0;
};

inline void put(uint8_t *p, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        p[i] = uint8_t(value);
        value >>= 8;
    }
}

inline uint64_t get(const uint8_t *p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | p[i];
    return value;
}

// TDA1 fixes the payload format at 48 kHz, stereo, signed 16-bit little-endian.
inline void encode(uint8_t *p, const Header &h) {
    memcpy(p, "TDA1", 4);
    p[4] = 1;
    p[5] = Channels;
    p[6] = Frames >> 8;
    p[7] = Frames & 255;
    put(p + 8, h.session);
    put(p + 16, h.sequence);
    put(p + 24, h.captureNs);
    put(p + 32, h.firstSample);
}

inline bool decode(const uint8_t *p, size_t size, Header &h) {
    if (size != PacketSize || memcmp(p, "TDA1", 4) || p[4] != 1 || p[5] != Channels ||
        ((unsigned(p[6]) << 8) | p[7]) != Frames)
        return false;
    h = {get(p + 8), get(p + 16), get(p + 24), get(p + 32)};
    return h.session != 0 && h.sequence != 0;
}
}
