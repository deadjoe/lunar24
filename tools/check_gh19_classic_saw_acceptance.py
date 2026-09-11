#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #110 / GH#19 S2: RED->GREEN acceptance gate for the CLASSIC DRONE saw value-jump
# correction (polyBLEP). Third sibling of check_gh19_blamp_acceptance.py (VCO triangle) and
# check_gh19_schmitt_blamp_acceptance.py (Schmitt slope). Same driver, same probe, same
# analyzer; different criterion column, different baseline, different scope.
#
# ---------------------------------------------------------------------------------------
# THE MEASURED OBJECT CHANGED -- READ THIS BEFORE INTERPRETING A RED VERDICT
# ---------------------------------------------------------------------------------------
# The two BLAMP siblings measure `blref_full_db`, whose band-limited reference is a
# TRIANGLE (the shape their oscillators actually emit). A classic drone saw has a value
# jump, so the correct reference family is the band-limited SAWCUBIC, which is what
# `blref_*` now holds on classic cells. Those two columns are therefore NOT comparable
# across slices and are NOT acceptance columns here.
#
# This gate's acceptance column is `blpin_full_db`: the residual measured against a
# reference pinned at the ANALYTIC f0 and the ANALYTIC window-start phase (no fit, no
# alignment search). It is comparable across slices because it does not depend on a search
# that the two slices could resolve differently. On classic cells the drift-OFF arm makes
# the corrected and uncorrected renders share one exact phase trajectory (@Kimi 69b64ff6
# pin 2), which is what makes a pinned-phase comparison meaningful at all.
#
# ---------------------------------------------------------------------------------------
# THE 36 IN-SCOPE CELLS ARE PINNED IN SOURCE, NEVER DERIVED FROM THE INPUT FILES
# ---------------------------------------------------------------------------------------
# 3 lanes x 4 sample rates x 3 taps. A header-only, truncated, or duplicate-ridden baseline
# therefore fails loudly (every required cell reports MISSING / duplicate / non-finite)
# instead of passing vacuously with "checked 0/36".
#
# ROLE IS AN AXIS, NOT A LABEL (ruling cd345dbb pin 3) -- with one correction that the
# volt=0 lane forced (@Kimi e6e645d2 ruling 1): the axis carries the BAND the cell actually
# occupies, NOT the generator's structural class. `drone_4` maps to classic group 2, whose
# generators 3 and 4 are kHigh (420-1800 Hz nominal) while `drone_1`'s are kLow (30-120 Hz),
# and the role is a property of the generator WITHIN the group (drone_bank.h:373-378), not of
# the group. But the group's shared VOLT knob transposes the whole group: at its runtime
# default of 30 semis down those kHigh generators land at 97-224 Hz. Labelling that row HIGH
# would assert band coverage the row does not have. All three lanes are tapped through the
# same accessor (droneChannel of their classic group), so they are like-for-like. Per-role
# thresholds exist because the two bands sit ~10 dB apart in the naive baseline and are
# corrected over very different numbers of periods per window.
#
# ---------------------------------------------------------------------------------------
# THRESHOLDS: SET FROM MEASUREMENT, NOT FROM ASPIRATION
# ---------------------------------------------------------------------------------------
# The defaults below are PROVISIONAL and are recorded as such. The slice's order is
# red-first: this gate was committed and run against the UNCORRECTED product, where every
# delta is exactly 0, and it returned rc=1 -- a judgement failure, not a crash and not a
# missing-file error. The thresholds are then set from the measured post-correction gains
# with margin over the weakest cell actually observed, and that revision is a recorded
# change with the measurement behind it, not a tuning to taste.
#
# ---------------------------------------------------------------------------------------
# THE dt <= 0.5 CONTRACT IS ASSERTED HERE, PER CELL, AND ITS SCOPE IS STATED
# ---------------------------------------------------------------------------------------
# polyblep_kernel.h's precondition is `0 <= dt <= 0.5` where dt is the product's
# per-sample phase increment = f0/sr. Each committed row carries `blf0_model_hz`, so the
# contract is CHECKABLE on every in-scope cell: this gate derives dt and fails structurally
# if it leaves the domain. TWO LIMITS, both deliberate and both stated so the assertion is
# not read as stronger than it is:
#   (a) it discharges the contract on the 36 MEASUREMENT cells, which is exactly the scope
#       the kernel header claims ("measured on real renders"). It is NOT a proof over the
#       product's reachable input space -- the setters are unclamped and the VOLT/TUNE
#       range descriptors have no authoritative source yet. That remains OPEN.
#   (b) blf0_model_hz is the model f0 with drift, jitter and mutual FM at their
#       measurement-arm values (drift OFF, modAmount 0, volt 30.0 -> mutual FM branch off),
#       so the derived dt is the arm's dt, not the worst-case product dt.
#
# Structural rejection (exit 2): a required cell missing from baseline or current, a
# duplicate row, an unparsable or placeholder ("-") or non-finite value, an sr mismatch
# against the canonical set, a non-positive model f0, a classic cell present in current but
# absent from the canonical set (new/renamed -> not like-for-like), or a dt outside
# [0, 0.5].
#
# Usage:
#   python3 check_gh19_classic_saw_acceptance.py \
#       --baseline tools/gh19_classic_saw_naive_baseline.tsv --current <analyzer.tsv>
#   python3 check_gh19_classic_saw_acceptance.py ... --self-check
# Exit 0 = acceptance met; 1 = a criterion cell fails (RED); 2 = structural problem.
#
# --self-check proves this gate is not merely red-because-broken: it must go GREEN on a
# synthetic all-cells-improved input, and it must go RED on ten specific corruptions,
# INCLUDING the removal negative control -- current == baseline, i.e. the correction absent
# -- where every delta must be EXACTLY 0.0 and the verdict must be criterion-RED (1), not
# structural-RED (2). A RED verdict from a gate that can never return GREEN is worthless;
# an exact-zero identity that is only checked to a tolerance would not distinguish "the
# correction was removed" from "the correction was left in and the pipeline is noisy".
import argparse
import math
import os
import sys
import tempfile

