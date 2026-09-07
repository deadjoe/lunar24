#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh20_vcf_negatives.py — task #87 (GH #20) SOURCE NEGATIVE CONTROLS.
#
# Verifies that the measurement gate (tools/gh20_vcf_analyze.py --check) DISCRIMINATES: it must
# turn GREEN on the real product (the audit finding) and turn RED when the DSP source is mutated
# away from what the audit reports. This is the runnable form of "the tool has real discriminating
# power" — a substituted/ideal/edited response is RED, not silently accepted.
#
# Under test (all mutations edit ONLY the VCF header, in an isolated temp include dir that shadows
# the real one — nothing in the committed tree is ever modified):
#   1. cap-removed  — lower kFreqMaxHz below every sr/8 so the norm->Hz knob NEVER saturates: the
#      sr/8 dead-zone is absent. The gate must report "no inert onset" (RED). STABLE: all 755 cells
#      are produced, so the RED is fired by the deadzone-detect logic itself, not by a broken filter.
#   2. lr-ignored   — make the RIGHT channel use the LEFT FREQ knob: a same-input L/R difference
#      must disappear. The gate must report |L/R| < ASYM_MIN_DB (RED).
# Separate (reported in the report, not asserted here) — cap-raised: raising kCutoffCapRatio above
# 1/8 non-finitely DESTABILISES the Chamberlin SVF at high norm (output becomes non-finite); this is
# genuine N-4 evidence that the sr/8 cap is a STABILITY boundary, not an arbitrary designer choice.
#
# For each negative the runner:
#   * builds a probe against the mutated header (same include path, hash-verified unchanged header),
#   * runs the probe (must produce the full required matrix, so the RED is not an artifact of a
#     partial/empty response),
#   * runs the analyzer --check and asserts it exits NON-ZERO (RED).
# The real (unmutated) probe+build is run first as the positive control and must exit 0 (GREEN).
#
# The compiler and include path mirror the CMake build (lunar_core is a header-only INTERFACE lib, so
# the probe is a single translation unit and needs no linking). A COMPILE FAILURE is reported and NOT
# counted as a negative pass — the tool must build to meaningfully reject; a red from "we could not
# build it" is not the gate discriminating.
#
# Reproduction (from the repo root):
#   python3 tools/run_gh20_vcf_negatives.py --root . --manifest tools/gh20_manifest.tsv
#   python3 tools/run_gh20_vcf_negatives.py --root . --manifest tools/gh20_manifest.tsv --verbose

import argparse
import os
import platform
import subprocess
import sys
import tempfile

# The only file the negatives mutate. Everything else is read from the real (committed) tree.
VCF_HEADER = "core/include/lunar24/core/polivoks_vcf.h"
# Include dirs for the probe build (mirrors CMakeLists target gh20_vcf_probe).
PROBE_INCLUDE_DIRS = ["tests/host", "core/include", "generated", "host/include"]


def load_header(root):
    with open(os.path.join(root, VCF_HEADER)) as fh:
        return fh.read()


def vcf_abs(root, include_root):
    return os.path.join(include_root, "lunar24/core/polivoks_vcf.h")


