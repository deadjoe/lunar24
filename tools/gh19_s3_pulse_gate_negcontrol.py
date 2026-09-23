#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# GH#19 S3 (task #119): the acceptance gate's OWN negative controls.
#
# WHY THIS FILE EXISTS SEPARATELY FROM THE GATE. A gate that returns PASS on the delivered artifacts
# is evidence only if it can be made to refuse. "The gate passed" and "the gate cannot fail" are
# indistinguishable from a PASS alone, so every refusal the task authorization names gets a control
# here: the artifact is mutated in exactly one way, and the control requires that the gate EXIT
# NORMALLY (exit 4, a typed refusal -- not a traceback, not a crash) and name the code the mutation
# is supposed to trip. A mutation that trips a DIFFERENT code counts as a failure: it would mean the
# gate is refusing for a reason other than the one it reports.
#
# THE CONTROLS ARE ON THE GATE, NOT ON THE DSP. Nothing here touches src/, core/ or tests/: the
# starting artifacts are real analyzer output and each control perturbs a copy of a text file. The
# source-level negative controls (bypass the correction, wrong sign, wrong position, restore the duty
# hard switch, remove the kernel width cap, delayed PWM, wrong block-boundary reset) live with the
# product acceptance test, because they have to be compiled to be observed at all.
#
# The unmutated inputs are run first as a positive control: it must PASS, and every mutated run must
# differ from it. Without that, "the mutation was caught" could just mean "this gate always refuses".
#
# Usage:
#   tools/gh19_s3_pulse_gate_negcontrol.py \
#       --criteria report/gh19-s3-pulse-product/acceptance_criteria.tsv \
#       --plan /tmp/gh19-s3-119/base/gh19_s3_plan.tsv \
#       --deltas /tmp/gh19-s3-119/an/deltas.tsv \
#       --base-report /tmp/gh19-s3-119/an/base_report.txt \
#       --cand-report /tmp/gh19-s3-119/an/cand_report.txt \
#       --base-arm /tmp/gh19-s3-119/base --cand-arm /tmp/gh19-s3-119/cand

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "gh19_s3_pulse_acceptance.py")

EXIT_PASS = 0
EXIT_REFUSE = 4

# A gate cell used by the mutations that need a specific row. Named rather than positional so a change
# in the plan does not silently retarget the control.
VICTIM = "vco_a_pulse_44100_220_pw10"
VICTIM2 = "vco_b_pulse_96000_880_pw90"
# A plan-static cell that is NOT a gate cell, for the plan mutation (dropping a gate cell from the
# plan must be caught; dropping an ungated one must not be relied on).
PLAN_EXTRA = "vco_a_pulsehi_44100_3520_pw10"

FILES = ("criteria.tsv", "plan.tsv", "deltas.tsv", "base_report.txt", "cand_report.txt")


def read(p):
    with open(p, "r") as fh:
        return fh.read()


def write(p, s):
    with open(p, "w") as fh:
        fh.write(s)


def lines_of(s):
    return s.splitlines(True)


def find_row(text, cid, sep="\t"):
    """Index of the line whose first field is `cid`, or None. Tab- or space-separated."""
    for i, ln in enumerate(lines_of(text)):
        f = ln.rstrip("\n").split(sep)
        if sep == "\t":
            if f and f[0] == cid:
                return i
        else:
            f = ln.split()
            if f and f[0] == cid:
                return i
    return None


def find_matrix_row(text, cid, ncols=13):
    """Index of the cell's DATA row inside the static matrix fence, or None.

    NOT find_row. The analyzer prints the cell id twice in the same file: once as the matrix row and
    once at the head of an alignment-proof line (`<id>  scale=0.500000 wrap=4 ...`) that also begins
    with the id. A mutation that lands on the alignment line changes nothing the gate reads, so the
    control would report a MISS that is really a control bug -- which is how this function came to
    exist. Matched by id AND by the pinned column count, inside the fence.
    """
    ls = lines_of(text)
    begin = end = None
    for i, ln in enumerate(ls):
        if ln.strip() == "-- BEGIN MATRIX static":
            begin = i
        elif ln.startswith("-- END MATRIX static"):
            end = i
    if begin is None or end is None or end < begin:
        return None
    for i in range(begin + 1, end):
        f = ls[i].split()
        if len(f) == ncols and f[0] == cid:
            return i
    return None


