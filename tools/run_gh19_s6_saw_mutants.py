#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""GH#19 S6 (task #120): the saw/invSaw acceptance gate must REJECT each way the candidate can be
wrong -- and it must be visible WHICH LAYER does the rejecting.

WHY THIS FILE EXISTS. `gh19_s6_saw_acceptance.py` prints a line only when a criterion FIRES; a
criterion that passes is silent. That is the right shape for a gate and the wrong shape for
evidence, because two whole classes of criterion had, before this runner, never been observed
firing in ANY run:

  * the three GUARD rows (GUARD-AMP / GUARD-DC / GUARD-FREQ), which are the guards against buying
    an "improvement" by moving the signal instead of cleaning it; in the red-first run the two arms
    were the same binary, so all three passed trivially and proved nothing;
  * the whole EQUALITY layer (`antiphase_midpoint_max_abs_output`, `sine_node_bit_diff_count`), for
    the same reason -- a naive head is trivially equal to itself. The SINE half of that layer is now
    an A/B against a neutralised arm (see ARM B below), so its second side no longer comes from a
    committed macOS snapshot; the mutation that lights it up (NC3) is unchanged.

A green light on an unfired criterion is not evidence. This runner lights each one up.

ARM B. Every gate run in this matrix is given `--neutral-arm` as well as `--cand-arm`: the equality
criterion compares the arm under test against the SAME BUILD with the two correction terms
neutralised, not against the committed pre-S6 snapshot (that comparison was not portable -- the
default patch evaluates std::sin and the two libms disagree by 1 ULP on 3.76% of arguments; see the
task report). Arm B is built ONCE from the PRODUCT header, not from a mutant's shadow: the
neutralisation anchor is the same two lines `unscaled-kernel` and `invsaw-sign-flip` rewrite, so
staging it on top of those shadows would fail the anchor-exactly-once assertion -- and arm B does not
depend on which mutant is under test, by definition it is the product minus the correction. It is
staged by tools/stage_gh19_s6_shadow.py, the same tool the product's own CMake target uses, so the
pipeline and this matrix cannot drift into running two different A/Bs under one name.

HOW IT MUTATES. Exactly as `run_gh19_s3_pulse_mutant.py` does: the probe SOURCE is unchanged, and
one header is copied into a shadow include root that is placed FIRST on the command line, so the
mutated header wins while every untouched header still resolves from the tree. Same compiler, same
flags, same source -- one shadow root is the only difference between the reference arm and a mutant
arm. Each anchor is verified to occur EXACTLY ONCE before anything is compiled: a stale anchor would
stage a PRISTINE probe that passes, which reads as "the gate cannot see this defect" when in truth
the defect was never applied.

THE MUTANTS, and the layer each one is aimed at:

  NC1 unscaled-kernel   The exact wrong alternative to the candidate: apply the value-jump kernel
                        at full amplitude wherever a node participates, instead of scaling each
                        node's correction by THAT NODE'S WEIGHT in the mix. This is the candidate
                        minus its whole justification. On stretch 0 the two corrections are equal
                        and opposite, so they cancel to exactly the naive waveform: the improvement
                        is 0.000 dB everywhere on that stretch. Aimed at the IMPROVEMENT layer.
                        ⚠️ AND IT IS THE POINT OF THIS RUNNER that the EQUALITY layer does NOT see
                        it: at the anti-phase midpoint the two unscaled corrections cancel just as
                        the two scaled ones do, and the naive waveform is 0 there too. The midpoint
                        is a NECESSARY regression guard (it catches a sign error, see NC2) and NOT a
                        discriminating criterion. This file asserts BOTH halves: NC1 must fire the
                        improvement criteria and must NOT fire the equality criteria.
  NC2 invsaw-sign-flip  One sign: the invSaw's correction added instead of subtracted. At the
                        midpoint the two terms then ADD instead of cancelling, so the output stops
                        being 0. Aimed at EQUALITY-ANTIPHASE -- this is the sign error the midpoint
                        DOES catch, and the reason it is kept.
  NC3 sine-node-leak    A leak of 1e-9 in the saw weight, floored only where the true weight is 0.
                        1e-9 against a signal of order 1 is invisible to every dB criterion in the
                        file -- 1e-6 dB on a residual a few tens of dB down -- and it is far above
                        double precision, so it is bit-visible. Aimed at EQUALITY-SINE: the exact-bit
                        criterion catches what no threshold can. This is its non-vacuity proof. Under
                        the A/B the two sides are the leak and its absence, which is the same pair
                        the old snapshot comparison used, so the mutation's aim is unchanged.
  NC4 output-gain       Every emitted sample scaled by 1.02. The fundamental moves 2% (limit 1%), so
                        a candidate can "improve" its residual-to-signal ratio simply by being
                        louder, and GUARD-AMP is what refuses it. The improvement columns stay green
                        here, which is exactly why this guard has to exist.
  NC5 dc-offset         +0.002 of DC on every emitted sample (limit 0.0015). Same shape as NC4 for
                        the DC guard.
  NC6 phase-drift       The rendered frequency drifts 2%. The zero-crossing frequency is read from
                        the PROBE, never from the analyzer whose residual columns are under test, so
                        this guard is independent of the columns it is guarding.

WHAT IS ASSERTED, per arm: the gate exits with a verdict, is never a refusal (exit 4 means no
judgement was issued, which is NOT a catch), never crashes, and the NAMED criterion fires. The
reference arm must be ACCEPTED by the same driver -- a driver that only ever produces RED proves
nothing about the gate it is driving.

Usage (used by CMake; also runnable by hand):
  run_gh19_s6_saw_mutants.py --compiler c++ --repo-root . \
      --analyzer tools/gh19_s6_saw_analyze.py --gate tools/gh19_s6_saw_acceptance.py \
      --criteria report/gh19-s6-saw-aa/acceptance_criteria.tsv \
      --baseline-arm report/gh19-s6-saw-aa/baseline_arm \
      --base-report report/gh19-s6-saw-aa/base_report.txt
