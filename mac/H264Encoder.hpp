#pragma once
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <memory>
#include <vector>

class H264Encoder {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    struct Frame {
        std::vector<uint8_t> bytes;
        bool key = false;
        double scaleMs = 0, encodeMs = 0;
    };

    H264Encoder(id<MTLDevice> device, int width, int height, int fps, int mbps);
    ~H264Encoder();
    Frame encode(CVPixelBufferRef source, bool forceKeyframe);
};
