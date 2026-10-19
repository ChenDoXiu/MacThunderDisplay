#import "H264Encoder.hpp"
#import <VideoToolbox/VideoToolbox.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <Foundation/Foundation.h>
#include "Transport.hpp"
#include <stdexcept>
#include <string>

namespace {
void check(OSStatus status,const char*action){if(status)throw std::runtime_error(std::string(action)+": "+std::to_string(status));}
struct Pending {
 dispatch_semaphore_t done=dispatch_semaphore_create(0);
 OSStatus status=noErr;H264Encoder::Frame frame;
};
void collect(Pending&p,OSStatus status,VTEncodeInfoFlags flags,CMSampleBufferRef sample){
 p.status=status;
 if(status||!sample||(flags&kVTEncodeInfo_FrameDropped)){if(!status)p.status=kVTVideoEncoderMalfunctionErr;return;}
 auto attrs=CMSampleBufferGetSampleAttachmentsArray(sample,false);
 CFBooleanRef notSync=(attrs&&CFArrayGetCount(attrs))?(CFBooleanRef)CFDictionaryGetValue((CFDictionaryRef)CFArrayGetValueAtIndex(attrs,0),kCMSampleAttachmentKey_NotSync):nullptr;
 p.frame.key=!notSync||!CFBooleanGetValue(notSync);
 auto format=CMSampleBufferGetFormatDescription(sample);int lengthBytes=0;size_t count=0;
 check(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format,0,nullptr,nullptr,&count,&lengthBytes),"H264 parameter sets");
 auto append=[&](const uint8_t*data,size_t size){p.frame.bytes.insert(p.frame.bytes.end(),{0,0,0,1});p.frame.bytes.insert(p.frame.bytes.end(),data,data+size);};
 if(p.frame.key)for(size_t i=0;i<count;i++){const uint8_t*data=nullptr;size_t size=0;check(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format,i,&data,&size,nullptr,nullptr),"H264 SPS/PPS");append(data,size);}
 auto block=CMSampleBufferGetDataBuffer(sample);if(!block||lengthBytes<1||lengthBytes>4)throw std::runtime_error("Invalid H264 sample");
 const size_t size=CMBlockBufferGetDataLength(block);if(size>16*1024*1024)throw std::runtime_error("H264 frame exceeds 16 MiB");
 std::vector<uint8_t>bytes(size);check(CMBlockBufferCopyDataBytes(block,0,size,bytes.data()),"H264 sample copy");
 size_t offset=0;while(offset+lengthBytes<=size){uint32_t n=0;for(int i=0;i<lengthBytes;i++)n=(n<<8)|bytes[offset++];if(!n||n>size-offset)throw std::runtime_error("Invalid H264 NAL length");append(bytes.data()+offset,n);offset+=n;}
 if(offset!=size||p.frame.bytes.empty())throw std::runtime_error("Incomplete H264 sample");
}
}

