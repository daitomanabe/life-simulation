#!/usr/bin/env python3
"""音ハメが「明るさ」ではなく「細胞の振る舞い」に入っているかを測る。

    python3 tests/motion_response.py Presets/vj_asharp_life_540.json \
        --music <slug>-events-v6-extreme-se.json \
        --capture captures/<slug>.jsonl

拍の瞬間に何が起きるかを、イベント同期平均で 2 つに分けて測る:

  拍:明  画面全体の平均輝度の跳ね     → 小さいほどよい（フラッシュは汚れ）
  拍:形  フレーム間の画素差の跳ね     → 大きいほどよい（線が動く）

純黒＋純白のヘアラインだけの画では、全体の明るさが上下しても汚れにしか見えない。
線が動いて初めて音に反応して見える。だから比 (形/明) を最大化する。

「音なし」の対照実験を必ず同時に走らせる。生命シミュレーションはそれ自体が
時間とともに漂うので、絶対値だけ見ると漂いを反応と誤認する。実際、初期の
マッピングは rms と輝度の相関 +0.45 を示したが、音を切っても +0.46 だった。

なぜ輝度が漏れるか: colorMap は t を 1.0 でクリップする。エージェントが散らばると
インクの総量が同じでも、飽和していた画素が飽和しなくなり、平均輝度が上がる。
したがって「密集度を変える振る舞い」(turnImpulse, sensorDistance) はフラッシュに
なりやすい。逆に attractorWeight を上げると、生まれ直した細胞がすぐ地形の尾根へ
引き戻されるので漏れが止まる。

実測（dm-Asharp-ambient-96-v1, 480x270, 40秒, 5秒目以降）:

    案                          拍:明   拍:形   形/明   flux:明
    beat → sensorBoost          0.??   -0.007    —      —      角度を一瞬いじるだけ
    beat → turnImpulse           ??    +0.005    —      —
    beat → resetPulse 0.7       0.214  +0.357   1.7x   0.091
      + tension → attractorWeight 0.016 +0.156  9.5x   0.010  ← 採用
      + resetPulse 1.0          0.107  +0.249   2.3x
      + resetPulse 1.6          0.232  +0.406   1.8x
    rms → stepsPerFrame         0.550  +0.269   0.5x          代謝は輝度に漏れる
    flux → turnImpulse 1.2      0.125  +0.177   1.4x   1.259  発火はフラッシュになる
    音なし（対照）                0.003  +0.000    —     0.002
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "LifeOfflineRender"
W, H = 480, 270


def render(scene, frames, raw_path, music=None, capture=None):
    cmd = [str(BIN), "--scene", str(scene), "--shaders", str(ROOT / "Shaders"),
           "--width", str(W), "--height", str(H), "--fps", "60",
           "--frames", str(frames), "--format", "none",
           "--output", tempfile.mkdtemp(prefix="motion_"), "--raw", str(raw_path)]
    if music:
        cmd += ["--music", str(music)]
    if capture:
        cmd += ["--audio", str(capture)]
    r = subprocess.run(cmd, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        sys.exit(f"render failed: {' '.join(cmd)}")


def load(path, n):
    return np.memmap(path, np.uint8, "r", shape=(n, H, W)).astype(np.float32)


DETREND_WIN = 120  # 2 秒


def detrend(x, win=DETREND_WIN):
    """移動平均を引く。長い漂いを除き、拍の時間スケールだけ残す。

    端の win//2 サンプルは信用しない。`mode="same"` はゼロ詰めするので、
    そこでは移動平均が実際より小さく出て、除トレンド後に巨大な偽の山が立つ。
    呼び出し側は EDGE 分のオンセットを捨てること — これを怠って、末尾の 1 拍
    だけで「拍の輝度の跳ね」が 0.016 → 0.120 に化けたことがある。
    """
    return x - np.convolve(x, np.ones(win) / win, mode="same")


EDGE = DETREND_WIN // 2  # detrend が信用できない端の幅


def eta(sig, onsets, pre=15, post=(20, 35), half=20):
    """イベント同期平均。オンセット前の平均から、直後のピークまでの上がり幅。"""
    if not onsets:
        return 0.0
    e = np.mean([sig[o - half:o + 2 * half] for o in onsets], axis=0)
    return float(e[post[0]:post[1]].max() - e[:pre].mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scene")
    ap.add_argument("--music", required=True)
    ap.add_argument("--capture", required=True)
    ap.add_argument("--frames", type=int, default=2400)
    ap.add_argument("--skip", type=int, default=300,
                    help="初期過渡を捨てるフレーム数（既定 5 秒）")
    ap.add_argument("--window", default=None, metavar="LO,HI",
                    help="解析するフレーム範囲。マルチライフのシーンでは、Slime が"
                         "見えている 1 セクションに絞らないと、無音の suspend 区間まで"
                         "含めてしまい『拍で形が動いていない』と誤検知する。"
                         "例: --window 720,1900 で Asharp の peak セクション。")
    a = ap.parse_args()

    if not BIN.exists():
        sys.exit(f"{BIN} がない。cmake --build build -j8")

    tmp = Path(tempfile.mkdtemp(prefix="motion_raw_"))
    render(a.scene, a.frames, tmp / "sound.bin", a.music, a.capture)
    render(a.scene, a.frames, tmp / "silent.bin")

    tj = json.loads(Path(a.music).read_text())["tracks"][0]
    N, S = a.frames, a.skip
    # 解析できる窓: 初期過渡 S を過ぎ、同期平均の前後 (20/40) が収まり、
    # かつ detrend の端 EDGE から離れていること。
    lo, hi = max(S, EDGE) + 20, N - 1 - EDGE - 40
    if a.window:
        wlo, whi = (int(x) for x in a.window.split(","))
        lo, hi = max(lo, wlo + EDGE), min(hi, whi - EDGE)

    def onsets(times):
        return [int(t * 60) for t in times if lo < t * 60 < hi]

    beats = onsets(tj["grid"]["beats_sec"])
    kicks = onsets(tj["grid"]["kick_onsets_sec"])
    if not beats:
        sys.exit("解析できる拍がない。--frames を増やすこと。")

    print(f"scene: {a.scene}")
    print(f"拍 {len(beats)} 発 / kick {len(kicks)} 発 を解析 "
          f"(frame {lo}..{hi} / {(hi - lo) / 60:.0f} 秒)\n")
    print(f'{"":10s} {"拍:明":>8s} {"拍:形":>8s} {"形/明":>7s} '
          f'{"kick:形":>8s} {"形の動き":>9s} {"明るさ":>7s}')

    rows = {}
    for label, f in (("音あり", "sound.bin"), ("音なし", "silent.bin")):
        img = load(tmp / f, N)
        bright = img.reshape(N, -1).mean(1)
        motion = np.abs(np.diff(img, axis=0)).reshape(N - 1, -1).mean(1)
        db, dm = detrend(bright), detrend(motion)
        eb, em = abs(eta(db, beats)), eta(dm, beats)
        rows[label] = (eb, em)
        print(f"{label:10s} {eb:8.3f} {em:8.3f} {em / max(eb, 1e-6):6.1f}x "
              f"{eta(dm, kicks):8.3f} {motion[S:].mean():9.3f} {bright[S:].mean():7.2f}")

    eb, em = rows["音あり"]
    sb, sm = rows["音なし"]
    print()

    # 「音なし」対照が成立するのは単一ライフのシーンだけ。マルチライフでは、
    # 音を切るとセクションが切り替わらず（切り替えは音の構造で駆動される）、
    # 検証したい生命がそもそも登場しない。その場合 sm==0 になるので、
    # 対照ではなく絶対値だけで「拍で形が動き、明るさに漏れていない」を見る。
    control = sm > 0.01
    ok = True
    if control:
        if em < sm + 0.02 or em < 0.05:
            print(f"  FAIL 拍で形が動いていない (音あり {em:.3f} / 音なし {sm:.3f})")
            ok = False
    elif em < 0.05:
        print(f"  FAIL 拍で形が動いていない (拍:形 {em:.3f})")
        ok = False
    else:
        print("  NOTE 音なし対照が空（マルチライフ）。絶対値で判定する。"
              " 振る舞いの厳密検証は単一ライフのプリセットで行うこと。")
    if em / max(eb, 1e-6) < 3.0:
        print(f"  FAIL 明るさへの漏れが大きい (形/明 = {em / max(eb, 1e-6):.1f}x, 3.0x 以上が必要)")
        ok = False
    if ok:
        extra = f"、無音時の {em / max(sm, 1e-6):.0f} 倍" if control else ""
        print(f"  PASS 拍は形で出ている (形/明 = {em / max(eb, 1e-6):.1f}x{extra})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
