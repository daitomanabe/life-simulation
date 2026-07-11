#pragma once
// LifeCore/Music/MusicFeatureState.h
// 楽曲の「構造」を表す毎フレーム状態。AudioFeatureState が音の *強さ* を運ぶのに
// 対し、こちらは音の *意味* — いまどのセクションか、どのイベントが鳴っていて、
// そのイベント自身のオートメーション曲線がいま何を指しているか — を運ぶ。
//
// AudioFeatureState は意図的に一切変更していない。既存のプリセット・OSC 入力・
// --audio replay はこのヘッダを知らないまま従来どおり動く。
//
// 生命シミュレーションにとって重要なのは、音の強さより *ルールの切り替わり* だ。
// Lenia の growthMu が変われば系は数秒かけて別の相に落ち着く。セクションは
// 12〜30 秒あるので、ちょうど落ち着く時間がある。イベントの curves_60fps は
// すでにシミュレーションと同じ 60fps で刻まれているので、平滑化も補間もなしに
// ルールへ直結できる。

#include "LifeCore/Audio/AudioFeatureState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace life {

// events JSON の 1 イベント定義（不変。MusicTimeline が所有する）。
struct MusicEvent {
    std::string id;
    std::string type;      // "granular_freeze", "warp_filter", ...
    std::string category;  // "motion", "texture", ...
    std::string cls;       // "trigger" | "automation" | ...
    std::string bus;
    int frameStart = 0;
    int frameEnd = 0;      // 排他的
    float tStart = 0.0f;
    float tEnd = 0.0f;
    bool sudden = false;
    bool interrupting = false;
    int priority = 0;

    // params.<name>.curves_60fps — frameStart 起点、長さ frameEnd-frameStart。
    std::vector<std::pair<std::string, std::vector<float>>> curves;

    const std::vector<float>* curve(const std::string& name) const {
        for (const auto& c : curves)
            if (c.first == name) return &c.second;
        return nullptr;
    }
};

struct MusicSection {
    std::string role;  // establish / peak / suspend / hinge / coda / build / drop
    float energy = 0.0f;
    int frameStart = 0;
    int frameEnd = 0;
};

// このフレームで走っているイベント（MusicTimeline が所有する定義への参照）。
struct ActiveEvent {
    const MusicEvent* def = nullptr;
    float progress = 0.0f;  // 0..1
    bool justStarted = false;
    bool justEnded = false;
};

// envelopes_60fps の各レーン。曲全体で定数のものもある（macro_* は 1.0 固定 —
// visualize-lyria 側で同じ結論に達している。ゲート条件に使ってはいけない）。
struct MusicLanes {
    float tension = 0.0f;
    float intensity = 0.0f;
    float wetRatio = 0.0f;
    float kickGate = 0.0f;
    float macroDensity = 1.0f;
    float macroIntensity = 1.0f;
    float masterRms = 0.0f;
    float drumsRms = 0.0f;
    float spatialActive = 0.0f;
    float seEnv = 0.0f;  // se_layer.env_60fps
};

struct MusicFeatureState {
    AudioFeatureState audio;  // WAV 由来の連続量（capture.jsonl か live OSC）

    uint32_t frame = 0;
    MusicLanes lanes;

    // 現在のセクション。
    int sectionIndex = -1;
    std::string sectionRole;
    float sectionEnergy = 0.0f;
    float sectionProgress = 0.0f;  // 0..1
    bool sectionChanged = false;   // このフレームが新セクションの先頭

    std::vector<ActiveEvent> events;  // このフレームで active なもののみ

    // --- 問い合わせ ---
    const ActiveEvent* find(const std::string& type) const {
        for (const auto& e : events)
            if (e.def && e.def->type == type) return &e;
        return nullptr;
    }
    bool eventActive(const std::string& type) const { return find(type) != nullptr; }

    // イベント自身の 60fps 曲線を、そのイベントのローカルフレームで引く。
    float eventCurve(const std::string& type, const std::string& curveName,
                     float fallback = 0.0f) const {
        const ActiveEvent* e = find(type);
        if (!e || !e->def) return fallback;
        const auto* c = e->def->curve(curveName);
        if (!c || c->empty()) return fallback;
        int local = int(frame) - e->def->frameStart;
        if (local < 0) local = 0;
        if (local >= int(c->size())) local = int(c->size()) - 1;
        return (*c)[size_t(local)];
    }
};

} // namespace life
