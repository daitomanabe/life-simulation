// LifeCore/IO/FrameRecorder.cpp
#include "LifeCore/IO/FrameRecorder.h"

#include "LifeCore/IO/ImageWriter.h"
#include "LifeCore/IO/NpyWriter.h"

#include <cstring>

namespace life {

FrameRecorder::~FrameRecorder() {
    if (pool_ && readback_.valid()) pool_->release(readback_);
}

bool FrameRecorder::encodeReadback(CommandGraph& graph, TextureHandle src) {
    auto desc = pool_->textureDesc(src);
    if (desc.format != PixelFormat::RGBA16F) {
        fprintf(stderr, "[life] FrameRecorder expects RGBA16F, got %s\n",
                pixelFormatName(desc.format));
        return false;
    }
    size_t needed = size_t(desc.width) * desc.height * 8;
    if (!readback_.valid() || pool_->bufferSize(readback_) < needed) {
        if (readback_.valid()) pool_->release(readback_);
        BufferDesc bd;
        bd.size = needed;
        bd.storage = StorageMode::Shared;
        bd.label = "FrameRecorder.readback";
        readback_ = pool_->createBuffer(bd);
        if (!readback_.valid()) return false;
    }
    width_ = desc.width;
    height_ = desc.height;
    graph.copyTextureToBuffer(src, readback_);
    pendingFetch_ = true;
    return true;
}

bool FrameRecorder::fetch() {
    if (!pendingFetch_ || !readback_.valid()) return false;
    void* src = pool_->bufferContents(readback_);
    if (!src) return false;
    pixels_.resize(size_t(width_) * height_ * 4);
    std::memcpy(pixels_.data(), src, pixels_.size() * sizeof(uint16_t));
    pendingFetch_ = false;
    return true;
}

bool FrameRecorder::writePNG(const std::string& path, float exposure,
                             std::string& outError) {
    if (pixels_.empty()) {
        outError = "no readback data (call encodeReadback + fetch first)";
        return false;
    }
    return writePNGFromHalfRGBA(path, pixels_.data(), width_, height_, exposure,
                                outError);
}

bool FrameRecorder::writeEXR(const std::string& path, std::string& outError) {
    if (pixels_.empty()) {
        outError = "no readback data (call encodeReadback + fetch first)";
        return false;
    }
    return writeEXRFromHalfRGBA(path, pixels_.data(), width_, height_, outError);
}

bool FrameRecorder::writeNPY(const std::string& path, std::string& outError) {
    if (pixels_.empty()) {
        outError = "no readback data (call encodeReadback + fetch first)";
        return false;
    }
    std::vector<float> rgba(pixels_.size());
    for (size_t i = 0; i < pixels_.size(); ++i) rgba[i] = half2float(pixels_[i]);
    return writeNPYFloat32(path, rgba.data(), rgba.size(), {height_, width_, 4u}, outError);
}

} // namespace life
