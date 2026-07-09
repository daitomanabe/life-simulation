#pragma once
// LifeCore/Audio/FeatureSmoother.h
// Turns raw OSC values into ChannelEnvelopes and derives FFT features. Push
// happens from the OSC thread (guarded), update(dt) runs on the sim thread.

#include "LifeCore/Audio/AudioFeatureState.h"

#include <array>
#include <mutex>

namespace life {

enum class AudioChannel : int { Kick = 0, Snare, Hihat, Perc, Beat, Count };

struct SmootherConfig {
    float attackRate = 40.0f;    // 1/s toward raw when rising
    float releaseRate = 6.0f;    // 1/s toward raw when falling
    float peakDecayRate = 1.5f;  // 1/s exponential peak decay
    float triggerDecayRate = 8.0f;
    float triggerThreshold = 0.35f;
    float holdThreshold = 0.2f;
    // Event-style senders fire "/kick 1.0" without a matching zero. If no OSC
    // arrives on a channel for idleDecayDelay seconds, raw decays toward 0 so
    // rules don't stay stuck high when the music stops (watchdog behavior).
    float idleDecayDelay = 0.25f; // s of silence before decay kicks in
    float idleDecayRate = 6.0f;   // 1/s exponential raw decay while idle
};

class FeatureSmoother {
public:
    // Thread-safe: called from the OSC receive thread.
    void pushChannel(AudioChannel ch, float value);
    void pushFFT(const float* bins, int count);

    // Sim thread: advance envelopes and derived features by dt seconds.
    void update(float dt);

    // Fill the channel/fft/derived sections of an AudioFeatureState.
    void fill(AudioFeatureState& state) const;

    SmootherConfig config;

private:
    struct PendingChannel {
        float value = 0.0f;
        bool dirty = false;
    };

    mutable std::mutex mutex_;
    std::array<PendingChannel, size_t(AudioChannel::Count)> pending_{};
    std::array<float, kFFTBins> pendingFFT_{};
    bool fftDirty_ = false;

    std::array<ChannelEnvelope, size_t(AudioChannel::Count)> env_{};
    std::array<bool, size_t(AudioChannel::Count)> wasAbove_{};
    std::array<float, size_t(AudioChannel::Count)> idleTime_{};
    std::array<float, kFFTBins> fft_{};
    std::array<float, kFFTBins> prevFFT_{};

    float rms_ = 0, low_ = 0, mid_ = 0, high_ = 0, centroid_ = 0, flux_ = 0;
};

} // namespace life
