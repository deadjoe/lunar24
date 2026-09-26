#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# GH#19 S6 PR-B (task #121): THE acceptance gate for the MOVING-DUTY (dynamic-PWM) slice.
#
# WHAT THIS IS. It reads the artifacts the pinned tools already produce and answers two separate
# questions in a fixed order:
#
#   REFUSE first -- are these the artifacts the contract describes, and is the reference on every cell
#   sharp enough for a residual to mean anything? A cell that fails here gets a NAMED REFUSAL and NO
#   verdict. Refusing is not a verdict about the product: it says this instrument, on this cell, is not
#   entitled to an opinion. (exit 4, no judgement issued at all)
#
#   JUDGE second -- given that every precondition held, is the corrected arm's residual an improvement
#   over the neutralised arm's, is the static route still a special case of the dynamic route, and is
#   the improvement column immune to the tail extrapolation? (exit 1 red, exit 0 green)
#
# The order is the whole design. A red verdict computed from artifacts that failed an input check would
# be indistinguishable from a red verdict about the DSP, and the negative controls depend on that
# distinction: the mutant leg asserts the neutralised arm goes red BY NAME and is NOT a refusal.
#
# THE FLOOR. Every judged number is a residual against a reference built on an oversampled grid. A
# reference that is not sharp enough contaminates the residual it is used to measure, and the
# contamination is INVISIBLE in the residual itself -- it makes the product look better, never worse.
# So the reference's own movement between two resolution levels (R1: taps proportional to L) must be
# shown to sit a MARGIN below the residual before the cell may be judged.
#
# *** THE ONE TRAP THIS FILE EXISTS TO NOT FALL INTO ***
# The analyzer's PER-CELL table has a column literally named `floor_margin_db`, and in that table it is
# the BAND-axis margin (res_effbd_db - refconv_rms_db). The JUDGED-axis margin appears only in the
# analyzer's summary block. Reading the per-cell column because of its name would turn 0 refusals into
# 3, on three real cells. So this gate COMPUTES the judged margin from the primary columns
# (res_db - refconv_rms_db) and never selects a column by its name. See the pinned criteria header.
#
# WHY SELECT BY `path` AND COMPARE BY EXACT ID. 66 scenario ids contain the substring `dynpwm`; only 56
# have a dynpwm path. Selecting by name prefix silently admits 10 extra cells, 2 of which are id-shaped
# exactly like in-scope moving-duty cells. The criteria file declares those 10 out of scope BY NAME.
#
# NO AGGREGATE. The arithmetic is per cell and never averaged: a cell that fails is named with its
# measured value, and there is no median, no mean, and no threshold that can be relaxed from the
# command line. `--improvement-min-db` does not exist and must not be added.

import argparse
import hashlib
import math
import os
import re
import struct
import sys

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

# Pinned from tools/gh19_s3_pulse_analyze.py's dynamic block. Kept as a literal rather than imported so
# that a change to the analyzer's column order is a REFUSAL here (the report no longer parses against
# the expectation) instead of being silently absorbed by a positional read.
DYNAMIC_COLS = ("id", "sr", "f0", "A", "fund", "dc", "res_db",
                "res_1k5k_db", "res_effbd_db", "refconv_rms_db", "refconv_peak",
                "res_1k5k2L_db", "res_effbd2L_db", "n")

# The constructed-cell arm's manifest name. Like the arm's own `gh19_s3_scenarios.tsv`, the producer's
# manifest name is part of its contract, so it is pinned here as a literal: a driver that renamed its
# table would be a REFUSAL (this gate can no longer find the cells) rather than a silently empty leg.
DEGEN_MANIFEST = "gh19_prb_degen_scenarios.tsv"

# Every criterion key, split by the type the criteria file must give it. Splitting the types is what
# makes "no unexpected keys" a real check: a numeric threshold that lost its decimal point would
# otherwise be rejected as a string rather than silently ignored.
NUM_CRITERIA = ("floor_margin_min_db", "max_res_db_contamination_db",
                "oversample_L", "taps_L", "oversample_L2", "taps_L2", "taps_ratio_min",
                "decimator_cutoff", "edge_eff_expected", "trace_min_frames",
                "window_warm_frames", "window_len_frames", "samples_frames", "peak",
                "improvement_min_db", "degen_nonvacuous_required",
                "require_skipped_zero", "require_unreferenced_zero")

STR_CRITERIA = ("gate_code_prefix", "refuse_class_prefix", "fail_class_prefix",
                "code_floor_missing", "code_floor_unresolved", "code_trace", "code_band",
                "code_cellset", "code_improvement", "code_degen_identity", "code_degen_nonvacuous",
                "code_missing_arm",
                # The rest of the vocabulary. These seven suffixes are emitted at call sites that pass
                # a bare literal, which is fine -- but until 2026-09-25 no `code_*` row existed for
                # them, so the criteria file could not rename them and a reader could not see that they
                # were part of the vocabulary at all. Naming every suffix the gate can emit makes the
                # criteria' completion claim checkable instead of half-true (ruled by the director).
                "code_criteria", "code_plan", "code_matrix_shape", "code_matrix_exit",
                "code_scenarios", "code_trace_shape", "code_window",
                "floor_margin_axis", "floor_margin_band_axis_reported_only", "improvement_axis",
                "improvement_provenance", "scale_delta_provenance",
                "degen_identity_compare", "negcontrol_degen_dispatch_arm",
                # The negative controls are pinned as SEPARATE fields rather than one conflated cell:
                # the arm name says WHO the control is, the class says WHAT it demonstrates, the exit
                # bit says HOW that demonstration is detected mechanically, and the tamper pair says
                # what falsifies the identity. All are REQUIRED (see the presence loop below) -- a
                # criteria file that lost one would otherwise lose a control silently.
                "negcontrol_degen_dispatch_class", "negcontrol_degen_dispatch_expect_exit_bit",
                "negcontrol_degen_identity_tamper", "negcontrol_degen_identity_tamper_expect_token",
                "degen_driver_stimulus_exit_bit", "degen_driver_premise_exit_bit")

# Quantities the authorization says must be REPORTED rather than gated. Required, so "reported
# separately" cannot become "not reported"; never given a threshold, because inventing one for a
# quantity the authorization left unbounded is exactly the defect the audit named.
REQUIRED_REPORTS = ("refconv_rms_db", "refconv_peak", "res_effbd_db", "res_effbd2L_db",
                    "report_floor_margin_db", "report_floor_margin_band_db",
                    "margin_source_column", "worst_floor_margin_db", "refused_cells",
                    "improvement_corrected_db", "improvement_neutralized_db",
                    "degen_moving_duty_measured_delta", "max_abs_fitted_scale_delta")

