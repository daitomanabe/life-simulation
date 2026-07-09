// LifeCore/IO/ImageWriter.mm
#include "LifeCore/IO/ImageWriter.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <vector>

// TinyEXR implementation lives in this TU (C++ part is fine inside ObjC++).
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#include <tinyexr/tinyexr.h>

namespace life {

namespace {

// arm64 has native half support via __fp16.
inline float halfToFloat(uint16_t h) {
    __fp16 v;
    std::memcpy(&v, &h, 2);
    return float(v);
}

inline uint8_t linearToSRGB8(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    float s = c <= 0.0031308f ? 12.92f * c
                              : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

} // namespace

bool writePNGFromHalfRGBA(const std::string& path, const uint16_t* pixels,
                          uint32_t width, uint32_t height, float exposure,
                          std::string& outError) {
    std::vector<uint8_t> rgba8(size_t(width) * height * 4);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        float r = halfToFloat(pixels[i * 4 + 0]) * exposure;
        float g = halfToFloat(pixels[i * 4 + 1]) * exposure;
        float b = halfToFloat(pixels[i * 4 + 2]) * exposure;
        rgba8[i * 4 + 0] = linearToSRGB8(r);
        rgba8[i * 4 + 1] = linearToSRGB8(g);
        rgba8[i * 4 + 2] = linearToSRGB8(b);
        rgba8[i * 4 + 3] = 255;
    }

    @autoreleasepool {
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, rgba8.data(), rgba8.size(), nullptr);
        CGImageRef image = CGImageCreate(
            width, height, 8, 32, size_t(width) * 4, colorSpace,
            kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, nullptr,
            false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
        if (!image) {
            outError = "CGImageCreate failed";
            return false;
        }

        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        if (!dest) {
            CGImageRelease(image);
            outError = "cannot create PNG destination: " + path;
            return false;
        }
        CGImageDestinationAddImage(dest, image, nullptr);
        bool ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
        CGImageRelease(image);
        if (!ok) outError = "PNG finalize failed: " + path;
        return ok;
    }
}

bool writeEXRFromHalfRGBA(const std::string& path, const uint16_t* pixels,
                          uint32_t width, uint32_t height, std::string& outError) {
    // TinyEXR's simple API takes float input and can store as fp16.
    std::vector<float> rgba(size_t(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = halfToFloat(pixels[i]);

    const char* err = nullptr;
    int ret = SaveEXR(rgba.data(), int(width), int(height), 4, /*save_as_fp16=*/1,
                      path.c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        outError = err ? err : "SaveEXR failed";
        if (err) FreeEXRErrorMessage(err);
        return false;
    }
    return true;
}

} // namespace life
