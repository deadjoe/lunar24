#!/usr/bin/env python3
"""Regenerate the Lunar 24 machine-registry C++ headers from spec/machine/lunar24.json.

Stdlib only (no third-party deps). Emits two committed headers under generated/:

  generated/lunar24/registry_ids.hpp  — stable id enums + counts + id-string lookups
  generated/lunar24/registry.hpp      — constexpr descriptor arrays (core descriptor structs)

Usage:
  python3 tools/generate_registry.py            # write + validate
  python3 tools/generate_registry.py --check    # fail if output differs from disk (CTest)

This generator is the authoritative schema validator. It enforces (and CTest verifies once
per run that output matches disk):
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
# semantic accessors (dead-simple, no enums needed here)
# ----------------------------------------------------------------------------

def elec(item):  # evidence/status presence helper
    return item


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
                p = dict(p); p["owner"] = m["id"]; self.parameters.append(p)
            for j in m["jacks"]:
                j = dict(j); j["module"] = m["id"]; self.jacks.append(j)
        for prog in self.programs:
            for p in prog.get("parameters", []):
                p = dict(p); p["owner"] = prog["id"]; self.parameters.append(p)

    def _ids(self):
        self.module_ids = [(sanitize(m["id"]), i) for i, m in enumerate(self.modules)]
        self.program_ids = [(sanitize(p["id"]), i) for i, p in enumerate(self.programs)]
        self.parameter_ids = [(sanitize(p["id"]), i) for i, p in enumerate(self.parameters)]
        self.jack_ids = [(sanitize(j["id"]), i) for i, j in enumerate(self.jacks)]

    def _validate(self):
        def dup_fail(group, label):
            seen = {}
            for item in group:
                en = sanitize(item["id"])
                if en in seen:
                    raise ValueError(f"enumerator collision {en!r} in {label}")
                seen[en] = True

        # stable id uniqueness (across every entity kind)
        all_ids = ([m["id"] for m in self.modules] + [p["id"] for p in self.programs] +
                   [p["id"] for p in self.parameters] + [j["id"] for j in self.jacks] +
                   [r["id"] for r in self.routes])
        dup = {s for s in set(all_ids) if all_ids.count(s) > 1}
        if dup:
            raise ValueError(f"duplicate stable ids: {sorted(dup)}")

        dup_fail(self.modules, "modules")
        dup_fail(self.programs, "programs")
        dup_fail(self.parameters, "parameters")
        dup_fail(self.jacks, "jacks")
        dup_fail(self.routes, "routes")

        known_modules = {m["id"] for m in self.modules}
        known_programs = {p["id"] for p in self.programs}
        for p in self.parameters:
            if p["owner"] not in known_modules and p["owner"] not in known_programs:
                raise ValueError(f"parameter {p['id']} unknown owner {p['owner']!r}")

        for p in self.parameters:
            lo, hi, dflt = float(p["min"]), float(p["max"]), float(p["default"])
            step = float(p.get("step", 0.0))
            if lo > hi:
                raise ValueError(f"parameter {p['id']} min>max ({lo}>{hi})")
            if not (lo <= dflt <= hi):
                raise ValueError(f"parameter {p['id']} default {dflt} outside [{lo},{hi}]")
            if step < 0:
                raise ValueError(f"parameter {p['id']} negative step")

        known_jacks = {j["id"] for j in self.jacks}
        sink_seen = set()
        for r in self.routes:
            for f in ("sourceJack", "sinkJack"):
                if r[f] not in known_jacks:
                    raise ValueError(f"route {r['id']} dangling {f}={r[f]!r}")
            if r["sinkJack"] in sink_seen:
                raise ValueError(f"jack {r['sinkJack']!r} is the sink of >1 normalized route")
            sink_seen.add(r["sinkJack"])

        for label, group in (("module", self.modules), ("parameter", self.parameters),
                             ("jack", self.jacks), ("route", self.routes),
                             ("program", self.programs)):
            for item in group:
                if not item.get("evidence"):
                    raise ValueError(f"{label} {item['id']} has no evidence")
                if item.get("status", "") not in VALID_STATUS:
                    raise ValueError(f"{label} {item['id']} bad/absent status {item.get('status')!r}")


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


# ----------------------------------------------------------------------------
# emit registry_ids.hpp
# ----------------------------------------------------------------------------

def gen_ids(reg):
    src = reg.spec["meta"].get("source", DEFAULT_SOURCE)
    out = [SPDX_HEAD, "#pragma once", "", "#include <lunar24/core/id_types.h>",
           "#include <cstdint>", "#include <string_view>", "",
           "namespace lunar24::core {", ""]

    def enum_block(kind, pairs):
        body = ",\n".join(f"    {en} = {i}" for en, i in pairs)
        out.append(f"enum class {kind}Id : std::uint32_t {{\n{body}\n}};\n")

    enum_block("Module", reg.module_ids)
    enum_block("Parameter", reg.parameter_ids)
    enum_block("Jack", reg.jack_ids)
    enum_block("Program", reg.program_ids)

    out.append("inline constexpr std::uint32_t kModuleCount = %d;" % len(reg.modules))
    out.append("inline constexpr std::uint32_t kParameterCount = %d;" % len(reg.parameters))
    out.append("inline constexpr std::uint32_t kJackCount = %d;" % len(reg.jacks))
    out.append("inline constexpr std::uint32_t kProgramCount = %d;" % len(reg.programs))
    out.append("")

    def strs(kind, items, pairs):
        out.append(f"inline constexpr std::string_view {kind.lower()}_id_string({kind}Id id) {{")
        out.append("  switch (id) {")
        for en, i in pairs:
            out.append(f"    case {kind}Id::{en}: return \"{items[i]['id']}\";")
        out.append("  }")
        out.append(f"  return \"(unknown {kind})\";")
        out.append("}\n")

    strs("Module", reg.modules, reg.module_ids)
    strs("Parameter", reg.parameters, reg.parameter_ids)
    strs("Jack", reg.jacks, reg.jack_ids)
    strs("Program", reg.programs, reg.program_ids)

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
            sanitize(m["id"]), qs(m["id"]), qs(m["name"]), qs(m["category"]),
            qs(m["description"]), pb, len(m["parameters"]), jb, len(m["jacks"]),
            evidence_expr(m, src), status_expr(m)))
    out.append("};\n")

    out.append("inline constexpr ParameterDescriptor kParameters[kParameterCount] = {")
    for p in reg.parameters:
        out.append("  { ParameterId::%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s }," % (
            sanitize(p["id"]), qs(p["id"]), qs(p["name"]), qs(p["owner"]), qs(p["unit"]),
            literal(p["min"]), literal(p["max"]), literal(p.get("step", 0.0)), literal(p["default"]),
            smoothing_expr(p), persistence_expr(p), role_expr(p),
            evidence_expr(p, src), status_expr(p)))
    out.append("};\n")

    out.append("inline constexpr JackDescriptor kJacks[kJackCount] = {")
    for j in reg.jacks:
        out.append("  { JackId::%s, %s, %s, ModuleId::%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s }," % (
            sanitize(j["id"]), qs(j["id"]), qs(j["name"]), sanitize(j["module"]),
            direction_expr(j), signal_type_expr(j), polarity_expr(j),
            literal(j["nominalMin"]), literal(j["nominalMax"]),
            literal(j.get("gateThresholdVolts", 0.0)), literal(j.get("hysteresisVolts", 0.0)),
            coupling_expr(j), evidence_expr(j, src), status_expr(j)))
    out.append("};\n")

    out.append("inline constexpr NormalizedRoute kNormalizedRoutes[%d] = {" % len(reg.routes))
    for r in reg.routes:
        out.append("  { %s, JackId::%s, JackId::%s, %s, %s, %s }," % (
            qs(r["id"]), sanitize(r["sourceJack"]), sanitize(r["sinkJack"]),
            qs(r.get("description", "")), evidence_expr(r, src), status_expr(r)))
    out.append("};\n")

    out.append("inline constexpr ProgramDescriptor kPrograms[kProgramCount] = {")
    for i, prog in enumerate(reg.programs):
        pb = (sum(len(x["parameters"]) for x in reg.modules) +
              sum(len(p.get("parameters", [])) for p in reg.programs[:i]))
        out.append("  { ProgramId::%s, %s, %s, %du, %s, %s, %s, %du, %du, %s, %s }," % (
            sanitize(prog["id"]), qs(prog["id"]), qs(prog["cartridge"]), int(prog["slot"]),
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
