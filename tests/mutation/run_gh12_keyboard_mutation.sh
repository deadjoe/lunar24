#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh12_keyboard_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the
# GH#12 keyboard-product-owner fix (@Kimi option A, msg 53c97177; GO msg e8b073c2).
#
# The oracle (tests/probes/gh12_keyboard_owner_probe.cpp, created under the @Kimi approval) drives
# the REAL MachineRuntimeDefinition through the real dispatch (EventTimebase -> per-sample
# applyControlEvent_ -> processFrame -> step_) and asserts the GH#12 CONTRACT: a canonical note
# (pitch + pressure + gate_on, the exact set InputStateMachine::translate emits) becomes a playable
# pitched voice on VCO-A/EG-A (AC-1 pitch ~1.0 V + VCO-A dryA frequency ~doubles, AC-2 gate -> EG,
# AC-3 audio, AC-4 bit-identical under 64/128/256, AC-4b note_on-while-held re-pitches, AC-5 = full
# ctest). On the base head (80fb95e) the probe is RED; after the fix it is GREEN (8/8).
#
# This driver injects a DETACHED production-source mutation and proves, per defense point:
#
#   REAL code (no mutation)       -> GREEN  (the probe passes — the fix is correct),
#   MUTATED code (one regression) -> RED    (the probe trips on a SPECIFIC pinned assertion — the
#                                           fix is load-bearing at that defense point).
#
# Seven contracted defense points (per @Kimi e8b073c2 + the ea57e5a2 ruling), each an ACTUAL
# production-source mutation (never a relaxed validator), each verified against a SPECIFIC probe
# assertion (a `[FAIL] <what>` line), not merely a non-zero failure count:
#
#   NC-1 events_choke        (machine_runtime.h)   the note-events sink in applyControlEvent_ is a
#                            no-op: notes never reach the KeyboardBehaviour, so the keyboard stays
#                            idle and never publishes a pitch. Pinned `AC-1 keyboard publishes`.
#   NC-2 routes_stay_deferred (machine_definition.h) the four keyboard routes are left kDeferred and
#                            OUT of kActiveRoutes (the pre-fix topology): the keyboard publishes the
#                            note CV but the PatchGraph has no keyboard edge, so VCO-A never receives
#                            it and stays at the base frequency. Pinned `AC-1 VCO-A dryA frequency`.
#   NC-3 no_bindings         (machine_definition.h)   the ctor never calls setKeyboardBindings, so
#                            kbdVOctOut_/kbdGateOut_ stay JackId{0} and the keyboard publishes to the
#                            wrong (jack 0) buffer slot. Pinned `AC-1 keyboard publishes`.
#   NC-4 direct_cvout_write  (machine_runtime.h)   the step_ publishes the pitch to the VCO-A sink
#                            jack directly (bypasses the keyboard_v_oct_out source-of-truth), so
#                            keyboard_v_oct_out is never a first-class published source. Pinned
#                            `AC-1 keyboard publishes`.
#   NC-5 wrong_1v_oct_scale  (machine_runtime.h)   the step_ publishes pitchCv*0.5 (+1 V only gives a
#                            half-octave) instead of the V/OCT pitch. Pinned `AC-1 keyboard publishes`.
#   NC-6 drive_graph_off     (machine_runtime.h)   the processBlock control-layer drive defaults to
#                            driveGraph=false, so no CV sink is resolved from the graph and VCO-A
#                            never sees the note. Pinned `AC-1 VCO-A dryA frequency`.
#   NC-7 relevel_freeze_revert (parameter_smoothing.h) @Kimi ea57e5a2 regression-lock: revert the
#                            recompute() fix to the OLD freeze (tau_seconds_==0 -> coefficient 0.0).
#                            The keyboard portamento (product default speed=0) then never advances
#                            from its held value, so a note is published as 0 V. Pinned
#                            `AC-1 keyboard publishes`.
#
# Each splices into a DETACHED shadow header under build/ so the probe compiles against an include
# path that shadows ONLY the mutated header(s) (isolated per mutation, never left behind) — no
# tracked path is ever written and the probe source is untouched.
#
# Evidence boundary (documented): NC-2 must keep the disposition/count statics self-consistent, so it
# reverts BOTH the four dispositions AND removes the four kActiveRoutes entries (countActiveRoutes()
# drops to 1 == the sole acyclic A->B route, so the kActiveRouteCount static_assert still holds and
# the ORACLE, not a compiler guard, catches the regression at runtime). NC-4/NC-5 share the same
# publishSourceValue_ anchor line but are spliced distinctly (different shadow sets, different
# intended defect) — the script keeps them as separate defense points named and spliced apart.
#
# The probe prints `  [PASS]/[FAIL] <what>` per check and a trailing `N checks, M failures` summary,
# returning 0 iff M==0. A real regression prints the summary AND trips the pinned [FAIL] line. This
# is intentionally NOT registered in CTest: the mutated build fails by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_DEF="core/include/lunar24/core/machine_definition.h"
SRC_RT="core/include/lunar24/core/machine_runtime.h"
SRC_SMOOTH="core/include/lunar24/core/parameter_smoothing.h"
TEST="tests/probes/gh12_keyboard_owner_probe.cpp"