# The canonical in-scope set: 3 lanes x 4 sample rates x 3 taps = 36.
# Each entry maps a cell id to its expected sample rate and its role. This is the SINGLE
# SOURCE OF TRUTH for coverage; the gate NEVER infers it from the baseline.
#
# ROLE IS THE MEASURED BAND, NOT THE GENERATOR'S STRUCTURAL CLASS -- and the difference is a
# real trap on the classic drone. `drone_bank.h:373-378` assigns roles to generators WITHIN a
# group: gens 3,4 are kHigh (420-1800 Hz at their nominal base). That is what makes drone_4's
# gen 3 the HIGH-role generator. But the group's shared VOLT knob transposes the whole group,
# and at the runtime default of 30 semis down the same kHigh generators land at 97-224 Hz --
# inside the LOW band. A cell labelled by its generator's structural class would therefore call
# that a HIGH cell while it radiates entirely in the LOW band, which is precisely the labelling
# error that made the first 12 drone4 rows look like ruling-3 coverage when they were not
# (@Kimi e6e645d2 ruling 1: "volt=30 落在 97-224Hz 是重复 LOW 带、违背补格初衷"). So the role
# below is the band the cell actually occupies, and the drone4 lane that discharges ruling 3 is
# the volt=0 one.
#
#   drone1_classic      LOW    group 0 gen 0, band 30-120 Hz, volt 30 semis down (default)
#   drone4_classic      LOW    group 2 gen 3, band 97-224 Hz, volt 30 semis down (default).
#                              Kept: a genuine second LOW-band lane on a DIFFERENT generator
#                              group, so the low-frequency correction is covered on two
#                              independent carriers. It is NOT the ruling-3 HIGH cell.
#   drone4_classic_v0   HIGH   group 2 gen 3, band ~551-1266 Hz, volt 0 semis down. THE
#                              ruling-3 HIGH cell (@Kimi e6e645d2 ruling 1, approved).
_LANES = (("drone1_classic", "LOW"),
          ("drone4_classic", "LOW"),
          ("drone4_classic_v0", "HIGH"))
