#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s3_coverage.py — task #118 (GH #19 S3): THE COVERAGE LOCK'S NEGATIVE CONTROLS.
#
# WHAT THIS IS FOR. gh19_s3_pulse_delta.py refuses to compute a delta unless both arms account for
# every cell the PINNED manifest declares. That refusal is the whole reason the matrices can be read as
# "the declared stimulus set was rendered and read". A lock nobody has tried to pick is not evidence,
# so this script tries to pick it, one way at a time, with the manifest HELD FIXED throughout.
#
# WHAT IS BEING LOCKED, AND THE HOLE IT CLOSES. @Codex reproduced the hole in the previous revision
# (msg 8aba3aeb): delete `vco_a_pulse_44100_220_pw10` from BOTH arms and decrement each arm's own
# `static` fence 80 -> 79, and the tool exited 0 while printing that every declared cell was present.
# It was: "declared" came from the arms, so what was locked was that the two arms agreed with EACH
# OTHER, not that they contained the expected stimulus set. Two independent things fix that, and BOTH
# are controlled here:
#
#   1. THE EXPECTATION IS A PINNED ARTIFACT. report/gh19-s3-pulse-aa/expected_cells.tsv is produced by
#      the probe's own buildPlan() from the DECLARED stimulus axes, before any render, and checked in.
#      The pin re-runs the probe and requires the plan it emits to be BYTE-IDENTICAL to that file, so
#      editing an axis without re-pinning it is a failure rather than a silent re-baseline.
#   2. BOTH ARMS ARE RECONCILED AGAINST IT, IN BOTH DIRECTIONS. A declared cell no arm consumed fails
#      (under-production or missed parse), and a consumed cell no manifest declares fails (an
#      undeclared stimulus). Neither arm is allowed to define the expectation.
#
# THE NEGATIVE CONTROLS, and what each one is for. Every arm-mutating control keeps its arm INTERNALLY
# CONSISTENT -- the fence is moved to match the data -- so the only thing that can reject it is the
# pinned-manifest lock. A control that left the fence inconsistent would be caught by the fence
# arithmetic instead, and would prove nothing about the lock.
#
#   drop_both        @Codex's exact reproduction: delete one cell from BOTH arms and move both fences
#                    to match. Every arm-internal count stays self-consistent, so only the pinned
#                    manifest can catch it. This is the control that exits 0 before the fix.
#   rename_both      the cell is not deleted, it is REPLACED by a grammar-legal but undeclared stimulus
#                    (`..._220_pw10` -> `..._225_pw10`). Counts are untouched and both arms still agree
#                    with each other; only "is declared" catches it. Deleting is easy to notice;
#                    swapping a legal id in is the shape a real axis edit would take.
#   drop_one_arm     the cell is missing from ONE arm only. Caught by the same manifest comparison, and
#                    it is the case a between-arms comparison also catches -- included to show the new
#                    lock did not lose the old one.
#   unparsed_row     the row is still in the file and the fence still says emitted=80, but the row text
#                    is gone. This is "produced but not parsed": the producer's own accounting says 80
#                    and the reader got 79, so the fence arithmetic must fail. It is the direction set
#                    equality alone cannot see.
#   skip_launder     the row is replaced by a SKIP line with a reason that is NOT true of that cell
#                    (`no source trace` on a static cell, which records no trace at all), with the
#                    fence made fully self-consistent (emitted=79 skipped=1 declared=80). A skip is
#                    still "consumed", so set equality passes and the arithmetic passes; the only
#                    thing that can reject this is checking the CONDITION the skip claims.
#   skip_noharm      the same, with the OTHER reason (`no harmonic below Nyquist`) on a cell whose
#                    floor((sr/2)/f0) is >= 1, i.e. a reference that does exist. Both reasons are
#                    covered because each is admitted by a different predicate.
#
# WHAT COUNTS AS RED. Same discipline as run_gh19_s3_negatives.py (#116 ruling): a control is RED only
# when the delta tool EXITS with the expected non-zero code AND prints the NAMED FATAL for that
# control. A Python traceback, an unexpected exit code, or a rejection that names something else is
# INVALID and the run fails -- it proves the tool broke, not that the lock held. Each control therefore
# declares the substring its rejection must contain, and the summary compares the observed status with
# the declared one instead of counting "non-zero" as success.
#
# AND THE POSITIVE HALF IS NOT ASSUMED. `pristine` must exit 0 AND print the coverage sentence with
# `0 skips` against the pinned manifest, and `plan_pin_intact` must find the plan unchanged: a tool
# that rejected everything would pass every negative control above and be worthless. Green-then-red on
# one lock, in one run.

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
from _gh19_textio import open_text

COVERAGE_TOOL = "tools/gh19_s3_pulse_delta.py"
PROBE_TARGET = "gh19_s3_pulse_probe"
BUILD_DIR = "build-118-rel"
PLAN_NAME = "gh19_s3_plan.tsv"

