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
import hashlib
import math
import os
import re
import struct
import sys
from _gh19_textio import Gh19TextIOError, open_text, read_text

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4
# NOT a verdict. Python exits 1 on an uncaught exception, which is EXIT_RED's value, so a crash
# inside this gate used to be indistinguishable from a legitimate RED by exit code alone. The S6
# baseline arm is committed PARTIALLY on purpose (only the sine cells' raw windows are read from
# it, see report/gh19-s6-saw-aa/baseline_arm/README.md), which makes "this file is not here" a
# reachable state rather than a hypothetical one. Unreadable inputs are therefore refused BY NAME
# and anything else unexpected lands here, so no failure can wear a verdict's exit code.
EXIT_CRASH = 3

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
                    "sine_node_worst_bit_diff_count",
                    "s6_wiring_call_sites")

REQUIRED_AXES = ("sides", "sample_rates", "frequencies")
PLAN_REV = "1"

# ---- THE S6 WIRING DECLARATION -----------------------------------------------------------------
#
# WHY THIS IS PART OF THE EQUALITY LAYER. `EQUALITY-SINE` is a same-build A/B: arm A is the product,
# arm B is the same source with the two value-jump corrections neutralised, and the two must agree
# bit-for-bit on the eight default sine-node windows. That comparison is conclusive only together
# with one structural fact -- that those two applications are the ONLY product code that consumes
# what S6 introduces. If some other path also read those weights or that residual, neutralising the
# two applications would not be the whole difference between the arms, and a bit-equal A/B could
# coexist with a default output the correction still influenced elsewhere.
#
# So the callers of every name S6 introduces are DECLARED, and the declaration is DATA in the pinned
# criteria file (`wiring <file> <name> <code-hit-count>` rows) rather than a table in this source
# file: the expectation lives where every other expectation lives, and a reviewer reads it there.
# The gate requires the declaration to equal the tree. An undeclared call site, a count that moved,
# a name that vanished, or a hit outside the guard block and the six declarations is a FAIL naming
# the file, the line and the identifier -- a different failure surface from "the A/B windows are not
# bit-identical", with its own code and its own reason string, because the two mean different things:
# one says the correction is not inert on the default patch, the other says this gate can no longer
# see all of the places it would have to be inert in.
S6_WIRE_NAMES = ("nodeWeight", "sawWeight", "invSawWeight",
                 "sawJumpResidual_", "sawBlepWeight_", "invSawBlepWeight_")
# Scanned relative to --repo-root. The claim is about the SHIPPED product, so tests and tools are not
# scanned; the scope is printed on every run rather than left implicit.
S6_WIRE_ROOTS = ("core", "host", "generated")
S6_WIRE_SUFFIXES = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx")
# The S6 guard block, located by this anchor (which must occur exactly once) and closed by brace
# matching. Every code hit that is not inside one of the six declarations themselves must be inside
# it: the guard is the only place the correction is allowed to reach the signal.
S6_GUARD_ANCHOR = "if (sw > 0.0 || iw > 0.0) {"

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


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


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
    with open_text(path) as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            out.append((n, line.split("\t")))
    return out


def parse_criteria(g, path):
    crit, axis, sets, cells, eq, report_only, reports = {}, {}, {}, [], [], [], set()
    wiring = {}
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
        elif key == "wiring":
            # `wiring <file> <identifier> <code-hit-count>`: the declared number of times `identifier`
            # appears in the CODE of `file` (comments and string contents do not count, see code_only).
            if len(f) != 4:
                g.refuse("CRITERIA", "%s:%d: wiring row needs exactly 4 fields" % (path, n))
                continue
            if f[2] not in S6_WIRE_NAMES:
                g.refuse("CRITERIA", "%s:%d: wiring row names %r, which is not one of the six names "
                                     "S6 introduces (%s)" % (path, n, f[2], ", ".join(S6_WIRE_NAMES)))
                continue
            try:
                cnt = int(f[3])
            except ValueError:
                g.refuse("CRITERIA", "%s:%d: wiring count %r is not an integer" % (path, n, f[3]))
                continue
            if (f[1], f[2]) in wiring:
                g.refuse("CRITERIA", "%s:%d: duplicate wiring row for %s in %s" % (path, n, f[2], f[1]))
                continue
            wiring[(f[1], f[2])] = cnt
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
    # The wiring declaration must cover ALL six names S6 introduces. A declaration that dropped one
    # would leave that name unconstrained while the structural half of the equality layer read as if
    # it were checking six, and the gap would be invisible in a passing run.
    undeclared = [n for n in S6_WIRE_NAMES if not any(k[1] == n for k in wiring)]
    if undeclared:
        g.refuse("CRITERIA", "no `wiring` row declares %s; every name S6 introduces must be declared"
                 % ",".join(undeclared))
    return crit, axis, sets, cells, eq, report_only, wiring


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
    text = read_text(path).splitlines()
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
    text = read_text(path).splitlines()
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
    with open_text(path) as fh:
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
    """None when the window cannot be read. The caller turns that into a named refusal: an absent
    or truncated raw is an input problem, and an input problem must not be able to reach the
    equality criteria as a comparison against a shorter or empty capture."""
    path = os.path.join(armdir, raw)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as fh:
        buf = fh.read()
    n = len(buf) // 8
    if n < warm + win:
        return None
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


