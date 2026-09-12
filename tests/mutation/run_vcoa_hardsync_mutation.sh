#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_vcoa_hardsync_mutation.sh — repeatable DISCRIMINATOR + NEGATIVE-CONTROL tool for the
# task #111 (GH #19, slice S5) VCO-A HARD-SYNC reset-discontinuity corrector.
# The shape follows the sibling runner tests/mutation/run_classic_saw_polyblep_mutation.sh (S2).
#
# ---------------------------------------------------------------------------------
# WHAT S5 ADDS. `vco_a.sync_in` is a registered, patchable gate jack with no consumer before
# task #111. S5 wires it: the runtime resolves the jack, interprets it with the SAME
# `sink_gate_interpret` the sequencer's EXT.CLOCK consumer uses (threshold/hysteresis from the
# jack's own descriptor), and on a RISING edge calls `Vco::requestSync()`. tick() then applies
# the reset AFTER its own advance, so the reset sample itself reads phase 0 — the value
# discontinuity and the new cycle start coincide on one sample — and band-limits that
# discontinuity with `*out -= 0.5 * jmp` (one half of the jump, the causal part of
# `b(n) = 1/2 + Si(pi*n)/pi`).
#
# ---------------------------------------------------------------------------------
# WHY the shape. Three things are load-bearing, and each gets its own control:
#
#   * the consumer EXISTS and is reached (`nc1`: unwire it),
#   * the SIGN of the jump term (`nc2`: `+= 0.5*jmp` doubles the jump instead of removing it),
#   * the SCALE of the jump term (`nc3`: `-= jmp` is the full-scale kernel, i.e. the
#     "copied the S2 kernel" error this slice is most likely to produce — S2's
#     `polyblepSaw`-style subtraction is full-scale by construction).
#
# A fourth arm uses the DEFERRED reset but drops only the band-limiting term. It matters
# because nc1 and it produce the SAME signal for a different reason: nc1 changes the
# measurement premise (no reset => the rendered cell is not M-periodic => the analyzer fails
# closed and the gate goes STRUCTURAL), while the deferred-reset-only arm keeps the premise
# and is therefore a JUDGEMENT red. Those two exit codes are never interchangeable, so both
# are asserted with their own expected rc and their own named rule.
#
# ---------------------------------------------------------------------------------
# [A2] / EQUIVALENCE. The plan requires nc1 to be byte-identical to the LITERAL pre-change
# source, so that "equivalent to pre-change" is MEASURED rather than asserted. This runner
# builds the pre-change source itself (git show at PRE_S5_REV for all four touched files,
# probe included — the old probe has no captureSync() and therefore renders 84 cells, not 96)
# and compares the two renders BY CELL ID. A whole-directory `diff -r` cannot do this job for
# two reasons: the raw filenames are assigned by REGISTRATION ORDER (`gh19_scnNNN.raw`,
# probe :427), and S5's 12 new cells are registered mid-file, so every later cell's filename
# shifts; and a render dir also holds `gh19_cpu.tsv`, which is timer telemetry that differs
# between two runs of the SAME source. So the comparison is id -> raw filename per directory,
# and only `*.raw` bytes.
#
# ISOLATION: the whole build runs in an INDEPENDENT TEMP SOURCE TREE (TREE) built into a
# separate TEMP BUILD DIR (WORK). The working-tree sources are NEVER mutated or restored, so a
# failed run cannot leave the tracked tree dirty.
#
# Each NEGATIVE CONTROL is an ACTUAL production-source mutation applied to the TEMP tree only.
# For each, the REAL product probe must COMPILE and EXIT 0 (a compile failure / abnormal exit
# is NOT a valid "RED" and aborts), then the acceptance gate must exit non-zero on a SPECIFIC,
# NAMED rule. Every negative control starts from the passing committed source, so each is a
# genuine passing->RED deflection.
#
# NOT REGISTERED IN CMAKE, deliberately: this is a dev-run artifact, exactly like the S2
# runner (no `*_mutation.sh` is registered anywhere in CMakeLists.txt), so it adds no CI cost.
#
# Usage:  bash tests/mutation/run_vcoa_hardsync_mutation.sh
#         PRE_S5_REV=<rev> bash tests/mutation/run_vcoa_hardsync_mutation.sh   # override [A2]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

TREE="$(mktemp -d "$(mktemp -d)/vcoa-hardsync-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/vcoa-hardsync-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