REQUIRED_AXES = ("plan_block", "scenarios_path", "out_of_scope_path", "lfo_wave", "sample_rates_hz",
                 "frequencies_hz", "duty_param", "pw_depth", "morph", "warm_frames", "win_frames",
                 "cell_count", "degen_pair_count")

# The analyzer's own line shape. The label is free; the exit code is not.
EXIT_RE = re.compile(r"^== .*: exit_code=(-?\d+) ==$")
FENCE_RE = re.compile(r"^-- END MATRIX dynamic declared=(\d+) emitted=(\d+) skipped=(\d+) "
                      r"unreferenced=(\d+)$")
BAND_RE = re.compile(r"^\s+BAND-DECL dynamic\b.*$")

# The gate cell id decodes to the axes that claim it. A cell whose id does not decode is not the cell
# the contract names, whatever its row says.
#
# *** `b` IS THE BASE WIDTH AND `d` IS THE MODULATION DEPTH -- NOT the other way round. ***
# Whichever way this is read, it looks plausible: both are percentages, both are 0.25/0.5/1.0-shaped,
# and the ids that expose the difference are the minority. The definition is the probe's own id builder
# (tests/probes/gh19_s3_pulse_probe.cpp:178-185), which appends `_b` + pwBase*100 and then `_d` +
# pwDepth*100. It was checked against the scenario table on all 56 cells: reading b as duty_param and d
# as pw_depth agrees on 56/56, and the swapped reading fits 0. Only the cells whose base and depth differ
# (b25_d100) discriminate at all, so a spot check on a b50_d50 cell would have confirmed either answer.
# `r` is the compiled-in LFO rate, which is NOT re-derivable from any manifest column -- five PAIRS of
# cells agree in every scenario-table column and differ only here. That is why the set is an
# enumeration, and why the decode below is checked against the manifest rather than used to build it.
CELL_RE = re.compile(r"^vco_(?P<side>[ab])_dynpwm_(?P<sr>\d+)_(?P<f0>\d+)_(?P<wave>tri|sq)"
                     r"_r(?P<r>\d+)_b(?P<duty>\d+)_d(?P<depth>\d+)$")

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
        self.out = None
        self.code_prefix = "PRB"
        self.refuse_prefix = "PRB-REFUSE-"
        self.fail_prefix = "PRB-FAIL-"
        # Default suffix -> the suffix actually printed, built from the criteria at load time. EMPTY
        # until then, and an empty map is the identity (`get(s, s)`), which is why this wiring cannot
        # change a byte of this gate's output while the criteria holds the default values.
        self.codes = {}
        self.reports = {}

    def say(self, *parts):
        line = " ".join(str(p) for p in parts)
        self.lines.append(line)
        print(line)

    def refuse(self, suffix, detail):
        # Mapped HERE and not at print time: the recorded suffix is what the detail line prints
        # (_report) AND what the counts line counts and prints (finish). Mapping once at the single
        # point where a code is recorded keeps those two surfaces describing the same string, which is
        # the property the counts line exists to provide.
        suffix = self.codes.get(suffix, suffix)
        self.refusals.append((suffix, detail))
        self._report("REFUSE", self.refuse_prefix, suffix, detail)

    def fail(self, suffix, detail):
        suffix = self.codes.get(suffix, suffix)
        self.failures.append((suffix, detail))
        self._report("FAIL", self.fail_prefix, suffix, detail)

    def report(self, key, value):
        """A quantity the authorization requires to be PRINTED. Never given a threshold here."""
        self.reports[key] = value

    def _report(self, bucket, prefix, suffix, detail):
        n = sum(1 for c, _ in (self.refusals if bucket == "REFUSE" else self.failures) if c == suffix)
        if n <= MAX_DETAIL:
            self.say("%s%s %s" % (prefix, suffix, detail))
        elif n == MAX_DETAIL + 1:
            self.say("%s%s ... further examples suppressed; the counts line carries the total" %
                     (prefix, suffix))


# --------------------------------------------------------------------------------------- parsing

def _rows(path, g=None, code="INPUT"):
    """Data rows of a tab-separated file, comments and blanks dropped, line number kept.

    Decoded as UTF-8 explicitly: without it Python uses the locale's preferred encoding, which is
    cp1252 on a Windows runner -- and a report that decoded differently on one platform would make the
    gate's verdict a function of the runner's locale. No `errors=` is passed on purpose: a byte this
    file cannot decode is a broken input and must raise, not be silently replaced with U+FFFD and then
    fed to a criterion.
    """
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            out.append((n, line, line.split("\t")))
    return out


def _num(g, code, path, line, col, raw):
    """A finite float, or None for the declared absence `-`. Anything else is a refusal.

    This deliberately does NOT fall back to `float()` in the caller: a NaN compared with `>=` is
    False, so a NaN would silently switch OFF whichever check it feeds -- which is the one failure
    those checks exist to catch.
    """
    if raw == "-":
        return None
    try:
        v = float(raw)
    except ValueError:
        g.refuse(code, "%s:%d: column %s is not a number: %r" % (path, line, col, raw))
        return None
    if not math.isfinite(v):
        g.refuse(code, "%s:%d: column %s is not finite: %r" % (path, line, col, raw))
        return None
    return v


