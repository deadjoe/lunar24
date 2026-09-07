#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_candidates.sh — measure the 24-cell blref_full_db matrix for each
# isolated R&D BLAMP candidate in a TEMP source tree (working-tree vco.h is
# NEVER touched). For each candidate whole-file vco.h variant it: rsyncs a
# clean tree, installs the candidate, cmake+builds gh19_alias_probe (Release),
# runs it, runs gh19_alias_analyze.py, then the acceptance gate.
#
# Usage:  tools/run_gh19_candidates.sh build/.vco_blamp_win4.h build/.vco_blamp_win8.h
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCO_SRC="$ROOT/core/include/lunar24/core/vco.h"
ANALYZE="$ROOT/tools/gh19_alias_analyze.py"
GATE="$ROOT/tools/check_gh19_blamp_acceptance.py"
BASE="$ROOT/tools/gh19_naive_baseline.tsv"
MANIFEST="$ROOT/tools/gh19_manifest.tsv"

[ $# -ge 1 ] || { echo "usage: $0 <candidate.vco.h> [more...]" >&2; exit 1; }
for cand in "$@"; do
  [ -f "$cand" ] || { echo "ERROR: no such candidate: $cand" >&2; exit 1; }
done

for cand in "$@"; do
  cand="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand")"
  TREE="$(mktemp -d "$(mktemp -d)/gh19-cand-tree.XXXXXX")"
  WORK="$(mktemp -d "$(mktemp -d)/gh19-cand-work.XXXXXX")"
  trap 'rm -rf "$TREE" "$WORK"' EXIT
  echo "=== candidate $(basename "$cand") ==="

  rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
    --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
    --exclude 'third_party' "$ROOT/" "$TREE/"
  ln -s "$ROOT/third_party" "$TREE/third_party"

  VCO="$TREE/core/include/lunar24/core/vco.h"
  grep -q 'Vco::tick' "$cand" || { echo "ERROR: $cand is not a vco.h." >&2; exit 1; }
  cp "$cand" "$VCO"

  cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "$WORK" --target gh19_alias_probe -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   probe build FAILED:" >&2; tail -8 "$WORK/build.err" >&2; exit 1; }

  OUT="$WORK/probe-out"; rm -rf "$OUT"; mkdir -p "$OUT"
  set +e
  "$WORK/gh19_alias_probe" --out "$OUT" >"$WORK/probe.log" 2>&1
  pc=$?
  set -e
  if [ "$pc" -ne 0 ]; then
    echo "   probe exited $pc (aborting):" >&2; tail -3 "$WORK/probe.log" >&2; exit 1
  fi

  python3 "$ANALYZE" --dir "$OUT" --manifest "$MANIFEST" > "$WORK/analyze.tsv" 2>/dev/null
  set +e
  python3 "$GATE" --baseline "$BASE" --current "$WORK/analyze.tsv" > "$WORK/gate.txt" 2>&1
  rc=$?
  set -e
  if grep -q 'PASS: all 8 x 880' "$WORK/gate.txt"; then verdict=GREEN; else verdict=RED; fi
  bins="$(grep -E '^Bins' "$WORK/gate.txt" | sed 's/^Bins (pinned): //' || echo '(no bins line)')"
  blockpart="$(grep -oE 'blockpart_max=[-+0-9.eE]+' "$WORK/probe.log" | head -1 | cut -d= -f2 || true)"
  echo "  verdict=$verdict  rc_gate=$rc  $bins  blockpart=${blockpart:-?}"
  echo "--- gate.txt ---"; sed 's/^/    /' "$WORK/gate.txt"
  # persist per-cell data for the report into the repo build/ dir
  base="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand" .h)"
  cp "$WORK/analyze.tsv" "$base.tsv"
  cp "$WORK/gate.txt" "$base.gate.txt"
  echo "  (saved $base.tsv + $base.gate.txt)"
  echo
  trap - EXIT
  rm -rf "$TREE" "$WORK"
done
