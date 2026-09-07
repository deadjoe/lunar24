#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh20_vcf_gate.py — task #87 (GH #20) CI entry point for the VCF response gate.
#
# Orchestrates the REAL-PRODUCT measurement end-to-end:
#   1. Run the probe (tests/probes/gh20_vcf_probe.cpp) into a scratch dir -> gh20_scenarios.tsv.
#   2. Run the verified analyzer (tools/gh20_vcf_analyze.py) with --check on that output.
# The analyzer is the authority: exit 0 means the produced matrix spans the manifest exactly AND the
# response shows the audit finding (sr/8 dead-zone, 8k cross-rate gap, same-input L/R asymmetry) with
# the measurement tool's negative controls passing. This is the runnable, CI-enforced form of the
# committed matrix. The gate never changes production DSP (reading only), so a cap-free / R-ignored
# source turns it RED (the source negatives are verified separately against an isolated mutated tree).
#
# Reproduction (from the build dir):
#   ctest -R gh20_vcf_acceptance

import argparse
import os
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True, help="path to the built gh20_vcf_probe")
    ap.add_argument("--manifest", required=True, help="path to tools/gh20_manifest.tsv")
    ap.add_argument("--analyzer", required=True, help="path to tools/gh20_vcf_analyze.py")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="gh20-gate-") as td:
        # 1) real-product probe -> tsV.
        probe_dir = os.path.join(td, "probe")
        os.makedirs(probe_dir, exist_ok=True)
        r = subprocess.run([args.probe, "--out", probe_dir])
        if r.returncode != 0:
            print("GATE: probe exited %d (a required cell was not produced)" % r.returncode,
                  file=sys.stderr)
            return r.returncode
        # 2) analyzer (--check) on the real probe output.
        a = subprocess.run([sys.executable, args.analyzer, "--dir", probe_dir,
                            "--manifest", args.manifest, "--check"])
        return a.returncode


if __name__ == "__main__":
    sys.exit(main())
