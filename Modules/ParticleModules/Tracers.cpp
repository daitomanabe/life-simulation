// Modules/ParticleModules/Tracers.cpp
#include "Modules/ParticleModules/Tracers.h"

#include "LifeCore/Math/Random.h"

#include <vector>

namespace life {

void TracersModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    ParticleSet2DDesc pd;
    pd.capacity = uint32_t(params_.value("particleCount", 4000000u));
    pd.pingPongPosVel = false; // each particle only ever writes its own lane
    pd.label = instanceName_ + ".tracers";
    set_.create(*ctx.resources, pd);

    BufferDesc dd;
    dd.size = size_t(width_) * size_t(height_) * sizeof(uint32_t);
    dd.storage = StorageMode::GPUPrivate;
    dd.label = instanceName_ + ".density";
    density_ = ctx.resources->createBuffer(dd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    // 4x4 black fallbacks for flowField (RG) / emitField (R) — always bound
    // so tracersMove's texture arguments are valid even when no scene
    // connection targets these ports (same rationale as SlimeMold's
    // attractorFallback_/flowFallback_). Shared storage because
    // uploadTexture needs CPU-visible memory.
    auto makeFallback = [&](PixelFormat fmt, const char* suffix) {
        TextureDesc fbd;
        fbd.width = 4;
        fbd.height = 4;
        fbd.format = fmt;
        fbd.storage = StorageMode::Shared;
        fbd.label = instanceName_ + suffix;
        TextureHandle h = ctx.resources->createTexture(fbd);
        std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fmt), 0);
        ctx.resources->uploadTexture(h, zeros.data(), size_t(fbd.width) * bytesPerPixel(fmt));
        return h;
    };
    flowFallback_ = makeFallback(PixelFormat::RG16F, ".flowFallback");
    flowInput_ = flowFallback_;
    emitFallback_ = makeFallback(PixelFormat::R16F, ".emitFallback");
    emitInput_ = emitFallback_;

    gpuParams_.particleCount = pd.capacity;
    gpuParams_.width = width_;
    gpuParams_.height = height_;

    // tint is read once here (not live via param()), same convention as
    // particleCount above.
    if (params_.contains("tint") && params_["tint"].is_array() && params_["tint"].size() >= 3) {
        tintR_ = params_["tint"][0].get<float>();
        tintG_ = params_["tint"][1].get<float>();
        tintB_ = params_["tint"][2].get<float>();
    }
}

void TracersModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x54524143u); // "TRAC"
    needsInit_ = true;
}

void TracersModule::updateCPU(const AudioFeatureState&) {}

void TracersModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = ctx.dt;
    gpuParams_.lifetime = param(ctx, "lifetime", 12.0f);
    gpuParams_.emitBias = param(ctx, "emitBias", 0.85f);
    gpuParams_.emitThreshold = param(ctx, "emitThreshold", 4.0f);
    gpuParams_.flowWeight = param(ctx, "flowWeight", 1.0f);
    gpuParams_.jitter = param(ctx, "jitter", 8.0f);
    gpuParams_.wallY = param(ctx, "wallY", 0.0f) > 0.5f ? 1u : 0u;
    gpuParams_.frameIndex = ctx.frameIndex;

    if (needsInit_) {
        gpuParams_.seed = seed_;
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("tracersInit")
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities()) // repurposed: holds age/life
            .buffer(2, set_.randomState())
            .uniforms(3, gpuParams_)
            .dispatch1D(gpuParams_.particleCount);

        ctx.graph->pass(instanceName_ + ".clearDensity")
            .pipeline("tracersClearDensity")
            .buffer(0, density_)
            .uniforms(1, gpuParams_)
            .dispatch1D(width_ * height_);

        needsInit_ = false;
    }

    gpuParams_.seed = pcgHash(seed_ ^ ctx.frameIndex);

    ctx.graph->pass(instanceName_ + ".move")
        .pipeline("tracersMove")
        .buffer(0, set_.positions())
        .buffer(1, set_.velocities())
        .buffer(2, set_.randomState())
        .read(0, flowInput_)
        .read(1, emitInput_)
        .uniforms(3, gpuParams_)
        .dispatch1D(gpuParams_.particleCount);

    TracersSplatParams sp;
    sp.particleCount = gpuParams_.particleCount;
    sp.width = width_;
    sp.height = height_;
    sp.wallY = gpuParams_.wallY;
    sp.bokehRadius = param(ctx, "bokehRadius", 3.5f);
    ctx.graph->pass(instanceName_ + ".splat")
        .pipeline("tracersSplat")
        .buffer(0, set_.positions())
        .buffer(1, set_.velocities())
        .buffer(2, density_)
        .uniforms(3, sp)
        .dispatch1D(gpuParams_.particleCount);

    TracersResolveParams rp;
    rp.width = width_;
    rp.height = height_;
    rp.gain = param(ctx, "gain", 0.9f);
    rp.density = param(ctx, "density", 0.6f);
    rp.tintR = tintR_;
    rp.tintG = tintG_;
    rp.tintB = tintB_;
    rp.edgeFade = param(ctx, "edgeFade", 36.0f);
    ctx.graph->pass(instanceName_ + ".resolve")
        .pipeline("tracersResolve")
        .buffer(0, density_)
        .write(0, output_)
        .uniforms(1, rp)
        .dispatch2D(width_, height_);
}

} // namespace life
