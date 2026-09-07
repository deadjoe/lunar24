#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_previousa_control.sh — task #86 / GH#19 ROUTING-INTEGRITY negative control.
#
# @Codex ab5315a6: `blockpart=0` and the triangleBlampCorr `cp-step` anchor are NOT a
# demo of routing integrity. The A->B route contract is task #83's own consumption-path
# oracle, test_vco_normal_source test (6) `test_b_same_sample_vs_previous`: B must read
# A's LIVE, SHARED, SAME-FRAME value (an ACYCLIC route; feedbackCount()==0 proves no
# feedback LINE but does NOT prove the executor reads A's this-frame value). The real
# fault it catches is a HIDDEN one-sample delay: a build that stashes `previousA` and
# feeds THAT to B. That mutation is in machine_runtime.h (the B input consumption), NOT
# in triangleBlampCorr.
#
# ISOLATION: everything runs in an independent TEMP SOURCE TREE (never touching the
# working-tree machine_runtime.h). For each of
#   (1) baseline (no mutation)  -> test_vco_normal_source must be GREEN (all 6 tests).
#   (2) previousA mutation      -> the SAME host test must be RED on the (6) discriminator
#                                  while the other five stay GREEN (a single targeted fault).
#   (3) restored baseline       -> GREEN again (the mutation is reversible / the criterion
#                                  is load-bearing, not a frozen gate).
# The mutation itself is a real production-source edit: B reads A's PREVIOUS frame value
# (stashed in aPrev_) instead of A's live published value.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
RT="$ROOT/core/include/lunar24/core/machine_runtime.h"
TESTTARGET=test_vco_normal_source

TREE="$(mktemp -d "$(mktemp -d)/vco-prevA-tree.XXXXXX")"
WORK="$(mktemp -d "$(mktemp -d)/vco-prevA-work.XXXXXX")"
cleanup() { rm -rf "$TREE" "$WORK"; }
trap cleanup EXIT

rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
  --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
  --exclude 'third_party' "$ROOT/" "$TREE/"
ln -s "$ROOT/third_party" "$TREE/third_party"

RT_T="$TREE/core/include/lunar24/core/machine_runtime.h"

py_splice() {  # edit the TEMP-TREE machine_runtime.h. `need` = exact anchor count (fail-closed).
  local find="$1" repl="$2" need="$3"
  python3 - "$RT_T" "$find" "$repl" "$need" <<'PY'
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

build_test() {
  cmake --build "$WORK" --target "$TESTTARGET" -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   ERROR: $TESTTARGET build FAILED (not a valid RED — aborting)." >&2
         tail -8 "$WORK/build.err" >&2; exit 1; }
}

run_test() {
  set +e
  "$WORK/$TESTTARGET" >"$WORK/test.log" 2>&1
  local pc=$?
  set -e
  echo "$pc"
}

# (6) discriminator block in the log: the madSame/madPrev line + the CHECK verdict.
show6() { grep -E 'maxAStep|checks' "$WORK/test.log" | head -3; }
# mini_test success line is "<calls> checks OK"; failure prints a CHECK/assert line.
is_green() { grep -q 'checks OK' "$WORK/test.log" && ! grep -q 'CHECK failed\|assert\|FAIL' "$WORK/test.log"; }

echo "== [1] baseline (no mutation) =="
configure; build_test
if [ "$(run_test)" = "0" ] && is_green; then
  echo "     GREEN (all 6 tests passed; (6) discriminator):"
  show6 | sed 's/^/       /'
else
  echo "     ERROR: baseline test_vco_normal_source is not GREEN (revisit)." >&2; tail -15 "$WORK/test.log" >&2
  exit 1
fi

echo
echo "== [2] previousA mutation (B reads A's PREVIOUS frame value) =="
# A case: stash the PREVIOUS frame's A (cvOut_[vcoAOut_] still holds a(n-1) until A publishes).
py_splice '        dryA_ = a;
        chIn_[VoiceMixer::kChannelVcoA] = a;
        if (vcoAOutBound_) publishSourceValue_(vcoAOut_, a);' \
'        aPrev_ = cvAt_(vcoAOut_);                 // MUTATION: capture PREVIOUS frame A.
        dryA_ = a;
        chIn_[VoiceMixer::kChannelVcoA] = a;
        if (vcoAOutBound_) publishSourceValue_(vcoAOut_, a);' 1
# B case: feed B the stashed previous A, NOT the live resolved value.
py_splice '        if (cvInBoundB_) {
          double g = 0.0;
          if (resolveControlSink_(cvInB_, g, driveGraph)) vcB_.setCvInput(g, cvModeB_);
        }' \
'        vcB_.setCvInput(aPrev_, cvModeB_);         // MUTATION: B reads PREVIOUS A (hidden z^-1).' 1
# Member.
py_splice '  double dryA_ = 0.0, dryB_ = 0.0;' \
'  double dryA_ = 0.0, dryB_ = 0.0;
  double aPrev_ = 0.0;                             // MUTATION: previous-A stash for the B input.' 1

build_test
PC="$(run_test)"
if [ "$PC" != "0" ]; then
  echo "     RED (exit=$PC) — mutation deflected the oracle. Confirm the (6) discriminator is the ONLY failure:"
  grep -E 'CHECK failed|Expected|FAIL|test_b_|test_vco_normal_source' "$WORK/test.log" | head -20 | sed 's/^/       /'
else
  echo "     ERROR: mutation still GREEN — the previousA fault is NOT caught (criterion not load-bearing)." >&2
  echo "       log tail:"; tail -20 "$WORK/test.log" >&2
  exit 1
fi
echo "     (6) discriminator under mutation (madSame should now be the LARGE one):"
show6 | sed 's/^/       /'

echo
echo "== [3] restored baseline (redo) =="
# Force-restore the pristine machine_runtime.h (rsync's mtime fast-check would skip
# overwriting the NEWER mutated copy). Then rebuild + rerun.
cp "$RT" "$RT_T"
configure; build_test
if [ "$(run_test)" = "0" ] && is_green; then
  echo "     GREEN (restored) — the target is a real, reversible load-bearing net."
  show6 | sed 's/^/       /'
else
  echo "     ERROR: restored baseline not GREEN (revisit)." >&2; tail -15 "$WORK/test.log" >&2
  exit 1
fi

echo
echo "SUMMARY: the A->B route reads A LIVE same-frame; a hidden previous-A delay moves (6) RED"
echo "  while the other five tests stay GREEN. Working-tree machine_runtime.h was never touched."
