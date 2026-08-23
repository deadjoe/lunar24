#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Full-machine completeness gate for the P0 registry (Phase A).

The registry (spec/machine/lunar24.json) is the IMPLEMENTATION. The manifest
(spec/machine/p0_inventory_manifest.json) is the INDEPENDENT full-machine TARGET
transcribed from the panel map (design/04 §1) and the manual-derived inventories
(design/01 §3-§4, design/05 §1) — NOT generated from the registry, so completeness
is auditable rather than self-proving (Codex constraint #1).

The gate does two things:

  1. CONSISTENCY INVARIANTS (always enforced, green on every commit):
     - no duplicate target key inside the manifest (重件);
     - every target module / cartridge / route carries its provenance evidence
       (source + panel-site or manual line) so a blank transcription is rejected;
     - every cartridge names exactly one program per slot (3 total);
     - every `mustComplete` item must (a) name a coherent manifest target and
       (b) still exist in the REGISTRY — that clause is the monotonic
       NON-REGRESSION guard (constraint #3): a commit may be <100% but can never
       drop an already-implemented item.

  2. CATEGORY-WISE COVERAGE REPORT (constraint #2): target / present / gap per
     category — modules / operable-controls+parameters / jacks / normalized+fixed
     routes / program identities. Numbers are NEVER collapsed into one percentage
     that could hide a gap. Parameter and jack *target* counts are a Phase-B
     per-module content enumeration, so until then those two categories are
     reported as "present-in-implemented-modules" with an explicit
     "Phase-B enumeration pending" target marker rather than a fabricated count.

With `--require-full` the gate additionally requires every target module, program
identity and route to be present. The CI runs this mode so the PR #2 exact head
enables it for real (constraint #3), instead of shipping an unused option.

Usage:
  python3 tools/check_registry_complete.py [--require-full]
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import generate_registry

SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")
MANIFEST_PATH = os.path.join(ROOT, "spec", "machine", "p0_inventory_manifest.json")

PROGRAM_SLOTS = (1, 2, 3)


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def evidence_ok(entry):
    """A transcribed item must carry at least a source ref OR a panel-site."""
    ev = entry.get("evidence") or {}
    refs = ev.get("ref") or ev.get("source") or ev.get("panelSite")
    return bool(refs) and bool(entry.get("status"))


def check(spec, manifest, require_full=False):
    """Return (problems, coverage). problems is [] iff the gate passes."""
    problems = []
    try:
        reg = generate_registry.Registry(spec)
    except ValueError as exc:
        return [f"registry invalid: {exc}"], None

    meta = manifest.get("meta") or {}
    if not meta.get("source"):
        problems.append("manifest.meta.source missing (must name the source)")
    tgt = manifest.get("target") or {}
    modules = tgt.get("modules", [])
    cartridges = tgt.get("cartridges", [])
    routes = tgt.get("normalizedRoutes", [])
    must_complete = manifest.get("mustComplete", [])

    module_ids = [m.get("stable_id") for m in modules]
    if len(module_ids) != len([x for x in module_ids if x is not None]) or \
       len(module_ids) != len(set(module_ids)):
        problems.append("manifest.target.modules has a duplicate or empty stable_id (重件)")

    for m in modules:
        if not (m.get("stable_id") and m.get("name") and m.get("category")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs stable_id+name+category")
        if not evidence_ok(m):
            problems.append(f"manifest module {m.get('stable_id')!r}: no evidence (source/panel-site)")

    for c in cartridges:
        if not c.get("cassette"):
            problems.append("manifest cartridge: missing cassette name")
        if len(c.get("programs", [])) != len(PROGRAM_SLOTS):
            problems.append(f"manifest cartridge {c.get('cassette')!r}: must list {len(PROGRAM_SLOTS)} programs")
        if not evidence_ok(c):
            problems.append(f"manifest cartridge {c.get('cassette')!r}: no evidence")

    for r in routes:
        if not (r.get("stable_id") and r.get("sourceJack") and r.get("sinkJack")):
            problems.append(f"manifest route {r.get('stable_id')!r}: needs stable_id+sourceJack+sinkJack")
        if not evidence_ok(r):
            problems.append(f"manifest route {r.get('stable_id')!r}: no evidence")

    target_modules = set(module_ids)
    target_program_ids = {
        f"program.{generate_registry.sanitize(c.get('cassette', '').lower())}.{slot}"
        for c in cartridges
        for slot in PROGRAM_SLOTS
    }
    target_routes = {r["stable_id"] for r in routes if r.get("stable_id")}

    present_modules = {m["stable_id"] for m in reg.modules}
    present_programs = {p["stable_id"] for p in reg.programs}
    present_routes = {r["stable_id"] for r in reg.routes}
    params_present = len(reg.parameters)
    jacks_present = len(reg.jacks)

    # mustComplete coherence: each names a manifest target AND the registry.
    target_all = (set(f"module:{x}" for x in target_modules)
                  | set(f"program:{x}" for x in target_program_ids)
                  | set(f"route:{x}" for x in target_routes))
    present_keys = (set(f"module:{x}" for x in present_modules)
                    | set(f"program:{x}" for x in present_programs)
                    | set(f"route:{x}" for x in present_routes))
    for key in must_complete:
        if key not in target_all:
            problems.append(f"mustComplete item {key!r} is not a manifest target "
                            f"(manifest itself is incoherent)")
        if key not in present_keys:
            problems.append(f"NON-REGRESSION: mustComplete item {key!r} dropped from the registry")

    def cover(target, present):
        return {"target": len(target), "present": len(target & present), "gap": sorted(target - present)}

    coverage = {
        "registry": {
            "modules": len(reg.modules), "params": params_present,
            "jacks": jacks_present, "routes": len(reg.routes), "programs": len(reg.programs),
        },
        "manifest": {
            "modules": len(modules), "cartridges": len(cartridges),
            "programIdentities": len(target_program_ids), "routes": len(routes),
            "mustComplete": len(must_complete),
        },
        "modules": cover(target_modules, present_modules),
        "programIdentities": cover(target_program_ids, present_programs),
        "routes": cover(target_routes, present_routes),
        "parameters": {
            "target": "Phase-B per-module enumeration (pending)",
            "present": params_present,
            "presentModules": sorted({m["_stable_owner"] for m in reg.parameters}),
        },
        "jacks": {
            "target": "Phase-B per-module enumeration (pending)",
            "present": jacks_present,
            "presentModules": sorted({j["_module_stable"] for j in reg.jacks}),
        },
    }

    if require_full:
        not_full = []
        if coverage["programIdentities"]["gap"]:
            not_full.append("program identities gaps=%s" % coverage["programIdentities"]["gap"])
        if coverage["routes"]["gap"]:
            not_full.append("route gaps=%s" % coverage["routes"]["gap"])
        if coverage["modules"]["gap"]:
            not_full.append("module gaps=%s" % coverage["modules"]["gap"])
        if not_full:
            problems.append("--require-full: " + "; ".join(not_full))

    return problems, coverage


def format_report(coverage):
    lines = []
    reg = coverage["registry"]
    lines.append("registry: %(modules)d modules / %(params)d params / %(jacks)d jacks / "
                 "%(routes)d routes / %(programs)d programs" % reg)
    m = coverage["manifest"]
    lines.append("manifest: %(modules)d modules / %(cartridges)d cartridges(%(programIdentities)d "
                 "program identities) / %(routes)d routes / %(mustComplete)d mustComplete" % m)
    lines.append("")
    lines.append("coverage (target / present / gap):")
    lines.append("  modules            : %(target)d / %(present)d / gaps=%(gap)s" % coverage["modules"])
    lines.append("  program identities : %(target)d / %(present)d / gaps=%(gap)s" % coverage["programIdentities"])
    lines.append("  normalized routes  : %(target)d / %(present)d / gaps=%(gap)s" % coverage["routes"])
    lines.append("  parameters         : target=%(target)s  present=%(present)d" % coverage["parameters"])
    lines.append("  jacks              : target=%(target)s  present=%(present)d" % coverage["jacks"])
    lines.append("")
    lines.append("note: 39 program identities is an identity list, NOT 39 implemented effects.")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--require-full", action="store_true",
                    help="fail unless every target module/program/route is present")
    args = ap.parse_args()

    problems, coverage = check(load_json(SPEC_PATH), load_json(MANIFEST_PATH),
                               require_full=args.require_full)
    lines = format_report(coverage)
    if problems:
        lines.append("")
        lines.append("FAIL:")
        for p in problems:
            lines.append("  - " + p)
        print("\n".join(lines))
        return 1
    lines.append("")
    lines.append("OK: manifest self-coherent; mustComplete non-regressing.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
