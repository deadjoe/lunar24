#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Reorder-regression for stable numeric identity (design/07 §2 identity; review
blocker #1).  Serialized ids must be the explicit, immutable numeric `id` carried
by each entity, never an index derived from JSON order.

This test reorders the committed spec (reverse module order, and each module's
parameter/jack lists) and asserts that the stable_id → numeric id mapping is
unchanged. If it were order-dependent, reversing the JSON would renumber the
enums and this test would fail.

Usage:
  python3 tests/core/test_id_stability.py
"""

import copy
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import generate_registry

SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")


def stable_map(reg):
    return {
        "module": {sid: nid for nid, sid, _ in reg.module_ids},
        "parameter": {sid: nid for nid, sid, _ in reg.parameter_ids},
        "jack": {sid: nid for nid, sid, _ in reg.jack_ids},
        "program": {sid: nid for nid, sid, _ in reg.program_ids},
    }


def main():
    with open(SPEC_PATH, encoding="utf-8") as fh:
        spec = json.load(fh)

    orig = generate_registry.Registry(spec)
    base = stable_map(orig)
    if not base["module"] or not base["parameter"] or not base["jack"]:
        raise SystemExit("spec has no entities to test")

    reordered = copy.deepcopy(spec)
    reordered["modules"] = list(reversed(reordered["modules"]))
    for m in reordered["modules"]:
        m["parameters"] = list(reversed(m.get("parameters", [])))
        m["jacks"] = list(reversed(m.get("jacks", [])))
    reordered["programs"] = list(reversed(reordered["programs"]))

    reg = generate_registry.Registry(reordered)
    now = stable_map(reg)

    # The numeric id for every stable id must be identical after a reorder.
    for kind in base:
        if base[kind] != now[kind]:
            for sid in base[kind]:
                if base[kind][sid] != now[kind].get(sid):
                    raise SystemExit(
                        f"ORDER-DEPENDENT id for {kind} {sid}: "
                        f"{base[kind][sid]} -> {now[kind].get(sid)}")
            raise SystemExit(f"id map size changed for {kind}")

    print(f"OK: {len(base['module'])} modules, {len(base['parameter'])} params, "
          f"{len(base['jack'])} jacks, {len(base['program'])} programs keep "
          f"stable numeric ids under a reorder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
