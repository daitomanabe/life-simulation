// LifeCore/Params/ParameterBus.cpp
#include "LifeCore/Params/ParameterBus.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace life {

MappingMode mappingModeFromString(const std::string& s) {
    if (s == "set") return MappingMode::Set;
    if (s == "mul" || s == "multiply") return MappingMode::Mul;
    return MappingMode::Add;
}

void ParameterBus::clear() {
    base_.clear();
    addSum_.clear();
    setValue_.clear();
    mulProduct_.clear();
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

float ParameterBus::featureValue(const MusicFeatureState& m, const std::string& name) {
    // --- レーン: "lane.<name>" ---
    if (name.rfind("lane.", 0) == 0) {
        const std::string l = name.substr(5);
        if (l == "tension") return m.lanes.tension;
        if (l == "intensity") return m.lanes.intensity;
        if (l == "wet_ratio") return m.lanes.wetRatio;
        if (l == "kick_gate") return m.lanes.kickGate;
        if (l == "macro_density") return m.lanes.macroDensity;
        if (l == "macro_intensity") return m.lanes.macroIntensity;
        if (l == "master_rms") return m.lanes.masterRms;
        if (l == "drums_rms") return m.lanes.drumsRms;
        if (l == "spatial_active") return m.lanes.spatialActive;
        if (l == "se_env") return m.lanes.seEnv;
        return 0.0f;
    }

    // --- セクション: "section.<what>" / "section.is:<role>" ---
    if (name.rfind("section.", 0) == 0) {
        const std::string w = name.substr(8);
        if (w.rfind("is:", 0) == 0) return m.sectionRole == w.substr(3) ? 1.0f : 0.0f;
        // "section.n:2" — 番号でひとつのセクションを指す。同じ role が続く曲
        // （Asharp は suspend が 3 連続）で、それぞれ別の生命を出すため。
        if (w.rfind("n:", 0) == 0)
            return m.sectionIndex == std::atoi(w.c_str() + 2) ? 1.0f : 0.0f;
        if (w == "energy") return m.sectionEnergy;
        if (w == "progress") return m.sectionProgress;
        if (w == "changed") return m.sectionChanged ? 1.0f : 0.0f;
        if (w == "index") return float(m.sectionIndex);
        return 0.0f;
    }

    // --- イベント: "event:<type>.<what>" ---
    if (name.rfind("event:", 0) == 0) {
        const std::string rest = name.substr(6);
        auto dot = rest.find('.');
        if (dot == std::string::npos) return m.eventActive(rest) ? 1.0f : 0.0f;
        const std::string type = rest.substr(0, dot);
        const std::string what = rest.substr(dot + 1);
        const ActiveEvent* e = m.find(type);
        if (what == "active") return e ? 1.0f : 0.0f;
        if (!e) return 0.0f;
        if (what == "started") return e->justStarted ? 1.0f : 0.0f;
        if (what == "ended") return e->justEnded ? 1.0f : 0.0f;
        if (what == "progress") return e->progress;
        // それ以外はイベント自身の 60fps 曲線名として引く。
        return m.eventCurve(type, what, 0.0f);
    }

    return featureValue(m.audio, name);
}

bool ParameterBus::sourceValue(const MusicFeatureState& m, const std::string& name,
                               float& out) {
    if (name.rfind("event:", 0) == 0) {
        const std::string rest = name.substr(6);
        auto dot = rest.find('.');
        if (dot != std::string::npos) {
            const std::string type = rest.substr(0, dot);
            const std::string what = rest.substr(dot + 1);
            const bool meta = (what == "active" || what == "started" ||
                               what == "ended" || what == "progress");
            if (!meta) {
                // イベント自身の曲線。鳴っていなければ値は存在しない。
                const ActiveEvent* e = m.find(type);
                if (!e || !e->def || !e->def->curve(what)) return false;
            }
        }
    }
    out = featureValue(m, name);
    return true;
}

void ParameterBus::update(const AudioFeatureState& audio, float dt) {
    MusicFeatureState m;
    m.audio = audio;
    update(m, dt);
}

void ParameterBus::update(const MusicFeatureState& music, float dt) {
    addSum_.clear();
    setValue_.clear();
    mulProduct_.clear();

    for (auto& ms : mappingStates_) {
        const AudioMappingSpec& s = ms.spec;

        // 値を持たない source（鳴っていないイベントの曲線）は、この 1 フレーム
        // 「無かったこと」にする。0 を寄与するのではなく、寄与そのものをしない。
        // add では同じことだが、set/mul では天と地ほど違う。
        float v = 0.0f;
        if (!sourceValue(music, s.source, v)) continue;

        // 物理単位 → 0..1（inMax > inMin のときだけ）
        if (s.inMax > s.inMin)
            v = std::clamp((v - s.inMin) / (s.inMax - s.inMin), 0.0f, 1.0f);

        v = v * s.scale + s.offset;

        if (s.smoothing > 1e-5f && ms.primed) {
            float k = 1.0f - std::exp(-dt / s.smoothing);
            ms.smoothedValue += (v - ms.smoothedValue) * k;
        } else {
            ms.smoothedValue = v;
        }
        ms.primed = true;

        switch (s.mode) {
        case MappingMode::Set:
            setValue_[s.target] = ms.smoothedValue;  // 後勝ち
            break;
        case MappingMode::Mul: {
            auto it = mulProduct_.find(s.target);
            if (it == mulProduct_.end()) mulProduct_[s.target] = ms.smoothedValue;
            else it->second *= ms.smoothedValue;
            break;
        }
        case MappingMode::Add:
        default:
            addSum_[s.target] += ms.smoothedValue;
            break;
        }
    }
}

float ParameterBus::value(const std::string& key, float fallback) const {
    if (auto it = overrides_.find(key); it != overrides_.end()) return it->second;

    float v;
    if (auto it = setValue_.find(key); it != setValue_.end()) v = it->second;
    else v = base(key, fallback);

    if (auto it = addSum_.find(key); it != addSum_.end()) v += it->second;
    if (auto it = mulProduct_.find(key); it != mulProduct_.end()) v *= it->second;
    return v;
}

void ParameterBus::setOverride(const std::string& key, float value) {
    overrides_[key] = value;
}

void ParameterBus::clearOverride(const std::string& key) { overrides_.erase(key); }

} // namespace life
