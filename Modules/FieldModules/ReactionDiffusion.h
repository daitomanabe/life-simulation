#pragma once
// Modules/FieldModules/ReactionDiffusion.h
// Gray-Scott reaction diffusion (design doc §12.1). The Phase 1 reference
// module: 2-channel RG32F ping-pong field, laplacian + feed/kill, audio
// modulation of the RULES (feed via kick, kill perturbation via snare, state
// noise via hihat) — not just the colors.
//
// Scene params (all audio-mappable through ParameterBus):
//   feed, kill, Du, Dv, timeScale, noiseAmount, killPerturb,
//   initSpots, initSpotRadius, colorMap{...}, blend, opacity

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class ReactionDiffusionModule : public FieldModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    TextureHandle outputField() const override { return field_.read(); }

private:
    // Mirrors RDParams in Shaders/Field/ReactionDiffusion.metal.
    struct RDParams {
        float Du = 1.0f;
        float Dv = 0.5f;
        float feed = 0.037f;
        float kill = 0.061f;
        float dt = 1.0f;
        float noiseAmount = 0.0f;
        float killPerturb = 0.0f;
        uint32_t seed = 0;
        uint32_t initSpots = 12;
        float initSpotRadius = 0.02f;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    Field2D field_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    RDParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
