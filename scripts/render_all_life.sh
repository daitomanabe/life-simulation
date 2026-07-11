#!/bin/bash
# scripts/render_all_life.sh — 全 17 曲を role ベースのマルチライフ映像として
# 書き出す。既に <slug>-life-v6.mp4 がある曲はスキップ（再開可能）。
#   bash scripts/render_all_life.sh [width height]
#
# 1 曲ずつ直列。ParticleLife/Slime は粒子数律速なので並列にしても GPU は 1 つ、
# 速くならない。各曲は render_life.sh が .tmp 経由で書くので、途中で止めても
# 完成済みの曲は無傷。

set -u
cd "$(dirname "$0")/.." || exit 1
W="${1:-1920}"
H="${2:-1080}"
TR="/Users/daitomacm5/development/sandbox/generative-sequencer/remix-beats/tracks"

SLUGS=$(ls "$TR" | grep '^dm-' | while read s; do
    [ -f "$TR/$s/$s-events-v6-extreme-se.json" ] && echo "$s"; done)

TOTAL=$(echo "$SLUGS" | wc -l | tr -d ' ')
i=0
START=$(date +%s)
for s in $SLUGS; do
    i=$((i + 1))
    OUT="$TR/$s/$s-life-v6.mp4"
    if [ -f "$OUT" ]; then
        echo "[$i/$TOTAL] skip $s (exists)"
        continue
    fi
    echo "[$i/$TOTAL] === $s ==="
    bash scripts/render_life.sh "$s" "$W" "$H" || { echo "FAILED: $s"; continue; }
done
ELAPSED=$(( $(date +%s) - START ))
echo "=== all life renders done in $((ELAPSED / 60))m ==="
ls -la "$TR"/dm-*/dm-*-life-v6.mp4 2>/dev/null | wc -l
