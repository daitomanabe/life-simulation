#pragma once
// LifeCore/Music/MusicTimeline.h
// remix-beats の events JSON (schema 6.0.0) を唯一の真実として読み、60fps の
// フレーム番号で引ける形に索引する。
//
//   LifeOfflineRender --music <slug>-events-v6-extreme-se.json
//                     --audio <slug>-capture-60fps.jsonl
//
// --audio が音の強さ（WAV 由来）を運び、--music が音の構造（JSON 由来）を運ぶ。
// 両者は独立に指定できる。--music だけでも動く（音量系は 0 のまま）。
//
// JSON をそのまま読むので、curves_60fps を別形式へ複製しない。remix_v6.py が
// 出した数値がそのままシミュレーションのルールに入る。

#include "LifeCore/Music/MusicFeatureState.h"

#include <string>
#include <vector>

namespace life {

class MusicTimeline {
public:
    // events JSON を読む。tracks[0] を対象にする（1 ファイル 1 曲）。
    bool load(const std::string& path, std::string& outError);

    // frame のセクション・active イベント・レーン値で ms を埋める。
    // ms.audio には触れない（--audio replay か live OSC が埋める）。
    void sample(uint32_t frame, MusicFeatureState& ms) const;

    bool loaded() const { return loaded_; }
    uint32_t frameCount() const { return nFrames_; }
    float bpm() const { return bpm_; }
    float fps() const { return fps_; }
    const std::string& trackName() const { return trackName_; }
    const std::vector<MusicEvent>& events() const { return events_; }
    const std::vector<MusicSection>& sections() const { return sections_; }

    // シーン JSON の検証用: この曲に実在するイベント型の一覧。
    std::vector<std::string> eventTypes() const;

private:
    struct Lane {
        std::string name;
        std::vector<float> v;
    };
    float lane(const std::string& name, uint32_t frame, float fallback) const;

    bool loaded_ = false;
    std::string trackName_;
    float bpm_ = 120.0f;
    float fps_ = 60.0f;
    uint32_t nFrames_ = 0;

    std::vector<MusicEvent> events_;
    std::vector<MusicSection> sections_;
    std::vector<Lane> lanes_;
    std::vector<float> seEnv_;
};

} // namespace life
