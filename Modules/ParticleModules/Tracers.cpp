// Modules/ParticleModules/Tracers.cpp
#include "Modules/ParticleModules/Tracers.h"

#include "LifeCore/IO/NpyWriter.h"
#include "LifeCore/Math/Random.h"

#include <cstring>
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

// Matches Shaders/Particle/Tracers.metal's tracerDepth() hash constant
// ("TRAC" tag) — Python needs this to reproduce the per-particle depth z.
static constexpr uint32_t kTracerDepthHashConstant = 0x54524143u;

void TracersModule::dumpState(SimulationContext& ctx, const std::string& dir,
                              nlohmann::json& meta) {
    std::string err;
    size_t n = gpuParams_.particleCount;

    // positions.npy / agelife.npy: (N, 2) float32. Both buffers are already
    // float2 in GPUPrivate memory, so the Shared readback copy IS the npy
    // payload (no per-element conversion needed).
    auto dumpVec2Buffer = [&](BufferHandle src, const char* suffix) {
        BufferDesc bd;
        bd.size = size_t(n) * 8;
        bd.storage = StorageMode::Shared;
        bd.label = instanceName_ + ".dump" + suffix + "Readback";
        BufferHandle rb = ctx.resources->createBuffer(bd);
        ctx.graph->beginFrame(ctx.frameIndex);
        ctx.graph->copyBufferToBuffer(src, rb, bd.size);
        ctx.graph->endFrame(true);
        const float* data = static_cast<const float*>(ctx.resources->bufferContents(rb));
        writeNPYFloat32(dir + "/" + instanceName_ + "." + suffix + ".npy", data, n * 2,
                        {uint32_t(n), 2u}, err);
        ctx.resources->release(rb);
    };
    dumpVec2Buffer(set_.positions(), "positions");
    dumpVec2Buffer(set_.velocities(), "agelife"); // repurposed buffer (see header)

    meta["gain"] = param(ctx, "gain", 0.9f);
    meta["density"] = param(ctx, "density", 0.6f);
    meta["tint"] = {tintR_, tintG_, tintB_};
    meta["edgeFade"] = param(ctx, "edgeFade", 36.0f);
    meta["bokehRadius"] = param(ctx, "bokehRadius", 3.5f);
    meta["wallY"] = gpuParams_.wallY;
    meta["particleCount"] = gpuParams_.particleCount;
    meta["depthHashConstant"] = kTracerDepthHashConstant;

    if (!err.empty()) fprintf(stderr, "[life] %s dumpState: %s\n", instanceName_.c_str(), err.c_str());
}

bool TracersModule::loadState(SimulationContext& ctx, const std::string& dir,
                              const nlohmann::json& meta) {
    uint32_t savedCount = meta.value("particleCount", 0u);
    if (savedCount != gpuParams_.particleCount) {
        fprintf(stderr,
                "[life] %s loadState: particleCount mismatch (scene has %u, snapshot has %u) — "
                "refusing to load\n",
                instanceName_.c_str(), gpuParams_.particleCount, savedCount);
        return false;
    }

    std::string err;
    std::vector<float> pos, ageLife;
    if (!readNPYFloat32(dir + "/" + instanceName_ + ".positions.npy",
                        {gpuParams_.particleCount, 2u}, pos, err)) {
        fprintf(stderr, "[life] %s loadState: %s\n", instanceName_.c_str(), err.c_str());
        return false;
    }
    if (!readNPYFloat32(dir + "/" + instanceName_ + ".agelife.npy",
                        {gpuParams_.particleCount, 2u}, ageLife, err)) {
        fprintf(stderr, "[life] %s loadState: %s\n", instanceName_.c_str(), err.c_str());
        return false;
    }

    auto stageVec2 = [&](const std::vector<float>& src, const char* suffix,
                         BufferHandle& outStaging) -> bool {
        BufferDesc bd;
        bd.size = src.size() * sizeof(float);
        bd.storage = StorageMode::Shared;
        bd.label = instanceName_ + ".load" + suffix + "Staging";
        outStaging = ctx.resources->createBuffer(bd);
        void* dst = outStaging.valid() ? ctx.resources->bufferContents(outStaging) : nullptr;
        if (!dst) {
            fprintf(stderr, "[life] %s loadState: failed to stage %s buffer\n",
                    instanceName_.c_str(), suffix);
            return false;
        }
        std::memcpy(dst, src.data(), bd.size);
        return true;
    };

    BufferHandle posStaging, ageLifeStaging;
    if (!stageVec2(pos, "Pos", posStaging) || !stageVec2(ageLife, "AgeLife", ageLifeStaging)) {
        if (posStaging.valid()) ctx.resources->release(posStaging);
        if (ageLifeStaging.valid()) ctx.resources->release(ageLifeStaging);
        return false;
    }

    // Same rationale as SlimeMoldModule::loadState(): re-run the cold-start
    // init (positions/age-life/randomState + density clear — the exact
    // passes encode()'s needsInit_ block runs) so nothing reads
    // uninitialized GPUPrivate memory, then blit the loaded positions/
    // age-life over the throwaway init values.
    gpuParams_.seed = seed_;
    ctx.graph->beginFrame(ctx.frameIndex);
    ctx.graph->pass(instanceName_ + ".loadInit")
        .pipeline("tracersInit")
        .buffer(0, set_.positions())
        .buffer(1, set_.velocities()) // repurposed: holds age/life
        .buffer(2, set_.randomState())
        .uniforms(3, gpuParams_)
        .dispatch1D(gpuParams_.particleCount);
    ctx.graph->pass(instanceName_ + ".loadClearDensity")
        .pipeline("tracersClearDensity")
        .buffer(0, density_)
        .uniforms(1, gpuParams_)
        .dispatch1D(width_ * height_);
    size_t bytes = size_t(gpuParams_.particleCount) * 8;
    ctx.graph->copyBufferToBuffer(posStaging, set_.positions(), bytes);
    ctx.graph->copyBufferToBuffer(ageLifeStaging, set_.velocities(), bytes);
    ctx.graph->endFrame(true);

    ctx.resources->release(posStaging);
    ctx.resources->release(ageLifeStaging);

    needsInit_ = false;

    double sum = 0.0;
    for (size_t i = 0; i < ageLife.size(); i += 2) sum += ageLife[i + 1]; // ageLife = (age, lifespan)
    double meanLife = ageLife.empty() ? 0.0 : sum / double(ageLife.size() / 2);
    fprintf(stderr, "[life] %s loadState: warm-started (particles=%u, lifespan.mean=%.4f)\n",
            instanceName_.c_str(), gpuParams_.particleCount, meanLife);
    return true;
}

} // namespace life
