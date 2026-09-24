#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh19_s6_saw_acceptance.py - task #120 (GH #19 S6) production acceptance gate for the saw / invSaw
# stretches of the continuous wave-morph ring.
#
# WHAT THIS DECIDES, AND ON WHAT EVIDENCE. It reads the PINNED expectation
# (report/gh19-s6-saw-aa/acceptance_criteria.tsv) and two analyzer reports produced from two rendered
# arms, and it decides whether the candidate arm clears both residual thresholds on every improvement
# cell while leaving the two degenerate positions exactly as the identities require. It never writes
# the expectation, and it never re-derives a threshold.
#
# THE MATRIX CANNOT BE SHRUNK QUIETLY. Three independently produced artifacts are reconciled against
# each other, and every one of them can fail the run on its own:
#
#   * the probe's OWN declared plan (gh19_s6_plan.tsv), written from its compiled-in axes BEFORE the
#     first render, must equal {cell} U {equality_cell} U {report_only_cell} exactly -- so a cell
#     deleted from an enumeration fails here rather than becoming a smaller matrix;
#   * the `axis_set` rows, from which this gate RE-DERIVES each group's cross product and requires
#     {cell} == A U B with A and B disjoint and EQ == {equality_cell};
#   * every id must DECODE to the axes that claim it, so an id cannot be relabelled to sit in a
#     group whose numbers it does not carry.
#
# THE TWO CRITERION FAMILIES ARE NOT INTERCHANGEABLE. One `criterion` name is not a quantity here:
#   * IMPROVEMENT cells clear `full_band_min_improvement_db` and `band_1k5k_min_improvement_db`;
#   * EQUALITY cells are the two positions where an improvement threshold is UNSATISFIABLE, not
#     merely hard -- the anti-phase midpoint (norm = 0.125) has amplitude AND jump 0, so the ratio
#     does not exist, and the sine node (norm = 0.5) has no jump to remove. They are asserted as
#     identities (`antiphase_midpoint_max_abs_output`, `sine_node_bit_diff_count`) and are NEVER run
#     through a residual threshold. Merging the families would fail a CORRECT implementation, which
#     is the degenerate-identity error this split exists to avoid.
#
# THE FREQUENCY GUARD IS READ FROM THE PROBE, NOT FROM THE ANALYZER. The correction changes waveform
# values only, so a zero crossing can move by at most one sample per edge, which over a window of
# `win` frames bounds the measurable shift at sr/win cycles. That is a derivation, not a tolerance:
# the bound printed at each violation is the derived one.
#
# EXIT CODES, and the difference between them matters: 0 = PASS, 1 = RED (the artifacts are sound and
# the DSP does not meet the criteria), 4 = REFUSE (an INPUT check failed, so NO judgement is issued --
# this is not a catch and not a red verdict, and re-running it unchanged cannot turn it green).

import argparse
import math
import os
import re
import struct
import sys

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

# Pinned literals rather than imports from the analyzer, for S3's stated reason: a change to the
# analyzer's column order then becomes a REFUSAL here (the report no longer parses against the
# expectation) instead of being silently absorbed.
MATRIX_COLS = ("id", "sr", "f0", "morph", "stretch", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "kmax", "max_abs_out")

REQUIRED_CRITERIA = ("full_band_min_improvement_db", "band_1k5k_min_improvement_db",
                     "max_fund_amp_rel_change", "max_dc_abs_delta",
                     "max_freq_shift_cycles_per_window",
                     "antiphase_midpoint_max_abs_output", "sine_node_bit_diff_count")

# Quantities the authorization says must be REPORTED rather than gated. Required, so "reported
# separately" cannot become "not reported"; the six improvised-free ones are never given a threshold,
# because inventing one for a quantity the authorization left unbounded is the defect the audit named.
REQUIRED_REPORTS = ("max_abs_fitted_scale_delta",
                    "max_pinned_scale_cost_db",
                    "worst_band_1k5k_improvement_db",
                    "worst_full_band_improvement_db",
                    "worst_pinned_scale_rect_full_band_improvement_db",
                    "worst_rect_full_band_improvement_db",
                    "antiphase_midpoint_worst_abs_output",
                    "sine_node_worst_bit_diff_count")

