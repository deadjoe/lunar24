#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_d3_div_timing_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task #98
# (GH #15 D3 divider) directional acceptance's ISOLATED negative control.
#
# The acceptance measures the DIVISION ITSELF across TWO entries:
#   1. LIVE lane  (tests/core/test_machine_runtime.cpp::d3_div_actual_timing_acceptance) — rate +
#      divider driven by live parametric ControlEvents through the SynthRuntime processBlock path.
#   2. RESTORE lane (tests/host/test_d3_divider_restore.cpp, the @Codex dd57c783 gap-fill) — a legal
#      DeviceState's RATE/DIVIDER restored by encode -> decode -> StandaloneAudioEngine::applyDeviceState
#      and observed at the harness's per-sample seam, with the real S&H capture-frame/held sequence.
#
# Both measure the divider ratio against an INDEPENDENT LF-edge reference (round(win*rateHz/sr) from the
# LIVE rate getter, NOT the divider getter), at a LEGAL rate (norm 0.5 -> the panel default 6 Hz), over
# a long window, for integer N and the default N=8.5 (described as a mean, intervals strictly alternating
# 8/9), on drone3 AND drone6, with asymmetric no-cross-talk and restore/block-split consistency. The
# measurement is BLIND to the ratio if a test only asserts "CV is nonzero / changes"; this runner injects
# a DETACHED production mutation and proves, per target:
#
#   REAL code (no mutation)       -> GREEN  (the acceptance passes — the divider ratio is correct),
#   MUTATED code (one regression) -> RED at a SPECIFIC PINNED division assertion, exit code EXACTLY 1.
#
# Injected mutation (a REAL production regression, never a relaxed validator):
#   d3_divider_acc_doubles   (core/include/lunar24/core/machine_runtime.h)  the PapaVoice divider clock
#                            accumulator advances by 2.0 per LF edge instead of 1.0
#                            (`lfEdgeAcc_ += 1.0` -> `+= 2.0`). Each capture then happens every divN/2
#                            edges, so the capture count DOUBLES. The N=16 line of EACH target —
#                            `real captures == LF-edges/16` — is the discriminator: it expects edges/16
#                            but now observes ~2x (edges/8). The N=1 case is UNAFFECTED (capture every
#                            edge either way), which is exactly why the earlier "CV nonzero/changes"
#                            checks passed under the same mutation — only the absolute ratio reference
#                            catches it.
#
# The mutation is spliced into a DETACHED shadow header under build/ so each test compiles against an
# include path that shadows ONLY machine_runtime.h (isolated, never left behind — no tracked path is ever
# written and the test source is untouched). The mutated run must terminate NORMALLY (print the
# `N checks, M failed` summary) AND hit the PINNED assertion line AND exit with EXACTLY rc=1 (a clean
# test-fail, never a crash-residual code like 134/139). A crash that leaves no summary, or any rc != 1,
# is REJECTED — a crash-residual FAIL is not a clean RED.
#
# No product topology / migration / route / 169-classification change beyond the timer's divider clock
# (and even that only in the shadow). Verification evidence only. Intentionally NOT registered in CTest:
# the mutated build fails by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_RT="core/include/lunar24/core/machine_runtime.h"

WORK="build/mut_d3_div_timing"

# Target definitions: `name|sources|include-dirs|pin`.
#   - include-dirs are the REAL include path for that target; the mutated build prepends -I"$WORK" so the
#     shadowed machine_runtime.h wins (the shadow lives at $WORK/lunar24/core/machine_runtime.h).
#   - pin is the exact "[FAIL] <pin>" assertion the mutated run must trip (the N=16 discriminator).
TARGETS=(
  "live|tests/core/test_machine_runtime.cpp tests/core/test_machine_runtime_allocator.cpp|-Icore/include -Igenerated -Itests/core|d3 timing N=16: real captures == LF-edges/16 (integer division)"
  "restore|tests/host/test_d3_divider_restore.cpp|-Icore/include -Ihost/include -Igenerated -Itests/core -Itests/host|d3 restore N=16: real captures == LF-edges/16 (integer division)"
)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [ ! -f "$SRC_RT" ]; then echo "ERROR: $SRC_RT not found (run from repo root / via this script)." >&2; exit 2; fi

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

