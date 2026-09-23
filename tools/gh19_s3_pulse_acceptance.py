#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# GH#19 S3 (task #119): THE production acceptance gate for the pulse two-edge correction.
#
# WHAT THIS IS. One gate, run once, over the artifacts the pinned tools already produce. It reads:
#
#   * the PINNED expectation (`report/gh19-s3-pulse-product/acceptance_criteria.tsv`) -- thresholds,
#     the axis declaration, and the enumerated cell list. Never written by this script.
#   * the PROBE'S OWN declared plan (`gh19_s3_plan.tsv`), written by the probe from its compiled-in
#     axes BEFORE the first render. The plan is the independent second opinion on coverage.
#   * the two analyzer reports (`gh19_s3_pulse_analyze.py --out`), one per arm, each carrying BOTH the
#     static matrix and the instrument self-check lines. Both come from ONE file per arm on purpose:
#     a matrix and a self-check taken from different runs would let a failed instrument be paired
#     with a passing matrix.
#   * the delta table (`gh19_s3_pulse_delta.py --out`) for the improvement columns.
#   * the two arms' `gh19_s3_scenarios.tsv` for the zero-crossing frequency reading, which is the
#     probe's own measurement and not the analyzer's projection.
#
# THE RULE THAT ORDERS EVERYTHING: REFUSAL BEFORE JUDGEMENT. Every check that decides whether these
# artifacts are the ones the contract describes runs to completion first. If any of them fails, the
# gate REFUSES (exit 4) and NO judgement is issued -- a red verdict computed from artifacts that
# failed an input check would be indistinguishable from a red verdict about the DSP, and the negative
# controls depend on that distinction. Judgement (exit 1 for RED, exit 0 for PASS) is reached only
# when every refusal check has passed.
#
# WHY A GATE AND NOT A REPORT. @Codex's order: "turn the measurements into ONE acceptance gate:
# the candidate exits normally and passes in full, and each isolated source mutation exits normally
# and hits its corresponding NAMED assertion". Every refusal and every judgement therefore prints a
# named code (`ACCEPT-REFUSE-*` / `ACCEPT-FAIL-*`) that a mutation runner can grep for. The arithmetic
# is per cell and never aggregated: a cell that fails is named with its measured value, and there is
# no averaging, no "median improvement", and no threshold that can be relaxed from the command line.

import argparse
import hashlib
import math
import os
import re
import sys

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

# Pinned from tools/gh19_s3_pulse_analyze.py's static block. Kept as a literal rather than imported so
# that a change to the analyzer's column order is a REFUSAL here (the report no longer parses against
# the expectation) instead of being silently absorbed.
MATRIX_COLS = ("id", "sr", "f0", "duty", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "floor_db", "harm_db", "kmax")

# Pinned from tools/gh19_s3_pulse_delta.py's HEADER, same reasoning.
DELTA_COLS = ("id", "kind", "sr", "f0", "duty_or_u",
              "res_base_db", "res_cand_db", "dres_db",
              "res1k5k_base_db", "res1k5k_cand_db", "dres1k5k_db",
              "reseffbd_base_db", "reseffbd_cand_db", "dreseffbd_db",
              "dfloor_db", "dharm_db", "dA", "dfund", "ddc",
              "refconv_base_db", "refconv_cand_db", "drefconv_db",
              "res1k5k2L_base_db", "res1k5k2L_cand_db", "dres1k5k2L_db",
              "reseffbd2L_base_db", "reseffbd2L_cand_db", "dreseffbd2L_db")

REQUIRED_CRITERIA = ("full_band_min_improvement_db", "band_1k5k_min_improvement_db",
                     "max_fund_amp_rel_change", "max_dc_abs_delta",
                     "max_freq_shift_cycles_per_window")

# Quantities the authorization says must be REPORTED rather than gated. Required, so "reported
# separately" cannot become "not reported"; never given a threshold, because inventing one for a
# quantity the authorization left unbounded is exactly the defect the audit named.
REQUIRED_REPORTS = ("max_abs_fitted_scale_delta",
                    "max_pinned_scale_cost_db",
                    "worst_band_1k5k_improvement_db",
                    "worst_full_band_improvement_db",
                    "worst_pinned_scale_rect_full_band_improvement_db",
                    "worst_rect_full_band_improvement_db")

REQUIRED_AXES = ("sides", "sample_rates", "frequencies", "duties")

PLAN_REV = "1"

# The gate cell id is the probe's own staticCellId, reproduced here so the id and the axes can be
# checked against each other. A cell whose id does not decode to the axes that claim it is not the
# cell the contract names.
CELL_RE = re.compile(r"^vco_(?P<side>[ab])_pulse_(?P<sr>\d+)_(?P<f0>\d+)_pw(?P<tok>\d+)$")

