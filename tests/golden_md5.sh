#!/bin/bash
# 決定性の回帰確認: 代表シーンの 40 フレーム目の MD5 を出す。シェーダ / モジュールを触る前後で実行して比べる。
# (値は GPU 機種ごとに違うので、同じマシンで前後比較すること)
# usage: bash tests/golden_md5.sh
cd "$(dirname "$0")/.." || exit 1
OUT="$(mktemp -d)"
for s in fluid_slime slime_basic coupled_life_basic; do
  ./build/LifeOfflineRender --scene Presets/$s.json --width 640 --height 360 --fps 60 --frames 40 \
    --format png --output "$OUT/$s" >/dev/null 2>&1 || { echo "$s RUN FAILED"; continue; }
  echo "$s $(md5 -q "$OUT/$s/frame_000039.png")"
done
rm -rf "$OUT"
