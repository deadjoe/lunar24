#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative tests for the registry completeness gate (Phase A).

The gate must reject each defect for its INTENDED reason, not stumble onto it by
accident. We mutate a copy of the real manifest/spec and assert the resulting
`problems` list contains the matching diagnostic, and that the un-mutated baseline
passes. Covers the tightened checks added to close the Phase-A false-green:

  重件 (duplicate module / program / route / param / endpoint),
  incomplete or back-formation evidence, a cartridge row missing a slot,
  a dangling or misdirected route endpoint, a present-but-empty module,
  mustComplete != the landed set, --require-full, AND the second-review (msg 143d0038)
  gates: implementation ⊆ independent target, the frozen complete fixed chain
  (required fixed route missing), ORCHE slot 2/3 provisional enforcement,
  terminal-owned endpoints validated, and a dangling-fixed-endpoint no-continue.

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
    if not has(problems, "duplicate stable_id"):
        raise SystemExit("duplicate module not flagged as 重件: %r" % problems)

    # 2. duplicate target program -> per-category 重件 (was silently set-folded before)
    dupp = copy.deepcopy(manifest)
    dupp["target"]["programs"].append(copy.deepcopy(dupp["target"]["programs"][0]))
    problems, _ = gate.check(spec, dupp)
    if not has(problems, "program duplicate stable_id"):
        raise SystemExit("duplicate program not flagged as per-category 重件: %r" % problems)

    # 3. duplicate normalized route -> per-category 重件
    dupr = copy.deepcopy(manifest)
    dupr["target"]["normalizedRoutes"].append(copy.deepcopy(dupr["target"]["normalizedRoutes"][0]))
    problems, _ = gate.check(spec, dupr)
    if not has(problems, "normalized route duplicate stable_id"):
        raise SystemExit("duplicate normalized route not flagged as 重件: %r" % problems)

    # 4. evidence-less module / back-formation provenance (ref only, no location)
    noev = copy.deepcopy(manifest)
    noev["target"]["modules"][0]["evidence"] = {"ref": "solar42N_manual_v15"}
    problems, _ = gate.check(spec, noev)
    if not has(problems, "incomplete evidence"):
        raise SystemExit("ref-only evidence not rejected (provenance shape too loose): %r" % problems)

    # 5. cartridge row missing a slot -> must list all N slots
    cart = copy.deepcopy(manifest)
    cart["target"]["programs"] = [p for p in cart["target"]["programs"]
                                  if not (p["cartridge"] == "CATHEDRAL" and p["slot"] == 2)]
    problems, _ = gate.check(spec, cart)
    if not has(problems, "must list all 3 slots"):
        raise SystemExit("cartridge missing a slot not flagged: %r" % problems)

    # 6. dangling normalized route endpoint (module present, jack not registered)
    dang = copy.deepcopy(manifest)
    dang["target"]["normalizedRoutes"][0]["sinkJack"] = "vco_a.not_real"
    problems, _ = gate.check(spec, dang)
    if not has(problems, "dangling"):
        raise SystemExit("dangling route endpoint not flagged: %r" % problems)

    # 7. misdirected fixed route (source is an input endpoint -> must be output)
    misdir = copy.deepcopy(manifest)
    misdir["target"]["fixedRoutes"][0]["source"] = "vcf.cv_l_in"  # vcf.cv_l_in is an INPUT
    problems, _ = gate.check(spec, misdir)
    if not has(problems, "expected output"):
        raise SystemExit("misdirected fixed-route source not flagged: %r" % problems)

    # 8. present-but-empty module (registry module with no parameters)
    emptyspec = copy.deepcopy(spec)
    for m in emptyspec["modules"]:
        if m.get("stable_id") == "vco_a":
            m["parameters"] = []
    problems, _ = gate.check(emptyspec, manifest)
    if not has(problems, "NO parameters"):
        raise SystemExit("present-but-empty module not flagged: %r" % problems)

    # 9. non-regression: a mustComplete item that the registry no longer provides.
    regress = copy.deepcopy(manifest)
    regress["mustComplete"].append("module:drone_1")  # a manifest target, NOT in registry
    problems, _ = gate.check(spec, regress)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("dropped mustComplete item not flagged as non-regression: %r" % problems)

    # 10. mustComplete != the landed set (must sync future additions into it).
    unsync = copy.deepcopy(manifest)
    unsync["mustComplete"] = [k for k in unsync["mustComplete"]
                              if k != "module:vco_b"]  # remove a landed key -> mismatch
    problems, _ = gate.check(spec, unsync)
    if not has(problems, "mustComplete != registry landed target keys"):
        raise SystemExit("mustComplete not required to match the landed set: %r" % problems)

    # 11. --require-full rejects an incomplete (but coherent) manifest.
    problems, _ = gate.check(spec, manifest, require_full=True)
    if not has(problems, "--require-full"):
        raise SystemExit("incomplete manifest did not trip --require-full: %r" % problems)
    if not has(problems, "program identity gaps"):
        raise SystemExit("--require-full did not report the program-identity gap: %r" % problems)

    # 12. implementation ⊆ independent target: a registry PROGRAM not in the manifest is an error
    #     (was silently green before the hard gate).
    rogue = copy.deepcopy(spec)
    rogue["programs"].append(copy.deepcopy(rogue["programs"][0]))
    rogue["programs"][-1]["id"] = 99
    rogue["programs"][-1]["stable_id"] = "program.rogue"
    rogue["programs"][-1]["cartridge"] = "ROGUE"
    # Programs also carry the X/Y/Z knob parameters; re-id them so they don't collide
    # with the module param id space (the registry validates a globally-unique param id).
    for i, parm in enumerate(rogue["programs"][-1]["parameters"]):
        parm["id"] = 200 + i
        parm["stable_id"] = "program.rogue.%s" % parm["stable_id"].split(".")[-1]
    problems, _ = gate.check(rogue, manifest)
    if not has(problems, "impl ⊄ target"):
        raise SystemExit("registry item absent from the independent target not flagged: %r" % problems)

    # 13. frozen fixed chain: removing a required fixed route trips --require-full even though
    #     other routes are defined (no false-green on the fixed topology).
    cut = copy.deepcopy(manifest)
    cut["target"]["fixedRoutes"] = [r for r in cut["target"]["fixedRoutes"]
                                    if r["stable_id"] != "fixed.eff_l_to_wet_l"]
    problems, _ = gate.check(spec, cut, require_full=True)
    if not has(problems, "required fixed routes missing"):
        raise SystemExit("missing required fixed route did not trip --require-full: %r" % problems)

    # 14. ORCHE slot 2/3 must be provisional (manual prints 'Program 1' for all three).
    orche = copy.deepcopy(manifest)
    for p in orche["target"]["programs"]:
        if p.get("cartridge") == "ORCHE" and p.get("slot") == 2:
            p["status"] = "confirmed"
    problems, _ = gate.check(spec, orche)
    if not has(problems, "ORCHE slot 2 must be provisional"):
        raise SystemExit("ORCHE slot 2 not forced provisional: %r" % problems)

    # 15. terminal-owned endpoints are validated: dropping the 'out' terminal makes its
    #     endpoint owners illegal (terminals are real owners, not free-floating names).
    noterm = copy.deepcopy(manifest)
    noterm["target"]["terminals"] = [t for t in noterm["target"]["terminals"]
                                     if t["stable_id"] != "out"]
    problems, _ = gate.check(spec, noterm)
    if not has(problems, "not a target module or terminal"):
        raise SystemExit("endpoint with a dropped terminal owner not flagged: %r" % problems)

    # 16. dangling fixed-route endpoint is an error (no `continue` on absent endpoint):
    #     removing an endpoint referenced by a fixed route must be caught.
    drop = copy.deepcopy(manifest)
    drop["target"]["paramsJackTargets"] = {
        "params": drop["target"]["paramsJackTargets"]["params"],
        "endpoints": [e for e in drop["target"]["paramsJackTargets"]["endpoints"]
                      if e["stable_id"] != "effector.l_out"],
    }
    problems, _ = gate.check(spec, drop)
    if not has(problems, "fixed.eff_l_to_wet_l"):
        raise SystemExit("dangling fixed-route endpoint (no continue) not flagged: %r" % problems)
    if not has(problems, "dangling"):
        raise SystemExit("dangling fixed-route endpoint not reported as dangling: %r" % problems)

    # 17. duplicate control stable_id -> 重件 (ledger must stay per-item; no synthesis).
    dupc = copy.deepcopy(manifest)
    dupc["target"]["controls"].append(copy.deepcopy(dupc["target"]["controls"][0]))
    problems, _ = gate.check(spec, dupc)
    if not has(problems, "control duplicate stable_id"):
        raise SystemExit("duplicate control not flagged as 重件: %r" % problems)

    # 18. control owner outside module|terminal|program -> rejected.
    badown = copy.deepcopy(manifest)
    badown["target"]["controls"][0]["owner"] = "not_a_module"
    problems, _ = gate.check(spec, badown)
    if not has(problems, "not a target module, terminal, or program"):
        raise SystemExit("control owner outside module|terminal|program not flagged: %r" % problems)

    # 19. control with an invalid kind -> rejected (kind enum is closed).
    badkind = copy.deepcopy(manifest)
    badkind["target"]["controls"][0]["kind"] = "slider"
    problems, _ = gate.check(spec, badkind)
    if not has(problems, "must be one of"):
        raise SystemExit("invalid control kind not flagged: %r" % problems)

    # 20. control with non-bool persistable -> rejected (Parameter is a clean subset).
    badpers = copy.deepcopy(manifest)
    badpers["target"]["controls"][0]["persistable"] = "yes"
    problems, _ = gate.check(spec, badpers)
    if not has(problems, "persistable must be a bool"):
        raise SystemExit("non-bool persistable not flagged: %r" % problems)

    # 21. region subtotal != inventory (the binding ledger check; no total-patching).
    badsub = copy.deepcopy(manifest)
    badsub["target"]["controlRegions"][0]["count"] += 1
    problems, _ = gate.check(spec, badsub)
    if not has(problems, "!= inventory"):
        raise SystemExit("region subtotal != inventory not flagged: %r" % problems)
    if not has(problems, "controlRegions subtotal"):
        raise SystemExit("region subtotal sum mismatch not flagged: %r" % problems)

    # 22. params[] != persistable control subset -> Parameter is NOT a shrink of controls[].
    drift = copy.deepcopy(manifest)
    keeper = [c for c in drift["target"]["controls"] if c["persistable"]][0]["stable_id"]
    drift["target"]["paramsJackTargets"]["params"] = [
        p for p in drift["target"]["paramsJackTargets"]["params"]
        if p["stable_id"] != keeper]
    problems, _ = gate.check(spec, drift)
    if not has(problems, "params[] != persistable control subset"):
        raise SystemExit("params != persistable subset not flagged: %r" % problems)

    # 23. controls[] with no controlRegions[] subtotals -> rejected (can't hide behind a total).
    noregions = copy.deepcopy(manifest)
    noregions["target"]["controlRegions"] = []
    problems, _ = gate.check(spec, noregions)
    if not has(problems, "no controlRegions[] subtotals"):
        raise SystemExit("controls[] without region subtotals not flagged: %r" % problems)

    # 24. control region whose module is not a valid owner -> rejected.
    badregmod = copy.deepcopy(manifest)
    badregmod["target"]["controlRegions"][0]["module"] = "not_a_module"
    problems, _ = gate.check(spec, badregmod)
    if not has(problems, "is not a target module, terminal, or program"):
        raise SystemExit("control region module outside allowed owners not flagged: %r" % problems)

    # 25. ref-only control evidence -> rejected (ledger rows need real provenance).
    badcvev = copy.deepcopy(manifest)
    badcvev["target"]["controls"][0]["evidence"] = {"ref": "x"}
    problems, _ = gate.check(spec, badcvev)
    if not has(problems, "incomplete evidence"):
        raise SystemExit("ref-only control evidence not flagged: %r" % problems)

    # 26. ref-only control-region evidence -> rejected (region subtotals are transcribed too).
    badrgev = copy.deepcopy(manifest)
    badrgev["target"]["controlRegions"][0]["evidence"] = {"ref": "x"}
    problems, _ = gate.check(spec, badrgev)
    if not has(problems, "incomplete evidence"):
        raise SystemExit("ref-only control-region evidence not flagged: %r" % problems)

    print("OK: registry completeness gate rejects each defect for its intended reason; "
          "baseline passes; --require-full trips on the known gaps; impl ⊆ target, "
          "fixed-chain freeze, ORCHE provisional, terminal-owner and dangling-fixed-endpoint "
          "are all gated; the control ledger enforces per-item 重件, owner/kind/persistable "
          "shapes, region-subtotal==inventory, and Parameter==persistable-driven subset.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
