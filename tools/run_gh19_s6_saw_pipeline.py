#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #120 / GH#19 S6: one-command REAL-PRODUCT pipeline behind the CTest gate
# `gh19_s6_saw_acceptance`. Runs the compiled product probe over the CURRENT head into a scratch
# dir, analyses it, and puts it through the acceptance gate against the FIXED HISTORICAL BASELINE.
# Exit 0 = ACCEPTED; 1 = RED (a criterion failed); 4 = REFUSED (an input check failed, no judgement).
#
# WHY THIS DRIVER EXISTS. `gh19_s6_saw_acceptance.py` issues a verdict, but nothing in CMake or CI
# invoked it, and the candidate side of that verdict lived in a scratch directory on one machine.
# The gate is only worth its runtime if a run can be contradicted by the next one: the candidate
# arm is therefore re-rendered HERE, from the binary CMake just built out of this tree, every run.
#
# WHY THE BASELINE SIDE *IS* A COMMITTED ARTIFACT AND THE CANDIDATE SIDE IS NOT. The criterion is
# "the correction improves the pre-correction product by >= 6 dB per cell". The baseline is the
# thing being improved on: it is history, it does not move, and re-rendering it every run would
# re-measure a frozen product. The candidate arm is deliberately NOT an input of this driver:
# `--cand-report` is not an argument at all, so "the gate is judging a checked-in file" is not a
# reachable state.
#
# THREE PINS, THREE NAMED REFUSALS, ALL BEFORE ANY MEASUREMENT.
# S3 pins two artifacts (its stimulus matrix and its report). S6 pins THREE, because S6 has a
# criterion that reads SAMPLES and not just a report column:
#
#   BASELINE-PIN        report/gh19-s6-saw-aa/baseline_arm/gh19_s6_scenarios.tsv
#                       the manifest: id -> raw file, warm, win, f0_meas_hz. Fixes the stimulus,
#                       the window geometry, and the f0 every GUARD-FREQ bound is computed from.
#   BASELINE-REPORT-PIN report/gh19-s6-saw-aa/base_report.txt
#                       the numbers. EVERY improvement column and every ACCEPT-REPORT figure is
#                       read out of this file's residual columns, never re-derived from the arm. A
#                       consistent edit to `res_db` / `res_1k5k_db` / `res_effbd_db` moves the
#                       published improvement of all 120 cells while leaving the manifest pin and
#                       the instrument self-check intact -- that state is what this refusal removes.
#   BASELINE-RAW-PIN    the eight `m50000` raw windows under baseline_arm/
#                       `sine_node_bit_diff_count` is the one criterion that compares SAMPLES: it
#                       reads the baseline's window frame by frame. Neither pin above covers a byte
#                       of it, so a window is pinned by digest too. Without this, the only judged
#                       criterion fed by raw data would be the only judged criterion whose input is
#                       not pinned.
#
# The instruments self-check on every run: the analyzer's per-arm report carries its own
# alignment gate and Parseval tripwires (which the gate re-checks and refuses on), and
# `--self-check` is run as a separate step so the reference-family self-check -- the one that
# caught the duplicated-node-order defect in this very instrument -- is exercised in CI too.
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_s6_saw_pipeline.py --probe build/gh19_s6_saw_probe \
#       --baseline-arm report/gh19-s6-saw-aa/baseline_arm \
#       --base-report report/gh19-s6-saw-aa/base_report.txt \
#       --criteria report/gh19-s6-saw-aa/acceptance_criteria.tsv \
#       --analyzer tools/gh19_s6_saw_analyze.py \
#       --gate tools/gh19_s6_saw_acceptance.py \
#       --out <scratch-dir>
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

# The pinned baseline manifest. Not a convenience default: if this file changes, the STIMULUS
# changed, and the run must say so rather than quietly judging against a different baseline.
BASELINE_SCENARIOS_SHA256 = \
    "09b5a2c03d8f43096220391bbf86c20a670af89ab52a65db5237679e065ba55b"

# The pinned baseline REPORT -- the numbers every improvement column is read out of. Pinned
# separately, and refused separately, because it protects a different failure: see the header.
BASELINE_REPORT_SHA256 = \
    "de30d2d3079473e80425f974a74b6da37066aa77f4afc8f3c71a540c44ee3696"

