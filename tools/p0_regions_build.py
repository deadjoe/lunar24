#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build the Phase-A three-entity panel ledger into p0_inventory_manifest.json.

TRANSCRIPTION TOOL, NOT a registry-derived generator: every row below is a hand-audit
of the panel/manual (solar42N_manual_text.txt; the full-panel render at lines 61-116,
the effector cartridge catalogue p23-24 at lines 1205-1310, the prose control sections).
Codex 3rd-review msg 66a83ca0 demanded we stop conflating three entity kinds; this tool
splits them (msg 06ef6b70 items 1/2/4). Codex 5th-review msg c7089521 (still NOT GO)
adds the following:

  - target.parameters[] carry an EXPLICIT `shape` (scalar|vector|record|mask) plus the
    matching cardinality / recordType / maskSize, instead of mis-scalar continuous/state
    fields (joystick=2-axis, plate_tune=12, pushbutton_value=8, seq_editor=up-to-16-step
    record, quantise_scale_editor=mask, seq_rhythm_length present).
  - target.actions[] give the 27 event-only widgets (12 plates, 8 buttons, encoder, 6 drone
    buttons) a stable action identity (note-trigger / trigger / encoder-rotate|press|long-press),
    plus keyboard.calibration_init/save; controlBindings[].to may be a parameter OR an action
    (binding semantics: operation + index/axis/menu context, not a bare string).
  - target.recordSchemas[] declare the keyboard_seq and keyboard_preset payload shapes; the 4
    presets reference one shared keyboard_preset record and explicitly exclude clock_bpm.
  - target.migrationAllowlist[] grandfathers the PRE-CORRECTION registry rogue ids (dot-space
    vcf.l.freq, vco_a.fm_amt/oct_high/sub/wave ...) as a MONOTONICALLY-SHRINKING explicit list:
    the gate is ALWAYS-ON (no new rogue unless allowlisted) and --require-full needs it empty.
  - capabilities split `patchableJacks` vs `internalEndpoints` (mixer/voices have no normal
    patchable jack — they are internal-only), derived from the hand-transcribed endpoint inventory.

Every region's expectedPanelControlCount is a manual count of the widgets on the panel;
if the emitted widgets do not reach it the tool FAILS (so a careless count is caught here,
and the completion gate independently re-checks manifest.expected == manifest.actual).

