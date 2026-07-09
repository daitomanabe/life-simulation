// LifeCore/Params/ParameterBus.cpp
#include "LifeCore/Params/ParameterBus.h"

#include <cmath>

namespace life {

void ParameterBus::clear() {
    base_.clear();
    audioContribution_.clear();
    overrides_.clear();
    mappings_.clear();
    mappingStates_.clear();
}

void ParameterBus::setBase(const std::string& key, float value) { base_[key] = value; }

float ParameterBus::base(const std::string& key, float fallback) const {
    auto it = base_.find(key);
    return it != base_.end() ? it->second : fallback;
}

void ParameterBus::addMapping(const AudioMappingSpec& spec) {
    mappings_.push_back(spec);
    mappingStates_.push_back({spec, 0.0f});
}

float ParameterBus::featureValue(const AudioFeatureState& s, const std::string& name) {
    // Optional ".raw/.smoothed/.peak/.trigger/.hold" suffix on channels.
    auto pick = [](const ChannelEnvelope& e, const std::string& part) -> float {
        if (part == "raw") return e.raw;
        if (part == "peak") return e.peak;
        if (part == "trigger") return e.trigger;
        if (part == "hold") return e.hold;
        return e.smoothed;
    };

    std::string channel = name, part = "smoothed";
    if (auto dot = name.find('.'); dot != std::string::npos) {
        channel = name.substr(0, dot);
        part = name.substr(dot + 1);
    }

    if (channel == "kick") return pick(s.kickEnv, part);
    if (channel == "snare") return pick(s.snareEnv, part);
    if (channel == "hihat") return pick(s.hihatEnv, part);
    if (channel == "perc") return pick(s.percEnv, part);
    if (channel == "beat") return pick(s.beatEnv, part);
    if (channel == "rms") return s.rms;
    if (channel == "low") return s.low;
    if (channel == "mid") return s.mid;
    if (channel == "high") return s.high;
    if (channel == "centroid") return s.centroid;
    if (channel == "flux") return s.flux;
    if (channel == "time") return s.time;
    return 0.0f;
}

void ParameterBus::update(const AudioFeatureState& audio, float dt) {
    audioContribution_.clear();
    for (auto& m : mappingStates_) {
        float v = featureValue(audio, m.spec.source) * m.spec.scale + m.spec.offset;
        if (m.spec.smoothing > 1e-5f) {
            float k = 1.0f - std::exp(-dt / m.spec.smoothing);
            m.smoothedValue += (v - m.smoothedValue) * k;
        } else {
            m.smoothedValue = v;
        }
        audioContribution_[m.spec.target] += m.smoothedValue;
    }
}

float ParameterBus::value(const std::string& key, float fallback) const {
    if (auto it = overrides_.find(key); it != overrides_.end()) return it->second;
    float v = base(key, fallback);
    if (auto it = audioContribution_.find(key); it != audioContribution_.end())
        v += it->second;
    return v;
}

void ParameterBus::setOverride(const std::string& key, float value) {
    overrides_[key] = value;
}

void ParameterBus::clearOverride(const std::string& key) { overrides_.erase(key); }

} // namespace life
