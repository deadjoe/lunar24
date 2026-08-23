#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Full-machine completeness gate for the P0 registry (Phase A).

The registry (spec/machine/lunar24.json) is the IMPLEMENTATION. The manifest
(spec/machine/p0_inventory_manifest.json) is the INDEPENDENT full-machine TARGET
transcribed from the manual (solar42N_manual_v15) and the panel map (design/04 §1) —
NOT generated from the registry, so completeness is auditable rather than
self-proving (Codex constraint #1).

Codex msg 06ef6b70 (third review, ledger rework) split the old conflated `controls[]`
(370) into THREE entities and killed two self-proving gates:

  target.panelControls[]           -- ONE row per REAL physical panel widget
      {stable_id, owner, kind, region, panelLabel, status, evidence}
      kind ∈ {continuous, selector-toggle, momentary-touch, rotary-encoder}
  target.parameters[]              -- the persisted LOGICAL state (the Parameter target)
      {stable_id, owner, kind, region, semanticLabel?, status, evidence}
      kind ∈ {continuous, selector-toggle, state-field}
  target.controlBindings[]         -- widget -> parameter, with a program/mode/global context
      {stable_id, from, to, context, status, evidence}
  target.controlRegions[]          -- per-panel-region subtotal; the DECLARED count is an
      INDEPENDENT hand-counted constant (`expectedPanelControlCount`), never `len(rows)`,
      so an omission in the transcription is caught here rather than self-proven.

The states of the 117 program X/Y/Z knobs and the keyboard menu/state fields are NOT
panel controls — they are logical parameters edited by the shared X/Y/Z / encoder. They
live in `parameters[]` and are bound to those widgets via `controlBindings[]`.

The gate checks:

  1. CONSISTENCY INVARIANTS (always on):
     - real provenance shape (status + ref + panelSite/section/line);
     - no duplicate stable_id within ANY category (重件), incl. panelControls/parameters/
       controlBindings/controlRegions;
     - every parameter/endpoint/panelControl `owner` is a target module or program/terminal;
       every kind is in its closed enum; every parameter and panelControl names a real region;
     - controlBindings: every `from` is a panelControl, every `to` a parameter, context present;
       every parameter is bound (no orphan persisted state); every continuous/selector-toggle
       panelControl drives ≥1 bound parameter;
     - REGION SUBTOTAL INDEPENDENCE: each region's emitted panelControl count == its
       INDEPENDENT expectedPanelControlCount, the subtotals sum to the full panelControls[]
       set, and no panelControl lives in a non-panel (state) region;
     - normalized/fixed route endpoints resolve in the inventory, are correctly directed,
       and internal (non-patchable) endpoints stay in fixed/route position;
     - per-CAPABILITY present-but-empty: a registry module that DECLARES a capability
       (parameters/jacks/controls) must actually provide it — the old blanket "every module
       must have a param AND a jack" is gone (voices legitimately has no persisted param);
     - implementation ⊆ independent target (module/program/route): a registry item absent from
       the target is an error, never silently green;
     - mustComplete == exactly the registry's landed target keys (auto-sync).

  2. CATEGORY-WISE COVERAGE (never one %): modules / program identities / params / panel
     controls / bindings / patchable jacks + internal endpoints / normalized routes / fixed
     routes. Params and endpoints are reported target-vs-present.

  With `--require-full` the gate compares target↔registry PER STABLE ID for Parameters and
  Patchable Jacks (gap = target item not implemented; rogue = implemented item not a target),
  requires every capability-declared target module to be transcribed, and requires every
  required fixed route to be defined. InternalEndpoint/FixedRoute per-ID comparison is deferred
  until the C++ id-space lands (Codex msg 06ef6b70: fixed schema must not bind to a target
  whose IDs are still moving). CI enables this mode against the exact head so PR #2 can turn
  it green for real.

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

VALID_STATUS = {"confirmed", "provisional"}
VALID_DIR = {"input", "output"}
VALID_KIND = {"audio", "control", "gate"}
PANEL_KINDS = {"continuous", "selector-toggle", "momentary-touch", "rotary-encoder"}
PARAM_KINDS = {"continuous", "selector-toggle", "state-field"}
# Parameter regions that hold LOGICAL state (program X/Y/Z, keyboard menu/state) but are NOT
# panel regions: they contribute no expectedPanelControlCount entry to controlRegions[].
STATE_REGIONS = {"program_params", "keyboard_state"}
PROGRAM_SLOTS = (1, 2, 3)


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def provenance_ok(entry):
    """A transcribed item must have a REAL provenance shape, not any non-empty ref."""
    if entry.get("status") not in VALID_STATUS:
        return False
    ev = entry.get("evidence") or {}
    if not ev.get("ref"):
        return False
    location = ev.get("panelSite") or ev.get("lineStart") or ev.get("lineEnd") or ev.get("section")
    return bool(location)


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
    terminals = tgt.get("terminals", [])
    programs = tgt.get("programs", [])
    panel_controls = tgt.get("panelControls", [])
    parameters = tgt.get("parameters", [])
    bindings = tgt.get("controlBindings", [])
    control_regions = tgt.get("controlRegions", [])
    pjt = tgt.get("paramsJackTargets") or {}
    endpoints = pjt.get("endpoints", [])
    norm_routes = tgt.get("normalizedRoutes", [])
    fixed_routes = tgt.get("fixedRoutes", [])
    required_fixed = tgt.get("requiredFixedRoutes", [])
    must_complete = manifest.get("mustComplete", [])

    def dup_check(items, cat, sid_key="stable_id"):
        seen = {}
        for it in items:
            sid = it.get(sid_key) if isinstance(it, dict) else None
            text = sid if sid else "<missing-id>"
            if text in seen:
                problems.append(f"{cat} duplicate stable_id {text!r} (重件)")
            seen[text] = True

    dup_check(modules, "module")
    dup_check(programs, "program")
    dup_check(panel_controls, "panelControl")
    dup_check(parameters, "parameter")
    dup_check(bindings, "controlBinding")
    dup_check(endpoints, "endpoint")
    dup_check(norm_routes, "normalized route")
    dup_check(fixed_routes, "fixed route")
    dup_check(control_regions, "controlRegion", sid_key="id")

    # ---- module shape + provenance + capability declaration -----------------
    for m in modules:
        if not (m.get("stable_id") and m.get("name") and m.get("category")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs stable_id+name+category")
        cap = m.get("capabilities")
        if not isinstance(cap, dict) or not all(
                isinstance(cap.get(k), bool) for k in ("parameters", "jacks", "controls")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs capabilities with "
                            f"parameters/jacks/controls each a bool")
        if not provenance_ok(m):
            problems.append(f"manifest module {m.get('stable_id')!r}: incomplete evidence "
                            f"(need status in {sorted(VALID_STATUS)} + ref + panelSite/section/line)")
    module_ids = {m["stable_id"] for m in modules if m.get("stable_id")}

    # ---- programs: 39 verbatim identities, full 13x3 grid, ORCHE anomaly -----
    grid = {}
    dup_slot = False
    for p in programs:
        if not (p.get("stable_id") and p.get("slot") in PROGRAM_SLOTS and p.get("name")):
            problems.append(f"manifest program {p.get('stable_id')!r}: needs stable_id+slot(1..3)+name")
        if not p.get("cartridge"):
            problems.append(f"manifest program {p.get('stable_id')!r}: missing cartridge name")
        if not provenance_ok(p):
            problems.append(f"manifest program {p.get('stable_id')!r}: incomplete evidence")
        key = (p.get("cartridge"), p.get("slot"))
        if key in grid:
            dup_slot = True
        grid[key] = p.get("stable_id")
    orche = [p for p in programs if p.get("cartridge") == "ORCHE"]
    if len(orche) == 3:
        for p in orche:
            if p.get("slot") in (2, 3) and p.get("status") != "provisional":
                problems.append(f"ORCHE slot {p.get('slot')} must be provisional "
                                f"(manual prints Program 1 for all three)")
    if not dup_slot and programs:
        carts = {p.get("cartridge") for p in programs}
        for c in sorted(carts):
            for slot in PROGRAM_SLOTS:
                if (c, slot) not in grid:
                    problems.append(f"manifest cartridge {c!r}: must list all {len(PROGRAM_SLOTS)} slots "
                                    f"(missing slot {slot})")
    program_stable_ids = {p["stable_id"] for p in programs if p.get("stable_id")}

    # ---- terminals (fixed-topology I/O blocks) ------------------------------
    terminal_ids = set()
    for term in terminals:
        if not (term.get("stable_id") and term.get("name")):
            problems.append(f"manifest terminal {term.get('stable_id')!r}: needs stable_id+name")
        if not provenance_ok(term):
            problems.append(f"manifest terminal {term.get('stable_id')!r}: incomplete evidence")
        terminal_ids.add(term.get("stable_id"))
    allowed_owners = module_ids | terminal_ids

    # ---- parameters: the persisted logical state (Parameter target) ---------
    param_sids = set()
    for it in parameters:
        sid = it.get("stable_id")
        if not (sid and it.get("owner") and it.get("kind") in PARAM_KINDS):
            problems.append(f"manifest parameter {sid!r}: needs stable_id+owner+kind in "
                            f"{sorted(PARAM_KINDS)}")
        if it.get("owner") not in (module_ids | program_stable_ids):
            problems.append(f"manifest parameter {sid!r}: owner {it.get('owner')!r} "
                            f"not a target module or program")
        if it.get("region") not in ({r.get("id") for r in control_regions} | STATE_REGIONS):
            problems.append(f"manifest parameter {sid!r}: region {it.get('region')!r} is not a "
                            f"panel region nor {sorted(STATE_REGIONS)}")
        if not provenance_ok(it):
            problems.append(f"manifest parameter {sid!r}: incomplete evidence")
        param_sids.add(sid)

    # ---- panelControls: one row per real physical widget --------------------
    pc_sids = set()
    region_actual = {}
    for c in panel_controls:
        sid = c.get("stable_id")
        if not (sid and c.get("owner") and c.get("kind") in PANEL_KINDS):
            problems.append(f"manifest panelControl {sid!r}: needs stable_id+owner+kind in "
                            f"{sorted(PANEL_KINDS)}")
        if c.get("owner") not in (allowed_owners | program_stable_ids):
            problems.append(f"manifest panelControl {sid!r}: owner {c.get('owner')!r} is not a "
                            f"target module, terminal, or program")
        if c.get("region") not in {r.get("id") for r in control_regions}:
            problems.append(f"manifest panelControl {sid!r}: region {c.get('region')!r} is not a "
                            f"declared panel controlRegion")
        if not provenance_ok(c):
            problems.append(f"manifest panelControl {sid!r}: incomplete evidence")
        pc_sids.add(sid)
        region_actual[c.get("region")] = region_actual.get(c.get("region"), 0) + 1

    # ---- controlBindings: widget -> parameter -------------------------------
    binding_from_pc = {}
    binding_to_param = {}
    for b in bindings:
        sid = b.get("stable_id")
        if not (sid and b.get("from") and b.get("to")):
            problems.append(f"manifest controlBinding {sid!r}: needs stable_id+from+to")
            continue
        if b.get("from") not in pc_sids:
            problems.append(f"manifest controlBinding {sid}: from {b.get('from')!r} is not a "
                            f"declared panelControl")
        if b.get("to") not in param_sids:
            problems.append(f"manifest controlBinding {sid}: to {b.get('to')!r} is not a "
                            f"declared parameter")
        if not b.get("context"):
            problems.append(f"manifest controlBinding {sid}: missing context "
                            f"(program/mode/global)")
        if not provenance_ok(b):
            problems.append(f"manifest controlBinding {sid}: incomplete evidence")
        binding_from_pc[b.get("from")] = binding_from_pc.get(b.get("from"), 0) + 1
        binding_to_param[b.get("to")] = binding_to_param.get(b.get("to"), 0) + 1

    # every persisted parameter must be reachable via a widget (no orphan state);
    # every continuous/selector-toggle panelControl must drive a persisted parameter.
    orphan_params = sorted(param_sids - set(binding_to_param))
    if orphan_params:
        problems.append(f"parameters with no controlBinding (orphan persisted state): "
                        f"{orphan_params}")
    unbound_persist = sorted(
        {c.get("stable_id") for c in panel_controls if c.get("kind") in
         {"continuous", "selector-toggle"}} - set(binding_from_pc))
    if unbound_persist:
        problems.append(f"continuous/selector-toggle panelControls with no controlBinding: "
                        f"{unbound_persist}")

    # ---- region subtotal INDEPENDENCE (no len(rows) self-proving) -----------
    by_rid = {}
    for r in control_regions:
        rid = r.get("id")
        by_rid[rid] = r
        if not (rid and r.get("module") and isinstance(r.get("expectedPanelControlCount"), int)):
            problems.append(f"control region {rid!r}: needs id+module+expectedPanelControlCount")
            continue
        if r.get("module") not in (allowed_owners | program_stable_ids):
            problems.append(f"control region {rid!r}: module {r.get('module')!r} is not a target "
                            f"module, terminal, or program")
        expected = r.get("expectedPanelControlCount")
        if expected < 0:
            problems.append(f"control region {rid!r}: expectedPanelControlCount must be >= 0")
        if expected != region_actual.get(rid, 0):
            problems.append(f"control region {rid!r}: expectedPanelControlCount={expected} != "
                            f"actual panelControls={region_actual.get(rid, 0)} "
                            f"(independent subtotal disagrees with the transcription)")
        if not provenance_ok(r):
            problems.append(f"control region {rid!r}: incomplete evidence")
    region_ids = set(by_rid)
    if control_regions:
        undeclared = sorted(set(region_actual) - region_ids)
        if undeclared:
            problems.append(f"panelControls in undeclared regions (no controlRegion subtotal): "
                            f"{undeclared}")
    total_counted = sum(r.get("expectedPanelControlCount")
                        for r in control_regions if isinstance(r.get("expectedPanelControlCount"), int))
    if panel_controls and total_counted != len(panel_controls):
        problems.append(f"controlRegions expectedPanelControlCount subtotal {total_counted} "
                        f"!= panelControls[] length {len(panel_controls)}")
    # A panel region must contain real widgets (0-count panel regions are not allowed; logical
    # state regions are separate and excluded from controlRegions[]).
    empty_regions = sorted(rid for rid, n in region_actual.items() if n == 0)
    if empty_regions:
        problems.append(f"panel controlRegions with no panelControls: {empty_regions}")

    # ---- endpoint inventory + route direction/dangling validation -----------
    inventory = {}
    internal_endpoints = set()
    for e in endpoints:
        if not (e.get("stable_id") and e.get("owner") and e.get("direction") in VALID_DIR):
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: needs "
                            f"stable_id+owner+direction(input|output)")
        if e.get("owner") not in allowed_owners:
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: owner {e.get('owner')!r} "
                            f"is not a target module or terminal")
        if not provenance_ok(e):
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: incomplete evidence")
        inventory[e.get("stable_id")] = e
        if not e.get("patchable"):
            internal_endpoints.add(e.get("stable_id"))

    def validate_routes(routes, enforce_direction, allowed_internal):
        for r in routes:
            sid = r.get("stable_id")
            expect = (("source", "output"), ("sink", "input")) if enforce_direction \
                     else (("source", None), ("sink", None))
            for role, want_dir in expect:
                ep = r.get(role) or r.get(role + "Jack")
                if not ep:
                    continue
                entry = inventory.get(ep)
                if entry is None:
                    problems.append(f"manifest route {sid}: {role} {ep!r} not in endpoint "
                                    f"inventory (dangling)")
                    continue
                if not allowed_internal and ep in internal_endpoints:
                    problems.append(f"manifest route {sid}: {role} {ep!r} is an internal "
                                    f"(non-patchable) endpoint — cannot be used in this route class")
                if enforce_direction and entry["direction"] != want_dir:
                    problems.append(f"manifest route {sid}: {role} {ep!r} is {entry['direction']}, "
                                    f"expected {want_dir}")

    validate_routes(norm_routes, enforce_direction=False, allowed_internal=False)
    validate_routes(fixed_routes, enforce_direction=True, allowed_internal=True)
    for r in fixed_routes:
        if r.get("kind") not in VALID_KIND:
            problems.append(f"fixed route {r.get('stable_id')!r}: kind must be one of "
                            f"{sorted(VALID_KIND)}")

    # ---- implementation ⊆ independent target (module/program/route) ---------
    target_program_ids = program_stable_ids
    target_norm_routes = {r["stable_id"] for r in norm_routes if r.get("stable_id")}
    present_modules = {m["stable_id"] for m in reg.modules}
    present_programs = {p["stable_id"] for p in reg.programs}
    present_routes = {r["stable_id"] for r in reg.routes}
    for mod in sorted(present_modules - module_ids):
        problems.append(f"implementation module {mod!r} not in independent target (impl ⊄ target)")
    for prog in sorted(present_programs - target_program_ids):
        problems.append(f"implementation program {prog!r} not in independent target (impl ⊄ target)")
    for route in sorted(present_routes - target_norm_routes):
        problems.append(f"implementation route {route!r} not in independent target (impl ⊄ target)")

    # ---- per-CAPABILITY present-but-empty (replaces blanket param+jack) ----
    # A registry module that DECLARES a capability must actually provide it; a module that
    # declares parameters=False (e.g. voices, momentary triggers) is NOT demanded to have any.
    tmodel = {m["stable_id"]: m for m in modules if m.get("stable_id")}
    reg_param_owners = {p["_stable_owner"] for p in reg.parameters}
    reg_jack_owners = {j["_module_stable"] for j in reg.jacks}
    cap_owners_target = {c.get("owner") for c in panel_controls}
    for mod in sorted(present_modules):
        tm = tmodel.get(mod)
        cap = (tm or {}).get("capabilities") or {}
        if cap.get("parameters") and mod not in reg_param_owners:
            problems.append(f"module {mod!r} declares capability parameters but the registry "
                            f"provides none")
        if cap.get("jacks") and mod not in reg_jack_owners:
            problems.append(f"module {mod!r} declares capability jacks but the registry "
                            f"provides none")
        if cap.get("controls") and mod not in cap_owners_target:
            problems.append(f"module {mod!r} declares capability controls but the target "
                            f"transcribes no panelControl")

    # ---- mustComplete coherence + non-regression + == landed set ------------
    target_all = (set(f"module:{x}" for x in module_ids)
                  | set(f"program:{x}" for x in target_program_ids)
                  | set(f"route:{x}" for x in target_norm_routes))
    present_keys = (set(f"module:{x}" for x in present_modules)
                    | set(f"program:{x}" for x in present_programs)
                    | set(f"route:{x}" for x in present_routes))
    for key in must_complete:
        if key not in target_all:
            problems.append(f"mustComplete item {key!r} is not a manifest target")
        if key not in present_keys:
            problems.append(f"NON-REGRESSION: mustComplete item {key!r} dropped from the registry")
    landed = set(must_complete) & target_all
    if landed != (present_keys & target_all):
        miss = sorted((present_keys & target_all) - landed)
        stray = sorted(landed - (present_keys & target_all))
        problems.append(f"mustComplete != registry landed target keys: add={miss} remove={stray} "
                        f"(future additions must be synced into mustComplete)")

    # ---- per-module transcription gaps vs declared capability --------------
    param_owners_target = {p["owner"] for p in parameters}
    endpoint_owners_target = {e["owner"] for e in endpoints}
    modules_missing_params = sorted(
        m for m in module_ids if (tmodel.get(m, {}).get("capabilities") or {}).get("parameters")
        and m not in param_owners_target)
    modules_missing_endpoints = sorted(
        m for m in module_ids if (tmodel.get(m, {}).get("capabilities") or {}).get("jacks")
        and m not in endpoint_owners_target)
    modules_no_inventory = sorted(
        set(module_ids) - (param_owners_target | endpoint_owners_target | cap_owners_target))
    # A module whose declared capability is untranscribed under the current manifest.
    modules_untranscribed = sorted(
        m for m in module_ids
        if (tmodel.get(m, {}).get("capabilities") or {}).get("parameters")
        and m not in param_owners_target)

    def cover(target, present):
        return {"target": len(target), "present": len(target & present), "gap": sorted(target - present)}

    tgt_patchable = {e["stable_id"] for e in endpoints if e.get("patchable")}
    reg_jack_sids = {j["stable_id"] for j in reg.jacks}
    reg_param_sids = {p["stable_id"] for p in reg.parameters}
    coverage = {
        "registry": {
            "modules": len(reg.modules), "params": len(reg.parameters),
            "jacks": len(reg.jacks), "routes": len(reg.routes), "programs": len(reg.programs),
        },
        "manifest": {
            "modules": len(modules), "terminals": len(terminals), "programs": len(programs),
            "panelControls": len(panel_controls), "parameters": len(parameters),
            "controlBindings": len(bindings), "controlRegions": len(control_regions),
            "endpointsTranscribed": len(endpoints), "normalizedRoutes": len(norm_routes),
            "fixedRoutes": len(fixed_routes), "requiredFixedRoutes": len(required_fixed),
            "mustComplete": len(must_complete),
        },
        "modules": cover(module_ids, present_modules),
        "programIdentities": cover(target_program_ids, present_programs),
        "normalizedRoutes": cover(target_norm_routes, present_routes),
        "panelControls": {"transcribed": len(panel_controls), "regions": len(control_regions),
                          "eventOnly": len([c for c in panel_controls if c.get("kind") in
                                            {"momentary-touch", "rotary-encoder"}])},
        "bindings": {"count": len(bindings), "orphanParams": orphan_params,
                     "unboundPersist": unbound_persist},
        "parameters": {"target": len(param_sids), "transcribed": len(param_sids),
                       "present": len(reg_param_sids), "missingModules": modules_missing_params,
                       "gap": sorted(param_sids - reg_param_sids),
                       "rogue": sorted(reg_param_sids - param_sids)},
        "jacks": {"target": len(tgt_patchable), "present": len(reg_jack_sids),
                  "missingModules": modules_missing_endpoints,
                  "gap": sorted(tgt_patchable - reg_jack_sids),
                  "rogue": sorted(reg_jack_sids - tgt_patchable),
                  "internal": sorted(internal_endpoints),
                  "untranscribedModules": modules_untranscribed},
        "fixedRoutes": {"target": len(fixed_routes), "defined": len(fixed_routes),
                        "required": required_fixed,
                        "gap": sorted(set(required_fixed) - {r["stable_id"] for r in fixed_routes}),
                        "note": "internal (non-patchable) P0 topology — no registry RouteId "
                                "equivalent; validated for provenance + endpoint/direction "
                                "coherence and required completeness under --require-full"},
    }

    if require_full:
        not_full = []
        if coverage["modules"]["gap"]:
            not_full.append("module gaps=%s" % coverage["modules"]["gap"])
        if coverage["programIdentities"]["gap"]:
            not_full.append("program identity gaps=%s" % coverage["programIdentities"]["gap"])
        if coverage["normalizedRoutes"]["gap"]:
            not_full.append("normalized-route gaps=%s" % coverage["normalizedRoutes"]["gap"])
        if coverage["parameters"]["gap"]:
            not_full.append("parameter target-not-implemented=%s" % coverage["parameters"]["gap"])
        if coverage["parameters"]["rogue"]:
            not_full.append("parameter registry-rogue=%s" % coverage["parameters"]["rogue"])
        if coverage["jacks"]["gap"]:
            not_full.append("patchable jack target-not-implemented=%s" % coverage["jacks"]["gap"])
        if coverage["jacks"]["rogue"]:
            not_full.append("patchable jack registry-rogue=%s" % coverage["jacks"]["rogue"])
        if modules_untranscribed:
            not_full.append("capability-declared modules untranscribed=%s" % modules_untranscribed)
        if coverage["fixedRoutes"]["gap"]:
            not_full.append("required fixed routes missing=%s" % coverage["fixedRoutes"]["gap"])
        if not_full:
            problems.append("--require-full: " + "; ".join(not_full))

    return problems, coverage


