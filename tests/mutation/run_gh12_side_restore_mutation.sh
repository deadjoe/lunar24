#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh12_side_restore_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the
# GH#12 task#101 per-side keyboard restore slice (@Codex contract msg 57a5ab2a §6, review
# msg b7d63c1f groups 1-3).
#
# The oracle (tests/probes/gh12_keyboard_side_restore_probe.cpp) drives the REAL chain
#   DeviceStateV1 -> encode -> decode -> buildMachineRuntimeCandidate -> applyKeyboardState
#   -> enqueueControlEvent / InputStateMachine::translate -> processBlock
# AND the REAL host single-commit entry (EngineHarness: encode -> decode ->
# StandaloneAudioEngine::applyDeviceState -> processBlock, four captured output channels),
# and asserts the task#101 contract: one KeyboardBehaviour + one ArpSeq PER SIDE, an explicit
# internal KeyboardSide carried from the producer to every emitted event, per-side bank
# resolution (Single/Twin bank0, Split left0/right1), the four registered outputs published
# mode-correctly, the 19 already-consumed parameters read back and behaviourally
# discriminating, the two new right-side outputs driving REAL existing consumers through
# legal user cables (override + removal), real 256-frame block boundaries vs a per-frame
# reference on all four audio channels, and the preserved-field / 4-preset payload round-trip.
# On the delivered head the probe is GREEN (130 checks, 0 failures).
#
# This driver injects a DETACHED production-source mutation and proves, per defense point:
#
#   REAL code (no mutation)       -> GREEN  (the probe passes — the slice is correct),
#   MUTATED code (one regression) -> RED    (the probe trips on a SPECIFIC pinned assertion —
#                                           the slice is load-bearing at that defense point).
#
# Ten contracted defense points, each an ACTUAL production-source mutation (never a relaxed
# validator, never a fault macro in a production header), each verified against a SPECIFIC
# probe assertion (a `[FAIL] <what>` line), not merely a failure count:
#
#   NC-1 drop_side_routing   (machine_runtime.h) keyboardEventSideIndex_ collapses EVERY event
#                            onto the Left instance: the side metadata never reaches identity
#                            handling, so a right-side note never latches. Pinned `B1 Twin: two
#                            plates are two independently gated notes`.
#   NC-2 shared_perf_state   (machine_runtime.h) the right gate is published from the LEFT
#                            instance's state — the two sides share performance state instead of
#                            owning it. Pinned `B1 Twin: releasing the left note leaves the right
#                            note held`.
#   NC-3 right_reads_left_bank (machine_runtime.h) read_behaviour_params is called with Left for
#                            BOTH instances, so under Split the right side installs the left bank's
#                            non-scalar scale editor. Pinned `C1 Split: each side installs ITS OWN
#                            bank's scale editor`.
#   NC-4 skip_configure      (machine_runtime.h) applyKeyboardState never calls
#                            KeyboardBehaviour::configure: the state's behaviour half is decoded
#                            and validated but never installed. Pinned `H10
#                            keyboard.portamento_speed (117) EXECUTED as the installed glide time
#                            constant` (the same executed readback NC-9 pins: NC-4 installs
#                            nothing at all, NC-9 installs nothing but keeps the params_ mirror).
#   NC-5 right_publishes_left (machine_runtime.h) pressure_out publishes the LEFT pitch under
#                            Twin/Split instead of the right V/oct (manual BEHAVIOUR layout).
#                            Pinned `E2 Split: pressure_out is the right pitch (2.0 V), not
#                            pitch + pressure`.
#   NC-6 validator_bypassed  (machine_candidate.h) validate_device_state's verdict is IGNORED:
#                            an invalid state is no longer rejected BY THE VALIDATOR. Its true
#                            meaning is "the validator no longer rejects" — NOT "a failure is
#                            still published" (that host-side claim is NC-10, which mutates the
#                            host commit path, not the validator). Pinned `O2 an out-of-range
#                            keyboard.mode is rejected_state with no definition`.
#   NC-7 clear_at_block_boundary (machine_runtime.h) the keyboard performance state is cleared at
#                            the END of every processBlock: the same control sequence renders
#                            differently per block partition. Pinned `N1 dryA is bit-identical
#                            across 64/256/irregular`.
#   NC-8 translate_drops_side (input_state_machine.h) translate() stamps Left on every emitted
#                            ControlEvent regardless of the input's side — the producer seam
#                            loses the metadata. Pinned `P1 translate(): a left note and a right
#                            note are two independent performances`.
#   NC-9 config_not_executed (keyboard_behaviour.h) configure() keeps `params_ = p` (the verbatim
#                            readback) but SKIPS the underlying setMode/setTimes/setNorm install,
#                            so tick() runs off the stale per-behaviour state. Pinned `H10
#                            keyboard.portamento_speed (117) EXECUTED as the installed glide time
#                            constant` — the executed-config readback is load-bearing, a params_
#                            mirror alone cannot pass it.
#   NC-10 host_error_commit  (standalone_audio_engine.h) the host's format gate records the
#                            rejection but FALLS THROUGH and commits the bad format anyway: the
#                            host installs a state it was required to reject. Pinned `Q3
#                            blockSize=0 is RejectedFormat; the live owner keeps format+plan+trace`.
#
# Each splices into a DETACHED shadow header under build/ so the probe compiles against an include
# path that shadows ONLY the mutated header(s) (isolated per mutation, never left behind) — no
# tracked path is ever written and the probe source is untouched.
#
# The probe prints `  [PASS]/[FAIL] <what>` per check and a trailing `N checks, M failures` summary,
# returning 0 iff M==0. A real regression must (a) exit with EXACTLY rc=1, (b) print the summary as
# its LAST line (a crash after the summary is NOT a clean RED), and (c) trip the pinned [FAIL] line.
# This is intentionally NOT registered in CTest: the mutated build fails by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_RT="core/include/lunar24/core/machine_runtime.h"
SRC_DEF="core/include/lunar24/core/machine_definition.h"
SRC_CAND="core/include/lunar24/core/machine_candidate.h"
SRC_ISM="core/include/lunar24/core/input_state_machine.h"
SRC_KB="core/include/lunar24/core/keyboard_behaviour.h"
SRC_HOST="host/include/host/standalone_audio_engine.h"
TEST="tests/probes/gh12_keyboard_side_restore_probe.cpp"

