// Modules/FieldModules/AudioToField.cpp
#include "Modules/FieldModules/AudioToField.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

namespace life {

void AudioToFieldModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    fd.format = PixelFormat::R16F;
    fd.pingPong = true;
    fd.boundary = BoundaryMode::Clamp;
    fd.label = instanceName_ + ".field";
    field_.create(*ctx.resources, fd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    gpuParams_.mode = params_.value("mode", 0u);
    gpuParams_.width = width_;
    gpuParams_.height = height_;

    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.0f;
    colorMap_.configure(params_);
}

void AudioToFieldModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x41544631); // "ATF1"
    needsInit_ = true;
}

void AudioToFieldModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void AudioToFieldModule::encode(SimulationContext& ctx) {
    gpuParams_.gain = param(ctx, "gain", 1.0f);
    gpuParams_.decay = param(ctx, "decay", 0.94f);
    gpuParams_.seed = seed_;

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        ctx.graph->pass(instanceName_ + ".clear")
            .pipeline("audioFieldClear")
            .write(0, field_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    ctx.graph->pass(instanceName_ + ".update")
        .pipeline("audioFieldUpdate")
        .read(0, field_.read())
        .write(1, field_.write())
        .uniforms(0, gpuParams_)
        .uniforms(1, au)
        .dispatch2D(width_, height_);
    field_.swap();

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                     width_, height_);
}

} // namespace life