"""

import argparse
import concurrent.futures
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The neutralisation edit is NOT re-spelled here: the A/B's arm B is one edit, and it is defined in
# one place (tools/stage_gh19_s6_shadow.py), shared with the CMake target that builds the product's
# arm B. Two spellings of "the correction is neutralised" could drift, and then the pipeline and this
# matrix would be running two different A/Bs under one name.
import stage_gh19_s6_shadow as shadow_stage

PROBE_SRC = "tests/probes/gh19_s6_saw_probe.cpp"
# ONE header carries every mutation, because every mutation is in the (P6) implementation or in the
# path that feeds it. vco_wave_map.h is deliberately NOT shadowed: the weights themselves are the
# thing under test and are taken from the tree exactly as the product ships them.
VCO_H = "core/include/lunar24/core/vco.h"

# The gate's own exit codes (mirrored from gh19_s6_saw_acceptance.py).
PIPE_PASS = 0
PIPE_RED = 1
PIPE_REFUSE = 4

# The correction block, verbatim, as the anchor for the two mutants that rewrite its operators.
CORRECTION_LINES = (
    "      if (sw > 0.0) v -= sw * r;   // the saw's jump is DOWNWARD (-2) -> subtract R\n"
    "      if (iw > 0.0) v += iw * r;   // the invSaw's is UPWARD (+2) -> add R\n"
)

# `emittedAt_`'s tail: the value the VCO emits for this sample, correction already in force.
EMIT_TAIL = "    return v;\n  }\n"

# ---------------------------------------------------------------------------------------------
# TWO REJECTION SURFACES, AND WHY A MUTANT MAY DECLARE EITHER ONE.
#
# Measured on the first full matrix run, not assumed: the analyzer's alignment license
# (`align_check_s6`) runs BEFORE any judged number exists, and for two of these defects it is what
# rejects the arm -- the gate never gets a report to judge. The license requires the rendered arm to
# equal the product's OWN pre-S6 law everywhere except inside the wrap window, and at the anti-phase
# midpoint cells (norm 0.125) that law is identically zero, so the license takes its DEGENERATE
# branch and demands the candidate's output be EXACTLY zero there -- no tolerance.
#
#   * `invsaw-sign-flip` makes the midpoint terms ADD instead of cancel, so the midpoint is no
#     longer zero: align_bad = 8, and the 8 are exactly the morph-0.125 cells.
#   * `dc-offset` shifts every emitted sample, so nothing matches the pre-S6 law anywhere:
#     align_bad = 168 = every rendered cell.
#   * `phase-drift` changes the rendered step, so every cell whose phase advances at all departs
#     from the pre-S6 law: align_bad = 160 = every rendered cell EXCEPT the 8 exact-zero midpoint
#     cells, which the license's degenerate branch passes trivially because 0 == 0 whatever the
#     step. That exception is itself informative: the cells that survive a frequency fault are the
#     ones that carry no signal there.
#
# So a mutant declares `expect_criterion` (rejected by the gate, with the code named) OR
# `expect_align` (rejected earlier, by the license, with the exact cell set named). Declaring the
# license as the surface is not a weakened assertion -- it is the measured truth about which layer
# answers first, and it is asserted MORE tightly than a criterion code (the exact cell set, not just
# a count). What would be dishonest is inventing a mutant engineered to slip past the license purely
# so a criterion can be seen firing; the three shadowed criteria are instead exercised directly on
# the gate, in `reachability()` below, and labelled there as what they are.
# ---------------------------------------------------------------------------------------------

MUTANTS = (
    dict(
        tag="unscaled-kernel",
        why="apply the value-jump kernel unscaled instead of per-node-weight (stretch 0 cancels "
            "to exactly naive)",
        edits=((CORRECTION_LINES,
                "      if (sw > 0.0) v -= r;   // MUTANT: kernel copied UNSCALED\n"
                "      if (iw > 0.0) v += r;   // MUTANT: kernel copied UNSCALED\n"),),
        expect_criterion=("IMPROVE-FULL", "IMPROVE-BAND"),
        forbid=("EQUALITY-ANTIPHASE", "EQUALITY-SINE"),
    ),
    dict(
        tag="invsaw-sign-flip",
        why="the invSaw correction added instead of subtracted: the midpoint terms ADD, not cancel",
        edits=(("      if (iw > 0.0) v += iw * r;   // the invSaw's is UPWARD (+2) -> add R\n",
                "      if (iw > 0.0) v -= iw * r;   // MUTANT: invSaw sign flipped\n"),),
        expect_criterion=None,
        expect_align=8,
        expect_align_kind="antiphase",
        forbid=(),
        why_align="the license's degenerate branch: the midpoint's pre-S6 law is identically 0, so a "
                  "candidate that is non-zero there is refused before the gate ever sees a report. "
                  "This is where the sign error IS caught -- the contract's midpoint row names the "
                  "defect correctly but is not the surface that answers first.",
    ),
    dict(
        tag="sine-node-leak",
        why="a 1e-9 leak where the saw weight is 0: invisible in dB, visible bit-for-bit",
        edits=(("      case VcoWaveform::kMorphRing:\n"
                "        return wave_map::sawWeight(wave_map::kRingEqual, morph_);\n",
                "      case VcoWaveform::kMorphRing:\n"
                "        {\n"
                "          const double w = wave_map::sawWeight(wave_map::kRingEqual, morph_);\n"
                "          return (w > 0.0) ? w : 1e-9;  // MUTANT: dB-invisible leak\n"
                "        }\n"),),
        expect_criterion=("EQUALITY-SINE",),
        forbid=(),
        why_reachable="this one DOES reach the gate: 1e-9 is below the license's tolerance band "
                      "(1e-6 * scale), so the arm stays licensed, while the equality criterion's "
                      "tolerance is exact -- the designed 'dB-invisible, bit-visible' case. It is "
                      "the leak that differs between the two A/B sides: arm A carries it, arm B "
                      "(neutralised) does not.",
    ),
    dict(
        tag="output-gain",
        why="every emitted sample 2% louder: the residual ratio improves, the fundamental moved",
        edits=((EMIT_TAIL, "    v *= 1.02;  // MUTANT: gain, not cleanliness\n" + EMIT_TAIL),),
        expect_criterion=("GUARD-AMP",),
        forbid=(),
        why_reachable="the license recovers the SCALE as a median of per-sample ratios, so a uniform "
                      "gain is licensed and the guard is what catches it. It also fires "
                      "EQUALITY-SINE, which is correct and expected: scaling every sample moves the "
                      "sine node too, and that criterion is an exact bit comparison.",
    ),
    dict(
        tag="dc-offset",
        why="+0.002 of DC on every emitted sample (the limit is 0.0015)",
        edits=((EMIT_TAIL, "    v += 0.002;  // MUTANT: DC, not cleanliness\n" + EMIT_TAIL),),
        expect_criterion=None,
        expect_align=168,
        expect_align_kind="every",
        forbid=(),
        why_align="an additive floor moves EVERY sample away from the pre-S6 law, so the license "
                  "refuses all 168 cells. This is why GUARD-DC is NOT exercised by a product mutant "
                  "here: a DC fault large enough to trip the guard is caught a layer earlier, and "
                  "the gate's dc column is fed by a report the license has already licensed. See "
                  "reachability(), which fires GUARD-DC on the gate directly.",
    ),
    dict(
        tag="phase-drift",
        why="the rendered frequency drifts 2%; the guard reads f0 from the probe, not the analyzer",
        edits=(("  const double step = instHz / sr_;\n",
                "  const double step = (instHz * 1.02) / sr_;  // MUTANT: rendered frequency drifts\n"),),
        expect_criterion=None,
        expect_align=160,
        expect_align_kind="all-but-antiphase",
        forbid=(),
        why_align="a changed step moves every sample whose phase advances, so the license refuses "
                  "every rendered cell except the 8 exact-zero midpoint cells -- and those pass only "
                  "because their pre-S6 law is identically 0, which is true of any step. So "
                  "GUARD-FREQ is a THIRD report-fed criterion the license answers first, and the "
                  "same 8 cells that cannot discriminate a sign error also cannot witness a "
                  "frequency fault. The frequency guard is exercised on the gate directly in "
                  "reachability(), against the probe-written scenario manifest it actually reads.",
    ),
)


def compile_cmd(repo_root, shadow_inc, out, compiler):
    """Mirrors the probe target's include set (header-only INTERFACE libs, so the source compiles
    standalone). The shadow root MUST come first, so a mutated header wins over the tree's while
    every untouched header still resolves from core/include."""
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


def stage_shadow(repo_root, shadow_root, edits):
    """Copy vco.h into a shadow include root and apply `edits`. Each anchor must occur exactly
    once; a stale anchor would stage a pristine probe that passes, which would read as "the gate
    cannot see this defect" instead of "the defect was never applied"."""
    src = os.path.join(repo_root, VCO_H)
    if not os.path.exists(src):
        print("INVALID: %s does not exist in %s" % (VCO_H, repo_root))
        return None
    text = open(src).read()
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            print("INVALID: anchor occurs %d times in %s (expected exactly 1):\n%r" % (n, VCO_H, old))
            return None
        text = text.replace(old, new)
    dst = os.path.join(shadow_root, VCO_H[len("core/include/"):])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w") as f:
        f.write(text)
    return dst


