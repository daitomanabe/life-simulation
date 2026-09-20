#pragma once
// LifeCore/Audio/AudioInput.h
// Bundles OSCReceiver + FeatureSmoother into the standard live input path:
//   OSC → FeatureSmoother → AudioFeatureState
// Offline replay bypasses this entirely (CaptureReplay produces the same
// AudioFeatureState), which is what keeps Realtime and Offline in sync.

#include "LifeCore/Audio/AudioFeatureState.h"
#include "LifeCore/Audio/FeatureSmoother.h"
#include "LifeCore/Audio/OSCReceiver.h"
#include "LifeCore/Audio/PresenceStore.h"

#include <string>

namespace life {

class AudioInput {
public:
    // Starts listening on the given UDP port for the standard address set
    // (/kick /snare /hihat /perc /beat /fft, plus /presence /presence/clear
    // for visitor tracking — see AudioInput.cpp's OSC dispatch).
    bool start(uint16_t port, std::string& outError);
    void stop();

    // Advance envelopes and produce the frame's AudioFeatureState.
    const AudioFeatureState& update(float dt, float time, uint32_t frameIndex);

    const AudioFeatureState& state() const { return state_; }
    OSCStats oscStats() const { return osc_.stats(); }
    FeatureSmoother& smoother() { return smoother_; }

private:
    OSCReceiver osc_;
    FeatureSmoother smoother_;
    PresenceStore presence_;
    AudioFeatureState state_;
};

} // namespace life