# --------------------------------------------------------------- the S6 wiring check (static)

def code_only(text):
    """The text with comments, and the CONTENTS of string and char literals, blanked out -- newlines
    and per-line lengths preserved. A comment or a message string that mentions a wired name is not
    a caller of it: counting those would make the declaration follow prose, and the declaration is
    supposed to be a statement about code."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if j < n and text[j] == c:
                out.append(c + " " * (j - i - 1) + c)  # keep the quotes, blank the contents
                i = j + 1
            else:  # unterminated on this line: kept verbatim rather than silently dropped
                out.append(text[i:j])
                i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _brace_span(lines, start):
    """(first, last) 1-based lines of the block opened at or after `start`, by brace matching, or None
    when the braces do not balance before the text ends. Called on comment/literal-stripped lines, so
    a brace inside a message string cannot move the span."""
    depth, seen = 0, False
    for k in range(start, len(lines) + 1):
        depth += lines[k - 1].count("{") - lines[k - 1].count("}")
        seen = seen or "{" in lines[k - 1]
        if seen and depth <= 0:
            return (start, k)
    return None


def wiring_scan(repo_root):
    """({(relpath, name): {line: count}}, {(relpath, name): [lines]}, n_files) over the scanned
    product roots, CODE only. A DECLARATION is identified by its own shape -- a code line that opens
    a block and names the symbol -- rather than by a pinned line number, so the check says "the
    declaration moved" instead of silently accepting a file whose numbers shifted."""
    hits, defs, n_files = {}, {}, 0
    for root in S6_WIRE_ROOTS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(repo_root, root)):
            dirnames[:] = sorted(d for d in dirnames if d not in (".git", "build"))
            for fn in sorted(filenames):
                if not fn.endswith(S6_WIRE_SUFFIXES):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, repo_root)
                with open_text(full) as fh:
                    lines = code_only(fh.read()).splitlines()
                n_files += 1
                for name in S6_WIRE_NAMES:
                    pat = re.compile(r"\b%s\b" % re.escape(name))
                    for i, line in enumerate(lines, 1):
                        c = len(pat.findall(line))
                        if c:
                            hits.setdefault((rel, name), {})[i] = c
                        if _decl_line(line, name):
                            defs.setdefault((rel, name), []).append(i)
    return hits, defs, n_files


def _decl_line(line, name):
    """A declaration of `name`: a code line that opens a block, names the symbol, and has only
    SPECIFIERS AND A TYPE before it. The last clause is what separates a declaration from a call
    that happens to open a block -- `if (sawWeight(c, n) > 0.0) {` also ends in a brace and also
    names the symbol, but its prefix contains `(`, which no return type does."""
    s = line.strip()
    if not s.endswith("{"):
        return False
    m = re.search(r"\b%s\s*\(" % re.escape(name), s)
    if not m:
        return False
    return re.match(r"^[\w:<>,\s\*&]+$", s[:m.start()]) is not None