# The pinned baseline RAW WINDOWS -- the samples `sine_node_bit_diff_count` compares. The eight
# `m50000` cells are the sine node at four sample rates on two VCOs. NOTE: the two sides are
# byte-identical at each sample rate (this was measured, not assumed), so these eight entries hold
# four distinct digests; all eight are listed because the map is file -> digest and the criterion
# iterates all eight cells.
BASELINE_RAW_SHA256 = {
    "gh19_s6_scn125.raw": "be8224478487ef1dabeb074ad9e747474e7965ad0f7835dbdfe1d153317e9e99",
    "gh19_s6_scn126.raw": "32cd07fca93f9482088a5d7e2fd6a8e41bb58b9eb84a3ab71bac8e16047f2c37",
    "gh19_s6_scn127.raw": "ca032312b2bef91fae78d8c4b3da12837130749c5587e15f6a58b9061fd0acde",
    "gh19_s6_scn128.raw": "1f998e1a37ac2e1432999857e7cd47ddded26094fdf711d9873d5159c4f09e7d",
    "gh19_s6_scn133.raw": "be8224478487ef1dabeb074ad9e747474e7965ad0f7835dbdfe1d153317e9e99",
    "gh19_s6_scn134.raw": "32cd07fca93f9482088a5d7e2fd6a8e41bb58b9eb84a3ab71bac8e16047f2c37",
    "gh19_s6_scn135.raw": "ca032312b2bef91fae78d8c4b3da12837130749c5587e15f6a58b9061fd0acde",
    "gh19_s6_scn136.raw": "1f998e1a37ac2e1432999857e7cd47ddded26094fdf711d9873d5159c4f09e7d",
}

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, check=True):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if check and p.returncode != 0:
        # Echo BOTH streams. The analyzer prints its matrix and its self-check on stdout and its
        # diagnostics on stderr, and the gate prints its refusal/failure rows on stdout, so a
        # stderr-only message hides the very rows that explain a RED or a refusal.
        sys.stderr.write("command failed (rc=%d): %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                         % (p.returncode, " ".join(cmd), p.stdout, p.stderr))
        sys.exit(p.returncode or 1)
    return p


def main():
    ap = argparse.ArgumentParser(description="GH#19 S6 real-product acceptance pipeline (CTest gate)")
    # The candidate side: built by CMake from THIS tree. There is no --cand-report.
    ap.add_argument("--probe", required=True, help="path to the gh19_s6_saw_probe binary")
    # The baseline side: the pinned pre-correction product, as committed artifacts.
    ap.add_argument("--baseline-arm", required=True,
                    help="dir holding the pinned baseline manifests and raw windows")
    ap.add_argument("--base-report", required=True,
                    help="the pinned baseline arm through the analyzer")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--analyzer", required=True, help="tools/gh19_s6_saw_analyze.py")
    ap.add_argument("--gate", required=True, help="tools/gh19_s6_saw_acceptance.py")
    ap.add_argument("--label", default="cand", help="analyzer label for the fresh arm")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    # ---- the pins are checked before anything is measured ----------------------------------
    pinned = os.path.join(args.baseline_arm, "gh19_s6_scenarios.tsv")
    if not os.path.exists(pinned):
        sys.stderr.write("REFUSE BASELINE-PIN: %s does not exist\n" % pinned)
        return EXIT_REFUSE
    got = sha256_of(pinned)
    if got != BASELINE_SCENARIOS_SHA256:
        sys.stderr.write(
            "REFUSE BASELINE-PIN: the committed baseline manifest is not the pinned one.\n"
            "  path   %s\n  pinned %s\n  actual %s\n"
            "Editing this file moves the stimulus, the window geometry and the f0 every GUARD-FREQ "
            "bound is computed from, so the run refuses instead of judging against an unstated "
            "baseline.\n" % (pinned, BASELINE_SCENARIOS_SHA256, got))
        return EXIT_REFUSE
    sys.stdout.write("ACCEPT-PIPELINE baseline_pin=OK sha256=%s path=%s\n" % (got, pinned))

    if not os.path.exists(args.base_report):
        sys.stderr.write("REFUSE BASELINE-REPORT-PIN: %s does not exist\n" % args.base_report)
        return EXIT_REFUSE
    got_report = sha256_of(args.base_report)
    if got_report != BASELINE_REPORT_SHA256:
        sys.stderr.write(
            "REFUSE BASELINE-REPORT-PIN: the committed baseline report is not the pinned one.\n"
            "  path   %s\n  pinned %s\n  actual %s\n"
            "The improvement of every gate cell is computed from this report's residual columns, "
            "so editing it moves the criterion the 120 improvement cells are judged against while "
            "the manifest pin still matches. The run refuses instead.\n"
            % (args.base_report, BASELINE_REPORT_SHA256, got_report))
        return EXIT_REFUSE
    sys.stdout.write("ACCEPT-PIPELINE base_report_pin=OK sha256=%s path=%s\n"
                     % (got_report, args.base_report))

    # The raw windows the sine-node criterion compares. Refused by name per file, so a run says
    # WHICH window moved rather than only that something did.
    for name in sorted(BASELINE_RAW_SHA256):
        p = os.path.join(args.baseline_arm, name)
        if not os.path.exists(p):
            sys.stderr.write("REFUSE BASELINE-RAW-PIN: %s does not exist\n" % p)
            return EXIT_REFUSE
        got_raw = sha256_of(p)
        if got_raw != BASELINE_RAW_SHA256[name]:
            sys.stderr.write(
                "REFUSE BASELINE-RAW-PIN: %s is not the pinned window.\n"
                "  pinned %s\n  actual %s\n"
                "`sine_node_bit_diff_count` reads this window frame by frame, and neither the "
                "manifest pin nor the report pin covers a byte of it.\n"
                % (p, BASELINE_RAW_SHA256[name], got_raw))
            return EXIT_REFUSE
    sys.stdout.write("ACCEPT-PIPELINE base_raw_pin=OK n=%d path=%s\n"
                     % (len(BASELINE_RAW_SHA256), args.baseline_arm))

    tmp = tempfile.mkdtemp(prefix="gh19s6pipe-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)
        cand_arm = os.path.join(tmp, "cand")
        os.makedirs(cand_arm, exist_ok=True)

        # ---- 1. the REAL current head, rendered by the binary this tree just built ----------
        run([args.probe, "--out", cand_arm])
        cand_plan = os.path.join(cand_arm, "gh19_s6_plan.tsv")
        if not os.path.exists(cand_plan):
            sys.stderr.write("REFUSE PROBE-PLAN: the fresh arm declares no plan at %s\n" % cand_plan)
            return EXIT_REFUSE
        sys.stdout.write("ACCEPT-PIPELINE fresh_arm=%s plan_sha256=%s\n"
                         % (cand_arm, sha256_of(cand_plan)))

        # ---- 2. the instrument's own self-check, on the reference family ---------------------
        sc = run([sys.executable, args.analyzer, "--self-check"])
        sys.stdout.write("ACCEPT-PIPELINE self_check=OK\n")
        if "***" in sc.stdout:
            sys.stderr.write("REFUSE SELF-CHECK: the instrument printed a failure marker\n")
            return EXIT_REFUSE

        # ---- 3. analyse the fresh arm -------------------------------------------------------
        cand_report = os.path.join(tmp, "cand_report.txt")
        an = run([sys.executable, args.analyzer, "--arm", cand_arm, "--label", args.label])
        with open(cand_report, "w", encoding="utf-8") as f:
            f.write(an.stdout)
        sys.stdout.write("ACCEPT-PIPELINE fresh_report=%s sha256=%s\n"
                         % (cand_report, sha256_of(cand_report)))

        # ---- 4. the 168-cell gate, fresh arm against the pinned baseline --------------------
        g = subprocess.run([sys.executable, args.gate,
                            "--criteria", args.criteria,
                            "--plan", cand_plan,
                            "--base-report", args.base_report,
                            "--cand-report", cand_report,
                            "--base-arm", args.baseline_arm,
                            "--cand-arm", cand_arm], capture_output=True, text=True)
        # Echo the gate's verdict table even on success. Otherwise a passing CTest entry records
        # only "rc=0" and the per-cell numbers -- the actual evidence -- exist nowhere in the CI
        # log. Costs ~20 lines.
        sys.stdout.write(g.stdout)
        if g.stderr:
            sys.stderr.write(g.stderr)
        if g.returncode != EXIT_PASS:
            return g.returncode
        return EXIT_PASS
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