_TAPS = ("t20", "t50", "t80")
CANONICAL = {}
for _lane, _role in _LANES:
    for _sr in (44100, 48000, 88200, 96000):
        for _tap in _TAPS:
            CANONICAL["%s_%d_%s" % (_lane, _sr, _tap)] = (_sr, _role)
assert len(CANONICAL) == 36, len(CANONICAL)

# The kernel's declared domain (polyblep_kernel.h kPolyblepMaxDt). Not a tuned threshold:
# it is the boundary below which the two correction windows [0,dt) and (1-dt,1] are
# disjoint (they touch at exactly 0.5).
POLYBLEP_MAX_DT = 0.5

# The column names this gate binds to. Read BY NAME from the header, in both the baseline
# and the analyzer's stdout, so a column insertion or reorder in the analyzer cannot
# silently retarget the acceptance column at a neighbour. The BLAMP siblings index by
# position and carry a comment explaining the map; binding by name is the stronger form and
# costs nothing here because both inputs carry a header row.
COL_ID = "id"
COL_SR = "sr"
COL_F0 = "blf0_model_hz"
COL_PIN = "blpin_full_db"
REQUIRED_COLS = (COL_ID, COL_SR, COL_F0, COL_PIN)

# The prefix that identifies a classic-saw row. Used to decide which rows are IN SCOPE for
# the "a cell in current is not canonical" check; every other analyzer row (vco, schmitt,
# sub, MACHINE, blank) is ignored silently.
CLASSIC_TAG = "classic"


def parse(path):
    """Return (rows, dups, problems). rows[id] = {sr, f0, pin}.

    Binds by column NAME from the file's own header. Only classic rows are in scope; any
    other row is ignored silently. A classic row that is unparsable, duplicated, or carries
    a placeholder ("-") is collected into `problems`."""
    rows, dups, problems = {}, [], []
    hdr = None
    for ln, line in enumerate(open(path, encoding="utf-8"), 1):
        s = line.strip()
        if not s or s.startswith("#") or s.startswith("MACHINE"):
            continue
        c = [x.strip() for x in line.rstrip("\n").split("\t")]
        if hdr is None:
            if c[0] != COL_ID:
                continue                       # not the header yet
            missing = [n for n in REQUIRED_COLS if n not in c]
            if missing:
                problems.append("%s:%d: header is missing required column(s): %s"
                                % (path, ln, ", ".join(missing)))
                return rows, dups, problems
            hdr = c
            continue
        if len(c) != len(hdr):
            continue                           # not a data row for this table
        cid = c[hdr.index(COL_ID)]
        if CLASSIC_TAG not in cid:
            continue                           # out-of-scope cell -> ignore silently.
        raw = {n: c[hdr.index(n)] for n in REQUIRED_COLS}
        if "-" in (raw[COL_SR], raw[COL_F0], raw[COL_PIN]):
            problems.append("%s:%d: placeholder '-' in %s (column not populated)"
                            % (path, ln, cid))
            continue
        try:
            sr = int(float(raw[COL_SR]))
            f0 = float(raw[COL_F0])
            pin = float(raw[COL_PIN])
        except ValueError:
            problems.append("%s:%d: unparsable numeric row %r" % (path, ln, cid))
            continue
        if not (math.isfinite(f0) and math.isfinite(pin)):
            problems.append("%s:%d: non-finite value in %s" % (path, ln, cid))
            continue
        if cid in rows:
            dups.append(cid)
            continue
        rows[cid] = {"sr": sr, "f0": f0, "pin": pin}
    if hdr is None:
        problems.append("%s: no header row found" % path)
    return rows, dups, problems


