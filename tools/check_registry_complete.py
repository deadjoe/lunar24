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
      {stable_id, owner, kind, region, leaf, panelLabel, positions|axes|operations, status, evidence}
      kind ∈ {continuous, selector-toggle, momentary-touch, rotary-encoder}
  target.parameters[]              -- the persisted LOGICAL state (the Parameter target)
      {stable_id, owner, kind, region, shape, cardinality|recordType|maskSize, semanticLabel?, ...}
      kind ∈ {continuous, selector-toggle, state-field}; shape ∈ {scalar, vector, record, mask}
  target.actions[]                 -- stable event identity for event-only widgets
      {stable_id, owner, name, kind, region, status, evidence}
      kind ∈ {trigger, encoder-rotate, encoder-press, encoder-long-press, init, save}
  target.controlBindings[]         -- widget -> parameter|action, with a structured context
      {stable_id, from, to, operation, context, axis?|index?, status, evidence}
  target.controlRegions[]          -- per-panel-region subtotal; the DECLARED count is an
      INDEPENDENT hand-counted constant (`expectedPanelControlCount`), never `len(rows)`.
  target.recordSchemas{}           -- declared payload shapes for record-typed parameters.
  manifest.migrationAllowlist{}    -- monotonically-shrinking legacy-rogue grandfather list.

Codex msg c7089521 (fifth review, still NOT GO) hardened this gate further:

  - Parameters carry an EXPLICIT shape (scalar|vector|record|mask) with cardinality /
    recordType / maskSize; a mis-scalar continuous/state field (joystick=2-axis,
    plate_tune=12, pushbutton_value=8, seq_editor=record, quantise_scale_editor=mask) is
    now structurally declared instead of an opaque semanticLabel.
  - Actions give the 27 event-only widgets a stable identity; controlBindings[].to may be a
    parameter OR an action, and binding semantics (operation + axis/index) are validated.
  - Context is a STRUCTURED closed-set object ({type: global|program|keyboard-menu|
    keyboard-mode, ...}); program context must reference a declared ProgramId, keyboard-menu
    must reference a legal menu, joystick axis / encoder operations are validated.
  - Per-ID gate is ALWAYS-ON for rogue ids: any registry Parameter/Jack not in the target AND
    not in `migrationAllowlist` is a NEW rogue and fails even without --require-full;
    --require-full additionally requires the allowlist empty and no rogue. Capability is split
    patchableJacks vs internalEndpoints (mixer/voices have no normal patchable jack). The
    empty-region check is `declaredRegionIds - actualRegionIds`.

1. CONSISTENCY INVARIANTS (always on):
   - real provenance shape (status + ref + panelSite/section/line);
   - no duplicate stable_id within ANY category (重件), incl. panelControls/parameters/actions/
     controlBindings/controlRegions;
   - every parameter/endpoint/panelControl/action `owner` is a target module or program/terminal;
     every kind/shape is in its closed enum; every parameter and panelControl names a real region;
   - controlBindings: every `from` is a panelControl, every `to` a parameter|action with a valid
     operation; context is a structured closed-set object with valid references; every parameter
     is bound (no orphan persisted state); every continuous/selector-toggle panelControl drives
     >=1 bound parameter; every event-only (momentary-touch/rotary-encoder) panelControl has >=1
     action binding;
   - REGION SUBTOTAL INDEPENDENCE: each region's emitted panelControl count == its INDEPENDENT
     expectedPanelControlCount, the subtotals sum to the full panelControls[] set, and no
     panelControl lives in a non-panel (state) region; a declared region with no widgets is an
     error (declaredRegionIds - actualRegionIds, not the old region_actual==0 form);
   - normalized/fixed route endpoints resolve in the inventory, are correctly directed, and
     internal (non-patchable) endpoints stay in fixed/route position;
   - per-CAPABILITY present-but-empty (parameters / patchableJacks / internalEndpoints /
     controls);
   - implementation ⊆ independent target (module/program/route): a registry item absent from the
     target is an error, never silently green;
   - mustComplete == exactly the registry's landed target keys (hand-authored in the manifest).

2. CATEGORY-WISE COVERAGE (never one %): modules / program identities / params / panel
   controls / actions / bindings / patchable jacks + internal endpoints / normalized routes /
   fixed routes.

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
import p0_regions_build

SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")
MANIFEST_PATH = os.path.join(ROOT, "spec", "machine", "p0_inventory_manifest.json")

VALID_STATUS = {"confirmed", "provisional"}
VALID_DIR = {"input", "output"}
VALID_KIND = {"audio", "control", "gate"}
PANEL_KINDS = {"continuous", "selector-toggle", "momentary-touch", "rotary-encoder"}
PARAM_KINDS = {"continuous", "selector-toggle", "state-field"}
SHAPES = {"scalar", "vector", "record", "mask"}
ACTION_KINDS = {"trigger", "encoder-rotate", "encoder-press", "encoder-long-press",
                "init", "save", "load", "initialise"}
# Binding semantics split (msg 9d8b5f43 item #1): a binding is THREE fields — the physical source
# edge (`sourceOperation`), what it targets (`targetKind`=parameter|action), and the effect
# (`targetOperation`). A single `operation` can no longer conflate "the encoder rotated" with
# "write this parameter", so the gate can enforce the EXACT per-widget source-op set and reject
# duplicate semantic tuples.
SOURCE_OPS = {"change", "rotate", "press", "release", "long_press"}
TGT_KINDS = {"parameter", "action"}
TGT_OPS = {"set", "adjust", "toggle", "invoke"}
# Per-widget-kind closed source-op set; the bindings wired for a widget must EQUAL it (not a subset,
# so deleting an encoder press edge or adding an undeclared release edge both fail).
WIDGET_SRC = {"continuous": {"change"}, "selector-toggle": {"change"},
              "momentary-touch": {"press", "release"},
              "rotary-encoder": {"rotate", "press", "long_press"}}
# Closed condition keys (msg 9d8b5f43 item #2): a compound gesture is expressed by a closed
# condition (heldControl board, menu item, preset slot, record index/field, mask index), not by a
# single opaque op.
# The ONLY condition selectors are the ones the binding model actually consumes: the held
# control that scopes a whole-vector write, the menu/mode item that scopes a menu dispatch, and
# the preset slot. recordField/recordIndex/maskIndex were declared but never validated nor used by
# any binding in the generator — so a binding carrying one passed silently. Removing them makes
# such a binding a closed-condition error (Codex 9th-review item #2b: remove-or-truly-validate).
COND_KEYS = {"heldControl", "menuItem", "presetSlot"}
PRESET_SLOTS = {"preset_a", "preset_b", "preset_c", "preset_d"}
# PRESETS two-level page state machine (Codex Phase-A 10th-review verdict msg e9f1f028). The manual
# (L1045-1046) documents TWO pages: rotate to scroll presets A-D (slot-list), then press to enter the
# selected slot's load/save/initialise sub-page (action-list). Each page is a REAL keyboard-menu
# context selector (page=slot-list | action-list) so the gate can tell a top-level slot list from an
# entered action sub-page instead of collapsing both into one `menu=presets` (the Round-10 root).
# `menuItem` on a presets binding must name a real DISPLAY ITEM of the current page — a slot in
# slot-list, a load/save/initialise action in action-list — and may be a DIFFERENT entity from the
# binding target, because the target may be a navigation action (preset_select_slot /
# preset_enter_subpage / preset_select_action) whose leaf is never shown on the panel. Display items
# and allowed targets per page are INDEPENDENT declarations (never inferred from any target's own
# `menu` field — that was the self-proving loop Codex diagnosed).
PRESET_PAGES = ("slot-list", "action-list")
PRESET_PAGE_ITEMS = {
    "slot-list":    ["preset_a", "preset_b", "preset_c", "preset_d"],
    "action-list":  ["presets_load", "presets_save", "presets_initialise"],
}
PRESET_PAGE_TARGETS = {
    "slot-list":    {"preset_select_slot", "preset_enter_subpage"},
    "action-list":  {"presets_load", "presets_save", "presets_initialise", "preset_select_action"},
}
CMD_ADDR_KINDS = {"record", "mask"}
SEQ_STEP_FIELDS = {"note", "value", "gate"}
CTX_TYPES = {"global", "program", "keyboard-menu", "keyboard-mode"}
KB_MENUS = {"behaviour", "mode", "arp", "sequencer", "portamento", "vibrato", "pressure",
            "quantiser", "clock", "calibration", "button-editor", "presets"}
# keyboard-mode modes: single/twin/split PLUS the calibration boot mode (a real Mode the user can
# enter; Codex a23a9618 item #2/#3 — calibration init/save live under it, not as an unbound menu).
KB_MODES = {"single", "twin", "split", "calibration"}
VALID_AXES = {"x", "y"}
# For each context type, the ONVALID key set under which the binding is valid (a closed context is
# only closed when the key set is precise, not just the type name — Codex a23a9618 item #3).
# 'program' requires 'program'; 'keyboard-menu' requires 'menu'; 'keyboard-mode' requires 'mode';
# 'global' forbids any context-specific selector (it is the unqualified default).
CTX_KEYS = {
    "global": set(),
    "program": {"program"},
    "keyboard-menu": {"menu"},
    "keyboard-mode": {"mode"},
}
# Independent legacy-rogue CEILING (Codex a23a9618 item #4): the frozen grandfather set of the
# pre-correction registry rogue ids, transcribed once here and NEVER grown from the manifest. The
# migration allowlist must be a subset of this ceiling (so a brand-new rogue cannot be masked by
# inventing an allow entry) AND equal to the ceiling-rogues still actually present in the registry
# (so a stale allow entry for a now-fixed id fails). The two together make it shrink-only.
LEGACY_ROGUE_PARAM_CEILING = {
    "envelope_a.attack", "envelope_a.release", "keyboard.pressure_signal",
    "program.cathedral.1.decay", "program.cathedral.1.octave_down",
    "program.cathedral.1.octave_up", "program.magic.1.delay", "program.magic.1.feedback",
    "program.magic.1.pitch", "vcf.l.dist", "vcf.l.freq", "vcf.l.gain", "vcf.l.mod",
    "vcf.l.res", "vcf.mode", "vcf.r.freq", "vco_a.fm_amt", "vco_a.oct_high",
    "vco_a.oct_low", "vco_a.shape", "vco_a.sub", "vco_a.wave", "vco_b.shape", "vco_b.wave",
}
LEGACY_ROGUE_JACK_CEILING = {"vcf.audio_in"}
# Parameter regions that hold LOGICAL state (program X/Y/Z, keyboard menu/state) but are NOT
# panel regions: they contribute no expectedPanelControlCount entry to controlRegions[].
STATE_REGIONS = {"program_params", "keyboard_state"}
PROGRAM_SLOTS = (1, 2, 3)
FAMILY_TYPES = {"reverb", "delay", "chorus", "vibrato", "tremolo", "wah", "filter",
                "phaser", "flanger", "pitch_shift", "pitched_delay", "ring_mod", "synth",
                "unknown"}


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


RECORD_PRIMITIVES = {"number", "voltage", "int", "bool", "label"}
RECORD_KINDS = {"record", "params"}


