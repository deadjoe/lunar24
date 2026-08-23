#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative tests for the registry completeness gate (Phase A, three-entity model).

The gate must reject each defect for its INTENDED reason, not stumble onto it by accident.
We mutate a copy of the real manifest/spec and assert the resulting `problems` list contains
the matching diagnostic, and that the un-mutated baseline passes. Covers the tightened checks
added to close the Phase-A false-green under the Codex msg 06ef6b70 ledger rework:

  the three-entity split (panelControls / parameters / controlBindings),
  INDEPENDENT region subtotals (expectedPanelControlCount is not self-derived),
  per-ID --require-full target↔registry for Parameters and Patchable Jacks,
  per-CAPABILITY present-but-empty (replaces the blanket param+jack check),
  no-orphan / no-unbound-persist binding invariants,
  plus the inherited gates (重件 by category, provenance, program grid, directed/in-dangling
  routes, fixed-chain freeze, ORCHE provisional, terminal-owner, impl ⊆ target, mustComplete).

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
    if not has(problems, "module duplicate stable_id"):
        raise SystemExit("duplicate module not flagged as 重件: %r" % problems)

    # 2. duplicate target program -> per-category 重件
    dupp = copy.deepcopy(manifest)
    dupp["target"]["programs"].append(copy.deepcopy(dupp["target"]["programs"][0]))
    problems, _ = gate.check(spec, dupp)
    if not has(problems, "program duplicate stable_id"):
        raise SystemExit("duplicate program not flagged as 重件: %r" % problems)

    # 3. duplicate normalized route -> per-category 重件
    dupr = copy.deepcopy(manifest)
    dupr["target"]["normalizedRoutes"].append(
        copy.deepcopy(dupr["target"]["normalizedRoutes"][0]))
    problems, _ = gate.check(spec, dupr)
    if not has(problems, "normalized route duplicate stable_id"):
        raise SystemExit("duplicate normalized route not flagged as 重件: %r" % problems)

    # 4. duplicate panel control -> per-category 重件 (three-entity split).
    dupc = copy.deepcopy(manifest)
    dupc["target"]["panelControls"].append(copy.deepcopy(dupc["target"]["panelControls"][0]))
    problems, _ = gate.check(spec, dupc)
    if not has(problems, "panelControl duplicate stable_id"):
        raise SystemExit("duplicate panelControl not flagged as 重件: %r" % problems)

    # 5. duplicate parameter -> 重件 (parameters are their own category now, not a controls shrink).
    dupp2 = copy.deepcopy(manifest)
    dupp2["target"]["parameters"].append(copy.deepcopy(dupp2["target"]["parameters"][0]))
    problems, _ = gate.check(spec, dupp2)
    if not has(problems, "parameter duplicate stable_id"):
        raise SystemExit("duplicate parameter not flagged as 重件: %r" % problems)

    # 6. duplicate controlBinding -> 重件 (bindings are their own category).
    dupb = copy.deepcopy(manifest)
    dupb["target"]["controlBindings"].append(copy.deepcopy(dupb["target"]["controlBindings"][0]))
    problems, _ = gate.check(spec, dupb)
    if not has(problems, "controlBinding duplicate stable_id"):
        raise SystemExit("duplicate controlBinding not flagged as 重件: %r" % problems)

    # 7. duplicate controlRegion (keyed by id, not stable_id) -> 重件.
    dupr2 = copy.deepcopy(manifest)
    dupr2["target"]["controlRegions"].append(copy.deepcopy(dupr2["target"]["controlRegions"][0]))
    problems, _ = gate.check(spec, dupr2)
    if not has(problems, "controlRegion duplicate stable_id"):
        raise SystemExit("duplicate controlRegion not flagged as 重件: %r" % problems)

    # 8. evidence-less module (ref only) -> provenance too loose.
    noev = copy.deepcopy(manifest)
    noev["target"]["modules"][0]["evidence"] = {"ref": "solar42N_manual_v15"}
    problems, _ = gate.check(spec, noev)
    if not has(problems, "incomplete evidence"):
        raise SystemExit("ref-only evidence not rejected (provenance shape too loose): %r" % problems)

    # 9. cartridge row missing a slot -> must list all N slots.
    cart = copy.deepcopy(manifest)
    cart["target"]["programs"] = [p for p in cart["target"]["programs"]
                                  if not (p["cartridge"] == "CATHEDRAL" and p["slot"] == 2)]
    problems, _ = gate.check(spec, cart)
    if not has(problems, "must list all 3 slots"):
        raise SystemExit("cartridge missing a slot not flagged: %r" % problems)

    # 10. dangling normalized route endpoint (module present, jack not registered).
    dang = copy.deepcopy(manifest)
    dang["target"]["normalizedRoutes"][0]["sinkJack"] = "vco_a.not_real"
    problems, _ = gate.check(spec, dang)
    if not has(problems, "dangling"):
        raise SystemExit("dangling route endpoint not flagged: %r" % problems)

    # 11. misdirected fixed route (source is an input endpoint -> must be output).
    misdir = copy.deepcopy(manifest)
    misdir["target"]["fixedRoutes"][0]["source"] = "vcf.cv_l_in"  # an INPUT
    problems, _ = gate.check(spec, misdir)
    if not has(problems, "expected output"):
        raise SystemExit("misdirected fixed-route source not flagged: %r" % problems)

    # 12. ORCHE slot 2/3 must be provisional (manual prints 'Program 1' for all three).
    orche = copy.deepcopy(manifest)
    for p in orche["target"]["programs"]:
        if p.get("cartridge") == "ORCHE" and p.get("slot") == 2:
            p["status"] = "confirmed"
    problems, _ = gate.check(spec, orche)
    if not has(problems, "ORCHE slot 2 must be provisional"):
        raise SystemExit("ORCHE slot 2 not forced provisional: %r" % problems)

    # 13. terminal-owned endpoints validated: dropping the 'out' terminal makes its endpoint owner illegal.
    noterm = copy.deepcopy(manifest)
    noterm["target"]["terminals"] = [t for t in noterm["target"]["terminals"]
                                     if t["stable_id"] != "out"]
    problems, _ = gate.check(spec, noterm)
    if not has(problems, "not a target module or terminal"):
        raise SystemExit("endpoint with a dropped terminal owner not flagged: %r" % problems)

    # 14. dangling fixed-route endpoint is an error (no `continue` on an absent endpoint).
    drop = copy.deepcopy(manifest)
    drop["target"]["paramsJackTargets"]["endpoints"] = [
        e for e in drop["target"]["paramsJackTargets"]["endpoints"]
        if e["stable_id"] != "effector.l_out"]
    problems, _ = gate.check(spec, drop)
    if not has(problems, "dangling"):
        raise SystemExit("dangling fixed-route endpoint not reported as dangling: %r" % problems)

    # 15. parameter owner outside module|program -> rejected.
    badpown = copy.deepcopy(manifest)
    badpown["target"]["parameters"][0]["owner"] = "not_a_module"
    problems, _ = gate.check(spec, badpown)
    if not has(problems, "not a target module or program"):
        raise SystemExit("parameter owner outside module|program not flagged: %r" % problems)

    # 16. parameter kind invalid -> closed enum.
    badpkind = copy.deepcopy(manifest)
    badpkind["target"]["parameters"][0]["kind"] = "slider"
    problems, _ = gate.check(spec, badpkind)
    if not has(problems, "kind in"):
        raise SystemExit("invalid parameter kind not flagged: %r" % problems)

    # 17. panelControl kind invalid -> closed enum (no more momentary-touch-event legacy kind).
    badckind = copy.deepcopy(manifest)
    badckind["target"]["panelControls"][0]["kind"] = "slider"
    problems, _ = gate.check(spec, badckind)
    if not has(problems, "kind in"):
        raise SystemExit("invalid panelControl kind not flagged: %r" % problems)

    # 18. panelControl owner outside module|terminal|program -> rejected.
    badcown = copy.deepcopy(manifest)
    badcown["target"]["panelControls"][0]["owner"] = "not_a_module"
    problems, _ = gate.check(spec, badcown)
    if not has(problems, "is not a target module, terminal, or program"):
        raise SystemExit("panelControl owner outside allowed owners not flagged: %r" % problems)

    # 19. binding from is not a panelControl.
    badfrom = copy.deepcopy(manifest)
    badfrom["target"]["controlBindings"][0]["from"] = "not_a_panel_control"
    problems, _ = gate.check(spec, badfrom)
    if not has(problems, "is not a declared panelControl"):
        raise SystemExit("binding from an unknown panelControl not flagged: %r" % problems)

    # 20. binding to is not a parameter.
    badto = copy.deepcopy(manifest)
    badto["target"]["controlBindings"][0]["to"] = "not_a_parameter"
    problems, _ = gate.check(spec, badto)
    if not has(problems, "is not a declared parameter"):
        raise SystemExit("binding to an unknown parameter not flagged: %r" % problems)

    # 21. binding missing context.
    badctx = copy.deepcopy(manifest)
    badctx["target"]["controlBindings"][0]["context"] = ""
    problems, _ = gate.check(spec, badctx)
    if not has(problems, "missing context"):
        raise SystemExit("binding without a context not flagged: %r" % problems)

    # 22. orphan parameter (a persisted state with no binding to reach it).
    orphan = copy.deepcopy(manifest)
    orphan["target"]["controlBindings"] = [
        b for b in orphan["target"]["controlBindings"] if b["to"] != "mixer.ch1_pan"]
    problems, _ = gate.check(spec, orphan)
    if not has(problems, "orphan persisted state"):
        raise SystemExit("orphan parameter not flagged: %r" % problems)

    # 23. continuous control with no binding (persistable widget that edits nothing).
    unb = copy.deepcopy(manifest)
    unb["target"]["controlBindings"] = [
        b for b in unb["target"]["controlBindings"] if b["from"] != "mixer.ch1_pan"]
    problems, _ = gate.check(spec, unb)
    if not has(problems, "no controlBinding"):
        raise SystemExit("unbound continuous/selector panelControl not flagged: %r" % problems)

    # 24. INDEPENDENT region subtotal != actual widgets (the len(rows) self-prove is gone).
    badsub = copy.deepcopy(manifest)
    badsub["target"]["controlRegions"][0]["expectedPanelControlCount"] += 1
    problems, _ = gate.check(spec, badsub)
    if not has(problems, "independent subtotal disagrees"):
        raise SystemExit("region expected != actual not flagged: %r" % problems)
    if not has(problems, "!= panelControls[] length"):
        raise SystemExit("region subtotal sum mismatch not flagged: %r" % problems)

    # 25. panelControl living in a region with no declared subtotal.
    undecl = copy.deepcopy(manifest)
    undecl["target"]["controlRegions"] = undecl["target"]["controlRegions"][1:]
    problems, _ = gate.check(spec, undecl)
    if not has(problems, "in undeclared regions"):
        raise SystemExit("panelControl with no declared region subtotal not flagged: %r" % problems)

    # 26. present-but-empty via CAPABILITY declaration (replaces the blanket param+jack check):
    #     a registry module that DECLARES parameters but provides none is an error.
    capempty = copy.deepcopy(spec)
    for m in capempty["modules"]:
        if m.get("stable_id") == "vco_a":
            m["parameters"] = []
    problems, _ = gate.check(capempty, manifest)
    if not has(problems, "declares capability parameters but the registry provides none"):
        raise SystemExit("capability-declared module with no params not flagged: %r" % problems)

    # 27. registry item absent from the independent target (impl ⊄ target).
    rogue = copy.deepcopy(spec)
    rogue["programs"].append(copy.deepcopy(rogue["programs"][0]))
    rogue["programs"][-1]["id"] = 99
    rogue["programs"][-1]["stable_id"] = "program.rogue"
    rogue["programs"][-1]["cartridge"] = "ROGUE"
    for i, parm in enumerate(rogue["programs"][-1]["parameters"]):
        parm["id"] = 200 + i
        parm["stable_id"] = "program.rogue.%s" % parm["stable_id"].split(".")[-1]
    problems, _ = gate.check(rogue, manifest)
    if not has(problems, "impl ⊄ target"):
        raise SystemExit("registry item absent from the independent target not flagged: %r" % problems)

    # 28. frozen fixed chain: removing a required fixed route trips --require-full.
    cut = copy.deepcopy(manifest)
    cut["target"]["fixedRoutes"] = [r for r in cut["target"]["fixedRoutes"]
                                    if r["stable_id"] != "fixed.eff_l_to_wet_l"]
    problems, _ = gate.check(spec, cut, require_full=True)
    if not has(problems, "required fixed routes missing"):
        raise SystemExit("missing required fixed route did not trip --require-full: %r" % problems)

    # 29. --require-full trips an incomplete (but coherent) manifest AND reports per-ID gaps.
    problems, _ = gate.check(spec, manifest, require_full=True)
    if not has(problems, "--require-full"):
        raise SystemExit("incomplete manifest did not trip --require-full: %r" % problems)
    if not has(problems, "program identity gaps"):
        raise SystemExit("--require-full did not report the program-identity gap: %r" % problems)
    if not has(problems, "parameter target-not-implemented"):
        raise SystemExit("--require-full did not report per-ID parameter gaps: %r" % problems)
    if not has(problems, "parameter registry-rogue"):
        raise SystemExit("--require-full did not report per-ID parameter rogues "
                         "(pre-correction ids like vco_a.oct_high): %r" % problems)

    # 30. fixed route invalid kind.
    badkind = copy.deepcopy(manifest)
    badkind["target"]["fixedRoutes"][0]["kind"] = "magic"
    problems, _ = gate.check(spec, badkind)
    if not has(problems, "kind must be one of"):
        raise SystemExit("invalid fixed-route kind not flagged: %r" % problems)

    # 31. mustComplete non-regression: a manifest target not in the registry.
    regress = copy.deepcopy(manifest)
    regress["mustComplete"].append("module:drone_1")  # a manifest target, NOT in registry
    problems, _ = gate.check(spec, regress)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("dropped mustComplete item not flagged as non-regression: %r" % problems)

    # 32. mustComplete != the landed set.
    unsync = copy.deepcopy(manifest)
    unsync["mustComplete"] = [k for k in unsync["mustComplete"] if k != "module:vco_b"]
    problems, _ = gate.check(spec, unsync)
    if not has(problems, "mustComplete != registry landed target keys"):
        raise SystemExit("mustComplete not required to match the landed set: %r" % problems)

    print("OK: completeness gate rejects each defect for its intended reason; baseline passes; "
          "--require-full is per-ID (gap + rogue), not a fake per-module green; the three-entity "
          "split, independent region subtotals, binding invariants, capability-declared "
          "present-but-empty, impl⊆target, fixed-chain freeze, ORCHE provisional, terminal-owner "
          "and dangling-fixed-endpoint are all gated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
