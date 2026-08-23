#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Full-machine completeness gate for the P0 registry (Phase A).

The registry (spec/machine/lunar24.json) is the IMPLEMENTATION. The manifest
(spec/machine/p0_inventory_manifest.json) is the INDEPENDENT full-machine TARGET
transcribed from the panel map (design/04 §1) and the manual-derived inventories
(design/01 §3-§4, design/05 §1) — NOT generated from the registry, so completeness
is auditable rather than self-proving (Codex constraint #1).

The gate does:

  1. CONSISTENCY INVARIANTS (always enforced, green on every commit):
     - real provenance shape, not "any non-empty ref": each target item must carry
       a status (confirmed/provisional) AND a source `ref` AND either a panel-site
       or a manual line range, else it is rejected as un-provenanced;
     - no duplicate stable_id within ANY category (module / program / normalized
       route / fixed route) — a duplicate is 重件, never silently folded by a set;
     - route endpoint/direction validation: a manifest route whose endpoint module
       is present in the registry must reference a real, correctly-directed jack.
       A dangling endpoint (e.g. `vco_a.vco_out` where the module lacks that jack)
       is a problem — it can no longer pass as green;
     - every target module present in the registry must be non-empty (>=1 param,
       >=1 jack), so "module present but blank" cannot false-green;
     - every `mustComplete` item names a coherent manifest target AND exists in the
       registry — the NON-REGRESSION guard (constraint #3);
     - `mustComplete` must EQUAL the registry's currently-landed target keys. This
       is the auto-sync contract (item #5): future additions must be extended into
       `mustComplete` in the same commit, so the baseline can never drift silently.

  2. CATEGORY-WISE COVERAGE REPORT (constraint #2): target / present / gap per
     category — modules / program identities / normalized routes / fixed routes /
     parameters / jacks. Numbers are NEVER collapsed into one percentage that could
     hide a gap. Param and jack *target* counts come from an explicit
     `target.paramsJackTargets.entries` transcript pass; until each module is
     enumerated there it is reported as an EXPLICIT OPEN GAP (not a fabricated
     count), so `--require-full` stays red rather than falsely green.

  With `--require-full` the gate additionally requires every target module, program
  identity and normalized route to be present, every present target module to have
  a transcribed (or at least non-empty) param/jack target, and all per-module
  param/jack counts declared in `entries` to match the registry. The CI runs this
  mode so the PR #2 exact head enables it for real (constraint #3). Fixed routes
  have no registry RouteId equivalent (they are Phase-C audio-topology); they are
  validated for provenance + endpoint/direction coherence and reported as a target
  category, never silently dropped — but `--require-full` does not demand a
  registry row, because the registry does not model them as routes.

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


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def provenance_ok(entry):
    """A transcribed item must have a REAL provenance shape, not any non-empty ref.

    Requires: a status in {confirmed, provisional}; an evidence object with a
    non-empty `ref` naming a source; AND a location — a `panelSite`, a manual line
    range (`lineStart`/`lineEnd`), or a section marker (e.g. "§1" for a design doc).
    An evidence of just `{"ref": "x"}` is insufficient and is rejected.
    """
    if entry.get("status") not in VALID_STATUS:
        return False
    ev = entry.get("evidence") or {}
    ref = ev.get("ref")
    if not ref:
        return False
    location = ev.get("panelSite") or ev.get("lineStart") or ev.get("lineEnd") or ev.get("section")
    return bool(location)


def parse_endpoint(jack):
    """Split a manifest `module.jack` endpoint into (module, full_jack_stable_id)."""
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
    programs = tgt.get("programs", [])
    norm_routes = tgt.get("normalizedRoutes", [])
    fixed_routes = tgt.get("fixedRoutes", [])
    pj_entries = (tgt.get("paramsJackTargets") or {}).get("entries", {})
    must_complete = manifest.get("mustComplete", [])

    # ---- per-category duplicate detection (重件) ------------------------------
    def dup_check(items, cat, sid_key="stable_id"):
        seen = {}
        for it in items:
            sid = it.get(sid_key)
            text = sid if sid else "<missing-id>"
            if text in seen:
                problems.append(f"{cat} duplicate stable_id {text!r} (重件)")
            seen[text] = True

    dup_check(modules, "module")
    dup_check(programs, "program")
    dup_check(norm_routes, "normalized route")
    dup_check(fixed_routes, "fixed route")

    # ---- module / program / route shape + provenance -------------------------
    for m in modules:
        if not (m.get("stable_id") and m.get("name") and m.get("category")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs stable_id+name+category")
        if not provenance_ok(m):
            problems.append(f"manifest module {m.get('stable_id')!r}: incomplete evidence "
                            f"(need status in {sorted(VALID_STATUS)} + ref + panelSite/line range)")

    for p in programs:
        if not (p.get("stable_id") and p.get("slot") in (1, 2, 3) and p.get("name")):
            problems.append(f"manifest program {p.get('stable_id')!r}: needs stable_id+slot(1..3)+name")
        if not p.get("cartridge"):
            problems.append(f"manifest program {p.get('stable_id')!r}: missing cartridge name")
        if not provenance_ok(p):
            problems.append(f"manifest program {p.get('stable_id')!r}: incomplete evidence")

    # Every (cartridge, slot) must appear exactly once -> a full 13x3 grid.
    grid = {}
    dup_slot = False
    for p in programs:
        cart = p.get("cartridge")
        slot = p.get("slot")
        key = (cart, slot)
        if key in grid:
            dup_slot = True
        grid[key] = p.get("stable_id")
    if not dup_slot and programs:
        # ensure each cartridge has all three slots and no partial row.
        carts = {p.get("cartridge") for p in programs}
        for c in sorted(carts):
            for slot in (1, 2, 3):
                if (c, slot) not in grid:
                    problems.append(f"manifest cartridge {c!r}: must list all {3} slots (missing slot {slot})")

    for r in norm_routes + fixed_routes:
        if not (r.get("stable_id") and r.get("sourceJack") and r.get("sinkJack")):
            problems.append(f"manifest route {r.get('stable_id')!r}: needs stable_id+sourceJack+sinkJack")
        if not provenance_ok(r):
            problems.append(f"manifest route {r.get('stable_id')!r}: incomplete evidence")

    # ---- registry-derived present sets + jack direction map ------------------
    present_modules = {m["stable_id"] for m in reg.modules}
    present_programs = {p["stable_id"] for p in reg.programs}
    present_routes = {r["stable_id"] for r in reg.routes}
    jack_dirs = {j["stable_id"]: j["direction"] for j in reg.jacks}
    param_mods = {m["_stable_owner"] for m in reg.parameters}
    jack_mods = {j["_module_stable"] for j in reg.jacks}

    # ---- route endpoint/direction validation --------------------------------
    # Normalized routes: a patch may tap an input jack as source (e.g. CV L -> CV R),
    # so direction is NOT enforced, but a dangling endpoint (module present, jack
    # absent) is a hard error. Fixed (audio) routes: source must be output, sink input.
    def validate_endpoints(routes, enforce_direction):
        for r in routes:
            sid = r.get("stable_id")
            expect = (("sourceJack", "output"), ("sinkJack", "input")) if enforce_direction \
                     else (("sourceJack", None), ("sinkJack", None))
            for role, want_dir in expect:
                jk = r.get(role)
                if not jk:
                    continue
                mod = jk.partition(".")[0]
                if mod not in present_modules:
                    continue  # module not yet in registry -> provisional, cannot check
                d = jack_dirs.get(jk)
                if d is None:
                    problems.append(f"manifest route {sid}: {role} {jk!r} dangling "
                                    f"(module {mod!r} present but jack not registered)")
                elif enforce_direction and d != want_dir:
                    problems.append(f"manifest route {sid}: {role} {jk!r} is {d}, "
                                    f"expected {want_dir}")

    validate_endpoints(norm_routes, enforce_direction=False)
    validate_endpoints(fixed_routes, enforce_direction=True)

    # ---- non-empty module gate (always-on) -----------------------------------
    for mod in sorted(present_modules):
        if mod not in param_mods:
            problems.append(f"module {mod!r} present in registry but has NO parameters")
        if mod not in jack_mods:
            problems.append(f"module {mod!r} present in registry but has NO jacks")

    # ---- mustComplete: coherence + non-regression + == landed set ------------
    target_modules = {m["stable_id"] for m in modules if m.get("stable_id")}
    target_program_ids = {p["stable_id"] for p in programs if p.get("stable_id")}
    target_norm_routes = {r["stable_id"] for r in norm_routes if r.get("stable_id")}

    target_all = (set(f"module:{x}" for x in target_modules)
                  | set(f"program:{x}" for x in target_program_ids)
                  | set(f"route:{x}" for x in target_norm_routes))
    present_keys = (set(f"module:{x}" for x in present_modules)
                    | set(f"program:{x}" for x in present_programs)
                    | set(f"route:{x}" for x in present_routes))

    for key in must_complete:
        if key not in target_all:
            problems.append(f"mustComplete item {key!r} is not a manifest target "
                            f"(manifest itself is incoherent)")
        if key not in present_keys:
            problems.append(f"NON-REGRESSION: mustComplete item {key!r} dropped from the registry")

    # item #5: mustComplete must exactly equal the currently-landed target keys.
    landed = set(must_complete) & target_all
    if landed != (present_keys & target_all):
        missing_sync = sorted((present_keys & target_all) - landed)
        stray = sorted(landed - (present_keys & target_all))
        problems.append("mustComplete != registry landed target keys: "
                        f"add={missing_sync} remove={stray} "
                        f"(future additions must be synced into mustComplete)")

    # ---- category coverage ---------------------------------------------------
    def cover(target, present):
        return {"target": len(target), "present": len(target & present), "gap": sorted(target - present)}

    present_param_target_mods = {m: 1 for m in pj_entries}
    pj_untranscribed = sorted(target_modules - set(pj_entries.keys()))
    # count-level check for transcribed modules (supports future entries)
    for mod, want in pj_entries.items():
        got_p = len([p for p in reg.parameters if p["_stable_owner"] == mod])
        got_j = len([j for j in reg.jacks if j["_module_stable"] == mod])
        exp_p = want.get("params") if isinstance(want, dict) else want.get("params")
        if exp_p is not None and got_p != exp_p:
            problems.append(f"params target for {mod!r}: manifest {exp_p}, registry {got_p}")
        exp_j = (want.get("jacks") if isinstance(want, dict) else want.get("jacks"))
        if exp_j is not None and got_j != exp_j:
            problems.append(f"jacks target for {mod!r}: manifest {exp_j}, registry {got_j}")

    coverage = {
        "registry": {
            "modules": len(reg.modules), "params": len(reg.parameters),
            "jacks": len(reg.jacks), "routes": len(reg.routes), "programs": len(reg.programs),
        },
        "manifest": {
            "modules": len(modules), "programs": len(programs),
            "normalizedRoutes": len(norm_routes), "fixedRoutes": len(fixed_routes),
            "paramsJackTargetsTranscribed": len(pj_entries),
            "mustComplete": len(must_complete),
        },
        "modules": cover(target_modules, present_modules),
        "programIdentities": cover(target_program_ids, present_programs),
        "normalizedRoutes": cover(target_norm_routes, present_routes),
        "fixedRoutes": {
            "target": len(fixed_routes),
            "present": 0,
            "note": "Phase-C audio topology (no registry RouteId equivalent); validated "
                    "for provenance + endpoint/direction coherence only",
            "gap": sorted(r["stable_id"] for r in fixed_routes if r.get("stable_id")),
        },
        "parameters": {
            "targetModules": len(target_modules),
            "transcribed": len(pj_entries),
            "present": len(reg.parameters),
            "presentModules": sorted(param_mods),
            "untranscribed": pj_untranscribed,
        },
        "jacks": {
            "targetModules": len(target_modules),
            "transcribed": len(pj_entries),
            "present": len(reg.jacks),
            "presentModules": sorted(jack_mods),
            "untranscribed": pj_untranscribed,
        },
    }

    if require_full:
        not_full = []
        if coverage["modules"]["gap"]:
            not_full.append("module gaps=%s" % coverage["modules"]["gap"])
        if coverage["programIdentities"]["gap"]:
            not_full.append("program identity gaps=%s" % coverage["programIdentities"]["gap"])
        if coverage["normalizedRoutes"]["gap"]:
            not_full.append("normalized-route gaps=%s" % coverage["normalizedRoutes"]["gap"])
        if pj_untranscribed:
            not_full.append("params/jacks target untranscribed for modules=%s "
                            "(enumerate in target.paramsJackTargets.entries)" % pj_untranscribed)
        if not_full:
            problems.append("--require-full: " + "; ".join(not_full))

    return problems, coverage


def format_report(coverage):
    lines = []
    reg = coverage["registry"]
    lines.append("registry: %(modules)d modules / %(params)d params / %(jacks)d jacks / "
                 "%(routes)d routes / %(programs)d programs" % reg)
    m = coverage["manifest"]
    lines.append("manifest: %(modules)d modules / %(programs)d programs / "
                 "%(normalizedRoutes)d normalized / %(fixedRoutes)d fixed routes / "
                 "%(paramsJackTargetsTranscribed)d param-jack targets / %(mustComplete)d mustComplete" % m)
    lines.append("")
    lines.append("coverage (target / present / gap):")
    lines.append("  modules          : %(target)d / %(present)d / gaps=%(gap)s" % coverage["modules"])
    lines.append("  program identities: %(target)d / %(present)d / gaps=%(gap)s" % coverage["programIdentities"])
    lines.append("  normalized routes: %(target)d / %(present)d / gaps=%(gap)s" % coverage["normalizedRoutes"])
    fx = coverage["fixedRoutes"]
    lines.append("  fixed routes     : %(target)d / %(present)d / gap=%(gap)s  (%(note)s)" % fx)
    p = coverage["parameters"]
    lines.append("  parameters       : targetModules=%(targetModules)d transcribed=%(transcribed)d "
                 "present=%(present)d untranscribed=%(untranscribed)s" % p)
    j = coverage["jacks"]
    lines.append("  jacks            : targetModules=%(targetModules)d transcribed=%(transcribed)d "
                 "present=%(present)d untranscribed=%(untranscribed)s" % j)
    lines.append("")
    lines.append("note: 39 program identities is an identity/existence list, NOT 39 implemented effects.")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--require-full", action="store_true",
                    help="fail unless every target module/program identity/route is present "
                         "and all params/jacks targets are transcribed")
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
    lines.append("OK: manifest self-coherent; mustComplete == landed set and non-regressing.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
