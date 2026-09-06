#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_vco_normal_source_mutation.sh — repeatable GENERATE -> BUILD/RUN -> RESTORE tool for the
# task #83 (GH #18) VCO-normal-source fix.
#
# The oracle (tests/host/test_vco_normal_source.cpp, created under the @Codex 4fe298c8 contract)
# drives encode -> decode -> StandaloneAudioEngine.applyDeviceState -> processBlock and asserts the
# GH #18 CONTRACT: default DRY A/B are live (no VCO-B DC lock), an asymmetric A-only change reaches B
# over the acyclic A->B route at cvAmt>0 (isolated at cvAmt==0), a B-only change never becomes the A
# source, a user cable overrides the route, and the default compiles to ZERO feedback edges. The fix
# itself is SOURCE-ONLY: RouteId 4 (route.vco_b_vco_out_to_cv_in) is corrected to source vco_a.dry_out
# in the registry (lunar24.json -> generated/lunar24/registry.hpp); NO runtime code carries the fix.
#
# This driver injects a DETACHED production-source mutation and proves, per defense point:
#
#   REAL code (no mutation)       -> GREEN  (the oracle passes — the source fix is correct),
#   MUTATED code (one regression) -> RED    (the oracle trips on a SPECIFIC pinned assertion — the
#                                          fix is load-bearing at that defense point).
#
# Six contracted defense points (@Codex 4fe298c8 + e14e62a8), each an ACTUAL production-source mutation
# (never a relaxed validator), each verified against a SPECIFIC assertion (the exact FAIL expression),
# not merely a non-zero failure count:
#
#   #1 source_reverts_to_B   (generated/lunar24/registry.hpp)  revert RouteId 4's source from
#                            vco_a.dry_out back to vco_b.vco_out — the defective self-edge. The
#                            compile-time static_assert in machine_definition.h is NEUTRALIZED ONLY in
#                            this shadow so the oracle independently catches the regression at RUNTIME
#                            (the oracle, not a compiler guard, is the load-bearing net here). The
#                            self-edge re-introduces a cyclic feedback line; pinned `fc == 0`.
#   #2 ignores_override      (core/include/lunar24/core/patch_graph.h)  normalizedActive() drops the
#                            countInto(sink)==0 gate, so a user cable that overrides the A->B route
#                            is ignored. Pinned `!(normalizedActive(vco_a_dry_out, vco_b_cv_in))`.
#   #3 self_read_sink_value (machine_runtime.h)  resolveSinkValue_ reads the SINK's own published CV
#                            (cvAt_(sink)) instead of the SOURCE (cvAt_(src)), so B reads itself and
#                            never sees A. Pinned `d > 1e-9` (test #2, cvAmt>0: A->B broken).
#   #4 miss_route4_fact     (machine_runtime.h)  sourceOfSink_ fails to find the vco_b_cv_in edge —
#                            the RouteId 4 A->B fact is missing from the compiled graph, so B falls
#                            back to 0 and A never reaches it. Pinned `d > 1e-9` (test #2).
#   #5 force_dryb_constant  (machine_runtime.h)  the fix MASKS the defect rather than fixing it: B is
#                            hard-coded to a constant, hiding whether it oscillates. Pinned
#                            `bMoved` (test #1: the fixed output must actually move, not sit at DC).
#   #6 hidden_z1_delay       (machine_runtime.h)  @Codex e14e62a8's CRITICAL gap: each runtime
#                            independently saves previousA (aThis_/aPrev_) and feeds THAT to B's cv_in.
#                            The graph is still a VALID acyclic A->B route (feedbackCount()==0 holds),
#                            so #1..#5 and every structural check stay GREEN — ONLY the same-vs-previous
#                            discriminator trips. Pinned `madSame < kSameTol` (test #6), under a FAST,
#                            NON-DEGENERATE A so one sample of delay is a large, observable divergence.
#
# #1 splices generated/lunar24/registry.hpp AND a compile-guard-neutralizing machine_definition.h
# shadow; #3/#4/#5/#6 splice core/include/lunar24/core/machine_runtime.h; #2 splices patch_graph.h.
# Each is spliced into a DETACHED shadow header under build/ so the oracle compiles against an
# include path that shadows ONLY the mutated header(s) (isolated per mutation, never left behind) —
# no tracked path is ever written and the oracle source is untouched.
#
# Evidence boundary (documented): the source-revert regression #1 is ALSO caught at COMPILE time by the
# machine_definition.h static_assert; #1 neutralizes that guard in the shadow specifically to prove the
# ORACLE independently detects the same defect at runtime. #3 vs #4 both surface as `d > 1e-9` (test #2)
# because that is the single observable A->B contract gate — #3 breaks it by reading the wrong VALUE,
# #4 by missing the correct route FACT; the script keeps them as separate defense points named and
# spliced distinctly.
#
# No product topology / migration / route / 169-classification change beyond the source fix. Verification
# evidence only. This is intentionally NOT registered in CTest: the mutated build fails by design.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SRC_REG="generated/lunar24/registry.hpp"
SRC_DEF="core/include/lunar24/core/machine_definition.h"
SRC_GRAPH="core/include/lunar24/core/patch_graph.h"
SRC_RT="core/include/lunar24/core/machine_runtime.h"
TEST="tests/host/test_vco_normal_source.cpp"