# The residual triples in deltas.tsv are printed to 2 decimals; a matrix column likewise. The worst
# case for `delta == C - B` when all three are rounded is 3 * 0.005. 0.02 leaves room without leaving
# enough room for a genuinely different pair of arms to pass.
ARMS_AGREE_TOL_DB = 0.02

BANDS = ("res_db", "res_1k5k_db", "res_effbd_db")

# How many examples of one named code to print before collapsing to a count.
MAX_DETAIL = 6


class Refuse(Exception):
    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code
        self.detail = detail


class Gate(object):
    def __init__(self):
        self.lines = []
        self.refusals = []
        self.failures = []

    def say(self, *parts):
        line = " ".join(str(p) for p in parts)
        self.lines.append(line)
        print(line)

    def refuse(self, code, detail):
        self.refusals.append((code, detail))
        self._report(self.refusals, "ACCEPT-REFUSE", code, detail)

    def fail(self, code, detail):
        self.failures.append((code, detail))
        self._report(self.failures, "ACCEPT-FAIL", code, detail)

    def _report(self, bucket, prefix, code, detail):
        """Print the first few examples of each named code, then a count.

        The cap is for legibility only: every code still appears, and the counts line in `finish`
        carries the total, so a truncated list can never be read as "only this many failed".
        """
        n = sum(1 for c, _ in bucket if c == code)
        if n <= MAX_DETAIL:
            self.say("%s-%s %s" % (prefix, code, detail))
        elif n == MAX_DETAIL + 1:
            self.say("%s-%s ... further examples suppressed; the counts line carries the total" %
                     (prefix, code))


# --------------------------------------------------------------------------------------- parsing

def _rows(path):
    """Data rows of a tab-separated file, comments (`#`) and blanks dropped, split in place."""
    out = []
    with open(path, "r") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            out.append((n, line.split("\t")))
    return out


def parse_criteria(g, path):
    crit, axis, cells, report_only, reports = {}, {}, [], [], set()
    for n, f in _rows(path):
        if len(f) < 2:
            g.refuse("CRITERIA", "%s:%d: row has fewer than 2 fields" % (path, n))
            continue
        key = f[0]
        if key == "criterion":
            if len(f) != 3:
                g.refuse("CRITERIA", "%s:%d: criterion row needs exactly 3 fields" % (path, n))
                continue
            if f[1] in crit:
                g.refuse("CRITERIA", "%s:%d: duplicate criterion %s" % (path, n, f[1]))
                continue
            try:
                crit[f[1]] = float(f[2])
            except ValueError:
                g.refuse("CRITERIA", "%s:%d: %s is not a number: %r" % (path, n, f[1], f[2]))
        elif key == "axis":
            if len(f) != 3:
                g.refuse("CRITERIA", "%s:%d: axis row needs exactly 3 fields" % (path, n))
                continue
            if f[1] in axis:
                g.refuse("CRITERIA", "%s:%d: duplicate axis %s" % (path, n, f[1]))
                continue
            axis[f[1]] = [v for v in f[2].split(",") if v]
        elif key in ("cell", "report_only_cell"):
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: %s row needs exactly 2 fields" % (path, n, key))
                continue
            (cells if key == "cell" else report_only).append(f[1])
        elif key == "report":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: report row needs exactly 2 fields" % (path, n))
                continue
            if f[1] in reports:
                g.refuse("CRITERIA", "%s:%d: duplicate report %s" % (path, n, f[1]))
            reports.add(f[1])
        else:
            g.refuse("CRITERIA", "%s:%d: unknown row key %r" % (path, n, key))

    missing = [k for k in REQUIRED_CRITERIA if k not in crit]
    extra = [k for k in crit if k not in REQUIRED_CRITERIA]
    if missing or extra:
        g.refuse("CRITERIA", "criterion keys missing=%s unexpected=%s" %
                 (",".join(missing) or "-", ",".join(extra) or "-"))
    missing = [k for k in REQUIRED_REPORTS if k not in reports]
    extra = sorted(k for k in reports if k not in REQUIRED_REPORTS)
    if missing or extra:
        g.refuse("CRITERIA", "report keys missing=%s unexpected=%s" %
                 (",".join(missing) or "-", ",".join(extra) or "-"))
    missing = [k for k in REQUIRED_AXES if k not in axis]
    extra = [k for k in axis if k not in REQUIRED_AXES]
    if missing or extra:
        g.refuse("CRITERIA", "axis keys missing=%s unexpected=%s" %
                 (",".join(missing) or "-", ",".join(extra) or "-"))

    for name, seq in (("cell", cells), ("report_only_cell", report_only)):
        dup = sorted({v for v in seq if seq.count(v) > 1})
        if dup:
            g.refuse("CRITERIA", "%s: duplicate ids %s" % (name, ",".join(dup)))
    both = sorted(set(cells) & set(report_only))
    if both:
        g.refuse("CRITERIA", "cell and report_only_cell overlap: %s" % ",".join(both))
    if not cells:
        g.refuse("CRITERIA", "no cell rows at all")

    # RE-DERIVE the gate cells from the axis declaration and require the enumeration to match it.
    # This is what stops a silent shrink: deleting rows from the enumeration while leaving the axes
    # alone makes the two disagree, and editing BOTH to a smaller product makes them disagree with
    # the probe's plan in the next check.
    derived = None
    if not missing:
        try:
            derived = set()
            for side in axis["sides"]:
                for sr in axis["sample_rates"]:
                    for f0 in axis["frequencies"]:
                        for duty in axis["duties"]:
                            derived.add("vco_%s_pulse_%d_%d_pw%d" %
                                        (side, int(sr), int(f0), int(round(float(duty) * 100))))
        except (ValueError, KeyError) as exc:
            g.refuse("CRITERIA", "axis values are not numeric: %s" % exc)
            derived = None
    if derived is not None:
        only_enum = sorted(set(cells) - derived)
        only_axis = sorted(derived - set(cells))
        if only_enum or only_axis:
            g.refuse("CRITERIA-ENUMERATION",
                     "enumerated %d vs axis product %d; enumerated-only=%s axis-only=%s" %
                     (len(set(cells)), len(derived),
                      ",".join(only_enum[:6]) or "-", ",".join(only_axis[:6]) or "-"))
    return crit, cells, report_only, reports


