# Phase 9 実装仕様: Lenia FFT 畳み込み + マルチカーネル + シム解像度分離

設計: Fable / 実装: Sonnet。設計指示書 §12.2 次段階（FFT convolution /
multi kernel）と §3.7 の注意（MPS に全部任せない）に対応 — **自前の
radix-2 Stockham FFT を Metal compute で実装**する（MPSGraph は使わない。
理由: CommandGraph の pass 単位 GPU timing・決定性・依存ゼロを保つ）。
全体制約は phase3 仕様の 9 項目を厳守。

## 絶対の後方互換条件

`convMode` 既定は "direct"。**既存 lenia_basic.json の 120 フレーム目の
md5 が本フェーズ適用前後で完全一致**すること（着手前に現状 md5 を記録して
おき、完了時に比較する）。direct 経路の既存カーネル leniaStep のロジックは
変更しない。

## 新規/変更ファイル

```
新規:
  Shaders/Common/FFT.metal            (Stockham FFT + complex mul)
  Presets/lenia_multikernel.json
変更:
  Modules/FieldModules/Lenia.h/.cpp   (convMode / simWidth/simHeight /
                                       kernels[] / FFT リソースとパス)
  Shaders/Field/Lenia.metal           (fft 経路用の追加カーネルのみ。
                                       既存 leniaInit/leniaStep は不変)
  LifeCore/Render/ColorMapPass.h/.cpp (サイズ差対応の encodeScaled 追加)
  Shaders/Render/ColorMap.metal       (colorMapFieldScaled カーネル追加。
                                       既存 colorMapField は不変)
```

CMakeLists / RegisterModules は変更不要（新モジュールなし）。

## 1. シム解像度の分離

- scene params に `simWidth` / `simHeight`（既定 0 = シーン解像度、
  従来どおり）。指定時は Lenia の全フィールドをそのサイズで確保。
- `convMode: "direct" | "fft"`（既定 direct）。**fft のときは
  simWidth/simHeight が 2 の冪であることを検証**（違えば setup で
  stderr エラーを出し direct にフォールバック）。
- 出力 output_ は常にシーン解像度の RGBA16F。サイズが異なる場合は
  `ColorMapPass::encodeScaled`（新設）を使う:
  `colorMapFieldScaled` は field を `access::sample` で受け、
  正規化座標の bilinear で読む（サイズ同一でも texel 中心サンプルは
  exact なので、scaled 版は常にこちらで良い — ただし既存
  colorMapField は他モジュールが使うため残す）。
- `outputField()`（結合ポート）はシム解像度の field をそのまま返してよい。
  消費側の sampleFieldWrap は正規化座標なのでサイズ非依存で正しく動く
  （検証済みの性質。コメントに明記）。

## 2. FFT (Shaders/Common/FFT.metal)

radix-2 Stockham autosort、行方向と列方向の 1D FFT を 1 stage 1 dispatch。
複素数は RG32F テクスチャ (R=re, G=im)。ping-pong。

```c
struct FFTParams {
    uint width; uint height;
    uint N;        // 変換方向の長さ (行FFTなら width)
    uint Ns;       // 現 stage の span (1, 2, 4, ... N/2)
    int  dir;      // +1 forward, -1 inverse
    uint normalize; // 1 なら 1/N を掛ける (inverse の最終 stage で)
};
kernel void fftStageX(texture2d<float,access::read> src,
                      texture2d<float,access::write> dst,
                      constant FFTParams&, uint2 gid); // gid.x in [0,W/2)
kernel void fftStageY(...);                            // gid.y in [0,H/2)
kernel void complexMulScale(texture2d<float,access::read> a,
                            texture2d<float,access::read> b,
                            texture2d<float,access::write> dst,
                            constant FFTParams&, uint2 gid);
    // dst = a * b * scale (scale は params 経由 or normalize フラグで 1/(W*H))
kernel void realToComplex(texture2d<float,access::read> real /*R32F*/,
                          texture2d<float,access::write> complex /*RG32F*/, ...);
kernel void complexToReal(...); // re 成分を取り出し
```

twiddle は shader 内 sincos で計算（LUT 不要）。実装式は任せるが、
**正しさは §6 の数値一致テストで機械検証すること**（形式的に書いた
つもりでも index/符号ミスは高確率で起きる — テストを先に組んでから
デバッグする進め方を推奨）。

C++ 側は `encodeFFT2D(graph, prefix, srcComplex, pingA, pingB, dir)` の
ようなヘルパーを Lenia.cpp 内 static でよい（汎用化は使用者が 2 つ
できてから — §22-18）。

## 3. FFT 畳み込み経路 (Lenia)

リソース (fft モード時のみ確保、全て simW×simH):
- stateComplex ping-pong ×2 (RG32F)
- kernelFFT[k] (RG32F) — カーネルごと
- potential (R32F) — 再利用可

