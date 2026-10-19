#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace td {
constexpr size_t HeaderSize=64, ChunkSize=1200;
constexpr uint16_t ControlPort=42420;
constexpr uint16_t FrameTimeValid=2; // captureNs is a fresh sender-clock timestamp.
constexpr uint16_t FrameCallbackTime=4; // Timestamp starts at the capture callback, not display presentation.
constexpr uint16_t FrameH264=8,FrameKeyframe=16;
uint64_t nowNs();
uint64_t random64();
void closeSocket(int fd);
int bindUdp(uint16_t port=0);
uint16_t socketPort(int fd);
void setSocketTimeout(int fd,int milliseconds);
struct Header {
 uint8_t type=1,chroma=1;
 uint16_t flags=0,width=0,height=0;
 uint64_t session=0,frame=0,captureNs=0;
 uint32_t epoch=1,unitSize=0,offset=0;
 uint16_t unit=0,units=0,fragment=0,fragments=0,payload=0;
};
std::array<uint8_t,HeaderSize> encodeHeader(const Header&h);
bool decodeHeader(const uint8_t*data,size_t size,Header&h);
}
