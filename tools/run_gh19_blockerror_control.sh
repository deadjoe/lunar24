#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_blockerror_control.sh — task #86 / GH#19 BLOCK-ERROR negative control
# (the "real cross-block-state corruption" that a blockpart_max>0 must catch).
#
# @Codex ab5315a6: `blockpart_max=0` on the correct BLAMP path is a NORMAL result, NOT
# a negative control. This tool proves the block-partition guard is NON-VACUOUS by
# injecting a genuine block-size-dependent (cross-block-state) fault into the ENGINE
# output — a per-processBlock-call offset that leaks into the rendered audio — and
# showing the probe reports blockpart_max > 1e-9 (gCode bit 8), while the correct path
# returns blockpart_max = 0. Same isolation discipline as the sibling runners: every
# build/mutation happens in a TEMP SOURCE TREE; the working-tree sources are never
# touched and no compile/abort is counted as a valid RED.
#
# THE CHOKE POINT. The harness drives the engine through
#   StandaloneAudioEngine::processBlock(planarIn,planarOut,..,frames)    (host include)
#     -> DeviceAdapter::renderBlock(SynthRuntime&, planarIn, planarOut, frames)
#          for f in [0,frames):  rt.processFrame(in, true);  writeOutput_(out,..,f)
#                                              (core/include/lunar24/core/device_adapter.h:285)
# The runtime's own `processBlock(const RuntimeInputs*, n, RuntimeOutput*, ..)`
# (machine_runtime.h:1596) is NOT on this path — the adapter loops processFrame directly. So a
# per-block counter injected into the ADAPTER would advance once per renderBlock call (the real
# per-processBlock boundary), while the SAME counter injected into the runtime's processBlock is
# dead code here (the adapter never calls it). For the block-partition check the two paths are:
#   frame path:  processBlock(...,1)  * 4096   -> renderBlock called 4096 times (counter->1..4096)
#   block path:  processBlock(...,4096) once    -> renderBlock called once       (counter->1)
# A correct DSP is partition-invariant (phase carries per-sample, so both advance identically ->
# maxdiff=0). A fault that advances state ONCE PER renderBlock call makes the two paths diverge.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DA="$ROOT/core/include/lunar24/core/device_adapter.h"
PROBE_TARGET=gh19_alias_probe
VCO="$ROOT/core/include/lunar24/core/vco.h"

TREE="$(mktemp -d "$(mktemp -d)/vco-blkerr-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/vco-blkerr-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

DA_T="$TREE/core/include/lunar24/core/device_adapter.h"
VCO_T="$TREE/core/include/lunar24/core/vco.h"

# The "correct path" baseline = the win8 windowed analytic BLAMP already integrated into the
# COMMITTED production vco.h (byte-identical to the old build/.vco_blamp_win8.h backup).
# @Codex (msg 1a8ed7f2): use the committed vco.h as the positive, not a gitignored build/ copy.
PRISTINE_VCO="$ROOT/core/include/lunar24/core/vco.h"

install_vco() { cp "$PRISTINE_VCO" "$VCO_T"; }

py_splice() {  # edit the TEMP-TREE file. `need` = exact anchor count (fail-closed).
  local file="$1" find="$2" repl="$3" need="$4"
  python3 - "$file" "$find" "$repl" "$need" <<'PY'
import sys
dst, find, repl, need = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(dst, encoding="utf-8").read()
if t.count(find) != need:
    sys.stderr.write(f"ERROR: anchor count {t.count(find)} != expected {need}: {find[:70]!r}\n")
    sys.exit(2)
open(dst, "w", encoding="utf-8").write(t.replace(find, repl))
PY
}

configure() { cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; }

build_probe() {
  cmake --build "$WORK" --target "$PROBE_TARGET" -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   ERROR: $PROBE_TARGET build FAILED (not a valid RED)." >&2; tail -8 "$WORK/build.err" >&2; exit 1; }
}

measure() {  # run the probe fresh; echo "<blockpart_max> <exit>".
  rm -rf "$WORK/probe-out"; mkdir -p "$WORK/probe-out"
  set +e
  "$WORK/$PROBE_TARGET" --out "$WORK/probe-out" >"$WORK/probe.log" 2>&1
  local pc=$?
  set -e
  local bp note
  bp="$(grep -oE 'blockpart_max=[-+0-9.eE+]+' "$WORK/probe.log" | head -1 | cut -d= -f2)"
  note="$(grep -oE 'blockpart_max=[-+0-9.eE+]+ \([^)]*\)' "$WORK/probe.log" | head -1 | sed 's/.*(//; s/)$//')"
  echo "${bp:-NA} $pc ${note:-?}"
}

