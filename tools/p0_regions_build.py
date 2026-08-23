#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build the Phase-A three-entity panel ledger into p0_inventory_manifest.json.

TRANSCRIPTION TOOL, NOT a registry-derived generator: every row below is a hand-audit
of the panel/manual (solar42N_manual_text.txt; the full-panel render at lines 61-116,
the effector cartridge catalogue p23-24 at lines 1205-1310, the prose control sections).
Codex 3rd-review msg 66a83ca0 demanded we stop conflating three entity kinds; this tool
splits them (msg 06ef6b70 items 1/2/4):

  - target.panelControls[]   : ONE row per REAL physical panel widget
      {stable_id, owner, kind (continuous|selector-toggle|momentary-touch|rotary-encoder),
       region, panelLabel, status, evidence}
  - target.parameters[]      : the persisted LOGICAL state (the Parameter target)
      {stable_id, owner, kind (continuous|selector-toggle|state-field),
       semanticLabel?, region, status, evidence}
  - target.controlBindings[] : widget -> parameter, with a program/mode/global context
      {stable_id, from (panelControl id), to (parameter id), context, status, evidence}
  - target.controlRegions[]  : per-panel-region subtotal; the DECLARED count is an
      INDEPENDENT hand-counted constant (`expectedPanelControlCount`), never `len(rows)`.

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

# Physical-widget kinds (panelControls) vs logical-value kinds (parameters).
K_CONT = "continuous"
K_SEL = "selector-toggle"
K_MOM = "momentary-touch"
K_RENC = "rotary-encoder"
K_STATE = "state-field"

PANEL_KINDS = {K_CONT, K_SEL, K_MOM, K_RENC}
PARAM_KINDS = {K_CONT, K_SEL, K_STATE}


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


class Ledger:
    """Collects panel controls, parameters, bindings, and region subtotals.

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
    """

    def __init__(self):
        self.panel = []
        self.params = []
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

    def emit(self, leaf, kind, count=1, param_kind=None, label=None,
             param_owner=None, param_leaf=None, context="global"):
        """Expand `count` identical widgets; each gets a persisted param + binding unless
        `param_kind` is None (an event-only widget: momentary touch / trigger)."""
        owner = self._cur["owner"]
        evid = self._cur["evidence"]
        site = self._cur["site"]
        for i, wleaf in enumerate(_count(leaf, count)):
            wid = "%s.%s" % (owner, wleaf)
            self._place({"stable_id": wid, "owner": owner, "kind": kind, "region": self._cur["rid"],
                         "panelLabel": wleaf, "status": "confirmed", "evidence": evid})
            if param_kind is None:
                continue
            po = param_owner or owner
            pleaf = param_leaf or wleaf
            psid = "%s.%s" % (po, pleaf)
            self.params.append({"stable_id": psid, "owner": po, "kind": param_kind,
                                "region": self._cur["rid"], "status": "confirmed",
                                "evidence": evid})
            if label:
                self.params[-1]["semanticLabel"] = label
            self.bindings.append({"stable_id": "bnd.%s->%s" % (wid, psid), "from": wid, "to": psid,
                                  "context": context, "status": "confirmed", "evidence": evid})

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


