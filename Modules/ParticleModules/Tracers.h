#pragma once
// Modules/ParticleModules/Tracers.h
// Passive tracer particles: no neighbour interaction, carried by an external
// flow field (e.g. fluid0.velocity) plus jitter, respawning biased toward
// bright emitField regions (e.g. slime0.trail) so the sparse dark ~70% of
// the strip reads as fine drifting particles instead of flat black.
//
// Same buffer-reuse convention as SlimeMold: ParticleSet2D::velocities() is
// repurposed to hold age/life (float2) instead of an actual velocity, since
// Tracers never ping-pongs and has no real per-particle velocity state to
// keep across frames (species/attributes stay unused, same as SlimeMold).
//
// Scene params: particleCount, lifetime, emitBias, emitThreshold, flowWeight,
// jitter, bokehRadius, gain, density, tint, edgeFade, wallY, plus the
// standard layer params blend/opacity (handled by SceneRunner/CompositePass).

#include "LifeCore/Particle/ParticleSet2D.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class TracersModule : public ParticleModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    void dumpState(SimulationContext& ctx, const std::string& dir,
                   nlohmann::json& meta) override;
    bool loadState(SimulationContext& ctx, const std::string& dir,
                  const nlohmann::json& meta) override;
    TextureHandle outputTexture() const override { return output_; }
    ParticleSetHandle particles() const override { return set_.handle(); }

    // flowField (RG velocity, typically fluid0.velocity) / emitField (scalar,
    // typically slime0.trail) input ports. Always bound — fall back to a 4x4
    // black texture when no scene connection targets the port, same pattern
    // as SlimeMold's attractorField/flowField.
    bool bindNamedInput(const std::string& port, TextureHandle h) override {
        if (port == "flowField") {
            flowInput_ = h.valid() ? h : flowFallback_;
            return true;
        }
        if (port == "emitField") {
            emitInput_ = h.valid() ? h : emitFallback_;
            return true;
        }
        return false;
    }

private:
    // Mirrors TracersParams in Shaders/Particle/Tracers.metal.
    struct TracersParams {
        uint32_t particleCount = 0;
        float dt = 1.0f / 60.0f;
        float lifetime = 12.0f;
        float emitBias = 0.85f;
        float emitThreshold = 4.0f;
        float flowWeight = 1.0f;
        float jitter = 8.0f;
        uint32_t wallY = 0;
        uint32_t seed = 0;
        uint32_t frameIndex = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    static_assert(sizeof(TracersParams) == 12 * 4,
                  "TracersParams must stay scalar-packed to match MSL");

    // Mirrors TracersSplatParams in Shaders/Particle/Tracers.metal.
    struct TracersSplatParams {
        uint32_t particleCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t wallY = 0;
        float bokehRadius = 3.5f;
    };
    static_assert(sizeof(TracersSplatParams) == 5 * 4,
                  "TracersSplatParams must stay scalar-packed to match MSL");

    // Mirrors TracersResolveParams in Shaders/Particle/Tracers.metal.
    struct TracersResolveParams {
        uint32_t width = 0;
        uint32_t height = 0;
        float gain = 0.9f;
        float density = 0.6f;
        float tintR = 0.85f, tintG = 0.93f, tintB = 1.0f;
        float edgeFade = 36.0f;
    };
    static_assert(sizeof(TracersResolveParams) == 8 * 4,
                  "TracersResolveParams must stay scalar-packed to match MSL");

    ParticleSet2D set_;
    BufferHandle density_;  // atomic_uint × width*height, fixed-point (scale 256)
    TextureHandle output_;
    TracersParams gpuParams_;
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float tintR_ = 0.85f, tintG_ = 0.93f, tintB_ = 1.0f; // read once at configure
    TextureHandle flowInput_;
    TextureHandle flowFallback_;
    TextureHandle emitInput_;
    TextureHandle emitFallback_;
};

} // namespace life
