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
# discontinuity with `*out -= 0.5 * jmp` (one half of the jump, on the post-reset sample).
#
# SCOPE OF THAT FORMULA: `-0.5*J` is a single-point MEDIAN correction at the reset sample, applied as
# the approximation this slice adopts — NOT the complete Si step residual `b(n) = 1/2 + Si(pi*n)/pi`,
# whose tail at the subsequent integer samples is not all zero and is left uncorrected here. The
# master's edge does land on a sample grid point in these cells (per-sample step 1/M, period M
# samples), but that alignment is NOT what removes the tail. The evidence on the record is the 12
# declared sync cells; nothing here extends to arbitrary f0, FM, or off-grid event phase, and this
# runner does NOT test that extension.
#
# ---------------------------------------------------------------------------------
# WHY the shape. Three things are load-bearing, and each gets its own control:
#
#   * the consumer EXISTS and is reached (`nc1`: unwire it),
#   * the SIGN of the jump term (`nc2`: `+= 0.5*jmp` doubles the jump instead of removing it),
#   * the SCALE of the jump term (`nc3`: `-= jmp` is the full-scale kernel, i.e. the
#     "copied the S2 kernel" error this slice is most likely to produce — S2's
#     `polyblepSaw`-style subtraction is full-scale by construction. ⚠️ Since 2026-09-13 the
#     PROBE refuses nc3 before it ever reaches judgement, so nc4 carries the live SCALE control;
#     see the nc3 note below for why, and for what nc3 witnesses instead).
#
# A fourth arm (nc4) uses the DEFERRED reset but drops only the band-limiting term. It matters
# because nc1 and it produce the SAME signal for a different reason: nc1 changes the
# measurement premise (no reset => the rendered cell is not M-periodic => the analyzer fails
# closed and the gate goes STRUCTURAL), while the deferred-reset-only arm keeps the premise
# and is therefore a JUDGEMENT red. Those two exit codes are never interchangeable, so both
# are asserted with their own expected rc and their own named rule.
#
# ⚠️ nc1 WAS RE-REGISTERED 2026-09-13, and the reason is worth keeping. Items 2 and 4 of the
# seven-item package added probe-side reconciliations (the master-edge index, and the DeviceState
# cable check) that fire on the SAME mutation, so nc1 is now refused by the PROBE first
# (measured PROBE_RC=65, two named reasons) and only then reaches the gate, which still returns
# STRUCTURAL rc=2 `NO criterion`. The arm therefore asserts BOTH surfaces, each with its own
# name. What made the old registration wrong was not the verdict but the ROUTE: `measure` aborts
# on any non-zero probe exit, so the arm could no longer run at all -- and an arm that cannot run
# is not a weaker check, it is an unreachable one, which any later reader would have taken for a
# pass. That is why `measure_keeps_render` exists and why its tolerance is bounded by a
# completeness check rather than left open.
#
# WHICH SURFACE OWNS A RED. There are two, and every arm must say which one it expects:
#   * the PROBE's substitution guard (`peak > hi`, hi = 0.55 for these audio-domain cells --
#     `gh19_alias_probe.cpp:161,198`) refuses the cell before any criterion exists. That is a
#     VALIDITY red, owned by the probe, asserted by expect_failclosed;
#   * the acceptance GATE then either finds a criterion and the cell misses it (JUDGEMENT red,
#     rc=1, expect_red) or finds no criterion at all (STRUCTURAL, rc=2).
# Conflating the first with the second would let "the signal was never valid" masquerade as
# "the gate discriminated", so the two are asserted separately and an abort is never a pass.
#
# nc2 belongs to the FIRST kind, and that is a MEASURED correction to this runner's original
# design rather than a preference. What decides it is the deviation from the correct kernel:
# nc2 applies `+0.5*jmp` where the correct term is `-0.5*jmp`, so it deviates by 1.0*jmp --
# TWICE the deviation of simply omitting the term (which is nc4 and is in range). That is
# enough to leave the peak guard, so the probe refuses all 12 sync cells and the gate never
# runs on them. An earlier draft predicted a judgement rc=1 here; the first real run
# (2026-09-12) falsified that. nc4 is the load-bearing judgement control.
#
# nc3 has the SAME 0.5*jmp magnitude as nc4 but the OPPOSITE sign, and an earlier draft expected it
# to stay in range too. The second real run (2026-09-13) falsified THAT: items 2 and 4 added
# probe-side reconciliations, and they refuse nc3 as well -- PARTIALLY (9 of the 12 sync cells),
# through the item-4 DeviceState cable reconstruction. Its PER-CELL named reason is the item-2
# absolute-timing one ("the reset is 1 frame(s) LATE"), because `-= jmp` makes the reset sample
# read the PREVIOUS value, which moves the reconstructed discontinuity one frame later -- so a
# SCALE error and nc5's genuine one-frame LATCH produce the SAME reason string. The arm's
# pre-registered JUDGEMENT red is therefore WITHDRAWN as UNMEASURABLE rather than re-read, and nc3
# is re-registered as probe-validity + gate-structural. What it still witnesses, and what nc1
# (a FULL refusal) cannot, is that a PARTIAL refusal ESCALATES to rc=2 instead of downgrading to
# the judgement code. Both expectations are pre-registered below, before this revision is run, so
# a further surprise is a finding rather than a rewrite.
#
# A FIFTH arm (task #111 item 3) defers the reset by exactly ONE frame. It is the one control
# that no dB figure could ever supply, because every figure this gate compares is computed over
# an M-periodic signal and is therefore invariant under shifting the same samples by one cell
# (@Codex 908f36e7 reproduced exactly that: shifting the periodic samples by one whole cell left
# the analyzer's residual, gap and period-deviation bit-for-bit unchanged). Only an index
# anchored OUTSIDE the signal -- the master's own published output, read per frame through the
# runtime's `controlVoltageAt` -- can see the difference, which is why item 2 added that
# reconciliation ahead of it. nc5 is pre-registered as a PROBE-owned validity red (the deferral
# voids the premise every cell rests on, so the probe refuses fail-closed and names the timing
# criterion), matching nc2's shape rather than extending the gate.
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
# The completeness bound `measure_keeps_render` asserts against: EVERY required id must have a row
# in the analyzer's table. Derived from the manifest, never hardcoded.
#
# This is deliberately a SET property rather than a row count, and the difference was measured, not
# assumed: the analyzer's table for a 96-cell render holds 107 data rows -- 96 cell rows plus 11
# `MACHINE` summary rows -- and it emits NO row at all for the two `required=0` ids
# (`vco_sub`, `vco_saw_pulse_morph`). A count equality would therefore have to encode two
# producer-side accidents at once, and would still not catch the failure that matters: a table of
# the right LENGTH that is missing the very row the gate is about to judge.
MANIFEST_REQUIRED="$(awk -F'\t' 'NR>1 && $0 !~ /^#/ && NF && $8=="1" {print $1}' "$MANIFEST")"

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

# nc5 (task #111 item 3): the SAME consumer, the SAME cable and the SAME edge detection — only the
# FRAME on which the reset is applied moves, by exactly ONE. This is the mutation the whole item-2
# criterion exists for: no residual-based figure can see it (a criterion computed over a periodic
# signal is invariant under a global sample shift, @Codex 908f36e7's "shift the same samples by one
# cell -> identical answer"), so if this arm is not RED then the gate is blind to a one-frame
# deferral no matter how many other arms pass.
CONSUMER_BLOCK_DEFER='        double sv = 0.0;
        if (deferSyncA_) { vcA_.requestSync(); deferSyncA_ = false; }
        if (syncInBoundA_ && resolveControlSink_(syncInA_, sv, driveGraph)) {
          const JackDescriptor* ds = findJackDescriptor_(syncInA_);
          if (ds != nullptr &&
              sink_gate_interpret(*ds, syncLatchA_, sv).edge == GateEdge::rising) {
            deferSyncA_ = true;
          }
        }
'
# The one-frame latch the deferral needs. Spliced as a whole declaration so the mutant cannot
# fail the build on an unused member under -Werror and be misread as "not a valid RED".
SYNC_MEMBER_ANCHOR='  JackId syncInA_{0};         bool syncInBoundA_ = false;'
SYNC_MEMBER_DEFER='  JackId syncInA_{0};         bool syncInBoundA_ = false;
  bool deferSyncA_ = false;'

