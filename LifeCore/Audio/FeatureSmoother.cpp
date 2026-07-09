// LifeCore/Audio/FeatureSmoother.cpp
#include "LifeCore/Audio/FeatureSmoother.h"

#include <algorithm>
#include <cmath>

namespace life {

void FeatureSmoother::pushChannel(AudioChannel ch, float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& p = pending_[size_t(ch)];
    // Keep the maximum within a frame so short spikes are not lost when OSC
    // arrives faster than the sim rate.
    p.value = p.dirty ? std::max(p.value, value) : value;
    p.dirty = true;
}

void FeatureSmoother::pushFFT(const float* bins, int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = std::min(count, kFFTBins);
    for (int i = 0; i < n; ++i) pendingFFT_[i] = bins[i];
    fftDirty_ = true;
}

static float expApproach(float current, float target, float rate, float dt) {
    // Frame-rate independent one-pole.
    float k = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * k;
}

void FeatureSmoother::update(float dt) {
    dt = std::max(dt, 1e-5f);

    // Take pending values under lock, process outside it.
    std::array<PendingChannel, size_t(AudioChannel::Count)> pending;
    std::array<float, kFFTBins> fftIn{};
    bool fftDirty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = pending_;
        for (auto& p : pending_) p.dirty = false;
        if (fftDirty_) {
            fftIn = pendingFFT_;
            fftDirty = true;
            fftDirty_ = false;
        }
    }

    for (size_t i = 0; i < env_.size(); ++i) {
        auto& e = env_[i];
        if (pending[i].dirty) {
            e.raw = pending[i].value;
            idleTime_[i] = 0.0f;
        } else {
            idleTime_[i] += dt;
            if (idleTime_[i] > config.idleDecayDelay)
                e.raw *= std::exp(-config.idleDecayRate * dt);
        }

        float rate = e.raw > e.smoothed ? config.attackRate : config.releaseRate;
        e.smoothed = expApproach(e.smoothed, e.raw, rate, dt);

        e.peak = std::max(e.raw, e.peak * std::exp(-config.peakDecayRate * dt));

        bool above = e.raw >= config.triggerThreshold;
        if (above && !wasAbove_[i]) e.trigger = 1.0f;
        else e.trigger *= std::exp(-config.triggerDecayRate * dt);
        wasAbove_[i] = above;

        e.hold = e.raw >= config.holdThreshold ? 1.0f : 0.0f;
    }

    if (fftDirty) {
        prevFFT_ = fft_;
        fft_ = fftIn;

        float sumSq = 0, sumLow = 0, sumMid = 0, sumHigh = 0;
        float weighted = 0, total = 0, flux = 0;
        for (int i = 0; i < kFFTBins; ++i) {
            float v = fft_[i];
            sumSq += v * v;
            if (i < 16) sumLow += v;
            else if (i < 64) sumMid += v;
            else sumHigh += v;
            weighted += v * float(i);
            total += v;
            float d = v - prevFFT_[i];
            if (d > 0) flux += d;
        }
        rms_ = std::sqrt(sumSq / kFFTBins);
        low_ = sumLow / 16.0f;
        mid_ = sumMid / 48.0f;
        high_ = sumHigh / 64.0f;
        centroid_ = total > 1e-6f ? (weighted / total) / float(kFFTBins - 1) : 0.0f;
        flux_ = flux / kFFTBins;
    }
}

void FeatureSmoother::fill(AudioFeatureState& s) const {
    s.kickEnv = env_[size_t(AudioChannel::Kick)];
    s.snareEnv = env_[size_t(AudioChannel::Snare)];
    s.hihatEnv = env_[size_t(AudioChannel::Hihat)];
    s.percEnv = env_[size_t(AudioChannel::Perc)];
    s.beatEnv = env_[size_t(AudioChannel::Beat)];

    s.kick = s.kickEnv.smoothed;
    s.snare = s.snareEnv.smoothed;
    s.hihat = s.hihatEnv.smoothed;
    s.perc = s.percEnv.smoothed;
    s.beat = s.beatEnv.smoothed;

    for (int i = 0; i < kFFTBins; ++i) s.fft[i] = fft_[i];
    s.rms = rms_;
    s.low = low_;
    s.mid = mid_;
    s.high = high_;
    s.centroid = centroid_;
    s.flux = flux_;
}

} // namespace life