def matrix_row_or_die(t, key, cid):
    i = find_matrix_row(t[key], cid)
    if i is None:
        raise SystemExit("control setup: no %d-column matrix row for %s in %s" % (13, cid, key))
    return i


# --------------------------------------------------------------------------------------------
# Mutations. Each takes the dict of file texts and returns a NEW dict. Exactly one thing changes.
# --------------------------------------------------------------------------------------------
def m_empty_deltas(t):
    t["deltas.tsv"] = ""
    return t


def m_empty_cand_matrix(t):
    t["cand_report.txt"] = ""
    return t


def m_deltas_header_only(t):
    ls = lines_of(t["deltas.tsv"])
    t["deltas.tsv"] = ls[0]
    return t


def m_gate_cell_missing_both_arms(t):
    """A gate cell absent from the matrices AND the delta table. The plan still declares it, so the
    gate must refuse rather than judge a smaller set."""
    for key, cid in (("base_report.txt", VICTIM), ("cand_report.txt", VICTIM)):
        ls = lines_of(t[key])
        del ls[matrix_row_or_die(t, key, cid)]
        t[key] = "".join(ls)
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    if i is None:
        raise SystemExit("control setup: %s not found in deltas" % VICTIM)
    del ls[i]
    t["deltas.tsv"] = "".join(ls)
    return t


def m_gate_cell_missing_one_arm(t):
    """Present in base and in the plan, absent from cand only."""
    ls = lines_of(t["cand_report.txt"])
    del ls[matrix_row_or_die(t, "cand_report.txt", VICTIM)]
    t["cand_report.txt"] = "".join(ls)
    return t


def m_delta_row_duplicated(t):
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    ls.insert(i, ls[i])
    t["deltas.tsv"] = "".join(ls)
    return t


def m_id_swap(t):
    """Swap two cells' ids, keeping their data. The ids then disagree with the metadata they carry."""
    ls = lines_of(t["deltas.tsv"])
    i, j = find_row(t["deltas.tsv"], VICTIM), find_row(t["deltas.tsv"], VICTIM2)
    if i is None or j is None:
        raise SystemExit("control setup: swap victims not found")
    a, b = ls[i].split("\t"), ls[j].split("\t")
    a[0], b[0] = b[0], a[0]
    ls[i], ls[j] = "\t".join(a), "\t".join(b)
    t["deltas.tsv"] = "".join(ls)
    return t


def m_nonfinite_delta(t):
    """A judgement column made non-finite. NaN compares false against every threshold, so a gate that
    reads it without checking would judge the cell CLEAN."""
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    f = ls[i].rstrip("\n").split("\t")
    f[13] = "nan"  # dreseffbd_db -- the column the full-band gate reads
    ls[i] = "\t".join(f) + "\n"
    t["deltas.tsv"] = "".join(ls)
    return t


def m_nonfinite_matrix(t):
    ls = lines_of(t["cand_report.txt"])
    i = matrix_row_or_die(t, "cand_report.txt", VICTIM)
    f = ls[i].split()
    f[4] = "inf"  # A -- the column the pinned-scale report divides by
    ls[i] = " ".join(f) + "\n"
    t["cand_report.txt"] = "".join(ls)
    return t


def m_wrong_kind(t):
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    f = ls[i].rstrip("\n").split("\t")
    f[1] = "mixed"
    ls[i] = "\t".join(f) + "\n"
    t["deltas.tsv"] = "".join(ls)
    return t