def parse_criteria(g, path):
    """The pinned expectation. Strict in both directions: a missing key and an unexpected key both
    refuse, so a criteria file that quietly lost a threshold cannot pass as a smaller contract."""
    num, sval, axis, cells, pairs, reports = {}, {}, {}, [], [], set()
    for n, _line, f in _rows(path, g, "CRITERIA"):
        if len(f) < 2:
            g.refuse("CRITERIA", "%s:%d: row has fewer than 2 fields" % (path, n))
            continue
        key = f[0]
        if key == "criterion":
            if len(f) != 3:
                g.refuse("CRITERIA", "%s:%d: criterion row needs exactly 3 fields" % (path, n))
                continue
            k, v = f[1], f[2]
            if k in num or k in sval:
                g.refuse("CRITERIA", "%s:%d: duplicate criterion %s" % (path, n, k))
                continue
            if k in NUM_CRITERIA:
                try:
                    num[k] = float(v)
                except ValueError:
                    g.refuse("CRITERIA", "%s:%d: %s must be numeric: %r" % (path, n, k, v))
            elif k in STR_CRITERIA:
                sval[k] = v
            else:
                g.refuse("CRITERIA", "%s:%d: unknown criterion key %r" % (path, n, k))
        elif key == "axis":
            if len(f) != 3:
                g.refuse("CRITERIA", "%s:%d: axis row needs exactly 3 fields" % (path, n))
                continue
            if f[1] in axis:
                g.refuse("CRITERIA", "%s:%d: duplicate axis %s" % (path, n, f[1]))
                continue
            axis[f[1]] = [v for v in f[2].split(",") if v]
        elif key in ("cell", "degen_pair"):
            if len(f) != (2 if key == "cell" else 4):
                g.refuse("CRITERIA", "%s:%d: %s row has %d fields, expected %d" %
                         (path, n, key, len(f), 2 if key == "cell" else 4))
                continue
            (cells if key == "cell" else pairs).append(tuple(f[1:]))
        elif key == "report":
            if len(f) != 2:
                g.refuse("CRITERIA", "%s:%d: report row needs exactly 2 fields" % (path, n))
                continue
            if f[1] in reports:
                g.refuse("CRITERIA", "%s:%d: duplicate report %s" % (path, n, f[1]))
            reports.add(f[1])
        else:
            g.refuse("CRITERIA", "%s:%d: unknown row key %r" % (path, n, key))

    for name, have, want in (("criterion/numeric", num, NUM_CRITERIA),
                             ("criterion/string", sval, STR_CRITERIA),
                             ("axis", axis, REQUIRED_AXES)):
        missing = [k for k in want if k not in have]
        extra = sorted(k for k in have if k not in want)
        if missing or extra:
            g.refuse("CRITERIA", "%s keys missing=%s unexpected=%s" %
                     (name, ",".join(missing) or "-", ",".join(extra) or "-"))
    missing = [k for k in REQUIRED_REPORTS if k not in reports]
    extra = sorted(k for k in reports if k not in REQUIRED_REPORTS)
    if missing or extra:
        g.refuse("CRITERIA", "report keys missing=%s unexpected=%s" %
                 (",".join(missing) or "-", ",".join(extra) or "-"))

    for name, seq in (("cell", [c[0] for c in cells]), ("degen_pair", [p[0] for p in pairs])):
        dup = sorted({v for v in seq if seq.count(v) > 1})
        if dup:
            g.refuse("CRITERIA", "%s: duplicate ids %s" % (name, ",".join(dup)))
    if not cells:
        g.refuse("CRITERIA", "no cell rows at all")

    g.code_prefix = sval.get("gate_code_prefix", g.code_prefix)
    g.refuse_prefix = sval.get("refuse_class_prefix", g.refuse_prefix)
    g.fail_prefix = sval.get("fail_class_prefix", g.fail_prefix)
    # THE CODE VOCABULARY IS THE CRITERIA'S. Every `code_*` row renames the suffix this file writes
    # below, keyed by that suffix: `code_floor_missing` -> `FLOOR-MISSING`, `code_degen_identity` ->
    # `DEGEN-IDENTITY`. The derivation is stated once, as a rule, rather than as a second table that
    # could drift from the call sites -- a duplicated encoding is exactly what that would be.
    # Until 2026-09-25 the `code_*` keys were DECLARED AND NEVER READ: renaming one changed this gate's
    # output not at all, while the criteria file's header promised that this gate READS it. Wiring them
    # makes the promise true -- and the same ruling required the vocabulary to be COMPLETE, all sixteen
    # suffixes rather than the nine that happened to be reached through a variable, since an unnamed
    # half is a half no reader can see or rename. The wiring is also the identity on the shipped criteria (defaults equal
    # the suffixes), so its zero-disturbance is checkable by diffing this gate's own transcript -- and
    # that check alone cannot tell a live read from a dead one, so a liveness control accompanies it
    # (see tools/gh19_prb_dynpwm_gate_negcontrol.py, `criteria_code_key_is_read`).
    for key, value in sval.items():
        if key.startswith("code_"):
            g.codes[key[len("code_"):].replace("_", "-").upper()] = value
    return num, sval, axis, [c[0] for c in cells], pairs, reports


PLAN_HEADER = ("block", "id")


def parse_plan(g, path, block):
    """The probe's declared plan: `plan_rev\\tN`, the header `block\\tid`, then `block\\tid` rows.

    The header is required rather than assumed, so a file that is merely id-shaped cannot pass for a
    plan, and the column order is the file's own declaration rather than a guess.
    """
    header_seen = False
    blocks = {}
    for n, _line, f in _rows(path, g, "PLAN"):
        if f[0] == "plan_rev":
            continue
        if tuple(f) == PLAN_HEADER:
            if header_seen:
                g.refuse("PLAN", "%s:%d: repeated header" % (path, n))
            header_seen = True
            continue
        if not header_seen:
            g.refuse("PLAN", "%s:%d: row before the header" % (path, n))
            continue
        if len(f) != 2:
            g.refuse("PLAN", "%s:%d: needs exactly 2 fields" % (path, n))
            continue
        blocks.setdefault(f[0], []).append(f[1])
    if not header_seen:
        g.refuse("PLAN", "%s: no `block\\tid` header" % path)
    ids = blocks.get(block, [])
    dup = sorted({v for v in ids if ids.count(v) > 1})
    if dup:
        g.refuse("PLAN", "%s: duplicate ids in block %s: %s" % (path, block, ",".join(dup)))
    return ids


def parse_dynamic_matrix(g, path):
    """The analyzer's dynamic block: the declared fence, the band declaration, and the per-cell rows.

    Everything between the BEGIN and END fences is read, and the fence's own counts are reconciled
    against what was actually read. The analyzer prints its `unreferenced` list INSIDE the block on
    purpose; a parser that reconciles only the rows it could parse would silently drop the referenced
    subset itself, which is the failure the fence exists to prevent.
    """
    rows, fence, edge_L, edge_2L, exit_code = {}, None, None, None, None
    header, inside = None, False
    for n, line, f in _rows(path, g, "REPORT"):
        m = EXIT_RE.match(line)
        if m:
            exit_code = int(m.group(1))
            continue
        if line.startswith("-- BEGIN MATRIX dynamic"):
            inside = True
            continue
        if FENCE_RE.match(line):
            g_ = FENCE_RE.match(line)
            fence = tuple(int(x) for x in g_.groups())
            inside = False
            continue
        flat = line.strip()
        if inside and header is None and flat.split()[:1] == ["id"]:
            header = tuple(flat.split())
            if header != DYNAMIC_COLS:
                g.refuse("MATRIX-SHAPE", "%s:%d: dynamic header is %s, pinned shape is %s" %
                         (path, n, ",".join(header), ",".join(DYNAMIC_COLS)))
            continue
        if BAND_RE.match(line):
            for tok in flat.split():
                if tok.startswith("edge_L="):
                    edge_L = float(tok.split("=", 1)[1])
                elif tok.startswith("edge_2L="):
                    edge_2L = float(tok.split("=", 1)[1])
            continue
        if inside and header is not None:
            v = flat.split()
            if len(v) != len(DYNAMIC_COLS):
                g.refuse("MATRIX-SHAPE", "%s:%d: dynamic row has %d columns, expected %d" %
                         (path, n, len(v), len(DYNAMIC_COLS)))
                continue
            cid = v[0]
            if cid in rows:
                g.refuse("MATRIX-SHAPE", "%s:%d: duplicate cell %s" % (path, n, cid))
                continue
            rec = dict(zip(DYNAMIC_COLS, v))
            rec["_line"], rec["_raw"] = n, v
            for col in DYNAMIC_COLS[1:]:
                rec[col] = _num(g, "MATRIX-SHAPE", path, n, col, rec[col])
            rows[cid] = rec

    if fence is None:
        g.refuse("MATRIX-SHAPE", "%s: no `-- END MATRIX dynamic` fence" % path)
    if header is None:
        g.refuse("MATRIX-SHAPE", "%s: no dynamic column header" % path)
    if exit_code is None:
        g.refuse("MATRIX-SHAPE", "%s: no `== ...: exit_code=N ==` line" % path)
    elif exit_code != 0:
        g.refuse("MATRIX-EXIT", "%s: the analyzer exited %d; its own checks did not all pass" %
                 (path, exit_code))
    if edge_L is None or edge_2L is None:
        g.refuse("MATRIX-SHAPE", "%s: no BAND-DECL line with edge_L/edge_2L" % path)
    return rows, fence, (edge_L, edge_2L)


