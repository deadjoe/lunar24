#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_cable_restore_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task#80
# (GH#12 9D C3) user-cable atomic-restore harness.
#
# The harness (tests/host/test_machine_cable_restore.cpp) drives encode -> decode ->
# StandaloneAudioEngine.applyDeviceState -> processBlock and asserts the restore contract (a)-(f).
# Its job is to prove the restore is LOAD-BEARING: if the production restore logic regresses, the
# harness must go RED. This driver injects a DETACHED production-source mutation and proves that:
#
#   REAL code (no mutation)      -> GREEN  (the harness passes — the restore is correct),
#   MUTATED code (one regression)-> RED    (the harness trips on a SPECIFIC assertion — the restore is
#                                           load-bearing at that defense point).
#
# @Codex (d865b9e5, item 4) requires the mutation set to cover FIVE contracted defense points, each
# as an ACTUAL production-source mutation (never a relaxed production validator), and each verified
# against a SPECIFIC assertion (the exact FAIL line), not merely a non-zero failure count:
#
#   #1 skip_restore_cable   (machine_definition.h:414)  restore never calls connect(). The
#        post-placement verify's cableCount()!=requested (count) gate must catch it -> whole-candidate
#        reject. Pinned assertion: `h.load(wired)` (test (b) first cable-bearing load returns false).
#   #2 miswire_source       (machine_definition.h:412)  restore connects the WRONG source. The verify's
#        cableConnected(requested_source,sink) (source) gate must catch it EVEN THOUGH the cable COUNT
#        matched. Pinned assertion: `cableConnected(env_follower_env_out, vco_b_cv_in)`.
#   #3 ignore_override      (patch_graph.h:179)         normalizedActive() ignores the countInto==0
#        gate (treats the route active despite a user cable overriding its sink). The harness (b) must
#        detect that a cable failed to override the self-edge. Pinned assertion:
#        `!(normalizedActive(vco_b_vco_out, vco_b_cv_in))` (the "route is OFF" check).
#   #4 graph_fail_still_publish (machine_candidate.h:90) the factory ACCEPTS a definition whose graph
#        failed to compile (the valid() gate is neutralised). The harness (f) must still see a TYPED
#        RejectedGraph from the real owner. Pinned assertion: `applyStatus()==RejectedGraph`.
#   #5 capacity_drop        (machine_definition.h:414)  connect() DOES return true (the cable is
#        placed) but a requested cable is immediately dropped; the verify's count gate must reject the
#        whole candidate — proving the restore does NOT trust a lone connect()==true as complete.
#        Pinned assertion: `h.load(wired)`.
#
# #1/#2/#5 splice machine_definition.h, #3 splices patch_graph.h, #4 splices machine_candidate.h.
# Each is spliced into a DETACHED shadow header under build/ so the harness compiles against an
# include path that shadows ONLY that one header (isolated per mutation, never left behind) — no
# tracked path is ever written and the harness source is untouched.
#
# Evidence boundaries (documented):
#   * A validation-coherent DeviceState cannot drive a "capacity replace / silent drop" through a real
#     over-subscribed source: the validator's per-jack cardinality gate rejects over-capacity BEFORE
#     the factory builds, and the single active normalized route (VCO-B self-edge) lives in a
#     NORMALIZED-route structure SEPARATE from the user-cable bank (patch_graph.h / normalizedActive —
#     probe: cableCountInto(vco_b_cv_in)==0 while normalizedActive==1). So the reachable restore
#     defenses are exactly the count / source / override / publish gates exercised by #1-#5.
#   * "ignore override" as a ROUTE-DECISION is the validator's check_routes responsibility and is
#     enforced BEFORE the factory builds; #3 mutates the QUERY the harness (b) actually reads, so it
#     proves the harness itself would catch an override-suppression regression.
#
# No product topology / migration / route / 169-classification change. Verification evidence only.
# This is intentionally NOT registered in CTest: the mutated build fails by design.

