#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_classic_saw_polyblep_mutation.sh — repeatable DISCRIMINATOR + NEGATIVE-CONTROL tool for
# the task #110 (GH #19, slice S2) classic-drone-saw polyBLEP value-jump corrector.
# The shape follows the sibling runner tests/mutation/run_schmitt_blamp_mutation.sh (task #109).
#
# ---------------------------------------------------------------------------------
# WHY the shape. S2 replaces the classic drone's `nonlinearity(sawtooth(v.phase))` with
# `nonlinearity(polyblepSaw(t, phaseInc))`, where `phaseInc = effFreq / sampleRate_` is the
# SAME increment the phase accumulator takes on the following line. Three things are
# load-bearing and each gets its own control:
#
#   * the correction is APPLIED AT ALL (zero it and nothing changes),
#   * its SIGN (the naive saw jumps +1 -> -1; a `+R` doubles the jump instead of removing it),
#   * its WINDOW WIDTH (the residual's support is +/- dt, so the `dt` passed must be THIS
#     sample's phase increment. drone_bank.h already has a local `dt` in the same function —
#     `const double dt = 1.0 / sampleRate_;` at :258, the ENVELOPE time step. Passing that one
#     is the exact name-collision defect a careless integration produces, and it is nc3.)
#   * the ORDER relative to the cubic (correcting the post-cubic value over-corrects by a
#     measured 1.500x — see nc4 and the S2 report).
#
# ISOLATION: the whole build runs in an INDEPENDENT TEMP SOURCE TREE (TREE) built into a
# separate TEMP BUILD DIR (WORK). The working-tree drone_bank.h is NEVER mutated or restored,
# so a failed run cannot leave the tracked tree dirty.
#
# Each NEGATIVE CONTROL is an ACTUAL production-source mutation applied to the TEMP tree only.
# For each, the REAL product probe must COMPILE and EXIT 0 (a compile failure / abnormal exit
# is NOT a valid "RED" and aborts), then the acceptance gate must exit non-zero on a SPECIFIC,
# NAMED rule. Every negative control starts from the passing committed source, so each is a
# genuine passing->RED deflection.
#
# Usage:  bash tests/mutation/run_classic_saw_polyblep_mutation.sh
#         PRE_S2_REV=<rev> bash tests/mutation/run_classic_saw_polyblep_mutation.sh   # override [A2]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

TREE="$(mktemp -d "$(mktemp -d)/classic-saw-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/classic-saw-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

# Every local build dir a developer may have is excluded -- build-san included (the ASan+UBSan
# leg's dir). The runner configures its OWN $WORK, so copying them is pure waste (~287 MB here).
rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude 'build-rel' --exclude 'build-dbg' \
  --exclude 'build-debug' --exclude 'build-san' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

BANK="$TREE/core/include/lunar24/core/drone_bank.h"
ANALYZE="$TREE/tools/gh19_alias_analyze.py"
GATE="$TREE/tools/check_gh19_classic_saw_acceptance.py"
BASE="$TREE/tools/gh19_classic_saw_naive_baseline.tsv"
MANIFEST="$TREE/tools/gh19_manifest.tsv"
# The reference the corrected arm is scored against, and the literal pre-S2 source for [A2].
PRE_S2_REV="${PRE_S2_REV:-1f667d53f9ae3c4d12b6851115dcb88f703e4e7e}"
NAIVE_RAWS="${NAIVE_RAWS:-}"

# Per-role acceptance thresholds. These DEFAULTS are load-bearing: a bare run must reproduce
# the APPROVED configuration. They used to default to EMPTY, which fell through to
# --min-gain-db 6.0 and reds all 24 LOW cells (measured ceiling +5.88 dB) -- so the positive
# control came back RED and anyone running this without the two env vars got a FALSE FAIL.
# Values = the ones approved for S2, set AFTER measuring the fixed arm (@Kimi 612691ea: the
# thresholds follow the measurement, they do not precede it): LOW 5.0 / HIGH 7.0 against
# measured weakest +5.88 / +7.85 => 0.88 / 0.85 dB margin. Changing a threshold requires new
# evidence, not an edit here.
MIN_LOW_DB="${MIN_LOW_DB:-5.0}"
MIN_HIGH_DB="${MIN_HIGH_DB:-7.0}"
# Inert while both per-role values above are present: the gate prefers them over this one.
MIN_DB="${MIN_DB:-6.0}"

