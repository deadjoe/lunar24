#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative tests for the registry completeness gate (Phase A, four-entity model).

The gate must reject each defect for its INTENDED reason, not stumble onto it by accident.
We mutate a copy of the real manifest/spec and assert the resulting `problems` list contains
the matching diagnostic, and that the un-mutated baseline passes. Covers the tightened checks
added to close the Phase-A false-green under the Codex msg 06ef6b70 ledger rework AND the
Codex msg c7089521 hardened shape/action/context/rogue gate:

  the three-entity split extended to four (panelControls / parameters / actions /
    controlBindings),
  INDEPENDENT region subtotals (expectedPanelControlCount is not self-derived),
  EXPLICIT parameter shape (scalar|vector|record|mask) with cardinality / recordType /
    maskSize coupling,
  structured closed-set context (global | program | keyboard-menu | keyboard-mode) with
    valid ProgramId / menu / mode references,
  binding semantics (operation ∈ {set,press,rotate,long_press}; press/rotate/long_press
    must target an action; joystick axis must target exactly owner.<axis>),
  ALWAYS-ON no-new-rogue (a registry Parameter/Jack absent from the target AND the migration
    allowlist fails in NORMAL mode, not just --require-full),
  per-ID --require-full target↔registry for Parameters and Patchable Jacks,
  per-CAPABILITY present-but-empty (parameters / patchableJacks / internalEndpoints / controls),
  no-orphan / no-unbound-persist / no-unbound-action-event binding invariants,
  empty-region check as declaredRegionIds - actualRegionIds,
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

    # 21. binding missing structured context (must be a {type: ...} object, not "").
    badctx = copy.deepcopy(manifest)
    badctx["target"]["controlBindings"][0]["context"] = ""
    problems, _ = gate.check(spec, badctx)
    if not has(problems, "missing structured context"):
        raise SystemExit("binding without a structured context not flagged: %r" % problems)

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

    # ---- c7089521 (fifth review) validations ---------------------------------
    # 33. parameter shape outside the closed enum.
    badshape = copy.deepcopy(manifest)
    badshape["target"]["parameters"][0]["shape"] = "array"
    problems, _ = gate.check(spec, badshape)
    if not has(problems, "shape 'array' not in"):
        raise SystemExit("parameter with an invalid shape not flagged: %r" % problems)

    # 34. shape=vector without an int cardinality.
    badvec = copy.deepcopy(manifest)
    badvec["target"]["parameters"][0]["shape"] = "vector"
    badvec["target"]["parameters"][0]["cardinality"] = None
    problems, _ = gate.check(spec, badvec)
    if not has(problems, "shape=vector needs int cardinality"):
        raise SystemExit("vector parameter without a cardinality not flagged: %r" % problems)

    # 35. shape=record without a declared recordType.
    badrec = copy.deepcopy(manifest)
    badrec["target"]["parameters"][0]["shape"] = "record"
    badrec["target"]["parameters"][0]["recordType"] = None
    problems, _ = gate.check(spec, badrec)
    if not has(problems, "needs a declared recordType"):
        raise SystemExit("record parameter without a recordType not flagged: %r" % problems)

    # 36. shape=mask without an int maskSize.
    badmask = copy.deepcopy(manifest)
    badmask["target"]["parameters"][0]["shape"] = "mask"
    badmask["target"]["parameters"][0]["maskSize"] = None
    problems, _ = gate.check(spec, badmask)
    if not has(problems, "shape=mask needs int maskSize"):
        raise SystemExit("mask parameter without a maskSize not flagged: %r" % problems)

    # 37. shape extras on the WRONG shape (cardinality on a scalar).
    badextra = copy.deepcopy(manifest)
    badextra["target"]["parameters"][0]["cardinality"] = 5
    problems, _ = gate.check(spec, badextra)
    if not has(problems, "cardinality only valid for shape=vector"):
        raise SystemExit("cardinality smuggled onto a scalar not flagged: %r" % problems)

    # 38. press operation targeting a PARAMETER (must be an action).
    pressparam = copy.deepcopy(manifest)
    pressparam["target"]["controlBindings"][0]["operation"] = "press"
    problems, _ = gate.check(spec, pressparam)
    if not has(problems, "is not a declared action"):
        raise SystemExit("press binding targeting a parameter not flagged: %r" % problems)

    # 39. duplicate action -> 重件.
    dupa = copy.deepcopy(manifest)
    dupa["target"]["actions"].append(copy.deepcopy(dupa["target"]["actions"][0]))
    problems, _ = gate.check(spec, dupa)
    if not has(problems, "action duplicate stable_id"):
        raise SystemExit("duplicate action not flagged as 重件: %r" % problems)

    # 40. action kind outside the closed enum.
    badakind = copy.deepcopy(manifest)
    badakind["target"]["actions"][0]["kind"] = "tap"
    problems, _ = gate.check(spec, badakind)
    if not has(problems, "kind 'tap' not in"):
        raise SystemExit("invalid action kind not flagged: %r" % problems)

    # 41. action owner outside module|program|terminal.
    badaowner = copy.deepcopy(manifest)
    badaowner["target"]["actions"][0]["owner"] = "not_a_module"
    problems, _ = gate.check(spec, badaowner)
    if not has(problems, "not a target module"):
        raise SystemExit("action with an invalid owner not flagged: %r" % problems)

    # 42. structured context type outside the closed set.
    badctype = copy.deepcopy(manifest)
    badctype["target"]["controlBindings"][0]["context"] = {"type": "nope"}
    problems, _ = gate.check(spec, badctype)
    if not has(problems, "context type 'nope' not in"):
        raise SystemExit("binding context type outside the closed set not flagged: %r" % problems)

    # 43. program context referencing an unknown ProgramId.
    badprog = copy.deepcopy(manifest)
    badprog["target"]["controlBindings"][0]["context"] = {"type": "program", "program": "program.nope"}
    problems, _ = gate.check(spec, badprog)
    if not has(problems, "unknown ProgramId"):
        raise SystemExit("program context with an unknown ProgramId not flagged: %r" % problems)

    # 44. keyboard-menu context referencing a legal-looking but unknown menu.
    badmenu = copy.deepcopy(manifest)
    badmenu["target"]["controlBindings"][0]["context"] = {"type": "keyboard-menu", "menu": "nope"}
    problems, _ = gate.check(spec, badmenu)
    if not has(problems, "keyboard-menu context menu 'nope' not in"):
        raise SystemExit("keyboard-menu context with an unknown menu not flagged: %r" % problems)

    # 45. keyboard-mode context referencing an unknown mode.
    badmode = copy.deepcopy(manifest)
    badmode["target"]["controlBindings"][0]["context"] = {"type": "keyboard-mode", "mode": "nope"}
    problems, _ = gate.check(spec, badmode)
    if not has(problems, "keyboard-mode context mode 'nope' not in"):
        raise SystemExit("keyboard-mode context with an unknown mode not flagged: %r" % problems)

    # 46. selector-toggle without a non-empty positions[].
    noselpos = copy.deepcopy(manifest)
    for c in noselpos["target"]["panelControls"]:
        if c["stable_id"] == "vco_a.oct_sel":
            c["positions"] = None
    problems, _ = gate.check(spec, noselpos)
    if not has(problems, "selector-toggle needs a non-empty positions"):
        raise SystemExit("selector-toggle without positions not flagged: %r" % problems)

    # 47. joystick binding on an axis the widget does not declare.
    badaxis = copy.deepcopy(manifest)
    for b in badaxis["target"]["controlBindings"]:
        if b["stable_id"] == "bnd.joystick.joy->joystick.x":
            b["axis"] = "z"
    problems, _ = gate.check(spec, badaxis)
    if not has(problems, "not a declared axis of joystick.joy"):
        raise SystemExit("joystick binding on an undeclared axis not flagged: %r" % problems)

    # 48. joystick axis must target exactly owner.<axis> (no cross-axis).
    wrongaxisparam = copy.deepcopy(manifest)
    for b in wrongaxisparam["target"]["controlBindings"]:
        if b["stable_id"] == "bnd.joystick.joy->joystick.x":
            b["to"] = "joystick.y"  # keep axis=x, but point at the Y parameter
    problems, _ = gate.check(spec, wrongaxisparam)
    if not has(problems, "must target 'joystick.x', not 'joystick.y'"):
        raise SystemExit("joystick axis bound to the wrong parameter not flagged: %r" % problems)

    # 49. ALWAYS-ON no-new-rogue: a registry Parameter/Jack absent from the target AND the
    #     migration allowlist fails in NORMAL mode (not just under --require-full).
    newrogue = copy.deepcopy(spec)
    vco_cards = [mm for mm in newrogue["modules"] if mm["stable_id"] == "vco_a"]
    vco_card = vco_cards[0]
    vco_param = copy.deepcopy(vco_card["parameters"][0])
    vco_param["id"] = 900
    vco_param["stable_id"] = "vco_a.rogue_new"
    vco_param["name"] = "Rogue param"
    vco_card["parameters"].append(vco_param)
    vco_jack = copy.deepcopy(vco_card["jacks"][0])
    vco_jack["id"] = 900
    vco_jack["stable_id"] = "vco_a.rogue_jack_new"
    vco_jack["name"] = "Rogue jack"
    vco_card["jacks"].append(vco_jack)
    problems, _ = gate.check(newrogue, manifest)
    if not has(problems, "parameter registry-rogue NOT migration-allowed (NEW)"):
        raise SystemExit("new parameter rogue not flagged in NORMAL mode (always-on): %r" % problems)
    if not has(problems, "patchable jack registry-rogue NOT migration-allowed (NEW)"):
        raise SystemExit("new jack rogue not flagged in NORMAL mode (always-on): %r" % problems)

    # 50. capability patchableJacks present-but-empty: a module that DECLARES the patchable-jacks
    #     capability but provides zero jacks in the registry. (We add a genuine zero-jack module
    #     rather than emptying vco_a, whose jacks anchor generated normalized routes.)
    capjack = copy.deepcopy(spec)
    capjack["modules"].append({
        "id": 99, "stable_id": "mutant", "name": "Mutant", "category": "util",
        "parameters": [], "jacks": [],
        "status": "confirmed",
        "evidence": {"ref": "solar42N_manual_v15", "lineStart": 1, "lineEnd": 1},
    })
    capman = copy.deepcopy(manifest)
    capman["target"]["modules"].append({
        "stable_id": "mutant", "name": "Mutant", "category": "util",
        "capabilities": {"parameters": False, "patchableJacks": True,
                         "internalEndpoints": False, "controls": False},
        "status": "confirmed",
        "evidence": {"ref": "solar42N_manual_v15", "lineStart": 1, "lineEnd": 1, "panelSite": "mutant"},
    })
    capman["mustComplete"] = capman["mustComplete"] + ["module:mutant"]
    problems, _ = gate.check(capjack, capman)
    if not has(problems, "declares capability patchableJacks but the registry provides no patchable jack"):
        raise SystemExit("patchableJacks capability present-but-empty not flagged: %r" % problems)

    # 51. capability internalEndpoints present-but-empty: a REGISTRY module (the capability loop only
    #     sweeps registry modules) that declares internalEndpoints but owns no non-patchable endpoint.
    #     envelope_a is in the registry and owns no internal endpoint in the target inventory.
    capint = copy.deepcopy(manifest)
    for mo in capint["target"]["modules"]:
        if mo["stable_id"] == "envelope_a":
            mo["capabilities"]["internalEndpoints"] = True
    problems, _ = gate.check(spec, capint)
    if not has(problems, "declares capability internalEndpoints but the target transcribes no internal endpoint"):
        raise SystemExit("internalEndpoints capability present-but-empty not flagged: %r" % problems)

    # 52. empty region check = declaredRegionIds - actualRegionIds (a declared region with no widgets).
    ghostreg = copy.deepcopy(manifest)
    ghostreg["target"]["controlRegions"].append({
        "id": "ghost_region",
        "module": "preamp",
        "expectedPanelControlCount": 0,
        "status": "confirmed",
        "evidence": {"ref": "solar42N_manual_v15", "lineStart": 300, "lineEnd": 300,
                     "panelSite": "ghost"},
    })
    problems, _ = gate.check(spec, ghostreg)
    if not has(problems, "declared but with no panelControls"):
        raise SystemExit("declared region with no widgets not flagged (declaredRegionIds-actualRegionIds): "
                         "%r" % problems)

    print("OK: completeness gate rejects each defect for its intended reason; baseline passes; "
          "--require-full is per-ID (gap + rogue), not a fake per-module green; the four-entity "
          "split, independent region subtotals, explicit parameter shape + cardinality/recordType/"
          "maskSize coupling, binding operation→action semantics, structured closed-set context, "
          "joystick-axis targeting, ALWAYS-ON no-new-rogue, capability-declared present-but-empty "
          "(parameters/patchableJacks/internalEndpoints), empty-region declared-actual, binding "
          "invariants, impl⊆target, fixed-chain freeze, ORCHE provisional, terminal-owner and "
          "dangling-fixed-endpoint are all gated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
