#pragma once
// Modules/ParticleModules/Boids.h
// Boids flocking (design doc §12.6, Reynolds 1987): separation / alignment /
// cohesion over the SpatialHashGrid 3×3 neighborhood, heading-hue RGB splat
// as the visual output (no ColorMapPass). Species buffer unused (stays 0).
//
// Scene params: particleCount, radius, sepWeight, aliWeight, cohWeight,
// minSpeed, maxSpeed, jitter, sepBoost, impulse, scatterPulse, splatGain.
//
// Audio mapping intent (§12.6): kick→sepBoost (separation burst),
// perc→impulse (direction impulse), snare.trigger→scatterPulse (scatter),
// hihat→jitter.

#include "LifeCore/Particle/ParticleSet2D.h"
#include "LifeCore/Sim/SimulationModule.h"
#include "LifeCore/Spatial/SpatialHashGrid.h"

namespace life {

class BoidsModule : public ParticleModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    ParticleSetHandle particles() const override { return set_.handle(); }

    // Phase 5 coupling port (design doc §10): the splat IS the state, so
    // only "output" is meaningful (no separate "field").
    TextureHandle namedOutput(const std::string& port) const override {
        if (port == "output") return outputTexture();
        return {};
    }

private:
    // Mirrors BoidsParams in Shaders/Particle/Boids.metal.
    struct BoidsParams {
        uint32_t particleCount = 0;
        float dt = 1.0f / 60.0f;
        float radius = 14.0f;
        float sepWeight = 60.0f;
        float aliWeight = 25.0f;
        float cohWeight = 18.0f;
        float minSpeed = 40.0f;
        float maxSpeed = 140.0f;
        float jitter = 0.0f;
        float sepBoost = 0.0f;
        float impulse = 0.0f;
        float scatterPulse = 0.0f;
        uint32_t seed = 0;
        uint32_t frameIndex = 0;
        float worldW = 0.0f;
        float worldH = 0.0f;
        float cellSize = 14.0f;
        uint32_t cellsX = 0;
        uint32_t cellsY = 0;
        float sepRadiusFrac = 0.35f;
    };
    static_assert(sizeof(BoidsParams) == 20 * 4,
                  "BoidsParams layout must stay scalar-packed to match MSL");

    ParticleSet2D set_;
    SpatialHashGrid grid_;
    BufferHandle rgb_; // uint × (w*h*3), fixed-point splat accumulator
    TextureHandle output_;
    BoidsParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
