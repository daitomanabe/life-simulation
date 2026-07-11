// Modules/ParticleModules/ParticleLife.cpp
#include "Modules/ParticleModules/ParticleLife.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

void ParticleLifeModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;
    pool_ = ctx.resources;

    const uint32_t count = uint32_t(params_.value("particleCount", 100000u));
    const uint32_t K = std::max(1u, uint32_t(params_.value("species", 6u)));
    const float rMax = params_.value("rMax", 24.0f);

    ParticleSet2DDesc pd;
    pd.capacity = count;
    pd.pingPongPosVel = true; // interaction sim: read prev state, write next
    pd.label = instanceName_ + ".particles";
    set_.create(*ctx.resources, pd);

    SpatialHashDesc hd;
    hd.maxParticles = count;
    hd.width = width_;
    hd.height = height_;
    hd.cellSize = rMax; // cellSize == interactionRadius (§9.3)
    hd.label = instanceName_ + ".grid";
    grid_.create(*ctx.resources, hd);

    // K×K interaction matrix — Shared so the CPU can rewrite it on a matrix
    // shuffle (snare) via bufferContents + memcpy.
    BufferDesc md;
    md.size = size_t(K) * K * sizeof(float);
    md.storage = StorageMode::Shared;
    md.label = instanceName_ + ".matrix";
    matrix_ = ctx.resources->createBuffer(md);

    // Per-species palette (K×4 floats, rgb + pad). Depends only on K, so fill
    // it once here. Default is the hue-wheel cosine palette. Setting
    // "monoColor": true collapses every species to white with only a
    // brightness step between them — for the #000/#fff VJ look, where any hue
    // reads as a mistake. Kept opt-in so existing scenes are unchanged.
    const bool monoColor = jsonParams().value("monoColor", false);
    BufferDesc cd;
    cd.size = size_t(K) * 4 * sizeof(float);
    cd.storage = StorageMode::Shared;
    cd.label = instanceName_ + ".speciesColor";
    speciesColor_ = ctx.resources->createBuffer(cd);
    if (auto* c = static_cast<float*>(ctx.resources->bufferContents(speciesColor_))) {
        for (uint32_t s = 0; s < K; ++s) {
            float t = float(s) / float(K);
            if (monoColor) {
                // 0.55..1.0 の白の濃淡。種の違いは明度だけで出す。
                float v = 0.55f + 0.45f * t;
                c[s * 4 + 0] = c[s * 4 + 1] = c[s * 4 + 2] = v;
            } else {
                const float phase[3] = {0.0f, 0.33f, 0.67f};
                for (int ch = 0; ch < 3; ++ch)
                    c[s * 4 + ch] = 0.5f + 0.5f * std::cos(6.2832f * (t + phase[ch]));
            }
            c[s * 4 + 3] = 1.0f;
        }
    }

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

    // 4x4 black fallback for the forceField input port (Phase 5 §3c /
    // design point 3): always bound so plStep's texture argument is valid
    // even when no scene connection targets "forceField". Shared storage
    // because uploadTexture requires CPU-visible memory (GPUPrivate can't
    // be written from the CPU).
    TextureDesc fbd;
    fbd.width = 4;
    fbd.height = 4;
    fbd.format = PixelFormat::R16F;
    fbd.storage = StorageMode::Shared;
    fbd.label = instanceName_ + ".fieldFallback";
    fieldFallback_ = ctx.resources->createTexture(fbd);
    std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fbd.format), 0);
    ctx.resources->uploadTexture(fieldFallback_, zeros.data(),
                                 size_t(fbd.width) * bytesPerPixel(fbd.format));
    fieldInput_ = fieldFallback_;

    gpuParams_.particleCount = count;
    gpuParams_.speciesCount = K;
    gpuParams_.worldW = float(width_);
    gpuParams_.worldH = float(height_);
    gpuParams_.cellSize = grid_.cellSize();
    gpuParams_.cellsX = grid_.cellsX();
    gpuParams_.cellsY = grid_.cellsY();
}

void ParticleLifeModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x504C4631); // "PLF1"
    shuffleCount_ = 0;
    prevShufflePulse_ = 0.0f;
    needsInit_ = true;
    regenerateMatrix();
}

