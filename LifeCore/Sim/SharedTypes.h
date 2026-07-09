#pragma once
// LifeCore/Sim/SharedTypes.h
// Uniform structs shared with Metal shaders. Layout must match
// Shaders/Common/CommonTypes.metal exactly — scalar floats/uints only, no
// simd types, so C++ and MSL agree without alignment surprises.

#include "LifeCore/Audio/AudioFeatureState.h"

#include <cstdint>

namespace life {

// Mirrors `AudioUniforms` in CommonTypes.metal (design doc §6.5).
struct AudioUniforms {
    float kick;
    float snare;
    float hihat;
    float perc;
    float beat;

    float rms;
    float low;
    float mid;
    float high;
    float centroid;
    float flux;

    float time;
    float deltaTime;

    float fft[kFFTBins];

    uint32_t frameIndex;
};

static_assert(sizeof(AudioUniforms) == (13 + kFFTBins + 1) * 4,
              "AudioUniforms layout must stay scalar-packed to match MSL");

inline AudioUniforms toAudioUniforms(const AudioFeatureState& s) {
    AudioUniforms u{};
    u.kick = s.kick;
    u.snare = s.snare;
    u.hihat = s.hihat;
    u.perc = s.perc;
    u.beat = s.beat;
    u.rms = s.rms;
    u.low = s.low;
    u.mid = s.mid;
    u.high = s.high;
    u.centroid = s.centroid;
    u.flux = s.flux;
    u.time = s.time;
    u.deltaTime = s.deltaTime;
    for (int i = 0; i < kFFTBins; ++i) u.fft[i] = s.fft[i];
    u.frameIndex = s.frameIndex;
    return u;
}

} // namespace life
