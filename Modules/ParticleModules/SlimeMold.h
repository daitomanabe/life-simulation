#pragma once
// Modules/ParticleModules/SlimeMold.h
// Physarum slime-mold agents (design doc §12.4, Jones 2010): a ParticleSet2D
// of agents senses/deposits into a shared trail Field2D. The first module to
// bridge Particle Core and Field2D Core (design doc §23.2 Scene B validates
// "particle reads field, particle writes field").
//
// Scene params: agentCount, moveSpeed, sensorAngle, sensorBoost,
// sensorDistance, turnSpeed, turnImpulse, jitter, depositAmount, resetPulse,
// decayRate, diffuseRate, spawnMode, showAgents, stepsPerFrame,
// colorMap{...}
//
// Audio mapping intent (§12.4): kick→depositAmount, snare.trigger→
// sensorBoost, hihat→jitter, perc→turnImpulse, beat.trigger→resetPulse.

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Particle/ParticleSet2D.h"
#include "LifeCore/Particle/ParticleSplatPass.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class SlimeMoldModule : public ParticleModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    ParticleSetHandle particles() const override { return set_.handle(); }

    // Trail read side — its own method (not part of ParticleModule), for
    // Phase 5 field couplers to sample.
    TextureHandle outputField() const { return trail_.read(); }

private:
    // Mirrors SlimeParams in Shaders/Particle/SlimeMold.metal.
    struct SlimeParams {
        uint32_t agentCount = 0;
        float dt = 1.0f / 60.0f;
        float moveSpeed = 55.0f;
        float sensorAngle = 0.6f;
        float sensorBoost = 0.0f;
        float sensorDistance = 9.0f;
        float turnSpeed = 10.0f;
        float turnImpulse = 0.0f;
        float jitter = 0.4f;
        float depositAmount = 1.0f;
        float resetPulse = 0.0f;
        float decayRate = 1.8f;
        float diffuseRate = 0.35f;
        uint32_t spawnMode = 0;
        uint32_t seed = 0;
        uint32_t frameIndex = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    ParticleSet2D set_;
    Field2D trail_;
    BufferHandle deposit_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    ParticleSplatPass splat_;
    SlimeParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace life
