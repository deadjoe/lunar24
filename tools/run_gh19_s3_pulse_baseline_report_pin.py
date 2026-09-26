#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #119 / GH#19 S3: the baseline REPORT pin must refuse an edited baseline, and that refusal
# must be protecting something that actually carries the verdict.
#
# WHAT THIS IS FOR. `run_gh19_s3_pulse_pipeline.py` now pins two committed artifacts: the baseline
# stimulus matrix (`gh19_s3_scenarios.tsv`) and the baseline report (`base_report.txt`). The second
# pin is only worth its line if two things are true, and neither is obvious:
#
#   1. IT FIRES, AND BY ITS OWN NAME. A run handed an edited report must stop BEFORE any
#      measurement, must say `REFUSE BASELINE-REPORT-PIN`, and must issue NO verdict -- a refusal
#      that still printed `ACCEPT-GATE verdict=` would be a judgement against an unstated baseline
#      wearing a refusal's exit code. It must also not be the MATRIX pin firing: the two surfaces
#      are separately named so that "the report moved" and "the stimulus moved" cannot be confused.
#
#   2. THE THING IT REFUSES IS NUMERICALLY LOAD-BEARING. If an edited report were inert -- if the
#      gate re-derived its numbers from somewhere else -- then pinning it would be decorative and
#      this runner would be proving a property the criterion does not depend on. So the tamper is
#      also fed to the delta tool and the gate directly, with the pin bypassed, and it must move
#      the published improvement columns. This mirrors the product-side rule used everywhere else
#      in this slice: a negative control that cannot be shown to change the answer is not a
#      control, it is decoration.
#
# HOW THE TAMPER STAYS INSIDE THE PARSE CONTRACT. The three residual columns are shifted by the
# SAME amount in every row. `gh19_s3_pulse_delta.py` requires |res_effbd_db - res_db| <=
# BAND_SLACK_DB for each row and refuses (`Fatal`) otherwise; an equal shift preserves every such
# difference and every column's arity, so the tampered report is a report the tools accept as
# well-formed. That is the point: this is not a malformed-input test, it is a well-formed report
# carrying different numbers, which is exactly the failure mode a SHA pin exists to catch. The
# column indices are read out of each block's OWN header line rather than hardcoded, so the fixture
# comes from the producer and cannot silently disagree with the format it is editing.
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_s3_pulse_baseline_report_pin.py --probe <path-to-gh19_s3_pulse_probe> \
#       --baseline-arm report/gh19-s3-pulse-product/baseline_arm \
#       --base-report report/gh19-s3-pulse-product/base_report.txt \
#       --criteria report/gh19-s3-pulse-product/acceptance_criteria.tsv \
#       --manifest report/gh19-s3-pulse-aa/expected_cells.tsv \
#       --out <scratch-dir>
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from _gh19_textio import open_text

# The two pinned baseline SHAs, mirrored from run_gh19_s3_pulse_pipeline.py. Duplicated on purpose:
# if this file imported them, an edit to the driver's constant would move both sides at once and
# the self-test could not notice that the pinned artifact had changed.
BASELINE_SCENARIOS_SHA256 = \
    "f3075e7ea021953a487ada97b9c8249ec4466c9ad47d0cb0ea3078f2a79d61bc"
BASELINE_REPORT_SHA256 = \
    "eab2aa0d9b6f4294fcbd543efce113fa6039e94177b5887e6b78bdd2214b22ca"

RESIDUAL_COLUMNS = ("res_db", "res_1k5k_db", "res_effbd_db")

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

