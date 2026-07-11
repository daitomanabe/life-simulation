#pragma once
// LifeCore/Params/ParameterBus.h
// Central parameter table. Flow (design doc §14.2):
//   OSC → AudioFeatureState ─┐
//                            ├→ Mapping → ParameterBus → SimulationUniforms
//   events JSON → MusicTimeline → MusicFeatureState ─┘
// Simulations never see OSC addresses or JSON; they read final values by key
// ("moduleName.paramName").
//
// 合成規則（override が全てに優先）:
//   value = ((set があれば set、なければ base) + Σ add) × Π mul
// `set` は base を置き換える。Lenia の growthMu のように「セクションの値そのもの
// に *なる*」べきパラメータがあり、加算の総和では表現できないため。

#include "LifeCore/Audio/AudioFeatureState.h"
#include "LifeCore/Music/MusicFeatureState.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace life {

enum class MappingMode : uint32_t {
    Add = 0,  // 既定。base に加算
    Set,      // base を置き換える（同一 target に複数あれば後勝ち）
    Mul,      // 最終値に乗算
};

MappingMode mappingModeFromString(const std::string& s);

struct AudioMappingSpec {
    // source 文法:
    //   音（AudioFeatureState 由来 — live OSC / capture.jsonl）
    //     "kick" "kick.trigger" "snare.peak" "rms" "low" "mid" "high"
    //     "centroid" "flux" "time"
    //   レーン（events JSON の envelopes_60fps）
    //     "lane.tension" "lane.wet_ratio" "lane.master_rms" "lane.se_env" ...
    //   セクション
    //     "section.energy" "section.progress" "section.changed"
    //     "section.is:coda"   → role が一致する間 1.0
    //     "section.n:2"       → 3 番目のセクションの間 1.0（同じ role が続く曲用）
    //
    // target の "<module>.opacity" は SceneRunner が毎フレーム読む。これで
    // セクションごとに「どの生命を見せるか」を切り替えられる。
    //   イベント
    //     "event:granular_freeze.active"    → 走っている間 1.0
    //     "event:granular_freeze.started"   → 開始フレームのみ 1.0
    //     "event:granular_freeze.progress"  → 0..1
    //     "event:warp_filter.cutoff_hz"     → そのイベント自身の 60fps 曲線
    std::string source;
    std::string target;  // "rd0.feed"

    // 曲線は物理単位（Hz, dB, …）で来る。inMax > inMin のとき、scale を掛ける
    // 前に 0..1 へ正規化する。これがないと cutoff_hz を growthMu へ繋げない。
    float inMin = 0.0f;
    float inMax = 0.0f;

    float scale = 1.0f;
    float offset = 0.0f;
    float smoothing = 0.0f;  // seconds to ~63% (0 = immediate)
    MappingMode mode = MappingMode::Add;
};

class ParameterBus {
public:
    void clear();

    // Base values come from the scene JSON.
    void setBase(const std::string& key, float value);
    float base(const std::string& key, float fallback = 0.0f) const;

    void addMapping(const AudioMappingSpec& spec);
    const std::vector<AudioMappingSpec>& mappings() const { return mappings_; }

    // 音のみ（従来経路。既存プリセットと live OSC はここを通る）。
    void update(const AudioFeatureState& audio, float dt);
    // 音 + 構造。music.audio が音の部分を兼ねる。
    void update(const MusicFeatureState& music, float dt);

    // Final value = ((set ?: base) + Σ add) × Π mul。override が優先。
    float value(const std::string& key, float fallback = 0.0f) const;

    // Manual override layer (future GUI / control OSC).
    void setOverride(const std::string& key, float value);
    void clearOverride(const std::string& key);

    // Resolve an audio feature by name ("kick", "kick.trigger", "low", ...).
    static float featureValue(const AudioFeatureState& s, const std::string& name);
    // Resolve any source name — 音・レーン・セクション・イベントを含む。
    static float featureValue(const MusicFeatureState& m, const std::string& name);

    // source が「いま値を持っているか」。イベント自身の曲線
    // ("event:warp_filter.cutoff_hz") は、そのイベントが鳴っていない間は
    // *値が存在しない*。0 ではない。
    //
    // これを区別しないと mode:set が壊れる。イベントが終わった瞬間に
    // featureValue が 0 を返し、set はターゲットを「0 のときの値」に永久に
    // 固定してしまう（base へは戻らない）。warp_filter は 158 秒の曲のうち
    // 最初の 12 秒しかないので、残り 96% でパラメータが凍りついていた。
    static bool sourceValue(const MusicFeatureState& m, const std::string& name,
                            float& out);

private:
    struct MappingState {
        AudioMappingSpec spec;
        float smoothedValue = 0.0f;
        // 最初の update では平滑化せず目標値を直接入れる。0 から立ち上げると、
        // mode:set のマッピング（growthMu など）が冒頭の数秒だけ 0 付近に沈む。
        bool primed = false;
    };

    std::unordered_map<std::string, float> base_;
    std::unordered_map<std::string, float> addSum_;
    std::unordered_map<std::string, float> setValue_;
    std::unordered_map<std::string, float> mulProduct_;
    std::unordered_map<std::string, float> overrides_;
    std::vector<AudioMappingSpec> mappings_;
    std::vector<MappingState> mappingStates_;
};

} // namespace life
