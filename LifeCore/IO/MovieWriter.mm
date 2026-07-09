// LifeCore/IO/MovieWriter.mm
#include "LifeCore/IO/MovieWriter.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace life {

namespace {

// arm64 has native half support via __fp16 (same technique as ImageWriter.mm).
inline float halfToFloat(uint16_t h) {
    __fp16 v;
    std::memcpy(&v, &h, 2);
    return float(v);
}

// Identical curve to ImageWriter.mm's linearToSRGB8 — kept as a private copy
// here (rather than shared) since ImageWriter.h/.mm is out of scope for this
// phase's diff (see phase10 spec: MovieWriter is a standalone new unit).
inline uint8_t linearToSRGB8(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    float s = c <= 0.0031308f ? 12.92f * c
                              : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

AVVideoCodecType codecToAVVideoCodecType(MovieCodec c) {
    switch (c) {
        case MovieCodec::H264: return AVVideoCodecTypeH264;
        case MovieCodec::HEVC: return AVVideoCodecTypeHEVC;
        case MovieCodec::ProRes422: return AVVideoCodecTypeAppleProRes422;
        case MovieCodec::ProRes4444: return AVVideoCodecTypeAppleProRes4444;
    }
    return AVVideoCodecTypeAppleProRes422;
}

bool codecUsesQualityKey(MovieCodec c) {
    return c == MovieCodec::H264 || c == MovieCodec::HEVC;
}

std::string nsErrorString(NSError* error) {
    return error ? std::string(error.localizedDescription.UTF8String) : std::string("unknown error");
}

} // namespace

struct MovieWriter::Impl {
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
};

MovieWriter::MovieWriter() : impl_(std::make_unique<Impl>()) {}

MovieWriter::~MovieWriter() {
    // Best-effort: if the caller opened but never finished (e.g. bailed out
    // on a per-frame error), don't leave AVFoundation mid-write; a half
    // written .mov with no matching finishWriting is otherwise harmless
    // (ARC releases everything) but cancel explicitly for a clean state.
    if (impl_ && impl_->writer && impl_->writer.status == AVAssetWriterStatusWriting) {
        @autoreleasepool {
            [impl_->input markAsFinished];
            [impl_->writer cancelWriting];
        }
    }
}

bool MovieWriter::open(const MovieWriterDesc& desc, std::string& outError) {
    if (desc.path.empty()) {
        outError = "MovieWriter::open: empty path";
        return false;
    }
    if (desc.width == 0 || desc.height == 0) {
        outError = "MovieWriter::open: width/height must be > 0";
        return false;
    }
    if (desc.fps <= 0.0) {
        outError = "MovieWriter::open: fps must be > 0";
        return false;
    }
    desc_ = desc;
    framesWritten_ = 0;

    @autoreleasepool {
        NSString* pathStr = [NSString stringWithUTF8String:desc.path.c_str()];
        NSURL* url = [NSURL fileURLWithPath:pathStr];
        // AVAssetWriter refuses to write over an existing file; offline
        // re-runs at the same --movie path are common, so clear it first.
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

        NSError* error = nil;
        impl_->writer = [[AVAssetWriter alloc] initWithURL:url
                                                    fileType:AVFileTypeQuickTimeMovie
                                                       error:&error];
        if (!impl_->writer) {
            outError = "AVAssetWriter init failed: " + nsErrorString(error);
            return false;
        }

        NSMutableDictionary<NSString*, id>* videoSettings = [NSMutableDictionary dictionary];
        videoSettings[AVVideoCodecKey] = codecToAVVideoCodecType(desc.codec);
        videoSettings[AVVideoWidthKey] = @(desc.width);
        videoSettings[AVVideoHeightKey] = @(desc.height);
        if (codecUsesQualityKey(desc.codec)) {
            float q = std::clamp(desc.quality, 0.0f, 1.0f);
            videoSettings[AVVideoCompressionPropertiesKey] = @{AVVideoQualityKey : @(q)};
        }

        impl_->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                           outputSettings:videoSettings];
        if (!impl_->input) {
            outError = "AVAssetWriterInput creation failed (unsupported codec/size?)";
            impl_->writer = nil;
            return false;
        }
        // Offline: we feed frames as fast as the simulation produces them,
        // no wall-clock pacing requirement (spec §1).
        impl_->input.expectsMediaDataInRealTime = NO;

        NSDictionary<NSString*, id>* pixelBufferAttributes = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (__bridge NSString*)kCVPixelBufferWidthKey : @(desc.width),
            (__bridge NSString*)kCVPixelBufferHeightKey : @(desc.height),
        };
        impl_->adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:impl_->input
                                        sourcePixelBufferAttributes:pixelBufferAttributes];

        if (![impl_->writer canAddInput:impl_->input]) {
            outError = "AVAssetWriter cannot add video input (codec/size unsupported on this device)";
            impl_->writer = nil;
            impl_->input = nil;
            impl_->adaptor = nil;
            return false;
        }
        [impl_->writer addInput:impl_->input];

        if (![impl_->writer startWriting]) {
            outError = "AVAssetWriter startWriting failed: " + nsErrorString(impl_->writer.error);
            return false;
        }
        [impl_->writer startSessionAtSourceTime:kCMTimeZero];
    }
    return true;
}

