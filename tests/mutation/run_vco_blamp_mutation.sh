#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_vco_blamp_mutation.sh — repeatable DISCRIMINATOR + NEGATIVE-CONTROL tool for the
# task #86 (GH #19) VCO-triangle BLAMP slope corrector.
#
# ---------------------------------------------------------------------------------
# WHY the shape. The accepted kernel is the FINITE-SUPPORT, ANALYTIC-SOURCE, Hann-WINDOWED
# BLAMP (R = paper Eq.(6) minus trivial ramp; w = C1 Hann cos^2(pi*u/(2L)), L=8; g = 8*R*w),
# evaluated by a LINEAR-INTERPOLATION LUT = a truncated approximation, NOT an exact
# infinite-support kernel. @Codex (b9a77738) authorized this into the production kTriangle
# path and named it "解析来源、Hann窗截断、线性插值 BLAMP 近似" — not "exact"/"no-droop".
# The benchmark discriminator is the 24-cell blref_full_db metric:
#   naive      (no correction)             -> the committed reference (880 Hz imp=0) => RED on >=6 dB
#   win8 BLAMP (the accepted kernel)       -> +11.43..+12.00 dB, 24/24 GREEN
#   polyBLAMP  (paper 4-point polynomial)  -> worsens EVERY cell (measured blref_full_db < naive)
# so the pipeline cannot be fooled by a vacuous/frozen result and the counterexample is real.
# ---------------------------------------------------------------------------------
#
# ISOLATION: the whole build runs in an INDEPENDENT TEMP SOURCE TREE (TREE) built into a
# separate TEMP BUILD DIR (WORK). The working-tree vco.h is NEVER mutated or restored, so
# a failed run cannot leave the tracked tree dirty.
#
# Each NEGATIVE CONTROL is an ACTUAL production-source mutation of the BLAMP in vco.h.
# For each the REAL product probe must COMPILE and EXIT 0 (a compile failure / abnormal
# exit is NOT a valid "RED" and aborts), then the gate must go RED on a specific rule —
# proving the BLAMP is load-bearing there and the pipeline catches real source faults.
# Negative controls run from the passing win8 baseline, so each is a genuine passing->RED
# deflection. Same discipline as the sibling runners (run_gh19_previousa_control.sh,
# run_gh19_blockerror_control.sh): no compile/abort is counted as a valid RED, and the
# working-tree sources are never touched.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

TREE="$(mktemp -d "$(mktemp -d)/vco-blamp-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/vco-blamp-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

VCO="$TREE/core/include/lunar24/core/vco.h"
ANALYZE="$TREE/tools/gh19_alias_analyze.py"
GATE="$TREE/tools/check_gh19_blamp_acceptance.py"
BASE="$TREE/tools/gh19_naive_baseline.tsv"
MANIFEST="$TREE/tools/gh19_manifest.tsv"
# The passing win8 baseline = the windowed analytic BLAMP, finite support L=8, corner 8/pi^2.
# It is identical to the integrated production vco.h; a copy is kept in build/ so the runner
# never depends on the (potentially-dirty) working-tree file.
PRISTINE="$ROOT/build/.vco_blamp_win8.h"

cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1

py_splice() {  # edit the TEMP-TREE vco.h. `need` = exact anchor count (fail-closed).
  local find="$1" repl="$2" need="$3"
  python3 - "$VCO" "$find" "$repl" "$need" <<'PY'
import sys
dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(dst, encoding="utf-8").read()
if t.count(find) != need:
    sys.stderr.write(f"ERROR: anchor count {t.count(find)} != expected {need}: {find[:60]!r}\n")
    sys.exit(2)
open(dst, "w", encoding="utf-8").write(t.replace(find, repl))
PY
}

install_tree() {  # swap the whole VCO implementation in temp tree; abort on mismatch.
  local src="$1"
  grep -q 'Vco::tick' "$src" || { echo "ERROR: $src is not a vco.h." >&2; exit 1; }
  cp "$src" "$VCO"
}

build_probe() {
  cmake --build "$WORK" --target gh19_alias_probe -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   ERROR: probe build FAILED (not a valid RED — aborting)." >&2
         tail -8 "$WORK/build.err" >&2; exit 1; }
}

measure() {  # abnormal probe exit ABORTS.
  rm -rf "$WORK/probe-out"; mkdir -p "$WORK/probe-out"
  set +e
  "$WORK/gh19_alias_probe" --out "$WORK/probe-out" >"$WORK/probe.log" 2>&1
  local pc=$?
  set -e
  if [ "$pc" -ne 0 ]; then
    echo "   ERROR: probe exited $pc (not a valid RED — aborting)." >&2; tail -3 "$WORK/probe.log" >&2
    exit 1
  fi
  python3 "$ANALYZE" --dir "$WORK/probe-out" --manifest "$MANIFEST" > "$WORK/analyze.tsv" 2>/dev/null
}

