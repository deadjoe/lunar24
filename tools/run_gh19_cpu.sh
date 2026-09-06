#!/usr/bin/env bash
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_cpu.sh — task #86 / GH#19 COST evidence: measure the probe's CPU columns (median +
# repeat-sample dispersion) for each given vco.h variant in an isolated TEMP source tree, so a
# BLAMP candidate can be compared against the naive baseline on the SAME machine + compiler.
#
# @Codex ab5315a6 (CPU item): raw means (497.97/499.92/494.33 ns/s) cannot be read as "essentially
# zero cost". For each variant this tool reports:
#   cpu_block_ns   = median per-sample pure processBlock cost (BLOCK item ⑤) over 7 lanes
#   cpu_block_var  = stdev across those lans (the repeat-sample dispersion)
#   cpu_render_loop= median per-sample WHOLE-harness loop cost over 7 reps
#   cpu_render_loop_var = stdev across reps
# and then (below, in the report) you may state ONLY whether THIS machine measurement RESOLVES the
# increment between two variants (i.e. the difference exceeds the pooled dispersion) — never a bare
# "essentially free" claim from a single mean.
#
# Usage:  tools/run_gh19_cpu.sh build/.vco_blamp_naive.h build/.vco_blamp_win8.h
# Both variant vco.h files are built REPRODUCIBLY from the committed vco.h by
# tools/build_gh19_blamp_candidates.py — run that FIRST (@Codex 1a8ed7f2):
#   python3 tools/build_gh19_blamp_candidates.py
# The naive variant (true triangle, corrector DISABLED) and win8 (the integrated production
# kernel) are generated from the same committed source — never hand-kept in build/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

[ $# -ge 1 ] || { echo "usage: $0 <variant.vco.h> [more...]" >&2; exit 1; }
for v in "$@"; do [ -f "$v" ] || { echo "ERROR: no such variant: $v" >&2; exit 1; }; done

for v in "$@"; do
  v="$(cd "$(dirname "$v")" && pwd)/$(basename "$v")"
  TREE="$(mktemp -d "$(mktemp -d)/gh19-cpu-tree.XXXXXX")"
  WORK="$(mktemp -d "$(mktemp -d)/gh19-cpu-work.XXXXXX")"
  trap 'rm -rf "$TREE" "$WORK"' EXIT
  echo "=== $(basename "$v") ==="

  rsync -a --delete --exclude 'build' --exclude 'build-asan' --exclude 'build-tsan' \
    --exclude 'cmake-build-debug' --exclude '.git' --exclude 'research' \
    --exclude 'third_party' "$ROOT/" "$TREE/"
  ln -s "$ROOT/third_party" "$TREE/third_party"
  VCO="$TREE/core/include/lunar24/core/vco.h"
  grep -q 'Vco::tick' "$v" || { echo "ERROR: $v is not a vco.h." >&2; exit 1; }
  cp "$v" "$VCO"

  cmake -S "$TREE" -B "$WORK" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "$WORK" --target gh19_alias_probe -j 8 >/dev/null 2>"$WORK/build.err" \
    || { echo "   probe build FAILED:" >&2; tail -8 "$WORK/build.err" >&2; exit 1; }

  OUT="$WORK/probe-out"; rm -rf "$OUT"; mkdir -p "$OUT"
  set +e
  "$WORK/gh19_alias_probe" --out "$OUT" >"$WORK/probe.log" 2>&1
  pc=$?
  set -e

  # The CPU rows are in gh19_cpu.tsv (separate from the scenario TSV). Extract median + stdev.
  TSV="$OUT/gh19_cpu.tsv"
  if [ -f "$TSV" ]; then
    for key in cpu_block_ns cpu_block_var cpu_render_loop cpu_render_loop_var cpu_block_size; do
      val="$(awk -F'\t' -v k="$key" '$1==k{print $5}' "$TSV")"
      printf '  %-22s %s\n' "$key" "${val:-NA}"
    done
  else
    echo "   (no $TSV — probe exited $pc)" >&2; tail -5 "$WORK/probe.log" >&2
  fi
  echo "   (probe exit=$pc; blockpart=$(grep -oE 'blockpart_max=[-+0-9.eE+]+' "$WORK/probe.log" | head -1 | cut -d= -f2 || echo '?'))"
  echo
  trap - EXIT
  rm -rf "$TREE" "$WORK"
done