def m_wrong_metadata(t):
    """sr in the row disagrees with the sr the id decodes to."""
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    f = ls[i].rstrip("\n").split("\t")
    f[2] = "96000"
    ls[i] = "\t".join(f) + "\n"
    t["deltas.tsv"] = "".join(ls)
    return t


def m_arms_not_from_these_matrices(t):
    """The delta table looks self-consistent but was not built from the two matrices under test: its
    cand column is moved so cand-base no longer equals the matrix delta."""
    ls = lines_of(t["deltas.tsv"])
    i = find_row(t["deltas.tsv"], VICTIM)
    f = ls[i].rstrip("\n").split("\t")
    f[6] = "%.2f" % (float(f[6]) + 3.0)  # res_cand_db
    ls[i] = "\t".join(f) + "\n"
    t["deltas.tsv"] = "".join(ls)
    return t


def m_power_unit_tripwire_removed(t):
    """The analyzer's Parseval self-check stops saying the mean squares agree. This is the power-unit
    tripwire: the residual columns are only powers in the declared sense if it holds."""
    s = t["cand_report.txt"]
    s = re.sub(r"MEAN SQUARES AGREE \(ok\)", "MEAN SQUARES DISAGREE", s)
    if s == t["cand_report.txt"]:
        raise SystemExit("control setup: no Parseval line to mutate")
    t["cand_report.txt"] = s
    return t


def m_align_gate_fails(t):
    s = t["cand_report.txt"]
    s = re.sub(r"(ALIGN-GATE ok=)1", r"\g<1>0", s)
    if s == t["cand_report.txt"]:
        raise SystemExit("control setup: no ALIGN-GATE line to mutate")
    t["cand_report.txt"] = s
    return t


def m_instrument_failure_marker(t):
    ls = lines_of(t["cand_report.txt"])
    ls.insert(1, "*** instrument failure marker injected by the negative control\n")
    t["cand_report.txt"] = "".join(ls)
    return t


def m_criteria_drop_criterion(t):
    ls = [l for l in lines_of(t["criteria.tsv"])
          if not l.startswith("criterion\tmax_fund_amp_rel_change")]
    t["criteria.tsv"] = "".join(ls)
    return t


def m_criteria_invented_threshold(t):
    """A threshold the authorization never set, on a quantity it explicitly left report-only. This is
    the defect class this gate was corrected for, so the gate must refuse rather than apply it."""
    t["criteria.tsv"] = t["criteria.tsv"] + "criterion\tmax_scale_rel_change\t0.01\n"
    return t


def m_criteria_drop_report(t):
    ls = [l for l in lines_of(t["criteria.tsv"])
          if not l.startswith("report\tmax_pinned_scale_cost_db")]
    t["criteria.tsv"] = "".join(ls)
    return t


def m_criteria_duplicate_cell(t):
    ls = lines_of(t["criteria.tsv"])
    for i, l in enumerate(ls):
        if l.startswith("cell\t") and l.rstrip("\n").endswith(VICTIM):
            ls.insert(i, l)
            break
    else:
        raise SystemExit("control setup: cell row %s not found" % VICTIM)
    t["criteria.tsv"] = "".join(ls)
    return t


def m_criteria_shrunk_enumeration(t):
    """One cell row deleted. The axis declaration still implies 72, so the enumeration no longer
    matches -- a silently smaller gate is a refusal, not a smaller gate."""
    ls = [l for l in lines_of(t["criteria.tsv"])
          if not (l.startswith("cell\t") and l.rstrip("\n").endswith(VICTIM))]
    t["criteria.tsv"] = "".join(ls)
    return t


def m_plan_drops_gate_cell(t):
    ls = [l for l in lines_of(t["plan.tsv"])
          if not (l.startswith("static\t") and l.rstrip("\n").endswith(VICTIM))]
    t["plan.tsv"] = "".join(ls)
    return t


def m_plan_drops_report_only_cell(t):
    ls = [l for l in lines_of(t["plan.tsv"])
          if not (l.startswith("static\t") and l.rstrip("\n").endswith(PLAN_EXTRA))]
    t["plan.tsv"] = "".join(ls)
    return t


