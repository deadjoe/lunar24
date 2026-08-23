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
  dispatch closure (Codex msg 720fc3d3): a keyboard-menu/mode binding must carry a menuItem selector
    or a single event fans over every menu item ("press one slot hits 3 actions", "ARP rotate
    changes 7 params"); a vector param needs an index (whole-vector binding banned); commandAddress /
    record-schema keys are exact key-sets; fields are unique + ORDER-preserving; the unused
    record{Field,Index}/maskIndex condition keys were removed from the closed set (a binding carrying
    one is rejected, not ignored),
  PRESETS two-page state machine (Codex msg e9f1f028): the presets workflow is a REAL two-page state
    machine (slot-list A-D -> action-list load/save/initialise), so a keyboard-menu=presets context
    must carry a closed `page` selector, menuItem may only name a declared DISPLAY ITEM (never a
    navigation action's own leaf), the enter-subpage edge must carry the slot it enters, the menu
    topology is an independent declaration (never inferred from a target's `menu` field), and both
    pages must actually be present, and the CLOSED relation set must match EXACTLY (Codex msg
    8cc637e9) — delete an edge, use a non-topology page, drop the enter slot, forge menuItem from the
    navigation action, misalign a slot/item, or rebind a PRESETS workflow action into a non-presets
    context (Codex msg 0153c91f) all fail for their intended reason,
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


def reg_param(spec, sid):
    for m in spec["modules"]:
        for p in m["parameters"]:
            if p["stable_id"] == sid:
                return p
    for pr in spec.get("programs", []):
        for p in pr.get("parameters", []):
            if p["stable_id"] == sid:
                return p
    return None


def tgt_param(manifest, sid):
    for p in manifest["target"]["parameters"]:
        if p["stable_id"] == sid:
            return p
    return None


def reg_jack(spec, sid):
    for m in spec["modules"]:
        for j in m.get("jacks", []):
            if j["stable_id"] == sid:
                return j
    return None


def reg_prog(spec, sid):
    for p in spec.get("programs", []):
        if p["stable_id"] == sid:
            return p
    return None


def reg_route(spec, sid):
    for r in spec.get("normalizedRoutes", []):
        if r["stable_id"] == sid:
            return r
    return None


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
    if not has(problems, "parameter target-not-implemented"):
        raise SystemExit("--require-full did not report per-ID parameter gaps: %r" % problems)
    # 29b. The live registry's program-identity layer is now COMPLETE (39/39, no gap), so the
    #      program-identity-gap branch (checker 1551) is exercised from a fixture copy with one
    #      identity-only target program dropped. digital.1 carries no params, so the drop is clean.
    dropp = copy.deepcopy(spec)
    dropp["programs"] = [p for p in dropp["programs"] if p["stable_id"] != "program.digital.1"]
    problems, _ = gate.check(dropp, manifest, require_full=True)
    if not has(problems, "program identity gaps"):
        raise SystemExit("--require-full did not report program-identity gap for a dropped program: %r" % problems)
    # --require-full must ALSO report a per-ID *parameter* rogue (a registry param
    # whose stable_id is absent from the frozen target and not on the allowlist).
    # Build this from a fixture copy: the live registry is now clean of rogues
    # (pre-correction ids like vco_a.oct_high were re-ID'd to target ids in
    # Phase B), so asserting against a still-present live rogue would be a
    # self-proving no-op rather than an independent check.
    roguep = copy.deepcopy(spec)
    roguep_mod = [x for x in roguep["modules"] if x["stable_id"] == "vco_a"][0]
    roguep_mod["parameters"].append(copy.deepcopy(roguep_mod["parameters"][0]))
    roguep_parm = roguep_mod["parameters"][-1]
    roguep_parm["id"] = 999
    roguep_parm["stable_id"] = "vco_a.rogue_oct_high"
    problems_p, _ = gate.check(roguep, manifest, require_full=True)
    if not has(problems_p, "parameter registry-rogue"):
        raise SystemExit("--require-full did not report a per-ID parameter rogue: %r" % problems_p)

    # 29c. Program identity CONTENT exact-compare (Codex msg f9a4bdae NON-GO): slice #57
    #      locked only the program stable-id SET; the identity content (id/cartridge/slot/
    #      name/status/family/selfOscillating/fieldEvidence/evidence ref+lines) drifted past
    #      NORMAL. These negatives prove each drift mode is caught for its OWN reason.
    #      (a) renumber: SWAP two programs' ids. Both stay in the unique contiguous id-space, so
    #          the id-identity gate passes, but the position identity drifted (cathedral.1 now
    #          carries cathedral.2's id) — exactly the renumber the content gate must catch.
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["id"], reg_prog(cp, "program.cathedral.2")["id"] = 1, 0
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "id 1 != frozen target 0"):
        raise SystemExit("program id renumber not flagged: %r" % problems)
    #      (b) identity-field drift: change the name (and independently the status).
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["name"] = "Glow"
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "name 'Glow' != frozen target 'Shimmer'"):
        raise SystemExit("program name drift not flagged: %r" % problems)
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["status"] = "provisional"
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "status 'provisional' != frozen target 'confirmed'"):
        raise SystemExit("program status drift not flagged: %r" % problems)
    #      (c) legal-but-wrong family (reverb -> delay: both in the vocabulary, wrong fact).
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["family"] = "delay"
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "family 'delay' != frozen target 'reverb'"):
        raise SystemExit("legal-but-wrong program family not flagged: %r" % problems)
    #      (d) garbage family: outside the closed vocabulary, rejected even with legal provenance.
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["family"] = "banana"
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "family 'banana' not in closed family vocabulary"):
        raise SystemExit("garbage program family not flagged: %r" % problems)
    #      (e) selfOsc assertion drift: unknown -> no, with a consistent concrete provenance so the
    #          unknown-enum biconditional does not shadow the identity exact-compare.
    cp = copy.deepcopy(spec)
    pso = reg_prog(cp, "program.cathedral.1")
    pso["selfOscillating"] = "no"
    pso["fieldEvidence"]["selfOscillating"] = "confirmed"
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "selfOscillating 'no' != frozen target 'unknown'"):
        raise SystemExit("program selfOsc assertion drift not flagged: %r" % problems)
    #      (f) evidence drift: change the manual line span of the cited program.
    cp = copy.deepcopy(spec)
    reg_prog(cp, "program.cathedral.1")["evidence"]["lineEnd"] = 1214
    problems, _ = gate.check(cp, manifest)
    if not has(problems, "evidence.lineEnd 1214 != frozen target 1213"):
        raise SystemExit("program evidence drift not flagged: %r" % problems)

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
    if not has(problems, "shape=vector needs positive int cardinality"):
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
    if not has(problems, "shape=mask needs positive int maskSize"):
        raise SystemExit("mask parameter without a maskSize not flagged: %r" % problems)

    # 37. shape extras on the WRONG shape (cardinality on a scalar).
    badextra = copy.deepcopy(manifest)
    badextra["target"]["parameters"][0]["cardinality"] = 5
    problems, _ = gate.check(spec, badextra)
    if not has(problems, "cardinality only valid for shape=vector"):
        raise SystemExit("cardinality smuggled onto a scalar not flagged: %r" % problems)

    # 38. press source operation on a continuous widget is not a declared source op (the
    #     kind→source-op map is EQUALITY not a subset; a press on a continuous slider is bogus).
    pressparam = copy.deepcopy(manifest)
    pressparam["target"]["controlBindings"][0]["sourceOperation"] = "press"
    problems, _ = gate.check(spec, pressparam)
    if not has(problems, "wired source operation 'press' is not declared"):
        raise SystemExit("press source op on a continuous widget not flagged: %r" % problems)

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
        if b["from"] == "joystick.joy" and b["to"] == "joystick.x":
            b["axis"] = "z"
    problems, _ = gate.check(spec, badaxis)
    if not has(problems, "not a declared axis of joystick.joy"):
        raise SystemExit("joystick binding on an undeclared axis not flagged: %r" % problems)

    # 48. joystick axis must target exactly owner.<axis> (no cross-axis).
    wrongaxisparam = copy.deepcopy(manifest)
    for b in wrongaxisparam["target"]["controlBindings"]:
        if b["from"] == "joystick.joy" and b["to"] == "joystick.x":
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

    # 53. momentary-touch missing release edge: a plate must express BOTH down (press) and up
    #     (release) edges (Codex a23a9618 option A). Dropping the release binding must fail.
    norelease = copy.deepcopy(manifest)
    norelease["target"]["controlBindings"] = [
        b for b in norelease["target"]["controlBindings"]
        if b["to"] != "keyboard.plate_1.release"
    ]
    problems, _ = gate.check(spec, norelease)
    if not has(problems, "declared source operation 'release' has no controlBinding"):
        raise SystemExit("plate with the release edge removed not flagged (needs both press/release): %r" % problems)

    # 54. declared encoder op not wired: the widget declares press but every press binding from it
    #     is deleted (the encoder fires press for both encoder.press and the presets_load action).
    nopress = copy.deepcopy(manifest)
    nopress["target"]["controlBindings"] = [
        b for b in nopress["target"]["controlBindings"]
        if not (b["from"] == "keyboard.encoder" and b["sourceOperation"] == "press")
    ]
    problems, _ = gate.check(spec, nopress)
    if not has(problems, "declared source operation 'press' has no controlBinding"):
        raise SystemExit("encoder press op declared but unwired not flagged: %r" % problems)

    # 55. index outside cardinality: a held-index binding must stay within the vector cardinality.
    badidx = copy.deepcopy(manifest)
    for b in badidx["target"]["controlBindings"]:
        if b["from"] == "keyboard.encoder" and b["to"] == "keyboard.plate_tune" and b.get("index") == 1:
            b["index"] = 99
    problems, _ = gate.check(spec, badidx)
    if not has(problems, "outside cardinality 1..12"):
        raise SystemExit("held-index binding outside the vector cardinality not flagged: %r" % problems)

    # 56. array element must be a closed typed element, not an inline anonymous map.
    anonelem = copy.deepcopy(manifest)
    sc = anonelem["target"]["recordSchemas"]["keyboard_seq"]
    sc["fields"][0]["element"] = {"name": "step"}  # drop type -> anonymous
    problems, _ = gate.check(spec, anonelem)
    if not has(problems, "closed typed element (got anonymous map)"):
        raise SystemExit("array element reduced to an anonymous map not flagged: %r" % problems)

    # 57. record field 'of' pointing at an undefined schema.
    badoff = copy.deepcopy(manifest)
    badoff["target"]["recordSchemas"]["keyboard_preset"]["fields"][0]["of"] = "bogus"
    problems, _ = gate.check(spec, badoff)
    if not has(problems, "is not a defined record/params schema"):
        raise SystemExit("record field referencing an undefined schema not flagged: %r" % problems)

    # 58. preset excludes must not overlap the referenced param-set (can't both store & exclude).
    overlap = copy.deepcopy(manifest)
    overlap["target"]["recordSchemas"]["keyboard_preset"]["excludes"].append("keyboard.behaviour")
    problems, _ = gate.check(spec, overlap)
    if not has(problems, "overlap the referenced param-set"):
        raise SystemExit("preset excludes overlapping its own param-set not flagged: %r" % problems)

    # 59. allowlist expanded to mask a NEW rogue: growing the migration allowlist beyond the
    #     independent legacy ceiling may not re-green a new registry rogue (monotonic, item #4).
    maskrogue = copy.deepcopy(spec)
    vco_card = [mm for mm in maskrogue["modules"] if mm["stable_id"] == "vco_a"][0]
    rogue = copy.deepcopy(vco_card["parameters"][0])
    rogue["id"] = 901
    rogue["stable_id"] = "vco_a.rogue_allowmask"
    vco_card["parameters"].append(rogue)
    maskman = copy.deepcopy(manifest)
    maskman["migrationAllowlist"]["parameters"].append("vco_a.rogue_allowmask")
    problems, _ = gate.check(maskrogue, maskman)
    if not has(problems, "migration allowlist parameters NOT in legacy ceiling"):
        raise SystemExit("allowlist masked a new rogue outside the legacy ceiling, not flagged: %r" % problems)

    # 60. STALE allow entry: a real target param (never a rogue) on the allowlist is stale.
    staleallow = copy.deepcopy(manifest)
    staleallow["migrationAllowlist"]["parameters"].append("keyboard.behaviour")
    problems, _ = gate.check(spec, staleallow)
    if not has(problems, "stale migration allowlist parameters"):
        raise SystemExit("allowlist entry for a non-rogue id not flagged as stale: %r" % problems)

    # 61. capability internalEndpoints is INDEPENDENT of the endpoint inventory: deleting vco_a's
    #     non-patchable endpoint must NOT co-flip its capability off. If capabilities were
    #     reverse-derived, dropping the endpoint would clear the cap and the gate would stay green;
    #     a frozen cap must keep failing.
    deleps = copy.deepcopy(manifest)
    deleps["target"]["paramsJackTargets"]["endpoints"] = [
        e for e in deleps["target"]["paramsJackTargets"]["endpoints"]
        if e["stable_id"] != "vco_a.audio_out"
    ]
    problems, _ = gate.check(spec, deleps)
    if not has(problems, "declares capability internalEndpoints but the target transcribes no internal endpoint"):
        raise SystemExit("deleting an internal endpoint did not leave its frozen capability present-but-empty: %r" % problems)

    # ---- msg 9d8b5f43 (seventh review) validations --------------------------
    # 62. wired source ops must EQUAL the closed kind→source-op map, not be merely a subset. An encoder
    #     is {rotate,press,long_press}; adding an undeclared release edge must fail.
    addrel = copy.deepcopy(manifest)
    encb = copy.deepcopy([b for b in addrel["target"]["controlBindings"]
                          if b["from"] == "keyboard.encoder"][0])
    encb["stable_id"] = "bnd.keyboard.encoder.release.extra"
    encb["sourceOperation"] = "release"
    addrel["target"]["controlBindings"].append(encb)
    problems, _ = gate.check(spec, addrel)
    if not has(problems, "wired source operation 'release' is not declared"):
        raise SystemExit("encoder release edge not rejected by kind→source-op equality: %r" % problems)

    # 63. duplicate semantic tuple (msg 9d8b5f43 item #1): two bindings with the identical
    #     (from,to,source,targetKind,targetOp,axis,index,condition,commandAddress) are unexecutable.
    duptup = copy.deepcopy(manifest)
    db = copy.deepcopy(duptup["target"]["controlBindings"][0])
    db["stable_id"] = "bnd.drone_1.tune_1.dup"  # distinct id, identical semantic tuple
    duptup["target"]["controlBindings"].append(db)
    problems, _ = gate.check(spec, duptup)
    if not has(problems, "duplicate controlBinding semantic tuple"):
        raise SystemExit("duplicate semantic tuple not flagged: %r" % problems)

    # 64. condition must carry only closed keys (msg 9d8b5f43 item #2): a 'heldPlate' typo is a
    #     masked predicate, not an allowed refinement.
    badcond = copy.deepcopy(manifest)
    for b in badcond["target"]["controlBindings"]:
        if b.get("condition"):
            b["condition"] = dict(b["condition"])
            b["condition"]["heldPlate"] = "keyboard.plate_1"
            break
    problems, _ = gate.check(spec, badcond)
    if not has(problems, "condition key 'heldPlate' not in"):
        raise SystemExit("condition with an unclosed key not flagged: %r" % problems)

    # 65. condition presetSlot is a closed set (preset_a..d); an out-of-range slot is rejected.
    badslot = copy.deepcopy(manifest)
    for b in badslot["target"]["controlBindings"]:
        if isinstance(b.get("condition"), dict) and b["condition"].get("presetSlot"):
            b["condition"] = dict(b["condition"])
            b["condition"]["presetSlot"] = "preset_e"
            break
    problems, _ = gate.check(spec, badslot)
    if not has(problems, "condition presetSlot 'preset_e' not in"):
        raise SystemExit("presetSlot outside the closed set not flagged: %r" % problems)

    # 66. commandAddress kind must be DERIVED from the target (msg 9d8b5f43 item #2, tightened by
    #     msg 23ea438f item #2): a mask/foreign kind on a record editor is rejected as a mismatch,
    #     not silently accepted.
    badca = copy.deepcopy(manifest)
    for b in badca["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps":
            b["commandAddress"] = {"kind": "index", "indexRange": [0, 15], "fields": ["note"]}
            break
    problems, _ = gate.check(spec, badca)
    if not has(problems, "commandAddress kind 'index' != expected 'record'"):
        raise SystemExit("commandAddress with a mismatched kind not flagged: %r" % problems)

    # 67. commandAddress indexRange must match the target's derived range (a descending [5,2] is
    #     malformed AND disagrees with the derived [0,15]).
    badir = copy.deepcopy(manifest)
    for b in badir["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps":
            b["commandAddress"] = {"kind": "record", "indexRange": [5, 2], "fields": ["note"]}
            break
    problems, _ = gate.check(spec, badir)
    if not has(problems, "commandAddress indexRange [5, 2] != expected [0, 15]"):
        raise SystemExit("commandAddress mismatched indexRange not flagged: %r" % problems)

    # 68. record commandAddress fields must be exactly the array element's field names; an unknown
    #     field can't be addressed.
    badfe = copy.deepcopy(manifest)
    for b in badfe["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps":
            b["commandAddress"] = {"kind": "record", "indexRange": [0, 15], "fields": ["velocity"]}
            break
    problems, _ = gate.check(spec, badfe)
    if not has(problems, "record commandAddress fields ['velocity'] != expected ['note', 'value', 'gate']"):
        raise SystemExit("record commandAddress with a mismatched field not flagged: %r" % problems)

    # 69. record graph must be acyclic (msg 9d8b5f43 item #3): a direct field referencing its own
    #     schema is a self-cycle.
    cyc = copy.deepcopy(manifest)
    cyc["target"]["recordSchemas"]["keyboard_seq"]["fields"][0]["element"]["of"] = "keyboard_seq"
    problems, _ = gate.check(spec, cyc)
    if not has(problems, "record graph has a cycle"):
        raise SystemExit("record self-cycle not flagged: %r" % problems)

    # 70. duplicate record field names rejected (msg 9d8b5f43 item #3).
    dupf = copy.deepcopy(manifest)
    dupf["target"]["recordSchemas"]["keyboard_seq"]["fields"].append(
        {"name": "steps", "type": "record", "of": "keyboard_step"})
    problems, _ = gate.check(spec, dupf)
    if not has(problems, "duplicate field names ['steps']"):
        raise SystemExit("duplicate record field name not flagged: %r" % problems)

    # 71. array count must be a positive int, not bool/0/-1 (msg 9d8b5f43 item #3).
    badcnt = copy.deepcopy(manifest)
    badcnt["target"]["recordSchemas"]["keyboard_seq"]["fields"][0]["count"] = 0
    problems, _ = gate.check(spec, badcnt)
    if not has(problems, "needs positive int count"):
        raise SystemExit("array count 0 not rejected as non-positive: %r" % problems)

    # 72. vector cardinality / mask maskSize must be a positive int, not bool/0/-1 (msg 9d8b5f43
    #     item #3): isinstance(True,int) is True, so bool must be excluded by strict typing.
    badcard = copy.deepcopy(manifest)
    for it in badcard["target"]["parameters"]:
        if it["stable_id"] == "keyboard.plate_tune":
            it["cardinality"] = True
    problems, _ = gate.check(spec, badcard)
    if not has(problems, "shape=vector needs positive int cardinality"):
        raise SystemExit("vector cardinality bool not rejected: %r" % problems)
    badmsk = copy.deepcopy(manifest)
    for it in badmsk["target"]["parameters"]:
        if it["stable_id"] == "keyboard.quantise_scale_editor":
            it["maskSize"] = -1
    problems, _ = gate.check(spec, badmsk)
    if not has(problems, "shape=mask needs positive int maskSize"):
        raise SystemExit("mask maskSize -1 not rejected as non-positive: %r" % problems)

    # 73. preset referenced-set + excludes must EXACTLY partition the keyboard state (msg 9d8b5f43
    #     item #3): deleting a stored param from BOTH sets leaves a partition miss.
    miss = copy.deepcopy(manifest)
    miss["target"]["recordSchemas"]["keyboard_params_minus_clock"]["params"].remove("keyboard.behaviour")
    problems, _ = gate.check(spec, miss)
    if not has(problems, "neither stored nor excluded"):
        raise SystemExit("keyboard-state partition miss not flagged: %r" % problems)

    # 74. capability frozen table must be EXACTLY the module-ID set (msg 9d8b5f43 item #3); a target
    #     module with no CAPABILITIES entry is not silently DEFAULT_CAP'd.
    capmis = copy.deepcopy(manifest)
    capmis["target"]["modules"].append({
        "stable_id": "mutant", "name": "Mutant", "category": "util",
        "capabilities": {"parameters": False, "patchableJacks": False,
                         "internalEndpoints": False, "controls": False},
        "status": "confirmed",
        "evidence": {"ref": "solar42N_manual_v15", "lineStart": 1, "lineEnd": 1, "panelSite": "mutant"},
    })
    problems, _ = gate.check(spec, capmis)
    if not has(problems, "!= module stable_ids"):
        raise SystemExit("capability table != module-ID set not flagged: %r" % problems)

    # ---- msg 23ea438f (eighth review) validations --------------------------
    # Item #2 — a condition / commandAddress must be an EXECUTABLE relationship, not merely a
    # closed key name. A held control must be a real momentary-touch element 1:1 with the index;
    # a menu item must name the invoked action's leaf; a commandAddress must match the target's
    # derived record/mask locator (kind/range/fields).
    # 75. heldControl naming a control absent from the panel inventory is a masked predicate.
    badheld = copy.deepcopy(manifest)
    for b in badheld["target"]["controlBindings"]:
        if isinstance(b.get("condition"), dict) and b["condition"].get("heldControl"):
            b["condition"] = dict(b["condition"])
            b["condition"]["heldControl"] = "keyboard.plate_99"
            break
    problems, _ = gate.check(spec, badheld)
    if not has(problems, "heldControl 'keyboard.plate_99' is not a declared panelControl"):
        raise SystemExit("heldControl naming an unknown control not flagged: %r" % problems)

    # 76. heldControl element suffix must be 1:1 with the binding index (hold plate 1 but write
    #     vector element 5 is un-executable).
    idxheld = copy.deepcopy(manifest)
    for b in idxheld["target"]["controlBindings"]:
        if b["from"] == "keyboard.encoder" and b["to"] == "keyboard.plate_tune" and b.get("index") == 1:
            b["index"] = 5
            break
    problems, _ = gate.check(spec, idxheld)
    if not has(problems, "does not match binding index"):
        raise SystemExit("heldControl element/index mismatch not flagged: %r" % problems)

    # 77. menuItem must name the invoked action's leaf (an arbitrary string can't be a highlighted
    #     item).
    badmenu = copy.deepcopy(manifest)
    for b in badmenu["target"]["controlBindings"]:
        c = b.get("condition")
        if isinstance(c, dict) and c.get("menuItem"):
            b["condition"] = dict(c)
            b["condition"]["menuItem"] = "not_a_real_item"
            break
    problems, _ = gate.check(spec, badmenu)
    if not has(problems, "does not name the target leaf"):
        raise SystemExit("menuItem not naming the action leaf not flagged: %r" % problems)

    # 78. commandAddress kind/range/fields must be DERIVED from the target; a mask locator on a
    #     record editor is a mixed address, not a valid one.
    badca2 = copy.deepcopy(manifest)
    for b in badca2["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps":
            b["commandAddress"] = {"kind": "mask", "indexRange": [0, 7], "fields": []}
            break
    problems, _ = gate.check(spec, badca2)
    if not has(problems, "commandAddress kind 'mask' != expected 'record'"):
        raise SystemExit("record editor given a mask commandAddress not flagged: %r" % problems)

    # Item #3 — actionManaged is NOT an arbitrary orphan escape: valid only on a preset payload, and
    # each payload must be covered by the A-D load/save/initialise relationship, not merely flagged.
    # 79. actionManaged lifted onto a non-preset (scalar) parameter is an escape hatch.
    amnon = copy.deepcopy(manifest)
    for p in amnon["target"]["parameters"]:
        if p.get("shape") != "record":
            p["actionManaged"] = True
            break
    problems, _ = gate.check(spec, amnon)
    if not has(problems, "actionManaged on non-preset-payload parameter"):
        raise SystemExit("actionManaged on a non-preset parameter not flagged: %r" % problems)

    # 80. a preset payload that is not flagged actionManaged must be reported (the boolean is
    #     required, not optional).
    pamiss = copy.deepcopy(manifest)
    for p in pamiss["target"]["parameters"]:
        if p.get("stable_id") == "keyboard.preset_a":
            p["actionManaged"] = False
            break
    problems, _ = gate.check(spec, pamiss)
    if not has(problems, "preset payload parameter(s) ['keyboard.preset_a'] must be actionManaged"):
        raise SystemExit("unflagged preset payload not reported: %r" % problems)

    # 81. each preset slot must be covered by ALL of load/save/initialise (dropping the save
    #     relationship for a slot is a real gap).
    covmiss = copy.deepcopy(manifest)
    covmiss["target"]["controlBindings"] = [
        b for b in covmiss["target"]["controlBindings"] if not
        ((b.get("condition") or {}).get("presetSlot") == "preset_a" and b.get("to") == "keyboard.presets_save")]
    problems, _ = gate.check(spec, covmiss)
    if not has(problems, "preset payload for slot preset_a not covered by A-D load/save/initialise"):
        raise SystemExit("preset slot missing a load/save/initialise relationship not flagged: %r" % problems)

    # Item #4 — schema/binding graph closure: params lists may not hold duplicates, the record DFS
    # must cross params -> parameter(recordType), and context is part of semantic identity.
    # 82. duplicate member in a params-kind schema is a closed-set contradiction.
    dupam = copy.deepcopy(manifest)
    dupam["target"]["recordSchemas"]["keyboard_params_minus_clock"]["params"].append("keyboard.behaviour")
    problems, _ = gate.check(spec, dupam)
    if not has(problems, "duplicate param members"):
        raise SystemExit("duplicate params member not flagged: %r" % problems)

    # 83. a cycle record -> params -> parameter(recordType) -> record must be caught (the DFS crosses
    #     both edge kinds). Adding preset_a (recordType=keyboard_preset) to the params set closes a
    #     2-cycle with keyboard_preset -> keyboard_params_minus_clock; moving it out of excludes keeps
    #     the keyboard-state partition intact so the only defect reported is the cycle.
    cyc2 = copy.deepcopy(manifest)
    cyc2["target"]["recordSchemas"]["keyboard_params_minus_clock"]["params"].append("keyboard.preset_a")
    cyc2["target"]["recordSchemas"]["keyboard_preset"]["excludes"].remove("keyboard.preset_a")
    problems, _ = gate.check(spec, cyc2)
    if not has(problems, "record graph has a cycle"):
        raise SystemExit("params->parameter(recordType) cycle not flagged: %r" % problems)

    # ---- msg 720fc3d3 (ninth review) validations: dispatch closure + exact schema ------
    # Item #1f — "press one slot hits 3 actions": a preset press binding that DROPS its per-item
    # selector (menuItem) leaves only presetSlot, so one press fires load+save+initialise at once.
    # Both the item-scoped selector requirement and the dispatch fan-out gate must catch it.
    p3 = copy.deepcopy(manifest)
    for b in p3["target"]["controlBindings"]:
        c = b.get("condition")
        if isinstance(c, dict) and "presetSlot" in c and "menuItem" in c:
            b["condition"] = dict(c)
            b["condition"].pop("menuItem")
    problems, _ = gate.check(spec, p3)
    if not has(problems, "must carry a menuItem condition"):
        raise SystemExit("press-one-slot with a dropped selector not flagged as un-item-scoped: %r" % problems)
    if not has(problems, "dispatch fan-out"):
        raise SystemExit("press-one-slot multi-target fan-out not flagged: %r" % problems)

    # Item #1f — "ARP rotate changes 7 params": an arp-menu rotate binding without a menuItem rotates
    # every arp param the menu holds (7), not one. Same two gates, and the fan-out is specifically the
    # 7 arp params (a 1-target "fan-out" would be a no-op and must not pass).
    a7 = copy.deepcopy(manifest)
    for b in a7["target"]["controlBindings"]:
        c = b.get("condition")
        ctx = b.get("context")
        if (isinstance(ctx, dict) and ctx.get("type") == "keyboard-menu" and ctx.get("menu") == "arp"
                and isinstance(c, dict) and "menuItem" in c):
            b["condition"] = dict(c)
            b["condition"].pop("menuItem")
    problems, _ = gate.check(spec, a7)
    if not has(problems, "must carry a menuItem condition"):
        raise SystemExit("arp-rotate with a dropped selector not flagged as un-item-scoped: %r" % problems)
    if not has(problems, "fires multiple targets"):
        raise SystemExit("arp-rotate 7-param fan-out not flagged as multi-target: %r" % problems)

    # Item #1c — whole-vector binding banned: a vector param (plate_tune / pushbutton_value) is
    # edited one element at a time via the global heldControl+index path. Deleting the index makes the
    # binding write every element at once, which the vector-needs-an-index rule must reject.
    wv = copy.deepcopy(manifest)
    for b in wv["target"]["controlBindings"]:
        if b.get("from") == "keyboard.encoder" and b.get("to") == "keyboard.plate_tune" and b.get("index") == 1:
            b["index"] = None
            break
    problems, _ = gate.check(spec, wv)
    if not has(problems, "whole-vector binding banned"):
        raise SystemExit("whole-vector plate_tune binding not flagged: %r" % problems)

    # Item #2a — commandAddress with an arbitrary junk key must be rejected (exact key-set).
    caek = copy.deepcopy(manifest)
    for b in caek["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps" and isinstance(b.get("commandAddress"), dict):
            b["commandAddress"] = dict(b["commandAddress"])
            b["commandAddress"]["junk"] = True
            break
    problems, _ = gate.check(spec, caek)
    if not has(problems, "commandAddress keys ['junk'] not allowed"):
        raise SystemExit("commandAddress junk key not flagged by exact key-set: %r" % problems)

    # Item #2a — commandAddress fields must be unique (a duplicated gate masks a re-address).
    cadup = copy.deepcopy(manifest)
    for b in cadup["target"]["controlBindings"]:
        if b.get("to") == "keyboard.seq_steps":
            b["commandAddress"] = {"kind": "record", "indexRange": [0, 15],
                                   "fields": ["gate", "note", "value", "gate"]}
            break
    problems, _ = gate.check(spec, cadup)
    if not has(problems, "contain a duplicate (must be unique)"):
        raise SystemExit("commandAddress duplicate field not flagged: %r" % problems)

    # Item #2b — the record* condition keys (recordField/recordIndex/maskIndex) were declared but
    # never validated nor used; they are now REMOVED from the closed set, so a binding carrying
    # condition.recordIndex='garbage' must be rejected rather than silently ignored.
    recid = copy.deepcopy(manifest)
    for b in recid["target"]["controlBindings"]:
        if isinstance(b.get("condition"), dict):
            b["condition"] = dict(b["condition"])
            b["condition"]["recordIndex"] = "garbage"
            break
    problems, _ = gate.check(spec, recid)
    if not has(problems, "condition key 'recordIndex' not in"):
        raise SystemExit("removed condition.recordIndex not rejected as an unclosed key: %r" % problems)

    # Item #2c — a params schema with an arbitrary top-level key must be rejected (exact key-set).
    ptop = copy.deepcopy(manifest)
    ptop["target"]["recordSchemas"]["keyboard_params_minus_clock"]["junk"] = True
    problems, _ = gate.check(spec, ptop)
    if not has(problems, "top-level keys ['junk'] not allowed"):
        raise SystemExit("params schema top-level junk key not flagged: %r" % problems)

    # Item #2d — an array field element with an arbitrary key must be rejected (exact key-set).
    elx = copy.deepcopy(manifest)
    elx["target"]["recordSchemas"]["keyboard_seq"]["fields"][0]["element"]["junk"] = True
    problems, _ = gate.check(spec, elx)
    if not has(problems, "element has keys ['junk'] not allowed"):
        raise SystemExit("array element junk key not flagged: %r" % problems)

    # Codex 10th-review (msg e9f1f028) — PRESETS two-page state machine. The presets workflow is a
    # REAL two-page machine (slot-list with A-D, then action-list with load/save/initialise); the
    # Round-10 root was collapsing both pages into one flat `menu=presets` with the navigation actions
    # masquerading as their own menuItem. Each of the four defects below must fail for ITS intended
    # reason.

    # (a) merge/delete a page: drop the `page` selector from every presets binding, flattening both
    #     pages back into one flat menu=presets. The context key-set MUST demand menu+page, and the
    #     two-page completeness must notice a page has no bindings.
    mergepg = copy.deepcopy(manifest)
    for b in mergepg["target"]["controlBindings"]:
        ctx = b.get("context")
        if (isinstance(ctx, dict) and ctx.get("type") == "keyboard-menu" and ctx.get("menu") == "presets"):
            b["context"] = {k: v for k, v in ctx.items() if k != "page"}
    problems, _ = gate.check(spec, mergepg)
    if not has(problems, "must carry exactly ['menu', 'page']"):
        raise SystemExit("merged presets pages (page selector dropped) not flagged by exact context "
                         "key-set: %r" % problems)
    if not has(problems, "page 'slot-list' has no bindings"):
        raise SystemExit("merged presets pages not flagged as a deleted/absent page: %r" % problems)

    # (b) bad page value: a page name outside the closed PRESET_PAGES topology must be rejected.
    badpage = copy.deepcopy(manifest)
    for b in badpage["target"]["controlBindings"]:
        ctx = b.get("context")
        if (isinstance(ctx, dict) and ctx.get("type") == "keyboard-menu"
                and ctx.get("menu") == "presets" and ctx.get("page") == "slot-list"):
            b["context"] = dict(ctx)
            b["context"]["page"] = "slotz"
            break
    problems, _ = gate.check(spec, badpage)
    if not has(problems, "presets page 'slotz' not in"):
        raise SystemExit("non-topology presets page not rejected: %r" % problems)

    # (c) enter lacks a slot: an enter-subpage binding without a closed presetSlot can't tell which
    #     slot's action-list it enters, so it must fail.
    noslot = copy.deepcopy(manifest)
    for b in noslot["target"]["controlBindings"]:
        if b.get("to") == "keyboard.preset_enter_subpage":
            b["condition"] = {k: v for k, v in b["condition"].items() if k != "presetSlot"}
            break
    problems, _ = gate.check(spec, noslot)
    if not has(problems, "enter-subpage must carry a closed presetSlot"):
        raise SystemExit("enter-subpage without presetSlot not flagged: %r" % problems)

    # (d) forge menuItem from the nav action itself: a slot-list binding whose menuItem is the
    #     preset_enter_subpage navigation action (not a real display item) must be rejected — the
    #     menu topology lives in the declared PRESET_PAGE_ITEMS, not in the target's own leaf.
    forgeslot = copy.deepcopy(manifest)
    for b in forgeslot["target"]["controlBindings"]:
        ctx = b.get("context")
        if (isinstance(ctx, dict) and ctx.get("type") == "keyboard-menu"
                and ctx.get("menu") == "presets" and ctx.get("page") == "slot-list"):
            b["condition"] = dict(b["condition"])
            b["condition"]["menuItem"] = "preset_enter_subpage"
            break
    problems, _ = gate.check(spec, forgeslot)
    if not has(problems, "is not a display item of page 'slot-list'"):
        raise SystemExit("nav action forged as a presets menuItem not rejected: %r" % problems)

    # Codex 11th-review (msg 8cc637e9) — PRESETS EXACT relation-set gate. The page/menuItem/slot
    # topology is correct in the data but the checks above only enforced "each page has >=1 binding
    # and values are in the allowed set", so a SINGLE-edge deletion still returned 0 problems. Each
    # defect below must be caught by the exact-set equality gate (missing one, extra one, or a
    # misaligned slot/item pairing) rather than silently passing.

    # Drop exactly ONE binding matching a predicate (Codex 11th-review msg 0153c91f: the earlier
    # (e)/(f)/(g) deleted ALL edges for a target, which would not catch a return to the weak
    # "at least one" gate — single-edge deletion must be what is tested).
    def _drop_first(m, pred):
        bs = m["target"]["controlBindings"]
        for i, b in enumerate(bs):
            if pred(b):
                del bs[i]
                return
        raise SystemExit("test setup: no binding matched drop predicate")

    # (e) delete ONE slot-list press edge (preset_a's preset_enter_subpage). Other slots remain, so the
    #     page/enter checks above still pass; only the exact relation set notices the missing edge.
    del_enter = copy.deepcopy(manifest)
    _drop_first(del_enter, lambda b: b.get("to") == "keyboard.preset_enter_subpage"
                and (b.get("condition") or {}).get("presetSlot") == "preset_a")
    problems, _ = gate.check(spec, del_enter)
    if not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("deleted one presets enter-subpage edge not flagged by exact relation set: %r"
                         % problems)

    # (f) delete ONE slot-list rotate edge (preset_a's preset_select_slot).
    del_select = copy.deepcopy(manifest)
    _drop_first(del_select, lambda b: b.get("to") == "keyboard.preset_select_slot"
                and (b.get("condition") or {}).get("menuItem") == "preset_a")
    problems, _ = gate.check(spec, del_select)
    if not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("deleted one presets select_slot edge not flagged by exact relation set: %r"
                         % problems)

    # (g) delete ONE action-list rotate edge (presets_load's preset_select_action).
    del_act = copy.deepcopy(manifest)
    _drop_first(del_act, lambda b: b.get("to") == "keyboard.preset_select_action"
                and (b.get("condition") or {}).get("menuItem") == "presets_load")
    problems, _ = gate.check(spec, del_act)
    if not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("deleted one presets select_action edge not flagged by exact relation set: %r"
                         % problems)

    # (h) enter slot/item mismatch: keep presetSlot=preset_a but re-point menuItem at a DIFFERENT slot.
    #     Both are valid slot-list display items and the target is allowed, so every per-binding and
    #     both-pages check passes; only the exact relation set sees the missing aligned tuple (and the
    #     extra misaligned one).
    mis_enter = copy.deepcopy(manifest)
    for b in mis_enter["target"]["controlBindings"]:
        if b.get("to") == "keyboard.preset_enter_subpage" \
                and (b.get("condition") or {}).get("presetSlot") == "preset_a":
            b["condition"] = dict(b["condition"])
            b["condition"]["menuItem"] = "preset_b"
            break
    problems, _ = gate.check(spec, mis_enter)
    if not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("enter presetSlot/menuItem mismatch not flagged by exact relation set: %r"
                         % problems)

    # (i) execute target/item mismatch: an action-list press whose target names one operation but
    #     menuItem names a DIFFERENT operation. Also delete the original matching edge for that gesture
    #     so the dispatch fan-out gate does not catch this coincidentally (Codex 8cc637e9) — the exact
    #     relation set is the sole catcher of a target/item misalignment.
    mis_exec = copy.deepcopy(manifest)
    mis_exec["target"]["controlBindings"] = [
        b for b in mis_exec["target"]["controlBindings"]
        if not (b.get("to") == "keyboard.presets_load"
                and (b.get("condition") or {}).get("presetSlot") == "preset_a")
    ]
    for b in mis_exec["target"]["controlBindings"]:
        if b.get("to") == "keyboard.presets_save" \
                and (b.get("condition") or {}).get("presetSlot") == "preset_a":
            b["condition"] = dict(b["condition"])
            b["condition"]["menuItem"] = "presets_load"
            break
    problems, _ = gate.check(spec, mis_exec)
    if not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("execute target/menuItem mismatch not flagged by exact relation set: %r"
                         % problems)

    # (j) workflow-action bypass (Codex 11th-review msg 0153c91f): copy a presets_load edge and rebind
    #     it into a LEGAL program context, dropping its condition. The exact-set projection previously
    #     only collected bindings whose context was `menu=presets`, so this escaped the matrix with
    #     NORMAL 0 problems while a PRESETS workflow action now existed outside the two-page machine.
    #     The six PRESETS workflow targets are a CLOSED set now: any binding whose `to` is one of them
    #     must sit on a PRESETS page, so a stray program/global-context binding is both a projectable
    #     extra AND an explicit error.
    byp = copy.deepcopy(manifest)
    src_b = next(b for b in byp["target"]["controlBindings"]
                 if b.get("to") == "keyboard.presets_load")
    stray = copy.deepcopy(src_b)
    stray["context"] = {"type": "program", "program": "program.cathedral.1"}
    stray["condition"] = {}
    byp["target"]["controlBindings"].append(stray)
    problems, _ = gate.check(spec, byp)
    if not has(problems, "bound outside a PRESETS page") or \
       not has(problems, "presets relation set != closed manual matrix"):
        raise SystemExit("presets workflow action rebound to a program context not flagged: %r"
                         % problems)

    # (k) Root A — false coverage: a registry ParameterDescriptor can only back a SCALAR target.
    #     Changing a scalar target's shape to vector (with the cardinality it demands) makes the
    #     still-scalar registry descriptor a false scalar over a real-vector target (Codex 22f545c1).
    a_pres = copy.deepcopy(manifest)
    a_tgt = tgt_param(a_pres, "vco_a.oct_sel")
    a_tgt["shape"] = "vector"; a_tgt["cardinality"] = 3; a_tgt.pop("positions", None)
    problems, _ = gate.check(spec, a_pres)
    if not has(problems, "cannot be represented as a scalar ParameterDescriptor"):
        raise SystemExit("Root A: non-scalar target faked as scalar not flagged: %r" % problems)

    # (l) Root B — selector cardinality: a bool (max-min=1) can never cover the multi-position
    #     selector the target declares. Re-introduce the bool mis-typing for vco_a.oct_sel.
    b_pres = copy.deepcopy(spec)
    b_parm = reg_param(b_pres, "vco_a.oct_sel")
    b_parm["min"] = 0.0; b_parm["max"] = 1.0; b_parm["step"] = 1.0
    problems, _ = gate.check(b_pres, manifest)
    if not has(problems, "selector-toggle cardinality"):
        raise SystemExit("Root B: bool selector-cardinality mismatch not flagged: %r" % problems)

    # (m) Root B (manifest self-coherence) — a selector-toggle parameter's positions must be a
    #     non-empty label list, not a bare scalar.
    m_pres = copy.deepcopy(manifest)
    m_tgt = tgt_param(m_pres, "vco_a.oct_sel")
    m_tgt["positions"] = "low,0,+3"
    problems, _ = gate.check(spec, m_pres)
    if not has(problems, "selector-toggle positions must be"):
        raise SystemExit("Root B: malformed selector-toggle positions not flagged: %r" % problems)

    # (n) Root C — per-field provenance: every registry param must carry a fieldEvidence audit.
    n_pres = copy.deepcopy(spec)
    n_parm = reg_param(n_pres, "vco_a.oct_sel")
    del n_parm["fieldEvidence"]
    problems, _ = gate.check(n_pres, manifest)
    if not has(problems, "missing fieldEvidence audit object"):
        raise SystemExit("Root C: missing fieldEvidence not flagged: %r" % problems)

    # (o) Root C — a fieldEvidence status must be in {confirmed, unverified, provisional}.
    o_pres = copy.deepcopy(spec)
    o_parm = reg_param(o_pres, "vco_a.oct_sel")
    o_parm["fieldEvidence"]["unit"] = "bogus"
    problems, _ = gate.check(o_pres, manifest)
    if not has(problems, "fieldEvidence.unit bad status"):
        raise SystemExit("Root C: invalid fieldEvidence status not flagged: %r" % problems)

    # (p) Codex 03848819 Root 2 — runaway positions: a registry parameter carries selector positions
    #     but its target is NOT a selector-toggle. Positions are only valid on a selector-toggle, so
    #     this is positions-on-the-wrong-entity (the option table would label a continuous control).
    p_pres = copy.deepcopy(spec)
    p_parm = reg_param(p_pres, "vco_a.tune")
    p_parm["positions"] = ["ON", "OFF"]
    problems, _ = gate.check(p_pres, manifest)
    if not has(problems, "but its target is not a selector-toggle"):
        raise SystemExit("Root 2: positions on a non-selector-toggle not flagged: %r" % problems)

    # (q) Codex 03848819 Root 2 — delete-positions-to-pass: an IMPLEMENTED selector-toggle target
    #     stripped of its positions is the "un-evidenced bool treated as implemented" class. The old
    #     gate skipped every selector check once positions were gone; now an implemented selector with
    #     no target evidence MUST stay a gap.
    q_pres = copy.deepcopy(manifest)
    q_tgt = tgt_param(q_pres, "vco_a.oct_sel")
    del q_tgt["positions"]
    problems, _ = gate.check(spec, q_pres)
    if not has(problems, "must stay a GAP"):
        raise SystemExit("Root 2: implemented selector with no target positions not flagged: %r"
                         % problems)

    # (r) Codex 03848819 Root 2 — option labels must equal the target EXACTLY (UI/MIDI must not get a
    #     bare 0/1/2 integer without its meaning). Re-label the implemented registry selector.
    r_pres = copy.deepcopy(spec)
    r_parm = reg_param(r_pres, "vco_a.oct_sel")
    r_parm["positions"] = ["-1", "0", "+12"]
    problems, _ = gate.check(r_pres, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("Root 2: mismatched registry selector labels not flagged: %r" % problems)

    # (s) Codex 03848819 Root 2 — a software-mapped selector value domain must be provisional, never an
    #     unverified-but-concrete range (the seemingly-certain bool mis-read). Extended by Codex
    #     644ea86e Root 2 to ALL SIX fieldEvidence fields: any consumed value on an implemented selector
    #     must be provisional or confirmed, never unverified (a concrete value can't be un-evidenced).
    s_pres = copy.deepcopy(spec)
    s_parm = reg_param(s_pres, "vco_a.oct_sel")
    s_parm["fieldEvidence"]["range"] = "unverified"
    problems, _ = gate.check(s_pres, manifest)
    if not has(problems, "implemented selector fieldEvidence.range is unverified"):
        raise SystemExit("Root 2: unverified selector range not flagged: %r" % problems)

    # (t) Codex 644ea86e Root 1 — UNIMPLEMENTED physical selector delete-to-pass: vco_b.oct_sel is a
    #     target selector-toggle whose panelControl carries the authoritative domain, but the registry
    #     does not implement it yet (--require-full reports it as target-not-implemented). Deleting its
    #     target positions must still fail via the target↔panel cross-check, not silently skip because
    #     the registry has no descriptor.
    t_pres = copy.deepcopy(manifest)
    t_tgt = tgt_param(t_pres, "vco_b.oct_sel")
    del t_tgt["positions"]
    problems, _ = gate.check(spec, t_pres)
    if not has(problems, "!= panelControl positions"):
        raise SystemExit("Root 1: unimplemented physical selector stripped of positions not flagged: %r"
                         % problems)

    # (u) Codex 644ea86e Root 1 — UNIMPLEMENTED physical selector domain mismatch: re-labelling the
    #     target's positions away from its panel widget must fail the cross-check even though no
    #     registry descriptor exists to compare against.
    u_pres = copy.deepcopy(manifest)
    u_tgt = tgt_param(u_pres, "vco_b.oct_sel")
    u_tgt["positions"] = ["a", "b", "c"]
    problems, _ = gate.check(spec, u_pres)
    if not has(problems, "!= panelControl positions"):
        raise SystemExit("Root 1: unimplemented physical selector with a mismatched domain not flagged: "
                         "%r" % problems)

    # (v) Codex 644ea86e Root 2 — label uniqueness: a selector whose value domain repeats a label is a
    #     degenerate mapping UI/MIDI cannot disambiguate (["same","same"]). A keyboard selector has no
    #     panel domain, so only the duplicate-label check is the catcher here.
    v_pres = copy.deepcopy(manifest)
    v_tgt = tgt_param(v_pres, "keyboard.behaviour")
    v_tgt["positions"] = ["same", "same"]
    problems, _ = gate.check(spec, v_pres)
    if not has(problems, "duplicate label"):
        raise SystemExit("Root 2: selector duplicate label not flagged: %r" % problems)

    # (w) Codex 2026-08-23 unknown-enum contract — jack: a signal-class value of `unknown` that is
    #     given a CONFIRMED/known provenance is a contradiction (an unknown is never a fact). The gate
    #     must reject it for its intended reason, not by accident.
    w_bad = copy.deepcopy(spec)
    w_jack = reg_jack(w_bad, "vco_a.cv_in")
    w_jack["signalType"] = "unknown"
    w_jack["fieldEvidence"]["signalType"] = "confirmed"
    problems, _ = gate.check(w_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("unknown-enum jack contract not enforced (unknown+confirmed passed): %r"
                         % problems)

    # (x) Codex 2026-08-23 unknown-enum contract — program: selfOscillating=unknown must not carry a
    #     confirmed provenance either.
    x_bad = copy.deepcopy(spec)
    x_prog = reg_prog(x_bad, "program.cathedral.1")
    x_prog["selfOscillating"] = "unknown"
    x_prog["fieldEvidence"]["selfOscillating"] = "confirmed"
    problems, _ = gate.check(x_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("unknown-enum program contract not enforced (selfOscillating unknown+confirmed "
                         "passed): %r" % problems)

    # (y) unknown-enum value not in the closed set (typo / invented type) must be rejected.
    y_bad = copy.deepcopy(spec)
    y_jack = reg_jack(y_bad, "vco_a.cv_in")
    y_jack["coupling"] = "bipolarish"
    problems, _ = gate.check(y_bad, manifest)
    if not has(problems, "coupling"):
        raise SystemExit("jack coupling not validated against the closed enum set: %r" % problems)

    # (z) Codex 587f5e72 REVERSE direction — a CONCRETE signal-class value with UNVERIFIED provenance
    #     must also fail (the old vco_b.vco_out=cv+unverified shape): a concrete value is either a
    #     documented hardware fact (confirmed) or a normalized mapping (provisional), never a guess.
    z_bad = copy.deepcopy(spec)
    z_jack = reg_jack(z_bad, "vco_b.vco_out")
    z_jack["signalType"] = "audio"
    z_jack["fieldEvidence"]["signalType"] = "unverified"
    problems, _ = gate.check(z_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("biconditional REVERSE not enforced (concrete+unverified jack passed): %r"
                         % problems)

    # (aa) Codex 587f5e72 bypass 1 — a program concrete family with UNVERIFIED provenance must fail.
    aa_bad = copy.deepcopy(spec)
    aa_prog = reg_prog(aa_bad, "program.cathedral.1")
    aa_prog["family"] = "reverb"
    aa_prog["fieldEvidence"]["family"] = "unverified"
    problems, _ = gate.check(aa_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("biconditional REVERSE for program family not enforced (concrete+unverified "
                         "family passed): %r" % problems)

    # (ab) Codex 587f5e72 bypass 2 — deleting the family evidence key entirely must fail (both
    #      ProgramFieldEvidence keys are required, each with a legal status).
    ab_bad = copy.deepcopy(spec)
    ab_prog = reg_prog(ab_bad, "program.cathedral.1")
    del ab_prog["fieldEvidence"]["family"]
    problems, _ = gate.check(ab_bad, manifest)
    if not has(problems, "fieldEvidence.family missing or bad status"):
        raise SystemExit("ProgramFieldEvidence.family key absence not enforced: %r" % problems)

    # (ac) forward direction for program family — family=unknown must not carry a confirmed fact.
    ac_bad = copy.deepcopy(spec)
    ac_prog = reg_prog(ac_bad, "program.cathedral.1")
    ac_prog["family"] = "unknown"
    ac_prog["fieldEvidence"]["family"] = "confirmed"
    problems, _ = gate.check(ac_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("family=unknown+confirmed not enforced: %r" % problems)

    # (ad) forward direction with PROVISIONAL (not just confirmed) — an unknown value never carries a
    #      non-unverified provenance, provisional included.
    ad_bad = copy.deepcopy(spec)
    ad_prog = reg_prog(ad_bad, "program.cathedral.1")
    ad_prog["selfOscillating"] = "unknown"
    ad_prog["fieldEvidence"]["selfOscillating"] = "provisional"
    problems, _ = gate.check(ad_bad, manifest)
    if not has(problems, "violates the unknown ⇔ unverified biconditional"):
        raise SystemExit("selfOscillating=unknown+provisional not enforced: %r" % problems)

    # ---- Phase B mods-gap landed-descriptor gate (Codex 17cc9a4a) ----
    # The 11 params + 7 jacks closed in this slice are frozen to explicit numeric ids, owners, kinds,
    # statuses and per-widget descriptorEvidence lines (and for jacks, direction/signalType/polarity/
    # coupling/nominalRange + fieldEvidence). The facts live in target.landedDescriptorFacts in the
    # manifest — NOT checker constants — so the manifest stays the single auditable target. Their
    # presence IS the declaration that they have landed, so a registry that DROPS one must FAIL normal
    # (it must not reopen a gap). Any renumber / re-owner / direction flip / status or evidence drift /
    # kind drift (selector-toggle->continuous via dropped positions) / legal-but-wrong signalType is
    # rejected by the landed-descriptor fact compare in check_registry_complete.py.

    # (ae) renumber a landed param id.
    ae_bad = copy.deepcopy(spec)
    reg_param(ae_bad, "vco_b.pwm")["id"] = 999
    problems, _ = gate.check(ae_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap renumber (param id) not enforced: %r" % problems)

    # (af) re-owner a landed param by relocating it into another module (owner is module-derived,
    #      so a drift means the entity physically moved).
    af_bad = copy.deepcopy(spec)
    af_p = reg_param(af_bad, "vco_b.pwm")
    for m in af_bad["modules"]:
        if m.get("stable_id") == "vco_b":
            m["parameters"].remove(af_p)
        if m.get("stable_id") == "vcf":
            m.setdefault("parameters", []).append(af_p)
    problems, _ = gate.check(af_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap re-owner (param) not enforced: %r" % problems)

    # (ag) evidence-line drift on a landed param (the fact's descriptorEvidence.line must match).
    ag_bad = copy.deepcopy(spec)
    reg_param(ag_bad, "vco_b.pwm")["evidence"]["line"] = 999
    problems, _ = gate.check(ag_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap evidence drift (param) not enforced: %r" % problems)

    # (ah) kind drift: drop the selector positions so a selector-toggle reads as continuous.
    ah_bad = copy.deepcopy(spec)
    del reg_param(ah_bad, "vcf.r_bp_lp")["positions"]
    problems, _ = gate.check(ah_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap kind drift (selector->continuous) not enforced: %r" % problems)

    # (ai) direction flip on a landed jack.
    ai_bad = copy.deepcopy(spec)
    reg_jack(ai_bad, "vco_a.wave_out")["direction"] = "input"
    problems, _ = gate.check(ai_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap direction flip (jack) not enforced: %r" % problems)

    # (aj) legal-but-wrong signalType on a landed jack (cv is a legal signal type, but wave_out is audio).
    aj_bad = copy.deepcopy(spec)
    reg_jack(aj_bad, "vco_a.wave_out")["signalType"] = "cv"
    problems, _ = gate.check(aj_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap legal-but-wrong signalType (jack) not enforced: %r" % problems)

    # (ak) status drift on a landed jack (values stay concrete, so only the landed lock catches it).
    ak_bad = copy.deepcopy(spec)
    reg_jack(ak_bad, "keyboard.pressure_out")["status"] = "provisional"
    problems, _ = gate.check(ak_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed mods-gap status drift (jack) not enforced: %r" % problems)

    # (al) drop a landed param entirely -> the MISSING landed descriptor must FAIL normal (Codex
    #      17cc9a4a: presence in landedDescriptorFacts IS the landed declaration, so a drop reopens
    #      a gap and must be a hard fail, never `continue`).
    al_bad = copy.deepcopy(spec)
    for m in al_bad["modules"]:
        if m.get("stable_id") == "vco_b":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "vco_b.pwm"]
    problems, _ = gate.check(al_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("landed mods-gap MISSING seen-param (vco_b.pwm) not enforced: %r" % problems)

    # (am) drop a landed jack entirely -> must FAIL normal.
    am_bad = copy.deepcopy(spec)
    for m in am_bad["modules"]:
        if m.get("stable_id") == "vco_a":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "vco_a.wave_out"]
    problems, _ = gate.check(am_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack"):
        raise SystemExit("landed mods-gap MISSING seen-jack (vco_a.wave_out) not enforced: %r" % problems)

    # (an) drop an OLD (pre-slice) param that the full 146-item mustComplete still covers -> the
    #      NON-REGRESSION landed-equality gate must FAIL normal. vco_a.morph is NOT a landed
    #      descriptor fact, so the MISSING-landed-descriptor gate is silent here; only the
    #      full-landed-set mustComplete covers it, which is exactly why a drop must still be caught
    #      (Codex 8165f8c2 Root 2: mustComplete == the whole current registry, old entities included).
    an_bad = copy.deepcopy(spec)
    for m in an_bad["modules"]:
        if m.get("stable_id") == "vco_a":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "vco_a.morph"]
    problems, _ = gate.check(an_bad, manifest)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("old-param drop (vco_a.morph) not enforced by mustComplete: %r" % problems)

    # (ao) drop an OLD (pre-slice) jack that the full mustComplete still covers -> must FAIL normal.
    ao_bad = copy.deepcopy(spec)
    for m in ao_bad["modules"]:
        if m.get("stable_id") == "vco_a":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "vco_a.cv_in"]
    problems, _ = gate.check(ao_bad, manifest)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("old-jack drop (vco_a.cv_in) not enforced by mustComplete: %r" % problems)

    # (ap) renumber a landed route -> facts are the only registry-specific id authority, so the
    #      numeric-id exact-compare must FAIL normal (an id-drift whose value is invisible to
    #      source/sink/status, which now derive from the canonical target).
    ap_bad = copy.deepcopy(spec)
    reg_route(ap_bad, "route.keyboard_v_oct_to_vco")["id"] = 99
    problems, _ = gate.check(ap_bad, manifest)
    if not has(problems, "id 99 !="):
        raise SystemExit("route renumber (id 0->99) not enforced by landedRouteFacts: %r" % problems)

    # (aq) wrong route endpoint (sink) -> canonical-target exact-compare FAIL. Use an EXISTING jack
    #      so only the endpoint (not a dangling-reference) mismatches; the wrong sink is caught as
    #      drift against target.normalizedRoutes, not a parallel facts copy.
    aq_bad = copy.deepcopy(spec)
    reg_route(aq_bad, "route.vco_b_vco_out_to_cv_in")["sinkJack"] = "vco_b.wave_out"
    problems, _ = gate.check(aq_bad, manifest)
    if not has(problems, "sinkJack"):
        raise SystemExit("route wrong sink (vco_b.cv_in -> vco_b.wave_out) not enforced: %r" % problems)

    # (ar) actual effective-evidence drift -> the route's evidence.line must equal the fact's
    #      descriptorEvidence.line; drifting it away (1142 -> 999) is caught at effective granularity.
    ar_bad = copy.deepcopy(spec)
    reg_route(ar_bad, "route.vcf_cv_l_to_cv_r")["evidence"]["line"] = 999
    problems, _ = gate.check(ar_bad, manifest)
    if not has(problems, "descriptorEvidence.line"):
        raise SystemExit("route evidence-line drift (1142 -> 999) not enforced: %r" % problems)

    # (as) delete an OLD (pre-slice) route covered by mustComplete -> MISSING landed route +
    #      NON-REGRESSION must both FAIL normal (Codex b527ef3b: whole current registry is mustComplete).
    as_bad = copy.deepcopy(spec)
    as_bad["normalizedRoutes"] = [r for r in as_bad["normalizedRoutes"]
                                  if r.get("stable_id") != "route.vcf_cv_l_to_cv_r"]
    problems, _ = gate.check(as_bad, manifest)
    if not has(problems, "MISSING landed descriptor route"):
        raise SystemExit("old-route drop (route.vcf_cv_l_to_cv_r) not MISSING-flagged: %r" % problems)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("old-route drop (route.vcf_cv_l_to_cv_r) not NON-REGRESSION-flagged: %r"
                        % problems)

    # (at) delete a NEW (this-slice) route covered by mustComplete -> also both gates FAIL.
    at_bad = copy.deepcopy(spec)
    at_bad["normalizedRoutes"] = [r for r in at_bad["normalizedRoutes"]
                                  if r.get("stable_id") != "route.vco_b_vco_out_to_cv_in"]
    problems, _ = gate.check(at_bad, manifest)
    if not has(problems, "MISSING landed descriptor route"):
        raise SystemExit("new-route drop (route.vco_b_vco_out_to_cv_in) not MISSING-flagged: %r"
                        % problems)
    if not has(problems, "NON-REGRESSION"):
        raise SystemExit("new-route drop (route.vco_b_vco_out_to_cv_in) not NON-REGRESSION-flagged: %r"
                        % problems)

    # (au) delete a FACT for a still-landed route -> the facts key-set must equal the present route
    #      set; a missing fact is a hard fail (Codex f2868b6b: dropping a fact must not NORMAL-pass).
    au_bad = copy.deepcopy(manifest)
    del au_bad["target"]["landedRouteFacts"]["routes"]["route.keyboard_v_oct_to_vco_b"]
    problems, _ = gate.check(spec, au_bad)
    if not has(problems, "absent from target.landedRouteFacts"):
        raise SystemExit("dropped route fact (route.keyboard_v_oct_to_vco_b) not enforced: %r"
                        % problems)

    # (av) actual route status drift -> status now derives from the canonical target, so a
    #      confirmed->provisional drift on the actual side must FAIL normal.
    av_bad = copy.deepcopy(spec)
    reg_route(av_bad, "route.vcf_cv_l_to_cv_r")["status"] = "provisional"
    problems, _ = gate.check(av_bad, manifest)
    if not has(problems, "status 'provisional'"):
        raise SystemExit("route actual status drift (confirmed -> provisional) not enforced: %r"
                        % problems)

    # (aw) drift the CANONICAL target sink to a different LEGAL input (so no dangling ref): because
    #      source/sink/status come only from target.normalizedRoutes, the actual sink no longer
    #      matches and the gate must FAIL — previously a parallel facts copy hid this.
    aw_bad = copy.deepcopy(manifest)
    for tr in aw_bad["target"]["normalizedRoutes"]:
        if tr.get("stable_id") == "route.vco_b_vco_out_to_cv_in":
            tr["sinkJack"] = "vco_a.cv_in"
    problems, _ = gate.check(spec, aw_bad)
    if not has(problems, "sinkJack"):
        raise SystemExit("canonical target sink drift (vco_b.cv_in -> vco_a.cv_in) not enforced: %r"
                        % problems)

    # (ax) drift the CANONICAL target status: the actual confirmed vs target provisional mismatch must
    #      FAIL (status authority is the canonical target, and it is a legal value, so no other gate
    #      catches it).
    ax_bad = copy.deepcopy(manifest)
    for tr in ax_bad["target"]["normalizedRoutes"]:
        if tr.get("stable_id") == "route.keyboard_v_oct_to_vco":
            tr["status"] = "provisional"
    problems, _ = gate.check(spec, ax_bad)
    if not has(problems, "status 'confirmed'"):
        raise SystemExit("canonical target status drift (confirmed -> provisional) not enforced: %r"
                        % problems)

    # (ay) drift the CANONICAL target evidence span so it no longer CONTAINS the descriptorEvidence.line
    #      -> the fact line must sit inside the target route's span; moving the span away is caught.
    ay_bad = copy.deepcopy(manifest)
    for tr in ay_bad["target"]["normalizedRoutes"]:
        if tr.get("stable_id") == "route.vcf_cv_l_to_cv_r":
            tr["evidence"]["lineStart"] = 2000
            tr["evidence"]["lineEnd"] = 2001
    problems, _ = gate.check(spec, ay_bad)
    if not has(problems, "outside target span"):
        raise SystemExit("canonical target evidence-span drift (1142 outside [2000,2001]) not enforced: %r"
                        % problems)

    # (az) drift BOTH the canonical target evidence ref AND the fact descriptorEvidence.ref to the
    #      same wrong_source while the ACTUAL (spec) route is unchanged. Because the registry route
    #      carries no ref, the generator fills it with generate_registry.DEFAULT_SOURCE
    #      (solar42N_manual_v15), so fact.ref==target.ref==wrong_source still NORMAL-passed before
    #      the actual-effective-ref comparison was added (Codex msg 478e06f6). The actual ref must
    #      equal the fact/target ref, so this must FAIL now.
    az_bad = copy.deepcopy(manifest)
    for tr in az_bad["target"]["normalizedRoutes"]:
        if tr.get("stable_id") == "route.vcf_cv_l_to_cv_r":
            tr["evidence"]["ref"] = "wrong_source"
    az_bad["target"]["landedRouteFacts"]["routes"]["route.vcf_cv_l_to_cv_r"][
        "descriptorEvidence"]["ref"] = "wrong_source"
    problems, _ = gate.check(spec, az_bad)
    if not has(problems, "evidence.ref"):
        raise SystemExit("canonical target+fact ref sync-drift (both -> wrong_source, actual on "
                        "DEFAULT_SOURCE) not enforced: %r" % problems)

    # ---- landed MODULE identity gate negatives (Codex msg c4e6c0ff) ----
    # The module gate is a single authority: target.modules carries name/category/status/evidence,
    # landedModuleFacts carries ONLY the numeric id, and the facts key-set must equal the present
    # (landed) module set. Mutating the ACTUAL module below simulates a registry-side edit; mutating
    # the manifest simulates an editorial/facts-side change. Every identity field must be caught.

    # (ba) renumber a landed module -> the fact's authoritative id is the only registry-specific one.
    ba_bad = copy.deepcopy(spec)
    for m in ba_bad["modules"]:
        if m.get("stable_id") == "envelope_b":
            m["id"] = 99
    problems, _ = gate.check(ba_bad, manifest)
    if not has(problems, "id 99 !="):
        raise SystemExit("module id renumber (envelope_b 5 -> 99) not enforced: %r" % problems)

    # (bb) name drift -> the actual name must equal target.modules.
    bb_bad = copy.deepcopy(spec)
    for m in bb_bad["modules"]:
        if m.get("stable_id") == "envelope_a":
            m["name"] = "Env A"
    problems, _ = gate.check(bb_bad, manifest)
    if not has(problems, "name 'Env A' !="):
        raise SystemExit("module name drift ('Envelope A' -> 'Env A') not enforced: %r" % problems)

    # (bc) category drift -> a legal category (source) is still a wrong fact for a filter module.
    bc_bad = copy.deepcopy(spec)
    for m in bc_bad["modules"]:
        if m.get("stable_id") == "vcf":
            m["category"] = "source"
    problems, _ = gate.check(bc_bad, manifest)
    if not has(problems, "category 'source' !="):
        raise SystemExit("module category drift (processing -> source) not enforced: %r" % problems)

    # (bd) status drift -> values stay concrete so only the target comparison catches it.
    bd_bad = copy.deepcopy(spec)
    for m in bd_bad["modules"]:
        if m.get("stable_id") == "envelope_b":
            m["status"] = "provisional"
    problems, _ = gate.check(bd_bad, manifest)
    if not has(problems, "status 'provisional' !="):
        raise SystemExit("module status drift (confirmed -> provisional) not enforced: %r" % problems)

    # (be) evidence lineStart drift -> the module's cited span must equal target.modules.
    be_bad = copy.deepcopy(spec)
    for m in be_bad["modules"]:
        if m.get("stable_id") == "keyboard":
            m["evidence"]["lineStart"] = 999
    problems, _ = gate.check(be_bad, manifest)
    if not has(problems, "evidence.lineStart 999"):
        raise SystemExit("module evidence.lineStart drift (559 -> 999) not enforced: %r" % problems)

    # (bf) delete a fact for a still-landed module -> the facts key-set must equal the present set
    #      (direction a: a registry module with no fact must FAIL).
    bf_bad = copy.deepcopy(manifest)
    del bf_bad["target"]["landedModuleFacts"]["modules"]["vco_b"]
    problems, _ = gate.check(spec, bf_bad)
    if not has(problems, "absent from target.landedModuleFacts"):
        raise SystemExit("module-fact delete (vco_b fact dropped, module still present) not enforced: %r"
                        % problems)

    # (bg) delete a whole landed module (and its route) -> direction b: a fact naming an absent
    #      module must FAIL. The route is removed because its sink envelope_b.gate_in disappears with
    #      the module, so the dangling-endpoint validator would otherwise fire first and mask the
    #      module gate.
    bg_bad = copy.deepcopy(spec)
    bg_bad["modules"] = [m for m in bg_bad["modules"] if m.get("stable_id") != "envelope_b"]
    bg_bad["normalizedRoutes"] = [r for r in bg_bad["normalizedRoutes"]
                                  if r.get("stable_id") != "route.keyboard_gate_to_eg_b"]
    problems, _ = gate.check(bg_bad, manifest)
    if not has(problems, "MISSING landed module 'envelope_b'"):
        raise SystemExit("module delete (envelope_b + its route removed) not enforced: %r" % problems)

    # (bh) envelope_a.gate_in nominalRange back to CONFIRMED -> the landed descriptor fact locks
    #      nominalRange=unverified (Codex msg c419bad0 NON-GO), so undoing the correction must FAIL.
    bh_bad = copy.deepcopy(spec)
    for m in bh_bad["modules"]:
        if m.get("stable_id") == "envelope_a":
            for j in m["jacks"]:
                if j.get("stable_id") == "envelope_a.gate_in":
                    j["fieldEvidence"]["nominalRange"] = "confirmed"
    problems, _ = gate.check(bh_bad, manifest)
    if not has(problems, "implementation jack 'envelope_a.gate_in'"):
        raise SystemExit("envelope_a.gate_in nominalRange reverting to confirmed not enforced: %r"
                        % problems)

    # (bi) A/B gate_input evidence back to 576 -> L576 is the GATE LEFT OUTPUT rail (0-10V) only;
    #      the gate INPUT normalization is L577 (Codex msg c419bad0 NON-GO), so a 576 cite must FAIL
    #      for BOTH envelopes now that their facts lock line 577.
    bi_bad = copy.deepcopy(spec)
    for m in bi_bad["modules"]:
        if m.get("stable_id") in ("envelope_a", "envelope_b"):
            for j in m["jacks"]:
                if j.get("stable_id") in ("envelope_a.gate_in", "envelope_b.gate_in"):
                    j["evidence"]["line"] = 576
    problems, _ = gate.check(bi_bad, manifest)
    if not has(problems, "descriptorEvidence.line=576"):
        raise SystemExit("A/B gate_input evidence reverting to line 576 (output rail) not enforced: "
                        "%r" % problems)

    # (bj)-(bq) LFO slice (Codex msg 2ec93491): the newly-landed lfo_a/lfo_b modules and their
    # params/jacks must be caught by the SAME landed-module / landed-param / landed-jack gates — a
    # renumber, delete, or key evidence-line drift on a NEW entity (not just the older ones) must FAIL
    # normal. lfo carries no normalized route, so a module delete needs no dangling-endpoint strip.

    # (bj) renumber a landed LFO module id.
    bj_bad = copy.deepcopy(spec)
    for m in bj_bad["modules"]:
        if m.get("stable_id") == "lfo_a":
            m["id"] = 99
    problems, _ = gate.check(bj_bad, manifest)
    if not has(problems, "id 99 !="):
        raise SystemExit("LFO module id renumber (lfo_a 6 -> 99) not enforced: %r" % problems)

    # (bk) delete the LFO module fact (module still landed) -> facts key-set must equal present set.
    bk_bad = copy.deepcopy(manifest)
    del bk_bad["target"]["landedModuleFacts"]["modules"]["lfo_b"]
    problems, _ = gate.check(spec, bk_bad)
    if not has(problems, "absent from target.landedModuleFacts"):
        raise SystemExit("LFO module-fact delete (lfo_b fact dropped, module still present) not "
                        "enforced: %r" % problems)

    # (bl) delete a whole LFO module -> a fact naming an absent module must FAIL. No route to strip.
    bl_bad = copy.deepcopy(spec)
    bl_bad["modules"] = [m for m in bl_bad["modules"] if m.get("stable_id") != "lfo_a"]
    problems, _ = gate.check(bl_bad, manifest)
    if not has(problems, "MISSING landed module 'lfo_a'"):
        raise SystemExit("LFO module delete (lfo_a removed) not enforced: %r" % problems)

    # (bm) renumber a landed LFO param id.
    bm_bad = copy.deepcopy(spec)
    reg_param(bm_bad, "lfo_a.wave")["id"] = 999
    problems, _ = gate.check(bm_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("LFO param id renumber (lfo_a.wave 147 -> 999) not enforced: %r" % problems)

    # (bn) delete a landed LFO param -> the MISSING landed descriptor must FAIL normal.
    bn_bad = copy.deepcopy(spec)
    for m in bn_bad["modules"]:
        if m.get("stable_id") == "lfo_a":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "lfo_a.rate"]
    problems, _ = gate.check(bn_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("LFO param delete (lfo_a.rate) not enforced: %r" % problems)

    # (bo) delete a landed LFO jack -> must FAIL normal.
    bo_bad = copy.deepcopy(spec)
    for m in bo_bad["modules"]:
        if m.get("stable_id") == "lfo_a":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "lfo_a.cv_out"]
    problems, _ = gate.check(bo_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack"):
        raise SystemExit("LFO jack delete (lfo_a.cv_out) not enforced: %r" % problems)

    # (bp) key evidence-line drift on a landed LFO param (speed_mult selector line).
    bp_bad = copy.deepcopy(spec)
    reg_param(bp_bad, "lfo_a.speed_mult")["evidence"]["line"] = 999
    problems, _ = gate.check(bp_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("LFO param evidence drift (speed_mult line) not enforced: %r" % problems)

    # (bq) key evidence-line drift on a landed LFO jack (cv_out nominal-range cite).
    bq_bad = copy.deepcopy(spec)
    reg_jack(bq_bad, "lfo_a.cv_out")["evidence"]["line"] = 999
    problems, _ = gate.check(bq_bad, manifest)
    if not has(problems, "descriptorEvidence.line=999"):
        raise SystemExit("LFO jack evidence drift (cv_out line) not enforced: %r" % problems)

    # (br)-(bx) landed-param FULL descriptor lock (Codex msg 7d57e347 NON-GO). The param facts must
    # now exact-lock the WHOLE descriptor — numeric mapping (min/max/default/step), name/unit/
    # smoothing/persistence, status, evidence.line, and the six-field fieldEvidence — so a placeholder
    # range/default/unit can never silently flip into a confirmed "hardware fact", and the fact
    # key-set itself is exact (drop a field / add garbage fails).

    # (br) numeric-range drift: a placeholder max must not silently broaden into a hardware range.
    br_bad = copy.deepcopy(spec)
    reg_param(br_bad, "lfo_a.rate")["max"] = 20000
    problems, _ = gate.check(br_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed param max drift (lfo_a.rate 20 -> 20000) not enforced: %r" % problems)

    # (bs) evidence-status drift: a placeholder range must not be silently elevated to confirmed.
    bs_bad = copy.deepcopy(spec)
    reg_param(bs_bad, "lfo_a.rate")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(bs_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed param evidence-status drift (range unverified->confirmed) not "
                        "enforced: %r" % problems)

    # (bt) unit drift: a placeholder unit must not silently change.
    bt_bad = copy.deepcopy(spec)
    reg_param(bt_bad, "lfo_b.rate")["unit"] = "bpm"
    problems, _ = gate.check(bt_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed param unit drift (lfo_b.rate hz -> bpm) not enforced: %r" % problems)

    # (bu) default drift: a placeholder default must not silently change.
    bu_bad = copy.deepcopy(spec)
    reg_param(bu_bad, "lfo_a.wave")["default"] = 0
    problems, _ = gate.check(bu_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("landed param default drift (lfo_a.wave 0.5 -> 0) not enforced: %r"
                        % problems)

    # (bv) a concrete selector's software mapping may be provisional (Codex msg 2ec93491 accepts) but
    #      must NOT be upgradeable to confirmed.
    bv_bad = copy.deepcopy(spec)
    reg_param(bv_bad, "lfo_a.speed_mult")["fieldEvidence"]["default"] = "confirmed"
    problems, _ = gate.check(bv_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("selector fieldEvidence.default provisional->confirmed not enforced: "
                        "%r" % problems)

    # (bw) fact key-set: dropping a field from a landed param fact must FAIL (exact key-set).
    bw_bad = copy.deepcopy(manifest)
    del bw_bad["target"]["landedDescriptorFacts"]["parameters"]["lfo_a.rate"]["min"]
    problems, _ = gate.check(spec, bw_bad)
    if not has(problems, "key-set"):
        raise SystemExit("landed param fact field drop (lfo_a.rate min) not enforced: %r" % problems)

    # (bx) fact key-set: adding a garbage/target-foreign field to a landed param fact must FAIL.
    #      `positions` is a canonical target.parameters concern, NOT part of a landed descriptor fact.
    bx_bad = copy.deepcopy(manifest)
    bx_bad["target"]["landedDescriptorFacts"]["parameters"]["lfo_a.wave"]["positions"] = ["x1"]
    problems, _ = gate.check(spec, bx_bad)
    if not has(problems, "key-set"):
        raise SystemExit("landed param fact garbage key (lfo_a.wave positions) not enforced: "
                        "%r" % problems)

    print("OK: completeness gate rejects each defect for its intended reason; baseline passes; "
          "--require-full is per-ID (gap + rogue), not a fake per-module green; the four-entity "
          "split, independent region subtotals, explicit parameter shape + cardinality/recordType/"
          "maskSize coupling, binding operation→action semantics, structured closed-set context, "
          "joystick-axis targeting, ALWAYS-ON no-new-rogue, capability-declared present-but-empty "
          "(parameters/patchableJacks/internalEndpoints), empty-region declared-actual, binding "
          "invariants, impl⊆target, fixed-chain freeze, ORCHE provisional, terminal-owner and "
          "dangling-fixed-endpoint are all gated; dispatch closure (menuItem-selector + whole-vector "
          "ban), exact commandAddress/record-schema key-sets, order-preserving unique fields, the "
          "removed record* condition keys, and the PRESETS two-page state machine (closed page "
          "selector, display-item menuItem, enter-carries-slot, independent topology, both-pages-"
          "present, and the EXACT closed relation set — single-edge deletion, enter slot/item "
          "misalignment, execute target/item misalignment, and a workflow action rebound outside "
          "a PRESETS page all fail) are gated too; Root A (non-scalar target faked as a scalar "
          "descriptor), Root B (selector-toggle cardinality / malformed manifest positions), Root C "
          "(missing or invalid fieldEvidence), and Codex 03848819 Root 2 (positions on a non-selector, "
          "delete-positions-to-pass, mismatched selector labels, unverified selector range) are gated "
          "too (Codex 22f545c1/03848819).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
