#pragma once
// LifeCore/Particle/ParticleSplatPass.h
// Splats a particle position buffer into an RGBA16F texture (design doc
// §8.4: every particle system must resolve to a texture before Composite —
// "ParticleBuffer → ParticleSplat → RGBA16Float Texture → Composite"). Same
// reusable-pass shape as LifeCore/Render/ColorMapPass.h.
//
// Accumulation goes through a uint fixed-point density buffer (scale 256)
// via atomic adds — integer addition is order independent, so the result is
// deterministic regardless of GPU thread scheduling (no float atomics, per
// the suite-wide constraint). The buffer is created lazily on first encode()
// and reused across frames; splatResolve clears each cell right after
// reading it, so a separate clear pass is only needed once, up front.

#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <cstdint>
#include <string>

namespace life {

class ParticleSplatPass {
public:
    struct Params {
        float weight = 1.0f;                // per-particle contribution (scale 256 fixed point)
        float r = 1.0f, g = 1.0f, b = 1.0f;  // resolve-time color
        float gain = 1.0f;
    };

    // density buffer (w*h*4 bytes, atomic_uint) is created lazily inside
    // encode() via graph.resources(), labelled "<labelPrefix>.density".
    void encode(CommandGraph& graph, const std::string& labelPrefix,
               BufferHandle positions, uint32_t count, TextureHandle target,
               uint32_t w, uint32_t h, const Params& p);

private:
    BufferHandle density_;
    uint32_t densityWidth_ = 0;
    uint32_t densityHeight_ = 0;
    bool needsClear_ = true;
};

} // namespace life