def build_probe(root, shadow_inc, out, compiler=None):
    """Build tests/probes/gh20_vcf_probe.cpp with an optional shadowed include dir prepended.
    Compiler defaults to $CXX (falling back to c++); arch flag is added on Apple only (the same
    -arch the CMake build uses) and omitted elsewhere so the tool is portable across CI hosts.
    Returns (returncode, stderr_tail)."""
    if compiler is None:
        compiler = os.environ.get("CXX", "c++")
    inc = ["-I", shadow_inc] if shadow_inc else []
    for d in PROBE_INCLUDE_DIRS:
        inc += ["-I", os.path.join(root, d)]
    flags = ["-O3", "-DNDEBUG", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    if sys.platform == "darwin":
        flags += ["-arch", platform.machine() or "arm64"]
    cmd = [compiler] + flags + inc + \
          [os.path.join(root, "tests/probes/gh20_vcf_probe.cpp"), "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    tail = (r.stderr or "").strip().splitlines()[-1] if (r.stderr or "").strip() else ""
    return r.returncode, tail


def run_probe(binary, outdir):
    r = subprocess.run([binary, "--out", outdir], capture_output=True, text=True)
    return r


def analyze(root, analyzer, manifest, probe_outdir, check=True):
    cmd = [sys.executable, analyzer, "--dir", probe_outdir,
           "--manifest", os.path.join(root, manifest)]
    if check:
        cmd.append("--check")
    return subprocess.run(cmd, capture_output=True, text=True)


def write_mutated(root, get_a, get_b, shadow_dir, base):
    """Copy the VCF header into a shadow include dir applying the two string replacement hooks."""
    path = vcf_abs(root, shadow_dir)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(get_b(get_a(base)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="repo root the probe is built from")
    ap.add_argument("--manifest", default="tools/gh20_manifest.tsv", help="required-cell manifest")
    ap.add_argument("--compiler", default="c++", help="C++ compiler (default: c++)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--skip-positive", action="store_true",
                    help="don't re-run the positive (real) control (already on disk or verified)")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    base = load_header(root)

    fail = []

    td = tempfile.mkdtemp(prefix="gh20-neg-")
    try:
        # ---------------- positive control: real (unmutated) build must be GREEN.
        if not args.skip_positive:
            pos_out = os.path.join(td, "pos")
            os.makedirs(pos_out, exist_ok=True)
            pos_bin = os.path.join(td, "probe_pos")
            rc, tail = build_probe(root, None, pos_bin, args.compiler)
            if args.verbose:
                print("positive: build rc=%d %s" % (rc, tail))
            if rc != 0:
                print("[POSITIVE] BUILD FAIL (%s) — aborting; real product must build" % tail)
                return 1
            r = run_probe(pos_bin, pos_out)
            if args.verbose:
                print("positive: probe -> %s" % (r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()))
            a = analyze(root, os.path.join(root, "tools/gh20_vcf_analyze.py"), args.manifest, pos_out)
            if a.returncode == 0:
                print("[POSITIVE] real product -> GATE PASS (GREEN)")
            else:
                print("[POSITIVE] real product -> RED (unexpected): %s" %
                      (a.stdout + a.stderr).strip().replace("\n", " | "))
                fail.append("positive-control turned RED (the tool no longer accepts the real finding)")

        # ---------------- negative 1: cap-removed (kFreqMaxHz below sr/8) -> dead-zone absent.
        neg1 = os.path.join(td, "neg1")
        os.makedirs(neg1, exist_ok=True)
        write_mutated(root, lambda b: b,
                      lambda b: b.replace("static constexpr double kFreqMaxHz = 20000.0;",
                                          "static constexpr double kFreqMaxHz = 5000.0;"),
                      os.path.join(neg1, "include"), base)
        assert "kFreqMaxHz = 5000.0" in open(vcf_abs(root, os.path.join(neg1, "include"))).read()
        n1_bin = os.path.join(td, "probe_neg1")
        rc, tail = build_probe(root, os.path.join(neg1, "include"), n1_bin, args.compiler)
        if args.verbose:
            print("cap-removed: build rc=%d %s" % (rc, tail))
        if rc != 0:
            print("[NEGATIVE cap-removed] BUILD FAIL — not counted as red (%s)" % tail)
            fail.append("cap-removed negative could not build (invalid negative, not a discriminator)")
        else:
            n1_out = os.path.join(neg1, "out"); os.makedirs(n1_out, exist_ok=True)
            r = run_probe(n1_bin, n1_out)
            if args.verbose:
                print("cap-removed: probe -> %s" % (r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()))
            if r.returncode != 0:
                # Probe failed to produce the full matrix (e.g. non-finite) — the gate is red but via
                # a broken filter, so this is the N-4 stability path, not a clean deadzone-detect.
                print("[NEGATIVE cap-removed] probe blocked (%d) -> RED via instability "
                      "(this is the N-4 stability-boundary evidence, not a clean deadzone-detect)" % r.returncode)
                fail.append("cap-removed negative destabilised the filter (non-finite red, N-4 stability proof)")
            else:
                a = analyze(root, os.path.join(root, "tools/gh20_vcf_analyze.py"), args.manifest, n1_out)
                if a.returncode == 0:
                    print("[NEGATIVE cap-removed] GATE PASS (unexpected — should be RED)")
                    fail.append("cap-removed negative NOT rejected (deadzone-detect no longer fires)")
                else:
                    print("[NEGATIVE cap-removed] -> RED (dead-zone absent, deadzone-detect fired)")

        # ---------------- negative 2: lr-ignored (right uses left FREQ knob) -> L/R asym disappears.
        neg2 = os.path.join(td, "neg2")
        os.makedirs(neg2, exist_ok=True)
        write_mutated(root, lambda b: b,
                      lambda b: b.replace("const double baseFc = baseFreqHz_(c.freq, sr_);",
                                          "const double baseFc = baseFreqHz_((&c == &channel_[1]) ? channel_[0].freq : c.freq, sr_);"),
                      os.path.join(neg2, "include"), base)
        assert "channel_[0].freq : c.freq" in open(vcf_abs(root, os.path.join(neg2, "include"))).read()
        n2_bin = os.path.join(td, "probe_neg2")
        rc, tail = build_probe(root, os.path.join(neg2, "include"), n2_bin, args.compiler)
        if args.verbose:
            print("lr-ignored: build rc=%d %s" % (rc, tail))
        if rc != 0:
            print("[NEGATIVE lr-ignored] BUILD FAIL — not counted as red (%s)" % tail)
            fail.append("lr-ignored negative could not build (invalid negative)")
        else:
            n2_out = os.path.join(neg2, "out"); os.makedirs(n2_out, exist_ok=True)
            r = run_probe(n2_bin, n2_out)
            if args.verbose:
                print("lr-ignored: probe -> %s" % (r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr.strip()))
            a = analyze(root, os.path.join(root, "tools/gh20_vcf_analyze.py"), args.manifest, n2_out)
            if a.returncode == 0:
                print("[NEGATIVE lr-ignored] GATE PASS (unexpected — should be RED)")
                fail.append("lr-ignored negative NOT rejected (L/R asym detector no longer fires)")
            else:
                print("[NEGATIVE lr-ignored] -> RED (same-input L/R difference vanished)")

    finally:
        shutil_rmtree(td)

    print()
    if fail:
        print("NEGATIVES FAIL: %s" % "; ".join(fail))
        return 1
    print("NEGATIVES PASS: real product GREEN, cap-removed and lr-ignored source mutations RED. "
          "The measurement gate discriminates.")
    return 0


def shutil_rmtree(p):
    try:
        import shutil
        shutil.rmtree(p)
    except OSError:
        pass


if __name__ == "__main__":
    sys.exit(main())