The machine registry (lunar24.json) is NOT a source; the manifest stays an independent
target. Endpoints/modules/programs/normalizedRoutes/fixedRoutes are preserved; lfo cv_out
evidence is re-pointed to the manual (Codex item #4).

Usage:  python3 tools/p0_regions_build.py   (rewrites the manifest in place)
"""

import json
import os
import sys

REF = "solar42N_manual_v15"
MANIFEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "spec", "machine", "p0_inventory_manifest.json")
SPEC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "spec", "machine", "lunar24.json")

# Physical-widget kinds (panelControls) vs logical-value kinds (parameters).
K_CONT = "continuous"
K_SEL = "selector-toggle"
K_MOM = "momentary-touch"
K_RENC = "rotary-encoder"
K_STATE = "state-field"

PANEL_KINDS = {K_CONT, K_SEL, K_MOM, K_RENC}
PARAM_KINDS = {K_CONT, K_SEL, K_STATE}

# Explicit parameter shapes (Codex c7089521 item #1): a mis-scalar field is no longer allowed.
SH_SCALAR = "scalar"
SH_VECTOR = "vector"
SH_RECORD = "record"
SH_MASK = "mask"
SHAPES = {SH_SCALAR, SH_VECTOR, SH_RECORD, SH_MASK}

# Keyboard menus: closed set of editing surfaces the shared encoder drives (Codex item #4).
KB_MENUS = ("behaviour", "mode", "arp", "sequencer", "portamento", "vibrato",
            "pressure", "quantiser", "clock", "calibration", "button-editor", "presets")

# Action kinds (event-only widget behavioural identity, Codex item #3).
ACT_TRIGGER = "trigger"
ACT_ENC_ROTATE = "encoder-rotate"
ACT_ENC_PRESS = "encoder-press"
ACT_ENC_LONG = "encoder-long-press"
ACT_INIT = "init"
ACT_SAVE = "save"
ACT_LOAD = "load"
ACT_INITIALISE = "initialise"
ACTION_KINDS = {ACT_TRIGGER, ACT_ENC_ROTATE, ACT_ENC_PRESS, ACT_ENC_LONG,
                ACT_INIT, ACT_SAVE, ACT_LOAD, ACT_INITIALISE}

# Binding semantics split into SOURCE (the physical input gesture) and TARGET (what it does to
# the destination) — Codex 7th-review (msg 9d8b5f43) item #1. A single `operation` cannot mean both
# "the encoder rotated" and "write this parameter", so we carry the three separately.
SRC_CHANGE = "change"      # continuous knob / regulator drag
SRC_ROTATE = "rotate"      # encoder rotate event
SRC_PRESS = "press"        # momentary down / gate-on edge
SRC_RELEASE = "release"    # momentary up / gate-off edge
SRC_LONG = "long_press"    # encoder held press
SOURCE_OPS = {SRC_CHANGE, SRC_ROTATE, SRC_PRESS, SRC_RELEASE, SRC_LONG}

TK_PARAM = "parameter"
TK_ACTION = "action"
TARGET_KINDS = {TK_PARAM, TK_ACTION}

TO_SET = "set"             # write the value
TO_ADJUST = "adjust"       # increment/decrement a continuous regulator
TO_TOGGLE = "toggle"       # flip a discrete selector state
TO_INVOKE = "invoke"       # fire an event-only action
TARGET_OPS = {TO_SET, TO_ADJUST, TO_TOGGLE, TO_INVOKE}

# The physical source operations a widget KIND can produce. The gate enforces that every binding
# FROM a widget uses one of these AND that the set actually wired equals the declared set, so a
# newly-invented source edge (e.g. an encoder `release`) is rejected.
WIDGET_SRC = {
    K_CONT: {SRC_CHANGE},
    K_SEL: {SRC_CHANGE},
    K_MOM: {SRC_PRESS, SRC_RELEASE},
    K_RENC: {SRC_ROTATE, SRC_PRESS, SRC_LONG},
}

# Closed condition keys for compound workflows (msg 9d8b5f43 item #2). Each locates / guards a
# sub-action. The gate validates the value against the closed grammar per context.
COND_KEYS = {"heldControl", "menuItem", "presetSlot", "recordField", "recordIndex", "maskIndex"}
PRESET_SLOTS = ("preset_a", "preset_b", "preset_c", "preset_d")
# PRESETS two-level page state machine (Codex Phase-A 10th-review verdict msg e9f1f028). The presets
# user flow is genuinely TWO PAGES, not one flat menu: the manual (L1045-1046) says rotate to scroll
# presets A-D, then press to enter the selected slot's load/save/initialise sub-page. Each page is a
# REAL keyboard-menu context selector (page=slot-list | action-list), so the gate can tell a
# top-level slot list from an entered action sub-page instead of collapsing them into one
# `menu=presets`. The navigation actions are NOT display items, so no `menuItem` ever names a
# navigation action's own leaf — the topology is an independent declaration, never self-proven from a
# target's `menu` field.
PRESET_PAGE_SLOT_LIST = "slot-list"
PRESET_PAGE_ACTION_LIST = "action-list"
PRESET_PAGES = (PRESET_PAGE_SLOT_LIST, PRESET_PAGE_ACTION_LIST)
SEQ_STEP_FIELDS = ("note", "value", "gate")


def ev(line_start, line_end=None, site=None):
    e = {"ref": REF, "lineStart": line_start}
    if line_end is not None:
        e["lineEnd"] = line_end
    if site is not None:
        e["panelSite"] = site
    return e


def _count(leaf, n):
    if n == 1:
        return [leaf]
    return ["%s_%d" % (leaf, i) for i in range(1, n + 1)]


def ctx_global():
    return {"type": "global"}


def ctx_program(program_id):
    return {"type": "program", "program": program_id}


def ctx_menu(menu):
    return {"type": "keyboard-menu", "menu": menu}


def ctx_menu_page(page):
    """A keyboard-menu context with a real page selector (PRESETS only).

    The PRESETS workflow is a two-page state machine (slot-list -> action-list), so the context must
    carry the current page, not just the menu name. Without it the slot-list and action-list would be
    indistinguishable and the checker could not validate a page-scoped `menuItem` (Codex e9f1f028).
    """
    c = ctx_menu("presets")
    c["page"] = page
    return c


def ctx_mode(mode):
    return {"type": "keyboard-mode", "mode": mode}


class Ledger:
    """Collects panel controls, parameters, actions, bindings, and region subtotals.

    `region()` declares a region with an INDEPENDENT hand-counted widget count. The
    helper then expands a spec of widget entries; each entry may be:

      (leaf, kind, count, param_kind|None, semantic_label|None)

        leaf            widget leaf name (module-prefixed id is `<owner>.<leaf>`)
        kind            panelControl kind (continuous|selector-toggle|momentary-touch|rotary-encoder)
        count           number of identical widgets in this region
        param_kind      parameter kind for the persisted value; None means this widget is an
                        EVENT (momentary touch / trigger) with no persisted logical state.
                        For the common case the persisted parameter carries the SAME leaf id
                        as the widget, and a binding maps widget -> parameter (context global).
        semantic_label  optional p23/p24 text for program X/Y/Z knobs.

    Codex c7089521 extensions: `panel_text` is the VERBATIM printed label (kept separate from
    the internal `leaf`); selector-toggle widgets take `positions` (discrete position labels);
    the joystick takes `axes=[x,y]`; the rotary-encoder takes `operations`; a parameter takes
    `shape` plus cardinality / recordType / maskSize. `bind()` writes a structured context and
    binding semantics (operation + optional axis/index) instead of a bare string.
    """

    def __init__(self):
        self.panel = []
        self.params = []
        self.actions = []
        self.bindings = []
        self.regions = []

    def region(self, rid, owner, site, expected, evidence):
        """Open a region. `expected` is the INDEPENDENT manually-counted widget count."""
        self._cur = {"rid": rid, "owner": owner, "site": site,
                     "expected": expected, "evidence": evidence, "placed": 0}
        self.regions.append(self._cur)

    def _place(self, widget):
        self._cur["placed"] += 1
        self.panel.append(widget)

    def _mk_widget(self, wleaf, kind, panel_text, positions, axes, operations):
        owner = self._cur["owner"]
        wid = "%s.%s" % (owner, wleaf)
        w = {"stable_id": wid, "owner": owner, "kind": kind, "region": self._cur["rid"],
             "leaf": wleaf, "panelLabel": panel_text or wleaf,
             "status": "confirmed", "evidence": self._cur["evidence"]}
        if positions is not None:
            w["positions"] = positions
        if axes is not None:
            w["axes"] = axes
        if operations is not None:
            w["operations"] = operations
        return w

    def emit(self, leaf, kind, count=1, param_kind=None, label=None,
             param_owner=None, param_leaf=None, context=None,
             panel_text=None, positions=None, axes=None, operations=None,
             shape=None, cardinality=None, record_type=None, mask_size=None):
        """Expand `count` identical widgets; each gets a persisted param + binding unless
        `param_kind` is None (an event-only widget: momentary touch / trigger / encoder)."""
        owner = self._cur["owner"]
        evid = self._cur["evidence"]
        for i, wleaf in enumerate(_count(leaf, count)):
            wid = "%s.%s" % (owner, wleaf)
            self._place(self._mk_widget(wleaf, kind, panel_text, positions, axes, operations))
            if param_kind is None:
                continue
            po = param_owner or owner
            pleaf = param_leaf or wleaf
            psid = "%s.%s" % (po, pleaf)
            self.add_param(psid, po, param_kind, self._cur["rid"], evid, semantic=label,
                           shape=shape, cardinality=cardinality, record_type=record_type,
                           mask_size=mask_size, positions=positions)
            # A continuous regulator/nudge: source edge = change, targets a parameter, effect =
            # adjust (continuous) or a plain write (discrete selector). Action effects use `invoke`.
            tgt_op = TO_ADJUST if kind == K_CONT else TO_SET
            self.bind(wid, psid, context or ctx_global(), source=SRC_CHANGE, kind=TK_PARAM,
                      target_op=tgt_op, evidence=evid)

    def add_param(self, sid, owner, kind, region, evidence, semantic=None,
                  shape=None, cardinality=None, record_type=None, mask_size=None,
                  action_managed=False, status="confirmed", menu=None, mode=None, positions=None):
        p = {"stable_id": sid, "owner": owner, "kind": kind, "region": region,
             "shape": shape or SH_SCALAR, "status": status, "evidence": evidence}
        if semantic:
            p["semanticLabel"] = semantic
        if cardinality is not None:
            p["cardinality"] = cardinality
        if record_type is not None:
            p["recordType"] = record_type
        if mask_size is not None:
            p["maskSize"] = mask_size
        if action_managed:
            p["actionManaged"] = True
        # A selector-toggle parameter's discrete value domain is a reproducible, complete fact: the
        # builder projects it here (Codex 644ea86e) so `positions` survives a rebuild instead of
        # being a hand-entered manifest edit. Physical selectors are projected from the same-stable-id
        # panelControl widget's positions (forwarded by emit()); keyboard menu selectors carry the
        # closed set from the manual (KPOS). A selector-toggle with NO positions has no evidenced
        # value domain and must stay a GAP.
        if positions is not None:
            p["positions"] = positions
        # A keyboard-state param declares which menu/mode it is edited under (Codex 9th-review
        # item #1), so the gate can validate that a `menuItem` condition naming it belongs to the
        # binding's own menu/mode rather than being an arbitrary string. Vector params (plate_tune /
        # pushbutton_value) are edited via held plate/button + encoder under the GLOBAL context, not a
        # navigable menu, so they carry neither.
        if menu is not None:
            p["menu"] = menu
        if mode is not None:
            p["mode"] = mode
        self.params.append(p)
        return sid

    def bind(self, from_pc, to_sid, context, source=None, kind=None, target_op=None,
             axis=None, index=None, condition=None, command_address=None, status="confirmed",
             evidence=None):
        # Binding semantics are a THREE-WAY split (msg 9d8b5f43 item #1): the physical source edge
        # (`source`), what it targets (`kind` = parameter|action), and the effect on that target
        # (`target_op` = set|adjust|toggle|invoke). A single `operation` can no longer conflate "the
        # encoder rotated" with "write this parameter".
        #
        # unique id: (from, to, index, condition, context, source/target op, commandAddress). The same
        # encoder drives a shared vector at N held indices (index), the same sub-page action is
        # selected by N slots (condition), and the SAME from/to can appear under different legal
        # contexts or be reached by different gestures — so the id must carry all of them or two
        # distinct bindings collide (Codex 23ea438f #4). Stable-id therefore includes the structured
        # context, the source/target triple and the commandAddress, not just from/to/index/condition.
        key = to_sid
        if axis is not None:
            key += "[axis=%s]" % axis
        if index is not None:
            key += "[%s]" % index
        if condition:
            key += "{%s}" % ",".join("%s=%s" % (k, condition[k]) for k in sorted(condition))
        if context and context.get("type"):
            sel = ";".join("%s=%s" % (k, context[k]) for k in sorted(context) if k != "type")
            key += "(%s%s)" % (context["type"], (":" + sel if sel else ""))
        # The id must be faithful to the FULL semantic tuple (Codex 9th-review item #2): targetKind
        # and axis were missing, so two bindings differing only in target-kind (parameter vs action)
        # or axis collided. Now the id carries source:targetKind:targetOp and the axis selector, so it
        # can never collide with a tuple the checker would treat as distinct.
        key += "#%s:%s:%s" % ((source or SRC_CHANGE), (kind or TK_PARAM), (target_op or TO_SET))
        if command_address is not None:
            key += "@" + json.dumps(command_address, sort_keys=True, ensure_ascii=False)
        b = {"stable_id": "bnd.%s->%s" % (from_pc, key), "from": from_pc, "to": to_sid,
             "sourceOperation": source or SRC_CHANGE, "targetKind": kind or TK_PARAM,
             "targetOperation": target_op or TO_SET, "context": context, "status": status,
             "evidence": evidence or self._cur.get("evidence")}
        if axis is not None:
            b["axis"] = axis
        if index is not None:
            b["index"] = index
        if command_address is not None:
            b["commandAddress"] = command_address
        if condition:
            b["condition"] = condition
        self.bindings.append(b)
        return b

    def add_action(self, sid, owner, name, kind, region, evidence, status="confirmed",
                   menu=None, mode=None):
        a = {"stable_id": sid, "owner": owner, "name": name, "kind": kind,
             "region": region, "status": status, "evidence": evidence}
        # A keyboard-menu/mode action declares which menu/mode it ships in (Codex 9th-review #1) so a
        # `menuItem` condition is validated against a real item set, not just any string.
        if menu is not None:
            a["menu"] = menu
        if mode is not None:
            a["mode"] = mode
        self.actions.append(a)
        return sid

    def close(self):
        cur = self._cur
        declared = cur["placed"]
        if declared != cur["expected"]:
            raise SystemExit(
                "region %s: emitted %d panel widgets but expectedPanelControlCount=%d "
                "(recount the panel)" % (cur["rid"], declared, cur["expected"]))
        self.regions[-1] = {"id": cur["rid"], "module": cur["owner"], "panelSite": cur["site"],
                            "expectedPanelControlCount": cur["expected"],
                            "status": "confirmed", "evidence": cur["evidence"]}
        self._cur = None


# Verbatim printed panel labels (best-effort from the manual render/prose). Where the exact text
# is not evidenced the generator falls back to the upper-cased leaf; positions below are the
# discrete selector states (Codex item #4 requires them in the schema rather than an opaque label).
P = {
    # VCO
    "cv_amt": "CV AMT", "morph": "MORPHING", "tune": "TUNE", "pwm": "PW AMT",
    "pw": "PW", "lin_exp": "LIN / EXP", "oct_sel": "OCT +3 / LOW", "sub_sel": "SUB -1",
    # drone classic
    "tune": "TUNE", "mute": "MUTE", "mod": "MOD", "volt": "VOLT",
    "att": "ATT", "rls": "RLS", "gate_hold": "GATE-HOLD",
    # drone new
    "rate": "RATE", "divider": "DIVIDER", "pitch": "PITCH", "noise": "NOISE",
    "hi_low": "RANGE", "fm": "FM", "am": "AM", "rate_switch": "RATE SWT", "hold": "HOLD",
    # env
    "hold": "HOLD", "self_gen": "SELF-GEN", "a": "A", "d": "D", "s": "S", "r": "R",
    # vcf
    "l_freq": "FREQ L", "l_res": "RES L", "l_mod": "MOD L", "l_bp_lp": "BP/LP L",
    "r_freq": "FREQ R", "r_res": "RES R", "r_mod": "MOD R", "r_bp_lp": "BP/LP R",
    "dist": "DIST", "gain": "GAIN", "link": "LINK",
    # effector
    "x": "X", "y": "Y", "z": "Z", "blend": "BLEND", "master": "MASTER",
    "phone": "PHONE", "select_l": "SELECT L", "select_r": "SELECT R",
    # lfo
    "wave": "WAVE", "speed_mult": "SPEED MULT",
    # seq
    "pulser": "PULSER", "clock": "CLOCK", "stages": "STAGES",
    "step_cv": "STEP CV", "step_gate": "STEP GATE",
    # joystick
    "joy": "JOYSTICK", "offset_x": "X OFFSET", "offset_y": "Y OFFSET",
    # preamp / env follower
    "gain": "GAIN", "attack": "ATTACK", "release": "RELEASE",
    # mixer
    "pan": "PAN", "vol": "VOL",
}

# Discrete selector position labels (closed set per selector, Codex item #4). Position labels
# are given for every selector-toggle so the gate never sees an opaque one-state switch.
POS = {
    "lin_exp": ["lin", "exp"],
    "oct_sel": ["low", "0", "+3"],
    "sub_sel": ["0", "-1"],
    "mute": ["off", "on"],
    "mod": ["off", "on"],
    "gate_hold": ["off", "on"],
    "hi_low": ["hi", "low"],
    "fm": ["off", "on"],
    "am": ["off", "on"],
    "rate_switch": ["off", "on"],
    "hold": ["off", "on"],
    "self_gen": ["off", "on"],
    "l_bp_lp": ["bp", "lp"],
    "r_bp_lp": ["bp", "lp"],
    "link": ["off", "on"],
    "select_l": ["1", "2", "3"],
    "select_r": ["1", "2", "3"],
    "speed_mult": ["x1", "x6", "x10"],
    "clock": ["int", "ext"],
    "stages": ["1", "2", "3", "4", "5"],
    "step_gate": ["off", "on"],
}

# Keyboard-state selector positions (Codex 644ea86e Root 1). These ARE NOT physical panel widgets —
# they are software keyboard settings edited via the shared encoder under a menu/mode — so their
# discrete value domain is transcribed here from the manual, not projected from a panelControl.
# A keyboard selector-toggle param with a manual-enumerated domain belongs here; the 4 clock/rhythm
# toggles (arp_clock/arp_rhythm/seq_clock/seq_rhythm) and the quantise scale MASK editor are NOT
# (no enumerated manual set) and intentionally stay without positions + GAP.
KPOS = {
    "behaviour": ["Single", "Twin", "Split"],
    "mode": ["keyboard", "arpeggiator", "sequencer"],
    "arp_hold": ["off", "on"],
    "arp_direction": ["forward", "backward", "ping-pong", "random"],
    "arp_variation": ["off", "x1", "x2", "x3"],
    "seq_run": ["free", "keyboard"],
    "seq_direction": ["forward", "backward", "ping-pong", "random"],
    "seq_cv_output": ["continuous", "gated"],
    "portamento_legato": ["off", "on"],
    "pressure_output": ["pressure", "asr", "ad", "loop", "random"],
    "quantise_load_scale": ["semitones", "ionian", "dorian", "phrygian", "lydian", "mixolydian",
                            "aeolian", "locrian", "blues-major", "blues-minor",
                            "pentatonic-major", "pentatonic-minor", "folk", "japanese", "gamelan",
                            "gypsy", "arabian", "flamenco", "whole-tone"],
    "dac_vref": ["internal", "ex"],
    "encoder_direction": ["normal", "reversed"],
}


def build():
    L = Ledger()

    # CLASSIC drones 1/2/4/5 : 5 generators x (TUNE/MUTE/MOD) + VOLT/ATT/RLS/GATE-HOLD = 19.
    for mod, site in [("drone_1", "左上 DRONE 1/2"), ("drone_2", "左上 DRONE 1/2"),
                      ("drone_4", "右上 DRONE 4/5"), ("drone_5", "右上 DRONE 4/5")]:
        L.region("drone_classic_" + mod, mod, site, 19, ev(294, 312, site))
        L.emit("tune", K_CONT, 5, K_CONT, panel_text=P["tune"])
        L.emit("mute", K_SEL, 5, K_SEL, panel_text=P["mute"], positions=POS["mute"])
        L.emit("mod", K_SEL, 5, K_SEL, panel_text=P["mod"], positions=POS["mod"])
        L.emit("volt", K_CONT, 1, K_CONT, panel_text=P["volt"])
        L.emit("att", K_CONT, 1, K_CONT, panel_text=P["att"])
        L.emit("rls", K_CONT, 1, K_CONT, panel_text=P["rls"])
        L.emit("gate_hold", K_SEL, 1, K_SEL, panel_text=P["gate_hold"], positions=POS["gate_hold"])
        L.close()

    # NEW drones 3/6 (Papa Srapa) : RATE/MOD/DIVIDER/PITCH/NOISE + the AR ATT/RLS + switches
    # HI-LOW/FM/AM/RATE + HOLD = 12. (S&H is a display/circuit region, NOT a control — Codex.)
    for mod, site in [("drone_3", "左中 DRONE 3"), ("drone_6", "右中 DRONE 6")]:
        L.region("drone_new_" + mod, mod, site, 12, ev(322, 370, site))
        L.emit("rate", K_CONT, 1, K_CONT, panel_text=P["rate"])
        L.emit("mod", K_CONT, 1, K_CONT, panel_text=P["mod"])
        L.emit("divider", K_CONT, 1, K_CONT, panel_text=P["divider"])
        L.emit("pitch", K_CONT, 1, K_CONT, panel_text=P["pitch"])
        L.emit("noise", K_CONT, 1, K_CONT, panel_text=P["noise"])
        L.emit("att", K_CONT, 1, K_CONT, panel_text=P["att"])
        L.emit("rls", K_CONT, 1, K_CONT, panel_text=P["rls"])
        L.emit("hi_low", K_SEL, 1, K_SEL, panel_text=P["hi_low"], positions=POS["hi_low"])
        L.emit("fm", K_SEL, 1, K_SEL, panel_text=P["fm"], positions=POS["fm"])
        L.emit("am", K_SEL, 1, K_SEL, panel_text=P["am"], positions=POS["am"])
        L.emit("rate_switch", K_SEL, 1, K_SEL, panel_text=P["rate_switch"], positions=POS["rate_switch"])
        L.emit("hold", K_SEL, 1, K_SEL, panel_text=P["hold"], positions=POS["hold"])
        L.close()

    # VCO A / VCO B : 8 physical controls each (Codex). No duplicate wave/shape/CV-FM, and the
    # octave +3/low is ONE 3-position selector, not two.
    for mod, site in [("vco_a", "中上 VCO A"), ("vco_b", "中上 VCO B")]:
        L.region(mod, mod, site, 8, ev(376, 422, site))
        L.emit("cv_amt", K_CONT, 1, K_CONT, panel_text=P["cv_amt"])
        L.emit("morph", K_CONT, 1, K_CONT, panel_text=P["morph"])      # morphing waveform
        L.emit("tune", K_CONT, 1, K_CONT, panel_text=P["tune"])
        L.emit("pwm", K_CONT, 1, K_CONT, panel_text=P["pwm"])
        L.emit("pw", K_CONT, 1, K_CONT, panel_text=P["pw"])            # pulse width / SHAPE
        L.emit("lin_exp", K_SEL, 1, K_SEL, panel_text=P["lin_exp"], positions=POS["lin_exp"])
        L.emit("oct_sel", K_SEL, 1, K_SEL, panel_text=P["oct_sel"], positions=POS["oct_sel"])
        L.emit("sub_sel", K_SEL, 1, K_SEL, panel_text=P["sub_sel"], positions=POS["sub_sel"])
        L.close()

    # ENVELOPE A / B : HOLD + SELF-GENERATION + ADSR = 6 each (Codex adds self-generation).
    for mod, site in [("envelope_a", "中部 ENV A"), ("envelope_b", "中部 ENV B")]:
        L.region(mod, mod, site, 6, ev(385, 408, site))
        L.emit("hold", K_SEL, 1, K_SEL, panel_text=P["hold"], positions=POS["hold"])
        L.emit("self_gen", K_SEL, 1, K_SEL, panel_text=P["self_gen"], positions=POS["self_gen"])
        L.emit("a", K_CONT, 1, K_CONT, panel_text=P["a"])
        L.emit("d", K_CONT, 1, K_CONT, panel_text=P["d"])
        L.emit("s", K_CONT, 1, K_CONT, panel_text=P["s"])
        L.emit("r", K_CONT, 1, K_CONT, panel_text=P["r"])
        L.close()

    # VOICE MIXER : 10 channels x (PAN/VOL) = 20.
    L.region("mixer", "mixer", "中部 VOICE MIXER", 20, ev(1105, 1113, "中部 VOICE MIXER"))
    for i in range(1, 11):
        L.emit("ch%d_pan" % i, K_CONT, 1, K_CONT, panel_text=P["pan"])
        L.emit("ch%d_vol" % i, K_CONT, 1, K_CONT, panel_text=P["vol"])
    L.close()

    # DUAL VCF : per-side FREQ/RES/MOD + BP-LP; shared DIST/GAIN/LINK.
    L.region("dual_vcf", "vcf", "中上 DUAL VCF", 11, ev(1118, 1153, "中上 DUAL VCF"))
    L.emit("l_freq", K_CONT, 1, K_CONT, panel_text=P["l_freq"])
    L.emit("l_res", K_CONT, 1, K_CONT, panel_text=P["l_res"])
    L.emit("l_mod", K_CONT, 1, K_CONT, panel_text=P["l_mod"])
    L.emit("l_bp_lp", K_SEL, 1, K_SEL, panel_text=P["l_bp_lp"], positions=POS["l_bp_lp"])
    L.emit("r_freq", K_CONT, 1, K_CONT, panel_text=P["r_freq"])
    L.emit("r_res", K_CONT, 1, K_CONT, panel_text=P["r_res"])
    L.emit("r_mod", K_CONT, 1, K_CONT, panel_text=P["r_mod"])
    L.emit("r_bp_lp", K_SEL, 1, K_SEL, panel_text=P["r_bp_lp"], positions=POS["r_bp_lp"])
    L.emit("dist", K_CONT, 1, K_CONT, panel_text=P["dist"])
    L.emit("gain", K_CONT, 1, K_CONT, panel_text=P["gain"])
    L.emit("link", K_SEL, 1, K_SEL, panel_text=P["link"], positions=POS["link"])
    L.close()

    # DUAL EFFECTOR : shared X/Y/Z + BLEND + MASTER + PHONE + two 1-2-3 selectors = 8.
    # (No duplicate cartridge-slot control; PHONE volume added — Codex.)
    L.region("dual_effector", "effector", "中上 DUAL EFFECTOR", 8, ev(1159, 1199, "中上 DUAL EFFECTOR"))
    L.emit("x", K_CONT, 1, K_CONT, panel_text=P["x"])
    L.emit("y", K_CONT, 1, K_CONT, panel_text=P["y"])
    L.emit("z", K_CONT, 1, K_CONT, panel_text=P["z"])
    L.emit("blend", K_CONT, 1, K_CONT, panel_text=P["blend"])
    L.emit("master", K_CONT, 1, K_CONT, panel_text=P["master"])
    L.emit("phone", K_CONT, 1, K_CONT, panel_text=P["phone"])
    L.emit("select_l", K_SEL, 1, K_SEL, panel_text=P["select_l"], positions=POS["select_l"])
    L.emit("select_r", K_SEL, 1, K_SEL, panel_text=P["select_r"], positions=POS["select_r"])
    L.close()

    # LFO A / LFO B : wave + rate + x1/x6/x10.
    for mod, site in [("lfo_a", "下排 LFO A"), ("lfo_b", "下排 LFO B")]:
        L.region(mod, mod, site, 3, ev(425, 442, site))
        L.emit("wave", K_CONT, 1, K_CONT, panel_text=P["wave"])
        L.emit("rate", K_CONT, 1, K_CONT, panel_text=P["rate"])
        L.emit("speed_mult", K_SEL, 1, K_SEL, panel_text=P["speed_mult"], positions=POS["speed_mult"])
        L.close()

    # 5 STEP SEQ : pulser is a CONTINUOUS fine-tune knob (Codex); clock/stages selectors;
    # 5 step-CV + 5 step-gate.
    L.region("seq", "sequencer", "下排 5-step seq", 13, ev(766, 775, "下排 5-step seq"))
    L.emit("pulser", K_CONT, 1, K_CONT, panel_text=P["pulser"])
    L.emit("clock", K_SEL, 1, K_SEL, panel_text=P["clock"], positions=POS["clock"])
    L.emit("stages", K_SEL, 1, K_SEL, panel_text=P["stages"], positions=POS["stages"])
    L.emit("step_cv", K_CONT, 5, K_CONT, panel_text=P["step_cv"])
    L.emit("step_gate", K_SEL, 5, K_SEL, panel_text=P["step_gate"], positions=POS["step_gate"])
    L.close()

    # JOYSTICK : one 2-axis stick + two offset regulators = 3 physical widgets. The stick edits
    # TWO persisted axes (X and Y) with a separate binding each (Codex item #1/#4: not one scalar).
    L.region("joystick", "joystick", "下排 joystick", 3, ev(446, 470, "下排 joystick"))
    L.emit("joy", K_CONT, 1, None, panel_text=P["joy"], axes=["x", "y"])
    L.add_param("joystick.x", "joystick", K_CONT, "joystick", ev(446, 470, "下排 joystick"),
                semantic="joystick X axis CV")
    L.add_param("joystick.y", "joystick", K_CONT, "joystick", ev(446, 470, "下排 joystick"),
                semantic="joystick Y axis CV")
    L.bind("joystick.joy", "joystick.x", ctx_global(), source=SRC_CHANGE, kind=TK_PARAM,
           target_op=TO_SET, axis="x")
    L.bind("joystick.joy", "joystick.y", ctx_global(), source=SRC_CHANGE, kind=TK_PARAM,
           target_op=TO_SET, axis="y")
    L.emit("offset_x", K_CONT, 1, K_CONT, panel_text=P["offset_x"])
    L.emit("offset_y", K_CONT, 1, K_CONT, panel_text=P["offset_y"])
    L.close()

    # PREAMP : GAIN only (EXT SOURCE is a jack; built-in mic auto-bypassed — Codex item #2).
    L.region("preamp", "preamp", "下排 preamp", 1, ev(516, 555, "下排 preamp"))
    L.emit("gain", K_CONT, 1, K_CONT, panel_text=P["gain"])
    L.close()

    # ENVELOPE FOLLOWER : attack + release.
    L.region("env_follower", "env_follower", "下排 env follower", 2, ev(551, 553, "下排 env follower"))
    L.emit("attack", K_CONT, 1, K_CONT, panel_text=P["attack"])
    L.emit("release", K_CONT, 1, K_CONT, panel_text=P["release"])
    L.close()

    # KEYBOARD physical: 12 touchplates + 8 pushbuttons + a push/turn encoder = 21 widgets.
    # Plates/buttons are MOMENTARY (touch/press event) and the encoder is ROTARY — none stores a
    # value of its own. Each gets an ACTION identity (Codex item #3) and a binding to that action;
    # the persisted keyboard STATE lives in `keyboard_state` and is edited via the encoder.
    kb_evid = ev(559, 654, "底部 12 触摸片")
    L.region("keyboard_phys", "keyboard", "底部 12 触摸片", 21, kb_evid)
    for i in range(1, 13):
        w = "keyboard.plate_%d" % i
        L._place(L._mk_widget("plate_%d" % i, K_MOM, "NOTE PLATE %d" % i, None, None, None))
        # A touchplate is a MOMENTARY contact: BOTH a press (down / gate-on) and a release
        # (up / gate-off) edge — the release threshold is a sensor judgement, not this event
        # (Codex a23a9618 confirms option A). Core contract distinguishes gate_on/off.
        ap = L.add_action("%s.press" % w, "keyboard", "note plate %d press" % i, ACT_TRIGGER,
                          "keyboard_phys", kb_evid)
        L.bind(w, ap, ctx_global(), source=SRC_PRESS, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=kb_evid)
        ar = L.add_action("%s.release" % w, "keyboard", "note plate %d release" % i, ACT_TRIGGER,
                          "keyboard_phys", kb_evid)
        L.bind(w, ar, ctx_global(), source=SRC_RELEASE, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=kb_evid)
    for i in range(1, 9):
        w = "keyboard.pushbutton_%d" % i
        L._place(L._mk_widget("pushbutton_%d" % i, K_MOM, "FUNC BUTTON %d" % i, None, None, None))
        ap = L.add_action("%s.press" % w, "keyboard", "function button %d press" % i, ACT_TRIGGER,
                          "keyboard_phys", kb_evid)
        L.bind(w, ap, ctx_global(), source=SRC_PRESS, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=kb_evid)
        ar = L.add_action("%s.release" % w, "keyboard", "function button %d release" % i, ACT_TRIGGER,
                          "keyboard_phys", kb_evid)
        L.bind(w, ar, ctx_global(), source=SRC_RELEASE, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=kb_evid)
    L._place(L._mk_widget("encoder", K_RENC, "ENCODER", None, None,
                          [{"name": "rotate"}, {"name": "press"}, {"name": "long_press"}]))
    for nm, kind in [("rotate", ACT_ENC_ROTATE), ("press", ACT_ENC_PRESS),
                     ("long_press", ACT_ENC_LONG)]:
        a = L.add_action("keyboard.encoder.%s" % nm, "keyboard", "encoder %s" % nm, kind,
                         "keyboard_phys", kb_evid)
        L.bind("keyboard.encoder", a, ctx_global(), source=nm, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=kb_evid)
    # Held-index encoders: HOLD plate i then turn the encoder -> set element i of plate_tune, and
    # HOLD button j then turn the encoder -> set element j of pushbutton_value. Each carries an
    # explicit `index` plus a closed `heldControl` condition, and a semantic (source,target) triple:
    # rotate -> parameter set on the held element (Codex 9d8b5f43 item #1/#2 — the gesture writes a
    # parameter, not the encoder's own rotate event, and the held plate/button is the condition).
    for i in range(1, 13):
        L.bind("keyboard.encoder", "keyboard.plate_tune", ctx_global(), source=SRC_ROTATE,
               kind=TK_PARAM, target_op=TO_SET, index=i,
               condition={"heldControl": "keyboard.plate_%d" % i}, evidence=kb_evid)
    for j in range(1, 9):
        L.bind("keyboard.encoder", "keyboard.pushbutton_value", ctx_global(), source=SRC_ROTATE,
               kind=TK_PARAM, target_op=TO_SET, index=j,
               condition={"heldControl": "keyboard.pushbutton_%d" % j}, evidence=kb_evid)
    L.close()

    # DRONE VOICES 1-6 pushbutton triggers (module 'voices') : 6 momentary, NO persisted state,
    # each with a trigger action (Codex item #3).
    vc_evid = ev(778, 788, "右下 DRONE VOICES")
    L.region("drone_voices", "voices", "右下 DRONE VOICES", 6, vc_evid)
    for i in range(1, 7):
        w = "voices.button_%d" % i
        L._place(L._mk_widget("button_%d" % i, K_MOM, "DRONE %d" % i, None, None, None))
        ap = L.add_action("%s.press" % w, "voices", "drone voice %d trigger" % i, ACT_TRIGGER,
                          "drone_voices", vc_evid)
        L.bind(w, ap, ctx_global(), source=SRC_PRESS, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=vc_evid)
        ar = L.add_action("%s.release" % w, "voices", "drone voice %d release" % i, ACT_TRIGGER,
                          "drone_voices", vc_evid)
        L.bind(w, ar, ctx_global(), source=SRC_RELEASE, kind=TK_ACTION, target_op=TO_INVOKE,
               evidence=vc_evid)
    L.close()

    return L


# Keyboard menu / state parameters. These are NOT panel controls — they are LOGICAL state
# edited via the shared encoder / pushbuttons (Codex: three-entity split). Codex c7089521
# item #1/#2: every row now declares an EXPLICIT shape; seq_editor is a seq record (the menu is
# not a parameter), plate_tune/pushbutton_value are vectors, quantise_scale_editor is a mask,
# the 4 presets are records referencing the shared keyboard_preset payload, and seq_rhythm_length +
# the full calibration set (DAC vref, touch/release threshold, pressure min/max, MPR121, debounce)
# are now transcribed. (owner, leaf, kind, shape, cardinality/recordType/maskSize, menu, semantic,
#  lineStart, lineEnd)
SCALAR = (SH_SCALAR, {})


def _vec(n):
    return (SH_VECTOR, {"cardinality": n})


def _rec(t):
    return (SH_RECORD, {"recordType": t})


def _mask(n):
    return (SH_MASK, {"maskSize": n})


KEYBOARD_STATE_ROWS = [
    # (leaf, kind, shape(shape,extras), menu, semantic, lineStart, lineEnd)
    ("behaviour", K_SEL, SCALAR, "behaviour", "Single/Twin/Split", 659, 733),
    ("mode", K_SEL, SCALAR, "mode", None, 809, 818),
    ("arp_hold", K_SEL, SCALAR, "arp", None, 822, 857),
    ("arp_clock", K_SEL, SCALAR, "arp", None, 822, 857),
    ("arp_direction", K_SEL, SCALAR, "arp", None, 822, 857),
    ("arp_variation", K_SEL, SCALAR, "arp", None, 822, 857),
    ("arp_interval", K_CONT, SCALAR, "arp", None, 822, 857),
    ("arp_rhythm", K_SEL, SCALAR, "arp", None, 822, 857),
    ("arp_length", K_CONT, SCALAR, "arp", None, 822, 857),
    ("seq_run", K_SEL, SCALAR, "sequencer", None, 866, 910),
    ("seq_length", K_CONT, SCALAR, "sequencer", None, 866, 910),
    ("seq_clock", K_SEL, SCALAR, "sequencer", None, 866, 910),
    ("seq_direction", K_SEL, SCALAR, "sequencer", None, 866, 910),
    ("seq_cv_output", K_SEL, SCALAR, "sequencer", None, 866, 910),
    ("seq_rhythm", K_SEL, SCALAR, "sequencer", None, 866, 910),
    ("seq_rhythm_length", K_CONT, SCALAR, "sequencer", None, 866, 910),
    ("seq_steps", K_STATE, _rec("keyboard_seq"), "sequencer", "up to 16 step note/value + gate", 866, 910),
    ("portamento_speed", K_CONT, SCALAR, "portamento", None, 916, 923),
    ("portamento_legato", K_SEL, SCALAR, "portamento", None, 916, 923),
    ("vibrato_speed", K_CONT, SCALAR, "vibrato", None, 934, 946),
    ("vibrato_depth", K_CONT, SCALAR, "vibrato", None, 934, 946),
    ("vibrato_delay", K_CONT, SCALAR, "vibrato", None, 934, 946),
    ("vibrato_pressure", K_CONT, SCALAR, "vibrato", None, 934, 946),
    ("pressure_output", K_SEL, SCALAR, "pressure", None, 951, 965),
    ("pressure_rise", K_CONT, SCALAR, "pressure", None, 951, 965),
    ("pressure_fall", K_CONT, SCALAR, "pressure", None, 951, 965),
    ("quantise_scale_editor", K_SEL, _mask(12), "quantiser", "scale mask", 970, 984),
    ("quantise_load_scale", K_SEL, SCALAR, "quantiser", None, 970, 984),
    ("root_note", K_CONT, SCALAR, "mode", None, 992, 993),
    ("clock_bpm", K_CONT, SCALAR, "clock", None, 1028, 1029),
    ("calibration_v_oct", K_CONT, SCALAR, "calibration", "V/oct calibration", 1050, 1093),
    ("calibration_pressure", K_CONT, SCALAR, "calibration", "pressure calibration", 1050, 1093),
    ("dac_vref", K_SEL, SCALAR, "calibration", "DAC voltage reference source", 1050, 1093),
    ("touch_threshold", K_CONT, SCALAR, "calibration", "touch threshold (650)", 1050, 1093),
    ("release_threshold", K_CONT, SCALAR, "calibration", "release threshold (690)", 1050, 1093),
    ("pressure_min", K_CONT, SCALAR, "calibration", "pressure minimum", 1050, 1093),
    ("pressure_max", K_CONT, SCALAR, "calibration", "pressure maximum", 1050, 1093),
    ("mpr121_charge", K_CONT, SCALAR, "calibration", "MPR121 charge", 1050, 1093),
    ("mpr121_discharge", K_CONT, SCALAR, "calibration", "MPR121 discharge", 1050, 1093),
    ("debounce", K_CONT, SCALAR, "calibration", "debounce", 1050, 1093),
    ("encoder_direction", K_SEL, SCALAR, "calibration", "normal/reversed", 1050, 1093),
    ("plate_tune", K_CONT, _vec(12), "button-editor", "plate tuning", 620, 660),
    ("pushbutton_value", K_CONT, _vec(8), "button-editor", "button offset", 601, 613),
    ("preset_a", K_STATE, _rec("keyboard_preset"), "presets", "preset A payload", 1040, 1045),
    ("preset_b", K_STATE, _rec("keyboard_preset"), "presets", "preset B payload", 1040, 1045),
    ("preset_c", K_STATE, _rec("keyboard_preset"), "presets", "preset C payload", 1040, 1045),
    ("preset_d", K_STATE, _rec("keyboard_preset"), "presets", "preset D payload", 1040, 1045),
]

# Keyboard calibration menu ACCESS actions (Codex item #2/#3): init + save are menu-invoked
# actions, not persisted state. They exist so the gate can validate a stable event target even
# though no single panel widget maps to them 1:1 (unbound is allowed for actions).
KB_CAL_ACTIONS = [
    ("calibration_init", "INIT CALIBRATION VALUES (press+hold 1s)", ACT_INIT, 1050, 1093),
    ("calibration_save", "SAVE CALIBRATION SETTINGS (press+hold 1s)", ACT_SAVE, 1050, 1093),
]

# Keyboard PRESETS sub-page actions (manual L1045-1046). The manual ONLY confirms two preconditions:
# "rotate the encoder to scroll through presets A to D" (slot SELECTION) and "click the encoder to
# enter a sub-selection page where you can load, save or initialise a preset" (enter SUB-PAGE). It
# does NOT document the sub-page ITEM-selection or ITEM-execution gestures, so those are PROVISIONAL
# (Codex 23ea438f #1). We therefore build the workflow as: [rotate=select slot (confirmed) -> press
# =enter sub-page (confirmed) -> item selection + invoke selected item (provisional)]. Each
# load/save/initialise acts on a SELECTED slot, so it is bound once per slot under a closed
# `presetSlot` condition; the sub-page execution edge is a SINGLE uniform provisional press — and
# crucially NOT rotate, which the manual reserves for slot selection.
KB_PRESET_ACTIONS = [
    ("presets_load", "LOAD PRESET", ACT_LOAD),
    ("presets_save", "SAVE PRESET", ACT_SAVE),
    ("presets_initialise", "INITIALISE PRESET", ACT_INITIALISE),
]

# Presets two-page workflow NAVIGATION actions (Codex Phase-A 10th-review verdict msg e9f1f028).
# These drive the A-D slot list -> load/save/initialise action list state machine but are NOT
# manual-displayed items, so they can never be a `menuItem` value (only a real display item can be).
# Rotate=select-slot and press=enter-sub-page are the two manual edges (confirmed); sub-page
# item-selection is undocumented, so it stays provisional (Codex 23ea438f #1). Each is a first-class
# transient ACTION so the dispatch edge is a declared entity, not prose.
KB_PRESET_NAV = [
    ("preset_select_slot", "rotate to select preset slot A-D", ACT_ENC_ROTATE, SRC_ROTATE,
     "confirmed"),
    ("preset_enter_subpage", "press to enter the selected slot's load/save/init sub-page",
     ACT_ENC_PRESS, SRC_PRESS, "confirmed"),
    ("preset_select_action", "rotate to select load/save/init sub-page action", ACT_ENC_ROTATE,
     SRC_ROTATE, "provisional"),
]


# Closed editing-address schema for the sequencer + scale-mask editors (Codex 9d8b5f43 item #2):
# these are NOT one opaque `set` over a whole editor object — the binding declares HOW the write is
# located (step index + field for the seq record, mask degree for the scale mask) so the target is
# addressable rather than "the whole editor".
KSTATE_COMMAND_ADDRESS = {
    "seq_steps": {"kind": "record", "indexRange": [0, 15], "fields": list(SEQ_STEP_FIELDS)},
    "quantise_scale_editor": {"kind": "mask", "indexRange": [0, 11]},
}

# Preset payloads are ACTION-MANAGED storage: they are written only by presets_load / presets_save
# / presets_initialise, never by a direct encoder `set` on the payload (Codex 9d8b5f43 item #2).
ACTION_MANAGED_KSTATE = {"preset_a", "preset_b", "preset_c", "preset_d"}


def add_keyboard_state(L):
    """Add the keyboard logical state, binding each to its editing widget.

    `keyboard_state` is a PARAMETER region (not a panel region): it is state edited via the
    shared encoder / pushbuttons, not a run of physical widgets, so it has no
    expectedPanelControlCount entry in controlRegions[].
    """
    kb_site = "底部 12 触摸片"
    for leaf, kind, (shape, extras), menu, label, ls, le in KEYBOARD_STATE_ROWS:
        sid = "keyboard.%s" % leaf
        evd = ev(ls, le, kb_site)
        is_vector = shape == SH_VECTOR
        # A keyboard-state param declares the menu (or calibration MODE) it is edited under, so the
        # gate can validate that a `menuItem` naming it belongs to the binding's own menu/mode rather
        # than being an arbitrary string (Codex 9th-review item #1b). Vector params (plate_tune /
        # pushbutton_value) are edited via held plate/button + encoder under the GLOBAL context, not a
        # navigable menu, so they carry neither menu nor mode.
        if menu == "calibration":
            p_menu, p_mode = None, "calibration"
        elif is_vector:
            p_menu, p_mode = None, None
        else:
            p_menu, p_mode = menu, None
        # A keyboard selector-toggle's discrete value domain is projected from the manual closed set
        # (KPOS); an un-evidenced selector (no KPOS entry) keeps no positions and stays a GAP.
        kpos = KPOS.get(leaf) if kind == K_SEL else None
        L.add_param(sid, "keyboard", kind, "keyboard_state", evd, semantic=label,
                    shape=shape, cardinality=extras.get("cardinality"),
                    record_type=extras.get("recordType"), mask_size=extras.get("maskSize"),
                    action_managed=(leaf in ACTION_MANAGED_KSTATE),
                    menu=p_menu, mode=p_mode, positions=kpos)
        # Presets are action-managed (written only by presets_* actions), not encoder-settable.
        if leaf in ACTION_MANAGED_KSTATE:
            continue
        # Vector params (plate_tune / pushbutton_value) are edited via held plate/button + encoder
        # under the GLOBAL context with an explicit index (Codex 9th-review item #1c), NOT a
        # navigable menu — so they get NO menu binding here; a whole-vector menu binding would be a
        # fan-out bug. The heldControl+index path is emitted separately in add_keyboard_phys().
        if is_vector:
            continue
        # The shared keyboard encoder is a ROTARY: it edits a menu value by turning (source=rotate,
        # not `change` — change is the continuous-knob gesture), stepping a discrete value (set) or
        # nudging a continuous one (adjust).
        tgt_op = TO_ADJUST if kind == K_CONT else TO_SET
        # Calibration is entered at POWER-UP (press+hold encoder on boot, manual 1050), i.e. a MODE
        # not a navigable menu, so its parameters live under keyboard-mode=calibration (Codex
        # 23ea438f #1) rather than a plain keyboard-menu=calibration.
        ctx = ctx_mode("calibration") if menu == "calibration" else ctx_menu(menu)
        ca = KSTATE_COMMAND_ADDRESS.get(leaf)
        # Dispatch closure (Codex 9th-review item #1a): a menu/mode encoder binding must carry a
        # closed `menuItem` naming the highlighted item, so ONE turn edits exactly ONE item instead of
        # fanning out over every item the menu holds.
        L.bind("keyboard.encoder", sid, ctx, source=SRC_ROTATE, kind=TK_PARAM,
               target_op=tgt_op, command_address=ca, condition={"menuItem": leaf}, evidence=evd)
    for leaf, name, kind, ls, le in KB_CAL_ACTIONS:
        evd = ev(ls, le, kb_site)
        a = L.add_action("keyboard.%s" % leaf, "keyboard", name, kind, "keyboard_state", evd,
                         mode="calibration")
        # Calibration is a cursor-based BOOT MODE (manual 1050): long-press the encoder on the
        # HIGHLIGHTED calibration item to execute it. INIT and SAVE are two different highlighted
        # items, so each carries a closed `menuItem` condition — without it the two bindings would
        # be identical and could not be told apart. The context is keyboard-mode=calibration, NOT a
        # keyboard-menu=calibration, because calibration is entered at power-up (a mode, Codex
        # 23ea438f #1).
        L.bind("keyboard.encoder", a, ctx_mode("calibration"), source=SRC_LONG, kind=TK_ACTION,
               target_op=TO_INVOKE, condition={"menuItem": leaf}, evidence=evd)
    # PRESETS two-page state machine (Codex Phase-A 10th-review verdict msg e9f1f028). The manual
    # (L1045-1046) documents TWO pages: rotate to scroll presets A-D (slot-list), then press to enter
    # the selected slot's load/save/initialise sub-page (action-list). Each page is a REAL
    # keyboard-menu context (page=slot-list | action-list) so the gate can tell the two apart instead
    # of collapsing them into one flat `menu=presets` (the Round-10 root Codex found). The navigation
    # actions (preset_select_slot / preset_enter_subpage / preset_select_action) drive the workflow but
    # are NOT manual-displayed items, so every binding's `menuItem` is a REAL display item — a slot in
    # slot-list, a load/save/initialise action in action-list — NEVER a navigation action's own leaf.
    # That keeps the menu topology an INDEPENDENT declaration rather than self-proven from a target's
    # `menu` field. Gesture evidence: rotate=select-slot and press=enter-sub-page are the two manual
    # edges (confirmed); sub-page item-select + item-execution are undocumented, so they stay
    # provisional (Codex 23ea438f #1). The three load/save/initialise ACTIONS are confirmed (they are
    # manual items); only the execute gesture (the binding) is provisional.
    for leaf, name, kind, src, st in KB_PRESET_NAV:
        evd = ev(1040, 1045, kb_site)
        L.add_action("keyboard.%s" % leaf, "keyboard", name, kind, "keyboard_state", evd,
                     menu="presets", status=st)
    for leaf, name, kind in KB_PRESET_ACTIONS:
        evd = ev(1040, 1045, kb_site)
        L.add_action("keyboard.%s" % leaf, "keyboard", name, kind, "keyboard_state", evd,
                     menu="presets")
    # slot-list page: rotate scrolls the A-D highlight, press opens that slot's action-list. The
    # enter edge CARRIES the current slot (presetSlot) so the checker can confirm a sub-page is
    # entered for a real slot, not an unbound press (Codex item #2/#5).
    for slot in PRESET_SLOTS:
        # rotate -> preset_select_slot (menuItem = the slot now highlighted).
        L.bind("keyboard.encoder", "keyboard.preset_select_slot",
               ctx_menu_page(PRESET_PAGE_SLOT_LIST), source=SRC_ROTATE, kind=TK_ACTION,
               target_op=TO_INVOKE, condition={"menuItem": slot}, status="confirmed",
               evidence=ev(1040, 1045, kb_site))
        # press -> preset_enter_subpage (menuItem = the slot, presetSlot = the slot being entered).
        L.bind("keyboard.encoder", "keyboard.preset_enter_subpage",
               ctx_menu_page(PRESET_PAGE_SLOT_LIST), source=SRC_PRESS, kind=TK_ACTION,
               target_op=TO_INVOKE, condition={"presetSlot": slot, "menuItem": slot},
               status="confirmed", evidence=ev(1040, 1045, kb_site))
    # action-list page: rotate scrolls load/save/initialise, press acts on the selected slot. The
    # execute edge carries BOTH a closed `presetSlot` (which slot) and `menuItem` (which action), so
    # one press invokes exactly one of the three, not all at once (dispatch closure).
    for leaf, name, kind in KB_PRESET_ACTIONS:
        # rotate -> preset_select_action (menuItem = the operation now highlighted).
        L.bind("keyboard.encoder", "keyboard.preset_select_action",
               ctx_menu_page(PRESET_PAGE_ACTION_LIST), source=SRC_ROTATE, kind=TK_ACTION,
               target_op=TO_INVOKE, condition={"menuItem": leaf}, status="provisional",
               evidence=ev(1040, 1045, kb_site))
        for slot in PRESET_SLOTS:
            # press -> load/save/initialise for this slot (menuItem = the operation, presetSlot = slot).
            L.bind("keyboard.encoder", "keyboard.%s" % leaf,
                   ctx_menu_page(PRESET_PAGE_ACTION_LIST), source=SRC_PRESS, kind=TK_ACTION,
                   target_op=TO_INVOKE, condition={"presetSlot": slot, "menuItem": leaf},
                   status="provisional", evidence=ev(1040, 1045, kb_site))


# p23/p24 semantic labels per (cartridge, program). X/Y/Z knob meaning for each of the 39
# programs, transcribed verbatim from the manual's cartridge catalogue (lines 1205-1303).
PROGRAM_LABELS = {
    "cathedral": [
        ("Octave up", "Octave down", "Decay"),
        ("Feedback", "Delay", "Reverb"),
        ("Feedback", "Delay", "Reverb"),
    ],
    "magic": [
        ("Feedback", "Delay", "Pitch"),
        ("Feedback", "Delay", "Pitch"),
        ("Feedback", "Delay", "Pitch"),
    ],
    "time": [
        ("Feedback", "Delay", "Reverb"),
        ("Feedback", "Delay", "Mod depth"),
        ("Feedback", "Delay/Vibrato rate", "Mod depth"),
    ],
    "vibrotrem": [
        ("Depth", "Rate", "Reverb"),
        ("Depth", "Rate", "Reverb"),
        ("Depth", "Rate", "Reverb"),
    ],
    "filter": [
        ("Filter amount", "Envelope", "Reverb"),
        ("HP cutoff", "LP cutoff", "Resonance"),
        ("Cut 1", "Cut 2", "Resonance"),
    ],
    "vibe": [
        ("Depth", "Rate", "Reverb"),
        ("Depth", "Rate", "Reverb"),
        ("Resonance", "Rate", "Mod depth"),
    ],
    "pitch_shifter": [
        ("Octave down", "Octave up", "Direct"),
        ("Octave down", "Octave up", "Direct"),
        ("Pitch 1", "Pitch 2", "Voice mix"),
    ],
    "infinity": [
        ("Pre delay", "Pre delay mod", "Decay"),
        ("Feedback", "Delay", "Pitch"),
        ("Feedback", "Delay", "Pitch"),
    ],
    "string_ringer": [
        ("Frequency", "Resonance", "Sub"),
        ("Frequency", "Rate", "Reverb"),
        ("Pitch speed", "S&H rate", "Freq ring mod"),
    ],
    "syntex_1": [
        ("Vibrato rate", "Resonance", "Sub"),
        ("Tremolo rate", "Resonance", "Sub"),
        ("Tone", "Color", "Sub"),
    ],
    "digital": [
        ("Sample rate", "Cutoff", "Input gain"),
        ("Sample rate", "LFO speed", "LFO amount"),
        ("Sample rate", "Envelope amount", "Input gain"),
    ],
    "generator": [
        ("Pitch 1", "Pitch 2", "FM 2-1"),
        ("LFO rate", "Pitch", "Pitch mod +/-"),
        ("Cutoff", "Pitch", "LP/HP"),
    ],
    "orche": [
        ("Delay time", "Feedback amount", "Trigger threshold"),
        ("Delay time", "Feedback amount", "Trigger threshold"),
        ("Delay time", "Feedback amount", "Delay mod LFO/RND"),
    ],
}


def add_program_params(L, manifest):
    """117 program-owned X/Y/Z persisted state, bound to the shared effector X/Y/Z widget.

    Each carries its p23/p24 semantic label. The widget `effector.x/y/z` is shared and binds
    to the program's X/Y/Z parameter under that program's context (a structured `program`
    context that references the ProgramId — Codex item #4, not a bare string).
    """
    for p in manifest["target"]["programs"]:
        cat = p.get("cartridge", "").lower().replace(" ", "_").replace("-", "_")
        slot = int(p.get("slot", 1)) - 1
        labels = PROGRAM_LABELS.get(cat)
        own = p["stable_id"]
        e = {"ref": REF, "lineStart": 1205, "lineEnd": 1303, "panelSite": "p23/p24"}
        status = p.get("status", "provisional")
        for idx, axis in enumerate(("x", "y", "z")):
            widget = "effector.%s" % axis          # shared effector X/Y/Z widget
            psid = "%s.%s" % (own, axis)           # program.<cart>.slot.<axis> persisted state
            sem = labels[slot][idx] if labels else None
            L.add_param(psid, own, K_CONT, "program_params", e, semantic=sem, status=status)
            L.bind(widget, psid, ctx_program(own), source=SRC_CHANGE, kind=TK_PARAM,
                   target_op=TO_ADJUST, status=status, evidence=e)


def _compact(o):
    return json.dumps(o, ensure_ascii=False)


def _list_lines(o, indent):
    pad = "  " * indent
    if not o:
        return ["[]"]
    if all(not isinstance(x, (dict, list)) for x in o):
        return ["[" + ", ".join(_compact(x) for x in o) + "]"]
    lines = ["["]
    n = len(o)
    for i, x in enumerate(o):
        comma = "," if i < n - 1 else ""
        if isinstance(x, dict):
            lines.append(pad + "  " + _compact(x) + comma)
        elif isinstance(x, list):
            lines.append(pad + "  " + _list_lines(x, indent + 1)[0] + comma)
        else:
            lines.append(pad + "  " + _compact(x) + comma)
    lines.append(pad + "]")
    return lines


def _dump(m):
    def dict_lines(o, indent):
        pad = "  " * indent
        if not o:
            return [pad + "{}"]
        lines = [pad + "{"]
        items = list(o.items())
        n = len(items)
        for i, (k, v) in enumerate(items):
            comma = "," if i < n - 1 else ""
            kk = _compact(k)
            if isinstance(v, dict):
                sub = dict_lines(v, indent + 1)
                sub[-1] = sub[-1] + comma
                lines.append(pad + "  " + kk + ": " + sub[0][len(pad) + 2:])
                lines.extend(sub[1:])
            elif isinstance(v, list):
                sub = _list_lines(v, indent + 1)
                sub[-1] = sub[-1] + comma
                lines.append(pad + "  " + kk + ": " + sub[0])
                lines.extend(sub[1:])
            else:
                lines.append(pad + "  " + kk + ": " + _compact(v) + comma)
        lines.append(pad + "}")
        return lines

    return "\n".join(dict_lines(m, 0)) + "\n"


# Per-module capability declaration (independent transcription; replaces the old generic
# "every module must have a param AND a jack" blanket gate). `voices` (drone triggers) has
# parameters=False: it is momentary-touch only. Terminals are NOT modules; their endpoints are
# the terminal I/O. Codex msg 06ef6b70 item #3; c7089521 item #5 splits `jacks` into
# `patchableJacks` vs `internalEndpoints` (mixer / voices have no normal patchable jack).
#
# Codex a23a9618 (sixth review) item #5: capabilities must be an INDEPENDENT hand-authored table,
# NOT derived from the endpoint inventory — if an endpoint is missing from the transcript the
# capability stays TRUE (as it is on the panel) and the gate's present-but-empty check catches the
# transcription gap. The frozen table below is the ground fact; it is never re-derived from
# endpoints.
CAPABILITIES = {
    "vco_a":        {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "vco_b":        {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_1":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_2":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_3":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_4":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_5":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "drone_6":      {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "envelope_a":   {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "envelope_b":   {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "mixer":        {"parameters": True, "patchableJacks": False, "internalEndpoints": True, "controls": True},
    "vcf":          {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "effector":     {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "lfo_a":        {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "lfo_b":        {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "sequencer":    {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "joystick":     {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "preamp":       {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "env_follower": {"parameters": True, "patchableJacks": True, "internalEndpoints": True,  "controls": True},
    "keyboard":     {"parameters": True, "patchableJacks": True, "internalEndpoints": False, "controls": True},
    "voices":       {"parameters": False, "patchableJacks": False, "internalEndpoints": True, "controls": True},
}
def inject_capabilities(modules, _endpoints):
    """Assign the INDEPENDENT hand-authored capability table (never derived from endpoints).

    The `_endpoints` argument is retained for call-site compatibility only; capabilities are the
    frozen ground fact. The gate cross-checks capability vs what was actually transcribed, so a
    missing endpoint transcription cannot silently co-turn a capability off. There is NO default
    fallback: a module whose stable_id is absent from CAPABILITIES (or a CAPABILITIES entry with no
    matching module) fails directly — the table must be EXACTLY the module-ID set (msg 9d8b5f43
    item #3).
    """
    module_ids = {m.get("stable_id") for m in modules}
    missing = {m.get("stable_id") for m in modules} - set(CAPABILITIES)
    extra = set(CAPABILITIES) - module_ids
    if missing:
        raise ValueError("modules missing from CAPABILITIES: %s" % sorted(missing))
    if extra:
        raise ValueError("CAPABILITIES entries with no module: %s" % sorted(extra))
    for m in modules:
        sid = m.get("stable_id")
        m["capabilities"] = CAPABILITIES[sid]


# Top-level record payload schemas for the non-scalar keyboard records (Codex c7089521 item
# #1/#2). A record type is only closed once every `of`/`element` reference resolves to a real
# schema and its body is validated, so `keyboard_seq.element` and `keyboard_preset.payload.of` both
# point at defined types (Codex a23a9618 item #1) — no inline anonymous maps, no dangling strings.

# The preset payload is the FUNCTIONAL keyboard param set: everything a preset stores, EXCLUDING
# clock BPM, the calibration block, and presets A-D themselves (the latter avoids self-recursion
# and erroneous storage of a preset inside a preset).
KEYBOARD_NON_PRESET = {
    "clock_bpm",
    "calibration_v_oct", "calibration_pressure", "dac_vref",
    "touch_threshold", "release_threshold", "pressure_min", "pressure_max",
    "mpr121_charge", "mpr121_discharge", "debounce", "encoder_direction",
    "preset_a", "preset_b", "preset_c", "preset_d",
}


def _preset_params():
    return ["keyboard.%s" % leaf for leaf, *_ in KEYBOARD_STATE_ROWS
            if leaf not in KEYBOARD_NON_PRESET]


RECORD_SCHEMAS = {
    "keyboard_step": {
        "note": "One sequencer step: the closed element type of keyboard_seq.steps.",
        "kind": "record",
        "fields": [{"name": "note", "type": "number"},
                   {"name": "value", "type": "voltage"},
                   {"name": "gate", "type": "bool"}],
    },
    "keyboard_seq": {
        "note": "Sequencer step data (seq_editor), up to 16 steps of note/value + gate. element is "
                "a CLOSED reference to keyboard_step, not an inline anonymous map.",
        "kind": "record",
        "fields": [{"name": "steps", "type": "array", "count": 16,
                    "element": {"name": "step", "type": "record", "of": "keyboard_step"}}],
    },
    "keyboard_params_minus_clock": {
        "note": "The functional keyboard param-set a preset stores: all keyboard parameters "
                "EXCLUDING clock_bpm, the calibration block, and presets A-D (avoids recursion + "
                "erroneous storage). Closed param-set type referenced by keyboard_preset.payload.",
        "kind": "params",
        "params": _preset_params(),
    },
    "keyboard_preset": {
        "note": "Keyboard parameter payload for presets A-D (manual L1040-1045).",
        "kind": "record",
        "fields": [{"name": "payload", "type": "record", "of": "keyboard_params_minus_clock"}],
        "excludes": ["keyboard.%s" % leaf for leaf in sorted(KEYBOARD_NON_PRESET)],
    },
}

# Grandfather the PRE-CORRECTION registry rogue ids (Codex c7089521 item #5). The allowlist is
# COMPUTED here, not hard-coded, so a rebuild reproduces the manifest exactly rather than popping a
# stale hand-entered list (Codex 644ea86e reproducibility): it is exactly the set of registry
# Parameter/Jack ids NOT in the freshly-built target. As Phase B re-ids the id-space toward the
# target the list SHRINKS to empty, and --require-full requires it empty AND no rogue. The gate's
# independent legacy ceiling (LEGACY_ROGUE_*_CEILING in check_registry_complete.py) still forbids
# any try to mask a genuinely-NEW rogue: an allow entry for an id outside that ceiling fails there.
def _registry_rogues(L, endpoints):
    """Return ({param rogues}, {jack rogues}) = registry ids not in the freshly-built target."""
    with open(SPEC, encoding="utf-8") as fh:
        reg = json.load(fh)
    reg_param_sids = set()
    reg_jack_sids = set()
    for mo in reg.get("modules", []):
        reg_jack_sids |= {j.get("stable_id") for j in mo.get("jacks", [])}
        reg_param_sids |= {p.get("stable_id") for p in mo.get("parameters", [])}
    for pr in reg.get("programs", []):
        reg_param_sids |= {p.get("stable_id") for p in pr.get("parameters", [])}

    target_param_sids = {p["stable_id"] for p in L.params}
    target_patchable = {e["stable_id"] for e in endpoints if e.get("patchable")}
    return sorted(reg_param_sids - target_param_sids), sorted(reg_jack_sids - target_patchable)


def main():
    with open(MANIFEST, encoding="utf-8") as fh:
        m = json.load(fh)

    L = build()
    add_keyboard_state(L)
    add_program_params(L, m)
    pjt = m["target"].get("paramsJackTargets") or {}
    inject_capabilities(m["target"]["modules"], pjt.get("endpoints", []))

    # fixed lfo cv_out evidence -> manual LFO section (Codex item #4).
    for ep in pjt.get("endpoints", []):
        if ep.get("stable_id") in ("lfo_a.cv_out", "lfo_b.cv_out"):
            ep["evidence"] = ev(427, 442)
            ep["status"] = "confirmed"

    tgt = m["target"]
    tgt["panelControls"] = L.panel
    tgt["controlBindings"] = L.bindings
    tgt["parameters"] = L.params
    tgt["actions"] = L.actions
    tgt["controlRegions"] = L.regions
    tgt["recordSchemas"] = RECORD_SCHEMAS
    # paramsJackTargets records jacks + internal endpoints ONLY (the old stale .params list is
    # dropped). Per-item Parameter id-space lives on parameters[].
    tgt["paramsJackTargets"] = {
        "note": ("Three-entity split (Codex msg 06ef6b70): panelControls[] = real physical panel "
                 "widgets; parameters[] = the persisted logical state (Parameter target); "
                 "controlBindings[] = widget -> parameter|action with a structured context "
                 "(global / program / keyboard-menu). Codex msg c7089521: parameters carry an "
                 "explicit shape/cardinality/recordType/maskSize; actions[] gives the event-only "
                 "widgets a stable identity; migrationAllowlist grandfathers pre-correction rogues "
                 "(always-on no-new-rogue). Fixed internal endpoints are independent of patchable "
                 "jacks."),
        "endpoints": pjt.get("endpoints", []),
    }
    allow_params, allow_jacks = _registry_rogues(L, pjt.get("endpoints", []))
    m["migrationAllowlist"] = {
        "note": "Monotonically-shrinking migration allowlist for pre-correction registry rogue ids. "
                "The gate is ALWAYS-ON: any registry param/jack not in the target and not in this "
                "list is a NEW rogue and fails even without --require-full. --require-full requires "
                "the list empty AND no rogue. Computed from the actual registry, never hand-set.",
        "parameters": allow_params,
        "jacks": allow_jacks,
    }

    with open(MANIFEST, "w", encoding="utf-8") as fh:
        fh.write(_dump(m))

    print("panelControls:", len(tgt["panelControls"]),
          "parameters:", len(tgt["parameters"]),
          "actions:", len(tgt["actions"]),
          "bindings:", len(tgt["controlBindings"]),
          "regions:", len(tgt["controlRegions"]))
    for r in tgt["controlRegions"]:
        print("  region %-18s module=%-12s widgets=%d" %
              (r["id"], r["module"], r["expectedPanelControlCount"]))


if __name__ == "__main__":
    sys.exit(main())