WORK="build/mut_vco_normal_source"
EXE_REAL="$WORK/harness_real"
EXE_MUT="$WORK/harness_mut"

COMMON_INC=(-I"$WORK" -Icore/include -Ihost/include -Igenerated -Itests/core -Itests/host)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

for f in "$SRC_REG" "$SRC_DEF" "$SRC_GRAPH" "$SRC_RT" "$TEST"; do
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
REAL_SUM="$(printf '%s\n' "$REAL_OUT" | grep -oE '\[test_vco_normal_source\] [0-9]+ checks OK' | tail -1)"
if [ "$REAL_RC" -ne 0 ] || [ -z "$REAL_SUM" ]; then
  echo "   ERROR: REAL build did not pass (rc=$REAL_RC, '$REAL_SUM')." >&2
  exit 1
fi
N="$(printf '%s' "$REAL_SUM" | grep -oE '[0-9]+ checks OK' | grep -oE '[0-9]+')"
echo "   real: rc=$REAL_RC, $REAL_SUM ($N checks, 0 failures — source fix correct)."

# Python splice: write a shadow of `src` at $WORK/<rel> that applies a literal find->repl.
# `need` is the exact occurrences count of `find` in the real source (must match exactly, so a
# mutated copy can never silently no-op). For need>1 every occurrence is replaced.
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

# Python splice: neutralize the 3-line compile-time static_assert in a shadow of machine_definition.h.
# This is used ONLY by mutation #1 so the ORACLE (not a compiler guard) is what catches the regression.
py_remove_static_assert() {
  local src="$1" rel="$2"
  python3 - "$src" "$WORK/$rel" <<'PY'
import os, re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding="utf-8").read()
m = re.search(r"static_assert\(kActiveRoutes\[0\]\.sourceJack == lunar24::registry::JackId::vco_a_dry_out.*?\);\n", t, re.S)
if not m:
    sys.stderr.write("ERROR: route static_assert not found in machine_definition.h\n")
    sys.exit(2)
t = t[:m.start()] + "// [M1] static_assert NEUTRALIZED in shadow, to prove the oracle independently\n//       catches the source-revert regression at runtime (not via a compiler guard).\n" + t[m.end():]
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8").write(t)
PY
}

# Python splice: the @Codex e14e62a8 HIDDEN 1-sample-delay regression (#6). Each runtime INDEPENDENTLY
# saves previousA (aThis_/aPrev_) and feeds THAT to B's cv_in instead of A's live same-frame value. The
# compiled result is still a perfectly VALID acyclic A->B route (feedbackCount()==0 continues to hold),
# so this passes the structural check; ONLY the new same-vs-previous discriminator
# (test_b_same_sample_vs_previous, `madSame < kSameTol`) catches it. Three edits in one shadow:
#   (a) add members aThis_/aPrev_ (per-runtime saved previous-A),
#   (b) in the kVcoA slot: hold the OLD A before the tick (aPrev_ = aThis_) and store the new a after,
#   (c) resolveSinkValue_ reads aPrev_ ONLY for src==vco_a_dry_out — the live cvOut_[vco_a_dry_out]
#       (what controlVoltageAt returns) is left alone, so the refSame reference still reads the SAME-frame
#       A and DIVERGES from what B actually consumed (previous A).
py_hidden_z1() {
  local src="$1" rel="$2"
  python3 - "$src" "$WORK/$rel" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src, encoding="utf-8").read()
