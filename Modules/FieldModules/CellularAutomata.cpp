// Modules/FieldModules/CellularAutomata.cpp
#include "Modules/FieldModules/CellularAutomata.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

namespace life {

void CellularAutomataModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    fd.format = PixelFormat::R16F;
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

    gpuParams_.initDensity = params_.value("initDensity", 0.18f);
    gpuParams_.cyclicStates = params_.value("cyclicStates", 12u);
    gpuParams_.width = width_;
    gpuParams_.height = height_;

    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.0f;
    colorMap_.configure(params_);
}

void CellularAutomataModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x43414d31); // "CAM1"
    needsInit_ = true;
}

void CellularAutomataModule::updateCPU(const AudioFeatureState& audio) {
    audio_ = audio;
}

void CellularAutomataModule::encode(SimulationContext& ctx) {
    gpuParams_.rule = uint32_t(param(ctx, "rule", 0.0f) + 0.5f);
    gpuParams_.injectAmount = param(ctx, "injectAmount", 0.0f);

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        gpuParams_.seed = seed_;
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("caInit")
            .write(0, field_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    uint32_t stepsPerFrame =
        uint32_t(std::max(1.0f, param(ctx, "stepsPerFrame", 1.0f)));
    uint32_t steps = std::max(1u, ctx.substeps) * stepsPerFrame;
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 389u + s));
        ctx.graph->pass(instanceName_ + ".step")
            .pipeline("caStep")
            .read(0, field_.read())
            .write(1, field_.write())
            .uniforms(0, gpuParams_)
            .uniforms(1, au)
            .dispatch2D(width_, height_);
        field_.swap();
    }

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                     width_, height_);
}

} // namespace life