REFUSAL_NAME = "REFUSE BASELINE-REPORT-PIN"
# The other pin's prose, quoted from the driver. The two refusal NAMES are not substrings of each
# other by construction, but the check below is on the message body so that "the report pin fired"
# and "the matrix pin fired and happened to be formatted similarly" cannot be confused.
MATRIX_REFUSAL_TEXT = "the committed baseline matrix is not the pinned one"
VERDICT_MARK = "ACCEPT-GATE verdict="


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def shift_residuals(text, db):
    """Shift every matrix block's three residual columns by `db`, uniformly across rows.

    Returns (new_text, n_rows, n_blocks). The report has no fence markers -- each block is a
    contiguous run of fixed-arity rows whose first line is a header, and the runs are separated by
    blank lines and by prose blocks of other arities. So a block is recognised structurally: a line
    whose fields carry all three residual names opens one, the width it declares fixes the rows
    that belong to it, and any line of a different width closes it. Column positions are read out
    of that header rather than hardcoded, so the fixture cannot drift from the format the tools
    parse. A run whose header lacks the names is not a matrix and is left alone; if NO block
    matches, the caller reports that as INVALID rather than staging a pristine 'tampered' report,
    which would make the pin look like it works when nothing had moved.

    The shift is the SAME for all three columns in every row, which is what keeps the result a
    report the tools accept: `gh19_s3_pulse_delta.py` requires |res_effbd_db - res_db| <=
    BAND_SLACK_DB row by row, and an equal shift preserves every such difference. A tamper that
    made the report malformed would be caught by the parser and would prove nothing about the pin.
    """
    out, n_rows, n_blocks = [], 0, 0
    idx, width = None, None
    for line in text.splitlines(keepends=True):
        fields = line.split()
        if fields and all(name in fields for name in RESIDUAL_COLUMNS) and "id" in fields:
            idx = [fields.index(name) for name in RESIDUAL_COLUMNS]
            width = len(fields)
            n_blocks += 1
            out.append(line)
            continue
        if idx is None or len(fields) != width:
            # A width change ends the block; the row opening whatever follows is examined on its
            # own terms, so blocks need not be adjacent to be found.
            if idx is not None and fields and len(fields) != width:
                idx, width = None, None
            out.append(line)
            continue
        for i in idx:
            fields[i] = "%.6f" % (float(fields[i]) + db)
        n_rows += 1
        out.append("  " + "  ".join(fields) + "\n")
    return "".join(out), n_rows, n_blocks