edits = [
    # (a) members
    ("double dryA_ = 0.0, dryB_ = 0.0;",
     "double dryA_ = 0.0, dryB_ = 0.0;\n  // [MUT #6 hidden z^-1] per-runtime saved previous-A, fed to B.\n  double aPrev_ = 0.0, aThis_ = 0.0;"),
    # (b) A slot: hold the OLD A before the tick, store this-frame a after
    ("        double a = 0.0;\n        vcA_.tick(&a);\n        dryA_ = a;",
     "        double a = 0.0;\n        aPrev_ = aThis_;   // [MUT #6] hold the OLD A for B to read.\n        vcA_.tick(&a);\n        aThis_ = a;\n        dryA_ = a;"),
    # (c) resolveSinkValue_: the A->B edge consumes PREVIOUS A, not the live same-frame value
    ("const double live = cvAt_(src);",
     "const double live = (src == JackId::vco_a_dry_out) ? aPrev_ : cvAt_(src);   // [MUT #6] B reads PREVIOUS A."),
]
for find, repl in edits:
    n = t.count(find)
    if n != 1:
        sys.stderr.write(f"ERROR: anchor count {n} != 1 for: {find[:70]}\n")
        sys.exit(2)
    t = t.replace(find, repl)
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "w", encoding="utf-8").write(t)
PY
}

# Compile the oracle against the current shadow set, run it, and REQUIRE a RED run that trips the
# pinned assertion `expect` (a `FAIL <file>:<line>: <expr>` line containing `expect`) — never "just
# non-zero". The real baseline proved the oracle passes unmutated; a mutated build that (a) goes RED
# (rc!=0) AND (b) trips the exact pinned assertion proves that defense point is load-bearing.
# @Codex e14e62a8: the run MUST terminate NORMALLY and hit the pinned assertion — a crash that leaves a
# residual FAIL is NOT acceptable. mini_test.h CHECK never aborts, so a real regression always prints the
# `[suite] N/M checks FAILED` summary; require that summary so a crash/no-summary run is rejected.
run_mutation() {
  local name="$1" expect="$2"
  echo
  echo "== [mutation #${name}] splice + build + run — expect RED at '${expect}' =="
  if ! g++ -std=c++17 -O0 -o "$EXE_MUT" "$TEST" "${COMMON_INC[@]}" 2>"$WORK/err_${name}.txt"; then
    echo "   ERROR: mutation #${name} MUTATED build (g++) failed." >&2
    cat "$WORK/err_${name}.txt" >&2
    exit 1
  fi
  set +e
  MUT_OUT="$("$EXE_MUT" 2>&1)"
  MUT_RC=$?
  set -e
  if [ "$MUT_RC" -eq 0 ]; then
    echo "   ERROR: mutation #${name} did NOT go RED (rc=0). The fix is NOT load-bearing here." >&2
    exit 1
  fi
  # Normal-termination proof: the run must print the [suite] N/M checks FAILED summary. A crash would
  # leave NO such summary — reject that (a crash-residual FAIL is not a clean RED).
  MUT_SUM="$(printf '%s\n' "$MUT_OUT" | grep -oE '\[test_vco_normal_source\] [0-9]+/[0-9]+ checks FAILED' | tail -1)"
  if [ -z "$MUT_SUM" ]; then
    echo "   ERROR: mutation #${name} produced no '[..] N/M checks FAILED' summary — abnormal termination (crash), not a clean RED." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  if ! printf '%s\n' "$MUT_OUT" | awk -v e="$expect" '
        index($0, "FAIL ") > 0 && index($0, e) > 0 { print; found = 1 }
        END { if (!found) exit 1 }'; then
    echo "   ERROR: mutation #${name} went RED but did NOT trip the pinned assertion '${expect}'." >&2
    printf '%s\n' "$MUT_OUT" | tail -15 >&2
    exit 1
  fi
  MUT_FAILS="$(printf '%s' "$MUT_SUM" | grep -oE '[0-9]+/' | head -1 | tr -d '/')"
  echo "   mutated: rc=$MUT_RC, pinned assertion trips, $MUT_SUM ($MUT_FAILS subtests fail)."
}