bool MovieWriter::appendFrame(const uint16_t* halfRGBA, std::string& outError) {
    if (!halfRGBA) {
        outError = "MovieWriter::appendFrame: null pixel data";
        return false;
    }
    if (!impl_->writer || !impl_->input || !impl_->adaptor) {
        outError = "MovieWriter::appendFrame called before a successful open()";
        return false;
    }
    if (impl_->writer.status != AVAssetWriterStatusWriting) {
        outError = "AVAssetWriter not in writing state: " + nsErrorString(impl_->writer.error);
        return false;
    }

    // Offline: block until the input catches up rather than dropping frames
    // (spec §1 — "1ms スリープで待つ").
    while (!impl_->input.readyForMoreMediaData) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (impl_->writer.status != AVAssetWriterStatusWriting) {
            outError = "AVAssetWriter failed while waiting for input: " +
                       nsErrorString(impl_->writer.error);
            return false;
        }
    }

    bool appended = false;
    @autoreleasepool {
        CVPixelBufferRef pixelBuffer = nullptr;
        CVReturn cvErr = CVPixelBufferPoolCreatePixelBuffer(
            kCFAllocatorDefault, impl_->adaptor.pixelBufferPool, &pixelBuffer);
        if (cvErr != kCVReturnSuccess || !pixelBuffer) {
            outError = "CVPixelBufferPoolCreatePixelBuffer failed (" + std::to_string(cvErr) + ")";
            return false;
        }

        CVPixelBufferLockBaseAddress(pixelBuffer, 0);
        uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
        const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        const uint32_t width = desc_.width;
        const uint32_t height = desc_.height;
        const float exposure = desc_.exposure;

        // Source is tightly packed (width*4 halfs/row); destination row
        // stride comes from CVPixelBufferGetBytesPerRow and may be padded
        // for alignment — never assume it equals width*4 (spec §1).
        for (uint32_t y = 0; y < height; ++y) {
            const uint16_t* srcRow = halfRGBA + size_t(y) * width * 4;
            uint8_t* dstRow = base + size_t(y) * bytesPerRow;
            for (uint32_t x = 0; x < width; ++x) {
                float r = halfToFloat(srcRow[x * 4 + 0]) * exposure;
                float g = halfToFloat(srcRow[x * 4 + 1]) * exposure;
                float b = halfToFloat(srcRow[x * 4 + 2]) * exposure;
                uint8_t* px = dstRow + size_t(x) * 4;
                // kCVPixelFormatType_32BGRA byte order.
                px[0] = linearToSRGB8(b);
                px[1] = linearToSRGB8(g);
                px[2] = linearToSRGB8(r);
                px[3] = 255;
            }
        }

        CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferTransferFunction_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);

        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

        CMTime pts = CMTimeMakeWithSeconds(double(framesWritten_) / desc_.fps, 600);
        appended = [impl_->adaptor appendPixelBuffer:pixelBuffer withPresentationTime:pts];
        if (!appended) outError = "appendPixelBuffer failed: " + nsErrorString(impl_->writer.error);
        CVPixelBufferRelease(pixelBuffer);
    }

    if (appended) framesWritten_++;
    return appended;
}

bool MovieWriter::finish(std::string& outError) {
    if (!impl_->writer || !impl_->input) {
        outError = "MovieWriter::finish called before a successful open()";
        return false;
    }
    if (impl_->writer.status != AVAssetWriterStatusWriting) {
        outError = "AVAssetWriter not in writing state at finish(): " + nsErrorString(impl_->writer.error);
        return false;
    }

    @autoreleasepool {
        [impl_->input markAsFinished];

        dispatch_semaphore_t sema = dispatch_semaphore_create(0);
        __block AVAssetWriter* writer = impl_->writer;
        [impl_->writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sema);
        }];
        dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

        if (writer.status != AVAssetWriterStatusCompleted) {
            outError = "finishWriting failed: " + nsErrorString(writer.error);
            return false;
        }
    }
    return true;
}

} // namespace life