def parse_gate(lines):
    """The gate's machine-readable surface: the verdict line, the per-code counts, and the two
    equality readings. Counts are read rather than the detail lines because the detail lines are
    suppressed past MAX_DETAIL."""
    out = {"verdict": None, "fail_counts": {}, "refuse_counts": {}, "reports": {}}
    for line in lines:
        tok = line.split()
        if len(tok) >= 2 and tok[0] == "ACCEPT-GATE" and tok[1].startswith("verdict="):
            out["verdict"] = tok[1].split("=", 1)[1]
        elif len(tok) >= 3 and tok[0] == "ACCEPT-COUNT":
            # `ACCEPT-COUNT FAIL-<code> n=<count>` / `ACCEPT-COUNT REFUSE-<code> n=<count>`.
            kind, _, code = tok[1].partition("-")
            if kind in ("FAIL", "REFUSE") and tok[2].startswith("n="):
                bucket = out["fail_counts"] if kind == "FAIL" else out["refuse_counts"]
                bucket[code] = int(tok[2].split("=", 1)[1])
        elif len(tok) >= 2 and tok[0] == "ACCEPT-REPORT" and "=" in tok[1]:
            k, _, v = tok[1].partition("=")
            out["reports"][k] = v
    return out


MATRIX_HEADER = ("id", "sr", "f0", "morph", "stretch", "A", "fund", "dc", "res_db",
                 "res_1k5k_db", "res_effbd_db", "kmax", "max_abs_out")


def read_criteria(path):
    """The three facts this driver needs out of the criteria file: the ids of the cells at the
    anti-phase midpoint (so a license refusal can be asserted as an exact cell SET, not merely a
    count), the full rendered id list (so the complement can be asserted the same way), and the
    thresholds the reachability stage aims its tampers at -- read here rather than re-typed, so a
    tamper can never be built to a stale limit."""
    groups = {"cell": [], "equality_cell": [], "report_only_cell": []}
    for line in open(path):
        t = line.split()
        if t and t[0] in groups:
            groups[t[0]].append(t[1])
        elif len(t) == 3 and t[0] == "criterion":
            groups[t[1]] = float(t[2])
    anti = [c for c in groups["equality_cell"] if c.rsplit("_m", 1)[-1] == "12500"]
    groups["anti"] = anti
    groups["all"] = groups["cell"] + groups["equality_cell"] + groups["report_only_cell"]
    groups["rendered"] = len(groups["all"])
    groups["all_but_anti"] = [c for c in groups["all"] if c not in set(anti)]
    return groups


def tamper_report(src, dst, col, transform):
    """Copy an analyzer report, rewriting ONE column of its MATRIX block.

    `transform(cell_id, value) -> value` is applied to every row; every other byte -- the instrument
    lines, the header, the declared/emitted counts -- is copied verbatim, so the gate reads a
    WELL-FORMED report that states something different, not a malformed input. Returns the number of
    rows actually rewritten: a call that matched nothing must not be able to pass as a tamper.
    """
    n = 0
    with open(src) as fin, open(dst, "w") as fout:
        idx = None
        for line in fin:
            t = line.split()
            if idx is None and tuple(t) == MATRIX_HEADER:
                idx = MATRIX_HEADER.index(col)
                fout.write(line)
                continue
            if idx is not None and t and t[0].startswith("vco_") and len(t) == len(MATRIX_HEADER):
                t[idx] = "%.17g" % transform(t[0], float(t[idx]))
                fout.write(" ".join(t) + "\n")
                n += 1
                continue
            fout.write(line)
    return n