def evaluate(baseline_path, current_path, min_low, min_high):
    """Return (rc, lines). rc: 0 accept, 1 criterion RED, 2 structural RED."""
    base, base_dups, base_prob = parse(baseline_path)
    cur, cur_dups, cur_prob = parse(current_path)
    out = []

    def emit(s):
        out.append(s)

    fails = [*base_prob, *cur_prob]
    # A classic-looking cell that is NOT canonical = new/renamed/like-for-like-breaking.
    # This is the check that makes a manifest extension (e.g. adding a second VOLT setting)
    # fail closed until the canonical set and the baseline are extended WITH it.
    for k in sorted(cur):
        if k not in CANONICAL:
            fails.append("%s: classic cell in current is NOT in the canonical 36-set "
                         "(new/renamed -> not like-for-like; extend CANONICAL and the "
                         "committed baseline together)" % k)
    for k in sorted(set(base_dups) | set(cur_dups)):
        fails.append("%s: duplicate row in input" % k)

    # The field widths here MUST mirror the data row's below (30/6/7/10/10/10/10, then the
    # verdict). They did not: the gain column was declared %-6s while its heading "dB_gain" is
    # seven characters, so the heading overflowed its field and ran straight into "verdict"
    # ("dB_gainverdict"), and every heading after it sat three columns left of its own column.
    # Cosmetic -- nothing parses this table -- but this is the text a human reads in CI.
    emit("%-30s%-6s%-7s%-10s%-10s%-10s%-10s%s"
         % ("cell", "role", "sr", "dt", "naive", "current", "dB_gain", "verdict"))

    # Iterate the CANONICAL set, not the baseline file. Data rows in baseline/current that
    # are NOT canonical are either already flagged above or ignored.
    checked = {"LOW": 0, "HIGH": 0}
    weakest = {"LOW": None, "HIGH": None}
    for cid in sorted(CANONICAL):
        exp_sr, role = CANONICAL[cid]
        min_gain = min_low if role == "LOW" else min_high
        if cid not in base:
            fails.append("%s: MISSING from naive baseline (a required cell has no "
                         "reference -> baseline truncated/vacuous?)" % cid)
            continue
        if cid not in cur:
            fails.append("%s: MISSING from current analyzer output" % cid)
            continue
        b, c = base[cid], cur[cid]
        if b["sr"] != exp_sr or c["sr"] != exp_sr:
            fails.append("%s: sr mismatch (canonical %d, baseline %d, current %d)"
                         % (cid, exp_sr, b["sr"], c["sr"]))
            continue
        if not (b["f0"] > 0.0):
            fails.append("%s: non-positive baseline blf0_model_hz -> dt underivable" % cid)
            continue

        # The kernel's domain, asserted per cell. See the header for the two stated limits.
        dt = b["f0"] / float(exp_sr)
        if not (0.0 <= dt <= POLYBLEP_MAX_DT):
            fails.append("%s: dt=%.9f is outside the polyBLEP domain [0, %g] -> the kernel's "
                         "precondition is violated on this cell" % (cid, dt, POLYBLEP_MAX_DT))
            continue

        # Positive = more band-limited (better): residual dB are negative, lower is better.
        gain = b["pin"] - c["pin"]
        ok = gain >= min_gain
        if ok:
            checked[role] += 1
            weakest[role] = gain if weakest[role] is None else min(weakest[role], gain)
        verdict = "GREEN" if ok else "RED"
        if not ok:
            fails.append("%s (%s): gain %.2f dB < %.2f dB required" % (cid, role, gain, min_gain))
        emit("%-30s%-6s%-7d%-10.3e%-10.2f%-10.2f%-10.2f%s"
             % (cid, role, c["sr"], dt, b["pin"], c["pin"], gain, verdict))

    emit("")
    for role in ("LOW", "HIGH"):
        total = sum(1 for v in CANONICAL.values() if v[1] == role)
        w = weakest[role]
        emit("%s role: passed=%d/%d%s" % (role, checked[role], total,
             "" if w is None else "  weakest gain=%.2f dB" % w))
    # The removal negative control as a REPORTED property, not just an assertion: on an
    # uncorrected run every delta is exactly 0.0 and this line says so in those words.
    exact_zero = all(base[k]["pin"] == cur[k]["pin"] for k in CANONICAL
                     if k in base and k in cur)
    emit("exact-zero deltas (correction absent): %s" % ("YES -- every in-scope cell's "
         "blpin_full_db is bit-equal to the baseline" if exact_zero else "no"))

    if fails:
        emit("")
        emit("FAIL:")
        for f in fails:
            emit("  - %s" % f)
        structural = any(("MISSING" in f or "duplicate" in f or "mismatch" in f
                          or "non-finite" in f or "NOT in the canonical" in f
                          or "unparsable" in f or "vacuous" in f or "underivable" in f
                          or "placeholder" in f or "outside the polyBLEP domain" in f
                          or "no header row" in f or "header is missing" in f) for f in fails)
        return (2 if structural else 1), out
    emit("")
    emit("PASS: all 36 classic-saw cells improve the pinned-phi residual by >= their role's "
         "threshold (LOW %g dB, HIGH %g dB)." % (min_low, min_high))
    return 0, out


