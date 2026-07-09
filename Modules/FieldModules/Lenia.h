#pragma once
// Modules/FieldModules/Lenia.h
// Single-channel Lenia (design doc §12.2, Chan 2018 arXiv:1812.05433).
// v1 spec: single radial kernel, direct convolution, wrap boundary.
// The kernel shell is precomputed on CPU (deterministic, normalized to
// Σ=1) into a small R32F texture; convolution + growth + update run fused
// in one compute pass.
//
// Phase 9 (docs/specs/phase9_lenia_fft.md) adds an FFT convolution path
// (convMode "fft") with an optional simWidth/simHeight sim-resolution
// separate from the scene/output resolution, and up to 4 simultaneous
// radial kernels ("kernels" param) summed in the growth function. convMode
// defaults to "direct" and the direct path — leniaInit/leniaStep in
// Shaders/Field/Lenia.metal, buildKernelTexture() below — is byte-for-byte
// unchanged (backward-compat contract: lenia_basic.json's rendered output
// must not move at all).
//
// Phase 11 (docs/specs/phase11_organisms.md) adds an "organism stamp init"
// path: initMode "stamp" places a known Lenia creature's cells (imported
// from the official Chakazul/Lenia catalogue via
// tools/import_lenia_organism.py, see Presets/organisms/*.json) into the
// field at up to 32 random positions/orientations via a new leniaStampInit
// kernel, instead of leniaInit's noise soup. leniaInit/leniaStep are again
// untouched (same backward-compat contract). useOrganismParams additionally
// lets the organism file's own R/mu/sigma/T (dt=1/T) parameters become this
// module's radius/growthMu/growthSigma/dt *defaults* (an explicit scene
// param still wins) — mirrors the phase9 kernels[0]-fallback pattern below.
//
// Scene params: radius (px), dt, growthMu, growthSigma, kernelShellMu,
// kernelShellSigma, initCoverage, initScale, noiseAmount, muJitter,
// colorMap{...}; phase9 adds simWidth, simHeight, convMode ("direct"|"fft"),
// kernels (array of {radiusScale, mu, sigma, weight, betas[]}, max 4);
// phase11 adds initMode ("noise"|"stamp"), organism (path to an organism
// JSON, required when initMode="stamp"), stampCount (default 6, max 32),
// stampRotate (bool, default true), useOrganismParams (bool, default true;
// mutually exclusive with "kernels" — that's a setup error, see setup()).
//
// Audio mapping intent (§12.2): kick→growth rate, hihat→noise injection,
// perc→local disturbance, fft→kernel/growth modulation (via mappings onto
// the params above; e.g. kick→dtBoost, hihat→noiseAmount, snare→muJitter).

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

#include <vector>

namespace life {

class LeniaModule : public FieldModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    // Sim-resolution field (simWidth_ x simHeight_, which equals the scene
    // resolution unless simWidth/simHeight params override it — phase9 §1).
    // Coupling consumers read this through sampleFieldWrap (normalized
    // coords), which is correct regardless of the field's actual pixel size
    // (verified property, see Shaders/Common/Sampling.metal), so this port
    // needs no special-casing for the sim/scene resolution split.
    TextureHandle outputField() const override { return field_.read(); }

    // Phase 5 coupling ports (design doc §10): "field" is the raw state,
    // "output" the color-mapped RGBA visual.
    TextureHandle namedOutput(const std::string& port) const override {
        if (port == "field") return outputField();
        if (port == "output") return outputTexture();
        return {};
    }

private:
    // Mirrors LeniaParams in Shaders/Field/Lenia.metal. Unchanged by phase9
    // (backward-compat contract) — the FFT path uses the separate
    // LeniaMultiParams struct below instead of extending this one.
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

    // One entry of the scene "kernels" array (phase9 §4). betas.size() <= 4
    // in practice (kernel shell "rings"); radiusScale/mu/sigma/weight mirror
    // the JSON fields 1:1.
    struct KernelDef {
        float radiusScale = 1.0f;
        float mu = 0.15f;
        float sigma = 0.017f;
        float weight = 1.0f;
        std::vector<float> betas{1.0f};
    };

