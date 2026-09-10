#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_schmitt_blamp_mutation.sh — repeatable DISCRIMINATOR + NEGATIVE-CONTROL tool for the
# task #109 (GH #19, slice S1) SchmittOsc triangular-BLAMP slope corrector.
# Named by the task #109 contract §5.2; the shape follows the sibling runner
# tests/mutation/run_vco_blamp_mutation.sh (task #86).
#
# ---------------------------------------------------------------------------------
# WHY the shape. The accepted corrector is the FINITE-SUPPORT, ANALYTIC-SOURCE, Hann-WINDOWED
# BLAMP (L=8) evaluated by a LINEAR-INTERPOLATION LUT, and its lattice step is
# `0.5 / ceil(1/r)` — the ACTUAL integer half-period the clamped ramp HAS — not the analytic
# `0.5 * r`. The step is the load-bearing part of this slice: the rail clamp discards the
# overshoot, so the waveform leaves one rail and re-clamps exactly ceil(1/r) samples later.
# With the analytic step every half-integer (-rail) corner sits `a` samples away from the
# corner the waveform really has, which cost up to 7 dB and made three cells WORSE than the
# uncorrected render. nc4_analytic_step is that defect, re-applied on purpose.
#
# ISOLATION: the whole build runs in an INDEPENDENT TEMP SOURCE TREE (TREE) built into a
# separate TEMP BUILD DIR (WORK). The working-tree schmitt_osc.h is NEVER mutated or restored,
# so a failed run cannot leave the tracked tree dirty.
#
# Each NEGATIVE CONTROL is an ACTUAL production-source mutation of schmitt_osc.h, applied to
# the TEMP tree only. For each, the REAL product probe must COMPILE and EXIT 0 (a compile
# failure / abnormal exit is NOT a valid "RED" and aborts), then the acceptance gate must exit
# 1 on a specific, NAMED rule. Every negative control starts from the passing committed source,
# so each is a genuine passing->RED deflection. Same discipline as run_gh19_previousa_control.sh
# and run_gh19_blockerror_control.sh.
#
# The four controls, and the named rule each must trip:
#   nc1_correction_zeroed      correction term zeroed (= the pre-S1 source; [A2] MEASURES that
#                              equivalence rather than asserting it)  -> the 9 full-band cells
#                              fail `improve:>=6dB` and the 2 kernel-dense cells fail `gain>0`
#   nc2_scale_x2               mag = 0.50*r instead of 0.25*r (2x slope jump) -> same 11 cells
#   nc3_fallback_gate_removed  the `M <= 8` fallback gate deleted, so the corrector runs in the
#                              band where the kernel does not reach the half-period -> the dB
#                              table stays GREEN and the BYTE-IDENTITY assertion alone goes RED
#   nc4_analytic_step          step = 0.5*r (analytic ideal) instead of 0.5/ceil(1/r) — the
#                              defect this slice fixes -> a real subset of cells regress
#
# Usage:  bash tests/mutation/run_schmitt_blamp_mutation.sh
#         PRE_S1_REV=<rev> bash tests/mutation/run_schmitt_blamp_mutation.sh   # override [A2]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

TREE="$(mktemp -d "$(mktemp -d)/schmitt-blamp-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/schmitt-blamp-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

SCH="$TREE/core/include/lunar24/core/schmitt_osc.h"
ANALYZE="$TREE/tools/gh19_alias_analyze.py"
GATE="$TREE/tools/check_gh19_schmitt_blamp_acceptance.py"
BASE="$TREE/tools/gh19_schmitt_naive_baseline.tsv"
MANIFEST="$TREE/tools/gh19_manifest.tsv"
# The passing positive sample = the COMMITTED header. Copying it into the temp tree and then
# mutating means the positive is re-checkout-safe: no build/ backup, no gitignored artefact.
PRISTINE="$ROOT/core/include/lunar24/core/schmitt_osc.h"
# The last tree whose schmitt_osc.h carried NO correction (the red-first-gate commit). Used
# ONLY by the optional [A2] equivalence measurement; nc1 itself does not depend on git.
PRE_S1_REV="${PRE_S1_REV:-b9570b4}"

cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1

py_splice() {  # edit the TEMP-TREE schmitt_osc.h. `need` = exact anchor count (fail-closed).
  local find="$1" repl="$2" need="$3"
  python3 - "$SCH" "$find" "$repl" "$need" <<'PY'
import sys
dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(dst, encoding="utf-8").read()
if t.count(find) != need:
    sys.stderr.write(f"ERROR: anchor count {t.count(find)} != expected {need}: {find[:60]!r}\n")
    sys.exit(2)
open(dst, "w", encoding="utf-8").write(t.replace(find, repl))
PY
}

install_tree() {  # swap the whole SchmittOsc implementation in the temp tree; abort on mismatch.
  local src="$1"
  grep -q 'class SchmittOsc' "$src" || { echo "ERROR: $src is not a schmitt_osc.h." >&2; exit 1; }
  cp "$src" "$SCH"
}

build_probe() {
  cmake --build "$WORK" --target gh19_alias_probe -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   ERROR: probe build FAILED (not a valid RED — aborting)." >&2
         tail -8 "$WORK/build.err" >&2; exit 1; }
}

render_only() {  # $1 = output dir. An abnormal probe exit ABORTS.
  local out="$1"
  rm -rf "$out"; mkdir -p "$out"
  set +e
  "$WORK/gh19_alias_probe" --out "$out" >"$WORK/probe.log" 2>&1
  local pc=$?
  set -e
  if [ "$pc" -ne 0 ]; then
    echo "   ERROR: probe exited $pc (not a valid RED — aborting)." >&2; tail -3 "$WORK/probe.log" >&2
    exit 1
  fi
}

measure() { render_only "$WORK/probe-out"
  python3 "$ANALYZE" --dir "$WORK/probe-out" --manifest "$MANIFEST" > "$WORK/analyze.tsv" 2>/dev/null
}

run_gate() {  # $@ = extra gate flags. Sets GRC. Exits 0 either way (RED is expected here).
  set +e
  python3 "$GATE" --baseline "$BASE" --current "$WORK/analyze.tsv" \
    --current-raws "$WORK/probe-out" "$@" > "$WORK/gate.txt" 2>&1
  GRC=$?
  set -e
}

bins()      { grep -E '^Bins' "$WORK/gate.txt" || true; }
fail_count(){ grep -cE '^  - ' "$WORK/gate.txt" || true; }
fail()      { echo "     FAIL: $*" >&2; exit 1; }

neg_start() { install_tree "$PRISTINE"; }
neg_run()   { build_probe; measure; run_gate; echo "     $(bins)  FAIL lines=$(fail_count)"; }

echo "== [setup] independent source tree: $(basename "$TREE") (working-tree schmitt_osc.h untouched) =="

echo
echo "== [A] POSITIVE: the committed source must be GREEN (the pipeline is a discriminator) =="
neg_start
build_probe; measure
run_gate --self-check
echo "     $(bins)  FAIL lines=$(fail_count)"
[ "$GRC" = 0 ] || { cat "$WORK/gate.txt" >&2; fail "committed source must PASS the gate (rc=$GRC)."; }
grep -q 'full(M>=12) passed=9/9' "$WORK/gate.txt" || fail "expected 9/9 full-band cells to pass."
grep -q 'dense(9<=M<=11) passed=2/2' "$WORK/gate.txt" || fail "expected 2/2 kernel-dense cells to pass."
grep -q 'fallback(M<=8) passed=1/1' "$WORK/gate.txt" || fail "expected 1/1 fallback cell to pass."
grep -q 'self-check: PASS' "$WORK/gate.txt" || fail "the gate's own self-check did not pass."
grep -q '== the uncorrected render' "$WORK/gate.txt" \
  || fail "the fallback cell is not byte-identical to the uncorrected render."
