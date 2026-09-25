#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s3_pulse_mutants.py — task #119 (GH #19 S3) ISOLATED PRODUCTION MUTANTS.
#
# The S3 slice claims that the emitted pulse is the TWO-EDGE correction
# `naive + R(t, w) - R(frac(t - duty), w)` with the kernel width capped at w = min(dt, 0.5), that
# the PWM CV is consumed in the frame it is published, that the hard-sync jump is sized on the
# EMITTED signal, and that none of it depends on where the block boundaries fall. A test that
# passes on the shipped source proves nothing about whether it DISCRIMINATES, so every load-bearing
# claim is re-run against a source mutated away from it, in an ISOLATED shadow include tree: the
# committed tree is never written to.
#
# Each mutant is ONE focused edit to a production header, chosen so the mutant stays a well-formed,
# BUILDABLE program (a mutant that does not compile proves nothing about the test):
#
#   bypass-correction        pulse_blep_kernel.h  the correction returns 0 unconditionally: the
#                                                 product pulse is the naive hard switch again.
#   wrong-sign               pulse_blep_kernel.h  edge B's residual is ADDED instead of subtracted:
#                                                 the second edge's kernel has the wrong polarity.
#   wrong-position           pulse_blep_kernel.h  edge B is read at frac(t + duty) instead of
#                                                 frac(t - duty): the correction sits on the wrong
#                                                 side of the cycle.
#   restore-duty-hard-switch pulse_blep_kernel.h  the WITHDRAWN duty guard is put back
#                                                 (`if (!polyblepPulseWindowsDisjoint(duty, dt))
#                                                 return 0.0;`), which is what the header's boundary
#                                                 1 note forbids ("adding one is a defect, not a
#                                                 safety net" — it caused a 0.5625 jump at
#                                                 duty = 2*dt with no edge crossed).
#   remove-width-cap         pulse_blep_kernel.h  `w = dt` again: the kernel width is no longer
#                                                 capped above half a sample.
#   pwm-one-frame-late       machine_runtime.h    the PWM consumer reads the PREVIOUS frame's
#                                                 resolved CV, a one-sample latency that a
#                                                 constant-CV test cannot see.
#   block-boundary-graph-drive device_adapter.h   the control graph is driven at the BLOCK
#                                                 boundary instead of per frame (`(f == 0)`),
#                                                 which makes the partition a state boundary.
#   phase-reset-per-sample   machine_runtime.h    the raw `syncPulse()` (phase := 0, no jump) at
#                                                 every runtime entry. The product drives the
#                                                 runtime one frame at a time, so this is a
#                                                 PER-SAMPLE reset: it freezes the pulse, and it
#                                                 is the non-vacuity guard that catches it.
#   output-one-frame-late    device_adapter.h     the product's output is held back one frame. The
#                                                 ONLY mutation here that a phase fit cannot see
#                                                 (a uniform delay is a phase shift), so it is the
#                                                 absolute-origin anchor that has to reject it.
#
# WHAT COUNTS AS RED (the point of this runner — a compile error is NOT a red):
#   * the REFERENCE (unmutated) arm must exit 0 AND print its COMPLETE summary AND report 0 failures;
#   * each MUTANT arm must exit with an expected NORMAL failure status (1), AND print its complete
#     summary, AND name the SPECIFIC check this mutant is supposed to trip.
#   A build failure, a signal/abort exit (>=128 or negative), a zero exit, a truncated/absent
#   summary, or a failure on some OTHER check is reported as an INVALID mutant, not as a red —
#   the two failure surfaces stay named separately.
#
# Reproduction (from the repo root):
#   python3 tools/run_gh19_s3_pulse_mutants.py
#   python3 tools/run_gh19_s3_pulse_mutants.py --verbose
#   python3 tools/run_gh19_s3_pulse_mutants.py --only wrong-sign,remove-width-cap

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from _gh19_textio import open_text

