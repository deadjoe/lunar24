#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #86 / GH#19: one-command REAL-PRODUCT pipeline used by the CTest gate
# `gh19_blamp_acceptance`. Runs the compiled product probe into a scratch dir, feeds it
# to the verified analyzer, then runs the BLAMP acceptance gate (current vs the committed
# naive baseline). Exit 0 = accepted; non-zero = a required triangle cell fails.
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
        sys.stderr.write("command failed: %s\n%s\n" % (" ".join(cmd), p.stderr))
        sys.exit(p.returncode or 1)
    return p


def main():
    ap = argparse.ArgumentParser(description="GH#19 BLAMP acceptance pipeline (CTest gate)")
    ap.add_argument("--probe", required=True, help="path to the gh19_alias_probe binary")
    ap.add_argument("--manifest", required=True, help="tools/gh19_manifest.tsv")
    ap.add_argument("--baseline", required=True, help="tools/gh19_naive_baseline.tsv")
    ap.add_argument("--analyzer", required=True, help="tools/gh19_alias_analyze.py")
    ap.add_argument("--gate", required=True, help="tools/check_gh19_blamp_acceptance.py")
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
        run([sys.executable, args.gate, "--baseline", args.baseline,
             "--current", tsv])
        return 0
    finally:
        if args.out is None:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
