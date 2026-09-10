#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_d5_hold_or_term_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task #107
# (GH #15 D5) drone_3_hold(288) / drone_6_hold(300) -> AR envelope target OR term acceptance's
# DETACHED negative controls.
#
# The acceptance (tests/core/test_machine_runtime.cpp, section "(48) GH#15 D5 ...") measures the NEW
# OR term across SIX entries:
#   (1) d5_hold_or_term_acceptance          — HOLD is an OR term on the TARGET: hold=1 against a LOW
#                                             true gate pins the voice at exactly 1.0 at EVERY sample
#                                             while gate() KEEPS REPORTING FALSE (the three-way
#                                             discriminator: level==1.0 AND gate()==false AND
#                                             hold()==true), and hold=0 releases again.
#   (2) d5_hold_dispatch_and_range_lock     — BOTH lanes (batch applyDspParam AND the live
#                                             ControlEvent path) reach the REAL OR term, and the
#                                             selector's unit domain is locked: every value outside
#                                             {0,1} is rejected invalid_value and keeps the old state.
#   (3) d5_hold_transition_behaviour        — every row of the six-row transition table, against a
#                                             CLOSED FORM measured on the RENDER (dt/attSeconds vs
#                                             dt/rlsSeconds), plus the gate-edge invisibility proof.
#   (4) d5_hold_env_out_isolation_and_cohort— HOLD is target-only (never resets the level, never
#                                             perturbs the two stage seconds) and PER-VOICE; ENV OUT
#                                             follows the held level through the row's OWN descriptor.
#   (5) d5_default_equivalence_lock_hold    — THE REGRESSION LOCK (highest weight): an explicit hold=0
#                                             through the real batch lane is BIT-IDENTICAL to the
#                                             un-applied default on the OPEN-gate path, hold=1 against a
#                                             LOW gate reproduces the pre-D5 OPEN voice BIT-FOR-BIT, both
#                                             non-vacuity legs are asserted, and the lock carries its OWN
#                                             flipped-default detector (cable driven LOW + hold NEVER
#                                             applied -> must release per the D4 law, which a flipped
#                                             member default cannot do).
#   (6) the write-back to state_disposition.h: the two ids moved transfer_unavailable ->
#                                             applied_to_dsp (2 remaining, both GH#19 pwm).
#
# A test that only asserted "the level went up" would be BLIND to all of that. This runner injects
# DETACHED production mutations and proves, per case, that the acceptance is load-bearing:
#
#   REAL code (no mutation)       -> GREEN  (all entries carry their claim),
#   MUTATED code (one regression) -> RED at a SPECIFIC PINNED assertion, exit code EXACTLY 1.
#
# Injected mutations (REAL production regressions, never a relaxed validator):
#
#   #1 d5_hold_ignored            (machine_runtime.h)  the parameter reaches the setter and is
#      DROPPED: `void setDrone3Hold(bool on) { pv3_.setHold(on); }` -> `void setDrone3Hold(bool) { }`.
#      This is the canonical "wired the parameter, never wired the VALUE" bug. The dispatch switch is
#      untouched, so BOTH lanes still report `applied` and the id still counts as applied_to_dsp —
#      only a criterion that OBSERVES the envelope's target can see it. The pinned discriminator is
#      (1)'s hold=1-against-a-LOW-gate line. The D4-semantics line (hold=off + gate LOW still
#      releases) must STAY GREEN: the mutation removes the HOLD path, not the GATE path.
#
#   #2 d5_hold_default_flipped    (ar_envelope.h)  the ONE named member default is flipped:
#      `bool hold_ = false;` -> `= true;`. The pinned discriminator is the assertion that states the
#      invariant the flip breaks — "an UNAPPLIED hold reads back off (the registry initial selector
#      position)": the parameter is never applied, so only the MEMBER default can satisfy it.
#      FINDING (reported to @Kimi as an oracle-coverage note, then closed by his ruling — the lock
#      originally did NOT catch this flip): the "explicit hold=0 is BIT-IDENTICAL to the un-applied
#      default" leg cannot see a flipped member default, because its config A leaves the true gate
#      open (target 1.0 either way) and its config B APPLIES hold=0 before rendering. The flip is
#      caught render-level by the transition criterion's D4-law row (a LOW gate before any hold
#      apply) and by "an UNAPPLIED hold reads back off". Per @Kimi's ruling the lock additionally now
#      carries its OWN detector: `neverApplied` — cable driven LOW, hold never applied anywhere —
#      which must release per the D4 law and be bit-identical to the explicit hold=0 config. This
#      mutation therefore reds that leg directly as well as the pinned assertion; both are the lock's
#      real content now, not an over-claim in its text.
#      The D4-derived stage-second pin must STAY GREEN, which proves the flip is a targeted
#      HOLD-default regression and not a general breakage of the AR law.
#
#   #3 d5_or_term_removed         (ar_envelope.h)  the OR term is deleted from the target:
#      `const double target = (gate_ || hold_) ? 1.0 : 0.0;` -> `= gate_ ? 1.0 : 0.0;`. The parameter,
#      the readback, the dispatch and the stage seconds all still work perfectly — HOLD simply stops
#      influencing the envelope. This is the mutation that proves the slice is about the TARGET
#      EXPRESSION and not merely about storing a bit. The D4-semantics line must STAY GREEN.
#
#   #4 d5_hold_shares_rls         (ar_envelope.h)  the tick law makes the rising branch share the
#      RELEASE stage: `level_ += dt / attSeconds_;` -> `level_ += dt / rlsSeconds_;`. Because the
#      classic mapping makes the ATT and RLS norm spans numerically identical (0.001..1.0 s), a plain
#      "swap mapAttSeconds/mapRlsSeconds" mutation is UNOBSERVABLE in the readback and in any
#      single-stage closed form — so this is the detectable form of a stage merge. The readback
#      getters are untouched (attSeconds()/rlsSeconds() still report the mapped values), so the
#      mapping pins must STAY GREEN; only the RENDER criterion (3) can see it. The pinned
#      discriminator is D5's OWN §5.4 closed form — "engaging HOLD mid-release resumes the RISE at
#      dt/attSeconds from the current level" — which D4 never asserts. NOTE (honest scope): the
#      spliced anchor is the same tick line as D4's NC-4 `d4_attack_uses_release_stage`; the
#      mutation text therefore coincides, but the CLAIM under test is distinct — D4 pins the
#      gate-open ramp, D5 pins the mid-release HOLD-resume closed form, and this case exists because
#      the OR term is what makes that resume closed form reachable at all. The falling-branch D4 law
#      must STAY GREEN (the mutation touches only the rising branch).
#
# Each mutation is spliced into DETACHED shadow headers under build/ so the test compiles against an
# include path that shadows ONLY the mutated headers (isolated, never left behind — no tracked path is
# ever written and the test sources are untouched). The mutated run must terminate NORMALLY (print the
# `N checks, M failed` summary) AND hit the PINNED assertion line AND exit with EXACTLY rc=1 (a clean
# test-fail, never a crash-residual code like 134/139). A crash that leaves no summary, or any rc != 1,
# is REJECTED — a crash-residual FAIL is not a clean RED.
#
# No product topology / migration / route / disposition-class change beyond the mutated lines (and
# even those only in the shadow). Verification evidence only. Intentionally NOT registered in CTest:
# the mutated builds fail by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_RT="core/include/lunar24/core/machine_runtime.h"
SRC_AR="core/include/lunar24/core/ar_envelope.h"