# ---- test targets (mirrors CMakeLists: header-only consumers plus the allocator TU) -------------
TARGETS = {
    "test_gh19_s3_pulse_product_acceptance": {
        "sources": ["tests/host/test_gh19_s3_pulse_product_acceptance.cpp",
                    "tests/host/test_state_apply_oracle_allocator.cpp"],
        "suite": "test_gh19_s3_pulse_product_acceptance",
        # mini_test.h: success -> stdout "[<suite>] N checks OK" rc 0
        #              failure -> "[<suite>] F/N checks FAILED" rc 1, plus "FAIL file:line: expr".
        # This suite's allocator probe NEEDS the allocator TU, which owns the replaced
        # operator new/delete that the zero-alloc render-window check counts.
        "inc": ["tests/host", "tests/core", "host/include"],
        "summary_ok": re.compile(r"^\[test_gh19_s3_pulse_product_acceptance\] \d+ checks OK$", re.M),
        "summary_red": re.compile(r"^\[test_gh19_s3_pulse_product_acceptance\] \d+/\d+ checks "
                                  r"FAILED$", re.M),
        "named": re.compile(r"^FAIL .*:\d+: (?P<expr>.*)$", re.M),
    },
}

KERNEL = "core/include/lunar24/core/pulse_blep_kernel.h"
MACHINE_RT = "core/include/lunar24/core/machine_runtime.h"

ACCEPT = "test_gh19_s3_pulse_product_acceptance"

