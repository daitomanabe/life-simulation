# 引き継ぎ — 音楽反応マルチライフ (2026-07-11)

Daito Manabe の楽曲 17 曲すべてを、生命シミュレーションの映像として書き出すまでを
このセッションで作った。新しい担当者は **まずこのファイルと
[docs/music_reactive.md](docs/music_reactive.md) を読めば再開できる。**

---

## 0. いま何が終わっているか

**全 17 曲の life 映像が完成している。**

```
<remix-beats>/tracks/<slug>/<slug>-life-v6.mp4
  1920×1080 / 60fps / 音声付き / 完全モノクロ / 計 78GB
  <remix-beats> = /Users/daitomacm5/development/sandbox/generative-sequencer/remix-beats
```

QC 済み（全曲・全セクション）: 彩度ゼロ・暗転なし・音声トラックあり。

両リポジトリともコミット済み・作業ツリーはクリーン:
- **life-simulation**: `0763d69` `0cda1d5` `7a8bffd`（コア / モノクロ / プリセット・テスト・docs）
- **visualize-lyria-music-fff**: `ab181f6`（VJ レイヤーへのブリッジ）

映像 (`*.mp4`)・capture (`*.jsonl`)・`renders/` はどちらも gitignore 済みで、リポジトリには
入っていない。

## 1. 何ができるようになったか

音楽が生命シミュレーションを **3 階層** で駆動する。すべて events JSON の直読みで決定的、
ライブ OSC とオフライン再生が同じ数値を通る。

| 階層 | 入力（JSON/WAV のどこ） | → 生命の何 |
|---|---|---|
| 音の**強さ** | capture.jsonl（kick/rms/hihat/centroid…） | 細胞の振る舞い（新生・震え・分岐角・速度） |
| 音の**構造** | events JSON の sections / events / 60fps 曲線 | 生命体のルール（相・時計・凍結・再生成） |
| 曲の**展開** | events JSON の section role | どの生命を見せるか（CA→Slime→Lenia→PL…） |

**強さと構造を分けたのが設計の核。** 生命にとって面白いのは音量の上下ではなく、
ルールが切り替わって系が別の相へ落ち着くこと。だから強さは運動へ、構造はルールへ割り当てた。

## 2. 全曲を作り直す手順

```bash
cd /Users/daitomacm5/development/sandbox/life-simulation
cmake --build build -j8                 # 必要なら

# 1. capture（音の強さ）を全曲分。既にあれば skip される。
TR=/Users/daitomacm5/development/sandbox/generative-sequencer/remix-beats/tracks
V=/Users/daitomacm5/development/sandbox/visualize-lyria-music-fff/.venv/bin/python
for s in $(ls $TR | grep '^dm-'); do
  [ -f "captures/$s.jsonl" ] && continue
  $V tools/music_to_capture.py --events "$TR/$s/$s-events-v6-extreme-se.json" \
     --wav "$TR/$s/$s-remix-v6-extreme-se.wav" --stems "$TR/$s/stems" \
     --out "captures/$s.jsonl"
done

# 2. 全曲の life 映像。既にある曲は skip（再開可能）。実測 124 分。
bash scripts/render_all_life.sh              # 1080p
bash scripts/render_all_life.sh 960 540      # プレビュー解像度
```

1 曲だけなら `bash scripts/render_life.sh <slug>`。出力は tracks/<slug>/ の隣、
`.tmp` 経由で mv するので途中で止めても完成済みは無傷。

## 3. どこで何を変えるか

| やりたいこと | 触る場所 |
|---|---|
| role → どの生命 | `Presets/vj_life.json` の musicMappings（`section.is:<role>` → `<mod>.opacity`） |
| 音 → 細胞の振る舞い | 同 audioMappings（resetPulse/sensorAngle/moveSpeed…） |
| 生命体のルール | 同 musicMappings（section.energy→growthMu 等） |
| 1 曲を作り込む | `Presets/vj_asharp_life.json`（`section.n:` で連続 role を別生命に）を雛形に |
| 新しい source 文法 | `LifeCore/Params/ParameterBus.cpp` の `featureValue(MusicFeatureState)` |
| freeze/reseed の挙動 | `LifeCore/Sim/SceneRunner.cpp` の encode ループ |
| 落とし穴の一覧 | `docs/music_reactive.md` |