GATE_ARGS=(--min-gain-db "$MIN_DB" --min-gain-low-db "$MIN_LOW_DB" --min-gain-high-db "$MIN_HIGH_DB")

[ -f "$BASE" ] || { echo "ERROR: missing $BASE — regenerate the 36-cell S2 naive baseline first." >&2; exit 1; }

cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1

py_splice() {  # edit the TEMP-TREE drone_bank.h. `need` = exact anchor count (fail-closed).
  local find="$1" repl="$2" need="$3"
  python3 - "$BANK" "$find" "$repl" "$need" <<'PY'
import sys
dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(dst, encoding="utf-8").read()
if t.count(find) != need:
    sys.stderr.write(f"ERROR: anchor count {t.count(find)} != expected {need}: {find[:60]!r}\n")
    sys.exit(2)
open(dst, "w", encoding="utf-8").write(t.replace(find, repl))
PY
}

# The corrected call site, exactly as the implementation writes it. Every control splices this
# WHOLE block, `phaseInc` declaration included, so a control that stops using `phaseInc` drops
# it too -- otherwise `-Wunused-variable` under -Werror fails the build and the runner aborts
# with "not a valid RED" instead of producing a verdict.
FIXED_BLOCK='      const double phaseInc = effFreq / sampleRate_;
      const double s =
          v.muted ? 0.0 : v.amplitude * nonlinearity(polyblepSaw(v.phase / twoPi_, phaseInc));'

NC1_BLOCK='      const double s = v.muted ? 0.0 : v.amplitude * nonlinearity(sawtooth(v.phase));'

NC2_BLOCK='      const double phaseInc = effFreq / sampleRate_;
      const double s =
          v.muted ? 0.0
                  : v.amplitude *
                        nonlinearity(sawtooth(v.phase) +
                                     polyblepResidual(v.phase / twoPi_, phaseInc));'

NC3_BLOCK='      const double s =
          v.muted ? 0.0 : v.amplitude * nonlinearity(polyblepSaw(v.phase / twoPi_, dt));'

NC4_BLOCK='      const double phaseInc = effFreq / sampleRate_;
      const double s =
          v.muted ? 0.0
                  : v.amplitude * (nonlinearity(sawtooth(v.phase)) -
                                   polyblepResidual(v.phase / twoPi_, phaseInc));'

# The integration is TWO edits: the include AND the call site. A missing include is a build
# failure, which the runner would report as "not a valid RED" — an abort for a reason that has
# nothing to do with the mutation under test. So both are anchored.
INCLUDE_LINE='#include "lunar24/core/polyblep_kernel.h"'
INCLUDE_AND_NL="$INCLUDE_LINE
"

# A control is only meaningful if the corrected block and its include are each present exactly
# once; if the integration text and these constants ever drift apart, that must fail loudly
# rather than splice nothing.
check_anchor() {
  local nf ni
  read -r nf ni <<<"$(python3 - "$BANK" "$FIXED_BLOCK" "$INCLUDE_LINE" <<'PY'
import sys
t = open(sys.argv[1], encoding="utf-8").read()
print(t.count(sys.argv[2]), t.count(sys.argv[3]))
PY
)"
  if [ "$nf" != "1" ]; then
    echo "ERROR: the corrected call-site block appears $nf time(s) in drone_bank.h, expected 1." >&2
    echo "       The runner's FIXED_BLOCK and the implementation have drifted apart." >&2
    exit 1
  fi
  if [ "$ni" != "1" ]; then
    echo "ERROR: '$INCLUDE_LINE' appears $ni time(s) in drone_bank.h, expected 1." >&2
    echo "       The runner's INCLUDE_LINE and the implementation have drifted apart." >&2
    exit 1
  fi
}