def tamper_scenarios(src_dir, dst_dir, cycles):
    """Copy an arm's directory, rewriting the `f0_meas_hz` column of its scenario manifest.

    GUARD-FREQ does not read the analyzer's report at all -- it reads the PROBE's own zero-crossing
    measurement out of `gh19_s6_scenarios.tsv`, which is exactly why it is the guard for "the
    residual improved because the pitch moved". So the only honest way to exercise it is to move
    that column. The raws are symlinked rather than copied (the arm is ~1.5 MB of windows and the
    gate resolves paths through the directory either way); every other byte of the manifest is
    copied verbatim, so the gate reads a well-formed arm that states a different pitch.

    The shift is `cycles * sr_hz / win` -- i.e. `cycles` times the gate's OWN bound, formed from the
    same two columns the gate forms it from, so the tamper is over the limit by construction rather
    than by a number chosen here. Returns (rows rewritten, smallest bound applied).
    """
    os.makedirs(dst_dir, exist_ok=True)
    for name in sorted(os.listdir(src_dir)):
        if name != "gh19_s6_scenarios.tsv":
            os.symlink(os.path.abspath(os.path.join(src_dir, name)),
                       os.path.join(dst_dir, name))
    n, min_bound = 0, None
    src = os.path.join(src_dir, "gh19_s6_scenarios.tsv")
    with open(src) as fin, open(os.path.join(dst_dir, "gh19_s6_scenarios.tsv"), "w") as fout:
        lines = [l.rstrip("\n") for l in fin if l.strip()]
        head = lines[0].split("\t")
        col = {k: i for i, k in enumerate(head)}
        for need in ("f0_meas_hz", "sr_hz", "win"):
            if need not in col:
                raise ValueError("%s: no `%s` column to tamper" % (src, need))
        fout.write(lines[0] + "\n")
        for line in lines[1:]:
            t = line.split("\t")
            bound = float(t[col["sr_hz"]]) / float(t[col["win"]])
            t[col["f0_meas_hz"]] = "%.17g" % (float(t[col["f0_meas_hz"]]) + cycles * bound)
            fout.write("\t".join(t) + "\n")
            min_bound = bound if min_bound is None else min(min_bound, bound)
            n += 1
    return n, min_bound


def reachability(tmp, ref, args, crit, bad):
    """Exercise the gate's OWN criteria on a tampered input, and say exactly what that does and does
    not prove.

    WHY THIS EXISTS. Three of the gate's judged criteria were measured to be unreachable by a
    PRODUCT mutant: `EQUALITY-ANTIPHASE`, `GUARD-DC` and `GUARD-FREQ` are all fed by artifacts the
    alignment license has already licensed, and every product mutation that would move their
    quantities is refused a layer earlier (see the block comment above MUTANTS). Leaving them
    unobserved would mean the matrix could not tell "this criterion is satisfied" from "this
    criterion can never fire at all" -- the exact blind spot this file was written to close.

    WHAT IT PROVES: the gate fires each criterion, under its own code, when its quantity moves, and
    the failure names the quantity. WHAT IT DOES NOT PROVE, AND MUST NOT BE READ AS: that a defect
    in the product reaches it. S3's lesson was that a contract negative control has to be aimed at
    the production source, and this stage is deliberately NOT that -- it never compiles or renders
    anything. It is a reachability check on the gate alone. The product-side evidence for these
    three criteria is the align-license rejection reported above, which is where the same defects
    are in fact caught.

    Returns the number of cases that actually fired their criterion, so the caller's summary reports
    what happened rather than what was intended (a constant here would survive a case silently
    dropping out of the loop).
    """
    n_fired = 0
    ref_report = ref.get("report")
    ref_arm = ref.get("arm")
    if not ref_report or not ref_arm or not os.path.exists(ref_report):
        bad.append("reachability: the reference arm produced no report to tamper with")
        return 0

    def fire(code, what, base_report, cand_report, cand_arm):
        """Run the gate once on a tampered input and assert that the aimed-at criterion fired.

        The label is part of the EVIDENCE, not only of this docstring: a reader of the run has to be
        able to tell these lines apart from the arm table above them without reading the source, so
        it is printed immediately before each one. Returns True when this case fired its criterion,
        so the caller's summary can state how many were exercised instead of a constant.
        """
        print("REACHABILITY (GATE-ONLY SYNTHETIC INPUT; NOT a product negative control; NOT an arm "
              "of the mutant matrix): %s -- %s" % (code, what), flush=True)
        g = subprocess.run([sys.executable, args.gate,
                            "--criteria", args.criteria,
                            "--plan", args.plan or os.path.join(args.baseline_arm, "gh19_s6_plan.tsv"),
                            "--base-report", base_report,
                            "--cand-report", cand_report,
                            "--base-arm", args.baseline_arm,
                            "--cand-arm", cand_arm,
                            "--neutral-arm", args.neutral_arm,
                            "--repo-root", args.repo_root], capture_output=True, text=True)
        pr = parse_gate((g.stdout or "").splitlines())
        fired = sorted(pr["fail_counts"])
        print("%-20s %-7s %-9s %s" % ("tamper:" + code, g.returncode, pr["verdict"],
                                      ",".join(fired) or "-"), flush=True)
        if g.returncode != PIPE_RED or pr["verdict"] != "RED":
            bad.append("reachability/%s: expected RED, got rc=%s verdict=%s (a criterion that cannot "
                       "be made to fire is indistinguishable from one that always passes)"
                       % (code, g.returncode, pr["verdict"]))
            return False
        if code not in pr["fail_counts"]:
            bad.append("reachability/%s: the tamper (%s) did not fire the criterion aimed at it "
                       "(fired: %s)" % (code, what, ",".join(fired) or "-"))
            return False
        return True

    # The two tampered quantities are set to values that are unambiguously on the wrong side of the
    # frozen criteria, read out of the criteria file rather than invented here: the DC shift exceeds
    # `max_dc_abs_delta`, and the midpoint reading is given a small non-zero value because
    # `antiphase_midpoint_max_abs_output` is an exact identity and any departure from 0 fires it.
    dc_shift = crit["max_dc_abs_delta"] * 2.0
    cases = (
        ("GUARD-DC", "dc", lambda cid, v: v + dc_shift,
         "every cell's dc column shifted by +%.6g (the limit is %.6g)" % (dc_shift,
                                                                        crit["max_dc_abs_delta"])),
        ("EQUALITY-ANTIPHASE", "max_abs_out", lambda cid, v: 0.001 if cid in crit["anti"] else v,
         "the %d anti-phase-midpoint cells' max_abs_out set to 0.001 (the criterion is exactly 0)"
         % len(crit["anti"])),
    )
    for code, col, fn, what in cases:
        dst = os.path.join(tmp, "tamper_%s.txt" % code)
        n = tamper_report(ref_report, dst, col, fn)
        if n == 0:
            bad.append("reachability/%s: the tamper rewrote no rows, so nothing was exercised "
                       "(a tamper that matches nothing is not a passing tamper)" % code)
            continue
        if fire(code, what, args.base_report, dst, ref_arm):
            n_fired += 1

    # GUARD-FREQ is the one that does not read the report at all, so it needs its own surface: the
    # pitch column of the probe-written scenario manifest. Aimed at 10x the gate's own bound.
    cycles = 10.0
    arm2 = os.path.join(tmp, "candarm_freq")
    n, min_bound = tamper_scenarios(ref_arm, arm2, cycles)
    if n == 0:
        bad.append("reachability/GUARD-FREQ: the scenario tamper rewrote no rows")
    elif fire("GUARD-FREQ",
              "all %d scenario rows' f0_meas_hz shifted by %.1f x the gate's own bound sr/win "
              "(smallest bound applied %.6g Hz)" % (n, cycles, min_bound),
              args.base_report, ref_report, arm2):
        n_fired += 1

    return n_fired


