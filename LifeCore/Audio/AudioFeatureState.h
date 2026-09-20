#pragma once
// LifeCore/Audio/AudioFeatureState.h
// Normalized audio input state (design doc §2.2). OSC is NEVER handed to a
// simulation directly — everything goes through this struct, so replay files
// and live input are interchangeable.

#include <cstdint>

namespace life {

inline constexpr int kFFTBins = 128;

// Per-channel envelope set (design doc §2.4): raw OSC value plus derived
// envelopes, because raw values straight into the GPU look cheap.
struct ChannelEnvelope {
    float raw = 0.0f;      // last received OSC value
    float smoothed = 0.0f; // asymmetric one-pole (fast attack, slow release)
    float peak = 0.0f;     // max-hold with exponential decay
    float trigger = 0.0f;  // 1.0 on rising edge, exponential decay
    float hold = 0.0f;     // 1.0 while raw >= threshold
};

// Sensor-agnostic visitor-presence input (design doc: depth-sensor/LiDAR
// tracking along the wall — hardware not chosen yet, so this only ever
// carries normalized positions, never a sensor-specific payload). Populated
// from /presence OSC via LifeCore/Audio/PresenceStore.h, exactly like fft/
// kick/... are populated from their own addresses via FeatureSmoother.
//
// This struct carries RAW per-point data plus its age (seconds since that
// id was last refreshed, dt-accumulated by PresenceStore — never
// std::chrono, so replay stays reproducible). It does NOT decide which
// points still count or whether the input has gone silent — that policy
// (holdSeconds / watchdogSeconds) belongs to the consuming module
// (PresenceField), which is configured per scene.
inline constexpr int kMaxPresencePoints = 64;

struct PresencePoint {
    uint32_t id = 0;        // from OSC; lets a mover keep identity across frames
    float x = 0.0f;         // normalized 0..1 along the wall's length (wraps)
    float y = 0.0f;         // normalized 0..1, 0 = ceiling, 1 = floor (image y-down)
    float strength = 1.0f;  // OSC's optional 4th float; defaults to 1.0 when absent
    float age = 0.0f;       // seconds since last refreshed for this id (dt-accumulated)
};

struct PresenceState {
    PresencePoint points[kMaxPresencePoints] = {};
    uint32_t count = 0;                  // populated entries in points[]
    float secondsSinceMessage = 1.0e9f;  // dt-accumulated time since the last /presence or
                                         // /presence/clear of any kind ("connection alive?");
                                         // huge default reads as "never seen one"
};

struct AudioFeatureState {
    // Primary channels (smoothed values — safe defaults for mappings).
    float kick = 0.0f;
    float snare = 0.0f;
    float hihat = 0.0f;
    float perc = 0.0f;
    float beat = 0.0f;

    float fft[kFFTBins] = {};

    // Features derived from fft (design doc §2.3).
    float rms = 0.0f;
    float low = 0.0f;      // mean of fft[0..15]
    float mid = 0.0f;      // mean of fft[16..63]
    float high = 0.0f;     // mean of fft[64..127]
    float centroid = 0.0f; // spectral centroid, normalized 0..1
    float flux = 0.0f;     // positive spectral difference vs previous frame

    float time = 0.0f;
    float deltaTime = 0.0f;
    uint32_t frameIndex = 0;

    // Full envelope sets, addressable from audio mappings as e.g.
    // "kick.trigger", "snare.peak" (plain "kick" == smoothed).
    ChannelEnvelope kickEnv, snareEnv, hihatEnv, percEnv, beatEnv;

    // Visitor presence (see PresenceState above). Reuses this same struct's
    // plumbing (SceneRunner::step -> module->updateCPU(audio)) instead of a
    // parallel per-frame channel, so no module signature changes — every
    // existing module already takes AudioFeatureState and simply ignores
    // fields it doesn't use, exactly as it does today with fft/kick/etc.
    // Default-constructed (count 0) for every caller that doesn't run a
    // live OSC receiver (LifeBench's "silent" state, LifeOfflineRender with
    // or without --audio, CaptureReplay — none of them populate it), which
    // is what keeps those paths byte-identical before and after this field
    // was added.
    PresenceState presence;
};

} // namespace life
