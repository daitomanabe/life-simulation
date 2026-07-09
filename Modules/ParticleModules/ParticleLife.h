#pragma once
// Modules/ParticleModules/ParticleLife.h
// Particle Life (design doc §12.5, §23.3 Scene C): K species with a signed
// K×K interaction matrix, spatial-hash neighbor search, species-colored RGB
// splat as the visual output (no ColorMapPass — the splat IS the picture).
// The first consumer of SpatialHashGrid; validates Phase 4's deterministic
// counting-sort pipeline at 100k+ particles.
//
// Scene params: particleCount, species, rMax, beta, forceScale, friction,
// maxSpeed, jitter, forceBoost, shufflePulse, splatGain.
//
// Audio mapping intent (§12.5): kick→forceBoost (attraction boost),
// snare.trigger→shufflePulse (matrix shuffle on rising edge through 0.5),
// hihat→jitter.
//
// Matrix shuffles are seeded by (seed_, shuffleCount_) only, so a capture
// replay reproduces the exact same matrices.

#include "LifeCore/Particle/ParticleSet2D.h"
#include "LifeCore/Sim/SimulationModule.h"
#include "LifeCore/Spatial/SpatialHashGrid.h"

namespace life {

class ParticleLifeModule : public ParticleModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    ParticleSetHandle particles() const override { return set_.handle(); }

private:
    void regenerateMatrix();

    // Mirrors PLParams in Shaders/Particle/ParticleLife.metal.
    struct PLParams {
        uint32_t particleCount = 0;
        uint32_t speciesCount = 6;
        float dt = 1.0f / 60.0f;
        float rMax = 24.0f;
        float beta = 0.3f;
        float forceScale = 60.0f;
        float friction = 4.0f;
        float maxSpeed = 160.0f;
        float jitter = 0.0f;
        float forceBoost = 0.0f;
        uint32_t seed = 0;
        uint32_t frameIndex = 0;
        float worldW = 0.0f;
        float worldH = 0.0f;
        float cellSize = 24.0f;
        uint32_t cellsX = 0;
        uint32_t cellsY = 0;
    };
    static_assert(sizeof(PLParams) == 17 * 4,
                  "PLParams layout must stay scalar-packed to match MSL");

    ParticleSet2D set_;
    SpatialHashGrid grid_;
    BufferHandle matrix_;       // K*K float, Shared (CPU-regenerated on shuffle)
    BufferHandle speciesColor_; // K*4 float, Shared (cosine palette)
    BufferHandle rgb_;          // uint × (w*h*3), fixed-point splat accumulator
    TextureHandle output_;
    PLParams gpuParams_;
    AudioFeatureState audio_{};
    ResourcePool* pool_ = nullptr;
    uint32_t seed_ = 0;
    uint32_t shuffleCount_ = 0;
    float prevShufflePulse_ = 0.0f;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