def build():
    L = Ledger()

    # CLASSIC drones 1/2/4/5 : 5 generators x (TUNE/MUTE/MOD) + VOLT/ATT/RLS/GATE-HOLD = 19.
    for mod, site in [("drone_1", "左上 DRONE 1/2"), ("drone_2", "左上 DRONE 1/2"),
                      ("drone_4", "右上 DRONE 4/5"), ("drone_5", "右上 DRONE 4/5")]:
        L.region("drone_classic_" + mod, mod, site, 19, ev(294, 312, site))
        L.emit("tune", K_CONT, 5, K_CONT)
        L.emit("mute", K_SEL, 5, K_SEL)
        L.emit("mod", K_SEL, 5, K_SEL)
        L.emit("volt", K_CONT, 1, K_CONT)
        L.emit("att", K_CONT, 1, K_CONT)
        L.emit("rls", K_CONT, 1, K_CONT)
        L.emit("gate_hold", K_SEL, 1, K_SEL)
        L.close()

    # NEW drones 3/6 (Papa Srapa) : RATE/MOD/DIVIDER/PITCH/NOISE + the AR ATT/RLS + switches
    # HI-LOW/FM/AM/RATE + HOLD = 12. (S&H is a display/circuit region, NOT a control — Codex.)
    for mod, site in [("drone_3", "左中 DRONE 3"), ("drone_6", "右中 DRONE 6")]:
        L.region("drone_new_" + mod, mod, site, 12, ev(322, 370, site))
        L.emit("rate", K_CONT, 1, K_CONT)
        L.emit("mod", K_CONT, 1, K_CONT)
        L.emit("divider", K_CONT, 1, K_CONT)
        L.emit("pitch", K_CONT, 1, K_CONT)
        L.emit("noise", K_CONT, 1, K_CONT)
        L.emit("att", K_CONT, 1, K_CONT)
        L.emit("rls", K_CONT, 1, K_CONT)
        L.emit("hi_low", K_SEL, 1, K_SEL)
        L.emit("fm", K_SEL, 1, K_SEL)
        L.emit("am", K_SEL, 1, K_SEL)
        L.emit("rate_switch", K_SEL, 1, K_SEL)
        L.emit("hold", K_SEL, 1, K_SEL)
        L.close()

    # VCO A / VCO B : 8 physical controls each (Codex). No duplicate wave/shape/CV-FM, and the
    # octave +3/low is ONE 3-position selector, not two.
    for mod, site in [("vco_a", "中上 VCO A"), ("vco_b", "中上 VCO B")]:
        L.region(mod, mod, site, 8, ev(376, 422, site))
        L.emit("cv_amt", K_CONT, 1, K_CONT)
        L.emit("morph", K_CONT, 1, K_CONT)      # morphing waveform (saw->inv saw / sine->tri)
        L.emit("tune", K_CONT, 1, K_CONT)
        L.emit("pwm", K_CONT, 1, K_CONT)
        L.emit("pw", K_CONT, 1, K_CONT)         # pulse width / SHAPE
        L.emit("lin_exp", K_SEL, 1, K_SEL)
        L.emit("oct_sel", K_SEL, 1, K_SEL)      # +3 / low (3-position)
        L.emit("sub_sel", K_SEL, 1, K_SEL)      # -1 / sub
        L.close()

    # ENVELOPE A / B : HOLD + SELF-GENERATION + ADSR = 6 each (Codex adds self-generation).
    for mod, site in [("envelope_a", "中部 ENV A"), ("envelope_b", "中部 ENV B")]:
        L.region(mod, mod, site, 6, ev(385, 408, site))
        L.emit("hold", K_SEL, 1, K_SEL)
        L.emit("self_gen", K_SEL, 1, K_SEL)
        L.emit("a", K_CONT, 1, K_CONT)
        L.emit("d", K_CONT, 1, K_CONT)
        L.emit("s", K_CONT, 1, K_CONT)
        L.emit("r", K_CONT, 1, K_CONT)
        L.close()

    # VOICE MIXER : 10 channels x (PAN/VOL) = 20.
    L.region("mixer", "mixer", "中部 VOICE MIXER", 20, ev(1105, 1113, "中部 VOICE MIXER"))
    for i in range(1, 11):
        L.emit("ch%d_pan" % i, K_CONT, 1, K_CONT)
        L.emit("ch%d_vol" % i, K_CONT, 1, K_CONT)
    L.close()

    # DUAL VCF : per-side FREQ/RES/MOD + BP-LP; shared DIST/GAIN/LINK.
    L.region("dual_vcf", "vcf", "中上 DUAL VCF", 11, ev(1118, 1153, "中上 DUAL VCF"))
    L.emit("l_freq", K_CONT, 1, K_CONT)
    L.emit("l_res", K_CONT, 1, K_CONT)
    L.emit("l_mod", K_CONT, 1, K_CONT)
    L.emit("l_bp_lp", K_SEL, 1, K_SEL)
    L.emit("r_freq", K_CONT, 1, K_CONT)
    L.emit("r_res", K_CONT, 1, K_CONT)
    L.emit("r_mod", K_CONT, 1, K_CONT)
    L.emit("r_bp_lp", K_SEL, 1, K_SEL)
    L.emit("dist", K_CONT, 1, K_CONT)
    L.emit("gain", K_CONT, 1, K_CONT)
    L.emit("link", K_SEL, 1, K_SEL)
    L.close()

    # DUAL EFFECTOR : shared X/Y/Z + BLEND + MASTER + PHONE + two 1-2-3 selectors = 8.
    # (No duplicate cartridge-slot control; PHONE volume added — Codex.)
    L.region("dual_effector", "effector", "中上 DUAL EFFECTOR", 8, ev(1159, 1199, "中上 DUAL EFFECTOR"))
    L.emit("x", K_CONT, 1, K_CONT)
    L.emit("y", K_CONT, 1, K_CONT)
    L.emit("z", K_CONT, 1, K_CONT)
    L.emit("blend", K_CONT, 1, K_CONT)
    L.emit("master", K_CONT, 1, K_CONT)
    L.emit("phone", K_CONT, 1, K_CONT)
    L.emit("select_l", K_SEL, 1, K_SEL)
    L.emit("select_r", K_SEL, 1, K_SEL)
    L.close()

    # LFO A / LFO B : wave + rate + x1/x6/x10.
    for mod, site in [("lfo_a", "下排 LFO A"), ("lfo_b", "下排 LFO B")]:
        L.region(mod, mod, site, 3, ev(425, 442, site))
        L.emit("wave", K_CONT, 1, K_CONT)
        L.emit("rate", K_CONT, 1, K_CONT)
        L.emit("speed_mult", K_SEL, 1, K_SEL)
        L.close()

    # 5 STEP SEQ : pulser is a CONTINUOUS fine-tune knob (Codex); clock/stages selectors;
    # 5 step-CV + 5 step-gate.
    L.region("seq", "sequencer", "下排 5-step seq", 13, ev(766, 775, "下排 5-step seq"))
    L.emit("pulser", K_CONT, 1, K_CONT)
    L.emit("clock", K_SEL, 1, K_SEL)
    L.emit("stages", K_SEL, 1, K_SEL)
    L.emit("step_cv", K_CONT, 5, K_CONT)
    L.emit("step_gate", K_SEL, 5, K_SEL)
    L.close()

    # JOYSTICK : one 2-axis stick + two offset regulators = 3 physical widgets.
    L.region("joystick", "joystick", "下排 joystick", 3, ev(446, 470, "下排 joystick"))
    L.emit("joy", K_CONT, 1, K_CONT)            # two-axis X/Y stick (X + Y persisted)
    L.emit("offset_x", K_CONT, 1, K_CONT)
    L.emit("offset_y", K_CONT, 1, K_CONT)
    L.close()

    # PREAMP : GAIN only (EXT SOURCE is a jack; built-in mic auto-bypassed — Codex item #2).
    L.region("preamp", "preamp", "下排 preamp", 1, ev(516, 555, "下排 preamp"))
    L.emit("gain", K_CONT, 1, K_CONT)
    L.close()

    # ENVELOPE FOLLOWER : attack + release.
    L.region("env_follower", "env_follower", "下排 env follower", 2, ev(551, 553, "下排 env follower"))
    L.emit("attack", K_CONT, 1, K_CONT)
    L.emit("release", K_CONT, 1, K_CONT)
    L.close()

    # KEYBOARD physical: 12 touchplates + 8 pushbuttons + a push/turn encoder = 21 widgets.
    # Plates/buttons are MOMENTARY (touch/press event) and the encoder is ROTARY — none stores a
    # value of its own. The persisted keyboard STATE (plate tune, button offset, calibration, arp,
    # seq, presets, ...) lives in the `keyboard_state` parameter region and is edited via the
    # encoder (Codex: "the encoder must express rotate + press/long-press, not a single momentary
    # param").
    L.region("keyboard_phys", "keyboard", "底部 12 触摸片", 21, ev(559, 654, "底部 12 触摸片"))
    L.emit("plate", K_MOM, 12, None)            # momentary note/gate trigger (no persisted value)
    L.emit("pushbutton", K_MOM, 8, None)        # momentary function trigger (no persisted value)
    L.emit("encoder", K_RENC, 1, None)          # rotate + press/long-press; edits a highlighted param
    L.close()

    # DRONE VOICES 1-6 pushbutton triggers (module 'voices') : 6 momentary, NO persisted state.
    L.region("drone_voices", "voices", "右下 DRONE VOICES", 6, ev(778, 788, "右下 DRONE VOICES"))
    L.emit("button", K_MOM, 6, None)
    L.close()

    return L


