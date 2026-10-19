#include "H264Decoder.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
std::string errorText(int result){char text[AV_ERROR_MAX_STRING_SIZE];av_strerror(result,text,sizeof(text));return text;}
}
struct H264Decoder::Impl {
 AVCodecContext*context=nullptr;AVBufferRef*device=nullptr;AVFrame*decoded=nullptr,*software=nullptr;AVPacket*packet=nullptr;
 bool hardware=false;std::string backend="FFmpeg software";
 static AVPixelFormat format(AVCodecContext*,const AVPixelFormat*formats){for(;*formats!=AV_PIX_FMT_NONE;formats++)if(*formats==AV_PIX_FMT_VAAPI)return *formats;return AV_PIX_FMT_NONE;}
 Impl(){
  try{open(true);decoded=av_frame_alloc();software=av_frame_alloc();packet=av_packet_alloc();if(!decoded||!software||!packet)throw std::bad_alloc();}
  catch(...){close();av_frame_free(&decoded);av_frame_free(&software);av_packet_free(&packet);throw;}
 }
 void close(){avcodec_free_context(&context);av_buffer_unref(&device);hardware=false;}
 void open(bool tryHardware){
  close();backend="FFmpeg software";
  if(tryHardware){
   std::vector<std::string>paths;if(const char*path=std::getenv("THUNDERDISPLAY_VAAPI_DEVICE"))paths.push_back(path);
   else {std::error_code ec;for(const auto&entry:std::filesystem::directory_iterator("/dev/dri",ec))if(entry.path().filename().string().rfind("renderD",0)==0)paths.push_back(entry.path().string());std::sort(paths.begin(),paths.end());}
   for(const auto&path:paths)if(av_hwdevice_ctx_create(&device,AV_HWDEVICE_TYPE_VAAPI,path.c_str(),nullptr,0)>=0){hardware=true;backend="VAAPI hardware ("+path+")";break;}
  }
  const AVCodec*codec=avcodec_find_decoder(AV_CODEC_ID_H264);if(!codec)throw std::runtime_error("FFmpeg H264 decoder unavailable");
  context=avcodec_alloc_context3(codec);if(!context)throw std::bad_alloc();context->max_pixels=9437184;context->thread_count=1;context->flags|=AV_CODEC_FLAG_LOW_DELAY;context->err_recognition=AV_EF_CAREFUL|AV_EF_EXPLODE;
  if(hardware){context->hw_device_ctx=av_buffer_ref(device);context->get_format=format;}
  const int result=avcodec_open2(context,codec,nullptr);if(result<0){if(hardware){open(false);return;}throw std::runtime_error("Open H264 decoder: "+errorText(result));}
  std::cerr<<"[H264Decoder] "<<backend<<"\n";
 }
 ~Impl(){av_packet_free(&packet);av_frame_free(&decoded);av_frame_free(&software);close();}
 bool failure(int status,const char*phase){
  std::cerr<<"[H264Decoder] "<<phase<<": "<<errorText(status)<<"; requesting IDR\n";
  if(hardware){std::cerr<<"[H264Decoder] hardware path failed; switching to software decode\n";open(false);}else avcodec_flush_buffers(context);
  return false;
 }
 bool decode(const std::vector<std::vector<uint8_t>>&units,YuvFrame&output){
  size_t bytes=0;for(const auto&u:units)bytes+=u.size();if(!bytes||bytes>16*1024*1024)return false;
  av_packet_unref(packet);int result=av_new_packet(packet,int(bytes));if(result<0)throw std::bad_alloc();size_t offset=0;for(const auto&u:units){memcpy(packet->data+offset,u.data(),u.size());offset+=u.size();}
  result=avcodec_send_packet(context,packet);if(result<0)return failure(result,"submit");
  av_frame_unref(decoded);result=avcodec_receive_frame(context,decoded);if(result<0)return failure(result,"receive");
  if((decoded->flags&AV_FRAME_FLAG_CORRUPT)||decoded->decode_error_flags)return failure(AVERROR_INVALIDDATA,"corrupt output");
  AVFrame*image=decoded;
  if(decoded->format==AV_PIX_FMT_VAAPI){av_frame_unref(software);result=av_hwframe_transfer_data(software,decoded,0);if(result<0)return failure(result,"surface download");image=software;}
  if(image->width<128||image->height<128||image->width>4096||image->height>4096||(image->width&1)||(image->height&1)||int64_t(image->width)*image->height>9437184)throw std::runtime_error("H264 output dimensions invalid");
  if(image->format!=AV_PIX_FMT_NV12&&image->format!=AV_PIX_FMT_YUV420P)throw std::runtime_error("Unsupported H264 pixel format");
  output.width=image->width;output.height=image->height;
  for(int p=0;p<3;p++)output.planes[p].resize(size_t(output.width>>bool(p))*(output.height>>bool(p)));
  for(int row=0;row<output.height;row++)memcpy(output.planes[0].data()+size_t(row)*output.width,image->data[0]+row*image->linesize[0],output.width);
  const int cw=output.width/2,ch=output.height/2;
  if(image->format==AV_PIX_FMT_NV12){for(int row=0;row<ch;row++){const auto*uv=image->data[1]+row*image->linesize[1];auto*u=output.planes[1].data()+size_t(row)*cw,*v=output.planes[2].data()+size_t(row)*cw;for(int x=0;x<cw;x++){u[x]=uv[x*2];v[x]=uv[x*2+1];}}}
  else for(int p=1;p<3;p++)for(int row=0;row<ch;row++)memcpy(output.planes[p].data()+size_t(row)*cw,image->data[p]+row*image->linesize[p],cw);
  return true;
 }
};
H264Decoder::H264Decoder():impl(std::make_unique<Impl>()){}
H264Decoder::~H264Decoder()=default;
bool H264Decoder::decode(const std::vector<std::vector<uint8_t>>&u,YuvFrame&f){return impl->decode(u,f);}
void H264Decoder::reset(){avcodec_flush_buffers(impl->context);}
const char*H264Decoder::name()const{return impl->backend.c_str();}
