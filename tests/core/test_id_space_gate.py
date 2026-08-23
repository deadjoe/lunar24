#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Id-space / bank-capacity gate (third review, item 1).

The state banks are indexed by the SERIALIZED numeric id, not by the count. So a
bank must be sized for the id-space (one-past-the-last id), and a SPARSE id — a
hole between two ids, or a jump past the bank — must be caught. This test:

  1. reads the emitted id-space constants from generated/registry_ids.hpp and the
     bank capacities from device_capacities.h, and asserts capacity >= id-space
     for parameter / jack / route (the three banks that are actually indexed);
  2. re-derives the same from the committed spec and cross-checks the two;
  3. demonstrates the hazard by injecting a sparse parameter id into a spec copy,
     regenerating the id-space, and asserting it now exceeds the bank — exactly
     what the compile-time static_assert in registry_ids.hpp would reject.

Usage:
  python3 tests/core/test_id_space_gate.py
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import generate_registry

SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")
IDS_HPP = os.path.join(ROOT, "generated", "lunar24", "registry_ids.hpp")
CAPS_HPP = os.path.join(ROOT, "core", "include", "lunar24", "core", "device_capacities.h")

# The three banks that are indexed by a serialized id -> their capacity + id-space.
BANKS = (
    ("parameter", "param", "kDeviceParamCapacity", "kParameterIdSpace"),
    ("jack", "patch", "kDevicePatchCapacity", "kJackIdSpace"),
    ("route", "route", "kDeviceRouteCapacity", "kRouteIdSpace"),
)


def _const_decl(path, name):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    for ln in text.splitlines():
        if re.search(rf"\b{name}\s*=", ln):
            return ln.strip()
    raise SystemExit(f"constant {name} not found in {path}")


def _const(path, name):
    decl = _const_decl(path, name)
    m = re.search(r"= (\d+)", decl)
    if not m:
        raise SystemExit(f"constant {name} has no numeric value in {path}")
    return int(m.group(1))


def main():
    with open(SPEC_PATH, encoding="utf-8") as fh:
        spec = json.load(fh)
    reg = generate_registry.Registry(spec)

    id_space_from_spec = {
        "parameter": generate_registry._id_space(reg.parameter_ids),
        "jack": generate_registry._id_space(reg.jack_ids),
        "route": generate_registry._id_space(reg.route_ids),
    }

    bad = []
    for kind, _, cap_name, space_name in BANKS:
        cap = _const(CAPS_HPP, cap_name)
        space_emitted = _const(IDS_HPP, space_name)
        space_from_spec = id_space_from_spec[kind]
        # emitted id-space must match a fresh derivation (guards the generator,
        # so a stale generated header is caught by this test, not by the build).
        if space_emitted != space_from_spec:
            bad.append(f"{space_name} emitted {space_emitted} != derived {space_from_spec}")
        # id-space is declared uint64_t (never uint32_t): one-past the largest
        # legal u32 id is 0x100000000, which a uint32_t literal cannot hold. The
        # type lock proves the full u32 id domain stays representable.
        decl = _const_decl(IDS_HPP, space_name)
        if "std::uint64_t" not in decl:
            bad.append(f"{space_name} must be std::uint64_t so one-past-of-max-u32-id "
                       f"stays representable (got {decl!r})")
        # the bank must cover the id-space (a sparse id is a hole, not a shrunk count).
        if cap < space_emitted:
            bad.append(f"{cap_name} ({cap}) < id-space {space_emitted}")

    if bad:
        raise SystemExit("id-space gate violations:\n  " + "\n  ".join(bad))

    # --- sparse-id hazard demonstration -------------------------------
    sparse = json.loads(json.dumps(spec))
    # Send one parameter id past the param bank: a valid uint32 id, but a hole
    # that pushes id-space beyond the bank the C++ static_assert protects.
    sparse_id = 1000
    sparse["modules"][0]["parameters"][0]["id"] = sparse_id
    reg_sp = generate_registry.Registry(sparse)  # validates OK: id is in uint32 range
    space_sp = generate_registry._id_space(reg_sp.parameter_ids)
    param_cap = _const(CAPS_HPP, "kDeviceParamCapacity")
    if space_sp != sparse_id + 1:
        raise SystemExit(f"id-space should be maxId+1 ({space_sp} != {sparse_id}+1)")
    if space_sp <= param_cap:
        raise SystemExit("hazard spec did not push id-space past the bank (test is stale)")

    # --- 0xFFFFFFFF upper-bound regression -----------------------------
    # The largest legal u32 id. one-past-the-last = 0x100000000, which does not
    # fit uint32_t. The generator must emit it as a uint64_t literal so the
    # header stays well-formed; only the capacity static_assert rejects this
    # spec (the one-past outruns the bank) — never a malformed/overflowed literal.
    boundary = json.loads(json.dumps(spec))
    boundary["modules"][0]["parameters"][0]["id"] = 0xFFFFFFFF
    reg_b = generate_registry.Registry(boundary)  # validates OK: 0xFFFFFFFF is a legal u32 id
    space_b = generate_registry._id_space(reg_b.parameter_ids)
    if space_b != 0x100000000:
        raise SystemExit(f"boundary id-space should be 2^32 ({space_b} != 0x100000000)")
    if space_b <= param_cap:
        raise SystemExit("boundary id-space should exceed the param bank (capacity gate must reject it)")

    print(f"OK: id-space <= bank (parameter {_const(IDS_HPP, 'kParameterIdSpace')}, "
          f"jack {_const(IDS_HPP, 'kJackIdSpace')}, route {_const(IDS_HPP, 'kRouteIdSpace')}); "
          f"hazard spec id={sparse_id} -> id-space {space_sp} > param bank {param_cap} (would trip "
          f"the generated static_assert); boundary id=0xFFFFFFFF -> id-space {space_b} (uint64, "
          f"exceeds bank {param_cap} -> rejected by the capacity gate, not a malformed literal)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
