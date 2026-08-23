#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative tests for the registry completeness gate (Phase A).

The gate must reject each defect for its INTENDED reason, not stumble onto it by
accident. We mutate a copy of the real manifest/spec and assert the resulting
`problems` list contains the matching diagnostic (重件 / no-evidence /
NON-REGRESSION / --require-full), and that the un-mutated baseline passes.

Usage:
  python3 tests/core/test_registry_complete_negative.py
"""

import copy
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import check_registry_complete as gate

SPEC = os.path.join(ROOT, "spec", "machine", "lunar24.json")
MANIFEST = os.path.join(ROOT, "spec", "machine", "p0_inventory_manifest.json")


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def has(problems, needle):
    return any(needle in p for p in problems)


def main():
    spec = load(SPEC)
    manifest = load(MANIFEST)

    # Baseline must pass (coherent, non-regressing; full not required).
    problems, _ = gate.check(spec, manifest)
    if problems:
        raise SystemExit("baseline gate should pass, got problems: %r" % problems)

    # 1. duplicate target module -> 重件
    dup = copy.deepcopy(manifest)
    dup["target"]["modules"].append(copy.deepcopy(dup["target"]["modules"][0]))
    problems, _ = gate.check(spec, dup)
    if not has(problems, "duplicate"):
        raise SystemExit("duplicate module not flagged as 重件: %r" % problems)

    # 2. evidence-less transcribed module -> no-evidence
    noev = copy.deepcopy(manifest)
    noev["target"]["modules"][0].pop("evidence")
    problems, _ = gate.check(spec, noev)
    if not has(problems, "no evidence"):
        raise SystemExit("evidence-less module not flagged: %r" % problems)

    # 3. cartridge with wrong program count -> must list N programs
    cart = copy.deepcopy(manifest)
    cart["target"]["cartridges"][0]["programs"] = ["Shimmer"]
    problems, _ = gate.check(spec, cart)
    if not has(problems, "must list 3 programs"):
        raise SystemExit("short cartridge not flagged: %r" % problems)

    # 4. non-regression: a mustComplete item that the registry no longer provides.
    regress = copy.deepcopy(manifest)
    regress["mustComplete"].append("module:drone_1")  # a manifest target, NOT in registry
    problems, _ = gate.check(spec, regress)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("dropped mustComplete item not flagged as non-regression: %r" % problems)

    # 5. --require-full rejects an incomplete (but coherent) manifest.
    problems, _ = gate.check(spec, manifest, require_full=True)
    if not has(problems, "--require-full"):
        raise SystemExit("incomplete manifest did not trip --require-full: %r" % problems)
    if not has(problems, "program identities gaps"):
        raise SystemExit("--require-full did not report the program-identity gap: %r" % problems)

    print("OK: registry completeness gate rejects each defect for its intended reason; "
          "baseline passes; --require-full trips on the known gaps.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
