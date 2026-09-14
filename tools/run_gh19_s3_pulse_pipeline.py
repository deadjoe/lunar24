#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #119 / GH#19 S3: one-command REAL-PRODUCT pipeline behind the CTest gate
# `gh19_s3_pulse_acceptance`. Runs the compiled product probe over the CURRENT head into a
# scratch dir, analyses it, computes the deltas against the FIXED HISTORICAL BASELINE, and puts
# that through the acceptance gate -- and then, when asked, through the gate's own refusal
# self-test. Exit 0 = accepted with every refusal control hit; non-zero = a required cell fails.
#
# ---------------------------------------------------------------------------------------------
# WHY THIS DRIVER EXISTS (the review point it answers).
#
# The S3 acceptance gate (`tools/gh19_s3_pulse_acceptance.py`) already existed, was already
# verified by 22 refusal controls, and already issued a correct verdict. But NOTHING in CMake or
# CI ever invoked it. `CMakeLists.txt` registered only `gh19_s3_pulse_probe` -- the raw
# measurement, which asserts that the matrix was produced, not that it is good enough -- and the
# candidate side of the verdict lived in the checked-in file
# `report/gh19-s3-pulse-product/cand_report.txt`.
#
# So the thing CI exercised was "the probe still runs", while the thing that decides whether each
# of the 72 gate cells still clears BOTH bands by >= 6 dB was a file in git that no run could
# contradict. A future edit to the kernel -- or to the PWM law, or to the allocator -- could ship
# with every gate green and no cell re-measured. That is the gap this driver closes: the
# candidate arm is re-rendered HERE, from the binary CMake just built out of this tree, every run.
#
# WHY THE BASELINE SIDE *IS* A COMMITTED ARTIFACT AND THE CANDIDATE SIDE IS NOT.
# The criterion is "the correction improves the pre-correction product by >= 6 dB per cell". The
# baseline is the thing being improved on: it is history, it does not move, and re-rendering it
# every run would re-measure a frozen product while making a regression in the CURRENT head
# harder to attribute. It is pinned as a committed artifact:
#
#   report/gh19-s3-pulse-product/baseline_arm/gh19_s3_scenarios.tsv
#     rendered by the `gh19_s3_pulse_probe` of the pinned pre-correction tree d9991872
#     sha256 f3075e7ea021953a487ada97b9c8249ec4466c9ad47d0cb0ea3078f2a79d61bc
#   report/gh19-s3-pulse-product/base_report.txt
#     that arm through the analyzer, sha256 eab2aa0d9b6f4294fcbd543efce113fa6039e94177b5887e6b78bdd2214b22ca
#
# The candidate arm and its report are deliberately NOT committed inputs here: they are produced
# into a scratch directory on every run and discarded. `--cand-report` is not an argument of this
# driver at all, so "the gate is judging a checked-in file" is not a reachable state.
#
# The pin is checked, not trusted: the committed baseline matrix's sha256 is compared against the
# pinned constant below and a mismatch is a REFUSAL (exit 4) before any judgement is issued --
# otherwise editing the baseline would silently move the criterion instead of failing it.
#
# ---------------------------------------------------------------------------------------------
# EXACT INVOCATIONS. These are not guesses: each was recovered by reproducing the committed
# artifacts byte-for-byte from the pinned arms (base_report.txt eab2aa0d..., cand_report.txt
# 27589bd0..., deltas.tsv ea375503...) before this driver was written. In particular
# `--self-check` is what puts the instrument self-check into the report, and `--taps 2001` is
# required for the decimator band check to pass (at the default 65 the analyzer reports rc=3 and
# the effective band collapses to 50% of the output Nyquist, which would silently narrow the band
# the `res_effbd_db` gate column is computed over).
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_s3_pulse_pipeline.py --probe <path-to-gh19_s3_pulse_probe> \
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

