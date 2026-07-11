#!/usr/bin/env python3
"""remix-beats の楽曲アセットを LifeCore の AudioFeatureState capture (JSONL) に変換する。

LifeRealtime --record が書くのと同じ 1 行 1 フレームの形式を出力するので、
LifeOfflineRender --audio <out.jsonl> でそのまま再生できる。C++ 側の変更は不要。

    python3 tools/music_to_capture.py \
        --events  <slug>-events-v6-extreme-se.json \
        --wav     <slug>-remix-v6-extreme-se.wav \
        --stems   stems/ \
        --out     <slug>-capture-60fps.jsonl

包絡線 (raw/smoothed/peak/trigger/hold) は LifeCore/Audio/FeatureSmoother.cpp と
同じ式・同じ既定係数で計算する。したがってライブ OSC 入力とオフライン replay は
同じ数値を通り、`kick.trigger` などのマッピングは両者で一致する。

FFT は 128 本の mel バンド (20Hz–16kHz)。FeatureSmoother が low/mid/high を
bin 0..15 / 16..63 / 64..127 で切るため、mel スケールだとその境界が
おおよそ 253Hz / 1520Hz になり、音楽的な帯域分割と一致する。
"""
import argparse
import json
import math
from pathlib import Path

import numpy as np

FPS = 60.0
N_FFT_BINS = 128
FMIN, FMAX = 20.0, 16000.0
SR = 48000

# FeatureSmoother.cpp の SmootherConfig 既定値。
ATTACK_RATE = 40.0
RELEASE_RATE = 6.0
PEAK_DECAY_RATE = 1.5
TRIGGER_DECAY_RATE = 8.0
TRIGGER_THRESHOLD = 0.35
HOLD_THRESHOLD = 0.2
# idleDecay は「OSC が来ない」ときの watchdog。オフラインでは毎フレーム値が
# 来るので発火しない。ゆえに再現不要。


def envelopes(raw, dt):
    """FeatureSmoother::update() を raw[] の全フレームに対して回す。

    戻り値は (n, 5) の float32 で、列は CaptureWriter の順序
    [raw, smoothed, peak, trigger, hold] に一致する。
    """
    n = len(raw)
    out = np.zeros((n, 5), np.float32)
    smoothed = peak = trigger = 0.0
    was_above = False
    k_peak = math.exp(-PEAK_DECAY_RATE * dt)
    k_trig = math.exp(-TRIGGER_DECAY_RATE * dt)
    k_att = 1.0 - math.exp(-ATTACK_RATE * dt)
    k_rel = 1.0 - math.exp(-RELEASE_RATE * dt)
    for i in range(n):
        r = float(raw[i])
        smoothed += (r - smoothed) * (k_att if r > smoothed else k_rel)
        peak = max(r, peak * k_peak)
        above = r >= TRIGGER_THRESHOLD
        trigger = 1.0 if (above and not was_above) else trigger * k_trig
        was_above = above
        out[i] = (r, smoothed, peak, trigger, 1.0 if r >= HOLD_THRESHOLD else 0.0)
    return out


def mel_fft(y, n_frames):
    """60fps の 128 本 mel スペクトル。各フレーム 0..1 に正規化。"""
    import librosa

    hop = SR // int(FPS)  # 800 samples @ 48k = 正確に 1/60 s
    S = librosa.feature.melspectrogram(
        y=y, sr=SR, n_fft=2048, hop_length=hop, n_mels=N_FFT_BINS,
        fmin=FMIN, fmax=FMAX, power=2.0)
    db = librosa.power_to_db(S, ref=np.max, top_db=80.0)
    v = ((db + 80.0) / 80.0).astype(np.float32)  # 0..1
    v = np.clip(v, 0.0, 1.0)
    if v.shape[1] < n_frames:
        v = np.pad(v, ((0, 0), (0, n_frames - v.shape[1])), mode="edge")
    return v[:, :n_frames].T  # (n_frames, 128)


def derived(fft):
    """FeatureSmoother の rms/low/mid/high/centroid/flux を同じ式で。"""
    n = len(fft)
    idx = np.arange(N_FFT_BINS, dtype=np.float32)
    rms = np.sqrt((fft ** 2).sum(1) / N_FFT_BINS)
    low = fft[:, :16].sum(1) / 16.0
    mid = fft[:, 16:64].sum(1) / 48.0
    high = fft[:, 64:].sum(1) / 64.0
    total = fft.sum(1)
    centroid = np.where(total > 1e-6, (fft @ idx) / np.maximum(total, 1e-6)
                        / (N_FFT_BINS - 1), 0.0)
    prev = np.vstack([fft[:1], fft[:-1]])
    flux = np.maximum(fft - prev, 0.0).sum(1) / N_FFT_BINS
    return [np.asarray(a, np.float32) for a in
            (rms, low, mid, high, centroid, flux)]