# The jump term (vco.h tick()). The FIXED form, then the three mutations.
JMP_FIXED='  if (synced) *out -= 0.5 * jmp;'
JMP_NC2='  if (synced) *out += 0.5 * jmp;'
JMP_NC3='  if (synced) *out -= jmp;'
JMP_NC4='  if (synced) { (void)jmp; }'

# A control is only meaningful if each anchor is present exactly once; if the integration text
# and these constants ever drift apart, that must fail loudly rather than splice nothing.
check_anchor() {
  local n1 n2 n3
  read -r n1 n2 n3 <<<"$(python3 - "$RT" "$CONSUMER_BLOCK" "$VCO" "$JMP_FIXED" "$SYNC_MEMBER_ANCHOR" <<'PY'
import sys
print(open(sys.argv[1], encoding="utf-8").read().count(sys.argv[2]),
      open(sys.argv[3], encoding="utf-8").read().count(sys.argv[4]),
      open(sys.argv[1], encoding="utf-8").read().count(sys.argv[5]))
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
  if [ "$n3" != "1" ]; then
    echo "ERROR: the sync-jack member declaration appears $n3 time(s) in machine_runtime.h, expected 1." >&2
    echo "       nc5's member splice would otherwise silently do nothing." >&2
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

# nc2 mutates the kernel far enough that the PROBE's own substitution guard can fire before
# the gate ever sees a cell. That is a real, informative outcome, but it is neither a
# judgement red nor a reason to abandon the controls that have not run yet -- so this variant
# records the probe's verdict and keeps the run alive. It deliberately does NOT fall back to
# running the gate: with no criteria produced there is nothing for the gate to judge, and
# running it anyway would let nc1's stale gate output be mistaken for this arm's result.
PROBE_RC=0
measure_may_fail_closed() {  # render; a fail-closed probe is RECORDED, not fatal.
  PROBE_RC=0
  rm -rf "$WORK/probe-out"; mkdir -p "$WORK/probe-out"
  set +e
  "$WORK/gh19_alias_probe" --out "$WORK/probe-out" >"$WORK/probe.log" 2>&1
  PROBE_RC=$?
  set -e
  if [ "$PROBE_RC" -ne 0 ]; then
    echo "   NOTE: the probe itself refused (exit $PROBE_RC) — recorded, not fatal."
    tail -3 "$WORK/probe.log" | sed 's/^/   | /'
    return 0
  fi
  if ! python3 "$ANALYZE" --dir "$WORK/probe-out" --manifest "$MANIFEST" \
       > "$WORK/analyze.tsv" 2>"$WORK/analyze.err"; then
    echo "   ERROR: analyzer failed (not a valid RED — aborting)." >&2
    tail -5 "$WORK/analyze.err" >&2; exit 1
  fi
}

# The third outcome, which nc1 needs and neither of the other two provide: the probe REFUSES
# (so `measure` would abort) yet its render is COMPLETE and reviewable — every cell it could not
# produce is recorded in gh19_scenarios.tsv with its own signal string, which is exactly the
# premise failure the gate's STRUCTURAL code exists to report. Neither "abort" nor "skip the
# analyzer" is honest here, so this variant records the probe's exit code and ALWAYS analyzes.
#
# It is not a way around the stale-artifact discipline that `require_criterion` enforces: the TSV
# this produces is written from THIS arm's own render, in THIS call, and the arm asserts the
# probe's named refusal AND reads the gate's output from THIS call's `run_gate`. Nothing here can
# read an earlier arm's verdict, because nothing here reads a file this call did not just write.
#
# The probe is not the only side that refuses here: the analyzer has its OWN coverage/validity gate
# and exits non-zero when a required cell has no producible signal, which is the same premise
# failure seen from the other end. That is the expected state for this arm, so a non-zero analyzer
# exit is recorded rather than fatal -- but ONLY alongside a COMPLETE table, so a genuinely broken
# apparatus cannot be laundered into "the gate went STRUCTURAL". The bound is stated in the function.
measure_keeps_render() {  # render (record PROBE_RC, never fatal) + ALWAYS full-matrix analyze.
                          # A non-zero ANALYZER exit is tolerated ONLY if the table it wrote is
                          # COMPLETE -- see the completeness check below.
  PROBE_RC=0
  AN_RC=0
  rm -rf "$WORK/probe-out"; mkdir -p "$WORK/probe-out"
  set +e
  "$WORK/gh19_alias_probe" --out "$WORK/probe-out" >"$WORK/probe.log" 2>&1
  PROBE_RC=$?
  set -e
  if [ "$PROBE_RC" -ne 0 ]; then
    echo "   NOTE: the probe refused (exit $PROBE_RC); its render is still analyzed, because"
    echo "         every cell it could not produce is recorded with its own named signal."
  fi
  rm -f "$WORK/analyze.tsv"
  set +e
  python3 "$ANALYZE" --dir "$WORK/probe-out" --manifest "$MANIFEST" \
    > "$WORK/analyze.tsv" 2>"$WORK/analyze.err"
  AN_RC=$?
  set -e
  # The analyzer exits non-zero for TWO different reasons, and only one of them is this arm's
  # subject. It exits 1 when its OWN coverage/validity gate finds a required cell unproducible --
  # that IS the premise failure this arm asserts, and the table it wrote is the evidence. It also
  # exits non-zero when the apparatus itself broke (unreadable manifest, crash mid-write). Both
  # would make the gate go STRUCTURAL, and asserting the STRUCTURAL code on a broken apparatus
  # would be a check that cannot fail for its stated reason. So the tolerance is bounded by ONE
  # thing: a table in which EVERY required manifest id has a row. A required id with no row is not
  # a premise failure to be reported -- it is the apparatus failing to account for a cell it is
  # supposed to judge, and this arm must not launder it into a STRUCTURAL red.
  _missing="$(awk -F'\t' '
      FNR==NR { if (FNR>1 && $0 !~ /^#/ && NF && $8=="1") req[$1]=1; next }
      FNR>1   { seen[$1]=1 }
      END     { for (i in req) if (!(i in seen)) printf "%s ", i }
    ' "$MANIFEST" "$WORK/analyze.tsv")"
  if [ -n "$_missing" ]; then
    echo "   ERROR: the analyzer's table is MISSING rows for required id(s):" >&2
    echo "          $_missing" >&2
    echo "          An incomplete table is not a valid RED (analyzer rc=$AN_RC, aborting)." >&2
    tail -5 "$WORK/analyze.err" >&2; exit 1
  fi
  if [ "$AN_RC" -ne 0 ]; then
    echo "   NOTE: the analyzer exited $AN_RC but accounted for every required id ($(wc -l < "$WORK/analyze.tsv") rows):"
    echo "         it is REPORTING the premise failure, which is the state this arm asserts."
    head -3 "$WORK/analyze.err" | sed 's/^/   | /'
  fi
}

run_gate() {  # [$1 = current TSV; default $WORK/analyze.tsv]  [$@:2 = extra gate flags].
              # Sets GRC. Exits 0 either way (RED is expected here).
  local cur="${1:-$WORK/analyze.tsv}"
  if [ ! -s "$cur" ]; then
    GRC=99
    printf 'no current TSV to judge: %s\n' "$cur" > "$WORK/gate.txt"
    return 0
  fi
  set +e
  python3 "$GATE" --baseline "$BASE" --current "$cur" "${GATE_ARGS[@]}" "${@:2}" \
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

# The probe-owned counterpart of expect_red. A mutated kernel violent enough to leave the peak
# guard is REFUSED before any criterion exists, so there is no rc to assert -- what must be
# asserted is that the refusal happened AND that it happened for the pre-registered reason.
# Accepting "the probe exited non-zero" alone would also accept a build problem or a crash.
expect_failclosed() {  # $1 = control name, $2 = pre-registered reason substring
  local name="$1" needle="$2"
  if [ "$PROBE_RC" -eq 0 ]; then
    echo "   FAIL: $name — the probe ACCEPTED this kernel (exit 0), so it is not the validity" >&2
    echo "         red pre-registered for this arm. Re-derive the expectation from measurement." >&2
    return 1
  fi
  if ! grep -qF "$needle" "$WORK/probe.log"; then
    echo "   FAIL: $name — the probe refused (exit $PROBE_RC) but NOT for the pre-registered" >&2
    echo "         reason '$needle'. Actual tail of the probe log:" >&2
    tail -3 "$WORK/probe.log" >&2
    return 1
  fi
  echo "   OK: $name — probe refused (exit $PROBE_RC), pre-registered reason: $needle"
  return 0
}

# A JUDGEMENT arm may only assert a gate verdict if the probe actually produced cells. Without
# this guard a refused probe would leave $WORK/gate.txt holding the PREVIOUS arm's output, and
# the following expect_red would "pass" by reading someone else's result — the same
# stale-artifact error this runner exists to prevent. A refusal here is a FAILED prediction
# (the arm was pre-registered as a judgement red), not a pass and not a silent skip.
require_criterion() {  # $1 = arm name. 0 iff the probe produced cells for the gate to judge.
  local name="$1"
  if [ "$PROBE_RC" -ne 0 ]; then
    echo "   FAIL: $name — the probe refused (exit $PROBE_RC), so no criterion exists and the" >&2
    echo "         judgement verdict pre-registered for this arm is UNMEASURABLE. Probe tail:" >&2
    tail -3 "$WORK/probe.log" >&2
    return 1
  fi
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

# ---------------------------------------------------------------------------------------------
# THE DEFAULT-BEHAVIOUR REGRESSION LOCK (GH#19 S5).
#
# S5 must change the DEFAULT (un-synced) behaviour in exactly one way -- it ADDS the declared sync
# cells -- and in no other way. `raws_equal_by_id` alone cannot lock that: it reports counts and
# exits 0 whenever the SHARED ids match, so a set that shrank on BOTH sides (a probe edit that
# stops rendering a cell on each side, or a manifests/probe divergence) passes silently, and the
# shared COUNT it prints is asserted nowhere. This lock closes both gaps by asserting the SET:
#
# The expectation is DERIVED from a manifest, never a literal here, and the rules are stated as
# inclusion against the DECLARATION rather than equality between the two renders:
#
#   * every id the manifest marks `required=1` MUST be produced. That is the same column the
#     analyzer's own fail-closed coverage predicate keys on, so a manifest edit moves this
#     expectation with it instead of hiding behind a hardcoded number.
#   * no render may produce an id its manifest does not declare at all (either required=1 or
#     required=0). `required=0` is DECLARED-BUT-OPTIONAL: allowed on either side, demanded on
#     neither. Stating it that way is what keeps the lock correct when a cell is intentionally
#     optional, and the vacuity guards below stop that tolerance being widened until the whole
#     rule is empty.
#   * the current manifest's sync cells (`path == vco_a_sync_tri`) must all be RENDERED, and must
#     all be required=1. "How many new cells" is a property of the declaration, not of this script.
#     ⚠️ The part NAME lives in the manifest's `path` column. The `part` column holds the STIMULUS
#     (`220`/`440`/`880`, `l0`/`p20`/`t50`, or the sample rate) and NEVER a part name -- keying this
#     clause on `part` made it unsatisfiable, so the clause was silently vacuous from the day it was
#     written while the fixture below (hand-shaped to match the code) kept reporting it GREEN. The
#     fixture now takes its header and its column positions FROM THE PRODUCER, and case (l) pins the
#     exact regression: a manifest that spells the part name in the STIMULUS column declares no sync
#     cell and must be RED.
#   * every id present in BOTH renders must be byte-identical in its *.raw payload.
#
# Because the expectation comes from a manifest that neither render can edit, "the render lost a
# declared cell", "the render invented a cell" and "both sides drifted together" are each RED --
# none of which a shared-count comparison can see.
ids_equivalent() {  # $1=A dir(pre-S5)  $2=B dir(product)  $3=pre-S5 manifest  $4=current manifest
                    # $5=label  [$6="allow-sync-absent" — nc1 only; see the clause in the script]
  local out rc script
  # The comparison script is written to a file with a TOP-LEVEL here-document and then run BY PATH.
  # It is deliberately NOT inlined as `out="$(python3 - <<'PY' ... PY)"`: a here-document nested in a
  # command substitution is not fully literal, because the comsub parser still tracks quote
  # characters in its body -- so a single apostrophe in a comment (e.g. "analyzer's", "cell's")
  # opens an unmatched quote and the ENTIRE runner fails to parse. Reproduced minimally:
  #   out="$(python3 - <<'PY' <newline> x = 1  # it's fine <newline> PY <newline> )"   -> syntax error
  # The failure is loud (parse error, nothing runs), but it would silently cost a whole edit-review
  # cycle, so the hazard is removed structurally rather than by avoiding apostrophes.
  script="$(mktemp "${TMPDIR:-/tmp}/gh19-id-lock.XXXXXX")" || return 1
  cat > "$script" <<'PY'
import os
import sys


def load_ids(d):
    p = os.path.join(d, "gh19_scenarios.tsv")
    out = {}
    try:
        fh = open(p, encoding="utf-8")
    except OSError as e:
        sys.exit("ERROR: cannot read %s (%s)" % (p, e))
    with fh:
        hdr = fh.readline().rstrip("\n").split("\t")
        if "id" not in hdr or "raw" not in hdr:
            sys.exit("ERROR: %s lacks id/raw columns" % p)
        i, r = hdr.index("id"), hdr.index("raw")
        for ln in fh:
            c = ln.rstrip("\n").split("\t")
            if len(c) <= max(i, r) or not c[i].strip():
                continue
            cid = c[i].strip()
            if cid in out:
                sys.exit("ERROR: duplicate id %s in %s" % (cid, p))
            out[cid] = c[r].strip()
    if not out:
        sys.exit("ERROR: no cells in %s" % p)
    return out


def manifest_rows(p):
    try:
        fh = open(p, encoding="utf-8")
    except OSError as e:
        sys.exit("ERROR: cannot read %s (%s)" % (p, e))
    rows = []
    with fh:
        hdr = fh.readline().rstrip("\n").split("\t")
        for k in ("id", "required", "path"):
            if k not in hdr:
                sys.exit("ERROR: %s lacks the %s column" % (p, k))
        ii, ir, ip = hdr.index("id"), hdr.index("required"), hdr.index("path")
        for ln in fh:
            c = ln.rstrip("\n").split("\t")
            if len(c) <= max(ii, ir, ip) or not c[ii].strip():
                continue
            rows.append((c[ii].strip(), c[ir].strip(), c[ip].strip()))
    return rows


a, b = load_ids(sys.argv[1]), load_ids(sys.argv[2])
pre_rows, cur_rows = manifest_rows(sys.argv[3]), manifest_rows(sys.argv[4])
# argv[5] is optional: the literal token "allow-sync-absent" relaxes the sync-presence clause for
# the ONE arm whose mutation is supposed to block the sync cells (nc1). It is a literal token, not
# a truthy flag, so a typo'd positional argument cannot silently switch the clause off. An EMPTY
# 6th argument counts as absent -- the shell wrapper always passes the slot, so "" must mean "not
# set" rather than reaching the typo guard (the lock self-test caught exactly that).
_sixth = sys.argv[5] if len(sys.argv) > 5 else ""
if _sixth and _sixth != "allow-sync-absent":
    sys.exit("ERROR: unrecognized 6th argument %r (only 'allow-sync-absent' is defined)" % _sixth)
allow_sync_absent = (_sixth == "allow-sync-absent")
# required==1 is the same column the analyzer's fail-closed coverage predicate keys on, so the
# expectation moves with the declaration instead of being a number written down here.
expect_pre = {i for i, req, _ in pre_rows if req == "1"}
expect_cur = {i for i, req, _ in cur_rows if req == "1"}
# `required=0` rows are DECLARED but optional: a render may omit them, so they are allowed but never
# demanded. Deriving this from the manifest (rather than assuming the two sets are equal) is what
# keeps the lock honest when a cell is declared optional -- and the vacuity guards below stop that
# tolerance from being abused to make the whole rule empty.
allow_pre = {i for i, _, _ in pre_rows}
allow_cur = {i for i, _, _ in cur_rows}
expect_new = {i for i, _, path in cur_rows if path == "vco_a_sync_tri"}

fails = []
if not expect_pre or not expect_cur:
    fails.append("a manifest's required==1 rule selected NO ids -- the expectation itself is "
                 "empty, so this lock would pass vacuously")
if not expect_new:
    fails.append("the current manifest declares NO vco_a_sync_tri cell -- the sync-cell declaration "
                 "is gone, so this lock would pass vacuously")
if not expect_new <= expect_cur:
    fails.append("the declared sync cells are not all required=1 in the current manifest -- the "
                 "declaration is internally inconsistent (%d of %d)"
                 % (len(expect_new & expect_cur), len(expect_new)))

# ---- A: the literal pre-S5 render -----------------------------------------------------------------
miss_a = sorted(expect_pre - set(a))
if miss_a:
    fails.append("pre-S5 render is MISSING %d declared-required cell(s): %s" % (len(miss_a), miss_a[:6]))
extra_a = sorted(set(a) - allow_pre)
if extra_a:
    fails.append("pre-S5 render produced %d id(s) its manifest does not declare: %s"
                 % (len(extra_a), extra_a[:6]))

# ---- B: the product render ------------------------------------------------------------------------
miss_b = sorted(expect_cur - set(b))
if miss_b:
    fails.append("product render is MISSING %d declared-required cell(s): %s" % (len(miss_b), miss_b[:6]))
extra_b = sorted(set(b) - allow_cur)
if extra_b:
    fails.append("product render produced %d id(s) its manifest does not declare: %s"
                 % (len(extra_b), extra_b[:6]))
# "Rendered" means the row carries a *.raw PAYLOAD. A bare set-of-ids check cannot see the third
# state -- declared, present as a row, not rendered -- and that state is exactly how a fail-closed
# probe records a premise failure: it writes a row per declared cell with an EMPTY raw field. Such
# a cell must not be readable as "produced" by this lock.
rendered_b = set(cid for cid, raw in b.items() if raw)
sync_absent = sorted(c for c in expect_new if c not in rendered_b)
if sync_absent:
    if not allow_sync_absent:
        fails.append("the declared sync cells were NOT rendered: %d missing %s"
                     % (len(sync_absent), sync_absent[:6]))
    elif len(sync_absent) != len(expect_new):
        fails.append("allow-sync-absent was passed, but only %d of %d declared sync cells are "
                     "absent from B -- the flag explains a WHOLE blocked group or nothing at all; "
                     "a partial absence is a different failure, not this one"
                     % (len(sync_absent), len(expect_new)))
    else:
        print("NOTE: allow-sync-absent: all %d declared sync cells are ABSENT from B. The arm "
              "asserts that absence, and the probe's named reason for it, separately."
              % len(sync_absent))
elif allow_sync_absent:
    fails.append("allow-sync-absent was passed but every declared sync cell WAS rendered -- the "
                 "flag is not justified for this render, so it would be a silent weakening")

# ---- the default behaviour must be untouched: every shared id byte-identical ---------------------
# `sync_absent` cells are excluded here on purpose: they are still rows in B, so they enter the
# intersection only if the A side declares them too, and their empty payload would otherwise be
# reported as a missing file -- a confusing second failure for a cell whose absence is already
# named above. This is a de-duplication of the message, NOT a tolerance: the clause above is what
# decides whether that absence is legitimate, and it fails closed for every arm but the one that
# passed the literal flag.
differ, missing_file = [], []
for cid in sorted(set(a) & set(b)):
    if cid in sync_absent:
        continue
    fa, fb = os.path.join(sys.argv[1], a[cid]), os.path.join(sys.argv[2], b[cid])
    if not (os.path.isfile(fa) and os.path.isfile(fb)):
        missing_file.append(cid)
        continue
    with open(fa, "rb") as x, open(fb, "rb") as y:
        if x.read() != y.read():
            differ.append(cid)
if differ:
    fails.append("shared ids are NOT byte-identical: %d %s" % (len(differ), differ[:6]))
if missing_file:
    fails.append("a mapped *.raw file is missing: %d %s" % (len(missing_file), missing_file[:6]))

print("required_pre=%d required_cur=%d sync_declared=%d a_cells=%d b_cells=%d shared=%d byte_differ=%d"
      % (len(expect_pre), len(expect_cur), len(expect_new), len(a), len(b),
         len(set(a) & set(b)), len(differ) + len(missing_file)))
for f in fails:
    print("LOCK-FAIL: " + f)
sys.exit(1 if fails else 0)
PY
  set +e
  out="$(python3 "$script" "$1" "$2" "$3" "$4" "${6:-}" 2>&1)"; rc=$?
  set -e
  rm -f "$script"
  echo "   $out"
  if [ "$rc" -ne 0 ]; then
    echo "   FAIL: $5 — the default-behaviour id-set lock is RED (see LOCK-FAIL above)." >&2
    return 1
  fi
  if [ "${6:-}" = "allow-sync-absent" ]; then
    echo "   OK: $5 — every declared-required pre-existing cell rendered, no undeclared cell in"
    echo "       either render, every shared cell BYTE-IDENTICAL, and all declared sync cells"
    echo "       ABSENT as declared by this arm."
  else
    echo "   OK: $5 — every declared-required cell rendered, no undeclared cell in either render,"
    echo "       the declared sync cells present, and every shared cell BYTE-IDENTICAL."
  fi
  return 0
}

# ---------------------------------------------------------------------------------------------
# RED-FIRST EVIDENCE FOR THE LOCK ITSELF.
#
# An assertion that has never been observed RED is not evidence, so the lock's own negative cases
# are exercised on SYNTHETIC dirs -- no build, no render, milliseconds -- and each must be red for
# ITS OWN NAMED REASON. The control case (a faithful pair) must be GREEN, or a lock that simply
# always failed would "pass" this test. Cases, and the gap each one covers:
#   (control 1) a faithful pair stays GREEN, or a lock that always failed would "pass" this test
#   (control 2) a declared-OPTIONAL cell in A and absent from B stays GREEN, or an over-constrained
#               lock would "pass" every red case while contradicting the contract
#   (a) a shared id's bytes differ         -> byte-identity, the only thing raws_equal_by_id caught
#   (b) a required pre-existing id is absent from B -> inclusion against the declaration; a count
#                                             check could still "pass" if B gained another id
#   (c) an undeclared extra id in B        -> the declaration bound, the direction S5 itself must obey
#   (d) the pre-S5 side ALSO loses a cell  -> the manifest-derived expectation; "shrink both sides
#                                             together" is the failure a mutual comparison cannot see
#   (e) the sync-cell declaration empties  -> the vacuity guard on the expectation
#   (f) an undeclared extra id in A        -> the pre-S5 side is bound by its own manifest too
#   (g) a sync cell declared required=0    -> the declaration is internally inconsistent
#   (control 3) the literal `allow-sync-absent` flag with the WHOLE sync group blocked stays GREEN,
#               and (h) the same render WITHOUT the flag is RED -- together these show the flag
#               removes exactly one red case and is not a no-op
#   (i) the flag passed where the sync cells WERE rendered -> the flag is unjustified, RED
#   (j) the flag passed AND a pre-existing cell missing   -> still RED: it covers the sync clause only
#   (k) the flag passed with only PART of the sync group blocked -> RED: all of it or none of it
#   (l) the part name spelled in the manifest's STIMULUS column -> RED: the sync declaration must be
#       read from the producer's `path` column. Keying it on `part` made the clause unsatisfiable,
#       i.e. silently vacuous, which is the bug this case now pins.
ids_equivalent_selftest() {
  local d rc fails n ok
  n=0
  d="$(mktemp -d)"
  # ---- the manifest fixtures -------------------------------------------------------------------
  # The header line AND every column position are taken from the PRODUCER (`$MANIFEST`), never
  # written by hand here. A hand-shaped header only proves this script agrees with itself, and that
  # is exactly how the `part`/`path` mix-up survived: the old fixtures spelled the part name in
  # `part`, which no real manifest row ever does (the producer's `part` column holds the STIMULUS:
  # 220/440/880, l0/p20/t50, or the sample rate). Rows are given as "id:required:pathname:stim",
  # and the stim value is written to BOTH `stim` and `part` because that is the producer's shape.
  fx_manifest() {  # $1=out  $2...=rows
    local out="$1"; shift
    awk -F'\t' -v OFS='\t' -v rows="$*" '
      NR==1 { n=NF; for (i=1;i<=NF;i++) idx[$i]=i; print; next }
      { next }
      END {
        m=split(rows,R," ")
        for (k=1;k<=m;k++) {
          split(R[k],F,":")
          for (i=1;i<=n;i++) col[i]="-"
          if (idx["id"])       col[idx["id"]]=F[1]
          if (idx["required"]) col[idx["required"]]=F[2]
          if (idx["path"])     col[idx["path"]]=F[3]
          if (idx["stim"])     col[idx["stim"]]=F[4]
          if (idx["part"])     col[idx["part"]]=F[4]
          s=""; for (i=1;i<=n;i++) s = s (i>1?OFS:"") col[i]
          print s
        }
      }' "$MANIFEST" > "$out"
    # A projection is only meaningful if the producer really carries the columns claimed above: if
    # `path` or `required` were missing every row would read "-" and the fixture would silently be
    # an EMPTY declaration again, i.e. the very failure this rewrite exists to prevent.
    local ncol nrow ipos
    ncol="$(head -1 "$out" | awk -F'\t' '{print NF}')"
    nrow="$(( $(wc -l < "$out") - 1 ))"
    ipos="$(head -1 "$out" | tr '\t' '\n' | grep -n -x -e path -e required | wc -l | tr -d ' ')"
    if [ "${ipos:-0}" -ne 2 ] || [ "$nrow" -lt 1 ]; then
      echo "   SELFTEST SETUP FAIL: fixture $out is not producer-shaped (ncol=$ncol named_cols=$ipos rows=$nrow)" >&2
      fx_fail=1
      return 0
    fi
  }
  fx_one() {  # the single pre-existing part name, used for every non-sync fixture cell
    echo "vco_a_tri"
  }
  fx_fail=0
  # pre-S5 declares c1..c3; the current manifest adds the sync cell s1, whose part NAME is
  # `vco_a_sync_tri` (in `path`) and whose stimulus is 220.
  fx_manifest "$d/pre.tsv"        "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220"
  fx_manifest "$d/cur.tsv"        "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220 s1:1:vco_a_sync_tri:220"
  fx_manifest "$d/cur-nosync.tsv" "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220"
  # c0 is DECLARED-BUT-OPTIONAL (required=0): produces no violation whether present or absent.
  fx_manifest "$d/pre-opt.tsv"    "c0:0:$(fx_one):220 c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220"
  # internal inconsistency: a sync cell declared required=0 instead of 1.
  fx_manifest "$d/cur-sync-optional.tsv" "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220 s1:0:vco_a_sync_tri:220"
  # TWO declared sync cells, for the "the flag explains a WHOLE blocked group or nothing" guard.
  fx_manifest "$d/cur-2sync.tsv"  "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220 s1:1:vco_a_sync_tri:220 s2:1:vco_a_sync_tri:220"
  # (l) THE REGRESSION, pinned: the part name spelled in the STIMULUS column while `path` names a
  #     non-sync part. This is precisely the shape the old `part == vco_a_sync_tri` predicate was
  #     satisfied by, and no real manifest row has it -- so it must declare NO sync cell and be RED.
  fx_manifest "$d/cur-name-in-stim.tsv" "c1:1:$(fx_one):220 c2:1:$(fx_one):220 c3:1:$(fx_one):220 s1:1:$(fx_one):vco_a_sync_tri"
  if [ "$fx_fail" -ne 0 ]; then
    echo "   SELFTEST RESULT: FAIL — the manifest fixtures could not be projected from the producer." >&2
    rm -rf "$d"
    return 1
  fi

  mk() {  # $1=dir  $2=ids  $3=c1 byte payload. An id prefixed with '?' is written as a BLOCKED
          # row -- present, with an EMPTY payload and no *.raw file -- which is exactly how the
          # fail-closed probe records a cell whose premise did not hold.
    : > "$1/gh19_scenarios.tsv"
    printf 'id\traw\n' >> "$1/gh19_scenarios.tsv"
    local spec c
    for spec in $2; do
      c="${spec#\?}"
      if [ "$spec" != "$c" ]; then
        printf '%s\t\n' "$c" >> "$1/gh19_scenarios.tsv"
        continue
      fi
      printf '%s\t%s.raw\n' "$c" "$c" >> "$1/gh19_scenarios.tsv"
      if [ "$c" = "c1" ]; then printf '%s' "$3" > "$1/$c.raw"; else printf 'x' > "$1/$c.raw"; fi
    done
  }

  fails=0
  # Each RED case must be red for ITS OWN NAMED REASON -- a bare "non-zero exit" would also be
  # satisfied by a crash, a missing interpreter or a typo'd path, none of which is the lock working.
  expect_lock() {  # $1=A ids $2=B ids $3=a c1 payload $4=b c1 payload $5=want(ok|red) $6=label $7=red reason [$8=current manifest] [$9=pre-S5 manifest] [$10="allow-sync-absent"]
    local out rc cur="${8:-$d/cur.tsv}" pre="${9:-$d/pre.tsv}"
    n=$((n + 1))
    rm -rf "$d/A" "$d/B"; mkdir -p "$d/A" "$d/B"
    mk "$d/A" "$1" "$3"; mk "$d/B" "$2" "$4"
    set +e
    out="$(ids_equivalent "$d/A" "$d/B" "$pre" "$cur" "$6" "${10:-}" 2>&1)"; rc=$?
    set -e
    if [ "$5" = "ok" ]; then
      if [ "$rc" -ne 0 ]; then
        echo "   SELFTEST FAIL: $6 — expected GREEN, got rc=$rc"; echo "$out" | sed 's/^/      /'
        fails=$((fails + 1))
      fi
      return 0
    fi
    if [ "$rc" -eq 0 ]; then
      echo "   SELFTEST FAIL: $6 — expected RED, got rc=0 (the lock cannot see this)"
      fails=$((fails + 1))
    elif ! printf '%s' "$out" | grep -qF "$7"; then
      echo "   SELFTEST FAIL: $6 — RED, but not for the named reason '$7':"; echo "$out" | sed 's/^/      /'
      fails=$((fails + 1))
    fi
  }

  # (control 1) faithful pair: every required cell rendered, exactly the sync cell added, bytes equal.
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "v" ok  "control_faithful" ""
  # (control 2) a declared-OPTIONAL (required=0) cell present in A and absent from B must stay GREEN
  #     -- this is the control against the lock over-constraining: required=0 is allowed, not
  #     demanded. Without it, a stricter `set(a) == set(b) - sync` rule would look "safer" and pass
  #     every red case while being wrong about the contract.
  expect_lock "c0 c1 c2 c3" "c1 c2 c3 s1" "v" "v" ok  "control_declared_optional_absent_in_b" "" \
              "$d/cur.tsv" "$d/pre-opt.tsv"
  # (a) a shared id (c2) differs byte-for-byte -> the byte-identity rule, the ONE thing the old
  #     count-only comparison could already catch. Kept so the new rules can't regress it.
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "DIFFERENT" red "(a)byte_differ" \
              "shared ids are NOT byte-identical"
  # (b) a required pre-existing cell vanishes from B (B renders c1,c3 + s1) -> the inclusion rule in
  #     the direction that matters for "S5 adds cells and removes none".
  expect_lock "c1 c2 c3" "c1 c3 s1" "v" "v" red "(b)b_missing_required" \
              "product render is MISSING"
  # (c) an undeclared extra id appears in B -> the declaration bound, the direction S5 itself must
  #     not take (an invented cell is not a fix).
  expect_lock "c1 c2 c3" "c1 c2 c3 s1 zz" "v" "v" red "(c)b_extra_undeclared" \
              "product render produced"
  # (d) BOTH sides lose c3 -> the manifest-derived expectation. THE case a mutual comparison cannot
  #     see: the shared set still matches itself perfectly, so a count/equality check stays green.
  expect_lock "c1 c2" "c1 c2 s1" "v" "v" red "(d)both_sides_shrank" \
              "pre-S5 render is MISSING"
  # (e) the current manifest declares no sync cell at all -> the expectation is vacuous (this is the
  #     guard against the whole lock silently passing on an empty rule).
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "v" red "(e)no_sync_declared" \
              "declares NO vco_a_sync_tri" "$d/cur-nosync.tsv"
  # (f) an undeclared extra id appears in the PRE-S5 render -> the pre-S5 side is bound by its own
  #     manifest too; otherwise "the pre-S5 side" could be padded to make the shared set match.
  expect_lock "c1 c2 c3 zz" "c1 c2 c3 s1" "v" "v" red "(f)a_extra_undeclared" \
              "pre-S5 render produced"
  # (g) a sync cell declared required=0 -> the declaration is internally inconsistent (the sync
  #     cells are the whole point of S5, so they cannot also be optional).
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "v" red "(g)sync_declared_optional" \
              "not all required=1" "$d/cur-sync-optional.tsv"

  # ---- the `allow-sync-absent` relaxation (nc1's arm). A relaxation is the most dangerous kind of
  # change to a lock: it removes a red case by construction, so every case below exists to show it
  # removes EXACTLY one clause, and only where the flag was literally passed.
  #
  # (control 3) the flag with the WHOLE declared sync group blocked -> GREEN. This is the state nc1
  #     produces: the probe refuses the sync cells, writes a row per cell with no payload, and
  #     nc1 asserts that absence and its named cause itself. Without this control the flag would
  #     look like a pure weakening.
  expect_lock "c1 c2 c3" "c1 c2 c3 ?s1" "v" "v" ok  "control_blocked_sync_group_allowed" "" \
              "$d/cur.tsv" "$d/pre.tsv" "allow-sync-absent"
  # (h) THE SAME RENDER WITHOUT THE FLAG must still be RED. If it were not, the flag would be a
  #     no-op and every other case here would be measuring nothing.
  expect_lock "c1 c2 c3" "c1 c2 c3 ?s1" "v" "v" red "(h)blocked_without_flag" \
              "were NOT rendered"
  # (i) the flag passed but the sync cells ARE rendered -> RED. A flag that silently did nothing
  #     would leave a future reader believing a relaxation was in force when it was not.
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "v" red "(i)flag_unjustified" \
              "not justified for this render" "$d/cur.tsv" "$d/pre.tsv" "allow-sync-absent"
  # (j) the flag passed AND a pre-existing cell is missing -> still RED. The relaxation covers the
  #     sync-clause only; the inclusion rule that protects the 84 pre-existing cells is untouched.
  expect_lock "c1 c2 c3" "c1 c3 ?s1" "v" "v" red "(j)flag_does_not_cover_preexisting" \
              "product render is MISSING" "$d/cur.tsv" "$d/pre.tsv" "allow-sync-absent"
  # (k) the flag passed with only PART of the declared sync group blocked -> RED. "All of it or
  #     none of it" is what keeps the flag from becoming a general tolerance for absent sync cells.
  expect_lock "c1 c2 c3" "c1 c2 c3 ?s1 s2" "v" "v" red "(k)flag_partial_absence" \
              "only 1 of 2" "$d/cur-2sync.tsv" "$d/pre.tsv" "allow-sync-absent"
  # (l) the part name spelled in the STIMULUS column -> RED. The lock must read the part NAME from
  #     the producer's `path` column; a declaration that only LOOKS like one (the name sitting in
  #     `part`, which is where the stimulus lives) is not a declaration at all. Without this case
  #     the predicate could silently revert to `part` and every other case here would stay green,
  #     because the fixtures are projected from the same producer and would follow it back.
  expect_lock "c1 c2 c3" "c1 c2 c3 s1" "v" "v" red "(l)sync_name_in_stim_column" \
              "declares NO vco_a_sync_tri cell" "$d/cur-name-in-stim.tsv"

  rm -rf "$d"
  if [ "$fails" -ne 0 ]; then
    echo "   SELFTEST RESULT: FAIL — $fails of $n case(s)" >&2
    return 1
  fi
  # The count is printed, not implied: a case that silently stopped being executed would otherwise
  # leave this function reporting PASS on fewer cases than the list above claims to cover.
  echo "   SELFTEST RESULT: PASS — $n case(s): controls GREEN; (a)..(l) each RED for its own named"
  echo "   reason, and the allow-sync-absent flag removes exactly one of them."
  return 0
}

echo "=== GH#19 S5 VCO-A hard-sync mutation runner ==="
echo "tree: $TREE"
echo "rev for [A2]: $PRE_S5_REV"
echo "thresholds: gain>=${MIN_GAIN_DB} dB (880: ${MIN_GAIN_HIGH_DB}), gap>=${MIN_GAP_DB} dB, per<=${MAX_PER_DEV}"
echo

########################################################################################
# [0] THE LOCK'S OWN RED-FIRST EVIDENCE — before any build, on synthetic dirs. An assertion that
# has never been seen RED is not evidence, so the id-set lock is required to be able to fail for
# each named reason (mutation of a shared cell's bytes, a cell disappearing from either side, an
# undeclared extra cell, an emptied sync declaration) while its faithful control case stays GREEN.
# Run FIRST so a lock that cannot fail is caught in milliseconds, not after ~17 min of renders.
########################################################################################
echo "[0] id-set lock self-test (synthetic; no build, no render)"
LOCK_SELFTEST_OK=0
if ids_equivalent_selftest; then
  LOCK_SELFTEST_OK=1
else
  echo "   FAIL: the id-set lock's own red-first self-test is RED — every [A2] verdict it produces" >&2
  echo "         would be untrustworthy, so this run is aborted before any render." >&2
  echo "RESULT: FAIL — lock self-test did not pass."
  exit 1
fi
echo

########################################################################################
# [G] THE GATE'S OWN CONTROLS, RUN IN THIS ROUND.
#
# The gate ships a control suite of its own (`--self-check`) covering both failure surfaces this
# runner also asserts from the outside: "absent criterion" must be STRUCTURAL rc=2 naming
# "NO criterion", while "removal" and "one-below-880" must be JUDGEMENT rc=1 naming their own
# rules. Those fixtures are synthetic, but their LAYOUT is not:
# `self_check()` pins its column indices against the analyzer's REAL header before any control
# runs, so a producer-side column change fails here rather than silently validating the wrong
# field. Running it in-round means the gate is asked to account for itself in the same run whose
# verdicts this runner reports.
#
# WHAT THIS ARM DOES AND DOES NOT CLAIM — stated narrowly, because the honest version is weaker
# than it first looks:
#   IT DOES witness that the gate's suite returns rc=0 and prints its PASS line, i.e. none of the
#   gate's controls failed. `self_check()` appends every failure to one list and `main()` folds
#   that list into rc, so rc=0 is behavioral rather than decorative.
#   IT DOES NOT witness that all of the controls RAN. The PASS line is a HARDCODED string
#   (`check_gh19_hardsync_acceptance.py:505`) printed whenever the failure list is empty, and the
#   per-control labels ("vacuous-anchor", "premise-broken", ...) are used only inside `fails`
#   messages -- they are never printed on a PASS. So grepping for those names would pass even if a
#   control had been deleted from `self_check()`. That grep is deliberately NOT made here: a check
#   that cannot fail for its stated reason is worse than no check.
# The gap that leaves -- "did a control silently disappear?" -- is not closable from outside the
# gate, and it is not what this slice's evidence rests on. What this runner relies on is the
# REAL-RENDER arm below: nc1's own render is the in-round witness that the STRUCTURAL rc=2 code
# path is live on a production mutation, and nc3/nc4 are the same for judgement rc=1.
echo "[G] gate self-check (the acceptance gate's own controls, in this round)"
GATE_SC_FAILED=0
set +e
python3 "$GATE" --self-check > "$WORK/gate-selfcheck.txt" 2>&1
GATE_SC_RC=$?
set -e
if [ "$GATE_SC_RC" -ne 0 ]; then
  echo "   FAIL: the gate's self-check is RED (rc=$GATE_SC_RC) — the gate's own controls are not" >&2
  echo "         satisfied, so no verdict it produces in this round can be trusted." >&2
  sed -n '1,30p' "$WORK/gate-selfcheck.txt" >&2
  GATE_SC_FAILED=1
else
  _sc_fail=""
  grep -qF "self-check PASS" "$WORK/gate-selfcheck.txt" \
    || _sc_fail="rc=0 but the PASS line was not printed (the suite did not reach its summary)"
  grep -qF "FAILED" "$WORK/gate-selfcheck.txt" \
    && _sc_fail="${_sc_fail:+$_sc_fail; }rc=0 but the report says FAILED"
  if [ -n "$_sc_fail" ]; then
    echo "   FAIL: the gate's self-check returned rc=0 but: $_sc_fail" >&2
    sed -n '1,30p' "$WORK/gate-selfcheck.txt" >&2
    GATE_SC_FAILED=1
  else
    echo "   OK: rc=0 and PASS line present — none of the gate's own controls failed in this"
    echo "       round. (Which controls RAN is not observable from outside the gate; see above.)"
  fi
fi
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
PRE_MANIFEST="$WORK/pre-s5-manifest.tsv"
if git -C "$ROOT" cat-file -e "$PRE_S5_REV:core/include/lunar24/core/vco.h" 2>/dev/null; then
  for f in "${S5_FILES[@]}"; do git -C "$ROOT" show "$PRE_S5_REV:$f" > "$TREE/$f"; done
  # The PRE-S5 manifest is the independent expectation the lock is judged against, so it is taken
  # from the same rev as the pre-S5 source. It is NOT derived from the current manifest (that would
  # make the expectation depend on the thing under test), and NOT written down here as a number.
  git -C "$ROOT" show "$PRE_S5_REV:tools/gh19_manifest.tsv" > "$PRE_MANIFEST"
  build_probe
  render_only "$WORK/pre-s5-out"
  PRE_RENDERED=1
  echo "   rendered the literal pre-S5 source into $WORK/pre-s5-out"
  echo "   pre-S5 manifest: $PRE_MANIFEST"
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
NC5_RED=0

# nc1: the consumer removed. The jack is still patched (the probe's cable and the binding are
# untouched), so the slave free-runs at its own f0 -- which is NOT M-periodic.
#
# RE-REGISTERED 2026-09-13 from a MEASUREMENT (`scratch/nc1_repro.sh`), not by moving the old
# expectation to whatever the mutant did. Items 2 and 4 of the seven-item package added probe-side
# reconciliations that fire on this exact mutation, so the consumer's removal is now caught on TWO
# surfaces by THREE separately-named assertions:
#   * PROBE_RC=65 = bit 1 (a required cell was not produced) | bit 64 (item 4's DeviceState cable
#     produced no master-grid resets). Each half is asserted with its OWN reason string below.
#   * the analyzer returns rc=1 and says `required-cell-missing ... [not-produced:sync-timing: no
#     reset was ever applied ...]` for all 12 sync cells: it REFUSES to answer, while still writing
#     a row for every required id. That is why this arm calls `measure_keeps_render` rather than
#     `measure` -- `measure` aborts on ANY non-zero probe exit, so the old registration had become
#     UNREACHABLE rather than wrong, and an unreachable arm reads as a pass for the wrong reason.
#   * the gate then goes STRUCTURAL rc=2 naming `NO criterion`, i.e. the original registration still
#     holds -- reached through the probe's refusal rather than instead of it.
# A probe-owned VALIDITY red and a gate STRUCTURAL red are different claims (see the header), so
# both are asserted here with their own names and their own expected codes; neither can pass by
# reading the other's evidence, and `run_gate` reads only the table THIS call wrote.
echo
echo "[B/nc1] consumer_removed — vcA_.requestSync() call site unwired"
restore_source
py_splice "$RT" "$CONSUMER_BLOCK" "" 1
build_probe
measure_keeps_render
expect_failclosed "nc1_consumer_removed (item 2: the per-cell timing premise)" \
  "required cell not produced [sync-timing: no reset was ever applied" || NC_FAILED=1
expect_failclosed "nc1_consumer_removed (item 4: the DeviceState cable)" \
  "the sync cable declared in the DeviceState did NOT produce master-grid resets" || NC_FAILED=1
if [ "$PRE_RENDERED" -eq 1 ]; then
  # LOCK (replaces the old bare `raws_equal_by_id`, which was REMOVED entirely rather than kept
  # underneath): asserted as a SET against the manifest-declared expectation, so a cell that
  # vanishes from BOTH sides -- or from either side -- is RED here instead of passing on a matching
  # shared count. Byte-identity of the shared cells is one of the clauses below.
  #
  # `allow-sync-absent` is passed because this is the ONE arm whose whole subject is that the
  # declared sync cells are NOT rendered: the lock relaxes exactly that clause, only when the WHOLE
  # declared sync group is absent, and its own self-test proves partial absence, an unjustified
  # flag, and a missing pre-existing cell all stay RED. The arm asserts the absence and its named
  # cause separately, in the two expect_failclosed calls above.
  if ids_equivalent "$WORK/pre-s5-out" "$WORK/probe-out" "$PRE_MANIFEST" "$MANIFEST" \
                    "nc1_vs_pre-s5" "allow-sync-absent" > "$WORK/eq-nc1.txt" 2>&1; then
    cat "$WORK/eq-nc1.txt"
    echo "   OK: nc1's render is the pre-S5 set exactly, plus only the declared sync cells,"
    echo "       and every pre-existing cell is BYTE-IDENTICAL to the literal pre-S5 source."
    EQ_OK=1
  else
    echo "   FAIL: the default-behaviour id-set lock is RED for nc1 — nc1 is not a pure"
    echo "         consumer-removal of the literal pre-S5 source." >&2
    cat "$WORK/eq-nc1.txt" >&2
    NC_FAILED=1
  fi
fi
run_gate
# nc1's gate signature is the STRUCTURAL code: the analyzer refused to answer, so the gate reports
# a required cell with no criterion instead of a cell that missed a threshold.
expect_red "nc1_consumer_removed" "NO criterion" 2 || NC_FAILED=1
NC1_RED="$(red_count)"
report_arm nc1

# nc2: the jump term ADDED instead of subtracted => the discontinuity is doubled, not removed.
# PRE-REGISTERED before this revision was run: a PROBE-OWNED VALIDITY red. Deviation from the
# correct kernel is 1.0*jmp -- twice the no-correction case -- which is enough to leave the
# audio-domain peak guard, so all 12 sync cells are refused and the gate produces no criterion
# for them. This arm therefore says NOTHING about whether the gate discriminates; nc3 and nc4
# carry that. `report_arm` is deliberately NOT called: it reads $WORK/gate.txt, which would
# still hold nc1's output, and reporting that as nc2's would be a fabricated result.
echo
echo "[B/nc2] jump_sign_flipped — out += 0.5*jmp instead of -="
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC2" 1
build_probe
measure_may_fail_closed
expect_failclosed "nc2_jump_sign_flipped" "over-scale" || NC_FAILED=1
if [ "$PROBE_RC" -ne 0 ]; then
  echo "   gate not run for nc2: no criterion was produced, so there is nothing for it to judge."
fi
NC2_RED="n/a — probe refused; no criterion reached the gate"

# nc3: the full-scale kernel — `out -= jmp`. This is the "copied the S2 kernel" error: S2's
# polyblepSaw subtracts the FULL-scale residual because a saw wrap spans the whole range,
# whereas a hard-sync jump J is arbitrary and only its causal half belongs on this sample.
#
# RE-REGISTERED 2026-09-13 from a MEASUREMENT, and the original registration is WITHDRAWN rather
# than quietly re-read. It was pre-registered as a JUDGEMENT red (probe accepts -> gate rc=1
# naming `220/440:>=`, which v2 did measure at red=12/12). In v3 the probe REFUSES this mutant
# FIRST: item 4's DeviceState cable check reconstructs the reset frame from the produced audio and
# reconciles it with the master's own published edge, and this kernel's doubled correction moves
# that reconstruction by one frame -> `SYNC-STATE-CABLE ... reset #0 reconstructed at frame 512
# but the master's own published output has NO rising edge there (the preceding master edge is at
# frame 511, i.e. the reset is 1 frame(s) LATE)`, exit 65, PARTIAL: 87 cells produced / 9 of the 12
# sync cells blocked. So the judgement verdict this arm pre-registered is UNMEASURABLE, and saying
# so is the honest outcome: nc4 carries the judgement path (it is the load-bearing control for it),
# and what nc3 now witnesses is that a PARTIAL refusal still escalates to the gate's STRUCTURAL
# code -- a claim nc1, which refuses all 12, cannot make.
# ⚠️ RULED (@Codex 8296a601, 2026-09-12) -- record, do not re-litigate: this mutant is an AMPLITUDE
# error, not a timing one, yet it trips a criterion whose reason string names a TIMING cause. Two
# errors hitting the same symptom MAY stay; the refusal is correct (fail-closed: this render does not
# establish that the reset lands on the master edge), but the reason is a symptom, not a cause. Do
# NOT read the reason string as root-cause localisation -- distinguish this arm from nc5 (a genuine
# one-frame late consumption) BY INJECTION SITE: nc3 injects into vco.h's tick() correction
# (`*out -= jmp` instead of `- 0.5 * jmp`), nc5 injects into machine_runtime.h's consumer call site
# (deferring the request by one frame). The shared reason string is NOT evidence of a shared cause,
# and the reason string is deliberately NOT changed here (that would be a change of criterion text).
echo
echo "[B/nc3] full_scale_kernel — out -= jmp (the S2 kernel copied without the 1/2)"
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC3" 1
build_probe
# `measure_keeps_render`, NOT `measure_may_fail_closed`: the latter returns before running the
# analyzer, so `$WORK/analyze.tsv` would still hold nc1's table and the gate call below would
# report someone else's verdict. This arm needs a table written by THIS call.
measure_keeps_render
expect_failclosed "nc3_full_scale_kernel (item 4: the DeviceState cable reconstruction)" \
  "the sync cable declared in the DeviceState did NOT produce master-grid resets" || NC_FAILED=1
run_gate
expect_red "nc3_full_scale_kernel" "NO criterion" 2 || NC_FAILED=1
NC3_RED="$(red_count)"
report_arm nc3

# nc4: the DEFERRED reset KEPT but the band-limiting term dropped. The premise still holds
# (the reset still happens every M samples => the cell is still M-periodic), so this arm must
# produce a REAL criterion and fail as a JUDGEMENT red (rc=1). Together with nc1 this separates
# the two failure modes the slice could hide behind. PRE-REGISTERED: probe accepts, rc=1,
# naming `220/440:>=` — this is the load-bearing judgement control, since dropping the term
# entirely is exactly the pre-S5 behaviour.
echo
echo "[B/nc4] correction_absent — deferred reset kept, band-limited jump term dropped"
restore_source
py_splice "$VCO" "$JMP_FIXED" "$JMP_NC4" 1
build_probe
measure_may_fail_closed
NC4_RED="n/a — probe refused; no criterion reached the gate"
if require_criterion "nc4_correction_absent"; then
  run_gate
  expect_red "nc4_correction_absent" "220/440:>=" 1 || NC_FAILED=1
  NC4_RED="$(red_count)"
  report_arm nc4
else
  NC_FAILED=1
fi

# nc5: the reset DEFERRED BY ONE FRAME. Same consumer, same cable, same edge detector, same
# kernel -- only the frame on which `requestSync()` is called moves, by exactly one. This is the
# arm the entire item-2 criterion exists for, and the reason it has to exist is that NO
# residual-based figure can see it: a criterion evaluated over an M-periodic signal is invariant
# under a global shift of the same samples by one cell, which is @Codex's 908f36e7 experiment
# (shifting the periodic samples by one whole cell left the analyzer's answer bit-for-bit
# unchanged: residual -31.7247754567 dB, gap 14.0525637166, per=0). A gate that only ever
# compares such figures would call this mutant GREEN no matter how many other arms pass.
# PRE-REGISTERED: the deferral breaks the cell's stated premise -- the reset is no longer applied
# on the frame the master's own published output rises -- so the probe VOIDS the cell
# fail-closed (per-cell signal `sync-timing: ...`, non-zero exit) and no criterion reaches the
# gate. Its evidence is therefore the PROBE-OWNED counterpart, exactly like nc2: the refusal must
# happen AND must name the timing criterion, not merely exit non-zero.
echo
echo "[B/nc5] sync_deferred_one_frame — requestSync() latched and applied on the NEXT frame"
restore_source
py_splice "$RT" "$CONSUMER_BLOCK" "$CONSUMER_BLOCK_DEFER" 1
py_splice "$RT" "$SYNC_MEMBER_ANCHOR" "$SYNC_MEMBER_DEFER" 1
build_probe
measure_may_fail_closed
expect_failclosed "nc5_sync_deferred_one_frame" "the reset is 1 frame(s) LATE" || NC_FAILED=1
if [ "$PROBE_RC" -ne 0 ]; then
  echo "   gate not run for nc5: the premise the 12 cells rest on is void, so there is nothing"
  echo "   for the gate to judge -- and running it anyway would report nc4's stale verdict."
fi
NC5_RED="n/a — probe refused; no criterion reached the gate"
echo

echo "=== runner summary ==="
if [ "$EQ_OK" -eq 1 ]; then
  EQWORD="yes"
elif [ "$PRE_RENDERED" -eq 1 ]; then
  EQWORD="NO — the pre-S5 render was built and nc1 did NOT match it"
else
  EQWORD="not measured (rev unavailable)"
fi
echo "  [0]  lock self-test     : $([ "$LOCK_SELFTEST_OK" -eq 1 ] && echo 'GREEN controls + (a)..(l): 15 cases, each RED for its OWN named reason (incl. the three that keep the sync-absence relaxation honest, and (l) pinning the part-name column)' || echo 'NOT RUN')"
echo "  [G]  gate self-check    : $([ "$GATE_SC_FAILED" -eq 0 ] && echo 'rc=0 + PASS (none of the gate'\''s own controls failed in THIS round)' || echo 'FAILED — see above')"
echo "  [A]  positive           : GREEN 12/12 (rc=0)"
echo "  nc1  consumer removed   : VALIDITY red at the PROBE (exit code + two named reasons: item-2"
echo "                           timing premise, item-4 DeviceState cable) AND STRUCTURAL rc=2 at the"
echo "                           gate (named: NO criterion; ${NC1_RED} per-cell RED lines, which is"
echo "                           expected — a structural red names cells with NO criterion, not"
echo "                           cells that missed one);"
echo "                           pre-S5 id SET rendered exactly (manifest-derived), only the"
echo "                           declared sync cells added, pre-existing cells *.raw-identical: $EQWORD"
echo "  nc2  jump sign flipped  : VALIDITY red at the PROBE (substitution guard, not the gate);"
echo "                           ${NC2_RED}"
echo "  nc3  full-scale kernel  : VALIDITY red at the PROBE (item-4 DeviceState cable reconstruction;"
echo "                           PARTIAL — 9 of the 12 sync cells refused, 87 produced) AND STRUCTURAL"
echo "                           rc=2 at the gate (named: NO criterion). This arm's pre-registered"
echo "                           JUDGEMENT red is UNMEASURABLE and is WITHDRAWN, not re-read: nc4"
echo "                           carries the judgement path."
echo "                           What nc3 witnesses, and nc1 (a FULL refusal) cannot, is that a"
echo "                           PARTIAL refusal ESCALATES instead of downgrading. The gate prints a"
echo "                           row only for a cell that HAS a criterion, so the 9 refused cells are"
echo "                           named in its problem list and get NO row at all — while the"
echo "                           ${NC3_RED} cells that DID reach judgement all missed it (gain"
echo "                           +0.00 dB) and ARE RED rows. The structural bucket being non-empty"
echo "                           is what makes the exit code 2 rather than 1, even with ${NC3_RED}"
echo "                           RED rows in the same table."
echo "  nc4  correction absent  : RED ${NC4_RED}/12 as judgement rc=1 (load-bearing judgement control)"
echo "  nc5  reset deferred 1f  : VALIDITY red at the PROBE (item-2 timing criterion, not the gate);"
echo "                           ${NC5_RED}"
if [ "$GATE_SC_FAILED" -ne 0 ]; then
  echo "RESULT: FAIL — the acceptance gate's OWN controls ([G]) did not all hold, so no gate verdict"
  echo "               produced in this round is admissible. See the [G] FAIL lines above."
  exit 1
fi
if [ "$NC_FAILED" -ne 0 ]; then
  echo "RESULT: FAIL — a control did not behave as pre-registered, or a verdict was read from a"
  echo "               stale artifact. See the per-arm FAIL lines above."
  exit 1
fi
echo "RESULT: PASS — fixed arm GREEN 12/12; nc1 probe-refused (validity, 2 named reasons) + structural"
echo "               rc=2 RED at the gate (pre-S5 equivalence: $EQWORD);"
echo "               nc2 probe-refused (validity); nc3 probe-refused (validity, PARTIAL, item-4) +"
echo "               structural rc=2; nc4 judgement RED ${NC4_RED}/12;"
echo "               nc5 probe-refused (validity, item-2 absolute-timing criterion)."
