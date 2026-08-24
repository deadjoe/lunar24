#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative control for test_jack_id_baseline.py: the frozen-baseline comparison
MUST flag a renumbering (the @Claude half: "任何重编号必须报红"). We feed it a
map that simulates a mid-registry insert (a jack pushed in before an existing
one, so a later jack's numeric id shifts) and assert the comparator reports the
drift rather than passing.

This is not a spec fixture — it drives the exact compare function the main test
uses, so a config that accidentally stops detecting shifts breaks here first.

Run: python3 tests/core/test_jack_id_baseline_negative.py
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_jack_id_baseline as baseline


def main():
    with open(baseline.BASELINE_PATH, encoding="utf-8") as fh:
        base = json.load(fh)

    if len(base) < 2:
        raise SystemExit("baseline too small to exercise a shift")

    # Pick an id that is NOT the minimum, so we can bump-and-shift the next one
    # and still have a "new" id in the safety range below it.
    shifted = dict(base)
    # Simulate an insert at stable id "vco_a.cv_in" region by shifting the
    # numeric id upward for the least-numeric stable id; this is the classic
    # "insert a jack before an existing one" renumber.
    low_sid = min(base, key=lambda s: base[s])
    shifted[low_sid] = base[low_sid] + 1  # an existing jack renumbered

    ok, problems = baseline.jack_map_matches(shifted, base)
    if ok:
        raise SystemExit(
            "FAIL: comparator passed a renumbered JackId map — "
            "the cross-edit red-flag is not firing")
    if not problems:
        raise SystemExit("FAIL: comparator reported a mismatch but no shifts")

    print(f"OK: renumber of {low_sid} correctly flagged "
          f"({len(problems)} problem reported)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