# The cell every arm-mutating control operates on. It is a STATIC cell so that the two skip controls can
# use a reason that is false for it by construction (`static` records no trace), and it is not one of
# the weak `pulsehi` cells the report already excludes from the release surface.
TARGET = "vco_a_pulse_44100_220_pw10"
TARGET_BLOCK = "static"
# A grammar-legal stimulus the manifest does NOT declare: the same cell with the base frequency moved by
# one integer step. Legality is the point -- an illegal id would be rejected by the grammar check and
# would not test the manifest at all.
TARGET_RENAMED = "vco_a_pulse_44100_225_pw10"

# The two reasons the analyzer is allowed to declare, quoted exactly as the delta tool matches them.
REASON_NO_TRACE = "no source trace"
REASON_NO_HARM = "no harmonic below Nyquist"

END_FENCE = re.compile(r"^(-- END MATRIX )(\w+)((?:\s+\w+=\S+)*)$")
FIELD = re.compile(r"(\w+)=(\S+)")


class Invalid(Exception):
    """A control that could not be evaluated. Never counted as a hit."""


def read_lines(path):
    with open_text(path) as fh:
        return fh.read().split("\n")


def write_lines(path, lines):
    with open_text(path, "w") as fh:
        fh.write("\n".join(lines))


def fence_index(lines, block):
    for i, ln in enumerate(lines):
        m = END_FENCE.match(ln)
        if m and m.group(2) == block:
            return i
    raise Invalid("no '-- END MATRIX %s' fence" % block)


def fence_fields(lines, block):
    m = END_FENCE.match(lines[fence_index(lines, block)])
    return dict(FIELD.findall(m.group(3)))


def set_fence(lines, block, **kw):
    """Rewrite named fields of a block's END fence, preserving field order and any extra fields."""
    i = fence_index(lines, block)
    m = END_FENCE.match(lines[i])
    fields = dict(FIELD.findall(m.group(3)))
    order = [k for k, _ in FIELD.findall(m.group(3))]
    for k, v in kw.items():
        if k not in fields:
            raise Invalid("fence for %s has no field %s" % (block, k))
        fields[k] = str(v)
    lines[i] = m.group(1) + m.group(2) + "".join(" %s=%s" % (k, fields[k]) for k in order)
    return {k: int(v) for k, v in fields.items()}


def find_row(lines, block, cid):
    """The index of the data row for `cid` INSIDE its block's fence. Scoped to the fence on purpose: a
    bare id search would also hit the align_check lines above the blocks, which are a different table."""
    start = None
    for i, ln in enumerate(lines):
        if ln.strip() == "-- BEGIN MATRIX %s" % block:
            start = i
        elif start is not None and ln.strip().startswith("-- END MATRIX %s" % block):
            for j in range(start, i):
                if lines[j].split()[:1] == [cid]:
                    return j
            raise Invalid("the %s block has no row for %s" % (block, cid))
    raise Invalid("no %s block in this arm" % block)


def bump(lines, block, **delta):
    """Move only the named fence fields by the given amounts. Reading the fence first and writing the
    sum back keeps the mutators from having to know the block's absolute counts."""
    f = fence_fields(lines, block)
    return set_fence(lines, block, **{k: int(f[k]) + v for k, v in delta.items()})


# ---------------------------------------------------------------- the mutators

def mut_drop_both(lines):
    del lines[find_row(lines, TARGET_BLOCK, TARGET)]
    bump(lines, TARGET_BLOCK, declared=-1, emitted=-1)
    return lines


def mut_rename_both(lines):
    """Replace the cell with a DIFFERENT, grammar-legal, undeclared stimulus.

    The row has to be internally consistent or the id-metadata pin would reject it and the control
    would be testing the pin instead of the manifest. Three fields say which stimulus this is: the id
    token, the f0 column, and kmax = floor((sr/2)/f0), which the analyzer prints from the cell it
    rendered. All three move together -- which is exactly what re-running a changed axis would do.

    THE ROW SHAPE IS 13 TOKENS and kmax is the LAST of them, which is why `tok[-1]` is the kmax
    column and not one of the two band-residual columns this revision added. The analyzer's own
    column list is the authority (gh19_s3_pulse_analyze.py, the static block's say() header):
    id, sr, f0, duty, A, fund, dc, res_db, res_1k5k_db, res_effbd_db, floor_db, harm_db, kmax.
    Appending a column anywhere but the end moves this index, so the helper asserts the shape below
    rather than trusting the count to stay right by itself."""
    import math
    j = find_row(lines, TARGET_BLOCK, TARGET)
    tok = lines[j].split()          # [id, sr, f0, duty, A, fund, dc, res_db, 1k5k, effbd, fdb, hdb,
                                    #  kmax] -- 13 fields, kmax LAST
    if len(tok) != 13:
        raise Invalid("the %s row has %d fields, not the 13 this mutation indexes (id, sr, f0, duty, "
                      "A, fund, dc, res_db, res_1k5k_db, res_effbd_db, floor_db, harm_db, kmax)"
                      % (TARGET_BLOCK, len(tok)))
    sr, f0_old, f0_new = float(tok[1]), float(tok[2]), float(TARGET_RENAMED.split("_")[4])
    if f0_old != float(TARGET.split("_")[4]):
        raise Invalid("row for %s carries f0=%g, not the id's own" % (TARGET, f0_old))
    tok[0] = TARGET_RENAMED
    tok[2] = "%d" % f0_new
    tok[-1] = "%d" % int(math.floor((sr / 2.0) / f0_new))
    lines[j] = "  " + "  ".join("%-13s" % t if i == 0 else t for i, t in enumerate(tok))
    return lines