def parse_scenarios(g, path):
    """The renderer's scenario table. Selection is by `path` and only by `path` -- see the header."""
    out = {}
    header = None
    for n, _line, f in _rows(path, g, "SCENARIOS"):
        if header is None and f[:1] == ["id"]:
            header = f
            continue
        if header is None:
            g.refuse("SCENARIOS", "%s:%d: row before the header" % (path, n))
            continue
        if len(f) != len(header):
            g.refuse("SCENARIOS", "%s:%d: %d columns, header has %d" % (path, n, len(f), len(header)))
            continue
        rec = dict(zip(header, f))
        if rec["id"] in out:
            g.refuse("SCENARIOS", "%s:%d: duplicate id %s" % (path, n, rec["id"]))
        out[rec["id"]] = rec
    if header is None:
        g.refuse("SCENARIOS", "%s: no header" % path)
    return out


def arm_render_bytes(g, table, base, cell, what, code):
    """The PRODUCT'S RENDERED OUTPUT for `cell`, as raw bytes, read through the arm's own scenario table.

    The degenerate-identity criterion is a statement about the product's OUTPUT (criteria section 5:
    "the dynamic route's output must be BYTE-IDENTICAL to the static route's output"), so this leg reads
    the render the probe wrote and compares it byte for byte. It deliberately does NOT compare the
    analyzer's per-cell metric row: those are derived quantities computed over the analysis WINDOW, so
    two renders differing only in the warm-up, or outside the window, would still produce identical rows
    -- and a check named `byte_identical` that can pass on renders that are not byte-identical is
    claiming more than it measures, which is worse than not having the check.

    Resolving the path through the table (rather than guessing a filename) is what makes the row's
    `raw` column load-bearing: a table pointing at the wrong file is compared, and fails, instead of
    being bypassed by a name this gate invented.
    """
    row = table.get(cell)
    if row is None:
        g.refuse("CELLSET", "%s %s is absent from the scenario table read out of %s: the render cannot "
                            "be compared" % (what, cell, base))
        return None
    name = row.get("raw")
    if not name or name == "-":
        g.refuse("CELLSET", "%s %s: the table names no render (`raw`=%r)" % (what, cell, name))
        return None
    path = os.path.join(base, name)
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        g.refuse(code, "%s %s: cannot read the render %s (%s)" %
                 (what, cell, path, exc.__class__.__name__))
        return None


def max_abs_sample_delta(a, b):
    """Largest |sample| difference between two renders, or None if they are not the same length.

    A length mismatch is NOT reported as a small difference: two renders of different lengths are a
    different kind of fact from two renders that agree everywhere but here."""
    if len(a) != len(b) or len(a) % 8 != 0:
        return None
    n = len(a) // 8
    xa = struct.unpack("<%dd" % n, a)
    xb = struct.unpack("<%dd" % n, b)
    return max(abs(p - q) for p, q in zip(xa, xb)) if n else None


def trace_frames(g, path):
    """Data rows of a recorded duty trace, excluding its `frame\\t...` header."""
    n = -1
    header = False
    for _n, _line, f in _rows(path, g, "TRACE"):
        if not header:
            if f[:1] == ["frame"]:
                header = True
                n = 0
                continue
            g.refuse("TRACE-SHAPE", "%s: first row is not a `frame` header" % path)
            return None
        n += 1
    if not header:
        g.refuse("TRACE-SHAPE", "%s: empty" % path)
        return None
    return n


def decode_cell(g, code, cid):
    m = CELL_RE.match(cid)
    if not m:
        g.refuse(code, "cell id does not decode: %s" % cid)
        return None
    d = m.groupdict()
    return {"side": d["side"], "sr": int(d["sr"]), "f0": int(d["f0"]), "wave": d["wave"],
            "depth": int(d["depth"]) / 100.0, "duty": int(d["duty"]) / 100.0}


# ------------------------------------------------------------------------------------- the gate

def check_fence(g, name, fence, expect_declared, parsed=None):
    """Every count the fence carries, reconciled -- including against the rows actually in the file.
    `skipped` and `unreferenced` are pinned to zero by the criteria: a cell with no source trace is
    counted `skipped` but STAYS INSIDE the analyzed subset, so a run that simply raised the budget
    without supplying the trace would move a hole from `unreferenced` to `skipped` and look identical
    to a reader. Both faces are checked.

    `parsed` is the number of data rows this gate read between the fences, and comparing it to
    `emitted` is the half that was missing until 2026-09-25. `emitted != declared` compares the fence
    to ITSELF -- both numbers come off the same line -- so it can never notice a row deleted from the
    file, and until this check existed a matrix missing a row was reconciled clean. The analyzer's
    producer-side invariant is `emitted + skipped == declared` and a skipped cell prints `no source
    trace` instead of a data row, so on any run this gate does not already refuse, `parsed` must equal
    `emitted` exactly.
    """
    if fence is None:
        return
    declared, emitted, skipped, unreferenced = fence
    if declared != expect_declared:
        g.refuse("CELLSET", "%s: fence declared=%d but the criteria enumerate %d cells" %
                 (name, declared, expect_declared))
    if emitted != declared:
        g.refuse("MATRIX-SHAPE", "%s: fence declared=%d emitted=%d" % (name, declared, emitted))
    if parsed is not None and parsed != emitted:
        g.refuse("MATRIX-SHAPE", "%s: the fence declares emitted=%d but %d data rows were read "
                                 "between the fences; the fence's own count is not reconciled against "
                                 "the file, so a row removed from the matrix would otherwise go "
                                 "unnoticed here" % (name, emitted, parsed))
    if skipped != 0:
        g.refuse("TRACE", "%s: fence skipped=%d; a skipped cell is inside the subset and has no trace" %
                 (name, skipped))
    if unreferenced != 0:
        g.refuse("CELLSET", "%s: fence unreferenced=%d; the declared set is not the analyzed set" %
                 (name, unreferenced))