PLAN_HEADER = ("block", "id")


def parse_plan(g, path):
    """The probe's declared plan: `plan_rev\\tN`, the header `block\\tid`, then `block\\tid` rows.

    The header is required rather than assumed, so a file that is merely id-shaped cannot pass for a
    plan, and the column order is the file's own declaration rather than a guess.
    """
    plan_rev = None
    header_seen = False
    blocks = {}
    for n, f in _rows(path):
        if f[0] == "plan_rev":
            plan_rev = f[1] if len(f) > 1 else None
            continue
        if tuple(f) == PLAN_HEADER:
            if header_seen:
                g.refuse("PLAN", "%s:%d: repeated header" % (path, n))
            header_seen = True
            continue
        if len(f) != 2:
            g.refuse("PLAN", "%s:%d: %d fields, expected 2 (block, id)" % (path, n, len(f)))
            continue
        if not header_seen:
            g.refuse("PLAN", "%s:%d: row before the `block\\tid` header" % (path, n))
            continue
        blocks.setdefault(f[0], []).append(f[1])
    if plan_rev != PLAN_REV:
        g.refuse("PLAN", "%s: plan_rev=%r, expected %r" % (path, plan_rev, PLAN_REV))
    if not header_seen:
        g.refuse("PLAN", "%s: the `block\\tid` header never appeared" % path)
    if "static" not in blocks:
        g.refuse("PLAN", "%s: no `static` block" % path)
        return set()
    static = blocks["static"]
    dup = sorted({v for v in static if static.count(v) > 1})
    if dup:
        g.refuse("PLAN", "static block has duplicate ids: %s" % ",".join(dup))
    return set(static)


def parse_deltas(g, path):
    rows = _rows(path)
    if not rows:
        g.refuse("DELTAS-SHAPE", "%s: no data rows" % path)
        return {}
    n, header = rows[0]
    if tuple(header) != DELTA_COLS:
        g.refuse("DELTAS-SHAPE", "%s:%d: header mismatch; got %d cols, expected %d" %
                 (path, n, len(header), len(DELTA_COLS)))
        return {}
    out = {}
    for n, f in rows[1:]:
        if len(f) != len(DELTA_COLS):
            g.refuse("DELTAS-SHAPE", "%s:%d: %d fields, expected %d" % (path, n, len(f), len(DELTA_COLS)))
            continue
        if f[0] in out:
            g.refuse("DELTAS-SHAPE", "%s:%d: duplicate id %s" % (path, n, f[0]))
            continue
        row = dict(zip(DELTA_COLS, f))
        row["_line"] = n
        out[f[0]] = row
    if not out:
        g.refuse("DELTAS-SHAPE", "%s: zero usable rows" % path)
    return out


def _num(g, code, path, line, col, val):
    if val == "-":
        return None
    try:
        x = float(val)
    except ValueError:
        g.refuse(code, "%s:%d: %s=%r is not a number" % (path, line, col, val))
        return None
    if not math.isfinite(x):
        g.refuse(code, "%s:%d: %s=%r is not finite" % (path, line, col, val))
        return None
    return x