WORK="build/mut_d5_hold_or_term"

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
  "live|d5_hold_ignored|d5 core: HOLD=1 against the LOW gate re-opens the voice to exactly 1.0|d5 core: hold=off + gate LOW still releases the voice (D4 semantics preserved)"
  "live|d5_hold_default_flipped|d5 core: an UNAPPLIED hold reads back off (the registry initial selector position)|d5 transition: the render path consumes the two closed-form stage seconds"
  "live|d5_or_term_removed|d5 core: HOLD=1 against the LOW gate re-opens the voice to exactly 1.0|d5 core: hold=off + gate LOW still releases the voice (D4 semantics preserved)"
  "live|d5_hold_shares_rls|d5 transition: engaging HOLD mid-release resumes the RISE at dt/attSeconds from the current level (no reset to 0)|d5 transition: hold=off, gate high->low falls at exactly dt/rlsSeconds from 1.0 (D4 law)"
)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_RT" "$SRC_AR"; do
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
    d5_hold_ignored)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "void setDrone3Hold(bool on) { pv3_.setHold(on); }" \
        "void setDrone3Hold(bool) { }  /* [MUT #1 D5 the HOLD value reaches the setter and is dropped] */" 1
      ;;
    d5_hold_default_flipped)
      py_splice "$SRC_AR" "lunar24/core/ar_envelope.h" \
        "bool hold_ = false;" \
        "bool hold_ = true;  /* [MUT #2 D5 the member HOLD default flipped on] */" 1
      ;;
    d5_or_term_removed)
      py_splice "$SRC_AR" "lunar24/core/ar_envelope.h" \
        "const double target = (gate_ || hold_) ? 1.0 : 0.0;" \
        "const double target = gate_ ? 1.0 : 0.0;  /* [MUT #3 D5 the OR term deleted from the target] */" 1
      ;;
    d5_hold_shares_rls)
      py_splice "$SRC_AR" "lunar24/core/ar_envelope.h" \
        "level_ += dt / attSeconds_;" \
        "level_ += dt / rlsSeconds_;  /* [MUT #4 D5 the rising branch shares the RELEASE stage] */" 1
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
  # The D5 section must be present and green in the REAL run (a mutation runner over a section that
  # never executes would "pass" vacuously). Count the assertions BETWEEN the D5 section header and the
  # next numbered section header, so the count cannot be satisfied by assertions from other sections.
  d5sum="$(printf '%s\n' "$output" | awk '
    /^\(48\) GH#15 D5/ { in_d5 = 1; next }
    /^\([0-9]+[a-z]?\) / { in_d5 = 0 }
    in_d5 && /^  \[(PASS|FAIL)\]/ { n++ }
    END { print n + 0 }')"
  if [ "${d5sum:-0}" -lt 90 ]; then
    echo "   ERROR: ${name} REAL run printed only ${d5sum} D5-section assertions (expected >= 90) — the D5 acceptance did not run." >&2
    exit 1
  fi
  touch "$WORK/real_${name}.ok"
  echo "   ${name}: rc=$rc, $sum (all $d5sum D5-section assertions green)."
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
echo "-> PASS: the D5 HOLD-as-OR-term acceptance (task #107) is load-bearing at every injected defense"
echo "        point: the HOLD VALUE reaching the envelope (NC-1), the ONE member default that keeps an"
echo "        untouched voice bit-identical to pre-D5 (NC-2), the OR term itself in the target"
echo "        expression (NC-3) and the ATTACK stage that the HOLD-resume closed form is built on"
echo "        (NC-4) each went real=GREEN, mutated=CLEAN RED (rc=1) hitting its pinned assertion,"
echo "        reproduced repeatably; each mutation was isolated to its own discriminator."
