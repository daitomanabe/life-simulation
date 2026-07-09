# Phase 8 実装仕様: Fluid (Stable Fluids) + Boids flowField 結合

設計: Fable / 実装: Sonnet。設計指示書 §21.1 (Stam 1999 / GPU Gems ch.38)。
導入条件「Field2D と Coupling が安定してから」は Phase 5 完了で成立。
全体制約は phase3 仕様の 9 項目を厳守。

## 目的

音で駆動する 2D 流体（速度場 + 染料場）。染料が絵になり、速度場が
`connections` で他モジュールを流す（第一弾: Boids が流れに乗る）。

## 設計要点

- **境界は toroidal (wrap)** — §7.5 の哲学どおり。壁境界条件が消えるので
  実装が大幅に単純化する（advect も Jacobi も全て wrapCoord / repeat
  sampler でよい。障害物なし）。
- **vorticity confinement 必須** — これがないと渦が数秒で拡散して
  死んだ絵になる（VJ 的に本質）。
- 全パス compute、field は ResourcePool 経由、乱数は seed 由来
  （既存制約どおり）。

## 新規/変更ファイル

```
新規:
  Modules/FieldModules/Fluid.h/.cpp
  Shaders/Field/Fluid.metal
  Presets/fluid_basic.json
変更:
  Modules/ParticleModules/Boids.h/.cpp   (flowField 入力ポート追加)
  Shaders/Particle/Boids.metal           (BoidsParams 末尾 + flow サンプル)
  Modules/RegisterModules.cpp            ("Fluid" 登録)
  CMakeLists.txt                         (Fluid.cpp 追加)
  Presets/coupled_life_basic.json は変更しない（新プリセットで検証）
```

## 1. FluidModule (FieldModule)

fields (全て ping-pong, wrap):
- velocity: RG32F
- dye: RGBA16F（色付き染料 — そのまま絵になる）
- pressure: R32F
- divergence: R32F (single, ping-pong 不要)

`namedOutput`: "velocity" → velocity.read(), "field"/"dye" → dye.read(),
"output" → outputTexture()。outputTexture は dye を ColorMap **せず**
そのまま使いたいので、encode 最後に dye → output_ (RGBA16F) へ
exposure だけ掛ける軽い kernel (fluidPresent) でコピー（dye は
既に色。ColorMapPass は使わない）。

### FluidParams (C++/MSL ミラー、この順。C++ 側に static_assert を書く)

```c
uint  width; uint height;
float dt;
float velDissipation;   // 速度の維持率/秒 (0.05 → *= exp(-0.05*dt))
float dyeDissipation;   // 染料の維持率/秒
float vorticity;        // confinement ε (0=off, 2-6 が実用域)
float impulse;          // kick → 速度+染料インパルス強度
float impulseRadius;    // px
float turbulence;       // hihat → 小スケールランダム力
float injectHue;        // 注入色相の基準 (beat で回すのは mapping で)
float low; float mid; float high; // CPU で AudioFeatureState から転記
uint  seed; uint frameIndex;
```

## 2. カーネル (Shaders/Field/Fluid.metal)

repeat sampler の bilinear は既存 `sampleFieldWrap`（.x のみ）に加え、
**float4 版 `sampleFieldWrap4` を Sampling.metal に追記**（dye/velocity 用。
既存関数は変更しない）。

1. `fluidClear(vel write, dye write, ...)` — reset 時。
2. `fluidAdvectVel(velR sample, velW write, params)` —
   semi-Lagrangian: `x_prev = pos - vel(pos)*dt` → bilinear sample →
   `*= exp(-velDissipation*dt)`。
3. `fluidForces(velR read, velW write, params)` — 以下を加算:
   - kick インパルス: 位置 = hash(seed, frameIndex/20) のランダム点
     （12 フレーム毎に移動）、`vel += impulse * exp(-r²/R²) * dir`
     (dir = 放射状)。impulse は CPU 側で kick マッピング済みの値。
   - turbulence: hihat 由来。per-pixel の rand01 角度の微小力。
   - fft 帯域力: low → 画面下 1/3 に上向きの浮力様の力 ×low、
     mid → 中央帯に横向きシア ×mid、high → 上 1/3 に微細撹拌 ×high
     （「fft が force field を変形する」§1.2 の実装）。
