#pragma once
#include <string>
#include "KeyboardOptions.hpp"
struct DisplayRequest {
 int width=2240,height=1400,retina=1,fps=60,mbps=400,chroma=1,linkMbps=1000;
 double scale=1.4;
 bool h264=false;
};
int runReceiver(const std::string &host,bool fullscreen,const DisplayRequest &request,const KeyboardOptions &keyboard={},bool settingsStdin=false);
