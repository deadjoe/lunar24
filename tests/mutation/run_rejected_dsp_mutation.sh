#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_rejected_dsp_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task#78
# (GH#12 9C) gap#1 owner-atomicity RejectedDspApply harness.
#
# The harness (tests/mutation/owner_atomicity_rejected_dsp.cpp) can ONLY reach the real
# RejectedDspApply path through a SOURCE mutation: no real DeviceStateV1 does (the validator's
# check_scalar_value is strictly-stronger-or-equal to dspParamValid_, and every direct-scalar
# applyDspParam case is "setter then return applied"). So this driver:
#
#   GENERATE  — copies core/include/lunar24/core/machine_runtime.h into a scratch dir under build/
#               and splices a CONDITIONAL rejection right after the `case ParameterId::vco_b_morph:`
#               label (BEFORE setVcoBMorph(v)), so the harness can drive a per-value rejection.
#   BUILD/RUN — compiles the harness twice and asserts:
#                 (a) MUTATED  : expect GREEN  (40 checks, 0 failures) — the four owner-atomicity
#                                sequences hold at the real RejectedDspApply path.
#                 (b) UNMUTATED: expect RED    (40 checks, 13 failures at the RejectedDspApply
#                                expectations) — proves the rejection is genuinely LOAD-BEARING and
#                                the harness is NOT vacuous (without the mutation it is not the
#                                intended path).
#   RESTORE   — removes the scratch dir. Nothing under core/ or any tracked path is ever written.
#               deterministically repeatable; run it any number of times with no residue.
#
# No product topology / migration / route / 169-classification change. Verification evidence only.
# This is intentionally NOT registered in CTest: without the mutation the harness fails by design.

set -euo pipefail

# Repo root = parent of tests/mutation (this script's directory).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC="core/include/lunar24/core/machine_runtime.h"
HARNESS="tests/mutation/owner_atomicity_rejected_dsp.cpp"

# Scratch under build/ so it never lands in the source tree or git.
WORK="build/mut_owner_atomicity"
MUT_HDR="$WORK/lunar24/core/machine_runtime.h"
EXE_MUT="$WORK/harness_mut"
EXE_REAL="$WORK/harness_real"
BLOCK="$WORK/_block.txt"

if [ ! -f "$SRC" ]; then echo "ERROR: $SRC not found (run from repo root / via this script)." >&2; exit 2; fi
if [ ! -f "$HARNESS" ]; then echo "ERROR: $HARNESS not found." >&2; exit 2; fi

echo "== [generate] build scratch + patch copy of $SRC =="
rm -rf "$WORK"
mkdir -p "$WORK/lunar24/core"

# The conditional rejection inserted between the vco_b_morph case label and its setter. Conditioned
# ONLY on the legal-B value (>= 1.0), so A's default morph (0.5) still applies and passes.
cat > "$BLOCK" <<'MUT'
        // [DETACHED MUTATION for owner-atomicity 4-sequence test] A legal value 1.0 is admitted by
        // dspParamValid_ but this setter returns a per-value failure, so applyDspState bails at this
        // id (firstFailId=vco_b_morph, firstFailStatus=invalid_value) and the candidate builder maps
        // dspApplyOk()==false to rejected_dsp_apply. Conditioned ONLY on the legal-B value (>=1.0)
        // so A's default morph (0.5) still applies and passes. Real host/validator/publish logic is
        // unchanged; this exists so the four owner-atomicity sequences can be driven at a real
        // RejectedDspApply path.
        if (v >= 1.0) { lastApplyStatus_ = ParameterApplyStatus::invalid_value; return lastApplyStatus_; }
MUT

# Copy the real header, then splice the block immediately after the unique vco_b_morph case label
# (before setVcoBMorph(v)), so the per-value rejection runs before the setter mutates any state.
# macOS awk rejects a multi-line value passed with -v, so read the block via getline < blockfile.
awk -v blockfile="$BLOCK" '
  { if ($0 ~ /case ParameterId::vco_b_morph:/) {
      print $0
      while ((getline line < blockfile) > 0) print line
      close(blockfile)
      next
    }
    print $0 }
' "$SRC" > "$MUT_HDR"

echo "   patched -> $MUT_HDR"
echo "   (sanity: exactly one vco_b_morph case label, one setVcoBMorph call)"
grep -c "case ParameterId::vco_b_morph:" "$MUT_HDR"
grep -c "setVcoBMorph(v);" "$MUT_HDR"

echo
echo "== [build/run] MUTATED build — expect GREEN (40 checks, 0 failures) =="
g++ -std=c++20 -O0 -o "$EXE_MUT" "$HARNESS" -I"$WORK" -Icore/include -Ihost/include -Igenerated
if ! "$EXE_MUT"; then
  echo "   ERROR: mutated build did NOT pass. See output above." >&2
  echo "== [restore] ==" ; rm -rf "$WORK" ; exit 1
fi

echo
echo "== [build/run] UNMUTATED build — expect RED at the RejectedDspApply expectations =="
g++ -std=c++20 -O0 -o "$EXE_REAL" "$HARNESS" -Icore/include -Ihost/include -Igenerated
# Unmutated: applyDeviceState(B) ACCEPTS the legal value, so the harness must FAIL. Assert nonzero.
set +e
"$EXE_REAL";
REAL_RC=$?
set -e
if [ "$REAL_RC" -eq 0 ]; then
  echo "   ERROR: unmutated build unexpectedly PASSED — the mutation is NOT load-bearing!" >&2
  echo "== [restore] ==" ; rm -rf "$WORK" ; exit 1
fi
echo "   unmutated exit=$REAL_RC (nonzero as required — rejection is load-bearing)"

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched."
echo "-> PASS: gap#1 RejectedDspApply owner-atomicity evidence reproduced with a repeatable tool."
