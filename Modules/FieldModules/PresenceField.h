#pragma once
// Modules/FieldModules/PresenceField.h
// Sensor-agnostic visitor-presence input -> Field2D. The wall's depth-sensor
// rig isn't chosen yet, so nothing upstream of AudioFeatureState.presence is
// sensor-specific: some future process sends normalized positions over OSC
// (/presence — see LifeCore/Audio/AudioInput.cpp), PresenceStore turns that
// into a per-frame point list, and this module turns the point list into a
// Field2D other simulations already know how to consume (Fluid's
// forceField, SlimeMold's attractorField/flowField, ...). Structure/output
// pattern (namedOutput / outputField / outputTexture, ColorMapPass,
// registration) copied from AudioToField.
//
// Each frame: rasterize every currently-held point (age <= holdSeconds) as
// a soft radial blob — v += strength * exp(-(r/radius)^2), radius in
// METRES converted to pixels via pixelsPerMeter — using the MINIMUM IMAGE
// distance in x (the 93m strip wraps) and plain distance in y (floor/
// ceiling are walls). Asymmetric attack/release smoothing toward that
// target (same one-pole shape as FeatureSmoother's channel envelopes) is
// what also implements the OSC-loss watchdog fade: with zero points (nobody
// held, or /presence has gone silent past watchdogSeconds) the target is
// zero everywhere, and release-rate smoothing fades the whole field to it —
// no separate fade path needed.
//
// SceneRunner normally skips encode() for a layer whose opacity is ~0 (GPU-
// saving "hidden layers cost nothing" rule); this module overrides
// alwaysEncode() so fluid0.forceField etc. keep receiving fresh data every
// frame even while this module's own debug visualization stays invisible
// (opacity 0 is the intended default — raise it only to look at the raw
// field for debugging).
//
// Deterministic: every timing input here is ctx.dt or PresenceStore's
// dt-accumulated age/secondsSinceMessage — never std::chrono — so replaying
// recorded /presence input offline would reproduce byte-identically (no
// capture/replay file format is added by this change; only the timing is
// kept reproducible so one could be added later without touching this
// module).
//
// Scene params: radius (m, default 1.2), pixelsPerMeter (default 176.1 =
// 16380px / 93.0m for this wall — derive per-scene if the strip changes),
// attack (default 6.0 /s), release (default 1.5 /s), strengthScale
// (default 1.0), holdSeconds (default 0.5), watchdogSeconds (default 3.0),
// plus the standard blend/opacity/colorMap a field module has.

#include "LifeCore/Field/Field2D.h"
#include "LifeCore/Render/ColorMapPass.h"
#include "LifeCore/Sim/SimulationModule.h"

namespace life {

class PresenceFieldModule : public FieldModule {
public:
    void setup(SimulationContext& ctx) override;
    void reset(uint32_t seed) override;
    void updateCPU(const AudioFeatureState& audio) override;
    void encode(SimulationContext& ctx) override;
    TextureHandle outputTexture() const override { return output_; }
    TextureHandle outputField() const override { return field_.read(); }

    // Same "field"/"output" coupling ports as AudioToField.
    TextureHandle namedOutput(const std::string& port) const override {
        if (port == "field") return outputField();
        if (port == "output") return outputTexture();
        return {};
    }

    bool alwaysEncode() const override { return true; }

private:
    // Mirrors PresenceFieldParams in Shaders/Field/PresenceField.metal.
    struct PresenceFieldParams {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pointCount = 0;
        float radiusPx = 0.0f;
        float attack = 6.0f;
        float release = 1.5f;
        float dt = 1.0f / 60.0f;
        float strengthScale = 1.0f; // applied on the CPU side already; kept for parity/debug
    };
    static_assert(sizeof(PresenceFieldParams) == 8 * 4,
                  "PresenceFieldParams must stay scalar-packed to match MSL");

    // Mirrors PresenceFieldPoints in the shader: one float4 per tracked
    // point (xPx, yPx, strength, 0 padding), fixed capacity so this buffer's
    // size never changes at runtime.
    struct PresenceFieldPoints {
        float data[kMaxPresencePoints * 4] = {};
    };
    static_assert(sizeof(PresenceFieldPoints) == kMaxPresencePoints * 4 * 4,
                  "PresenceFieldPoints must stay scalar-packed to match MSL");

    Field2D field_;
    TextureHandle output_;
    ColorMapPass colorMap_;
    PresenceFieldParams gpuParams_;
    PresenceFieldPoints gpuPoints_;
    PresenceState presence_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool needsInit_ = true;
    bool watchdogTripped_ = false; // edge-detected so the log line fires once per transition
};

} // namespace life