def parse_matrix(g, path):
    """The `static` block of an analyzer report, plus the instrument lines from the same file."""
    text = open(path, "r").read().splitlines()
    begin = end = None
    for i, line in enumerate(text):
        if line.strip() == "-- BEGIN MATRIX static":
            begin = i
        elif line.startswith("-- END MATRIX static"):
            end = i
    if begin is None or end is None or end < begin:
        g.refuse("MATRIX-SHAPE", "%s: no `-- BEGIN/END MATRIX static` fence pair" % path)
        return {}, None

    decl = {}
    for tok in text[end].split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            decl[k] = v

    # The block opens with PROSE and then the pinned header line; the data rows are everything after
    # it and nothing before it. Anchoring on the header rather than on "looks like a row" is what
    # makes an unrecognised shape a refusal instead of prose silently parsed (or silently skipped).
    heads = [i for i in range(begin + 1, end) if tuple(text[i].split()) == MATRIX_COLS]
    if len(heads) != 1:
        g.refuse("MATRIX-SHAPE", "%s: the pinned header line appeared %d times in the static block, "
                                 "expected exactly 1" % (path, len(heads)))
        return {}, decl

    rows = {}
    for i in range(heads[0] + 1, end):
        f = text[i].split()
        if not f:
            continue
        if len(f) != len(MATRIX_COLS):
            g.refuse("MATRIX-SHAPE", "%s:%d: %d columns, expected %d: %s" %
                     (path, i + 1, len(f), len(MATRIX_COLS), " ".join(f)))
            continue
        if not CELL_RE.match(f[0]) and not f[0].startswith("vco_"):
            g.refuse("MATRIX-SHAPE", "%s:%d: row does not begin with a cell id: %s" %
                     (path, i + 1, " ".join(f)))
            continue
        d = {}
        for col, val in zip(MATRIX_COLS[1:], f[1:]):
            d[col] = _num(g, "MATRIX-SHAPE", path, i + 1, col, val)
        d["id"] = f[0]
        if d["id"] in rows:
            g.refuse("MATRIX-SHAPE", "%s:%d: duplicate id %s" % (path, i + 1, d["id"]))
            continue
        rows[d["id"]] = d
    return rows, decl


def parse_instrument(g, path):
    """The instrument's own verdict lines. These are the power-unit tripwire."""
    text = open(path, "r").read().splitlines()
    ok = 0
    for line in text:
        if "MEAN SQUARES AGREE (ok)" in line:
            ok += 1
        if "***" in line:
            g.refuse("ARMSELFCHECK", "%s: instrument failure marker: %s" % (path, line.strip()))
    for lbl in ("hann", "rect"):
        want = "(e) Parseval absolute, %s window" % lbl
        if not any(want in l and "MEAN SQUARES AGREE (ok)" in l for l in text):
            g.refuse("ARMSELFCHECK",
                     "%s: no OK `%s` Parseval line; the power-unit tripwire did not run" % (path, want))
    align = [l for l in text if l.startswith("ALIGN-GATE ")]
    if not align:
        g.refuse("ARMSELFCHECK", "%s: no ALIGN-GATE line; --align-check did not run" % path)
    else:
        kv = dict(tok.partition("=")[::2] for tok in align[-1].split()[1:])
        if kv.get("ok") != "1" or kv.get("bad") != "0":
            g.refuse("ARMSELFCHECK", "%s: %s" % (path, align[-1].strip()))
    return ok


def parse_scenarios(g, path):
    rows = _rows(path)
    if not rows:
        g.refuse("SCENARIOS-SHAPE", "%s: no data rows" % path)
        return {}
    n, header = rows[0]
    need = ("id", "sr_hz", "win", "f0_meas_hz")
    for col in need:
        if col not in header:
            g.refuse("SCENARIOS-SHAPE", "%s:%d: column %s missing" % (path, n, col))
            return {}
    idx = {c: header.index(c) for c in need}
    out = {}
    for n, f in rows[1:]:
        if len(f) != len(header):
            g.refuse("SCENARIOS-SHAPE", "%s:%d: %d fields, expected %d" % (path, n, len(f), len(header)))
            continue
        d = {"id": f[idx["id"]]}
        for col in need[1:]:
            d[col] = _num(g, "SCENARIOS-SHAPE", path, n, col, f[idx[col]])
        if d["id"] in out:
            g.refuse("SCENARIOS-SHAPE", "%s:%d: duplicate id %s" % (path, n, d["id"]))
            continue
        out[d["id"]] = d
    return out


