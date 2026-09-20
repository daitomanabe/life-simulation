// LifeCore/Params/ParameterBus.cpp
#include "LifeCore/Params/ParameterBus.h"

#include "LifeCore/Math/Random.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace life {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// FNV-1a 32-bit — only used to turn a source string into a noise seed tag,
// no cryptographic properties needed, just "different strings -> different
// streams".
uint32_t fnv1a32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// [0,1) pseudo-random value keyed by (seed, integer index) — same pcgHash
// building block Fluid/Boids/etc already use for GPU-side randomness.
float hashUnit(uint32_t seed, int64_t index) {
    uint32_t h = pcgHash(seed ^ pcgHash(static_cast<uint32_t>(index)));
    return float(h) * (1.0f / 4294967295.0f);
}

// Evaluates one "time.*" source at accumulated sim time `t` (seconds).
// period > 0 is guaranteed by parseTimeSource() at scene load, but this
// still guards against it (defensive, e.g. mappings added without going
// through Scene::parse).
float evalTimeSource(TimeSourceKind kind, float period, float phase, uint32_t noiseSeed,
                     double t) {
    if (!(period > 0.0f)) return 0.0f;
    // t/period in double, THEN reduce to a fraction of a cycle, THEN narrow.
    // Doing this in float loses the cycle position after a few hours: near
    // 28,800 s a float's step is 0.00195 s and the useful bits of x are gone.
    // Ramp and Noise need the integer part, so they keep the wide value.
    const double xWide = t / double(period) + double(phase);
    const float x = float(xWide - std::floor(xWide));
    switch (kind) {
    case TimeSourceKind::Sin:
        return std::sin(kTwoPi * x);
    case TimeSourceKind::Tri:
        // asin(sin(.)) is a triangle wave phase-aligned with sin: 0 at x=0
        // rising, +1 at quarter cycle, 0 at half, -1 at three-quarters.
        return (2.0f / 3.14159265358979323846f) * std::asin(std::sin(kTwoPi * x));
    case TimeSourceKind::Ramp:
        return x;
    case TimeSourceKind::Noise: {
        // Smooth value noise: interpolate between seeded random values at
        // `period`-second knots with a smoothstep — NOT white noise (no
        // per-frame jitter, no discontinuities at the knots).
        const double idx = std::floor(xWide);
        const float frac = float(xWide - idx);
        const float a = hashUnit(noiseSeed, int64_t(idx)) * 2.0f - 1.0f;
        const float b = hashUnit(noiseSeed, int64_t(idx) + 1) * 2.0f - 1.0f;
        const float s = frac * frac * (3.0f - 2.0f * frac);
        return a + (b - a) * s;
    }
    default:
        return 0.0f;
    }
}

} // namespace

MappingMode mappingModeFromString(const std::string& s) {
    if (s == "set") return MappingMode::Set;
    if (s == "mul" || s == "multiply") return MappingMode::Mul;
    return MappingMode::Add;
}

bool parseTimeSource(const std::string& source, TimeSourceKind& outKind, float& outPeriod,
                     float& outPhase, std::string& outError) {
    outKind = TimeSourceKind::None;
    outPeriod = 0.0f;
    outPhase = 0.0f;
    if (source.rfind("time.", 0) != 0) return true; // not a time source

    const std::string rest = source.substr(5); // "sin:600@0.25"
    const auto colon = rest.find(':');
    if (colon == std::string::npos) {
        outError = "malformed time source (expected \"time.<kind>:<period>\"): \"" +
                   source + "\"";
        return false;
    }
    const std::string kindStr = rest.substr(0, colon);
    std::string periodStr = rest.substr(colon + 1);
    std::string phaseStr;
    if (const auto at = periodStr.find('@'); at != std::string::npos) {
        phaseStr = periodStr.substr(at + 1);
        periodStr = periodStr.substr(0, at);
    }

    TimeSourceKind kind;
    if (kindStr == "sin") kind = TimeSourceKind::Sin;
    else if (kindStr == "tri") kind = TimeSourceKind::Tri;
    else if (kindStr == "ramp") kind = TimeSourceKind::Ramp;
    else if (kindStr == "noise") kind = TimeSourceKind::Noise;
    else {
        outError = "unknown time source kind \"" + kindStr + "\" in \"" + source +
                   "\" (expected sin, tri, ramp or noise)";
        return false;
    }

    auto parseFloatStrict = [](const std::string& s, float& out) -> bool {
        if (s.empty()) return false;
        try {
            size_t consumed = 0;
            out = std::stof(s, &consumed);
            return consumed == s.size();
        } catch (...) {
            return false;
        }
    };

    float period = 0.0f, phase = 0.0f;
    if (!parseFloatStrict(periodStr, period) || !(period > 0.0f)) {
        outError = "invalid or non-positive period in time source \"" + source + "\"";
        return false;
    }
    if (!phaseStr.empty() && !parseFloatStrict(phaseStr, phase)) {
        outError = "invalid phase in time source \"" + source + "\"";
        return false;
    }

    outKind = kind;
    outPeriod = period;
    outPhase = phase;
    return true;
}

void ParameterBus::clear() {
    base_.clear();
    addSum_.clear();
    setValue_.clear();
    mulProduct_.clear();
    overrides_.clear();
    mappings_.clear();
    mappingStates_.clear();
    clockTime_ = 0.0f;
}

void ParameterBus::setBase(const std::string& key, float value) { base_[key] = value; }

float ParameterBus::base(const std::string& key, float fallback) const {
    auto it = base_.find(key);
    return it != base_.end() ? it->second : fallback;
}

void ParameterBus::addMapping(const AudioMappingSpec& spec) {
    mappings_.push_back(spec);

    MappingState ms;
    ms.spec = spec;
    std::string err; // Scene::parse already validated this; a failure here
                      // (e.g. a mapping added without going through Scene)
                      // just falls back to timeKind == None, i.e. resolves
                      // like an unrecognized source name — 0, same as today.
    if (parseTimeSource(spec.source, ms.timeKind, ms.timePeriod, ms.timePhase, err) &&
        ms.timeKind != TimeSourceKind::None) {
        ms.noiseSeed = deriveSeed(seed_, fnv1a32(spec.source));
    }
    mappingStates_.push_back(std::move(ms));
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

        float v = 0.0f;
        if (ms.timeKind != TimeSourceKind::None) {
            // 時間ソース: 音・イベントとは無関係に、この bus 自身の時計だけ
            // から決まる。常に値を持つので sourceValue() の「値が無い」判定
            // は関係ない。
            v = evalTimeSource(ms.timeKind, ms.timePeriod, ms.timePhase, ms.noiseSeed,
                               clockTime_);
        } else if (!sourceValue(music, s.source, v)) {
            // 値を持たない source（鳴っていないイベントの曲線）は、この 1
            // フレーム「無かったこと」にする。0 を寄与するのではなく、寄与
            // そのものをしない。add では同じことだが、set/mul では天と地ほど
            // 違う。
            continue;
        }

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

    // "time.*" ソースの唯一の入力。dt はこの呼び出しが渡された値そのもの
    // なので、オフライン（固定 dt）でもリアルタイム（可変だが記録された
    // 同じ dt 列を再生）でも、同じ dt 列なら同じ clockTime_ 列になる。
    clockTime_ += dt;
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
