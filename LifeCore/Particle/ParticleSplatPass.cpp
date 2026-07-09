// LifeCore/Particle/ParticleSplatPass.cpp
#include "LifeCore/Particle/ParticleSplatPass.h"

namespace life {

namespace {
// Mirrors SplatParams in Shaders/Particle/ParticleSplat.metal.
struct SplatParams {
    uint32_t particleCount;
    uint32_t width;
    uint32_t height;
    float weight;
    float gainR;
    float gainG;
    float gainB;
    float gain;
};
} // namespace

void ParticleSplatPass::encode(CommandGraph& graph, const std::string& labelPrefix,
                               BufferHandle positions, uint32_t count,
                               TextureHandle target, uint32_t w, uint32_t h,
                               const Params& p) {
    if (!density_.valid() || densityWidth_ != w || densityHeight_ != h) {
        if (density_.valid()) graph.resources().release(density_);
        BufferDesc bd;
        bd.size = size_t(w) * size_t(h) * sizeof(uint32_t);
        bd.storage = StorageMode::GPUPrivate;
        bd.label = labelPrefix + ".density";
        density_ = graph.resources().createBuffer(bd);
        densityWidth_ = w;
        densityHeight_ = h;
        needsClear_ = true;
    }

    SplatParams sp{};
    sp.particleCount = count;
    sp.width = w;
    sp.height = h;
    sp.weight = p.weight;
    sp.gainR = p.r;
    sp.gainG = p.g;
    sp.gainB = p.b;
    sp.gain = p.gain;

    if (needsClear_) {
        graph.pass(labelPrefix + ".splatClear")
            .pipeline("splatClear")
            .buffer(0, density_)
            .uniforms(1, sp)
            .dispatch1D(w * h);
        needsClear_ = false;
    }

    graph.pass(labelPrefix + ".splatAccumulate")
        .pipeline("splatAccumulate")
        .buffer(0, positions)
        .buffer(1, density_)
        .uniforms(2, sp)
        .dispatch1D(count);

    graph.pass(labelPrefix + ".splatResolve")
        .pipeline("splatResolve")
        .buffer(0, density_)
        .write(0, target)
        .uniforms(1, sp)
        .dispatch2D(w, h);
}

} // namespace life