// Matrix entries in [-1,1], derived from (seed_, shuffleCount_) ONLY — a
// capture replay hits the same shuffle count at the same frame, so the exact
// matrix sequence reproduces (spec §2 "リプレイ決定性").
void ParticleLifeModule::regenerateMatrix() {
    if (!pool_) return;
    auto* m = static_cast<float*>(pool_->bufferContents(matrix_));
    if (!m) return;
    SplitMix64 rng(deriveSeed(seed_, 0x4D545831 + shuffleCount_)); // "MTX1"+n
    const uint32_t K = gpuParams_.speciesCount;
    for (uint32_t i = 0; i < K * K; ++i) m[i] = rng.nextRange(-1.0f, 1.0f);
}

void ParticleLifeModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void ParticleLifeModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = ctx.dt;
    gpuParams_.rMax = param(ctx, "rMax", 24.0f);
    gpuParams_.beta = param(ctx, "beta", 0.3f);
    gpuParams_.forceScale = param(ctx, "forceScale", 60.0f);
    gpuParams_.friction = param(ctx, "friction", 4.0f);
    gpuParams_.maxSpeed = param(ctx, "maxSpeed", 160.0f);
    gpuParams_.jitter = param(ctx, "jitter", 0.0f);
    gpuParams_.forceBoost = param(ctx, "forceBoost", 0.0f);
    gpuParams_.fieldForce = param(ctx, "fieldForce", 0.0f);
    gpuParams_.frameIndex = ctx.frameIndex;

    // snare.trigger → shufflePulse: regenerate the matrix on the frame the
    // value crosses 0.5 upward. Offline waits on the GPU every frame, so the
    // Shared-buffer rewrite can never race in-flight work.
    float shufflePulse = param(ctx, "shufflePulse", 0.0f);
    if (prevShufflePulse_ < 0.5f && shufflePulse >= 0.5f) {
        shuffleCount_++;
        regenerateMatrix();
    }
    prevShufflePulse_ = shufflePulse;

    AudioUniforms au = toAudioUniforms(audio_);
    const uint32_t count = gpuParams_.particleCount;

    if (needsInit_) {
        gpuParams_.seed = seed_;
        // Writes the current read side so this frame's grid build + step see
        // the spawn state (no swap in between).
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("plInit")
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.species())
            .buffer(3, set_.randomState())
            .uniforms(4, gpuParams_)
            .dispatch1D(count);

        // rgb_ is GPUPrivate (not zero-initialized); one-time clear.
        // splatResolveRGB zeroes cells after reading, so never again.
        SplatResolveRGBParams rp{width_, height_, 1.0f};
        ctx.graph->pass(instanceName_ + ".splatClearRGB")
            .pipeline("splatClearRGB")
            .buffer(0, rgb_)
            .uniforms(1, rp)
            .dispatch1D(width_ * height_ * 3);

        needsInit_ = false;
    }

    // Interaction step: build grid from read side → plStep (read → write) →
    // swap. Repeated per runner substep (§17 quality knob, house convention).
    uint32_t steps = std::max(1u, ctx.substeps);
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 251u + s));

        grid_.encodeBuild(*ctx.graph, instanceName_ + ".grid", set_.positions(),
                          count);

        ctx.graph->pass(instanceName_ + ".step")
            .pipeline("plStep")
            .read(0, fieldInput_)
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.positionsWrite())
            .buffer(3, set_.velocitiesWrite())
            .buffer(4, set_.species())
            .buffer(5, set_.randomState())
            .buffer(6, grid_.cellStart())
            .buffer(7, grid_.cellCount())
            .buffer(8, grid_.sortedIndices())
            .buffer(9, matrix_)
            .uniforms(10, gpuParams_)
            .uniforms(11, au)
            .dispatch1D(count);
        set_.swap();
    }

    // The splat IS the output (no ColorMapPass): clear to black, accumulate
    // species colors, resolve.
    ClearParams clear{0.0f, 0.0f, 0.0f, 1.0f};
    ctx.graph->pass(instanceName_ + ".clearOutput")
        .pipeline("clearTexture")
        .write(0, output_)
        .uniforms(0, clear)
        .dispatch2D(width_, height_);

    ctx.graph->pass(instanceName_ + ".splatAccum")
        .pipeline("plSplatAccum")
        .buffer(0, set_.positions()) // post-swap read side = freshly written
        .buffer(1, set_.species())
        .buffer(2, speciesColor_)
        .buffer(3, rgb_)
        .uniforms(4, gpuParams_)
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