# The pinned baseline matrix. Not a convenience default: if this file changes, the CRITERION
# changed, and the run must say so rather than quietly judging against a different baseline.
BASELINE_SCENARIOS_SHA256 = \
    "f3075e7ea021953a487ada97b9c8249ec4466c9ad47d0cb0ea3078f2a79d61bc"

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
        # Echo BOTH streams. The analyzer prints its per-cell matrix on stdout and its
        # diagnostics on stderr, and the gate prints the refusal/failure rows on stdout, so a
        # stderr-only message hides the very rows that explain a RED verdict.
        sys.stderr.write("command failed (rc=%d): %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                         % (p.returncode, " ".join(cmd), p.stdout, p.stderr))
        sys.exit(p.returncode or 1)
    return p


def main():
    ap = argparse.ArgumentParser(
        description="GH#19 S3 real-product acceptance pipeline (CTest gate)")
    # The candidate side: built by CMake from THIS tree. There is no --cand-report.
    ap.add_argument("--probe", required=True, help="path to the gh19_s3_pulse_probe binary")
    # The baseline side: the pinned pre-correction product, as committed artifacts.
    ap.add_argument("--baseline-arm", required=True,
                    help="dir holding the pinned baseline gh19_s3_scenarios.tsv")
    ap.add_argument("--base-report", required=True,
                    help="the pinned baseline arm through the analyzer")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--manifest", required=True,
                    help="the pinned declared stimulus plan (report/gh19-s3-pulse-aa/"
                         "expected_cells.tsv); the delta tool has no default for this on purpose")
    ap.add_argument("--analyzer", required=True, help="tools/gh19_s3_pulse_analyze.py")
    ap.add_argument("--delta", required=True, help="tools/gh19_s3_pulse_delta.py")
    ap.add_argument("--gate", required=True, help="tools/gh19_s3_pulse_acceptance.py")
    ap.add_argument("--negcontrol", default=None,
                    help="tools/gh19_s3_pulse_gate_negcontrol.py; when given, the gate's own "
                         "refusal controls are exercised on every CI run, so a gate that has "
                         "stopped refusing is caught by the same entry that runs it")
    ap.add_argument("--taps", default="2001",
                    help="analyzer decimator taps (default 2001: the value that makes the "
                         "effective-band check pass, so the res_effbd_db gate column covers the "
                         "band it is named after)")
    ap.add_argument("--label", default="cand", help="analyzer label for the fresh arm")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    # ---- the pin is checked before anything is measured ------------------------------------
    pinned = os.path.join(args.baseline_arm, "gh19_s3_scenarios.tsv")
    if not os.path.exists(pinned):
        sys.stderr.write("REFUSE BASELINE-PIN: %s does not exist\n" % pinned)
        return EXIT_REFUSE
    got = sha256_of(pinned)
    if got != BASELINE_SCENARIOS_SHA256:
        sys.stderr.write(
            "REFUSE BASELINE-PIN: the committed baseline matrix is not the pinned one.\n"
            "  path   %s\n  pinned %s\n  actual %s\n"
            "Editing this file moves the criterion the 72 gate cells are judged against, so the "
            "run refuses instead of judging against an unstated baseline.\n"
            % (pinned, BASELINE_SCENARIOS_SHA256, got))
        return EXIT_REFUSE
    sys.stdout.write("ACCEPT-PIPELINE baseline_pin=OK sha256=%s path=%s\n" % (got, pinned))

    tmp = tempfile.mkdtemp(prefix="gh19s3pipe-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)
        cand_arm = os.path.join(tmp, "cand")
        os.makedirs(cand_arm, exist_ok=True)

        # ---- 1. the REAL current head, rendered by the binary this tree just built ----------
        run([args.probe, "--out", cand_arm])
        cand_plan = os.path.join(cand_arm, "gh19_s3_plan.tsv")
        if not os.path.exists(cand_plan):
            sys.stderr.write("REFUSE PROBE-PLAN: the fresh arm declares no plan at %s\n" % cand_plan)
            return EXIT_REFUSE
        sys.stdout.write("ACCEPT-PIPELINE fresh_arm=%s plan_sha256=%s\n"
                         % (cand_arm, sha256_of(cand_plan)))

        # ---- 2. analyse it, with the exact recipe that reproduces the committed reports ----
        cand_report = os.path.join(tmp, "cand_report.txt")
        an = run([sys.executable, args.analyzer, "--arm", cand_arm, "--label", args.label,
                  "--align-check", "--self-check", "--taps", args.taps])
        with open(cand_report, "w", encoding="utf-8") as f:
            f.write(an.stdout)
        sys.stdout.write("ACCEPT-PIPELINE fresh_report=%s sha256=%s\n"
                         % (cand_report, sha256_of(cand_report)))

        # ---- 3. deltas against the pinned baseline, over the pinned declared cell set ------
        deltas = os.path.join(tmp, "deltas.tsv")
        run([sys.executable, args.delta, "--base", args.base_report, "--cand", cand_report,
             "--manifest", args.manifest, "--out", deltas])
        sys.stdout.write("ACCEPT-PIPELINE deltas=%s sha256=%s\n"
                         % (deltas, sha256_of(deltas)))

        # ---- 4. the 72-cell gate, on the fresh arm against the pinned baseline -------------
        gate_cmd = [sys.executable, args.gate,
                    "--criteria", args.criteria,
                    "--plan", cand_plan,
                    "--deltas", deltas,
                    "--base-report", args.base_report,
                    "--cand-report", cand_report,
                    "--base-arm", args.baseline_arm,
                    "--cand-arm", cand_arm]
        g = subprocess.run(gate_cmd, capture_output=True, text=True)
        # Echo the gate's verdict table even on success. Otherwise a passing CTest entry records
        # only "rc=0" and the per-cell numbers -- the actual evidence -- exist nowhere in the CI
        # log. Costs ~20 lines.
        sys.stdout.write(g.stdout)
        if g.stderr:
            sys.stderr.write(g.stderr)
        if g.returncode != EXIT_PASS:
            return g.returncode

        # ---- 5. the gate's own refusal self-test, on the artifacts just produced ------------
        if args.negcontrol:
            nc = run([sys.executable, args.negcontrol,
                      "--criteria", args.criteria,
                      "--plan", cand_plan,
                      "--deltas", deltas,
                      "--base-report", args.base_report,
                      "--cand-report", cand_report,
                      "--base-arm", args.baseline_arm,
                      "--cand-arm", cand_arm])
            sys.stdout.write(nc.stdout)
            # The control runner fails its own exit status when a control does not hit; echoing is
            # for the CI log, the verdict is the exit code.
        return EXIT_PASS
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
