#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_d4_ar_envelope_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task #106
# (GH #15 D4) drone_3/drone_6 ATT+RLS -> AR VCA envelope acceptance's DETACHED negative controls.
#
# The acceptance (tests/core/test_machine_runtime.cpp, section "(47) GH#15 D4 ...") measures the NEW
# Papa Srapa AR VCA envelope across SIX entries:
#   (1) d4_att_rls_mapping_acceptance      — the batch AND live lanes reach the REAL stage seconds,
#                                            against the closed form 0.001+0.999*norm (both ends),
#                                            with independent stages and an out-of-unit rejection.
#   (2) d4_gate_follows_cable             — drone_3.gate_in resolves a REAL cable through the single
#                                            sink resolver and closes/re-opens the voice.
#   (3) d4_att_rls_stage_rates_in_render  — the RENDERED level advances at dt/attSeconds (ATTACK) and
#                                            dt/rlsSeconds (RELEASE): the linear classic-drone law,
#                                            NOT an exponential and NOT a swapped stage.
#   (4) d4_env_out_publication_acceptance — the ENV OUT transfer is descriptor-driven
#                                            (nominalMin + level*(nominalMax-nominalMin)), never a
#                                            hard-coded voltage.
#   (5) d4_default_equivalence_lock       — THE REGRESSION LOCK (highest weight, @Kimi clause 1):
#                                            an unpatched gate_in must leave drone_3/6 BIT-IDENTICAL
#                                            to pre-D4, and the SAME cable driven HIGH must be
#                                            bit-identical to the unpatched default (= "default" means
#                                            OPEN, not merely "some constant"), with a driven-LOW
#                                            non-vacuity control in the same criterion.
#   (6) d4_cohort_fail_closed             — atomic fail-closed cohort admission + release semantics.
#
# A test that only asserted "the level changed / is nonzero" would be BLIND to all of that. This runner
# injects DETACHED production mutations and proves, per case, that the acceptance is load-bearing:
#
#   REAL code (no mutation)       -> GREEN  (all six carry their claim),
#   MUTATED code (one regression) -> RED at a SPECIFIC PINNED assertion, exit code EXACTLY 1.
#
# Injected mutations (REAL production regressions, never a relaxed validator):
#
#   #1 d4_env_never_applied_to_audio   (machine_runtime.h)  the AR envelope is still ticked but the
#      voice's summed audio is no longer multiplied by its gain (`*out = (a + n) * ar.level();` ->
#      `*out = (a + n);`). This is the classic "computed but never consumed" bug: every readback
#      (arLevel/arGate/stage seconds) still reports correctly and the release/attack STATE still runs,
#      so only a criterion that observes the AUDIO is allowed to see it. The driven-LOW non-vacuity
#      control of the lock is exactly that criterion: with the gain gone, config D (cable LOW) becomes
#      bit-identical to config B (unpatched), so the lock would be vacuous. The "cable held HIGH is
#      bit-identical" leg must STAY GREEN — that is the isolation proof (the mutation removes the
#      envelope from the audio, it does not change what "default" means).
#
#   #2 d4_gate_feed_severed           (machine_runtime.h)  the gate is resolved and then thrown away:
#      `pv.setVoiceGate(gateHigh);` -> `pv.setVoiceGate(DroneBank::kDefaultGroupGateOpen);`. Again
#      "computed but never consumed", for the GATE this time: the resolver, the sink latch and the
#      descriptor threshold all still run, so only the criterion that DRIVES a cable and reads the
#      voice back can see it. (2)'s -5 V close line is the discriminator. The drone_6 no-cross-talk
#      line must STAY GREEN — the mutation severs voice 0's feed, it does not smear voices together.
#
#   #3 d4_att_norm_ignored            (machine_runtime.h)  the ATTACK norm reaches the setter and is
#      dropped: `ar.setAttSeconds(DroneBank::mapAttSeconds(norm))` ->
#      `ar.setAttSeconds(DroneBank::mapAttSeconds(0.0))`. Implementation of a slice that "wires the
#      parameter" without wiring the value. The CLOSED-FORM discriminator at norm 0.5 (0.5005 s) and
#      at norm 1.0 (1.0 s) is the detector; the norm-0 line (0.001 s floor) must STAY GREEN, which is
#      exactly why the acceptance pins BOTH ENDS of the mapping instead of a single sample.
#
#   #4 d4_attack_uses_release_stage   (ar_envelope.h)  the tick law swaps a stage: the RISING branch
#      advances at `dt / rlsSeconds_` instead of `dt / attSeconds_`. Because the classic mapping makes
#      kAttNorm*Seconds and kRlsNorm*Seconds numerically identical (0.001..1.0 s), a plain
#      "swap mapAttSeconds/mapRlsSeconds" mutation is UNOBSERVABLE in BOTH the readback and the small/
#      large closed forms — so this case is the detectable form of a stage swap. The readback getters
#      are untouched (attSeconds()/rlsSeconds() still report the mapped values), so EVERY mapping
#      criterion must STAY GREEN; only the RENDER criterion (3) can see it, because with RLS norm 0.0
#      (0.001 s) the attack would clamp at 1.0 inside 48 samples instead of stopping at ~0.01998 after
#      the 480-frame window.
#
#   #5 d4_default_gate_flipped        (drone_bank.h)  the ONE named provisional default is flipped:
#      `static constexpr bool kDefaultGroupGateOpen = true;` -> `= false;`. @Kimi clause-1's explicit
#      second negative control: flipping the shared constant MUST turn the default-equivalence lock
#      RED. It is the "cable held HIGH is bit-identical to the unpatched default" leg that goes red —
#      that leg is precisely the assertion "default == OPEN" — while the pre-D4-vs-bound leg (A==B)
#      stays green (both configurations are unpatched, so both follow the flipped default equally).
#      CLOSURE NOTE (@Kimi clause 2): this flip also changes the CLASSIC drone groups 1/2/4/5, which
#      consume the same constant through DroneBank::GroupEnv. That is the point — the constant has
#      exactly ONE source and no mirror — but it means this case has no "pre-existing pin must stay
#      green" isolation proof spanning the classic section. Its must-not-trip is therefore the
#      UNRELATED mapping pin (the ATT norm 0.5 closed form), which proves the flip is a targeted
#      gate-default regression and not a general breakage.
#
# Each mutation is spliced into DETACHED shadow headers under build/ so the test compiles against an
# include path that shadows ONLY the mutated headers (isolated, never left behind — no tracked path is
# ever written and the test sources are untouched). The mutated run must terminate NORMALLY (print the
# `N checks, M failed` summary) AND hit the PINNED assertion line AND exit with EXACTLY rc=1 (a clean
# test-fail, never a crash-residual code like 134/139). A crash that leaves no summary, or any rc != 1,
# is REJECTED — a crash-residual FAIL is not a clean RED.
#
# No product topology / migration / route / 169-classification change beyond the mutated lines (and
# even those only in the shadow). Verification evidence only. Intentionally NOT registered in CTest:
# the mutated builds fail by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_RT="core/include/lunar24/core/machine_runtime.h"
SRC_AR="core/include/lunar24/core/ar_envelope.h"
SRC_DB="core/include/lunar24/core/drone_bank.h"