# -------------------------------------------------------------------------------------------------
# #1 — source_reverts_to_B. Revert RouteId 4 source to vco_b.vco_out (the pre-fix self-edge) AND
#      neutralize the compile-time static_assert in a shadow, so the ORACLE catches the regression.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24"
py_splice "$SRC_REG" "lunar24/registry.hpp"\
  "JackId::vco_a_dry_out, JackId::vco_b_cv_in"\
  "JackId::vco_b_vco_out, JackId::vco_b_cv_in" 1
py_remove_static_assert "$SRC_DEF" "lunar24/core/machine_definition.h"
run_mutation "source_reverts_to_B" "fc == 0"
rm -f "$WORK/lunar24/registry.hpp" "$WORK/lunar24/core/machine_definition.h"

# -------------------------------------------------------------------------------------------------
# #2 — ignores_override. normalizedActive() drops the countInto(sink)==0 gate, so a user cable that
#      should override the A->B route is ignored.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_GRAPH" "lunar24/core/patch_graph.h"\
  "return routeExists(source, sink) && countInto(sink) == 0u;"\
  "return routeExists(source, sink);   /* [MUT #2 ignore override] */" 1
run_mutation "ignores_override" "!rt->normalizedActive(JackId::vco_a_dry_out, JackId::vco_b_cv_in)"
rm -f "$WORK/lunar24/core/patch_graph.h"

# -------------------------------------------------------------------------------------------------
# #3 — self_read_sink_value. resolveSinkValue_ reads the SINK's own published CV instead of the
#      SOURCE, so B reads itself and never receives A.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "const double live = cvAt_(src);"\
  "const double live = cvAt_(sink);   /* [MUT #3 self-read] */" 1
run_mutation "self_read_sink_value" "d > 1e-9"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# #4 — miss_route4_fact. sourceOfSink_ fails to find the vco_b.cv_in edge, so RouteId 4's A->B fact
#      is missing from the compiled graph and B falls back to 0 (no A->B propagation).
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "if (edges_[i].sink == sink) { found = true; return edges_[i].source; }"\
  "if (edges_[i].sink == sink && sink != JackId::vco_b_cv_in) { found = true; return edges_[i].source; }   /* [MUT #4 miss A->B fact] */" 1
run_mutation "miss_route4_fact" "d > 1e-9"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# #5 — force_dryb_constant. The fix MASKS the defect instead of fixing it: B's output is hard-coded to
#      a constant, so the default DRY B rides at DC regardless of the route.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_splice "$SRC_RT" "lunar24/core/machine_runtime.h"\
  "vcB_.tick(&b);"\
  "vcB_.tick(&b); b = 0.0;   /* [MUT #5 force DRY B constant] */" 1
run_mutation "force_dryb_constant" "bMoved"
rm -f "$WORK/lunar24/core/machine_runtime.h"

# -------------------------------------------------------------------------------------------------
# #6 — hidden_z1_delay. @Codex e14e62a8's CRITICAL gap: an ACYCLIC A->B route proves feedbackCount()==0,
#      but does NOT prove B reads A's LIVE same-frame value. Each runtime independently saves previousA
#      (aThis_/aPrev_) and feeds that to B's cv_in. The compiled graph is still a VALID acyclic route
#      (feedbackCount()==0 continues to hold), so #1..#5 and the structural checks stay green — ONLY the
#      new same-vs-previous discriminator (test_b_same_sample_vs_previous, `madSame < kSameTol`) catches
#      it. The A->B value is delayed by exactly one sample under a FAST, NON-DEGENERATE A, so refSame
#      (which reads controlVoltageAt's live A via the harness) diverges from what B actually consumed.
rm -rf "$WORK/lunar24"; mkdir -p "$WORK/lunar24/core"
py_hidden_z1 "$SRC_RT" "lunar24/core/machine_runtime.h"
run_mutation "hidden_z1_delay" "madSame < kSameTol"
rm -f "$WORK/lunar24/core/machine_runtime.h"

echo
echo "== [restore] =="
rm -rf "$WORK"
echo "   removed $WORK; source tree untouched (only the scratch dir was written)."
echo "-> PASS: task#83 VCO-normal-source fix is load-bearing at all 6 contracted defense points"
echo "        (incl. the @Codex e14e62a8 hidden 1-sample-delay);"
echo "        real=GREEN ($N checks), each mutated=RED with a pinned specific assertion, reproduced repeatably."
