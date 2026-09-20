// Modules/ParticleModules/SlimeMold.cpp
#include "Modules/ParticleModules/SlimeMold.h"

#include "LifeCore/IO/NpyWriter.h"
#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <algorithm>
#include <vector>

namespace life {

namespace {
// Mirrors SplatParams in Shaders/Particle/ParticleSplat.metal — reused
// directly (via the generic splatAccumulate kernel) for the strayGain
// overlay's raw agent count, instead of a bespoke accumulate kernel.
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

void SlimeMoldModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    ParticleSet2DDesc pd;
    pd.capacity = uint32_t(params_.value("agentCount", 200000u));
    pd.pingPongPosVel = false; // each agent only ever writes its own lane
    pd.label = instanceName_ + ".agents";
    set_.create(*ctx.resources, pd);

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    fd.format = PixelFormat::R16F;
    fd.pingPong = true;
    fd.boundary = BoundaryMode::Wrap;
    fd.label = instanceName_ + ".trail";
    trail_.create(*ctx.resources, fd);

    BufferDesc dd;
    dd.size = size_t(width_) * size_t(height_) * sizeof(uint32_t);
    dd.storage = StorageMode::GPUPrivate;
    dd.label = instanceName_ + ".deposit";
    deposit_ = ctx.resources->createBuffer(dd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    // 4x4 black fallback for the attractorField input port (Phase 5 §3a /
    // design point 3): always bound so slimeMove's texture argument is
    // valid even when no scene connection targets "attractorField". Shared
    // storage because uploadTexture requires CPU-visible memory (GPUPrivate
    // can't be written from the CPU).
    TextureDesc fbd;
    fbd.width = 4;
    fbd.height = 4;
    fbd.format = PixelFormat::R16F;
    fbd.storage = StorageMode::Shared;
    fbd.label = instanceName_ + ".attractorFallback";
    attractorFallback_ = ctx.resources->createTexture(fbd);
    std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fbd.format), 0);
    ctx.resources->uploadTexture(attractorFallback_, zeros.data(),
                                 size_t(fbd.width) * bytesPerPixel(fbd.format));
    attractorInput_ = attractorFallback_;

    // 4x4 black RG fallback for the flowField input port (Phase 12) — same
    // rationale as attractorFallback_ above.
    TextureDesc ffd;
    ffd.width = 4;
    ffd.height = 4;
    ffd.format = PixelFormat::RG16F;
    ffd.storage = StorageMode::Shared;
    ffd.label = instanceName_ + ".flowFallback";
    flowFallback_ = ctx.resources->createTexture(ffd);
    std::vector<uint8_t> flowZeros(size_t(ffd.width) * ffd.height * bytesPerPixel(ffd.format), 0);
    ctx.resources->uploadTexture(flowFallback_, flowZeros.data(),
                                 size_t(ffd.width) * bytesPerPixel(ffd.format));
    flowInput_ = flowFallback_;

    gpuParams_.agentCount = pd.capacity;
    gpuParams_.width = width_;
    gpuParams_.height = height_;

    // Slime trails read best as a warm, low->high intensity glow; override
    // per scene via colorMap{}.
    colorMap_.params.dR = 0.55f;
    colorMap_.params.dG = 0.42f;
    colorMap_.params.dB = 0.30f;
    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.4f;
    colorMap_.configure(params_);
}

void SlimeMoldModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x534C4D31); // "SLM1"
    needsInit_ = true;
    sharedDensityNeedsClear_ = true;
}

void SlimeMoldModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void SlimeMoldModule::encode(SimulationContext& ctx) {
    // Final (audio-modulated) values for this frame, via ParameterBus.
    gpuParams_.dt = ctx.dt; // moveSpeed/turnSpeed are physical px/s, rad/s
    gpuParams_.moveSpeed = param(ctx, "moveSpeed", 55.0f);
    gpuParams_.sensorAngle = param(ctx, "sensorAngle", 0.6f);
    gpuParams_.sensorBoost = param(ctx, "sensorBoost", 0.0f);
    gpuParams_.sensorDistance = param(ctx, "sensorDistance", 9.0f);
    gpuParams_.turnSpeed = param(ctx, "turnSpeed", 10.0f);
    gpuParams_.turnImpulse = param(ctx, "turnImpulse", 0.0f);
    gpuParams_.jitter = param(ctx, "jitter", 0.4f);
    gpuParams_.depositAmount = param(ctx, "depositAmount", 1.0f);
    gpuParams_.resetPulse = param(ctx, "resetPulse", 0.0f);
    gpuParams_.decayRate = param(ctx, "decayRate", 1.8f);
    gpuParams_.diffuseRate = param(ctx, "diffuseRate", 0.35f);
    gpuParams_.spawnMode = uint32_t(param(ctx, "spawnMode", 0.0f) + 0.5f);
    gpuParams_.attractorWeight = param(ctx, "attractorWeight", 0.0f);
    gpuParams_.flowWeight = param(ctx, "flowWeight", 0.0f);
    gpuParams_.wallY = param(ctx, "wallY", 0.0f) > 0.5f ? 1u : 0u;
    gpuParams_.frameIndex = ctx.frameIndex;

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        gpuParams_.seed = seed_;
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("slimeInit")
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.randomState())
            .uniforms(3, gpuParams_)
            .dispatch1D(gpuParams_.agentCount);

        ctx.graph->pass(instanceName_ + ".clearTrail")
            .pipeline("slimeClearTrail")
            .write(0, trail_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);

        ctx.graph->pass(instanceName_ + ".clearDeposit")
            .pipeline("slimeClearDeposit")
            .buffer(0, deposit_)
            .uniforms(1, gpuParams_)
            .dispatch1D(width_ * height_);

        needsInit_ = false;
    }

    // stepsPerFrame is the module's own speed knob, ctx.substeps the
    // runner's offline quality knob (§17); total = product, same convention
    // as ReactionDiffusion/CellularAutomata.
    uint32_t stepsPerFrame =
        uint32_t(std::max(1.0f, param(ctx, "stepsPerFrame", 1.0f)));
    uint32_t steps = std::max(1u, ctx.substeps) * stepsPerFrame;
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 251u + s));

        ctx.graph->pass(instanceName_ + ".move")
            .pipeline("slimeMove")
            .buffer(0, set_.positions())
            .buffer(1, set_.velocities())
            .buffer(2, set_.randomState())
            .buffer(3, deposit_)
            .read(0, trail_.read())
            .read(1, attractorInput_)
            .read(2, flowInput_)
            .uniforms(4, gpuParams_)
            .uniforms(5, au)
            .dispatch1D(gpuParams_.agentCount);

        ctx.graph->pass(instanceName_ + ".trailUpdate")
            .pipeline("slimeTrailUpdate")
            .read(0, trail_.read())
            .write(1, trail_.write())
            .buffer(0, deposit_)
            .uniforms(1, gpuParams_)
            .dispatch2D(width_, height_);
        trail_.swap();
    }

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", trail_.read(), output_,
                     width_, height_);

    // Beads (below) and strayGain (further below) share ONE atomic_uint
    // width*height buffer (ParticleSplatPass::ensureDensity — the same one
    // showAgents uses) instead of each allocating their own; it must be
    // all-zero before either touches it, so the one-time clear lives here,
    // ahead of both.
    uint32_t beadStride = uint32_t(std::max(0.0f, param(ctx, "beadStride", 0.0f)));
    float strayGain = param(ctx, "strayGain", 0.0f);
    BufferHandle sharedDensity;
    if (beadStride > 0u || strayGain > 0.0f) {
        sharedDensity =
            splat_.ensureDensity(*ctx.graph, instanceName_ + ".splat", width_, height_);
        if (sharedDensityNeedsClear_) {
            ctx.graph->pass(instanceName_ + ".sharedDensityClear")
                .pipeline("slimeClearDeposit")
                .buffer(0, sharedDensity)
                .uniforms(1, gpuParams_)
                .dispatch1D(width_ * height_);
            sharedDensityNeedsClear_ = false;
        }
    }

    // Beads: every beadStride-th agent as a small lit sphere, under the same
    // light as the relief so the two read as one material. Race-free
    // claim/draw/release trio (see Shaders/Particle/SlimeMold.metal) — beads
    // cluster on organism heads and overlap constantly, so a naive
    // read-modify-write there was order-dependent (the actual cause of
    // room-b-sculpt.json's run-to-run non-determinism at scale).
    if (beadStride > 0u) {
        // slimeBeadsClaim/Draw/Release pack the owning bead's thread id into
        // 16 bits of the overlap-resolution key, so at most 65536 beads can
        // be told apart; raise the effective stride (fewer beads) rather
        // than risk two unrelated beads colliding on the same id.
        uint32_t beadCount = (gpuParams_.agentCount + beadStride - 1u) / beadStride;
        if (beadCount > 65536u) {
            beadStride = (gpuParams_.agentCount + 65535u) / 65536u;
            beadCount = (gpuParams_.agentCount + beadStride - 1u) / beadStride;
        }

        BeadParams bp;
        bp.agentCount = gpuParams_.agentCount;
        bp.stride = beadStride;
        bp.width = width_;
        bp.height = height_;
        bp.wallY = gpuParams_.wallY;
        bp.radius = param(ctx, "beadRadius", 5.0f);
        if (colorMap_.reliefEnabled) {
            const auto& r = colorMap_.relief;
            bp.lightX = r.lightX; bp.lightY = r.lightY; bp.lightZ = r.lightZ;
            bp.shininess = r.shininess;
            bp.tintR = r.tintR; bp.tintG = r.tintG; bp.tintB = r.tintB;
            bp.exposure = r.exposure;
            bp.edgeFade = r.edgeFade;
        }
        bp.exposure *= param(ctx, "beadGain", 1.0f);

        ctx.graph->pass(instanceName_ + ".beadsClaim")
            .pipeline("slimeBeadsClaim")
            .buffer(0, set_.positions())
            .buffer(1, sharedDensity)
            .uniforms(2, bp)
            .dispatch1D(beadCount);

        ctx.graph->pass(instanceName_ + ".beadsDraw")
            .pipeline("slimeBeadsDraw")
            .buffer(0, set_.positions())
            .buffer(1, sharedDensity)
            .uniforms(2, bp)
            .write(0, output_)
            .dispatch1D(beadCount);

        ctx.graph->pass(instanceName_ + ".beadsRelease")
            .pipeline("slimeBeadsRelease")
            .buffer(0, set_.positions())
            .buffer(1, sharedDensity)
            .uniforms(2, bp)
            .dispatch1D(beadCount);
    }

    // strayGain: a separate, soft-saturated overlay of ALL agents' raw
    // density (unlike showAgents, always full-population, never gated by
    // the showAgents knob), masked to hide the already-bright organism
    // interiors and show only agents wandering in the dark. Reuses the same
    // sharedDensity buffer as beads above (already cleared / left at zero
    // by whichever of the two ran first this frame).
    if (strayGain > 0.0f) {
        SplatParams accP{};
        accP.particleCount = gpuParams_.agentCount;
        accP.width = width_;
        accP.height = height_;
        accP.weight = 1.0f; // raw agent count per pixel (d = count/256 in the resolve)
        ctx.graph->pass(instanceName_ + ".strayAccumulate")
            .pipeline("splatAccumulate")
            .buffer(0, set_.positions())
            .buffer(1, sharedDensity)
            .uniforms(2, accP)
            .dispatch1D(gpuParams_.agentCount);

        StrayResolveParams sp2;
        sp2.width = width_;
        sp2.height = height_;
        sp2.strayGain = strayGain;
        sp2.strayDensity = param(ctx, "strayDensity", 1.5f);
        sp2.strayMaskLo = param(ctx, "strayMaskLo", 4.0f);
        sp2.strayMaskHi = param(ctx, "strayMaskHi", 14.0f);
        if (colorMap_.reliefEnabled) {
            const auto& r = colorMap_.relief;
            sp2.tintR = r.tintR; sp2.tintG = r.tintG; sp2.tintB = r.tintB;
            sp2.exposure = r.exposure;
            sp2.edgeFade = r.edgeFade;
        }
        ctx.graph->pass(instanceName_ + ".strayResolve")
            .pipeline("strayResolve")
            .buffer(0, sharedDensity)
            .read(0, trail_.read())
            .write(1, output_)
            .uniforms(1, sp2)
            .dispatch2D(width_, height_);
    }

    float showAgents = param(ctx, "showAgents", 0.0f);
    if (showAgents > 0.0f) {
        ParticleSplatPass::Params sp;
        sp.weight = showAgents;
        sp.r = sp.g = sp.b = 1.0f;
        sp.gain = 0.35f;
        splat_.encode(*ctx.graph, instanceName_ + ".splat", set_.positions(),
                     gpuParams_.agentCount, output_, width_, height_, sp);
    }
}