WORK="build/mut_d4_ar_envelope"

# Target definitions: `name|sources|include-dirs`.
#   - include-dirs are the REAL include path for that target; a mutated build prepends -I"$WORK" so the
#     shadowed headers win (the shadows live at $WORK/lunar24/core/...).
#   - the allocator TU is part of the same executable in CMakeLists.txt, so it must be compiled too.
TARGETS=(
  "live|tests/core/test_machine_runtime.cpp tests/core/test_machine_runtime_allocator.cpp|-Icore/include -Igenerated -Itests/core"
)

# Case definitions: `target|mutation|pin|must-not-trip-substring`.
#   - pin is the exact "[FAIL] <pin>" assertion the mutated run must trip.
#   - must-not-trip is a substring that must NOT appear in a [FAIL] line (isolation proof); '' = skip.
CASES=(
  "live|d4_env_never_applied_to_audio|D4 lock is NOT vacuous: the same cable driven LOW does change the drone_3 channel|D4 lock: a gate_in cable held HIGH"
  "live|d4_gate_feed_severed|gate_in at -5 V (< the 0 V threshold) closes the drone_3 voice gate|the UNPATCHED drone_6 voice is untouched by the drone_3 cable"
  "live|d4_att_norm_ignored|d3 att norm=0.5 -> 0.5005 s on the REAL stage (closed form 0.001+0.999*0.5)|d3 att norm=0 -> the classic 0.001 s floor"
  "live|d4_attack_uses_release_stage|the gate-open ramp advances at dt/attSeconds from the mapped ATT norm (linear, not exponential)|d3 att norm=0.5 -> 0.5005 s on the REAL stage"
  "live|d4_default_gate_flipped|D4 lock: a gate_in cable held HIGH is BIT-IDENTICAL to the unpatched default (default == OPEN)|d3 att norm=0.5 -> 0.5005 s on the REAL stage"
)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_RT" "$SRC_AR" "$SRC_DB"; do
  if [ ! -f "$f" ]; then
    echo "ERROR: expected source not found: $f (run from repo root / via this script)." >&2
    exit 2
  fi
