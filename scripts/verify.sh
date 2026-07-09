#!/bin/bash
# scripts/verify.sh — full regression suite for LifeSimSuite.
# Usage:
#   bash scripts/verify.sh              # build + all checks
#   SKIP_BUILD=1 bash scripts/verify.sh # reuse existing ./build
#
# Checks:
#   1. build (unless SKIP_BUILD=1)
#   2. every Presets/*.json renders 60 frames offline, exit 0, no error lines
#   3. determinism: representative presets render twice -> final md5 identical
#
# No `set -e`: failures are counted explicitly so the whole suite always runs
# (grep/lsof return nonzero on no-match and would kill a `set -e` script).

cd "$(dirname "$0")/.." || exit 1

FAIL=0
NOTE() { printf '%s\n' "$*"; }
PASS() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
FAILED() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }

TMP="$(mktemp -d /tmp/lifesim_verify.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# ---- 1. build --------------------------------------------------------------
if [ -z "$SKIP_BUILD" ]; then
  NOTE "== build =="
  cmake -B build -DCMAKE_BUILD_TYPE=Release > "$TMP/cmake.log" 2>&1
  if cmake --build build -j8 > "$TMP/build.log" 2>&1; then
    PASS "build"
  else
    FAILED "build (tail of log:)"
    tail -20 "$TMP/build.log"
    exit 1
  fi
fi

BIN=./build/LifeOfflineRender
[ -x "$BIN" ] || { NOTE "missing $BIN"; exit 1; }

# ---- 2. every preset renders clean ----------------------------------------
NOTE "== presets (60 frames each) =="
ERR_PATTERN='error|failed|invalid|unknown|not found|cannot'
for scene in Presets/*.json; do
  name="$(basename "$scene" .json)"
  out="$TMP/p_$name"
  if "$BIN" --scene "$scene" --frames 60 --output "$out" --format png \
      > "$TMP/$name.log" 2>&1; then
    if grep -qiE "$ERR_PATTERN" "$TMP/$name.log"; then
      FAILED "$name (error lines in output:)"
      grep -iE "$ERR_PATTERN" "$TMP/$name.log" | head -3
    elif [ ! -f "$out/frame_000059.png" ]; then
      FAILED "$name (no final frame written)"
    else
      PASS "$name"
    fi
  else
    FAILED "$name (exit $?)"
    tail -3 "$TMP/$name.log"
  fi
done

# ---- 3. determinism on representatives ------------------------------------
NOTE "== determinism (2 runs -> identical final md5) =="
DET_PRESETS="default slime_basic particle_life_basic fluid_basic"
for name in $DET_PRESETS; do
  scene="Presets/$name.json"
  [ -f "$scene" ] || continue
  a="$TMP/da_$name"; b="$TMP/db_$name"
  "$BIN" --scene "$scene" --frames 60 --width 320 --height 180 \
      --output "$a" --format png > /dev/null 2>&1
  "$BIN" --scene "$scene" --frames 60 --width 320 --height 180 \
      --output "$b" --format png > /dev/null 2>&1
  ma="$(md5 -q "$a/frame_000059.png" 2>/dev/null)"
  mb="$(md5 -q "$b/frame_000059.png" 2>/dev/null)"
  if [ -n "$ma" ] && [ "$ma" = "$mb" ]; then
    PASS "$name ($ma)"
  else
    FAILED "$name ('$ma' vs '$mb')"
  fi
done

NOTE ""
if [ "$FAIL" -eq 0 ]; then
  NOTE "verify: ALL GREEN"
else
  NOTE "verify: $FAIL failure(s)"
fi
exit "$FAIL"
