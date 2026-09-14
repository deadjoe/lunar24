#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s3_kernel_sweep.py — task #118 (GH #19 S3): compile and run the KERNEL-DOMAIN sweep.
#
# WHY THE SWEEP IS A SEPARATE DRIVER AND WHY THIS SCRIPT EXISTS. tests/probes/gh19_s3_pulse_probe.cpp
# measures the PRODUCT -- it renders real audio through encode -> owner.applyDeviceState ->
# processBlock and asks what the spectrum did. The defect @Codex e144b61 found is not in that
# instrument's field of view: it is a discontinuity in DUTY at a fixed phase, and a residual/spectral
# metric is blind to it, while the cells the probe renders all sit at duty values where the refuted
# guard happened to be constant. The sweep therefore compiles the header directly, with no product
# link at all, and sweeps (dt, duty, t) instead of rendering the product.
#
# A build of that driver against a header that does not compile is a TOOL failure, not a verdict, and
# is reported as such rather than as a measurement. The one assertion this script makes is the
# driver's own POSITIVE CONTROL: the guarded formula transcribed from the refuted revision (58f41be)
# must still show the 0.5625 jump at the review's coordinate. A driver that cannot reproduce the
# refutation is not evidence about the current header, and its output must not be quoted -- so the
# run fails closed instead. Nothing here asserts anything about the CURRENT header: what it measures
# is for the report and the reader to judge.

import argparse
import math
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "tools/gh19_s3_pulse_kernel_sweep.cpp"
BINARY = "/tmp/gh19_s3_sweep"

# The refuted revision's jump at dt = 220/44100, t = dt/4, duty = 2*dt +- 1e-12, as measured by the
# review and reproduced by the driver's own `guardedPulse`. The tolerance is loose on purpose: this
# is an "is the control live at all" test, not a reproduction of a number to 15 digits.
POSITIVE_CONTROL = ("REPRO jump guarded=", 0.5625, 1e-9)

# The review's own coordinate (144b1339), as the two numbers it printed. These are now the UNCAPPED
# arm's values (`OVER-REPRO-UNC`), which is exactly what the review was looking at: the pre-cap
# header. So the review's reproduction has become this section's POSITIVE CONTROL -- the tool must
# still show the old defect at the old coordinate, or it is not measuring the thing the cap changed.
# Both numbers are required, not merely a jump of the right size: a driver that produced a 4/9-ish
# step at some other coordinate would satisfy a size check and say nothing about the reported case.
REVIEW_OVER_UNC = (-0.928888888817778, -0.484444444337778)
# The same coordinate's closed form for the residual's own step, (2 - 1/dt)^2 at dt = 0.75.
REVIEW_OVER_CLOSED = (2.0 - 1.0 / 0.75) ** 2
# The eps the driver uses for the two-sided comparisons whose result is quoted. Named here rather
# than buried in the driver so a reader can convert any "difference" column back into a slope.
REVIEW_OVER_EPS = 1e-10

# The frozen inputs the reachable-box section is built from. Each is (path, needle, what); the
# runner re-reads them so a moved constant breaks the evidence instead of silently invalidating it.
BOX_SOURCES = [
    ("spec/machine/lunar24.json", '"stable_id": "vco_a.v_oct_in"', "V/OCT jack"),
    ("spec/machine/lunar24.json", '"stable_id": "vco_a.cv_in"', "CV jack"),
    ("spec/machine/lunar24.json", '"stable_id": "vco_a.tune"', "tune param"),
    ("spec/machine/lunar24.json", '"stable_id": "vco_a.oct_sel"', "oct_sel param"),
    ("core/include/lunar24/core/machine_definition.h", "kVcoBaseHzProvisional", "VCO base Hz"),
]


def preflight_box_sources():
    """Re-read the constants the box section assumes and report them, so the numbers in the
    evidence file are anchored to files rather than to a comment. Returns the list of lines to
    print, or raises RuntimeError with the missing source named."""
    lines = []
    import json
    spec = None
    for path, needle, what in BOX_SOURCES:
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            raise RuntimeError("box source %s (%s) is missing" % (path, what))
        if path.endswith(".json"):
            if spec is None:
                with open(full) as fh:
                    spec = json.load(fh)
            continue
        with open(full) as fh:
            txt = fh.read()
        if needle not in txt:
            raise RuntimeError("box source %s no longer contains %r (%s)" % (path, needle, what))
    if spec is None:
        raise RuntimeError("spec/machine/lunar24.json could not be read")

    def mod(sid):
        for m in spec["modules"]:
            if m.get("stable_id") == sid:
                return m
        raise RuntimeError("module %s not in the spec" % sid)

    va = mod("vco_a")
    want = {
        "vco_a.tune": (-1.0, 1.0),
        "vco_a.oct_sel": (0.0, 2.0),
        "vco_a.cv_amt": (0.0, 1.0),
    }
    for p in va["parameters"]:
        sid = p.get("stable_id")
        if sid in want:
            lo, hi = want.pop(sid)
            if float(p["min"]) != lo or float(p["max"]) != hi:
                raise RuntimeError("%s range moved to [%s, %s]" % (sid, p["min"], p["max"]))
            lines.append("BOX-SRC spec %-16s min=%s max=%s" % (sid, p["min"], p["max"]))
    if want:
        raise RuntimeError("spec lost parameters: %s" % ", ".join(sorted(want)))
    for j in va["jacks"]:
        sid = j.get("stable_id")
        if sid in ("vco_a.v_oct_in", "vco_a.cv_in"):
            lines.append("BOX-SRC spec %-16s nominal=[%s, %s] transfer=%s"
                         % (sid, j["nominalMin"], j["nominalMax"], j.get("transfer")))
    # The base Hz the whole box hangs off, read out of the production header rather than restated.
    with open(os.path.join(ROOT, "core/include/lunar24/core/machine_definition.h")) as fh:
        for ln in fh:
            if "kVcoBaseHzProvisional" in ln and "constexpr" in ln:
                lines.append("BOX-SRC header %s" % ln.strip())
    return lines