WORK="build/mut_gh12_side_restore"
EXE_REAL="$WORK/probe_real"
EXE_MUT="$WORK/probe_mut"

# -I"$WORK" FIRST so a shadow header under $WORK wins over the real include tree. tests/host is
# the probe's own include dir (test_engine_harness.h); host/include is the real host layer the
# harness forwards to (shadowed per-mutation by $WORK/host/... for NC-10).
COMMON_INC=(-I"$WORK" -Icore/include -Igenerated -Itests/host -Ihost/include)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_RT" "$SRC_DEF" "$SRC_CAND" "$SRC_ISM" "$SRC_KB" "$SRC_HOST" "$TEST"; do
  if [ ! -f "$f" ]; then echo "ERROR: $f not found (run from repo root / via this script)." >&2; exit 2; fi
done

echo "== [mutation] test the per-side keyboard slice is load-bearing (probe, header-only core, no iPlug2) =="
rm -rf "$WORK"
mkdir -p "$WORK"

echo
echo "== [build/run] REAL baseline — expect GREEN (130 checks, 0 failures) =="
c++ -std=c++17 -O0 -Wall -Wextra -Wpedantic -Werror -o "$EXE_REAL" "$TEST" "${COMMON_INC[@]}" 2>&1 || { echo "   ERROR: REAL build failed." >&2; exit 1; }
REAL_OUT="$("$EXE_REAL" 2>&1)"
REAL_RC=$?
REAL_SUM="$(printf '%s\n' "$REAL_OUT" | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1)"
REAL_LAST="$(printf '%s\n' "$REAL_OUT" | tail -n 1)"
REAL_FAILS="$(printf '%s' "$REAL_SUM" | grep -oE ', [0-9]+ failures' | tr -dc '0-9')"
if [ "$REAL_RC" -ne 0 ] || [ -z "$REAL_SUM" ] || [ "${REAL_FAILS:-}" != "0" ] || [ "$REAL_LAST" != "$REAL_SUM" ]; then
  echo "   ERROR: REAL baseline did not pass (rc=$REAL_RC, '$REAL_SUM', last line '$REAL_LAST')." >&2
  exit 1
fi
N="$(printf '%s' "$REAL_SUM" | grep -oE '[0-9]+ checks' | grep -oE '[0-9]+')"
echo "   real: rc=$REAL_RC, $REAL_SUM ($N checks, 0 failures — the slice is correct)."

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

