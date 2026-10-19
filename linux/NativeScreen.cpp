#include "NativeScreen.hpp"
#include <QGuiApplication>
#include <QScreen>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#ifdef TD_HAVE_WAYLAND
#include <wayland-client.h>
#include <cstring>

namespace {
struct Output {
 wl_output*handle=nullptr;
 QString name;
 QSize pixels;
 int transform=WL_OUTPUT_TRANSFORM_NORMAL;
};
struct Outputs {std::vector<std::unique_ptr<Output>>values;};
void geometry(void*data,wl_output*,int32_t,int32_t,int32_t,int32_t,int32_t,const char*,const char*,int32_t transform){static_cast<Output*>(data)->transform=transform;}
void mode(void*data,wl_output*,uint32_t flags,int32_t width,int32_t height,int32_t){if(flags&WL_OUTPUT_MODE_CURRENT)static_cast<Output*>(data)->pixels=QSize(width,height);}
void done(void*,wl_output*){}
void scale(void*,wl_output*,int32_t){}
void name(void*data,wl_output*,const char*value){static_cast<Output*>(data)->name=QString::fromUtf8(value);}
void description(void*,wl_output*,const char*){}
const wl_output_listener outputListener{geometry,mode,done,scale,name,description};
void global(void*data,wl_registry*registry,uint32_t id,const char*interface,uint32_t version){
 if(std::strcmp(interface,wl_output_interface.name))return;
 auto output=std::make_unique<Output>();
 output->handle=static_cast<wl_output*>(wl_registry_bind(registry,id,&wl_output_interface,std::min(version,4u)));
 if(!output->handle)return;
 wl_output_add_listener(output->handle,&outputListener,output.get());
 static_cast<Outputs*>(data)->values.push_back(std::move(output));
}
void removed(void*,wl_registry*,uint32_t){}
const wl_registry_listener registryListener{global,removed};
QSize waylandPixels(QScreen*screen){
 // Separate connection: never add listeners or dispatch events on Qt's own
 // wl_output objects. The current output mode is in physical hardware pixels.
 wl_display*display=wl_display_connect(nullptr);if(!display)return {};
 Outputs outputs;wl_registry*registry=wl_display_get_registry(display);
 wl_registry_add_listener(registry,&registryListener,&outputs);
 bool ready=wl_display_roundtrip(display)>=0&&wl_display_roundtrip(display)>=0;
 QSize result;
 if(ready){
  Output*match=nullptr;
  for(auto&output:outputs.values)if(output->name==screen->name()){match=output.get();break;}
  if(!match&&outputs.values.size()==1)match=outputs.values.front().get();
  if(match&&match->pixels.isValid()){
   result=match->pixels;
   if(match->transform&1)result.transpose();
  }
 }
 for(auto&output:outputs.values)wl_output_destroy(output->handle);
 wl_registry_destroy(registry);wl_display_disconnect(display);
 return result;
}
}
#endif

QSize nativeScreenPixels(QScreen*screen){
 if(!screen)return {1920,1080};
#ifdef TD_HAVE_WAYLAND
 if(QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))){
  const QSize pixels=waylandPixels(screen);
  if(pixels.isValid()){
   fprintf(stderr,"[NativeScreen] source=wl_output screen=%s logical=%dx%d qt_dpr=%.4f physical=%dx%d\n",screen->name().toUtf8().constData(),screen->size().width(),screen->size().height(),screen->devicePixelRatio(),pixels.width(),pixels.height());
   return pixels;
  }
 }
#endif
 const QSize pixels(int(std::lround(screen->size().width()*screen->devicePixelRatio())),int(std::lround(screen->size().height()*screen->devicePixelRatio())));
 fprintf(stderr,"[NativeScreen] source=qt_fallback screen=%s logical=%dx%d qt_dpr=%.4f physical=%dx%d\n",screen->name().toUtf8().constData(),screen->size().width(),screen->size().height(),screen->devicePixelRatio(),pixels.width(),pixels.height());
 return pixels;
}