**role 割り当て（全 17 曲共通、1 プリセット）:**
establish→CA / build→ParticleLife / peak→Slime / suspend→Lenia / hinge→CA / coda→Slime+PL。
17 曲すべてが同じ 6 role なので `section.is:` で全曲動く。曲固有にするなら `section.n:`。

## 4. 検証（目視に頼らない）

```bash
bash tests/music_timeline_check.sh          # freeze 窓が byte 単位で凍る / 境界がイベント通り / reseed は不連続
SKIP_BUILD=1 bash scripts/verify.sh         # 全プリセット 60 フレーム + 決定性 md5
python3 tests/motion_response.py tests/scenes/slime_only_540.json \
    --music <events.json> --capture captures/<slug>.jsonl
                                            # 拍の瞬間、明るさより形が動くか（形/明 3.8x で PASS）
```

`--dump-params <key,key,…>` を LifeOfflineRender に渡すと、パラメータの実値を毎フレーム
`<output>/params.csv` に吐く。「本当に反応しているか」を測る唯一の誠実な方法。

## 5. このセッションで潰したバグ（再発させないため）

1. **イベント終了後のパラメータ凍結** — `warp_filter` は 158s 中最初の 12s だけ存在。
   `mode:set` のマッピングが、イベント終了で `featureValue`→0 を返すと、target を 0 の値に
   永久固定していた。`sourceValue` で「値の不在」と「値 0」を区別して解決。
2. **音を明るさに割り当てていた** — colorMap は t を 1.0 でクリップするので、粒子が散らばると
   同じインク量でも平均輝度が上がる。deposit/decay を音で振るとフラッシュになる。動かすのは
   「形」を決めるパラメータだけ。`attractorWeight` を上げると散らばりが抑えられ漏れが止まる。
3. **測定ツールの端バグ** — `np.convolve(mode='same')` が端をゼロ詰めし、末尾 60 フレームの
   除トレンドが壊れて偽の山が立つ。`EDGE = win//2` 分のオンセットを捨てる。
4. **ParticleLife の色** — 種ごとにコサインパレットの色。`"monoColor":true` で明度差のみに。
5. **peak→suspend の暗転** — orbium は疎で 1080p に数個撒くと点。無音 suspend は Lenia basic に。

## 6. 保留 —「Remix VJ に使うか」（このセッションの元の問い）

映像は揃った。判断が要る:

- **単体で使う** vs **Remix に重ねる**（`visualize-lyria/vj/lifelayer.py` のブリッジ経由、
  `python -m vj.remix … --life-scene … --life-capture …`）。後者は仮設配管で、Metal 移行後に消える。
- **曲どうしの peak が似て見える** — role 固定の帰結（どの曲の peak も Slime）。曲ごとに
  個性を出すなら、曲名ハッシュで role→生命 を少しずらす（VJ の BASE_RULES と同じ手）。

## 7. 事務的な残り

- `renders/` に書き出しの中間物が **13GB**、`captures/` に 199MB。どちらも gitignore 済み、
  最終成果ではない。ディスクを空けたければ `renders/life_*`（各曲の作業ディレクトリ、
  movie は既に tracks/ へ mv 済み）と `renders/asharp_life`（旧 section.n: 版 4.3GB）を消せる。
- **速度の事実**: ParticleLife/Slime は解像度でなく粒子数律速。解像度を落としても速くならない。
  1080p 全曲 124 分。`--movie` の readback+H264 が GPU の倍のコスト。

## 8. 次の大きな一歩（visualize-lyria 側の HANDOVER.md §0 と連動）

母体は life-simulation に移す方針。VJ の描画（Python）を LifeCore のモジュールへ移植する。
最初の壁は **Metal に Geometry Shader が無い**こと — `vj/gl.py` の `_GS_LINES`/`_GS_TRI_WIRE`
は per-instance の頂点シェーダで四角形を展開する形に置き換える（出力は同じ、GS より速い）。
Asharp は現行 Python エンジンで最終形が書き出し済みで、pixel parity の基準になる。