# Compile the probe against the current shadow set, run it, and REQUIRE a RED run that trips the
# pinned assertion `expect` (a `  [FAIL] <what>` line containing `expect`) — never "just non-zero".
# The real baseline proved the probe passes unmutated; a mutated build that (a) exits with EXACTLY
# rc=1, (b) prints its `N checks, M failures` summary as the LAST line (a crash after the summary is
# NOT a clean RED), and (c) trips the exact pinned assertion proves that defense point is load-bearing.
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
  if [ "$MUT_RC" -ne 1 ]; then
    echo "   ERROR: mutation ${name} expected EXACTLY rc=1 (clean RED), got rc=$MUT_RC." >&2
    exit 1
  fi
  # Normal-termination proof: the run must print the `N checks, M failures` summary AND that
  # summary must be the LAST line. A crash would leave no summary; a crash AFTER the summary
  # would leave trailing output — both are rejected (not a clean RED).
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1)"
  MUT_LAST="$(printf '%s\n' "$MUT_OUT" | tail -n 1)"
  if [ -z "$MUT_SUM" ]; then
    echo "   ERROR: mutation ${name} produced no 'checks, failures' summary — abnormal termination (crash), not a clean RED." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  if [ "$MUT_LAST" != "$MUT_SUM" ]; then
    echo "   ERROR: mutation ${name} printed output AFTER its summary ('$MUT_LAST') — not a clean RED." >&2
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
  echo "   mutated: rc=$MUT_RC (exactly 1), summary is the last line, pinned assertion trips, $MUT_SUM."
}

# -------------------------------------------------------------------------------------------------
# NC-1 — drop_side_routing. keyboardEventSideIndex_ collapses every performance event onto Left:
#      the side never reaches identity handling, so a right-side note never latches.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  $'  std::uint32_t keyboardEventSideIndex_(KeyboardSide side) const {\n    return (keyboardMode_ == KeyboardMode::Single) ? 0u : keyboardSideIndex_(side);\n  }'\
  $'  std::uint32_t keyboardEventSideIndex_(KeyboardSide side) const {\n    (void)side;\n    return 0u;   /* [MUT NC-1] every event routed to the Left instance */\n  }' 1
run_mutation "NC-1 drop_side_routing" "B1 Twin: two plates are two independently gated notes"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-2 — shared_perf_state. The right gate is published from the LEFT instance's state: the two
#      sides share performance state instead of owning it.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  $'          publishSourceValue_(kbdGateRightOut_,\n                              (!single && keyboardBeh_[1].gate()) ? 10.0 : 0.0);'\
  $'          publishSourceValue_(kbdGateRightOut_,\n                              (!single && keyboardBeh_[0].gate()) ? 10.0 : 0.0);   /* [MUT NC-2] right gate reads the LEFT state */' 1
run_mutation "NC-2 shared_perf_state" "B1 Twin: releasing the left note leaves the right note held"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-3 — right_reads_left_bank. read_behaviour_params is called with Left for BOTH instances, so
#      under Split the right side installs the left bank's non-scalar scale editor.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "keyboardBeh_[s].configure(read_behaviour_params(bank, scaleEditor, keyboardMode_, side),"\
  "keyboardBeh_[s].configure(read_behaviour_params(bank, scaleEditor, keyboardMode_, KeyboardSide::Left),   /* [MUT NC-3] right reads the left bank */" 1
run_mutation "NC-3 right_reads_left_bank" "C1 Split: each side installs ITS OWN bank's scale editor"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-4 — skip_configure. applyKeyboardState never calls KeyboardBehaviour::configure: the state's
#      behaviour half is decoded and validated but never installed.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  $'      keyboardBeh_[s].configure(read_behaviour_params(bank, scaleEditor, keyboardMode_, side),\n                                sampleRate_);'\
  $'      (void)read_behaviour_params(bank, scaleEditor, keyboardMode_, side);   /* [MUT NC-4] read but never installed */' 1
run_mutation "NC-4 skip_configure" "H10 keyboard.portamento_speed (117) EXECUTED as the installed glide time constant"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-5 — right_publishes_left. pressure_out publishes the LEFT pitch under Twin/Split instead of
#      the right V/oct (manual BEHAVIOUR layout).
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "          publishSourceValue_(kbdPressureOut_, single ? pressL : pitchR);"\
  "          publishSourceValue_(kbdPressureOut_, single ? pressL : pitchL);   /* [MUT NC-5] right output publishes the LEFT value */" 1
run_mutation "NC-5 right_publishes_left" "E2 Split: pressure_out is the right pitch (2.0 V), not pitch + pressure"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-6 — validator_bypassed. The candidate chain ignores validate_device_state's verdict: an
#      invalid state is no longer rejected BY THE VALIDATOR. Its true meaning is exactly that —
#      "the validator no longer rejects". It is NOT a demonstration that "a failure is still
#      published": the candidate is never built here. The host-side claim (an owner committing a
#      state it was required to reject) is NC-10, which mutates the host commit path.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_CAND" "lunar24/core/machine_candidate.h"\
  $'  const StateValidationResult v = validate_device_state(state);\n  if (!v.ok) return {MachineCandidateStatus::rejected_state, v, nullptr};'\
  $'  const StateValidationResult v = validate_device_state(state);\n  /* [MUT NC-6] the validator verdict is ignored: an invalid state is no longer rejected HERE. */' 1