# Every mutant: (id, target, {file: [(old, new), ...]}, the check expression it must trip).
# `expect` is a substring that must appear in a NAMED failure line — never just "nonzero exit".
# `expect` names the bool local the suite uses, because mini_test.h stringifies the CHECK
# expression and this suite deliberately checks well-named locals.
MUTANTS = [
    {
        "id": "bypass-correction",
        "target": ACCEPT,
        "edits": {KERNEL: [(
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;",
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  return 0.0;  // MUTANT: the two-edge correction bypassed entirely\n"
            "  if (!(dt > 0.0)) return 0.0;")]},
        "expect": "theProductPulseEqualsTheTwoEdgeFormula",
        "claim": "the product's emitted pulse really carries the two-edge correction",
    },
    {
        "id": "wrong-sign",
        "target": ACCEPT,
        "edits": {KERNEL: [(
            "  return polyblepResidual(t, w) - polyblepResidual(b, w);",
            "  return polyblepResidual(t, w) + polyblepResidual(b, w);")]},
        "expect": "theProductPulseIsNotTheWrongSignVariant",
        "claim": "edge B's residual is SUBTRACTED (a pulse is not a saw with a shifted phase)",
    },
    {
        "id": "wrong-position",
        "target": ACCEPT,
        "edits": {KERNEL: [(
            "  double b = t - duty;",
            "  double b = t + duty;")]},
        "expect": "theProductPulseIsNotTheWrongEdgePositionVariant",
        "claim": "edge B's kernel sits at the phase it is actually seen from, frac(t - duty)",
    },
    {
        # The withdrawn switch, restored VERBATIM. It is caught only in the NARROW-duty cell
        # (min(duty, 1-duty) < 2*dt), which is exactly why that cell is in the fixture: on every
        # other cell in the set the guard is inert and the mutation would be a no-op.
        "id": "restore-duty-hard-switch",
        "target": ACCEPT,
        "edits": {KERNEL: [(
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;",
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;\n"
            "  if (!polyblepPulseWindowsDisjoint(duty, dt)) return 0.0;  // MUTANT: withdrawn guard")]},
        "expect": "theProductPulseIsNotTheWithdrawnDutyGuardedVariant",
        "claim": "the correction does NOT branch on a duty-derived regime label",
    },
    {
        "id": "remove-width-cap",
        "target": ACCEPT,
        "edits": {KERNEL: [(
            "  const double w = (dt < kPolyblepMaxDt) ? dt : kPolyblepMaxDt;",
            "  const double w = dt;")]},
        "expect": "theProductPulseIsNotTheUncappedKernelVariant",
        "claim": "the kernel WIDTH is capped at half a sample while the phase advances by dt",
    },
    {
        "id": "pwm-one-frame-late",
        "target": ACCEPT,
        "edits": {MACHINE_RT: [
            # CONSUME the previous frame's value (not merely seed a local from it — a seed that is
            # then overwritten by the resolver is a no-op mutant, which is exactly how an
            # ineffective negative control passes itself off as a green).
            ("""        double pwm = 0.0;
        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        vcA_.setPwCv(pwm);""",
             """        const double prevA = pwmLatchedA_;
        double pwm = 0.0;
        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        pwmLatchedA_ = pwm;
        vcA_.setPwCv(prevA);"""),
            ("""        double pwm = 0.0;
        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInB_, pwm, driveGraph));
        vcB_.setPwCv(pwm);""",
             """        const double prevB = pwmLatchedB_;
        double pwm = 0.0;
        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInB_, pwm, driveGraph));
        pwmLatchedB_ = pwm;
        vcB_.setPwCv(prevB);"""),
            ("  JackId pwmInA_{0};",
             "  JackId pwmInA_{0}; double pwmLatchedA_ = 0.0;"),
            ("  JackId pwmInB_{0};",
             "  JackId pwmInB_{0}; double pwmLatchedB_ = 0.0;"),
        ]},
        "expect": "thePwmSinkConsumesTheSameFramesPublishedCv",
        "claim": "the graph PWM CV is consumed in the SAME frame it is published",
    },
    {
        # WHERE THE BLOCK BOUNDARY ACTUALLY IS. Measured, not assumed: the product drives the
        # runtime ONE FRAME AT A TIME — DeviceAdapter::renderBlock loops `rt.processBlock(&in, 1,
        # &out, true)` (device_adapter.h:297-310, "One drain, one frame, one timebase"), so
        # MachineRuntime::processBlock never sees a multi-frame block on the product path. A
        # mutation inserted there is therefore a PER-SAMPLE mutation, not a block-boundary one: the
        # phase is re-seeded before every sample and the output goes constant (that mutation is kept
        # as `phase-reset-per-sample` below). The product's block boundary is the ADAPTER's
        # renderBlock, and the block-boundary defect this gate must reject is the canonical one:
        # the control graph driven at the block boundary instead of per frame, which turns the
        # partition into a state boundary. `driveGraph=false` makes the ONE control-sink resolver
        # return false and leave the value at its caller-side 0.0 (machine_runtime.h:3026-3029), so
        # on frames 1.. of every block the PWM CV latches to 0 and the duty differs between the
        # uniform and the irregular partition — while the audio keeps moving, so the comparison is
        # still live.
        "id": "block-boundary-graph-drive",
        "target": ACCEPT,
        "edits": {"core/include/lunar24/core/device_adapter.h": [(
            "    rt.processBlock(&in, 1, &out, true);",
            "    rt.processBlock(&in, 1, &out, (f == 0));  // MUTANT: graph driven per BLOCK")]},
        "expect": "irregularBlocksReproduceTheUniformPartitionExactly",
        "claim": "the control graph is driven per FRAME, so a block boundary is a delivery detail",
    },
    {
        # The seventh mutation the order lists, placed where it can be expressed: the RAW
        # `syncPulse()` primitive (phase := 0, no jump) at the top of every runtime processBlock.
        # As measured above this degenerates to a per-sample reset on the product path, which
        # FREEZES the pulse (constant +1, i.e. duty 1.0) rather than making it partition-dependent —
        # so the partition comparison is not the surface that catches it, its own non-vacuity guard
        # is: an invariance claim over a capture that does not move is not evidence. Kept as its own
        # mutant precisely because it documents that reading.
        "id": "phase-reset-per-sample",
        "target": ACCEPT,
        "edits": {MACHINE_RT: [(
            """    std::uint32_t ei = 0;
    for (std::size_t i = 0; i < n; ++i) {""",
            """    vcA_.syncPulse();  // MUTANT: phase re-seeded at every runtime entry
    std::uint32_t ei = 0;
    for (std::size_t i = 0; i < n; ++i) {""")]},
        "expect": "thePartitionStimulusIsChanging",
        "claim": "the audio state (phase) continues across frames; a frozen capture is not evidence",
    },
    {
        # THE ONE A PHASE FIT CANNOT SEE (director note, task #119): one frame of output latency on
        # the product's own output boundary. Every other mutant here is caught by a shape or a
        # plumbing claim; this one is caught ONLY by the absolute-origin anchor, because
        # `frac(phi + i*step)` with `phi' = phi - step` reproduces a uniformly delayed capture
        # EXACTLY — the fitted-phase check (`theProductPulseEqualsTheTwoEdgeFormula`) stays green
        # under this mutant, and that is the whole reason the pinned-phase criterion exists.
        #
        # The latch lives in DeviceAdapter (a per-ADAPTER member, so it persists across the
        # engine's block calls and the delay is a real product latency, not a per-block artefact),
        # which is where the product's output boundary actually is: StandaloneAudioEngine::
        # processBlock hands the block to `adapter_.renderBlock(...)`, and renderBlock drives the
        # runtime one frame at a time. `writeOutput_` is const and takes the value by const&, so the
        # mutation needs no other change; frame 0 of the engine therefore emits the latch's zero
        # initial value and every later frame emits its predecessor.
        "id": "output-one-frame-late",
        "target": ACCEPT,
        "edits": {"core/include/lunar24/core/device_adapter.h": [
            ("  bool hasPlan_ = false;",
             "  bool hasPlan_ = false;\n"
             "  RuntimeOutput latencyHeld_{};  // MUTANT: one frame of output latency"),
            ("""    RuntimeOutput out{};
    rt.processBlock(&in, 1, &out, true);
    writeOutput_(out, planarOut, f);  // output direction (single primitive).""",
             """    RuntimeOutput out{};
    rt.processBlock(&in, 1, &out, true);
    const RuntimeOutput delayed = latencyHeld_;  // MUTANT: emit the PREVIOUS frame's value
    latencyHeld_ = out;
    writeOutput_(delayed, planarOut, f);  // output direction (single primitive)."""),
        ]},
        "expect": "theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance",
        "claim": "the emitted sequence starts at the product's OWN frame-0 phase (no latency)",
    },
    {
        # THE SYNC CONSUMER, ONE FRAME LATE (director note `df6b7937` item 1): "add an isolated
        # mutation where the ACTUAL sync consumer is one frame late."
        #
        # ISOLATION IS THE POINT. The gate law is NOT touched: `sink_gate_interpret` is still called
        # on every frame with the same latch and the same published value, so the EDGE IS DETECTED
        # on exactly the frame the independent hysteresis reference predicts. Only the DELIVERY of
        # that edge to the oscillator slips by one frame — the request is parked in a member and
        # issued at the top of the next frame (a member, not a local, so it survives the frame
        # boundary: `MachineRuntime::processBlock` is entered once per frame on the product path).
        #
        # This is the mutant the OLD revision of the sync check could not see. That revision located
        # the reset by scanning the OUTPUT for the first divergence and compared the source against
        # the bare 5 V threshold, so a one-frame-late delivery would have moved the divergence and
        # the scan would simply have followed it. The rewritten check PREDICTS the frame from the
        # source before the output is consulted, so the reset is required to land on the predicted
        # frame and this arm is rejected by name.
        "id": "sync-consumer-one-frame-late",
        "target": ACCEPT,
        "edits": {MACHINE_RT: [
            ("""        double sv = 0.0;
        if (syncInBoundA_ && resolveControlSink_(syncInA_, sv, driveGraph)) {
          const JackDescriptor* ds = findJackDescriptor_(syncInA_);
          if (ds != nullptr &&
              sink_gate_interpret(*ds, syncLatchA_, sv).edge == GateEdge::rising) {
            vcA_.requestSync();
          }
        }""",
             """        const bool lateSyncEdgeA = syncPendingA_;  // MUTANT: deliver the PREVIOUS frame's edge
        syncPendingA_ = false;
        double sv = 0.0;
        if (syncInBoundA_ && resolveControlSink_(syncInA_, sv, driveGraph)) {
          const JackDescriptor* ds = findJackDescriptor_(syncInA_);
          if (ds != nullptr &&
              sink_gate_interpret(*ds, syncLatchA_, sv).edge == GateEdge::rising) {
            syncPendingA_ = true;  // MUTANT: DETECTED here, DELIVERED one frame later
          }
        }
        if (lateSyncEdgeA) vcA_.requestSync();"""),
            ("  GateClockSinkState syncLatchA_;",
             "  GateClockSinkState syncLatchA_;\n"
             "  bool syncPendingA_ = false;  // MUTANT: one frame of sync delivery latency"),
        ]},
        "expect": "theResetLandsOnTheFrameTheHysteresisReferencePredicted",
        "claim": "the sync edge is CONSUMED on the frame the hysteresis law raises it, not one later",
    },
]


