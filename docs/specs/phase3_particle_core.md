# Phase 3 実装仕様: Particle Core + Slime Mold

設計: Fable / 実装: Sonnet。設計指示書 §8, §12.4, §19 Phase 3 に対応。
参照実装（既存の流儀を必ず踏襲）: `Modules/FieldModules/Lenia.{h,cpp}`,
`Shaders/Field/Lenia.metal`, `LifeCore/Field/Field2D.h`,
`LifeCore/Render/ColorMapPass.h`, `LifeCore/Sim/SimulationModule.h`,
`LifeCore/Metal/CommandGraph.h`。

## 全体制約（違反禁止）

1. モジュールは pure C++ (.cpp)。Metal ヘッダを include しない。GPU 操作は
   Handle + `ctx.graph->pass(...)` 経由のみ。
2. uniform 構造体は **スカラー float/uint のみ**（simd 型・bool 禁止）。C++ 側
   (モジュール .h 内 private struct) と MSL 側 (モジュール .metal 内) を
   **同一順序で手書きミラー**する。
3. GPU リソースは全て `ctx.resources`（ResourcePool）経由。label は
   `instanceName_ + ".xxx"`。
4. 乱数は seed 由来のみ: CPU は `deriveSeed`/`pcgHash` (LifeCore/Math/Random.h)、
   GPU は `rand01(uint2, tag, seed)` (Shaders/Common/Random.metal)。
5. **float への atomic 禁止**。蓄積は `atomic_uint` 固定小数点（scale 256 または
   1024）— 整数加算は順序非依存なので決定性が保たれる。
6. 位置の wrap は toroidal: `pos = fract(pos / size) * size` 相当
   （負値対応必須）。テクスチャ近傍は `wrapCoord`。
7. 出力は RGBA16F（ColorMapPass + 追加スプラット）。
8. 既存コードの改変は CMakeLists.txt / Modules/RegisterModules.cpp への追記のみ。
9. shader 構造体名はライブラリ全体で一意（全 .metal が連結コンパイルされるため）。

## 新規ファイル

```
LifeCore/Particle/ParticleSet2D.h            (header-only, Field2D.h と同じ流儀)
LifeCore/Particle/ParticleSplatPass.h/.cpp   (ColorMapPass と同じ流儀の再利用パス)
Shaders/Common/Sampling.metal                (FieldSampler: bilinear wrap sample)
Shaders/Particle/ParticleSplat.metal
Shaders/Particle/SlimeMold.metal
Modules/ParticleModules/SlimeMold.h/.cpp
Presets/slime_basic.json
```

## 1. ParticleSet2D (LifeCore/Particle/ParticleSet2D.h)

SoA バッファ束。header-only。

```cpp
struct ParticleSet2DDesc {
    uint32_t capacity = 0;
    bool pingPongPosVel = false;  // 粒子間相互作用シムは true (Phase 4)
    std::string label;
};

class ParticleSet2D {
    bool create(ResourcePool& pool, const ParticleSet2DDesc& desc);
    void destroy();
    // pingPong 無効時は read/write が同一ハンドル
    BufferHandle positions() const;        BufferHandle positionsWrite() const;
    BufferHandle velocities() const;       BufferHandle velocitiesWrite() const;
    void swap();
    BufferHandle species() const;      // uint × capacity
    BufferHandle attributes() const;   // ParticleAttributes × capacity
    BufferHandle randomState() const;  // uint × capacity
    uint32_t capacity() const;
    ParticleSetHandle handle() const;  // SimulationModule.h の型に read 側を詰める
};
```

- バッファサイズ: positions/velocities = `capacity * 8` (float2)、species/random
  = `capacity * 4`、attributes = `capacity * 20`（age,life,mass,radius: float +
  flags: uint — 設計指示書 §8.2 の ParticleAttributes）。
- 全て StorageMode::GPUPrivate。label: `<label>.pos.a` など。

## 2. FieldSampler (Shaders/Common/Sampling.metal)

