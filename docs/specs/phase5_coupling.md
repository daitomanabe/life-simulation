# Phase 5 実装仕様: Field × Particle Coupling (Scene D)

設計: Fable / 実装: Sonnet。設計指示書 §10, §19 Phase 5, §23.4 Scene D に対応。
前提: Phase 3 + Phase 4 マージ済み。全体制約は phase3 仕様の 9 項目を厳守。

## 目的

Scene JSON の `connections` を実働させ、モジュール間でフィールドを渡す:

```json
"connections": [
  { "from": "lenia0.field", "to": "slime0.attractorField", "mode": "sample" }
]
```

個別デモではなく「一つの生命系」に見える状態（§23.4）を作る。

## 設計の要点

1. **ping-pong フィールドは read 側ハンドルが毎フレーム入れ替わる**。
   setup 時に固定ハンドルを配るのは誤り。SceneRunner が **毎フレーム
   encode 直前に再バインド**する。
2. ポート解決は SimulationModule の仮想メソッドで統一（dynamic_cast の
   型分岐をしない）。
3. 入力が未接続でもシェーダは同じパイプラインで動く: 各消費モジュールは
   setup 時に **4×4 黒テクスチャ (Shared + uploadTexture でゼロ充填)** を
   フォールバックとして作り、常にバインドする。

## 1. SimulationModule への追加 (LifeCore/Sim/SimulationModule.h)

既存 API は変更せず、以下を base class に追加（デフォルト実装付き）:

```cpp
// 名前付き出力ポート。"field" は FieldModule の outputField 相当。
virtual TextureHandle namedOutput(const std::string& port) const { return {}; }
// 名前付き入力ポート。受け付けたら true。毎フレーム呼ばれる。
virtual bool bindNamedInput(const std::string& port, TextureHandle h) {
    (void)port; (void)h; return false;
}
```

各モジュールの override:
- ReactionDiffusion / Lenia / CellularAutomata / AudioToField:
  `namedOutput("field")` → `outputField()`、`"output"` → `outputTexture()`。
- SlimeMold: `namedOutput("field")` と `"trail"` → trail read 側。
- ParticleLife / Boids: `namedOutput("output")` → outputTexture() のみ。

## 2. SceneRunner の connections 実装

- create 時: `scene.connections` を解析し
  `{srcModule*, srcPort, dstModule*, dstPort}` の解決済みリストを保持。
  モジュール名/ポートが解決できなければ create 失敗（エラー文字列）。
  ("lenia0.field" → name="lenia0", port="field"。'.' が無ければ port="field")
- step() 内、`m->encode(ctx)` ループの**直前**に毎フレーム:
  ```cpp
  for (auto& c : resolvedConnections_)
      c.dst->bindNamedInput(c.dstPort, c.src->namedOutput(c.srcPort));
  ```
- モジュールは encode 順 = scene の modules 配列順なので、上流を配列の
  先に書けば同フレーム結果が、後に書けば前フレーム結果が渡る（1 フレーム
  遅延は許容仕様。ドキュメントコメントに明記）。

## 3. 消費側モジュールの拡張

### 3a. SlimeMold — `attractorField` 入力 (§10.2 FieldSampler / §12.4 fft→attractor)

- `bindNamedInput("attractorField", h)`: メンバに保持。
- setup: fallback 黒 4×4 を作成し初期値に。
- SlimeParams 末尾に `float attractorWeight;` を追加（C++/MSL 両方、順序末尾）。
  scene param "attractorWeight" 既定 0.0。
- slimeMove: attractor テクスチャ引数を追加 (`access::sample`,
  texture index 1 に。trail は 0 のまま)。センサ評価を
  `sense = trail + attractorWeight * attractor` に変更
  （attractor も `sampleFieldWrap`）。

### 3b. ReactionDiffusion — `feedMap` 入力 (§10.1 Field modulates rules)

- RDParams 末尾に `float feedMapGain;` 追加。scene param 既定 0.0。
- rdStep に feedMap テクスチャ (`access::sample`, index 2) を追加:
  `feedLocal = p.feed + p.feedMapGain * sampleFieldWrap(feedMap, pos, w, h)`
  を反応式の feed に使用（既存の feed 参照を feedLocal に置換）。

### 3c. ParticleLife — `forceField` 入力 (§10.2 FieldToForce)

- PLParams 末尾に `float fieldForce;` 追加。scene param 既定 0.0。
- plStep に field テクスチャ (`access::sample`) を追加。粒子位置で勾配:
  ```metal
  float e = 2.0f;
  float gx = sampleFieldWrap(field, pi + float2(e,0), W, H)
           - sampleFieldWrap(field, pi - float2(e,0), W, H);
  float gy = sampleFieldWrap(field, pi + float2(0,e), W, H)
           - sampleFieldWrap(field, pi - float2(0,e), W, H);
  force += float2(gx, gy) * p.fieldForce;
  ```

## 4. Scene D プリセット (Presets/coupled_life_basic.json, §23.4)

1280×720, seed 424242。modules 配列順 = 依存順:
1. lenia0 (asymptotic, injectAmount 0.15, blend add, opacity 0.55)
2. af0 (AudioToField mode 1, blend screen, opacity 0.2)
3. slime0 (agentCount 300000, attractorWeight 0.8, blend screen, opacity 0.85)
4. rd0 (feed 0.0545, kill 0.062, feedMapGain 0.015, stepsPerFrame 6,
   blend screen, opacity 0.4)
5. pl0 (particleCount 80000, species 6, fieldForce 40, blend add, opacity 0.9)

connections:
```json
[
  { "from": "lenia0.field", "to": "slime0.attractorField", "mode": "sample" },
  { "from": "slime0.trail", "to": "rd0.feedMap",           "mode": "sample" },
  { "from": "rd0.field",    "to": "pl0.forceField",        "mode": "sample" }
]
```

audioMappings（各モジュールの既存パラメータへ §15.1 どおり）:
kick→lenia0.injectAmount(0.7), kick→slime0.depositAmount(1.0),
snare.trigger→pl0.shufflePulse(1.0), snare.trigger→lenia0.muJitter(0.8),
hihat→slime0.jitter(2.0), hihat→pl0.jitter(25),
beat.trigger→slime0.resetPulse(0.5), rms→af0.gain(0.8),
kick→pl0.forceBoost(1.0)。

## 5. 完了条件（エージェント自身で確認）

1. ビルド エラー 0。
2. 既存プリセット (default / lenia_basic / slime_basic / particle_life_basic /
   boids_basic / field_basic) が**全て**引き続き 60 フレーム正常レンダリング
   できる（後方互換の確認。エラー行ゼロ）。
3. `LifeOfflineRender --scene Presets/coupled_life_basic.json --frames 240
   --output /tmp/coupled_smoke --format png` 正常終了、最終 PNG 60KB 以上。
4. 決定性: 同 2 回で md5 一致。
5. **結合の実効確認**: attractorWeight を 0 にした一時シーンと比較し、
   slime トレイルの最終 PNG md5 が**異なる**こと（= attractor 結線が実際に
   挙動を変えている）。rd0.feedMapGain / pl0.fieldForce も同様に 0 比較で
   md5 差を確認。

見た目のバランス調整（opacity/gain 等）はレビュー側で行う。