# (name, mutation, expected exit code, expected code substring, what the control is for)
CONTROLS = (
    ("empty_deltas", m_empty_deltas, EXIT_REFUSE, "DELTAS-SHAPE",
     "an empty delta table is not a zero-row comparison"),
    ("deltas_header_only", m_deltas_header_only, EXIT_REFUSE, "DELTAS-SHAPE",
     "a header with no rows is not 152 clean cells"),
    ("empty_cand_matrix", m_empty_cand_matrix, EXIT_REFUSE, "MATRIX-SHAPE",
     "an empty arm is not an arm with no defects"),
    ("gate_cell_missing_both_arms", m_gate_cell_missing_both_arms, EXIT_REFUSE, "DELTAS-UNKNOWN-ID",
     "a gate cell missing from BOTH arms cannot be washed out by the two arms agreeing"),
    ("gate_cell_missing_one_arm", m_gate_cell_missing_one_arm, EXIT_REFUSE, "MATRIX-SHAPE",
     "a gate cell present in one arm only is not a comparison"),
    ("delta_row_duplicated", m_delta_row_duplicated, EXIT_REFUSE, "DELTAS-SHAPE",
     "a duplicated row must not be counted twice"),
    ("id_swap", m_id_swap, EXIT_REFUSE, "DELTAS-METADATA",
     "swapped ids make every delta belong to the wrong cell"),
    ("nonfinite_delta", m_nonfinite_delta, EXIT_REFUSE, "DELTAS-SHAPE",
     "NaN compares false against every threshold, so an unchecked NaN reads as CLEAN"),
    ("nonfinite_matrix", m_nonfinite_matrix, EXIT_REFUSE, "MATRIX-SHAPE",
     "a non-finite fitted scale must not reach the pinned-scale arithmetic"),
    ("wrong_kind", m_wrong_kind, EXIT_REFUSE, "DELTAS-METADATA",
     "a mixed row under a static id is a different measurement"),
    ("wrong_metadata", m_wrong_metadata, EXIT_REFUSE, "DELTAS-METADATA",
     "a row whose sr disagrees with its own id is not that cell"),
    ("arms_not_from_these_matrices", m_arms_not_from_these_matrices, EXIT_REFUSE, "ORDERING",
     "the deltas must come from the two matrices being judged"),
    ("power_unit_tripwire_removed", m_power_unit_tripwire_removed, EXIT_REFUSE, "ARMSELFCHECK",
     "the residual columns are powers only while the Parseval self-check holds"),
    ("align_gate_fails", m_align_gate_fails, EXIT_REFUSE, "ARMSELFCHECK",
     "an unaligned arm is not a phase-pinned comparison"),
    ("instrument_failure_marker", m_instrument_failure_marker, EXIT_REFUSE, "ARMSELFCHECK",
     "the instrument's own failure marker is fatal to the verdict"),
    ("criteria_drop_criterion", m_criteria_drop_criterion, EXIT_REFUSE, "CRITERIA",
     "a missing criterion must not become a criterion that passed"),
    ("criteria_invented_threshold", m_criteria_invented_threshold, EXIT_REFUSE, "CRITERIA",
     "a threshold the authorization never set must be refused, not applied"),
    ("criteria_drop_report", m_criteria_drop_report, EXIT_REFUSE, "CRITERIA",
     "reported-separately must not become not-reported"),
    ("criteria_duplicate_cell", m_criteria_duplicate_cell, EXIT_REFUSE, "CRITERIA",
     "a duplicated cell must not double-count"),
    ("criteria_shrunk_enumeration", m_criteria_shrunk_enumeration, EXIT_REFUSE, "CRITERIA-ENUMERATION",
     "the gate set is re-derived from the axes, so it cannot be shrunk by deleting a row"),
    ("plan_drops_gate_cell", m_plan_drops_gate_cell, EXIT_REFUSE, "PLAN",
     "a gate cell the probe never produced was never measured"),
    ("plan_drops_report_only_cell", m_plan_drops_report_only_cell, EXIT_REFUSE, "PLAN",
     "fully-reported-but-not-gated must not become not-reported"),
)