# Keyboard menu / state parameters. These are NOT panel controls — they are LOGICAL state
# edited via the shared encoder / pushbuttons (Codex: three-entity split). Presets A-D are
# state records (payloads), not four ordinary params; plate/button/calibration values too.
# (owner, leaf, kind, semanticLabel|None, lineStart, lineEnd)
KEYBOARD_STATE_ROWS = [
    ("behaviour", K_SEL, "Single/Twin/Split", 659, 733),
    ("mode", K_SEL, None, 809, 818),
    ("arp_hold", K_SEL, None, 822, 857), ("arp_clock", K_SEL, None, 822, 857),
    ("arp_direction", K_SEL, None, 822, 857), ("arp_variation", K_SEL, None, 822, 857),
    ("arp_interval", K_CONT, None, 822, 857), ("arp_rhythm", K_SEL, None, 822, 857),
    ("arp_length", K_CONT, None, 822, 857),
    ("seq_run", K_SEL, None, 866, 910), ("seq_length", K_CONT, None, 866, 910),
    ("seq_clock", K_SEL, None, 866, 910), ("seq_direction", K_SEL, None, 866, 910),
    ("seq_editor", K_SEL, None, 866, 910), ("seq_cv_output", K_SEL, None, 866, 910),
    ("seq_rhythm", K_SEL, None, 866, 910),
    ("portamento_speed", K_CONT, None, 916, 923), ("portamento_legato", K_SEL, None, 916, 923),
    ("vibrato_speed", K_CONT, None, 934, 946), ("vibrato_depth", K_CONT, None, 934, 946),
    ("vibrato_delay", K_CONT, None, 934, 946), ("vibrato_pressure", K_CONT, None, 934, 946),
    ("pressure_output", K_SEL, None, 951, 965), ("pressure_rise", K_CONT, None, 951, 965),
    ("pressure_fall", K_CONT, None, 951, 965),
    ("quantise_scale_editor", K_SEL, None, 970, 984), ("quantise_load_scale", K_SEL, None, 970, 984),
    ("root_note", K_CONT, None, 992, 993), ("clock_bpm", K_CONT, None, 1028, 1029),
    ("calibration_v_oct", K_CONT, "V/oct calibration", 1050, 1093),
    ("calibration_pressure", K_CONT, "pressure calibration", 1050, 1093),
    ("encoder_direction", K_SEL, None, 1050, 1093),
    ("plate_tune", K_CONT, "plate tuning", 620, 660),
    ("pushbutton_value", K_CONT, "button offset", 601, 613),
    ("preset_a", K_STATE, "preset A payload", 1040, 1045),
    ("preset_b", K_STATE, "preset B payload", 1040, 1045),
    ("preset_c", K_STATE, "preset C payload", 1040, 1045),
    ("preset_d", K_STATE, "preset D payload", 1040, 1045),
]


