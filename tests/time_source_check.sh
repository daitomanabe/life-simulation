#!/bin/bash
# tests/time_source_check.sh
# Self-check for the "time.*" ParameterBus sources (time.sin/tri/ramp/noise
# + the automation mapping list). Not a full metrological analysis (that's
# done by hand against room-b.json when tuning the installation) — just
# enough to fail loudly if the parsing or the waveform math regresses:
#   1. range sanity for each waveform (sin/tri/noise in -1..1, ramp in 0..1)
#   2. sin actually reverses sign (the whole point for fluid0.driftX)
#   3. two renders with the same seed/frames are byte-identical (params.csv)
#   4. a malformed/non-positive period is rejected at scene load, naming
#      the offending source string, instead of silently loading as 0
#
# usage: bash tests/time_source_check.sh

cd "$(dirname "$0")/.." || exit 1

BIN="./build/LifeOfflineRender"
SCENE="tests/scenes/time_source_probe.json"
KEYS="probe.sin,probe.tri,probe.ramp,probe.noise"
FRAMES=250   # period is 2s @ 60fps = 120 frames -> ~2 full cycles
FAIL=0

PASS() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
FAILED() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }

for f in "$BIN" "$SCENE"; do
    [ -e "$f" ] || { echo "missing: $f"; exit 1; }
done

TMP="$(mktemp -d /tmp/lifesim_time.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

render() {
    "$BIN" --scene "$SCENE" --fps 60 --frames "$FRAMES" --format none \
        --dump-params "$KEYS" --output "$1" > "$TMP/log_$2.txt" 2>&1
}

echo "== render (1/2) =="
mkdir -p "$TMP/a"
render "$TMP/a" a || { echo "render failed"; cat "$TMP/log_a.txt"; exit 1; }

# ---- 1/2. range + sign-reversal sanity -------------------------------------
echo "== 1/2. waveform ranges and sign reversal =="
read -r sinMin sinMax <<EOF
$(awk -F, 'NR>1{v=$2; if(min==""||v<min)min=v; if(max==""||v>max)max=v} END{print min, max}' "$TMP/a/params.csv")
EOF
read -r triMin triMax <<EOF
$(awk -F, 'NR>1{v=$3; if(min==""||v<min)min=v; if(max==""||v>max)max=v} END{print min, max}' "$TMP/a/params.csv")
EOF
read -r rampMin rampMax <<EOF
$(awk -F, 'NR>1{v=$4; if(min==""||v<min)min=v; if(max==""||v>max)max=v} END{print min, max}' "$TMP/a/params.csv")
EOF
read -r noiseMin noiseMax <<EOF
$(awk -F, 'NR>1{v=$5; if(min==""||v<min)min=v; if(max==""||v>max)max=v} END{print min, max}' "$TMP/a/params.csv")
EOF

check_range() {
    # name lo hi expectLo expectHi
    awk -v lo="$2" -v hi="$3" -v elo="$4" -v ehi="$5" \
        'BEGIN{ if (lo < elo-0.05 || lo > elo+0.35 || hi < ehi-0.35 || hi > ehi+0.05) exit 1; exit 0 }' \
        && PASS "$1: [$2, $3] ~ [$4, $5]" || FAILED "$1: [$2, $3] NOT close to [$4, $5]"
}
check_range "time.sin"   "$sinMin"   "$sinMax"   -1.0 1.0
check_range "time.tri"   "$triMin"   "$triMax"   -1.0 1.0
check_range "time.ramp"  "$rampMin"  "$rampMax"  0.0  1.0
check_range "time.noise" "$noiseMin" "$noiseMax" -1.0 1.0

CROSSINGS=$(awk -F, 'NR>2{if (prev<0 && $2>=0) c++} {prev=$2} END{print c+0}' "$TMP/a/params.csv")
if [ "$CROSSINGS" -ge 2 ]; then PASS "time.sin crosses zero $CROSSINGS times (reverses direction)"
else FAILED "time.sin only crossed zero $CROSSINGS times over $FRAMES frames"; fi