def decode_cell(g, code, cid, where):
    m = CELL_RE.match(cid)
    if not m:
        g.refuse(code, "%s: id %s is not a gate-cell id" % (where, cid))
        return None
    return {"side": m.group("side"), "sr": int(m.group("sr")), "f0": int(m.group("f0")),
            "duty": int(m.group("tok")) / 100.0}


# ------------------------------------------------------------------------------------- the gate

def run(g, args):
    crit, cells, report_only, reports = parse_criteria(g, args.criteria)
    plan_static = parse_plan(g, args.plan)
    deltas = parse_deltas(g, args.deltas)
    base, base_decl = parse_matrix(g, args.base_report)
    cand, cand_decl = parse_matrix(g, args.cand_report)
    base_scn = parse_scenarios(g, os.path.join(args.base_arm, "gh19_s3_scenarios.tsv"))
    cand_scn = parse_scenarios(g, os.path.join(args.cand_arm, "gh19_s3_scenarios.tsv"))
    parse_instrument(g, args.base_report)
    parse_instrument(g, args.cand_report)

    gate_cells = sorted(set(cells))
    g.say("ACCEPT-GATE criteria=%s cells=%d report_only=%d plan_static=%d" %
          (os.path.basename(args.criteria), len(gate_cells), len(report_only), len(plan_static)))

    # ---- the plan reconciliation: the probe's declared static set IS the two pinned groups -------
    if plan_static:
        want = set(gate_cells) | set(report_only)
        missing = sorted(want - plan_static)
        extra = sorted(plan_static - want)
        if missing or extra:
            g.refuse("PLAN", "plan static != gate cells U report_only cells; "
                             "declared-but-not-pinned=%s pinned-but-not-declared=%s" %
                     (",".join(missing[:6]) or "-", ",".join(extra[:6]) or "-"))

    # ---- declared counts must agree with the plan, which is what makes "skipped" visible ---------
    for name, decl in (("base", base_decl), ("cand", cand_decl)):
        if not decl:
            continue
        try:
            d, e, s = int(decl["declared"]), int(decl["emitted"]), int(decl["skipped"])
        except (KeyError, ValueError):
            g.refuse("MATRIX-SHAPE", "%s: unreadable `-- END MATRIX static` counts: %s" % (name, decl))
            continue
        if s != 0 or e != d or d != len(plan_static):
            g.refuse("MATRIX-SHAPE", "%s: declared=%d emitted=%d skipped=%d but the plan declares %d" %
                     (name, d, e, s, len(plan_static)))

    # ---- per-cell coverage and metadata, in all four artifacts ----------------------------------
    for cid in gate_cells:
        dec = decode_cell(g, "DELTAS-UNKNOWN-ID", cid, "criteria")
        if dec is None:
            continue
        if cid not in plan_static:
            g.refuse("PLAN", "gate cell %s is not in the probe's declared static plan" % cid)
        if cid in report_only:
            g.refuse("CRITERIA", "gate cell %s is also pinned as report_only" % cid)

        row = deltas.get(cid)
        if row is None:
            g.refuse("DELTAS-UNKNOWN-ID", "gate cell %s is absent from the delta table" % cid)
        else:
            ln = row["_line"]
            if row["kind"] != "static":
                g.refuse("DELTAS-METADATA", "%s: kind=%r, expected static" % (cid, row["kind"]))
            for col, want in (("sr", dec["sr"]), ("f0", dec["f0"])):
                got = _num(g, "DELTAS-METADATA", args.deltas, ln, col, row[col])
                if got is not None and abs(got - want) > 1e-9:
                    g.refuse("DELTAS-METADATA", "%s: id says %s=%d, row says %s=%s" %
                             (cid, col, want, col, row[col]))
            got = _num(g, "DELTAS-METADATA", args.deltas, ln, "duty_or_u", row["duty_or_u"])
            if got is not None and abs(got - dec["duty"]) > 1e-9:
                g.refuse("DELTAS-METADATA", "%s: id says duty=%.3f, row says %s" %
                         (cid, dec["duty"], row["duty_or_u"]))
            for col in ("dres_db", "dres1k5k_db", "dreseffbd_db"):
                _num(g, "DELTAS-SHAPE", args.deltas, ln, col, row[col])

        for name, mat, path in (("base", base, args.base_report), ("cand", cand, args.cand_report)):
            d = mat.get(cid)
            if d is None:
                g.refuse("MATRIX-SHAPE", "%s: gate cell %s is absent from the static matrix" % (name, cid))
                continue
            for col, want in (("sr", dec["sr"]), ("f0", dec["f0"])):
                if d[col] is not None and abs(d[col] - want) > 1e-9:
                    g.refuse("MATRIX-SHAPE", "%s/%s: id says %s=%d, matrix says %.6f" %
                             (name, cid, col, want, d[col]))
            if d["duty"] is not None and abs(d["duty"] - dec["duty"]) > 5e-3:
                g.refuse("MATRIX-SHAPE", "%s/%s: id says duty=%.3f, matrix says %.6f" %
                         (name, cid, dec["duty"], d["duty"]))
            for col in ("A", "fund", "dc") + BANDS:
                if d.get(col) is None:
                    g.refuse("MATRIX-SHAPE", "%s/%s: column %s is not a finite number" % (name, cid, col))

    # ---- the arms the deltas were built from are the arms whose matrices we are judging ----------
    # A swapped or mismatched pair is the failure mode where every delta silently changes sign and
    # the gate would report a spectacular improvement that belongs to the wrong revision.
    for cid in gate_cells:
        row = deltas.get(cid)
        b, c = base.get(cid), cand.get(cid)
        if row is None or b is None or c is None:
            continue
        for col, dbase, dcand in (("res_db", "res_base_db", "res_cand_db"),
                                  ("res_1k5k_db", "res1k5k_base_db", "res1k5k_cand_db"),
                                  ("res_effbd_db", "reseffbd_base_db", "reseffbd_cand_db")):
            # Read through _num, not float(): a NaN here would make the comparison below false and
            # silently switch this cross-check OFF for that cell, which is the one failure it exists
            # to catch. `-` is a declared absence and is the only value that may skip it.
            vb = _num(g, "DELTAS-SHAPE", args.deltas, row["_line"], dbase, row[dbase])
            vc = _num(g, "DELTAS-SHAPE", args.deltas, row["_line"], dcand, row[dcand])
            if vb is None or vc is None:
                continue
            want = vc - vb
            got = c[col] - b[col]
            if abs(got - want) > ARMS_AGREE_TOL_DB:
                g.refuse("ORDERING", "%s: %s delta %.4f does not match cand-base %.4f (%.6f vs %.6f); "
                                     "the delta table was not built from these two matrices" %
                         (cid, col, want, got, c[col], b[col]))

    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    # ---- JUDGEMENT -------------------------------------------------------------------------------
    full_min = crit["full_band_min_improvement_db"]
    band_min = crit["band_1k5k_min_improvement_db"]
    max_amp = crit["max_fund_amp_rel_change"]
    max_dc = crit["max_dc_abs_delta"]
    max_cycles = crit["max_freq_shift_cycles_per_window"]

    # ---- reported, never judged -----------------------------------------------------------------
    # `pinned_res_db` is exact, not an estimate. res_db = 10*log10(||e||^2/||A r||^2) with e the
    # least-squares residual, so <e, r> = 0 and for any other scale A' the numerator obeys
    # ||x - A' r||^2 = ||e||^2 + (A'-A)^2 ||r||^2. Dividing by the new denominator A'^2 and writing
    # ||e||^2/||r||^2 = A^2 * 10^(res/10) gives the closed form below.
    #
    # WHICH BAND THIS IS EXACT IN, AND WHY IT IS NAMED AFTER THAT BAND. Orthogonality is a property of
    # the inner product A was fitted in -- the unwindowed full band, column `res_db`, which the
    # analyzer's own self-check pins as `band(full, rect) == res_db` to 7e-13. The gate's own two
    # columns (`res_1k5k_db`, `res_effbd_db`) are Hann-weighted band powers of the SAME fitted
    # reference, and the instrument publishes only ONE scale A per cell, so A is not that band's
    # least-squares scale and the identity does not carry over unchanged. Reporting a "pinned-scale"
    # figure for a Hann column would therefore be a number computed in a different measurement than
    # the one it is named after -- the unit error this gate exists to refuse. So the pinned figure is
    # the RECT-band pair (pinned vs unpinned in one band), the Hann gate columns are reported as
    # measured and are not claimed to be pinned, and the gap between the two windows is measured and
    # printed rather than assumed away.
    def pinned_res_db(a_base, a_cand, res_cand):
        d = a_base - a_cand
        return (res_cand
                + 10.0 * math.log10(1.0 + (d * d) / ((a_cand * a_cand) * (10.0 ** (res_cand / 10.0))))
                - 20.0 * math.log10(a_base / a_cand))

    worst_full = min(float(deltas[c]["dreseffbd_db"]) for c in gate_cells)
    worst_band = min(float(deltas[c]["dres1k5k_db"]) for c in gate_cells)
    worst_rect = min(float(deltas[c]["dres_db"]) for c in gate_cells)
    gap = max(max(abs(base[c]["res_db"] - base[c]["res_effbd_db"]),
                  abs(cand[c]["res_db"] - cand[c]["res_effbd_db"])) for c in gate_cells)

    def _finite(v):
        return v is not None and math.isfinite(v)

    # The reports are DERIVED from the same two matrices the judgement reads, so a matrix that does not
    # carry the fitted scale and the residuals cannot silently drop them: an uncomputable report is a
    # REFUSAL, not an omission. `A > 0` is part of computable, not a separate claim: the closed form
    # divides by A and takes a log of it.
    lack = [c for c in gate_cells
            if not (_finite(base[c]["A"]) and _finite(cand[c]["A"])
                    and base[c]["A"] > 0.0 and cand[c]["A"] > 0.0
                    and _finite(base[c]["res_db"]) and _finite(cand[c]["res_db"]))]
    if lack:
        for name in REQUIRED_REPORTS:
            g.say("ACCEPT-REPORT %s=UNAVAILABLE" % name)
        g.refuse("MATRIX-SHAPE", "%d gate cell(s), starting with %s, do not carry a finite POSITIVE "
                                 "fitted scale A and a finite full-band residual in BOTH arms; the "
                                 "reported quantities cannot be derived" % (len(lack), lack[0]))
        return finish(g, "REFUSE", EXIT_REFUSE)

    max_dA = max(abs(cand[c]["A"] - base[c]["A"]) for c in gate_cells)
    costs, pinned_deltas = [], []
    for cid in gate_cells:
        p = pinned_res_db(base[cid]["A"], cand[cid]["A"], cand[cid]["res_db"])
        costs.append(p - cand[cid]["res_db"])
        pinned_deltas.append(p - base[cid]["res_db"])
    max_cost = max(costs)
    worst_pinned = min(pinned_deltas)

    computed = {
        "max_abs_fitted_scale_delta": "%.6f" % max_dA,
        "max_pinned_scale_cost_db": "%+.4f" % max_cost,
        "worst_band_1k5k_improvement_db": "%+.2f" % worst_band,
        "worst_full_band_improvement_db": "%+.2f" % worst_full,
        "worst_pinned_scale_rect_full_band_improvement_db": "%+.3f" % worst_pinned,
        "worst_rect_full_band_improvement_db": "%+.3f" % worst_rect,
    }
    for name in REQUIRED_REPORTS:
        g.say("ACCEPT-REPORT %s=%s" % (name, computed[name]))
    g.say("ACCEPT-EVIDENCE improvement_cells=%d worst_dreseffbd_db=%.2f worst_dres1k5k_db=%.2f "
          "worst_dres_db=%.2f band_rect_vs_effbd_max_gap_db=%.4f" %
          (len(gate_cells), worst_full, worst_band, worst_rect, gap))
    g.say("ACCEPT-NOTE the two ``worst_..._rect_full_band...`` reports are one band's pair: the same "
          "gate cells, the same fixed reference, phase and denominator, once with each arm's own "
          "fitted scale and once with the scale pinned to the BASELINE arm's value. The difference is "
          "the part of the rect-band improvement the fit accounts for. The ``worst_full_band`` and "
          "``worst_band_1k5k`` reports are the gate's own Hann columns, unchanged; no pinned-scale "
          "value is claimed for them because the published single A is not that band's least-squares "
          "scale. All six are report-only: the authorization sets no threshold on any of them.")

    short = [c for c in gate_cells
             if float(deltas[c]["dreseffbd_db"]) > -full_min or float(deltas[c]["dres1k5k_db"]) > -band_min]
    for cid in short:
        r = deltas[cid]
        g.fail("IMPROVEMENT",
               "%s: dreseffbd_db=%s (need <= %+.2f) dres1k5k_db=%s (need <= %+.2f)" %
               (cid, r["dreseffbd_db"], -full_min, r["dres1k5k_db"], -band_min))

    for cid in gate_cells:
        b, c = base[cid], cand[cid]
        if b["fund"] == 0.0:
            g.fail("GUARD-AMP", "%s: fundamental projection is zero in the baseline" % cid)
            continue
        rel = abs(c["fund"] - b["fund"]) / abs(b["fund"])
        if rel > max_amp:
            g.fail("GUARD-AMP", "%s: fundamental amplitude moved %.6f (%.6f -> %.6f), limit %.4f" %
                   (cid, rel, b["fund"], c["fund"], max_amp))
        ddc = abs(c["dc"] - b["dc"])
        if ddc > max_dc:
            g.fail("GUARD-DC", "%s: |DC| changed by %.6f (%.6f -> %.6f), limit %.4f" %
                   (cid, ddc, b["dc"], c["dc"], max_dc))

    # The frequency guard is read from the PROBE's zero-crossing measurement, not from the analyzer
    # whose residual columns are under test: the correction changes waveform values only, so a
    # crossing can move by at most one sample per edge, which over a window of `win` frames bounds
    # the measurable shift at sr/win cycles. That is a derivation, not a tolerance.
    for cid in gate_cells:
        bs, cs = base_scn.get(cid), cand_scn.get(cid)
        if bs is None or cs is None:
            g.fail("GUARD-FREQ", "%s: no scenario row in one of the arms" % cid)
            continue
        if bs["f0_meas_hz"] is None or cs["f0_meas_hz"] is None:
            g.fail("GUARD-FREQ", "%s: f0_meas_hz is not finite in one of the arms" % cid)
            continue
        if bs["win"] != cs["win"] or bs["sr_hz"] != cs["sr_hz"]:
            g.fail("GUARD-FREQ", "%s: the two arms analysed different windows/sample rates" % cid)
            continue
        bound = max_cycles * bs["sr_hz"] / bs["win"]
        shift = abs(cs["f0_meas_hz"] - bs["f0_meas_hz"])
        if shift > bound:
            g.fail("GUARD-FREQ", "%s: zero-crossing frequency moved %.6f Hz (%.6f -> %.6f), "
                                 "bound sr/win = %.6f Hz" %
                   (cid, shift, bs["f0_meas_hz"], cs["f0_meas_hz"], bound))

    # The report-only cells are never judged, but the gate still requires that they were measured --
    # otherwise "fully reported but not gated" could quietly mean "not reported".
    for cid in sorted(set(report_only)):
        for name, mat in (("base", base), ("cand", cand)):
            if cid not in mat:
                g.refuse("MATRIX-SHAPE", "report-only cell %s is absent from the %s matrix" % (cid, name))
        if cid not in deltas:
            g.refuse("DELTAS-UNKNOWN-ID", "report-only cell %s is absent from the delta table" % cid)
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    if g.failures:
        return finish(g, "RED", EXIT_RED)
    return finish(g, "PASS", EXIT_PASS)


