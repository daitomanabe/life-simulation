#pragma once
// LifeCore/IO/MovieWriter.h
// Direct .mov export via AVAssetWriter (design doc §3.6 / phase10 spec).
// Pure C++ interface — no AVFoundation/CoreMedia/CoreVideo type leaks into
// this header; the .mm translation unit owns every Objective-C object
// behind a PIMPL (same convention as MetalContext/ResourcePool/CommandGraph
// in LifeCore/Metal). Input is RGBA16F half data, the suite's unified
// readback format (see LifeCore/IO/FrameRecorder.h) — same as ImageWriter's
// writePNGFromHalfRGBA, just fed in one frame at a time.

#include <cstdint>
#include <memory>
#include <string>

namespace life {

enum class MovieCodec { H264, HEVC, ProRes422, ProRes4444 };

struct MovieWriterDesc {
    std::string path;          // .mov
    uint32_t width = 0, height = 0;
    double fps = 30.0;
    MovieCodec codec = MovieCodec::ProRes422;
    float quality = 0.9f;      // H264/HEVC only (0..1 -> AVVideoQualityKey)
    float exposure = 1.0f;     // linear -> sRGB exposure, same as PNG path
};

class MovieWriter {
public:
    MovieWriter();
    ~MovieWriter();

    MovieWriter(const MovieWriter&) = delete;
    MovieWriter& operator=(const MovieWriter&) = delete;

    bool open(const MovieWriterDesc& desc, std::string& outError);

    // FrameRecorder::halfPixels()-shaped RGBA16F raw values (tightly
    // packed, width*height*4 halfs) for one frame. Internally converted
    // linear->sRGB 8-bit BGRA (same curve as ImageWriter) into a
    // CVPixelBuffer. Calls must be in frame order, from a single thread.
    bool appendFrame(const uint16_t* halfRGBA, std::string& outError);

    // Must be called exactly once after the last appendFrame(), before
    // destruction. Waits synchronously for AVAssetWriter to finish.
    bool finish(std::string& outError);

    uint64_t framesWritten() const { return framesWritten_; }

    const std::string& path() const { return desc_.path; }
    MovieCodec codec() const { return desc_.codec; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MovieWriterDesc desc_;
    uint64_t framesWritten_ = 0;
};

} // namespace life