struct H264Encoder::Impl {
 id<MTLDevice>device;id<MTLCommandQueue>queue;MPSImageLanczosScale*scale;
 CVPixelBufferRef scaled=nullptr;VTCompressionSessionRef session=nullptr;
 int width,height,fps,mbps;uint64_t index=0;size_t sourceWidth=0,sourceHeight=0;
 Impl(id<MTLDevice>d,int w,int h,int f,int m):device(d),width(w),height(h),fps(f),mbps(m){queue=[d newCommandQueue];create();}
 void create(){
  NSDictionary*spec=@{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES,(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl:@YES};
  NSDictionary*attributes=@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES};
  check(VTCompressionSessionCreate(kCFAllocatorDefault,width,height,kCMVideoCodecType_H264,(__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attributes,nullptr,nullptr,nullptr,&session),"Create hardware H264 encoder");
  auto set=[&](CFStringRef key,id value){check(VTSessionSetProperty(session,key,(__bridge CFTypeRef)value),[(__bridge NSString*)key UTF8String]);};
  try{
   set(kVTCompressionPropertyKey_RealTime,@YES);set(kVTCompressionPropertyKey_AllowFrameReordering,@NO);
   set(kVTCompressionPropertyKey_ExpectedFrameRate,@(fps));set(kVTCompressionPropertyKey_AverageBitRate,@(int64_t(mbps)*1000000));
   set(kVTCompressionPropertyKey_ProfileLevel,(__bridge id)kVTProfileLevel_H264_High_AutoLevel);
   set(kVTCompressionPropertyKey_ColorPrimaries,(__bridge id)kCVImageBufferColorPrimaries_ITU_R_709_2);
   set(kVTCompressionPropertyKey_TransferFunction,(__bridge id)kCVImageBufferTransferFunction_ITU_R_709_2);
   set(kVTCompressionPropertyKey_YCbCrMatrix,(__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_709_2);
   check(VTCompressionSessionPrepareToEncodeFrames(session),"Prepare hardware H264");
  }catch(...){VTCompressionSessionInvalidate(session);CFRelease(session);session=nullptr;throw;}
  fprintf(stderr,"[H264Encoder] VideoToolbox hardware required, low latency, no B frames, %dx%d @ %d, target=%d Mbps\n",width,height,fps,mbps);
 }
 ~Impl(){if(session){VTCompressionSessionInvalidate(session);CFRelease(session);}if(scaled)CVPixelBufferRelease(scaled);}
 CVPixelBufferRef resize(CVPixelBufferRef source){
  const size_t w=CVPixelBufferGetWidth(source),h=CVPixelBufferGetHeight(source);const bool resizing=w!=size_t(width)||h!=size_t(height);
  if(w!=sourceWidth||h!=sourceHeight){sourceWidth=w;sourceHeight=h;fprintf(stderr,"[Scaler] %zux%zu -> %dx%d filter=%s codec=H264\n",w,h,width,height,resizing?"Metal Lanczos":"native (no resize)");}
  if(!resizing)return source;
  if(!scaled){NSDictionary*a=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES};check(CVPixelBufferCreate(kCFAllocatorDefault,width,height,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)a,&scaled),"H264 scaled buffer");scale=[[MPSImageLanczosScale alloc]initWithDevice:device];scale.edgeMode=MPSImageEdgeModeClamp;}
  auto texture=[&](CVPixelBufferRef pixel,MTLTextureUsage usage){MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:CVPixelBufferGetWidth(pixel) height:CVPixelBufferGetHeight(pixel) mipmapped:NO];d.usage=usage;d.storageMode=MTLStorageModeShared;return [device newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(pixel) plane:0];};
  id<MTLTexture>src=texture(source,MTLTextureUsageShaderRead),dst=texture(scaled,MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead);if(!src||!dst)throw std::runtime_error("Cannot map H264 scaling surfaces");
  id<MTLCommandBuffer>command=[queue commandBuffer];[scale encodeToCommandBuffer:command sourceTexture:src destinationTexture:dst];[command commit];[command waitUntilCompleted];if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("H264 Lanczos failed");return scaled;
 }
 Frame encode(CVPixelBufferRef source,bool force){
  const uint64_t began=td::nowNs();source=resize(source);const uint64_t resized=td::nowNs();
  if(!session){create();force=true;}
  auto pending=std::make_shared<Pending>();NSDictionary*options=(force||index%uint64_t(fps*2)==0)?@{(id)kVTEncodeFrameOptionKey_ForceKeyFrame:@YES}:nil;
  check(VTCompressionSessionEncodeFrameWithOutputHandler(session,source,CMTimeMake(index++,fps),CMTimeMake(1,fps),(__bridge CFDictionaryRef)options,nullptr,^(OSStatus status,VTEncodeInfoFlags flags,CMSampleBufferRef sample){
   try{collect(*pending,status,flags,sample);}catch(const std::exception&e){pending->status=kVTVideoEncoderMalfunctionErr;fprintf(stderr,"[H264Encoder] %s\n",e.what());}
   dispatch_semaphore_signal(pending->done);
  }),"Submit hardware H264 frame");
  if(dispatch_semaphore_wait(pending->done,dispatch_time(DISPATCH_TIME_NOW,1000000000))){VTCompressionSessionInvalidate(session);CFRelease(session);session=nullptr;throw std::runtime_error("Hardware H264 callback timeout");}
  if(pending->status){const OSStatus error=pending->status;VTCompressionSessionInvalidate(session);CFRelease(session);session=nullptr;check(error,"Hardware H264 callback");}pending->frame.scaleMs=(resized-began)/1e6;pending->frame.encodeMs=(td::nowNs()-resized)/1e6;return std::move(pending->frame);
 }
};
H264Encoder::H264Encoder(id<MTLDevice>d,int w,int h,int f,int m):impl(std::make_unique<Impl>(d,w,h,f,m)){}
H264Encoder::~H264Encoder()=default;
H264Encoder::Frame H264Encoder::encode(CVPixelBufferRef source,bool force){return impl->encode(source,force);}
