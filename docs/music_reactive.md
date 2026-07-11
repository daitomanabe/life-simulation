# 音とJSONに反応する生命 — MusicTimeline

生命シミュレーションを楽曲に反応させる経路は **2 本ある**。混ぜてはいけない。

| | 何を運ぶか | どこから来るか | 使う CLI |
|---|---|---|---|
| `AudioFeatureState` | 音の **強さ** — kick/snare/hihat/perc/beat の包絡線、fft[128]、rms/low/mid/high/centroid/flux | live OSC、または WAV から作った capture JSONL | `--audio` |
| `MusicFeatureState` | 音の **意味** — セクション、25 種のイベント、そのイベント自身の 60fps オートメーション曲線 | remix-beats の events JSON | `--music` |

両者は独立に指定できる。どちらか一方でも動く。

## なぜ 2 本必要か

生命シミュレーションにとって面白いのは、音量が上下することではなく **ルールが
切り替わること** だ。Lenia の `growthMu` を 0.15 から 0.16 へ動かすと、系は数秒
かけてまったく別の相へ落ち着く。セクションは 12〜42 秒あるので、ちょうど落ち着く
時間がある。逆に kick の一撃で `growthMu` を揺らしても、系は反応する前に元へ戻る。

だから **強さは運動へ、構造はルールへ** 割り当てる。

```json
"audioMappings": [
  { "source": "kick.trigger", "target": "slime0.depositAmount", "scale": 0.30 }
],
"musicMappings": [
  { "source": "section.energy", "target": "lenia0.growthMu",
    "scale": 0.035, "offset": 0.130, "mode": "set", "smoothing": 1.2 }
]
```

`mode: "set"` に注意。加算では相が定まらない — セクションの値そのものに *なる*
必要がある。`add`（既定）/ `set` / `mul` の合成規則は

    value = ((set があれば set、なければ base) + Σ add) × Π mul

## source 文法

```
音              kick  kick.trigger  snare.peak  rms  low  mid  high  centroid  flux
レーン          lane.tension  lane.wet_ratio  lane.master_rms  lane.se_env
                lane.kick_gate  lane.spatial_active  lane.intensity
セクション      section.energy  section.progress  section.changed  section.is:coda
イベント        event:granular_freeze.active      走っている間 1.0
                event:reverse_swell.started       開始フレームのみ 1.0
                event:warp_filter.cutoff_hz       そのイベント自身の 60fps 曲線
```

イベントの `curves_60fps` は **すでにシミュレーションと同じ 60fps で刻まれている**。
補間も平滑化も要らず、そのままルールへ入る。これが `--music` を JSON 直読みに
した理由 — 別形式へ複製すると、この一致が壊れる。

曲線は物理単位（Hz, dB）で来るので `inMin` / `inMax` で 0..1 へ正規化してから
`scale` を掛ける:

```json
{ "source": "event:warp_filter.cutoff_hz", "target": "slime0.sensorDistance",
  "inMin": 200.0, "inMax": 8000.0, "scale": 9.0, "offset": 6.0, "mode": "set" }
```

## 予約パラメータ — freeze と reseed

`SimulationModule` には何も足していない。どちらも「モジュールを呼ぶか呼ばないか」
の話なので、`SceneRunner` が解釈する。

| キー | 意味 |
|---|---|
| `<module>.freeze` | `> 0.5` の間 `encode()` を飛ばす。場は最後の状態のまま合成される |
| `<module>.reseed` | `0.5` を跨いだ立ち上がりで `reset()`。種は scene.seed とフレーム番号から決まるので再現性がある |

```json
{ "source": "event:granular_freeze.active", "target": "lenia0.freeze", "scale": 1.0 },
{ "source": "event:reverse_swell.started",  "target": "lenia0.reseed", "scale": 1.0 }
```

`tests/music_timeline_check.sh` が、凍結窓の内側で場が完全に静止し、窓の外では
毎フレーム動き、境界がイベント定義と 1 フレームの狂いもなく一致することを md5 で
検証する。目視には頼らない。

## 使い方

```bash
# 1. 楽曲アセット → capture JSONL（音の強さ）
python3 tools/music_to_capture.py \
    --events <slug>-events-v6-extreme-se.json \
    --wav    <slug>-remix-v6-extreme-se.wav \
    --stems  stems/ \
    --out    captures/<slug>.jsonl

# 2. レンダー（音 + 構造）
./build/LifeOfflineRender --scene Presets/vj_asharp_life.json --shaders Shaders \
    --fps 60 --frames 9489 --format none \
    --audio captures/<slug>.jsonl \
    --music <slug>-events-v6-extreme-se.json \
    --movie renders/<slug>-life.mov --codec prores422
```

包絡線 (raw/smoothed/peak/trigger/hold) は `music_to_capture.py` が
`LifeCore/Audio/FeatureSmoother.cpp` と同じ式・同じ既定係数で計算する。だから
オフライン replay とライブ OSC 入力は同じ数値を通り、`kick.trigger` は両者で
一致する。

## 曲の展開に応じて生命を切り替える

音ハメが「1 つの生命を音に反応させる」なら、これは「セクションごとに *どの生命を
見せるか* を変える」層。remix 映像側で Composition が phrase ごとにシーンを差し替える
のと同じ考え方。`Presets/vj_asharp_life.json` が実例（CA → Slime → Lenia →
ParticleLife → Slime → CA → Slime+PL の 7 セクション）。

仕組みは 1 つだけ: **`opacity` を SceneRunner が毎フレーム ParameterBus から引く。**
だから `musicMappings` で切り替えられる。