def check_wiring(g, repo_root, wiring):
    """Compare the pinned declaration against the tree. Structural discrepancies are FAILURES (they
    are facts about the product: a caller moved, or a name is no longer where the declaration says);
    only 'the source cannot be read' is a REFUSAL, because that is an input problem and no judgement
    about the product can be issued from it. Returns the number of code hits found, or None when the
    scan could not be made at all."""
    vco = "core/include/lunar24/core/vco.h"
    for rel, name in sorted(wiring):
        if not os.path.exists(os.path.join(repo_root, rel)):
            g.refuse("WIRING-SOURCE", "the criteria declare %d hits of %s in %s, but that file does "
                                      "not exist under --repo-root %s" % (wiring[(rel, name)], name,
                                                                         rel, repo_root))
    if g.refusals:
        return None
    try:
        hits, defs, n_files = wiring_scan(repo_root)
    except Gh19TextIOError as exc:
        g.refuse("WIRING-SOURCE", "the scanned product tree could not be read as UTF-8: %s. The "
                                  "wiring declaration is a claim about the text of %s, so a file "
                                  "whose bytes are not that text cannot be checked against it -- and "
                                  "must not be read with substitution, which would silently rewrite "
                                  "the evidence this gate is reading" % (exc, ", ".join(S6_WIRE_ROOTS)))
        return None
    g.say("ACCEPT-WIRING roots=%s files_scanned=%d names=%d declared_rows=%d"
          % (",".join(S6_WIRE_ROOTS), n_files, len(S6_WIRE_NAMES), len(wiring)))

    # 1. The declaration, per file and name, must equal the tree -- in both directions. A missing row
    #    and an extra caller are the same class of error (the declaration no longer describes the
    #    product) and are reported with the same code, but named separately so a log says which.
    for (rel, name), want in sorted(wiring.items()):
        got = sum(hits.get((rel, name), {}).values())
        if got != want:
            lines = ",".join(str(i) for i in sorted(hits.get((rel, name), {})))
            g.fail("S6-WIRING", "%s: declared %d code hit(s) of %s, found %d%s"
                   % (rel, want, name, got, (" at lines " + lines) if lines else ""))
    for (rel, name), per_line in sorted(hits.items()):
        if (rel, name) not in wiring:
            g.fail("S6-WIRING", "%s: %d code hit(s) of %s at lines %s are NOT declared"
                   % (rel, sum(per_line.values()), name,
                      ",".join(str(i) for i in sorted(per_line))))

    # 2. Non-vacuity of the scan itself. A rename, or a scanner that lost a file, would otherwise turn
    #    "nothing unexpected was found" into "nothing was looked at", which is the same false reading
    #    this whole layer exists to avoid.
    total = sum(sum(v.values()) for v in hits.values())
    for name in S6_WIRE_NAMES:
        if not any(k[1] == name for k in hits):
            g.fail("S6-WIRING", "no code hit at all for %s: the declaration names a symbol that is "
                                "not in the scanned product tree" % name)

    # 3. Structure. The guard block is located by its anchor (exactly once) and must be the only place
    #    outside the declarations themselves that the three corrected names are read; each name must
    #    still have exactly one declaration line, found by its own shape rather than by a line number.
    try:
        vco_text = read_text(os.path.join(repo_root, vco))
    except Gh19TextIOError as exc:
        g.refuse("WIRING-SOURCE", "%s could not be read as UTF-8: %s. The guard region is placed by "
                                  "reading this file's text, so it cannot be placed in a file whose "
                                  "bytes are not that text" % (vco, exc))
        return total
    vco_lines = code_only(vco_text).splitlines()
    anchors = [i for i, ln in enumerate(vco_lines, 1) if S6_GUARD_ANCHOR in ln]
    if len(anchors) != 1:
        g.fail("S6-WIRING", "the guard anchor %r occurs %d times in %s (expected exactly 1); the A/B's "
                            "neutralisation anchor is the same block, so this gate cannot place the "
                            "one region the correction is allowed to reach"
               % (S6_GUARD_ANCHOR, len(anchors), vco))
    else:
        span = _brace_span(vco_lines, anchors[0])
        if span is None:
            g.fail("S6-WIRING", "the guard block opened at %s:%d does not close" % (vco, anchors[0]))
        else:
            # The region extends upward over the CONTIGUOUS lines that already name one of the six
            # symbols: the guard tests `sw` and `iw`, and those two are bound by the statements
            # directly above it (the guard's read of the weights is therefore part of the same
            # statement group, and a region starting at the anchor alone would report the two
            # bindings as hits "outside the guard" when they are what the guard is about). The walk
            # cannot widen the region over ordinary code: it stops at the first line without a hit.
            lo = anchors[0]
            while lo > 1 and any(re.search(r"\b%s\b" % re.escape(n), vco_lines[lo - 2])
                                 for n in S6_WIRE_NAMES):
                lo -= 1
            inside = sorted({n for (rel, n), per_line in hits.items() if rel == vco
                             for i in per_line if lo <= i <= span[1]})
            for name in ("sawBlepWeight_", "invSawBlepWeight_", "sawJumpResidual_"):
                if name not in inside:
                    g.fail("S6-WIRING", "the guard region at %s:%d-%d does not read %s; the block "
                                        "that is supposed to be the only consumer of the correction "
                                        "is not" % (vco, lo, span[1], name))
    for name in S6_WIRE_NAMES:
        found = sorted((rel, i) for (rel, n), lines in defs.items() if n == name for i in lines)
        if len(found) != 1:
            g.fail("S6-WIRING", "%s: %d declaration lines in the scanned product tree (expected "
                                "exactly 1; a declaration is a line that opens a block and has only "
                                "specifiers and a type before the symbol): %s"
                   % (name, len(found),
                      " ".join("%s:%d" % f for f in found) if found else "-"))
    return total