def wiring_control(tmp, ref, args, bad):
    """Exercise the `wiring` declaration -- the STATIC half of the equality layer -- as an A/B whose
    only difference is one planted caller.

    WHY THIS EXISTS. `S6-WIRING` is a judged criterion with its own code and its own reason string,
    and like the three criteria `reachability()` covers, a criterion that is silent on success and
    has never been seen firing cannot be told apart from one that cannot fire. What is different here
    is that its input is not a number: it is the SHAPE of the product source, so the tamper is a
    source tree, not a report column.

    THE A/B. Two copies of the same three scanned roots (core, host, generated -- ~1.5 MB, cheap).
    The gate is run with `--repo-root` pointing at each, with EVERY other input held fixed: the same
    criteria, the same plan, the same reports, the same arms, including the neutral arm. The pristine
    copy must reproduce the reference arm's verdict exactly (same inputs, only the scanned tree
    differs, and it differs by nothing) -- if it does not, then the scan is not measuring the tree
    and the second half below would prove nothing. The planted copy carries ONE added line that
    references `invSawWeight` from an existing file, and must go RED with `S6-WIRING` and no other
    change of verdict. An extra call site is the exact failure the declaration exists to catch, and
    it is planted in a file the declaration already covers (so the failure is a COUNT that moved, not
    an undeclared file).

    Returns the number of firings observed (0, 1, or 2 -- the second only if the negative half also
    misbehaves), so the caller's summary reports what happened rather than what was intended.
    """
    report = ref.get("report")
    arm = ref.get("arm")
    if not report or not arm or not os.path.exists(report):
        bad.append("wiring: the reference arm produced no report to reuse (this stage runs the gate "
                   "on FIXED inputs and varies only the scanned tree)")
        return 0

    planted_rel = "core/include/lunar24/core/vco.h"
    planted_line = ("// GH#19 S6 wiring control: one extra caller, planted by "
                    "run_gh19_s6_saw_mutants.py\n"
                    "static double wiring_control_ghost_(const Boundaries& c, double n) {\n"
                    "  return wave_map::invSawWeight(c, n);\n"
                    "}\n")
    verdicts = {}
    for label, plant in (("pristine", False), ("planted", True)):
        root = os.path.join(tmp, "wiretree_" + label)
        for sub in ("core", "host", "generated"):
            src = os.path.join(args.repo_root, sub)
            if not os.path.isdir(src):
                bad.append("wiring: %s does not exist under --repo-root %s, so the scanned tree "
                           "cannot be reproduced for the control" % (sub, args.repo_root))
                return 0
            shutil.copytree(src, os.path.join(root, sub),
                            ignore=shutil.ignore_patterns(".git", "build"))
        if plant:
            with open(os.path.join(root, planted_rel), "a") as fh:
                fh.write(planted_line)
        g = subprocess.run([sys.executable, args.gate,
                            "--criteria", args.criteria,
                            "--plan", args.plan or os.path.join(args.baseline_arm,
                                                                "gh19_s6_plan.tsv"),
                            "--base-report", args.base_report,
                            "--cand-report", report,
                            "--base-arm", args.baseline_arm,
                            "--cand-arm", arm,
                            "--neutral-arm", args.neutral_arm,
                            "--repo-root", root], capture_output=True, text=True)
        pr = parse_gate((g.stdout or "").splitlines())
        verdicts[label] = (g.returncode, pr, (g.stdout or "").splitlines())
        print("WIRING-CONTROL %-8s rc=%-3s verdict=%-7s fired=%s"
              % (label, g.returncode, pr["verdict"],
                 ",".join(sorted(pr["fail_counts"])) or "-"), flush=True)

    rc0, pr0, _ = verdicts["pristine"]
    rc1, pr1, so1 = verdicts["planted"]
    n_fired = 0
    # The negative half. The pristine copy is the same tree the reference arm was judged on, so it
    # must give the same verdict; anything else means the control is not varying what it claims to.
    if rc0 != ref.get("rc") or pr0["verdict"] != ref["parsed"]["verdict"]:
        bad.append("wiring/pristine: the copied tree produced rc=%s verdict=%s, but the reference arm "
                   "produced rc=%s verdict=%s on the same inputs; the control would then be "
                   "attributing to the planted line what the copy itself changed"
                   % (rc0, pr0["verdict"], ref.get("rc"), ref["parsed"]["verdict"]))
    elif "S6-WIRING" in pr0["fail_counts"]:
        bad.append("wiring/pristine: S6-WIRING fired on the UNMODIFIED tree, so it fires for "
                   "something other than an extra caller and the planted half below proves nothing")
    else:
        n_fired += 1
    # The positive half, and the reason it is unconditional: this is the firing the declaration
    # exists for, and a control that only reports its own success would hide its absence.
    if rc1 != PIPE_RED or pr1["verdict"] != "RED" or "S6-WIRING" not in pr1["fail_counts"]:
        bad.append("wiring/planted: one added caller of invSawWeight was expected to make the gate "
                   "RED with S6-WIRING; got rc=%s verdict=%s fired=%s"
                   % (rc1, pr1["verdict"], ",".join(sorted(pr1["fail_counts"])) or "-"))
    else:
        fires = [l for l in so1 if l.startswith("FAIL-S6-WIRING")]
        print("WIRING-CONTROL        %s" % (fires[0] if fires else
                                            "(S6-WIRING counted, detail line not captured)"),
              flush=True)
        n_fired += 1
    return n_fired


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def build_neutral_arm(repo_root, tmp, compiler, plan_path):
    """Compile + render arm B ONCE, and share it across every arm of the matrix.

    WHY ONCE, AND WHY FROM THE PRODUCT HEADER. The neutralisation anchor is the same two lines two of
    the mutants rewrite (`unscaled-kernel`, `invsaw-sign-flip`), so staging the neutral edit on top of
    THOSE shadows would fail the anchor-exactly-once assertion -- and the fix is not a second anchor,
    it is the observation that arm B does not depend on which mutant is under test: by definition it
    is arm A minus the correction, and arm A is the product. Building it from the product header also
    means the seven mutant runs share ONE compile and ONE render instead of seven.

    The plan identity and the witness non-vacuity are the GATE's refusals (NEUTRAL-ARM), and they run
    for every arm; the check here is the cheap early one, so a mis-staged shadow fails with a clear
    message before seven arms have been rendered. Returns (arm_dir, None) or (None, reason)."""
    shadow = os.path.join(tmp, "inc_neutral")
    os.makedirs(shadow, exist_ok=True)
    staged = shadow_stage.stage_neutral(repo_root, shadow, quiet=True)
    if staged is None:
        return None, ("the neutral arm's shadow header could not be staged (the INVALID line above "
                      "names the reason): without it arm B would be the product, and the equality "
                      "criterion would compare the product against itself")
    probe = os.path.join(tmp, "probe_neutral")
    b = subprocess.run(compile_cmd(repo_root, shadow, probe, compiler),
                       capture_output=True, text=True)
    if b.returncode != 0:
        return None, ("the neutral probe did not compile (arm B must build under the SAME command as "
                      "the arms): %s" % (b.stderr or b.stdout)[-2000:])
    arm = os.path.join(tmp, "arm_neutral")
    os.makedirs(arm, exist_ok=True)
    p = subprocess.run([probe, "--out", arm], capture_output=True, text=True)
    if p.returncode != 0:
        return None, ("the neutral probe failed to render (rc=%d): %s"
                      % (p.returncode, (p.stdout or "")[-2000:]))
    neutral_plan = os.path.join(arm, "gh19_s6_plan.tsv")
    if not os.path.exists(neutral_plan):
        return None, "the neutral arm rendered no plan at %s" % neutral_plan
    if not os.path.exists(plan_path):
        return None, "no plan to compare the neutral arm against at %s" % plan_path
    if sha256_of(neutral_plan) != sha256_of(plan_path):
        return None, ("the neutral arm's plan is not the one the other arms are judged against "
                      "(%s vs %s)" % (sha256_of(neutral_plan), sha256_of(plan_path)))
    print("NEUTRAL-ARM %s shadow_tree_sha256=%s neutral_sha256=%s plan_sha256=%s"
          % (arm, staged[1], staged[2], sha256_of(neutral_plan)), flush=True)
    return arm, None


