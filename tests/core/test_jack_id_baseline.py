#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Cross-edit stability regression for numeric JackIds (design/07 §2 identity;
@Claude add-on 1, msg 5fbc797c).

`test_id_stability.py` proves a *reorder* of the spec does not renumber the
enums. This test freezes the *current* stable-id → numeric-JackId map as a
committed baseline and asserts a freshly-built registry reproduces it exactly.
It is the guard @Claude asked for: "把当前 JackId 映射记成基线，回归比对，任何
重编号必须报红——这就是你现在最可能出错的地方."

The point is not to forbid ever renumbering; it is to make a renumbering a
*deliberate, visible* change. Adding a genuinely new jack is fine because it
never shifts an existing id (the generator assigns ids in spec order; appending
a jack to the end keeps all prior ids). But inserting a jack *into the middle*
of the spec shifts every later id, and that shift must change this baseline —
so the diff is explicit and reviewable rather than silent. The ONLY way to move
on after such a change is to also update the baseline here, which is exactly
the red-flag discipline @Claude wants.

Run: python3 tests/core/test_jack_id_baseline.py
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import generate_registry

SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")
BASELINE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "jack_id_baseline.json")


def current_jack_map(reg):
    """stable_id -> numeric JackId, as the generator serializes them."""
    return {sid: nid for nid, sid, _ in reg.jack_ids}


def jack_map_matches(now, baseline):
    """Return (ok, problems) comparing a freshly-built map to the baseline.

    `problems` lists every stable id whose numeric id shifted. Exposed as a
    function so the negative control exercises the exact red-flag logic without
    rebuilding a full spec fixture.
    """
    problems = []
    for sid in sorted(set(baseline) | set(now)):
        old = baseline.get(sid)
        new = now.get(sid)
        if old != new:
            problems.append(f"{sid}: {old!r} -> {new!r}")
    return (not problems), problems


def main():
    with open(SPEC_PATH, encoding="utf-8") as fh:
        spec = json.load(fh)
    with open(BASELINE_PATH, encoding="utf-8") as fh:
        baseline = json.load(fh)  # {sid: nid}

    reg = generate_registry.Registry(spec)
    now = current_jack_map(reg)

    ok, problems = jack_map_matches(now, baseline)
    if not ok:
        raise SystemExit(
            "JackId map drifted from the frozen baseline. Inspect the shift:\n"
            "  " + "\n  ".join(problems) + "\n"
            "If a NEW jack was appended (no renumber), regenerate the baseline.\n"
            "If an existing jack renumbered, that is the red @Claude wants —\n"
            "re-anchor the ids before swallowing a shift.")

    print(f"OK: {len(baseline)} jacks keep the frozen numeric JackIds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
