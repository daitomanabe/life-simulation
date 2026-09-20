#pragma once
// LifeCore/IO/FrameRecorder.h
// GPU→CPU readback plus image export. Readback is opt-in per frame (never
// automatic — design doc §16.3 forbids per-frame readback in realtime).
// Flow: encodeReadback() inside the frame → endFrame(wait) → writePNG/EXR.

#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace life {

class FrameRecorder {
public:
    explicit FrameRecorder(ResourcePool& pool) : pool_(&pool) {}
    ~FrameRecorder();

    // Blit `src` (must be RGBA16F) into an internal shared buffer. Call
    // between beginFrame/endFrame; data is valid after endFrame(wait=true).
    bool encodeReadback(CommandGraph& graph, TextureHandle src);

    // Copy the readback buffer to CPU memory. Returns false if no readback
    // was encoded.
    bool fetch();

    bool writePNG(const std::string& path, float exposure, std::string& outError);
    bool writeEXR(const std::string& path, std::string& outError);
    // dump-state: exposure-independent raw linear RGBA float32 (H,W,4) .npy,
    // for byte-for-byte Python-side parity checks against the render.
    bool writeNPY(const std::string& path, std::string& outError);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::vector<uint16_t>& halfPixels() const { return pixels_; }

private:
    ResourcePool* pool_ = nullptr;
    BufferHandle readback_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool pendingFetch_ = false;
    std::vector<uint16_t> pixels_; // RGBA16F raw
};

} // namespace life