# Build a target against a given include prefix (empty for REAL, -I"$WORK" for MUTATED) and run it.
# Prints the target's summary line and returns 0 on success; asserts the real run is GREEN and the
# mutated run is a CLEAN RED at the pinned assertion with EXACTLY rc=1.
run_one() {
  local name="$1" sources="$2" inc="$3" pin="$4"
  local output exe

  echo
  echo "== [${name}] real baseline — expect GREEN =="
  exe="$WORK/${name}_real"
  g++ -std=c++17 -O1 -o "$exe" $sources $inc 2>"$WORK/${name}_real_err.txt" \
    || { echo "   ERROR: ${name} REAL build (g++) failed." >&2; cat "$WORK/${name}_real_err.txt" >&2; exit 1; }
  output="$("$exe" 2>&1)"
  local rc=$?
  local sum="$(printf '%s\n' "$output" | grep -oE '[0-9]+ checks, 0 failed' | tail -1)"
  if [ "$rc" -ne 0 ] || [ -z "$sum" ]; then
    echo "   ERROR: ${name} REAL build did not pass (rc=$rc, '$sum')." >&2
    printf '%s\n' "$output" | grep -E '\[FAIL\]|checks,' | tail -15 >&2
    exit 1
  fi
  echo "   real: rc=$rc, $sum (divider ratio correct)."

  echo
  echo "== [${name}] mutation #d3_divider_acc_doubles — expect RED at '${pin}' =="
  exe="$WORK/${name}_mut"
  if ! g++ -std=c++17 -O1 -o "$exe" $sources -I"$WORK" $inc 2>"$WORK/${name}_mut_err.txt"; then
    echo "   ERROR: ${name} MUTATED build (g++) failed." >&2
    cat "$WORK/${name}_mut_err.txt" >&2
    exit 1
  fi
  set +e
  output="$("$exe" 2>&1)"
  local mrc=$?
  set -e
  # REQUIRE EXACTLY rc=1 (a normal test-fail). rc=0 (no RED) or a crash-residual rc (134/139/etc) are
  # both rejected — only a clean, summary-producing RED with the pinned assertion proves load-bearing.
  if [ "$mrc" -ne 1 ]; then
    echo "   ERROR: ${name} mutation exited rc=$mrc (expected EXACTLY 1 — a clean, summary-producing RED)." >&2
    printf '%s\n' "$output" | tail -15 >&2
    exit 1
  fi
  # Normal-termination proof: must print the `N checks, M failed` summary. A crash leaves no summary.
  local msum="$(printf '%s\n' "$output" | grep -oE '[0-9]+ checks, [0-9]+ failed' | tail -1)"
  if [ -z "$msum" ]; then
    echo "   ERROR: ${name} mutation produced no 'N checks, M failed' summary — abnormal termination." >&2
    printf '%s\n' "$output" | tail -15 >&2
    exit 1
  fi
  if ! printf '%s\n' "$output" | grep -qF "[FAIL] ${pin}"; then
    echo "   ERROR: ${name} mutation went RED but did NOT trip the pinned assertion '${pin}'." >&2
    printf '%s\n' "$output" | grep 'FAIL' | tail -15 >&2
    exit 1
  fi
  local fails="$(printf '%s' "$msum" | grep -oE ', [0-9]+ failed' | grep -oE '[0-9]+')"
  echo "   mutated: rc=$mrc, pinned division assertion trips, $msum ($fails subtests fail)."
}

rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
  "lfEdgeAcc_ += 1.0" \
  "lfEdgeAcc_ += 2.0   /* [MUT #1 d3 divider acc doubles] */" 1

OVERALL=0
for entry in "${TARGETS[@]}"; do
  name="${entry%%|*}"
  rest="${entry#*|}"
  sources="${rest%%|*}"
  rest="${rest#*|}"
  inc="${rest%%|*}"
  pin="${rest#*|}"
  run_one "$name" "$sources" "$inc" "$pin" || OVERALL=1
done

echo
echo "== [restore] =="
rm -rf "$WORK"
if [ "$OVERALL" -ne 0 ]; then
  echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
  echo "-> FAIL: one or more targets did not behave as expected. See the per-target diagnostics above." >&2
  exit 1
fi
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: the D3 divider-ratio measurement (task #98) is load-bearing at the injected defense point"
echo "        on BOTH entries (live ControlEvent lane + real DeviceState restore lane); each went real=GREEN,"
echo "        mutated=CLEAN RED (rc=1) hitting its pinned N=16 assertion, reproduced repeatably."