run_mutation "NC-6 validator_bypassed" "O2 an out-of-range keyboard.mode is rejected_state with no definition"
rm -f "$WORK/lunar24/core/machine_candidate.h"

# -------------------------------------------------------------------------------------------------
# NC-7 — clear_at_block_boundary. The keyboard performance state is cleared at the END of every
#      processBlock: the same control sequence renders differently per block partition.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  $'      out[i] = processFrame(inputs[i], driveGraph);\n    }\n  }'\
  $'      out[i] = processFrame(inputs[i], driveGraph);\n    }\n    /* [MUT NC-7] clear keyboard performance state at every block boundary */\n    {\n      ControlEvent r{};\n      r.kind = ControlEventKind::reset;\n      applyControlEvent_(r);\n    }\n  }' 1
run_mutation "NC-7 clear_at_block_boundary" "N1 dryA is bit-identical across 64/256/irregular"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# NC-8 — translate_drops_side. translate() stamps Left on every emitted ControlEvent regardless
#      of the input's side — the producer seam loses the metadata.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_ISM" "lunar24/core/input_state_machine.h"\
  "    out[n].side = in.side;       // GH#12 task#101: the SAME side every event of this input carries"\
  "    out[n].side = KeyboardSide::Left;   /* [MUT NC-8] translate() drops the producer's side */" 1
run_mutation "NC-8 translate_drops_side" "P1 translate(): a left note and a right note are two independent performances"
rm -f "$WORK/lunar24/core/input_state_machine.h"

# -------------------------------------------------------------------------------------------------
# NC-9 — config_not_executed (review group 2). configure() keeps `params_ = p` (the verbatim
#      readback an acceptance could be fooled by) but SKIPS the underlying install, so tick()
#      runs off the stale per-behaviour state. The executed-config readback must catch it.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_KB" "lunar24/core/keyboard_behaviour.h"\
  $'    pressure_.setMode(static_cast<PressureOutput>(p.pressureOutput));\n    pressure_.setTimes(p.pressureRise, p.pressureFall);\n    portamento_.setNorm(p.portamentoSpeed, p.portamentoLegato);\n    vibrato_.setNorm(p.vibratoSpeed, p.vibratoDepth, p.vibratoDelay, p.vibratoPressure);'\
  $'    /* [MUT NC-9] `params_ = p` above is KEPT; the underlying install is skipped, so the\n       verbatim readback still reports the request while tick() runs off the stale state. */' 1
run_mutation "NC-9 config_not_executed" "H10 keyboard.portamento_speed (117) EXECUTED as the installed glide time constant"
rm -f "$WORK/lunar24/core/keyboard_behaviour.h"

# -------------------------------------------------------------------------------------------------
# NC-10 — host_error_commit (review group 1). The host's format gate records the rejection but
#      FALLS THROUGH and commits the bad format anyway: the owner installs a configuration it
#      was required to reject. The probe's live-owner atomicity checks must catch it.
rm -rf "$WORK/host"; mkdir -p "$WORK/host"
py_splice "$SRC_HOST" "host/standalone_audio_engine.h"\
  $'  if (maxBlockSize <= 0 || inputCapability < 0 || outputCapability < 2) {\n    stateApplyStatus_ = StateApplyStatus::RejectedFormat;\n    lastStateValidation_ = StateValidationResult{};\n    return StateApplyStatus::RejectedFormat;\n  }'\
  $'  if (maxBlockSize <= 0 || inputCapability < 0 || outputCapability < 2) {\n    /* [MUT NC-10] host error-commit: the rejection is recorded but execution FALLS THROUGH\n       and the bad format is committed anyway. */\n    stateApplyStatus_ = StateApplyStatus::RejectedFormat;\n    lastStateValidation_ = StateValidationResult{};\n  }' 1
run_mutation "NC-10 host_error_commit" "Q3 blockSize=0 is RejectedFormat; the live owner keeps format+plan+trace"
rm -f "$WORK/host/standalone_audio_engine.h"

echo
echo "== [mutation] RESULT: unmutated probe GREEN ($N/$N) + all 10 defense points RED on a pinned assertion (rc=1, summary last). =="
echo "   The GH#12 task#101 per-side keyboard slice is load-bearing at every contracted defense point."