def check_floor_and_shape(g, name, rows, cells, crit, arm, scn, trace_min):
    """Per-cell refusal checks. The judged margin is COMPUTED here from the primary columns."""
    margins, band_margins, missing_floor = [], [], []
    for cid in cells:
        rec = rows.get(cid)
        if rec is None:
            g.refuse("CELLSET", "%s: cell %s has no dynamic row" % (name, cid))
            continue
        dec = decode_cell(g, "CELLSET", cid)
        if dec is None:
            continue
        ln = rec["_line"]
        for col, want in (("sr", dec["sr"]), ("f0", dec["f0"])):
            if rec[col] is not None and abs(rec[col] - want) > 1e-9:
                g.refuse("MATRIX-SHAPE", "%s/%s: id says %s=%d, row says %.6f" %
                         (name, cid, col, want, rec[col]))
        if rec["n"] is not None and rec["n"] != crit["window_len_frames"]:
            g.refuse("WINDOW", "%s/%s: n=%g but the criteria pin window_len_frames=%g; a window that "
                               "changed length is a changed metric, not a re-run" %
                     (name, cid, rec["n"], crit["window_len_frames"]))

        if rec["res_db"] is None or rec["refconv_rms_db"] is None:
            # `-` in refconv_rms/peak is the declared absence of a reference for this cell.
            g.refuse("FLOOR-MISSING", "%s/%s: no reference movement was measured (refconv_rms_db=-)" %
                     (name, cid))
            missing_floor.append(cid)
            continue
        # *** computed, never read by column name ***
        margin = rec["res_db"] - rec["refconv_rms_db"]
        margins.append((margin, cid))
        if rec["res_effbd_db"] is not None:
            band_margins.append((rec["res_effbd_db"] - rec["refconv_rms_db"], cid))
        if margin < crit["floor_margin_min_db"]:
            g.refuse("FLOOR-UNRESOLVED",
                     "%s/%s: reference movement %.2f dB is only %.2f dB below the residual "
                     "(res_db %.2f - floor %.2f); the criteria require %.1f dB, so this cell's numbers "
                     "would measure the reference, not the product" %
                     (name, cid, rec["refconv_rms_db"], margin, rec["res_db"],
                      rec["refconv_rms_db"], crit["floor_margin_min_db"]))

        # trace coverage: the recorded duty track must cover the WHOLE window the residual is taken
        # over. Necessary, not sufficient: the FIR needs a few frames past the window end and the
        # analyzer clamps, which the tail-poison control bounds separately.
        s = scn.get(cid)
        if s is None:
            g.refuse("TRACE", "%s/%s: absent from the scenario table" % (name, cid))
            continue
        # The id decode is checked against the manifest instead of being trusted, so the `b`/`d` reading
        # cannot be wrong in a way nobody notices: a swapped reading survives every b50_d50 cell and dies
        # on the first b25_d100 one. Cheap, and it makes the decode above load-bearing rather than
        # decorative -- a wrong decode with no consumer is a trap laid for the next edit.
        for tok, col, want in (("duty", "duty_param", dec["duty"]), ("depth", "pw_depth", dec["depth"])):
            if s.get(col) is not None and abs(float(s[col]) - want) > 1e-9:
                g.refuse("CELLSET", "%s/%s: id token %s implies %s=%.4f, the manifest says %s" %
                         (name, cid, tok, col, want, s[col]))
        if s.get("lfo_wave") is not None and s["lfo_wave"] != dec["wave"]:
            g.refuse("CELLSET", "%s/%s: id says lfo_wave=%s, the manifest says %s" %
                     (name, cid, dec["wave"], s["lfo_wave"]))
        src = s.get("src")
        if not src or src == "-":
            g.refuse("TRACE", "%s/%s: scenario row names no duty trace (src=%r)" % (name, cid, src))
            continue
        tpath = os.path.join(arm, src)
        if not os.path.isfile(tpath):
            g.refuse("TRACE", "%s/%s: trace file %s is not on disk" % (name, cid, src))
            continue
        got = trace_frames(g, tpath)
        if got is not None and got < trace_min:
            g.refuse("TRACE", "%s/%s: trace %s has %d frames, the window needs %d" %
                     (name, cid, src, got, trace_min))
    return margins, band_margins, missing_floor


