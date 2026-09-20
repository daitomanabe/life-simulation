// LifeCore/Audio/AudioInput.cpp
#include "LifeCore/Audio/AudioInput.h"

#include <cmath>
#include <cstdio>

namespace life {

namespace {

// Wraps x into [0,1) (the 93m wall's x coordinate is toroidal) and clamps y
// into [0,1] (floor/ceiling are walls, not toroidal) — input validation at
// the OSC trust boundary, before a point ever reaches the sim.
float wrapX01(float x) { return x - std::floor(x); }
float clampY01(float y) { return y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y); }

} // namespace

bool AudioInput::start(uint16_t port, std::string& outError) {
    auto* smoother = &smoother_;
    auto* presence = &presence_;
    return osc_.start(
        port,
        [smoother, presence](const std::string& address, const float* values, int count) {
            if (address == "/presence") {
                // Sensor-agnostic visitor tracking (hardware not chosen yet):
                // repeated points, either "id, x, y" (3 floats) or
                // "id, x, y, strength" (4 floats) per point, all in one
                // message using ONE stride throughout. Counts that are a
                // clean multiple of both 3 and 4 (e.g. 12) are genuinely
                // ambiguous on the wire — this prefers stride 4 in that
                // case; a sender that only wants the 3-float form should
                // avoid sending an exact multiple-of-4 point count, or
                // just always send a strength (simplest: always send 4).
                if (count < 3) return;
                int stride = (count % 4 == 0) ? 4 : (count % 3 == 0 ? 3 : 0);
                if (stride == 0) return; // malformed; ignored by design
                static int lastLoggedStride = 0; // OSC receive thread only
                if (stride != lastLoggedStride) {
                    fprintf(stderr,
                            "[life] /presence: reading %d-float stride (%s)\n", stride,
                            stride == 4 ? "id,x,y,strength" : "id,x,y (strength defaults to 1.0)");
                    lastLoggedStride = stride;
                }
                for (int i = 0; i + stride <= count; i += stride) {
                    uint32_t id = uint32_t(values[i]);
                    float x = wrapX01(values[i + 1]);
                    float y = clampY01(values[i + 2]);
                    float strength = (stride == 4) ? values[i + 3] : 1.0f;
                    presence->pushPoint(id, x, y, strength);
                }
                return;
            }
            if (address == "/presence/clear") {
                presence->pushClear();
                return;
            }
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
            // Unknown addresses are ignored by design; the input bus is fixed
            // (audio channels above, plus the /presence pair — design doc's
            // sensor-agnostic presence path).
        },
        outError);
}

void AudioInput::stop() { osc_.stop(); }

const AudioFeatureState& AudioInput::update(float dt, float time, uint32_t frameIndex) {
    smoother_.update(dt);
    smoother_.fill(state_);
    presence_.update(dt, state_.presence);
    state_.time = time;
    state_.deltaTime = dt;
    state_.frameIndex = frameIndex;
    return state_;
}

} // namespace life
