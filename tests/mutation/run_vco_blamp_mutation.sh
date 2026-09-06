#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_vco_blamp_mutation.sh — repeatable BUILD -> MUTATE -> MEASURE -> RESTORE tool for the
# task #86 (GH #19) VCO-triangle BLAMP (slope) correction.
#
# Each defense point is an ACTUAL production-source mutation of the BLAMP in
# core/include/lunar24/core/vco.h (never a relaxed validator), applied to a PRIVATE backup of the
# real BLAMP version (the backup is the source of truth; tracked vco.h on disk is NEVER git-checked-
# out, so the uncommitted BLAMP work is not destroyed). For each mutation the REAL product probe is
# rebuilt, run through the verified analyzer, and the GH#19 acceptance gate (check_gh19_blamp_
# acceptance.py) must go RED on a SPECIFIC criterion — proving the BLAMP is load-bearing at that
# defense point. The script then restores the pristine BLAMP and proves the gate goes GREEN again.
#
# Defense points (each ends normally, hits a specific failure, recovers green):
#   #1 bypass_     correction branch disabled         -> 880Hz imp=0.00dB (missing 6dB) RED
#   #2 signflip_   peak/valley correction signs swap  -> correction backwards -> cell WORSE than baseline RED
#   #3 scale_small 0.01x correction                   -> 880Hz imp << 6dB RED
#   #4 scale_big   10x correction (overshoot)         -> cell WORSE (overcorrection) RED
#   #5 delay_corr  correction anchored one sample late-> mistuned residual -> 880Hz imp << 6dB RED
#
# Structural controls NOT covered by this alias-metric (cross-lane isolation, volume fake-improvement,
# block-boundary reset) are asserted by the product probe itself (A/B isolation + blockpart_max=0) and
# by test_vco.cpp's phase-advance / amplitude / block-invariance checks.
#
# Evidence boundary: this is intentionally NOT registered in CTest — the mutated build fails by design;
# run it under a scratch build (BUILD_DIR) so the tracked tree stays clean.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

# Pristine BLAMP vco.h: a private backup made before this script mutates anything. The script makes
# its own backup from the CURRENT working vco.h on entry (so it captures the real BLAMP under test,
# whatever state it is in), and restores it after every mutation.
VCO="core/include/lunar24/core/vco.h"
BACKUP="$ROOT/build/.vco_blamp_pristine.h"
mkdir -p build
cp "$VCO" "$BACKUP"

BUILD_DIR="${BUILD_DIR:-build/blamp-mut}"
ANALYZE="tools/gh19_alias_analyze.py"
MANIFEST="tools/gh19_manifest.tsv"
GATE="tools/check_gh19_blamp_acceptance.py"
BASE="tools/gh19_naive_baseline.tsv"
PROBE_OUT="$BUILD_DIR/probe-out"

purge() { rm -rf "$BUILD_DIR"; }
trap 'purge; cp "$BACKUP" "$VCO"' EXIT   # always restore the pristine BLAMP on exit.

# Python splice on the backup -> current working vco.h. `need` = exact anchor count.
py_splice() {
  local find="$1" repl="$2" need="$3"
  python3 - "$BACKUP" "$VCO" "$find" "$repl" "$need" <<'PY'
import os, sys
src, dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
t = open(src, encoding="utf-8").read()
n = t.count(find)
if n != need:
    sys.stderr.write(f"ERROR: anchor count {n} != expected {need}: {find[:60]!r}\n")
    sys.exit(2)
t = t.replace(find, repl)
open(dst, "w", encoding="utf-8").write(t)
PY
}

build_probe() {
  cmake --build "$BUILD_DIR" --target gh19_alias_probe -j 8 >/dev/null 2>"$BUILD_DIR/build.err" \
    || { echo "   ERROR: probe build failed"; tail -5 "$BUILD_DIR/build.err" >&2; exit 1; }
}

measure() {  # runs the probe + analyzer for the CURRENT vco.h; writes $"$PROBE_OUT"
  rm -rf "$PROBE_OUT"; mkdir -p "$PROBE_OUT"
  "$BUILD_DIR/gh19_alias_probe" --out "$PROBE_OUT" >/dev/null 2>&1
  python3 "$ANALYZE" --dir "$PROBE_OUT" --manifest "$MANIFEST" > "$BUILD_DIR/analyze.tsv" 2>/dev/null
}

