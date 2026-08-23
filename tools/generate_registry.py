#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the Lunar 24 machine-registry C++ headers from spec/machine/lunar24.json.

Stdlib only (no third-party deps). Emits two committed headers under generated/:

  generated/lunar24/registry_ids.hpp  — stable id enums + counts + id-string lookups
  generated/lunar24/registry.hpp      — constexpr descriptor arrays (core descriptor structs)

Usage:
  python3 tools/generate_registry.py            # write + validate
  python3 tools/generate_registry.py --check    # fail if output differs from disk (CTest)
  python3 tools/generate_registry.py --validate <path>  # validate only; non-zero on invalid

Identity is order-independent: every serialized entity carries an explicit,
immutable numeric `id` in the spec. Enum values are those explicit numbers, NOT an
index derived from JSON order — so reordering (or inserting) entries never renumbers
serialized ids. This generator is the authoritative schema validator, enforcing
(and CTest verifies once per run) that output matches disk:
  * every entity has an explicit numeric id, unique per kind, and >= 0
  * every stable id unique, verbatim and after C++-identifier sanitising
  * every ParameterId owner (module or program) exists
  * every JackId endpoint in a NormalizedRoute exists; a sink is the target of <=1 route
  * min <= max, default in [min, max], step >= 0
  * every descriptor carries an EvidenceRef and an EvidenceStatus