set -euo pipefail

# Repo root = parent of tests/mutation (this script's directory).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_DEF="core/include/lunar24/core/machine_definition.h"
SRC_GRAPH="core/include/lunar24/core/patch_graph.h"
SRC_CAND="core/include/lunar24/core/machine_candidate.h"
TEST="tests/host/test_machine_cable_restore.cpp"

# Scratch under build/ so it never lands in the source tree or git.
WORK="build/mut_cable_restore"
EXE_REAL="$WORK/harness_real"
EXE_MUT="$WORK/harness_mut"

COMMON_INC=(-I"$WORK" -Icore/include -Ihost/include -Igenerated -Itests/core -Itests/host)

# Always restore the scratch dir, even on a compile/run failure or interrupt — a failed run must
# never leave a mutated copy behind (only restore, never touch the source tree).
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_DEF" "$SRC_GRAPH" "$SRC_CAND" "$TEST"; do
  if [ ! -f "$f" ]; then echo "ERROR: $f not found (run from repo root / via this script)." >&2; exit 2; fi
done

echo "== [generate] build scratch dirs =="
rm -rf "$WORK"
mkdir -p "$WORK"

echo
echo "== [build/run] REAL baseline — expect GREEN =="
g++ -std=c++17 -O0 -o "$EXE_REAL" "$TEST" "${COMMON_INC[@]}" 2>&1 || { echo "   ERROR: REAL build (g++) failed." >&2; exit 1; }
REAL_OUT="$("$EXE_REAL" 2>&1)"
REAL_RC=$?
REAL_SUM="$(printf '%s\n' "$REAL_OUT" | grep -oE '\[test_machine_cable_restore\] [0-9]+ checks OK' | tail -1)"
if [ "$REAL_RC" -ne 0 ] || [ -z "$REAL_SUM" ]; then
  echo "   ERROR: REAL build did not pass (rc=$REAL_RC, '$REAL_SUM')." >&2
  exit 1
fi
N="$(printf '%s' "$REAL_SUM" | grep -oE '[0-9]+ checks OK' | grep -oE '[0-9]+')"
echo "   real: rc=$REAL_RC, $REAL_SUM ($N checks, 0 failures — restore correct)."

