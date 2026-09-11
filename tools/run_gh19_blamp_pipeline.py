#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #86 / GH#19: one-command REAL-PRODUCT pipeline used by the CTest gates
# `gh19_blamp_acceptance` (VCO triangle) and `gh19_schmitt_blamp_acceptance` (Schmitt
# oscillator, task #109). Runs the compiled product probe into a scratch dir, feeds it
# to the verified analyzer, then runs the acceptance gate (current vs the committed
# naive baseline). Exit 0 = accepted; non-zero = a required cell fails.
#
# The gate and the baseline are ARGUMENTS, so both slices share this one driver rather
# than forking it: the only thing that differs between them is the criterion file and
# which committed baseline it reads. Exit 0 = accepted; non-zero = a required cell fails.
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_blamp_pipeline.py --probe <path-to-gh19_alias_probe> \
#       --manifest tools/gh19_manifest.tsv --baseline tools/gh19_naive_baseline.tsv \
#       --out <scratch-dir>
import argparse
import os
import subprocess
import sys
import tempfile


def run(cmd, check=True):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if check and p.returncode != 0:
        # Echo BOTH streams: the analyzer and the gates report their per-cell table on
        # stdout and only their diagnostics on stderr, so a stderr-only message hides the
        # very rows that explain a RED verdict (which is what the negative controls need).
        sys.stderr.write("command failed (rc=%d): %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                         % (p.returncode, " ".join(cmd), p.stdout, p.stderr))
        sys.exit(p.returncode or 1)
    return p


def main():
    ap = argparse.ArgumentParser(description="GH#19 BLAMP acceptance pipeline (CTest gate)")
    ap.add_argument("--probe", required=True, help="path to the gh19_alias_probe binary")
    ap.add_argument("--manifest", required=True, help="tools/gh19_manifest.tsv")
    ap.add_argument("--baseline", required=True, help="tools/gh19_naive_baseline.tsv")
    ap.add_argument("--analyzer", required=True, help="tools/gh19_alias_analyze.py")
    ap.add_argument("--gate", required=True, help="tools/check_gh19_blamp_acceptance.py")
    # Optional gate arguments. Both default OFF so the VCO sibling -- which shares this
    # driver and has neither an M<=8 byte-identity assertion nor a self-check -- keeps
    # running with exactly the argument list it ran with before.
    ap.add_argument("--gate-wants-raws", action="store_true",
                    help="also pass --current-raws <probe dir> to the gate (the Schmitt "
                         "gate's fallback byte-identity assertion needs the raw renders)")
    ap.add_argument("--gate-self-check", action="store_true",
                    help="also pass --self-check to the gate, so the gate's own "
                         "non-vacuity cases are exercised on every CI run")
    ap.add_argument("--gate-extra-arg", action="append", default=[],
                    help="pass one more argument through to the gate, in order; repeat "
                         "once per argument. Used by the task #110 classic-saw gate, "
                         "which takes per-role thresholds (--min-gain-low-db / "
                         "--min-gain-high-db) that the VCO and Schmitt gates do not "
                         "have. Additive and default-empty, so neither sibling changes.")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="gh19blamp-") if args.out is None else args.out
    try:
        probe_dir = os.path.join(tmp, "probe")
        os.makedirs(probe_dir, exist_ok=True)
        run([args.probe, "--out", probe_dir])          # render the product matrix
        tsv = os.path.join(tmp, "analyze.tsv")
        an = run([sys.executable, args.analyzer, "--dir", probe_dir,
                  "--manifest", args.manifest])
        with open(tsv, "w", encoding="utf-8") as f:
            f.write(an.stdout)
        gate_cmd = [sys.executable, args.gate, "--baseline", args.baseline,
                    "--current", tsv]
        if args.gate_wants_raws:
            gate_cmd += ["--current-raws", probe_dir]
        if args.gate_self_check:
            gate_cmd += ["--self-check"]
        gate_cmd += list(args.gate_extra_arg)
        g = run(gate_cmd)
        # Echo the gate's verdict table even on success. Otherwise a passing CTest entry
        # records only "rc=0" and the per-cell numbers -- the actual evidence -- exist
        # nowhere in the CI log. Costs ~14 lines.
        sys.stdout.write(g.stdout)
        return 0
    finally:
        if args.out is None:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
