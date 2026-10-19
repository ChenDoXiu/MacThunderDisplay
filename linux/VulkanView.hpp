#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "Cursor.hpp"
#include "H264Decoder.hpp"
class VulkanView {
 struct Impl;
 std::unique_ptr<Impl> impl;
public:
 struct Timings {
  double fence=0,feed=0,acquire=0,record=0,presentCPU=0,decodeGPU=0,presentGPU=0;
  bool gpuValid=false;
 };
 struct Completion {
  double decodeGPU=0,presentGPU=0;
  uint64_t hostNs=0,errorNs=0;
  bool decoded=false;
 };
 explicit VulkanView(SDL_Window *window);
 ~VulkanView();
 bool show(int width,int height,int chroma,const std::vector<std::vector<uint8_t>>&units);
 bool showYuv(const YuvFrame&image);
 void setCursor(const td::CursorState&cursor);
 bool redraw();
 void resize();
 const char *gpuName() const;
 const Timings& timings() const;
 bool pollCompletion(Completion& result,bool pollGPU=true);
 bool completionPending() const;
 void setStatsOverlay(const std::string& text);
};
