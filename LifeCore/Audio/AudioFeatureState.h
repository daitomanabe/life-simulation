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
};

} // namespace life