def runner_main():
    ap = argparse.ArgumentParser(
        description="GH#19 S3: the baseline report pin must refuse an edited baseline, and the "
                    "edit it refuses must move the verdict evidence")
    ap.add_argument("--probe", required=True, help="path to the gh19_s3_pulse_probe binary; this "
                    "runner compiles nothing itself, it only feeds the pipeline a tampered "
                    "baseline side")
    ap.add_argument("--pipeline", required=True, help="tools/run_gh19_s3_pulse_pipeline.py")
    ap.add_argument("--baseline-arm", required=True)
    ap.add_argument("--base-report", required=True)
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--delta", required=True)
    ap.add_argument("--gate", required=True)
    ap.add_argument("--shift-worse-db", type=float, default=3.0,
                    help="shift applied to the baseline report's residual columns to make the "
                         "baseline look worse (default 3.0 dB); the tamper that is checked END TO "
                         "END: refused by the pin, and -- with the pin bypassed -- still accepted "
                         "by the gate while the published improvements grow by that amount")
    ap.add_argument("--shift-better-db", type=float, default=-30.0,
                    help="the same uniform shift in the other direction (default -30.0 dB): the "
                         "baseline looks better, so every published improvement shrinks by that "
                         "much and the gate's own 6 dB criterion must reject it (asserted). The "
                         "point is that one artifact carries the verdict both ways while the "
                         "stimulus pin stays green")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    if not os.path.exists(args.base_report):
        sys.stderr.write("INVALID: %s does not exist\n" % args.base_report)
        return 2
    pristine_sha = sha256_of(args.base_report)
    if pristine_sha != BASELINE_REPORT_SHA256:
        sys.stderr.write("INVALID: %s is not the pinned baseline report (pinned %s, actual %s).\n"
                         "The whole point of this runner is to edit the PINNED report; editing a "
                         "report that is already unpinned proves nothing.\n"
                         % (args.base_report, BASELINE_REPORT_SHA256, pristine_sha))
        return 2

    scenarios = os.path.join(args.baseline_arm, "gh19_s3_scenarios.tsv")
    scen_sha_before = sha256_of(scenarios) if os.path.exists(scenarios) else None

    tmp = tempfile.mkdtemp(prefix="gh19s3brpin-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)

        problems = []

        # ---- build the two tampers ----------------------------------------------------------
        text = open(args.base_report, encoding="utf-8").read()
        tampers = {}
        for tag, db in (("worse", args.shift_worse_db), ("better", args.shift_better_db)):
            new_text, n_rows, n_blocks = shift_residuals(text, db)
            if n_rows == 0 or n_blocks == 0:
                sys.stderr.write("INVALID: the tamper (%s) matched no block/row in %s -- the "
                                 "column layout has changed and this fixture no longer edits "
                                 "anything\n" % (tag, args.base_report))
                return 2
            path = os.path.join(tmp, "base_report_%s.txt" % tag)
            with open_text(path, "w") as f:
                f.write(new_text)
            tampers[tag] = {"path": path, "sha": sha256_of(path), "rows": n_rows,
                            "blocks": n_blocks, "db": db}
            print("[tamper %-6s] %d block(s), %d row(s), residual columns shifted %+.1f dB  "
                  "sha256=%s" % (tag, n_blocks, n_rows, db, tampers[tag]["sha"][:16]))
            if tampers[tag]["sha"] == pristine_sha:
                problems.append("the %s tamper produced a file identical to the pinned report; "
                                "nothing was edited" % tag)

        # The stimulus is NOT touched -- asserted, not assumed. This is the "only the report's
        # numbers move" half of the requirement, and it is the half that makes the two pins
        # genuinely separate surfaces.
        scen_sha_after = sha256_of(scenarios) if os.path.exists(scenarios) else None
        if scen_sha_before != scen_sha_after:
            problems.append("the baseline stimulus changed during the run (%s -> %s); this runner "
                            "must only ever edit the report" % (scen_sha_before, scen_sha_after))
        if scen_sha_before != BASELINE_SCENARIOS_SHA256:
            problems.append("the baseline stimulus is not the pinned one (pinned %s, actual %s): "
                            "with the matrix also moved, a refusal could be the matrix pin firing "
                            "and the report pin would go untested"
                            % (BASELINE_SCENARIOS_SHA256, scen_sha_before))

        # ---- 1. the pin must refuse, by name, with no verdict -------------------------------
        p = subprocess.run(
            [sys.executable, args.pipeline,
             "--probe", args.probe,
             "--baseline-arm", args.baseline_arm,
             "--base-report", tampers["worse"]["path"],
             "--criteria", args.criteria,
             "--manifest", args.manifest,
             "--analyzer", args.analyzer,
             "--delta", args.delta,
             "--gate", args.gate,
             "--out", os.path.join(tmp, "refused")],
            capture_output=True, text=True)
        print("[pin        ] pipeline with the tampered baseline report: rc=%d" % p.returncode)
        if p.returncode != EXIT_REFUSE:
            problems.append("the driver exited rc=%d on the tampered report, not the refusal (%d). "
                            "An edited baseline was judged." % (p.returncode, EXIT_REFUSE))
        if REFUSAL_NAME not in p.stderr:
            problems.append("the driver did not name `%s` on stderr; the refusal text was:\n%s"
                            % (REFUSAL_NAME, p.stderr[-1500:]))
        if MATRIX_REFUSAL_TEXT in p.stderr:
            problems.append("the refusal that fired is the baseline MATRIX pin, not the report "
                            "pin: the report pin would then be untested by this run")
        if VERDICT_MARK in p.stdout:
            problems.append("the driver printed `%s` while refusing: a refusal must issue NO "
                            "judgement, or an unstated baseline gets a verdict with a refusal's "
                            "exit code attached" % VERDICT_MARK)
        # Nothing may be measured before the pin: the probe render is step 1 of the driver, so a
        # refusal that got as far as rendering would have burned a full 72-cell run first.
        for mark in ("ACCEPT-PIPELINE fresh_arm=", "ACCEPT-PIPELINE fresh_report=",
                     "ACCEPT-PIPELINE deltas="):
            if mark in p.stdout:
                problems.append("the driver reached `%s` before refusing: the pin is not being "
                                "checked before measurement" % mark)
        if "ACCEPT-PIPELINE baseline_pin=OK" not in p.stdout:
            problems.append("the driver's output does not show the MATRIX pin passing first, so a "
                            "report-pin refusal cannot be distinguished from an early abort")

        # ---- 2. the refused edit must be numerically load-bearing ---------------------------
        # The pin is bypassed here on purpose: the delta tool and the gate are invoked directly on
        # the tampered report, exactly as the driver would have done had it not refused. If the
        # gate does not even change its answer, the pin protects nothing.
        ref = os.path.join(tmp, "pipeline_pristine")
        r = subprocess.run(
            [sys.executable, args.pipeline,
             "--probe", args.probe,
             "--baseline-arm", args.baseline_arm,
             "--base-report", args.base_report,
             "--criteria", args.criteria,
             "--manifest", args.manifest,
             "--analyzer", args.analyzer,
             "--delta", args.delta,
             "--gate", args.gate,
             "--out", ref],
            capture_output=True, text=True)
        if r.returncode != EXIT_PASS:
            # A pristine baseline that does not pass makes every "the tamper changed the answer"
            # claim unreadable, so this is a hard stop rather than a problem row.
            sys.stderr.write("INVALID: the pristine baseline did not PASS through the pipeline "
                             "(rc=%d). Without a passing reference there is no answer for the "
                             "tamper to change.\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                             % (r.returncode, r.stdout[-4000:], r.stderr[-4000:]))
            return 2
        print("[reference  ] pristine baseline through the pipeline: rc=0 (PASS)")
        cand_report = os.path.join(ref, "cand_report.txt")
        cand_arm = os.path.join(ref, "cand")
        cand_plan = os.path.join(cand_arm, "gh19_s3_plan.tsv")
        ref_deltas = os.path.join(ref, "deltas.tsv")
        ref_fields = read_delta_fields(ref_deltas)

        for tag in ("worse", "better"):
            tdir = os.path.join(tmp, "bypass_" + tag)
            os.makedirs(tdir, exist_ok=True)
            deltas = os.path.join(tdir, "deltas.tsv")
            d = subprocess.run(
                [sys.executable, args.delta, "--base", tampers[tag]["path"],
                 "--cand", cand_report, "--manifest", args.manifest, "--out", deltas],
                capture_output=True, text=True)
            if d.returncode != 0:
                problems.append("the delta tool rejected the %s tamper (rc=%d): a uniform shift of "
                                "all three residual columns is supposed to stay inside the parse "
                                "and BAND_SLACK contract, so this is the fixture driving the "
                                "contract, not the contract catching a bad baseline.\n%s"
                                % (tag, d.returncode, d.stderr[-1500:]))
                continue
            moved = delta_movement(ref_fields, read_delta_fields(deltas))
            print("[bypass %-6s] the SAME report edit, pin bypassed: deltas moved; worst "
                  "|d improvement| among the three judged columns = %.3f dB"
                  % (tag, moved))
            if moved <= 0.0:
                problems.append("the %s tamper left every improvement column bit-identical. The "
                                "gate does not read the report's residual columns, so pinning "
                                "them is decorative -- which is a finding about the PIN, not about "
                                "the tamper" % tag)

            gres = subprocess.run(
                [sys.executable, args.gate,
                 "--criteria", args.criteria,
                 "--plan", cand_plan,
                 "--deltas", deltas,
                 "--base-report", tampers[tag]["path"],
                 "--cand-report", cand_report,
                 "--base-arm", args.baseline_arm,
                 "--cand-arm", cand_arm],
                capture_output=True, text=True)
            verdicts = [v for v in ("PASS", "RED", "REFUSE")
                        if VERDICT_MARK + v in gres.stdout]
            print("[bypass %-6s] gate on the tampered baseline: rc=%d verdict=%s"
                  % (tag, gres.returncode, ",".join(verdicts) or "?"))
            if tag == "worse":
                # The load-bearing claim in its strongest form: a well-formed report carrying
                # different numbers is accepted and publishes different improvements. That state
                # is precisely what the SHA pin removes.
                if gres.returncode != EXIT_PASS or verdicts != ["PASS"]:
                    problems.append("with the pin bypassed, the `worse` tamper did not still pass "
                                    "(rc=%d, verdict=%s). The claim this runner makes is that such "
                                    "an edit is INVISIBLE to the gate, so it must not be caught by "
                                    "anything else -- if another check does catch it, the report "
                                    "pin's value must be restated in those terms."
                                    % (gres.returncode, ",".join(verdicts) or "none"))
            else:
                # The same edit, other direction: the baseline looks better, so the improvements
                # shrink and the gate's OWN improvement criterion -- no tolerance is added here --
                # must reject it. This is the same artifact carrying the verdict the other way, and
                # it is asserted, not merely printed: an edit that could only ever loosen the gate
                # would be a weaker fact than an edit that moves the verdict in both directions.
                if gres.returncode != EXIT_RED or verdicts != ["RED"]:
                    problems.append("with the pin bypassed, the `better` tamper (%+.1f dB on the "
                                    "baseline's residual columns) was not rejected (rc=%d, "
                                    "verdict=%s). Every gate cell improves by %.1f dB less than "
                                    "published, which is far below the 6 dB the criterion asks "
                                    "for; a PASS here would mean the improvement columns are not "
                                    "read from this report at all."
                                    % (args.shift_better_db, gres.returncode,
                                       ",".join(verdicts) or "none", -args.shift_better_db))

        if problems:
            print("\nBASELINE-REPORT-PIN FAILED:")
            for prob in problems:
                print("  - %s" % prob)
            return 1
        print("\nBASELINE-REPORT-PIN PASS: an edited baseline report is refused by name before any "
              "measurement and issues no verdict, while the stimulus pin reports OK first so the "
              "two surfaces are distinguishable; and the edit that was refused is not inert. With "
              "the pin bypassed, the same well-formed report -- the stimulus pin still green -- is "
              "ACCEPTED when the baseline is made to look worse (and publishes %+.1f dB more "
              "improvement than it earned) and is REJECTED by the gate's own 6 dB criterion when "
              "the baseline is made to look better. The report carries the verdict in both "
              "directions, so the SHA pin removes a state the gate demonstrably accepts."
              % args.shift_worse_db)
        return 0
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


def read_delta_fields(path):
    """Map cell id -> its row, keyed by the header the delta tool itself wrote. The header is the
    first non-empty line and is tab-separated with no comment marker, so it is identified by
    carrying the judged column name rather than by position or by a prefix."""
    fields = {}
    with open(path, encoding="utf-8") as f:
        header = None
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if header is None:
                if "dreseffbd_db" in parts and "id" in parts:
                    header = parts
                continue
            if len(parts) != len(header):
                continue
            row = dict(zip(header, parts))
            fields[row["id"]] = row
    if header is None:
        sys.stderr.write("INVALID: no delta header found in %s\n" % path)
    return fields


def delta_movement(before, after):
    """Largest absolute change in any judged delta column, at the cell where it is largest. Cells
    missing from either side are skipped rather than counted as zero, so a tamper that dropped a
    cell cannot masquerade as a no-op."""
    worst = 0.0
    for cid, row in before.items():
        other = after.get(cid)
        if other is None:
            continue
        for col in ("dreseffbd_db", "dres1k5k_db"):
            if col in row and col in other:
                worst = max(worst, abs(float(other[col]) - float(row[col])))
    return worst


if __name__ == "__main__":
    sys.exit(runner_main())