namespace {
nlohmann::json reliefParamsToJson(const ReliefParams& r) {
    return {
        {"channel", r.channel},        {"frame", r.frame},
        {"inputScale", r.inputScale},  {"heightScale", r.heightScale},
        {"lightX", r.lightX},          {"lightY", r.lightY},        {"lightZ", r.lightZ},
        {"ambient", r.ambient},        {"diffuse", r.diffuse},
        {"specular", r.specular},      {"shininess", r.shininess},  {"rim", r.rim},
        {"contourFreq", r.contourFreq},{"contourGain", r.contourGain},
        {"contourWidth", r.contourWidth},
        {"shadowSteps", r.shadowSteps},{"shadowLength", r.shadowLength},
        {"shadowStrength", r.shadowStrength},
        {"tint", {r.tintR, r.tintG, r.tintB}},
        {"grain", r.grain},            {"exposure", r.exposure},
        {"logCurve", r.logCurve},      {"logRange", r.logRange},
        {"edgeFade", r.edgeFade},
    };
}
} // namespace

void SlimeMoldModule::dumpState(SimulationContext& ctx, const std::string& dir,
                                nlohmann::json& meta) {
    std::string err;

    // trail.npy: (H, W) float32 — the CURRENT read side (same one colorMap/
    // relief reads for this frame). R16F texture -> Shared buffer blit ->
    // half->float on CPU.
    {
        BufferDesc bd;
        bd.size = size_t(width_) * height_ * bytesPerPixel(PixelFormat::R16F);
        bd.storage = StorageMode::Shared;
        bd.label = instanceName_ + ".dumpTrailReadback";
        BufferHandle rb = ctx.resources->createBuffer(bd);
        ctx.graph->beginFrame(ctx.frameIndex);
        ctx.graph->copyTextureToBuffer(trail_.read(), rb);
        ctx.graph->endFrame(true);
        const uint16_t* half = static_cast<const uint16_t*>(ctx.resources->bufferContents(rb));
        std::vector<float> trail(size_t(width_) * height_);
        if (half)
            for (size_t i = 0; i < trail.size(); ++i) trail[i] = half2float(half[i]);
        writeNPYFloat32(dir + "/" + instanceName_ + ".trail.npy", trail.data(), trail.size(),
                        {height_, width_}, err);
        ctx.resources->release(rb);
    }

    // positions.npy: (N, 2) float32 — GPUPrivate float2 buffer, already
    // float32 in memory, so the readback buffer IS the npy payload.
    {
        size_t n = gpuParams_.agentCount;
        BufferDesc bd;
        bd.size = size_t(n) * 8;
        bd.storage = StorageMode::Shared;
        bd.label = instanceName_ + ".dumpPosReadback";
        BufferHandle rb = ctx.resources->createBuffer(bd);
        ctx.graph->beginFrame(ctx.frameIndex);
        ctx.graph->copyBufferToBuffer(set_.positions(), rb, bd.size);
        ctx.graph->endFrame(true);
        const float* pos = static_cast<const float*>(ctx.resources->bufferContents(rb));
        writeNPYFloat32(dir + "/" + instanceName_ + ".positions.npy", pos, n * 2,
                        {uint32_t(n), 2u}, err);
        ctx.resources->release(rb);
    }

    meta["relief"] = reliefParamsToJson(colorMap_.relief);
    meta["reliefEnabled"] = colorMap_.reliefEnabled;
    meta["wallY"] = gpuParams_.wallY;
    meta["agentCount"] = gpuParams_.agentCount;

    // Same effective-stride clamp as encode() (kept in sync by hand, not by
    // sharing code, since encode()'s bead block is not itself a function —
    // see life::SlimeMoldModule::encode above for the source of truth).
    uint32_t beadStride = uint32_t(std::max(0.0f, param(ctx, "beadStride", 0.0f)));
    float beadRadius = param(ctx, "beadRadius", 5.0f);
    float beadGain = param(ctx, "beadGain", 1.0f);
    if (beadStride > 0u) {
        uint32_t beadCount = (gpuParams_.agentCount + beadStride - 1u) / beadStride;
        if (beadCount > 65536u) {
            beadStride = (gpuParams_.agentCount + 65535u) / 65536u;
        }
    }
    meta["beadStride"] = beadStride;
    meta["beadRadius"] = beadRadius;
    meta["beadGain"] = beadGain;
    if (beadStride > 0u) {
        BeadParams bp;
        bp.agentCount = gpuParams_.agentCount;
        bp.stride = beadStride;
        bp.width = width_;
        bp.height = height_;
        bp.wallY = gpuParams_.wallY;
        bp.radius = beadRadius;
        if (colorMap_.reliefEnabled) {
            const auto& r = colorMap_.relief;
            bp.lightX = r.lightX; bp.lightY = r.lightY; bp.lightZ = r.lightZ;
            bp.shininess = r.shininess;
            bp.tintR = r.tintR; bp.tintG = r.tintG; bp.tintB = r.tintB;
            bp.exposure = r.exposure;
            bp.edgeFade = r.edgeFade;
        }
        bp.exposure *= beadGain;
        meta["beadParams"] = {
            {"radius", bp.radius},
            {"light", {bp.lightX, bp.lightY, bp.lightZ}},
            {"ambient", bp.ambient}, {"diffuse", bp.diffuse},
            {"specular", bp.specular}, {"shininess", bp.shininess},
            {"tint", {bp.tintR, bp.tintG, bp.tintB}},
            {"exposure", bp.exposure}, {"edgeFade", bp.edgeFade},
        };
    }

    if (!err.empty()) fprintf(stderr, "[life] %s dumpState: %s\n", instanceName_.c_str(), err.c_str());
}

} // namespace life