def expected_command_address(to_param, record_schemas):
    """Derive the EXACT commandAddress a binding MUST carry for an addressable target.

    A command-address locates a WRITE inside an editor object, so its kind/range/fields are not free
    (Codex 23ea438f #2). For a mask target it is `{kind: mask, indexRange: [0, maskSize-1]}`. For a
    record target it is `{kind: record, indexRange: [0, array.count-1], fields: <element field names>}`,
    where the editable array field is the record's first `array` field and the fields are that array
    element's field names (or the single element name for a primitive array). A non-addressable
    target returns None.
    """
    shape = (to_param or {}).get("shape")
    if shape == "mask":
        sz = (to_param or {}).get("maskSize")
        if type(sz) is int and sz > 0:
            return {"kind": "mask", "indexRange": [0, sz - 1]}
        return None
    if shape == "record":
        rt = (to_param or {}).get("recordType")
        sch = record_schemas.get(rt) or {}
        if not isinstance(sch, dict) or sch.get("kind") != "record":
            return None
        arr = next((f for f in sch.get("fields") or []
                    if isinstance(f, dict) and f.get("type") == "array"), None)
        if not arr:
            return None
        cnt = arr.get("count")
        if type(cnt) is not int or cnt <= 0:
            return None
        elem = arr.get("element") or {}
        if elem.get("type") == "record" and elem.get("of") in record_schemas:
            esch = record_schemas[elem["of"]]
            # Preserve the reference record's field ORDER (Codex 9th-review item #2a): the derived
            # fields must be EXACTLY equal to the declared ones as a list, so sorted() would silently
            # accept a re-ordered command-address. The element schema is validated elsewhere for
            # duplicate names, so order is the only thing to preserve here.
            fields = [f.get("name") for f in esch.get("fields") or [] if isinstance(f, dict)]
        else:
            fields = [elem.get("name") or elem.get("type")]
        return {"kind": "record", "indexRange": [0, cnt - 1], "fields": fields}
    return None