# ---- 3. determinism ---------------------------------------------------------
echo "== 3. determinism: same seed + frames -> byte-identical params.csv =="
mkdir -p "$TMP/b"
render "$TMP/b" b || { echo "render failed"; cat "$TMP/log_b.txt"; exit 1; }
if diff -q "$TMP/a/params.csv" "$TMP/b/params.csv" >/dev/null; then
    PASS "two renders produced identical params.csv"
else
    FAILED "two renders of the same scene/seed/frames diverged"
fi

# ---- 4. malformed period is rejected at load, not silently zero -----------
echo "== 4. malformed/non-positive period is rejected at scene load =="
BAD="$TMP/bad_period.json"
sed 's/"time.sin:2"/"time.sin:0"/' "$SCENE" > "$BAD"
OUT="$("$BIN" --scene "$BAD" --fps 60 --frames 1 --format none --output "$TMP/bad" 2>&1)"
CODE=$?
if [ "$CODE" -ne 0 ] && printf '%s' "$OUT" | grep -q "time.sin:0"; then
    PASS "non-positive period rejected, error names \"time.sin:0\""
else
    FAILED "non-positive period was NOT rejected with a message naming the source (exit=$CODE)"
fi

BAD2="$TMP/bad_kind.json"
sed 's/"time.sin:2"/"time.bogus:2"/' "$SCENE" > "$BAD2"
OUT2="$("$BIN" --scene "$BAD2" --fps 60 --frames 1 --format none --output "$TMP/bad2" 2>&1)"
CODE2=$?
if [ "$CODE2" -ne 0 ] && printf '%s' "$OUT2" | grep -q "time.bogus"; then
    PASS "unknown time source kind rejected, error names \"time.bogus:2\""
else
    FAILED "unknown time source kind was NOT rejected with a message naming the source (exit=$CODE2)"
fi

# 5. The clock must still be accurate after hours of dt accumulation.
# A float clock adding 1/60 drifts 2.8 s after one hour and 623 s after eight
# (near 28,800 s a float's step is 0.00195 s, so ~6% of every dt is rounded
# away), and stops advancing entirely at 78 h. With a 2 s period one hour of
# drift is more than a whole cycle, so the ramp value below is a sharp test.
# ~30 s to run: one simulated hour at 64x64, writing only the last frame.
echo
echo "== 5. clock accuracy after one simulated hour (catches a float clock) =="
LONG="$TMP/long"
"$BIN" --scene "$SCENE" --width 64 --height 64 --fps 60 --frames 216000 --warmup 215999 \
    --format none --output "$LONG" --dump-params "$KEYS" >/dev/null 2>&1
if [ ! -f "$LONG/params.csv" ]; then
    FAILED "one-hour run produced no params.csv"
else
    python3 - "$LONG/params.csv" <<'PY_EOF'
import csv, sys
rows = list(csv.reader(open(sys.argv[1])))
header, last = rows[0], rows[-1]
frame = int(last[0])
t = frame / 60.0                      # exact: what the clock SHOULD read
col = {name: i for i, name in enumerate(header)}
ramp_col = next(i for name, i in col.items() if "ramp" in name.lower()) \
    if any("ramp" in n.lower() for n in col) else 3
got = float(last[ramp_col])
want = (t / 2.0) % 1.0                # period 2 s, phase 0
err = abs(got - want)
err = min(err, 1.0 - err)             # ramp wraps
if err < 1e-3:
    print(f"  \033[32mPASS\033[0m ramp at t={t:.4f}s is {got:.6f}, expected {want:.6f} (err {err:.2e})")
else:
    print(f"  \033[31mFAIL\033[0m ramp at t={t:.4f}s is {got:.6f}, expected {want:.6f} "
          f"(err {err:.2e}) -- the clock has drifted; is it still a double?")
    sys.exit(1)
PY_EOF
    if [ $? -eq 0 ]; then PASS_SILENT=1; else FAILED "clock drifted over one simulated hour"; fi
fi

echo
if [ "$FAIL" -eq 0 ]; then echo "ALL PASS"; else echo "$FAIL CHECK(S) FAILED"; fi
exit "$FAIL"
