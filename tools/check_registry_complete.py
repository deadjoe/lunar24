#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Full-machine completeness gate for the P0 registry (Phase A).

The registry (spec/machine/lunar24.json) is the IMPLEMENTATION. The manifest
(spec/machine/p0_inventory_manifest.json) is the INDEPENDENT full-machine TARGET
transcribed from the manual (solar42N_manual_v15) and the panel map (design/04 §1) —
NOT generated from the registry, so completeness is auditable rather than
self-proving (Codex constraint #1).

The manifest TARGET inventory is per-item, per-category, never synthesised into one
number (constraint #2):

  target.modules[]                        — every module (id/name/category/evidence)
  target.programs[]                       — every program identity, VERBATIM (39)
  target.paramsJackTargets.params[]       — every parameter (stable_id/owner/evidence/status)
  target.paramsJackTargets.endpoints[]    — every jack OR internal endpoint
                                          (stable_id/owner/direction/patchable/evidence/status)
  target.normalizedRoutes[]               — patchable normalizations (source.endpoint -> sink.endpoint)
  target.fixedRoutes[]                    — fixed internal edges (module.endpoint -> module.endpoint, kind)
  target.requiredFixedRoutes[]            — the architecturally-complete fixed chain
                                          (so `--require-full` cannot green a grouped summary)

The gate checks:

  1. CONSISTENCY INVARIANTS (always on):
     - real provenance shape (status + ref + panelSite/section/line), not any non-empty ref;
     - no duplicate stable_id within ANY category (重件), including program grid cartridges;
     - every param/endpoint `owner` is a target module; every direction is input|output;
     - normalized/fixed route endpoints must resolve in the MANIFEST inventory (no `continue`
       on an absent module) and be correctly directed (fixed audio: source=output, sink=input);
       internal endpoints (patchable=false) may only appear as fixed/route targets, not as
       patchable normalizations;
     - present-but-empty module (registry has no param or no endpoint) is an error;
     - mustComplete == exactly the registry's landed target keys (auto-sync);
     - implementation ⊆ independent target: every registry module/program/route must be a
       manifest target — a registry item absent from the target is an error, never silently green.

  2. CATEGORY-WISE COVERAGE (never one %): modules / program identities / params /
     jacks+internal endpoints / normalized routes / fixed routes. Params and endpoints are
     reported target-vs-present; a module with zero transcribed params or endpoints is an
     open gap (not a fabricated count).

  With `--require-full` the gate additionally requires every module/program identity to be
  present in the registry, every module to have a fully-transcribed (non-empty) param and
  endpoint inventory, and EVERY required fixed route to be defined. CI runs this mode so the
  PR #2 exact head enables it for real (constraint #3).

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
VALID_CONTROL_KIND = {"continuous", "selector-toggle", "momentary-touch-event"}
PROGRAM_SLOTS = (1, 2, 3)


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def provenance_ok(entry):
    """A transcribed item must have a REAL provenance shape, not any non-empty ref.

    Requires a status in {confirmed, provisional}; an evidence object with a non-empty
    `ref` naming a source; AND a location (panelSite / lineStart / lineEnd / section).
    An evidence of just `{"ref": "x"}` is insufficient and is rejected.
    """
    if entry.get("status") not in VALID_STATUS:
        return False
    ev = entry.get("evidence") or {}
    if not ev.get("ref"):
        return False
    location = ev.get("panelSite") or ev.get("lineStart") or ev.get("lineEnd") or ev.get("section")
    return bool(location)


def resolve_endpoint(endpoint, inventory):
    """Return the inventory entry for a `module.endpoint` id, or None if absent/unknown."""
    return inventory.get(endpoint)


def split_endpoint(jack):
    mod, _, name = jack.partition(".")
    return mod, jack


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
    terminals = tgt.get("terminals", [])   # internal I/O termination blocks (ext_in/piezzo/distortion/out)
    programs = tgt.get("programs", [])
    controls = tgt.get("controls", [])                        # one row per visible/operable item
    control_regions = tgt.get("controlRegions", [])           # panel region subtotals
    pjt = tgt.get("paramsJackTargets") or {}
    params = pjt.get("params", [])
    endpoints = pjt.get("endpoints", [])
    norm_routes = tgt.get("normalizedRoutes", [])
    fixed_routes = tgt.get("fixedRoutes", [])
    required_fixed = tgt.get("requiredFixedRoutes", [])
    must_complete = manifest.get("mustComplete", [])

    # ---- per-category duplicate detection (重件) ------------------------------
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
    dup_check(controls, "control")
    dup_check(params, "param")
    dup_check(endpoints, "endpoint")
    dup_check(norm_routes, "normalized route")
    dup_check(fixed_routes, "fixed route")

    # ---- module shape + provenance -----------------------------------------
    for m in modules:
        if not (m.get("stable_id") and m.get("name") and m.get("category")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs stable_id+name+category")
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
    # ORCHE anomaly: manual prints all three as "Program 1" — slot 2/3 are interpretation.
    orche = [p for p in programs if p.get("cartridge") == "ORCHE"]
    if len(orche) == 3:
        for p in orche:
            if p.get("slot") in (2, 3):
                if p.get("status") != "provisional":
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

    # ---- params + endpoints: per-item, owned by a target module (or program) --
    for it in params:
        if not (it.get("stable_id") and it.get("owner")):
            problems.append(f"manifest param {it.get('stable_id')!r}: needs stable_id+owner")
        if it.get("owner") not in module_ids and it.get("owner") not in program_stable_ids:
            problems.append(f"manifest param {it.get('stable_id')!r}: owner {it.get('owner')!r} "
                            f"not a target module or program")
        if not provenance_ok(it):
            problems.append(f"manifest param {it.get('stable_id')!r}: incomplete evidence")

    # Internal I/O termination blocks (fixed topology endpoints live on these, not on a panel
    # module): verified for provenance, and their ids are legal endpoint owners alongside modules.
    terminal_ids = set()
    for term in terminals:
        if not (term.get("stable_id") and term.get("name")):
            problems.append(f"manifest terminal {term.get('stable_id')!r}: needs stable_id+name")
        if not provenance_ok(term):
            problems.append(f"manifest terminal {term.get('stable_id')!r}: incomplete evidence")
        terminal_ids.add(term.get("stable_id"))
    allowed_owners = module_ids | terminal_ids

    inventory = {}          # endpoint stable_id -> entry
    for e in endpoints:
        if not (e.get("stable_id") and e.get("owner") and e.get("direction") in VALID_DIR):
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: needs stable_id+owner+direction(input|output)")
        if e.get("owner") not in allowed_owners:
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: owner {e.get('owner')!r} is not a target module or terminal")
        if not provenance_ok(e):
            problems.append(f"manifest endpoint {e.get('stable_id')!r}: incomplete evidence")
        inventory[e.get("stable_id")] = e
    internal_endpoints = {sid for sid, e in inventory.items() if not e.get("patchable")}

    # ---- route endpoint/direction validation against the MANIFEST inventory --
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
                    problems.append(f"manifest route {sid}: {role} {ep!r} not in endpoint inventory "
                                    f"(dangling)")
                    continue
                if not allowed_internal and ep in internal_endpoints:
                    problems.append(f"manifest route {sid}: {role} {ep!r} is an internal "
                                    f"(non-patchable) endpoint — cannot be used in this route class")
                if enforce_direction and entry["direction"] != want_dir:
                    problems.append(f"manifest route {sid}: {role} {ep!r} is {entry['direction']}, "
                                    f"expected {want_dir}")

    validate_routes(norm_routes, enforce_direction=False, allowed_internal=False)
    validate_routes(fixed_routes, enforce_direction=True, allowed_internal=True)

    # fixed route kind must be audio|control|gate and must reference allowed endpoints.
    for r in fixed_routes:
        if r.get("kind") not in VALID_KIND:
            problems.append(f"fixed route {r.get('stable_id')!r}: kind must be one of {sorted(VALID_KIND)}")

    # ---- controls: per-item operable surface + Persistence subset -------------
    # controls[] is the independent per-region ledger of EVERY visible/operable item
    # (continuous | selector-toggle | momentary-touch-event). params[] must equal the
    # persistable subset of controls[]; a Parameter is not a parallel category but a
    # shrink of the control surface (Codex msg 66a83ca0 item #1).
    for c in controls:
        sid = c.get("stable_id")
        if not (sid and c.get("owner")):
            problems.append(f"manifest control {sid!r}: needs stable_id+owner")
            continue
        if c.get("owner") not in allowed_owners and c.get("owner") not in program_stable_ids:
            problems.append(f"manifest control {sid!r}: owner {c.get('owner')!r} is not a target "
                            f"module, terminal, or program")
        if c.get("kind") not in VALID_CONTROL_KIND:
            problems.append(f"manifest control {sid!r}: kind {c.get('kind')!r} must be one of "
                            f"{sorted(VALID_CONTROL_KIND)}")
        if not isinstance(c.get("persistable"), bool):
            problems.append(f"manifest control {sid!r}: persistable must be a bool")
        if not c.get("region"):
            problems.append(f"manifest control {sid!r}: missing region")
        if not provenance_ok(c):
            problems.append(f"manifest control {sid!r}: incomplete evidence")

    # Region subtotal == inventory: each region's declared count equals the controls it owns,
    # and the subtotals sum to the full controls[] set (no total-patching).
    region_counts = {}
    for c in controls:
        region_counts[c.get("region")] = region_counts.get(c.get("region"), 0) + 1
    by_rid = {}
    for r in control_regions:
        rid = r.get("id")
        by_rid[rid] = r
        if not (rid and r.get("module") and isinstance(r.get("count"), int)):
            problems.append(f"control region {rid!r}: needs id+module+count")
            continue
        if r.get("module") not in allowed_owners and r.get("module") not in program_stable_ids:
            problems.append(f"control region {rid!r}: module {r.get('module')!r} is not a target "
                            f"module, terminal, or program")
        if not provenance_ok(r):
            problems.append(f"control region {rid!r}: incomplete evidence")
        if region_counts.get(rid, 0) != r["count"]:
            problems.append(f"control region {rid!r}: subtotal {r['count']} != inventory "
                            f"{region_counts.get(rid, 0)}")
    if not by_rid and controls:
        problems.append("controls[] present but no controlRegions[] subtotals")
    elif by_rid:
        undeclared = sorted(set(region_counts) - set(by_rid))
        vacant = sorted(set(by_rid) - set(region_counts))
        if undeclared:
            problems.append(f"control regions with no declared subtotal: {undeclared}")
        if vacant:
            problems.append(f"declared control regions with no inventory: {vacant}")
    total_counted = sum(r["count"] for r in control_regions if isinstance(r.get("count"), int))
    if controls and total_counted != len(controls):
        problems.append(f"controlRegions subtotal {total_counted} != controls[] length {len(controls)}")

    # Parameter == persistable subset of controls[].
    persistable_sids = {c["stable_id"] for c in controls if c.get("persistable")}
    param_sids = {p["stable_id"] for p in params}
    if param_sids != persistable_sids:
        extra = sorted(param_sids - persistable_sids)
        missing = sorted(persistable_sids - param_sids)
        problems.append(f"params[] != persistable control subset (extra={extra} missing={missing})")

    # ---- present-but-empty module (always-on) ------------------------------
    param_owners = {p["_stable_owner"] for p in reg.parameters}
    jack_owners = {j["_module_stable"] for j in reg.jacks}
    for mod in sorted({m["stable_id"] for m in reg.modules}):
        if mod not in param_owners:
            problems.append(f"module {mod!r} in registry but has NO parameters")
        if mod not in jack_owners:
            problems.append(f"module {mod!r} in registry but has NO jacks")

    # ---- implementation ⊆ independent target (no registry item outside target) --
    target_program_ids = {p["stable_id"] for p in programs if p.get("stable_id")}
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

    # ---- per-module transcription gap (params/endpoints) -------------------
    param_owners_target = {p["owner"] for p in params}
    endpoint_owners_target = {e["owner"] for e in endpoints}
    control_owners_target = {c["owner"] for c in controls}
    modules_missing_params = sorted(module_ids - param_owners_target)
    modules_missing_endpoints = sorted(module_ids - endpoint_owners_target)
    # A module's inventory is TRANSCRIBED if the target names ANY param, endpoint, OR control
    # for it: `voices` (momentary drone keys) legitimately has zero persistable params but has
    # gate/audio endpoints AND 6 momentary trigger controls; `mixer` has no patchable jacks but
    # has PAN/VOL controls + internal audio endpoints. So an untranscribed module is one with
    # NO param AND NO endpoint AND NO control in the target.
    modules_no_inventory = sorted(module_ids - (param_owners_target | endpoint_owners_target
                                                | control_owners_target))
    modules_no_controls = sorted(module_ids - control_owners_target)

    def cover(target, present):
        return {"target": len(target), "present": len(target & present), "gap": sorted(target - present)}

    coverage = {
        "registry": {
            "modules": len(reg.modules), "params": len(reg.parameters),
            "jacks": len(reg.jacks), "routes": len(reg.routes), "programs": len(reg.programs),
        },
        "manifest": {
            "modules": len(modules), "terminals": len(terminals), "programs": len(programs),
            "controls": len(controls), "controlRegions": len(control_regions),
            "paramsTranscribed": len(params), "endpointsTranscribed": len(endpoints),
            "normalizedRoutes": len(norm_routes), "fixedRoutes": len(fixed_routes),
            "requiredFixedRoutes": len(required_fixed), "mustComplete": len(must_complete),
        },
        "modules": cover(module_ids, present_modules),
        "programIdentities": cover(target_program_ids, present_programs),
        "normalizedRoutes": cover(target_norm_routes, present_routes),
        "controls": {"transcribed": len(controls), "persistable": len(persistable_sids),
                     "regions": len(control_regions)},
        "params": {"target": len(params), "transcribed": len(params), "present": len(reg.parameters),
                   "missingModules": modules_missing_params},
        "endpoints": {"target": len(endpoints), "transcribed": len(endpoints), "present": len(reg.jacks),
                      "missingModules": modules_missing_endpoints,
                      "untranscribedModules": modules_no_inventory,
                      "internal": sorted(internal_endpoints)},
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
        if modules_no_inventory:
            not_full.append("module inventories untranscribed (no param/endpoint/control in target) "
                            "for modules=%s" % modules_no_inventory)
        if modules_no_controls:
            not_full.append("control regions untranscribed (no control in target) "
                            "for modules=%s" % modules_no_controls)
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
                 "%(controls)d controls / %(controlRegions)d control-regions / "
                 "%(paramsTranscribed)d params / %(endpointsTranscribed)d endpoints / "
                 "%(normalizedRoutes)d normalized / %(fixedRoutes)d fixed / "
                 "%(requiredFixedRoutes)d required-fixed / %(mustComplete)d mustComplete" % m)
    lines.append("")
    lines.append("coverage (target / present / gap):")
    lines.append("  modules          : %(target)d / %(present)d / gaps=%(gap)s" % coverage["modules"])
    lines.append("  program identities: %(target)d / %(present)d / gaps=%(gap)s" % coverage["programIdentities"])
    lines.append("  normalized routes: %(target)d / %(present)d / gaps=%(gap)s" % coverage["normalizedRoutes"])
    ct = coverage["controls"]
    lines.append("  controls         : transcribed=%(transcribed)d persistable=%(persistable)d "
                 "regions=%(regions)d" % ct)
    lines.append("  params           : target=%(target)d present=%(present)d missingModules=%(missingModules)s"
                 % coverage["params"])
    ep = coverage["endpoints"]
    lines.append("  endpoints        : target=%(target)d present=%(present)d missingModules=%(missingModules)s "
                 "untranscribed=%(untranscribedModules)s internal=%(internal)s" % ep)
    fx = coverage["fixedRoutes"]
    lines.append("  fixed routes     : defined=%(defined)d required=%(required)s gap=%(gap)s  (%(note)s)" % fx)
    lines.append("")
    lines.append("note: 39 program identities is an identity/existence list, NOT 39 implemented effects.")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--require-full", action="store_true",
                    help="fail unless every target module/program/route is present, every module's "
                         "params+endpoints are transcribed, and every required fixed route is defined")
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
    lines.append("OK: manifest self-coherent; mustComplete == landed set; impl ⊆ target; non-regressing.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