def validate_record_schemas(record_schemas, param_sids, param_record_types=None):
    """Validate each record schema BODY, not just that its name is referenced (Codex a23a9618
    item #1). A record is only closed when every `element`/`of` resolves to a real schema of the
    right kind, no array element is an inline anonymous map, and a preset's excludes do not
    contradict the params it actually stores. msg 9d8b5f43 item #3 adds: no direct/indirect record
    cycle; duplicate field names rejected; each field type's key-set precise; array/record sizes
    positive and non-bool; and the preset referenced-set + excludes must EXACTLY partition the
    keyboard state (no overlap, no miss)."""
    out = []

    # ---- record-graph cycle detection (direct or indirect) -------------------
    # Crosses both edge kinds (Codex 23ea438f #4): a record/array `of` reference AND a params-set
    # -> parameter(recordType) reference, so `record -> params -> parameter(recordType) -> record`
    # is a real cycle just like a direct `record -> record`.
    def _refs(nm, record_schemas, param_record_types):
        sch = record_schemas.get(nm) or {}
        if not isinstance(sch, dict):
            return []
        kind = sch.get("kind")
        if kind == "params":
            refs = set()
            for p in sch.get("params") or []:
                rt = (param_record_types or {}).get(p)
                if rt in record_schemas:
                    refs.add(rt)
            return sorted(refs)
        if kind != "record":
            return []
        refs = set()
        for f in sch.get("fields") or []:
            if not isinstance(f, dict):
                continue
            if f.get("type") == "record":
                of = f.get("of")
                if of in record_schemas:
                    refs.add(of)
            elif f.get("type") == "array":
                elem = f.get("element") or {}
                if elem.get("type") == "record" and elem.get("of") in record_schemas:
                    refs.add(elem["of"])
        return sorted(refs)

    WHITE, GREY, BLACK = 0, 1, 2
    color = {nm: WHITE for nm in record_schemas}

    def _dfs(nm):
        color[nm] = GREY
        for r in _refs(nm, record_schemas, param_record_types):
            if color.get(r) == GREY:
                return r
            if color.get(r) == WHITE:
                c = _dfs(r)
                if c is not None:
                    return c
        color[nm] = BLACK
        return None

    for nm in record_schemas:
        if color.get(nm) == WHITE:
            c = _dfs(nm)
            if c is not None:
                out.append(f"record schema {nm!r}: record graph has a cycle involving {c!r}")
                break

    for name, sch in record_schemas.items():
        if not isinstance(sch, dict):
            out.append(f"record schema {name!r} must be an object")
            continue
        kind = sch.get("kind")
        if kind not in RECORD_KINDS:
            out.append(f"record schema {name!r}: kind {kind!r} not in {sorted(RECORD_KINDS)}")
            continue
        # Exact top-level key-set (Codex 9th-review item #2c): a params schema carries only
        # kind/note/params, a record only kind/note/fields/excludes. A stray key masks a wrong shape
        # and must be rejected, not ignored.
        top_allowed = {"kind", "note"} | ({"params"} if kind == "params" else {"fields", "excludes"})
        top_extra = sorted({k for k in sch if k not in top_allowed})
        if top_extra:
            out.append(f"record schema {name!r}: top-level keys {top_extra} not allowed for kind "
                       f"{kind!r} (exact key-set is {sorted(top_allowed)})")
        if kind == "params":
            plist = sch.get("params") or []
            if not plist:
                out.append(f"record schema {name!r}: params list must be non-empty")
            dup_ps = sorted({p for p in plist if plist.count(p) > 1})
            if dup_ps:
                out.append(f"record schema {name!r}: duplicate param members {dup_ps}")
            for psid in plist:
                if psid not in param_sids:
                    out.append(f"record schema {name!r}: param {psid!r} is not a declared parameter")
            continue
        fields = sch.get("fields") or []
        if not fields:
            out.append(f"record schema {name!r}: record needs a non-empty fields list")
        field_names = [f.get("name") for f in fields if isinstance(f, dict)]
        dupnames = sorted({n for n in set(field_names) if field_names.count(n) > 1})
        if dupnames:
            out.append(f"record schema {name!r}: duplicate field names {dupnames}")
        excludes = set(sch.get("excludes") or [])
        for xsid in excludes:
            if xsid not in param_sids:
                out.append(f"record schema {name!r}: exclude {xsid!r} is not a declared parameter")
        for f in fields:
            fn = f.get("name")
            ftype = f.get("type")
            if not fn or (ftype not in RECORD_PRIMITIVES and ftype not in {"array", "record"}):
                out.append(f"record schema {name!r}: field {fn!r} type {ftype!r} is not a valid "
                           f"record field type")
                continue
            # Each field type carries a precise key-set: primitive fields take only name+type, record
            # adds `of`, array adds `count`+`element`. A stray key masks a wrong shape.
            allowed = {"name", "type"}
            if ftype == "record":
                allowed |= {"of"}
            elif ftype == "array":
                allowed |= {"count", "element"}
            extra = sorted({k for k in f if k not in allowed})
            if extra:
                out.append(f"record schema {name!r}: field {fn!r} has keys {extra} not allowed "
                           f"for type {ftype!r}")
            if ftype == "record":
                of = f.get("of")
                if of not in record_schemas or record_schemas[of].get("kind") not in RECORD_KINDS:
                    out.append(f"record schema {name!r}: field {fn!r} of={of!r} is not a defined "
                               f"record/params schema")
                else:
                    ref = record_schemas[of]
                    if ref.get("kind") == "params":
                        overlap = set(ref.get("params") or []) & excludes
                        if overlap:
                            out.append(f"record schema {name!r}: excludes {sorted(overlap)} overlap "
                                       f"the referenced param-set {of!r} (cannot exclude a param it "
                                       f"stores)")
            elif ftype == "array":
                count = f.get("count")
                # positive int AND not bool: isinstance(True,int) is True and 0/-1 are ints.
                if not (type(count) is int and count > 0):
                    out.append(f"record schema {name!r}: field {fn!r} needs positive int count "
                               f"(got {count!r}, bool excluded)")
                elem = f.get("element")
                if not isinstance(elem, dict) or not elem.get("type"):
                    out.append(f"record schema {name!r}: array field {fn!r} element must be a "
                               f"closed typed element (got anonymous map) — no inline anonymous "
                               f"element shape")
                    continue
                etype = elem.get("type")
                if etype == "record":
                    of = elem.get("of")
                    if of not in record_schemas or record_schemas[of].get("kind") not in RECORD_KINDS:
                        out.append(f"record schema {name!r}: array field {fn!r} element of={of!r} "
                                   f"is not a defined record/params schema")
                elif etype not in RECORD_PRIMITIVES:
                    out.append(f"record schema {name!r}: array field {fn!r} element type "
                               f"{etype!r} not in {sorted(RECORD_PRIMITIVES)}")
                # Exact key-set on the ELEMENT too (Codex 9th-review item #2c): a primitive element
                # takes name+type, a record element adds `of`. A stray key on an element masks a wrong
                # shape and must be rejected.
                elem_allowed = {"name", "type"} | ({"of"} if etype == "record" else set())
                elem_extra = sorted({k for k in elem if k not in elem_allowed})
                if elem_extra:
                    out.append(f"record schema {name!r}: array field {fn!r} element has keys "
                               f"{elem_extra} not allowed for type {etype!r}")
        # Precise partition: a record that references a params-kind set AND defines `excludes` must
        # have those two EXACTLY cover the keyboard param domain — no overlap (a param both stored
        # and excluded) and no miss (a stored param silently dropped from both). msg 9d8b5f43 item
        # #3.
        param_ref_fields = [f for f in fields
                            if isinstance(f, dict) and f.get("type") == "record"
                            and record_schemas.get(f.get("of"), {}).get("kind") == "params"]
        if excludes and param_ref_fields:
            included = set()
            for f in param_ref_fields:
                included |= set((record_schemas[f["of"]].get("params") or []))
            keyboard_domain = {p for p in param_sids if p.startswith("keyboard.")}
            if included & excludes:
                out.append(f"record schema {name!r}: params {sorted(included & excludes)} both stored "
                           f"and excluded (overlap)")
            missing = keyboard_domain - included - excludes
            if missing:
                out.append(f"record schema {name!r}: params {sorted(missing)} neither stored nor "
                           f"excluded (keyboard-state partition has a miss)")
    return out


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
    landed_facts = tgt.get("landedDescriptorFacts") or {}
    landed_params = landed_facts.get("parameters") or {}
    landed_jacks = landed_facts.get("jacks") or {}
    landed_route_facts = tgt.get("landedRouteFacts") or {}
    landed_routes = landed_route_facts.get("routes") or {}

    modules = tgt.get("modules", [])
    terminals = tgt.get("terminals", [])
    programs = tgt.get("programs", [])
    panel_controls = tgt.get("panelControls", [])
    parameters = tgt.get("parameters", [])
    actions = tgt.get("actions", [])
    bindings = tgt.get("controlBindings", [])
    control_regions = tgt.get("controlRegions", [])
    record_schemas = tgt.get("recordSchemas", {}) or {}
    pjt = tgt.get("paramsJackTargets") or {}
    endpoints = pjt.get("endpoints", [])
    norm_routes = tgt.get("normalizedRoutes", [])
    fixed_routes = tgt.get("fixedRoutes", [])
    required_fixed = tgt.get("requiredFixedRoutes", [])
    must_complete = manifest.get("mustComplete", [])
    allowlist = manifest.get("migrationAllowlist") or {}
    allow_params = set(allowlist.get("parameters", []) or [])
    allow_jacks = set(allowlist.get("jacks", []) or [])

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
    dup_check(actions, "action")
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
                isinstance(cap.get(k), bool) for k in ("parameters", "patchableJacks",
                                                       "internalEndpoints", "controls")):
            problems.append(f"manifest module {m.get('stable_id')!r}: needs capabilities with "
                            f"parameters/patchableJacks/internalEndpoints/controls each a bool")
        if not provenance_ok(m):
            problems.append(f"manifest module {m.get('stable_id')!r}: incomplete evidence "
                            f"(need status in {sorted(VALID_STATUS)} + ref + panelSite/section/line)")
    module_ids = {m["stable_id"] for m in modules if m.get("stable_id")}
    # msg 9d8b5f43 item #3: the capability frozen table must be EXACTLY the module-ID set. No
    # DEFAULT_CAP fallback exists in the generator; here the gate independently verifies the
    # hand-written CAPABILITIES keys == module stable_ids, and that every module has a real entry.
    cap_keys = set(p0_regions_build.CAPABILITIES)
    if cap_keys != module_ids:
        problems.append(f"capability table {sorted(cap_keys)} != module stable_ids "
                        f"{sorted(module_ids)} (must be exactly equal, no DEFAULT_CAP fallback)")

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
        shape = it.get("shape")
        if shape not in SHAPES:
            problems.append(f"manifest parameter {sid!r}: shape {shape!r} not in {sorted(SHAPES)}")
        # Positive-int cardinality/maskSize, and NOT bool: isinstance(True,int) is True and 0/-1 are
        # ints, so all three would pass a bare `isinstance(int)` (Codex 9d8b5f43 item #3). Use strict
        # `type(x) is int` so bool and non-positive are rejected.
        if shape == "vector":
            card = it.get("cardinality")
            if not (type(card) is int and card > 0):
                problems.append(f"manifest parameter {sid!r}: shape=vector needs positive int "
                                f"cardinality (got {card!r})")
        if shape == "record" and it.get("recordType") not in record_schemas:
            problems.append(f"manifest parameter {sid!r}: shape=record needs a declared "
                            f"recordType (have {sorted(record_schemas)})")
        if shape == "mask":
            ms = it.get("maskSize")
            if not (type(ms) is int and ms > 0):
                problems.append(f"manifest parameter {sid!r}: shape=mask needs positive int "
                                f"maskSize (got {ms!r})")
        # A selector-toggle PARAMETER carries discrete positions (Root B, Codex 22f545c1): whenever
        # a target declares positions, the registry descriptor cardinality must equal it (checked in
        # the descriptor-coherence block below). Here: positions must be a non-empty label list, and
        # only valid for a selector-toggle on a scalar state field.
        if it.get("kind") == "selector-toggle":
            pos = it.get("positions")
            if pos is not None:
                if not (isinstance(pos, list) and pos and all(isinstance(x, str) and x for x in pos)):
                    problems.append(f"manifest parameter {sid!r}: selector-toggle positions must be "
                                    f"a non-empty list of non-empty labels (got {pos!r})")
                if shape != "scalar":
                    problems.append(f"manifest parameter {sid!r}: selector-toggle positions only "
                                    f"valid for shape=scalar (got {shape!r})")
        elif it.get("positions") is not None:
            problems.append(f"manifest parameter {sid!r}: positions only valid for "
                            f"kind=selector-toggle")
        if shape != "vector" and it.get("cardinality") is not None:
            problems.append(f"manifest parameter {sid!r}: cardinality only valid for shape=vector")
        if shape != "record" and it.get("recordType") is not None:
            problems.append(f"manifest parameter {sid!r}: recordType only valid for shape=record")
        if shape != "mask" and it.get("maskSize") is not None:
            problems.append(f"manifest parameter {sid!r}: maskSize only valid for shape=mask")
        if it.get("owner") not in (module_ids | program_stable_ids):
            problems.append(f"manifest parameter {sid!r}: owner {it.get('owner')!r} "
                            f"not a target module or program")
        if it.get("region") not in ({r.get("id") for r in control_regions} | STATE_REGIONS):
            problems.append(f"manifest parameter {sid!r}: region {it.get('region')!r} is not a "
                            f"panel region nor {sorted(STATE_REGIONS)}")
        if not provenance_ok(it):
            problems.append(f"manifest parameter {sid!r}: incomplete evidence")
        param_sids.add(sid)

    # ---- record schema bodies: the reference is not enough, the body must be closed (item #1) ---
    param_record_types = {it.get("stable_id"): it.get("recordType") for it in parameters
                          if it.get("shape") == "record" and it.get("recordType")}
    problems.extend(validate_record_schemas(record_schemas, param_sids, param_record_types))

    # ---- panelControls: one row per real physical widget --------------------
    pc_sids = set()
    pc_by_id = {}
    region_actual = {}
    for c in panel_controls:
        sid = c.get("stable_id")
        if not (sid and c.get("owner") and c.get("kind") in PANEL_KINDS):
            problems.append(f"manifest panelControl {sid!r}: needs stable_id+owner+kind in "
                            f"{sorted(PANEL_KINDS)}")
        if not c.get("leaf") or not c.get("panelLabel"):
            problems.append(f"manifest panelControl {sid!r}: needs leaf + panelLabel "
                            f"(verbatim panel label separate from the internal leaf)")
        if c.get("kind") == "selector-toggle" and (not c.get("positions") or
                                                   not isinstance(c.get("positions"), list)):
            problems.append(f"manifest panelControl {sid!r}: selector-toggle needs a non-empty "
                            f"positions[] (discrete position labels)")
        if c.get("kind") == "rotary-encoder" and not c.get("operations"):
            problems.append(f"manifest panelControl {sid!r}: rotary-encoder needs operations "
                            f"[{ {'rotate','press','long_press'} }]")
        if c.get("axes") and not set(c.get("axes")) <= VALID_AXES:
            problems.append(f"manifest panelControl {sid!r}: axes {c.get('axes')} not within "
                            f"{sorted(VALID_AXES)}")
        if c.get("owner") not in (allowed_owners | program_stable_ids):
            problems.append(f"manifest panelControl {sid!r}: owner {c.get('owner')!r} is not a "
                            f"target module, terminal, or program")
        if c.get("region") not in {r.get("id") for r in control_regions}:
            problems.append(f"manifest panelControl {sid!r}: region {c.get('region')!r} is not a "
                            f"declared panel controlRegion")
        if not provenance_ok(c):
            problems.append(f"manifest panelControl {sid!r}: incomplete evidence")
        pc_sids.add(sid)
        pc_by_id[sid] = c
        region_actual[c.get("region")] = region_actual.get(c.get("region"), 0) + 1

    region_ids = {r.get("id") for r in control_regions}

    # ---- actions: stable event identity for event-only widgets -------------
    action_sids = set()
    act_kinds = {}   # action kind -> count (independent of the parameter-shape `sz` counter)
    for a in actions:
        sid = a.get("stable_id")
        if not (sid and a.get("owner") and a.get("name")):
            problems.append(f"manifest action {sid!r}: needs stable_id+owner+name")
        if a.get("kind") not in ACTION_KINDS:
            problems.append(f"manifest action {sid!r}: kind {a.get('kind')!r} not in "
                            f"{sorted(ACTION_KINDS)}")
        act_kinds[a.get("kind")] = act_kinds.get(a.get("kind"), 0) + 1
        if a.get("owner") not in (allowed_owners | program_stable_ids):
            problems.append(f"manifest action {sid!r}: owner {a.get('owner')!r} is not a target "
                            f"module, terminal, or program")
        if a.get("region") not in (region_ids | STATE_REGIONS):
            problems.append(f"manifest action {sid!r}: region {a.get('region')!r} is not a panel "
                            f"region nor {sorted(STATE_REGIONS)}")
        if not provenance_ok(a):
            problems.append(f"manifest action {sid!r}: incomplete evidence")
        action_sids.add(sid)

    # ---- controlBindings: widget -> parameter|action ------------------------
    binding_from_pc = {}
    binding_to_param = {}
    binding_to_action = {}
    ops_by_pc = {}    # from -> set of operations actually wired (post-op validation)
    axes_by_pc = {}   # from -> set of axes actually wired (axis bindings only)
    param_by_sid = {p.get("stable_id"): p for p in parameters}
    # menuItems / modeItems: the set of stable_ids each keyboard menu (or calibration MODE) actually
    # holds, from the declared `menu`/`mode` on parameters+actions. A `menuItem` condition must name an
    # item of the binding's OWN menu/mode (Codex 9th-review item #1b), not an arbitrary string.
    menu_items = {}
    mode_items = {}
    for _it in list(parameters) + list(actions):
        _sid = _it.get("stable_id") or ""
        if _it.get("menu"):
            menu_items.setdefault(_it["menu"], set()).add(_sid)
        if _it.get("mode"):
            mode_items.setdefault(_it["mode"], set()).add(_sid)
    for b in bindings:
        sid = b.get("stable_id")
        if not (sid and b.get("from") and b.get("to")):
            problems.append(f"manifest controlBinding {sid!r}: needs stable_id+from+to")
            continue
        if b.get("from") not in pc_sids:
            problems.append(f"manifest controlBinding {sid}: from {b.get('from')!r} is not a "
                            f"declared panelControl")
        to_param = param_by_sid.get(b.get("to"))
        if "operation" in b:
            # The legacy conflated field is rejected outright: it cannot express the split, and the
            # whole point of the split is that the gate no longer carries an `op`-based coupling.
            problems.append(f"manifest controlBinding {sid}: legacy conflated 'operation' field "
                            f"present (must use sourceOperation+targetKind+targetOperation)")
            continue
        src = b.get("sourceOperation")
        if src not in SOURCE_OPS:
            problems.append(f"manifest controlBinding {sid}: sourceOperation {src!r} not in "
                            f"{sorted(SOURCE_OPS)}")
        tkind = b.get("targetKind")
        if tkind not in TGT_KINDS:
            problems.append(f"manifest controlBinding {sid}: targetKind {tkind!r} not in "
                            f"{sorted(TGT_KINDS)}")
        top = b.get("targetOperation")
        if top not in TGT_OPS:
            problems.append(f"manifest controlBinding {sid}: targetOperation {top!r} not in "
                            f"{sorted(TGT_OPS)}")
        # An action can only be INVOKED; a parameter can only be set/adjust/toggled, never invoked.
        if tkind == "action" and top != "invoke":
            problems.append(f"manifest controlBinding {sid}: targetKind=action must use "
                            f"targetOperation=invoke (got {top!r})")
        if tkind == "parameter" and top == "invoke":
            problems.append(f"manifest controlBinding {sid}: targetKind=parameter cannot carry "
                            f"targetOperation=invoke")
        if tkind == "action":
            if b.get("to") not in action_sids:
                problems.append(f"manifest controlBinding {sid}: action target {b.get('to')!r} "
                                f"is not a declared action")
            else:
                binding_to_action[b.get("to")] = True
        else:
            if b.get("to") not in param_sids:
                problems.append(f"manifest controlBinding {sid}: parameter target {b.get('to')!r} "
                                f"is not a declared parameter")
            else:
                binding_to_param[b.get("to")] = True
        ctx = b.get("context")
        ctype = None
        if not isinstance(ctx, dict) or not ctx.get("type"):
            problems.append(f"manifest controlBinding {sid}: missing structured context "
                            f"({{type: global|program|keyboard-menu|keyboard-mode}})")
        else:
            ctype = ctx["type"]
            if ctype not in CTX_TYPES:
                problems.append(f"manifest controlBinding {sid}: context type {ctype!r} not in "
                                f"{sorted(CTX_TYPES)}")
            else:
                # The context is only closed when the KEY SET is precise for its type, not just the
                # type name (Codex a23a9618 item #3): 'global' carries no selector, program carries
                # only 'program', menu only 'menu', mode only 'mode'. An extra selector key is a
                # masked-context bug regardless of whether the type name is legal.
                actual_ctx_keys = {k for k in ctx if k != "type"}
                # The PRESETS menu is a two-page state machine, so keyboard-menu=presets must carry
                # a `page` selector (Codex e9f1f028); every other menu is a single implicit page.
                if ctype == "keyboard-menu" and ctx.get("menu") == "presets":
                    needed_ctx_keys = {"menu", "page"}
                else:
                    needed_ctx_keys = CTX_KEYS[ctype]
                if actual_ctx_keys != needed_ctx_keys:
                    problems.append(f"manifest controlBinding {sid}: context type {ctype!r} must "
                                    f"carry exactly {sorted(needed_ctx_keys)} (got "
                                    f"{sorted(actual_ctx_keys)})")
                if ctype == "program" and ctx.get("program") not in program_stable_ids:
                    problems.append(f"manifest controlBinding {sid}: program context references "
                                    f"unknown ProgramId {ctx.get('program')!r}")
                elif ctype == "keyboard-menu" and ctx.get("menu") not in KB_MENUS:
                    problems.append(f"manifest controlBinding {sid}: keyboard-menu context menu "
                                    f"{ctx.get('menu')!r} not in {sorted(KB_MENUS)}")
                elif ctype == "keyboard-menu" and ctx.get("menu") == "presets" and \
                        ctx.get("page") not in PRESET_PAGES:
                    problems.append(f"manifest controlBinding {sid}: presets page "
                                    f"{ctx.get('page')!r} not in {sorted(PRESET_PAGES)}")
                elif ctype == "keyboard-mode" and ctx.get("mode") not in KB_MODES:
                    problems.append(f"manifest controlBinding {sid}: keyboard-mode context mode "
                                    f"{ctx.get('mode')!r} not in {sorted(KB_MODES)}")
        if b.get("axis") is not None:
            fromw = pc_by_id.get(b.get("from")) or {}
            axes = fromw.get("axes") or []
            if b["axis"] not in axes:
                problems.append(f"manifest controlBinding {sid}: axis {b['axis']!r} is not a "
                                f"declared axis of {b.get('from')}")
            expect_to = fromw.get("owner") + "." + b["axis"] if fromw.get("owner") else None
            if expect_to and b.get("to") != expect_to:
                problems.append(f"manifest controlBinding {sid}: axis {b['axis']!r} must target "
                                f"{expect_to!r}, not {b.get('to')!r}")
        if b.get("index") is not None:
            # A held-index binding ("hold plate i then turn encoder") must land on a VECTOR
            # parameter and stay inside its cardinality (Codex a23a9618 item #2). An index on a
            # scalar/record is a malformed binding.
            card = (to_param or {}).get("cardinality")
            if (to_param or {}).get("shape") != "vector" or not isinstance(card, int):
                problems.append(f"manifest controlBinding {sid}: index {b['index']!r} requires "
                                f"the target {b.get('to')!r} to be a vector parameter")
            elif not (1 <= b["index"] <= card):
                problems.append(f"manifest controlBinding {sid}: index {b['index']!r} outside "
                                f"cardinality 1..{card} of {b.get('to')!r}")
        elif to_param and to_param.get("shape") == "vector":
            # A vector parameter WITHOUT an index is a whole-vector binding: it would write every
            # element at once (Codex 9th-review item #1c). Vector params (plate_tune / pushbutton_value)
            # are edited one element at a time via the heldControl+index path in the GLOBAL context.
            problems.append(f"manifest controlBinding {sid}: vector target {b.get('to')!r} needs an "
                            f"index (whole-vector binding banned)")
        # Condition: the KEY NAMES are closed, and the RELATIONSHIP is executable (msg 23ea438f #2).
        cond = b.get("condition")
        if cond is not None:
            if not isinstance(cond, dict) or not cond:
                problems.append(f"manifest controlBinding {sid}: condition must be a non-empty "
                                f"object of closed keys")
            else:
                for k, v in cond.items():
                    if k not in COND_KEYS:
                        problems.append(f"manifest controlBinding {sid}: condition key {k!r} not in "
                                        f"{sorted(COND_KEYS)}")
                        continue
                    if k == "presetSlot":
                        if v not in PRESET_SLOTS:
                            problems.append(f"manifest controlBinding {sid}: condition presetSlot "
                                            f"{v!r} not in {sorted(PRESET_SLOTS)}")
                        elif ("keyboard." + v) not in param_sids:
                            problems.append(f"manifest controlBinding {sid}: presetSlot {v!r} has "
                                            f"no matching keyboard payload parameter")
                    elif k == "menuItem":
                        # menuItem names the highlighted menu/mode item the binding acts on. It may be a
                        # PARAMETER OR an ACTION (Codex 9th-review item #1b), so the old "actions only"
                        # restriction is gone. It must equal the target's leaf (self-consistency) AND the
                        # target must genuinely belong to the binding's own menu/mode.
                        toid = b.get("to") or ""
                        leaf = toid.split(".", 1)[-1] if "." in toid else toid
                        if ctype == "keyboard-menu" and ctx.get("menu") == "presets":
                            # PRESETS two-page state machine (Codex e9f1f028): menuItem names a real
                            # DISPLAY ITEM of the current page (a slot in slot-list, a load/save/
                            # initialise action in action-list) — it is NOT necessarily the target's
                            # leaf, because a navigation action (preset_select_slot /
                            # preset_enter_subpage / preset_select_action) is bound per slot/operation
                            # but is never itself a displayed choice. The topology is an INDEPENDENT
                            # declaration (PRESET_PAGE_ITEMS / PRESET_PAGE_TARGETS) never inferred from
                            # a target's own `menu` field (the self-proving loop Codex diagnosed).
                            page = ctx.get("page")
                            if page not in PRESET_PAGES:
                                problems.append(f"manifest controlBinding {sid}: presets menuItem "
                                                f"{v!r} page {page!r} not in {sorted(PRESET_PAGES)}")
                            elif v not in PRESET_PAGE_ITEMS[page]:
                                problems.append(f"manifest controlBinding {sid}: presets menuItem "
                                                f"{v!r} is not a display item of page {page!r} "
                                                f"(display items "
                                                f"{sorted(PRESET_PAGE_ITEMS[page])})")
                            elif leaf not in PRESET_PAGE_TARGETS[page]:
                                problems.append(f"manifest controlBinding {sid}: presets page "
                                                f"{page!r} menuItem {v!r} target {toid!r} not in "
                                                f"allowed targets "
                                                f"{sorted(PRESET_PAGE_TARGETS[page])}")
                        elif v != leaf:
                            problems.append(f"manifest controlBinding {sid}: menuItem {v!r} does "
                                            f"not name the target leaf {leaf!r}")
                        elif ctype == "keyboard-menu" and ctx.get("menu") not in menu_items:
                            problems.append(f"manifest controlBinding {sid}: menuItem {v!r} menu "
                                            f"{ctx.get('menu')!r} has no declared items")
                        elif ctype == "keyboard-menu" and toid not in menu_items.get(ctx.get("menu"), set()):
                            problems.append(f"manifest controlBinding {sid}: menuItem {v!r} target "
                                            f"{toid!r} is not an item of menu {ctx.get('menu')!r}")
                        elif ctype == "keyboard-mode" and ctx.get("mode") not in mode_items:
                            problems.append(f"manifest controlBinding {sid}: menuItem {v!r} mode "
                                            f"{ctx.get('mode')!r} has no declared items")
                        elif ctype == "keyboard-mode" and toid not in mode_items.get(ctx.get("mode"), set()):
                            problems.append(f"manifest controlBinding {sid}: menuItem {v!r} target "
                                            f"{toid!r} is not an item of mode {ctx.get('mode')!r}")
                        elif ctype not in ("keyboard-menu", "keyboard-mode"):
                            problems.append(f"manifest controlBinding {sid}: menuItem only applies "
                                            f"to a keyboard-menu/keyboard-mode context (got "
                                            f"{ctype!r})")
                    elif k == "heldControl":
                        # The held control must be a REAL momentary-touch control, and it must map
                        # 1:1 with the vector element the binding writes (its numeric suffix equals
                        # the binding index, and it sits on the same owner as the target).
                        hw = pc_by_id.get(v)
                        if hw is None:
                            problems.append(f"manifest controlBinding {sid}: heldControl {v!r} is "
                                            f"not a declared panelControl")
                        else:
                            if hw.get("kind") != "momentary-touch":
                                problems.append(f"manifest controlBinding {sid}: heldControl {v!r} "
                                                f"is not a momentary-touch control")
                            if b.get("index") is not None:
                                suffix = (hw.get("leaf") or "").rsplit("_", 1)[-1]
                                if suffix != str(b["index"]):
                                    problems.append(f"manifest controlBinding {sid}: heldControl "
                                                    f"{v!r} (element {suffix!r}) does not match "
                                                    f"binding index {b['index']}")
                            if to_param and hw.get("owner") != to_param.get("owner"):
                                problems.append(f"manifest controlBinding {sid}: heldControl {v!r} "
                                                f"owner {hw.get('owner')!r} != target owner "
                                                f"{to_param.get('owner')!r}")
        # commandAddress: present ONLY on an addressable record/mask editor, and its kind/range/
        # fields must be DERIVED from the target shape + record schema / maskSize (msg 23ea438f #2).
        ca = b.get("commandAddress")
        expect_ca = expected_command_address(to_param, record_schemas)
        if ca is not None:
            if expect_ca is None:
                problems.append(f"manifest controlBinding {sid}: commandAddress on non-addressable "
                                f"target {b.get('to')!r} (shape {to_param.get('shape') if to_param else None!r}; "
                                f"only record/mask editors take a commandAddress)")
            else:
                ca_extra = sorted({k for k in ca if k not in ("kind", "indexRange", "fields")})
                if ca_extra:
                    problems.append(f"manifest controlBinding {sid}: commandAddress keys {ca_extra} "
                                    f"not allowed (exact key-set is kind/indexRange/fields)")
                if not isinstance(ca, dict) or ca.get("kind") != expect_ca["kind"]:
                    problems.append(f"manifest controlBinding {sid}: commandAddress kind "
                                    f"{ca.get('kind') if isinstance(ca, dict) else None!r} != "
                                    f"expected {expect_ca['kind']!r} for target {b.get('to')!r}")
                elif (not isinstance(ca.get("indexRange"), list) or ca["indexRange"] !=
                      expect_ca["indexRange"]):
                    problems.append(f"manifest controlBinding {sid}: commandAddress indexRange "
                                    f"{ca.get('indexRange')!r} != expected "
                                    f"{expect_ca['indexRange']!r} for target {b.get('to')!r}")
                elif expect_ca["kind"] == "record":
                    flds = ca.get("fields")
                    if not isinstance(flds, list) or not flds:
                        problems.append(f"manifest controlBinding {sid}: record commandAddress needs "
                                        f"a non-empty fields list")
                    elif len(set(flds)) != len(flds):
                        problems.append(f"manifest controlBinding {sid}: record commandAddress fields "
                                        f"{flds} contain a duplicate (must be unique)")
                    # EXACT list equality (Codex 9th-review item #2a): set-equality would silently
                    # accept a re-ordered or dropped-and-readded field, so compare against the derived
                    # list in order.
                    elif flds != expect_ca["fields"]:
                        problems.append(f"manifest controlBinding {sid}: record commandAddress fields "
                                        f"{flds} != expected {expect_ca['fields']} for target "
                                        f"{b.get('to')!r}")
        elif expect_ca is not None:
            problems.append(f"manifest controlBinding {sid}: addressable target {b.get('to')!r} "
                            f"(shape {to_param.get('shape')!r}) needs a commandAddress")
        # Dispatch closure (Codex 9th-review item #1a): a binding in a keyboard-menu/keyboard-mode
        # context MUST be item-scoped — it MUST carry a closed `menuItem` naming the highlighted item.
        # Without it the encoder fires for EVERY item the menu holds (selector-less fan-out). The
        # condition key-set check above has already verified any menuItem it carries belongs to this
        # binding's own menu/mode, so here we only require the selector to be present.
        if ctype in ("keyboard-menu", "keyboard-mode"):
            if not isinstance(cond, dict) or "menuItem" not in cond:
                problems.append(f"manifest controlBinding {sid}: a {ctype} context binding must carry "
                                f"a menuItem condition (one event must act on one item, not fan out "
                                f"over the whole menu)")
        if not provenance_ok(b):
            problems.append(f"manifest controlBinding {sid}: incomplete evidence")
        binding_from_pc[b.get("from")] = binding_from_pc.get(b.get("from"), 0) + 1
        if src in SOURCE_OPS:
            ops_by_pc.setdefault(b.get("from"), set()).add(src)
        if b.get("axis") is not None:
            axes_by_pc.setdefault(b.get("from"), set()).add(b["axis"])

    # ---- PRESETS two-page state machine invariants (Codex e9f1f028) -------------
    # The presets workflow is a real two-page machine (slot-list -> action-list), so BOTH pages must
    # actually be present in the bindings (delete/merge a page -> fail) and the enter-subpage edge
    # must carry the slot it enters (enter without a slot -> fail). Without these the data cannot
    # tell a top-level A-D slot list from an entered load/save/init action sub-page — the exact
    # Round-10 root Codex diagnosed.
    preset_pages_seen = {}
    preset_enter_no_slot = []
    for b in bindings:
        bctx = b.get("context") or {}
        if bctx.get("type") == "keyboard-menu" and bctx.get("menu") == "presets":
            page = bctx.get("page")
            preset_pages_seen[page] = preset_pages_seen.get(page, 0) + 1
            if (b.get("to") or "").rsplit(".", 1)[-1] == "preset_enter_subpage":
                cond = b.get("condition") or {}
                if "presetSlot" not in cond:
                    preset_enter_no_slot.append(b.get("stable_id"))
    for page in PRESET_PAGES:
        if not preset_pages_seen.get(page):
            problems.append(f"presets two-page state machine: page {page!r} has no bindings "
                            f"(both {sorted(PRESET_PAGES)} must exist)")
    if preset_enter_no_slot:
        problems.append(f"presets enter-subpage must carry a closed presetSlot: "
                        f"{sorted(preset_enter_no_slot)}")

    # ---- PRESETS EXACT relation-set gate (Codex 11th-review verdict msg 8cc637e9) ----
    # The page/menuItem/slot topology is correct in the data, but the gate above only enforced "each
    # page has >=1 binding and values are in the allowed set" — so deleting ANY single edge (one slot's
    # enter-subpage, one slot's select_slot, one operation's select_action) still returned 0 problems,
    # and an enter whose menuItem and presetSlot disagree also passed. The manual's PRESETS matrix is
    # CLOSED and small (2 pages x 4 slots x 3 operations), so we PROJECT every presets binding onto a
    # canonical relation tuple and require EXACT SET EQUALITY with that closed matrix. A tuple matches
    # only when menuItem is the real surface item AND the press edges pin menuItem == presetSlot (and
    # the execute edge additionally pins target == menuItem); missing one, extra one, or a misaligned
    # slot/item pairing all fail (the misalignment is BOTH a missing aligned tuple and an extra one).
    preset_action_labels = PRESET_PAGE_ITEMS["action-list"]
    expected_preset_relations = set()
    for s in PRESET_SLOTS:
        expected_preset_relations.add(("slot-list", "rotate", "preset_select_slot", s, None))
        expected_preset_relations.add(("slot-list", "press", "preset_enter_subpage", s, s))
    for o in preset_action_labels:
        expected_preset_relations.add(("action-list", "rotate", "preset_select_action", o, None))
        for s in PRESET_SLOTS:
            expected_preset_relations.add(("action-list", "press", o, o, s))
    # Round 12 (Codex 11th-review verdict msg 0153c91f): the exact set above only collected bindings
    # whose CONTEXT is `menu=presets`, so a PRESETS workflow action (preset_select_slot /
    # preset_enter_subpage / preset_select_action / presets_load / presets_save / presets_initialise)
    # could be REBOUND into a legal program/global context and escape the matrix — NORMAL gate returns
    # 0 problems while the six workflow actions now exist outside the two-page machine. So the six
    # workflow targets are a CLOSED set: ANY binding whose `to` is one of them must sit on a PRESETS
    # page (and therefore inside the expected set). We additionally project such a stray binding so it
    # shows up as an extra tuple, AND emit an explicit diagnosis rather than only a count.
    presets_workflow_targets = {
        "keyboard.preset_select_slot", "keyboard.preset_enter_subpage",
        "keyboard.preset_select_action", "keyboard.presets_load",
        "keyboard.presets_save", "keyboard.presets_initialise",
    }
    actual_preset_relations = set()
    for b in bindings:
        bctx = b.get("context") or {}
        is_presets_page = (bctx.get("type") == "keyboard-menu" and bctx.get("menu") == "presets")
        is_workflow_target = (b.get("to") in presets_workflow_targets)
        if is_workflow_target and not is_presets_page:
            problems.append(f"controlBinding {b.get('stable_id')!r}: PRESETS workflow action "
                            f"{b.get('to')!r} bound outside a PRESETS page (context {bctx!r}); "
                            f"the PRESETS workflow actions may only appear in the presets "
                            f"two-page machine")
        if is_presets_page or is_workflow_target:
            cond = b.get("condition") or {}
            page = bctx.get("page") if is_presets_page else None
            actual_preset_relations.add(
                (page, b.get("sourceOperation"), (b.get("to") or "").rsplit(".", 1)[-1],
                 cond.get("menuItem"), cond.get("presetSlot")))
    if actual_preset_relations != expected_preset_relations:
        missing = sorted(expected_preset_relations - actual_preset_relations)
        extra = sorted(actual_preset_relations - expected_preset_relations)
        problems.append(
            f"presets relation set != closed manual matrix: missing={len(missing)} "
            f"extra={len(extra)} (missing={missing}; extra={extra})")

    # ---- reject duplicate semantic tuples (msg 9d8b5f43 item #1) ----------------
    # Two bindings describing the SAME (from,to,source,targetKind,targetOp,axis,index,condition,
    # commandAddress) are un-executable — both would fire. Surface them rather than let one win.
    seen_tup = {}
    for b in bindings:
        # Context is part of semantic identity (msg 23ea438f #4): the SAME from/to under two legal
        # contexts (e.g. rotate-to-set X in menu A vs menu B) is two distinct bindings, not one.
        tup = (b.get("from"), b.get("to"), b.get("sourceOperation"), b.get("targetKind"),
               b.get("targetOperation"), b.get("axis"), b.get("index"),
               json.dumps(b.get("context"), sort_keys=True) if b.get("context") else None,
               json.dumps(b.get("condition"), sort_keys=True) if b.get("condition") else None,
               json.dumps(b.get("commandAddress"), sort_keys=True) if b.get("commandAddress") else None)
        if tup in seen_tup:
            problems.append(f"duplicate controlBinding semantic tuple at {b.get('stable_id')!r}"
                            f" (also {seen_tup[tup]!r})")
        else:
            seen_tup[tup] = b.get("stable_id")

    # ---- reject dispatch fan-out (Codex 9th-review item #1) -------------------
    # One physical event under one selector state must hit exactly ONE target. Group bindings by
    # (from, context, sourceOperation, axis, index, condition) — the state that decides WHICH binding
    # fires; if a group holds TWO OR MORE distinct targets, the same event+state would trigger them all
    # at once. This is the selector-less fan-out the 13 keyboard encoder groups (menu params, presets,
    # calibration) had, and joystick X/Y axis targeting is naturally excluded because the group key
    # carries `axis`.
    dispatch_groups = {}
    for b in bindings:
        grp_key = (b.get("from"),
                   json.dumps(b.get("context"), sort_keys=True) if b.get("context") else None,
                   b.get("sourceOperation"), b.get("axis"), b.get("index"),
                   json.dumps(b.get("condition"), sort_keys=True) if b.get("condition") else None)
        dispatch_groups.setdefault(grp_key, {})[b.get("to")] = True
    for grp_key, tos in dispatch_groups.items():
        if len(tos) > 1:
            problems.append(f"binding dispatch fan-out: one event ({grp_key[0]}, "
                            f"{grp_key[2]!r}, ctx={grp_key[1]}, axis={grp_key[3]}, "
                            f"index={grp_key[4]}, cond={grp_key[5]}) fires multiple targets "
                            f"{sorted(tos)}")

    # ---- per-widget completeness: wired source ops == declared source ops (msg 9d8b5f43 item #1) --
    # EQUALITY, not subset: deleting an encoder press edge OR adding an undeclared release edge must
    # both fail. The declared set is the closed kind→source-op map (WIDGET_SRC); the encoder's own
    # `operations` list, if present, must agree with it.
    for c in panel_controls:
        sid = c.get("stable_id")
        kind = c.get("kind")
        declared_src = WIDGET_SRC.get(kind)
        if declared_src is not None:
            explicit = c.get("operations")
            if explicit is not None:
                expl_src = {o.get("name") for o in explicit}
                if expl_src != declared_src:
                    problems.append(f"panelControl {sid}: declared operations {sorted(expl_src)} "
                                    f"do not match kind {kind!r} {sorted(declared_src)}")
            wired_src = ops_by_pc.get(sid, set())
            for o in sorted(declared_src - wired_src):
                problems.append(f"panelControl {sid}: declared source operation {o!r} has no "
                                f"controlBinding (declared source ops must equal wired)")
            for o in sorted(wired_src - declared_src):
                problems.append(f"panelControl {sid}: wired source operation {o!r} is not declared "
                                f"by kind {kind!r} (wired source ops must equal declared)")
        declared_axes = c.get("axes") or []
        wired_axes = axes_by_pc.get(sid, set())
        for ax in declared_axes:
            if ax not in wired_axes:
                problems.append(f"panelControl {sid}: declared axis {ax!r} has no controlBinding")
        for ax in sorted(wired_axes - set(declared_axes)):
            problems.append(f"panelControl {sid}: wired axis {ax!r} is not declared by the widget")

    # every persisted parameter must be reachable via a widget (no orphan state); every
    # continuous/selector-toggle panelControl must drive a persisted parameter; every event-only
    # widget must carry at least one action binding. ACTION-MANAGED params (preset payloads) are
    # written only by presets_* actions, so they are exempt from the widget-reachability rule (msg
    # 9d8b5f43 item #2).
    # ACTION-MANAGED params (preset payloads) are written only by presets_* actions, so they are
    # exempt from the widget-reachability rule — but the flag must NOT be an arbitrary orphan-escape
    # (msg 23ea438f #3): it is valid only on a preset payload, and each payload must be covered by
    # the A-D load/save/initialise action relationship, not merely by the boolean.
    action_managed_sids = {p.get("stable_id") for p in parameters if p.get("actionManaged")}
    preset_payload_sids = {p.get("stable_id") for p in parameters
                           if p.get("shape") == "record" and p.get("recordType") == "keyboard_preset"}
    if action_managed_sids != preset_payload_sids:
        extra = sorted(action_managed_sids - preset_payload_sids)
        if extra:
            problems.append(f"actionManaged on non-preset-payload parameter(s) {extra} (only preset "
                            f"payloads, written by presets_load/save/initialise, may carry "
                            f"actionManaged)")
        missing = sorted(preset_payload_sids - action_managed_sids)
        if missing:
            problems.append(f"preset payload parameter(s) {missing} must be actionManaged "
                            f"(written only by presets_load/save/initialise)")
    preset_op_actions = {"keyboard.presets_load", "keyboard.presets_save",
                         "keyboard.presets_initialise"}
    slot_ops = {s: set() for s in PRESET_SLOTS}
    for b in bindings:
        cs = b.get("condition") or {}
        ps = cs.get("presetSlot")
        if ps in slot_ops and b.get("targetKind") == "action" and b.get("to") in preset_op_actions:
            slot_ops[ps].add(b.get("to"))
    for ps in sorted(slot_ops):
        if preset_op_actions - slot_ops[ps]:
            problems.append(f"keyboard preset payload for slot {ps} not covered by A-D "
                            f"load/save/initialise (missing {sorted(preset_op_actions - slot_ops[ps])})")
    orphan_params = sorted(param_sids - set(binding_to_param) - action_managed_sids)
    if orphan_params:
        problems.append(f"parameters with no controlBinding (orphan persisted state): "
                        f"{orphan_params}")
    unbound_persist = sorted(
        {c.get("stable_id") for c in panel_controls if c.get("kind") in
         {"continuous", "selector-toggle"}} - set(binding_from_pc))
    if unbound_persist:
        problems.append(f"continuous/selector-toggle panelControls with no controlBinding: "
                        f"{unbound_persist}")
    unbound_action_events = sorted(
        {c.get("stable_id") for c in panel_controls if c.get("kind") in
         {"momentary-touch", "rotary-encoder"}} - set(binding_from_pc))
    if unbound_action_events:
        problems.append(f"event-only panelControls with no action binding: {unbound_action_events}")

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
    if control_regions:
        undeclared = sorted(set(region_actual) - set(by_rid))
        if undeclared:
            problems.append(f"panelControls in undeclared regions (no controlRegion subtotal): "
                            f"{undeclared}")
    total_counted = sum(r.get("expectedPanelControlCount")
                        for r in control_regions if isinstance(r.get("expectedPanelControlCount"), int))
    if panel_controls and total_counted != len(panel_controls):
        problems.append(f"controlRegions expectedPanelControlCount subtotal {total_counted} "
                        f"!= panelControls[] length {len(panel_controls)}")
    # A DECLARED panel region must contain real widgets: empty_or_absent = declaredRegionIds -
    # actualRegionIds (Codex item #5), NOT the old region_actual==0 form. Logical state regions are
    # separate and excluded from controlRegions[].
    declared_region_ids = set(by_rid)
    actual_region_ids = {rid for rid, n in region_actual.items() if n > 0}
    empty_regions = sorted(declared_region_ids - actual_region_ids)
    if empty_regions:
        problems.append(f"panel controlRegions declared but with no panelControls: {empty_regions}")

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

    # ---- program identity CONTENT exact-compare (Codex msg f9a4bdae NON-GO) ----
    # Slice #57 (b1b17df) locked only the program stable-id SET; the identity content (family /
    # selfOscillating / fieldEvidence provenance / cartridge / status / evidence ref+lines) still
    # drifted past NORMAL. The independent target now freezes id/family/selfOscillating/fieldEvidence
    # per program, so the gate exact-compares the actual descriptor against the frozen target facts
    # by stable_id. `id` is position-identity (matches the 0..38 manual order) and is also compared
    # so a renumber is caught.
    tprog = {p["stable_id"]: p for p in programs if p.get("stable_id")}
    aprog = {p["stable_id"]: p for p in reg.programs}
    # Closed family vocabulary (design/05): a registry program family must be a known DSP family
    # (or "unknown"); a garbage family is rejected even if it happens to carry legal provenance.
    for sid, p in aprog.items():
        fam = p.get("family")
        if fam not in FAMILY_TYPES:
            problems.append(f"implementation program {sid!r}: family {fam!r} not in closed "
                            f"family vocabulary {sorted(FAMILY_TYPES)}")
        if fam not in FAMILY_TYPES:
            continue
        t = tprog.get(sid)
        if t is None:
            continue
        # Each identity field must equal the frozen target fact. `id` is position-identity
        # (0..38, matches the manual order), so a renumber is caught here; cartridge/slot/
        # name/status are the remaining immutable identity facts.
        for f in ("id", "cartridge", "slot", "name", "status", "family", "selfOscillating"):
            if p.get(f) != t.get(f):
                problems.append(f"implementation program {sid!r}: {f} {p.get(f)!r} != frozen target "
                                f"{t.get(f)!r}")
        tfe = t.get("fieldEvidence") or {}
        pfe = p.get("fieldEvidence") or {}
        for f in ("family", "selfOscillating"):
            if pfe.get(f) != tfe.get(f):
                problems.append(f"implementation program {sid!r}: fieldEvidence.{f} {pfe.get(f)!r} "
                                f"!= frozen target {tfe.get(f)!r}")
        te = t.get("evidence") or {}
        aev = p.get("evidence") or {}
        for f in ("ref", "lineStart", "lineEnd"):
            if aev.get(f) != te.get(f):
                problems.append(f"implementation program {sid!r}: evidence.{f} {aev.get(f)!r} "
                                f"!= frozen target {te.get(f)!r}")

    # ---- landed-descriptor fact gate (Codex 17cc9a4a) ----
    # The frozen identity/evidence facts for this slice live in
    # target.landedDescriptorFacts (hand-authored in the manifest; the builder only loads
    # and preserves it — Codex msg 8165f8c2 Root 1). Each FACTS entry's
    # appearance IS the declaration that it has landed, so a fact the registry lacks is
    # a MISSING landed descriptor and must FAIL normal (it must not reopen a gap). The
    # facts carry a descriptorEvidence.line at widget granularity (matching what the
    # registry cites); the parent region-span `evidence` in the registry is untouched —
    # only the facts' own descriptorEvidence line is compared. Exact-compare each fact:
    # renumber / re-owner / kind drift / status / descriptorEvidence.line, and for a jack
    # direction / signalType / polarity / coupling / nominalRange / fieldEvidence are all
    # caught at the landed-descriptor granularity.
    reg_param_by_fact = {p["stable_id"]: p for p in reg.parameters}
    reg_jack_by_fact = {j["stable_id"]: j for j in reg.jacks}
    for sid, f in sorted(landed_params.items()):
        rp = reg_param_by_fact.get(sid)
        if rp is None:
            problems.append(f"MISSING landed descriptor param {sid!r}: in "
                            f"target.landedDescriptorFacts but absent from the registry")
            continue
        kind = "selector-toggle" if rp.get("positions") else "continuous"
        de = f.get("descriptorEvidence") or {}
        got = (rp.get("id"), rp.get("_stable_owner"), kind, rp.get("status"),
               (rp.get("evidence") or {}).get("line"))
        want = (f["id"], f["owner"], f["kind"], f["status"], de.get("line"))
        if got != want:
            problems.append(f"implementation param {sid!r}: landed descriptor fact "
                            f"(id={got[0]}, owner={got[1]}, kind={got[2]}, status={got[3]}, "
                            f"descriptorEvidence.line={got[4]}) != target.landedDescriptorFacts "
                            f"(id={want[0]}, owner={want[1]}, kind={want[2]}, status={want[3]}, "
                            f"descriptorEvidence.line={want[4]})")
    fe_keys = ("nominalRange", "toleratedRange", "threshold", "saturation", "transfer",
               "signalType", "polarity", "coupling")
    for sid, f in sorted(landed_jacks.items()):
        rj = reg_jack_by_fact.get(sid)
        if rj is None:
            problems.append(f"MISSING landed descriptor jack {sid!r}: in "
                            f"target.landedDescriptorFacts but absent from the registry")
            continue
        rjfe = rj.get("fieldEvidence") or {}
        de = f.get("descriptorEvidence") or {}
        ffe = f.get("fieldEvidence") or {}
        got = (rj.get("id"), rj.get("_module_stable"), rj.get("direction"), rj.get("signalType"),
               rj.get("polarity"), rj.get("nominalMin"), rj.get("nominalMax"), rj.get("coupling"),
               rj.get("status"), (rj.get("evidence") or {}).get("line"),
               *(rjfe.get(k) for k in fe_keys))
        want = (f["id"], f["owner"], f["direction"], f["signalType"], f["polarity"], f["min"], f["max"],
                f["coupling"], f["status"], de.get("line"), *(ffe.get(k) for k in fe_keys))
        if got != want:
            problems.append(f"implementation jack {sid!r}: landed descriptor fact "
                            f"(id={got[0]}, owner={got[1]}, direction={got[2]}, signalType={got[3]}, "
                            f"polarity={got[4]}, nominalMin={got[5]}, nominalMax={got[6]}, "
                            f"coupling={got[7]}, status={got[8]}, descriptorEvidence.line={got[9]}, "
                            f"fieldEvidence={got[10:]}) != target.landedDescriptorFacts "
                            f"(id={want[0]}, owner={want[1]}, direction={want[2]}, signalType={want[3]}, "
                            f"polarity={want[4]}, min={want[5]}, max={want[6]}, coupling={want[7]}, "
                            f"status={want[8]}, descriptorEvidence.line={want[9]}, "
                            f"fieldEvidence={want[10:]})")

    # ---- landed ROUTE fact gate (Codex msg b527ef3b / f2868b6b NON-GO) ----
    # target.landedRouteFacts is the ONLY registry-specific declaration of what has landed for
    # normalized routes; it deliberately holds just the fields the canonical target.normalizedRoutes
    # does NOT (numeric `id` + a widget-granularity descriptorEvidence{ref,line}). source/sink/status
    # are NEVER duplicated here — target.normalizedRoutes is their single authority, and the actual
    # route is exact-compared against it, so drifting the canonical target sink/status (or the actual
    # sink/status) is caught without a parallel copy to lie about. The facts key-set must EXACTLY
    # equal the present (landed) route set and every fact must reference a canonical target route:
    # dropping a fact is a hard fail no matter which route it was.
    #
    # Chain of authority per landed route:
    #   facts key-set == present_routes (deleting a fact   -> fail; removing a route -> MISSING+regen)
    #   fact.sid ∈ canonical target.normalizedRoutes      (a fact may not name a non-canonical route)
    #   actual.sourceJack/sinkJack/status == canonical route            (single authority, no dup copy)
    #   actual.id == fact.id                                           (renumber caught)
    #   fact.descriptorEvidence.ref == canonical route evidence.ref    (same citation, not a new one)
    #   fact.descriptorEvidence.line ∈ [route.lineStart, lineEnd]      (widget line inside target span)
    #   actual.evidence.line == fact.descriptorEvidence.line           (effective evidence == fact)
    reg_route_by_fact = {r["stable_id"]: r for r in reg.routes}
    troute_by_id = {r["stable_id"]: r for r in norm_routes if r.get("stable_id")}
    fact_sids = set(landed_routes.keys())
    # (a) every landed route must have a fact
    for sid in sorted(present_routes - fact_sids):
        problems.append(f"implementation route {sid!r}: present in the registry but absent from "
                        f"target.landedRouteFacts (every landed route must have a route fact)")
    # (b) every fact must name a present (landed) route
    for sid in sorted(fact_sids - present_routes):
        problems.append(f"MISSING landed descriptor route {sid!r}: in target.landedRouteFacts "
                        f"but absent from the registry")
    # (c) every fact must reference a canonical target.normalizedRoutes entry
    for sid in sorted(fact_sids - troute_by_id.keys()):
        problems.append(f"landed route fact {sid!r}: not in target.normalizedRoutes (a route fact "
                        f"must reference a canonical target route)")
    for sid in sorted(fact_sids & present_routes & troute_by_id.keys()):
        rr = reg_route_by_fact[sid]
        t = troute_by_id[sid]
        f = landed_routes[sid]
        # source/sink/status: canonical target.normalizedRoutes is the single authority.
        for field in ("sourceJack", "sinkJack", "status"):
            if rr.get(field) != t.get(field):
                problems.append(f"implementation route {sid!r}: {field} {rr.get(field)!r} "
                                f"!= target.normalizedRoutes {t.get(field)!r}")
        # id: registry-specific, carried in the fact (renumber caught).
        if rr.get("id") != f.get("id"):
            problems.append(f"implementation route {sid!r}: id {rr.get('id')!r} != landed fact "
                            f"id {f.get('id')!r}")
        de = f.get("descriptorEvidence") or {}
        te = t.get("evidence") or {}
        # descriptorEvidence must cite the SAME evidence ref as the canonical target route, and its
        # widget-granularity line must fall inside the target route's region span.
        de_line, de_ref = de.get("line"), de.get("ref")
        if de_ref != te.get("ref"):
            problems.append(f"implementation route {sid!r}: descriptorEvidence.ref {de_ref!r} "
                            f"!= target evidence ref {te.get('ref')!r}")
        ls, le = te.get("lineStart"), te.get("lineEnd")
        if de_line is not None and (ls is not None and de_line < ls or
                                    le is not None and de_line > le):
            problems.append(f"implementation route {sid!r}: descriptorEvidence.line {de_line} "
                            f"outside target span [{ls},{le}]")
        # effective evidence: the actual route's evidence.line must equal the fact's.
        if (rr.get("evidence") or {}).get("line") != de_line:
            problems.append(f"implementation route {sid!r}: evidence.line "
                            f"{(rr.get('evidence') or {}).get('line')} != descriptorEvidence.line "
                            f"{de_line}")
        # actual EFFECTIVE evidence ref: route evidence carries no `ref` in the registry, so the
        # generator fills it with generate_registry.DEFAULT_SOURCE (registry meta has no `source`,
        # evidence_expr falls back to the DEFAULT_SOURCE). That is the ref the generated C++
        # descriptor actually carries, so it must equal the fact/target ref. Without this, drifting
        # the target ref AND the fact ref together while the actual stays on DEFAULT_SOURCE slipped
        # past NORMAL (Codex msg 478e06f6).
        actual_ref = (rr.get("evidence") or {}).get("ref", generate_registry.DEFAULT_SOURCE)
        if actual_ref != de_ref:
            problems.append(f"implementation route {sid!r}: evidence.ref {actual_ref!r} "
                            f"!= descriptorEvidence.ref {de_ref!r}")

    # ---- per-CAPABILITY present-but-empty (replaces blanket param+jack) ----
    tmodel = {m["stable_id"]: m for m in modules if m.get("stable_id")}
    reg_param_owners = {p["_stable_owner"] for p in reg.parameters}
    reg_jack_owners = {j["_module_stable"] for j in reg.jacks}
    cap_owners_target = {c.get("owner") for c in panel_controls}
    internal_owners_target = {e.get("owner") for e in endpoints if not e.get("patchable")}
    for mod in sorted(present_modules):
        tm = tmodel.get(mod)
        cap = (tm or {}).get("capabilities") or {}
        if cap.get("parameters") and mod not in reg_param_owners:
            problems.append(f"module {mod!r} declares capability parameters but the registry "
                            f"provides none")
        if cap.get("patchableJacks") and mod not in reg_jack_owners:
            problems.append(f"module {mod!r} declares capability patchableJacks but the registry "
                            f"provides no patchable jack")
        if cap.get("internalEndpoints") and mod not in internal_owners_target:
            problems.append(f"module {mod!r} declares capability internalEndpoints but the target "
                            f"transcribes no internal endpoint")
        if cap.get("controls") and mod not in cap_owners_target:
            problems.append(f"module {mod!r} declares capability controls but the target "
                            f"transcribes no panelControl")

    # ---- mustComplete coherence + non-regression + == landed set ------------
    # Landed-equality covers the FULL actual registry (every landed module / program identity /
    # normalized route / parameter / patchable jack), not just the descriptor-fact slice: the
    # manifest mustComplete is the single auditable list of exactly what the registry currently
    # holds, so deleting ANY entity (old or new) is caught as a drop, and adding any is a gap
    # until it is synced into mustComplete.
    present_reg_params = {p["stable_id"] for p in reg.parameters}
    present_reg_jacks = {j["stable_id"] for j in reg.jacks}
    target_all = (set(f"module:{x}" for x in module_ids)
                  | set(f"program:{x}" for x in target_program_ids)
                  | set(f"route:{x}" for x in target_norm_routes)
                  | set(f"parameter:{p['stable_id']}" for p in parameters)
                  | set(f"jack:{e['stable_id']}" for e in endpoints if e.get("patchable")))
    present_keys = (set(f"module:{x}" for x in present_modules)
                    | set(f"program:{x}" for x in present_programs)
                    | set(f"route:{x}" for x in present_routes)
                    | set(f"parameter:{x}" for x in present_reg_params)
                    | set(f"jack:{x}" for x in present_reg_jacks))
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
    modules_untranscribed = sorted(
        m for m in module_ids
        if (tmodel.get(m, {}).get("capabilities") or {}).get("parameters")
        and m not in param_owners_target)

    def cover(target, present):
        return {"target": len(target), "present": len(target & present), "gap": sorted(target - present)}

    tgt_patchable = {e["stable_id"] for e in endpoints if e.get("patchable")}
    reg_jack_sids = {j["stable_id"] for j in reg.jacks}
    reg_param_sids = {p["stable_id"] for p in reg.parameters}
    # ALWAYS-ON no-new-rogue (Codex item #5): a registry Parameter/Jack that is neither in the
    # target nor on the migration allowlist is a NEW rogue and fails even without --require-full.
    new_param_rogue = sorted(reg_param_sids - param_sids - allow_params)
    if new_param_rogue:
        problems.append(f"parameter registry-rogue NOT migration-allowed (NEW): {new_param_rogue}")
    new_jack_rogue = sorted(reg_jack_sids - tgt_patchable - allow_jacks)
    if new_jack_rogue:
        problems.append(f"patchable jack registry-rogue NOT migration-allowed (NEW): {new_jack_rogue}")
    rogue_params_migrate = sorted((reg_param_sids - param_sids) & allow_params)
    rogue_jacks_migrate = sorted((reg_jack_sids - tgt_patchable) & allow_jacks)
    # Item #4 (monotonic allowlist): the manifest allowlist is trusted only up to the independent
    # ceiling. Any allow entry OUTSIDE the ceiling is an attempt to mask a new rogue; any allow
    # entry for an id that is no longer a registry rogue is stale. Both fail.
    allow_param_not_ceiling = sorted(allow_params - LEGACY_ROGUE_PARAM_CEILING)
    if allow_param_not_ceiling:
        problems.append(f"migration allowlist parameters NOT in legacy ceiling (cannot mask a "
                        f"new rogue): {allow_param_not_ceiling}")
    allow_jack_not_ceiling = sorted(allow_jacks - LEGACY_ROGUE_JACK_CEILING)
    if allow_jack_not_ceiling:
        problems.append(f"migration allowlist jacks NOT in legacy ceiling (cannot mask a new "
                        f"rogue): {allow_jack_not_ceiling}")
    still_param = sorted((reg_param_sids - param_sids) & LEGACY_ROGUE_PARAM_CEILING)
    still_jack = sorted((reg_jack_sids - tgt_patchable) & LEGACY_ROGUE_JACK_CEILING)
    stale_param = sorted(allow_params - set(still_param))
    if stale_param:
        problems.append(f"stale migration allowlist parameters (id no longer a registry rogue): "
                        f"{stale_param}")
    stale_jack = sorted(allow_jacks - set(still_jack))
    if stale_jack:
        problems.append(f"stale migration allowlist jacks (id no longer a registry rogue): "
                        f"{stale_jack}")
    # ---- Root A/B/C: descriptor coherence vs independent target (Codex 22f545c1) ----------
    # A ParameterDescriptor may only back a SCALAR target. A non-scalar target (vector/mask/record)
    # has real shape/cardinality and must stay a GAP rather than be faked as a scalar (Root A).
    # A selector-toggle target with KNOWN positions has discrete cardinality; the registry
    # descriptor's (max-min)/step+1 AND its option labels must equal it — a bool (max-min=1)
    # can never cover a 3-position selector (Root B + Codex 03848819 Root 2).
    # Every scalar ParameterDescriptor carries a per-field Provenance audit; unknown value
    # domains are provisional, never silently concrete (Root C).
    #
    # Codex 03848819 Root 2 closes the "delete positions to pass" hole. The old check only
    # validated cardinality WHEN the target already held positions, so a selector-toggle target
    # stripped of its positions silently skipped every check. Here we iterate the INDEPENDENT
    # TARGET (not the registry), and:
    #   * a selector-toggle target with NO positions has no evidenced value domain → it MUST stay a
    #     GAP; if the registry implements it at all that is the "un-evidenced bool treated as
    #     implemented" class Codex flagged.
    #   * a selector-toggle target WITH positions, once the registry implements it, must carry
    #     EXACTLY the same option count AND labels (UI/MIDI must never receive a bare 0/1/2 integer
    #     without knowing what each value means). A target-with-positions that is NOT yet implemented
    #     is a legitimate Phase-B GAP — the whole module may still be unimplemented — and --require-full
    #     sees it via parameters.gap, so it is never reported here.
    #   * the value domain is a software-normalized index space, not a documented hardware value,
    #     so the `range` field evidence must be `provisional`; an `unverified` range next to a
    #     concrete min/max is precisely the "seemingly-certain bool" mis-read.
    FE_FIELDS = ("range", "unit", "default", "step", "smoothing", "persistence")
    FE_STATUS = {"confirmed", "unverified", "provisional"}
    reg_param_by_sid = {p["stable_id"]: p for p in reg.parameters}
    # Physical selector domain: a panelControl of kind selector-toggle carries the authoritative
    # position list for its stable_id (Codex 644ea86e Root 1). The builder projects target positions
    # FROM these panel widgets, so the gate cross-checks every physical selector's target domain
    # against its panel widget — regardless of whether the registry has implemented it yet. This is
    # what closes the "unimplemented target stripped of positions (delete-to-pass)" hole: vco_b.oct_sel
    # has a panel domain, so deleting its target positions must fail even though the registry's
    # --require-full already reports it as target-not-implemented.
    panel_pos_by_sid = {
        pc["stable_id"]: pc.get("positions")
        for pc in panel_controls
        if pc.get("kind") == "selector-toggle" and pc.get("positions")
    }
    for tp in parameters:
        if tp.get("kind") != "selector-toggle":
            continue
        sid = tp.get("stable_id")
        tpos = tp.get("positions")
        rp = reg_param_by_sid.get(sid)
        # Label uniqueness (Codex 644ea86e Root 2): a selector's value domain must not repeat a
        # label — two identical options are indistinguishable to UI/MIDI (["same","same"]).
        if tpos and len(tpos) != len(set(tpos)):
            problems.append(
                f"parameter {sid}: selector positions contain a duplicate label {tpos!r} "
                f"(UI/MIDI cannot disambiguate two identical options)")
        # target↔panel cross-check: for a physical selector the domain is EXACTLY its panel widget's,
        # and this holds even when the registry has not yet implemented the parameter.
        if sid in panel_pos_by_sid:
            ppos = panel_pos_by_sid[sid]
            if tpos != ppos:
                problems.append(
                    f"parameter {sid}: target positions {tpos!r} != panelControl positions "
                    f"{ppos!r} (physical selector domain must be projected from and equal its "
                    f"panel widget)")
        if not tpos:
            if rp is not None:
                problems.append(
                    f"parameter {sid}: selector-toggle target has NO positions (no evidence for a "
                    f"value domain) but the registry implements it — it must stay a GAP")
            continue
        if rp is None:
            continue
        rpos = rp.get("positions")
        if rpos != tpos:
            problems.append(
                f"parameter {sid}: registry selector options {rpos!r} != target positions "
                f"{tpos!r} (UI/MIDI must see exactly the documented labels)")
        card = (rp["max"] - rp["min"]) / rp["step"] + 1.0 if rp["step"] > 0 else None
        if card is None or abs(card - len(tpos)) > 1e-9:
            problems.append(
                f"parameter {sid}: selector-toggle cardinality {card!r} != positions count "
                f"{len(tpos)} (bool cannot cover a multi-position selector)")
        fe = rp.get("fieldEvidence") or {}
        # Six-field non-unverified (Codex 644ea86e Root 2): an implemented selector consumes a value
        # for every fieldEvidence field, so an `unverified` status next to a concrete value is the
        # "seemingly-certain but un-evidenced" mis-read. Software-mapped index space → provisional;
        # documented hardware fact → confirmed; never unverified on a consumed value.
        for fk in FE_FIELDS:
            if fe.get(fk) == "unverified":
                problems.append(
                    f"parameter {sid}: implemented selector fieldEvidence.{fk} is unverified — a "
                    f"consumed value must be provisional or confirmed, never unverified (a concrete "
                    f"value can't also be un-evidenced)")
    for p in reg.parameters:
        tp = param_by_sid.get(p["stable_id"])
        tshape = (tp or {}).get("shape")
        if tshape not in (None, "scalar"):
            problems.append(
                f"parameter {p['stable_id']}: target shape {tshape!r} is non-scalar — cannot be "
                f"represented as a scalar ParameterDescriptor (must gap)")
        # A registry parameter may only carry selector positions if its target is a selector-toggle.
        if (tp or {}).get("kind") != "selector-toggle" and p.get("positions"):
            problems.append(
                f"parameter {p['stable_id']}: registry carries selector positions but its target "
                f"is not a selector-toggle (positions are only valid on a selector-toggle)")
        fe = p.get("fieldEvidence")
        if not isinstance(fe, dict):
            problems.append(f"parameter {p['stable_id']}: missing fieldEvidence audit object")
        else:
            for fk in FE_FIELDS:
                if fk not in fe:
                    problems.append(f"parameter {p['stable_id']}: fieldEvidence missing {fk}")
                elif fe[fk] not in FE_STATUS:
                    problems.append(f"parameter {p['stable_id']}: fieldEvidence.{fk} bad status "
                                    f"{fe[fk]!r}")
    # ---- unknown-enum / evidence contract (Codex 2026-08-23, 587f5e72) -----------
    # Signal class (signalType/polarity/coupling) and program family/selfOscillating are
    # their own audited facts. The rule is a BICONDITIONAL: value == unknown ⇔ evidence ==
    # unverified. Forward: an `unknown` value can never be a confirmed/provisional fact.
    # Reverse: a concrete value (audio/cv/gate/clock/unipolar/bipolar/ac/dc/reverb/...) can
    # never carry `unverified` provenance — it is either a documented hardware fact
    # (confirmed) or a software-normalized mapping (provisional), never a guess. Both
    # directions are rejected below.
    JACK_FE_SIGNAL = ("signalType", "polarity", "coupling")
    SIG_TYPES = {"audio", "cv", "gate", "clock", "unknown"}
    POL_TYPES = {"unipolar", "bipolar", "unknown"}
    COUP_TYPES = {"ac", "dc", "unknown"}

    def unknown_evidence_consistent(value, st):
        """unknown ⇔ unverified; a concrete value requires confirmed/provisional."""
        if value == "unknown":
            return st == "unverified"
        return st != "unverified"

    for j in reg.jacks:
        fe = j.get("fieldEvidence")
        if not isinstance(fe, dict):
            problems.append(f"jack {j['stable_id']}: missing fieldEvidence audit object")
            continue
        for fk, allowed in ((JACK_FE_SIGNAL[0], SIG_TYPES),
                            (JACK_FE_SIGNAL[1], POL_TYPES),
                            (JACK_FE_SIGNAL[2], COUP_TYPES)):
            if j.get(fk) not in allowed:
                problems.append(f"jack {j['stable_id']}: {fk} {j.get(fk)!r} not in {sorted(allowed)}")
            st = fe.get(fk)
            if st not in FE_STATUS:
                problems.append(f"jack {j['stable_id']}: fieldEvidence.{fk} bad status {st!r}")
            elif not unknown_evidence_consistent(j.get(fk), st):
                problems.append(
                    f"jack {j['stable_id']}: {fk}={j.get(fk)!r} with fieldEvidence.{fk}={st!r} "
                    f"violates the unknown ⇔ unverified biconditional (an unknown value needs "
                    f"unverified provenance; a concrete value needs confirmed/provisional, "
                    f"never unverified)")
    SO_TYPES = {"unknown", "no", "yes"}
    for prog in reg.programs:
        pfe = prog.get("fieldEvidence")
        if not isinstance(pfe, dict):
            problems.append(f"program {prog['stable_id']}: missing fieldEvidence audit object")
            continue
        # both keys must be present, each with a legal status
        for pk in ("family", "selfOscillating"):
            pst = pfe.get(pk)
            if pst not in FE_STATUS:
                problems.append(f"program {prog['stable_id']}: fieldEvidence.{pk} missing or bad "
                                f"status {pst!r}")
        if prog.get("selfOscillating") not in SO_TYPES:
            problems.append(f"program {prog['stable_id']}: selfOscillating "
                            f"{prog.get('selfOscillating')!r} not in {sorted(SO_TYPES)}")
        for pk in ("family", "selfOscillating"):
            st = pfe.get(pk)
            if st in FE_STATUS and not unknown_evidence_consistent(prog.get(pk), st):
                problems.append(
                    f"program {prog['stable_id']}: {pk}={prog.get(pk)!r} with fieldEvidence.{pk}="
                    f"{st!r} violates the unknown ⇔ unverified biconditional (an unknown value "
                    f"needs unverified provenance; a concrete value needs confirmed/provisional, "
                    f"never unverified)")
    sz = {}
    for p in parameters:
        sz[p.get("shape", "scalar")] = sz.get(p.get("shape", "scalar"), 0) + 1
    coverage = {
        "registry": {
            "modules": len(reg.modules), "params": len(reg.parameters),
            "jacks": len(reg.jacks), "routes": len(reg.routes), "programs": len(reg.programs),
        },
        "manifest": {
            "modules": len(modules), "terminals": len(terminals), "programs": len(programs),
            "panelControls": len(panel_controls), "parameters": len(parameters),
            "actions": len(actions), "controlBindings": len(bindings),
            "controlRegions": len(control_regions), "recordSchemas": len(record_schemas),
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
                     "unboundPersist": unbound_persist, "actionBindings": len(binding_to_action),
                     "unboundActionEvents": unbound_action_events},
        "actions": {"target": len(action_sids), "transcribed": len(action_sids),
                    "kinds": act_kinds, "bound": sorted(binding_to_action),
                    "unbound": sorted(action_sids - set(binding_to_action))},
        "parameters": {"target": len(param_sids), "transcribed": len(param_sids),
                       "present": len(reg_param_sids), "missingModules": modules_missing_params,
                       "shapes": sz,
                       "gap": sorted(param_sids - reg_param_sids),
                       "rogue": sorted(reg_param_sids - param_sids),
                       "rogueMigrating": rogue_params_migrate,
                       "newRogue": new_param_rogue},
        "jacks": {"target": len(tgt_patchable), "present": len(reg_jack_sids),
                  "missingModules": [], "gap": sorted(tgt_patchable - reg_jack_sids),
                  "rogue": sorted(reg_jack_sids - tgt_patchable),
                  "rogueMigrating": rogue_jacks_migrate, "newRogue": new_jack_rogue,
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
        if allow_params or allow_jacks:
            not_full.append("migration allowlist must be empty under --require-full "
                            "(parameters=%s jacks=%s)" % (sorted(allow_params), sorted(allow_jacks)))
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
                 "%(panelControls)d panel-controls / %(parameters)d parameters / %(actions)d actions / "
                 "%(controlBindings)d bindings / %(controlRegions)d control-regions / "
                 "%(recordSchemas)d record-schemas / %(endpointsTranscribed)d endpoints / "
                 "%(normalizedRoutes)d normalized / %(fixedRoutes)d fixed / "
                 "%(requiredFixedRoutes)d required-fixed / %(mustComplete)d mustComplete" % m)
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
                 "unboundPersist=%(unboundPersist)s actionBindings=%(actionBindings)d "
                 "unboundActionEvents=%(unboundActionEvents)s" % bd)
    ac = coverage["actions"]
    lines.append("  actions          : %(transcribed)d kinds=%(kinds)s unbound=%(unbound)s" % ac)
    pr = coverage["parameters"]
    lines.append("  parameters       : target=%(target)d present=%(present)d "
                 "missingModules=%(missingModules)s shapes=%(shapes)s" % pr)
    lines.append("                    gap=%(gap)s rogue=%(rogue)s "
                 "rogueMigrating=%(rogueMigrating)s newRogue=%(newRogue)s" % pr)
    jk = coverage["jacks"]
    lines.append("  jacks            : target=%(target)d present=%(present)d "
                 "internal=%(internal)s newRogue=%(newRogue)s" % jk)
    fx = coverage["fixedRoutes"]
    lines.append("  fixed routes     : defined=%(defined)d required=%(required)s gap=%(gap)s  (%(note)s)"
                 % fx)
    lines.append("")
    lines.append("note: 39 program identities is an identity/existence list, NOT 39 implemented effects.")
    lines.append("note: rogue ids are ALWAYS-ON (must be on the migration allowlist); --require-full "
                 "requires the allowlist empty + no rogue. Internal endpoints & fixed routes are "
                 "coherence-validated (no registry id-space yet).")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--require-full", action="store_true",
                    help="fail unless every capability-declared target module/program/route is "
                         "present, per-ID Parameter/Jack target↔registry, every module is "
                         "transcribed, every required fixed route is defined, and the migration "
                         "allowlist is empty")
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
                 "event widgets action-bound; impl ⊆ target; per-capability present-but-empty; "
                 "no new rogue beyond the migration allowlist; mustComplete == landed set.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
