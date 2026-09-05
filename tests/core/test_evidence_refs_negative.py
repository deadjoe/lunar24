#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative tests for the EvidenceRef gate.

The gate must reject each defect for its INTENDED reason, not stumble onto it by
accident. Two layers:

  1. a synthetic single-ref registry text is driven through `check()` for each defect
     out-of-range, lineStart>lineEnd, blank lineStart, blank lineEnd, unknown source --
     and we assert the returned `problems` list contains exactly the matching diagnostic;
  2. the REAL generated registry is checked once as a baseline (must pass), and once
     with a single EvidenceRef's lineEnd flipped to a real blank manual line (must fail
     with the "blank line" diagnostic).

Scope note carried from the gate: this verifies the MECHANICAL grounding of a ref, not
its relevance. None of these tests claim a flipped ref is semantically wrong, only that
the gate flags it as structurally ungrounded.

Usage:
  python3 tests/core/test_evidence_refs_negative.py
"""

import json
import os
import sys

# Windows' default stdout encoding (cp1252) cannot encode the Unicode this test
# prints; force UTF-8 so stdout is identical on every runner.
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import check_evidence_refs as gate

REGISTRY = os.path.join(ROOT, "generated", "lunar24", "registry.hpp")
MANUAL = os.path.join(ROOT, "design", "reference", "solar42N_manual_text.txt")
LAYOUT = os.path.join(ROOT, "generated", "lunar24", "evidence_layout.json")


def real_source():
    """The real manual as a check() source — live text if present, else committed layout.

    The manual is gitignored, so it exists on the dev machine but not on CI (this test
    then must not open it). Two-mode matches the gate: ground against the real text when
    it is there, against the committed fingerprint when it is not. On CI that means we
    still exercise the REAL registry baseline + a real blank-flip, via the committed
    blankLines — the exact absent-manual path the gate uses there.
    """
    if os.path.exists(MANUAL):
        return gate.parse_manual(open(MANUAL, encoding="utf-8").read())
    layout = json.load(open(LAYOUT, encoding="utf-8"))["sources"]["solar42N_manual_v15"]
    return gate.LayoutSource(layout)


def real_blank_lines():
    """The real manual's blank line numbers — live if present, else from the committed layout."""
    if os.path.exists(MANUAL):
        lines = gate.parse_manual(open(MANUAL, encoding="utf-8").read())
        return [i + 1 for i, ln in enumerate(lines) if ln.strip() == ""]
    layout = json.load(open(LAYOUT, encoding="utf-8"))["sources"]["solar42N_manual_v15"]
    return [int(b) for b in layout["blankLines"]]

# A tiny deterministic manual with known blank lines (index 0-based -> line n+1).
SYN_MANUAL = ["line one", "", "line three", "line four", "", "line six"]


def has(problems, needle):
    return any(needle in p for p in problems)


def run_synthetic(registry_text, manual_lines):
    return gate.check(registry_text, {"solar42N_manual_v15": manual_lines})


def test_defects():
    # Each synthetic ref exercises one defect; assert the matching diagnostic.
    cases = [
        # (registry text, expected diagnostic fragment)
        ('''X EvidenceRef{"solar42N_manual_v15", 0u, 6u} Y''', "out of range"),
        ('''X EvidenceRef{"solar42N_manual_v15", 1u, 999u} Y''', "out of range"),
        ('''X EvidenceRef{"solar42N_manual_v15", 4u, 2u} Y''', "lineStart 4 > lineEnd 2"),
        ('''X EvidenceRef{"solar42N_manual_v15", 2u, 4u} Y''', "lineStart 2 is a blank line"),
        ('''X EvidenceRef{"solar42N_manual_v15", 1u, 5u} Y''', "lineEnd 5 is a blank line"),
        ('''X EvidenceRef{"other_manual", 1u, 2u} Y''', "unknown source document"),
    ]
    failures = []
    for text, expected in cases:
        problems = run_synthetic(text, SYN_MANUAL)
        if not has(problems, expected):
            failures.append(f"expected '{expected}' in {problems} for {text}")
    return failures