def compile_cmd(root, target, shadow_inc, out, compiler):
    """The shadow include root MUST come first, so a mutated header wins over the real tree while
    every untouched header still resolves from `core/include`/`generated`."""
    spec = TARGETS[target]
    cmd = [compiler, "-O3", "-DNDEBUG", "-std=c++17"]
    if sys.platform == "darwin":
        cmd += ["-arch", platform.machine() or "arm64"]
    if shadow_inc:
        cmd += ["-I", shadow_inc]
    cmd += ["-I", os.path.join(root, "core/include"), "-I", os.path.join(root, "generated")]
    for rel in spec.get("inc", []):
        cmd += ["-I", os.path.join(root, rel)]
    cmd += [os.path.join(root, s) for s in spec["sources"]]
    cmd += ["-o", out]
    return cmd


def build(root, target, shadow_inc, out, compiler):
    return subprocess.run(compile_cmd(root, target, shadow_inc, out, compiler),
                          capture_output=True, text=True)


def run(out):
    return subprocess.run([out], capture_output=True, text=True)


def dump(dump_dir, tag, r):
    """Keep every arm's raw output when asked: a mutant that is graded INVALID has to be
    diagnosable WITHOUT re-running the whole matrix."""
    if not dump_dir:
        return
    os.makedirs(dump_dir, exist_ok=True)
    with open_text(os.path.join(dump_dir, tag + ".txt"), "w") as fh:
        fh.write("$ rc=%d\n" % r.returncode)
        fh.write("--- stdout ---\n" + (r.stdout or ""))
        fh.write("--- stderr ---\n" + (r.stderr or ""))


