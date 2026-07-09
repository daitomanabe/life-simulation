# Phase 7 実装仕様: LifePresetLab (探索ランナー)

設計: Fable / 実装: Sonnet。設計指示書 §4.5 に対応:
parameter sweep / seed sweep / thumbnail 生成 / 良い状態だけ保存。
全体制約は phase3 仕様の 9 項目（本フェーズは新アプリ 1 個なので、
既存コード変更は CMakeLists.txt への app ターゲット追記のみ）。

## 目的

シーンのパラメータ空間・seed 空間を自動探索し、サムネイル一覧
（自己完結 HTML ギャラリー + JSON）を吐く。VJ 前の仕込みで
「生きているパラメータ」を素早く見つける道具。

```bash
./build/LifePresetLab --scene Presets/lenia_basic.json \
    --sweep sweeps/lenia_mu_sigma.json \
    --frames 240 --thumb-at 120,239 \
    --width 384 --height 216 \
    --output lab/lenia_run1
```

## 新規/変更ファイル

```
新規:
  Apps/LifePresetLab/main.cpp
  sweeps/lenia_mu_sigma.json      (サンプル sweep 設定)
変更:
  CMakeLists.txt                  (app ターゲット追記のみ)
```

## 1. Sweep 設定 JSON

```jsonc
{
  "mode": "cartesian",            // "cartesian" | "per-axis"
  "seeds": [7777, 1234],          // 省略時はシーンの seed のみ
  "axes": [
    { "target": "lenia0.growthMu", "values": [0.13, 0.15, 0.17] },
    { "target": "lenia0.growthSigma", "range": [0.014, 0.022], "steps": 3 }
  ]
}
```

- `values` 直接指定 or `range`+`steps`（線形等分、両端含む）。
- `mode: "cartesian"` = 全軸の直積 × seeds。
- `mode: "per-axis"` = ベースシーンから 1 軸ずつ変える（軸ごとの感度を見る）。
- variant 総数が 500 を超えたらエラーで拒否（暴発防止）。

## 2. パラメータの適用方法（重要）

ParameterBus ではなく**シーン JSON の直接変異**で適用する:
`"lenia0.growthMu"` → `scene.modules[name=="lenia0"].params["growthMu"] = v`。
理由: setup 時にしか読まれないパラメータ（radius, agentCount,
particleCount 等）も sweep 対象にできる。seed は `scene.seed = v`。
対象モジュール名が存在しなければエラー。

各 variant は **SceneRunner を作り直して** frames 回 step する
（AudioFeatureState はゼロの無音。`--audio capture.jsonl` オプションで
CaptureReader 再生も可 — 音反応込みの探索）。

## 3. 出力

```
lab/lenia_run1/
  variants.json      (全 variant のメタデータ)
  index.html         (自己完結ギャラリー)
  v0000_f000120.png  v0000_f000239.png
  v0001_f000120.png  ...
```

- サムネイル: `--thumb-at` で指定したフレーム（カンマ区切り、既定 =
  最終フレームのみ）で readback → PNG。
- variants.json の各エントリ:
  ```jsonc
  {
    "id": "v0000",
    "seed": 7777,
    "overrides": { "lenia0.growthMu": 0.13, "lenia0.growthSigma": 0.014 },
    "thumbs": ["v0000_f000120.png", "v0000_f000239.png"],
    "avgGpuMs": 1.23,
    "activity": { "meanLuma": 0.18, "stddevLuma": 0.09, "alive": true }
  }
  ```
- **activity 指標**（「良い状態だけ保存」の根拠）: 最終サムネイルの
  readback ピクセル (RGBA16F) から輝度 L = 0.2126R+0.7152G+0.0722B を
  計算し、平均と標準偏差。`alive = (0.01 <= meanLuma <= 0.7) &&
  (stddevLuma >= 0.02)`（真っ黒/真っ白/のっぺりを弾く）。
- `--alive-only` フラグ: alive でない variant のサムネイル PNG を
  最後に削除（variants.json には activity ごと残す）。

## 4. index.html

外部依存なしの単一 HTML（インライン CSS/JS、CDN 禁止）:
- グリッドでサムネイル表示（variant id + override 値の短いラベル付き）
- stddevLuma 降順で初期ソート。ソート切替（activity / id / gpuMs）は
  シンプルな JS で
- クリックで overrides JSON を <pre> 表示（コピペでシーンに移せる）
- alive でないものは薄く表示
- 生成はテンプレート文字列を C++ で組むだけでよい（凝らない）

## 5. 実行の進め方

- variant を順番に実行（並列不要 — GPU は 1 個）。1 variant ごとに
  stderr に `[lab] v0003/0018 lenia0.growthMu=0.17 ... gpu 1.2ms alive=1`
  形式で 1 行。
- 中断再開: `--resume` で、両サムネイル PNG が既に存在する variant は
  スキップ（決定的なので結果は同一）。variants.json は毎回全体を書き直す
  （スキップ分は既存 PNG から activity を再計算せず、PNG が全部あれば
  エントリを再構築 — メタは overrides と thumbs のみ埋め、activity は
  再計算する。実装を単純に保つこと）。

## 6. 完了条件（エージェント自身で確認）

1. ビルド エラー 0。
2. サンプル sweep (`sweeps/lenia_mu_sigma.json`, per-axis でも cartesian
   でもよいが 12 variant 以下に収める) を
   `--frames 96 --width 256 --height 144` で実行し、exit 0・全 variant の
   PNG と variants.json と index.html が生成される。
3. variants.json が有効な JSON で、全エントリに activity が入っている。
4. 決定性: 同じコマンドを 2 回実行（別 output dir）し、対応する
   サムネイル PNG の md5 が全て一致。
5. 存在しないモジュールを target にした sweep がエラーメッセージ付き
   exit 1 になる。

見た目の調整・本格的な sweep 設計はレビュー側で行う。
