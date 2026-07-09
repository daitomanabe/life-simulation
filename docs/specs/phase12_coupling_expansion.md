# Phase 12 実装仕様: 結合ポート拡張 — Fluid ⇄ Slime 双方向生態系

設計: Fable / 実装: Sonnet。設計指示書 §10.1「Particle writes Field /
Field modulates Particle rules」の拡張。狙いは **Fluid と Slime Mold の
双方向結合**: スライムのトレイルが流体を掻き混ぜ、流体がスライムを流す。
全体制約 9 項目 + 既存プリセット md5 級の後方互換（新ポート未使用時の
挙動不変）を厳守。

## 新しい結合ポート（2 個）

### 1. Fluid の `forceField` 入力（Field → Fluid）

- `FluidModule::bindNamedInput("forceField", h)` — 任意のスカラー field
  (R チャンネル) を受け、その**勾配**を速度場への力として加える
  （ParticleLife の fieldForce と同じ発想の場版）。
- FluidParams 末尾に `float forceFieldGain;` 追加
  （static_assert 更新、scene param "forceFieldGain" 既定 0）。
- fluidForces カーネルに `access::sample` テクスチャを 1 本追加し:
  ```metal
  if (p.forceFieldGain != 0.0f) {
      float e = 2.0f;
      float gx = sampleFieldWrap(ff, pixelPos + float2(e,0), w, h)
               - sampleFieldWrap(ff, pixelPos - float2(e,0), w, h);
      float gy = sampleFieldWrap(ff, pixelPos + float2(0,e), w, h)
               - sampleFieldWrap(ff, pixelPos - float2(0,e), w, h);
      vel += float2(gx, gy) * p.forceFieldGain;
  }
  ```
- フォールバックは既存流儀の 4×4 黒 Shared テクスチャ。

### 2. SlimeMold の `flowField` 入力（Fluid velocity → Slime）

- `SlimeMoldModule::bindNamedInput("flowField", h)` — RG 速度場を受ける。
- SlimeParams 末尾に `float flowWeight;` 追加（scene param 既定 0）。
- slimeMove に velocity テクスチャ（`access::sample`、空いている index）
  を追加し、移動段で:
  ```metal
  float2 flow = sampleFieldWrap4(flowTex, pos, w, h).xy;
  pos += (heading * p.moveSpeed + flow * p.flowWeight) * p.dt;
  ```
  （heading 自体は変えない — 感覚は保ちつつ「流される」。
  Boids の flowField が操舵に効くのと対照的な設計で、
  スライムは「水中の菌」の挙動になる）

## プリセット Presets/fluid_slime.json（双方向デモ）

1280×720, seed 51515:
1. fluid0 (Phase 8 の fluid_basic と同じベース値、opacity 0.7, blend add,
   forceFieldGain 60)
2. slime0 (agentCount 300000, flowWeight 1.6, depositAmount 1.0,
   blend screen, opacity 0.9)

connections:
```json
[
  { "from": "slime0.trail",    "to": "fluid0.forceField" },
  { "from": "fluid0.velocity", "to": "slime0.flowField" }
]
```

audioMappings: kick→fluid0.impulse (250, sm 0.05) と
kick→fluid0.dyeInject (0.4, sm 0.05)、kick→slime0.depositAmount (1.2),
hihat→fluid0.turbulence (1.5) + slime0.jitter (2.0),
snare.trigger→slime0.sensorBoost (1.0), beat.trigger→slime0.resetPulse (0.4)。

## 完了条件（エージェント自身で確認）

1. ビルド エラー 0。`bash scripts/verify.sh`（SKIP_BUILD=1 可）が
   ALL GREEN（既存プリセット全部 + 決定性 — 新ポート未使用時の
   後方互換の機械的証明）。
2. fluid_slime.json 300f: exit 0・エラー行 0・非単色。
3. md5 決定性 ×2。
4. 結合実効 ×2 方向: forceFieldGain=0 版・flowWeight=0 版それぞれと
   baseline の最終 md5 が異なる。
5. LifeBench fluid_slime 720p の gpu/frame を報告。

見た目の調整はレビュー側。
