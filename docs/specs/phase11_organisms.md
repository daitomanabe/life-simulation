# Phase 11 実装仕様: Lenia オーガニズムプリセット (stamp init)

設計: Fable / 実装: Sonnet。設計指示書 §12.2 次段階「organism preset」。
既知の Lenia 生物（Orbium 等）を公式データから移植し、フィールドに
スタンプして「本物の生物が泳ぐ」状態を作る。
全体制約 9 項目 + Phase 9 の後方互換条件（lenia_basic md5 不変）を厳守。

## データの出所（重要 — 捏造禁止）

生物の cells データを**記憶から書き起こすことは禁止**（数値が 1 つ
違うだけで死ぬ）。必ず公式リポジトリから取得する:

1. `git clone --depth 1 https://github.com/Chakazul/Lenia /tmp/lenia_official`
2. パターンデータ（`Python/animals.json` 等、RLE エンコードされた
   cells + params {R, T, m, s, b}）を特定
3. **リポジトリ内の RLE デコーダ実装**（Python/LeniaND.py や JavaScript
   実装内の rle2arr 相当）を読み、その仕様を忠実に Python スクリプトへ
   移植して cells をデコード（推測でデコード規則を書かない）
4. デコード結果を我々のスキーマで保存:

```jsonc
// Presets/organisms/orbium.json
{
  "name": "Orbium",
  "source": "Chakazul/Lenia animals.json (code=O2u?)",
  "R": 13, "T": 10, "mu": 0.15, "sigma": 0.015 相当,   // 公式値
  "betas": [1.0],
  "cells": [[0.0, 0.1, ...], ...]   // row-major float 2D
}
```

Orbium（必須）+ もう 1〜2 種（回転系 or 大型を任意選定、同スキーマ）。
変換スクリプトは `tools/import_lenia_organism.py` として恒久保存
（再現可能性のため。実行には /tmp のクローンが必要な旨をヘッダに記載）。

## Lenia モジュール拡張

scene params:
- `initMode`: "noise"（既定、従来）| "stamp"
- `organism`: organisms JSON のパス（stamp 時必須）
- `stampCount`: 個数（既定 6、最大 32）
- `stampRotate`: true で 90°単位回転 + 反転を個体ごとにランダム適用（既定 true）
- `useOrganismParams`: true（既定）で radius/growthMu/growthSigma/dt を
  organism ファイルの値で上書き（dt = 1/T）。kernels 配列との併用は
  不可（エラー）。**growthMode は 0 (standard) を既定にすること** —
  Orbium は standard growth の生物であり asymptotic では別物になる。

実装:
- cells を R32F テクスチャ（Shared + uploadTexture）で GPU へ
- `leniaStampInit` カーネル（新設。既存 leniaInit は不変）:
  スタンプ i ∈ [0, stampCount) の位置 = rand01(i, ...) ハッシュ、
  回転/反転 = ハッシュから 8 通り。各ピクセル gid について全スタンプを
  走査し、逆変換した organism ローカル座標が [0, orgW/H) 内なら
  `A = max(A, cells[...])`（バイリニアでなく nearest でよい）。
  stampCount ≤ 32 なのでピクセルあたり 32 回のループで十分軽い。
- C++ 側: organism JSON のロード/検証（cells 行長の一致、値域 [0,1]）。
  失敗時は stderr エラー + noise init にフォールバック。

## プリセット Presets/lenia_orbium.json

1280×720、シーン解像度シム（simWidth/H 0、convMode direct — Orbium は
R=13 の direct で十分軽い）、initMode stamp、orbium、stampCount 8、
growthMode 0、injectAmount 0、noiseAmount 0。
audioMappings は控えめに:
- hihat → lenia0.noiseAmount (scale 0.15, sm 0.05) — 微弱シマー
- kick → lenia0.injectAmount (scale 0.25, sm 0.08) — キックでスープ注入
  （生物と相互作用する餌場になる）
snare→muJitter は**入れない**（Orbium は殺されやすい）。

## 完了条件（エージェント自身で確認）

1. ビルド エラー 0。後方互換: lenia_basic 120f md5 が現行と不変、
   lenia_multikernel（Phase 9 が先にマージされている場合）60f エラーなし。
2. **Orbium 生存 + 滑空の実証（本命）**: lenia_orbium を stampCount 1・
   固定 seed で 400f レンダリング。フレーム 0 / 200 / 399 の PNG を
   Read で目視し、(a) 生物の特徴的な形（オービウムの弧状の膜）が保たれて
   いる、(b) **位置が明確に移動している**（滑空）、(c) 消滅も爆発も
   していない、の 3 点を確認して報告に画像パスを記載。
   さらに variants ではフレーム 399 の meanLuma が frame 10 の
   0.3〜3.0 倍に収まること（LifePresetLab の activity 計算式を流用した
   一時スクリプトで可）。
3. stampCount 8 + stampRotate で 300f: エラーなし・md5 決定性 ×2。
4. organism JSON の cells を故意に 1 行短くした破損ファイルで
   noise フォールバック + stderr 警告が出る。
5. tools/import_lenia_organism.py を再実行すると同一の
   orbium.json が再生成される（決定的変換）。

見た目調整はレビュー側。データ移植の忠実性を最優先すること。
