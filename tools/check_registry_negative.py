#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative gate: every tests/core/reg_negative/*.json fixture must be rejected by
the registry validator *for its intended reason*.

This is a message-asserting wrapper, so a fixture that fails for an accidental
reason (e.g. a schema change) no longer silently passes: it must raise a
ValueError whose text contains the fixture's `meta.expectedError`. A fixture that
is unexpectedly ACCEPTED (validator got lax) is a failure, not a pass.

Usage:
  python3 tools/check_registry_negative.py
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import generate_registry

NEG_DIR = os.path.join(ROOT, "tests", "core", "reg_negative")


def main():
    names = sorted(n for n in os.listdir(NEG_DIR) if n.endswith(".json"))
    failures = []
    for name in names:
        path = os.path.join(NEG_DIR, name)
        with open(path, encoding="utf-8") as fh:
            spec = json.load(fh)
        expected = (spec.get("meta") or {}).get("expectedError", "")
        try:
            generate_registry.Registry(spec)
        except ValueError as exc:
            msg = str(exc)
            if expected and expected.lower() in msg.lower():
                print(f"  [{name}] OK — rejected for the intended reason: {msg}")
                continue
            failures.append(
                f"  [{name}] rejected but NOT for the intended reason "
                f"(expected {expected!r}, got {msg!r})")
            continue
        failures.append(f"  [{name}] ACCEPTED — validator should have rejected it")
    if failures:
        print("NEGATIVE GATE FAILED:")
        for f in failures:
            print(f)
        return 1
    print(f"OK: all {len(names)} negative fixtures rejected for their intended reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