def finish(g, verdict, code):
    for label, bucket in (("REFUSE", g.refusals), ("FAIL", g.failures)):
        g.dump(label, bucket)
    g.say("ACCEPT-GATE verdict=%s refusals=%d failures=%d"
          % (verdict, len(g.refusals), len(g.failures)))
    if verdict == "REFUSE":
        g.say("ACCEPT-NOTE no judgement is issued: the artifacts failed an input check, so a red or "
              "green verdict here would not be a statement about the DSP.")
    if g.out:
        with open_text(g.out, "w") as fh:
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
    # The NEUTRAL ARM is the same build with the two S6 correction terms neutralised; it is what
    # `EQUALITY-SINE` compares the candidate against (see the criterion's comment in the criteria
    # file). It is REQUIRED rather than optional: an absent neutral arm would silently turn the
    # equality layer into "no criterion was evaluated", which is the failure mode that layer exists
    # to prevent. Rendered by tools/run_gh19_s6_saw_pipeline.py (and, for the mutant matrix, by
    # tools/run_gh19_s6_saw_mutants.py) from tools/stage_gh19_s6_shadow.py's shadow include root.
    ap.add_argument("--neutral-arm", required=True,
                    help="probe --out directory of the neutralised-correction arm (B)")
    ap.add_argument("--repo-root", required=True,
                    help="checkout the PINNED WIRING DECLARATION is checked against (see `wiring` rows)")
    ap.add_argument("--out", default=None, help="also write the full transcript here")
    args = ap.parse_args(argv)

    g = Gate(args.out)
    g.say("ACCEPT-GATE tool=gh19_s6_saw_acceptance.py")
    g.say("ACCEPT-GATE criteria=%s" % args.criteria)
    g.say("ACCEPT-GATE base_report=%s" % args.base_report)
    g.say("ACCEPT-GATE cand_report=%s" % args.cand_report)
    g.say("ACCEPT-GATE base_arm=%s" % args.base_arm)
    g.say("ACCEPT-GATE cand_arm=%s" % args.cand_arm)
    g.say("ACCEPT-GATE neutral_arm=%s" % args.neutral_arm)
    g.say("ACCEPT-GATE repo_root=%s" % args.repo_root)

    for name, path in (("criteria", args.criteria), ("plan", args.plan),
                       ("base_report", args.base_report), ("cand_report", args.cand_report),
                       ("base_arm", args.base_arm), ("cand_arm", args.cand_arm),
                       ("neutral_arm", args.neutral_arm), ("repo_root", args.repo_root)):
        if not os.path.exists(path):
            g.refuse("INPUT-MISSING", "%s: %s does not exist" % (name, path))
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    crit, axis, sets, cells, eq, report_only, wiring = parse_criteria(g, args.criteria)
    # The wiring declaration is the STATIC half of the equality layer: the A/B below can only show
    # that the correction does not reach the default patch, while this shows that the correction has
    # no other reachable caller at all. It is checked before the matrices are read, so a product
    # whose wiring drifted fails on that fact rather than on a downstream number.
    wiring_hits = check_wiring(g, args.repo_root, wiring)
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)
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
    base_idx = cand_idx = neutral_idx = None
    # The neutral arm's manifest is read exactly as the other two are. It carries MEASURED columns
    # (`peak`, `f0_meas_hz`) which differ from the candidate's on the off-default cells -- measured
    # here, and expected: those columns describe what was rendered, and B's rendering differs from
    # A's wherever the correction is in force. What must match is the PLAN (checked above, byte for
    # byte) and the per-cell stimulus triple (raw, warm, win), which is what the windows are read
    # through; requiring whole-file equality would refuse every legitimate run.
    for label, armdir, sink in (("base", args.base_arm, "base"), ("cand", args.cand_arm, "cand"),
                                ("neutral", args.neutral_arm, "neutral")):
        try:
            idx = read_arm_index(os.path.join(armdir, "gh19_s6_scenarios.tsv"))
        except (OSError, ValueError) as exc:
            g.refuse("ARM-MANIFEST", "%s arm: cannot read its scenario manifest: %s" % (label, exc))
            continue
        if sink == "base":
            base_idx = idx
        elif sink == "cand":
            cand_idx = idx
        else:
            neutral_idx = idx
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

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
    # ---- THE A/B's SECOND SIDE, AND WHY IT IS NOT THE FIRST ------------------------------------
    # `EQUALITY-SINE` compares arm A (the product, both correction terms in force) against arm B
    # (the same build with those two terms neutralised). Both sides come from one compiler with one
    # set of flags, so the identity is decidable on any platform; the previous form compared against
    # a committed macOS snapshot and was not (the default patch evaluates std::sin, and Apple's libm
    # and glibc disagree by exactly 1 ULP on 3.76% of identical arguments -- 0 locally, 652 in CI,
    # reproduced against a literal pre-S6 tree; see report/2026-09-25-task120-gh19-s6-morph-aa.md).
    #
    # ARM B MUST BE NON-VACUOUS, and that is why this block runs BEFORE the identity it guards. If
    # the neutralisation failed to take effect (a stale shadow include root, a wrong include order)
    # then B IS A, the windows agree trivially, and the gate would pass while measuring nothing --
    # the degenerate identity this layer exists to refuse. Two independent checks rule that out: the
    # two arms' plan files must be byte-identical (same scenario list), and the arms must DIFFER at
    # morph 0.0, where the correction IS in force (the saw node: sawWeight = 1, invSawWeight = 0).
    # Both are REFUSALS rather than failures: they say this run cannot judge, not that the product
    # is wrong.
    neutral_plan = os.path.join(args.neutral_arm, "gh19_s6_plan.tsv")
    if not os.path.exists(neutral_plan):
        g.refuse("NEUTRAL-ARM", "the neutral arm declares no plan at %s: arm B was not rendered by "
                                "the probe, so the A/B has no second side" % neutral_plan)
    elif sha256_of(neutral_plan) != sha256_of(args.plan):
        g.refuse("NEUTRAL-ARM", "the neutral arm's plan is not the candidate's (%s vs %s): the two "
                                "arms did not render the same scenario list"
                 % (sha256_of(neutral_plan), sha256_of(args.plan)))
    witness = [c for c in cells if abs(tag_morph(CELL_RE.match(c).group("tag"))) < 1e-12]
    if not witness:
        g.refuse("NEUTRAL-ARM", "no gated cell sits at morph 0.0, so the witness that the two arms "
                                "differ where the correction is in force cannot be taken")
    elif neutral_idx is not None:
        w_unreadable, w_same = [], 0
        for cid in sorted(witness):
            cm, nm = cand_idx.get(cid), neutral_idx.get(cid)
            if cm is None or nm is None:
                w_unreadable.append(cid)
                continue
            a = read_window(args.cand_arm, cm[0], cm[1], cm[2])
            b = read_window(args.neutral_arm, nm[0], nm[1], nm[2])
            if a is None or b is None:
                w_unreadable.append(cid)
                continue
            n = bit_diff_count(a, b)
            if n <= 0:
                w_same += 1
        if w_unreadable:
            g.refuse("NEUTRAL-ARM", "%d of %d witness cells at morph 0.0 cannot be read from both "
                                    "arms, e.g. %s" % (len(w_unreadable), len(witness),
                                                       w_unreadable[0]))
        elif w_same == len(witness):
            g.refuse("NEUTRAL-ARM", "arm A and arm B are IDENTICAL on all %d witness cells at morph "
                                    "0.0, where the two correction terms are in force: the "
                                    "neutralisation did not take effect, so B is A and the equality "
                                    "criterion below would compare the product against itself"
                     % len(witness))
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)

    sine_worst = 0
    for cid in sorted(sine):
        cm, nm = cand_idx.get(cid), neutral_idx.get(cid)
        if cm is None or nm is None:
            g.refuse("MATRIX-SHAPE", "%s: no manifest row in one of the two A/B arms" % cid)
            continue
        a = read_window(args.cand_arm, cm[0], cm[1], cm[2])
        b = read_window(args.neutral_arm, nm[0], nm[1], nm[2])
        if a is None or b is None:
            # Named refusal rather than a comparison against a shorter or empty capture. Arm B is
            # rendered fresh by this run's driver, so an unreadable window here is an input problem
            # and never a statement about the product.
            g.refuse("EQUALITY-SINE", "%s: could not read the %s window (%s in %s)"
                     % (cid, "cand" if a is None else "neutral",
                        cm[0] if a is None else nm[0],
                        args.cand_arm if a is None else args.neutral_arm))
            continue
        n = bit_diff_count(a, b)
        if n < 0:
            g.refuse("EQUALITY-SINE", "%s: the two arms' windows are not the same length" % cid)
            continue
        if n > sine_worst:
            sine_worst = n
    if g.refusals:
        return finish(g, "REFUSE", EXIT_REFUSE)
    if sine_worst != crit["sine_node_bit_diff_count"]:
        g.fail("EQUALITY-SINE", "sine-node worst bit-difference count = %d, criterion %d; at morph "
                                "0.5 the neutralised-correction arm and the product arm do NOT agree "
                                "bit for bit, so the two correction terms are not inert on the "
                                "default patch" % (sine_worst, crit["sine_node_bit_diff_count"]))

    # ---- REPORTS: emitted before the verdict, because an uncomputable report is a REFUSAL ------
    max_dA = max(abs(cand[c]["A"] - base[c]["A"]) for c in cells)
    costs = [pinned_res_db(base[c]["A"], cand[c]["A"], cand[c]["res_db"]) - cand[c]["res_db"]
             for c in cells]
    max_cost = max(costs)
    worst_band = min(dband[c] for c in cells)
    worst_full = min(dfeff[c] for c in cells)
    worst_rect = min(dres[c] for c in cells)
    # `res_db` is a residual-to-signal ratio, so MORE NEGATIVE IS BETTER and an improvement is
    # `base - scored`. Spelled the same way as `worst_rect` directly above: the first version of
    # this line wrote `pinned - base`, one operand order away from its sibling, which both inverted
    # the sign and -- because `min` over a negated improvement selects its maximum -- reported the
    # BEST cell under a name that says worst. The figure is report-only either way, so the gate
    # stayed PASS while the row read -7.59 dB where the truth was +7.30 dB; the sign convention is
    # not cosmetic here. Do not "simplify" this into a cost.
    worst_pinned = min(base[c]["res_db"]
                       - pinned_res_db(base[c]["A"], cand[c]["A"], cand[c]["res_db"])
                       for c in cells)
    got_reports = {
        "max_abs_fitted_scale_delta": "%.6f" % max_dA,
        "max_pinned_scale_cost_db": "%+.4f" % max_cost,
        "worst_band_1k5k_improvement_db": "%+.2f" % worst_band,
        "worst_full_band_improvement_db": "%+.2f" % worst_full,
        "worst_pinned_scale_rect_full_band_improvement_db": "%+.2f" % worst_pinned,
        "worst_rect_full_band_improvement_db": "%+.2f" % worst_rect,
        "antiphase_midpoint_worst_abs_output": "%.17g" % anti_worst,
        "sine_node_worst_bit_diff_count": "%d" % sine_worst,
        # The static half of the equality layer, as a number the log carries: how many code hits of
        # the six names S6 introduces the scanned product tree actually contains. The A/B above can
        # only show the correction does not reach the DEFAULT patch; this is the count behind the
        # statement that it has no other reachable caller (pinned per file and name by `wiring` rows).
        "s6_wiring_call_sites": "%s" % ("UNSCANNED" if wiring_hits is None else wiring_hits),
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
    # An uncaught exception exits 1, which is EXIT_RED: the gate would be reporting a RED it never
    # judged. Everything expected is refused by name inside main(); this catches the rest and gives
    # it an exit code no verdict can produce, with a marker a driver can look for.
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except BaseException as exc:  # noqa: BLE001 -- the point is to catch everything
        sys.stderr.write("ACCEPT-GATE CRASH %s: %s\n" % (type(exc).__name__, exc))
        sys.stderr.write("ACCEPT-GATE CRASH no verdict was issued; exit %d is not PASS/RED/REFUSE\n"
                         % EXIT_CRASH)
        raise SystemExit(EXIT_CRASH)
