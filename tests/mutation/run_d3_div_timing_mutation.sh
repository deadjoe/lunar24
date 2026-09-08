#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_d3_div_timing_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task #98
# (GH #15 D3 divider) directional acceptance's ISOLATED negative control.
#
# The acceptance test (d3_div_actual_timing_acceptance in tests/core/test_machine_runtime.cpp, added
# under the @Codex a99f6489 gap-fill) measures the DIVISION ITSELF: it drives rate + divider through the
# live codec->owner->processBlock lane at a LEGAL rate (norm 0.5 -> the panel default 6 Hz), renders a
# long window, observes the REAL S&H output, and reconciles the capture count against an INDEPENDENT
# LF-edge reference derived from the live rate getter (NOT the divider getter) — for integer N and the
# default N=8.5 (described as a mean, intervals alternating 8/9), on drone3 AND drone6, with asymmetric
# no-cross-talk and block-split/restore consistency.
#
# The acceptance is BLIND to the ratio if the test only asserts "CV is nonzero / changes" — the
# pre-existing (a)-(b) checks had exactly that blind spot. This runner injects a DETACHED production
# mutation and proves, per defense point:
#
#   REAL code (no mutation)       -> GREEN  (the acceptance passes — the divider ratio is correct),
#   MUTATED code (one regression) -> RED    (the acceptance trips a SPECIFIC pinned division assertion —
#                                           the ratio measurement is load-bearing at that defense point).
#
# Injected mutation (an ACTUAL production regression, never a relaxed validator):
#   d3_divider_acc_doubles   (core/include/lunar24/core/machine_runtime.h)  the PapaVoice divider clock
#                            accumulator advances by 2.0 per LF edge instead of 1.0
#                            (`lfEdgeAcc_ += 1.0` -> `+= 2.0`). Each capture then happens every divN/2
#                            edges, so the capture count DOUBLES. The N=16 line —
#                            `real captures == LF-edges/16` — is the discriminator: it expects edges/16
#                            but now observes ~2x (edges/8). The N=1 case is UNAFFECTED (capture every
#                            edge either way), which is exactly why the old "CV nonzero/changes" checks
#                            (which saw the ratio change for N=16 but only asserted "nonzero") passed
#                            under the same mutation — only the absolute ratio reference catches it.
#
# The mutation is spliced into a DETACHED shadow header under build/ so the test compiles against an
# include path that shadows ONLY machine_runtime.h (isolated, never left behind — no tracked path is
# ever written and the test source is untouched). The mutated run must terminate NORMALLY (print the
# `N checks, M failed` summary) and hit the PINNED assertion line — a crash that leaves no summary is
# rejected (a crash-residual FAIL is not a clean RED).
#
# No product topology / migration / route / 169-classification change beyond the timer's divider clock
# (and even that only in the shadow). Verification evidence only. Intentionally NOT registered in CTest:
# the mutated build fails by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_RT="core/include/lunar24/core/machine_runtime.h"
TEST="tests/core/test_machine_runtime.cpp"
TEST_ALLOC="tests/core/test_machine_runtime_allocator.cpp"

WORK="build/mut_d3_div_timing"
EXE_REAL="$WORK/harness_real"
EXE_MUT="$WORK/harness_mut"
COMMON_INC=(-I"$WORK" -Icore/include -Igenerated -Itests/core)

# The discriminator: the N=16 absolute-ratio assertion (captures == LF-edges/16). Under `+=2.0` the
# capture count doubles, so this trips (edges/8 far from edges/16). The N=1 line is unaffected.
PIN="d3 timing N=16: real captures == LF-edges/16 (integer division)"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_RT" "$TEST" "$TEST_ALLOC"; do
  if [ ! -f "$f" ]; then echo "ERROR: $f not found (run from repo root / via this script)." >&2; exit 2; fi
done

echo "== [generate] build scratch dirs =="
rm -rf "$WORK"
mkdir -p "$WORK"

echo
echo "== [build/run] REAL baseline — expect GREEN =="
g++ -std=c++17 -O1 -o "$EXE_REAL" "$TEST" "$TEST_ALLOC" "${COMMON_INC[@]}" 2>&1 \
  || { echo "   ERROR: REAL build (g++) failed." >&2; exit 1; }
