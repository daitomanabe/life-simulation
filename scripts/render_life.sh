#!/bin/bash
# scripts/render_life.sh — 1 曲を role ベースのマルチライフ映像として書き出す。
#   bash scripts/render_life.sh <slug> [width height]
# 既定 1920x1080。音声トラック付き .mov を tracks/<slug>/ の隣に出す。
#
# .tmp を付けて書き、成功時だけ mv する（途中で落ちても既存映像は失われない）。

set -u
cd "$(dirname "$0")/.." || exit 1

SLUG="$1"
W="${2:-1920}"
H="${3:-1080}"
TR="/Users/daitomacm5/development/sandbox/generative-sequencer/remix-beats/tracks"
V="/Users/daitomacm5/development/sandbox/visualize-lyria-music-fff/.venv/bin/python"
EV="$TR/$SLUG/$SLUG-events-v6-extreme-se.json"
WAV="$TR/$SLUG/$SLUG-remix-v6-extreme-se.wav"
CAP="captures/$SLUG.jsonl"
OUT="$TR/$SLUG/$SLUG-life-v6.mp4"

for f in "$EV" "$WAV" "$CAP"; do
    [ -e "$f" ] || { echo "[render_life] missing: $f"; exit 1; }
done

NF=$("$V" -c "import json;print(json.load(open('$EV'))['tracks'][0]['n_frames'])")
mkdir -p "renders/life_$SLUG"
SILENT="renders/life_$SLUG/_silent.mov"

echo "[render_life] $SLUG  ${W}x${H}  $NF frames"
./build/LifeOfflineRender --scene Presets/vj_life.json --shaders Shaders \
    --width "$W" --height "$H" --fps 60 --frames "$NF" --format none \
    --audio "$CAP" --music "$EV" \
    --output "renders/life_$SLUG" \
    --movie "$SILENT" --codec h264 --quality 0.85 || exit 1

# 音声を多重化（remix WAV をそのまま載せる）
ffmpeg -v error -i "$SILENT" -i "$WAV" -c:v copy -c:a aac -b:a 256k -shortest \
    -y "$OUT.tmp.mp4" || exit 1
mv -f "$OUT.tmp.mp4" "$OUT"
rm -f "$SILENT"
echo "[render_life] wrote $OUT"