def mut_drop_one_arm(lines):
    return mut_drop_both(lines)


def mut_unparsed_row(lines):
    del lines[find_row(lines, TARGET_BLOCK, TARGET)]   # fence untouched: produced, not read
    return lines


def _to_skip(lines, reason):
    j = find_row(lines, TARGET_BLOCK, TARGET)
    lines[j] = "  %s %s" % (TARGET, reason)            # the analyzer's own skip-line shape
    bump(lines, TARGET_BLOCK, emitted=-1, skipped=+1)
    return lines


def mut_skip_launder(lines):
    return _to_skip(lines, REASON_NO_TRACE)


def mut_skip_noharm(lines):
    return _to_skip(lines, REASON_NO_HARM)


def mut_identity(lines):
    return lines


# name, arms to mutate, mutator, expected rc, NAMED criterion substring, declared status
CONTROLS = [
    ("pristine", (), mut_identity, 0,
     "manifest coverage: every one of the 152 declared cells was consumed by BOTH arms (0 skips)",
     "GREEN"),
    ("drop_both", ("base", "cand"), mut_drop_both, 4,
     "%s is declared in the manifest but consumed by nothing" % TARGET, "RED"),
    ("rename_both", ("base", "cand"), mut_rename_both, 4,
     "%s was consumed but is not declared in the manifest" % TARGET_RENAMED, "RED"),
    ("drop_one_arm", ("cand",), mut_drop_one_arm, 4,
     "cand: %s is declared in the manifest but consumed by nothing" % TARGET, "RED"),
    ("unparsed_row", ("cand",), mut_unparsed_row, 4,
     "static parsed 79 rows != emitted=80", "RED"),
    ("skip_launder", ("cand",), mut_skip_launder, 4,
     "%s was declared skipped as 'no source trace', but that is not true of this cell" % TARGET, "RED"),
    ("skip_noharm", ("cand",), mut_skip_noharm, 4,
     "%s was declared skipped as 'no harmonic below Nyquist', but that is not true of this cell"
     % TARGET, "RED"),
]