gate() {  # returns the acceptance-gate exit code for the CURRENT analyze.tsv
  ( python3 "$GATE" --baseline "$BASE" --current "$BUILD_DIR/analyze.tsv" \
      > "$BUILD_DIR/gate.txt" 2>&1 || true )
  grep -q 'PASS: all 24' "$BUILD_DIR/gate.txt" && echo green || echo red
}

configure() {
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
}

echo "== [setup] configure scratch build ($BUILD_DIR) =="
configure

echo
echo "== [build/measure] REAL BLAMP baseline =="
build_probe; measure
V="$(gate)"
echo "   real BLAMP: gate=$V (expect green)"
[ "$V" = "green" ] || { echo "   ERROR: pristine BLAMP did not pass the gate." >&2; exit 1; }

run_mutation() {
  local name="$1" find="$2" repl="$3" need="${4:-1}" exp="$5"
  echo
  echo "== [mutation #$name] $exp =="
  py_splice "$find" "$repl" "$need"
  build_probe; measure
  V="$(gate)"
  # a RED gate asserts the acceptance failed AT THE SPECIFIC cell(s) named by the criterion.
  echo "   mutate $name: gate=$V (expect red)"
  if [ "$exp" = "RED-880" ]; then
    # expect only the 880Hz >=6dB rule to fail (imp<6dB), 220/440 still green.
    if [ "$V" = "red" ] && grep -q '880:>=6dB' "$BUILD_DIR/gate.txt"; then
      echo "   PASS: $name RED on 880Hz>=6dB rule."
    else
      echo "   FAIL: $name did not trip the 880Hz>=6dB rule (gate=$V)." >&2; exit 1
    fi
  elif [ "$exp" = "RED-WORSE" ]; then
    # expect the signal to be WORSE than the baseline (negative improvement) — sign/scale/overshoot.
    if grep -qE 'imp=-[0-9]' "$BUILD_DIR/gate.txt"; then
      echo "   PASS: $name RED (registered a negative improvement = worse than naive)."
    else
      echo "   FAIL: $name did not register a worsening (no imp=-..)." >&2; exit 1
    fi
  fi
  cp "$BACKUP" "$VCO"    # restore the pristine BLAMP between mutations.
}

# Anchor in the current BLAMP vco.h, applied to the correction call:
CORR_CALL='*out += triangleBlampCorr(cumPitch_, step);'

run_mutation "bypass"      'if (wave_ == VcoWaveform::kTriangle) {' \
  'if (false && wave_ == VcoWaveform::kTriangle) {' 1 RED-880
run_mutation "signflip"    'if (uPeak <= kBlampUmax) corr -= dt * blampG(uPeak);
  if (uVal <= kBlampUmax) corr += dt * blampG(uVal);' \
  'if (uPeak <= kBlampUmax) corr += dt * blampG(uPeak);
  if (uVal <= kBlampUmax) corr -= dt * blampG(uVal);' 1 RED-WORSE
run_mutation "scale_small" "$CORR_CALL" \
  '*out += 0.01 * triangleBlampCorr(cumPitch_, step);' 1 RED-880
run_mutation "scale_big"   "$CORR_CALL" \
  '*out += 10.0 * triangleBlampCorr(cumPitch_, step);' 1 RED-WORSE
run_mutation "delay_corr"  "$CORR_CALL" \
  '*out += triangleBlampCorr(cumPitch_ - step, step);' 1 RED-880

echo
echo "== [recovery] restore pristine BLAMP, rebuild, re-measure =="
cp "$BACKUP" "$VCO"
build_probe; measure
V="$(gate)"
echo "   recovery: gate=$V (expect green)"
[ "$V" = "green" ] || { echo "   ERROR: BLAMP did not recover green after mutations." >&2; exit 1; }

echo
echo "ALL NEGATIVE-CONTROL MUTATIONS: each RED on a specific criterion and recovered GREEN."