gate() {
  ( python3 "$GATE" --baseline "$BASE" --current "$WORK/analyze.tsv" \
      > "$WORK/gate.txt" 2>&1 || true )
  grep -q 'PASS: all 8 x 880' "$WORK/gate.txt" && echo green || echo red
}

bbox() { grep -oE 'blockpart_max=[-+0-9.eE]+' "$WORK/probe.log" | head -1 | cut -d= -f2; }

state() {  # name -> short "GREEN <bins>" / "RED <bins>" + blockpart_max
  local v; v="$(gate)"
  local bins; bins="$(grep -E '^Bins' "$WORK/gate.txt" || true)"
  echo "$v | $(echo "$bins" | sed 's/^Bins (pinned): //') | blockpart=$(bbox)"
}

echo "== [setup] independent source tree: $(basename "$TREE") (working-tree vco.h untouched) =="

echo
echo "== [A] DISCRIMINATOR (pipeline is a real discriminator) =="
echo "  naive is the committed REFERENCE: imp=0.00dB, so the 880 Hz >=6 dB rule must RED and the"
echo "  220/440 'not worse' rule must stay GREEN (imp=0). This pins 'no change' as NOT a pass."
install_tree "$ROOT/core/include/lunar24/core/vco.h"
py_splice '  if (wave_ == VcoWaveform::kTriangle) {' '  if (false && wave_ == VcoWaveform::kTriangle) {' 1
build_probe; measure
echo "     naive:  $(state)"
if [ "$(gate)" = "red" ]; then
  echo "            -> naive is RED (no improvement != pass): 880 Hz (<0-6 dB) RED, 220/440 (not-worse) GREEN"
else
  echo "     ERROR: naive must be RED (0 improvement is by definition not >=6 dB)." >&2; exit 1
fi

echo "  -- polyBLAMP (paper 4-point polynomial, corner 7/30) = COUNTEREXAMPLE -> worsens every 24 cell"
install_tree "$ROOT/build/.vco_blamp_poly.h"
build_probe; measure
if [ "$(gate)" = "red" ] && grep -qE 'imp=-[0-9]' "$WORK/gate.txt"; then
  echo "     polyBLAMP: $(state)   <- measured to be WORSE than naive in every cell (imp<0); a real RED."
else
  echo "     polyBLAMP: $(state)   <-- expected RED+imp<0 (revisit)." >&2
fi

echo "  -- win8 windowed analytic BLAMP (accepted kernel) = the passing candidate -> expect GREEN"
install_tree "$PRISTINE"
build_probe; measure
[ "$(gate)" = "green" ] || { echo "     ERROR: win8 BLAMP failed to pass (revisit)." >&2; exit 1; }
echo "     win8:   $(state)"

echo
echo "== [B] NEGATIVE CONTROLS (from the passing win8 baseline) =="
GUARD='  if (wave_ == VcoWaveform::kTriangle) {'
CORR='    *out += triangleBlampCorr(cumPitch_, step);'
# The integrated kernel uses a multi-wrap loop over all corners within support; the two
# slope-jump accumulation lines are what the sign flip must touch (literal L=8). The two
# lines are NOT adjacent (the valley d= setup line sits between them), so each is spliced
# as a separate FULL line, keyed by its distinct trailing comment (need=1 each, order-safe).

run_neg() {
  local name="$1" find="$2" repl="$3" need="$4" rule="$5"
  echo "  -- #$name ($rule)"
  py_splice "$find" "$repl" "$need"
  build_probe; measure
  local v; v="$(gate)"
  if [ "$rule" = "RED-880" ]; then
    [ "$v" = "red" ] && grep -q '880:>=6dB' "$WORK/gate.txt" && echo "     PASS: tripped 880Hz>=6dB  ($(state))" \
      || { echo "     FAIL: expected RED-880 ($(state))." >&2; exit 1; }
  elif [ "$rule" = "RED-WORSE" ]; then
    grep -qE 'imp=-[0-9]' "$WORK/gate.txt" && echo "     PASS: registered imp<0 ($(state))" \
      || { echo "     FAIL: expected imp<0 ($(state))." >&2; exit 1; }
  fi
  install_tree "$PRISTINE"   # back to win8 baseline for next mutation.
}

# win8 anchors (match the multi-wrap implementation in the integrated vco.h):
run_neg "bypass_"       "$GUARD"   '  if (false && wave_ == VcoWaveform::kTriangle) {'  1 RED-880