def run(g, args):
    crit, sval, axis, cells, pairs, reports = parse_criteria(g, args.criteria)

    # REFUSE BEFORE JUDGING, applied to the criteria itself. A criteria file that failed to load is not
    # the contract, so nothing below it can be judged against it -- and until 2026-09-25 the judging
    # code ran anyway: a dropped `floor_margin_min_db` row was correctly refused by the presence check
    # above and then indexed at the floor comparison, so the run died with `KeyError` and exit 1
    # instead of a named refusal and REFUSE(4). Exit 1 is this tool's RED verdict, so a gate that had
    # in fact issued no verdict at all was reported to the caller as a failed product.
    #
    # One guard is sufficient here and it is worth saying why rather than leaving it to be re-derived:
    # `crit` and `sval` are built only by parse_criteria, every required key is checked for presence
    # there, and no other code writes them. So an absent key is always recorded as a refusal before
    # this line, and every index below is reached only with a complete criteria.
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    if cells and axis.get("cell_count") and axis["cell_count"][0] != str(len(cells)):
        g.refuse("CELLSET", "axis cell_count=%s but the enumeration has %d rows" %
                 (axis["cell_count"][0], len(cells)))
    if axis.get("degen_pair_count") and axis["degen_pair_count"][0] != str(len(pairs)):
        g.refuse("CELLSET", "axis degen_pair_count=%s but there are %d degen_pair rows" %
                 (axis["degen_pair_count"][0], len(pairs)))
    if sval.get("floor_margin_axis") != "res_db":
        g.refuse("CRITERIA", "floor_margin_axis=%r; this gate computes that margin only from res_db" %
                 sval.get("floor_margin_axis"))
    if sval.get("floor_margin_band_axis_reported_only") not in DYNAMIC_COLS:
        g.refuse("CRITERIA", "the declared companion band axis %r is not a dynamic column" %
                 sval.get("floor_margin_band_axis_reported_only"))
    if crit.get("require_skipped_zero") != 1 or crit.get("require_unreferenced_zero") != 1:
        g.refuse("CRITERIA", "require_skipped_zero=%s require_unreferenced_zero=%s; both must be 1" %
                 (crit.get("require_skipped_zero"), crit.get("require_unreferenced_zero")))
    if sval.get("improvement_axis") != "res_db":
        g.refuse("CRITERIA", "improvement_axis=%r; this gate computes the improvement only on res_db" %
                 sval.get("improvement_axis"))
    # WHERE THE FLOOR CAME FROM. The rule used to be "a borrowed threshold must say so", and the only
    # accepted marker was `borrowed_from_*`. The floor is no longer borrowed -- it was re-derived from
    # this slice's own 56-cell survey, because 6.0 was refuted by measurement (three cells at 5.79, 4.70
    # and 5.79 dB). So there are now exactly two admissible answers, and the check has to keep refusing
    # the third: a `derived_from_` value naming anything else would be a way to stop saying where the
    # number came from by asserting it came from somewhere, which is what the original rule existed to
    # prevent. Accepted: a `borrowed_from_*` marker (the old rule, unchanged), or the one surveyed
    # value, which names the survey this slice ran AND is in evidence.
    prov = sval.get("improvement_provenance") or ""
    if not (prov.startswith("borrowed_from_") or prov == "derived_from_prb_survey"):
        g.refuse("CRITERIA", "improvement_min_db=%g does not declare where it came from "
                             "(improvement_provenance=%r); a borrowed threshold must say so, and a "
                             "derived one must name the survey it was derived from" %
                 (crit.get("improvement_min_db", float("nan")), sval.get("improvement_provenance")))
    if sval.get("scale_delta_provenance") is None:
        g.refuse("CRITERIA", "max_abs_fitted_scale_delta has no provenance marker")
    # The negative controls are pinned as SEPARATE fields (arm / claim class / expected exit bit /
    # tamper mechanism / expected token). Each is REQUIRED: a criteria file that lost one would lose a
    # control silently, and a control nobody has to name is a control nobody has to run.
    # PRESENCE ONLY, deliberately. The gate cannot itself check a claim about a run it does not perform
    # -- the tamper and the shadow arm are exercised by the mutant runner -- and a check that pretended
    # to would be a check whose name claims more than it measures, which is the defect this slice
    # already had to fix once in its own identity leg.
    for key, what in (
            ("negcontrol_degen_dispatch_arm",
             "the degenerate-dispatch control arm is not named"),
            ("negcontrol_degen_dispatch_class",
             "the degenerate-dispatch control declares no claim class (it must say whether the arm "
             "falsifies the identity or only demonstrates the premise is live)"),
            ("negcontrol_degen_dispatch_expect_exit_bit",
             "the degenerate-dispatch control declares no expected driver exit bit, so how it is "
             "detected cannot be mechanical"),
            ("negcontrol_degen_identity_tamper",
             "the identity's falsifiability control names no tamper mechanism, so nothing would falsify "
             "the identity"),
            ("negcontrol_degen_identity_tamper_expect_token",
             "the identity tamper declares no expected token (bucket included, because a refusal would "
             "be the wrong outcome rather than a different spelling of the right one)"),
            ("degen_driver_stimulus_exit_bit", "the driver's stimulus exit bit is not pinned"),
            ("degen_driver_premise_exit_bit", "the driver's premise exit bit is not pinned")):
        if sval.get(key) is None:
            g.refuse("CRITERIA", "%s (criteria key %r)" % (what, key))

    plan = parse_plan(g, args.plan, axis.get("plan_block", ["dynamic"])[0])
    scn = parse_scenarios(g, os.path.join(args.cand_arm, "gh19_s3_scenarios.tsv"))
    cand, cand_fence, cand_edges = parse_dynamic_matrix(g, args.cand_report)

    # An ABSENT arm is a missing INPUT, not a product verdict. Without this the judgement below would
    # compare against nothing and emit 56 `PRB-FAIL-IMPROVEMENT` lines -- i.e. a red verdict about the
    # DSP derived from an arm that was never rendered, which is the single most misleading thing this
    # gate could print. Same for the degenerate arms: the criteria enumerate the pairs, so if the renders
    # are not supplied the identity check must REFUSE rather than quietly not run.
    # The DEFAULT suffix, not the criteria value: the criteria's `code_missing_arm` reaches this code
    # through the one mapping built at load time (Gate.codes), so reading it here as well would be a
    # second path to the same string -- two paths that can disagree, which is the shape of defect this
    # slice has already had to fix twice.
    ma = "MISSING-ARM"
    if not args.neut_report or not args.neut_arm:
        g.refuse(ma, "--neut-report and --neut-arm are required: the improvement is a comparison, and a "
                      "comparison against an arm that was not rendered cannot be reported as either an "
                      "improvement or a regression")
    if pairs and not (args.degen_static_arm and args.degen_dyn_arm):
        g.refuse(ma, "%d degenerate pairs are pinned but the static arm and/or the constructed-cell arm "
                     "was not supplied, so PRB-DEGEN-IDENTITY would not run at all" % len(pairs))

    g.say("%s GATE criteria=%s cells=%d pairs=%d plan_%s=%d" %
          (sval.get("gate_code_prefix", "PRB"), os.path.basename(args.criteria), len(cells),
           len(pairs), axis.get("plan_block", ["dynamic"])[0], len(plan)))

    # ---- the three-way cell-set reconciliation ---------------------------------------------------
    # (a) the enumeration, (b) the probe's declared plan (a SET: its order is the probe's axis order),
    # (c) the renderer's scenario table filtered by `path` (an ORDERED SEQUENCE: this is the order the
    # enumeration is written in, so a plain diff against the table is a clean line-by-line check).
    want_paths = set(axis.get("scenarios_path", []))
    by_path = [r["id"] for r in scn.values() if r["path"] in want_paths]
    if cells != by_path:
        only_e = sorted(set(cells) - set(by_path))
        only_s = sorted(set(by_path) - set(cells))
        g.refuse("CELLSET", "enumeration %d vs scenarios-path %d; enumerated-only=%s table-only=%s" %
                 (len(cells), len(by_path), ",".join(only_e[:6]) or "-", ",".join(only_s[:6]) or "-"))
    if set(cells) != set(plan):
        g.refuse("CELLSET", "enumeration vs the probe's declared %s block; only-enumerated=%s "
                            "only-declared=%s" %
                 (axis.get("plan_block", ["dynamic"])[0],
                  ",".join(sorted(set(cells) - set(plan))[:6]) or "-",
                  ",".join(sorted(set(plan) - set(cells))[:6]) or "-"))
    # The named out-of-scope boundary: ids that LOOK like moving-duty cells but are not this slice's.
    oos = sorted(i for i, r in scn.items() if "dynpwm" in i and r["path"] not in want_paths)
    got_paths = set(scn[i]["path"] for i in oos)
    declared_oos = set(axis.get("out_of_scope_path", []))
    if got_paths != declared_oos:
        g.refuse("CELLSET", "out-of-scope paths measured=%s declared=%s" %
                 (",".join(sorted(got_paths)), ",".join(sorted(declared_oos))))
    leaked = sorted(set(oos) & set(cells))
    if leaked:
        g.refuse("CELLSET", "cells declared out of scope are inside the enumeration: %s" %
                 ",".join(leaked))

    check_fence(g, "cand", cand_fence, len(cells), len(cand))
    if cand_edges[0] is not None:
        edge_eff = min(cand_edges)
        if abs(edge_eff - crit["edge_eff_expected"]) > 1e-9:
            g.refuse("BAND", "edge_eff=min(edge_L=%.9f, edge_2L=%.9f)=%.9f, but the criteria pin %.9f; "
                             "the two reference levels are not on the R1 instrument" %
                     (cand_edges[0], cand_edges[1], edge_eff, crit["edge_eff_expected"]))

    margins, band_margins, _mf = check_floor_and_shape(
        g, "cand", cand, cells, crit, args.cand_arm, scn, crit["trace_min_frames"])

    # The reference and companion readouts, printed as the criteria require. These are the numbers a
    # reviewer needs to re-check the floor argument without re-running the analyzer, so they are
    # printed even when the run refuses -- a refusal that hid the measurement it was based on would
    # make "the instrument is not entitled to an opinion" unfalsifiable.
    def _extreme(seq, cells_, key, pick):
        vals = [(cells_.get(c, {}).get(key), c) for c in cells_]
        vals = [(v, c) for v, c in vals if v is not None]
        if not vals:
            return None, "-"
        return pick(vals)

    for key, pick in (("refconv_rms_db", max), ("refconv_peak", max),
                      ("res_effbd_db", min), ("res_effbd2L_db", min)):
        v, cid = _extreme(None, cand, key, pick)
        g.report(key, "-" if v is None else "%.4f" % v)
        if v is not None:
            g.say("%s REPORT %s=%s at %s" % (g.code_prefix, key, "%.4f" % v, cid))
    if margins:
        m, cid = min(margins)
        g.report("report_floor_margin_db", "%.4f" % m)
        g.report("worst_floor_margin_db", "%s=%.4f" % (cid, m))
        g.say("%s REPORT report_floor_margin_db=%.4f at %s (computed %s - %s)" %
              (g.code_prefix, m, cid, "res_db", "refconv_rms_db"))
    if band_margins:
        bm, bcid = min(band_margins)
        g.report("report_floor_margin_band_db", "%.4f" % bm)
        g.say("%s REPORT report_floor_margin_band_db=%.4f at %s (companion axis, not judged)" %
              (g.code_prefix, bm, bcid))
    g.report("margin_source_column", "computed:%s-%s" % (sval.get("floor_margin_axis"), "refconv_rms_db"))
    # Named for what it counts. An earlier revision set this to the number of cells the floor check
    # SCANNED, which read as "56 cells were refused" on a run that refused none of them -- a count whose
    # value contradicts its own name is worse than no count, because it is quoted.
    g.report("refused_cells", len(_mf))
    g.say("%s REPORT refused_cells=%d (cells refused on the floor; %d were scanned)" %
          (g.code_prefix, len(_mf), len(margins)))

    neut = {}
    if args.neut_report:
        nscn = parse_scenarios(g, os.path.join(args.neut_arm, "gh19_s3_scenarios.tsv"))
        neut, nfence, nedges = parse_dynamic_matrix(g, args.neut_report)
        check_fence(g, "neut", nfence, len(cells), len(neut))
        if nedges[0] is not None and abs(min(nedges) - crit["edge_eff_expected"]) > 1e-9:
            g.refuse("BAND", "neut: edge_eff=%.9f, but the criteria pin %.9f" %
                     (min(nedges), crit["edge_eff_expected"]))
        check_floor_and_shape(g, "neut", neut, cells, crit, args.neut_arm, nscn,
                              crit["trace_min_frames"])

    # ---- the degenerate pairs: all three renders must be present before the identity can be checked ---
    # Three ids per pair, three renders to load: the static side out of the arm, and the constant-duty and
    # moving-duty sides out of the CONSTRUCTED-cell arm. Both sides of the identity must come from the
    # same build, so the transcript records which directory each side was read from -- see PRB INPUT.
    dst_bytes, dyn_bytes = {}, {}
    if pairs and args.degen_static_arm and args.degen_dyn_arm:
        dscn = parse_scenarios(g, os.path.join(args.degen_static_arm, "gh19_s3_scenarios.tsv"))
        yscn = parse_scenarios(g, os.path.join(args.degen_dyn_arm, DEGEN_MANIFEST))
        for sid, cid, mid in pairs:
            for cell, table, base, store, what in (
                    (sid, dscn, args.degen_static_arm, dst_bytes, "degen static side"),
                    (cid, yscn, args.degen_dyn_arm, dyn_bytes, "degen constant-duty side"),
                    (mid, yscn, args.degen_dyn_arm, dyn_bytes, "degen moving-duty side")):
                blob = arm_render_bytes(g, table, base, cell, what, ma)
                if blob is None:
                    continue
                if cell in store and store[cell] != blob:
                    g.refuse("CELLSET", "%s %s: two scenario rows name renders that disagree" %
                             (what, cell))
                    continue
                store[cell] = blob

    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    # ---- JUDGEMENT -------------------------------------------------------------------------------
    inv = 1.0 / crit["improvement_min_db"] if crit["improvement_min_db"] else math.inf
    imp_c, imp_n = [], []
    for cid in cells:
        c, n = cand.get(cid), neut.get(cid)
        if c is None or c["res_db"] is None:
            continue
        if n is None or n["res_db"] is None:
            g.fail("IMPROVEMENT", "%s: the neutralised arm has no residual for this cell, so there is "
                                  "nothing to compare against" % cid)
            continue
        # Signed so that POSITIVE means the corrected arm is better (its residual is further down).
        # Read through both columns; the sign convention is stated once, here, and not re-derived later.
        d = n["res_db"] - c["res_db"]
        imp_c.append((d, cid))
        imp_n.append(n["res_db"])
        if d < crit["improvement_min_db"]:
            g.fail("IMPROVEMENT",
                   "%s: corrected %.2f dB vs neutralised %.2f dB -> improvement %.2f dB, below the "
                   "pinned %.1f dB" % (cid, c["res_db"], n["res_db"], d, crit["improvement_min_db"]))

    # Both sides of the comparison are printed, not just the verdict: the threshold's discriminating
    # margin is an argument about where it sits between these two measured numbers, so a reader must
    # be able to see the numbers without re-running anything.
    if imp_c:
        worst_d, wid = min(imp_c)
        g.report("improvement_corrected_db", "%.4f" % cand[wid]["res_db"])
        g.report("improvement_neutralized_db", "%.4f" % neut[wid]["res_db"])
        g.say("%s REPORT improvement_min=%.4f dB at %s (corrected %.4f vs neutralised %.4f, pinned %.1f)"
              % (g.code_prefix, worst_d, wid, cand[wid]["res_db"], neut[wid]["res_db"],
                 crit["improvement_min_db"]))

    # The fitted-scale movement, REPORTED and never gated -- the S3 precedent, and the audit's rule
    # that inventing a threshold for a quantity the authorization left unbounded is the defect itself.
    if neut:
        deltas = [abs(cand[c]["A"] - neut[c]["A"]) for c in cells
                  if c in cand and c in neut and cand[c]["A"] is not None and neut[c]["A"] is not None]
        if deltas:
            worst = max(deltas)
            g.report("max_abs_fitted_scale_delta", "%.6g" % worst)
            g.say("%s REPORT max_abs_fitted_scale_delta=%.6g (reported, not gated)" %
                  (sval.get("gate_code_prefix", "PRB"), worst))

    # ---- the degenerate identity, and its mandatory non-vacuity control --------------------------
    moving_delta = None
    if dst_bytes and dyn_bytes:
        for sid, cid, mid in pairs:
            a, b = dst_bytes.get(sid), dyn_bytes.get(cid)
            if a is None or b is None:
                continue
            if sval.get("degen_identity_compare") != "byte_identical":
                g.refuse("CRITERIA", "degen_identity_compare=%r is not supported" %
                         sval.get("degen_identity_compare"))
                continue
            if a != b:
                # Report the size of the disagreement in samples too: a byte count alone does not tell a
                # reader whether this is float noise or a different waveform.
                n = sum(1 for p, q in zip(a, b) if p != q)
                d = max_abs_sample_delta(a, b)
                g.fail("DEGEN-IDENTITY",
                       "%s vs %s: byte-identical required, %d byte(s) differ (max |sample delta| %s)" %
                       (sid, cid, n, "n/a, lengths %d vs %d" % (len(a), len(b)) if d is None
                        else "%.6g" % d))
            # NON-VACUITY: the SAME comparison on a render whose duty genuinely moves must DIFFER. An
            # identity that nothing can break is not evidence that the two routes share a construction.
            m = dyn_bytes.get(mid)
            if m is None:
                continue
            if a == m:
                g.fail("DEGEN-NONVACUOUS",
                       "%s vs %s: a MOVING-duty render compares identical to the static route, so the "
                       "identity above is vacuous" % (sid, mid))
                continue
            d = max_abs_sample_delta(a, m)
            if d is not None and (moving_delta is None or d > moving_delta):
                moving_delta = d
        if crit.get("degen_nonvacuous_required") == 1 and moving_delta is not None:
            g.report("degen_moving_duty_measured_delta", "%.6g" % moving_delta)
            g.say("%s REPORT degen_moving_duty_measured_delta=%.6g (largest separation of any moving-duty "
                  "pair from its static twin, in product units)" %
                  (sval.get("gate_code_prefix", "PRB"), moving_delta))

    if g.failures:
        return finish(g, "RED", EXIT_RED)
    return finish(g, "PASS", EXIT_PASS)


