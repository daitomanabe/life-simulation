// Modules/ParticleModules/Boids.cpp
#include "Modules/ParticleModules/Boids.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <algorithm>
#include <vector>

namespace life {

namespace {
// Mirrors SplatResolveRGBParams in Shaders/Particle/ParticleSplat.metal.
struct SplatResolveRGBParams {
    uint32_t width;
    uint32_t height;
    float gain;
};
// Mirrors ClearParams in Shaders/Render/Composite.metal (clearTexture).
struct ClearParams {
    float r, g, b, a;
};
} // namespace

void BoidsModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    const uint32_t count = uint32_t(params_.value("particleCount", 100000u));
    const float radius = params_.value("radius", 14.0f);

    ParticleSet2DDesc pd;
    pd.capacity = count;
    pd.pingPongPosVel = true; // interaction sim: read prev state, write next
    pd.label = instanceName_ + ".boids";
    set_.create(*ctx.resources, pd);

    SpatialHashDesc hd;
    hd.maxParticles = count;
    hd.width = width_;
    hd.height = height_;
    hd.cellSize = radius; // cellSize == interactionRadius (§9.3)
    hd.label = instanceName_ + ".grid";
    grid_.create(*ctx.resources, hd);

    BufferDesc rd;
    rd.size = size_t(width_) * height_ * 3 * sizeof(uint32_t);
    rd.storage = StorageMode::GPUPrivate;
    rd.label = instanceName_ + ".rgb";
    rgb_ = ctx.resources->createBuffer(rd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    // 4x4 black fallback for the flowField input port (Phase 8 §3 / design
    // point 3): always bound so boidsStep's texture argument is valid even
    // when no scene connection targets "flowField". Shared storage because
    // uploadTexture requires CPU-visible memory (GPUPrivate can't be written
    // from the CPU). RG16F is enough — only .xy is ever sampled.
    TextureDesc fbd;
    fbd.width = 4;
    fbd.height = 4;
    fbd.format = PixelFormat::RG16F;
    fbd.storage = StorageMode::Shared;
    fbd.label = instanceName_ + ".flowFallback";
    flowFallback_ = ctx.resources->createTexture(fbd);
    std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fbd.format), 0);
    ctx.resources->uploadTexture(flowFallback_, zeros.data(),
                                 size_t(fbd.width) * bytesPerPixel(fbd.format));
    flowInput_ = flowFallback_;

    gpuParams_.particleCount = count;
    gpuParams_.worldW = float(width_);
    gpuParams_.worldH = float(height_);
    gpuParams_.cellSize = grid_.cellSize();
    gpuParams_.cellsX = grid_.cellsX();
    gpuParams_.cellsY = grid_.cellsY();
}

void BoidsModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x424F4931); // "BOI1"
    needsInit_ = true;
}

void BoidsModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void BoidsModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = ctx.dt;
    gpuParams_.radius = param(ctx, "radius", 14.0f);
    gpuParams_.sepWeight = param(ctx, "sepWeight", 60.0f);
    gpuParams_.aliWeight = param(ctx, "aliWeight", 25.0f);
    gpuParams_.cohWeight = param(ctx, "cohWeight", 18.0f);
    gpuParams_.minSpeed = param(ctx, "minSpeed", 40.0f);
    gpuParams_.maxSpeed = param(ctx, "maxSpeed", 140.0f);
    gpuParams_.jitter = param(ctx, "jitter", 0.0f);
    gpuParams_.sepBoost = param(ctx, "sepBoost", 0.0f);
    gpuParams_.sepRadiusFrac = param(ctx, "sepRadiusFrac", 0.35f);
    gpuParams_.impulse = param(ctx, "impulse", 0.0f);
    gpuParams_.scatterPulse = param(ctx, "scatterPulse", 0.0f);
    gpuParams_.flowWeight = param(ctx, "flowWeight", 0.0f);
    gpuParams_.frameIndex = ctx.frameIndex;

    AudioUniforms au = toAudioUniforms(audio_);
    const uint32_t count = gpuParams_.particleCount;

    if (needsInit_) {
        gpuParams_.seed = seed_;
        // Writes the current read side so this frame's grid build + step see
        // the spawn state (no swap in between).
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("boidsInit")
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.randomState())
            .uniforms(3, gpuParams_)
            .dispatch1D(count);

        // rgb_ is GPUPrivate (not zero-initialized); one-time clear.
        SplatResolveRGBParams rp{width_, height_, 1.0f};
        ctx.graph->pass(instanceName_ + ".splatClearRGB")
            .pipeline("splatClearRGB")
            .buffer(0, rgb_)
            .uniforms(1, rp)
            .dispatch1D(width_ * height_ * 3);

        needsInit_ = false;
    }

    // Flocking step: build grid from read side → boidsStep (read → write) →
    // swap. Repeated per runner substep (§17 quality knob, house convention).
    uint32_t steps = std::max(1u, ctx.substeps);
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 251u + s));

        grid_.encodeBuild(*ctx.graph, instanceName_ + ".grid", set_.positions(),
                          count);

        ctx.graph->pass(instanceName_ + ".step")
            .pipeline("boidsStep")
            .read(0, flowInput_)
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.positionsWrite())
            .buffer(3, set_.velocitiesWrite())
            .buffer(4, set_.randomState())
            .buffer(5, grid_.cellStart())
            .buffer(6, grid_.cellCount())
            .buffer(7, grid_.sortedIndices())
            .uniforms(8, gpuParams_)
            .uniforms(9, au)
            .dispatch1D(count);
        set_.swap();
    }

    // Heading-hue splat is the output: clear to black, accumulate, resolve.
    ClearParams clear{0.0f, 0.0f, 0.0f, 1.0f};
    ctx.graph->pass(instanceName_ + ".clearOutput")
        .pipeline("clearTexture")
        .write(0, output_)
        .uniforms(0, clear)
        .dispatch2D(width_, height_);

    ctx.graph->pass(instanceName_ + ".splatAccum")
        .pipeline("boidsSplatAccum")
        .buffer(0, set_.positions()) // post-swap read side = freshly written
        .buffer(1, set_.velocities())
        .buffer(2, rgb_)
        .uniforms(3, gpuParams_)
        .dispatch1D(count);

    SplatResolveRGBParams rp{width_, height_, param(ctx, "splatGain", 1.0f)};
    ctx.graph->pass(instanceName_ + ".splatResolve")
        .pipeline("splatResolveRGB")
        .buffer(0, rgb_)
        .write(0, output_)
        .uniforms(1, rp)
        .dispatch2D(width_, height_);
}

} // namespace life
