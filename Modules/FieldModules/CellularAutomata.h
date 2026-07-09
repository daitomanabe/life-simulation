#pragma once
// Modules/FieldModules/CellularAutomata.h
// Discrete cellular automata layer (design doc §12.3): fast pattern source /
// mask / glitch layer. Rules: 0 Game of Life, 1 Brian's Brain, 2 Seeds,
// 3 Cyclic CA. State lives in an R16F field (cell value 0..1; Brian's Brain
// uses 1.0 firing / 0.5 refractory; cyclic stores state/N).
//
// Scene params: rule, cyclicStates, stepsPerFrame (CA can run faster than
// the frame rate), injectAmount, colorMap{...}
// Audio intent: hihat→injectAmount (random cell injection), snare→rule
// switch (map snare.trigger→rule with scale), beat→rule morph later.

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class CellularAutomataModule : public FieldModule {
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
    // Mirrors CAParams in Shaders/Field/CellularAutomata.metal.
    struct CAParams {
        uint32_t rule = 0;
        uint32_t cyclicStates = 12;
        float injectAmount = 0.0f;
        float initDensity = 0.18f;
        uint32_t seed = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    Field2D field_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    CAParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