REQUIRED_AXES = ("sides", "sample_rates", "frequencies")
PLAN_REV = "1"

# The gate cell id is the probe's own sawCellId, reproduced here so the id and the axes can be checked
# against each other. `sawmixhi` first: `sawmix` is a prefix of it.
CELL_RE = re.compile(r"^vco_(?P<side>[ab])_(?P<fam>sawmixhi|sawmix)_(?P<sr>\d+)_(?P<f0>\d+)_"
                     r"(?P<tag>m\d+)$")

# The tag encodes the morph: m6250 is 0.0625. Written as a division so the mapping cannot be
# mistyped as a table, and the decoded value is compared against the matrix's own `morph` column.
TAG_DIVISOR = 100000.0

MAX_DETAIL = 6


def tag_morph(tag):
    return int(tag[1:]) / TAG_DIVISOR


class Gate:
    def __init__(self, out=None):
        self.refusals = []
        self.failures = []
        self.lines = []
        self.out = out

    def say(self, s):
        self.lines.append(s)
        print(s)

    def refuse(self, code, detail):
        self.refusals.append((code, detail))

    def fail(self, code, detail):
        self.failures.append((code, detail))

    def dump(self, prefix, bucket):
        counts = {}
        for c, d in bucket:
            counts[c] = counts.get(c, 0) + 1
        for c in sorted(counts):
            self.say("ACCEPT-COUNT %s-%s n=%d" % (prefix, c, counts[c]))
        shown = {}
        for c, d in bucket:
            shown[c] = shown.get(c, 0) + 1
            if shown[c] <= MAX_DETAIL:
                self.say("%s-%s %s" % (prefix, c, d))
            elif shown[c] == MAX_DETAIL + 1:
                self.say("%s-%s ... further examples suppressed; the counts line carries the total"
                         % (prefix, c))


# --------------------------------------------------------------------------------------- parsing

def _rows(path):
    """Data rows of a tab-separated file: comments (`#`) and blanks dropped."""
    out = []
    with open(path) as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            out.append((n, line.split("\t")))
    return out


def parse_criteria(g, path):
    crit, axis, sets, cells, eq, report_only, reports = {}, {}, {}, [], [], [], set()
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
        elif key == "axis_set":
            if len(f) != 4:
                g.refuse("CRITERIA", "%s:%d: axis_set row needs exactly 4 fields" % (path, n))
                continue
            sets.setdefault(f[1], {})[f[2]] = [v for v in f[3].split(",") if v]
        elif key == "cell":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: cell row needs exactly 2 fields" % (path, n))
                continue
            cells.append(f[1])
        elif key == "equality_cell":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: equality_cell row needs exactly 2 fields" % (path, n))
                continue
            eq.append(f[1])
        elif key == "report_only_cell":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: report_only_cell row needs exactly 2 fields"
                         % (path, n))
                continue
            report_only.append(f[1])
        elif key == "report":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: report row needs exactly 2 fields" % (path, n))
                continue
            if f[1] in reports:
                g.refuse("CRITERIA", "%s:%d: duplicate report %s" % (path, n, f[1]))
            reports.add(f[1])
        else:
            g.refuse("CRITERIA", "%s:%d: unknown row key %r" % (path, n, key))

    for label, have, want in (("criterion", tuple(crit), REQUIRED_CRITERIA),
                              ("report", tuple(reports), REQUIRED_REPORTS)):
        missing = [k for k in want if k not in have]
        extra = sorted(k for k in have if k not in want)
        if missing or extra:
            g.refuse("CRITERIA", "%s keys missing=%s unexpected=%s"
                     % (label, ",".join(missing) or "-", ",".join(extra) or "-"))
    missing = [k for k in REQUIRED_AXES if k not in axis]
    if missing:
        g.refuse("CRITERIA", "axis rows missing=%s" % ",".join(missing))
    for name, grp in sorted(sets.items()):
        for need in ("positions", "frequencies"):
            if need not in grp:
                g.refuse("CRITERIA", "axis_set %s has no `%s` row" % (name, need))
    for label, lst in (("cell", cells), ("equality_cell", eq), ("report_only_cell", report_only)):
        if len(set(lst)) != len(lst):
            dup = sorted({x for x in lst if lst.count(x) > 1})
            g.refuse("CRITERIA", "%s rows contain duplicates: %s" % (label, ",".join(dup[:3])))
    return crit, axis, sets, cells, eq, report_only