# Every local build dir a developer may have is excluded -- build-san included (the ASan+UBSan
# leg's dir). The runner configures its OWN $WORK, so copying them is pure waste.
rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude 'build-rel' --exclude 'build-dbg' \
  --exclude 'build-debug' --exclude 'build-san' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

VCO="$TREE/core/include/lunar24/core/vco.h"
RT="$TREE/core/include/lunar24/core/machine_runtime.h"
ANALYZE="$TREE/tools/gh19_alias_analyze.py"
GATE="$TREE/tools/check_gh19_hardsync_acceptance.py"
BASE="$TREE/tools/gh19_hardsync_naive_baseline.tsv"
MANIFEST="$TREE/tools/gh19_manifest.tsv"

# The four files S5 touches. [A2] restores ALL of them from PRE_S5_REV: the probe is as
# load-bearing as the DSP here, because the pre-change probe has no captureSync() and so does
# not render the 12 sync cells at all.
PRE_S5_REV="${PRE_S5_REV:-6f4481ae31af4f11b355c0b880cc2f802225bb7d}"
S5_FILES=(
  "core/include/lunar24/core/vco.h"
  "core/include/lunar24/core/machine_runtime.h"
  "core/include/lunar24/core/machine_definition.h"
  "tests/probes/gh19_alias_probe.cpp"
)

# Per-role acceptance thresholds. These DEFAULTS are load-bearing: a bare run must reproduce the
# APPROVED configuration rather than some other one (the S2 lesson: empty defaults once fell
# through to a wrong threshold and the positive control came back RED, i.e. a FALSE FAIL for
# anyone running it bare). Values = the ones set for S5 AFTER measuring the fixed arm (the S2
# precedent @Kimi 612691ea: the thresholds follow the measurement, they do not precede it):
# measured gains +9.90..+10.37 (220/440) and +12.32..+16.43 (880), weakest anchor gap 13.52 dB.
# So the defaults sit 0.90 / 0.82 dB under the weakest cell of each bin, matching S2's ~0.85 dB
# margin discipline. Changing a threshold requires new evidence, not an edit here.
MIN_GAIN_DB="${MIN_GAIN_DB:-9.0}"
MIN_GAIN_HIGH_DB="${MIN_GAIN_HIGH_DB:-11.5}"
MIN_GAP_DB="${MIN_GAP_DB:-10.0}"
MAX_PER_DEV="${MAX_PER_DEV:-1e-9}"

GATE_ARGS=(--min-gain-db "$MIN_GAIN_DB" --min-gain-high-db "$MIN_GAIN_HIGH_DB"
           --min-gap-db "$MIN_GAP_DB" --max-period-dev "$MAX_PER_DEV")

[ -f "$BASE" ] || { echo "ERROR: missing $BASE — regenerate the 12-cell S5 naive baseline first." >&2; exit 1; }

cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1

py_splice() {  # edit a TEMP-TREE source file. `need` = exact anchor count (fail-closed).
  local file="$1" find="$2" repl="$3" need="$4"
  python3 - "$file" "$find" "$repl" "$need" <<'PY'
import sys
dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(dst, encoding="utf-8").read()
if t.count(find) != need:
    sys.stderr.write(f"ERROR: anchor count {t.count(find)} != expected {need} in {dst}: {find[:70]!r}\n")
    sys.exit(2)
open(dst, "w", encoding="utf-8").write(t.replace(find, repl))
PY
}

# ---- The integration text, exactly as the implementation writes it. ------------------------
# Every constant below is spliced as a WHOLE block so a control never leaves a variable
# declared-but-unused: under -Werror that fails the build and the runner would abort with
# "not a valid RED" for a reason that has nothing to do with the mutation under test.

# The runtime consumer (machine_runtime.h, kVcoA slot). Removing it is nc1.
CONSUMER_BLOCK='        double sv = 0.0;
        if (syncInBoundA_ && resolveControlSink_(syncInA_, sv, driveGraph)) {
          const JackDescriptor* ds = findJackDescriptor_(syncInA_);
          if (ds != nullptr &&
              sink_gate_interpret(*ds, syncLatchA_, sv).edge == GateEdge::rising) {
            vcA_.requestSync();
          }
        }
'

# The jump term (vco.h tick()). The FIXED form, then the three mutations.
JMP_FIXED='  if (synced) *out -= 0.5 * jmp;'
JMP_NC2='  if (synced) *out += 0.5 * jmp;'
JMP_NC3='  if (synced) *out -= jmp;'
JMP_NC4='  if (synced) { (void)jmp; }'