def test_real_baseline_passes():
    reg_text = open(REGISTRY, encoding="utf-8").read()
    problems = gate.check(reg_text, {"solar42N_manual_v15": real_source()})
    if problems:
        return [f"real registry should pass but flagged {len(problems)} problem(s): {problems[:3]}"]
    return []


def test_real_flip_to_blank_goes_red():
    reg_text = open(REGISTRY, encoding="utf-8").read()

    # Find a real blank manual line that is IN RANGE and comes AFTER the first ref's
    # own lineStart, so flipping that ref's lineEnd yields the "blank" diagnostic rather
    # than an out-of-range / a>b one. First ref is {lineStart=376, lineEnd=422}. Works
    # from the live text (dev) or the committed blankLines (CI absent-manual path).
    first_start = int(gate.collect_refs(reg_text)[0][1])
    blank_after_start = next(
        (b for b in real_blank_lines() if b > first_start),
        None,
    )
    if blank_after_start is None:
        return ["could not locate a blank manual line after first ref to serve as flip target"]

    # Flip ONLY the first EvidenceRef's lineEnd to that blank line. The source string
    # in m.group(1) is captured WITHOUT its surrounding quotes, so re-emit them.
    def flip_first_end(m):
        return f'EvidenceRef{{"{m.group(1)}", {m.group(2)}u, {blank_after_start}u}}'

    mutated = gate.REF_RE.sub(flip_first_end, reg_text, count=1)
    problems = gate.check(mutated, {"solar42N_manual_v15": real_source()})
    if not has(problems, f"lineEnd {blank_after_start} is a blank line"):
        return [f"flipped ref should flag blank lineEnd {blank_after_start} but got {problems[:3]}"]
    return []


def test_absent_mode_blank_detection():
    # The CI path grounds refs against the committed LAYOUT, not live text. A ref whose
    # boundary falls on a blank line (per the committed blankLines) must be caught even
    # though the manual text is not on disk. Drive check() with a LayoutSource built from
    # SYN_MANUAL's fingerprint — the same code CI exercises. SYN_MANUAL lines 2 and 5 are
    # blank.
    bl = gate.blank_lines(SYN_MANUAL)
    layout = {"lineCount": len(SYN_MANUAL), "blankLines": bl}
    src_map = {"solar42N_manual_v15": gate.LayoutSource(layout)}

    problems = gate.check('X EvidenceRef{"solar42N_manual_v15", 1u, 2u} Y', src_map)
    if not has(problems, "lineEnd 2 is a blank line"):
        return [f"absent-mode: ref ending on blank line 2 not flagged: {problems[:3]}"]
    problems = gate.check('X EvidenceRef{"solar42N_manual_v15", 5u, 6u} Y', src_map)
    if not has(problems, "lineStart 5 is a blank line"):
        return [f"absent-mode: ref starting on blank line 5 not flagged: {problems[:3]}"]
    problems = gate.check('X EvidenceRef{"solar42N_manual_v15", 1u, 3u} Y', src_map)
    if problems:
        return [f"absent-mode: clean ref should pass but got {problems[:3]}"]
    return []


def main():
    failures = []
    failures += test_defects()
    failures += test_real_baseline_passes()
    failures += test_real_flip_to_blank_goes_red()
    failures += test_absent_mode_blank_detection()
    if failures:
        print("FAIL: EvidenceRef negative control")
        for f in failures:
            print(f"  {f}")
        return 1
    print("OK: EvidenceRef gate negative control — each defect rejected for its intended reason; "
          "real registry baseline passes; a single ref flipped to a blank line makes the gate "
          "go red; absent-mode (CI) LayoutSource catches a blank boundary without live text")
    return 0


if __name__ == "__main__":
    sys.exit(main())
