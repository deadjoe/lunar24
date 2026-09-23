#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #119 / GH#19 S3: does the CHECKED-IN acceptance pipeline actually reject a bad kernel?
#
# WHAT THIS IS FOR. `run_gh19_s3_pulse_pipeline.py` re-renders the candidate arm from the current
# head and puts it through the 72-cell gate. A gate that only ever says PASS is indistinguishable
# from a gate that cannot fail, and "we ran it and it was green" is exactly the kind of evidence
# that stops being true the moment someone edits the criterion. So this runner shows the pipeline
# failing, on a real production mutation, through the same driver CMake invokes.
#
# HOW IT ISOLATES THE MUTATION. Two probes are compiled HERE, from the same source file, by the
# same compiler, with the same flags, into the same scratch directory. The ONLY difference between
# them is one shadow include root holding one mutated header:
#
#   reference : -I <shadow-pristine>/... = the tree's own headers, unmodified
#   mutant    : -I <shadow-mutant>/...   = the tree's own headers, with pulse_blep_kernel.h's
#                                          polyblepPulseCorrection() returning 0.0 immediately
#
# The mutation is `bypass-correction`, the same one the isolated acceptance-surface matrix
# (`run_gh19_s3_pulse_mutants.py`) uses: the two-edge correction is bypassed entirely, so the
# product emits the naive pulse. That is the single change this slice is about, and the prediction
# is specific: the correction is what buys the >= 6 dB in both bands, so with it bypassed the
# improvement columns must come in short and the gate must say so BY NAME.
#
# WHAT COUNTS AS EVIDENCE, AND WHAT DOES NOT:
#   * the REFERENCE arm must go through the pipeline and exit 0 (PASS) -- otherwise a red mutant
#     would only prove the pipeline is broken;
#   * the MUTANT arm must exit exactly EXIT_RED (1) -- not a crash, not a build failure, and NOT
#     EXIT_REFUSE (4). A refusal means the artifacts failed an input check and "no judgement is
#     issued": that is a broken measurement, not a rejected mutation, and it must never be counted
#     as the pipeline catching anything;
#   * the mutant run must print the NAMED improvement failure (`ACCEPT-FAIL-IMPROVEMENT`) and the
#     gate must report `verdict=RED`. Asserting only "non-zero exit" would pass on a refusal.
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_s3_pulse_pipeline_mutant.py --compiler <c++> --repo-root . --out <scratch>
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile

PROBE_SRC = "tests/probes/gh19_s3_pulse_probe.cpp"
KERNEL = "core/include/lunar24/core/pulse_blep_kernel.h"

# The mutation, as an exact-anchor (old, new) pair. Written the same way the isolated matrix writes
# its mutants, and the anchor is verified to occur EXACTLY ONCE before anything is compiled: a stale
# anchor would otherwise produce a pristine "mutant" that passes, which reads as "the pipeline
# cannot see this defect" when in fact the defect was never applied.
BYPASS_CORRECTION = (
    "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
    "  if (!(dt > 0.0)) return 0.0;",
    "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
    "  return 0.0;  // MUTANT: the two-edge correction bypassed entirely\n"
    "  if (!(dt > 0.0)) return 0.0;",
)

# The pipeline's own exit codes (mirrored from run_gh19_s3_pulse_pipeline.py / the gate).
PIPE_PASS = 0
PIPE_RED = 1
PIPE_REFUSE = 4

NAMED_FAILURE = "ACCEPT-FAIL-IMPROVEMENT"
RED_VERDICT = "ACCEPT-GATE verdict=RED"


def compile_cmd(repo_root, shadow_inc, out, compiler):
    """Mirrors the probe target's include set. The shadow root MUST come first, so a mutated
    header wins over the tree's while every untouched header still resolves from
    core/include/generated."""
    cmd = [compiler, "-O3", "-DNDEBUG", "-std=c++17"]
    if sys.platform == "darwin":
        cmd += ["-arch", platform.machine() or "arm64"]
    cmd += ["-I", shadow_inc]
    cmd += ["-I", os.path.join(repo_root, "core/include"),
            "-I", os.path.join(repo_root, "generated"),
            "-I", os.path.join(repo_root, "tests/host"),
            "-I", os.path.join(repo_root, "host/include")]
    cmd += [os.path.join(repo_root, PROBE_SRC), "-o", out]
    return cmd


