#pragma once
// Modules/FieldModules/Fluid.h
// Stable Fluids (design doc §21.1, Stam 1999 / GPU Gems ch.38): audio-driven
// 2D fluid — a velocity field (RG32F) that advects a colored dye field
// (RGBA16F, the visual output) and, via the "velocity" named output,
// optionally drives other modules (first consumer: Boids flowField).
// Toroidal (wrap) boundary throughout — no obstacles (design doc §7.5).
//
// Scene params: velDissipation, dyeDissipation, vorticity, impulse,
// impulseRadius, turbulence, injectHue, jacobiIterations, exposure,
// colorMap is NOT used (dye is already color).
//
// Audio mapping intent (§1.2 "fft deforms the force field"): kick->impulse,
// hihat->turbulence, beat.trigger->injectHue; low/mid/high are transcribed
// straight from AudioFeatureState each frame (not through ParameterBus —
// there is no per-scene base value for them) to drive the fft-band forces in
// fluidForces/fluidAdvectDye.

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class FluidModule : public FieldModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    TextureHandle outputField() const override { return dye_.read(); }

    // Phase 5/8 coupling ports (design doc §10, phase8 §1): "velocity" is
    // the raw RG velocity field (consumed by Boids flowField), "field"/"dye"
    // both expose the dye state, "output" the exposure-only visual.
    TextureHandle namedOutput(const std::string& port) const override {
        if (port == "velocity") return velocity_.read();
        if (port == "field" || port == "dye") return dye_.read();
        if (port == "output") return outputTexture();
        return {};
    }
    // forceField input (Phase 12): a scalar field (e.g. slime trail) whose
    // gradient is added to the velocity field — matter in the coupled field
    // literally stirs the fluid. Fallback = 4x4 black (zero gradient).
    bool bindNamedInput(const std::string& port, TextureHandle h) override {
        if (port == "forceField") {
            forceFieldInput_ = h.valid() ? h : forceFieldFallback_;
            return true;
        }
        return false;
    }

private:
    // Mirrors FluidParams in Shaders/Field/Fluid.metal (this exact order).
    struct FluidParams {
        uint32_t width = 0;
        uint32_t height = 0;
        float dt = 1.0f / 60.0f;
        float velDissipation = 0.05f;
        float dyeDissipation = 0.12f;
        float vorticity = 4.0f;
        float impulse = 0.0f;
        float impulseRadius = 60.0f;
        float turbulence = 0.0f;
        float injectHue = 0.0f;
        float low = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        uint32_t seed = 0;
        uint32_t frameIndex = 0;
        float dyeInject = 0.02f; // dye replacement per frame at impulse center
        float forceFieldGain = 0.0f; // Phase 12: gradient force from forceField input
        uint32_t wallY = 0;          // strip world: floor/ceiling are walls, x still wraps
        float driftX = 0.0f;         // px/s^2 along x, sin profile in y
    };
    static_assert(sizeof(FluidParams) == 19 * 4,
                  "FluidParams layout must stay scalar-packed to match MSL");

    Field2D velocity_;   // RG32F, ping-pong, wrap
    Field2D dye_;        // RGBA16F, ping-pong, wrap
    Field2D pressure_;   // R32F, ping-pong, wrap (warm-started, reset-cleared only)
    Field2D divergence_; // R32F, single (no ping-pong — recomputed every frame)
    TextureHandle output_;
    FluidParams gpuParams_;
    AudioFeatureState audio_{};
    uint32_t seed_ = 0;
    bool needsInit_ = true;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    TextureHandle forceFieldInput_;
    TextureHandle forceFieldFallback_;
};

} // namespace life