def grade(target, r, expect=None, require_zero=False):
    """Return (verdict, detail). Separates the two failure surfaces: a run that did not complete
    normally is INVALID, never a red; only a complete run can be GREEN or RED."""
    spec = TARGETS[target]
    if r.returncode < 0 or r.returncode >= 128:
        return "INVALID", "abnormal exit rc=%d (signal/abort) — not a red" % r.returncode
    combined = (r.stdout or "") + (r.stderr or "")
    ok = spec["summary_ok"].search(combined)
    red = spec["summary_red"].search(combined)
    if require_zero:
        if r.returncode != 0 or not ok:
            return "INVALID", ("reference did not complete cleanly: rc=%d summary=%r" %
                               (r.returncode, (ok or red).group(0) if (ok or red) else None))
        return "GREEN", (ok.group(0),)
    # mutant arm
    if r.returncode != 1:
        return "INVALID", "expected a normal failure exit 1, got rc=%d" % r.returncode
    if not red:
        return "INVALID", "no complete summary line — the run was truncated or crashed mid-suite"
    names = [m.group("expr").strip() for m in spec["named"].finditer(combined)]
    if not names:
        return "INVALID", "summary present (%s) but NO named failing check" % red.group(0)
    hit = [n for n in names if expect in n]
    if not hit:
        # Print ALL named failures (bounded): a 4-name sample hides whether the targeted check is
        # merely absent or the run died before reaching it, and that distinction is the whole
        # diagnosis.
        shown = names if len(names) <= 12 else (names[:12] + ["... +%d more" % (len(names) - 12)])
        return "INVALID", ("failed on a DIFFERENT check than this mutant targets; want %r, got %s" %
                           (expect, shown))
    return "RED", (red.group(0), hit[0], len(names))


