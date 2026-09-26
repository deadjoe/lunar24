#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# GH#19 S6 PR-B (task #121): the one-command driver behind the CTest entry `gh19_prb_dynpwm_acceptance`.
#
# WHAT IT DOES, IN ORDER. Renders the FRESH arm from the binary CMake just built out of this tree,
# renders the 12 CONSTRUCTED cells the degenerate-identity criterion needs, renders the NEUTRALISED arm
# from a second binary (same source, same compiler, same flags, one shadow include root), analyses both
# arms with the pinned R1 recipe, and hands all of it to the gate. Exit 0 = the gate issued a PASS
# verdict; the gate's own exit code otherwise (1 = RED, 4 = REFUSED with no judgement issued).
#
# WHY BOTH ARMS ARE RE-RENDERED HERE AND NEITHER IS AN INPUT. The improvement criterion is a comparison
# between two arms. A driver that accepted `--cand-report` would make "the gate is judging a checked-in
# file" a reachable state, and the two arms would then be free to come from different builds while the
# transcript still claimed a comparison between them. So neither report is an argument; both binaries
# are, and both arms are rendered from them on every run.
#
# WHY THE CONSTRUCTED ARM COMES FROM ITS OWN BINARY. Three of the four inputs the gate reads are
# products of compiled-in cell tables (gh19_s3_pulse_probe.cpp:159-165 kDynSpecs; the degen driver's
# own table). A manifest cannot create them, and shadow-include cannot reach a .cpp literal -- so the
# two tables are two binaries. See CMakeLists.txt for why the degen driver is not more cells in the S3
# probe.
#
# THE PINS. There is no committed baseline here to pin: both arms are rendered fresh, so the classic
# "REFUSE BASELINE-PIN" failure mode (a frozen artifact edited to move the criterion) has no surface.
# What takes its place is the HARD PRECONDITION below, which is about the DIFF this slice is allowed to
# carry, and the gate's own input checks.
#
# *** THE HARD PRECONDITION, AND ITS ONE HONEST LIMIT. ***
# The criteria file (section 6) pins: `git diff main -- core/ host/ tests/probes/` must be empty before
# any leg runs. This driver evaluates it when the ref is resolvable and REFUSES by name when the diff
# is non-empty.
# It CANNOT be evaluated in the CI probe-gate job: `.github/workflows/ci.yml:113` checks out with
# `actions/checkout@v4` and no `fetch-depth`, i.e. depth 1, so neither `main` nor `origin/main` exists
# in that checkout. A refusal there would be permanent -- a gate that can never pass is worse than no
# gate -- so in that case the driver prints `product_diff=UNEVALUATED` with the reason and PROCEEDS.
# That line is deliberately loud and does not say "OK": an unevaluated precondition is not a satisfied
# one, and the reader of a CI log must be able to see which of the two they are looking at. Closing it
# properly means giving the workflow a base ref to diff against, which is a CI change and not this
# slice's to make.
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