"""

import difflib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC_PATH = os.path.join(ROOT, "spec", "machine", "lunar24.json")
OUT_DIR = os.path.join(ROOT, "generated", "lunar24")
IDS_HPP = os.path.join(OUT_DIR, "registry_ids.hpp")
REG_HPP = os.path.join(OUT_DIR, "registry.hpp")

DEFAULT_SOURCE = "solar42N_manual_v15"

SPDX_HEAD = (
    "// GENERATED FILE - DO NOT EDIT. Regenerate with tools/generate_registry.py.\n"
    "// Source: spec/machine/lunar24.json\n"
    "// Copyright (c) 2026 Lunar 24 contributors\n"
    "// SPDX-License-Identifier: Apache-2.0\n"
)

VALID_STATUS = ("confirmed", "unverified", "provisional")


def sanitize(ident):
    s = re.sub(r"[^A-Za-z0-9_]", "_", ident)
    if not s or not (s[0].isalpha() or s[0] == "_"):
        s = "_" + s
    return s


def literal(value):
    """Emit a double literal that is valid even for whole-number values."""
    f = float(value)
    if f.is_integer():
        return str(int(f))
    return repr(f)


# ----------------------------------------------------------------------------
# Registry: load + validate
# ----------------------------------------------------------------------------

class Registry:
    def __init__(self, spec):
        self.spec = spec
        self.modules = spec["modules"]
        self.routes = spec["normalizedRoutes"]
        self.programs = spec["programs"]

        self.parameters = []   # module params then program params
        self.jacks = []        # by module order
        self._flatten()
        self._ids()
        self._validate()

    def _flatten(self):
        for m in self.modules:
            for p in m["parameters"]:
                p = dict(p); p["owner"] = m["id"]; p["_stable_owner"] = m["stable_id"] \
                    if "stable_id" in m else m.get("id_string", m["id"])
                self.parameters.append(p)
            for j in m["jacks"]:
                j = dict(j); j["module"] = m["id"]; j["_module_stable"] = m.get("stable_id", m["id"])
                self.jacks.append(j)
        for prog in self.programs:
            for p in prog.get("parameters", []):
                p = dict(p); p["owner"] = prog["id"]; p["_stable_owner"] = prog.get("stable_id", prog["id"])
                self.parameters.append(p)

    def _ids(self):
        # present a stable string id for each entity so a reorder-regression may key on it.
        self.module_ids = [(m["id"], m["stable_id"], i) for i, m in enumerate(self.modules)]
        self.program_ids = [(p["id"], p["stable_id"], i) for i, p in enumerate(self.programs)]
        self.parameter_ids = [(p["id"], p["stable_id"], i) for i, p in enumerate(self.parameters)]
        self.jack_ids = [(j["id"], j["stable_id"], i) for i, j in enumerate(self.jacks)]
        self.route_ids = [(r.get("id", i), r["stable_id"], i) for i, r in enumerate(self.routes)]

    def _validate(self):
        # --- explicit numeric ids: present, integer, unique per kind, uint32-range ---
        # The id IS the persisted numeric value, so it must be expressible in the
        # u32 wire type (order-independent identity, design/07 §7). Both under- and
        # over-flow are rejected.
        for label, group in (("module", self.modules), ("program", self.programs),
                             ("parameter", self.parameters), ("jack", self.jacks),
                             ("route", self.routes)):
            seen = set()
            for item in group:
                nid = item.get("id", None)
                if not isinstance(nid, int) or nid < 0 or nid > 0xFFFFFFFF:
                    raise ValueError(f"{label} {item.get('stable_id', '?')} is missing a numeric id "
                                     f"in [0, 0xFFFFFFFF] (got {nid!r})")
                if nid in seen:
                    raise ValueError(f"{label} numeric id {nid} is reused")
                seen.add(nid)

        # enumerator collisions after C++-identifier sanitising (stable ids)
        for label, group, key in (("modules", self.modules, "stable_id"),
                                   ("programs", self.programs, "stable_id"),
                                   ("parameters", self.parameters, "stable_id"),
                                   ("jacks", self.jacks, "stable_id"),
                                   ("routes", self.routes, "stable_id")):
            seen = {}
            for item in group:
                en = sanitize(item[key])
                if en in seen:
                    raise ValueError(f"enumerator collision {en!r} in {label}")
                seen[en] = True

        # stable-id uniqueness across every entity kind
        all_ids = ([m["stable_id"] for m in self.modules] + [p["stable_id"] for p in self.programs] +
                   [p["stable_id"] for p in self.parameters] + [j["stable_id"] for j in self.jacks] +
                   [r["stable_id"] for r in self.routes])
        dup = {s for s in set(all_ids) if all_ids.count(s) > 1}
        if dup:
            raise ValueError(f"duplicate stable ids: {sorted(dup)}")

        # owner existence
        known_modules = {m["stable_id"] for m in self.modules}
        known_programs = {p["stable_id"] for p in self.programs}
        for p in self.parameters:
            if p["_stable_owner"] not in known_modules and p["_stable_owner"] not in known_programs:
                raise ValueError(f"parameter {p['stable_id']} unknown owner {p['_stable_owner']!r}")

        for p in self.parameters:
            lo, hi, dflt = float(p["min"]), float(p["max"]), float(p["default"])
            step = float(p.get("step", 0.0))
            if lo > hi:
                raise ValueError(f"parameter {p['stable_id']} min>max ({lo}>{hi})")
            if not (lo <= dflt <= hi):
                raise ValueError(f"parameter {p['stable_id']} default {dflt} outside [{lo},{hi}]")
            if step < 0:
                raise ValueError(f"parameter {p['stable_id']} negative step")

        known_jacks = {j["stable_id"] for j in self.jacks}
        sink_seen = set()
        for r in self.routes:
            for f in ("sourceJack", "sinkJack"):
                if r[f] not in known_jacks:
                    raise ValueError(f"route {r['stable_id']} dangling {f}={r[f]!r}")
            if r["sinkJack"] in sink_seen:
                raise ValueError(f"jack {r['sinkJack']!r} is the sink of >1 normalized route")
            sink_seen.add(r["sinkJack"])

        for label, group in (("module", self.modules), ("parameter", self.parameters),
                             ("jack", self.jacks), ("route", self.routes),
                             ("program", self.programs)):
            for item in group:
                if not item.get("evidence"):
                    raise ValueError(f"{label} {item['stable_id']} has no evidence")
                if item.get("status", "") not in VALID_STATUS:
                    raise ValueError(f"{label} {item['stable_id']} bad/absent status {item.get('status')!r}")
                if label == "parameter" and item.get("rangeEvidence", "unverified") not in VALID_STATUS:
                    raise ValueError(f"parameter {item['stable_id']} bad rangeEvidence")
                if label == "parameter":
                    # Selector positions are structural (the generated option table is built from
                    # them), so a malformed positions[] is a hard spec error here. fieldEvidence
                    # STATUS is policy, not structure — the completeness gate reports it as a
                    # problem rather than aborting the whole registry (Codex 03848819 Root 1/2),
                    # and generation still rejects an unknown status via status_from() during
                    # emission, so a bad status cannot survive a real build.
                    poss = item.get("positions")
                    if poss is not None and (not isinstance(poss, list) or not poss or
                                             not all(isinstance(x, str) and x for x in poss)):
                        raise ValueError(f"parameter {item['stable_id']} bad positions "
                                         f"(need a non-empty list of non-empty labels)")
                if label == "jack":
                    fe = item.get("fieldEvidence", {})
                    if not all(fe.get(k, "unverified") in VALID_STATUS
                               for k in ("nominalRange", "toleratedRange",
                                         "threshold", "saturation", "transfer")):
                        raise ValueError(f"jack {item['stable_id']} bad fieldEvidence")


# ----------------------------------------------------------------------------
# C++ expression builders
# ----------------------------------------------------------------------------

def evidence_expr(item, src):
    ev = item.get("evidence", {})
    s = ev.get("ref", src)
    ls = ev.get("lineStart", ev.get("line", 0))
    le = ev.get("lineEnd", ev.get("line", 0))
    return f'EvidenceRef{{"{s}", {ls}u, {le}u}}'


def status_expr(item):
    return {"confirmed": "EvidenceStatus::confirmed",
            "unverified": "EvidenceStatus::unverified",
            "provisional": "EvidenceStatus::provisional"}[item["status"]]


def status_from(s):
    return {"confirmed": "EvidenceStatus::confirmed",
            "unverified": "EvidenceStatus::unverified",
            "provisional": "EvidenceStatus::provisional"}[s]


def smoothing_expr(p):
    return {"none": "Smoothing::none", "linear": "Smoothing::linear",
            "seconds": "Smoothing::seconds"}[p["smoothing"]]


def persistence_expr(p):
    return {"transient": "Persistence::transient", "preset": "Persistence::preset",
            "deviceState": "Persistence::deviceState"}[p["persistence"]]


def role_expr(p):
    return {"x": "ParamRole::x", "y": "ParamRole::y", "z": "ParamRole::z"}.get(
        p.get("role", "knob"), "ParamRole::knob")


def signal_type_expr(j):
    return {"audio": "SignalType::audio", "cv": "SignalType::cv",
            "gate": "SignalType::gate", "clock": "SignalType::clock"}[j["signalType"]]


def polarity_expr(j):
    return {"unipolar": "Polarity::unipolar", "bipolar": "Polarity::bipolar"}[j["polarity"]]


def coupling_expr(j):
    return {"ac": "Coupling::ac", "dc": "Coupling::dc"}[j["coupling"]]


def direction_expr(j):
    return {"input": "PinDirection::input", "output": "PinDirection::output"}[j["direction"]]


def transfer_expr(j):
    return {"linear": "SignalTransfer::linear", "exponential": "SignalTransfer::exponential",
            "none": "SignalTransfer::none", "unknown": "SignalTransfer::unknown"}[j.get("transfer", "unknown")]


def saturation_expr(j):
    return {"none": "SaturationType::none", "hard": "SaturationType::hard",
            "soft": "SaturationType::soft", "unknown": "SaturationType::unknown"}[j.get("saturation", "unknown")]


def field_evidence_expr(j):
    fe = j.get("fieldEvidence", {})
    nr = status_from(fe.get("nominalRange", "unverified"))
    tr = status_from(fe.get("toleratedRange", "unverified"))
    t = status_from(fe.get("threshold", "unverified"))
    s = status_from(fe.get("saturation", "unverified"))
    xf = status_from(fe.get("transfer", "unverified"))
    return f"FieldEvidence{{{nr}, {tr}, {t}, {s}, {xf}}}"


def parameter_field_evidence_expr(p):
    # ParameterFieldEvidence aggregates in field order: range, unit, initial(default),
    # step, smoothing, persistence (Codex 03848819 Root 1). The JSON `fieldEvidence`
    # keys are range/unit/default/step/smoothing/persistence; `default` maps to `initial`
    # in the C++ struct (a C++ keyword cannot be a member name).
    fe = p.get("fieldEvidence", {})
    r = status_from(fe.get("range", p.get("rangeEvidence", "unverified")))
    u = status_from(fe.get("unit", "unverified"))
    i = status_from(fe.get("default", "unverified"))
    s = status_from(fe.get("step", "unverified"))
    sm = status_from(fe.get("smoothing", "unverified"))
    pe = status_from(fe.get("persistence", "unverified"))
    return f"ParameterFieldEvidence{{{r}, {u}, {i}, {s}, {sm}, {pe}}}"


def option_table(reg):
    """Flatten all selector positions into one shared label table + per-param offsets."""
    labels = []
    offsets = {}
    for p in reg.parameters:
        pos = p.get("positions")
        if pos:
            offsets[p["stable_id"]] = len(labels)
            labels.extend(pos)
    return labels, offsets


# ----------------------------------------------------------------------------
# emit registry_ids.hpp
# ----------------------------------------------------------------------------

def _id_space(pairs):
    """One-past-the-last serialized id, computed in Python (arbitrary precision)
    so it can never overflow a u32 at the host: the emitted constant is a literal,
    not a runtime `max+1`. An empty kind yields 0 (no bank to size against)."""
    if not pairs:
        return 0
    return max(nid for nid, _, _ in pairs) + 1


def gen_ids(reg):
    src = reg.spec["meta"].get("source", DEFAULT_SOURCE)
    out = [SPDX_HEAD, "#pragma once", "", "#include <lunar24/core/device_capacities.h>",
           "#include <lunar24/core/id_types.h>",
           "#include <cstdint>", "#include <string_view>", "",
           "namespace lunar24::core {", ""]

    def enum_block(kind, pairs):
        body = ",\n".join(f"    {sanitize(sid)} = {nid}" for nid, sid, _ in pairs)
        out.append(f"enum class {kind}Id : std::uint32_t {{\n{body}\n}};\n")

    enum_block("Module", reg.module_ids)
    enum_block("Parameter", reg.parameter_ids)
    enum_block("Jack", reg.jack_ids)
    enum_block("Program", reg.program_ids)
    enum_block("Route", reg.route_ids)

    out.append("inline constexpr std::uint32_t kModuleCount = %d;" % len(reg.modules))
    out.append("inline constexpr std::uint32_t kParameterCount = %d;" % len(reg.parameters))
    out.append("inline constexpr std::uint32_t kJackCount = %d;" % len(reg.jacks))
    out.append("inline constexpr std::uint32_t kProgramCount = %d;" % len(reg.programs))
    out.append("inline constexpr std::uint32_t kRouteCount = %d;" % len(reg.routes))
    out.append("")

    # Serialized id SPACE (one-past-the-last id). A state/storage bank is indexed
    # by these ids, so the count alone is not enough: a sparse id (a hole between
    # two ids) makes the id-space larger than the count and must still be covered.
    # id-space is a literal, not a runtime max+1, so it can never overflow at the
    # host — but one-past of the MAX allowed u32 id (0xFFFFFFFF) is 0x100000000,
    # which does not fit a uint32_t. Emitted as uint64_t so the full u32 id domain
    # stays representable; the capacity static_assert still rejects an id-space
    # that outruns its bank, now as a clear compile error rather than a malformed
    # literal.
    out.append("inline constexpr std::uint64_t kModuleIdSpace = %d;" % _id_space(reg.module_ids))
    out.append("inline constexpr std::uint64_t kParameterIdSpace = %d;" % _id_space(reg.parameter_ids))
    out.append("inline constexpr std::uint64_t kJackIdSpace = %d;" % _id_space(reg.jack_ids))
    out.append("inline constexpr std::uint64_t kProgramIdSpace = %d;" % _id_space(reg.program_ids))
    out.append("inline constexpr std::uint64_t kRouteIdSpace = %d;" % _id_space(reg.route_ids))
    out.append("")

    # Compile-time gate: every state bank must be large enough for the ids it is
    # indexed by. If a spec id goes sparse beyond the bank, this fails to compile —
    # the "sparse id" hazard is caught here, not at a later runtime bank access.
    out.append("static_assert(kDeviceParamCapacity >= kParameterIdSpace, \"parameter bank too small for ParameterId space\");")
    out.append("static_assert(kDevicePatchCapacity >= kJackIdSpace, \"patch bank too small for JackId space\");")
    out.append("static_assert(kDeviceRouteCapacity >= kRouteIdSpace, \"route bank too small for RouteId space\");")
    out.append("")

    def strs(kind, items, pairs):
        out.append(f"inline constexpr std::string_view {kind.lower()}_id_string({kind}Id id) {{")
        out.append("  switch (id) {")
        for nid, sid, _ in pairs:
            out.append(f"    case {kind}Id::{sanitize(sid)}: return \"{sid}\";")
        out.append("  }")
        out.append(f"  return \"(unknown {kind})\";")
        out.append("}\n")

    strs("Module", reg.modules, reg.module_ids)
    strs("Parameter", reg.parameters, reg.parameter_ids)
    strs("Jack", reg.jacks, reg.jack_ids)
    strs("Program", reg.programs, reg.program_ids)
    strs("Route", reg.routes, reg.route_ids)

    out.append("}  // namespace lunar24::core")
    out.append("")
    return "\n".join(out)


# ----------------------------------------------------------------------------
# emit registry.hpp  (exact aggregate -> struct field order)
# ----------------------------------------------------------------------------

def gen_registry(reg):
    src = reg.spec["meta"].get("source", DEFAULT_SOURCE)
    out = [SPDX_HEAD, "#pragma once", "", "#include <lunar24/core/descriptors.h>",
           "#include <lunar24/registry_ids.hpp>", "",
           "namespace lunar24::registry {", "using namespace lunar24::core;", ""]

    def qs(s):
        return '"' + s.replace('"', "'") + '"'

    out.append("inline constexpr ModuleDescriptor kModules[kModuleCount] = {")
    for i, m in enumerate(reg.modules):
        pb = sum(len(x["parameters"]) for x in reg.modules[:i])
        jb = sum(len(x["jacks"]) for x in reg.modules[:i])
        out.append("  { ModuleId::%s, %s, %s, %s, %s, %du, %du, %du, %du, %s, %s }," % (
            sanitize(m["stable_id"]), qs(m["stable_id"]), qs(m["name"]), qs(m["category"]),
            qs(m["description"]), pb, len(m["parameters"]), jb, len(m["jacks"]),
            evidence_expr(m, src), status_expr(m)))
    out.append("};\n")

    # Shared selector-option label table. Every selector parameter's `options` pointer
    # points at its slice of this array; `kParameterOptionLabelCount` lets the C++ test
    # round-trip the whole table (each label counted exactly once, no overlap).
    option_labels, option_offsets = option_table(reg)
    out.append("inline constexpr const char* kParameterOptionLabels[] = {")
    for lab in option_labels:
        out.append("  %s," % qs(lab))
    out.append("};\n")
    out.append("inline constexpr std::uint32_t kParameterOptionLabelCount = %d;" % len(option_labels))
    out.append("")

    out.append("inline constexpr ParameterDescriptor kParameters[kParameterCount] = {")
    for p in reg.parameters:
        pos = p.get("positions")
        if pos:
            oc = len(pos)
            op = "&kParameterOptionLabels[%d]" % option_offsets[p["stable_id"]]
        else:
            oc, op = 0, "nullptr"
        out.append("  { ParameterId::%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %du, %s }," % (
            sanitize(p["stable_id"]), qs(p["stable_id"]), qs(p["name"]), qs(p["_stable_owner"]), qs(p["unit"]),
            literal(p["min"]), literal(p["max"]), literal(p.get("step", 0.0)), literal(p["default"]),
            smoothing_expr(p), persistence_expr(p), role_expr(p),
            evidence_expr(p, src), status_expr(p), parameter_field_evidence_expr(p), oc, op))
    out.append("};\n")

    out.append("inline constexpr JackDescriptor kJacks[kJackCount] = {")
    for j in reg.jacks:
        out.append("  { JackId::%s, %s, %s, ModuleId::%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %du, %s, %s, %s, %s, %s, %s }," % (
            sanitize(j["stable_id"]), qs(j["stable_id"]), qs(j["name"]), sanitize(j["_module_stable"]),
            direction_expr(j), signal_type_expr(j), polarity_expr(j),
            literal(j["nominalMin"]), literal(j["nominalMax"]),
            literal(j.get("toleratedMin", 0.0)), literal(j.get("toleratedMax", 0.0)),
            literal(j.get("modulationDepthPerVolt", 1.0)), transfer_expr(j), saturation_expr(j),
            int(j.get("maxCables", 1)),
            literal(j.get("gateThresholdVolts", 0.0)), literal(j.get("hysteresisVolts", 0.0)),
            coupling_expr(j), evidence_expr(j, src), status_expr(j), field_evidence_expr(j)))
    out.append("};\n")

    out.append("inline constexpr NormalizedRoute kNormalizedRoutes[%d] = {" % len(reg.routes))
    for r in reg.routes:
        out.append("  { RouteId::%s, %s, JackId::%s, JackId::%s, %s, %s, %s }," % (
            sanitize(r["stable_id"]), qs(r["stable_id"]), sanitize(r["sourceJack"]),
            sanitize(r["sinkJack"]), qs(r.get("description", "")),
            evidence_expr(r, src), status_expr(r)))
    out.append("};\n")

    out.append("inline constexpr ProgramDescriptor kPrograms[kProgramCount] = {")
    for i, prog in enumerate(reg.programs):
        pb = (sum(len(x["parameters"]) for x in reg.modules) +
              sum(len(p.get("parameters", [])) for p in reg.programs[:i]))
        out.append("  { ProgramId::%s, %s, %s, %du, %s, %s, %s, %du, %du, %s, %s }," % (
            sanitize(prog["stable_id"]), qs(prog["stable_id"]), qs(prog["cartridge"]), int(prog["slot"]),
            qs(prog["name"]), qs(prog["family"]),
            "true" if prog.get("selfOscillating") else "false",
            pb, len(prog.get("parameters", [])), evidence_expr(prog, src), status_expr(prog)))
    out.append("};\n")

    out.append("}  // namespace lunar24::registry")
    out.append("")
    return "\n".join(out)


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def main():
    args = sys.argv[1:]

    # Usage:
    #   generate_registry.py                       write + validate (default spec)
    #   generate_registry.py --check               compare generated output vs disk, no write
    #   generate_registry.py --validate <path>     validate only (no write); non-zero on invalid
    if args and args[0] == "--validate":
        if len(args) != 2:
            raise SystemExit("usage: generate_registry.py --validate <spec.json>")
        with open(args[1], "r", encoding="utf-8") as fh:
            spec = json.load(fh)
        Registry(spec)
        print(f"VALID: {args[1]}")
        return 0

    check = "--check" in args
    if len([a for a in args if not a.startswith("--")]) > 0:
        raise SystemExit("usage: generate_registry.py [--check]")

    with open(SPEC_PATH, "r", encoding="utf-8") as fh:
        spec = json.load(fh)
    reg = Registry(spec)

    ids_text = gen_ids(reg)
    reg_text = gen_registry(reg)
    os.makedirs(OUT_DIR, exist_ok=True)

    if check:
        d1 = _diff(IDS_HPP, ids_text)
        d2 = _diff(REG_HPP, reg_text)
        if d1 or d2:
            sys.stderr.write("REGENERATION MISMATCH (run generate_registry.py):\n")
            sys.stderr.write(d1 + d2)
            return 1
        return 0

    with open(IDS_HPP, "w", encoding="utf-8") as fh:
        fh.write(ids_text)
    with open(REG_HPP, "w", encoding="utf-8") as fh:
        fh.write(reg_text)
    print(f"counts: {len(reg.modules)} modules, {len(reg.parameters)} params, "
          f"{len(reg.jacks)} jacks, {len(reg.routes)} routes, {len(reg.programs)} programs")
    print(f"wrote: {IDS_HPP}")
    print(f"wrote: {REG_HPP}")
    return 0


def _diff(path, text):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            existing = fh.read()
    except FileNotFoundError:
        return f"  {path}: missing\n"
    if existing == text:
        return ""
    return "".join(difflib.unified_diff(existing.splitlines(True), text.splitlines(True),
                                        fromfile=path, tofile="<regenerated>"))


if __name__ == "__main__":
    sys.exit(main())
