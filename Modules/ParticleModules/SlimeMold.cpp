// Modules/ParticleModules/SlimeMold.cpp
#include "Modules/ParticleModules/SlimeMold.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <algorithm>
#include <vector>

namespace life {

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

} // namespace life