def finish(g, verdict, code):
    for label, bucket in (("REFUSE", g.refusals), ("FAIL", g.failures)):
        counts = {}
        for c, _ in bucket:
            counts[c] = counts.get(c, 0) + 1
        for c in sorted(counts):
            g.say("ACCEPT-COUNT %s-%s n=%d" % (label, c, counts[c]))
    g.say("ACCEPT-GATE verdict=%s refusals=%d failures=%d" %
          (verdict, len(g.refusals), len(g.failures)))
    if verdict == "REFUSE":
        g.say("ACCEPT-NOTE no judgement is issued: the artifacts failed an input check, so a red or "
              "green verdict here would not be a statement about the DSP.")
    if g.out:
        with open(g.out, "w") as fh:
            fh.write("\n".join(g.lines) + "\n")
    return code


def main(argv):
    ap = argparse.ArgumentParser(description="GH#19 S3 (task #119) production acceptance gate.")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--plan", required=True, help="the probe's declared plan (gh19_s3_plan.tsv)")
    ap.add_argument("--deltas", required=True, help="gh19_s3_pulse_delta.py --out")
    ap.add_argument("--base-report", required=True, help="analyzer --out for the baseline arm")
    ap.add_argument("--cand-report", required=True, help="analyzer --out for the candidate arm")
    ap.add_argument("--base-arm", required=True, help="probe --out directory of the baseline arm")
    ap.add_argument("--cand-arm", required=True, help="probe --out directory of the candidate arm")
    ap.add_argument("--out", default=None, help="also write the full transcript here")
    args = ap.parse_args(argv)

    g = Gate()
    g.out = args.out
    g.say("ACCEPT-GATE tool=gh19_s3_pulse_acceptance.py")
    g.say("ACCEPT-GATE base_report=%s" % args.base_report)
    g.say("ACCEPT-GATE cand_report=%s" % args.cand_report)
    # The verdict is only about the artifacts it actually read, so it records their hashes. Without
    # this a transcript cannot be tied to a revision, and a report that cites it cannot be re-checked.
    # Informational: nothing is compared against a pin here, because a criteria file that pinned its
    # own input hashes could not also be the thing under review.
    for label, path in (("criteria", args.criteria), ("plan", args.plan), ("deltas", args.deltas),
                        ("base_report", args.base_report), ("cand_report", args.cand_report)):
        try:
            with open(path, "rb") as fh:
                digest = hashlib.sha256(fh.read()).hexdigest()
        except OSError as exc:
            digest = "UNREADABLE(%s)" % exc.__class__.__name__
        g.say("ACCEPT-INPUT %s sha256=%s path=%s" % (label, digest, path))
    return run(g, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