# Splice ONE production-source mutation into an ISOLATED shadow header, compile/run the harness
# against it, and REQUIRE the red run to expose the SPECIFIC pinned assertion (a `FAIL <file>:<line>:
# <expr>` line) — never "just non-zero". The real baseline proved the harness passes unmutated; a
# mutated build that (a) goes RED (rc!=0) AND (b) trips the exact pinned assertion proves that defense
# point is load-bearing. rc!=0 is checked because an old-style `if(false)` regression could crash; the
# mini_test record-and-continue still prints the pinned FAIL line before any later early-exit, so the
# assertion check is robust to a follow-on abort.
run_mutation() {
  local name="$1" src="$2" match="$3" repl="$4" expect="$5"
  echo
  echo "== [mutation #${name}] ($src) splice + build + run — expect RED at '$expect' =="
  # Isolate: only THIS mutation's shadow header may be on the include path (drop any leftover shadow
  # from a prior mutation so #3/#4's patch_graph.h / machine_candidate.h runs are not contaminated).
  rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
  local rel="${src#core/include/}"            # e.g. lunar24/core/patch_graph.h
  local mut_hdr="$WORK/$rel"
  # index() is a literal substring match (no regex escaping needed); the replacement prints verbatim.
  awk -v m="$match" -v r="$repl" 'index($0, m) { print r; next } { print }' "$src" > "$mut_hdr"

  # Sanity: the target appears EXACTLY once in the real source and ZERO times in the shadow copy.
  # Fixed-string (-F) because the anchors contain regex-special chars (parens, brackets, `==`).
  local real_hits mut_hits
  real_hits="$(grep -F -c -- "$match" "$src" || true)"
  mut_hits="$(grep -F -c -- "$match" "$mut_hdr" || true)"
  if [ "$real_hits" != "1" ] || [ "$mut_hits" != "0" ]; then
    echo "   ERROR: mutation #${name} splice anchor not unique (real=$real_hits shadow=$mut_hits)." >&2
    exit 1
  fi

  if ! g++ -std=c++17 -O0 -o "$EXE_MUT" "$TEST" "${COMMON_INC[@]}" 2>"$WORK/err_$name.txt"; then
    echo "   ERROR: mutation #${name} MUTATED build (g++) failed." >&2
    cat "$WORK/err_$name.txt" >&2
    exit 1
  fi
  set +e
  MUT_OUT="$("$EXE_MUT" 2>&1)"
  MUT_RC=$?
  set -e
  if [ "$MUT_RC" -eq 0 ]; then
    echo "   ERROR: mutation #${name} did NOT go RED (rc=0). The restore is NOT load-bearing here." >&2
    exit 1
  fi
  # The SPECIFIC assertion: a FAIL line that contains the pinned expression. (mini_test writes
  # `FAIL <file>:<line>: <expr>` to stderr immediately, record-and-continue.)
  if ! printf '%s\n' "$MUT_OUT" | awk -v e="$expect" '
        index($0, "FAIL ") > 0 && index($0, e) > 0 { print; found = 1 }
        END { if (!found) exit 1 }'; then
    echo "   ERROR: mutation #${name} went RED but did NOT trip the pinned assertion '$expect'." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '\[test_machine_cable_restore\] [0-9]+/[0-9]+ checks FAILED' | tail -1)"
  if [ -n "$MUT_SUM" ]; then
    MUT_FAILS="$(printf '%s' "$MUT_SUM" | grep -oE '[0-9]+/' | head -1 | tr -d '/')"
    echo "   mutated: rc=$MUT_RC, pinned assertion trips, $MUT_SUM ($MUT_FAILS subtests fail)."
  else
    echo "   mutated: rc=$MUT_RC, pinned assertion trips (no FAILED summary line — assertion fired before a follow-on abort)."
  fi
}

run_mutation "skip_restore_cable" "$SRC_DEF" \
  "if (!runtime_.connect(source, sink)) { restoreFailed = true; break; }" \
  "        if (false) { restoreFailed = true; break; }   /* [MUT #1 skip] connect never called */" \
  "h.load(wired)"

run_mutation "miswire_source" "$SRC_DEF" \
  "const JackId source = state_.cableSource[i];" \
  "        const JackId source = static_cast<JackId>(0);   /* [MUT #2 miswire] wrong source */" \
  "cableConnected(JackId::env_follower_env_out, JackId::vco_b_cv_in)"

run_mutation "ignore_override" "$SRC_GRAPH" \
  "return routeExists(source, sink) && countInto(sink) == 0u;" \
  "    return routeExists(source, sink);   /* [MUT #3 ignore override] */" \
  "!(h.runtime()->normalizedActive(JackId::vco_b_vco_out, JackId::vco_b_cv_in))"

run_mutation "graph_fail_still_publish" "$SRC_CAND" \
  "if (!definition->valid())" \
  "  if (false)   /* [MUT #4 graph fail still publishes] */" \
  "StateApplyStatus::RejectedGraph"

run_mutation "capacity_drop" "$SRC_DEF" \
  "if (!runtime_.connect(source, sink)) { restoreFailed = true; break; }" \
  "        runtime_.connect(source, sink); runtime_.disconnect(source, sink); if (false) { restoreFailed = true; break; }   /* [MUT #5 capacity drop] */" \
  "h.load(wired)"

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: task#80 cable-restore is load-bearing at all 5 contracted defense points;"
echo "        real=GREEN ($N checks), each mutated=RED with a pinned specific assertion, reproduced repeatably."