def derive_group(name, grp, axis):
    """The group's cross product, from its own declared positions x frequencies and the global side and
    sample-rate axes. This is the second, independent statement of what the group contains: a group
    shrunk by editing its `axis_set` row disagrees with the enumeration instead of redefining it."""
    out = set()
    for side in axis.get("sides", []):
        for sr in axis.get("sample_rates", []):
            for f in grp.get("frequencies", []):
                for tag in grp.get("positions", []):
                    out.add("vco_%s_sawmix_%s_%s_%s" % (side, sr, f, tag))
    return out


def parse_plan(g, path):
    rev = None
    ids = []
    for n, f in _rows(path):
        if f[0] == "plan_rev":
            rev = f[1]
        elif f == ["block", "id"]:
            continue
        elif len(f) == 2:
            ids.append(f[1])
        else:
            g.refuse("PLAN-SHAPE", "%s:%d: unexpected plan row: %s" % (path, n, " ".join(f)))
    if rev != PLAN_REV:
        g.refuse("PLAN-SHAPE", "%s: plan_rev=%r, expected %r" % (path, rev, PLAN_REV))
    return ids


def parse_matrix(g, path):
    """One arm's report: the instrument lines and the static matrix, from the SAME file on purpose."""
    text = open(path).read().splitlines()
    begin = end = None
    for i, line in enumerate(text):
        if line.strip() == "-- BEGIN MATRIX static":
            begin = i
        elif line.startswith("-- END MATRIX static"):
            end = i
    if begin is None or end is None or end < begin:
        g.refuse("MATRIX-SHAPE", "%s: no `-- BEGIN/END MATRIX static` fence pair" % path)
        return {}, {}
    decl = {}
    for tok in text[end].split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            decl[k] = v
    heads = [i for i in range(begin + 1, end) if tuple(text[i].split()) == MATRIX_COLS]
    if len(heads) != 1:
        g.refuse("MATRIX-SHAPE", "%s: the pinned header line appeared %d times, expected exactly 1"
                 % (path, len(heads)))
        return {}, decl
    rows = {}
    for i in range(heads[0] + 1, end):
        f = text[i].split()
        if not f:
            continue
        if len(f) != len(MATRIX_COLS):
            g.refuse("MATRIX-SHAPE", "%s:%d: %d columns, expected %d" % (path, i + 1, len(f),
                                                                        len(MATRIX_COLS)))
            continue
        d = {}
        bad = False
        for col, val in zip(MATRIX_COLS[1:], f[1:]):
            try:
                d[col] = float(val)
            except ValueError:
                g.refuse("MATRIX-SHAPE", "%s:%d: %s is not a number: %r" % (path, i + 1, col, val))
                bad = True
        if bad:
            continue
        d["id"] = f[0]
        if d["id"] in rows:
            g.refuse("MATRIX-SHAPE", "%s:%d: duplicate id %s" % (path, i + 1, d["id"]))
            continue
        rows[d["id"]] = d
    return rows, decl


def parse_instrument(g, path):
    text = open(path).read().splitlines()
    for line in text:
        if "***" in line:
            g.refuse("ARMSELFCHECK", "%s: instrument failure marker: %s" % (path, line.strip()))
    for lbl in ("hann", "rect"):
        want = "(e) Parseval absolute, %s window" % lbl
        if not any(want in l and "MEAN SQUARES AGREE (ok)" in l for l in text):
            g.refuse("ARMSELFCHECK", "%s: no OK `%s` Parseval line; the power-unit tripwire did not "
                                     "run" % (path, want))
    align = [l for l in text if l.startswith("ALIGN-GATE ")]
    if not align:
        g.refuse("ARMSELFCHECK", "%s: no ALIGN-GATE line; the alignment gate did not run" % path)
    else:
        kv = dict(tok.partition("=")[::2] for tok in align[-1].split()[1:])
        if kv.get("ok") == "0" and kv.get("checked") == "0":
            g.refuse("ARMSELFCHECK", "%s: the alignment gate checked no cells" % path)
        if kv.get("bad") != "0":
            g.refuse("ARMSELFCHECK", "%s: %s" % (path, align[-1].strip()))