REAL_OUT="$("$EXE_REAL" 2>&1)"
REAL_RC=$?
REAL_SUM="$(printf '%s\n' "$REAL_OUT" | grep -oE '[0-9]+ checks, 0 failed' | tail -1)"
if [ "$REAL_RC" -ne 0 ] || [ -z "$REAL_SUM" ]; then
  echo "   ERROR: REAL build did not pass (rc=$REAL_RC, '$REAL_SUM')." >&2
  exit 1
fi
N="$(printf '%s' "$REAL_SUM" | grep -oE '[0-9]+ checks' | grep -oE '[0-9]+')"
echo "   real: rc=$REAL_RC, $REAL_SUM ($N checks, 0 failures — divider ratio correct)."

# Python splice: write a shadow of `src` at $WORK/<rel> that applies a literal find->repl, requiring
# the anchor to appear exactly once (so a mutated copy can never silently no-op).
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

# Compile the test against the current shadow, run it, and REQUIRE a RED run that trips the pinned
# assertion `pin` (a `FAIL <label>` line containing `pin`) — never "just non-zero". The real baseline
# proved the test passes unmutated; a mutated build that (a) goes RED (rc!=0) AND (b) trips the exact
# pinned assertion proves that division point is load-bearing. The run MUST terminate NORMALLY (print
# the `N checks, M failed` summary): a crash that leaves no summary is NOT acceptable.
run_mutation() {
  local name="$1"
  echo
  echo "== [mutation #${name}] splice + build + run — expect RED at '${PIN}' =="
  if ! g++ -std=c++17 -O1 -o "$EXE_MUT" "$TEST" "$TEST_ALLOC" "${COMMON_INC[@]}" 2>"$WORK/err_${name}.txt"; then
    echo "   ERROR: mutation #${name} MUTATED build (g++) failed." >&2
    cat "$WORK/err_${name}.txt" >&2
    exit 1
  fi
  set +e
  MUT_OUT="$("$EXE_MUT" 2>&1)"
  MUT_RC=$?
  set -e
  if [ "$MUT_RC" -eq 0 ]; then
    echo "   ERROR: mutation #${name} did NOT go RED (rc=0). The ratio measurement is NOT load-bearing." >&2
    exit 1
  fi
  # Normal-termination proof: must print the `N checks, M failed` summary. A crash would leave no
  # such summary (the g_fail>0 short-circuit at main()'s return still runs it) — but require it.
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '[0-9]+ checks, [0-9]+ failed' | tail -1)"
  if [ -z "$MUT_SUM" ]; then
    echo "   ERROR: mutation #${name} produced no 'N checks, M failed' summary — abnormal termination." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  if ! printf '%s\n' "$MUT_OUT" | grep -qF "[FAIL] ${PIN}"; then
    echo "   ERROR: mutation #${name} went RED but did NOT trip the pinned assertion '${PIN}'." >&2
    printf '%s\n' "$MUT_OUT" | grep 'FAIL' | tail -15 >&2
    exit 1
  fi
  MUT_FAILS="$(printf '%s' "$MUT_SUM" | grep -oE ', [0-9]+ failed' | grep -oE '[0-9]+')"
  echo "   mutated: rc=$MUT_RC, pinned division assertion trips, $MUT_SUM ($MUT_FAILS subtests fail)."
}

# -------------------------------------------------------------------------------------------------
# #1 — d3_divider_acc_doubles. The PapaVoice divider accumulator advances by 2.0 per LF edge instead
#      of 1.0, so captures happen every divN/2 edges and the count doubles. The N=16 line is the
#      discriminator (edges/8 far from edges/16); the N=1 line is deliberately unaffected (capture every
#      edge either way), so only the absolute-ratio reference — not a "nonzero CV" check — catches it.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "lfEdgeAcc_ += 1.0"\
  "lfEdgeAcc_ += 2.0   /* [MUT #1 d3 divider acc doubles] */" 1
run_mutation "d3_divider_acc_doubles"
rm -f "$WORK/lunar24/core/machine_runtime.h"

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: the D3 divider-ratio measurement (task #98) is load-bearing at the injected defense point;"
echo "        real=GREEN ($N checks), mutated=RED hitting the pinned '$PIN' assertion, reproduced repeatably."