def _rewrite(path, fn):
    """Apply fn(lines) -> lines to a copy of `path`; return the temp path."""
    with open(path, encoding="utf-8") as fh:
        raw = [l.rstrip("\n") for l in fh if l.strip()]
    fh = tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False, encoding="utf-8")
    fh.write("\n".join(fn(raw)) + "\n")
    fh.close()
    return fh.name


def _drop_row(cid):
    return lambda raw: [l for l in raw if not l.startswith(cid + "\t")]


def _dup_row(cid):
    def fn(raw):
        out = []
        for l in raw:
            out.append(l)
            if l.startswith(cid + "\t"):
                out.append(l)
        return out
    return fn


def _set_col(cid, col, value):
    """Set column `col` (a NAME) on row `cid`, using the file's own header."""
    def fn(raw):
        hdr = None
        out = []
        for l in raw:
            c = l.split("\t")
            if hdr is None and c[0] == COL_ID:
                hdr = c
            if hdr is not None and c[0] == cid and col in hdr and len(c) > hdr.index(col):
                c[hdr.index(col)] = value
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def _shift_current(delta, ids):
    """Reduce the listed cells' blpin_full_db by `delta` (more negative = better).

    Retained for callers that genuinely want a RELATIVE shift, but it is NOT the helper to
    build a must-be-RED fixture from: a flat shift is only RED when the cell already starts
    near zero. `_worsen_one_below_threshold` is the anchored equivalent and is what NC-8 uses.
    """
    def fn(raw):
        hdr = None
        out = []
        for l in raw:
            c = l.split("\t")
            if hdr is None and c[0] == COL_ID:
                hdr = c
            if (hdr is not None and c[0] in ids
                    and len(c) > hdr.index(COL_PIN)):
                if c[hdr.index(COL_PIN)] != "-":
                    c[hdr.index(COL_PIN)] = "%.6f" % (float(c[hdr.index(COL_PIN)]) - delta)
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def _improve_all_past_threshold(min_low, min_high):
    """Improve every canonical cell past ITS OWN role's threshold, with margin.

    Role-aware on purpose. The single `min_low + 1` shift this replaced was correct only while
    the LOW and HIGH thresholds happened to be the same number; the moment they differ, the
    HIGH cells land short of `min_high` and NC-1 reports WRONG on a gate that is behaving
    exactly as designed. A fixture that must be re-derived every time a threshold is tuned is a
    fixture that will eventually be "fixed" by weakening the gate instead of the fixture.
    """
    deltas = {cid: (min_low if role == "LOW" else min_high) + 1.0
              for cid, (_sr, role) in CANONICAL.items()}

    def fn(raw):
        hdr = None
        out = []
        for l in raw:
            c = l.split("\t")
            if hdr is None and c[0] == COL_ID:
                hdr = c
            if hdr is not None and c[0] in deltas and len(c) > hdr.index(COL_PIN):
                if c[hdr.index(COL_PIN)] != "-":
                    c[hdr.index(COL_PIN)] = "%.6f" % (float(c[hdr.index(COL_PIN)])
                                                      - deltas[c[0]])
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def _is_exactly_zero(baseline, current):
    """True only if every in-scope cell's acceptance value is BIT-EQUAL across the two
    files. This is the removal negative control's identity, checked exactly rather than to
    a tolerance: a tolerance would not separate 'the correction was removed' from 'the
    correction is present and the pipeline is noisy'."""
    base, _, _ = parse(baseline)
    cur, _, _ = parse(current)
    return all(k in cur and base[k]["pin"] == cur[k]["pin"] for k in base) and bool(base)