def read_arm_index(path):
    """id -> (raw filename, warm, win, f0_meas_hz) from a probe-written manifest. `f0_meas_hz` is the
    PROBE's own zero-crossing measurement, carried here so the frequency guard reads the instrument
    that did not produce the residual columns under test."""
    with open(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    head = lines[0].split("\t")
    idx = {k: i for i, k in enumerate(head)}
    for need in ("id", "raw", "warm", "win", "f0_meas_hz"):
        if need not in idx:
            raise ValueError("%s: no `%s` column" % (path, need))
    out = {}
    for n, l in enumerate(lines[1:], 2):
        f = l.split("\t")
        if len(f) != len(head):
            raise ValueError("%s:%d: %d fields, header has %d" % (path, n, len(f), len(head)))
        try:
            f0 = float(f[idx["f0_meas_hz"]])
        except ValueError:
            f0 = float("nan")
        out[f[idx["id"]]] = (f[idx["raw"]], int(f[idx["warm"]]), int(f[idx["win"]]), f0)
    return out


def read_window(armdir, raw, warm, win):
    with open(os.path.join(armdir, raw), "rb") as fh:
        buf = fh.read()
    n = len(buf) // 8
    vals = struct.unpack("<%dd" % n, buf[:n * 8])
    return vals[warm:warm + win]


def bit_diff_count(a, b):
    """Frames whose float64 bit pattern differs, or -1 when the two captures are not the same length.
    A length mismatch is NOT compared over the common prefix: it is a refusal, because 'the shorter
    one matched' is not the identity this criterion asserts."""
    if len(a) != len(b):
        return -1
    n = 0
    for x, y in zip(a, b):
        if struct.pack("<d", x) != struct.pack("<d", y):
            n += 1
    return n


def finish(g, verdict, code):
    for label, bucket in (("REFUSE", g.refusals), ("FAIL", g.failures)):
        g.dump(label, bucket)
    g.say("ACCEPT-GATE verdict=%s refusals=%d failures=%d"
          % (verdict, len(g.refusals), len(g.failures)))
    if verdict == "REFUSE":
        g.say("ACCEPT-NOTE no judgement is issued: the artifacts failed an input check, so a red or "
              "green verdict here would not be a statement about the DSP.")
    if g.out:
        with open(g.out, "w") as fh:
            fh.write("\n".join(g.lines) + "\n")
    return code


def pinned_res_db(a_base, a_cand, res_cand):
    """The candidate's residual had it been scored with the BASELINE's fitted scale -- exact, not an
    estimate. res_db = 10*log10(||e||^2/||A r||^2) with e the least-squares residual, so <e,r> = 0 and
    for any other scale A' the numerator obeys ||x - A'r||^2 = ||e||^2 + (A'-A)^2 ||r||^2. Dividing by
    the new denominator A'^2 and writing ||e||^2/||r||^2 = A^2 * 10^(res/10) gives the closed form.

    IT IS EXACT IN ONE BAND AND IS NAMED AFTER THAT BAND. Orthogonality is a property of the inner
    product A was fitted in -- the unwindowed full band, column `res_db`. The analyzer publishes ONE
    scale per cell, so A is not the Hann band's least-squares scale and this identity does not carry
    over to a Hann column. Reporting a "pinned scale" figure for a Hann column would be a number
    computed in a different measurement than the one it is named after, so the pinned figures are the
    RECT pair only and the gap between the two windows is reported rather than assumed away."""
    d = a_base - a_cand
    return (res_cand
            + 10.0 * math.log10(1.0 + (d * d) / ((a_cand * a_cand) * (10.0 ** (res_cand / 10.0))))
            - 20.0 * math.log10(a_base / a_cand))


def main(argv):
    ap = argparse.ArgumentParser(description="GH#19 S6 saw/invSaw production acceptance gate")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--plan", required=True, help="the probe's declared plan (gh19_s6_plan.tsv)")
    ap.add_argument("--base-report", required=True, help="analyzer --out for the baseline arm")
    ap.add_argument("--cand-report", required=True, help="analyzer --out for the candidate arm")
    ap.add_argument("--base-arm", required=True, help="probe --out directory of the baseline arm")
    ap.add_argument("--cand-arm", required=True, help="probe --out directory of the candidate arm")
    ap.add_argument("--out", default=None, help="also write the full transcript here")
    args = ap.parse_args(argv)

    g = Gate(args.out)
    g.say("ACCEPT-GATE tool=gh19_s6_saw_acceptance.py")
    g.say("ACCEPT-GATE criteria=%s" % args.criteria)
    g.say("ACCEPT-GATE base_report=%s" % args.base_report)
    g.say("ACCEPT-GATE cand_report=%s" % args.cand_report)
    g.say("ACCEPT-GATE base_arm=%s" % args.base_arm)
    g.say("ACCEPT-GATE cand_arm=%s" % args.cand_arm)

    for name, path in (("criteria", args.criteria), ("plan", args.plan),
                       ("base_report", args.base_report), ("cand_report", args.cand_report),
                       ("base_arm", args.base_arm), ("cand_arm", args.cand_arm)):
        if not os.path.exists(path):
            g.refuse("INPUT-MISSING", "%s: %s does not exist" % (name, path))
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    crit, axis, sets, cells, eq, report_only = parse_criteria(g, args.criteria)
    plan = parse_plan(g, args.plan)
    base, base_decl = parse_matrix(g, args.base_report)
    cand, cand_decl = parse_matrix(g, args.cand_report)
    parse_instrument(g, args.base_report)
    parse_instrument(g, args.cand_report)

    for name, path, decl in (("base", args.base_report, base_decl),
                             ("cand", args.cand_report, cand_decl)):
        if decl.get("capped") == "1":
            g.refuse("CAPPED", "%s: the reference series was truncated below Nyquist; this is not a "
                               "full-band measurement and the gate refuses it" % path)
        if not (base if name == "base" else cand):
            g.refuse("MATRIX-SHAPE", "%s: no matrix rows parsed" % path)

    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    # ---- RECONCILIATION: plan == enumeration, and the enumeration == the axes -------------------
    if len(set(plan)) != len(plan):
        dup = sorted({x for x in plan if plan.count(x) > 1})
        g.refuse("PLAN-DUPLICATE", "the plan declares %d duplicate ids: %s"
                 % (len(dup), ",".join(dup[:3])))
    want = set(cells) | set(eq) | set(report_only)
    extra = sorted(set(plan) - want)
    missing = sorted(want - set(plan))
    if extra or missing:
        g.refuse("PLAN-TOTAL", "plan vs criteria enumeration: PLAN-EXTRA=%d PLAN-MISSING=%d %s"
                 % (len(extra), len(missing), ",".join((extra + missing)[:3])))
    for name in sorted(sets):
        got = derive_group(name, sets[name], axis)
        if name == "EQ":
            if got != set(eq):
                g.refuse("AXES",
                         "axis_set EQ derives %d cells, the equality_cell enumeration has %d; "
                         "missing=%s extra=%s"
                         % (len(got), len(set(eq)), ",".join(sorted(set(eq) - got)[:3]) or "-",
                            ",".join(sorted(got - set(eq))[:3]) or "-"))
            continue
        if not got or not got <= set(cells):
            g.refuse("AXES", "axis_set %s derives %d cells, %d of which are not `cell` rows"
                     % (name, len(got), len(got - set(cells))))
    a_set = derive_group("A", sets.get("A", {}), axis)
    b_set = derive_group("B", sets.get("B", {}), axis)
    if a_set & b_set:
        g.refuse("AXES", "axis groups A and B overlap in %d cells: %s"
                 % (len(a_set & b_set), ",".join(sorted(a_set & b_set)[:3])))
    if (a_set | b_set) != set(cells):
        g.refuse("AXES", "A U B has %d cells, the `cell` enumeration has %d; missing=%s extra=%s"
                 % (len(a_set | b_set), len(set(cells)),
                    ",".join(sorted(set(cells) - (a_set | b_set))[:3]) or "-",
                    ",".join(sorted((a_set | b_set) - set(cells))[:3]) or "-"))
    if set(cells) & set(eq):
        g.refuse("AXES", "a cell appears in BOTH the improvement and the equality group: %s"
                 % ",".join(sorted(set(cells) & set(eq))[:3]))

    # Every id must DECODE against the pinned id grammar -- this is a statement about the FORM of
    # every id in `want`, gated or not.
    for cid in sorted(want):
        if not CELL_RE.match(cid):
            g.refuse("CELL-ID", "%s does not decode against the pinned id grammar" % cid)
    # AXIS MEMBERSHIP IS A STATEMENT ABOUT THE *GATED* GRID ONLY, and the restriction is not a
    # relaxation: a `report_only_cell` is off-axis BY CONSTRUCTION. The criteria file says so in
    # its own words -- they are "positions the probe renders and the report covers, but which are
    # in NEITHER gated group (the high-frequency set, past the point where a two-point step kernel
    # has a band-limited referent)" -- and their frequencies (3520, 5000 Hz) are deliberately
    # outside the three the gated grid is built from. Applying the `frequencies` axis to them
    # refuses 32 cells the contract requires the report to cover. Measured, not hypothesised: the
    # first run of this gate against the frozen naive head returned exactly that, all 32 at once.
    # Nothing is left unchecked by the restriction: a report_only id is pinned BY NAME in the
    # criteria, its presence in BOTH matrices is required below, `plan == {cell} U {equality_cell}
    # U {report_only_cell}` is required exactly in both directions (so an id present here but
    # absent from the probe's independently written plan is a refusal), and the gated cells are
    # additionally forced onto the axes by the group derivation itself, which rebuilds them from
    # the axis rows rather than trusting the enumeration.
    for cid in sorted(set(cells) | set(eq)):
        m = CELL_RE.match(cid)
        if m and (m.group("sr") not in axis.get("sample_rates", []) or
                  m.group("f0") not in axis.get("frequencies", [])):
            g.refuse("CELL-ID", "%s carries sr=%s f0=%s, which are not on the axes"
                     % (cid, m.group("sr"), m.group("f0")))
    for name, mat, path in (("base", base, args.base_report), ("cand", cand, args.cand_report)):
        for cid in sorted(want):
            if cid not in mat:
                g.refuse("MATRIX-SHAPE", "%s: cell %s is absent from the %s matrix" % (path, cid,
                                                                                       name))
                continue
            m = CELL_RE.match(cid)
            if not m:
                continue
            d = mat[cid]
            for col, wantv in (("sr", float(m.group("sr"))), ("f0", float(m.group("f0"))),
                               ("morph", tag_morph(m.group("tag")))):
                if abs(d[col] - wantv) > 1e-9 * max(abs(wantv), 1.0):
                    g.refuse("MATRIX-SHAPE", "%s/%s: id says %s=%g, matrix says %g"
                             % (path, cid, col, wantv, d[col]))
        for cid in sorted(set(report_only)):
            if cid not in mat:
                g.refuse("MATRIX-SHAPE", "%s: report-only cell %s is absent from the %s matrix"
                         % (path, cid, name))
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    # ---- THE ARMS MUST BE THE SAME EXPERIMENT ---------------------------------------------------
    for cid in sorted(want):
        b, c = base[cid], cand[cid]
        if b["stretch"] != c["stretch"] or abs(b["morph"] - c["morph"]) > 1e-12:
            g.refuse("ARM-PAIRING", "%s: the two arms are not the same cell (stretch %.0f/%.0f)"
                     % (cid, b["stretch"], c["stretch"]))
            break

    # ---- MEASUREMENTS --------------------------------------------------------------------------
    base_idx = read_arm_index(os.path.join(args.base_arm, "gh19_s6_scenarios.tsv"))
    cand_idx = read_arm_index(os.path.join(args.cand_arm, "gh19_s6_scenarios.tsv"))

    def _finite(v):
        return v is not None and math.isfinite(v)

    def cell_ok(cid, mat):
        d = mat[cid]
        return (_finite(d["A"]) and d["A"] > 0.0 and _finite(d["res_db"]))

    # ---- JUDGEMENT: improvement cells ----------------------------------------------------------
    lack = [c for c in cells if not (cell_ok(c, base) and cell_ok(c, cand))]
    if lack:
        g.refuse("MATRIX-SHAPE", "%d improvement cells lack a finite fitted scale or full-band "
                                 "residual in one arm, e.g. %s" % (len(lack), lack[0]))
        return finish(g, "REFUSE", EXIT_REFUSE)

    dres = {c: base[c]["res_db"] - cand[c]["res_db"] for c in cells}
    dband = {c: base[c]["res_1k5k_db"] - cand[c]["res_1k5k_db"] for c in cells}
    dfeff = {c: base[c]["res_effbd_db"] - cand[c]["res_effbd_db"] for c in cells}

    full_min = crit["full_band_min_improvement_db"]
    band_min = crit["band_1k5k_min_improvement_db"]
    max_amp = crit["max_fund_amp_rel_change"]
    max_dc = crit["max_dc_abs_delta"]
    max_cycles = crit["max_freq_shift_cycles_per_window"]

    for cid in sorted(cells):
        if dfeff[cid] < full_min:
            g.fail("IMPROVE-FULL", "%s: full-band improvement %+.3f dB, need >= %+.3f (%.3f -> %.3f)"
                    % (cid, dfeff[cid], full_min, base[cid]["res_effbd_db"], cand[cid]["res_effbd_db"]))
        if dband[cid] < band_min:
            g.fail("IMPROVE-BAND", "%s: 100-5000 Hz improvement %+.3f dB, need >= %+.3f (%.3f -> "
                                   "%.3f)" % (cid, dband[cid], band_min, base[cid]["res_1k5k_db"],
                                              cand[cid]["res_1k5k_db"]))

    # Guard rows: the ways an improvement can be faked, checked on the same cells in the same pass.
    for cid in sorted(cells):
        b, c = base[cid], cand[cid]
        if b["fund"] == 0.0:
            g.refuse("GUARD-AMP", "%s: the baseline fundamental is 0, so a relative change is not "
                                  "defined" % cid)
            continue
        rel = abs(c["fund"] - b["fund"]) / abs(b["fund"])
        if rel > max_amp:
            g.fail("GUARD-AMP", "%s: fundamental amplitude moved %.6f (%.6f -> %.6f), limit %.4f"
                   % (cid, rel, b["fund"], c["fund"], max_amp))
        ddc = abs(c["dc"] - b["dc"])
        if ddc > max_dc:
            g.fail("GUARD-DC", "%s: |DC| changed by %.6f (%.6f -> %.6f), limit %.4f"
                   % (cid, ddc, b["dc"], c["dc"], max_dc))

    # The frequency guard is read from the PROBE's zero-crossing measurement, never from the analyzer
    # whose residual columns are under test.
    for cid in sorted(cells):
        bs, cs = base_idx.get(cid), cand_idx.get(cid)
        if bs is None or cs is None:
            g.fail("GUARD-FREQ", "%s: no scenario row in one of the arms" % cid)
            continue
        if bs[2] != cs[2]:
            g.fail("GUARD-FREQ", "%s: the two arms analysed different window lengths (%d/%d)"
                   % (cid, bs[2], cs[2]))
            continue
        bf0, cf0 = bs[3], cs[3]
        if not (_finite(bf0) and _finite(cf0)):
            g.fail("GUARD-FREQ", "%s: f0_meas_hz is not finite in one of the arms" % cid)
            continue
        bound = max_cycles * base[cid]["sr"] / bs[2]
        shift = abs(cf0 - bf0)
        if shift > bound:
            g.fail("GUARD-FREQ", "%s: zero-crossing frequency moved %.6f Hz (%.6f -> %.6f), bound "
                                 "sr/win = %.6f Hz" % (cid, shift, bf0, cf0, bound))

    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    # ---- JUDGEMENT: the two equality identities ------------------------------------------------
    anti = [c for c in eq if abs(tag_morph(CELL_RE.match(c).group("tag")) - 0.125) < 1e-12]
    sine = [c for c in eq if abs(tag_morph(CELL_RE.match(c).group("tag")) - 0.5) < 1e-12]
    if len(anti) + len(sine) != len(eq):
        g.refuse("MATRIX-SHAPE", "the equality group has %d cells but only %d are at morph 0.125 or "
                                 "0.5" % (len(eq), len(anti) + len(sine)))
        return finish(g, "REFUSE", EXIT_REFUSE)

    anti_worst = max(cand[c]["max_abs_out"] for c in anti)
    anti_worst_base = max(base[c]["max_abs_out"] for c in anti)
    if anti_worst != crit["antiphase_midpoint_max_abs_output"]:
        g.fail("EQUALITY-ANTIPHASE", "anti-phase midpoint worst |output| = %.17g, criterion %.17g"
               % (anti_worst, crit["antiphase_midpoint_max_abs_output"]))
    sine_worst = 0
    for cid in sorted(sine):
        bw, cw = base_idx.get(cid), cand_idx.get(cid)
        if bw is None or cw is None:
            g.refuse("MATRIX-SHAPE", "%s: no manifest row in one of the arms" % cid)
            continue
        a = read_window(args.base_arm, bw[0], bw[1], bw[2])
        b = read_window(args.cand_arm, cw[0], cw[1], cw[2])
        n = bit_diff_count(a, b)
        if n < 0:
            g.refuse("EQUALITY-SINE", "%s: the two arms' windows are not the same length" % cid)
            continue
        if n > sine_worst:
            sine_worst = n
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)
    if sine_worst != crit["sine_node_bit_diff_count"]:
        g.fail("EQUALITY-SINE", "sine-node worst bit-difference count = %d, criterion %d; the "
                                "default output is NOT bit-identical to pre-S6"
               % (sine_worst, crit["sine_node_bit_diff_count"]))

    # ---- REPORTS: emitted before the verdict, because an uncomputable report is a REFUSAL ------
    max_dA = max(abs(cand[c]["A"] - base[c]["A"]) for c in cells)
    costs = [pinned_res_db(base[c]["A"], cand[c]["A"], cand[c]["res_db"]) - cand[c]["res_db"]
             for c in cells]
    max_cost = max(costs)
    worst_band = min(dband[c] for c in cells)
    worst_full = min(dfeff[c] for c in cells)
    worst_rect = min(dres[c] for c in cells)
    worst_pinned = min(pinned_res_db(base[c]["A"], cand[c]["A"], cand[c]["res_db"])
                       - base[c]["res_db"] for c in cells)
    got_reports = {
        "max_abs_fitted_scale_delta": "%.6f" % max_dA,
        "max_pinned_scale_cost_db": "%+.4f" % max_cost,
        "worst_band_1k5k_improvement_db": "%+.2f" % worst_band,
        "worst_full_band_improvement_db": "%+.2f" % worst_full,
        "worst_pinned_scale_rect_full_band_improvement_db": "%+.2f" % worst_pinned,
        "worst_rect_full_band_improvement_db": "%+.2f" % worst_rect,
        "antiphase_midpoint_worst_abs_output": "%.17g" % anti_worst,
        "sine_node_worst_bit_diff_count": "%d" % sine_worst,
    }
    for name in REQUIRED_REPORTS:
        if name not in got_reports or got_reports[name] is None:
            g.refuse("REPORT-MISSING", "required report %s could not be computed" % name)
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)
    for name in REQUIRED_REPORTS:
        g.say("ACCEPT-REPORT %s=%s" % (name, got_reports[name]))
    g.say("ACCEPT-REPORT antiphase_midpoint_worst_abs_output_base=%.17g" % anti_worst_base)
    g.say("ACCEPT-REPORT n_improvement=%d n_equality=%d n_report_only=%d"
          % (len(cells), len(eq), len(report_only)))
    for cid in sorted(anti):
        g.say("ACCEPT-EQUALITY anti %s base_max_abs=%.17g cand_max_abs=%.17g"
              % (cid, base[cid]["max_abs_out"], cand[cid]["max_abs_out"]))

    if g.failures:
        return finish(g, "RED", EXIT_RED)
    return finish(g, "PASS", EXIT_PASS)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
