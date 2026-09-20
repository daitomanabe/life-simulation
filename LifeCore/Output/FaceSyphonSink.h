#pragma once
// LifeCore/Output/FaceSyphonSink.h
// Pure C++ facade — Metal/Syphon types stay inside FaceSyphonSink.mm (design
// doc §3.8), same as SyphonSink. Publishes the render target as N per-face
// Syphon (Metal) servers instead of one whole-strip server: Room B drives
// three wall screens (west/north/east) as one continuous 16380x692 strip
// (x wraps; floor/ceiling are walls) so the mapping software would otherwise
// have to crop and scale a single wide feed itself. This sink does that
// crop+scale on the GPU and hands each wall its own already-sized server.
// Only compiled when LIFE_WITH_SYPHON is enabled (see CMakeLists.txt /
// external/Syphon) — same gate as SyphonSink.

#include "LifeCore/Output/OutputSink.h"

#include <memory>
#include <string>

namespace life {

class FaceSyphonSink : public OutputSink {
public:
    // Publishes faceCount servers named "<baseName> west"/"north"/"east" for
    // the faceCount == 3 case (Room B's convention); "<baseName> 1".."
    // <baseName> N" for any other faceCount. An empty baseName drops the
    // prefix (bare "west"/"north"/"east"). Each server publishes at
    // outWidth x outHeight (default 6816x864, the wall/projector
    // resolution); the source face size is (sink width / faceCount) x (sink
    // height), so this is normally an upscale.
    //
    // grainAmount: film grain applied in facePresentToBGRA, at OUTPUT
    // resolution (post-upscale), independently per output pixel — see
    // Shaders/Render/FacePresent.metal. Same meaning as a module's
    // "relief.grain" (a multiplicative jitter of 1 + amount*(rand-0.5));
    // that knob should be left at 0 once this is in use, since grain now
    // belongs here. 0 (default) is a true no-op: no extra GPU work, output
    // is bit-identical to grainAmount not being passed at all.
    explicit FaceSyphonSink(std::string baseName, uint32_t faceCount = 3,
                            uint32_t outWidth = 6816, uint32_t outHeight = 864,
                            float grainAmount = 0.0f);
    ~FaceSyphonSink() override;

    // Fails (outError set) if width is not evenly divisible by faceCount —
    // deliberately, rather than silently rounding a face boundary.
    bool start(MetalContext& metal, ResourcePool& pool, uint32_t width, uint32_t height,
              std::string& outError) override;
    void publish(CommandGraph& graph, TextureHandle frame) override;
    void pump(bool& shouldQuit) override;
    void stop() override;
    const char* name() const override { return "FaceSyphon"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string baseName_;
    uint32_t faceCount_;
    uint32_t outWidth_;
    uint32_t outHeight_;
    float grainAmount_;
};

} // namespace life