def add_keyboard_state(L, panel_sids):
    """Add the keyboard logical state, binding each to its editing widget.

    `keyboard_state` is a PARAMETER region (not a panel region): it is state edited via the
    shared encoder / pushbuttons, not a run of physical widgets, so it has no
    expectedPanelControlCount entry in controlRegions[].
    """
    kb_site = "底部 12 触摸片"
    for leaf, kind, label, ls, le in KEYBOARD_STATE_ROWS:
        sid = "keyboard.%s" % leaf
        evd = ev(ls, le, kb_site)
        L.params.append({"stable_id": sid, "owner": "keyboard", "kind": kind,
                         "region": "keyboard_state", "status": "confirmed", "evidence": evd})
        if label:
            L.params[-1]["semanticLabel"] = label
        # Each persisted keyboard state field is edited via the shared encoder (rotate changes the
        # selected/highlighted parameter; the momentary plates/buttons select the note/gate event).
        src = "keyboard.encoder"
        L.bindings.append({"stable_id": "bnd.%s->%s" % (src, sid), "from": src, "to": sid,
                           "context": "global", "status": "confirmed", "evidence": evd})


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
    to the program's X/Y/Z parameter under that program's context.
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
            L.params.append({"stable_id": psid, "owner": own, "kind": K_CONT,
                             "semanticLabel": sem, "region": "program_params",
                             "status": status, "evidence": e})
            L.bindings.append({"stable_id": "bnd.%s->%s" % (widget, psid), "from": widget,
                               "to": psid, "context": own, "status": status, "evidence": e})


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
# the terminal I/O. Codex msg 06ef6b70 item #3.
CAPABILITIES = {
    "voices": {"parameters": False, "jacks": True, "controls": True},
}
DEFAULT_CAP = {"parameters": True, "jacks": True, "controls": True}