    // Mirrors LeniaMultiParams in Shaders/Field/Lenia.metal (this exact
    // order) — the leniaGrowthMulti kernel's per-kernel growth parameters.
    // Deliberately separate from LeniaParams above (phase9 spec: "既存
    // LeniaParams のレイアウトは変えず").
    struct LeniaMultiParams {
        float dt = 0.1f;
        float kMu[4] = {0.15f, 0.15f, 0.15f, 0.15f};
        float kSigma[4] = {0.017f, 0.017f, 0.017f, 0.017f};
        float kWeight[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        uint32_t numKernels = 1;
        uint32_t growthMode = 1;
        float noiseAmount = 0.0f;
        float muJitter = 0.0f;
        float injectAmount = 0.0f;
        uint32_t injectCount = 3;
        uint32_t radius = 13;
        uint32_t seed = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameIndex = 0;
    };
    static_assert(sizeof(LeniaMultiParams) == 24 * 4,
                 "LeniaMultiParams layout must stay scalar-packed to match MSL");

    // Mirrors LeniaStampParams in Shaders/Field/Lenia.metal (this exact
    // order) — leniaStampInit's parameters (phase11 §"Lenia モジュール拡張").
    // Stamp positions/orientations are re-derived from `seed` inside the
    // kernel (rand01(i, ...) hashing), so this struct only needs the sizes
    // + counts, not per-stamp data.
    struct LeniaStampParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t orgWidth = 0;
        uint32_t orgHeight = 0;
        uint32_t stampCount = 6;
        uint32_t stampRotate = 1; // bool banned in uniform structs (§constraint 2)
        uint32_t seed = 0;
    };
    static_assert(sizeof(LeniaStampParams) == 7 * 4,
                 "LeniaStampParams layout must stay scalar-packed to match MSL");

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

    // ---- phase9: sim-resolution separation + FFT convolution ----
    uint32_t simWidth_ = 0;
    uint32_t simHeight_ = 0;
    bool useFFT_ = false;
    std::vector<KernelDef> kernels_; // empty unless "kernels" scene param given
    LeniaMultiParams multiParams_{};
    // Per-frame param() fallbacks for growthMu/growthSigma (§4 direct-mode
    // fallback when "kernels" is given without convMode=fft: kernels[0]'s
    // mu/sigma become the new *default*, but an explicit scene "growthMu"/
    // "growthSigma" still wins — see encode()). Equal to 0.15/0.017 (the
    // module's normal defaults) unless that fallback path is taken.
    float fallbackGrowthMu_ = 0.15f;
    float fallbackGrowthSigma_ = 0.017f;

    // FFT-mode-only GPU resources (created in setup() only when useFFT_).
    TextureHandle fftPingA_, fftPingB_;  // RG32F scratch, simW x simH
    TextureHandle stateFFTStable_;       // RG32F, this substep's state spectrum
    TextureHandle kernelFFT_[4];         // RG32F, one per kernel slot (persistent)
    TextureHandle potential_[4];         // R32F, one per kernel slot (recomputed/frame)
    TextureHandle kernelImageUpload_;    // R32F Shared, CPU staging for kernel images

    // ---- phase11: organism stamp init (docs/specs/phase11_organisms.md) ----
    bool useStampInit_ = false;    // initMode == "stamp" AND organism loaded OK
    bool organismLoaded_ = false;  // organism JSON loaded + validated (setup())
    bool stampRotate_ = true;
    uint32_t stampCount_ = 6;
    uint32_t orgWidth_ = 0, orgHeight_ = 0;
    TextureHandle organismCells_;  // R32F Shared, orgWidth_ x orgHeight_, uploaded once
    // Organism-derived "dt" default (1/T) when useOrganismParams applies —
    // same fallback-default pattern as fallbackGrowthMu_/fallbackGrowthSigma_
    // above, just for a different param and trigger (organism file, not
    // kernels[0]). An explicit scene "dt" still wins (see encode()).
    float fallbackDt_ = 0.1f;
};

} // namespace life