def finish(g, verdict, code):
    for key in REQUIRED_REPORTS:
        if key not in g.reports and key not in ("max_abs_fitted_scale_delta",
                                                "degen_moving_duty_measured_delta"):
            # Quantities this verdict did not get to compute are printed as `-`, so "not reported"
            # cannot hide behind "the run refused before it got there".
            g.report(key, "-")
    for key in REQUIRED_REPORTS:
        g.say("%s REPORT %s=%s" % (g.code_prefix, key, g.reports.get(key, "-")))
    for label, bucket in (("REFUSE", g.refusals), ("FAIL", g.failures)):
        counts = {}
        for c, _ in bucket:
            counts[c] = counts.get(c, 0) + 1
        for c in sorted(counts):
            g.say("%s COUNT %s %s n=%d" % (g.code_prefix, label,
                                           (g.refuse_prefix if label == "REFUSE" else g.fail_prefix) + c,
                                           counts[c]))
    g.say("%s GATE verdict=%s refusals=%d failures=%d" %
          (g.code_prefix, verdict, len(g.refusals), len(g.failures)))
    if verdict == "REFUSE":
        g.say("%s NOTE no judgement is issued: the artifacts failed an input check, so a red or green "
              "verdict here would not be a statement about the product." % g.code_prefix)
    if g.out:
        with open(g.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(g.lines) + "\n")
    return code


