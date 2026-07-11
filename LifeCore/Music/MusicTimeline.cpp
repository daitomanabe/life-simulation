// LifeCore/Music/MusicTimeline.cpp
#include "LifeCore/Music/MusicTimeline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace life {

namespace {

std::vector<float> floatArray(const nlohmann::json& j) {
    std::vector<float> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& v : j)
        out.push_back(v.is_number() ? v.get<float>() : 0.0f);
    return out;
}

} // namespace

bool MusicTimeline::load(const std::string& path, std::string& outError) {
    std::ifstream f(path);
    if (!f) {
        outError = "cannot open music timeline: " + path;
        return false;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        outError = std::string("music timeline parse error: ") + e.what();
        return false;
    }

    if (!doc.contains("tracks") || !doc["tracks"].is_array() ||
        doc["tracks"].empty()) {
        outError = "music timeline has no tracks[]: " + path;
        return false;
    }
    const auto& t = doc["tracks"][0];

    trackName_ = t.value("track", std::string());
    bpm_ = t.value("bpm", 120.0f);
    nFrames_ = t.value("n_frames", 0u);

    // --- envelopes_60fps ---
    if (t.contains("envelopes_60fps")) {
        const auto& e = t["envelopes_60fps"];
        fps_ = e.value("fps", 60.0f);
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (!it.value().is_array()) continue;  // "fps" はスカラ
            lanes_.push_back({it.key(), floatArray(it.value())});
        }
    }

    // --- sections ---
    if (t.contains("sections")) {
        for (const auto& s : t["sections"]) {
            MusicSection sec;
            sec.role = s.value("role", std::string("default"));
            sec.energy = s.value("energy", 0.0f);
            sec.frameStart = s.value("frame_start", 0);
            sections_.push_back(sec);
        }
        // frame_start しかないので、次セクションの開始で閉じる。
        for (size_t i = 0; i < sections_.size(); ++i) {
            sections_[i].frameEnd = (i + 1 < sections_.size())
                                        ? sections_[i + 1].frameStart
                                        : int(nFrames_);
        }
    }

    // --- events ---
    if (t.contains("events")) {
        for (const auto& e : t["events"]) {
            MusicEvent ev;
            ev.id = e.value("id", std::string());
            ev.type = e.value("type", std::string());
            ev.category = e.value("category", std::string());
            ev.cls = e.value("class", std::string());
            ev.bus = e.value("bus", std::string());
            ev.frameStart = e.value("frame_start", 0);
            ev.frameEnd = e.value("frame_end", 0);
            ev.tStart = e.value("t_start", 0.0f);
            ev.tEnd = e.value("t_end", 0.0f);
            ev.sudden = e.value("sudden", false);
            ev.interrupting = e.value("interrupting", false);
            ev.priority = e.value("priority", 0);
            if (e.contains("curves_60fps") && e["curves_60fps"].is_object()) {
                for (auto it = e["curves_60fps"].begin();
                     it != e["curves_60fps"].end(); ++it) {
                    ev.curves.emplace_back(it.key(), floatArray(it.value()));
                }
            }
            events_.push_back(std::move(ev));
        }
        // 開始フレーム順。sample() の線形走査を素直にする。
        std::sort(events_.begin(), events_.end(),
                  [](const MusicEvent& a, const MusicEvent& b) {
                      return a.frameStart < b.frameStart;
                  });
    }

    // --- se_layer.env_60fps ---
    if (t.contains("se_layer") && t["se_layer"].contains("env_60fps"))
        seEnv_ = floatArray(t["se_layer"]["env_60fps"]);

    if (nFrames_ == 0) {
        outError = "music timeline has n_frames = 0: " + path;
        return false;
    }
    loaded_ = true;
    return true;
}

float MusicTimeline::lane(const std::string& name, uint32_t frame,
                          float fallback) const {
    for (const auto& l : lanes_) {
        if (l.name != name) continue;
        if (l.v.empty()) return fallback;
        size_t i = std::min<size_t>(frame, l.v.size() - 1);
        return l.v[i];
    }
    return fallback;
}

void MusicTimeline::sample(uint32_t frame, MusicFeatureState& ms) const {
    ms.frame = frame;
    ms.events.clear();
    if (!loaded_) return;

    ms.lanes.tension = lane("tension", frame, 0.0f);
    ms.lanes.intensity = lane("intensity", frame, 0.0f);
    ms.lanes.wetRatio = lane("wet_ratio", frame, 0.0f);
    ms.lanes.kickGate = lane("kick_gate", frame, 0.0f);
    ms.lanes.macroDensity = lane("macro_density", frame, 1.0f);
    ms.lanes.macroIntensity = lane("macro_intensity", frame, 1.0f);
    ms.lanes.masterRms = lane("master_rms", frame, 0.0f);
    ms.lanes.drumsRms = lane("drums_rms", frame, 0.0f);
    ms.lanes.spatialActive = lane("spatial_active", frame, 0.0f);
    ms.lanes.seEnv = seEnv_.empty()
                         ? 0.0f
                         : seEnv_[std::min<size_t>(frame, seEnv_.size() - 1)];

    // --- section ---
    int idx = -1;
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (int(frame) >= sections_[i].frameStart &&
            int(frame) < sections_[i].frameEnd) {
            idx = int(i);
            break;
        }
    }
    if (idx < 0 && !sections_.empty())
        idx = int(sections_.size()) - 1;  // 末尾の余韻は最後のセクション扱い

    ms.sectionChanged = false;
    if (idx >= 0) {
        const MusicSection& s = sections_[size_t(idx)];
        ms.sectionIndex = idx;
        ms.sectionRole = s.role;
        ms.sectionEnergy = s.energy;
        int span = std::max(1, s.frameEnd - s.frameStart);
        ms.sectionProgress =
            std::clamp(float(int(frame) - s.frameStart) / float(span), 0.0f, 1.0f);
        ms.sectionChanged = (int(frame) == s.frameStart);
    }

    // --- active events ---
    for (const auto& e : events_) {
        if (e.frameStart > int(frame)) break;  // frameStart 昇順
        if (int(frame) >= e.frameEnd) continue;
        ActiveEvent a;
        a.def = &e;
        int span = std::max(1, e.frameEnd - e.frameStart);
        a.progress = float(int(frame) - e.frameStart) / float(span);
        a.justStarted = (int(frame) == e.frameStart);
        a.justEnded = (int(frame) == e.frameEnd - 1);
        ms.events.push_back(a);
    }
}

std::vector<std::string> MusicTimeline::eventTypes() const {
    std::vector<std::string> out;
    for (const auto& e : events_)
        if (std::find(out.begin(), out.end(), e.type) == out.end())
            out.push_back(e.type);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace life