def run_arm(tag, repo_root, tmp, compiler, args):
    """Compile + render + analyze + gate one arm. Returns a result dict, never raises."""
    res = {"tag": tag, "stage": "", "ok": False}
    shadow = os.path.join(tmp, "inc_" + tag)
    os.makedirs(shadow, exist_ok=True)
    edits = () if tag == "reference" else next(m["edits"] for m in MUTANTS if m["tag"] == tag)
    if stage_shadow(repo_root, shadow, edits) is None:
        res["stage"] = "INVALID-ANCHOR"
        return res

    probe = os.path.join(tmp, "probe_" + tag)
    b = subprocess.run(compile_cmd(repo_root, shadow, probe, compiler),
                       capture_output=True, text=True)
    if b.returncode != 0:
        res["stage"] = "BUILD-FAILED"
        res["log"] = (b.stderr or b.stdout)[-2000:]
        return res

    arm = os.path.join(tmp, "arm_" + tag)
    os.makedirs(arm, exist_ok=True)
    p = subprocess.run([probe, "--out", arm], capture_output=True, text=True)
    if p.returncode != 0:
        res["stage"] = "PROBE-FAILED rc=%d" % p.returncode
        res["log"] = (p.stdout or "")[-2000:]
        return res

    report = os.path.join(tmp, "report_" + tag + ".txt")
    a = subprocess.run([sys.executable, args.analyzer, "--arm", arm, "--label", tag,
                        "--out", report], capture_output=True, text=True)
    # The alignment license's verdict, parsed rather than inferred: `align_bad` stays None when the
    # line is absent, and None is NOT read as zero anywhere below -- a missing parse would otherwise
    # turn "the analyzer never reported" into "the analyzer reported nothing wrong".
    so = a.stdout or ""
    res["align_stdout"] = so
    m = re.search(r"^ALIGN-GATE ok=(\d+) checked=(\d+) bad=(\d+)", so, re.M)
    res["align_checked"] = int(m.group(2)) if m else None
    res["align_bad"] = int(m.group(3)) if m else None
    res["align_ids"] = re.findall(r"^  ALIGN-FAIL (\S+)", so, re.M)
    if a.returncode != 0 or not os.path.exists(report):
        res["stage"] = "ANALYZE-FAILED rc=%d" % a.returncode
        res["log"] = so[-2000:]
        return res

    g = subprocess.run([sys.executable, args.gate,
                        "--criteria", args.criteria,
                        "--plan", args.plan or os.path.join(args.baseline_arm, "gh19_s6_plan.tsv"),
                        "--base-report", args.base_report,
                        "--cand-report", report,
                        "--base-arm", args.baseline_arm,
                        "--cand-arm", arm,
                        "--neutral-arm", args.neutral_arm,
                        "--repo-root", repo_root], capture_output=True, text=True)
    res["rc"] = g.returncode
    res["parsed"] = parse_gate((g.stdout or "").splitlines())
    res["stdout"] = g.stdout or ""
    res["report"] = report
    res["arm"] = arm
    res["stage"] = "GATED"
    return res