WORK="build/mut_gh12_keyboard"
EXE_REAL="$WORK/probe_real"
EXE_MUT="$WORK/probe_mut"

COMMON_INC=(-I"$WORK" -Icore/include -Igenerated)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_DEF" "$SRC_RT" "$SRC_SMOOTH" "$TEST"; do
  if [ ! -f "$f" ]; then echo "ERROR: $f not found (run from repo root / via this script)." >&2; exit 2; fi
done

echo "== [mutation] test the fix is load-bearing (probe, header-only core, no iPlug2) =="
rm -rf "$WORK"
mkdir -p "$WORK"

echo
echo "== [build/run] REAL baseline — expect GREEN (8 checks, 0 failures) =="
c++ -std=c++17 -O0 -Wall -Wextra -Wpedantic -Werror -o "$EXE_REAL" "$TEST" "${COMMON_INC[@]}" 2>&1 || { echo "   ERROR: REAL build failed." >&2; exit 1; }
REAL_OUT="$("$EXE_REAL" 2>&1)"
REAL_RC=$?
REAL_SUM="$(printf '%s\n' "$REAL_OUT" | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1)"
REAL_FAILS="$(printf '%s' "$REAL_SUM" | grep -oE ', [0-9]+ failures' | tr -dc '0-9')"
if [ "$REAL_RC" -ne 0 ] || [ -z "$REAL_SUM" ] || [ "${REAL_FAILS:-}" != "0" ]; then
  echo "   ERROR: REAL baseline did not pass (rc=$REAL_RC, '$REAL_SUM')." >&2
  exit 1
fi
N="$(printf '%s' "$REAL_SUM" | grep -oE '[0-9]+ checks' | grep -oE '[0-9]+')"
echo "   real: rc=$REAL_RC, $REAL_SUM ($N checks, 0 failures — the fix is correct)."

# Python splice: write a shadow of `src` at $WORK/<rel> that applies a literal find->repl.
# `need` is the exact occurrences count of `find` in the real source (must match exactly, so a
# mutated copy can never silently no-op). For need>1 every occurrence is replaced.
py_splice() {
  local src="$1" rel="$2" find="$3" repl="$4" need="$5"
  python3 - "$src" "$WORK/$rel" "$find" "$repl" "$need" <<'PY'
import os, sys
src, dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
t = open(src, encoding="utf-8").read()
n = t.count(find)
if n != need:
    sys.stderr.write(f"ERROR: anchor count {n} != expected {need} in {src}: {find[:70]}\n")
    sys.exit(2)
t = t.replace(find, repl)          # count==need, replace applies the same count
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8").write(t)
PY
}

