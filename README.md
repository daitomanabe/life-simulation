# LifeSimSuite

音に反応する生命シミュレーション群を Metal 上で実行する共通基盤。
設計指示書は [docs/Metal_Life_Simulation_Core_Design_v1.md](docs/Metal_Life_Simulation_Core_Design_v1.md)。

単発の VJ エフェクトではなく、**Lenia / Reaction Diffusion / Cellular Automata /
Slime Mold / Particle Life / Boids を同一の Metal 実行基盤（LifeCore）に載せる**
ことが目的。音は見た目ではなく**ルール**を変える（kick → birth rate、
snare → kernel/topology、hihat → micro noise、fft → force field）。

## 現状 (2026-07-09)

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | Core 基盤 + Reaction Diffusion + 3 アプリ | ✅ 完了 |
| 2 | Field 系完成 (Lenia / Cellular Automata / AudioToField) | ✅ 完了 |
| 3 | Particle 系 (ParticleSet2D / ParticleSplat / Slime Mold) | ✅ 完了 |
| 4 | Spatial Hash (Particle Life / Boids) | 実装中 |
| 5 | Field×Particle Coupling | 仕様済み ([docs/specs/](docs/specs/)) |

開発体制: 設計・レビュー・検証 = Fable、実装 = Sonnet サブエージェント。
各フェーズの実装仕様書は `docs/specs/phaseN_*.md`。

実測 (Apple M5 Max):
- RD 単体 1080p: **0.47 ms/frame** (~2100 fps 容量)
- Scene A (Lenia R13 + RD×8steps + CA + AudioToField + 4層 composite) 1080p:
  **7.75 ms/frame** (~129 fps 容量、支配項は Lenia direct convolution)
- Slime Mold 1M agents 1080p: **0.30 ms/frame** (~3400 fps 容量)
- 決定性: 同一 seed → 同一出力 (PNG MD5 一致を確認済み、Slime は
  整数 atomic 蓄積により粒子系でもバイト一致)

## ビルド

必要なもの: macOS (Apple Silicon), CommandLineTools, CMake 3.24+。
フル Xcode 不要 — **シェーダは実行時コンパイル**（`Shaders/*.metal` を起動時に
連結してコンパイル。hot reload 可能な構造）。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

外部依存はすべて `external/` に vendor 済み (nlohmann/json, CLI11, TinyEXR+miniz)。
PNG は ImageIO (macOS 標準) で書くため追加依存なし。

## アプリ

### LifeRealtime — VJ 用 headless リアルタイム実行

```bash
./build/LifeRealtime --scene Presets/field_basic.json --fps 60 --osc-port 9000 \
    --dump-every 300 --dump-dir frames --record capture.jsonl
```

- OSC 受信: `/kick /snare /hihat /perc /beat` (float 0..1)、`/fft` (float×128)
- 毎秒 status 行 (fps / GPU ms / pass 数 / OSC 流量 / 特徴量)
- `--record` で AudioFeatureState を JSONL 記録 → オフラインで再現可能
- テスト送信: `python3 tools/send_test_osc.py --port 9000 --bpm 128 --duration 30`

### LifeOfflineRender — 作品出力用 決定的レンダラー

```bash
./build/LifeOfflineRender --scene Presets/field_basic.json \
    --width 3840 --height 2160 --fps 30 --frames 3600 --substeps 4 \
    --audio capture.jsonl --output renders/shot001 --format both --resume
```

- 固定 dt / 固定 seed / `--audio` でライブ入力を再生 → **ライブの瞬間を高解像度で再現**
- PNG (sRGB, `--exposure`) + EXR (linear half) + `metadata.json`
- `--resume`: 既存フレームは書き出しスキップ（シムは回すので状態は正確）

### LifeBench — GPU 性能測定

```bash
./build/LifeBench --scene Presets/field_basic.json --sizes 512,1024,1920x1080 \
    --frames 240 --output benchmark.json
```

pass 別 GPU 時間 (MTLCounterSampleBuffer, encoder 境界)、readback コスト、
テクスチャ/バッファメモリを表示し JSON 保存。

## Scene JSON

```jsonc
{
  "seed": 1234,
  "resolution": [1920, 1080],
  "modules": [
    { "type": "Lenia", "name": "lenia0", "enabled": true,
      "params": { "radius": 13, "growthMode": 1, "dt": 0.35,
                  "blend": "add", "opacity": 1.0,
                  "colorMap": { "d": [0.62, 0.44, 0.30], "inputScale": 1.0 } } }
  ],
  "connections": [],          // Phase 5 で Field↔Particle 結線に使用
  "audioMappings": [
    { "source": "kick", "target": "lenia0.injectAmount",
      "scale": 0.8, "smoothing": 0.08 }
  ]
}
```