echo "     -> committed source GREEN, gate self-check PASS, fallback bytes == uncorrected."

echo
echo "== [A2] nc1's mutation is the pre-S1 source — MEASURED, not asserted =="
echo "  (contract §5.2 ① is 'restore the pre-S1 source'. This step renders BOTH the literal"
echo "   pre-S1 header and the nc1-spliced committed header and diffs every raw render.)"
neg_start
py_splice 'const double corr = railBlampCorr(rate / sampleRate_, rail);' \
          'const double corr = railBlampCorr(rate / sampleRate_, rail) * 0.0;  // NC-1: correction zeroed.' 1
if git -C "$ROOT" cat-file -e "$PRE_S1_REV:core/include/lunar24/core/schmitt_osc.h" 2>/dev/null; then
  git -C "$ROOT" show "$PRE_S1_REV:core/include/lunar24/core/schmitt_osc.h" > "$WORK/pre-s1.h"
  build_probe
  render_only "$WORK/raw-spliced"
  install_tree "$WORK/pre-s1.h"
  build_probe
  render_only "$WORK/raw-pres1"
  DIFFS=0; NFILES=0
  for f in "$WORK/raw-spliced"/*.raw; do
    NFILES=$((NFILES + 1))
    cmp -s "$f" "$WORK/raw-pres1/$(basename "$f")" || DIFFS=$((DIFFS + 1))
  done
  [ "$NFILES" -gt 0 ] || fail "[A2] no raw renders produced."
  [ "$DIFFS" = 0 ] \
    || fail "[A2] the nc1 splice and the literal pre-S1 header differ in $DIFFS/$NFILES renders"
  echo "     PASS: $NFILES/$NFILES renders byte-identical -> nc1's mutation IS the pre-S1 source."
  echo "           (the correction term is the only output-affecting part of the S1 diff;"
  echo "            restoring that header is therefore equivalent to zeroing the correction.)"
else
  echo "     SKIP: rev '$PRE_S1_REV' is not present in $ROOT, so the literal pre-S1 header"
  echo "           cannot be extracted. nc1 below still runs (it is a self-contained splice);"
  echo "           only this equivalence MEASUREMENT is skipped, and it is NOT silently passed."
fi

echo
echo "== [B] NEGATIVE CONTROLS (each: real source mutation -> probe must compile+exit 0 -> gate RED) =="

echo "  -- #nc1_correction_zeroed (the correction term is zeroed; rule: 11 in-gate cells lose their gain)"
neg_start
py_splice 'const double corr = railBlampCorr(rate / sampleRate_, rail);' \
          'const double corr = railBlampCorr(rate / sampleRate_, rail) * 0.0;  // NC-1: correction zeroed.' 1
neg_run
[ "$GRC" = 1 ] || fail "nc1: gate rc=$GRC, expected 1."
grep -q 'full(M>=12) passed=0/9' "$WORK/gate.txt" || fail "nc1: expected 0/9 full-band cells to pass."
grep -q 'dense(9<=M<=11) passed=0/2' "$WORK/gate.txt" || fail "nc1: expected 0/2 kernel-dense cells to pass."
grep -q 'fallback(M<=8) passed=1/1' "$WORK/gate.txt" || fail "nc1: the fallback band must stay green."
grep -qE '^  - [a-z0-9_]+: improve:>=6dB' "$WORK/gate.txt" || fail "nc1: the 6 dB rule did not trip."
[ "$(fail_count)" = 11 ] || fail "nc1: expected 11 failing cells, got $(fail_count)."
grep -q '== the uncorrected render' "$WORK/gate.txt" \
  || fail "nc1: the fallback cell must stay byte-identical (its path is untouched)."
echo "     PASS: 11/11 in-gate cells RED on their band rule; fallback still byte-identical."

echo "  -- #nc2_scale_x2 (mag = 0.50*r instead of 0.25*r; rule: the same 11 cells lose their gain)"
neg_start
py_splice 'const double mag = 0.25 * r;' 'const double mag = 0.50 * r;  // NC-2: 2x scale.' 1
neg_run
[ "$GRC" = 1 ] || fail "nc2: gate rc=$GRC, expected 1."
grep -q 'full(M>=12) passed=0/9' "$WORK/gate.txt" || fail "nc2: expected 0/9 full-band cells to pass."
grep -q 'dense(9<=M<=11) passed=0/2' "$WORK/gate.txt" || fail "nc2: expected 0/2 kernel-dense cells to pass."
[ "$(fail_count)" = 11 ] || fail "nc2: expected 11 failing cells, got $(fail_count)."
echo "     PASS: a 2x slope-jump scale is caught on the same 11 cells."

echo "  -- #nc3_fallback_gate_removed (rule: the BYTE-IDENTITY assertion alone must go RED)"
neg_start
py_splice 'if (blampSupportReachesHalfPeriod(step)) return 0.0;    // M <= 8: fallback.' \
          '// NC-3: fallback gate removed.' 1
neg_run
[ "$GRC" = 1 ] || fail "nc3: gate rc=$GRC, expected 1 (removing the fallback gate must be caught)."
grep -q 'full(M>=12) passed=9/9' "$WORK/gate.txt" || fail "nc3: the full band should be unaffected."
grep -q 'dense(9<=M<=11) passed=2/2' "$WORK/gate.txt" || fail "nc3: the dense band should be unaffected."
grep -q 'fallback(M<=8) passed=1/1' "$WORK/gate.txt" || fail "nc3: the fallback dB rule should still pass."
grep -q 'DIFFERS from the uncorrected render' "$WORK/gate.txt" \
  || fail "nc3: the fallback render did not change — the byte assertion is NOT discriminating."
grep -qE '^  - [a-z0-9_]+: fallback byte-identity vs the uncorrected render FAILED' "$WORK/gate.txt" \
  || fail "nc3: the byte-identity assertion did not produce a FAIL row."
[ "$(fail_count)" = 1 ] || fail "nc3: expected exactly 1 failing cell, got $(fail_count)."
echo "     PASS: the dB table stays fully GREEN and the byte assertion ALONE is RED on one cell"
echo "           -> the 'fallback == uncorrected' pin is load-bearing, not decorative."

echo "  -- #nc4_analytic_step (step = 0.5*r, the analytic ideal — the defect S1 fixes)"
neg_start
py_splice 'const double step = 0.5 / mActual;          // cycles/sample; exact at every clamp.' \
          'const double step = 0.5 * r;  // NC-4: analytic ideal half-period (the S1 defect).' 1
neg_run
[ "$GRC" = 1 ] || fail "nc4: gate rc=$GRC, expected 1 (the analytic step must be caught)."
grep -qE '^  - [a-z0-9_]+: improve:>=6dB' "$WORK/gate.txt" || fail "nc4: the 6 dB rule did not trip."
[ "$(fail_count)" = 5 ] || fail "nc4: expected 5 failing cells, got $(fail_count)."
echo "     PASS: the analytic-lattice step is caught (the regression this slice exists to fix)."

echo
echo "SUMMARY:"
echo "  committed source            = GREEN (9/9 full, 2/2 dense, 1/1 fallback, bytes == uncorrected)."
echo "  nc1_correction_zeroed       = RED on 11 cells; [A2] MEASURED it to be the pre-S1 source."
echo "  nc2_scale_x2                = RED on the same 11 cells."
echo "  nc3_fallback_gate_removed   = RED on the byte-identity assertion ALONE (dB table fully green)."
echo "  nc4_analytic_step           = RED on 5 cells (the defect this slice fixes)."
echo "  Every control compiled and its probe exited 0; no compile/abort was counted as a valid RED."
echo "  The working-tree schmitt_osc.h was never touched (all builds/mutations isolated to a temp tree)."