# Python splice (multi-edit): apply a list of (find, repl) edits, each verified to occur exactly once,
# into one shadow. Used only by NC-2 (dispositions + kActiveRoutes must be reverted TOGETHER so the
# count statics stay self-consistent — the ORACLE, not a compiler guard, catches the regression).
py_splice_multi() {
  local src="$1" rel="$2"
  python3 - "$src" "$WORK/$rel" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
edits = [
    ("  {RouteId::route_keyboard_v_oct_to_vco,  RouteDisposition::kActive},",
     "  {RouteId::route_keyboard_v_oct_to_vco,  RouteDisposition::kDeferred},   /* [MUT NC-2] */"),
    ("  {RouteId::route_keyboard_gate_to_eg,    RouteDisposition::kActive},",
     "  {RouteId::route_keyboard_gate_to_eg,    RouteDisposition::kDeferred},   /* [MUT NC-2] */"),
    ("  {RouteId::route_keyboard_v_oct_to_vco_b, RouteDisposition::kActive},",
     "  {RouteId::route_keyboard_v_oct_to_vco_b, RouteDisposition::kDeferred},   /* [MUT NC-2] */"),
    ("  {RouteId::route_keyboard_gate_to_eg_b,  RouteDisposition::kActive},",
     "  {RouteId::route_keyboard_gate_to_eg_b,  RouteDisposition::kDeferred},   /* [MUT NC-2] */"),
    ("    lookupRoute(RouteId::route_keyboard_v_oct_to_vco),\n    lookupRoute(RouteId::route_keyboard_gate_to_eg),\n    lookupRoute(RouteId::route_keyboard_v_oct_to_vco_b),\n    lookupRoute(RouteId::route_keyboard_gate_to_eg_b),",
     "    /* [MUT NC-2] keyboard routes removed from kActiveRoutes — pre-fix topology. */"),
]
t = open(src, encoding="utf-8").read()
for find, repl in edits:
    n = t.count(find)
    if n != 1:
        sys.stderr.write(f"ERROR: anchor count {n} != 1 for: {find[:70]}\n")
        sys.exit(2)
    t = t.replace(find, repl)
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8").write(t)
PY
}

# Compile the probe against the current shadow set, run it, and REQUIRE a RED run that trips the
# pinned assertion `expect` (a `  [FAIL] <what>` line containing `expect`) — never "just non-zero".
# The real baseline proved the probe passes unmutated; a mutated build that (a) goes RED (rc!=0) AND
# (b) trips the exact pinned assertion proves that defense point is load-bearing. The run MUST
# terminate NORMALLY (print its `N checks, M failures` summary) — a crash/no-summary run is rejected.
run_mutation() {
  local name="$1" expect="$2"
  echo
  echo "== [mutation ${name}] splice + build + run — expect RED at '${expect}' =="
  if ! c++ -std=c++17 -O0 -Wall -Wextra -Wpedantic -Werror -o "$EXE_MUT" "$TEST" "${COMMON_INC[@]}" 2>"$WORK/err_${name}.txt"; then
    echo "   ERROR: mutation ${name} MUTATED build failed." >&2
    cat "$WORK/err_${name}.txt" >&2
    exit 1
  fi
  set +e
  MUT_OUT="$("$EXE_MUT" 2>&1)"
  MUT_RC=$?
  set -e
  if [ "$MUT_RC" -eq 0 ]; then
    echo "   ERROR: mutation ${name} did NOT go RED (rc=0). The fix is NOT load-bearing here." >&2
    exit 1
  fi
  # Normal-termination proof: the run must print the `N checks, M failures` summary. A crash would
  # leave NO such summary — reject that (a crash-residual FAIL is not a clean RED).
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1)"
  if [ -z "$MUT_SUM" ]; then
    echo "   ERROR: mutation ${name} produced no 'checks, failures' summary — abnormal termination (crash), not a clean RED." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  if ! printf '%s\n' "$MUT_OUT" | awk -v e="$expect" '
        index($0, "[FAIL]") > 0 && index($0, e) > 0 { print; found = 1 }
        END { if (!found) exit 1 }'; then
    echo "   ERROR: mutation ${name} went RED but did NOT trip the pinned assertion '${expect}'." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  MUT_FAILS="$(printf '%s' "$MUT_SUM" | grep -oE ', [0-9]+ failures' | tr -dc '0-9')"
  echo "   mutated: rc=$MUT_RC, pinned assertion trips, $MUT_SUM."
}

# -------------------------------------------------------------------------------------------------
# NC-1 — events_choke. The note-events sink in applyControlEvent_ is a no-op: notes never reach the
#      KeyboardBehaviour, so it stays idle and never publishes a pitch.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "auto kbdSink = [this](const ControlEvent& nkb) { keyboardBeh_.handleControlEvent(nkb); };"\
  "auto kbdSink = [](const ControlEvent& nkb) { (void)nkb; };   /* [MUT NC-1] notes never reach KeyboardBehaviour */" 1
run_mutation "NC-1 events_choke" "AC-1 keyboard publishes ~1.0 V pitch CV"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-2 — routes_stay_deferred. The pre-fix topology: four keyboard routes left kDeferred and out of
#      kActiveRoutes. Dispositions AND the active list are reverted together so countActiveRoutes()
#      drops to 1 == the sole acyclic A->B route (the kActiveRouteCount static_assert still holds —
#      the ORACLE, not a compiler guard, catches this at runtime).
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice_multi "$SRC_DEF" "lunar24/core/machine_definition.h"
run_mutation "NC-2 routes_stay_deferred" "AC-1 VCO-A dryA frequency ~doubles (+1 octave)"
rm -f "$WORK/lunar24/core/machine_definition.h"

