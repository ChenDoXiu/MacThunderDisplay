#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct YuvFrame {
 int width=0,height=0;
 std::array<std::vector<uint8_t>,3>planes;
};
class H264Decoder {
 struct Impl;std::unique_ptr<Impl>impl;
public:
 H264Decoder();~H264Decoder();
 bool decode(const std::vector<std::vector<uint8_t>>&units,YuvFrame&image);
 void reset();
 const char*name()const;
};