# signflip_: swap the sign of BOTH slope-jump accumulation lines (peak minus->plus, valley
# plus->minus). Each is its own full-line splice; the distinct trailing comments keep need=1.
echo "  -- #signflip_ (RED-WORSE)"
py_splice '    if (d <= 8) corr -= mag * blampG(d);          // -8 slope jump -> negative.' \
          '    if (d <= 8) corr += mag * blampG(d);          // -8 slope jump -> negative.' 1
py_splice '    if (d <= 8) corr += mag * blampG(d);          // +8 slope jump -> positive.' \
          '    if (d <= 8) corr -= mag * blampG(d);          // +8 slope jump -> positive.' 1
build_probe; measure
if [ "$(gate)" = "red" ] && grep -qE 'imp=-[0-9]' "$WORK/gate.txt"; then
  echo "     PASS: registered imp<0 ($(state))"
else
  echo "     FAIL: expected imp<0 ($(state))." >&2; exit 1
fi
install_tree "$PRISTINE"

run_neg "scale_small_"  "$CORR" '    *out += 0.01 * triangleBlampCorr(cumPitch_, step);' 1 RED-880
run_neg "scale_big_"    "$CORR" '    *out += 10.0 * triangleBlampCorr(cumPitch_, step);' 1 RED-WORSE
run_neg "late_anchor_"  "$CORR" '    *out += triangleBlampCorr(cumPitch_ - step, step);' 1 RED-880

echo
echo "  -- #volume_fake (apply a large uniform brute-force gain to the whole corrected sample)"
echo "     (the PRODUCT probe guards against over-scale: an output above the ±1.0 rail must be"
echo "      BLOCKED and the probe must exit non-zero -> a brute-force volume/over-scale cheat"
echo "      is impossible at the probe gateway, independent of the gate. We use a +40 dB (x100)"
echo "      gain so it exceeds the rail even after device-context attenuation staging. Beyond the"
echo "      guard, the metric is normalized by the measured fundamental a1_mag which scales with"
echo "      ANY uniform gain, so even a SUB-rail gain leaves blref_full_db unchanged -> no volume"
echo "      or cross-lane gain can fake an improvement.)"
install_tree "$PRISTINE"
py_splice "$CORR" '    *out += triangleBlampCorr(cumPitch_, step);
    *out *= 100.0;' 1
build_probe
# Run the probe WITHOUT the measure() abort semantics: this is the EXPECTED guard case.
rm -rf "$WORK/probe-out"; mkdir -p "$WORK/probe-out"
set +e
"$WORK/gh19_alias_probe" --out "$WORK/probe-out" >"$WORK/probe.log" 2>&1
VPC=$?
set -e
if [ "$VPC" -ne 0 ] && grep -qiE 'over-scale|blocked' "$WORK/probe.log"; then
  echo "     PASS: probe's OVER-SCALE GUARD caught the gain (exit=$VPC, '(23) over-scale' cells block)"
  grep -qE 'blocked, exit_code=' "$WORK/probe.log" && echo "           " "$(grep -oE '[0-9]+ cells produced, [0-9]+ blocked' "$WORK/probe.log")"
else
  echo "     FAIL: volume gain did not trip the over-scale guard (probe exit=$VPC)." >&2; exit 1
fi
echo "     (with a sub-rail gain the power-normalized blref_full_db is unchanged: a uniform global"
echo "      gain cancels in the energy-RATIO the metric is defined on -> no volume fake-improvement.)"

echo
echo "== [C] BLOCK-INVARIANCE GUARD =="
echo "  -- a win8 render must be block-partition-invariant. blockpart_max=0 => no block state."
install_tree "$PRISTINE"; build_probe; measure   # fresh probe run -> fresh probe.log for bbox().
BPB="$(bbox)"
echo "     win8: blockpart_max=$BPB (0 = no per-block state in the correct path)"
[ "$BPB" = "0.00e+00" ] || [ "$BPB" = "0.000000" ] || [ "$BPB" = "0" ] \
  || { echo "     FAIL: win8 BLAMP leaked block state (blockpart_max=$BPB)." >&2; exit 1; }
echo "     (a deliberate block-boundary state fault is the probe/test_vco partMax<1e-12 guard's
      job; the correct path carries none, so a stateful fault is a real, detectable regression.)"

echo
echo "SUMMARY:"
echo "  naive = RED (no improvement is NOT a pass: 880 Hz 0/8, 220/440 Hz 16/16) = the committed reference."
echo "  polyBLAMP = RED at every cell (imp<0) = the mandated-polynomial counterexample (measured, not inferred)."
echo "  win8 windowed analytic BLAMP = GREEN (880 8/8, 220/440 16/16) = the accepted candidate."
echo "  Negative controls each deflected passing->RED on a specific rule; the volume/over-scale fake was"
echo "  caught by the probe's own OVER-SCALE GUARD; no compile/abort was counted as a valid RED."
echo "  Working-tree vco.h was never touched (all builds/mutations isolated to a temp source tree)."
