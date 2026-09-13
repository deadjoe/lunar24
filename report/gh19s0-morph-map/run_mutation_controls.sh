#!/bin/sh
# Raft task #116 (GH #19 S0) -- PRODUCTION negative controls.
#
# Why this exists, separately from the probe's own N1..N4: the in-probe controls substitute a
# stand-in source inside the probe, so they only prove the PROBE's predicates have discriminating
# power. They cannot stand in for the contract's required mutant sources, because a fixture written
# by hand only proves the code agrees with itself. This script instead mutates the REAL production
# headers and runs the SAME probe binary source against them.
#
# No tracked file is ever modified. Each mutant is built from a COPY of core/include placed FIRST on
# the include path, so the patched header wins over the pristine one in the tree.
#
# Each mutant must (a) run to completion -- the probe prints its "checks=N failures=M" summary --
# and (b) produce a FAIL on the SPECIFIC named check(s) it targets. A mutant that merely exits
# non-zero does NOT count: the failure surface must be the named one.
#
# Usage: run_mutation_controls.sh [worktree-root]   (default: the tree this script lives in)

set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WT=${1:-$(CDPATH= cd -- "$HERE/../.." && pwd)}
PROBE="$WT/tests/probes/gh19s0_morph_map_probe.cpp"

[ -f "$PROBE" ] || { echo "cannot find $PROBE" >&2; exit 2; }
[ -d "$WT/core/include" ] || { echo "cannot find $WT/core/include" >&2; exit 2; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

CXX=${CXX:-c++}
INC="-I$WT/core/include -I$WT/generated -I$WT/host/include -I$WT/tests/host"
FLAGS="-std=c++17 -O2"
# The REFERENCE build carries the warning set the real builds use. The mutant builds deliberately do
# NOT: a mutation can make a variable unused (e.g. `if (false)` skips the block that uses it), which
# -Werror would reject for a reason that has nothing to do with the mutation under test.
REF_FLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror"

build() { # build <label> <extra-include-first> [flags]
  $CXX ${3:-$FLAGS} $2 $INC "$PROBE" -o "$TMP/$1.bin" 2> "$TMP/$1.build.log"
}

run() { # run <label> -> $TMP/<label>.out
  "$TMP/$1.bin" --out "$TMP/$1.out" --frames 1024 > "$TMP/$1.log" 2>&1
  echo $? > "$TMP/$1.rc"
}

summary() { # summary <label> -> the checks=N failures=M line
  grep -E '^checks=[0-9]+ failures=[0-9]+$' "$TMP/$1.log" | tail -1
}

# fires <label> <tag-prefix> : FAIL line for that tag present?
fires() {
  grep -q "^FAIL \[$2" "$TMP/$1.log"
}

fails=0
report() { # report <label> <expected-tag> <verdict>
  s=$(summary "$1")
  if [ -z "$s" ]; then
    echo "  WRONG-SURFACE  $1: the probe did not run to completion (no summary line)"
    fails=$((fails + 1))
    return
  fi
  if [ "$3" = yes ]; then
    echo "  ok             $1: $s ; FAIL [$2...] present  <-- the mutant IS caught, by name"
  else
    echo "  NOT-CAUGHT     $1: $s ; no FAIL [$2...]  <-- the check is NOT load-bearing"
    fails=$((fails + 1))
  fi
}

echo "== reference build (pristine production headers) =="
if ! build ref "-I$WT/core/include" "$REF_FLAGS"; then
  echo "  reference build FAILED:"; sed -n '1,20p' "$TMP/ref.build.log"; exit 2
fi
run ref
echo "  $(summary ref)  (expect failures=0)"

echo
echo "== mutant 1: FIXED WAVEFORM -- emittedAt_ takes the legacy branch unconditionally =="
echo "   patch: vco.h  'if (waveMap_ != VcoWaveMap::kLegacy) {'  ->  'if (false) {'"
mkdir -p "$TMP/m1"
cp -R "$WT/core/include" "$TMP/m1/include"
sed -i.bak 's/if (waveMap_ != VcoWaveMap::kLegacy) {/if (false) {  \/\/ MUTANT 1/' \
    "$TMP/m1/include/lunar24/core/vco.h"
grep -q 'MUTANT 1' "$TMP/m1/include/lunar24/core/vco.h" || { echo "  patch did not apply"; exit 2; }
if build m1 "-I$TMP/m1/include"; then
  run m1
  fires m1 "M1." && report m1 "M1." yes || report m1 "M1." no
else
  echo "  mutant build FAILED:"; sed -n '1,20p' "$TMP/m1.build.log"; fails=$((fails + 1))
fi

echo
echo "== mutant 2: WRONG SIDE -- the B morph control also writes A (cross-VCO leak) =="
echo "   patch: machine_runtime.h  setVcoBMorph also calls vcA_.setMorph(m)"
mkdir -p "$TMP/m2"
cp -R "$WT/core/include" "$TMP/m2/include"
sed -i.bak 's/void setVcoBMorph(double m) { vcB_.setMorph(m); }/void setVcoBMorph(double m) { vcB_.setMorph(m); vcA_.setMorph(m); }  \/\/ MUTANT 2/' \
    "$TMP/m2/include/lunar24/core/machine_runtime.h"
grep -q 'MUTANT 2' "$TMP/m2/include/lunar24/core/machine_runtime.h" || { echo "  patch did not apply"; exit 2; }
if build m2 "-I$TMP/m2/include"; then
  run m2
  fires m2 "M5.B" && report m2 "M5.B" yes || report m2 "M5.B" no
else
  echo "  mutant build FAILED:"; sed -n '1,20p' "$TMP/m2.build.log"; fails=$((fails + 1))
fi

echo
echo "== mutant 3: INTERVAL DISCONTINUITY -- the stretch weight is a step, not a crossfade =="
echo "   patch: vco_wave_map.h  sampleAt returns a or b instead of (1-u)*a + u*b"
mkdir -p "$TMP/m3"
cp -R "$WT/core/include" "$TMP/m3/include"
sed -i.bak 's/  return (1.0 - pos.u) \* a + pos.u \* b;/  return (pos.u < 0.5) ? a : b;  \/\/ MUTANT 3/' \
    "$TMP/m3/include/lunar24/core/vco_wave_map.h"
grep -q 'MUTANT 3' "$TMP/m3/include/lunar24/core/vco_wave_map.h" || { echo "  patch did not apply"; exit 2; }
if build m3 "-I$TMP/m3/include"; then
  run m3
  fires m3 "M2cont" && report m3 "M2cont" yes || report m3 "M2cont" no
else
  echo "  mutant build FAILED:"; sed -n '1,20p' "$TMP/m3.build.log"; fails=$((fails + 1))
fi

echo
echo "== detail: every FAIL line each mutant produced =="
for m in m1 m2 m3; do
  echo "  [$m]"
  grep -E '^FAIL ' "$TMP/$m.log" 2>/dev/null | sed 's/^/    /' || true
done

echo
if [ "$fails" -eq 0 ]; then
  echo "MUTATION CONTROLS: all 3 production mutants ran to completion and were caught by name."
  exit 0
fi
echo "MUTATION CONTROLS: $fails problem(s) -- see above."
exit 1