- `source`: `kick|snare|hihat|perc|beat` (+ `.raw/.smoothed/.peak/.trigger/.hold`)、
  `rms|low|mid|high|centroid|flux`
- 最終値 = シーンの base 値 + Σ(mapping の寄与)。モジュールは
  `param(ctx, "feed", default)` で音変調済みの値だけを見る（OSC を知らない）

同梱プリセット: `default.json` (RD), `lenia_basic.json`, `ca_basic.json`,
`audio_field.json`, `field_basic.json` (Scene A: 4 モジュール合成)。

## アーキテクチャ

```
OSC → AudioFeatureState → AudioMapping → ParameterBus → Uniforms → Metal
                 ↑ CaptureReplay (JSONL) で記録/再生 = Realtime と Offline の同一性

SceneRunner (全アプリ共通の実行コア; アプリは pacing と IO ポリシーだけ)
  beginFrame → module.encode ×N → Composite → [readback] → endFrame
```

```
LifeCore/
  Metal/   MetalContext(実行時シェーダコンパイル) PipelineCache ResourcePool
           CommandGraph(ComputePass記述→dispatch, pass毎GPU timing) GPUTimer
  Audio/   OSCReceiver(依存ゼロOSC 1.0) FeatureSmoother(attack/release/peak/
           trigger/hold + アイドル減衰) AudioInput
  Params/  ParameterBus AudioMapping Scene(JSON)
  Field/   Field2D PingPongTexture (BoundaryMode: Wrap/Clamp/Mirror/Zero)
  Render/  ColorMapPass(cosine palette) CompositePass(add/screen/multiply/max/alpha)
  IO/      FrameRecorder(readback) ImageWriter(PNG=ImageIO, EXR=TinyEXR)
           CaptureReplay(JSONL)
  Sim/     SimulationModule(interface) ModuleFactory SceneRunner SharedTypes
  Math/    Random(SplitMix64/PCG, GPU と同一ハッシュ)
Modules/
  FieldModules/  ReactionDiffusion  Lenia  CellularAutomata  AudioToField
Shaders/  (実行時に Common/ → 各モジュールの順で連結し 1 ライブラリにコンパイル)
Apps/     LifeRealtime  LifeOfflineRender  LifeBench (すべて薄い runner)
```

### 基盤としての設計ポイント（類似アプリへの転用）

- **ハンドルベース**: モジュール層は `TextureHandle`/`BufferHandle` しか見ない。
  Metal API は `.mm` (LifeCore/Metal, IO) に閉じ、シミュレーションモジュールは
  pure C++ で書ける。`LifeCore/Metal + Audio + Params + IO + Math` は生命シム
  非依存の汎用層で、別のオーディオリアクティブ Metal アプリの土台にそのまま使える。
- **リソースは ResourcePool 経由のみ**（リーク検出・メモリ計測が常時可能）。
- **モジュールは encode するだけ**。command buffer の所有・commit・GPU timing は
  CommandGraph に集約。
- **全出力は RGBA16F に統一** → Composite が単純化、Field/Particle の合成が破綻しない。
- **runner だけを分ける**: Realtime / Offline / Bench は同じ SceneRunner を使う。

## シミュレーションメモ

- **ReactionDiffusion**: Gray-Scott、Karl Sims 定式化 (Du=1.0, Dv=0.5,
  laplacian 0.2/0.05, dt=1)。`stepsPerFrame`(既定8) × runner `--substeps`。
  ⚠ Pearson の連続系係数 (Du=0.16) をそのまま使うと成長が 6 倍遅く四角く崩れる。
- **Lenia**: 単一チャンネル、直接畳み込み (R=13 → 27×27)、カーネルは CPU 生成
  Σ=1 正規化 R32F テクスチャ。`growthMode`: 0=標準 (2·bell−1) は
  ランダムスープから崩壊しやすい / **1=asymptotic (bell−A) は持続する**（既定）。
  `injectAmount` が音駆動の「誕生」（kick にマップ推奨）。
- **CellularAutomata**: rule 0=Life, 1=Brian's Brain, 2=Seeds, 3=Cyclic。
  `injectAmount` (hihat 推奨) でセル注入。
- **AudioToField**: fft[128] → R16F field。mode 0=スクロール spectrogram /
  1=radial アナライザ (decay trail)。Phase 5 で他モジュールのルール変調に接続予定。

## 次のステップ (設計指示書 §19)

1. **Phase 3**: ParticleSet2D (SoA buffers) + ParticleSplat + TrailDeposit +
   FieldSampler → Slime Mold (100k agents)
2. **Phase 4**: SpatialHashGrid (cell sort + range) → Particle Life / Boids
3. **Phase 5**: FieldToForce / connections 実装 → Scene D「Coupled Life」
4. Lenia FFT convolution (MPSGraph FFT / vDSP)、multi-kernel、organism preset
5. 出力抽象化: Syphon / NDI (LifeRealtime の出力先として)