def run_delta(worktree, work, base, cand, manifest):
    p = subprocess.run([sys.executable, COVERAGE_TOOL, "--base", base, "--cand", cand,
                        "--manifest", manifest, "--out", os.path.join(work, "deltas.tsv")],
                       cwd=worktree, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def fresh_plan(worktree, work):
    """Build and re-run the probe so the plan it emits NOW can be compared with the pinned artifact.
    A failed build, a missing binary or a non-zero probe exit is INVALID: the pin could not be
    evaluated, which is not the same as the pin being satisfied."""
    build = subprocess.run(["cmake", "--build", BUILD_DIR, "--target", PROBE_TARGET, "-j", "8"],
                           cwd=worktree, capture_output=True, text=True)
    if build.returncode != 0:
        raise Invalid("probe build failed:\n%s" % build.stdout[-1200:])
    binp = os.path.join(worktree, BUILD_DIR, PROBE_TARGET)
    if not os.path.exists(binp):
        raise Invalid("no probe binary at %s" % binp)
    out = os.path.join(work, "plan-check")
    if os.path.isdir(out):
        shutil.rmtree(out)
    p = subprocess.run([binp, "--out", out], cwd=worktree, capture_output=True, text=True)
    if p.returncode != 0:
        raise Invalid("probe exited %d:\n%s" % (p.returncode, p.stderr[-1200:]))
    path = os.path.join(out, PLAN_NAME)
    if not os.path.exists(path):
        raise Invalid("the probe produced no %s" % PLAN_NAME)
    return path


def pin_compare(fresh, manifest):
    a, b = read_lines(fresh), read_lines(manifest)
    if a == b:
        return True, ("the plan the probe emitted just now is byte-identical to %s (%d lines)"
                      % (os.path.basename(manifest), len(b)))
    diff = list(difflib.unified_diff(b, a, "pinned/%s" % os.path.basename(manifest),
                                     "fresh/%s" % PLAN_NAME, lineterm=""))
    return False, ("the probe's plan DIFFERS from the pinned manifest -- a declared axis changed "
                   "without re-pinning:\n%s" % "\n".join(diff[:24]))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--worktree", required=True,
                    help="the candidate worktree; the plan pin rebuilds and re-runs the probe there")
    ap.add_argument("--base-arm", required=True, help="PRISTINE baseline analyzer --out file")
    ap.add_argument("--cand-arm", required=True, help="PRISTINE candidate analyzer --out file")
    ap.add_argument("--manifest", default="report/gh19-s3-pulse-aa/expected_cells.tsv")
    ap.add_argument("--work", default="/tmp/gh19-s3/coverage")
    args = ap.parse_args(argv)

    manifest = args.manifest
    if not os.path.isabs(manifest):
        manifest = os.path.join(args.worktree, manifest)
    if not os.path.exists(manifest):
        print("FATAL: manifest %s does not exist" % manifest)
        return 4
    if os.path.isdir(args.work):
        shutil.rmtree(args.work)
    os.makedirs(args.work)

    results = []
    print("=== COVERAGE LOCK 1/2: the expectation is a pinned artifact ===")
    try:
        fresh = fresh_plan(args.worktree, args.work)
        ok, msg = pin_compare(fresh, manifest)
        print("  plan_pin_intact    rc=%d (want 0)  %s" % (0 if ok else 1, "GREEN" if ok else "RED"))
        print("    %s" % msg.replace("\n", "\n    "))
        results.append(("plan_pin_intact", "GREEN" if ok else "RED"))
        # The pin's own negative control: a plan that differs by ONE declared cell must be rejected. A
        # comparison that accepted anything would make the pin decorative.
        lines = read_lines(fresh)
        for i, ln in enumerate(lines):
            if ln.split("\t")[-1] == TARGET:
                del lines[i]
                break
        else:
            raise Invalid("the fresh plan does not declare %s" % TARGET)
        drift = os.path.join(args.work, "plan_drifted.tsv")
        write_lines(drift, lines)
        ok2, msg2 = pin_compare(drift, manifest)
        print("  plan_pin_drift     rc=%d (want 1)  %s" % (0 if ok2 else 1, "RED" if not ok2 else "GREEN"))
        print("    %s" % msg2.replace("\n", "\n    "))
        results.append(("plan_pin_drift", "RED" if not ok2 else "GREEN"))
    except Invalid as e:
        print("  INVALID: %s" % e)
        results.append(("plan_pin", "INVALID"))

    print("\n=== COVERAGE LOCK 2/2: both arms reconciled against it (manifest held fixed) ===")
    print("target cell %s in the %s block; no control edits the manifest" % (TARGET, TARGET_BLOCK))
    for name, arms, mut, want_rc, want_msg, want_status in CONTROLS:
        d = os.path.join(args.work, name)
        os.makedirs(d)
        base = os.path.join(d, "base.txt")
        cand = os.path.join(d, "cand.txt")
        shutil.copyfile(args.base_arm, base)
        shutil.copyfile(args.cand_arm, cand)
        try:
            for arm, path in (("base", base), ("cand", cand)):
                if arm in arms:
                    write_lines(path, mut(read_lines(path)))
            rc, text = run_delta(args.worktree, d, base, cand, manifest)
        except Invalid as e:
            print("  %-18s INVALID  %s" % (name, e))
            results.append((name, "INVALID"))
            continue
        # The exit code is only half of it: a traceback also exits non-zero, and an unrelated FATAL is
        # not this control's criterion. Both are INVALID, never hits.
        if "Traceback (most recent call last)" in text:
            status = "INVALID"
        elif rc == want_rc and want_msg in text:
            status = want_status
        else:
            status = "MISSED"
        print("  %-18s rc=%d (want %d)  %s" % (name, rc, want_rc, status))
        for ln in text.strip().split("\n"):
            if ln.startswith("FATAL") or ln.startswith("manifest coverage"):
                print("      %s" % ln)
        results.append((name, status))

    print("\n=== SUMMARY ===")
    declared = {n: s for n, _, _, _, _, s in CONTROLS}
    declared["plan_pin_intact"] = "GREEN"
    declared["plan_pin_drift"] = "RED"
    bad = 0
    for name, status in results:
        want = declared.get(name, "GREEN")
        ok = (status == want)
        bad += 0 if ok else 1
        print("  %-18s %-8s %s" % (name, status, "as declared" if ok else "*** DECLARED %s ***" % want))
    n_invalid = sum(1 for _, s in results if s == "INVALID")
    print("  %d of %d controls behaved as declared (%d invalid)"
          % (len(results) - bad, len(results), n_invalid))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