```metal
// posPx: ピクセル座標。repeat sampler で toroidal bilinear。
inline float sampleFieldWrap(texture2d<float, access::sample> t, float2 posPx,
                             float w, float h) {
    constexpr sampler s(filter::linear, address::repeat, coord::normalized);
    return t.sample(s, posPx / float2(w, h)).x;
}
```

注意: 読む側の texture 引数は `access::sample` で受けること。

## 3. ParticleSplat (再利用パス)

Shaders/Particle/ParticleSplat.metal:

```metal
struct SplatParams {
    uint particleCount; uint width; uint height;
    float weight;        // 1粒子の寄与 (固定小数点 scale 256 で加算)
    float gainR; float gainG; float gainB;  // resolve 時の色
    float gain;
};
kernel void splatClear(device atomic_uint* density, constant SplatParams&, uint id);
    // id < width*height: atomic_store 0
kernel void splatAccumulate(device const float2* positions,
                            device atomic_uint* density,
                            constant SplatParams&, uint id);
    // id < particleCount: cell = wrap(floor(pos)); atomic_fetch_add(weight*256)
kernel void splatResolve(device atomic_uint* density,
                         texture2d<float, access::read_write> target,
                         constant SplatParams&, uint2 gid);
    // d = atomic_load / 256; target += float4(gain{R,G,B} * d * gain, 0)
    // その後 atomic_store 0 (自セルのみなので安全) → splatClear は初期化時のみ必要
```

LifeCore/Particle/ParticleSplatPass.h/.cpp — ColorMapPass と同じ形:

```cpp
class ParticleSplatPass {
public:
    struct Params { float weight=1.0f; float r=1, g=1, b=1; float gain=1.0f; };
    // density バッファ (w*h*4 bytes) は内部で lazily 作成 (pool 経由, label prefix)
    void encode(CommandGraph& graph, const std::string& labelPrefix,
                BufferHandle positions, uint32_t count,
                TextureHandle target, uint32_t w, uint32_t h, const Params& p);
};
```

encode 内: 初回のみ density 作成 + splatClear → 毎回 splatAccumulate
(dispatch1D(count)) → splatResolve (dispatch2D(w,h))。

## 4. Slime Mold (Jones 2010 Physarum モデル)

### 状態
- ParticleSet2D: positions = 位置(px)、velocities = **単位方向ベクトル**
  (heading)。pingPong 不要（自分の状態しか書かない）。
- trail: Field2D R16F ping-pong (シーン解像度、Wrap)。
- deposit: BufferHandle uint × (w*h)（atomic 蓄積、固定小数点 1024）。
- output: RGBA16F テクスチャ。

### SlimeParams (C++/MSL ミラー、この順)

```c
uint  agentCount;
float dt;             // 秒
float moveSpeed;      // px/s
float sensorAngle;    // rad (片側)
float sensorBoost;    // snare → sensorAngle 倍率ブースト (0..1)
float sensorDistance; // px
float turnSpeed;      // rad/s
float turnImpulse;    // perc → ランダム転回の追加確率強度
float jitter;         // hihat → 方向ノイズ (rad/frame スケール)
float depositAmount;  // 1 agent が 1 step で置く量
float resetPulse;     // beat → respawn 割合ゲート
float decayRate;      // trail 減衰 (1/s), trail *= exp(-decayRate*dt)
float diffuseRate;    // 0..1: 3x3 平均へのブレンド率
uint  spawnMode;      // 0 uniform, 1 filled circle, 2 ring
uint  seed;
uint  frameIndex;
uint  width;
uint  height;
```

### カーネル (Shaders/Particle/SlimeMold.metal)

1. `slimeInit(pos, heading, random, SlimeParams)` — dispatch1D(agentCount)。
   spawnMode に応じ配置（rand01 は `uint2(id, 定数タグ)` で呼ぶ）。heading は
   ランダム角の unit vec。randomState[id] = pcg_hash(id ^ seed)。
2. `slimeClearTrail(texture write)` / `slimeClearDeposit(device atomic_uint*)` —
   reset 時のみ。