# -------------------------------------------------------------------------------------------------
# NC-3 — no_bindings. The ctor never calls setKeyboardBindings, so kbdVOctOut_/kbdGateOut_ stay
#      JackId{0} and the keyboard publishes to the wrong (jack 0) buffer slot.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_DEF" "lunar24/core/machine_definition.h"\
  $'    runtime_.setKeyboardBindings(lunar24::registry::JackId::keyboard_v_oct_out,\n                                 lunar24::registry::JackId::keyboard_gate_left_main_out);'\
  $'    /* [MUT NC-3] keyboard bindings never set -> kbdVOctOut_/kbdGateOut_ stay {0}. */' 1
run_mutation "NC-3 no_bindings" "AC-1 keyboard publishes ~1.0 V pitch CV"
rm -f "$WORK/lunar24/core/machine_definition.h"

# -------------------------------------------------------------------------------------------------
# NC-4 — direct_cvout_write. The step_ publishes the pitch to the VCO-A sink jack directly
#      (bypasses the keyboard_v_oct_out source-of-truth), so keyboard_v_oct_out is never a published
#      source and VCO-A reads cvAt_(source=keyboard_v_oct_out)=0.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "        publishSourceValue_(kbdVOctOut_, pitchCv);"\
  "        publishSourceValue_(JackId::vco_a_v_oct_in, pitchCv);   /* [MUT NC-4] direct-write to VCO sink, keyboard jack never published */" 1
run_mutation "NC-4 direct_cvout_write" "AC-1 keyboard publishes ~1.0 V pitch CV"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-5 — wrong_1v_oct_scale. The step_ publishes pitchCv*0.5 (+1 V only gives a half-octave) instead
#      of the V/OCT pitch.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "        publishSourceValue_(kbdVOctOut_, pitchCv);"\
  "        publishSourceValue_(kbdVOctOut_, pitchCv * 0.5);   /* [MUT NC-5] wrong 1V/oct scale: +1 V gives half-octave */" 1
run_mutation "NC-5 wrong_1v_oct_scale" "AC-1 keyboard publishes ~1.0 V pitch CV"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-6 — drive_graph_off. The processBlock control-layer drive defaults to driveGraph=false, so no CV
#      sink is resolved from the graph and VCO-A never sees the note (AC-1 freq RED; the keyboard
#      still publishes, so AC-1 publish stays green — this is the DISTINCT control-layer bypass).
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  $'  void processBlock(const RuntimeInputs* inputs, std::size_t n, RuntimeOutput* out,\n                    bool driveGraph = true) {'\
  $'  void processBlock(const RuntimeInputs* inputs, std::size_t n, RuntimeOutput* out,\n                    bool driveGraph = false) {   /* [MUT NC-6] control layer not driven */' 1
run_mutation "NC-6 drive_graph_off" "AC-1 VCO-A dryA frequency ~doubles (+1 octave)"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-7 — release_freeze_revert (@Kimi ea57e5a2 regression-lock). Revert the recompute() fix to the OLD
#      freeze (tau_seconds_==0 -> coefficient 0.0). The keyboard portamento (product default speed=0)
#      then never advances from its held value, so a note is published as 0 V.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_SMOOTH" "lunar24/core/parameter_smoothing.h"\
  $'      if (tau_seconds_ <= 0.0) {\n        coefficient_ = 1.0;\n      } else {\n        coefficient_ = 1.0 - std::exp(-1.0 / (sample_rate_ * tau_seconds_));\n      }'\
  $'      if (tau_seconds_ <= 0.0) { coefficient_ = 0.0; } /* [MUT NC-7] pre-fix freeze */\n      else { coefficient_ = 1.0 - std::exp(-1.0 / (sample_rate_ * tau_seconds_)); }' 1
run_mutation "NC-7 relevel_freeze_revert" "AC-1 keyboard publishes ~1.0 V pitch CV"
rm -f "$WORK/lunar24/core/parameter_smoothing.h"

echo
echo "== [mutation] RESULT: unmutated probe GREEN (8/8) + all 7 defense points RED on a pinned assertion. =="
echo "   The GH#12 keyboard-owner fix is load-bearing at every contracted defense point."