def run_gate(args, d):
    cmd = [sys.executable, GATE,
           "--criteria", os.path.join(d, "criteria.tsv"),
           "--plan", os.path.join(d, "plan.tsv"),
           "--deltas", os.path.join(d, "deltas.tsv"),
           "--base-report", os.path.join(d, "base_report.txt"),
           "--cand-report", os.path.join(d, "cand_report.txt"),
           "--base-arm", args.base_arm, "--cand-arm", args.cand_arm]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return p.returncode, p.stdout


def main(argv):
    ap = argparse.ArgumentParser(description="negative controls for the GH#19 S3 acceptance gate")
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--deltas", required=True)
    ap.add_argument("--base-report", required=True)
    ap.add_argument("--cand-report", required=True)
    ap.add_argument("--base-arm", required=True)
    ap.add_argument("--cand-arm", required=True)
    ap.add_argument("--only", default=None, help="run one control by name")
    args = ap.parse_args(argv)

    pristine = {"criteria.tsv": read(args.criteria),
                "plan.tsv": read(args.plan),
                "deltas.tsv": read(args.deltas),
                "base_report.txt": read(args.base_report),
                "cand_report.txt": read(args.cand_report)}

    tmp = tempfile.mkdtemp(prefix="gh19-s3-negctl-")
    try:
        # PINNED CONTROL: the unmutated artifacts. If this does not PASS, no control below can
        # distinguish "the gate caught the mutation" from "this gate refuses everything".
        for name, text in pristine.items():
            write(os.path.join(tmp, name), text)
        code, out = run_gate(args, tmp)
        print("CONTROL pristine            exit=%d expected=%d  %s" %
              (code, EXIT_PASS, "PASS" if code == EXIT_PASS else "MISMATCH"))
        if code != EXIT_PASS:
            print(out)
            print("NEGCTL-GATE verdict=INVALID reason=the unmutated artifacts did not pass")
            return 2

        ok = bad = 0
        for name, mut, want_exit, want_code, why in CONTROLS:
            if args.only and name != args.only:
                continue
            t = dict(pristine)
            try:
                t = mut(t)
            except SystemExit as e:
                print("CONTROL %-32s SETUP FAILED: %s" % (name, e))
                bad += 1
                continue
            if t == pristine:
                print("CONTROL %-32s SETUP FAILED: the mutation changed nothing" % name)
                bad += 1
                continue
            for fname in FILES:
                write(os.path.join(tmp, fname), t[fname])
            code, out = run_gate(args, tmp)
            trip = want_code in out
            # A traceback means the gate crashed rather than refused: the exit code may even match
            # by accident, so it is checked separately from the code.
            crashed = "Traceback (most recent call last)" in out
            good = (code == want_exit) and trip and not crashed
            print("CONTROL %-32s exit=%d expected=%d code=%-22s %s%s" %
                  (name, code, want_exit, want_code, "RED-HIT" if good else "MISS",
                   "  (CRASHED)" if crashed else ""))
            if not good:
                bad += 1
                if not trip:
                    print("         the gate refused, but not under %s. Its own output:" % want_code)
                else:
                    print("         the gate exited %d where %d was required. Its own output:" % (code, want_exit))
                for ln in out.splitlines():
                    if ln.startswith("ACCEPT-GATE verdict") or "ACCEPT-COUNT" in ln or "ACCEPT-REFUSE" in ln:
                        print("         | " + ln)
            else:
                ok += 1

        print("NEGCTL-GATE ok=%d bad=%d controls=%d" % (ok, bad, ok + bad))
        print("NEGCTL-GATE verdict=%s" % ("ALL-HIT" if bad == 0 else "INCOMPLETE"))
        return 0 if bad == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
