// LifeCore/Audio/AudioInput.cpp
#include "LifeCore/Audio/AudioInput.h"

namespace life {

bool AudioInput::start(uint16_t port, std::string& outError) {
    auto* smoother = &smoother_;
    return osc_.start(
        port,
        [smoother](const std::string& address, const float* values, int count) {
            if (count <= 0) return;
            if (address == "/fft") {
                smoother->pushFFT(values, count);
            } else if (address == "/kick") {
                smoother->pushChannel(AudioChannel::Kick, values[0]);
            } else if (address == "/snare") {
                smoother->pushChannel(AudioChannel::Snare, values[0]);
            } else if (address == "/hihat") {
                smoother->pushChannel(AudioChannel::Hihat, values[0]);
            } else if (address == "/perc") {
                smoother->pushChannel(AudioChannel::Perc, values[0]);
            } else if (address == "/beat") {
                smoother->pushChannel(AudioChannel::Beat, values[0]);
            }
            // Unknown addresses are ignored by design; the input bus is fixed.
        },
        outError);
}

void AudioInput::stop() { osc_.stop(); }

const AudioFeatureState& AudioInput::update(float dt, float time, uint32_t frameIndex) {
    smoother_.update(dt);
    smoother_.fill(state_);
    state_.time = time;
    state_.deltaTime = dt;
    state_.frameIndex = frameIndex;
    return state_;
}

} // namespace life
