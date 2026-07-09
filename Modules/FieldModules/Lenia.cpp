// Modules/FieldModules/Lenia.cpp
#include "Modules/FieldModules/Lenia.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <cmath>
#include <vector>

namespace life {

void LeniaModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    fd.format = PixelFormat::R32F;
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

    gpuParams_.radius = params_.value("radius", 13u);
    gpuParams_.kernelSize = gpuParams_.radius * 2 + 1;
    gpuParams_.initCoverage = params_.value("initCoverage", 0.4f);
    gpuParams_.initScale = params_.value("initScale", 24.0f);
    gpuParams_.width = width_;
    gpuParams_.height = height_;
    kernelShellMu_ = params_.value("kernelShellMu", 0.5f);
    kernelShellSigma_ = params_.value("kernelShellSigma", 0.15f);

    // Lenia looks best on dark→bright organic palettes; override per scene.
    colorMap_.params.dR = 0.60f;
    colorMap_.params.dG = 0.45f;
    colorMap_.params.dB = 0.30f;
    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.0f;
    colorMap_.configure(params_);

    buildKernelTexture(ctx);
}

void LeniaModule::buildKernelTexture(SimulationContext& ctx) {
    // Radial gaussian shell K(r) = bell(r, mu, sigma), r normalized to [0,1],
    // normalized so the kernel sums to 1 (potential u stays in [0,1] for
    // state in [0,1]).
    const uint32_t R = gpuParams_.radius;
    const uint32_t size = gpuParams_.kernelSize;
    std::vector<float> weights(size_t(size) * size, 0.0f);

    double sum = 0.0;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = float(int(x) - int(R));
            float dy = float(int(y) - int(R));
            float r = std::sqrt(dx * dx + dy * dy) / float(R);
            float w = 0.0f;
            if (r <= 1.0f && r > 0.0f) {
                float t = (r - kernelShellMu_) / kernelShellSigma_;
                w = std::exp(-0.5f * t * t);
            }
            weights[size_t(y) * size + x] = w;
            sum += w;
        }
    }
    if (sum > 0.0) {
        float inv = float(1.0 / sum);
        for (auto& w : weights) w *= inv;
    }

    TextureDesc kd;
    kd.width = size;
    kd.height = size;
    kd.format = PixelFormat::R32F;
    kd.storage = StorageMode::Shared; // CPU-uploaded LUT
    kd.label = instanceName_ + ".kernel";
    kernel_ = ctx.resources->createTexture(kd);
    ctx.resources->uploadTexture(kernel_, weights.data(), size * sizeof(float));
}

void LeniaModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x4C454E31); // "LEN1"
    needsInit_ = true;
}

void LeniaModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void LeniaModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = param(ctx, "dt", 0.1f);
    gpuParams_.growthMu = param(ctx, "growthMu", 0.15f);
    gpuParams_.growthSigma = param(ctx, "growthSigma", 0.017f);
    gpuParams_.noiseAmount = param(ctx, "noiseAmount", 0.0f);
    gpuParams_.muJitter = param(ctx, "muJitter", 0.0f);
    gpuParams_.injectAmount = param(ctx, "injectAmount", 0.0f);
    gpuParams_.injectCount = uint32_t(param(ctx, "injectCount", 3.0f) + 0.5f);
    gpuParams_.frameIndex = ctx.frameIndex;
    gpuParams_.growthMode = uint32_t(param(ctx, "growthMode", 1.0f) + 0.5f);

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        gpuParams_.seed = seed_;
        ctx.graph->pass(instanceName_ + ".init")
            .pipeline("leniaInit")
            .write(0, field_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    uint32_t steps = std::max(1u, ctx.substeps);
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 197u + s));
        ctx.graph->pass(instanceName_ + ".step")
            .pipeline("leniaStep")
            .read(0, field_.read())
            .write(1, field_.write())
            .read(2, kernel_)
            .uniforms(0, gpuParams_)
            .uniforms(1, au)
            .dispatch2D(width_, height_);
        field_.swap();
    }

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                     width_, height_);
}

} // namespace life