def band_env(y, lo, hi, n_frames):
    """[lo,hi] Hz の 60fps RMS 包絡線を 97 パーセンタイルで正規化。"""
    import librosa
    from scipy.signal import butter, sosfilt

    sos = butter(4, [lo / (SR / 2), min(hi, SR / 2 - 1) / (SR / 2)],
                 btype="band", output="sos")
    b = sosfilt(sos, y)
    hop = SR // int(FPS)
    e = librosa.feature.rms(y=b.astype(np.float32), frame_length=2 * hop,
                            hop_length=hop, center=True)[0]
    if len(e) < n_frames:
        e = np.pad(e, (0, n_frames - len(e)), mode="edge")
    e = e[:n_frames]
    ref = float(np.percentile(e, 97)) + 1e-9
    return np.clip(e / ref, 0.0, 1.0).astype(np.float32)


def impulses(times_sec, n_frames, decay_frames=6.0):
    """オンセット時刻列 → 立ち上がり 1.0 の指数減衰インパルス列。

    `trigger` の立ち上がり検出のため、オンセットの当該フレームだけ 1.0 に
    する（減衰は次フレームから）。これで raw が閾値 0.35 を跨ぐ。
    """
    out = np.zeros(n_frames, np.float32)
    k = math.exp(-1.0 / decay_frames)
    for t in times_sec:
        f = int(round(float(t) * FPS))
        if 0 <= f < n_frames:
            out[f] = 1.0
    # 減衰を前方に伝播（新しいオンセットが来たら 1.0 で上書き）
    for i in range(1, n_frames):
        out[i] = max(out[i], out[i - 1] * k)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", required=True, help="*-events-v6-*.json")
    ap.add_argument("--wav", required=True, help="remix WAV (48k)")
    ap.add_argument("--stems", default=None, help="stems/ ディレクトリ")
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=0, help="0 = 曲全体")
    a = ap.parse_args()

    import librosa

    ev = json.loads(Path(a.events).read_text())
    tj = ev["tracks"][0]
    n_frames = int(tj["n_frames"])
    if a.frames:
        n_frames = min(n_frames, a.frames)
    dt = 1.0 / FPS

    y, _ = librosa.load(a.wav, sr=SR, mono=True)
    need = int(n_frames * SR / FPS) + 2048
    if len(y) < need:
        y = np.pad(y, (0, need - len(y)))

    fft = mel_fft(y, n_frames)
    rms, low, mid, high, centroid, flux = derived(fft)

    # --- チャンネル raw 値 ---
    grid = tj["grid"]
    beat_raw = impulses(grid["beats_sec"], n_frames, decay_frames=5.0)

    stems = Path(a.stems) if a.stems else None

    def stem(name):
        if not stems:
            return None
        for ext in (".flac", ".wav"):
            p = stems / (name + ext)
            if p.exists():
                s, _ = librosa.load(str(p), sr=SR, mono=True)
                if len(s) < need:
                    s = np.pad(s, (0, need - len(s)))
                return s
        return None

    drums = stem("drums")
    if drums is not None:
        kick_raw = band_env(drums, 20, 120, n_frames)
        snare_raw = np.maximum(band_env(drums, 180, 450, n_frames),
                               band_env(drums, 2000, 6000, n_frames) * 0.8)
        hihat_raw = band_env(drums, 7000, 16000, n_frames)
    else:
        kick_raw = band_env(y, 20, 120, n_frames)
        snare_raw = band_env(y, 180, 450, n_frames)
        hihat_raw = band_env(y, 7000, 16000, n_frames)

    # kick はアンビエント曲だと帯域包絡線が鈍い。events JSON が持つ
    # kick_onsets を重ねて、trigger が確実に立つようにする。
    kick_raw = np.maximum(kick_raw,
                          impulses(grid["kick_onsets_sec"], n_frames, 4.0))

    other = stem("other")
    perc_raw = (band_env(other, 300, 4000, n_frames) if other is not None
                else band_env(y, 300, 4000, n_frames))

    chans = {
        "kick": envelopes(kick_raw, dt),
        "snare": envelopes(snare_raw, dt),
        "hihat": envelopes(hihat_raw, dt),
        "perc": envelopes(perc_raw, dt),
        "beat": envelopes(beat_raw, dt),
    }

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        for i in range(n_frames):
            rec = {
                "f": i,
                "t": round(i * dt, 6),
                "dt": round(dt, 8),
                "drv": [round(float(x[i]), 5) for x in
                        (rms, low, mid, high, centroid, flux)],
                "fft": [round(float(v), 4) for v in fft[i]],
            }
            for name, e in chans.items():
                rec[name] = [round(float(v), 5) for v in e[i]]
            f.write(json.dumps(rec, separators=(",", ":")) + "\n")

    print(f"{out}  frames={n_frames}  dur={n_frames / FPS:.2f}s")
    for name, e in chans.items():
        raw = e[:, 0]
        trig = int((e[:, 3] > 0.99).sum())
        print(f"  {name:6s} raw mean={raw.mean():.3f} max={raw.max():.3f} "
              f"triggers={trig}")


if __name__ == "__main__":
    main()