def stage_shadow(repo_root, shadow_root, mutate):
    """Copy the one header into a shadow include root, optionally mutated. Returns the shadow
    path (the mutated header's own relative location under core/include)."""
    src = os.path.join(repo_root, KERNEL)
    if not os.path.exists(src):
        sys.stderr.write("INVALID: %s does not exist in %s\n" % (KERNEL, repo_root))
        return None
    text = open(src).read()
    rel = KERNEL[len("core/include/"):]
    dst = os.path.join(shadow_root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if mutate:
        old, new = BYPASS_CORRECTION
        n = text.count(old)
        if n != 1:
            sys.stderr.write("INVALID: the bypass-correction anchor occurs %d times in %s "
                             "(expected exactly 1); a stale anchor would stage a PRISTINE probe "
                             "and the run would prove nothing\n" % (n, KERNEL))
            return None
        text = text.replace(old, new)
    with open(dst, "w") as f:
        f.write(text)
    return dst


def main():
    ap = argparse.ArgumentParser(
        description="GH#19 S3: the acceptance pipeline must reject a bypassed correction")
    ap.add_argument("--compiler", required=True, help="the C++ compiler CMake is using")
    ap.add_argument("--repo-root", default=".", help="repo root")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    # Everything else is handed to the pipeline unchanged, so this runner cannot quietly run a
    # different pipeline than CMake does: there is one driver and these are its arguments.
    ap.add_argument("--pipeline", required=True, help="tools/run_gh19_s3_pulse_pipeline.py")
    ap.add_argument("--baseline-arm", required=True)
    ap.add_argument("--base-report", required=True)
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--delta", required=True)
    ap.add_argument("--gate", required=True)
    args = ap.parse_args()

    root = args.repo_root
    tmp = tempfile.mkdtemp(prefix="gh19s3mut-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)

        results = {}
        for tag, mutate in (("reference", False), ("bypass-correction", True)):
            shadow = os.path.join(tmp, "inc_" + tag)
            os.makedirs(shadow, exist_ok=True)
            staged = stage_shadow(root, shadow, mutate)
            if staged is None:
                print("INVALID: could not stage the %s arm" % tag)
                return 2
            probe = os.path.join(tmp, "probe_" + tag)
            b = subprocess.run(compile_cmd(root, shadow, probe, args.compiler),
                               capture_output=True, text=True)
            if b.returncode != 0:
                # A compile error is NOT a red: it says nothing about whether the pipeline can
                # judge a working product.
                sys.stderr.write("INVALID: the %s probe did not compile:\n%s\n" %
                                 (tag, b.stderr[-2000:]))
                return 2
            arm_dir = os.path.join(tmp, "pipe_" + tag)
            p = subprocess.run(
                [sys.executable, args.pipeline,
                 "--probe", probe,
                 "--baseline-arm", args.baseline_arm,
                 "--base-report", args.base_report,
                 "--criteria", args.criteria,
                 "--manifest", args.manifest,
                 "--analyzer", args.analyzer,
                 "--delta", args.delta,
                 "--gate", args.gate,
                 "--out", arm_dir],
                capture_output=True, text=True)
            out = p.stdout
            results[tag] = {
                "rc": p.returncode,
                "named": NAMED_FAILURE in out,
                "verdict_red": RED_VERDICT in out,
                "refused": ("ACCEPT-GATE verdict=REFUSE" in out
                            or "REFUSE BASELINE-PIN" in out),
                "out": out,
                "err": p.stderr,
            }
            print("[%-18s] pipeline rc=%d  named_improvement_failure=%s  verdict_red=%s"
                  % (tag, p.returncode, results[tag]["named"], results[tag]["verdict_red"]))

        sys.stdout.write("\n--- reference pipeline output ---\n")
        sys.stdout.write(results["reference"]["out"])
        sys.stdout.write("\n--- bypass-correction pipeline output ---\n")
        sys.stdout.write(results["bypass-correction"]["out"])

        ref, mut = results["reference"], results["bypass-correction"]
        problems = []
        if ref["rc"] != PIPE_PASS:
            problems.append("the reference arm did not PASS (rc=%d): a red mutant would then only "
                            "prove the pipeline is broken" % ref["rc"])
        if mut["rc"] == PIPE_REFUSE or mut["refused"]:
            problems.append("the mutant arm REFUSED (rc=%d): the artifacts failed an input check, "
                            "so NO judgement was issued. A refusal is not the pipeline catching a "
                            "defect and must never be counted as one" % mut["rc"])
        if mut["rc"] != PIPE_RED:
            problems.append("the mutant arm exited rc=%d, not the gate's RED (%d)"
                            % (mut["rc"], PIPE_RED))
        if not mut["named"]:
            problems.append("the mutant arm did not name %s: a non-zero exit could be any other "
                            "failure and would not show the improvement columns are the thing "
                            "that rejected it" % NAMED_FAILURE)
        if not mut["verdict_red"]:
            problems.append("the mutant arm's gate did not report `%s`" % RED_VERDICT)
        if problems:
            print("\nPIPELINE-MUTANT FAILED:")
            for p in problems:
                print("  - %s" % p)
            return 1
        print("\nPIPELINE-MUTANT PASS: the reference went through the checked-in pipeline and was "
              "accepted; bypassing the two-edge correction in the product source was rejected by "
              "the same pipeline with the named failure %s, not by a refusal and not by a crash."
              % NAMED_FAILURE)
        return 0
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