3. `slimeMove(pos, heading, random, trailRead(sample), deposit(atomic), SlimeParams,
   AudioUniforms)` — dispatch1D:
   - sensorAngleEff = sensorAngle * (1 + sensorBoost)
   - 3 センサ (front / ±sensorAngleEff, 距離 sensorDistance) で
     `sampleFieldWrap(trail, ...)`
   - Jones 標準ステア: F>FL && F>FR → 直進; FL>FR → +turn; FR>FL → −turn;
     F 最小（両側が強い）→ ランダムに ±turn。turn 量 = turnSpeed * dt。
   - jitter: heading をランダム角 ±jitter*dt で揺らす。
   - turnImpulse: rand < turnImpulse*0.05 → ランダム方向へ大きく転回。
   - resetPulse: rand < resetPulse*0.02 → 位置・方向を respawn。
   - pos += heading * moveSpeed * dt; toroidal wrap。
   - deposit: `atomic_fetch_add(&deposit[cellIndex], uint(depositAmount * 1024))`。
4. `slimeTrailUpdate(trailIn read, trailOut write, deposit device uint*,
   SlimeParams)` — dispatch2D:
   - mean = 3x3 平均 (wrapCoord)
   - v = mix(center, mean, diffuseRate) * exp(-decayRate * dt)
   - v += float(deposit[idx]) / 1024.0
   - deposit[idx] = 0（自セル、plain write で可）
   - trailOut = v

### モジュール (Modules/ParticleModules/SlimeMold.h/.cpp)

`class SlimeMoldModule : public ParticleModule` + `TextureHandle outputField()
const`（trail read 側を返す独自メソッド、Phase 5 coupling 用）。

- setup: ctx.width/height でリソース作成。agentCount は params_ から
  (`"agentCount"`, 既定 200000)。ColorMapPass 構成（既定 channel 0,
  inputScale 1.4, d=[0.55,0.42,0.30]）。ParticleSplatPass メンバも作る。
- reset: `seed_ = deriveSeed(seed, 0x534C4D31)` ("SLM1"), needsInit フラグ。
- encode: param() で全パラメータ取得 → needsInit なら init 3 パス →
  substeps × stepsPerFrame(既定1) 回 { slimeMove → slimeTrailUpdate → swap } →
  colorMap → `showAgents` param > 0 なら splat (weight=showAgents,
  白 gain 0.35)。
- particles() は set_.handle() を返す。outputTexture() は output_。

### プリセット Presets/slime_basic.json (Scene B, §23.2)

1280×720, seed 909, SlimeMold 単体:
agentCount 200000, moveSpeed 55, sensorAngle 0.6, sensorDistance 9,
turnSpeed 10, depositAmount 1.0, decayRate 1.8, diffuseRate 0.35,
jitter 0.4, spawnMode 0, showAgents 0, blend "add", opacity 1.0。
audioMappings (§12.4):
- kick → slime0.depositAmount, scale 1.2, smoothing 0.08
- snare.trigger → slime0.sensorBoost, scale 1.0, smoothing 0.02
- hihat → slime0.jitter, scale 2.5, smoothing 0.05
- perc → slime0.turnImpulse, scale 1.0, smoothing 0.03
- beat.trigger → slime0.resetPulse, scale 0.6, smoothing 0.02

## 5. 統合

- CMakeLists.txt: lifecore ターゲットに ParticleSplatPass.cpp と SlimeMold.cpp
  を追加（コメント行 `# Modules` / `# Render` の流儀を守る）。
- Modules/RegisterModules.cpp: `f.registerType("SlimeMold", ...)` 追加。

## 6. 完了条件（エージェントが自分で確認するもの）

1. `cmake --build build -j8` がエラー 0。
2. `./build/LifeOfflineRender --scene Presets/slime_basic.json --frames 120
   --output /tmp/slime_smoke --format png` が正常終了し、
   `/tmp/slime_smoke/frame_000119.png` が 30KB 以上（≒ 非単色 = 網目が出て
   いる）。stderr に `[life] pass ...` のエラー行が 1 つも出ないこと。
3. 同コマンドを 2 回実行し最終フレームの md5 が一致（決定性）。

深い視覚検証・ベンチ・チューニングはレビュー側（Fable）が行うので不要。
