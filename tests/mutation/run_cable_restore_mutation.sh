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
# harness must go RED. This driver injects a DETACHED source mutation into the restore block and
# proves that:
#
#   REAL code (no mutation)      -> GREEN  (the harness passes — the restore is correct),
#   MUTATED code (one regression)-> RED    (the harness trips — the restore is load-bearing).
#
# Two isolated mutations hit the two distinct defense points of the restore block:
#
#   #1 skip_restore_cable (machine_definition.h:414)  — the restore never calls connect(), so the
#        requested cable is never placed. The post-placement verify's cableCount()!=requested
#        (count) check must catch it -> whole-candidate reject (load()==false). Without the harness
#        this regression would silently hand back a machine missing a requested wire.
#   #2 miswire_source      (machine_definition.h:412)  — the restore connects the WRONG source. The
#        verify's cableConnected(requested_source, sink) (source check) must catch it, even though the
#        cable COUNT matches (a wrong-source wire honors count but is not the requested wire).
#
# Both are spliced into a DETACHED copy of machine_definition.h under build/ so the harness is
# compiled against an include path that shadows the real header — no tracked path is ever written.
# The ONLY observable change to the harness is the shadowed header; the harness source is untouched.
#
# Evidence boundaries (documented, mirroring gap#1 run_rejected_dsp_mutation.sh):
#   * "capacity replace silent drop" is NOT drivable through a validation-coherent DeviceState. The
#     validator's source/sink per-jack cardinality gate (source_used <= maxCables, check_cables)
#     rejects an over-subscribed source BEFORE the factory builds, and the single active normalized
#     route (VCO-B self-edge) is carried in a NORMALIZED-route structure separate from the user-cable
#     bank (patch_graph.h / normalizedActive — probe: cableCountInto(vco_b_cv_in)==0 while
#     normalizedActive==1). So user-cable connect() can neither displace a requested cable nor displace
#     the route through a valid state; the verify's count/source defenses are the reachable gate.
#   * "ignore override" is the validator's check_routes responsibility (routeOverridden bits reconciled
#     against actual cable facts) and is enforced BEFORE the factory builds; the restore does not
#     consult routeOverridden by design and cannot regress it. The route-suppression itself is covered
#     by harness (b).
#
# No product topology / migration / route / 169-classification change. Verification evidence only.
# This is intentionally NOT registered in CTest: the mutated build fails by design.

set -euo pipefail

# Repo root = parent of tests/mutation (this script's directory).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC="core/include/lunar24/core/machine_definition.h"
TEST="tests/host/test_machine_cable_restore.cpp"

# Scratch under build/ so it never lands in the source tree or git.
WORK="build/mut_cable_restore"
MUT_HDR="$WORK/lunar24/core/machine_definition.h"
EXE_REAL="$WORK/harness_real"
EXE_MUT="$WORK/harness_mut"

COMMON_INC=(-I"$WORK" -Icore/include -Ihost/include -Igenerated -Itests/core -Itests/host)

# Always restore the scratch dir, even on a compile/run failure or interrupt — a failed run must
# never leave a mutated copy behind (only restore, never touch the source tree).
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [ ! -f "$SRC" ]; then echo "ERROR: $SRC not found (run from repo root / via this script)." >&2; exit 2; fi
if [ ! -f "$TEST" ]; then echo "ERROR: $TEST not found." >&2; exit 2; fi

echo "== [generate] build scratch dirs =="
rm -rf "$WORK"
mkdir -p "$WORK/lunar24/core"

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

# Splice the mutation into the shadow copy and compile/run the harness against it, expecting RED.
# The REAL baseline proved the harness passes unmutated; the MUTATED build must now TRIP (rc!=0) and
# print the FAILED summary (proving the regression is load-bearing), not crash.
run_mutation() {
  local name="$1" match="$2" repl="$3"
  echo
  echo "== [mutation #${name}] splice + build + run — expect RED =="
  # Copy the real header, then replace the unique target line. index() is a literal substring match
  # (no regex escaping needed); the replacement is printed verbatim.
  awk -v m="$match" -v r="$repl" '
    index($0, m) { print r; next }
    { print }
  ' "$SRC" > "$MUT_HDR"

  # Sanity: exactly one occurrence of the target in BOTH the real source and the shadow copy. Use a
  # FIXED string (-F) — the anchors contain regex-special chars (parens, brackets) that must be searched
  # literally; the awk index() below is already a literal substring match, so this must agree with it.
  local real_hits mut_hits
  real_hits="$(grep -F -c -- "$match" "$SRC" || true)"
  mut_hits="$(grep -F -c -- "$match" "$MUT_HDR" || true)"
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
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '\[test_machine_cable_restore\] [0-9]+/[0-9]+ checks FAILED' | tail -1)"
  if [ "$MUT_RC" -eq 0 ]; then
    echo "   ERROR: mutation #${name} did NOT go RED (rc=0). The restore is NOT load-bearing for this regression." >&2
    exit 1
  fi
  if [ -z "$MUT_SUM" ]; then
    echo "   ERROR: mutation #${name} did not reach the FAILED summary (crash / early abort). rc=$MUT_RC" >&2
    printf '%s\n' "$MUT_OUT" | tail -20 >&2
    exit 1
  fi
  MUT_FAILS="$(printf '%s' "$MUT_SUM" | grep -oE '[0-9]+/' | tr -d '/')"
  MUT_TOTAL="$(printf '%s' "$MUT_SUM" | grep -oE '/[0-9]+ ' | tr -d '/ ')"
  if [ "${MUT_FAILS:-0}" -le 0 ]; then
    echo "   ERROR: mutation #${name} reported '$MUT_SUM' but 0 failures." >&2
    exit 1
  fi
  echo "   mutated: rc=$MUT_RC, $MUT_SUM ($MUT_FAILS of $MUT_TOTAL checks FAIL — the regression is load-bearing)."
}

run_mutation "skip_restore_cable" \
  "!runtime_.connect(source, sink)" \
  "        if (false) { restoreFailed = true; break; }   // [MUTATION #1] connect never called"

run_mutation "miswire_source" \
  "const JackId source = state_.cableSource[i];" \
  "        const JackId source = static_cast<JackId>(0);   // [MUTATION #2] wrong source"

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: task#80 cable-restore is load-bearing; real=GREEN ($N checks), mutated=RED, reproduced repeatably."
