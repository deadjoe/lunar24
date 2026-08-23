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
import tempfile

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

    # -- frozen classic-drone CV-input dedup (Codex msg 23f32c19) ----------------
    # The panel's single CV jack (panel label "CV", manual L56) is the SAME physical input the
    # manual prose calls "the input of CV MOD" (L297-300). The target must transcribe it ONCE,
    # as the canonical <owner>.cv_mod_in — the old <owner>.cv_in is a banned duplicate. Pin the
    # endpoint total (111) and patchable count (64) and the canonical per-owner patchable set.
    _eps = manifest["target"]["paramsJackTargets"]["endpoints"]
    _patch = [e for e in _eps if e.get("patchable")]
    if len(_eps) != 111 or len(_patch) != 64:
        raise SystemExit("frozen endpoint counts drift: total=%d patchable=%d (want 111/64)"
                         % (len(_eps), len(_patch)))
    _CANON = {"cv_mod_in", "gate_in", "env_out"}
    for _n in (1, 2, 4, 5):
        _owner = "drone_%d" % _n
        _have = {e["stable_id"].rsplit(".", 1)[1] for e in _eps
                 if e["owner"] == _owner and e.get("patchable")}
        if _have != _CANON:
            raise SystemExit("drone-%d canonical patchable set drift: %r" % (_n, sorted(_have)))

    # dedup-negative 1: a banned classic-drone cv_in alias must never reappear (always-on).
    alias = copy.deepcopy(manifest)
    for _n in (1, 2, 4, 5):
        alias["target"]["paramsJackTargets"]["endpoints"].append({
            "stable_id": "drone_%d.cv_in" % _n, "owner": "drone_%d" % _n,
            "kind": "endpoint", "direction": "input", "patchable": True,
            "status": "confirmed", "evidence": {"ref": "solar42N_manual_v15",
            "panelSite": "left-top drone classic", "lineStart": 56}})
    problems, _ = gate.check(spec, alias)
    if not has(problems, "classic drone CV-input alias must not reappear"):
        raise SystemExit("classic-drone cv_in alias reappearance not flagged: %r" % problems)

    # dedup-negative 2: a classic drone whose canonical patchable set drifts fails (always-on).
    drift = copy.deepcopy(manifest)
    for e in drift["target"]["paramsJackTargets"]["endpoints"]:
        if e["stable_id"] == "drone_1.cv_mod_in":
            e["patchable"] = False  # canonical cv_mod_in silently demoted -> patchable set shrinks
    problems, _ = gate.check(spec, drift)
    if not has(problems, "classic drone drone_1 patchable set"):
        raise SystemExit("classic-drone canonical patchable set drift not flagged: %r" % problems)

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
        parm["id"] = 500 + i  # far above the landed param id-space (now 200 = effector.select_r)
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

    # 31. mustComplete non-regression: a manifest target not in the registry. All 21 modules have
    #     now landed (through drone_6, Codex msg b121ba1b), so there is no unimplemented module left
    #     to use as the sentinel. Repoint to a still-unimplemented target PARAMETER (the keyboard
    #     complex is a known Phase-B gap) so a mustComplete key for an unimplemented target is still a
    #     regression drop.
    regress = copy.deepcopy(manifest)
    regress["mustComplete"].append("parameter:keyboard.seq_steps")  # manifest target, NOT in registry
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

    # (by)-(bz) NESTED key-set (Codex msg a42e4791): the exact-shape gate must also cover the two
    # nested objects. The value tuple already catches a MISSING nested key, but an EXTRA/garbage
    # nested key is invisible to it (`.get` ignores unknowns), so only an explicit nested key-set
    # check can fail it. descriptorEvidence must be exactly {line}; fieldEvidence exactly the six
    # keys {range,unit,default,step,smoothing,persistence}.

    # (by) descriptorEvidence nested garbage key.
    by_bad = copy.deepcopy(manifest)
    by_bad["target"]["landedDescriptorFacts"]["parameters"]["lfo_a.rate"]["descriptorEvidence"] \
        ["garbage"] = 1
    problems, _ = gate.check(spec, by_bad)
    if not has(problems, "descriptorEvidence key-set"):
        raise SystemExit("descriptorEvidence nested garbage key (lfo_a.rate) not enforced: "
                        "%r" % problems)

    # (bz) fieldEvidence nested garbage key.
    bz_bad = copy.deepcopy(manifest)
    bz_bad["target"]["landedDescriptorFacts"]["parameters"]["lfo_a.rate"]["fieldEvidence"]["garbage"] \
        = "confirmed"
    problems, _ = gate.check(spec, bz_bad)
    if not has(problems, "fieldEvidence key-set"):
        raise SystemExit("fieldEvidence nested garbage key (lfo_a.rate) not enforced: %r" % problems)

    # (ca)-(ci) joystick slice (Codex msg add70a95): the newly-landed joystick module and its 4
    # continuous params (x/y/offset_x/offset_y, software-normalized 0..1 placeholders) + 2 bipolar
    # CV outputs (x_out/y_out, nominal -10..+10V) must fall under the SAME landed-module /
    # landed-param / landed-jack gates. Codex scope is explicit: "补删除/renumber，以及参数 fieldEvidence
    # 或输出 range/evidence 漂移负例" — so cover deletion, id renumber, param fieldEvidence drift, and
    # output range/evidence drift. (cj)/(ck) add evidence-line fallback: x/y must cite L450 and
    # offset_x/offset_y L452, not the module header L449 (Codex 12bc53ba). joystick carries no
    # normalized route, so no dangling-endpoint strip.

    # (ca) renumber a landed joystick module id.
    ca_bad = copy.deepcopy(spec)
    for m in ca_bad["modules"]:
        if m.get("stable_id") == "joystick":
            m["id"] = 99
    problems, _ = gate.check(ca_bad, manifest)
    if not has(problems, "id 99 !="):
        raise SystemExit("joystick module id renumber (joystick 8 -> 99) not enforced: %r" % problems)

    # (cb) delete the joystick module fact (module still landed).
    cb_bad = copy.deepcopy(manifest)
    del cb_bad["target"]["landedModuleFacts"]["modules"]["joystick"]
    problems, _ = gate.check(spec, cb_bad)
    if not has(problems, "absent from target.landedModuleFacts"):
        raise SystemExit("joystick module-fact delete (fact dropped, module still present) not "
                        "enforced: %r" % problems)

    # (cc) delete the whole joystick module -> a fact naming an absent module must FAIL. No route to
    #      strip (joystick owns no normalized route).
    cc_bad = copy.deepcopy(spec)
    cc_bad["modules"] = [m for m in cc_bad["modules"] if m.get("stable_id") != "joystick"]
    problems, _ = gate.check(cc_bad, manifest)
    if not has(problems, "MISSING landed module 'joystick'"):
        raise SystemExit("joystick module delete not enforced: %r" % problems)

    # (cd) renumber a landed joystick param id.
    cd_bad = copy.deepcopy(spec)
    reg_param(cd_bad, "joystick.x")["id"] = 999
    problems, _ = gate.check(cd_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("joystick param id renumber (joystick.x 153 -> 999) not enforced: %r" % problems)

    # (ce) delete a landed joystick param.
    ce_bad = copy.deepcopy(spec)
    for m in ce_bad["modules"]:
        if m.get("stable_id") == "joystick":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "joystick.x"]
    problems, _ = gate.check(ce_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("joystick param delete (joystick.x) not enforced: %r" % problems)

    # (cf) delete a landed joystick jack.
    cf_bad = copy.deepcopy(spec)
    for m in cf_bad["modules"]:
        if m.get("stable_id") == "joystick":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "joystick.x_out"]
    problems, _ = gate.check(cf_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack"):
        raise SystemExit("joystick jack delete (joystick.x_out) not enforced: %r" % problems)

    # (cg) param fieldEvidence drift: a software-normalized placeholder range must not be elevated to
    #      confirmed (it stays unverified until a real hardware range is sourced).
    cg_bad = copy.deepcopy(spec)
    reg_param(cg_bad, "joystick.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(cg_bad, manifest)
    if not has(problems, "implementation param 'joystick.x'"):
        raise SystemExit("joystick param fieldEvidence drift (range unverified->confirmed) not "
                        "enforced: %r" % problems)

    # (ch) output range drift: the bipolar -10..+10V nominal range must not silently narrow.
    ch_bad = copy.deepcopy(spec)
    reg_jack(ch_bad, "joystick.x_out")["nominalMin"] = 0
    problems, _ = gate.check(ch_bad, manifest)
    if not has(problems, "implementation jack 'joystick.x_out'"):
        raise SystemExit("joystick output range drift (x_out nominalMin -10 -> 0) not enforced: "
                        "%r" % problems)

    # (ci) output evidence drift: the x_out nominal-range citation line must not move.
    ci_bad = copy.deepcopy(spec)
    reg_jack(ci_bad, "joystick.y_out")["evidence"]["line"] = 999
    problems, _ = gate.check(ci_bad, manifest)
    if not has(problems, "descriptorEvidence.line=999"):
        raise SystemExit("joystick output evidence drift (y_out line) not enforced: %r" % problems)

    # (cj) axis evidence fallback: joystick.x must cite L450 ("position by X and Y"), not the module
    #      header L449. Rolling the axis citation back to the module title must FAIL (Codex 12bc53ba).
    cj_bad = copy.deepcopy(spec)
    reg_param(cj_bad, "joystick.x")["evidence"]["line"] = 449
    problems, _ = gate.check(cj_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("joystick axis evidence fallback (joystick.x line 450 -> 449) not enforced: "
                        "%r" % problems)

    # (ck) offset evidence fallback: joystick.offset_x must cite L452 ("two voltage offset regulators"),
    #      not the module header L449. Rolling the offset citation back to the module title must FAIL.
    ck_bad = copy.deepcopy(spec)
    reg_param(ck_bad, "joystick.offset_x")["evidence"]["line"] = 449
    problems, _ = gate.check(ck_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("joystick offset evidence fallback (joystick.offset_x line 452 -> 449) not "
                        "enforced: %r" % problems)

    # (cl)-(cu) preamp + envelope follower slice (Codex msg ebd65910): the two newly-landed modules
    # and their 3 software-normalized params (preamp.gain, env_follower.attack/release) + 3 jacks
    # (preamp.ext_source_in audio input, env_follower.env_out 0..+10V CV, env_follower.gate_out 0..+8V
    # gate) fall under the SAME landed-module / landed-param / landed-jack gates. Cover per-kind
    # deletion + id renumber + the key evidence/range drift a reviewer cares about: the module id,
    # the audio input signalType, the gate output / envelope output nominal range, the placeholder
    # gain fieldEvidence, and the envelope/gate evidence line. No normalized route is owned by either
    # module, so no dangling-endpoint strip is needed.

    # (cl) renumber a landed preamp module id.
    cl_bad = copy.deepcopy(spec)
    for m in cl_bad["modules"]:
        if m.get("stable_id") == "preamp":
            m["id"] = 99
    problems, _ = gate.check(cl_bad, manifest)
    if not has(problems, "id 99 !="):
        raise SystemExit("preamp module id renumber (preamp 9 -> 99) not enforced: %r" % problems)

    # (cm) delete the preamp module fact (module still landed).
    cm_bad = copy.deepcopy(manifest)
    del cm_bad["target"]["landedModuleFacts"]["modules"]["preamp"]
    problems, _ = gate.check(spec, cm_bad)
    if not has(problems, "absent from target.landedModuleFacts"):
        raise SystemExit("preamp module-fact delete (fact dropped, module still present) not "
                        "enforced: %r" % problems)

    # (cn) delete the whole env_follower module -> a fact naming an absent module must FAIL. env_follower
    #      owns no normalized route, so nothing else needs stripping.
    cn_bad = copy.deepcopy(spec)
    cn_bad["modules"] = [m for m in cn_bad["modules"] if m.get("stable_id") != "env_follower"]
    problems, _ = gate.check(cn_bad, manifest)
    if not has(problems, "MISSING landed module 'env_follower'"):
        raise SystemExit("env_follower module delete not enforced: %r" % problems)

    # (co) renumber a landed env_follower param id.
    co_bad = copy.deepcopy(spec)
    reg_param(co_bad, "env_follower.attack")["id"] = 999
    problems, _ = gate.check(co_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("env_follower param id renumber (env_follower.attack 158 -> 999) not "
                        "enforced: %r" % problems)

    # (cp) delete a landed env_follower param.
    cp_bad = copy.deepcopy(spec)
    for m in cp_bad["modules"]:
        if m.get("stable_id") == "env_follower":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "env_follower.release"]
    problems, _ = gate.check(cp_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("env_follower param delete (env_follower.release) not enforced: %r" % problems)

    # (cq) delete a landed env_follower jack.
    cq_bad = copy.deepcopy(spec)
    for m in cq_bad["modules"]:
        if m.get("stable_id") == "env_follower":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "env_follower.env_out"]
    problems, _ = gate.check(cq_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack"):
        raise SystemExit("env_follower jack delete (env_follower.env_out) not enforced: %r" % problems)

    # (cr) output range drift: the env_follower.env_out 0..+10V CV nominal range must not widen.
    cr_bad = copy.deepcopy(spec)
    reg_jack(cr_bad, "env_follower.env_out")["nominalMax"] = 11
    problems, _ = gate.check(cr_bad, manifest)
    if not has(problems, "implementation jack 'env_follower.env_out'"):
        raise SystemExit("env_follower output range drift (env_out nominalMax 10 -> 11) not enforced: "
                        "%r" % problems)

    # (cs) param fieldEvidence drift: a software-normalized placeholder range must not be elevated to
    #      confirmed (it stays unverified until a real hardware gain range is sourced).
    cs_bad = copy.deepcopy(spec)
    reg_param(cs_bad, "preamp.gain")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(cs_bad, manifest)
    if not has(problems, "implementation param 'preamp.gain'"):
        raise SystemExit("preamp.gain fieldEvidence drift (range unverified->confirmed) not "
                        "enforced: %r" % problems)

    # (ct) output evidence drift: the env_follower.gate_out 0..+8V citation line must not move.
    ct_bad = copy.deepcopy(spec)
    reg_jack(ct_bad, "env_follower.gate_out")["evidence"]["line"] = 999
    problems, _ = gate.check(ct_bad, manifest)
    if not has(problems, "descriptorEvidence.line=999"):
        raise SystemExit("env_follower output evidence drift (gate_out line) not enforced: "
                        "%r" % problems)

    # (cu) audio-input signalType drift: preamp.ext_source_in is a confirmed AUDIO input; silently
    #      reclassifying it as CV must FAIL.
    cu_bad = copy.deepcopy(spec)
    reg_jack(cu_bad, "preamp.ext_source_in")["signalType"] = "cv"
    problems, _ = gate.check(cu_bad, manifest)
    if not has(problems, "implementation jack 'preamp.ext_source_in'"):
        raise SystemExit("preamp audio input signalType drift (ext_source_in audio -> cv) not "
                        "enforced: %r" % problems)

    # (cv)-(df) 5-step sequencer slice (Codex msg 6bedef35): the newly-landed sequencer module, its
    # 13 params (pulser/clock/stages + step_cv_1..5 + step_gate_1..5) and 4 jacks
    # (ext_clock_in/clock_out/cv_out/gate_out) sit under the same landed-module/param/jack +
    # descriptor-coherence gates. Codex explicitly calls out the stages correction: the canonical
    # positions are (3,4,5), NOT (1,2,3,4,5) — L499 says "the maximum number of steps (3, 4, or 5)".
    # Cover the target correction, per-kind deletion/renumber, param mapping/evidence drift, and
    # jack range/evidence drift a reviewer cares about.

    # (cv) regression: reverting the frozen-target stages positions to the OLD (1,2,3,4,5) must FAIL
    #      (the registry carries the corrected (3,4,5) set).
    cv_bad = copy.deepcopy(manifest)
    cv_bad["target"]["parameters"] = [
        {**p, "positions": ["1", "2", "3", "4", "5"]} if p.get("stable_id") == "sequencer.stages"
        else p for p in cv_bad["target"]["parameters"]]
    problems, _ = gate.check(spec, cv_bad)
    if not has(problems, "target positions"):
        raise SystemExit("sequencer.stages target positions reverted to (1,2,3,4,5) not rejected "
                        "(canonical is (3,4,5)): %r" % problems)

    # (cw) delete a landed sequencer module fact (module still present).
    cw_bad = copy.deepcopy(manifest)
    del cw_bad["target"]["landedModuleFacts"]["modules"]["sequencer"]
    problems, _ = gate.check(spec, cw_bad)
    if not has(problems, "absent from target.landedModuleFacts"):
        raise SystemExit("sequencer module-fact delete (fact dropped, module still present) not "
                        "enforced: %r" % problems)

    # (cx) delete the whole sequencer module -> a fact naming an absent module must FAIL.
    cx_bad = copy.deepcopy(spec)
    cx_bad["modules"] = [m for m in cx_bad["modules"] if m.get("stable_id") != "sequencer"]
    problems, _ = gate.check(cx_bad, manifest)
    if not has(problems, "MISSING landed module 'sequencer'"):
        raise SystemExit("sequencer module delete not enforced: %r" % problems)

    # (cy) renumber a landed sequencer param id (stages 162 -> 999).
    cy_bad = copy.deepcopy(spec)
    reg_param(cy_bad, "sequencer.stages")["id"] = 999
    problems, _ = gate.check(cy_bad, manifest)
    if not has(problems, "landed descriptor"):
        raise SystemExit("sequencer param id renumber (sequencer.stages 162 -> 999) not enforced: "
                        "%r" % problems)

    # (cz) delete a landed sequencer param (seq_gate carries conditional items; strip step_cv_1).
    cz_bad = copy.deepcopy(spec)
    for m in cz_bad["modules"]:
        if m.get("stable_id") == "sequencer":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "sequencer.step_cv_1"]
    problems, _ = gate.check(cz_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("sequencer param delete (sequencer.step_cv_1) not enforced: %r" % problems)

    # (da) delete a landed sequencer jack (cv_out).
    da_bad = copy.deepcopy(spec)
    for m in da_bad["modules"]:
        if m.get("stable_id") == "sequencer":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "sequencer.cv_out"]
    problems, _ = gate.check(da_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack"):
        raise SystemExit("sequencer jack delete (sequencer.cv_out) not enforced: %r" % problems)

    # (db) param fieldEvidence drift: a confirmed step_cv 0..5V range must not be silently downgraded
    #      to unverified, nor must its unit drop to a placeholder.
    db_bad = copy.deepcopy(spec)
    reg_param(db_bad, "sequencer.step_cv_1")["fieldEvidence"]["range"] = "unverified"
    problems, _ = gate.check(db_bad, manifest)
    if not has(problems, "implementation param 'sequencer.step_cv_1'"):
        raise SystemExit("sequencer.step_cv_1 fieldEvidence drift (range confirmed->unverified) not "
                        "enforced: %r" % problems)

    # (dc) param descriptorEvidence line drift: the pulser citation line must not move (L500).
    dc_bad = copy.deepcopy(spec)
    reg_param(dc_bad, "sequencer.pulser")["evidence"]["line"] = 501
    problems, _ = gate.check(dc_bad, manifest)
    if not has(problems, "implementation param 'sequencer.pulser'"):
        raise SystemExit("sequencer.pulser descriptorEvidence.line drift (500 -> 501) not enforced: "
                        "%r" % problems)

    # (dd) jack range drift: the seq cv_out 0..+5V CV nominal range must not widen.
    dd_bad = copy.deepcopy(spec)
    reg_jack(dd_bad, "sequencer.cv_out")["nominalMax"] = 6
    problems, _ = gate.check(dd_bad, manifest)
    if not has(problems, "implementation jack 'sequencer.cv_out'"):
        raise SystemExit("sequencer output range drift (cv_out nominalMax 5 -> 6) not enforced: "
                        "%r" % problems)

    # (de) jack range drift: the seq gate_out 0..+10V gate nominal range must not widen.
    de_bad = copy.deepcopy(spec)
    reg_jack(de_bad, "sequencer.gate_out")["nominalMax"] = 11
    problems, _ = gate.check(de_bad, manifest)
    if not has(problems, "implementation jack 'sequencer.gate_out'"):
        raise SystemExit("sequencer output range drift (gate_out nominalMax 10 -> 11) not enforced: "
                        "%r" % problems)

    # (df) clock-jack signalType drift: sequencer.ext_clock_in is a confirmed CLOCK input; silently
    #      reclassifying it as CV must FAIL.
    df_bad = copy.deepcopy(spec)
    reg_jack(df_bad, "sequencer.ext_clock_in")["signalType"] = "cv"
    problems, _ = gate.check(df_bad, manifest)
    if not has(problems, "implementation jack 'sequencer.ext_clock_in'"):
        raise SystemExit("sequencer clock input signalType drift (ext_clock_in clock -> cv) not "
                        "enforced: %r" % problems)

    # (dg) delete a MIDDLE mixer channel (ch5 = VCO A vol, id 182).
    dg_bad = copy.deepcopy(spec)
    for m in dg_bad["modules"]:
        if m.get("stable_id") == "mixer":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "mixer.ch5_vol"]
    problems, _ = gate.check(dg_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("mixer middle-channel delete (mixer.ch5_vol) not enforced: %r" % problems)

    # (dh) delete an END mixer channel (ch10 = DRONE 6 vol, id 192, last in the chain L1112).
    dh_bad = copy.deepcopy(spec)
    for m in dh_bad["modules"]:
        if m.get("stable_id") == "mixer":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "mixer.ch10_vol"]
    problems, _ = gate.check(dh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("mixer end-channel delete (mixer.ch10_vol) not enforced: %r" % problems)

    # (di) delete the FIRST mixer channel (ch1 = DRONE 1 pan, id 173, head of the chain L1112).
    di_bad = copy.deepcopy(spec)
    for m in di_bad["modules"]:
        if m.get("stable_id") == "mixer":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "mixer.ch1_pan"]
    problems, _ = gate.check(di_bad, manifest)
    if not has(problems, "MISSING landed descriptor param"):
        raise SystemExit("mixer head-channel delete (mixer.ch1_pan) not enforced: %r" % problems)

    # (dj) renumber a landed mixer param id (mixer.ch1_pan 173 -> 999).
    dj_bad = copy.deepcopy(spec)
    reg_param(dj_bad, "mixer.ch1_pan")["id"] = 999
    problems, _ = gate.check(dj_bad, manifest)
    if not has(problems, "implementation param 'mixer.ch1_pan'"):
        raise SystemExit("mixer param id renumber (mixer.ch1_pan 173 -> 999) not enforced: %r"
                        % problems)

    # (dk) PAN descriptorEvidence line drift: the PAN pot citation (L1105) must not move.
    dk_bad = copy.deepcopy(spec)
    reg_param(dk_bad, "mixer.ch1_pan")["evidence"]["line"] = 1106
    problems, _ = gate.check(dk_bad, manifest)
    if not has(problems, "implementation param 'mixer.ch1_pan'"):
        raise SystemExit("mixer.ch1_pan PAN descriptorEvidence.line drift (1105 -> 1106) not "
                        "enforced: %r" % problems)

    # (dl) VOL descriptorEvidence line drift: the VOL pot citation (L1113) must not move.
    dl_bad = copy.deepcopy(spec)
    reg_param(dl_bad, "mixer.ch10_vol")["evidence"]["line"] = 1114
    problems, _ = gate.check(dl_bad, manifest)
    if not has(problems, "implementation param 'mixer.ch10_vol'"):
        raise SystemExit("mixer.ch10_vol VOL descriptorEvidence.line drift (1113 -> 1114) not "
                        "enforced: %r" % problems)

    # (dm) fieldEvidence provenance drift: a PAN is an all-unverified normalized placeholder; silently
    #      flipping range to confirmed is an evidence over-claim that must FAIL.
    dm_bad = copy.deepcopy(spec)
    reg_param(dm_bad, "mixer.ch5_pan")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(dm_bad, manifest)
    if not has(problems, "implementation param 'mixer.ch5_pan'"):
        raise SystemExit("mixer.ch5_pan fieldEvidence provenance drift (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (dn) channel-mapping drift: mixer.ch5 is VCO A in the L1112 identity chain (name "PAN VCO A");
    #      silently rebinding that channel to VCO B (name), not just renumbering, must FAIL.
    dn_bad = copy.deepcopy(spec)
    reg_param(dn_bad, "mixer.ch5_pan")["name"] = "PAN VCO B"
    problems, _ = gate.check(dn_bad, manifest)
    if not has(problems, "implementation param 'mixer.ch5_pan'"):
        raise SystemExit("mixer.ch5_pan chain-mapping drift (name 'PAN VCO A' -> 'PAN VCO B') not "
                        "enforced: %r" % problems)

    # (do) effector module delete: pulling the whole landed module (id 13) out of the registry must
    #      fail as a MISSING landed module (cascading to its param/jack facts).
    do_bad = copy.deepcopy(spec)
    do_bad["modules"] = [m for m in do_bad["modules"] if m.get("stable_id") != "effector"]
    problems, _ = gate.check(do_bad, manifest)
    if not has(problems, "MISSING landed module 'effector'"):
        raise SystemExit("effector module delete not enforced: %r" % problems)

    # (dp) effector module id renumber: effector is a landed module with id 13; renumbering it
    #      (13 -> 999) is a registry-id vs landed-fact-id mismatch and must fail.
    dp_bad = copy.deepcopy(spec)
    for m in dp_bad["modules"]:
        if m.get("stable_id") == "effector":
            m["id"] = 999
    problems, _ = gate.check(dp_bad, manifest)
    if not has(problems, "implementation module 'effector': id"):
        raise SystemExit("effector module id renumber (13 -> 999) not enforced: %r" % problems)

    # (dq) effector param delete: a landed X knob (effector.x, id 193) must not silently reopen as a
    #      gap.
    dq_bad = copy.deepcopy(spec)
    for m in dq_bad["modules"]:
        if m.get("stable_id") == "effector":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "effector.x"]
    problems, _ = gate.check(dq_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'effector.x'"):
        raise SystemExit("effector param delete (effector.x) not enforced: %r" % problems)

    # (dr) effector param id renumber: effector.x id 193 -> 999 must fail.
    dr_bad = copy.deepcopy(spec)
    reg_param(dr_bad, "effector.x")["id"] = 999
    problems, _ = gate.check(dr_bad, manifest)
    if not has(problems, "implementation param 'effector.x'"):
        raise SystemExit("effector param id renumber (effector.x 193 -> 999) not enforced: %r"
                        % problems)

    # (ds) effector jack delete: a landed patchable CV input (effector.cv_x_in, id 40) must not
    #      silently reopen as a gap.
    ds_bad = copy.deepcopy(spec)
    for m in ds_bad["modules"]:
        if m.get("stable_id") == "effector":
            m["jacks"] = [j for j in m.get("jacks", [])
                          if j.get("stable_id") != "effector.cv_x_in"]
    problems, _ = gate.check(ds_bad, manifest)
    if not has(problems, "MISSING landed descriptor jack 'effector.cv_x_in'"):
        raise SystemExit("effector jack delete (effector.cv_x_in) not enforced: %r" % problems)

    # (dt) effector jack id renumber: effector.cv_x_in id 40 -> 999 must fail.
    dt_bad = copy.deepcopy(spec)
    reg_jack(dt_bad, "effector.cv_x_in")["id"] = 999
    problems, _ = gate.check(dt_bad, manifest)
    if not has(problems, "implementation jack 'effector.cv_x_in'"):
        raise SystemExit("effector jack id renumber (effector.cv_x_in 40 -> 999) not enforced: %r"
                        % problems)

    # (du) selector positions drift: effector SELECT L is an opaque 1-2-3 selector; UI/MIDI must
    #      never see a reordered value domain. Changing the registry option order must fail.
    du_bad = copy.deepcopy(spec)
    reg_param(du_bad, "effector.select_l")["positions"] = ["2", "1", "3"]
    problems, _ = gate.check(du_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("effector.select_l selector positions drift (['1','2','3'] -> "
                        "['2','1','3']) not enforced: %r" % problems)

    # (dv) X/Y/Z descriptorEvidence line drift: the shared X/Y/Z glyph-row citation (L1183) must not
    #      move.
    dv_bad = copy.deepcopy(spec)
    reg_param(dv_bad, "effector.x")["evidence"]["line"] = 1184
    problems, _ = gate.check(dv_bad, manifest)
    if not has(problems, "implementation param 'effector.x'"):
        raise SystemExit("effector.x descriptorEvidence.line drift (1183 -> 1184) not enforced: %r"
                        % problems)

    # (dw) BLEND descriptorEvidence line drift: the BLEND citation (L1162) must not move.
    dw_bad = copy.deepcopy(spec)
    reg_param(dw_bad, "effector.blend")["evidence"]["line"] = 1163
    problems, _ = gate.check(dw_bad, manifest)
    if not has(problems, "implementation param 'effector.blend'"):
        raise SystemExit("effector.blend descriptorEvidence.line drift (1162 -> 1163) not "
                        "enforced: %r" % problems)

    # (dx) MASTER descriptorEvidence line drift: the MASTER citation (L1163) must not move.
    dx_bad = copy.deepcopy(spec)
    reg_param(dx_bad, "effector.master")["evidence"]["line"] = 1164
    problems, _ = gate.check(dx_bad, manifest)
    if not has(problems, "implementation param 'effector.master'"):
        raise SystemExit("effector.master descriptorEvidence.line drift (1163 -> 1164) not "
                        "enforced: %r" % problems)

    # (dy) PHONE descriptorEvidence line drift: the PHONE citation (L1153) must not move.
    dy_bad = copy.deepcopy(spec)
    reg_param(dy_bad, "effector.phone")["evidence"]["line"] = 1154
    problems, _ = gate.check(dy_bad, manifest)
    if not has(problems, "implementation param 'effector.phone'"):
        raise SystemExit("effector.phone descriptorEvidence.line drift (1153 -> 1154) not "
                        "enforced: %r" % problems)

    # (dz) CV input nominal range drift: effector.cv_x_in is a confirmed -10..+10V bipolar input;
    #      widening its nominalMax must fail.
    dz_bad = copy.deepcopy(spec)
    reg_jack(dz_bad, "effector.cv_x_in")["nominalMax"] = 11
    problems, _ = gate.check(dz_bad, manifest)
    if not has(problems, "implementation jack 'effector.cv_x_in'"):
        raise SystemExit("effector.cv_x_in nominal range drift (10 -> 11) not enforced: %r"
                        % problems)

    # (ea) CV input polarity drift: effector.cv_x_in is a confirmed bipolar CV input; silently
    #      reclassifying it as unipolar must fail.
    ea_bad = copy.deepcopy(spec)
    reg_jack(ea_bad, "effector.cv_x_in")["polarity"] = "unipolar"
    problems, _ = gate.check(ea_bad, manifest)
    if not has(problems, "implementation jack 'effector.cv_x_in'"):
        raise SystemExit("effector.cv_x_in polarity drift (bipolar -> unipolar) not enforced: %r"
                        % problems)

    # (eb) CV input fieldEvidence drift: effector.cv_x_in carries a confirmed -10..+10V nominalRange;
    #      silently downgrading it to unverified is an evidence under-claim that must fail.
    eb_bad = copy.deepcopy(spec)
    reg_jack(eb_bad, "effector.cv_x_in")["fieldEvidence"]["nominalRange"] = "unverified"
    problems, _ = gate.check(eb_bad, manifest)
    if not has(problems, "implementation jack 'effector.cv_x_in'"):
        raise SystemExit("effector.cv_x_in fieldEvidence drift (nominalRange confirmed->unverified) "
                        "not enforced: %r" % problems)

    # (ec) voices module delete: pulling the whole landed module (id 14) out of the registry must
    #      fail as a MISSING landed module (its 6 internal gate-out endpoints stay non-landed).
    ec_bad = copy.deepcopy(spec)
    ec_bad["modules"] = [m for m in ec_bad["modules"] if m.get("stable_id") != "voices"]
    problems, _ = gate.check(ec_bad, manifest)
    if not has(problems, "MISSING landed module 'voices'"):
        raise SystemExit("voices module delete not enforced: %r" % problems)

    # (ed) voices module id renumber: voices is a landed module with id 14; renumbering it
    #      (14 -> 999) is a registry-id vs landed-fact-id mismatch and must fail.
    ed_bad = copy.deepcopy(spec)
    for m in ed_bad["modules"]:
        if m.get("stable_id") == "voices":
            m["id"] = 999
    problems, _ = gate.check(ed_bad, manifest)
    if not has(problems, "implementation module 'voices': id"):
        raise SystemExit("voices module id renumber (14 -> 999) not enforced: %r" % problems)

    # (ee) voices module target name drift: the registry module name stays in lock-step with
    #      target.modules ('Drone voices 1-6'); a silent rename is a target drift.
    ee_bad = copy.deepcopy(spec)
    for m in ee_bad["modules"]:
        if m.get("stable_id") == "voices":
            m["name"] = "Drone voices"
    problems, _ = gate.check(ee_bad, manifest)
    if not has(problems, "implementation module 'voices': name"):
        raise SystemExit("voices module name drift ('Drone voices 1-6' -> 'Drone voices') not "
                        "enforced: %r" % problems)

    # (ef) voices module evidence drift: the target evidence span (L106-118) is the authority;
    #      moving lineStart is a target-evidence drift and must fail.
    ef_bad = copy.deepcopy(spec)
    for m in ef_bad["modules"]:
        if m.get("stable_id") == "voices":
            m["evidence"]["lineStart"] = 999
    problems, _ = gate.check(ef_bad, manifest)
    if not has(problems, "implementation module 'voices': evidence.lineStart"):
        raise SystemExit("voices module evidence.lineStart drift (106 -> 999) not enforced: %r"
                        % problems)

    # ---- DRONE 1 classic voice slice (Codex msg 28e00d92) --------------------
    # The DRONE 1 voice is 5 generators, each with its own TUNE / MUTE / MOD; a shared VOLT
    # transposes all five, ATT/RLS set attack/release, HOLD is a panel control, and three
    # patchable jacks (cv_mod_in / gate_in / env_out) expose the voice. Because the 5 generators are
    # positionally identical, the slice is wrong if the group cardinally drifts (first / middle / last
    # delete), if any id/name mapping renumbers or renames a member, if a MUTE/MOD selector loses its
    # value domain, if a VOLT/ATT/RLS/HOLD evidence line moves or its provenance over-claims, or if a
    # jack's range / type / polarity / evidence drifts. Each must FAIL normal for its own reason.

    # (eg) module delete: pulling the whole landed module (id 15) out of the registry must fail as a
    #      MISSING landed module (cascading to its param/jack facts).
    eg_bad = copy.deepcopy(spec)
    eg_bad["modules"] = [m for m in eg_bad["modules"] if m.get("stable_id") != "drone_1"]
    problems, _ = gate.check(eg_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_1'"):
        raise SystemExit("drone_1 module delete not enforced: %r" % problems)

    # (eh) module id renumber: drone_1 is a landed module with id 15; renumbering it (15 -> 999) is a
    #      registry-id vs landed-fact-id mismatch and must fail.
    eh_bad = copy.deepcopy(spec)
    for m in eh_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["id"] = 999
    problems, _ = gate.check(eh_bad, manifest)
    if not has(problems, "implementation module 'drone_1': id"):
        raise SystemExit("drone_1 module id renumber (15 -> 999) not enforced: %r" % problems)

    # (ei) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 1'); a silent rename is a target drift.
    ei_bad = copy.deepcopy(spec)
    for m in ei_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["name"] = "DRONE ONE"
    problems, _ = gate.check(ei_bad, manifest)
    if not has(problems, "implementation module 'drone_1': name"):
        raise SystemExit("drone_1 module name drift ('DRONE 1' -> 'DRONE ONE') not enforced: %r"
                        % problems)

    # (ej) module evidence drift: the module spans L288-320; moving lineStart is a target drift.
    ej_bad = copy.deepcopy(spec)
    for m in ej_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["evidence"]["lineStart"] = 289
    problems, _ = gate.check(ej_bad, manifest)
    if not has(problems, "implementation module 'drone_1': evidence.lineStart"):
        raise SystemExit("drone_1 module evidence.lineStart drift (288 -> 289) not enforced: %r"
                        % problems)

    # (ek) 5-generator cardinality - FIRST generator: deleting tune_1 pulls the head of the 5-way
    #      lattice out, which must not silently reopen as a gap.
    ek_bad = copy.deepcopy(spec)
    for m in ek_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_1.tune_1"]
    problems, _ = gate.check(ek_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_1.tune_1'"):
        raise SystemExit("drone_1 first-generator delete (drone_1.tune_1) not enforced: %r" % problems)

    # (el) 5-generator cardinality - MIDDLE generator: generator 3 is the midpoint; deleting tune_3
    #      must fail.
    el_bad = copy.deepcopy(spec)
    for m in el_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_1.tune_3"]
    problems, _ = gate.check(el_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_1.tune_3'"):
        raise SystemExit("drone_1 middle-generator delete (drone_1.tune_3) not enforced: %r" % problems)

    # (em) 5-generator cardinality - LAST generator: generator 5 is the tail; deleting tune_5 must fail.
    em_bad = copy.deepcopy(spec)
    for m in em_bad["modules"]:
        if m.get("stable_id") == "drone_1":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_1.tune_5"]
    problems, _ = gate.check(em_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_1.tune_5'"):
        raise SystemExit("drone_1 last-generator delete (drone_1.tune_5) not enforced: %r" % problems)

    # (en) ID mapping: renumber a landed generator param id (drone_1.tune_1 201 -> 999) must fail.
    en_bad = copy.deepcopy(spec)
    reg_param(en_bad, "drone_1.tune_1")["id"] = 999
    problems, _ = gate.check(en_bad, manifest)
    if not has(problems, "implementation param 'drone_1.tune_1'"):
        raise SystemExit("drone_1 param id renumber (tune_1 201 -> 999) not enforced: %r" % problems)

    # (eo) NAME mapping: a landed mute param must keep its exact label ('MUTE 1'); a silent rename is a
    #      target-drift.
    eo_bad = copy.deepcopy(spec)
    reg_param(eo_bad, "drone_1.mute_1")["name"] = "MUTE A"
    problems, _ = gate.check(eo_bad, manifest)
    if not has(problems, "implementation param 'drone_1.mute_1'"):
        raise SystemExit("drone_1 param name drift (mute_1 'MUTE 1' -> 'MUTE A') not enforced: %r"
                        % problems)

    # (ep) MUTE selector positions drift: mute_1 is an off/on toggle; reordering the domain is a UI/MIDI
    #      re-mapping and must fail.
    ep_bad = copy.deepcopy(spec)
    reg_param(ep_bad, "drone_1.mute_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(ep_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_1.mute_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (eq) MOD selector positions drift: mod_1 is the pitch-modulation toggle; its domain must not move.
    eq_bad = copy.deepcopy(spec)
    reg_param(eq_bad, "drone_1.mod_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(eq_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_1.mod_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (er) VOLT descriptorEvidence.line drift: the VOLT transposition knob cites L309; moving it must
    #      fail.
    er_bad = copy.deepcopy(spec)
    reg_param(er_bad, "drone_1.volt")["evidence"]["line"] = 310
    problems, _ = gate.check(er_bad, manifest)
    if not has(problems, "implementation param 'drone_1.volt'"):
        raise SystemExit("drone_1.volt descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (es) ATT descriptorEvidence.line drift: attack cites L309; moving it must fail.
    es_bad = copy.deepcopy(spec)
    reg_param(es_bad, "drone_1.att")["evidence"]["line"] = 310
    problems, _ = gate.check(es_bad, manifest)
    if not has(problems, "implementation param 'drone_1.att'"):
        raise SystemExit("drone_1.att descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (et) RLS descriptorEvidence.line drift: release cites L309; moving it must fail.
    et_bad = copy.deepcopy(spec)
    reg_param(et_bad, "drone_1.rls")["evidence"]["line"] = 310
    problems, _ = gate.check(et_bad, manifest)
    if not has(problems, "implementation param 'drone_1.rls'"):
        raise SystemExit("drone_1.rls descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (eu) HOLD descriptorEvidence.line drift: gate_hold cites the panel HOLD label (L53); moving it must
    #      fail.
    eu_bad = copy.deepcopy(spec)
    reg_param(eu_bad, "drone_1.gate_hold")["evidence"]["line"] = 54
    problems, _ = gate.check(eu_bad, manifest)
    if not has(problems, "implementation param 'drone_1.gate_hold'"):
        raise SystemExit("drone_1.gate_hold descriptorEvidence.line drift (53 -> 54) not enforced: %r"
                        % problems)

    # (ev) continuous placeholder provenance over-claim: VOLT is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    ev_bad = copy.deepcopy(spec)
    reg_param(ev_bad, "drone_1.volt")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(ev_bad, manifest)
    if not has(problems, "implementation param 'drone_1.volt'"):
        raise SystemExit("drone_1.volt fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (ew) cv_mod_in nominal range drift: the CV MOD input occupies the placeholder -5..+5 range; even
    #      though the evidence is unverified the gate freezes the value, so drifting nominalMax must fail.
    ew_bad = copy.deepcopy(spec)
    reg_jack(ew_bad, "drone_1.cv_mod_in")["nominalMax"] = 6
    problems, _ = gate.check(ew_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.cv_mod_in'"):
        raise SystemExit("drone_1.cv_mod_in nominal range drift (5 -> 6) not enforced: %r" % problems)

    # (ex) cv_mod_in polarity drift: cv_mod_in is a CV input whose polarity is unknown/unverified;
    #      silently asserting a concrete polarity is a type over-claim and must fail.
    ex_bad = copy.deepcopy(spec)
    reg_jack(ex_bad, "drone_1.cv_mod_in")["polarity"] = "unipolar"
    problems, _ = gate.check(ex_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.cv_mod_in'"):
        raise SystemExit("drone_1.cv_mod_in polarity drift (unknown -> unipolar) not enforced: %r"
                        % problems)

    # (ey) gate_in signalType drift: gate_in is a confirmed GATE input; silently reclassifying it as a
    #      CV input is a type drift and must fail.
    ey_bad = copy.deepcopy(spec)
    reg_jack(ey_bad, "drone_1.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(ey_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.gate_in'"):
        raise SystemExit("drone_1.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (ez) gate_in nominal range drift: the gate input placeholder is 0..+10; drifting nominalMax must
    #      fail.
    ez_bad = copy.deepcopy(spec)
    reg_jack(ez_bad, "drone_1.gate_in")["nominalMax"] = 9
    problems, _ = gate.check(ez_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.gate_in'"):
        raise SystemExit("drone_1.gate_in nominal range drift (10 -> 9) not enforced: %r" % problems)

    # (fa) env_out nominal range drift: env_out is the confirmed -10..+10V bipolar CV output (L157);
    #      widening nominalMax must fail.
    fa_bad = copy.deepcopy(spec)
    reg_jack(fa_bad, "drone_1.env_out")["nominalMax"] = 11
    problems, _ = gate.check(fa_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.env_out'"):
        raise SystemExit("drone_1.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (fb) env_out polarity drift: env_out is a confirmed bipolar CV output; silently reclassifying it
    #      as unipolar must fail.
    fb_bad = copy.deepcopy(spec)
    reg_jack(fb_bad, "drone_1.env_out")["polarity"] = "unipolar"
    problems, _ = gate.check(fb_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.env_out'"):
        raise SystemExit("drone_1.env_out polarity drift (bipolar -> unipolar) not enforced: %r"
                        % problems)

    # (fc) env_out descriptorEvidence.line drift: env_out cites the ENV VOICES output spec (L157);
    #      moving the citation must fail.
    fc_bad = copy.deepcopy(spec)
    reg_jack(fc_bad, "drone_1.env_out")["evidence"]["line"] = 158
    problems, _ = gate.check(fc_bad, manifest)
    if not has(problems, "implementation jack 'drone_1.env_out'"):
        raise SystemExit("drone_1.env_out descriptorEvidence.line drift (157 -> 158) not enforced: %r"
                        % problems)

    # ---- DRONE 2 classic voice slice (Codex msg 3bc2111c) --------------------
    # Same structural/evidence boundary as DRONE 1, with the corrected factual framing baked in from
    # the start (chords/triads, 5V keyboard-only, HOLD provisional). Wrong if the group cardinality
    # drifts (first / middle / last delete), if any id/name mapping renumbers or renames a member, if
    # a MUTE/MOD selector loses its value domain, if a VOLT/ATT/RLS/HOLD evidence line moves or its
    # provenance over-claims, if a jack's range / type / polarity / evidence drifts, or if a drone_2
    # member leaks a drone_1 stable_id (copy-paste from the DRONE 1 slice). Each must FAIL normal for
    # its own reason.

    # (fd) module delete: pulling the whole landed module (id 16) out of the registry must fail.
    fd_bad = copy.deepcopy(spec)
    fd_bad["modules"] = [m for m in fd_bad["modules"] if m.get("stable_id") != "drone_2"]
    problems, _ = gate.check(fd_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_2'"):
        raise SystemExit("drone_2 module delete not enforced: %r" % problems)

    # (fe) module id renumber: drone_2 is a landed module with id 16; renumbering (16 -> 999) must fail.
    fe_bad = copy.deepcopy(spec)
    for m in fe_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["id"] = 999
    problems, _ = gate.check(fe_bad, manifest)
    if not has(problems, "implementation module 'drone_2': id"):
        raise SystemExit("drone_2 module id renumber (16 -> 999) not enforced: %r" % problems)

    # (ff) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 2'); a silent rename is a target drift.
    ff_bad = copy.deepcopy(spec)
    for m in ff_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["name"] = "DRONE TWO"
    problems, _ = gate.check(ff_bad, manifest)
    if not has(problems, "implementation module 'drone_2': name"):
        raise SystemExit("drone_2 module name drift ('DRONE 2' -> 'DRONE TWO') not enforced: %r"
                        % problems)

    # (fg) module evidence drift: the module spans L288-320; moving lineStart is a target drift.
    fg_bad = copy.deepcopy(spec)
    for m in fg_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["evidence"]["lineStart"] = 289
    problems, _ = gate.check(fg_bad, manifest)
    if not has(problems, "implementation module 'drone_2': evidence.lineStart"):
        raise SystemExit("drone_2 module evidence.lineStart drift (288 -> 289) not enforced: %r"
                        % problems)

    # (fh) 5-generator cardinality - FIRST generator: deleting drone_2.tune_1 must fail.
    fh_bad = copy.deepcopy(spec)
    for m in fh_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_2.tune_1"]
    problems, _ = gate.check(fh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_2.tune_1'"):
        raise SystemExit("drone_2 first-generator delete (drone_2.tune_1) not enforced: %r" % problems)

    # (fi) 5-generator cardinality - MIDDLE generator: deleting drone_2.tune_3 must fail.
    fi_bad = copy.deepcopy(spec)
    for m in fi_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_2.tune_3"]
    problems, _ = gate.check(fi_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_2.tune_3'"):
        raise SystemExit("drone_2 middle-generator delete (drone_2.tune_3) not enforced: %r" % problems)

    # (fj) 5-generator cardinality - LAST generator: deleting drone_2.tune_5 must fail.
    fj_bad = copy.deepcopy(spec)
    for m in fj_bad["modules"]:
        if m.get("stable_id") == "drone_2":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_2.tune_5"]
    problems, _ = gate.check(fj_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_2.tune_5'"):
        raise SystemExit("drone_2 last-generator delete (drone_2.tune_5) not enforced: %r" % problems)

    # (fk) ID mapping: renumber a landed generator param id (drone_2.tune_1 220 -> 999) must fail.
    fk_bad = copy.deepcopy(spec)
    reg_param(fk_bad, "drone_2.tune_1")["id"] = 999
    problems, _ = gate.check(fk_bad, manifest)
    if not has(problems, "implementation param 'drone_2.tune_1'"):
        raise SystemExit("drone_2 param id renumber (tune_1 220 -> 999) not enforced: %r" % problems)

    # (fl) NAME mapping: a landed mute param must keep its exact label ('MUTE 1'); a silent rename is a
    #      target drift.
    fl_bad = copy.deepcopy(spec)
    reg_param(fl_bad, "drone_2.mute_1")["name"] = "MUTE A"
    problems, _ = gate.check(fl_bad, manifest)
    if not has(problems, "implementation param 'drone_2.mute_1'"):
        raise SystemExit("drone_2 param name drift (mute_1 'MUTE 1' -> 'MUTE A') not enforced: %r"
                        % problems)

    # (fm) MUTE selector positions drift: mute_1 is an off/on toggle; reordering the domain is a UI/MIDI
    #      re-mapping and must fail.
    fm_bad = copy.deepcopy(spec)
    reg_param(fm_bad, "drone_2.mute_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(fm_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_2.mute_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (fn) MOD selector positions drift: mod_1 is the pitch-modulation toggle; its domain must not move.
    fn_bad = copy.deepcopy(spec)
    reg_param(fn_bad, "drone_2.mod_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(fn_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_2.mod_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (fo) VOLT descriptorEvidence.line drift: the VOLT transposition knob cites L309; moving it must
    #      fail.
    fo_bad = copy.deepcopy(spec)
    reg_param(fo_bad, "drone_2.volt")["evidence"]["line"] = 310
    problems, _ = gate.check(fo_bad, manifest)
    if not has(problems, "implementation param 'drone_2.volt'"):
        raise SystemExit("drone_2.volt descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (fp) ATT descriptorEvidence.line drift: attack cites L309; moving it must fail.
    fp_bad = copy.deepcopy(spec)
    reg_param(fp_bad, "drone_2.att")["evidence"]["line"] = 310
    problems, _ = gate.check(fp_bad, manifest)
    if not has(problems, "implementation param 'drone_2.att'"):
        raise SystemExit("drone_2.att descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (fq) RLS descriptorEvidence.line drift: release cites L309; moving it must fail.
    fq_bad = copy.deepcopy(spec)
    reg_param(fq_bad, "drone_2.rls")["evidence"]["line"] = 310
    problems, _ = gate.check(fq_bad, manifest)
    if not has(problems, "implementation param 'drone_2.rls'"):
        raise SystemExit("drone_2.rls descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (fr) HOLD descriptorEvidence.line drift: gate_hold cites the panel HOLD label (L53); moving it
    #      must fail.
    fr_bad = copy.deepcopy(spec)
    reg_param(fr_bad, "drone_2.gate_hold")["evidence"]["line"] = 54
    problems, _ = gate.check(fr_bad, manifest)
    if not has(problems, "implementation param 'drone_2.gate_hold'"):
        raise SystemExit("drone_2.gate_hold descriptorEvidence.line drift (53 -> 54) not enforced: %r"
                        % problems)

    # (fs) continuous placeholder provenance over-claim: VOLT is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    fs_bad = copy.deepcopy(spec)
    reg_param(fs_bad, "drone_2.volt")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(fs_bad, manifest)
    if not has(problems, "implementation param 'drone_2.volt'"):
        raise SystemExit("drone_2.volt fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (ft) cv_mod_in nominal range drift: the CV MOD input occupies the placeholder -5..+5 range; even
    #      though the evidence is unverified the gate freezes the value, so drifting nominalMax must fail.
    ft_bad = copy.deepcopy(spec)
    reg_jack(ft_bad, "drone_2.cv_mod_in")["nominalMax"] = 6
    problems, _ = gate.check(ft_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.cv_mod_in'"):
        raise SystemExit("drone_2.cv_mod_in nominal range drift (5 -> 6) not enforced: %r" % problems)

    # (fu) cv_mod_in polarity drift: cv_mod_in is a CV input whose polarity is unknown/unverified;
    #      silently asserting a concrete polarity is a type over-claim and must fail.
    fu_bad = copy.deepcopy(spec)
    reg_jack(fu_bad, "drone_2.cv_mod_in")["polarity"] = "unipolar"
    problems, _ = gate.check(fu_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.cv_mod_in'"):
        raise SystemExit("drone_2.cv_mod_in polarity drift (unknown -> unipolar) not enforced: %r"
                        % problems)

    # (fv) gate_in signalType drift: gate_in is a confirmed GATE input; silently reclassifying it as a
    #      CV input is a type drift and must fail.
    fv_bad = copy.deepcopy(spec)
    reg_jack(fv_bad, "drone_2.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(fv_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.gate_in'"):
        raise SystemExit("drone_2.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (fw) gate_in nominal range drift: the gate input placeholder is 0..+10; drifting nominalMax must
    #      fail.
    fw_bad = copy.deepcopy(spec)
    reg_jack(fw_bad, "drone_2.gate_in")["nominalMax"] = 9
    problems, _ = gate.check(fw_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.gate_in'"):
        raise SystemExit("drone_2.gate_in nominal range drift (10 -> 9) not enforced: %r" % problems)

    # (fx) env_out nominal range drift: env_out is the confirmed -10..+10V bipolar CV output (L157);
    #      widening nominalMax must fail.
    fx_bad = copy.deepcopy(spec)
    reg_jack(fx_bad, "drone_2.env_out")["nominalMax"] = 11
    problems, _ = gate.check(fx_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.env_out'"):
        raise SystemExit("drone_2.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (fy) env_out polarity drift: env_out is a confirmed bipolar CV output; silently reclassifying it
    #      as unipolar must fail.
    fy_bad = copy.deepcopy(spec)
    reg_jack(fy_bad, "drone_2.env_out")["polarity"] = "unipolar"
    problems, _ = gate.check(fy_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.env_out'"):
        raise SystemExit("drone_2.env_out polarity drift (bipolar -> unipolar) not enforced: %r"
                        % problems)

    # (fz) env_out descriptorEvidence.line drift: env_out cites the ENV VOICES output spec (L157);
    #      moving the citation must fail.
    fz_bad = copy.deepcopy(spec)
    reg_jack(fz_bad, "drone_2.env_out")["evidence"]["line"] = 158
    problems, _ = gate.check(fz_bad, manifest)
    if not has(problems, "implementation jack 'drone_2.env_out'"):
        raise SystemExit("drone_2.env_out descriptorEvidence.line drift (157 -> 158) not enforced: %r"
                        % problems)

    # (ga) cross-owner leak: a drone_2 descriptor member must not be re-owner'd to a drone_1 stable_id
    #      (a copy-paste from the DRONE 1 slice). Renaming drone_2.tune_1 to drone_1.tune_1 must fail —
    #      the gate catches it as an enumerator collision (two members resolve to drone_1_tune_1).
    ga_bad = copy.deepcopy(spec)
    reg_param(ga_bad, "drone_2.tune_1")["stable_id"] = "drone_1.tune_1"
    problems, _ = gate.check(ga_bad, manifest)
    if not has(problems, "enumerator collision"):
        raise SystemExit("drone_2 cross-owner leak (tune_1 -> drone_1.tune_1) not enforced: %r"
                        % problems)

    # ---- DRONE 4 classic voice slice (Codex msg 483a9dc5) --------------------
    # Same structural/evidence boundary as DRONE 1/2, but env_out carries the mandated KEY DIFF:
    # manual L157 "ENV VOICES 1,2,3,6,7,8" does NOT list 4/5, so the -10..+10V/bipolar confirmed from
    # DRONE 1/2 is NOT copied here. env_out keeps output+cv confirmed (L317-318) but nominalRange +
    # polarity BOTH stay unverified/unknown, and the description asserts no specific voltage. So the
    # env_out negatives are OPPOSITE-polarity to DRONE 2: a polarity GUESS to bipolar MUST fail, and
    # raising range evidence to confirmed MUST fail. Wrong if the group cardinality drifts (first /
    # middle / last delete), if any id/name mapping renumbers or renames a member, if a MUTE/MOD
    # selector loses its value domain, if a VOLT/ATT/RLS/HOLD evidence line moves or its provenance
    # over-claims, if a jack's range / type / polarity / evidence drifts, or if a drone_4 member leaks
    # a drone_1 or drone_2 stable_id (copy-paste from a prior slice). Each must FAIL normal for its
    # own reason.

    # (gc) module delete: pulling the whole landed module (id 17) out of the registry must fail.
    gc_bad = copy.deepcopy(spec)
    gc_bad["modules"] = [m for m in gc_bad["modules"] if m.get("stable_id") != "drone_4"]
    problems, _ = gate.check(gc_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_4'"):
        raise SystemExit("drone_4 module delete not enforced: %r" % problems)

    # (gd) module id renumber: drone_4 is a landed module with id 17; renumbering (17 -> 999) must fail.
    gd_bad = copy.deepcopy(spec)
    for m in gd_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["id"] = 999
    problems, _ = gate.check(gd_bad, manifest)
    if not has(problems, "implementation module 'drone_4': id"):
        raise SystemExit("drone_4 module id renumber (17 -> 999) not enforced: %r" % problems)

    # (ge) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 4'); a silent rename is a target drift.
    ge_bad = copy.deepcopy(spec)
    for m in ge_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["name"] = "DRONE FOUR"
    problems, _ = gate.check(ge_bad, manifest)
    if not has(problems, "implementation module 'drone_4': name"):
        raise SystemExit("drone_4 module name drift ('DRONE 4' -> 'DRONE FOUR') not enforced: %r"
                        % problems)

    # (gf) module evidence drift: the module spans L288-320; moving lineStart is a target drift.
    gf_bad = copy.deepcopy(spec)
    for m in gf_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["evidence"]["lineStart"] = 289
    problems, _ = gate.check(gf_bad, manifest)
    if not has(problems, "implementation module 'drone_4': evidence.lineStart"):
        raise SystemExit("drone_4 module evidence.lineStart drift (288 -> 289) not enforced: %r"
                        % problems)

    # (gg) 5-generator cardinality - FIRST generator: deleting drone_4.tune_1 must fail.
    gg_bad = copy.deepcopy(spec)
    for m in gg_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_4.tune_1"]
    problems, _ = gate.check(gg_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_4.tune_1'"):
        raise SystemExit("drone_4 first-generator delete (drone_4.tune_1) not enforced: %r" % problems)

    # (gh) 5-generator cardinality - MIDDLE generator: deleting drone_4.tune_3 must fail.
    gh_bad = copy.deepcopy(spec)
    for m in gh_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_4.tune_3"]
    problems, _ = gate.check(gh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_4.tune_3'"):
        raise SystemExit("drone_4 middle-generator delete (drone_4.tune_3) not enforced: %r" % problems)

    # (gi) 5-generator cardinality - LAST generator: deleting drone_4.tune_5 must fail.
    gi_bad = copy.deepcopy(spec)
    for m in gi_bad["modules"]:
        if m.get("stable_id") == "drone_4":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_4.tune_5"]
    problems, _ = gate.check(gi_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_4.tune_5'"):
        raise SystemExit("drone_4 last-generator delete (drone_4.tune_5) not enforced: %r" % problems)

    # (gj) ID mapping: renumber a landed generator param id (drone_4.tune_1 239 -> 999) must fail.
    gj_bad = copy.deepcopy(spec)
    reg_param(gj_bad, "drone_4.tune_1")["id"] = 999
    problems, _ = gate.check(gj_bad, manifest)
    if not has(problems, "implementation param 'drone_4.tune_1'"):
        raise SystemExit("drone_4 param id renumber (tune_1 239 -> 999) not enforced: %r" % problems)

    # (gk) NAME mapping: a landed mute param must keep its exact label ('MUTE 1'); a silent rename is a
    #      target drift.
    gk_bad = copy.deepcopy(spec)
    reg_param(gk_bad, "drone_4.mute_1")["name"] = "MUTE A"
    problems, _ = gate.check(gk_bad, manifest)
    if not has(problems, "implementation param 'drone_4.mute_1'"):
        raise SystemExit("drone_4 param name drift (mute_1 'MUTE 1' -> 'MUTE A') not enforced: %r"
                        % problems)

    # (gl) MUTE selector positions drift: mute_1 is an off/on toggle; reordering the domain is a UI/MIDI
    #      re-mapping and must fail.
    gl_bad = copy.deepcopy(spec)
    reg_param(gl_bad, "drone_4.mute_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(gl_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_4.mute_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (gm) MOD selector positions drift: mod_1 is the pitch-modulation toggle; its domain must not move.
    gm_bad = copy.deepcopy(spec)
    reg_param(gm_bad, "drone_4.mod_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(gm_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_4.mod_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (gn) VOLT descriptorEvidence.line drift: the VOLT transposition knob cites L309; moving it must
    #      fail.
    gn_bad = copy.deepcopy(spec)
    reg_param(gn_bad, "drone_4.volt")["evidence"]["line"] = 310
    problems, _ = gate.check(gn_bad, manifest)
    if not has(problems, "implementation param 'drone_4.volt'"):
        raise SystemExit("drone_4.volt descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (go) ATT descriptorEvidence.line drift: attack cites L309; moving it must fail.
    go_bad = copy.deepcopy(spec)
    reg_param(go_bad, "drone_4.att")["evidence"]["line"] = 310
    problems, _ = gate.check(go_bad, manifest)
    if not has(problems, "implementation param 'drone_4.att'"):
        raise SystemExit("drone_4.att descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (gp) RLS descriptorEvidence.line drift: release cites L309; moving it must fail.
    gp_bad = copy.deepcopy(spec)
    reg_param(gp_bad, "drone_4.rls")["evidence"]["line"] = 310
    problems, _ = gate.check(gp_bad, manifest)
    if not has(problems, "implementation param 'drone_4.rls'"):
        raise SystemExit("drone_4.rls descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (gq) HOLD descriptorEvidence.line drift: gate_hold cites the panel HOLD label (L53); moving it
    #      must fail.
    gq_bad = copy.deepcopy(spec)
    reg_param(gq_bad, "drone_4.gate_hold")["evidence"]["line"] = 54
    problems, _ = gate.check(gq_bad, manifest)
    if not has(problems, "implementation param 'drone_4.gate_hold'"):
        raise SystemExit("drone_4.gate_hold descriptorEvidence.line drift (53 -> 54) not enforced: %r"
                        % problems)

    # (gr) continuous placeholder provenance over-claim: VOLT is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    gr_bad = copy.deepcopy(spec)
    reg_param(gr_bad, "drone_4.volt")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(gr_bad, manifest)
    if not has(problems, "implementation param 'drone_4.volt'"):
        raise SystemExit("drone_4.volt fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (gs) cv_mod_in nominal range drift: the CV MOD input occupies the placeholder -5..+5 range; even
    #      though the evidence is unverified the gate freezes the value, so drifting nominalMax must fail.
    gs_bad = copy.deepcopy(spec)
    reg_jack(gs_bad, "drone_4.cv_mod_in")["nominalMax"] = 6
    problems, _ = gate.check(gs_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.cv_mod_in'"):
        raise SystemExit("drone_4.cv_mod_in nominal range drift (5 -> 6) not enforced: %r" % problems)

    # (gt) cv_mod_in polarity drift: cv_mod_in is a CV input whose polarity is unknown/unverified;
    #      silently asserting a concrete polarity is a type over-claim and must fail.
    gt_bad = copy.deepcopy(spec)
    reg_jack(gt_bad, "drone_4.cv_mod_in")["polarity"] = "unipolar"
    problems, _ = gate.check(gt_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.cv_mod_in'"):
        raise SystemExit("drone_4.cv_mod_in polarity drift (unknown -> unipolar) not enforced: %r"
                        % problems)

    # (gu) gate_in signalType drift: gate_in is a confirmed GATE input; silently reclassifying it as a
    #      CV input is a type drift and must fail.
    gu_bad = copy.deepcopy(spec)
    reg_jack(gu_bad, "drone_4.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(gu_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.gate_in'"):
        raise SystemExit("drone_4.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (gv) gate_in nominal range drift: the gate input placeholder is 0..+10; drifting nominalMax must
    #      fail.
    gv_bad = copy.deepcopy(spec)
    reg_jack(gv_bad, "drone_4.gate_in")["nominalMax"] = 9
    problems, _ = gate.check(gv_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.gate_in'"):
        raise SystemExit("drone_4.gate_in nominal range drift (10 -> 9) not enforced: %r" % problems)

    # (gw) env_out nominal range drift: env_out holds the placeholder -10..+10 (unasserted, not the
    #      DRONE 1/2 confirmed V); widening nominalMax must fail.
    gw_bad = copy.deepcopy(spec)
    reg_jack(gw_bad, "drone_4.env_out")["nominalMax"] = 11
    problems, _ = gate.check(gw_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.env_out'"):
        raise SystemExit("drone_4.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (gx) env_out polarity GUESS: manual L157 excludes 4/5, so env_out polarity is unknown/unverified.
    #      Guessing a concrete bipolar (as DRONE 1/2 lands) is a mandated-negative over-claim and must
    #      fail.
    gx_bad = copy.deepcopy(spec)
    reg_jack(gx_bad, "drone_4.env_out")["polarity"] = "bipolar"
    problems, _ = gate.check(gx_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.env_out'"):
        raise SystemExit("drone_4.env_out polarity guess (unknown -> bipolar) not enforced: %r"
                        % problems)

    # (gy) env_out nominalRange evidence over-claim: L157 excludes 4/5 so nominalRange stays unverified;
    #      silently raising the evidence to confirmed is a mandated-negative over-claim and must fail.
    gy_bad = copy.deepcopy(spec)
    reg_jack(gy_bad, "drone_4.env_out")["fieldEvidence"]["nominalRange"] = "confirmed"
    problems, _ = gate.check(gy_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.env_out'"):
        raise SystemExit("drone_4.env_out fieldEvidence over-claim (nominalRange unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (gz) env_out descriptorEvidence.line drift: env_out cites the output-of-envelope spec (L318);
    #      moving the citation must fail.
    gz_bad = copy.deepcopy(spec)
    reg_jack(gz_bad, "drone_4.env_out")["evidence"]["line"] = 319
    problems, _ = gate.check(gz_bad, manifest)
    if not has(problems, "implementation jack 'drone_4.env_out'"):
        raise SystemExit("drone_4.env_out descriptorEvidence.line drift (318 -> 319) not enforced: %r"
                        % problems)

    # (ha) cross-owner leak: a drone_4 descriptor member must not be re-owner'd to a drone_1 stable_id
    #      (a copy-paste from the DRONE 1 slice). Renaming drone_4.tune_1 to drone_1.tune_1 must fail —
    #      the gate catches it as an enumerator collision (two members resolve to drone_1_tune_1).
    ha_bad = copy.deepcopy(spec)
    reg_param(ha_bad, "drone_4.tune_1")["stable_id"] = "drone_1.tune_1"
    problems, _ = gate.check(ha_bad, manifest)
    if not has(problems, "enumerator collision"):
        raise SystemExit("drone_4 cross-owner leak (tune_1 -> drone_1.tune_1) not enforced: %r"
                        % problems)

    # ---- DRONE 5 classic voice slice (Codex msg 6e25b926) --------------------
    # Same structural/evidence boundary as DRONE 4, and the same mandated KEY DIFF on env_out:
    # manual L157 "ENV VOICES 1,2,3,6,7,8" does NOT list 4/5, so the -10..+10V/bipolar confirmed from
    # DRONE 1/2 is NOT copied here. env_out keeps output+cv confirmed (L317-318) but nominalRange +
    # polarity BOTH stay unverified/unknown, and the description asserts no specific voltage. So the
    # env_out negatives are OPPOSITE-polarity to DRONE 2: a polarity GUESS to bipolar MUST fail, and
    # raising range evidence to confirmed MUST fail. Wrong if the group cardinality drifts (first /
    # middle / last delete), if any id/name mapping renumbers or renames a member, if a MUTE/MOD
    # selector loses its value domain, if a VOLT/ATT/RLS/HOLD evidence line moves or its provenance
    # over-claims, if a jack's range / type / polarity / evidence drifts, or if a drone_5 member leaks
    # a drone_1 or drone_2 stable_id (copy-paste from a prior slice). Each must FAIL normal for its
    # own reason.

    # (hb) module delete: pulling the whole landed module (id 18) out of the registry must fail.
    hb_bad = copy.deepcopy(spec)
    hb_bad["modules"] = [m for m in hb_bad["modules"] if m.get("stable_id") != "drone_5"]
    problems, _ = gate.check(hb_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_5'"):
        raise SystemExit("drone_5 module delete not enforced: %r" % problems)

    # (hc) module id renumber: drone_5 is a landed module with id 18; renumbering (18 -> 999) must fail.
    hc_bad = copy.deepcopy(spec)
    for m in hc_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["id"] = 999
    problems, _ = gate.check(hc_bad, manifest)
    if not has(problems, "implementation module 'drone_5': id"):
        raise SystemExit("drone_5 module id renumber (18 -> 999) not enforced: %r" % problems)

    # (hd) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 5'); a silent rename is a target drift.
    hd_bad = copy.deepcopy(spec)
    for m in hd_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["name"] = "DRONE FIVE"
    problems, _ = gate.check(hd_bad, manifest)
    if not has(problems, "implementation module 'drone_5': name"):
        raise SystemExit("drone_5 module name drift ('DRONE 5' -> 'DRONE FIVE') not enforced: %r"
                        % problems)

    # (he) module evidence drift: the module spans L288-320; moving lineStart is a target drift.
    he_bad = copy.deepcopy(spec)
    for m in he_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["evidence"]["lineStart"] = 289
    problems, _ = gate.check(he_bad, manifest)
    if not has(problems, "implementation module 'drone_5': evidence.lineStart"):
        raise SystemExit("drone_5 module evidence.lineStart drift (288 -> 289) not enforced: %r"
                        % problems)

    # (hf) 5-generator cardinality - FIRST generator: deleting drone_5.tune_1 must fail.
    hf_bad = copy.deepcopy(spec)
    for m in hf_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_5.tune_1"]
    problems, _ = gate.check(hf_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_5.tune_1'"):
        raise SystemExit("drone_5 first-generator delete (drone_5.tune_1) not enforced: %r" % problems)

    # (hg) 5-generator cardinality - MIDDLE generator: deleting drone_5.tune_3 must fail.
    hg_bad = copy.deepcopy(spec)
    for m in hg_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_5.tune_3"]
    problems, _ = gate.check(hg_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_5.tune_3'"):
        raise SystemExit("drone_5 middle-generator delete (drone_5.tune_3) not enforced: %r" % problems)

    # (hh) 5-generator cardinality - LAST generator: deleting drone_5.tune_5 must fail.
    hh_bad = copy.deepcopy(spec)
    for m in hh_bad["modules"]:
        if m.get("stable_id") == "drone_5":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_5.tune_5"]
    problems, _ = gate.check(hh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_5.tune_5'"):
        raise SystemExit("drone_5 last-generator delete (drone_5.tune_5) not enforced: %r" % problems)

    # (hi) ID mapping: renumber a landed generator param id (drone_5.tune_1 258 -> 999) must fail.
    hi_bad = copy.deepcopy(spec)
    reg_param(hi_bad, "drone_5.tune_1")["id"] = 999
    problems, _ = gate.check(hi_bad, manifest)
    if not has(problems, "implementation param 'drone_5.tune_1'"):
        raise SystemExit("drone_5 param id renumber (tune_1 258 -> 999) not enforced: %r" % problems)

    # (hj) NAME mapping: a landed mute param must keep its exact label ('MUTE 1'); a silent rename is a
    #      target drift.
    hj_bad = copy.deepcopy(spec)
    reg_param(hj_bad, "drone_5.mute_1")["name"] = "MUTE A"
    problems, _ = gate.check(hj_bad, manifest)
    if not has(problems, "implementation param 'drone_5.mute_1'"):
        raise SystemExit("drone_5 param name drift (mute_1 'MUTE 1' -> 'MUTE A') not enforced: %r"
                        % problems)

    # (hk) MUTE selector positions drift: mute_1 is an off/on toggle; reordering the domain is a
    #      re-mapping and must fail.
    hk_bad = copy.deepcopy(spec)
    reg_param(hk_bad, "drone_5.mute_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(hk_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_5.mute_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (hl) MOD selector positions drift: mod_1 is the pitch-modulation toggle; its domain must not move.
    hl_bad = copy.deepcopy(spec)
    reg_param(hl_bad, "drone_5.mod_1")["positions"] = ["on", "off"]
    problems, _ = gate.check(hl_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_5.mod_1 selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (hm) VOLT descriptorEvidence.line drift: the VOLT transposition knob cites L309; moving it must
    #      fail.
    hm_bad = copy.deepcopy(spec)
    reg_param(hm_bad, "drone_5.volt")["evidence"]["line"] = 310
    problems, _ = gate.check(hm_bad, manifest)
    if not has(problems, "implementation param 'drone_5.volt'"):
        raise SystemExit("drone_5.volt descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (hn) ATT descriptorEvidence.line drift: attack cites L309; moving it must fail.
    hn_bad = copy.deepcopy(spec)
    reg_param(hn_bad, "drone_5.att")["evidence"]["line"] = 310
    problems, _ = gate.check(hn_bad, manifest)
    if not has(problems, "implementation param 'drone_5.att'"):
        raise SystemExit("drone_5.att descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (ho) RLS descriptorEvidence.line drift: release cites L309; moving it must fail.
    ho_bad = copy.deepcopy(spec)
    reg_param(ho_bad, "drone_5.rls")["evidence"]["line"] = 310
    problems, _ = gate.check(ho_bad, manifest)
    if not has(problems, "implementation param 'drone_5.rls'"):
        raise SystemExit("drone_5.rls descriptorEvidence.line drift (309 -> 310) not enforced: %r"
                        % problems)

    # (hp) HOLD descriptorEvidence.line drift: gate_hold cites the panel HOLD label (L53); moving it
    #      must fail.
    hp_bad = copy.deepcopy(spec)
    reg_param(hp_bad, "drone_5.gate_hold")["evidence"]["line"] = 54
    problems, _ = gate.check(hp_bad, manifest)
    if not has(problems, "implementation param 'drone_5.gate_hold'"):
        raise SystemExit("drone_5.gate_hold descriptorEvidence.line drift (53 -> 54) not enforced: %r"
                        % problems)

    # (hq) continuous placeholder provenance over-claim: VOLT is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    hq_bad = copy.deepcopy(spec)
    reg_param(hq_bad, "drone_5.volt")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(hq_bad, manifest)
    if not has(problems, "implementation param 'drone_5.volt'"):
        raise SystemExit("drone_5.volt fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (hr) cv_mod_in nominal range drift: the CV MOD input occupies the placeholder -5..+5 range; even
    #      though the evidence is unverified the gate freezes the value, so drifting nominalMax must fail.
    hr_bad = copy.deepcopy(spec)
    reg_jack(hr_bad, "drone_5.cv_mod_in")["nominalMax"] = 6
    problems, _ = gate.check(hr_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.cv_mod_in'"):
        raise SystemExit("drone_5.cv_mod_in nominal range drift (5 -> 6) not enforced: %r" % problems)

    # (hs) cv_mod_in polarity drift: cv_mod_in is a CV input whose polarity is unknown/unverified;
    #      silently asserting a concrete polarity is a type over-claim and must fail.
    hs_bad = copy.deepcopy(spec)
    reg_jack(hs_bad, "drone_5.cv_mod_in")["polarity"] = "unipolar"
    problems, _ = gate.check(hs_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.cv_mod_in'"):
        raise SystemExit("drone_5.cv_mod_in polarity drift (unknown -> unipolar) not enforced: %r"
                        % problems)

    # (ht) gate_in signalType drift: gate_in is a confirmed GATE input; silently reclassifying it as a
    #      CV input is a type drift and must fail.
    ht_bad = copy.deepcopy(spec)
    reg_jack(ht_bad, "drone_5.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(ht_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.gate_in'"):
        raise SystemExit("drone_5.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (hu) gate_in nominal range drift: the gate input placeholder is 0..+10; drifting nominalMax must
    #      fail.
    hu_bad = copy.deepcopy(spec)
    reg_jack(hu_bad, "drone_5.gate_in")["nominalMax"] = 9
    problems, _ = gate.check(hu_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.gate_in'"):
        raise SystemExit("drone_5.gate_in nominal range drift (10 -> 9) not enforced: %r" % problems)

    # (hv) env_out nominal range drift: env_out holds the placeholder -10..+10 (unasserted, not the
    #      DRONE 1/2 confirmed V); widening nominalMax must fail.
    hv_bad = copy.deepcopy(spec)
    reg_jack(hv_bad, "drone_5.env_out")["nominalMax"] = 11
    problems, _ = gate.check(hv_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.env_out'"):
        raise SystemExit("drone_5.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (hw) env_out polarity GUESS: manual L157 excludes 4/5, so env_out polarity is unknown/unverified.
    #      Guessing a concrete bipolar (as DRONE 1/2 lands) is a mandated-negative over-claim and must
    #      fail.
    hw_bad = copy.deepcopy(spec)
    reg_jack(hw_bad, "drone_5.env_out")["polarity"] = "bipolar"
    problems, _ = gate.check(hw_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.env_out'"):
        raise SystemExit("drone_5.env_out polarity guess (unknown -> bipolar) not enforced: %r"
                        % problems)

    # (hx) env_out nominalRange evidence over-claim: L157 excludes 4/5 so nominalRange stays unverified;
    #      silently raising the evidence to confirmed is a mandated-negative over-claim and must fail.
    hx_bad = copy.deepcopy(spec)
    reg_jack(hx_bad, "drone_5.env_out")["fieldEvidence"]["nominalRange"] = "confirmed"
    problems, _ = gate.check(hx_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.env_out'"):
        raise SystemExit("drone_5.env_out fieldEvidence over-claim (nominalRange unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (hy) env_out descriptorEvidence.line drift: env_out cites the output-of-envelope spec (L318);
    #      moving the citation must fail.
    hy_bad = copy.deepcopy(spec)
    reg_jack(hy_bad, "drone_5.env_out")["evidence"]["line"] = 319
    problems, _ = gate.check(hy_bad, manifest)
    if not has(problems, "implementation jack 'drone_5.env_out'"):
        raise SystemExit("drone_5.env_out descriptorEvidence.line drift (318 -> 319) not enforced: %r"
                        % problems)

    # (hz) cross-owner leak: a drone_5 descriptor member must not be re-owner'd to a drone_1 stable_id
    #      (a copy-paste from the DRONE 1 slice). Renaming drone_5.tune_1 to drone_1.tune_1 must fail —
    #      the gate catches it as an enumerator collision (two members resolve to drone_1_tune_1).
    hz_bad = copy.deepcopy(spec)
    reg_param(hz_bad, "drone_5.tune_1")["stable_id"] = "drone_1.tune_1"
    problems, _ = gate.check(hz_bad, manifest)
    if not has(problems, "enumerator collision"):
        raise SystemExit("drone_5 cross-owner leak (tune_1 -> drone_1.tune_1) not enforced: %r"
                        % problems)

    # ---- DRONE 3 "Papa Srapa" voice slice (Codex msg b86a53c0) --------------
    # DIFFERENT envelope boundary from DRONE 4/5: manual L157 "ENV VOICES 1,2,3,6,7,8" DOES list
    # voice 3, so drone_3.env_out IS a confirmed -10..+10V/bipolar output (NOT the unverified/
    # unknown placeholder DRONE 4/5 carry). So the env_out negatives are OPPOSITE-polarity here:
    # drifting polarity away from bipolar MUST fail, and downgrading nominalRange evidence to
    # unverified MUST fail. The 5V gate is produced by the keyboard (buttons 3 and 6, L331-332),
    # NOT by the gate_in input rail — so asserting gate_in polarity unipolar (reverse-inferring
    # from the keyboard's 5V gate) is a mandated-negative over-claim and MUST fail. cv_out is the
    # confirmed 0..+12V/unipolar square-wave modulator output (L158 + L345-346). clock_in (clock
    # sets the S&H speed) and noise_in (only the S&H/IN identity) have unverified electrical
    # characteristics. Selector domains: hi_low = [hi,low]; fm/am/rate_switch/hold = [off,on]
    # (all-six provisional). Continuous params are software-normalized 0..1 placeholders.
    # Wrong if the group cardinality drifts, if any id/name mapping renumbers or renames a member,
    # if a continuous param loses its 0..1 placeholder provenance, if a selector loses its value
    # domain, if a jack's range / type / polarity / evidence drifts, or if a drone_3 member leaks
    # a drone_1 stable_id (copy-paste from a prior slice). Each must FAIL normal for its own reason.

    # (ia) module delete: pulling the whole landed module (id 19) out of the registry must fail.
    ia_bad = copy.deepcopy(spec)
    ia_bad["modules"] = [m for m in ia_bad["modules"] if m.get("stable_id") != "drone_3"]
    problems, _ = gate.check(ia_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_3'"):
        raise SystemExit("drone_3 module delete not enforced: %r" % problems)

    # (ib) module id renumber: drone_3 is a landed module with id 19; renumbering (19 -> 999) must fail.
    ib_bad = copy.deepcopy(spec)
    for m in ib_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["id"] = 999
    problems, _ = gate.check(ib_bad, manifest)
    if not has(problems, "implementation module 'drone_3': id"):
        raise SystemExit("drone_3 module id renumber (19 -> 999) not enforced: %r" % problems)

    # (ic) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 3'); a silent rename is a target drift.
    ic_bad = copy.deepcopy(spec)
    for m in ic_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["name"] = "DRONE THREE"
    problems, _ = gate.check(ic_bad, manifest)
    if not has(problems, "implementation module 'drone_3': name"):
        raise SystemExit("drone_3 module name drift ('DRONE 3' -> 'DRONE THREE') not enforced: %r"
                        % problems)

    # (id) module evidence drift: the module spans L329-370; moving lineStart is a target drift.
    id_bad = copy.deepcopy(spec)
    for m in id_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["evidence"]["lineStart"] = 330
    problems, _ = gate.check(id_bad, manifest)
    if not has(problems, "implementation module 'drone_3': evidence.lineStart"):
        raise SystemExit("drone_3 module evidence.lineStart drift (329 -> 330) not enforced: %r"
                        % problems)

    # (ie) 7-continuous cardinality - FIRST generator: deleting drone_3.rate must fail.
    ie_bad = copy.deepcopy(spec)
    for m in ie_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_3.rate"]
    problems, _ = gate.check(ie_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_3.rate'"):
        raise SystemExit("drone_3 first-continuous delete (drone_3.rate) not enforced: %r" % problems)

    # (if) 7-continuous cardinality - MIDDLE generator: deleting drone_3.pitch must fail.
    if_bad = copy.deepcopy(spec)
    for m in if_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_3.pitch"]
    problems, _ = gate.check(if_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_3.pitch'"):
        raise SystemExit("drone_3 middle-continuous delete (drone_3.pitch) not enforced: %r" % problems)

    # (ig) 7-continuous cardinality - LAST generator: deleting drone_3.rls must fail.
    ig_bad = copy.deepcopy(spec)
    for m in ig_bad["modules"]:
        if m.get("stable_id") == "drone_3":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_3.rls"]
    problems, _ = gate.check(ig_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_3.rls'"):
        raise SystemExit("drone_3 last-continuous delete (drone_3.rls) not enforced: %r" % problems)

    # (ih) ID mapping: renumber a landed continuous param id (drone_3.rate 277 -> 999) must fail.
    ih_bad = copy.deepcopy(spec)
    reg_param(ih_bad, "drone_3.rate")["id"] = 999
    problems, _ = gate.check(ih_bad, manifest)
    if not has(problems, "implementation param 'drone_3.rate'"):
        raise SystemExit("drone_3 param id renumber (rate 277 -> 999) not enforced: %r" % problems)

    # (ii) NAME mapping: a landed selector param must keep its exact label ('RANGE'); a silent rename
    #      is a target drift.
    ii_bad = copy.deepcopy(spec)
    reg_param(ii_bad, "drone_3.hi_low")["name"] = "RANGE SW"
    problems, _ = gate.check(ii_bad, manifest)
    if not has(problems, "implementation param 'drone_3.hi_low'"):
        raise SystemExit("drone_3 param name drift (hi_low 'RANGE' -> 'RANGE SW') not enforced: %r"
                        % problems)

    # (ij) hi_low selector positions drift: RANGE is hi/low; reordering the domain is a re-mapping and
    #      must fail.
    ij_bad = copy.deepcopy(spec)
    reg_param(ij_bad, "drone_3.hi_low")["positions"] = ["low", "hi"]
    problems, _ = gate.check(ij_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_3.hi_low selector positions drift (['hi','low'] -> ['low','hi']) "
                        "not enforced: %r" % problems)

    # (ik) FM selector positions drift: fm is the off/on toggle; its domain must not move.
    ik_bad = copy.deepcopy(spec)
    reg_param(ik_bad, "drone_3.fm")["positions"] = ["on", "off"]
    problems, _ = gate.check(ik_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_3.fm selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (il) RATE descriptorEvidence.line drift: RATE CONTROL cites L345; moving it must fail.
    il_bad = copy.deepcopy(spec)
    reg_param(il_bad, "drone_3.rate")["evidence"]["line"] = 346
    problems, _ = gate.check(il_bad, manifest)
    if not has(problems, "implementation param 'drone_3.rate'"):
        raise SystemExit("drone_3.rate descriptorEvidence.line drift (345 -> 346) not enforced: %r"
                        % problems)

    # (im) PITCH descriptorEvidence.line drift: PITCH KNOBS cites L347; moving it must fail.
    im_bad = copy.deepcopy(spec)
    reg_param(im_bad, "drone_3.pitch")["evidence"]["line"] = 348
    problems, _ = gate.check(im_bad, manifest)
    if not has(problems, "implementation param 'drone_3.pitch'"):
        raise SystemExit("drone_3.pitch descriptorEvidence.line drift (347 -> 348) not enforced: %r"
                        % problems)

    # (in) HOLD descriptorEvidence.line drift: hold cites the panel HOLD label (L82); moving it must
    #      fail.
    in_bad = copy.deepcopy(spec)
    reg_param(in_bad, "drone_3.hold")["evidence"]["line"] = 83
    problems, _ = gate.check(in_bad, manifest)
    if not has(problems, "implementation param 'drone_3.hold'"):
        raise SystemExit("drone_3.hold descriptorEvidence.line drift (82 -> 83) not enforced: %r"
                        % problems)

    # (io) continuous placeholder provenance over-claim: RATE is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    io_bad = copy.deepcopy(spec)
    reg_param(io_bad, "drone_3.rate")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(io_bad, manifest)
    if not has(problems, "implementation param 'drone_3.rate'"):
        raise SystemExit("drone_3.rate fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (ip) cv_out nominal range drift: cv_out is the confirmed 0..+12V output; drifting nominalMax must
    #      fail.
    ip_bad = copy.deepcopy(spec)
    reg_jack(ip_bad, "drone_3.cv_out")["nominalMax"] = 13
    problems, _ = gate.check(ip_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.cv_out'"):
        raise SystemExit("drone_3.cv_out nominal range drift (12 -> 13) not enforced: %r" % problems)

    # (iq) cv_out polarity drift: cv_out is a confirmed UNIPOLAR output; flipping to bipolar is a type
    #      over-claim and must fail.
    iq_bad = copy.deepcopy(spec)
    reg_jack(iq_bad, "drone_3.cv_out")["polarity"] = "bipolar"
    problems, _ = gate.check(iq_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.cv_out'"):
        raise SystemExit("drone_3.cv_out polarity drift (unipolar -> bipolar) not enforced: %r"
                        % problems)

    # (ir) env_out nominal range drift: env_out is the confirmed -10..+10V output; widening nominalMax
    #      must fail.
    ir_bad = copy.deepcopy(spec)
    reg_jack(ir_bad, "drone_3.env_out")["nominalMax"] = 11
    problems, _ = gate.check(ir_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.env_out'"):
        raise SystemExit("drone_3.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (is) env_out polarity drift (KEY DIFF): manual L157 DOES list voice 3, so env_out polarity is a
    #      CONFIRMED bipolar. Dropping it to unknown (as DRONE 4/5 land) is an evidence downgrade and
    #      must fail.
    is_bad = copy.deepcopy(spec)
    reg_jack(is_bad, "drone_3.env_out")["polarity"] = "unknown"
    problems, _ = gate.check(is_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.env_out'"):
        raise SystemExit("drone_3.env_out polarity drift (bipolar -> unknown) not enforced: %r"
                        % problems)

    # (it) env_out nominalRange evidence downgrade (KEY DIFF): because L157 lists voice 3 the
    #      nominalRange evidence is CONFIRMED (unlike DRONE 4/5); silently downgrading it to unverified
    #      is a mandated-negative over-claim and must fail.
    it_bad = copy.deepcopy(spec)
    reg_jack(it_bad, "drone_3.env_out")["fieldEvidence"]["nominalRange"] = "unverified"
    problems, _ = gate.check(it_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.env_out'"):
        raise SystemExit("drone_3.env_out fieldEvidence downgrade (nominalRange confirmed->unverified) "
                        "not enforced: %r" % problems)

    # (iu) gate_in signalType drift: gate_in is a confirmed GATE input; silently reclassifying it as a CV
    #      input is a type drift and must fail.
    iu_bad = copy.deepcopy(spec)
    reg_jack(iu_bad, "drone_3.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(iu_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.gate_in'"):
        raise SystemExit("drone_3.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (iv) gate_in polarity over-claim (KEY keyboard-5V reverse inference): the documented 5V gate is
    #      PRODUCED by the keyboard buttons (L331-332), NOT by this input rail, so gate_in polarity stays
    #      unknown/unverified. Reverse-inferring a concrete unipolar from the keyboard's 5V gate is a
    #      mandated-negative over-claim and must fail.
    iv_bad = copy.deepcopy(spec)
    reg_jack(iv_bad, "drone_3.gate_in")["polarity"] = "unipolar"
    problems, _ = gate.check(iv_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.gate_in'"):
        raise SystemExit("drone_3.gate_in polarity over-claim (unknown -> unipolar, keyboard-5V "
                        "reverse inference) not enforced: %r" % problems)

    # (iw) gate_in nominal range drift: the gate input placeholder is 0..+10; drifting nominalMax must
    #      fail.
    iw_bad = copy.deepcopy(spec)
    reg_jack(iw_bad, "drone_3.gate_in")["nominalMax"] = 9
    problems, _ = gate.check(iw_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.gate_in'"):
        raise SystemExit("drone_3.gate_in nominal range drift (10 -> 9) not enforced: %r" % problems)

    # (ix) clock_in signalType drift: clock_in is a confirmed CLOCK input; reclassifying as CV is a type
    #      drift and must fail.
    ix_bad = copy.deepcopy(spec)
    reg_jack(ix_bad, "drone_3.clock_in")["signalType"] = "cv"
    problems, _ = gate.check(ix_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.clock_in'"):
        raise SystemExit("drone_3.clock_in signalType drift (clock -> cv) not enforced: %r" % problems)

    # (iy) noise_in nominal range drift: the noise input placeholder is -5..+5; drifting nominalMax must
    #      fail.
    iy_bad = copy.deepcopy(spec)
    reg_jack(iy_bad, "drone_3.noise_in")["nominalMax"] = 6
    problems, _ = gate.check(iy_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.noise_in'"):
        raise SystemExit("drone_3.noise_in nominal range drift (5 -> 6) not enforced: %r" % problems)

    # (iz) cross-owner leak: a drone_3 descriptor member must not be re-owner'd to a drone_1 stable_id
    #      (a copy-paste from the DRONE 1 slice). Renaming drone_3.att to drone_1.att must fail — the
    #      gate catches it as an enumerator collision (two members resolve to drone_1_att).
    iz_bad = copy.deepcopy(spec)
    reg_param(iz_bad, "drone_3.att")["stable_id"] = "drone_1.att"
    problems, _ = gate.check(iz_bad, manifest)
    if not has(problems, "enumerator collision"):
        raise SystemExit("drone_3 cross-owner leak (att -> drone_1.att) not enforced: %r" % problems)

    # (ja) drone_3.cv_out descriptorEvidence.line drift: cv_out is the confirmed 0..+12V/unipolar
    #      square-wave modulator output, so its descriptor evidence anchor must be the output spec
    #      L158 (same as the LFO output). Reverting L158 -> L346 (which only proves the CV OUT
    #      identity) must fail — the anchor no longer proves the confirmed electrical range.
    ja_bad = copy.deepcopy(spec)
    reg_jack(ja_bad, "drone_3.cv_out")["evidence"]["line"] = 346
    problems, _ = gate.check(ja_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.cv_out'"):
        raise SystemExit("drone_3.cv_out descriptorEvidence.line drift (158 -> 346) not enforced: %r"
                        % problems)

    # (jb) drone_3.env_out descriptorEvidence.line drift: env_out is the confirmed -10..+10V/bipolar
    #      ENV VOICES output, so its descriptor evidence anchor must be L157 (ENV VOICES lists voice 3).
    #      Reverting L157 -> L334 (which only proves the envelope output identity) must fail.
    jb_bad = copy.deepcopy(spec)
    reg_jack(jb_bad, "drone_3.env_out")["evidence"]["line"] = 334
    problems, _ = gate.check(jb_bad, manifest)
    if not has(problems, "implementation jack 'drone_3.env_out'"):
        raise SystemExit("drone_3.env_out descriptorEvidence.line drift (157 -> 334) not enforced: %r"
                        % problems)

    # ---- DRONE 6 "Papa Srapa" voice slice (Codex msg b121ba1b) --------------
    # SAME group structure as DRONE 3 (12 params ids 289-300, 5 jacks ids 60-64, module id 20,
    # mustComplete 346->364), but the CV OUT boundary is the OPPOSITE: manual L158 "LFOs: 0…+10V
    # VOICE 3, 5 MODULATOR: 0….+12V" does NOT list voice 6, so drone_6.cv_out is identity-only — its
    # nominalRange and polarity are UNVERIFIED/unknown (numeric placeholder 0..12, no voltage asserted),
    # NOT the confirmed 0..+12V/unipolar DRONE 3 carries. So the cv_out negatives are the reverse-inference
    # OVER-CLAIMS (asserting unipolar / confirming the range from L158 for a voice L158 does not name).
    # env_out IS still confirmed -10..+10V/bipolar (L157 "ENV VOICES 1,2,3,6,7,8" DOES list voice 6).
    # The 5V gate is produced by the keyboard (buttons 3 and 6, L331-332), NOT the gate_in rail, so
    # gate_in polarity unipolar is a mandated-negative over-claim; gate_in signalType is confirmed gate.
    # clock_in (clock sets S&H speed) and noise_in (only the S&H/IN identity) have unverified electrical
    # characteristics. Selector domains: hi_low=[hi,low]; fm/am/rate_switch/hold=[off,on] (all-six
    # provisional). Continuous params are software-normalized 0..1 placeholders. Wrong if the group
    # cardinality drifts, if any id/name mapping renumbers or renames a member, if a continuous param
    # loses its 0..1 placeholder provenance, if a selector loses its value domain, if a jack's range /
    # type / polarity / evidence drifts, if the cv_out range or polarity is OVER-CLAIMED (L158 lists 3,5
    # not 6), or if a drone_6 member leaks a drone_1 stable_id. Each must FAIL normal for its own reason.

    # (ka) module delete: pulling the whole landed module (id 20) out of the registry must fail.
    ka_bad = copy.deepcopy(spec)
    ka_bad["modules"] = [m for m in ka_bad["modules"] if m.get("stable_id") != "drone_6"]
    problems, _ = gate.check(ka_bad, manifest)
    if not has(problems, "MISSING landed module 'drone_6'"):
        raise SystemExit("drone_6 module delete not enforced: %r" % problems)

    # (kb) module id renumber: drone_6 is a landed module with id 20; renumbering (20 -> 999) must fail.
    kb_bad = copy.deepcopy(spec)
    for m in kb_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["id"] = 999
    problems, _ = gate.check(kb_bad, manifest)
    if not has(problems, "implementation module 'drone_6': id"):
        raise SystemExit("drone_6 module id renumber (20 -> 999) not enforced: %r" % problems)

    # (kc) module target name drift: the registry module name stays in lock-step with target.modules
    #      ('DRONE 6'); a silent rename is a target drift.
    kc_bad = copy.deepcopy(spec)
    for m in kc_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["name"] = "DRONE SIX"
    problems, _ = gate.check(kc_bad, manifest)
    if not has(problems, "implementation module 'drone_6': name"):
        raise SystemExit("drone_6 module name drift ('DRONE 6' -> 'DRONE SIX') not enforced: %r"
                        % problems)

    # (kd) module evidence drift: the module spans L329-369; moving lineStart is a target drift.
    kd_bad = copy.deepcopy(spec)
    for m in kd_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["evidence"]["lineStart"] = 330
    problems, _ = gate.check(kd_bad, manifest)
    if not has(problems, "implementation module 'drone_6': evidence.lineStart"):
        raise SystemExit("drone_6 module evidence.lineStart drift (329 -> 330) not enforced: %r"
                        % problems)

    # (ke) 7-continuous cardinality - FIRST generator: deleting drone_6.rate must fail.
    ke_bad = copy.deepcopy(spec)
    for m in ke_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_6.rate"]
    problems, _ = gate.check(ke_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_6.rate'"):
        raise SystemExit("drone_6 first-continuous delete (drone_6.rate) not enforced: %r" % problems)

    # (kf) 7-continuous cardinality - MIDDLE generator: deleting drone_6.pitch must fail.
    kf_bad = copy.deepcopy(spec)
    for m in kf_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_6.pitch"]
    problems, _ = gate.check(kf_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_6.pitch'"):
        raise SystemExit("drone_6 middle-continuous delete (drone_6.pitch) not enforced: %r" % problems)

    # (kg) 7-continuous cardinality - LAST generator: deleting drone_6.rls must fail.
    kg_bad = copy.deepcopy(spec)
    for m in kg_bad["modules"]:
        if m.get("stable_id") == "drone_6":
            m["parameters"] = [p for p in m.get("parameters", [])
                               if p.get("stable_id") != "drone_6.rls"]
    problems, _ = gate.check(kg_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'drone_6.rls'"):
        raise SystemExit("drone_6 last-continuous delete (drone_6.rls) not enforced: %r" % problems)

    # (kh) ID mapping: renumber a landed continuous param id (drone_6.rate 289 -> 999) must fail.
    kh_bad = copy.deepcopy(spec)
    reg_param(kh_bad, "drone_6.rate")["id"] = 999
    problems, _ = gate.check(kh_bad, manifest)
    if not has(problems, "implementation param 'drone_6.rate'"):
        raise SystemExit("drone_6 param id renumber (rate 289 -> 999) not enforced: %r" % problems)

    # (ki) NAME mapping: a landed selector param must keep its exact label ('RANGE'); a silent rename
    #      is a target drift.
    ki_bad = copy.deepcopy(spec)
    reg_param(ki_bad, "drone_6.hi_low")["name"] = "RANGE SW"
    problems, _ = gate.check(ki_bad, manifest)
    if not has(problems, "implementation param 'drone_6.hi_low'"):
        raise SystemExit("drone_6 param name drift (hi_low 'RANGE' -> 'RANGE SW') not enforced: %r"
                        % problems)

    # (kj) hi_low selector positions drift: RANGE is hi/low; reordering the domain is a re-mapping and
    #      must fail.
    kj_bad = copy.deepcopy(spec)
    reg_param(kj_bad, "drone_6.hi_low")["positions"] = ["low", "hi"]
    problems, _ = gate.check(kj_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_6.hi_low selector positions drift (['hi','low'] -> ['low','hi']) "
                        "not enforced: %r" % problems)

    # (kk) FM selector positions drift: fm is the off/on toggle; its domain must not move.
    kk_bad = copy.deepcopy(spec)
    reg_param(kk_bad, "drone_6.fm")["positions"] = ["on", "off"]
    problems, _ = gate.check(kk_bad, manifest)
    if not has(problems, "registry selector options"):
        raise SystemExit("drone_6.fm selector positions drift (['off','on'] -> ['on','off']) "
                        "not enforced: %r" % problems)

    # (kl) RATE descriptorEvidence.line drift: RATE CONTROL cites L345; moving it must fail.
    kl_bad = copy.deepcopy(spec)
    reg_param(kl_bad, "drone_6.rate")["evidence"]["line"] = 346
    problems, _ = gate.check(kl_bad, manifest)
    if not has(problems, "implementation param 'drone_6.rate'"):
        raise SystemExit("drone_6.rate descriptorEvidence.line drift (345 -> 346) not enforced: %r"
                        % problems)

    # (km) PITCH descriptorEvidence.line drift: PITCH KNOBS cites L347; moving it must fail.
    km_bad = copy.deepcopy(spec)
    reg_param(km_bad, "drone_6.pitch")["evidence"]["line"] = 348
    problems, _ = gate.check(km_bad, manifest)
    if not has(problems, "implementation param 'drone_6.pitch'"):
        raise SystemExit("drone_6.pitch descriptorEvidence.line drift (347 -> 348) not enforced: %r"
                        % problems)

    # (kn) HOLD descriptorEvidence.line drift: hold cites the panel HOLD label (L82); moving it must
    #      fail.
    kn_bad = copy.deepcopy(spec)
    reg_param(kn_bad, "drone_6.hold")["evidence"]["line"] = 83
    problems, _ = gate.check(kn_bad, manifest)
    if not has(problems, "implementation param 'drone_6.hold'"):
        raise SystemExit("drone_6.hold descriptorEvidence.line drift (82 -> 83) not enforced: %r"
                        % problems)

    # (ko) continuous placeholder provenance over-claim: RATE is a software-normalized 0..1 placeholder
    #      (all-six unverified); silently elevating fieldEvidence.range to confirmed is an evidence
    #      over-claim and must fail.
    ko_bad = copy.deepcopy(spec)
    reg_param(ko_bad, "drone_6.rate")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(ko_bad, manifest)
    if not has(problems, "implementation param 'drone_6.rate'"):
        raise SystemExit("drone_6.rate fieldEvidence over-claim (range unverified->confirmed) "
                        "not enforced: %r" % problems)

    # (kp) cv_out nominal range drift: the cursor cv_out placeholder is 0..12; drifting nominalMax must
    #      fail (a placeholder value is still locked, even with range evidence unverified).
    kp_bad = copy.deepcopy(spec)
    reg_jack(kp_bad, "drone_6.cv_out")["nominalMax"] = 13
    problems, _ = gate.check(kp_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.cv_out'"):
        raise SystemExit("drone_6.cv_out nominal range drift (12 -> 13) not enforced: %r" % problems)

    # (kq) cv_out polarity OVER-CLAIM (KEY DIFF): L158 "VOICE 3, 5 MODULATOR" does NOT list voice 6, so
    #      drone_6.cv_out polarity must stay unknown/unverified; reverse-inferring a concrete unipolar
    #      (as DRONE 3 lands) is a mandated-negative over-claim and must fail.
    kq_bad = copy.deepcopy(spec)
    reg_jack(kq_bad, "drone_6.cv_out")["polarity"] = "unipolar"
    problems, _ = gate.check(kq_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.cv_out'"):
        raise SystemExit("drone_6.cv_out polarity over-claim (unknown -> unipolar, L158 does not list "
                        "voice 6) not enforced: %r" % problems)

    # (kr) cv_out nominalRange evidence OVER-CLAIM (KEY DIFF): the range is not proven for voice 6, so
    #      nominalRange must stay unverified; confirming it must fail.
    kr_bad = copy.deepcopy(spec)
    reg_jack(kr_bad, "drone_6.cv_out")["fieldEvidence"]["nominalRange"] = "confirmed"
    problems, _ = gate.check(kr_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.cv_out'"):
        raise SystemExit("drone_6.cv_out nominalRange evidence over-claim (unverified->confirmed, L158 "
                        "does not list voice 6) not enforced: %r" % problems)

    # (ks) cv_out descriptorEvidence.line drift: the identity anchor is L346 (square-wave modulator /
    #      CV OUT). Reverting L346 -> L158 (the LFO range line that names voices 3 and 5, not 6) would
    #      falsely prove a range for voice 6 and must fail.
    ks_bad = copy.deepcopy(spec)
    reg_jack(ks_bad, "drone_6.cv_out")["evidence"]["line"] = 158
    problems, _ = gate.check(ks_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.cv_out'"):
        raise SystemExit("drone_6.cv_out descriptorEvidence.line drift (346 -> 158) not enforced: %r"
                        % problems)

    # (kt) env_out nominal range drift: env_out is the confirmed -10..+10V output; widening nominalMax
    #      must fail.
    kt_bad = copy.deepcopy(spec)
    reg_jack(kt_bad, "drone_6.env_out")["nominalMax"] = 11
    problems, _ = gate.check(kt_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.env_out'"):
        raise SystemExit("drone_6.env_out nominal range drift (10 -> 11) not enforced: %r" % problems)

    # (ku) env_out polarity drift (KEY DIFF): manual L157 DOES list voice 6, so env_out polarity is a
    #      CONFIRMED bipolar. Dropping it to unknown is an evidence downgrade and must fail.
    ku_bad = copy.deepcopy(spec)
    reg_jack(ku_bad, "drone_6.env_out")["polarity"] = "unknown"
    problems, _ = gate.check(ku_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.env_out'"):
        raise SystemExit("drone_6.env_out polarity drift (bipolar -> unknown) not enforced: %r"
                        % problems)

    # (kv) env_out nominalRange evidence downgrade (KEY DIFF): because L157 lists voice 6 the nominalRange
    #      evidence is CONFIRMED; silently downgrading it to unverified must fail.
    kv_bad = copy.deepcopy(spec)
    reg_jack(kv_bad, "drone_6.env_out")["fieldEvidence"]["nominalRange"] = "unverified"
    problems, _ = gate.check(kv_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.env_out'"):
        raise SystemExit("drone_6.env_out fieldEvidence downgrade (nominalRange confirmed->unverified) "
                        "not enforced: %r" % problems)

    # (kw) gate_in signalType drift: gate_in is a confirmed GATE input; reclassifying as CV is a type
    #      drift and must fail.
    kw_bad = copy.deepcopy(spec)
    reg_jack(kw_bad, "drone_6.gate_in")["signalType"] = "cv"
    problems, _ = gate.check(kw_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.gate_in'"):
        raise SystemExit("drone_6.gate_in signalType drift (gate -> cv) not enforced: %r" % problems)

    # (kx) gate_in polarity over-claim (KEY keyboard-5V reverse inference): the documented 5V gate is
    #      PRODUCED by the keyboard buttons (L331-332), NOT by this input rail, so gate_in polarity stays
    #      unknown/unverified. Reverse-inferring unipolar is a mandated-negative over-claim and must fail.
    kx_bad = copy.deepcopy(spec)
    reg_jack(kx_bad, "drone_6.gate_in")["polarity"] = "unipolar"
    problems, _ = gate.check(kx_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.gate_in'"):
        raise SystemExit("drone_6.gate_in polarity over-claim (unknown -> unipolar, keyboard-5V "
                        "reverse inference) not enforced: %r" % problems)

    # (ky) clock_in signalType drift: clock_in is a confirmed CLOCK input; reclassifying as CV is a type
    #      drift and must fail.
    ky_bad = copy.deepcopy(spec)
    reg_jack(ky_bad, "drone_6.clock_in")["signalType"] = "cv"
    problems, _ = gate.check(ky_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.clock_in'"):
        raise SystemExit("drone_6.clock_in signalType drift (clock -> cv) not enforced: %r" % problems)

    # (kz) cross-owner leak: a drone_6 descriptor member must not be re-owner'd to a drone_1 stable_id
    #      (a copy-paste from the DRONE 1 slice). Renaming drone_6.att to drone_1.att must fail — the
    #      gate catches it as an enumerator collision (two members resolve to drone_1_att).
    kz_bad = copy.deepcopy(spec)
    reg_param(kz_bad, "drone_6.att")["stable_id"] = "drone_1.att"
    problems, _ = gate.check(kz_bad, manifest)
    if not has(problems, "enumerator collision"):
        raise SystemExit("drone_6 cross-owner leak (att -> drone_1.att) not enforced: %r" % problems)

    # (la) drone_6.env_out descriptorEvidence.line drift: env_out is the confirmed -10..+10V/bipolar
    #      ENV VOICES output, so its descriptor evidence anchor must be L157 (ENV VOICES lists voice 6).
    #      Reverting L157 -> L334 (which only proves the envelope output identity) must fail.
    la_bad = copy.deepcopy(spec)
    reg_jack(la_bad, "drone_6.env_out")["evidence"]["line"] = 334
    problems, _ = gate.check(la_bad, manifest)
    if not has(problems, "implementation jack 'drone_6.env_out'"):
        raise SystemExit("drone_6.env_out descriptorEvidence.line drift (157 -> 334) not enforced: %r"
                        % problems)

    # (lb) patch-capacity gate (Codex msg cc68ab2b Option A — "exactly 65, not 96/128"): with the
    #      frozen patchable-jack inventory FULL, the C++ patch capacity must EXACTLY equal the
    #      serialized jack id-space. A headroom drift to 96 (the "not rounded to 96/128" regression
    #      Codex ruled against) must be caught. The Python fixture cannot mutate the C++ header
    #      constant directly, so the honest way to force the drift is to point the checker's header
    #      read at a temp header carrying the drifted constant; the data manifest stays untouched.
    #      This also proves the gate is >= -free (a cap of 96 would satisfy >= 65 but is still wrong).
    _cap_hpp_saved = gate.CAPS_HPP
    _cap_tmp = tempfile.NamedTemporaryFile("w", suffix=".h", delete=False)
    _cap_tmp.write("#pragma once\n#include <cstddef>\nnamespace lunar24::core {\n"
                   "inline constexpr std::size_t kDevicePatchCapacity = 96;\n}\n")
    _cap_tmp.close()
    try:
        gate.CAPS_HPP = _cap_tmp.name
        problems_cap, _ = gate.check(spec, manifest)
    finally:
        gate.CAPS_HPP = _cap_hpp_saved
        os.unlink(_cap_tmp.name)
    if not has(problems_cap, "must EXACTLY equal the jack id-space kJackIdSpace"):
        raise SystemExit("patch capacity drifted to 96 (headroom) not flagged by the == id-space "
                         "gate: %r" % problems_cap)

    # (lc) missing kDevicePatchCapacity constant (Codex msg cce4fa09 item #1 — "FAIL closed, never
    #      pass open"): if the capacity constant is deleted or becomes unreadable to the reader,
    #      the gate must report a hard problem, not silently pass. The Python fixture cannot delete
    #      the header constant, so point the checker's header read at a temp header that omits it;
    #      the reader returns None and the gate must flag the missing constant.
    _cap_hpp_saved = gate.CAPS_HPP
    _cap_tmp = tempfile.NamedTemporaryFile("w", suffix=".h", delete=False)
    _cap_tmp.write("#pragma once\n#include <cstddef>\nnamespace lunar24::core {\n}\n")
    _cap_tmp.close()
    try:
        gate.CAPS_HPP = _cap_tmp.name
        problems_cap_miss, _ = gate.check(spec, manifest)
    finally:
        gate.CAPS_HPP = _cap_hpp_saved
        os.unlink(_cap_tmp.name)
    if not has(problems_cap_miss, "cannot read the patch-bank capacity gate constant"):
        raise SystemExit("missing kDevicePatchCapacity constant not flagged (fail-open): %r"
                         % problems_cap_miss)

    # (ld) missing kJackIdSpace constant — the same fail-closed requirement on the id-space side.
    _ids_hpp_saved = gate.IDS_HPP
    _ids_tmp = tempfile.NamedTemporaryFile("w", suffix=".h", delete=False)
    _ids_tmp.write("#pragma once\n#include <cstddef>\nnamespace lunar24::core {\n}\n")
    _ids_tmp.close()
    try:
        gate.IDS_HPP = _ids_tmp.name
        problems_space_miss, _ = gate.check(spec, manifest)
    finally:
        gate.IDS_HPP = _ids_hpp_saved
        os.unlink(_ids_tmp.name)
    if not has(problems_space_miss, "cannot read the patch-bank capacity gate constant"):
        raise SystemExit("missing kJackIdSpace constant not flagged (fail-open): %r"
                         % problems_space_miss)

    # (le)-(ll) Cathedral Program 2/3 X/Y/Z descriptor lock (Codex msg 3cad5c27). The six program
    #      cathedral.2/.3 x/y/z params (ids 301-306) are landed descriptor facts locked by the same
    #      exact-compare as module params — so id renumber, owner / cross-owner leak, per-line evidence
    #      drift, and a fieldEvidence overclaim (unverified -> confirmed) must all fail normal. Unlike
    #      the classic/generator voices, cathedral.2/.3 are program X/Y/Z params, so their ROLE is
    #      also locked: a program x/y/z param's role must equal its stable-id .x/.y/.z suffix (it is
    #      NOT a free choice of a valid enum name). A deletion of either terminator (cathedral.2.x
    #      first, cathedral.3.z last) is a landed/mustComplete drop, not a reopenable gap.

    # (le) id renumber: cathedral.2.x is id 301; renumbering (301 -> 999) must fail.
    le_bad = copy.deepcopy(spec)
    reg_param(le_bad, "program.cathedral.2.x")["id"] = 999
    problems, _ = gate.check(le_bad, manifest)
    if not has(problems, "implementation param 'program.cathedral.2.x'"):
        raise SystemExit("cathedral.2.x param id renumber (301 -> 999) not enforced: %r" % problems)

    # (lf) owner / cross-owner leak: cathedral.2.x must stay owned by program.cathedral.2. Moving it
    #      under program.cathedral.3's parameter array makes the generated param owner
    #      program.cathedral.3 (the carrying program), which the landed fact rejects.
    lf_bad = copy.deepcopy(spec)
    _c2 = next(pr for pr in lf_bad["programs"] if pr.get("stable_id") == "program.cathedral.2")
    _c3 = next(pr for pr in lf_bad["programs"] if pr.get("stable_id") == "program.cathedral.3")
    _x = [p for p in _c2["parameters"] if p["stable_id"] == "program.cathedral.2.x"][0]
    _c2["parameters"] = [p for p in _c2["parameters"] if p["stable_id"] != "program.cathedral.2.x"]
    _c3.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(lf_bad, manifest)
    if not has(problems, "implementation param 'program.cathedral.2.x'"):
        raise SystemExit("cathedral.2.x cross-owner leak (-> program.cathedral.3) not enforced: %r"
                         % problems)

    # (lg) ROLE drift: a program X/Y/Z param's role is part of its locked identity and must equal the
    #      stable-id suffix. cathedral.2.x being role y (a valid enum, but the wrong position) must
    #      fail — this is the gate added for this slice (the landed exact-compare does not compare
    #      role, so a role mismatch is only caught here). needle is the role gate's own signature.
    lg_bad = copy.deepcopy(spec)
    reg_param(lg_bad, "program.cathedral.2.x")["role"] = "y"
    problems, _ = gate.check(lg_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("cathedral.2.x role drift (x -> y) not enforced by the role gate: %r"
                         % problems)

    # (lh) per-line descriptorEvidence drift: cathedral.2.x cites L1216; moving it (1216 -> 1217) is a
    #      target drift and must fail.
    lh_bad = copy.deepcopy(spec)
    reg_param(lh_bad, "program.cathedral.2.x")["evidence"]["line"] = 1217
    problems, _ = gate.check(lh_bad, manifest)
    if not has(problems, "implementation param 'program.cathedral.2.x'"):
        raise SystemExit("cathedral.2.x descriptorEvidence.line drift (1216 -> 1217) not enforced: "
                         "%r" % problems)

    # (li) fieldEvidence overclaim: a placeholder range must not be silently elevated to confirmed
    #      (the KEY DIFF — no physical feedback/reverb/delay unit/range/default is being asserted).
    li_bad = copy.deepcopy(spec)
    reg_param(li_bad, "program.cathedral.2.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(li_bad, manifest)
    if not has(problems, "implementation param 'program.cathedral.2.x'"):
        raise SystemExit("cathedral.2.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (lj) FIRST terminator delete: dropping cathedral.2.x is a landed/mustComplete drop, not a gap.
    lj_bad = copy.deepcopy(spec)
    _c2 = next(pr for pr in lj_bad["programs"] if pr.get("stable_id") == "program.cathedral.2")
    _c2["parameters"] = [p for p in _c2["parameters"] if p["stable_id"] != "program.cathedral.2.x"]
    problems, _ = gate.check(lj_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.cathedral.2.x'"):
        raise SystemExit("cathedral.2.x terminator delete (first) not enforced: %r" % problems)

    # (lk) LAST terminator delete: dropping cathedral.3.z is likewise a landed/mustComplete drop.
    lk_bad = copy.deepcopy(spec)
    _c3 = next(pr for pr in lk_bad["programs"] if pr.get("stable_id") == "program.cathedral.3")
    _c3["parameters"] = [p for p in _c3["parameters"] if p["stable_id"] != "program.cathedral.3.z"]
    problems, _ = gate.check(lk_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.cathedral.3.z'"):
        raise SystemExit("cathedral.3.z terminator delete (last) not enforced: %r" % problems)

    # (ll) cathedral.2/.3, magic.2/.3, time.1/.2/.3, vibrotrem.1/.2/.3, filter.1/.2/.3, vibe.1/.2/.3
    #      and pitch_shifter.1/.2/.3 must NOT be re-openable as an honest gap: with those programs'
    #      params landed, the --require-full residual must exclude every one of those ids. It widens
    #      exactly across the 12 keyboard complex + 54 other program XYZ params (66 total).
    _res_full, _ = gate.check(spec, manifest, require_full=True)
    _gap_lines = [_p for _p in _res_full if "parameter target-not-implemented" in _p]
    if len(_gap_lines) != 1:
        raise SystemExit("unexpected --require-full residual shape: %r" % _gap_lines)
    _txt = _gap_lines[0]
    _body = _txt[_txt.index("[") + 1:_txt.rindex("]")]
    _gap_ids = [x.strip().strip("'").strip('"') for x in _body.split(",")] if _body.strip() else []
    _kb = [x for x in _gap_ids if x.startswith("keyboard.")]
    _prog = [x for x in _gap_ids if x.startswith("program.")]
    if len(_gap_ids) != 66:
        raise SystemExit("residual honest-gap total %d != 66 (12 keyboard complex + 54 program XYZ); "
                         "cathedral.2/.3 + magic.2/.3 + time.1/.2/.3 + vibrotrem.1/.2/.3 + "
                         "filter.1/.2/.3 + vibe.1/.2/.3 + pitch_shifter.1/.2/.3 must have landed and "
                         "closed exactly 57" % len(_gap_ids))
    if len(_kb) != 12 or len(_prog) != 54:
        raise SystemExit("residual split keyboard=%d program=%d != 12/54: %r" % (len(_kb), len(_prog),
                                                                                _gap_ids))
    for _sid in ("program.cathedral.2.x", "program.cathedral.2.y", "program.cathedral.2.z",
                 "program.cathedral.3.x", "program.cathedral.3.y", "program.cathedral.3.z",
                 "program.magic.2.x", "program.magic.2.y", "program.magic.2.z",
                 "program.magic.3.x", "program.magic.3.y", "program.magic.3.z",
                 "program.time.1.x", "program.time.1.y", "program.time.1.z",
                 "program.time.2.x", "program.time.2.y", "program.time.2.z",
                 "program.time.3.x", "program.time.3.y", "program.time.3.z",
                 "program.vibrotrem.1.x", "program.vibrotrem.1.y", "program.vibrotrem.1.z",
                 "program.vibrotrem.2.x", "program.vibrotrem.2.y", "program.vibrotrem.2.z",
                 "program.vibrotrem.3.x", "program.vibrotrem.3.y", "program.vibrotrem.3.z",
                 "program.filter.1.x", "program.filter.1.y", "program.filter.1.z",
                 "program.filter.2.x", "program.filter.2.y", "program.filter.2.z",
                 "program.filter.3.x", "program.filter.3.y", "program.filter.3.z",
                 "program.vibe.1.x", "program.vibe.1.y", "program.vibe.1.z",
                 "program.vibe.2.x", "program.vibe.2.y", "program.vibe.2.z",
                 "program.vibe.3.x", "program.vibe.3.y", "program.vibe.3.z",
                 "program.pitch_shifter.1.x", "program.pitch_shifter.1.y", "program.pitch_shifter.1.z",
                 "program.pitch_shifter.2.x", "program.pitch_shifter.2.y", "program.pitch_shifter.2.z",
                 "program.pitch_shifter.3.x", "program.pitch_shifter.3.y", "program.pitch_shifter.3.z"):
        if _sid in _gap_ids:
            raise SystemExit("landed program param %s re-opened as an honest gap (mandate: the 54 "
                             "other program XYZ gaps are preserved, NOT cathedral.2/3, magic.2/3, "
                             "time.1/2/3, vibrotrem.1/2/3, filter.1/2/3, vibe.1/2/3 or "
                             "pitch_shifter.1/2/3): %r" % (_sid, _gap_ids))
    for _sid in ("program.cathedral.1.x", "program.cathedral.1.y", "program.cathedral.1.z",
                 "program.magic.1.x", "program.magic.1.y", "program.magic.1.z"):
        if _sid in _gap_ids:
            raise SystemExit("already-landed program param %s incorrectly left as a gap: %r"
                             % (_sid, _gap_ids))

    # (lm)-(lt) MAGIC Program 2/3 X/Y/Z descriptor lock (Codex msg 62370964). The six program
    #      magic.2/.3 x/y/z params (ids 307-312) are landed descriptor facts locked by the exact-
    #      compare — id renumber, owner / cross-owner leak, per-line evidence drift and a fieldEvidence
    #      overclaim must all fail normal. ROLE is also locked (a program x/y/z param's role must equal
    #      its stable-id .x/.y/.z suffix), and a terminator delete is a landed/mustComplete drop. KEY
    #      DIFF vs cathedral: the PLACEHOLDER values (unit=norm, 0..1, default 0) are chosen so that NO
    #      feedback/delay/pitch unit, range or default is asserted — MAGIC 1's concrete millisecond /
    #      semitone values are deliberately NOT reused here.

    # (lm) id renumber: magic.2.x is id 307; renumbering (307 -> 999) must fail.
    lm_bad = copy.deepcopy(spec)
    reg_param(lm_bad, "program.magic.2.x")["id"] = 999
    problems, _ = gate.check(lm_bad, manifest)
    if not has(problems, "implementation param 'program.magic.2.x'"):
        raise SystemExit("magic.2.x param id renumber (307 -> 999) not enforced: %r" % problems)

    # (ln) owner / cross-owner leak: magic.2.x must stay owned by program.magic.2; moving it under
    #      program.magic.3's array makes the generated owner program.magic.3, which the landed fact
    #      rejects.
    ln_bad = copy.deepcopy(spec)
    _m2 = next(pr for pr in ln_bad["programs"] if pr.get("stable_id") == "program.magic.2")
    _m3 = next(pr for pr in ln_bad["programs"] if pr.get("stable_id") == "program.magic.3")
    _x = [p for p in _m2["parameters"] if p["stable_id"] == "program.magic.2.x"][0]
    _m2["parameters"] = [p for p in _m2["parameters"] if p["stable_id"] != "program.magic.2.x"]
    _m3.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(ln_bad, manifest)
    if not has(problems, "implementation param 'program.magic.2.x'"):
        raise SystemExit("magic.2.x cross-owner leak (-> program.magic.3) not enforced: %r"
                         % problems)

    # (lp) ROLE drift: magic.2.x being role y (valid but wrong position) must fail (role gate).
    lp_bad = copy.deepcopy(spec)
    reg_param(lp_bad, "program.magic.2.x")["role"] = "y"
    problems, _ = gate.check(lp_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("magic.2.x role drift (x -> y) not enforced by the role gate: %r"
                         % problems)

    # (lq) per-line descriptorEvidence drift: magic.2.x cites L1217; moving it (1217 -> 1218) is a
    #      target drift and must fail.
    lq_bad = copy.deepcopy(spec)
    reg_param(lq_bad, "program.magic.2.x")["evidence"]["line"] = 1218
    problems, _ = gate.check(lq_bad, manifest)
    if not has(problems, "implementation param 'program.magic.2.x'"):
        raise SystemExit("magic.2.x descriptorEvidence.line drift (1217 -> 1218) not enforced: "
                         "%r" % problems)

    # (lr) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical feedback/delay/pitch unit/range/default is being asserted here).
    lr_bad = copy.deepcopy(spec)
    reg_param(lr_bad, "program.magic.2.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(lr_bad, manifest)
    if not has(problems, "implementation param 'program.magic.2.x'"):
        raise SystemExit("magic.2.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (ls) FIRST terminator delete: dropping magic.2.x is a landed/mustComplete drop, not a gap.
    ls_bad = copy.deepcopy(spec)
    _m2 = next(pr for pr in ls_bad["programs"] if pr.get("stable_id") == "program.magic.2")
    _m2["parameters"] = [p for p in _m2["parameters"] if p["stable_id"] != "program.magic.2.x"]
    problems, _ = gate.check(ls_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.magic.2.x'"):
        raise SystemExit("magic.2.x terminator delete (first) not enforced: %r" % problems)

    # (lt) LAST terminator delete: dropping magic.3.z is likewise a landed/mustComplete drop.
    lt_bad = copy.deepcopy(spec)
    _m3 = next(pr for pr in lt_bad["programs"] if pr.get("stable_id") == "program.magic.3")
    _m3["parameters"] = [p for p in _m3["parameters"] if p["stable_id"] != "program.magic.3.z"]
    problems, _ = gate.check(lt_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.magic.3.z'"):
        raise SystemExit("magic.3.z terminator delete (last) not enforced: %r" % problems)

    # (lu)-(mb) TIME Program 1/2/3 X/Y/Z descriptor lock (Codex msg 7cafb3d6). The nine program
    #      time.1/.2/.3 x/y/z params (ids 313-321) are landed descriptor facts locked by the exact-
    #      compare — id renumber, owner / cross-owner leak, per-line evidence drift and a fieldEvidence
    #      overclaim must all fail normal. ROLE is also locked (a program x/y/z param's role must equal
    #      its stable-id .x/.y/.z suffix), and ALL three terminator shapes across the whole TIME family
    #      (first time.1.x / middle time.2.y / last time.3.z) are a landed/mustComplete drop, not a gap.
    #      KEY DIFF vs the other program slices: no feedback/delay/reverb/mod-depth/vibrato-rate physical
    #      unit/range/default is asserted — the labels are NOT used to derive time/frequency values.

    # (lu) id renumber: time.1.x is id 313; renumbering (313 -> 999) must fail.
    lu_bad = copy.deepcopy(spec)
    reg_param(lu_bad, "program.time.1.x")["id"] = 999
    problems, _ = gate.check(lu_bad, manifest)
    if not has(problems, "implementation param 'program.time.1.x'"):
        raise SystemExit("time.1.x param id renumber (313 -> 999) not enforced: %r" % problems)

    # (lv) owner / cross-owner leak: time.1.x must stay owned by program.time.1; moving it under
    #      program.time.2's array makes the generated owner program.time.2, which the landed fact rejects.
    lv_bad = copy.deepcopy(spec)
    _t1 = next(pr for pr in lv_bad["programs"] if pr.get("stable_id") == "program.time.1")
    _t2 = next(pr for pr in lv_bad["programs"] if pr.get("stable_id") == "program.time.2")
    _x = [p for p in _t1["parameters"] if p["stable_id"] == "program.time.1.x"][0]
    _t1["parameters"] = [p for p in _t1["parameters"] if p["stable_id"] != "program.time.1.x"]
    _t2.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(lv_bad, manifest)
    if not has(problems, "implementation param 'program.time.1.x'"):
        raise SystemExit("time.1.x cross-owner leak (-> program.time.2) not enforced: %r" % problems)

    # (lw) ROLE drift: time.1.x being role y (valid but wrong position) must fail (role gate).
    lw_bad = copy.deepcopy(spec)
    reg_param(lw_bad, "program.time.1.x")["role"] = "y"
    problems, _ = gate.check(lw_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("time.1.x role drift (x -> y) not enforced by the role gate: %r" % problems)

    # (lx) per-line descriptorEvidence drift: time.1.x cites L1211; moving it (1211 -> 1212) is a
    #      target drift and must fail.
    lx_bad = copy.deepcopy(spec)
    reg_param(lx_bad, "program.time.1.x")["evidence"]["line"] = 1212
    problems, _ = gate.check(lx_bad, manifest)
    if not has(problems, "implementation param 'program.time.1.x'"):
        raise SystemExit("time.1.x descriptorEvidence.line drift (1211 -> 1212) not enforced: %r"
                         % problems)

    # (ly) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical feedback/delay/reverb/mod-depth/vibrato-rate unit/range/default is being
    #      asserted here).
    ly_bad = copy.deepcopy(spec)
    reg_param(ly_bad, "program.time.1.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(ly_bad, manifest)
    if not has(problems, "implementation param 'program.time.1.x'"):
        raise SystemExit("time.1.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (lz) FIRST terminator delete across the family: dropping time.1.x is a landed/mustComplete drop.
    lz_bad = copy.deepcopy(spec)
    _t1 = next(pr for pr in lz_bad["programs"] if pr.get("stable_id") == "program.time.1")
    _t1["parameters"] = [p for p in _t1["parameters"] if p["stable_id"] != "program.time.1.x"]
    problems, _ = gate.check(lz_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.time.1.x'"):
        raise SystemExit("time.1.x terminator delete (family first) not enforced: %r" % problems)

    # (ma) MIDDLE terminator delete across the family: dropping time.2.y is likewise a landed/mustComplete
    #      drop.
    ma_bad = copy.deepcopy(spec)
    _t2 = next(pr for pr in ma_bad["programs"] if pr.get("stable_id") == "program.time.2")
    _t2["parameters"] = [p for p in _t2["parameters"] if p["stable_id"] != "program.time.2.y"]
    problems, _ = gate.check(ma_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.time.2.y'"):
        raise SystemExit("time.2.y terminator delete (family middle) not enforced: %r" % problems)

    # (mb) LAST terminator delete across the family: dropping time.3.z is likewise a landed/mustComplete
    #      drop.
    mb_bad = copy.deepcopy(spec)
    _t3 = next(pr for pr in mb_bad["programs"] if pr.get("stable_id") == "program.time.3")
    _t3["parameters"] = [p for p in _t3["parameters"] if p["stable_id"] != "program.time.3.z"]
    problems, _ = gate.check(mb_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.time.3.z'"):
        raise SystemExit("time.3.z terminator delete (family last) not enforced: %r" % problems)

    # (mc)-(mj) VIBROTREM Program 1/2/3 X/Y/Z descriptor lock (Codex msg 2a7c2e94). The nine program
    #      vibrotrem.1/.2/.3 x/y/z params (ids 322-330) are landed descriptor facts locked by the exact-
    #      compare — id renumber, owner / cross-owner leak, per-line evidence drift and a fieldEvidence
    #      overclaim must all fail normal. ROLE is also locked (a program x/y/z param's role must equal
    #      its stable-id .x/.y/.z suffix), and ALL three terminator shapes across the whole VIBROTREM
    #      family (first vibrotrem.1.x / middle vibrotrem.2.y / last vibrotrem.3.z) are a landed/
    #      mustComplete drop, not a gap. KEY DIFF vs the other program slices: no depth/rate/reverb
    #      physical unit/range/default is derived from the Tremolo/Vibrato/Chorus names or the
    #      Depth/Rate/Reverb labels — the software-normalized 0..1 placeholders are NOT dressed up as
    #      parameterized values.

    # (mc) id renumber: vibrotrem.1.x is id 322; renumbering (322 -> 999) must fail.
    mc_bad = copy.deepcopy(spec)
    reg_param(mc_bad, "program.vibrotrem.1.x")["id"] = 999
    problems, _ = gate.check(mc_bad, manifest)
    if not has(problems, "implementation param 'program.vibrotrem.1.x'"):
        raise SystemExit("vibrotrem.1.x param id renumber (322 -> 999) not enforced: %r" % problems)

    # (md) owner / cross-owner leak: vibrotrem.1.x must stay owned by program.vibrotrem.1; moving it
    #      under program.vibrotrem.2's array makes the generated owner program.vibrotrem.2, which the
    #      landed fact rejects.
    md_bad = copy.deepcopy(spec)
    _v1 = next(pr for pr in md_bad["programs"] if pr.get("stable_id") == "program.vibrotrem.1")
    _v2 = next(pr for pr in md_bad["programs"] if pr.get("stable_id") == "program.vibrotrem.2")
    _x = [p for p in _v1["parameters"] if p["stable_id"] == "program.vibrotrem.1.x"][0]
    _v1["parameters"] = [p for p in _v1["parameters"] if p["stable_id"] != "program.vibrotrem.1.x"]
    _v2.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(md_bad, manifest)
    if not has(problems, "implementation param 'program.vibrotrem.1.x'"):
        raise SystemExit("vibrotrem.1.x cross-owner leak (-> program.vibrotrem.2) not enforced: %r"
                         % problems)

    # (me) ROLE drift: vibrotrem.1.x being role y (valid but wrong position) must fail (role gate).
    me_bad = copy.deepcopy(spec)
    reg_param(me_bad, "program.vibrotrem.1.x")["role"] = "y"
    problems, _ = gate.check(me_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("vibrotrem.1.x role drift (x -> y) not enforced by the role gate: %r"
                         % problems)

    # (mf) per-line descriptorEvidence drift: vibrotrem.1.x cites L1233; moving it (1233 -> 1234) is a
    #      target drift and must fail.
    mf_bad = copy.deepcopy(spec)
    reg_param(mf_bad, "program.vibrotrem.1.x")["evidence"]["line"] = 1234
    problems, _ = gate.check(mf_bad, manifest)
    if not has(problems, "implementation param 'program.vibrotrem.1.x'"):
        raise SystemExit("vibrotrem.1.x descriptorEvidence.line drift (1233 -> 1234) not enforced: %r"
                         % problems)

    # (mg) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical depth/rate/reverb unit/range/default is being asserted here).
    mg_bad = copy.deepcopy(spec)
    reg_param(mg_bad, "program.vibrotrem.1.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(mg_bad, manifest)
    if not has(problems, "implementation param 'program.vibrotrem.1.x'"):
        raise SystemExit("vibrotrem.1.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (mh) FIRST terminator delete across the family: dropping vibrotrem.1.x is a landed/mustComplete
    #      drop.
    mh_bad = copy.deepcopy(spec)
    _v1 = next(pr for pr in mh_bad["programs"] if pr.get("stable_id") == "program.vibrotrem.1")
    _v1["parameters"] = [p for p in _v1["parameters"] if p["stable_id"] != "program.vibrotrem.1.x"]
    problems, _ = gate.check(mh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibrotrem.1.x'"):
        raise SystemExit("vibrotrem.1.x terminator delete (family first) not enforced: %r" % problems)

    # (mi) MIDDLE terminator delete across the family: dropping vibrotrem.2.y is likewise a landed/
    #      mustComplete drop.
    mi_bad = copy.deepcopy(spec)
    _v2 = next(pr for pr in mi_bad["programs"] if pr.get("stable_id") == "program.vibrotrem.2")
    _v2["parameters"] = [p for p in _v2["parameters"] if p["stable_id"] != "program.vibrotrem.2.y"]
    problems, _ = gate.check(mi_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibrotrem.2.y'"):
        raise SystemExit("vibrotrem.2.y terminator delete (family middle) not enforced: %r" % problems)

    # (mj) LAST terminator delete across the family: dropping vibrotrem.3.z is likewise a landed/
    #      mustComplete drop.
    mj_bad = copy.deepcopy(spec)
    _v3 = next(pr for pr in mj_bad["programs"] if pr.get("stable_id") == "program.vibrotrem.3")
    _v3["parameters"] = [p for p in _v3["parameters"] if p["stable_id"] != "program.vibrotrem.3.z"]
    problems, _ = gate.check(mj_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibrotrem.3.z'"):
        raise SystemExit("vibrotrem.3.z terminator delete (family last) not enforced: %r" % problems)

    # (mk)-(mr) FILTER Program 1/2/3 X/Y/Z descriptor lock (Codex msg 8f5f8a22). The nine program
    #      filter.1/.2/.3 x/y/z params (ids 331-339) are landed descriptor facts locked by the exact-
    #      compare — id renumber, owner / cross-owner leak, per-line evidence drift and a fieldEvidence
    #      overclaim must all fail normal. ROLE is also locked (a program x/y/z param's role must equal
    #      its stable-id .x/.y/.z suffix), and ALL three terminator shapes across the whole FILTER
    #      family (first filter.1.x / middle filter.2.y / last filter.3.z) are a landed/
    #      mustComplete drop, not a gap. KEY DIFF vs the other program slices: no Hz/Q/dB/taper
    #      physical unit/range/default is derived from the Auto Wah/HP-LP/Notch program names or the
    #      Filter amount/Envelope/HP cutoff/LP cutoff/Resonance/Cut 1/Cut 2 labels — the software-
    #      normalized 0..1 placeholders are NOT dressed up as parameterized values (L1234 OCR read
    #      "Evelope"; written per frozen target semanticLabel "Envelope").

    # (mk) id renumber: filter.1.x is id 331; renumbering (331 -> 999) must fail.
    mk_bad = copy.deepcopy(spec)
    reg_param(mk_bad, "program.filter.1.x")["id"] = 999
    problems, _ = gate.check(mk_bad, manifest)
    if not has(problems, "implementation param 'program.filter.1.x'"):
        raise SystemExit("filter.1.x param id renumber (331 -> 999) not enforced: %r" % problems)

    # (ml) owner / cross-owner leak: filter.1.x must stay owned by program.filter.1; moving it
    #      under program.filter.2's array makes the generated owner program.filter.2, which the
    #      landed fact rejects.
    ml_bad = copy.deepcopy(spec)
    _f1 = next(pr for pr in ml_bad["programs"] if pr.get("stable_id") == "program.filter.1")
    _f2 = next(pr for pr in ml_bad["programs"] if pr.get("stable_id") == "program.filter.2")
    _x = [p for p in _f1["parameters"] if p["stable_id"] == "program.filter.1.x"][0]
    _f1["parameters"] = [p for p in _f1["parameters"] if p["stable_id"] != "program.filter.1.x"]
    _f2.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(ml_bad, manifest)
    if not has(problems, "implementation param 'program.filter.1.x'"):
        raise SystemExit("filter.1.x cross-owner leak (-> program.filter.2) not enforced: %r"
                         % problems)

    # (mm) ROLE drift: filter.1.x being role y (valid but wrong position) must fail (role gate).
    mm_bad = copy.deepcopy(spec)
    reg_param(mm_bad, "program.filter.1.x")["role"] = "y"
    problems, _ = gate.check(mm_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("filter.1.x role drift (x -> y) not enforced by the role gate: %r"
                         % problems)

    # (mn) per-line descriptorEvidence drift: filter.1.x cites L1233; moving it (1233 -> 1234) is a
    #      target drift and must fail.
    mn_bad = copy.deepcopy(spec)
    reg_param(mn_bad, "program.filter.1.x")["evidence"]["line"] = 1234
    problems, _ = gate.check(mn_bad, manifest)
    if not has(problems, "implementation param 'program.filter.1.x'"):
        raise SystemExit("filter.1.x descriptorEvidence.line drift (1233 -> 1234) not enforced: %r"
                         % problems)

    # (mo) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical Hz/Q/dB/taper unit/range/default is being asserted here).
    mo_bad = copy.deepcopy(spec)
    reg_param(mo_bad, "program.filter.1.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(mo_bad, manifest)
    if not has(problems, "implementation param 'program.filter.1.x'"):
        raise SystemExit("filter.1.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (mp) FIRST terminator delete across the family: dropping filter.1.x is a landed/mustComplete
    #      drop, not a gap.
    mp_bad = copy.deepcopy(spec)
    _f1 = next(pr for pr in mp_bad["programs"] if pr.get("stable_id") == "program.filter.1")
    _f1["parameters"] = [p for p in _f1["parameters"] if p["stable_id"] != "program.filter.1.x"]
    problems, _ = gate.check(mp_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.filter.1.x'"):
        raise SystemExit("filter.1.x terminator delete (family first) not enforced: %r" % problems)

    # (mq) MIDDLE terminator delete across the family: dropping filter.2.y is likewise a landed/
    #      mustComplete drop.
    mq_bad = copy.deepcopy(spec)
    _f2 = next(pr for pr in mq_bad["programs"] if pr.get("stable_id") == "program.filter.2")
    _f2["parameters"] = [p for p in _f2["parameters"] if p["stable_id"] != "program.filter.2.y"]
    problems, _ = gate.check(mq_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.filter.2.y'"):
        raise SystemExit("filter.2.y terminator delete (family middle) not enforced: %r" % problems)

    # (mr) LAST terminator delete across the family: dropping filter.3.z is likewise a landed/
    #      mustComplete drop.
    mr_bad = copy.deepcopy(spec)
    _f3 = next(pr for pr in mr_bad["programs"] if pr.get("stable_id") == "program.filter.3")
    _f3["parameters"] = [p for p in _f3["parameters"] if p["stable_id"] != "program.filter.3.z"]
    problems, _ = gate.check(mr_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.filter.3.z'"):
        raise SystemExit("filter.3.z terminator delete (family last) not enforced: %r" % problems)

    # (ms)-(mz) VIBE Program 1/2/3 X/Y/Z descriptor lock (Codex msg 1176d4f3). The nine program
    #      vibe.1/.2/.3 x/y/z params (ids 340-348) are landed descriptor facts locked by the exact-
    #      compare — id renumber, owner / cross-owner leak, per-line evidence drift and a fieldEvidence
    #      overclaim must all fail normal. ROLE is also locked (a program x/y/z param's role must equal
    #      its stable-id .x/.y/.z suffix), and a terminator delete is a landed/mustComplete drop. KEY
    #      DIFF vs filter/vibrotrem: the PLACEHOLDER values (unit=norm, 0..1, default 0) are chosen so
    #      that NO Phaser/Flanger/Resonance-flanger frequency, time or feedback unit, range or default
    #      is asserted — no values are derived from the Depth/Rate/Reverb/Resonance/Mod-depth labels.

    # (ms) id renumber: vibe.1.x is id 340; renumbering (340 -> 999) must fail.
    ms_bad = copy.deepcopy(spec)
    reg_param(ms_bad, "program.vibe.1.x")["id"] = 999
    problems, _ = gate.check(ms_bad, manifest)
    if not has(problems, "implementation param 'program.vibe.1.x'"):
        raise SystemExit("vibe.1.x param id renumber (340 -> 999) not enforced: %r" % problems)

    # (mt) owner / cross-owner leak: vibe.1.x must stay owned by program.vibe.1; moving it
    #      under program.vibe.2's array makes the generated owner program.vibe.2, which the landed
    #      fact rejects.
    mt_bad = copy.deepcopy(spec)
    _v1 = next(pr for pr in mt_bad["programs"] if pr.get("stable_id") == "program.vibe.1")
    _v2 = next(pr for pr in mt_bad["programs"] if pr.get("stable_id") == "program.vibe.2")
    _x = [p for p in _v1["parameters"] if p["stable_id"] == "program.vibe.1.x"][0]
    _v1["parameters"] = [p for p in _v1["parameters"] if p["stable_id"] != "program.vibe.1.x"]
    _v2.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(mt_bad, manifest)
    if not has(problems, "implementation param 'program.vibe.1.x'"):
        raise SystemExit("vibe.1.x cross-owner leak (-> program.vibe.2) not enforced: %r" % problems)

    # (mu) ROLE drift: vibe.1.x being role y (valid but wrong position) must fail (role gate).
    mu_bad = copy.deepcopy(spec)
    reg_param(mu_bad, "program.vibe.1.x")["role"] = "y"
    problems, _ = gate.check(mu_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("vibe.1.x role drift (x -> y) not enforced by the role gate: %r" % problems)

    # (mv) per-line descriptorEvidence drift: vibe.1.x cites L1233; moving it (1233 -> 1234) is a
    #      target drift and must fail.
    mv_bad = copy.deepcopy(spec)
    reg_param(mv_bad, "program.vibe.1.x")["evidence"]["line"] = 1234
    problems, _ = gate.check(mv_bad, manifest)
    if not has(problems, "implementation param 'program.vibe.1.x'"):
        raise SystemExit("vibe.1.x descriptorEvidence.line drift (1233 -> 1234) not enforced: %r"
                         % problems)

    # (mw) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical frequency/time/feedback unit/range/default is being asserted here).
    mw_bad = copy.deepcopy(spec)
    reg_param(mw_bad, "program.vibe.1.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(mw_bad, manifest)
    if not has(problems, "implementation param 'program.vibe.1.x'"):
        raise SystemExit("vibe.1.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (mx) FIRST terminator delete across the family: dropping vibe.1.x is a landed/mustComplete drop.
    mx_bad = copy.deepcopy(spec)
    _v1 = next(pr for pr in mx_bad["programs"] if pr.get("stable_id") == "program.vibe.1")
    _v1["parameters"] = [p for p in _v1["parameters"] if p["stable_id"] != "program.vibe.1.x"]
    problems, _ = gate.check(mx_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibe.1.x'"):
        raise SystemExit("vibe.1.x terminator delete (family first) not enforced: %r" % problems)

    # (my) MIDDLE terminator delete across the family: dropping vibe.2.y is likewise a landed/
    #      mustComplete drop.
    my_bad = copy.deepcopy(spec)
    _v2 = next(pr for pr in my_bad["programs"] if pr.get("stable_id") == "program.vibe.2")
    _v2["parameters"] = [p for p in _v2["parameters"] if p["stable_id"] != "program.vibe.2.y"]
    problems, _ = gate.check(my_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibe.2.y'"):
        raise SystemExit("vibe.2.y terminator delete (family middle) not enforced: %r" % problems)

    # (mz) LAST terminator delete across the family: dropping vibe.3.z is likewise a landed/
    #      mustComplete drop.
    mz_bad = copy.deepcopy(spec)
    _v3 = next(pr for pr in mz_bad["programs"] if pr.get("stable_id") == "program.vibe.3")
    _v3["parameters"] = [p for p in _v3["parameters"] if p["stable_id"] != "program.vibe.3.z"]
    problems, _ = gate.check(mz_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.vibe.3.z'"):
        raise SystemExit("vibe.3.z terminator delete (family last) not enforced: %r" % problems)

    # (na)-(ni) PITCH SHIFTER Program 1/2/3 X/Y/Z descriptor lock (Codex msg 7071e673). The nine
    #      program pitch_shifter.1/.2/.3 x/y/z params (ids 349-357) are landed descriptor facts locked
    #      by the exact-compare — id renumber, owner / cross-owner leak, per-line evidence drift and a
    #      fieldEvidence overclaim must all fail normal. ROLE is also locked (a program x/y/z param's
    #      role must equal its stable-id .x/.y/.z suffix), ALL three terminator shapes across the whole
    #      PITCH SHIFTER family (first pitch_shifter.1.x / middle pitch_shifter.2.y / last
    #      pitch_shifter.3.z) are a landed/mustComplete drop, and Direct is locked as a CONTINUOUS
    #      placeholder — it must NOT be re-shaped into a selector. KEY DIFF: no octave/pitch/direct/
    #      voice-mix semitone, ratio or level unit/range/default is asserted — the labels are NOT used
    #      to derive values.

    # (na) id renumber: pitch_shifter.1.x is id 349; renumbering (349 -> 999) must fail.
    na_bad = copy.deepcopy(spec)
    reg_param(na_bad, "program.pitch_shifter.1.x")["id"] = 999
    problems, _ = gate.check(na_bad, manifest)
    if not has(problems, "implementation param 'program.pitch_shifter.1.x'"):
        raise SystemExit("pitch_shifter.1.x param id renumber (349 -> 999) not enforced: %r" % problems)

    # (nb) owner / cross-owner leak: pitch_shifter.1.x must stay owned by program.pitch_shifter.1;
    #      moving it under program.pitch_shifter.2's array makes the generated owner
    #      program.pitch_shifter.2, which the landed fact rejects.
    nb_bad = copy.deepcopy(spec)
    _p1 = next(pr for pr in nb_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.1")
    _p2 = next(pr for pr in nb_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.2")
    _x = [p for p in _p1["parameters"] if p["stable_id"] == "program.pitch_shifter.1.x"][0]
    _p1["parameters"] = [p for p in _p1["parameters"] if p["stable_id"] != "program.pitch_shifter.1.x"]
    _p2.setdefault("parameters", []).append(_x)
    problems, _ = gate.check(nb_bad, manifest)
    if not has(problems, "implementation param 'program.pitch_shifter.1.x'"):
        raise SystemExit("pitch_shifter.1.x cross-owner leak (-> program.pitch_shifter.2) not "
                         "enforced: %r" % problems)

    # (nc) ROLE drift: pitch_shifter.1.x being role y (valid but wrong position) must fail (role gate).
    nc_bad = copy.deepcopy(spec)
    reg_param(nc_bad, "program.pitch_shifter.1.x")["role"] = "y"
    problems, _ = gate.check(nc_bad, manifest)
    if not has(problems, "stable-id position"):
        raise SystemExit("pitch_shifter.1.x role drift (x -> y) not enforced by the role gate: %r"
                         % problems)

    # (nd) per-line descriptorEvidence drift: pitch_shifter.1.x cites L1260; moving it (1260 -> 1261)
    #      is a target drift and must fail.
    nd_bad = copy.deepcopy(spec)
    reg_param(nd_bad, "program.pitch_shifter.1.x")["evidence"]["line"] = 1261
    problems, _ = gate.check(nd_bad, manifest)
    if not has(problems, "implementation param 'program.pitch_shifter.1.x'"):
        raise SystemExit("pitch_shifter.1.x descriptorEvidence.line drift (1260 -> 1261) not "
                         "enforced: %r" % problems)

    # (ne) fieldEvidence overclaim: a placeholder range must not be elevated to confirmed (the KEY
    #      DIFF — no physical octave/pitch/direct/voice-mix unit/range/default is asserted here).
    ne_bad = copy.deepcopy(spec)
    reg_param(ne_bad, "program.pitch_shifter.1.x")["fieldEvidence"]["range"] = "confirmed"
    problems, _ = gate.check(ne_bad, manifest)
    if not has(problems, "implementation param 'program.pitch_shifter.1.x'"):
        raise SystemExit("pitch_shifter.1.x fieldEvidence overclaim (range unverified->confirmed) not "
                         "enforced: %r" % problems)

    # (nf) FIRST terminator delete across the family: dropping pitch_shifter.1.x is a landed/
    #      mustComplete drop.
    nf_bad = copy.deepcopy(spec)
    _p1 = next(pr for pr in nf_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.1")
    _p1["parameters"] = [p for p in _p1["parameters"] if p["stable_id"] != "program.pitch_shifter.1.x"]
    problems, _ = gate.check(nf_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.pitch_shifter.1.x'"):
        raise SystemExit("pitch_shifter.1.x terminator delete (family first) not enforced: %r"
                         % problems)

    # (ng) MIDDLE terminator delete across the family: dropping pitch_shifter.2.y is likewise a
    #      landed/mustComplete drop.
    ng_bad = copy.deepcopy(spec)
    _p2 = next(pr for pr in ng_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.2")
    _p2["parameters"] = [p for p in _p2["parameters"] if p["stable_id"] != "program.pitch_shifter.2.y"]
    problems, _ = gate.check(ng_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.pitch_shifter.2.y'"):
        raise SystemExit("pitch_shifter.2.y terminator delete (family middle) not enforced: %r"
                         % problems)

    # (nh) LAST terminator delete across the family: dropping pitch_shifter.3.z is likewise a landed/
    #      mustComplete drop.
    nh_bad = copy.deepcopy(spec)
    _p3 = next(pr for pr in nh_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.3")
    _p3["parameters"] = [p for p in _p3["parameters"] if p["stable_id"] != "program.pitch_shifter.3.z"]
    problems, _ = gate.check(nh_bad, manifest)
    if not has(problems, "MISSING landed descriptor param 'program.pitch_shifter.3.z'"):
        raise SystemExit("pitch_shifter.3.z terminator delete (family last) not enforced: %r"
                         % problems)

    # (ni) Direct must NOT become a selector: Direct (program.pitch_shifter.1.z) is a CONTINUOUS
    #      software-normalized 0..1 placeholder, not a binary selector. Shaping it into a selector
    #      (giving it positions) must fail — the landed fact is kind=continuous, and the gate rejects
    #      registry selector-positions on a non-selector-toggle landed param.
    ni_bad = copy.deepcopy(spec)
    _p1 = next(pr for pr in ni_bad["programs"] if pr.get("stable_id") == "program.pitch_shifter.1")
    for _p in _p1["parameters"]:
        if _p["stable_id"] == "program.pitch_shifter.1.z":
            _p["positions"] = ["Direct", "Wet"]
    problems, _ = gate.check(ni_bad, manifest)
    if not has(problems, "registry carries selector positions"):
        raise SystemExit("Direct re-shaped as a selector (positions on a continuous landed param) not "
                         "enforced: %r" % problems)

    print("OK: completeness gate rejects each defect for its intended reason;baseline passes; "
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
          "too (Codex 22f545c1/03848819); the voice-mixer slice (Codex 920fa79b) closes the 10-channel "
          "PAN/VOL chain too — begin (ch1_pan)/middle (ch5_vol)/end (ch10_vol) channel delete, param id "
          "renumber, PAN/VOL descriptorEvidence.line drift, six-field provenance over-claim, and "
          "channel-mapping (name) drift all fail; the dual-effector slice (Codex 1a47b5d0) closes the "
          "effector module (id 13) too — module delete / module id renumber, a landed param delete "
          "(effector.x) + param id renumber, a landed jack delete (effector.cv_x_in) + jack id renumber, "
          "selector positions drift (SELECT L ['1','2','3'] reorder), X/Y/Z / BLEND / MASTER / PHONE "
          "descriptorEvidence.line drift, and CV input nominal-range / polarity / fieldEvidence drift "
          "all fail; the dual-effector-slice successor, the drone-voices module closure (Codex msg "
          "c212dcfb), closes the voices module (id 14) too — module delete, module id renumber, "
          "module target name drift and module evidence.lineStart drift all fail; its successor, the "
          "DRONE 1 classic voice slice (Codex msg 28e00d92), closes the drone_1 module (id 15) too — "
          "module delete / module id renumber / module target name drift / module evidence.lineStart "
          "drift, the 5-generator cardinality (first tune_1 / middle tune_3 / last tune_5 delete), a "
          "generator id renumber + name mapping drift, MUTE + MOD selector positions reorder, "
          "VOLT/ATT/RLS/HOLD descriptorEvidence.line drift, the continuous VOLT placeholder provenance "
          "over-claim, and the cv_mod_in / gate_in / env_out range, type (signalType/polarity) and "
          "descriptorEvidence.line drift all fail; its successor, the DRONE 2 classic voice slice "
          "(Codex msg 3bc2111c), closes the drone_2 module (id 16) too — module delete / module id "
          "renumber / module target name drift / module evidence.lineStart drift, the 5-generator "
          "cardinality (first tune_1 / middle tune_3 / last tune_5 delete), a generator id renumber + "
          "name mapping drift, MUTE + MOD selector positions reorder, VOLT/ATT/RLS/HOLD "
          "descriptorEvidence.line drift, the continuous VOLT placeholder provenance over-claim, the "
          "cv_mod_in / gate_in / env_out range, type (signalType/polarity) and descriptorEvidence.line "
          "drift, and the cross-owner leak (a drone_2 member renamed to a drone_1 stable_id) all fail; "
          "its successor, the DRONE 4 classic voice slice (Codex msg 483a9dc5), closes the drone_4 "
          "module (id 17) too — module delete / module id renumber / module target name drift / module "
          "evidence.lineStart drift, the 5-generator cardinality (first tune_1 / middle tune_3 / last "
          "tune_5 delete), a generator id renumber + name mapping drift, MUTE + MOD selector positions "
          "reorder, VOLT/ATT/RLS/HOLD descriptorEvidence.line drift, the continuous VOLT placeholder "
          "provenance over-claim, the cv_mod_in / gate_in range + type (signalType/polarity) drift, the "
          "env_out range / descriptorEvidence.line drift, and the cross-owner leak (a drone_4 member "
          "renamed to a drone_1 stable_id) all fail — and, as the mandated KEY DIFF, the env_out "
          "polarity guess to bipolar and the env_out nominalRange evidence over-claim (both forbidden "
          "because manual L157 'ENV VOICES 1,2,3,6,7,8' does NOT list 4/5) also fail; its successor, "
          "the DRONE 5 classic voice slice (Codex msg 6e25b926), closes the drone_5 module (id 18) too "
          "— module delete / module id renumber / module target name drift / module evidence.lineStart "
          "drift, the 5-generator cardinality (first tune_1 / middle tune_3 / last tune_5 delete), a "
          "generator id renumber + name mapping drift, MUTE + MOD selector positions reorder, "
          "VOLT/ATT/RLS/HOLD descriptorEvidence.line drift, the continuous VOLT placeholder provenance "
          "over-claim, the cv_mod_in / gate_in range + type (signalType/polarity) drift, the env_out "
          "range / descriptorEvidence.line drift, and the cross-owner leak (a drone_5 member renamed to "
          "a drone_1 stable_id) all fail — and, as the mandated KEY DIFF, the env_out polarity guess to "
          "bipolar and the env_out nominalRange evidence over-claim (both forbidden because manual L157 "
          "'ENV VOICES 1,2,3,6,7,8' does NOT list 4/5) also fail; its successor, the DRONE 3 \"Papa Srapa\" "
          "voice slice (Codex msg b86a53c0), closes the drone_3 module (id 19) too — module delete / module "
          "id renumber / module target name drift / module evidence.lineStart drift, the 7-continuous "
          "cardinality (first rate / middle pitch / last rls delete), a continuous param id renumber + "
          "name mapping drift, hi_low + FM selector positions reorder, RATE/PITCH/HOLD "
          "descriptorEvidence.line drift, the continuous RATE placeholder provenance over-claim, the "
          "cv_out range + polarity (unipolar) drift, the gate_in signalType + nominal-range drift, the "
          "clock_in signalType drift, the noise_in nominal-range drift, and the cross-owner leak (a "
          "drone_3 member renamed to a drone_1 stable_id) all fail — and, as the OPPOSITE-polarity KEY "
          "DIFF ground, the env_out polarity drift away from bipolar (to unknown) and the env_out "
          "nominalRange evidence downgrade (confirmed -> unverified) both fail because manual L157 'ENV "
          "VOICES 1,2,3,6,7,8' DOES list voice 3, and the gate_in polarity over-claim (unknown -> unipolar, "
          "reverse-inferred from the keyboard-produced 5V gate) fails too.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
