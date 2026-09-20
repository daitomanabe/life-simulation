// Modules/FieldModules/ReactionDiffusion.cpp
#include "Modules/FieldModules/ReactionDiffusion.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <vector>

namespace life {

void ReactionDiffusionModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    // RG32F: Gray-Scott is sensitive around the feed/kill boundary; half
    // precision drifts visibly on long offline renders.
    fd.format = PixelFormat::RG32F;
    fd.pingPong = true;
    fd.boundary = BoundaryMode::Wrap;
    fd.label = instanceName_ + ".state";
    field_.create(*ctx.resources, fd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    // 4x4 black fallback for the feedMap input port (Phase 5 §3b / design
    // point 3): always bound so rdStep's texture argument is valid even
    // when no scene connection targets "feedMap". Shared storage because
    // uploadTexture requires CPU-visible memory (GPUPrivate can't be
    // written from the CPU).
    TextureDesc fbd;
    fbd.width = 4;
    fbd.height = 4;
    fbd.format = PixelFormat::R16F;
    fbd.storage = StorageMode::Shared;
    fbd.label = instanceName_ + ".feedMapFallback";
    feedMapFallback_ = ctx.resources->createTexture(fbd);
    std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fbd.format), 0);
    ctx.resources->uploadTexture(feedMapFallback_, zeros.data(),
                                 size_t(fbd.width) * bytesPerPixel(fbd.format));
    feedMapInput_ = feedMapFallback_;

    colorMap_.configure(params_);
    gpuParams_.initSpots = params_.value("initSpots", 12u);
    gpuParams_.initSpotRadius = params_.value("initSpotRadius", 0.02f);
    gpuParams_.width = width_;
    gpuParams_.height = height_;
}

void ReactionDiffusionModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x52444d31); // "RDM1"
    needsInit_ = true;
}

void ReactionDiffusionModule::updateCPU(const AudioFeatureState& audio) {
    audio_ = audio;
}

void ReactionDiffusionModule::encode(SimulationContext& ctx) {
    // Final (audio-modulated) rule values for this frame, via ParameterBus.
    gpuParams_.Du = param(ctx, "Du", 1.0f);
    gpuParams_.Dv = param(ctx, "Dv", 0.5f);
    gpuParams_.feed = param(ctx, "feed", 0.037f);
    gpuParams_.kill = param(ctx, "kill", 0.061f);
    gpuParams_.noiseAmount = param(ctx, "noiseAmount", 0.0f);
    gpuParams_.killPerturb = param(ctx, "killPerturb", 0.0f);
    gpuParams_.feedMapGain = param(ctx, "feedMapGain", 0.0f);
    gpuParams_.seed = seed_;
    // Gray-Scott's canonical explicit-Euler step is dt=1.0 per iteration;
    // timeScale lets scenes slow it down, substeps raise offline quality.
    float timeScale = param(ctx, "timeScale", 1.0f);
    gpuParams_.dt = timeScale;

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("rdInit")
            .write(0, field_.read()) // init writes the CURRENT read side
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    // RD needs several iterations per visual frame to evolve at a usable
    // pace; stepsPerFrame is the module's own speed knob, ctx.substeps the
    // runner's offline quality knob (§17). Total = product.
    uint32_t stepsPerFrame = uint32_t(std::max(1.0f, param(ctx, "stepsPerFrame", 8.0f)));
    uint32_t steps = std::max(1u, ctx.substeps) * stepsPerFrame;
    for (uint32_t s = 0; s < steps; ++s) {
        // Per-substep frame tag keeps the GPU noise stream advancing.
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 131u + s));
        ctx.graph->pass(instanceName_ + ".step")
            .pipeline("rdStep")
            .read(0, field_.read())
            .write(1, field_.write())
            .read(2, feedMapInput_)
            .uniforms(0, gpuParams_)
            .uniforms(1, au)
            .dispatch2D(width_, height_);
        field_.swap();
    }

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                     width_, height_, ctx.dt);
}

} // namespace life