# The directories the slice must not touch. Anything under these is product or product evidence.
PRODUCT_DIRS = ("core", "host", "tests/probes")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, check=True):
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if check and p.returncode != 0:
        # Echo BOTH streams. The analyzer prints its per-cell matrix on stdout and its diagnostics on
        # stderr; the degen driver prints its plan reconciliation on stderr. A stderr-only message
        # hides the rows that explain the failure.
        sys.stderr.write("command failed (rc=%d): %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                         % (p.returncode, " ".join(cmd), p.stdout, p.stderr))
        sys.exit(p.returncode or 1)
    return p


def write_text(path, text):
    """Text out with BOTH the codec and the newline pinned.

    The codec because the platform default is locale-dependent (on Windows it is cp1252, which mangles
    any non-ASCII byte rather than failing); the newline because a translated CRLF would make the file's
    bytes depend on the host that produced it, and these reports are compared across hosts.
    """
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def product_diff(repo_root):
    """(state, detail) for the hard precondition. state is 'OK', 'VIOLATION' or 'UNEVALUATED'."""
    for ref in ("main", "origin/main"):
        probe = subprocess.run(["git", "-C", repo_root, "rev-parse", "--verify", "--quiet", ref],
                               capture_output=True, text=True, encoding="utf-8")
        if probe.returncode != 0:
            continue
        d = subprocess.run(["git", "-C", repo_root, "diff", "--name-only", ref, "--"] + list(PRODUCT_DIRS),
                           capture_output=True, text=True, encoding="utf-8")
        if d.returncode != 0:
            return "UNEVALUATED", "git diff failed against %s" % ref
        names = [l for l in d.stdout.splitlines() if l.strip()]
        if names:
            return "VIOLATION", "%d product path(s) differ from %s: %s" % (len(names), ref,
                                                                          ",".join(names[:5]))
        return "OK", "no product path differs from %s" % ref
    return "UNEVALUATED", "neither `main` nor `origin/main` resolves in this checkout " \
                          "(a depth-1 CI checkout has only the merge commit)"


def main():
    ap = argparse.ArgumentParser(
        description="GH#19 S6 PR-B (task #121) real-product acceptance pipeline (CTest gate)")
    # No --cand-report / --neut-report: both arms are rendered here, from these binaries. See header.
    ap.add_argument("--probe", required=True, help="the gh19_s3_pulse_probe binary")
    ap.add_argument("--degen-probe", required=True, help="the gh19_prb_degen_probe binary")
    ap.add_argument("--neutral-probe", required=True,
                    help="the same probe source compiled against the shadow include root in which the "
                         "pulse correction is disabled -- the neutralised arm of the improvement "
                         "criterion (same compiler, same flags, only the include order differs)")
    ap.add_argument("--criteria", required=True, help="pinned expectation TSV")
    ap.add_argument("--analyzer", required=True, help="tools/gh19_s3_pulse_analyze.py")
    ap.add_argument("--gate", required=True, help="tools/gh19_prb_dynpwm_acceptance.py")
    ap.add_argument("--negcontrol", default=None,
                    help="tools/gh19_prb_dynpwm_gate_negcontrol.py; when given, the gate's own named "
                         "refusal controls run on every CI run, so a gate that has stopped refusing is "
                         "caught by the same entry that runs it")
    ap.add_argument("--repo-root", default=".", help="for the product-diff precondition")
    # R1's recipe, pinned: taps proportional to L so the floor attributes to RESOLUTION ALONE.
    ap.add_argument("--taps", default="2001", help="analyzer decimator taps at L=8")
    ap.add_argument("--taps2", default="4001",
                    help="analyzer decimator taps at L=16 (ratio (taps2-1)/(taps-1) must be >= "
                         "taps_ratio_min); the registered S3 recipe used the SAME count at both levels, "
                         "which is why this flag exists")
    ap.add_argument("--dynamic-cells", default="56", help="declared reference subset (all of them)")
    ap.add_argument("--conv-cells", default="56", help="convergence-checked cells")
    ap.add_argument("--degen-peak-hi", default="0.55",
                    help="the degen driver's amplitude envelope ceiling. Widened ONLY for mutation arms, "
                         "whose output can leave the shipped envelope legitimately")
    ap.add_argument("--label", default="prb", help="analyzer label for the fresh arm")
    ap.add_argument("--out", default=None, help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    # ---- the hard precondition, before any leg runs ---------------------------------------------
    state, detail = product_diff(args.repo_root)
    if state == "VIOLATION":
        sys.stderr.write(
            "REFUSE PRODUCT-DIFF: this slice adds tools, a report and criteria; it does not touch "
            "product code or the probes.\n  %s\n"
            "The arms rendered below would then be rendering a DIFFERENT product than the one the "
            "criteria were pinned against, so the run refuses instead of judging it.\n" % detail)
        return EXIT_REFUSE
    print("PRB-PIPELINE PRECONDITION product_diff=%s %s" % (state, detail))

    # The R1 ratio is a criterion (taps_ratio_min), so a recipe that violates it must not be runnable
    # by accident. Checked here as well as in the criteria so a mistyped flag fails before the render.
    try:
        t1, t2 = int(args.taps), int(args.taps2)
    except ValueError:
        sys.stderr.write("REFUSE R1-RECIPE: --taps/--taps2 must be integers (got %r/%r)\n"
                         % (args.taps, args.taps2))
        return EXIT_REFUSE
    if t1 < 2 or t2 < 2 or (t2 - 1) < 2 * (t1 - 1):
        sys.stderr.write("REFUSE R1-RECIPE: (taps2-1)/(taps-1) = %s must be >= 2.000, got %d/%d\n"
                         % ("%.3f" % ((t2 - 1) / (t1 - 1)) if t1 > 1 else "n/a", t2, t1))
        return EXIT_REFUSE

    tmp = tempfile.mkdtemp(prefix="gh19prbpipe-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)
        cand_arm = os.path.join(tmp, "cand")
        neut_arm = os.path.join(tmp, "neut")
        degen_arm = os.path.join(tmp, "degen")
        for d in (cand_arm, neut_arm, degen_arm):
            os.makedirs(d, exist_ok=True)

        # ---- 1. the two arms of the improvement comparison, both from THIS build ---------------
        run([args.probe, "--out", cand_arm])
        cand_plan = os.path.join(cand_arm, "gh19_s3_plan.tsv")
        if not os.path.exists(cand_plan):
            sys.stderr.write("REFUSE PROBE-PLAN: the fresh arm declares no plan at %s\n" % cand_plan)
            return EXIT_REFUSE
        print("PRB-PIPELINE fresh_arm=%s plan_sha256=%s" % (cand_arm, sha256_of(cand_plan)))

        run([args.neutral_probe, "--out", neut_arm])
        neut_plan = os.path.join(neut_arm, "gh19_s3_plan.tsv")
        if not os.path.exists(neut_plan):
            sys.stderr.write("REFUSE PROBE-PLAN: the neutralised arm declares no plan at %s\n"
                             % neut_plan)
            return EXIT_REFUSE
        print("PRB-PIPELINE neutral_arm=%s plan_sha256=%s" % (neut_arm, sha256_of(neut_plan)))

        # ---- 2. the constructed cells the degenerate-identity criterion needs -------------------
        dg = run([args.degen_probe, "--out", degen_arm, "--peak-hi", args.degen_peak_hi],
                 check=False)
        # The driver's plan reconciliation is on stderr and is the line a log-grepper reads, so it is
        # echoed whether or not the exit code was clean.
        sys.stdout.write(dg.stdout)
        if dg.stderr:
            sys.stderr.write(dg.stderr)
        if dg.returncode != 0:
            sys.stderr.write(
                "REFUSE DEGEN-DRIVER: the constructed-cell driver exited %d. Its own exit bits name "
                "which of its controls failed -- a refused driver produced no render to compare, so no "
                "identity can be judged from it.\n" % dg.returncode)
            return EXIT_REFUSE
        degen_manifest = os.path.join(degen_arm, "gh19_prb_degen_scenarios.tsv")
        if not os.path.exists(degen_manifest):
            sys.stderr.write("REFUSE DEGEN-DRIVER: no scenario table at %s\n" % degen_manifest)
            return EXIT_REFUSE
        print("PRB-PIPELINE degen_arm=%s manifest_sha256=%s"
              % (degen_arm, sha256_of(degen_manifest)))

        # ---- 3. analyse both arms with the pinned R1 recipe -------------------------------------
        recipe = ["--align-check", "--self-check", "--taps", args.taps, "--taps2", args.taps2,
                  "--dynamic-cells", args.dynamic_cells, "--conv-cells", args.conv_cells]
        cand_report = os.path.join(tmp, "cand_report.txt")
        an = run([sys.executable, args.analyzer, "--arm", cand_arm, "--label", args.label + "_cand"]
                 + recipe)
        write_text(cand_report, an.stdout)
        print("PRB-PIPELINE fresh_report=%s sha256=%s" % (cand_report, sha256_of(cand_report)))

        neut_report = os.path.join(tmp, "neut_report.txt")
        an = run([sys.executable, args.analyzer, "--arm", neut_arm, "--label", args.label + "_neut"]
                 + recipe)
        write_text(neut_report, an.stdout)
        print("PRB-PIPELINE neutral_report=%s sha256=%s" % (neut_report, sha256_of(neut_report)))

        # ---- 4. the gate, over the fresh arms ---------------------------------------------------
        gate_cmd = [sys.executable, args.gate,
                    "--criteria", args.criteria,
                    "--plan", cand_plan,
                    "--cand-report", cand_report,
                    "--cand-arm", cand_arm,
                    "--neut-report", neut_report,
                    "--neut-arm", neut_arm,
                    # The static side of the identity is the FRESH ARM, not a third artifact: the claim
                    # is about this build's two routes, and reading the static side from anywhere else
                    # would be comparing this build against another one while calling it an identity.
                    "--degen-static-arm", cand_arm,
                    "--degen-dyn-arm", degen_arm]
        g = subprocess.run(gate_cmd, capture_output=True, text=True, encoding="utf-8")
        # Echo the gate's verdict table even on success. Otherwise a passing CTest entry records only
        # "rc=0" and the per-cell numbers -- the actual evidence -- exist nowhere in the CI log.
        sys.stdout.write(g.stdout)
        if g.stderr:
            sys.stderr.write(g.stderr)
        if g.returncode != EXIT_PASS:
            return g.returncode

        # ---- 5. the gate's own refusal self-test, on the artifacts just produced ---------------
        if args.negcontrol:
            nc = run([sys.executable, args.negcontrol,
                      "--criteria", args.criteria,
                      "--plan", cand_plan,
                      "--cand-report", cand_report,
                      "--cand-arm", cand_arm,
                      "--neut-report", neut_report,
                      "--neut-arm", neut_arm,
                      "--degen-static-arm", cand_arm,
                      "--degen-dyn-arm", degen_arm])
            sys.stdout.write(nc.stdout)
            # The control runner fails with its own exit status when a control does not hit; echoing is
            # for the CI log, the verdict is the exit code.
        return EXIT_PASS
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
