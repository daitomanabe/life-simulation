#pragma once
// Modules/FieldModules/AudioToField.h
// Converts fft[128] into a 2D R16F field (design doc §15.3). This is the
// bridge that later modulates other simulations (local feed maps, kernel
// shaping, attractor fields — Phase 5 couplers); until then its color-mapped
// output already works as a spectral layer.
//
// Modes: 0 scroll (spectrogram history, bins → X, time → Y)
//        1 radial (circular analyzer, bins → angle, magnitude → radius,
//                  exponential decay trail)
// Scene params: mode, gain, decay, colorMap{...}

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class AudioToFieldModule : public FieldModule {
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
    // Mirrors AudioFieldParams in Shaders/Field/AudioToField.metal.
    struct AudioFieldParams {
        uint32_t mode = 0;
        float gain = 1.0f;
        float decay = 0.94f;
        uint32_t seed = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    Field2D field_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    AudioFieldParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
