#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_d3_div_timing_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the task #98
# (GH #15 D3 divider) directional acceptance's DETACHED negative controls.
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
# 8/9), on drone3 AND drone6, with asymmetric no-cross-talk, render-call-boundary consistency and
# BLOCK-SIZE invariance. The measurement is BLIND to the ratio if a test only asserts "CV is nonzero /
# changes"; this runner injects DETACHED production mutations and proves, per case:
#
#   REAL code (no mutation)       -> GREEN  (the acceptance passes — the divider ratio is correct),
#   MUTATED code (one regression) -> RED at a SPECIFIC PINNED assertion, exit code EXACTLY 1.
#
# Injected mutations (REAL production regressions, never a relaxed validator):
#
#   #1 d3_divider_acc_doubles  (core/include/lunar24/core/machine_runtime.h)  the PapaVoice divider
#      clock accumulator advances by 2.0 per LF edge instead of 1.0 (`lfEdgeAcc_ += 1.0` -> `+= 2.0`).
#      Each capture then happens every divN/2 edges, so the capture count DOUBLES. The N=16 line of
#      EACH entry — `real captures == LF-edges/16` — is the discriminator: it expects edges/16 but now
#      observes ~2x (edges/8). The N=1 case is UNAFFECTED (capture every edge either way), which is
#      exactly why the earlier "CV nonzero/changes" checks passed under the same mutation — only the
#      absolute ratio reference catches it.
#
#   #2 block_boundary_remainder_reset  (@Codex e40bdb0b)  a block-oriented regression that zeroes the
#      divider remainder at every block boundary with frames > 1 — i.e. the fractional carry between
#      captures is lost whenever the host renders more than one frame per call. Two splices, both in
#      the detached shadow: a SynthRuntime passthrough that zeroes pv3_/pv6_ lfEdgeAcc_ (the lane
#      accumulator), and a DeviceAdapter::renderBlock call to it guarded by `frames > 1`. With
#      divN_ = 8.5 and an LF edge every ~8000 frames, a 64-frame block never accumulates a group, so
#      NO capture ever fires and the published S&H CV stays at its initial value. The block=1
#      reference trajectory (renderSampled) is untouched, the audio path is untouched (the S&H CV is
#      never summed into WET), and the ratio checks are untouched — ONLY the new block-size invariance
#      criterion (F) sees it. That isolation is asserted: case #2 must NOT trip the pre-existing N=16 pin.
#
# Each mutation is spliced into DETACHED shadow headers under build/ so each test compiles against an
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
SRC_AD="core/include/lunar24/core/device_adapter.h"

WORK="build/mut_d3_div_timing"

# Target definitions: `name|sources|include-dirs`.
#   - include-dirs are the REAL include path for that target; a mutated build prepends -I"$WORK" so the
#     shadowed headers win (the shadows live at $WORK/lunar24/core/...).
TARGETS=(
  "live|tests/core/test_machine_runtime.cpp tests/core/test_machine_runtime_allocator.cpp|-Icore/include -Igenerated -Itests/core"
  "restore|tests/host/test_d3_divider_restore.cpp|-Icore/include -Ihost/include -Igenerated -Itests/core -Itests/host"
)

# Case definitions: `target|mutation|pin|must-not-trip-substring`.
#   - pin is the exact "[FAIL] <pin>" assertion the mutated run must trip.
#   - must-not-trip is a substring that must NOT appear in a [FAIL] line (isolation proof); '' = skip.
CASES=(
  "live|d3_divider_acc_doubles|d3 timing N=16: real captures == LF-edges/16 (integer division)|"
  "restore|d3_divider_acc_doubles|d3 restore N=16: real captures == LF-edges/16 (integer division)|"
  "restore|block_boundary_remainder_reset|d3 restore block-size invariance: uniform 64 partition -> S&H CV == per-sample reference at every block boundary (drone3)|d3 restore N=16"
)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [ ! -f "$SRC_RT" ] || [ ! -f "$SRC_AD" ]; then
  echo "ERROR: expected sources not found (run from repo root / via this script)." >&2
  exit 2
fi

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
    d3_divider_acc_doubles)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "lfEdgeAcc_ += 1.0" \
        "lfEdgeAcc_ += 2.0   /* [MUT #1 d3 divider acc doubles] */" 1
      ;;
    block_boundary_remainder_reset)
      py_splice "$SRC_RT" "lunar24/core/machine_runtime.h" \
        "  double drone6Divider() const { return pv6_.divider(); }" \
        "  double drone6Divider() const { return pv6_.divider(); }
  // [MUT #2 hook] shadow-only passthrough so the adapter can (wrongly) drop the divider carry.
  void mutResetDividerRemainderOnBlock() { pv3_.lfEdgeAcc_ = 0.0; pv6_.lfEdgeAcc_ = 0.0; }" 1
      py_splice "$SRC_AD" "lunar24/core/device_adapter.h" \
        "  if (!hasPlan_) return;
  for (int f = 0; f < frames; ++f) {" \
        "  if (!hasPlan_) return;
  if (frames > 1) rt.mutResetDividerRemainderOnBlock();  /* [MUT #2 block>1 divider-remainder reset] */
  for (int f = 0; f < frames; ++f) {" 1
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
  touch "$WORK/real_${name}.ok"
  echo "   ${name}: rc=$rc, $sum (divider ratio correct)."
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
echo "-> PASS: the D3 divider-ratio measurement (task #98) is load-bearing at the injected defense point"
echo "        on BOTH entries (live ControlEvent lane + real DeviceState restore lane); each went real=GREEN,"
echo "        mutated=CLEAN RED (rc=1) hitting its pinned assertion, reproduced repeatably; the block-size"
echo "        mutation is isolated to the new criterion (it does not trip the pre-existing N=16 pin)."
