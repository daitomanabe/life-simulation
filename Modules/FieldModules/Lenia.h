#pragma once
// Modules/FieldModules/Lenia.h
// Single-channel Lenia (design doc §12.2, Chan 2018 arXiv:1812.05433).
// v1 spec: single radial kernel, direct convolution, wrap boundary.
// The kernel shell is precomputed on CPU (deterministic, normalized to
// Σ=1) into a small R32F texture; convolution + growth + update run fused
// in one compute pass. FFT convolution and multi-kernel come later.
//
// Scene params: radius (px), dt, growthMu, growthSigma, kernelShellMu,
// kernelShellSigma, initCoverage, initScale, noiseAmount, muJitter,
// colorMap{...}
//
// Audio mapping intent (§12.2): kick→growth rate, hihat→noise injection,
// perc→local disturbance, fft→kernel/growth modulation (via mappings onto
// the params above; e.g. kick→dtBoost, hihat→noiseAmount, snare→muJitter).

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class LeniaModule : public FieldModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    TextureHandle outputField() const override { return field_.read(); }

    // Phase 5 coupling ports (design doc §10): "field" is the raw state,
    // "output" the color-mapped RGBA visual.
    TextureHandle namedOutput(const std::string& port) const override {
        if (port == "field") return outputField();
        if (port == "output") return outputTexture();
        return {};
    }

private:
    // Mirrors LeniaParams in Shaders/Field/Lenia.metal.
    struct LeniaParams {
        float dt = 0.1f;
        float growthMu = 0.15f;
        float growthSigma = 0.017f;
        float noiseAmount = 0.0f;
        float muJitter = 0.0f;
        float initCoverage = 0.4f;
        float initScale = 24.0f;
        float injectAmount = 0.0f;
        uint32_t injectCount = 3;
        uint32_t radius = 13;
        uint32_t kernelSize = 27; // 2*radius + 1
        uint32_t seed = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
        uint32_t growthMode = 1;
    };

    void buildKernelTexture(SimulationContext& ctx);

    Field2D field_;
    TextureHandle kernel_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    LeniaParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float kernelShellMu_ = 0.5f;
    float kernelShellSigma_ = 0.15f;
};

} // namespace life