def format_report(coverage):
    lines = []
    reg = coverage["registry"]
    lines.append("registry: %(modules)d modules / %(params)d params / %(jacks)d jacks / "
                 "%(routes)d routes / %(programs)d programs" % reg)
    m = coverage["manifest"]
    lines.append("manifest: %(modules)d modules / %(terminals)d terminals / %(programs)d programs / "
                 "%(panelControls)d panel-controls / %(parameters)d parameters / "
                 "%(controlBindings)d bindings / %(controlRegions)d control-regions / "
                 "%(endpointsTranscribed)d endpoints / %(normalizedRoutes)d normalized / "
                 "%(fixedRoutes)d fixed / %(requiredFixedRoutes)d required-fixed / "
                 "%(mustComplete)d mustComplete" % m)
    lines.append("")
    lines.append("coverage (target / present / gap):")
    lines.append("  modules          : %(target)d / %(present)d / gaps=%(gap)s" % coverage["modules"])
    lines.append("  program identities: %(target)d / %(present)d / gaps=%(gap)s"
                 % coverage["programIdentities"])
    lines.append("  normalized routes: %(target)d / %(present)d / gaps=%(gap)s"
                 % coverage["normalizedRoutes"])
    pc = coverage["panelControls"]
    lines.append("  panel controls   : transcribed=%(transcribed)d regions=%(regions)d "
                 "eventOnly=%(eventOnly)d" % pc)
    bd = coverage["bindings"]
    lines.append("  bindings         : %(count)d orphanParams=%(orphanParams)s "
                 "unboundPersist=%(unboundPersist)s" % bd)
    pr = coverage["parameters"]
    lines.append("  parameters       : target=%(target)d present=%(present)d "
                 "missingModules=%(missingModules)s" % pr)
    jk = coverage["jacks"]
    lines.append("  jacks            : target=%(target)d present=%(present)d "
                 "missingModules=%(missingModules)s internal=%(internal)s" % jk)
    fx = coverage["fixedRoutes"]
    lines.append("  fixed routes     : defined=%(defined)d required=%(required)s gap=%(gap)s  (%(note)s)"
                 % fx)
    lines.append("")
    lines.append("note: 39 program identities is an identity/existence list, NOT 39 implemented effects.")
    lines.append("note: per-ID Parameter/Jack comparison runs under --require-full; internal "
                 "endpoints & fixed routes are coherence-validated (no registry id-space yet).")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--require-full", action="store_true",
                    help="fail unless every capability-declared target module/program/route is "
                         "present, per-ID Parameter/Jack target↔registry, every module is "
                         "transcribed, and every required fixed route is defined")
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
    lines.append("OK: manifest self-coherent; region subtotals independent; every parameter bound; "
                 "impl ⊆ target; per-capability present-but-empty; mustComplete == landed set.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