# A control is only meaningful if each anchor is present exactly once; if the integration text
# and these constants ever drift apart, that must fail loudly rather than splice nothing.
check_anchor() {
  local n1 n2
  read -r n1 n2 <<<"$(python3 - "$RT" "$CONSUMER_BLOCK" "$VCO" "$JMP_FIXED" <<'PY'
import sys
print(open(sys.argv[1], encoding="utf-8").read().count(sys.argv[2]),
      open(sys.argv[3], encoding="utf-8").read().count(sys.argv[4]))
PY
)"
  if [ "$n1" != "1" ]; then
    echo "ERROR: the sync consumer block appears $n1 time(s) in machine_runtime.h, expected 1." >&2
    echo "       The runner's CONSUMER_BLOCK and the implementation have drifted apart." >&2
    exit 1
  fi
  if [ "$n2" != "1" ]; then
    echo "ERROR: '$JMP_FIXED' appears $n2 time(s) in vco.h, expected 1." >&2
    echo "       The runner's JMP_FIXED and the implementation have drifted apart." >&2
    exit 1
  fi
}

restore_source() {  # put the COMMITTED sources back, so every control starts from green.
  local f
  for f in "${S5_FILES[@]}"; do cp "$ROOT/$f" "$TREE/$f"; done
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

measure() {  # render + full-matrix analyze. Any analyzer failure ABORTS.
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
# output must name the rule. rc=1 is a JUDGEMENT red (a cell missed a criterion -- measured and
# failed); rc=2 is a STRUCTURAL red (a required cell had no criterion at all, i.e. the
# measurement premise itself did not hold). They are different claims about different things,
# so the two are never interchangeable and each arm states which one it must produce.
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

# The gate's per-cell table ends in a bare verdict column GREEN or RED — there is no
# `verdict=` token — and its summary line is `PASS: all 12 hard-sync cells ...`.
red_count() { grep -cE 'RED$' "$WORK/gate.txt" 2>/dev/null || true; }
green_count() { grep -cE 'GREEN$' "$WORK/gate.txt" 2>/dev/null || true; }
green_cells() { grep -E 'GREEN$' "$WORK/gate.txt" 2>/dev/null | awk '{print $1}' | tr '\n' ' ' || true; }

# The EXTREME dB gains this arm reached, read off the gate table
# (`cell sr f0 naive current dB_gain gap per rule verdict` => field 6 is dB_gain). This is the
# other half of the threshold discipline: a threshold must sit ABOVE what a control can reach,
# or that control stops discriminating -- and a bare count cannot show that.
gain_extremes() {
  awk '$NF ~ /^(GREEN|RED)$/ {
      g = $6 + 0
      if (!seen) { max = g; min = g; seen = 1 }
      if (g > max) max = g
      if (g < min) min = g
    }
    END { if (seen) printf "best %+.2f / worst %+.2f dB", max, min; else printf "n/a" }' \
    "$WORK/gate.txt"
}

# One control's evidence block: the gate's own verdict counts, the extremes it reached, and the
# names of any cells it failed to flip.
report_arm() {  # $1 = arm label
  local gc
  gc="$(green_cells)"
  echo "   red=$(red_count) of 12 sync cells; gain $(gain_extremes)"
  [ -z "$gc" ] || echo "   NOTE: $(green_count) cell(s) stayed GREEN under $1: $gc"
}

# Signal-only, ID-KEYED render equivalence between two render dirs that may not share a cell
# set (the pre-change render has 84 cells; S5 renders 96). Compares, for every id present in
# BOTH dirs, that id's *.raw bytes are identical; reports the three counts. Fails closed if a
# row is duplicated, if the scenario TSV lacks id/raw, or if a mapped file is missing.
raws_equal_by_id() {  # $1, $2 = render dirs. 0 iff every SHARED id matches byte-for-byte.
  python3 - "$1" "$2" <<'PY'
import os
import sys

def load(d):
    p = os.path.join(d, "gh19_scenarios.tsv")
    out = {}
    with open(p, encoding="utf-8") as fh:
        hdr = fh.readline().rstrip("\n").split("\t")
        if "id" not in hdr or "raw" not in hdr:
            sys.exit(f"ERROR: {p} lacks id/raw columns")
        i, r = hdr.index("id"), hdr.index("raw")
        for ln in fh:
            c = ln.rstrip("\n").split("\t")
            if len(c) <= max(i, r) or not c[i].strip():
                continue
            cid = c[i].strip()
            if cid in out:
                sys.exit(f"ERROR: duplicate id {cid} in {p}")
            out[cid] = c[r].strip()
    if not out:
        sys.exit(f"ERROR: no cells in {p}")
    return out

a, b = load(sys.argv[1]), load(sys.argv[2])
shared = sorted(set(a) & set(b))
differ, missing = [], []
for cid in shared:
    fa = os.path.join(sys.argv[1], a[cid])
    fb = os.path.join(sys.argv[2], b[cid])
    if not (os.path.isfile(fa) and os.path.isfile(fb)):
        missing.append(cid)
        continue
    with open(fa, "rb") as x, open(fb, "rb") as y:
        if x.read() != y.read():
            differ.append(cid)
print("shared=%d a_only=%d b_only=%d differ=%d" %
      (len(shared), len(set(a) - set(b)), len(set(b) - set(a)), len(differ) + len(missing)))
if missing:
    print("MISSING-FILE: " + " ".join(missing[:8]))
if differ:
    print("DIFFER: " + " ".join(differ[:8]))
sys.exit(1 if (differ or missing) else 0)
PY
}

# One shared-id equivalence check: its counts, plus an assertion on how many ids the two dirs
# are expected to share. The expected counts are MEASURED on the record, not inferred.
ids_equivalent() {  # $1=A dir, $2=B dir, $3=expected shared count, $4=label
  local out rc
  set +e
  out="$(raws_equal_by_id "$1" "$2")"; rc=$?
  set -e
  echo "   $out"
  if [ "$rc" -ne 0 ]; then
    echo "   FAIL: $4 — the shared ids are NOT byte-identical." >&2
    return 1
  fi
  local shared
  shared="$(printf '%s' "$out" | sed -n '1s/.*shared=\([0-9]*\).*/\1/p')"
  if [ "$shared" != "$3" ]; then
    echo "   FAIL: $4 — shared id count $shared, expected $3. The cell sets have drifted." >&2
    return 1
  fi
  echo "   OK: $4 — all $shared shared ids are BYTE-IDENTICAL."
  return 0
}

echo "=== GH#19 S5 VCO-A hard-sync mutation runner ==="
echo "tree: $TREE"
echo "rev for [A2]: $PRE_S5_REV"
echo "thresholds: gain>=${MIN_GAIN_DB} dB (880: ${MIN_GAIN_HIGH_DB}), gap>=${MIN_GAP_DB} dB, per<=${MAX_PER_DEV}"
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
grep -E 'Bins \(pinned\)|exact-zero deltas' "$WORK/gate.txt" || true
cp "$WORK/gate.txt" "$WORK/gate-fixed.txt"
cp "$WORK/analyze.tsv" "$WORK/analyze-fixed.tsv"
cp "$WORK/probe-out/gh19_scenarios.tsv" "$WORK/scn-fixed.tsv"
echo

########################################################################################
# [A2] REGRESSION LOCK, MADE EXECUTABLE — the literal PRE-S5 source (all four touched files,
# probe included) must reproduce the 84 pre-existing cells byte-for-byte while the 12 new sync
# cells are absent from it. This is the Stage A "unwired => 84 cells byte-identical" lock.
########################################################################################
echo "[A2] literal pre-S5 source (git $PRE_S5_REV) vs the committed source"
PRE_RENDERED=0
if git -C "$ROOT" cat-file -e "$PRE_S5_REV:core/include/lunar24/core/vco.h" 2>/dev/null; then
  for f in "${S5_FILES[@]}"; do git -C "$ROOT" show "$PRE_S5_REV:$f" > "$TREE/$f"; done
  build_probe
  render_only "$WORK/pre-s5-out"
  PRE_RENDERED=1
  echo "   rendered the literal pre-S5 source into $WORK/pre-s5-out"
else
  echo "   NOTE: rev $PRE_S5_REV not available in this clone; [A2] equivalence NOT measured." >&2
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
NC4_RED=0

# nc1: the consumer removed. The jack is still patched (the probe's cable and the binding are
# untouched), so the slave free-runs at its own f0 -- which is NOT M-periodic. The analyzer
# therefore fails closed (no criterion) and the gate must go STRUCTURAL (rc=2), NOT judgement.
# Its 84 pre-existing cells must also be byte-identical to the literal pre-S5 render, which is
# how "removing the consumer changes nothing else" is measured rather than assumed.
echo
echo "[B/nc1] consumer_removed — vcA_.requestSync() call site unwired"
restore_source
py_splice "$RT" "$CONSUMER_BLOCK" "" 1
build_probe
measure
if [ "$PRE_RENDERED" -eq 1 ]; then
  if raws_equal_by_id "$WORK/pre-s5-out" "$WORK/probe-out" > "$WORK/eq-nc1.txt" 2>&1; then
    echo "   $(cat "$WORK/eq-nc1.txt" | tr '\n' ' ')"
    echo "   OK: nc1's shared cells are BYTE-IDENTICAL to the literal pre-S5 source ([A2] measured)."
    EQ_OK=1
  else
    echo "   FAIL: nc1's render differs from the literal pre-S5 source — nc1 is not what it claims." >&2
    cat "$WORK/eq-nc1.txt" >&2
    NC_FAILED=1
  fi
fi
run_gate
# nc1's signature is the STRUCTURAL code: the analyzer refused to answer, so the gate reports a
# required cell with no criterion instead of a cell that missed a threshold.
expect_red "nc1_consumer_removed" "NO criterion" 2 || NC_FAILED=1
NC1_RED="$(red_count)"
report_arm nc1

# nc2: the jump term ADDED instead of subtracted => the discontinuity is doubled, not removed.
echo
echo "[B/nc2] jump_sign_flipped — out += 0.5*jmp instead of -="
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC2" 1
build_probe
measure
run_gate
expect_red "nc2_jump_sign_flipped" "220/440:>=" || NC_FAILED=1
NC2_RED="$(red_count)"
report_arm nc2

# nc3: the full-scale kernel — `out -= jmp`. This is the "copied the S2 kernel" error: S2's
# polyblepSaw subtracts the FULL-scale residual because a saw wrap spans the whole range,
# whereas a hard-sync jump J is arbitrary and only its causal half belongs on this sample.
echo
echo "[B/nc3] full_scale_kernel — out -= jmp (the S2 kernel copied without the 1/2)"
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC3" 1
build_probe
measure
run_gate
expect_red "nc3_full_scale_kernel" "220/440:>=" || NC_FAILED=1
NC3_RED="$(red_count)"
report_arm nc3

# nc4: the DEFERRED reset KEPT but the band-limiting term dropped. The premise still holds
# (the reset still happens every M samples => the cell is still M-periodic), so this arm must
# produce a REAL criterion and fail as a JUDGEMENT red (rc=1). Together with nc1 this separates
# the two failure modes the slice could hide behind.
echo
echo "[B/nc4] correction_absent — deferred reset kept, band-limited jump term dropped"
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC4" 1
build_probe
measure
run_gate
expect_red "nc4_correction_absent" "220/440:>=" 1 || NC_FAILED=1
NC4_RED="$(red_count)"
report_arm nc4
echo

echo "=== runner summary ==="
if [ "$EQ_OK" -eq 1 ]; then
  EQWORD="yes"
elif [ "$PRE_RENDERED" -eq 1 ]; then
  EQWORD="NO — the pre-S5 render was built and nc1 did NOT match it"
else
  EQWORD="not measured (rev unavailable)"
fi
echo "  [A]  positive           : GREEN 12/12 (rc=0)"
echo "  nc1  consumer removed   : RED ${NC1_RED}/12 as STRUCTURAL rc=2 (named: NO criterion);"
echo "                           84 shared cells *.raw-identical to pre-S5: $EQWORD"
echo "  nc2  jump sign flipped  : RED ${NC2_RED}/12 as judgement rc=1"
echo "  nc3  full-scale kernel  : RED ${NC3_RED}/12 as judgement rc=1"
echo "  nc4  correction absent  : RED ${NC4_RED}/12 as judgement rc=1"
if [ "$NC_FAILED" -ne 0 ]; then
  echo "RESULT: FAIL — a negative control did not discriminate, or nc1 was not the pre-S5 source."
  exit 1
fi
echo "RESULT: PASS — fixed arm GREEN 12/12; nc1 structural RED ${NC1_RED}/12 (pre-S5 equivalence: $EQWORD);"
echo "               nc2 RED ${NC2_RED}/12; nc3 RED ${NC3_RED}/12; nc4 RED ${NC4_RED}/12."
