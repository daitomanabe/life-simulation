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

// "time.*" のソース種別。None は「time. で始まらない = 時間ソースではない」。
enum class TimeSourceKind : uint32_t { None = 0, Sin, Tri, Ramp, Noise };

// "time.<kind>:<period>[@<phase>]" を解析する（AudioMappingSpec の source
// 文法コメント参照）。`source` が "time." で始まらないなら outKind に None
// を入れて true を返す（検証対象外）。"time." で始まるのに種別不明・period
// が正の数として読めない・phase が数として読めない、のいずれかなら false と
// 共に、その source 文字列を含む説明を outError に入れる。シーン読み込み時に
// これを呼んで false なら読み込みそのものを失敗させる — 壊れた period を
// 黙って 0 にはしない。
bool parseTimeSource(const std::string& source, TimeSourceKind& outKind, float& outPeriod,
                      float& outPhase, std::string& outError);

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
    //     "section.n:2"       → 3 番目のセクションの間 1.0（同じ役が続く曲用）
    //
    // target の "<module>.opacity" は SceneRunner が毎フレーム読む。これで
    // セクションごとに「どの生命を見せるか」を切り替えられる。
    //   イベント
    //     "event:granular_freeze.active"    → 走っている間 1.0
    //     "event:granular_freeze.started"   → 開始フレームのみ 1.0
    //     "event:granular_freeze.progress"  → 0..1
    //     "event:warp_filter.cutoff_hz"     → そのイベント自身の 60fps 曲線
    //   時間（TIME — 音声/イベント JSON 不要。ParameterBus が dt を積算する
    //   自前の時計だけから決まるので、オフラインでもリアルタイムでも同じ dt
    //   列なら bit-exact に再現できる。無人インスタレーションで「一定の力を
    //   ずっと」を避け、ゆっくり強弱・反転させたいときに使う）
    //     "time.sin:<period>"    正弦、-1..1、1 周期 = <period> 秒
    //     "time.tri:<period>"    三角波、-1..1
    //     "time.ramp:<period>"   のこぎり波、0..1
    //     "time.noise:<period>"  滑らかな値ノイズ、-1..1。<period> 秒おきに
    //                            シード済み乱数を smoothstep で補間する
    //                            （白色ノイズではない）
    //   <period> は正の秒数。壊れた／0 以下の値はシーン読み込み時にその
    //   source 文字列ごと明確なエラーで弾く（黙って 0 を出さない）。
    //   任意で位相オフセットを "@<0..1>" で付けられる（既定 0）:
    //     "time.sin:600@0.25"  → 1/4 周期分オフセットして始まる
    //   time.noise はシーンの seed と source 文字列自身のハッシュから種を
    //   作るので、同じシーン内の複数の time.noise は互いに無相関だが、
    //   同じシーン・同じ seed なら常に同じ列を再現する。
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

    // シーンの seed。addMapping() が "time.noise" の種を派生させるのに使う
    // ので、time.noise なマッピングを足す前に呼ぶこと（Scene::applyBaseParams
    // はそうしている）。呼ばなければ 0 固定 — それでも決定的。
    void setSeed(uint32_t seed) { seed_ = seed; }

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
        // spec.source が "time.*" なら addMapping() 時点で解析済み。毎フレーム
        // 文字列を見なくていいのと、"time." で始まらない他ソースと分岐できる。
        TimeSourceKind timeKind = TimeSourceKind::None;
        float timePeriod = 0.0f;
        float timePhase = 0.0f;
        uint32_t noiseSeed = 0; // time.noise 専用。 deriveSeed(seed_, hash(source))
    };

    std::unordered_map<std::string, float> base_;
    std::unordered_map<std::string, float> addSum_;
    std::unordered_map<std::string, float> setValue_;
    std::unordered_map<std::string, float> mulProduct_;
    std::unordered_map<std::string, float> overrides_;
    std::vector<AudioMappingSpec> mappings_;
    std::vector<MappingState> mappingStates_;
    uint32_t seed_ = 0;
    // bus 自身が dt を積算する時計。time.* ソースはこれだけを見る — オーディオ
    // にも events JSON にも依存しない。SceneRunner::simTime_ と同じ dt 列で
    // 同じように 0 から進むので、両者は常に一致する。
    //
    // double であることは必須。float で 1/60 を積算すると、8 時間後には真の
    // 経過時間から 623 秒ずれる（28,800 秒付近の float の刻みは 0.00195 秒で、
    // dt の約 6% が毎フレーム丸め落ちる）。18 分周期なら位相にして 208 度。
    // さらに刻みが dt を超える 78 時間で、時計は完全に進まなくなる。
    double clockTime_ = 0.0;
};

} // namespace life