def fm_scan(scan_root=None):
    """`instHz = frequencyHz() + fmDevHz_ * fmCv_` is the only place a step could become something
    other than a pitch. Both setters are declared and never CALLED, so on every product path the FM
    term is exactly 0. That is a claim about the tree, so it is checked against the tree here; if a
    caller appears, the box section's central identity (dt = frequencyHz()/sr) stops holding and
    this script fails closed rather than reporting a stale conclusion.

    The call regex requires a member access, so the declarations in vco.h and any prose that merely
    names the setters do not count as callers. Returns (calls, mentions)."""
    call_re = re.compile(r"(\.|->)setFm(Depth|Cv)\s*\(")
    calls, mentions = [], []
    for base, dirs, files in os.walk(scan_root or ROOT):
        dirs[:] = [d for d in dirs if not d.startswith("build")]
        for f in files:
            if not f.endswith((".h", ".hpp", ".cpp", ".cc")):
                continue
            p = os.path.join(base, f)
            try:
                with open(p, errors="replace") as fh:
                    for i, ln in enumerate(fh, 1):
                        if "setFmDepth" not in ln and "setFmCv" not in ln:
                            continue
                        rec = "%s:%d: %s" % (os.path.relpath(p, scan_root or ROOT), i, ln.strip())
                        mentions.append(rec)
                        if call_re.search(ln):
                            calls.append(rec)
            except OSError:
                pass
    return calls, mentions


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join("report", "gh19-s3-pulse-aa",
                                                  "kernel_sweep.txt"),
                    help="where to capture the sweep's stdout as evidence")
    ap.add_argument("--cc", default="c++")
    ap.add_argument("--binary", default=BINARY)
    # Both overrides exist for run_gh19_s3_over_negatives.py, which mutates COPIES of the headers (no
    # tracked file is touched) and must then be able to point this runner at them. A production run
    # passes neither.
    ap.add_argument("--include-dir", default=os.path.join("core", "include"),
                    help="where lunar24/core/*.h is read from (the negative controls point this at "
                         "a mutated copy; a normal run uses the production tree)")
    ap.add_argument("--fm-scan-root", default=ROOT,
                    help="tree the FM-caller scan walks (the negative controls plant a caller in a "
                         "scratch dir; a normal run scans the repo)")
    args = ap.parse_args(argv)
    os.chdir(ROOT)

    cmd = [args.cc, "-std=c++17", "-O2", "-I", args.include_dir, SRC, "-o", args.binary]
    print("compiling the header-only sweep against the CURRENT header:")
    print("  " + " ".join(cmd))
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        # A compile failure here is the header not building, which is a tool failure and is NOT a
        # statement about the kernel's behaviour. Fail closed and say which it is.
        sys.stderr.write("TOOL FAILURE (not a measurement): the sweep did not compile.\n")
        sys.stderr.write(b.stderr[-4000:] + "\n")
        return 2

    p = subprocess.run([args.binary], capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write("TOOL FAILURE (not a measurement): the sweep exited %d.\n" % p.returncode)
        sys.stderr.write(p.stderr[-4000:] + "\n")
        return 2
    out = p.stdout

    lines = out.splitlines()
    for ln in lines:
        print(ln)

    ok = True
    needle, want, tol = POSITIVE_CONTROL
    hits = [ln for ln in lines if ln.startswith(needle)]
    if not hits:
        print("\n*** POSITIVE CONTROL MISSING: the driver printed no %r line. Its output is not "
              "evidence about any formula. ***" % needle)
        ok = False
    else:
        # The line carries TWO numbers (the refuted formula's jump and the current header's), so parse
        # the named field rather than splitting on the first '=': splitting would pick up the second
        # number as part of the first field and crash the driver's own verdict check.
        m = re.search(r"guarded=([-+0-9.eE]+)", hits[0])
        if m is None:
            print("\n*** POSITIVE CONTROL UNPARSEABLE: %r has no guarded=<number> field. ***"
                  % hits[0])
            ok = False
        else:
            got = float(m.group(1))
            if abs(got - want) > tol:
                print("\n*** POSITIVE CONTROL DEAD: the refuted formula's reproduced jump is %.15f, "
                      "expected %.4f. The driver cannot see the refutation it exists to generalise, so "
                      "its numbers about the current header mean nothing. ***" % (got, want))
                ok = False
            else:
                print("\npositive control live: the refuted formula's jump reproduces %.4f." % got)

    # ---- the box the out-of-domain measurement is about -----------------------------------------
    # Printed and captured BEFORE the checks so that a failure still leaves the anchors (which files
    # the numbers came from) in the evidence file next to the failure.
    try:
        box_lines = preflight_box_sources()
    except (RuntimeError, OSError, ValueError) as e:
        print("\n*** BOX SOURCES UNREADABLE: %s. The reachable-box numbers cannot be anchored to "
              "the files they came from. ***" % e)
        box_lines = []
        ok = False
    fm_calls, fm_mentions = fm_scan(args.fm_scan_root)
    if fm_calls:
        print("\n*** FM SETTER GAINED A CALLER: %s. `instHz = frequencyHz() + fmDevHz_*fmCv_` is no "
              "longer identically frequencyHz(), so dt = frequencyHz()/sr (the identity the whole box "
              "section rests on) is no longer true. ***" % "; ".join(fm_calls))
        ok = False
    fm_lines = ["BOX-SRC fm  %s  (declaration/mention only, not a call site)" % h
                for h in fm_mentions]
    anchor = box_lines + fm_lines
    if anchor:
        print("")
        for ln in anchor:
            print(ln)

    def field(prefix, name):
        """The named `name=<number>` field of the FIRST line starting with `prefix`. Returns
        (value, raw) or (None, None) when the line or the field is absent -- absence is a failure
        of the driver, never a silent 0."""
        for ln in lines:
            if ln.startswith(prefix):
                m = re.search(re.escape(name) + r"\s*=\s*([-+0-9.eE]+)", ln)
                return (float(m.group(1)), ln) if m else (None, ln)
        return None, None

    def field_re(prefix, pattern):
        """The first capture of `pattern` on a line starting with `prefix`, for the lines whose
        number is not in `name=value` form."""
        for ln in lines:
            if ln.startswith(prefix):
                m = re.search(pattern, ln)
                if m:
                    return float(m.group(1)), ln
        return None, None

    def need(label, got, want, tol, why):
        nonlocal ok
        if got is None:
            print("\n*** %s: the driver printed no %s line at all, so this claim has no "
                  "measurement behind it. ***" % (label, label))
            ok = False
        elif abs(got - want) > tol:
            print("\n*** %s: measured %.15g, expected %.15g (tol %g). %s ***"
                  % (label, got, want, tol, why))
            ok = False
        else:
            print("%s: %.15g == %.15g" % (label, got, want))

    print("\n-- out-of-domain (dt > kPolyblepMaxDt) directed checks --")
    print("NOTE on the two arms. Every out-of-domain number is printed TWICE: *-CAP is the current "
          "header (kernel WIDTH capped at kPolyblepMaxDt, phase still advancing by the true step), "
          "*-UNC is the same formula with the width uncapped, i.e. the pre-cap header. UNC is this "
          "section's POSITIVE CONTROL: the checks below are conjunctions over both arms, so a run in "
          "which UNC stops showing the old defect fails just as loudly as one in which CAP starts "
          "showing it. A verdict from a single arm would be unreadable either way.")

    # 1. STOP / REVERSE: the kernel's first statement must make dt <= 0 bit-identical to naive.
    v, _ = field("OVER-STOP  total", "non-identical")
    need("OVER-STOP bit-identical naive", v, 0.0, 0.0,
         "a non-positive step must return the naive pulse EXACTLY; anything else means the "
         "correction is doing arithmetic on a step it has no kernel for")

    # 2. The review's coordinate. BOTH arms are asserted, in opposite directions: the uncapped arm
    #    must still reproduce the review's two numbers to the digit (the control is live), and the
    #    capped arm must NOT reproduce them (the cap changed the cell). A run where the two arms
    #    agree here would mean the cap is not reaching this coordinate at all.
    unc_lo, unc_hi = field_re("OVER-REPRO-UNC", r"t=0\.75-1e-10\s+out=([-+0-9.eE]+)")
    unc_hi2, _ = field_re("OVER-REPRO-UNC", r"t=0\.75\+1e-10\s+out=([-+0-9.eE]+)")
    need("OVER-REPRO-UNC lo (review's own value)", unc_lo, REVIEW_OVER_UNC[0], 1e-12,
         "the pre-cap header must still produce the value the review reported, or the tool has "
         "stopped looking at the coordinate the defect was found at")
    need("OVER-REPRO-UNC hi (review's own value)", unc_hi2, REVIEW_OVER_UNC[1], 1e-12,
         "same as above, the second of the review's two numbers")
    unc_jump, _ = field("OVER-REPRO-UNC", "diff")
    need("OVER-REPRO-UNC jump == closed form", unc_jump, REVIEW_OVER_CLOSED, 1e-9,
         "the uncapped arm's step across t=dt must still BE (2-1/dt)^2, not merely present")
    cap_lo, _ = field_re("OVER-REPRO-CAP", r"t=0\.75-1e-10\s+out=([-+0-9.eE]+)")
    cap_hi, _ = field_re("OVER-REPRO-CAP", r"t=0\.75\+1e-10\s+out=([-+0-9.eE]+)")
    cap_jump, _ = field("OVER-REPRO-CAP", "diff")
    if cap_lo is None or cap_hi is None:
        print("\n*** OVER-REPRO-CAP: the capped arm printed no coordinate values. ***")
        ok = False
    elif cap_lo == unc_lo and cap_hi == unc_hi2:
        print("\n*** OVER-REPRO-CAP: the capped arm is BIT-IDENTICAL to the uncapped one at "
              "dt=0.75, so the cap did not reach this coordinate and the whole out-of-domain "
              "section is measuring the pre-cap header. ***")
        ok = False
    else:
        print("OVER-REPRO-CAP differs from the control: %.15f/%.15f vs %.15f/%.15f"
              % (cap_lo, cap_hi, unc_lo, unc_hi2))
    # The capped step is the leftover of a CONTINUOUS function sampled at +-eps, so it must be
    # O(eps) and nowhere near the closed form. Both bounds are stated against eps rather than as
    # magic numbers, so the check travels if REVIEW_OVER_EPS ever changes.
    if cap_jump is None:
        print("\n*** OVER-REPRO-CAP: no 'diff' field, so the capped step has no measurement. ***")
        ok = False
    elif abs(cap_jump) > 10.0 * REVIEW_OVER_EPS:
        print("\n*** OVER-REPRO-CAP: the capped step across t=dt is %.6g, more than 10*eps (%.6g). "
              "A continuous function's two-sided difference is O(eps) with a small constant; this "
              "is large enough to be a jump. ***" % (cap_jump, 10.0 * REVIEW_OVER_EPS))
        ok = False
    elif abs(abs(cap_jump) - REVIEW_OVER_CLOSED) < 1e-6:
        print("\n*** OVER-REPRO-CAP: the capped step %.6g sits at the closed form %.6g, i.e. the "
              "residual's own step is still in the output. ***" % (cap_jump, REVIEW_OVER_CLOSED))
        ok = False
    else:
        print("OVER-REPRO-CAP step %.6g is O(eps) (eps=%.1e) and far from the closed form %.6g"
              % (cap_jump, REVIEW_OVER_EPS, REVIEW_OVER_CLOSED))
    # 2b. The rejected candidate's behaviour, on the record: switching the correction OFF out of
    #     domain makes the cell exactly the naive pulse. That is what the header note refuses.
    v, _ = field("OVER-REPRO-GUARDED", "naive")
    gv, _ = field_re("OVER-REPRO-GUARDED", r"guarded=([-+0-9.eE]+)")
    need("OVER-REPRO-GUARDED switch-off arm == naive", gv, v, 0.0,
         "the rejected 'fall back to naive out of domain' candidate must still be visible to this "
         "tool as exactly the naive pulse; if it is not, the control for the rejected strategy is "
         "dead and the header note's non-claim is unmeasured")
    # 2c. At dt = 0.5 the cap IS the identity (w = min(0.5, 0.5)), so the two arms must agree
    #     BIT FOR BIT at the boundary -- the same fact OVER-STRUCT-CAPISIDENTITY checks on a grid,
    #     pinned here at the single coordinate the review quoted.
    x0c, _ = field_re("OVER-REPRO-X0", r"cap_diff=([-+0-9.eE]+)")
    x0u, _ = field_re("OVER-REPRO-X0", r"unc_diff=([-+0-9.eE]+)")
    need("OVER-REPRO-X0 cap == unc at dt=0.5", x0c, x0u, 0.0,
         "dt=0.5 is the boundary the cap takes its value from, so the two arms must agree there "
         "exactly; a difference means the cap is not w = min(dt, cap)")

    # 3. Every dt in (0.5,1), BOTH arms. The capped difference must SHRINK with eps and must never
    #    approach the closed form; the uncapped one must sit ON the closed form. These are opposite
    #    assertions on the same coordinate, which is what makes the pair a control rather than two
    #    reports.
    res_cap_re = re.compile(r"^OVER-RESIDUAL-CAP (\d+\.\d+) +([-+0-9.eE]+) +([-+0-9.eE]+) +"
                            r"([-+0-9.eE]+) +([-+0-9.eE]+)")
    res_unc_re = re.compile(r"^OVER-RESIDUAL-UNC (\d+\.\d+) +([-+0-9.eE]+) +([-+0-9.eE]+)")
    cap_rows = 0
    for ln in lines:
        m = res_cap_re.match(ln)
        if not m:
            continue
        cap_rows += 1
        dt, e6, e9, e10, e12 = (float(m.group(i)) for i in range(1, 6))
        # LINEAR in eps, not merely shrinking. A continuous output's two-sided difference is
        # `C*eps + O(eps^2)`, so each column's value divided by the next must equal the eps ratio
        # between them (1000, 10, 100 for this column set) to within the O(eps^2) term. Monotonicity
        # alone would be satisfied by a function that is merely very steep; proportionality is what
        # "no jump here" actually means, so it is asserted as proportionality.
        for (ea, eb, la, lb) in ((e6, e9, 1e-6, 1e-9), (e9, e10, 1e-9, 1e-10),
                                 (e10, e12, 1e-10, 1e-12)):
            if ea == 0.0 and eb == 0.0:
                continue  # both below the print resolution; nothing to ratio
            if eb == 0.0:
                print("\n*** OVER-RESIDUAL-CAP dt=%.7f: the difference at eps=%g is exactly 0 while "
                      "at eps=%g it is %.3e, so the columns are not a linear function of eps. ***"
                      % (dt, lb, la, ea))
                ok = False
                continue
            got = abs(ea / eb)
            want = la / lb
            if not (0.5 * want <= got <= 2.0 * want):
                print("\n*** OVER-RESIDUAL-CAP dt=%.7f: the difference goes %.3e (eps=%g) -> %.3e "
                      "(eps=%g), a ratio of %.4g, but a continuous output's difference is "
                      "proportional to eps and the eps ratio is %.4g. Not proportional => there is "
                      "a jump in these columns. ***" % (dt, ea, la, eb, lb, got, want))
                ok = False
        # O(eps), so bounded by a small multiple of the largest eps used.
        if abs(e6) > 10.0 * 1e-6:
            print("\n*** OVER-RESIDUAL-CAP dt=%.7f: the difference at eps=1e-6 is %.6g, which is "
                  "not O(eps) for any constant below 10 -- larger than the eps itself by more than "
                  "the slope can explain. ***" % (dt, e6))
            ok = False
    unc_rows = 0
    for ln in lines:
        m = res_unc_re.match(ln)
        if not m:
            continue
        unc_rows += 1
        dt, e10, closed = float(m.group(1)), float(m.group(2)), float(m.group(3))
        # The two-sided difference of a step is `jump + O(eps * slope)`, so the leftover is a
        # RELATIVE quantity and the tolerance has to be one too, with an absolute floor for the
        # small end of the column (where the closed form itself is barely above the eps term).
        # 1e-6 relative is ~4 orders looser than what the driver's own print can resolve, and still
        # 3 orders tighter than the gap it is contrasted against (the capped arm sits at ~0).
        tol = max(1e-9, 1e-6 * closed)
        # Below 1e-3 the closed form is within reach of the eps term, so the "sits at" assertion is
        # only informative once the step is resolvable above it.
        if closed > 1e-3 and abs(e10 - closed) > tol:
            print("\n*** OVER-RESIDUAL-UNC dt=%.7f: the uncapped difference at eps=1e-10 is %.12e "
                  "but the closed form (2-1/dt)^2 is %.12e (tol %.3e). The control is not live, so "
                  "the capped arm's numbers are not evidence that anything changed. ***"
                  % (dt, e10, closed, tol))
            ok = False
        if closed > 1e-3 and abs(e10) < 1e-6:
            print("\n*** OVER-RESIDUAL-UNC dt=%.7f: the uncapped difference has collapsed to %.3e, "
                  "i.e. the control no longer carries the residual's own step. ***" % (dt, e10))
            ok = False
    if cap_rows == 0 or unc_rows == 0:
        print("\n*** OVER-RESIDUAL: capped rows=%d uncapped rows=%d -- one arm is missing, so "
              "there is nothing to contrast. ***" % (cap_rows, unc_rows))
        ok = False
    elif cap_rows != unc_rows:
        print("\n*** OVER-RESIDUAL: capped rows=%d but uncapped rows=%d; the two arms are not "
              "over the same dt set. ***" % (cap_rows, unc_rows))
        ok = False
    else:
        print("OVER-RESIDUAL: %d dt values x 2 arms -- capped shrinks linearly in eps and stays "
              "away from the closed form; uncapped sits on it (control live)" % cap_rows)

    # 4. The jump-versus-slope discriminator, and boundedness, from the same grid, BOTH arms.
    #    A JUMP's t-gap is flat in the t resolution; a CONTINUOUS function's is O(1/nT), so the
    #    1e3/1e5 ratio is ~1 for the former and ~100 for the latter. The expected pattern is now an
    #    EXACT INVERSION of the pre-cap one, and both halves are asserted so neither can drift.
    cap_re = re.compile(r"^OVER-GRID-CAP +(\S+) +([0-9.]+) +([-+0-9.eE]+) +([0-9.]+) +"
                        r"([0-9.]+) +([0-9.]+) +([0-9.]+) +([0-9.]+)")
    unc_re = re.compile(r"^OVER-GRID-UNC +(\S+) +([0-9.]+) +([0-9.]+) +([0-9.]+) +([0-9.]+) +"
                        r"([0-9.]+) +(\S+)")
    cap_grid, unc_grid = 0, 0
    worst_ex = None
    for ln in lines:
        m = cap_re.match(ln)
        if not m:
            continue
        cap_grid += 1
        dt = float(m.group(1)); ex = float(m.group(3)); ratio = float(m.group(8))
        worst_ex = ex if worst_ex is None else max(worst_ex, ex)
        if ratio < 50.0:
            print("\n*** OVER-GRID-CAP dt=%s: the t-gap ratio (1e3/1e5) is %.4g, so the t-gap did "
                  "NOT fall with resolution. Under the cap every dt uses w = min(dt, 0.5), and "
                  "w = 0.5 is continuous on the whole period, so a flat gap at ANY dt contradicts "
                  "the cap. ***" % (m.group(1), ratio))
            ok = False
    for ln in lines:
        m = unc_re.match(ln)
        if not m:
            continue
        unc_grid += 1
        dt = float(m.group(1)); ratio = float(m.group(6)); closed = m.group(7)
        if dt == 0.5:
            if ratio < 50.0:
                print("\n*** OVER-GRID-UNC dt=0.5: ratio %.4g -- the uncapped arm must agree with "
                      "the capped one at the boundary (w = dt = 0.5 there), and it does not. ***"
                      % ratio)
                ok = False
        else:
            # dt > 0.5: the residual's own step is in the output, so the gap must be FLAT in the t
            # resolution -- at EVERY dt past the boundary, from 0.5000001 to 1e300, not only on
            # (0.5,1). A ratio between the two signatures would mean the row is measuring neither.
            if ratio > 2.0:
                print("\n*** OVER-GRID-UNC dt=%s: ratio %.4g -- the t-gap FELL with resolution, "
                      "i.e. the control no longer carries the residual's own step and the capped "
                      "arm's flat gap is contrasted against nothing. ***" % (m.group(1), ratio))
                ok = False
            # The closed form is only defined where the residual's step is a step of the residual;
            # the column must be filled exactly on (0.5,1) and left empty outside it, or the two
            # arms are not reporting the same quantity.
            if dt < 1.0 and closed == "n/a":
                print("\n*** OVER-GRID-UNC dt=%s: the closed-form column is 'n/a' inside (0.5,1), "
                      "where it is exactly the value the flat gap must equal. ***" % m.group(1))
                ok = False
            if dt >= 1.0 and closed != "n/a":
                print("\n*** OVER-GRID-UNC dt=%s: the closed-form column reads %r at dt >= 1, "
                      "where (2-1/dt)^2 no longer describes the residual's step. ***"
                      % (m.group(1), closed))
                ok = False
    if cap_grid == 0 or unc_grid == 0:
        print("\n*** OVER-GRID: capped rows=%d uncapped rows=%d -- one arm is missing, so the "
              "discriminator has nothing to contrast. ***" % (cap_grid, unc_grid))
        ok = False
    elif cap_grid != unc_grid:
        print("\n*** OVER-GRID: capped rows=%d but uncapped rows=%d; the two arms are not over the "
              "same dt set. ***" % (cap_grid, unc_grid))
        ok = False
    else:
        print("OVER-GRID: %d dt values x 2 arms -- capped t-gap falls ~100x per 100x resolution at "
              "EVERY dt; uncapped holds a flat t-gap in (0.5,1) at (2-1/dt)^2" % cap_grid)
    if worst_ex is None:
        print("\n*** OVER-GRID-CAP: no excess column was printed. ***")
        ok = False
    elif worst_ex > 0.0:
        print("\n*** OVER-GRID-CAP: max|out| exceeds 1 by %.3e somewhere in the out-of-domain grid. "
              "***" % worst_ex)
        ok = False
    else:
        print("OVER-GRID-CAP: max|out| <= 1 everywhere measured out of domain (worst excess %.3e)"
              % worst_ex)

    # 5. STRUCTURE: the identities the header note actually claims, each as a directed count.
    v, _ = field("OVER-STRUCT-CAPISIDENTITY", "bit-different")
    need("OVER-STRUCT-CAPISIDENTITY", v, 0.0, 0.0,
         "min(dt, kPolyblepMaxDt) IS dt for dt <= 0.5, so the cap must be BIT-IDENTICAL to the "
         "uncapped formula in domain, not merely close. A non-zero count means the in-domain "
         "samples moved and the whole static evidence set is invalidated by this edit")
    v, _ = field("OVER-STRUCT-EXTENSION", "bit-different")
    need("OVER-STRUCT-EXTENSION", v, 0.0, 0.0,
         "every dt >= 0.5 must emit exactly the dt = 0.5 output, sample for sample. That identity "
         "is what lets the in-domain boundedness/continuity case analysis carry out of domain; "
         "without it the out-of-domain claims have no derivation behind them")
    v, _ = field("OVER-STRUCT-LABEL", "disagreements")
    need("OVER-STRUCT-LABEL", v, 0.0, 0.0,
         "polyblepPulseWindowsDisjoint must keep returning the same value now that the live width "
         "is w rather than dt; a disagreement means a cell's regime LABEL changed, which would "
         "silently reinterpret every static row keyed on disjointness")
    exv, _ = field("OVER-STRUCT-SUP", "excess_over_1")
    if exv is None:
        print("\n*** OVER-STRUCT-SUP: no excess_over_1 field, so the boundedness claim has no "
              "measurement behind it. ***")
        ok = False
    elif exv > 1e-12:
        print("\n*** OVER-STRUCT-SUP: sup|out| exceeds 1 by %.3e on a closed in-domain grid. ***"
              % exv)
        ok = False
    else:
        print("OVER-STRUCT-SUP: sup|out| - 1 = %.3e (floating-point round-off at the grid point; "
              "the bound is 1, not 1+eps)" % exv)

    # 6. REAL VCO: the directed checks that tie the kernel to the product path.
    v, _ = field("VCO-WIRING ", "bit-mismatches")
    need("VCO-WIRING emitted == kernel", v, 0.0, 0.0,
         "at kMorphRing+morph=1.0 the mix is exactly the pulse node, so tick()'s sample must equal "
         "polyblepPulse(phase(), effectiveDuty(), step) BIT FOR BIT -- in domain AND out of it. A "
         "mismatch means the product applies a different correction than the header under test")
    pw, _ = field("VCO-WIRING weights", "pulseWeight")
    tw, _ = field("VCO-WIRING weights", "triangleWeight")
    need("VCO-WIRING pulseWeight at morph=1", pw, 1.0, 0.0,
         "the wiring identity above only holds if the pulse node's weight is EXACTLY 1")
    need("VCO-WIRING triangleWeight at morph=1", tw, 0.0, 0.0,
         "and the triangle node's weight is EXACTLY 0; a non-zero weight would make the emitted "
         "sample a mix and the identity above a coincidence")
    v, _ = field("VCO-BLOCK", "bit-different")
    need("VCO-BLOCK chunking invariance", v, 0.0, 0.0,
         "the kernel holds no cross-sample state, so emitting 4096 samples in one call and in 64 "
         "calls must be bit-identical; a difference means the correction carries state it must not")
    vp, _ = field("VCO-SYNC", "phase_after_reset_nonzero")
    need("VCO-SYNC lands on phase 0", vp, 0.0, 0.0,
         "tick() resets cumPitch_ to 0 on a pending sync, so the post-reset sample must read phase "
         "0 for a reset at EVERY phase")
    vf, _ = field("VCO-SYNC", "formula_mismatches")
    need("VCO-SYNC emitted == documented formula", vf, 0.0, 0.0,
         "the post-reset sample must be 0.5*emittedAt_(0) + 0.5*(the un-reset emitted value), which "
         "is tick()'s `out -= 0.5*jmp` written the other way round")
    # `bitmatch` is on the driver's SECOND VCO-SYNC line (the phase/formula counters are on the
    # first), so this one has to scan past the first matching line -- `field` stops there by design.
    vb, _ = field_re("VCO-SYNC", r"bitmatch=([-+0-9.eE]+)")
    need("VCO-SYNC solved e0 == kernel at phase 0", vb, 1.0, 0.0,
         "e0 solved from the offset-0 arm must equal polyblepPulse(0, duty, step) exactly; that is "
         "what closes the sync identity over measured quantities alone")
    v, _ = field("VCO-STOP negative_pitch_returns", "negative_pitch_returns")
    need("VCO-STOP never reverses", v, 0.0, 0.0,
         "frequencyHz() clamps `p > 0 ? p : 0`, so no reachable configuration returns a negative "
         "pitch and the kernel's dt < 0 branch is unreachable from the product")
    stop_rows = [ln for ln in lines if ln.startswith("VCO-STOP ") and "freq=" in ln]
    if not stop_rows:
        print("\n*** VCO-STOP: no per-case stop rows, so the stop claim has no measurement. ***")
        ok = False
    for ln in stop_rows:
        m = re.search(r"freq=([-+0-9.eE]+).*samples!=first=(\d+)", ln)
        if not m:
            print("\n*** VCO-STOP unparseable: %r ***" % ln)
            ok = False
            continue
        if float(m.group(1)) != 0.0 or int(m.group(2)) != 0:
            print("\n*** VCO-STOP: %r -- the pitch is not clamped to exactly 0, or the emitted "
                  "signal is not constant. ***" % ln.strip())
            ok = False
    if stop_rows:
        print("VCO-STOP: %d stop routes, each freq=0 with a constant emitted sample"
              % len(stop_rows))
    # The driver prints this one as a prose line ending `= <value> at dt=<worst>`, not as a
    # `max=<value>` field, so it is read by pattern rather than by field name.
    v, _ = field_re("VCO-CROSSOVER max", r"= ([-+0-9.eE]+) at dt=")
    if v is None:
        print("\n*** VCO-CROSSOVER: no max-step field. ***")
        ok = False
    elif v > 2.0 * 2.5e-7 * 4.0:
        print("\n*** VCO-CROSSOVER: the largest step across the cap boundary is %.6g for a dt grid "
              "spacing of 2.5e-7. A continuous turn-on is bounded by a small multiple of the "
              "spacing; a larger step is a jump introduced by switching the cap in. ***" % v)
        ok = False
    else:
        print("VCO-CROSSOVER: max step %.6g across dt=0.5 at 2.5e-7 spacing (continuous turn-on)"
              % v)
    xrows = [ln for ln in lines if ln.startswith("VCO-CROSSOVER contract V/OCT")]
    if len(xrows) != 2:
        print("\n*** VCO-CROSSOVER: expected 2 contract V/OCT rows, got %d. ***" % len(xrows))
        ok = False
    else:
        want = {1.647131: 0, 1.769387: 1}
        for ln in xrows:
            m = re.search(r"V/OCT ([0-9.]+) V .*capped=(\d)", ln)
            if not m or want.get(float(m.group(1))) != int(m.group(2)):
                print("\n*** VCO-CROSSOVER: %r -- the contract point's cap state moved. The two "
                      "contract voltages bracket dt = kPolyblepMaxDt; if they no longer straddle "
                      "it, the crossover is not where the contract says it is. ***" % ln.strip())
                ok = False
        print("VCO-CROSSOVER: both contract V/OCT points land where the contract says "
              "(1.647131 V uncapped, 1.769387 V capped)")
    # The field name carries its own argument list (`mismatches_vs_out(dt=cap)=0`), which the
    # literal `name=` reader cannot see through.
    v, _ = field_re("VCO-CROSSOVER far side", r"mismatches_vs_out\(dt=cap\)=([-+0-9.eE]+)")
    need("VCO-CROSSOVER far side == out(dt=cap)", v, 0.0, 0.0,
         "just past the crossover the OBJECT's emitted samples must equal the standalone kernel "
         "evaluated at dt = kPolyblepMaxDt, which is the out-of-domain identity measured on the "
         "product rather than on the pure function")

    # 6. The reachable box, measured through the real Vco object.
    v, _ = field("BOX stop:", "max deviation")
    need("BOX stop emitted constant", v, 0.0, 0.0,
         "with the pitch clamped to 0 the phase must not advance at all")
    # Panel max is 440 * 2^(+3 + 1) at every sample rate.
    panel = 440.0 * 2.0 ** 4
    for ln in lines:
        if ln.startswith("BOX panel max") and "inside" in ln:
            m = re.search(r"f=\s*([0-9.]+)", ln)
            if not m or abs(float(m.group(1)) - panel) > 1e-9:
                print("\n*** BOX panel max: %r does not read f=%.6f. ***" % (ln, panel))
                ok = False
    # The declared box maxima, from the frozen registry ranges.
    exp_max = 440.0 * 2.0 ** (3 + 1 + 8 + 5)
    lin_max = 440.0 * 2.0 ** (3 + 1 + 8) * (1.0 + 5.0)
    need("BOX declared max CV exp", field("BOX declared max, CV exp", "f")[0], exp_max, 1e-6,
         "the declared box's exponential-CV maximum moved")
    need("BOX declared max CV lin", field("BOX declared max, CV lin", "f")[0], lin_max, 1e-6,
         "the declared box's linear-CV maximum moved")
    # The V/OCT at which the panel maximum leaves the contract: measured on the object, and
    # cross-checked against the closed form of the same law. Disagreement means either the law moved
    # or the bisection is not measuring what it claims to.
    thr = [ln for ln in lines if ln.startswith("BOX leaves the contract at V/OCT")]
    if not thr:
        print("\n*** BOX V/OCT threshold: no threshold line. Either no sample rate leaves the "
              "contract (the defect is then unreachable and that is a FINDING, not a silence) or the "
              "driver did not run. ***")
        ok = False
    else:
        for ln in thr:
            m = re.search(r"V/OCT = ([0-9.]+) V.*sr=([0-9]+)", ln)
            if not m:
                print("\n*** BOX V/OCT threshold unparseable: %r ***" % ln)
                ok = False
                continue
            got = float(m.group(1)); sr = float(m.group(2))
            want = math.log2(0.5 * sr / panel)
            need("BOX V/OCT threshold sr=%.0f" % sr, got, want, 1e-4,
                 "the measured threshold voltage disagrees with log2(0.5*sr/panel_max): the "
                 "bisection and the law it is measuring are not the same object")
    # Reverse: the emission cannot go backwards, it stops. Pinned as a measurement.
    v, _ = field("BOX CV lin -2V", "f")
    need("BOX reverse clamps to stop", v, 0.0, 0.0,
         "a negative CV must not produce a negative pitch (which would be a negative step)")

    if args.out:
        d = os.path.dirname(args.out)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(args.out, "w") as fh:
            fh.write("$ %s\n$ %s\n" % (" ".join(cmd), args.binary))
            fh.write("".join(ln + "\n" for ln in anchor))
            fh.write(out)
        print("captured to %s" % args.out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
