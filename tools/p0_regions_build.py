#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build the Phase-A per-region control ledger into p0_inventory_manifest.json.

TRANSCRIPTION TOOL, NOT a registry-derived generator: the per-region control spec
below is a hand-audit of the panel/manual (solar42N_manual_text.txt; the full-panel
render at manual lines 688-800, the effector catalogue p23-24 at lines 1207-1309,
the prose control sections). Every control row carries its own primary-evidence
anchor (manual line range or panel site). The tool expands the spec into:

  - target.controls[]       : ONE row per visible/operable item
      {stable_id, owner, kind (continuous|selector-toggle|momentary-touch-event),
       persistable (bool), region, status, evidence}
  - target.controlRegions[] : {id, module, panelSite, count, evidence} -> subtotal
  - target.paramsJackTargets.params[] : the persistable Parameter subset (regenerated)

The machine registry (lunar24.json) is NOT a source; the manifest stays an independent
target for the completion gate. Endpoints/fixedRoutes/modules/programs/normalizedRoutes
are preserved; lfo cv_out evidence is re-pointed to the manual (Codex item #4).

Usage:  python3 tools/p0_regions_build.py   (rewrites the manifest in place)
"""

import json
import os

REF = "solar42N_manual_v15"
MANIFEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "spec", "machine", "p0_inventory_manifest.json")

K_CONT = "continuous"
K_SEL = "selector-toggle"
K_MOM = "momentary-touch-event"


def ev(line_start, line_end=None, site=None):
    e = {"ref": REF, "lineStart": line_start}
    if line_end is not None:
        e["lineEnd"] = line_end
    if site is not None:
        e["panelSite"] = site
    return e


def _rows(owner, region, items, evidence):
    """items: list of (name, kind, persistable, count, status)."""
    out = []
    for name, kind, persistable, count, status in items:
        for i in range(1, count + 1):
            sid = "%s.%s_%d" % (owner, name, i) if count > 1 else "%s.%s" % (owner, name)
            out.append({"stable_id": sid, "owner": owner, "kind": kind,
                        "persistable": persistable, "region": region,
                        "status": status, "evidence": evidence})
    return out


def build_controls():
    controls = []
    regions = []        # (rid, module, panelSite, evidence, count)

    # CLASSIC drones 1/2/4/5 : 5 generators x (TUNE/MUTE/MOD) + VOLT/ATT/RLS/GATE-HOLD.
    for mod, site in [("drone_1", "左上 DRONE 1/2"), ("drone_2", "左上 DRONE 1/2"),
                      ("drone_4", "右上 DRONE 4/5"), ("drone_5", "右上 DRONE 4/5")]:
        e = ev(294, 312, site)
        items = [("tune", K_CONT, True, 5, "confirmed"), ("mute", K_SEL, True, 5, "confirmed"),
                 ("mod", K_SEL, True, 5, "confirmed"), ("volt", K_CONT, True, 1, "confirmed"),
                 ("att", K_CONT, True, 1, "confirmed"), ("rls", K_CONT, True, 1, "confirmed"),
                 ("gate_hold", K_SEL, True, 1, "confirmed")]
        rid = "drone_classic_" + mod
        rows = _rows(mod, rid, items, e)
        controls += rows
        regions.append((rid, mod, site, e, len(rows)))

    # NEW drones 3/6 (Papa Srapa) : LFO rate/mod/divider/pitch + hi-low + noise +
    # S&H + fm/am + att/rls (4 pots + 4 switches per manual L343-366).
    for mod, site in [("drone_3", "左中 DRONE 3"), ("drone_6", "右中 DRONE 6")]:
        e = ev(322, 370, site)
        items = [("rate", K_CONT, True, 1, "confirmed"), ("mod", K_CONT, True, 1, "confirmed"),
                 ("divider", K_CONT, True, 1, "confirmed"), ("pitch", K_CONT, True, 1, "confirmed"),
                 ("hi_low", K_SEL, True, 1, "confirmed"), ("noise", K_CONT, True, 1, "confirmed"),
                 ("sh", K_SEL, True, 1, "provisional"), ("fm", K_SEL, True, 1, "confirmed"),
                 ("am", K_SEL, True, 1, "confirmed"), ("rate_switch", K_SEL, True, 1, "provisional"),
                 ("att", K_CONT, True, 1, "confirmed"), ("rls", K_CONT, True, 1, "confirmed")]
        rid = "drone_new_" + mod
        rows = _rows(mod, rid, items, e)
        controls += rows
        regions.append((rid, mod, site, e, len(rows)))

    # VCO A / VCO B (mirror; A has SYNC, B has VCO OUT normalised to CV in).
    for mod, site in [("vco_a", "中上 VCO A/B"), ("vco_b", "中上 VCO A/B")]:
        e = ev(376, 422, site)
        items = [("tune", K_CONT, True, 1, "confirmed"), ("wave", K_SEL, True, 1, "confirmed"),
                 ("shape", K_CONT, True, 1, "confirmed"), ("pw", K_CONT, True, 1, "confirmed"),
                 ("pwm_att", K_CONT, True, 1, "provisional"), ("fm_att", K_CONT, True, 1, "confirmed"),
                 ("lin_exp", K_SEL, True, 1, "confirmed"), ("oct_up", K_SEL, True, 1, "confirmed"),
                 ("oct_low", K_SEL, True, 1, "confirmed"), ("sub", K_SEL, True, 1, "confirmed"),
                 ("cv_amt", K_CONT, True, 1, "confirmed")]
        rows = _rows(mod, mod, items, e)
        controls += rows
        regions.append((mod, mod, site, e, len(rows)))

    # ENVELOPE A / B : HOLD + ADSR.
    for mod, site in [("envelope_a", "中部 ENV A/B"), ("envelope_b", "中部 ENV A/B")]:
        e = ev(385, 408, site)
        items = [("hold", K_SEL, True, 1, "confirmed"), ("a", K_CONT, True, 1, "confirmed"),
                 ("d", K_CONT, True, 1, "confirmed"), ("s", K_CONT, True, 1, "confirmed"),
                 ("r", K_CONT, True, 1, "confirmed")]
        rows = _rows(mod, mod, items, e)
        controls += rows
        regions.append((mod, mod, site, e, len(rows)))

    # VOICE MIXER : 10 channels, each PAN + VOL.
    e = ev(1105, 1113, "中部 VOICE MIXER")
    mix_chan = ["drone1", "drone2", "drone3", "ext_audio", "vco_a",
                "vco_b", "preamp", "drone4", "drone5", "drone6"]
    rows = []
    for i, ch in enumerate(mix_chan, 1):
        for knob in ("pan", "vol"):
            rows.append({"stable_id": "mixer.ch%d_%s" % (i, knob), "owner": "mixer",
                         "kind": K_CONT, "persistable": True, "region": "mixer",
                         "status": "confirmed", "evidence": e})
    controls += rows
    regions.append(("mixer", "mixer", "中部 VOICE MIXER", e, len(rows)))

    # DUAL VCF (POLIVOKS) : per-side FREQ/RES/MOD + BP-LP; shared DIST/GAIN/LINK.
    e = ev(1118, 1153, "中上 DUAL VCF")
    items = [("l_freq", K_CONT, True, 1, "confirmed"), ("l_res", K_CONT, True, 1, "confirmed"),
             ("l_mod", K_CONT, True, 1, "confirmed"), ("l_bp_lp", K_SEL, True, 1, "confirmed"),
             ("r_freq", K_CONT, True, 1, "confirmed"), ("r_res", K_CONT, True, 1, "confirmed"),
             ("r_mod", K_CONT, True, 1, "confirmed"), ("r_bp_lp", K_SEL, True, 1, "confirmed"),
             ("dist", K_CONT, True, 1, "confirmed"), ("gain", K_CONT, True, 1, "confirmed"),
             ("link", K_SEL, True, 1, "confirmed")]
    rows = _rows("vcf", "dual_vcf", items, e)
    controls += rows
    regions.append(("dual_vcf", "vcf", "中上 DUAL VCF", e, len(rows)))

    # DUAL EFFECTOR : shared X/Y/Z + BLEND + MASTER VOLUME + per-side 1-2-3 + cartridge slots.
    e = ev(1159, 1199, "中上 DUAL EFFECTOR")
    items = [("x", K_CONT, True, 1, "confirmed"), ("y", K_CONT, True, 1, "confirmed"),
             ("z", K_CONT, True, 1, "confirmed"), ("blend", K_CONT, True, 1, "confirmed"),
             ("master_volume", K_CONT, True, 1, "confirmed"), ("select_l", K_SEL, True, 1, "confirmed"),
             ("select_r", K_SEL, True, 1, "confirmed"), ("cartridge_slot_l", K_SEL, True, 1, "confirmed"),
             ("cartridge_slot_r", K_SEL, True, 1, "confirmed")]
    rows = _rows("effector", "dual_effector", items, e)
    controls += rows
    regions.append(("dual_effector", "effector", "中上 DUAL EFFECTOR", e, len(rows)))

    # LFO A / LFO B : wave + rate + x1/x6/x10.
    for mod, site in [("lfo_a", "下排 LFO A"), ("lfo_b", "下排 LFO B")]:
        e = ev(425, 442, site)
        items = [("wave", K_CONT, True, 1, "confirmed"), ("rate", K_CONT, True, 1, "confirmed"),
                 ("speed_mult", K_SEL, True, 1, "confirmed")]
        rows = _rows(mod, mod, items, e)
        controls += rows
        regions.append((mod, mod, site, e, len(rows)))

    # 5 STEP SEQ : pulser + clock + stages + step CV/gate x5.
    e = ev(766, 775, "下排 5-step seq")
    items = [("pulser", K_SEL, True, 1, "confirmed"), ("clock", K_SEL, True, 1, "confirmed"),
             ("stages", K_SEL, True, 1, "confirmed"), ("step_cv", K_CONT, True, 5, "confirmed"),
             ("step_gate", K_SEL, True, 5, "confirmed")]
    rows = _rows("sequencer", "seq", items, e)
    controls += rows
    regions.append(("seq", "sequencer", "下排 5-step seq", e, len(rows)))

    # JOYSTICK : X + Y + two offset regulators.
    e = ev(446, 456, "下排 joystick")
    items = [("x", K_CONT, True, 1, "confirmed"), ("y", K_CONT, True, 1, "confirmed"),
             ("offset_x", K_CONT, True, 1, "confirmed"), ("offset_y", K_CONT, True, 1, "confirmed")]
    rows = _rows("joystick", "joystick", items, e)
    controls += rows
    regions.append(("joystick", "joystick", "下排 joystick", e, len(rows)))

    # PREAMP : GAIN only (EXT SOURCE is a JACK; built-in mic auto-bypassed). FIX per Codex.
    e = ev(516, 555, "下排 preamp")
    rows = _rows("preamp", "preamp", [("gain", K_CONT, True, 1, "confirmed")], e)
    controls += rows
    regions.append(("preamp", "preamp", "下排 preamp", e, len(rows)))

    # ENVELOPE FOLLOWER : attack + release.
    e = ev(551, 553, "下排 env follower")
    rows = _rows("env_follower", "env_follower",
                 [("attack", K_CONT, True, 1, "confirmed"), ("release", K_CONT, True, 1, "confirmed")], e)
    controls += rows
    regions.append(("env_follower", "env_follower", "下排 env follower", e, len(rows)))

    # KEYBOARD physical : 12 touchplates + encoder. Plates = momentary touch, note value persists.
    e = ev(559, 654, "底部 12 触摸片")
    rows = _rows("keyboard", "keyboard_phys",
                 [("plate", K_MOM, True, 12, "confirmed"), ("encoder", K_MOM, True, 1, "confirmed")], e)
    controls += rows
    regions.append(("keyboard_phys", "keyboard", "底部 12 触摸片", e, len(rows)))

    # DRONE VOICES 1-6 buttons (module 'voices') : 6 momentary TRIGGERS, NOT persisted.
    e = ev(778, 788, "右下 DRONE VOICES")
    rows = _rows("voices", "drone_voices", [("button", K_MOM, False, 6, "confirmed")], e)
    controls += rows
    regions.append(("drone_voices", "voices", "右下 DRONE VOICES", e, len(rows)))

    return controls, regions


# Keyboard multi-function params (36 semi-digital menu params; kinds classified by the
# manual's control semantics — enumerations are selector-toggle, value-style continuous).
# Embedded as a constant so the tool is deterministic and idempotent — it never re-reads
# the self-regenerated params[] (which would double-add the physical plates/encoder).
# (name, kind, lineStart, lineEnd)
KEYBOARD_PARAM_ROWS = [
    ("behaviour", K_SEL, 659, 733), ("mode", K_SEL, 809, 818),
    ("arp_hold", K_SEL, 822, 857), ("arp_clock", K_SEL, 822, 857),
    ("arp_direction", K_SEL, 822, 857), ("arp_variation", K_SEL, 822, 857),
    ("arp_interval", K_CONT, 822, 857), ("arp_rhythm", K_SEL, 822, 857),
    ("arp_length", K_CONT, 822, 857),
    ("seq_run", K_SEL, 866, 910), ("seq_length", K_CONT, 866, 910),
    ("seq_clock", K_SEL, 866, 910), ("seq_direction", K_SEL, 866, 910),
    ("seq_editor", K_SEL, 866, 910), ("seq_cv_output", K_SEL, 866, 910),
    ("seq_rhythm", K_SEL, 866, 910),
    ("portamento_speed", K_CONT, 916, 923), ("portamento_legato", K_SEL, 916, 923),
    ("vibrato_speed", K_CONT, 934, 946), ("vibrato_depth", K_CONT, 934, 946),
    ("vibrato_delay", K_CONT, 934, 946), ("vibrato_pressure", K_CONT, 934, 946),
    ("pressure_output", K_SEL, 951, 965), ("pressure_rise", K_CONT, 951, 965),
    ("pressure_fall", K_CONT, 951, 965),
    ("quantise_scale_editor", K_SEL, 970, 984), ("quantise_load_scale", K_SEL, 970, 984),
    ("root_note", K_CONT, 992, 993), ("clock_bpm", K_CONT, 1028, 1029),
    ("preset_a", K_SEL, 1040, 1045), ("preset_b", K_SEL, 1040, 1045),
    ("preset_c", K_SEL, 1040, 1045), ("preset_d", K_SEL, 1040, 1045),
    ("calibration_v_oct", K_CONT, 1050, 1093), ("calibration_pressure", K_CONT, 1050, 1093),
    ("encoder_direction", K_SEL, 1050, 1093),
]


def keyboard_params(controls, regions):
    """Add the 36 keyboard multi-function params as controls (deterministic constant)."""
    kb_site = "底部 12 触摸片"
    for name, kind, ls, le in KEYBOARD_PARAM_ROWS:
        controls.append({"stable_id": "keyboard.%s" % name, "owner": "keyboard",
                         "kind": kind, "persistable": True, "region": "keyboard_params",
                         "status": "confirmed", "evidence": ev(ls, le, kb_site)})
    regions.append(("keyboard_params", "keyboard", kb_site,
                    ev(559, 654, kb_site), len(KEYBOARD_PARAM_ROWS)))


def program_xyz(manifest, controls, regions):
    """117 program-owned X/Y/Z controls (39 programs x 3). Evidence = p23/p24."""
    rows = []
    for p in manifest["target"]["programs"]:
        own = p["stable_id"]
        e = {"ref": REF, "lineStart": 1207, "lineEnd": 1309, "panelSite": "p23/p24"}
        for name in ("x", "y", "z"):
            rows.append({"stable_id": "%s.%s" % (own, name), "owner": own, "kind": K_CONT,
                         "persistable": True, "region": "program_params",
                         "status": p.get("status", "provisional"), "evidence": e})
    controls += rows
    regions.append(("program_params", "effector", "p23/p24",
                    {"ref": REF, "lineStart": 1207, "lineEnd": 1309}, len(rows)))


def _compact(o):
    return json.dumps(o, ensure_ascii=False)


def _list_lines(o, indent):
    """Emit a list the way the hand-crafted manifest does: one element per line.

    Returns a list of lines whose first line is the opening '[' and that are
    already indented for `indent`. Keeps the diff to real content changes instead
    of a whole-file re-format from json.dump(indent=2).
    """
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
    """Compact serializer: dicts multi-line, array elements one compact line each."""
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
                sub[-1] = sub[-1] + comma          # comma on the value's closing brace
                lines.append(pad + "  " + kk + ": " + sub[0][len(pad) + 2:])
                lines.extend(sub[1:])
            elif isinstance(v, list):
                sub = _list_lines(v, indent + 1)
                sub[-1] = sub[-1] + comma          # comma on the value's closing bracket
                lines.append(pad + "  " + kk + ": " + sub[0])
                lines.extend(sub[1:])
            else:
                lines.append(pad + "  " + kk + ": " + _compact(v) + comma)
        lines.append(pad + "}")
        return lines

    return "\n".join(dict_lines(m, 0)) + "\n"


def main():
    with open(MANIFEST, encoding="utf-8") as fh:
        m = json.load(fh)

    controls, regions = build_controls()
    keyboard_params(controls, regions)
    program_xyz(m, controls, regions)

    # fixed lfo cv_out evidence -> manual LFO section (Codex item #4).
    for ep in m["target"]["paramsJackTargets"]["endpoints"]:
        if ep.get("stable_id") in ("lfo_a.cv_out", "lfo_b.cv_out"):
            ep["evidence"] = ev(427, 442)
            ep["status"] = "confirmed"

    m["target"]["controls"] = controls
    m["target"]["controlRegions"] = [
        {"id": rid, "module": mod, "panelSite": site, "count": cnt,
         "status": "confirmed", "evidence": e}
        for rid, mod, site, e, cnt in regions
    ]
    # Parameter == persistable subset of controls[]. (voices triggers are not persisted.)
    m["target"]["paramsJackTargets"]["params"] = [c for c in controls if c["persistable"]]
    m["target"]["paramsJackTargets"]["note"] = (
        "Per-control independent transcription. controls[] = every visible/operable item "
        "(continuous | selector-toggle | momentary-touch-event); params[] = the persistable "
        "Parameter subset of controls[]. controlRegions[] declares each panel region's subtotal; "
        "the completion gate asserts region subtotal == inventory. Fixed internal endpoints are "
        "independent of patchable jacks.")

    with open(MANIFEST, "w", encoding="utf-8") as fh:
        fh.write(_dump(m))

    print("controls:", len(m["target"]["controls"]),
          "params(persistable):", len(m["target"]["paramsJackTargets"]["params"]),
          "regions:", len(m["target"]["controlRegions"]))
    for r in m["target"]["controlRegions"]:
        print("  region %-18s module=%-12s count=%d" % (r["id"], r["module"], r["count"]))


if __name__ == "__main__":
    main()