4. `fluidVorticity(velR read, velW write, params)` — curl を中心差分で
   計算し confinement force を加算（curl の |∇|curl|| 正規化、標準実装）。
   1 パスで curl 計算と force 適用を行ってよい（近傍 curl を再計算する
   コストは許容、テクスチャを増やさない）。
5. `fluidDivergence(velR read, div write, params)` — 中心差分、wrap。
6. `fluidJacobi(pR read, div read, pW write, params)` — 標準 Jacobi 反復:
   `p' = (pL+pR+pT+pB - div) / 4`。C++ 側で `jacobiIterations`
   (scene param, 既定 28) 回 ping-pong。初回反復の前に pressure を
   クリアする kernel は不要 — 前フレームの pressure を初期値に使う
   （warm start、収束が速い）。reset 時のみクリア。
7. `fluidProject(velR read, p read, velW write, params)` —
   `vel -= ∇p`（中心差分、wrap）。
8. `fluidAdvectDye(dyeR sample4, velR read, dyeW write, params)` —
   dye を速度で advect、`*= exp(-dyeDissipation*dt)`、さらに
   kick インパルス位置に染料注入: 色 = cosine palette(injectHue +
   帯域重心)、量 = impulse。
9. `fluidPresent(dye read, out write, params)` — dye.rgb → output、
   alpha=1。

encode 順: (init) → AdvectVel → Forces → Vorticity → Divergence →
Jacobi×N → Project → AdvectDye → Present。substeps はループ全体に
掛ける（ctx.substeps、既定 1）。

## 3. Boids flowField 入力

- `bindNamedInput("flowField", h)` — velocity field (RG) を受ける。
  フォールバックは既存流儀の 4×4 黒 Shared テクスチャ。
- BoidsParams **末尾**に `float flowWeight;` 追加（static_assert 更新）。
- boidsStep: `float2 flow = sampleFieldWrapRG(field, pos, W, H)`
  （RG 用ヘルパーを Fluid.metal でなく Sampling.metal に追記してよい —
  float4 版の .xy を使うなら不要）。
  `acc += (flow * flowGain - vel) * flowWeight`。flowGain は固定 1.0 で
  よい（速度場は px/s スケールで作る）。scene param "flowWeight" 既定 0。

## 4. プリセット

### Presets/fluid_basic.json
1280×720, seed 606: Fluid 単体。velDissipation 0.02, dyeDissipation 0.12,
vorticity 4.0, impulse 基本 0.4, impulseRadius 60, jacobiIterations 28。
audioMappings: kick→fluid0.impulse (scale 5.0, sm 0.05),
hihat→fluid0.turbulence (1.5, sm 0.05), beat.trigger→fluid0.injectHue
(0.13, sm 0 — 累積はしない仕様なので base+trigger で色相が拍ごとに揺れる)。

### Presets/fluid_boids.json (結合デモ)
fluid0 (opacity 0.85, blend add) + boids0 (30000 羽, flowWeight 2.5,
blend screen, opacity 0.8)。
connections: `fluid0.velocity → boids0.flowField`。
audioMappings は fluid_basic と boids_basic の合成。

## 5. 完了条件（エージェント自身で確認）

1. ビルド エラー 0。
2. 回帰: coupled_life_basic / boids_basic / slime_basic 各 60f
   エラーなし（Boids 構造体変更後も既存プリセットが動く）。
3. `LifeOfflineRender --scene Presets/fluid_basic.json --frames 300`
   正常終了・最終 PNG 非単色（渦が見える）・エラー行 0。
   ⚠ 無音では kick=0 なので、impulse の **base 値 0.4** が常時弱く
   注入し続ける設計であること（無音でも絵が完全に死なないため）。
4. 決定性: fluid_basic 2 回で md5 一致。
5. 結合実効: fluid_boids で boids0.flowWeight=0 の一時シーンと比較して
   最終 md5 が異なる。
6. LifeBench fluid_basic 1280x720 で gpu/frame を報告（Jacobi28 込みで
   3ms 以下が目安、超えても失敗ではなく報告）。

数値・見た目の調整（vorticity/dissipation/注入色）はレビュー側で行う。