def inject_capabilities(modules):
    for m in modules:
        sid = m.get("stable_id")
        m["capabilities"] = CAPABILITIES.get(sid, dict(DEFAULT_CAP))


def main():
    with open(MANIFEST, encoding="utf-8") as fh:
        m = json.load(fh)

    L = build()
    panel_sids = [p["stable_id"] for p in L.panel]
    add_keyboard_state(L, panel_sids)
    add_program_params(L, m)
    inject_capabilities(m["target"]["modules"])

    # fixed lfo cv_out evidence -> manual LFO section (Codex item #4).
    for ep in m["target"]["paramsJackTargets"]["endpoints"]:
        if ep.get("stable_id") in ("lfo_a.cv_out", "lfo_b.cv_out"):
            ep["evidence"] = ev(427, 442)
            ep["status"] = "confirmed"

    tgt = m["target"]
    tgt["panelControls"] = L.panel
    tgt["controlBindings"] = L.bindings
    tgt["parameters"] = L.params
    tgt["controlRegions"] = L.regions
    # Canonical Parameter target is top-level `target.parameters[]`; `paramsJackTargets` records
    # jacks + internal endpoints ONLY (the old stale .params list is dropped). Per-item Parameter
    # id-space lives on parameters[] (Codex msg 06ef6b70: panelControls/parameters/controlBindings
    # three entities).
    pjt = tgt.get("paramsJackTargets") or {}
    tgt["paramsJackTargets"] = {
        "note": ("Three-entity split (Codex msg 06ef6b70): panelControls[] = real physical panel "
                 "widgets; parameters[] = the persisted logical state (Parameter target); "
                 "controlBindings[] = widget->parameter with a program/mode/global context. "
                 "controlRegions[] expectedPanelControlCount is an INDEPENDENT hand-counted "
                 "constant, never derived from rows. Fixed internal endpoints are independent of "
                 "patchable jacks."),
        "endpoints": pjt.get("endpoints", []),
    }

    with open(MANIFEST, "w", encoding="utf-8") as fh:
        fh.write(_dump(m))

    print("panelControls:", len(tgt["panelControls"]),
          "parameters:", len(tgt["parameters"]),
          "bindings:", len(tgt["controlBindings"]),
          "regions:", len(tgt["controlRegions"]))
    for r in tgt["controlRegions"]:
        print("  region %-18s module=%-12s widgets=%d" %
              (r["id"], r["module"], r["expectedPanelControlCount"]))


if __name__ == "__main__":
    sys.exit(main())