毎フレーム:
1. realToComplex(state.read → complexA)
2. FFT2D forward (行 log2W + 列 log2H stages)
3. カーネル k ごと: complexMulScale(stateFFT, kernelFFT[k]) →
   FFT2D inverse → complexToReal → potential_k
4. `leniaGrowthMulti` カーネル（新設）: A と potential_k (最大4枚を
   texture 配列引数でなく個別 texture 0..3 でバインド、未使用は
   フォールバック黒) から
   - standard: A' = A + dt·Σ h_k·(2·bell(u_k;μ_k,σ_k)−1)
   - asymptotic: A' = A + dt·(Σ h_k·bell_k / Σ h_k − A)
   - 既存の muJitter / noiseAmount / injectAmount / blob 注入ロジックを
     leniaStep から**コピーして**適用（direct 経路と同じ振る舞い。
     共有化のために既存カーネルを書き換えない — 後方互換 md5 を守る）。
   - per-kernel パラメータは constant バッファの固定長配列
     `float kMu[4]; float kSigma[4]; float kWeight[4];` を
     LeniaParams **とは別の** `LeniaMultiParams` 構造体で渡す
     （既存 LeniaParams のレイアウトを変えない）。

kernelFFT[k] の前計算（needsInit 時、GPU で）:
1. CPU で ring カーネル画像を生成（§4）→ simW×simH の R32F に
   **中心を (0,0) に置いた wrap 配置**（象限分割配置 = fftshift 相当）で
   upload（Σ=1 正規化済み）
2. realToComplex → FFT2D forward → kernelFFT[k] へコピー

## 4. マルチカーネル定義

scene params `kernels`（省略時 = 従来の単一カーネル動作）:

```jsonc
"kernels": [
  { "radiusScale": 1.0, "mu": 0.15, "sigma": 0.017,
    "weight": 1.0, "betas": [1.0] },
  { "radiusScale": 0.55, "mu": 0.30, "sigma": 0.035,
    "weight": 0.6, "betas": [1.0, 0.6] }
]
```

- 最大 4 個。`kernels` 指定時は **convMode=fft 必須**（direct なら
  setup エラー → 先頭カーネルのみで direct 続行 + stderr 警告）。
- カーネル形状（CPU 生成、既存 buildKernelTexture を一般化）:
  R_k = radius × radiusScale。B = betas.size() として
  `u = (r/R_k)·B, i = floor(u), K(r) = betas[i]·bell(u−i, 0.5, kernelShellSigma·B)`
  （r > R_k は 0。正規化 Σ=1 は従来どおり）。
- 音マッピングは従来パラメータ（dt, injectAmount, noiseAmount,
  muJitter）がそのまま効く。per-kernel の音変調は次フェーズ。

## 5. プリセット Presets/lenia_multikernel.json

1280×720 シーン、simWidth 1024 / simHeight 512、convMode "fft"、
radius 24、growthMode 1 (asymptotic)、injectAmount 0.2、injectCount 4、
kernels = 上の 2 個 + もう 1 個
`{ "radiusScale": 0.3, "mu": 0.22, "sigma": 0.028, "weight": 0.4, "betas": [1.0] }`。
audioMappings は lenia_basic と同じ 3 本（ターゲット名だけ合わせる）。

## 6. 完了条件（エージェント自身で確認）

1. **後方互換**: 着手前に `lenia_basic.json --frames 120` の最終 md5 を
   記録 → 実装後に再実行して**完全一致**。coupled_life_basic も 60f
   エラーなし。
2. ビルド エラー 0。
3. **FFT 数値検証 A（roundtrip）**: 一時テストコード（LifeBench に
   隠しフラグを足すのではなく、検証用の一時 main / 一時シーンで可。
   終わったら削除し、検証手順と数値を報告に残す）で 256×128 のランダム
   実数場を forward→inverse し、元との最大絶対誤差 < 1e-4。
4. **FFT 数値検証 B（畳み込み一致 = 本命）**: 同一 init・同一カーネル
   (R=13) の Lenia を convMode=direct と fft で **1 step だけ**進め、
   状態を readback して比較。最大絶対誤差 < 1e-3。
   （fft の simW/H はこのテストではシーン解像度と同じ 2 の冪、
   例えば 256×256 を使う）
5. lenia_multikernel.json 300f: exit 0・エラー行 0・最終 PNG 非単色、
   md5 決定性 ×2。
6. LifeBench: (a) lenia_basic (direct, R=13, シーン解像度 720p)、
   (b) 一時シーン direct R=32 simRes なし、(c) lenia_multikernel
   (fft, R=24, K=3, 1024×512) の gpu/frame を報告 —
   (c) が (b) より速いこと（FFT の存在意義の実証）。

チューニング（multikernel の見た目）はレビュー側で行う。