def main(argv):
    ap = argparse.ArgumentParser(description="GH#19 S6 PR-B (task #121) acceptance gate.")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--plan", required=True, help="the probe's declared plan (gh19_s3_plan.tsv)")
    ap.add_argument("--cand-report", required=True, help="analyzer --out for the corrected arm")
    ap.add_argument("--cand-arm", required=True, help="probe --out directory of the corrected arm")
    ap.add_argument("--neut-report", default=None, help="analyzer --out for the neutralised arm")
    ap.add_argument("--neut-arm", default=None, help="probe --out directory of the neutralised arm")
    ap.add_argument("--degen-static-arm", default=None,
                    help="arm directory holding the static side (its gh19_s3_scenarios.tsv names the renders)")
    ap.add_argument("--degen-dyn-arm", default=None,
                    help="arm directory holding the constructed constant/moving-duty cells "
                         "(its %s names the renders)" % DEGEN_MANIFEST)
    ap.add_argument("--out", default=None, help="also write the full transcript here")
    args = ap.parse_args(argv)

    g = Gate()
    g.out = args.out
    g.say("PRB GATE tool=gh19_prb_dynpwm_acceptance.py")
    # The verdict is only about the artifacts it actually read, so it records their hashes. Without
    # this a transcript cannot be tied to a revision and a report citing it cannot be re-checked.
    # Informational: nothing is compared against a pin, because a criteria file that pinned its own
    # input hashes could not also be the thing under review.
    for label, path in (("criteria", args.criteria), ("plan", args.plan),
                        ("cand_report", args.cand_report), ("neut_report", args.neut_report),
                        ("degen_static_manifest", os.path.join(args.degen_static_arm, "gh19_s3_scenarios.tsv")
                         if args.degen_static_arm else None),
                        ("degen_dyn_manifest", os.path.join(args.degen_dyn_arm, DEGEN_MANIFEST)
                         if args.degen_dyn_arm else None)):
        if not path:
            continue
        try:
            with open(path, "rb") as fh:
                digest = hashlib.sha256(fh.read()).hexdigest()
        except OSError as exc:
            digest = "UNREADABLE(%s)" % exc.__class__.__name__
        g.say("PRB INPUT %s sha256=%s path=%s" % (label, digest, path))
    return run(g, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
