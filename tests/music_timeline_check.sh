#!/bin/bash
# tests/music_timeline_check.sh
# --music (MusicTimeline) が本当にシミュレーションの *ルール* を動かしているか
# を、フレームの md5 だけで検証する。目視に頼らない。
#
#   bash tests/music_timeline_check.sh
#
# 検証項目（dm-Asharp-ambient-96-v1 のイベント配置に依存）:
#   1. granular_freeze (frame 577-652) の間、Lenia の場が完全に静止する
#   2. その窓の外では毎フレーム変化している（1 が「ただ止まっている」だけでない）
#   3. reverse_swell の開始フレーム (1777) で場が作り直される
#   4. --music 付きで 2 回回して同一出力（決定性）
#
# `set -e` は使わない。md5/grep は不一致で非零を返し、途中で死ぬため。

cd "$(dirname "$0")/.." || exit 1

TRACKS="${TRACKS:-/Users/daitomacm5/development/sandbox/generative-sequencer/remix-beats/tracks}"
SLUG="dm-Asharp-ambient-96-v1"
MUSIC="$TRACKS/$SLUG/$SLUG-events-v6-extreme-se.json"
SCENE="tests/scenes/freeze_probe.json"
BIN="./build/LifeOfflineRender"

FAIL=0
PASS() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
FAILED() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }

for f in "$BIN" "$MUSIC" "$SCENE"; do
    [ -e "$f" ] || { echo "missing: $f"; exit 1; }
done

TMP="$(mktemp -d /tmp/lifesim_music.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# reverse_swell の 1 本目 (1777) まで届く長さ。
FRAMES=1790

render() {
    "$BIN" --scene "$SCENE" --shaders Shaders --fps 60 --frames "$FRAMES" \
        --music "$MUSIC" --output "$1" --format png > "$TMP/log_$2.txt" 2>&1
}

echo "== render (1/2) =="
mkdir -p "$TMP/a"
render "$TMP/a" a || { echo "render failed"; cat "$TMP/log_a.txt"; exit 1; }

md5_of() { md5 -q "$TMP/a/frame_$(printf '%06d' "$1").png"; }

# ---- 1. 凍結窓の内側は全フレーム同一 ---------------------------------------
# frame 577 が最初の freeze フレーム。578..652 は 577 と同じでなければならない。
# (577 自体は「その前のフレームの状態」を保持したもの。)
echo "== 1. granular_freeze 577-652: field must be frozen =="
REF="$(md5_of 600)"
DIFFER=0
for f in 578 590 610 630 651; do
    [ "$(md5_of $f)" = "$REF" ] || DIFFER=$((DIFFER + 1))
done
if [ "$DIFFER" -eq 0 ]; then PASS "frames 578..651 are byte-identical"
else FAILED "$DIFFER of 5 sampled frames changed inside the freeze window"; fi

# ---- 2. 窓の外は毎フレーム動いている ---------------------------------------
echo "== 2. outside the window the field must evolve =="
SAME=0
for pair in "300 301" "400 401" "700 701" "900 901"; do
    set -- $pair
    [ "$(md5_of $1)" = "$(md5_of $2)" ] && SAME=$((SAME + 1))
done
if [ "$SAME" -eq 0 ]; then PASS "4 sampled consecutive pairs all differ"
else FAILED "$SAME of 4 consecutive pairs outside the window were identical"; fi

# ---- 3. 凍結の出入りが実際に境界で起きている -------------------------------
echo "== 3. the freeze boundary is exactly where the event says =="
# 576 -> 577 は動く（576 はまだ通常フレーム、577 で凍結が始まり 576 の絵を保つ…
# ではなく、577 は encode を飛ばすので 576 の絵のまま）。よって 576 == 577。
# 652 は frameEnd（排他）なので通常フレームに戻り、651 とは違うはず。
B1=$([ "$(md5_of 576)" = "$(md5_of 577)" ] && echo ok || echo no)
B2=$([ "$(md5_of 651)" != "$(md5_of 652)" ] && echo ok || echo no)
if [ "$B1" = ok ] && [ "$B2" = ok ]; then PASS "enters at 577, leaves at 652"
else FAILED "boundary wrong (enter=$B1 leave=$B2)"; fi

# ---- 4. reverse_swell.started で作り直される -------------------------------
echo "== 4. reverse_swell @1777 must reseed the field =="
# reseed は frame 1777 の encode 前に走るので、1777 は生まれたての場になる。
# 直前 1776 とは大きく違うはず。単に「違う」だけでは弱いので、通常の 1 フレーム
# 差分より大きいことを確認する。
if command -v python3 > /dev/null 2>&1; then
    python3 - "$TMP/a" <<'PY'
import sys, pathlib, struct, zlib
d = pathlib.Path(sys.argv[1])
try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("  SKIP  (PIL/numpy not available)"); sys.exit(0)

def g(i):
    return np.asarray(Image.open(d / f"frame_{i:06d}.png").convert("L"), np.float32)

normal = np.abs(g(1700) - g(1701)).mean()
at_seed = np.abs(g(1776) - g(1777)).mean()
print(f"  normal 1-frame delta = {normal:.3f}, reseed delta = {at_seed:.3f}")
if at_seed > normal * 5 and at_seed > 1.0:
    print("  \033[32mPASS\033[0m reseed produces a discontinuity")
else:
    print("  \033[31mFAIL\033[0m no reseed discontinuity at frame 1777")
    sys.exit(1)
PY
    [ $? -eq 0 ] || FAIL=$((FAIL + 1))
else
    echo "  SKIP (no python3)"
fi

# ---- 5. 決定性 --------------------------------------------------------------
echo "== 5. determinism: same inputs -> same final frame =="
mkdir -p "$TMP/b"
render "$TMP/b" b || { echo "second render failed"; exit 1; }
LAST=$(printf '%06d' $((FRAMES - 1)))
A="$(md5 -q "$TMP/a/frame_$LAST.png")"
B="$(md5 -q "$TMP/b/frame_$LAST.png")"
if [ "$A" = "$B" ]; then PASS "final frame md5 matches ($A)"
else FAILED "nondeterministic: $A vs $B"; fi

echo
if [ "$FAIL" -eq 0 ]; then printf '\033[32mall music-timeline checks passed\033[0m\n'
else printf '\033[31m%d check(s) failed\033[0m\n' "$FAIL"; fi
exit "$FAIL"