done

target_sources() {
  local want="$1" entry name rest
  for entry in "${TARGETS[@]}"; do
    name="${entry%%|*}"
    if [ "$name" = "$want" ]; then rest="${entry#*|}"; printf '%s\n' "${rest%%|*}"; return 0; fi
  done
  return 1
}
target_inc() {
  local want="$1" entry name rest
  for entry in "${TARGETS[@]}"; do
    name="${entry%%|*}"
    if [ "$name" = "$want" ]; then rest="${entry#*|}"; printf '%s\n' "${rest#*|}"; return 0; fi
  done
  return 1
}

echo "== [generate] build scratch dirs =="
rm -rf "$WORK"
mkdir -p "$WORK"

# Python splice: write a shadow of `src` at $WORK/<rel> that applies a literal find->repl, requiring
# the anchor to appear EXACTLY ONCE (so a mutated copy can never silently no-op).
py_splice() {
  local src="$1" rel="$2" find="$3" repl="$4" need="$5"
  python3 - "$src" "$WORK/$rel" "$find" "$repl" "$need" <<'PY'
import os, sys
src, dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
t = open(src, encoding="utf-8").read()
n = t.count(find)
if n != need:
    sys.stderr.write(f"ERROR: anchor count {n} != expected {need} in {src}: {find[:60]}\n")
    sys.exit(2)
t = t.replace(find, repl)          # count==need, replace applies the same count
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8").write(t)
PY
}

# Write the detached shadow(s) for one named mutation into $WORK/lunar24/... (fresh each time).
apply_mutation() {
  local name="$1"
  rm -rf "$WORK/lunar24"
  case "$name" in
    d4_env_never_applied_to_audio)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "*out = (a + n) * ar.level();" \
        "*out = (a + n);  /* [MUT #1 D4 env computed but never applied to the audio] */" 1
      ;;
    d4_gate_feed_severed)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "pv.setVoiceGate(gateHigh);" \
        "pv.setVoiceGate(DroneBank::kDefaultGroupGateOpen);  /* [MUT #2 D4 resolved gate discarded] */" 1
      ;;
    d4_att_norm_ignored)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "void setAttNorm(double norm) { ar.setAttSeconds(DroneBank::mapAttSeconds(norm)); }" \
        "void setAttNorm(double) { ar.setAttSeconds(DroneBank::mapAttSeconds(0.0)); }  /* [MUT #3 D4 att norm dropped] */" 1
      ;;
    d4_attack_uses_release_stage)
      py_splice "$SRC_AR" "lunar24/core/ar_envelope.h" \
        "level_ += dt / attSeconds_;" \
        "level_ += dt / rlsSeconds_;  /* [MUT #4 D4 attack advances at the release stage] */" 1
      ;;
    d4_default_gate_flipped)
      py_splice "$SRC_DB" "lunar24/core/drone_bank.h" \
        "static constexpr bool kDefaultGroupGateOpen = true;" \
        "static constexpr bool kDefaultGroupGateOpen = false;  /* [MUT #5 D4 default gate flipped] */" 1
      ;;
    *)
      echo "ERROR: unknown mutation '$name'" >&2
      exit 2
      ;;
  esac
}

# ---- phase 1: real baselines (one per target, expect GREEN) --------------------------------
echo
echo "== [phase 1] real baselines — expect GREEN =="
for entry in "${TARGETS[@]}"; do
  name="${entry%%|*}"; rest="${entry#*|}"
  sources="${rest%%|*}"; inc="${rest#*|}"
  exe="$WORK/${name}_real"
  g++ -std=c++17 -O1 -o "$exe" $sources $inc 2>"$WORK/${name}_real_err.txt" \
    || { echo "   ERROR: ${name} REAL build (g++) failed." >&2; cat "$WORK/${name}_real_err.txt" >&2; exit 1; }
  output="$("$exe" 2>&1)"
  rc=$?
  sum="$(printf '%s\n' "$output" | grep -oE '[0-9]+ checks, 0 failed' | tail -1)"
  if [ "$rc" -ne 0 ] || [ -z "$sum" ]; then
    echo "   ERROR: ${name} REAL build did not pass (rc=$rc, '$sum')." >&2
    printf '%s\n' "$output" | grep -E '\[FAIL\]|checks,' | tail -15 >&2
    exit 1
  fi
  # The D4 section must be present and green in the REAL run (a mutation runner over a section that
  # never executes would "pass" vacuously). Count the assertions BETWEEN the D4 section header and the
  # next numbered section header, so the count cannot be satisfied by assertions from other sections.
  d4sum="$(printf '%s\n' "$output" | awk '
    /^\(47\) GH#15 D4/ { in_d4 = 1; next }
    /^\([0-9]+\) /     { in_d4 = 0 }
    in_d4 && /^  \[(PASS|FAIL)\]/ { n++ }
    END { print n + 0 }')"
  if [ "${d4sum:-0}" -lt 60 ]; then
    echo "   ERROR: ${name} REAL run printed only ${d4sum} D4-section assertions (expected >= 60) — the D4 acceptance did not run." >&2
    exit 1
  fi
  touch "$WORK/real_${name}.ok"
  echo "   ${name}: rc=$rc, $sum (all $d4sum D4-section assertions green)."
