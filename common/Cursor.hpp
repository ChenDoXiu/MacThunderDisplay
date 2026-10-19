#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace td {
constexpr unsigned MaxCursorSize=128;
struct CursorImage {
 uint64_t id=0;
 uint16_t width=0,height=0,hotX=0,hotY=0;
 // Top-down, premultiplied sRGB RGBA8; dimensions and hot spot use stream pixels.
 std::vector<uint8_t> rgba;
};
struct CursorState {
 std::shared_ptr<const CursorImage> image;
 float x=0.5f,y=0.5f;
 bool visible=false;
};
// Lossless RGBA runs: little-endian u16 count followed by one RGBA pixel.
// Fall back to raw pixels when runs would be larger.
inline std::vector<uint8_t> encodeCursor(const std::vector<uint8_t>&rgba,bool&rle){
 std::vector<uint8_t> out;
 for(size_t p=0;p<rgba.size();){size_t count=1;while(count<65535&&p+(count+1)*4<=rgba.size()&&std::equal(rgba.begin()+p,rgba.begin()+p+4,rgba.begin()+p+count*4))count++;
  out.push_back(uint8_t(count));out.push_back(uint8_t(count>>8));out.insert(out.end(),rgba.begin()+p,rgba.begin()+p+4);p+=count*4;
 }
 rle=out.size()<rgba.size();return rle?out:rgba;
}
inline bool decodeCursor(const std::vector<uint8_t>&data,bool rle,size_t pixels,std::vector<uint8_t>&out){
 if(!pixels||pixels>MaxCursorSize*MaxCursorSize)return false;
 if(!rle){if(data.size()!=pixels*4)return false;out=data;return true;}
 out.clear();out.reserve(pixels*4);
 for(size_t p=0;p<data.size();p+=6){if(data.size()-p<6)return false;unsigned count=unsigned(data[p])|(unsigned(data[p+1])<<8);if(!count||count>pixels-out.size()/4)return false;
  for(unsigned i=0;i<count;i++)out.insert(out.end(),data.begin()+p+2,data.begin()+p+6);
 }
 return out.size()==pixels*4;
}
}