restore_source() {  # put the COMMITTED drone_bank.h back, so every control starts from green.
  cp "$ROOT/core/include/lunar24/core/drone_bank.h" "$BANK"
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

measure() {  # render + full-matrix analyze. The analyzer enforces 84/84 coverage and fails fast.
  render_only "$WORK/probe-out"
  if ! python3 "$ANALYZE" --dir "$WORK/probe-out" --manifest "$MANIFEST" \
       > "$WORK/analyze.tsv" 2>"$WORK/analyze.err"; then
    echo "   ERROR: analyzer failed (not a valid RED — aborting)." >&2
    tail -5 "$WORK/analyze.err" >&2; exit 1
  fi
}

run_gate() {  # $@ = extra gate flags. Sets GRC. Exits 0 either way (RED is expected here).
  set +e
  python3 "$GATE" --baseline "$BASE" --current "$WORK/analyze.tsv" "${GATE_ARGS[@]}" "$@" \
    > "$WORK/gate.txt" 2>&1
  GRC=$?
  set -e
}

# A named-rule assertion helper: the gate must have exited with the EXPECTED code AND its
# output must name the rule. rc=1 is a JUDGEMENT red (a cell missed its threshold); rc=2 is a
# STRUCTURAL red (the input could not be parsed). A structural red means the mutation broke the
# measurement rather than the DSP, which is not the discrimination the control claims — so the
# two are never interchangeable.
expect_red() {  # $1 = control name, $2 = required substring, $3 = expected rc (default 1)
  local name="$1" needle="$2" want="${3:-1}"
  if [ "$GRC" -eq 0 ]; then
    echo "   FAIL: $name stayed GREEN (rc=0) — this control does not discriminate." >&2
    sed -n '1,40p' "$WORK/gate.txt" >&2
    return 1
  fi
  if [ "$GRC" != "$want" ]; then
    echo "   FAIL: $name exited $GRC, expected $want (1=judgement, 2=structural)." >&2
    sed -n '1,40p' "$WORK/gate.txt" >&2
    return 1
  fi
  if ! grep -qF "$needle" "$WORK/gate.txt"; then
    echo "   FAIL: $name exited $GRC but did not name the rule: $needle" >&2
    sed -n '1,40p' "$WORK/gate.txt" >&2
    return 1
  fi
  echo "   OK: $name rc=$GRC, named rule present: $needle"
  return 0
}

# The gate emits a per-cell table whose last column is the bare verdict GREEN or RED — there is
# no `verdict=` token, and the summary lines are `FAIL:` / `PASS: all 36 ...`.
red_count() { grep -cE 'RED$' "$WORK/gate.txt" 2>/dev/null || true; }
green_count() { grep -cE 'GREEN$' "$WORK/gate.txt" 2>/dev/null || true; }

# Signal-only render equivalence. A render directory also holds gh19_cpu.tsv -- timer telemetry
# that differs between two runs of the SAME source -- so a whole-directory `diff -r` reports a
# difference no matter what: it compares timing as if it were signal, and can never pass. Compare
# the `*.raw` render products only, failing closed on a member missing from either side.
raws_equal() {  # $1, $2 = render dirs. 0 iff the *.raw sets are byte-identical.
  local a="$1" b="$2" f n=0
  for f in "$a"/*.raw; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    cmp -s "$f" "$b/$(basename "$f")" || return 1
  done
  [ "$n" -gt 0 ] || return 1
  for f in "$b"/*.raw; do
    [ -f "$f" ] || continue
    [ -f "$a/$(basename "$f")" ] || return 1
  done
  return 0
}

# Cells a control left GREEN. A control that discriminates on only some cells is a PARTIAL
# discriminator; naming the survivors is what makes the count auditable later.
green_cells() { grep -E 'GREEN$' "$WORK/gate.txt" 2>/dev/null | awk '{print $1}' | tr '\n' ' ' || true; }

# The per-role EXTREME gains this arm achieved, read off the gate table's dB_gain column (field 7;
# the table is `cell role sr dt naive current dB_gain verdict`). This is the other half of the
# threshold discipline: the threshold must sit ABOVE what a control can reach, or that control
# stops discriminating -- and a bare count cannot show that.
role_gains() {  # $1 = role label as printed in the table
  awk -v want="$1" '$2 == want && $NF ~ /^(GREEN|RED)$/ {
      g = $7 + 0
      if (!seen) { max = g; min = g; seen = 1 }
      if (g > max) max = g
      if (g < min) min = g
    }
    END { if (seen) printf "%s best %+.2f / worst %+.2f dB", want, max, min }' "$WORK/gate.txt"
}

# One control's evidence block: the gate's own verdict counts, the extremes it reached, and the
# names of any cells it failed to flip.
report_arm() {  # $1 = arm label
  local gc
  gc="$(green_cells)"
  echo "   red=$(red_count) of 36 classic cells; $(role_gains LOW) | $(role_gains HIGH)"
  [ -z "$gc" ] || echo "   NOTE: $(green_count) cell(s) stayed GREEN under $1: $gc"
}

echo "=== GH#19 S2 classic-saw polyBLEP mutation runner ==="
echo "tree: $TREE"
echo "rev for [A2]: $PRE_S2_REV"
echo

########################################################################################
# [A] POSITIVE — the committed (corrected) source must be GREEN on the acceptance gate.
########################################################################################
echo "[A] positive: committed corrected source must PASS the acceptance gate"
restore_source
check_anchor
build_probe
measure
run_gate
if [ "$GRC" -ne 0 ]; then
  echo "   FAIL: the committed source is RED (rc=$GRC). Fix the tree before trusting any control." >&2
  sed -n '1,60p' "$WORK/gate.txt" >&2
  exit 1
fi
echo "   OK: rc=0, $(green_count) cell(s) GREEN"
grep -E 'role: passed=|weakest gain' "$WORK/gate.txt" || true
cp "$WORK/gate.txt" "$WORK/gate-fixed.txt"
cp "$WORK/analyze.tsv" "$WORK/analyze-fixed.tsv"
echo

########################################################################################
# [A2] EQUIVALENCE MEASUREMENT — nc1 must BE the pre-S2 source, not merely resemble it.
# Build a tree whose drone_bank.h is the LITERAL base revision, render, and byte-compare
# every raw render against nc1's. This MEASURES the equivalence instead of asserting it.
########################################################################################
echo "[A2] literal pre-S2 source (git $PRE_S2_REV) vs the nc1 mutation"
restore_source
if git -C "$ROOT" cat-file -e "$PRE_S2_REV:core/include/lunar24/core/drone_bank.h" 2>/dev/null; then
  git -C "$ROOT" show "$PRE_S2_REV:core/include/lunar24/core/drone_bank.h" > "$BANK"
  build_probe
  render_only "$WORK/pre-s2-out"
  NAIVE_RAWS="$WORK/pre-s2-out"
  echo "   rendered the literal pre-S2 source into $NAIVE_RAWS"
else
  echo "   NOTE: rev $PRE_S2_REV not available in this clone; [A2] equivalence not measured."
fi
echo

########################################################################################
# [B] NEGATIVE CONTROLS — each a real production-source mutation. All MUST be RED.
########################################################################################
echo "[B] negative controls"
NC_FAILED=0
EQ_OK=0
NC1_RED=0
NC2_RED=0
NC3_RED=0

# nc1: the pre-S2 call site restored. Zero correction => zero gain => every cell fails the
# gain rule. Its render must ALSO be byte-identical to the literal pre-S2 source ([A2]).
echo
echo "[B/nc1] pre_s2_source — correction removed at the call site AND the include dropped"
restore_source
py_splice "$FIXED_BLOCK" "$NC1_BLOCK" 1
py_splice "$INCLUDE_AND_NL" "" 1
build_probe
measure
if [ -n "$NAIVE_RAWS" ]; then
  if raws_equal "$NAIVE_RAWS" "$WORK/probe-out"; then
    echo "   OK: nc1 *.raw renders are BYTE-IDENTICAL to the literal pre-S2 source ([A2] measured)."
    EQ_OK=1
  else
    echo "   FAIL: nc1 *.raw renders differ from the literal pre-S2 source — nc1 is not what it claims." >&2
    NC_FAILED=1
  fi
fi
run_gate
# nc1's signature is printed by the gate itself: the zero-delta report is a REPORTED property,
# so it is the control's named evidence rather than something the runner infers.
expect_red "nc1_pre_s2_source" "exact-zero deltas (correction absent): YES" || NC_FAILED=1
NC1_RED="$(red_count)"
report_arm nc1
if ! grep -qF "gain 0.00 dB < " "$WORK/gate.txt"; then
  echo "   NOTE: nc1 did not produce a literal 'gain 0.00 dB' line — check the delta wording."
fi

# nc2: the residual ADDED instead of subtracted => the jump is doubled, not removed.
echo
echo "[B/nc2] residual_sign_flipped — saw(t) + R instead of saw(t) - R"
restore_source
py_splice "$FIXED_BLOCK" "$NC2_BLOCK" 1
build_probe
measure
run_gate
expect_red "nc2_residual_sign_flipped" "dB required" || NC_FAILED=1
NC2_RED="$(red_count)"
report_arm nc2

# nc3: the WRONG `dt` — the envelope time step at :258 instead of this sample's phase
# increment. The window is ~100x too narrow, so the correction is nearly a no-op.
echo
echo "[B/nc3] window_dt_is_envelope_step — polyblepSaw(t, dt) with dt = 1.0/sampleRate_ (:258)"
restore_source
py_splice "$FIXED_BLOCK" "$NC3_BLOCK" 1
build_probe
measure
run_gate
expect_red "nc3_window_dt_is_envelope_step" "dB required" || NC_FAILED=1
NC3_RED="$(red_count)"
report_arm nc3

# nc4: correct AFTER the cubic instead of on the saw. The cubic compresses the unit jump to
# 2/3, so the same residual over-corrects by a measured 1.500x. Whether this still clears the
# threshold is an empirical question and is REPORTED either way rather than assumed.
echo
echo "[B/nc4] residual_after_cubic — nonlinearity(saw) - R (over-corrects by 1.500x)"
restore_source
py_splice "$FIXED_BLOCK" "$NC4_BLOCK" 1
build_probe
measure
run_gate
if [ "$GRC" -eq 0 ]; then
  echo "   MEASURED: nc4 stayed GREEN — the 1.5x over-correction still clears the threshold."
  echo "             Recorded as a sensitivity measurement, NOT counted as a discriminator."
  report_arm nc4
  cp "$WORK/analyze.tsv" "$WORK/analyze-nc4.tsv"
else
  expect_red "nc4_residual_after_cubic" "dB required" || NC_FAILED=1
  report_arm nc4
fi
NC4_RED="$(red_count)"
echo

echo "=== runner summary ==="
if [ "$EQ_OK" -eq 1 ]; then
  EQWORD="yes"
else
  EQWORD="NO — equivalence was not established for this run"
fi
echo "  [A]  positive          : GREEN 36/36 (rc=0)"
echo "  nc1  pre-S2 source     : RED ${NC1_RED}/36 on a named rule; *.raw-identical to pre-S2: $EQWORD"
echo "  nc2  residual flipped  : RED ${NC2_RED}/36 on a named rule"
echo "  nc3  window dt wrong   : RED ${NC3_RED}/36 on a named rule"
# nc4 has a legitimate GREEN branch above (its 1.5x over-correction may still clear the
# threshold). So every line that mentions nc4 must follow what the arm ACTUALLY did: a PASS
# that asserts "nc4 RED" when nc4 measured green would be the same class of mislabel the gate
# :578 fix addressed. nc1/nc2/nc3 are the controls that must discriminate; nc4 is measured.
if [ "$NC4_RED" -eq 0 ]; then
  NC4WORD="stayed GREEN — recorded sensitivity, NOT a discriminator"
else
  NC4WORD="RED ${NC4_RED}/36 on a named rule"
fi
echo "  nc4  residual post-cubic: ${NC4WORD}"
if [ "$NC_FAILED" -ne 0 ]; then
  echo "RESULT: FAIL — a negative control did not discriminate, or nc1 was not the pre-S2 source."
  exit 1
fi
echo "RESULT: PASS — fixed arm GREEN 36/36; nc1 RED ${NC1_RED}/36 (*.raw-identical to pre-S2: $EQWORD);"
echo "               nc2 RED ${NC2_RED}/36; nc3 RED ${NC3_RED}/36; nc4 ${NC4WORD}."