done

# ---- phase 2: mutated builds (expect a clean, pinned RED at exactly rc=1) -------------------
echo
echo "== [phase 2] mutated builds — expect CLEAN RED at the pinned assertion, rc=1 =="
for entry in "${CASES[@]}"; do
  tname="${entry%%|*}"; rest="${entry#*|}"
  mut="${rest%%|*}"; rest="${rest#*|}"
  pin="${rest%%|*}"; mustnot="${rest#*|}"

  if [ ! -f "$WORK/real_${tname}.ok" ]; then
    echo "   ERROR: no real baseline recorded for target '${tname}'." >&2
    exit 1
  fi
  sources="$(target_sources "$tname")"
  inc="$(target_inc "$tname")"

  echo
  echo "== [${tname} / ${mut}] mutation applied — expect RED at '${pin}' =="
  apply_mutation "$mut"
  exe="$WORK/${tname}_mut_${mut}"
  if ! g++ -std=c++17 -O1 -o "$exe" $sources -I"$WORK" $inc 2>"$WORK/err_${tname}_${mut}.txt"; then
    echo "   ERROR: ${tname}/${mut} MUTATED build (g++) failed." >&2
    cat "$WORK/err_${tname}_${mut}.txt" >&2
    exit 1
  fi
  set +e
  output="$("$exe" 2>&1)"
  mrc=$?
  set -e
  # REQUIRE EXACTLY rc=1 (a normal test-fail). rc=0 (no RED) or a crash-residual rc (134/139/etc) are
  # both rejected — only a clean, summary-producing RED with the pinned assertion proves load-bearing.
  if [ "$mrc" -ne 1 ]; then
    echo "   ERROR: ${tname}/${mut} mutation exited rc=$mrc (expected EXACTLY 1 — a clean, summary-producing RED)." >&2
    printf '%s\n' "$output" | tail -15 >&2
    exit 1
  fi
  # Normal-termination proof: must print the `N checks, M failed` summary. A crash leaves no summary.
  msum="$(printf '%s\n' "$output" | grep -oE '[0-9]+ checks, [0-9]+ failed' | tail -1)"
  if [ -z "$msum" ]; then
    echo "   ERROR: ${tname}/${mut} mutation produced no 'N checks, M failed' summary — abnormal termination." >&2
    printf '%s\n' "$output" | tail -15 >&2
    exit 1
  fi
  if ! printf '%s\n' "$output" | grep -qF "[FAIL] ${pin}"; then
    echo "   ERROR: ${tname}/${mut} mutation went RED but did NOT trip the pinned assertion '${pin}'." >&2
    printf '%s\n' "$output" | grep 'FAIL' | tail -15 >&2
    exit 1
  fi
  # Isolation proof: a targeted mutation must not also trip the pre-existing discriminator.
  if [ -n "$mustnot" ] && printf '%s\n' "$output" | grep -qF "[FAIL] ${mustnot}"; then
    echo "   ERROR: ${tname}/${mut} mutation is NOT isolated — it also tripped the pre-existing pin '${mustnot}'." >&2
    printf '%s\n' "$output" | grep 'FAIL' | tail -15 >&2
    exit 1
  fi
  fails="$(printf '%s' "$msum" | grep -oE ', [0-9]+ failed' | grep -oE '[0-9]+')"
  echo "   ${tname}/${mut}: rc=$mrc, pinned assertion trips, $msum ($fails subtests fail)."
done

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: the D4 AR VCA envelope acceptance (task #106) is load-bearing at every injected defense"
echo "        point: the envelope's AUDIO application (NC-1), the gate FEED (NC-2), the ATT norm VALUE"
echo "        (NC-3), the ATTACK stage choice in the tick law (NC-4) and the ONE shared provisional"
echo "        gate default (NC-5) each went real=GREEN, mutated=CLEAN RED (rc=1) hitting its pinned"
echo "        assertion, reproduced repeatably; each mutation was isolated to its own discriminator."