bp_val() { echo "$1" | awk '{print $1}'; }

echo "== [1] correct path (WIN8 candidate) =="
install_vco; configure; build_probe
R0="$(measure)"; BP0="$(bp_val "$R0")"; E0="$(echo "$R0" | awk '{print $2}')"; N0="$(echo "$R0" | awk '{print $3}')"
echo "     blockpart_max=$BP0 (probe exit=$E0, note=$N0)"
if awk -v b="$BP0" 'BEGIN{exit !(b<1e-9)}'; then
  echo "     GREEN: correct path is block-partition-invariant (maxdiff < 1e-9)."
else
  echo "     ERROR: correct path leaked block state (blockpart_max=$BP0)." >&2; exit 1
fi
grep -E 'blockpart_max|four_output_fail' "$WORK/probe.log" | head -2 | sed 's/^/       /'

echo
echo "== [2] block-error mutation (per-processBlock counter leaking into DRY-A output) =="
# Genuine CROSS-BLOCK state at the REAL choke point: a per-renderBlock-call counter added to the
# DRY-A output every sample. Under the frame path (renderBlock called 4096x, frames=1 each) the
# counter advances 4096 times -> samples 1..4095 get a growing offset; under the block path (one
# renderBlock, frames=4096) the counter advances once -> a constant offset. They diverge. Inject in
# DeviceAdapter::renderBlock (NOT the runtime's unused processBlock overload).
py_splice "$DA_T" \
'  if (!hasPlan_) return;
  for (int f = 0; f < frames; ++f) {
    RuntimeInputs in{0.0, 0.0};
    resolveInput_(planarIn, f, in);                     // input direction (single primitive).
    const RuntimeOutput out = rt.processFrame(in, true);  // drive the product runtime.
    writeOutput_(out, planarOut, f);                    // output direction (single primitive).
  }' \
'  if (!hasPlan_) return;
  ++blockCtr_;                                          // MUTATION: per-processBlock-call state.
  for (int f = 0; f < frames; ++f) {
    RuntimeInputs in{0.0, 0.0};
    resolveInput_(planarIn, f, in);                     // input direction (single primitive).
    RuntimeOutput out = rt.processFrame(in, true);        // drive the product runtime.
    out.dryA += 1e-6 * blockCtr_;                         // MUTATION: leak per-block state into DRY-A.
    writeOutput_(out, planarOut, f);                    // output direction (single primitive).
  }' 1
py_splice "$DA_T" \
'  std::uint64_t nonFinite_ = 0;' \
'  std::uint64_t nonFinite_ = 0;
  double blockCtr_ = 0.0;                                 // MUTATION: per-block (not per-sample) state.' 1

configure; build_probe
R1="$(measure)"; BP1="$(bp_val "$R1")"; E1="$(echo "$R1" | awk '{print $2}')"; N1="$(echo "$R1" | awk '{print $3}')"
echo "     blockpart_max=$BP1 (probe exit=$E1, note=$N1)"
if awk -v b="$BP1" 'BEGIN{exit !(b>1e-9)}'; then
  echo "     PASS: the block-partition guard FIRED on a real cross-block-state fault (bp>1e-9)."
  echo "           probe exit=$E1 (gCode bit8 = block mismatch)."
else
  echo "     FAIL: mutation did NOT trip the guard (blockpart_max=$BP1)." >&2; exit 1
fi
grep -E 'blockpart_max|block_partition' "$WORK/probe.log" | head -2 | sed 's/^/       /'

echo
echo "== [3] restore correct path (redo) =="
cp "$PRISTINE_VCO" "$VCO_T"; cp "$DA" "$DA_T"
configure; build_probe
R2="$(measure)"; BP2="$(bp_val "$R2")"
echo "     blockpart_max=$BP2"
if awk -v b="$BP2" 'BEGIN{exit !(b<1e-9)}'; then
  echo "     GREEN (restored) — the guard is a real, reversible net."
else
  echo "     ERROR: restore did not return to blockpart_max=0 ($BP2)." >&2; exit 1
fi

echo
echo "SUMMARY: correct BLAMP path blockpart_max=0; a genuine per-processBlock counter leak moves it"
echo "  to >1e-9 and sets gCode bit 8. Working-tree device_adapter.h + vco.h were untouched."