def _as_baseline_rows(baseline_path):
    """Make `--current` a byte-identical copy of the BASELINE.

    The removal negative control needs the current file to BE the baseline. This fixture was
    first written as `lambda raw: raw`, which returns whatever `--current` the caller passed.
    That is the removal control only when the caller happens to pass the baseline AS
    `--current` -- true of the standalone invocation it was developed against, false of the
    real pipeline, where `--current` is the freshly analyzed corrected arm. Under the pipeline
    the fixture degenerated into "current unchanged" and then asserted that a correctly GREEN
    arm is criterion-RED. Closing over the baseline path makes the control independent of the
    caller, so it tests the gate rather than the invocation.
    """
    with open(baseline_path, encoding="utf-8") as fh:
        rows = [l.rstrip("\n") for l in fh if l.strip()]
    return lambda raw: rows


def _worsen_one_below_threshold(baseline_path, cid, min_gain):
    """Push `cid`'s gain 1 dB BELOW its threshold, so the verdict must be criterion-RED.

    Anchored to the baseline's own value, NOT a flat "-1 dB" shift. A flat shift is RED only
    when the cell already starts near zero; against the real corrected arm this cell starts
    ~7.9 dB above a 5.5 dB threshold, so -1 dB left it GREEN and the fixture again asserted
    RED on a gate behaving exactly as designed. Mirroring `_improve_all_past_threshold`, which
    anchors upward at `threshold + 1`, makes this fixture threshold-aware in both directions.
    """
    base = None
    with open(baseline_path, encoding="utf-8") as fh:
        hdr = None
        for l in fh:
            c = l.rstrip("\n").split("\t")
            if hdr is None and c[0] == COL_ID:
                hdr = c
                continue
            if hdr is not None and c[0] == cid and len(c) > hdr.index(COL_PIN):
                base = float(c[hdr.index(COL_PIN)])
                break
    if base is None:
        raise ValueError("self-check: %s not present in %s" % (cid, baseline_path))
    # gain = baseline_pin - current_pin, so this pin yields gain == min_gain - 1.0.
    target = base - (min_gain - 1.0)

    def fn(raw):
        hd = None
        out = []
        for l in raw:
            c = l.split("\t")
            if hd is None and c[0] == COL_ID:
                hd = c
            if hd is not None and c[0] == cid and len(c) > hd.index(COL_PIN):
                c[hd.index(COL_PIN)] = "%.6f" % target
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def self_check(baseline, current, min_low, min_high):
    """Prove the gate can go GREEN, and that the corruptions below go RED.

    Case 2 is the removal negative control and is the reason `_is_exactly_zero` exists: it
    asserts the pipeline is exactly deterministic, so that a future non-determinism in the
    probe or analyzer cannot quietly turn 'correction absent' into 'correction present but
    noisy' while still producing a plausible number."""
    high_cell = sorted(k for k, v in CANONICAL.items() if v[1] == "HIGH")[0]
    low_cell = sorted(k for k, v in CANONICAL.items() if v[1] == "LOW")[0]
    # NC-1 shifts by threshold + margin, NOT by the threshold exactly: a cell landing
    # exactly on `gain >= min_gain` is decided by float representation, so a
    # threshold-equality fixture would be testing the arithmetic of the fixture rather
    # than the gate. (A real threshold must therefore be set with margin over the weakest
    # measured cell -- see the header.)
    cases = [
        # (label, expected rc, mutated baseline, mutated current, extra assertion)
        # BOTH mutation slots take the SAME thing: an OP, i.e. `fn(lines) -> lines`, which the
        # driver below applies via _rewrite(path, op). They did not always: the baseline slot
        # was wrapped in _rewrite while the current slot was CALLED as `mutc(current)`, so a
        # current-side fixture's op received the PATH where it expected rows -- `[l for l in
        # path]` yields its characters, and `parse()` then died on a list. NC-5 was the first
        # current-side fixture, so the self-check crashed there and cases 5-7, 9 and 10 had
        # never run at all. Three fixtures had been written to match the buggy call (NC-1 and
        # NC-2 wrapped themselves in _rewrite; NC-8 pre-applied it eagerly, which also leaked
        # its temp file whenever an earlier case aborted). All three are now plain ops.
        ("NC-1 every cell improved past its role's threshold -> must PASS",
         0, None, _improve_all_past_threshold(min_low, min_high), None),
        ("NC-2 correction ABSENT (current is a byte-identical copy of the baseline): every "
         "delta exactly 0.0, and the verdict must be criterion-RED (1), never structural (2)",
         1, None, _as_baseline_rows(baseline), "exact_zero"),
        ("NC-3 a required baseline row dropped -> structural",
         2, _drop_row("drone1_classic_88200_t50"), None, None),
        ("NC-4 a duplicate baseline row -> structural",
         2, _dup_row("drone4_classic_44100_t20"), None, None),
        ("NC-5 a current cell renamed out of the canonical set -> structural",
         2, None, _set_col("drone4_classic_96000_t80", COL_ID, "drone4_classic_96000_t81"),
         None),
        ("NC-6 a current cell's acceptance column left unpopulated ('-') -> structural",
         2, None, _set_col(low_cell, COL_PIN, "-"), None),
        ("NC-7 a current cell's sr edited to disagree with the canonical set -> structural",
         2, None, _set_col(low_cell, COL_SR, "22050"), None),
        ("NC-8 ONE high cell pushed 1 dB below its threshold -> criterion RED (not structural)",
         1, None, _worsen_one_below_threshold(baseline, high_cell, min_high), None),
        ("NC-9 a cell whose f0 pushes dt past the kernel's 0.5 domain -> structural",
         2, _set_col(low_cell, COL_F0, "40000.0"), None, None),
        ("NC-10 a non-positive model f0 -> structural",
         2, _set_col(low_cell, COL_F0, "0.0"), None, None),
    ]
    print("self-check: (1) must PASS; (2),(8) criterion-RED; (3)-(7),(9),(10) structural-RED")
    bad, tmps = 0, []
    try:
        for label, want, mutb, mutc, extra in cases:
            bp = baseline if mutb is None else _rewrite(baseline, mutb)
            cp = current if mutc is None else _rewrite(current, mutc)
            if mutb is not None:
                tmps.append(bp)
            if mutc is not None:
                tmps.append(cp)
            rc, _ = evaluate(bp, cp, min_low, min_high)
            ok = (rc == want)
            note = ""
            if extra == "exact_zero":
                ez = _is_exactly_zero(bp, cp)
                ok = ok and ez
                note = "  [exact-zero deltas: %s]" % ("YES" if ez else "NO")
            bad += 0 if ok else 1
            print("    %-5s rc=%d (want %d)  %s%s"
                  % ("OK  " if ok else "WRONG", rc, want, label, note))
    finally:
        # NEVER unlink an input. `--self-check` runs against the REAL committed baseline and
        # a real analyzer capture, and a mutation helper that returns one of those paths
        # would otherwise delete committed evidence on the way out. This guard is not
        # hypothetical: an earlier revision let NC-2 pass the baseline itself as `--current`
        # and the cleanup removed tools/gh19_classic_saw_naive_baseline.tsv.
        inputs = {os.path.realpath(p) for p in (baseline, current)}
        for p in tmps:
            if os.path.realpath(p) in inputs:
                raise SystemExit("self-check cleanup refused to delete an INPUT file: %s" % p)
            os.unlink(p)
    print("self-check: %s" % ("PASS" if not bad else "FAIL (%d case(s) wrong)" % bad))
    return (1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description="GH#19 S2 classic-drone saw polyBLEP acceptance gate")
    ap.add_argument("--baseline", required=True,
                    help="committed naive baseline TSV (tools/gh19_classic_saw_naive_baseline.tsv)")
    ap.add_argument("--current", required=True, help="current (corrected) analyzer TSV/stdout")
    ap.add_argument("--min-gain-db", type=float, default=6.0,
                    help="min dB improvement for BOTH roles unless a role override is "
                         "given. TRAP: the default 6.0 is NOT usable for S2. The LOW lane's "
                         "measured ceiling is +5.88 dB, so 6.0 fails all 24 LOW cells at once. "
                         "Pass --min-gain-low-db / --min-gain-high-db instead; the two roles "
                         "have different ceilings and one number cannot serve both.")
    ap.add_argument("--min-gain-low-db", type=float, default=None,
                    help="override for the LOW role (drone1_classic + drone4_classic, both LOW-band); default: --min-gain-db")
    ap.add_argument("--min-gain-high-db", type=float, default=None,
                    help="override for the HIGH role (drone4_classic_v0); default: --min-gain-db")
    ap.add_argument("--out", default=None, help="also write the report to this path")
    ap.add_argument("--self-check", action="store_true",
                    help="prove green-ability + ten corruptions go RED")
    args = ap.parse_args()

    min_low = args.min_gain_low_db if args.min_gain_low_db is not None else args.min_gain_db
    min_high = args.min_gain_high_db if args.min_gain_high_db is not None else args.min_gain_db

    print("GH#19 S2 classic-drone saw polyBLEP acceptance")
    print("  basis   = blpin_full_db (pinned analytic f0 + analytic window-start phase; "
          "NOT blref_full_db)")
    print("  scope   = 36 cells (24 LOW: drone1_classic x12 + drone4_classic x12; "
          "12 HIGH: drone4_classic_v0 x12)")
    # The label must follow the NUMBER's provenance, not just be printed unconditionally.
    # Once the thresholds are measured and approved, still labelling them "PROVISIONAL until
    # measured" contradicts the ruling that the threshold is set after measurement (@Kimi
    # 612691ea (5)) -- and the CI log echoes this line verbatim, so the mislabel would ship.
    if args.min_gain_low_db is not None and args.min_gain_high_db is not None:
        prov = ("per-role thresholds supplied by the caller, not the built-in default "
                "(S2: measured on the fixed arm, approved before the gate was wired)")
    elif args.min_gain_low_db is None and args.min_gain_high_db is None:
        prov = ("PROVISIONAL -- built-in default, NOT measured. One value applies to BOTH "
                "roles, and 6.0 would fail the whole LOW lane (measured ceiling +5.88 dB)")
    else:
        prov = ("PARTIAL per-role override -- one role measured, the other still on "
                "--min-gain-db, whose default is not usable here")
    print("  thresholds: LOW >= %g dB, HIGH >= %g dB  [%s]" % (min_low, min_high, prov))
    print("")

    rc, lines = evaluate(args.baseline, args.current, min_low, min_high)
    for s in lines:
        print(s)

    if args.self_check:
        print("")
        rc = self_check(args.baseline, args.current, min_low, min_high) or rc

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