```json
{ "source": "section.n:0", "target": "ca0.opacity",    "smoothing": 0.35 },
{ "source": "section.n:1", "target": "slime0.opacity", "smoothing": 0.35 },
{ "source": "section.n:2", "target": "lenia1.opacity", "smoothing": 0.35 }
```

`section.n:<k>` は k 番目のセクションの間だけ 1.0。同じ role が続く曲（Asharp は
suspend が 3 連続）で別々の生命を出すため。role で足りるなら `section.is:<role>`。

**見えない層は encode ごと止まる。** 5 層を常時回すと GPU 75ms/frame だが、一度に
1〜2 層しか見せないなら 11ms 台。生命なので「止めて再開」は嘘になるので、opacity が
`VISIBLE_EPS` (0.02) を跨いで **現れた瞬間に reset()** して生まれ直させる。隠れて
いる間の時間経過は演じない。実装は SceneRunner::step の encode ループ。

切り替え先が「生まれたて」で暗転しないよう注意する。orbium のような疎な生物は
1080p に数個撒いても点にしかならず、前の層が消えると画面が黒くなる。無音の
suspend 区間には Lenia basic（初期カバレッジ 0.30 から面を埋める）を割り当てた。
境界の暗転は `raw` を吐いて輝度を測って確認する（目視では 0.35 秒の谷を見逃す）。

**この割り当ては曲のセクション配置に固有。** 他の曲では `section.n:` の番号が別の
役割を指す。曲ごとに割り当てを作るか、`section.is:<role>` で書き直すこと。

## 全曲で使える role ベースの割り当て（`Presets/vj_life.json`）

remix-beats の全 17 曲は同じ 6 role（establish / build / peak / suspend / hinge /
coda）で構成される。だから `section.n:` の代わりに `section.is:<role>` を使えば、
**プリセット 1 個で全曲動く**。曲ごとに変えるのは music / audio / frames だけ。

| role | 生命 | |
|---|---|---|
| establish | CellularAutomata | 曲頭の立ち上がり。まばらな火花 |
| build | ParticleLife | 盛り上がり。粒子が集まる |
| peak | SlimeMold | 山。主役の網 |
| suspend | Lenia basic | 谷・無音。面を埋める指紋状 |
| hinge | CellularAutomata | つなぎ。斑点への回帰 |
| coda | Slime + ParticleLife | 終幕。2 つが重なる |

同じ role が連続しても `section.energy` が growthMu を変えるので、相は変わる。

```bash
bash scripts/render_life.sh <slug>            # 1080p、音声付き .mov を tracks/<slug>/ に
bash scripts/render_life.sh <slug> 960 540    # プレビュー解像度
```

`vj_asharp_life.json`（`section.n:` で 3 連続 suspend を別々の生命にする版）は、
特定の曲を作り込むときの雛形として残してある。

## 落とし穴

**`colorMap` の `d` は色ではなくコサインパレットの位相。** `d=[1,1,1]` は
t=0 で cos(2π)=1 → **背景が真っ白**になる。既定値 `d=[0,0.1,0.2]` も色付き。
VJ 出力の背景は #000 と決まっているので `"palette": "mono"` を使う
（`a=b=c=d=0.5`、黒→白の単調ランプ）。

**`macro_intensity` / `macro_density` は全曲・全フレームで定数 1.0。**
発火条件に使ってはいけない。`lane.tension` か `section.energy` を使う。

**SlimeMold の `moveSpeed` / `sensorDistance` / `diffuseRate` はピクセル単位。**
解像度を変えると構造の粗さと線の太さが比例して変わる。1080p 用のプリセットを
960×540 で流用してはいけない（`Presets/vj_asharp_life_540.json` を参照）。

**Lenia を 1080p で直に回すと構造が細かすぎて画面が「常時ディザ」になる。**
`simWidth`/`simHeight` を落として拡大サンプルすれば、生命体は画面いっぱいの
大きさになり、GPU も 10ms → 1ms 台に落ちる。

**`--movie` の書き出しは GPU より重い。** Slime 1080p は GPU 1.25 ms/frame
だが、readback + H264 で壁時計 17.4 ms/frame になる。速度を語るときはここを見る。
1080p マルチライフの全曲書き出しは実測 10 分（GPU は 5 分だが movie readback が倍）。

**ParticleLife は種ごとに色を出す。** `#000/#fff` の画では色は事故に見える。
`"monoColor": true` で種を白の濃淡（明度差のみ）に落とす。既定は色付きのまま
（opt-in）。他のモジュールは colorMap の `"palette": "mono"` で同じことをする。

**音ハメは明るさではなく振る舞いへ。** colorMap は t を 1.0 でクリップするので、
エージェントが散らばると同じインク量でも平均輝度が上がる。deposit/decay/diffuse/
splatGain を音で動かすとフラッシュになる。動かすのは resetPulse / sensorAngle /
moveSpeed / growthMu など「形」を決めるパラメータだけ。検証は
`tests/motion_response.py`（拍の瞬間の「明るさの跳ね」と「形の動き」を分けて測る）。
マルチライフのシーンは音を切るとセクションが進まないので、振る舞いの厳密検証は
単一ライフの `tests/scenes/slime_only_540.json` で行う。

## VJ 合成器へのブリッジ（仮設）

`--raw <path|->` は最終フレームを 8bit 輝度の生バイト列として吐く（ヘッダなし、
`width*height` バイト × frames）。visualize-lyria の `vj/lifelayer.py` がこれを
パイプで受け、VJ のシーンレイヤーとして MAX 合成する。

Metal 移行が進み VJ の描画自体が LifeCore のモジュールになれば、この配管は消える。