def main():
    ap = argparse.ArgumentParser(
        description="GH#19 S6: the acceptance gate must name the layer that rejects each defect")
    ap.add_argument("--compiler", required=True, help="the C++ compiler CMake is using")
    ap.add_argument("--repo-root", default=".", help="repo root")
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--gate", required=True)
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--baseline-arm", required=True,
                    help="the committed baseline arm (the naive head's raw windows)")
    ap.add_argument("--base-report", required=True,
                    help="the committed baseline report the improvement columns are read out of")
    ap.add_argument("--plan", default=None,
                    help="the probe's declared plan (default: the baseline arm's own)")
    ap.add_argument("--out", default=None, help="scratch dir (default: a tempdir, removed on exit)")
    ap.add_argument("--only", default=None, help="run one tag plus the reference (for a re-check)")
    ap.add_argument("--jobs", type=int, default=min(6, os.cpu_count() or 1),
                    help="arms to run concurrently (each is an independent directory)")
    args = ap.parse_args()

    root = args.repo_root
    tmp = tempfile.mkdtemp(prefix="gh19s6mut-") if args.out is None else args.out
    results = []
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)

        wanted = ["reference"] + [m["tag"] for m in MUTANTS
                                  if args.only is None or m["tag"] == args.only]
        # Arm B of the equality criterion, built and rendered ONCE for the whole matrix, before any
        # arm is judged. Every arm's gate run reads it, including the reference's: if it were staged
        # from a mutant's shadow the reference would be judged against a mutated arm B, and if it
        # failed to stage at all, every arm would be compared against the product and the equality
        # criterion would become the degenerate identity. Both are refused here rather than reported.
        neutral_arm, err = build_neutral_arm(root, tmp, args.compiler,
                                            args.plan or os.path.join(args.baseline_arm,
                                                                      "gh19_s6_plan.tsv"))
        if neutral_arm is None:
            print("\nS6-MUTANT FAILED:\n  * %s" % err, flush=True)
            return 1
        args.neutral_arm = neutral_arm
        # The arms are independent by construction -- each has its own shadow include root, its own
        # probe binary, its own arm directory and its own report -- so they run concurrently. The
        # ANALYZER is the cost (a pure-Python band-limited series per cell, minutes per arm), and
        # running the arms one after another put this gate near half an hour, which is more than a
        # PR-time gate should cost. Reporting order is fixed by `wanted`, not by completion.
        by_tag = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = {pool.submit(run_arm, t, root, tmp, args.compiler, args): t for t in wanted}
            for fut in concurrent.futures.as_completed(futs):
                t = futs[fut]
                by_tag[t] = fut.result()
                r = by_tag[t]
                print("ARM %s: %s" % (t, r["stage"] if r["stage"] != "GATED" else
                                      "rc=%d verdict=%s fired=%s" % (
                                          r["rc"], r["parsed"]["verdict"],
                                          ",".join(sorted(r["parsed"]["fail_counts"])) or "-")),
                      flush=True)
                if r["stage"] != "GATED" and "log" in r:
                    print(r["log"], flush=True)
        results = [by_tag[t] for t in wanted]

        # ---- JUDGEMENT ------------------------------------------------------------------------
        bad = []
        crit = read_criteria(args.criteria)
        ref = results[0]
        if ref["stage"] != "GATED":
            bad.append("the reference arm did not reach the gate: %s" % ref["stage"])
        elif ref["rc"] != PIPE_PASS or ref["parsed"]["verdict"] != "PASS":
            bad.append("the reference arm was not ACCEPTED (rc=%s verdict=%s); a driver that only "
                       "produces RED proves nothing about the gate it drives"
                       % (ref.get("rc"), ref["parsed"]["verdict"]))

        checked = [m for m in MUTANTS if args.only is None or m["tag"] == args.only]
        print("\n%-20s %-7s %-9s %s" % ("mutant", "rc", "verdict", "fired criteria"), flush=True)
        for r, m in zip(results[1:], checked):
            if m.get("expect_align") is not None:
                # This defect is answered by the analyzer's alignment license, one layer BELOW the
                # gate: the license runs before any judged number exists, so the gate must never have
                # been reached, and the refusal must be attested by a parsed count rather than by a
                # missing line read as zero.
                # The declared kind is printed WITH the count so a bare `160` is self-describing:
                # "160 of 168, all-but-antiphase" tells the reader what 160 means without having to
                # hold the cell groups in their head, and it is the same string the assertion below
                # checks against.
                print("%-20s %-7s %-9s %s" % (m["tag"], "-", "ANALYZE-REFUSED",
                                              "align_bad=%s of %s [%s]"
                                              % (r.get("align_bad"), crit["rendered"],
                                                 m.get("expect_align_kind"))), flush=True)
                if r["stage"] != "ANALYZE-FAILED rc=1":
                    bad.append("%s: expected the alignment license to refuse the arm (%s), got %s"
                               % (m["tag"], "ANALYZE-FAILED rc=1", r["stage"]))
                    continue
                if r.get("align_bad") is None:
                    bad.append("%s: the analyzer emitted no ALIGN-GATE line, so the refusal cannot be "
                               "attributed at all (an absent reading is not a reading of zero)"
                               % m["tag"])
                    continue
                if "parsed" in r:
                    bad.append("%s: the gate ran on an arm the alignment license had refused; a "
                               "judgement must not exist for an unlicensed arm" % m["tag"])
                kind = m.get("expect_align_kind")
                want_ids = {"every": crit["all"],
                            "antiphase": crit["anti"],
                            "all-but-antiphase": crit["all_but_anti"]}[kind]
                if r["align_bad"] != len(want_ids) or r["align_checked"] != crit["rendered"]:
                    bad.append("%s: the license refused %d of %d cells, expected %d of %d"
                               % (m["tag"], r["align_bad"], r["align_checked"], len(want_ids),
                                  crit["rendered"]))
                # The SET is the assertion, not the count: two different cell sets of the same size
                # would satisfy a count, and the whole point of this branch is naming which cells
                # the license answers for.
                got_ids, want_sorted = sorted(r["align_ids"]), sorted(want_ids)
                if got_ids != want_sorted:
                    extra = [c for c in got_ids if c not in set(want_sorted)]
                    absent = [c for c in want_sorted if c not in set(got_ids)]
                    bad.append("%s: the refused cells were not the declared set (%s); not expected: "
                               "%s; expected but licensed: %s"
                               % (m["tag"], kind, ",".join(extra) or "-", ",".join(absent) or "-"))
                continue
            if r["stage"] != "GATED":
                bad.append("%s: %s" % (m["tag"], r["stage"]))
                continue
            pr = r["parsed"]
            fired = sorted(pr["fail_counts"])
            print("%-20s %-7s %-9s %s" % (m["tag"], r["rc"], pr["verdict"],
                                          ",".join(fired) or "-"), flush=True)
            # A refusal is NOT a catch: exit 4 means no judgement was issued at all.
            if pr["refuse_counts"]:
                bad.append("%s: the gate REFUSED (%s); a refusal issues no judgement"
                           % (m["tag"], ",".join(sorted(pr["refuse_counts"]))))
            if r["rc"] == PIPE_REFUSE:
                bad.append("%s: exited EXIT_REFUSE (%d)" % (m["tag"], PIPE_REFUSE))
            if r["rc"] != PIPE_RED or pr["verdict"] != "RED":
                bad.append("%s: expected a RED verdict, got rc=%s verdict=%s"
                           % (m["tag"], r["rc"], pr["verdict"]))
            missing = [c for c in (m.get("expect_criterion") or ()) if c not in pr["fail_counts"]]
            if missing:
                bad.append("%s: the named criterion(s) %s did not fire (fired: %s)"
                           % (m["tag"], ",".join(missing), ",".join(fired) or "-"))
            leaked = [c for c in m["forbid"] if c in pr["fail_counts"]]
            if leaked:
                bad.append("%s: the criterion(s) %s fired, and this mutant is the case that says "
                           "they must NOT (they are aimed at a different defect than the one "
                           "under test here)" % (m["tag"], ",".join(leaked)))

        # NC1's second half, asserted explicitly because it is the load-bearing boundary claim:
        # the anti-phase midpoint must NOT be what catches "copied the kernel unscaled". If it ever
        # does, the boundary declaration in the criteria file and in vco_wave_map.h's (P6-note)
        # would be wrong and the midpoint would be being credited with discriminating power it
        # does not have.
        if results and results[0]["stage"] == "GATED":
            for r, m in zip(results[1:], checked):
                if r["stage"] != "GATED":
                    continue
                mid = r["parsed"]["reports"].get("antiphase_midpoint_worst_abs_output")
                ref_mid = results[0]["parsed"]["reports"].get("antiphase_midpoint_worst_abs_output")
                print("%-20s midpoint=%s (reference %s) sine_bit_diff=%s"
                      % (m["tag"], mid, ref_mid,
                         r["parsed"]["reports"].get("sine_node_worst_bit_diff_count")), flush=True)
                if m["tag"] == "unscaled-kernel" and mid != ref_mid:
                    bad.append("unscaled-kernel: the anti-phase midpoint moved (%s vs reference %s) "
                               "when it must not -- the boundary claim is that the midpoint "
                               "cannot discriminate this defect" % (mid, ref_mid))

        # The criteria measured to be unreachable from the product side, exercised on the gate
        # itself and reported as reachability -- never as a product negative control.
        if results and results[0]["stage"] == "GATED":
            print("", flush=True)
            n_reach = reachability(tmp, results[0], args, crit, bad)
            # The wiring declaration is the other gate criterion that can be silent because nothing
            # fired, and it is exercised the same way: on fixed inputs, varying only the scanned tree.
            print("", flush=True)
            n_wire = wiring_control(tmp, results[0], args, bad)
        else:
            n_reach = 0
            n_wire = 0

        try:
            shutil.rmtree(tmp)
        except OSError:
            pass

        if bad:
            print("\nS6-MUTANT FAILED:")
            for b in bad:
                print("  * %s" % b)
            return 1
        # The counts are DERIVED, not written into the sentence. A hardcoded "two" went stale the
        # moment a third license-shadowed criterion was measured, and a summary that understates the
        # finding while the three lines it summarises sit directly above it is worse than no summary.
        n_align = sum(1 for m in checked if m.get("expect_align") is not None)
        print("\nS6-MUTANT PASS: the reference went through the gate and was ACCEPTED; each of the "
              "%d defects was rejected by the layer named for it -- %d by a verdict on the gate and "
              "%d by the alignment license, which answers first -- the %d criteria no product "
              "mutant can reach were exercised directly on the gate and fired under their own "
              "names, and the wiring declaration was exercised by planting one extra caller into a "
              "copy of the scanned tree and observing S6-WIRING fire (and not fire on the same tree "
              "unplanted, %d of 2 halves). The anti-phase midpoint did NOT fire on the defect it "
              "cannot see."
              % (len(checked), len(checked) - n_align, n_align, n_reach, n_wire))
        return 0
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