def apply_edits(root, edits, shadow_root):
    """Copy each touched header into the shadow tree with the mutant edits applied.

    The shadow tree mirrors `core/include`, i.e. it is the INCLUDE ROOT (so `-I <shadow>` shadows
    `<lunar24/...>` and every untouched header still resolves from the real tree). Every replacement
    is asserted to land exactly once, so a silently-stale mutation cannot be graded as a red."""
    for rel, subs in edits.items():
        src = os.path.join(root, rel)
        with open_text(src) as fh:
            text = fh.read()
        for old, new in subs:
            if text.count(old) != 1:
                raise RuntimeError("mutation anchor found %d times (want 1) in %s:\n%r" %
                                   (text.count(old), rel, old[:160]))
            text = text.replace(old, new)
        inner = rel[len("core/include/"):] if rel.startswith("core/include/") else rel
        dst = os.path.join(shadow_root, inner)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open_text(dst, "w") as fh:
            fh.write(text)
    return shadow_root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="repo root")
    ap.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--only", default="", help="comma-separated mutant ids")
    ap.add_argument("--dump-dir", default="", help="write each arm's stdout/stderr here")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    want = [m for m in MUTANTS if not args.only or m["id"] in args.only.split(",")]
    if args.only:
        unknown = set(args.only.split(",")) - {m["id"] for m in MUTANTS}
        if unknown:
            print("unknown mutant id(s): %s" % ", ".join(sorted(unknown)))
            return 1

    td = tempfile.mkdtemp(prefix="gh19-s3-mut-")
    invalid, rows = [], []
    try:
        # ---------------- reference arm: must be rc 0 + complete summary + 0 failures.
        for target in sorted({m["target"] for m in want}):
            out = os.path.join(td, "bin_ref_" + target)
            b = build(root, target, None, out, args.compiler)
            if b.returncode != 0:
                print("[REFERENCE %s] BUILD FAIL — aborting; the real product must build" % target)
                print((b.stderr or "").strip()[-800:])
                return 1
            r = run(out)
            dump(args.dump_dir, "reference_" + target, r)
            verdict, detail = grade(target, r, require_zero=True)
            print("[REFERENCE %-40s] %s  %s" % (target, verdict, detail))
            if verdict != "GREEN":
                invalid.append("reference %s not clean: %s" % (target, detail))

        # ---------------- mutant arms.
        for m in want:
            tag = m["id"]
            if not m["edits"]:
                print("[MUTANT %-26s] INVALID  no mutation defined" % tag)
                invalid.append("%s: mutation not defined" % tag)
                continue
            shadow = os.path.join(td, "inc_" + tag)
            os.makedirs(shadow, exist_ok=True)
            try:
                apply_edits(root, m["edits"], shadow)
            except RuntimeError as exc:
                print("[MUTANT %-26s] INVALID  %s" % (tag, exc))
                invalid.append("%s: %s" % (tag, exc))
                continue
            out = os.path.join(td, "bin_" + tag)
            b = build(root, m["target"], shadow, out, args.compiler)
            if b.returncode != 0:
                tail = (b.stderr or "").strip().splitlines()[-1:] or [""]
                print("[MUTANT %-26s] INVALID  BUILD FAIL — a compile error is not a red (%s)" %
                      (tag, tail[0][:200]))
                invalid.append("%s: build failure (not counted as red)" % tag)
                continue
            r = run(out)
            dump(args.dump_dir, tag, r)
            verdict, detail = grade(m["target"], r, expect=m["expect"])
            print("[MUTANT %-26s] %-7s %s" % (tag, verdict, detail))
            if verdict == "RED":
                rows.append((tag, m["target"], detail[0], detail[1], detail[2]))
            else:
                invalid.append("%s: %s" % (tag, detail))
            if args.verbose:
                print("      claim: %s" % m["claim"])
    finally:
        shutil.rmtree(td, ignore_errors=True)

    print()
    print("%-26s %-24s %s" % ("mutant", "summary", "named failing check"))
    for tag, target, summary, name, n in rows:
        print("%-26s %-24s %s  (%s)" % (tag, summary, name, "1 of %d failures" % n))
    print()
    if invalid:
        print("MUTANTS INVALID (%d):" % len(invalid))
        for x in invalid:
            print("  - %s" % x)
        return 1
    print("MUTANTS PASS: the reference completed cleanly (rc 0, complete summary, 0 failures) and "
          "all %d production mutations were rejected, each by its OWN named check." % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
