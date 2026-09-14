#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s0_mutants.py — task #117 (GH #19 S0) ISOLATED PRODUCTION MUTANTS.
#
# The S0 slice claims that one continuous morph knob drives the VCO waveform, that the two PWM CV
# jacks are consumed per sample by the real product, and that depth 0 is strictly inert. A test that
# passes on the shipped source proves nothing about whether it DISCRIMINATES. So each claim is
# re-run against a source mutated away from it, in an ISOLATED shadow include tree: the committed
# tree is never written to.
#
# Each mutant is ONE focused edit to a production header, chosen so the mutant remains a
# well-formed, buildable program (a mutant that does not compile proves nothing about the test):
#
#   fixed-old-triangle      vco.h        wave_ defaults to the pre-#117 raw kTriangle, so the morph
#                                        knob reaches no ring node and the default is no longer sine.
#   wrong-side              vco_wave_map.h saw/inverted-saw node generators swapped: at the same
#                                        coordinate the ring swaps two OPPOSITE-polarity shapes.
#   mapping-jump            vco_wave_map.h sampleAt snaps to the nearest node instead of blending:
#                                        the ring becomes a discrete 5-way switch (a mapping jump).
#   missing-pwm-consumer    machine_definition.h  the two PWM jacks are left unbound, so the graph
#                                        CV never reaches the VCO. Every consumer claim must go red.
#   depth0-still-modulating vco.h        effectiveDuty() drops the depth factor, so depth 0 still
#                                        modulates (the classic "knob does nothing but the cable
#                                        still works" defect).
#   cv-one-sample-late      machine_runtime.h    the consumer reads the PREVIOUS frame's resolved CV
#                                        instead of this frame's: a one-sample latency that a
#                                        constant-CV test cannot see.
#   restore-skips-the-setter machine_runtime.h    the state-apply route (applyDspState, reached only
#                                        from the MachineRuntimeDefinition state ctor) stops calling
#                                        the PWM setter, so a restore silently loses the PWM depth
#                                        while the live lane is untouched.
#
# The five `acc-` mutants below re-grade five of those SAME production mutations against the step-5
# END-TO-END surface (tests/host/test_gh19_s0_morph_pwm_acceptance.cpp: codec -> validate ->
# owner.applyDeviceState -> DeviceAdapter/processBlock). That suite is only evidence if it can fail,
# so its load-bearing claims each get their own red rather than being counted as green coverage.
#
# WHAT COUNTS AS RED (the point of this runner — a compile error is NOT a red):
#   * the REFERENCE (unmutated) arm must exit 0 AND print its COMPLETE summary AND report 0 failures;
#   * each MUTANT arm must exit with an expected NORMAL failure status (1), AND print its complete
#     summary, AND name the SPECIFIC check this mutant is supposed to trip.
#   A build failure, a signal/abort exit (>=128 or negative), a zero exit, a truncated/absent
#   summary, or a failure on some OTHER check is reported as an INVALID mutant, not as a red.
#   This is the gap the task-116 runner had: it derived its verdict from grepping a PASS banner and
#   never pinned the positive arm's return code, so a crashed run and a judged red were
#   indistinguishable. Both failure surfaces are named separately here.
#
# Reproduction (from the repo root):
#   python3 tools/run_gh19_s0_mutants.py
#   python3 tools/run_gh19_s0_mutants.py --verbose
#   python3 tools/run_gh19_s0_mutants.py --only fixed-old-triangle,mapping-jump

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

# ---- test targets (mirrors CMakeLists: both are header-only consumers, no libraries to link) ----
TARGETS = {
    "test_vco": {
        "sources": ["tests/core/test_vco.cpp"],
        "suite": "vco",
        # mini_test.h: success -> stdout "[<suite>] N checks OK" rc 0
        #              failure -> stderr "[<suite>] F/N checks FAILED" rc 1, plus "FAIL file:line: expr"
        "summary_ok": re.compile(r"^\[vco\] \d+ checks OK$", re.M),
        "summary_red": re.compile(r"^\[vco\] \d+/\d+ checks FAILED$", re.M),
        "named": re.compile(r"^FAIL .*:\d+: (?P<expr>.*)$", re.M),
    },
    "test_machine_definition": {
        "sources": ["tests/core/test_machine_definition.cpp",
                    "tests/core/test_machine_definition_allocator.cpp"],
        "suite": "machine_definition",
        # local harness: success -> stdout "[PASS] N checks, 0 failed" rc 0
        #                failure -> stdout "[FAIL] N checks, M failed" rc 1, plus "FAIL: <label>"
        "summary_ok": re.compile(r"^\[PASS\] \d+ checks, 0 failed$", re.M),
        "summary_red": re.compile(r"^\[FAIL\] \d+ checks, \d+ failed$", re.M),
        "named": re.compile(r"^FAIL: (?P<expr>.*)$", re.M),
    },
    "test_state_apply_oracle_169": {
        "sources": ["tests/host/test_state_apply_oracle_169.cpp"],
        "suite": "test_state_apply_oracle_169",
        # mini_test.h again (same shape as test_vco), but this TU also needs the host headers.
        "inc": ["tests/host", "tests/core", "host/include"],
        "summary_ok": re.compile(r"^\[test_state_apply_oracle_169\] \d+ checks OK$", re.M),
        "summary_red": re.compile(r"^\[test_state_apply_oracle_169\] \d+/\d+ checks FAILED$", re.M),
        "named": re.compile(r"^FAIL .*:\d+: (?P<expr>.*)$", re.M),
    },
    "test_gh19_s0_morph_pwm_acceptance": {
        "sources": ["tests/host/test_gh19_s0_morph_pwm_acceptance.cpp",
                    "tests/host/test_state_apply_oracle_allocator.cpp"],
        "suite": "test_gh19_s0_morph_pwm_acceptance",
        # mini_test.h again, but this is task#117's step-5 acceptance surface: it needs the host
        # headers (EngineHarness) and it must link the allocator TU, which owns the replaced
        # operator new/delete that its zero-alloc render-window probe counts.
        "inc": ["tests/host", "tests/core", "host/include"],
        "summary_ok": re.compile(r"^\[test_gh19_s0_morph_pwm_acceptance\] \d+ checks OK$", re.M),
        "summary_red": re.compile(r"^\[test_gh19_s0_morph_pwm_acceptance\] \d+/\d+ checks FAILED$",
                                  re.M),
        "named": re.compile(r"^FAIL .*:\d+: (?P<expr>.*)$", re.M),
    },
}

VCO = "core/include/lunar24/core/vco.h"
WAVE_MAP = "core/include/lunar24/core/vco_wave_map.h"
MACHINE_DEF = "core/include/lunar24/core/machine_definition.h"
MACHINE_RT = "core/include/lunar24/core/machine_runtime.h"

# Every mutant: (id, target, {file: [(old, new), ...]}, the check expression it must trip).
# `expect` is a substring that must appear in a NAMED failure line — never just "nonzero exit".
MUTANTS = [
    {
        "id": "fixed-old-triangle",
        "target": "test_vco",
        "edits": {VCO: [("  VcoWaveform wave_ = VcoWaveform::kMorphRing;",
                         "  VcoWaveform wave_ = VcoWaveform::kTriangle;")]},
        "expect": "v.waveform() == core::VcoWaveform::kMorphRing",
        "claim": "the render law is the continuous morph ring, not the pre-#117 raw triangle",
    },
    {
        # The node-generator swap. NOTE: this mutant is NOT caught by the ring's node-ROUTING table
        # (test_vco (1c)) — that table uses the same header for both sides, so a swap of two node
        # FORMULAS cancels there (measured: (1c) stays green). It is caught by (1e), the independent
        # closed-form stretch check, which is why (1e) exists.
        "id": "wrong-side",
        "target": "test_vco",
        "edits": {WAVE_MAP: [("inline double sawShape(double p) { return 2.0 * p - 1.0; }",
                              "inline double sawShape(double p) { return -(2.0 * p - 1.0); }"),
                             ("inline double invSawShape(double p) { return -(2.0 * p - 1.0); }",
                              "inline double invSawShape(double p) { return 2.0 * p - 1.0; }")]},
        "expect": "worstSaw < 1e-15",
        "claim": "each ring node keeps the shape formula it is documented to have",
    },
    {
        "id": "mapping-jump",
        "target": "test_vco",
        "edits": {WAVE_MAP: [("  return (1.0 - pos.u) * a + pos.u * b;",
                              "  return (pos.u < 0.5) ? a : b;")]},
        "expect": "maxStep < 0.05",
        "claim": "morph is continuous across 0..1 (no discrete mapping jump)",
    },
    {
        "id": "missing-pwm-consumer",
        "target": "test_machine_definition",
        "edits": {MACHINE_DEF: [("""    runtime_.setVcoPwmBindings(lunar24::registry::JackId::vco_a_pwm_in,
                               lunar24::registry::JackId::vco_b_pwm_in);
""", "")]},
        "expect": "consumes the source's published value",
        "claim": "a patched PWM jack reaches its own VCO through the real graph",
    },
    {
        "id": "wrong-side-cable",
        "target": "test_machine_definition",
        "edits": {MACHINE_RT: [
            ("""        double pwm = 0.0;
        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInB_, pwm, driveGraph));
        vcB_.setPwCv(pwm);""",
             """        double pwm = 0.0;
        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        vcB_.setPwCv(pwm);"""),
        ]},
        "expect": "reads B's OWN source, not A's",
        "claim": "A's cable reaches A's VCO only — the two PWM jacks are independent",
    },
    {
        "id": "depth0-still-modulating",
        "target": "test_vco",
        "edits": {VCO: [("const double d = duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts;",
                         "const double d = duty_ + pwCv_ / kPwmCvFullScaleVolts;")]},
        "expect": "v.effectiveDuty() == canonical",
        "claim": "PWM depth 0 leaves the emitted samples untouched",
    },
    {
        # Both sides are latched here, so the mutant is rejected by whichever side's edge probe
        # runs first. Both A and B are probed in the witness precisely because their drivers are
        # different kinds of source (a product knob vs an injected DC).
        "id": "cv-one-sample-late",
        "target": "test_machine_definition",
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
        "expect": "same_frame_edge",
        "claim": "the graph CV is consumed in the SAME frame it is published",
    },
    {
        # applyDspState is reached ONLY from the MachineRuntimeDefinition state ctor, i.e. the
        # state-apply / restore route, so skipping the pwm ids HERE is a RESTORE-ONLY mutation: the
        # live lane's applyDspParam path (and its smoothing) is left intact. This is the claim that
        # the pwm disposition flip is only real if a restore re-reaches the SETTER, not merely if a
        # byte survives somewhere in the state.
        "id": "restore-skips-the-setter",
        "target": "test_state_apply_oracle_169",
        # NOTE the FORM. A bare `continue` for the two ids is caught by the whole-state count gate
        # (the production comment names that exact stray-continue case), so it REJECTS the
        # candidate — and a rejected candidate makes the per-item oracle dereference a null
        # definition and die on SIGSEGV. Measured: rc=-11, INVALID, and a crash would say nothing
        # about whether the restore surface is guarded. So this mutation keeps the accounting
        # intact (the id is still admitted, the candidate is still accepted) and skips only the
        # SETTER call on this route.
        "edits": {MACHINE_RT: [(
            "      const ParameterApplyStatus s = applyDspParam(id, v);",
            """      const ParameterApplyStatus s =
          (id == ParameterId::vco_a_pwm || id == ParameterId::vco_b_pwm)
              ? ParameterApplyStatus::applied
              : applyDspParam(id, v);""")]},
        "expect": "restore_keeps_pwm_depth",
        "claim": "a state restore re-reaches the PWM depth setter",
    },
    # ---- the ACCEPTANCE surface must discriminate too --------------------------------------------
    # Same five production mutations as above, graded against tests/host/test_gh19_s0_morph_pwm_
    # acceptance.cpp instead. `expect` here names the bool local the suite uses, because mini_test.h
    # stringifies the CHECK expression and this suite deliberately checks well-named locals.
    {
        "id": "acc-mapping-jump",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {WAVE_MAP: [("  return (1.0 - pos.u) * a + pos.u * b;",
                              "  return (pos.u < 0.5) ? a : b;")]},
        "expect": "morphSweepIsContinuous44k1",
        "claim": "the acceptance surface sees the morph ring as CONTINUOUS, not a 5-way switch",
    },
    {
        "id": "acc-missing-pwm-consumer",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_DEF: [("""    runtime_.setVcoPwmBindings(lunar24::registry::JackId::vco_a_pwm_in,
                               lunar24::registry::JackId::vco_b_pwm_in);
""", "")]},
        "expect": "pwmCvIsConsumedInTheSameFrameItIsPublished",
        "claim": "the acceptance surface requires the graph CV to actually reach the VCO",
    },
    {
        "id": "acc-cv-one-sample-late",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_RT: [
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
        "expect": "pwmCvIsConsumedInTheSameFrameItIsPublished",
        "claim": "the acceptance surface's same-frame check is a real discriminator (not a residual)",
    },
    {
        "id": "acc-depth0-still-modulating",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {VCO: [("const double d = duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts;",
                         "const double d = duty_ + pwCv_ / kPwmCvFullScaleVolts;")]},
        "expect": "depthZeroIsStrictlyInert",
        "claim": "the acceptance surface's depth-0 inertness claim is a real discriminator",
    },
    {
        "id": "acc-restore-skips-the-setter",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_RT: [(
            "      const ParameterApplyStatus s = applyDspParam(id, v);",
            """      const ParameterApplyStatus s =
          (id == ParameterId::vco_a_pwm || id == ParameterId::vco_b_pwm)
              ? ParameterApplyStatus::applied
              : applyDspParam(id, v);""")]},
        "expect": "repeatedRestoreReachesThePwmDepthSetter",
        "claim": "the acceptance surface's repeated-restore claim re-reaches the PWM setter",
    },
    {
        # ENDPOINT CONTROL 1 (director note ce80e516): the sink NEVER CONSUMES the PWM CV, so the
        # duty stays at the unmodulated base width. At the clamp endpoints the emitted waveform is
        # then the base-0.5 waveform while the independent reference predicts the 0.001 waveform, so
        # the NEW endpoint criterion is what has to fire — not the same-frame check, which this edit
        # also trips. The binding removal is the same edit as `acc-missing-pwm-consumer` above but it
        # is graded on the endpoint assertion, because that is the criterion the ruling asked to see
        # carry this class of defect.
        "id": "acc-endpoint-pwm-not-consumed",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_DEF: [("""    runtime_.setVcoPwmBindings(lunar24::registry::JackId::vco_a_pwm_in,
                               lunar24::registry::JackId::vco_b_pwm_in);
""", "")]},
        "expect": "negativeCvSaturatesAtDutyMin",
        "claim": "the endpoint criterion rejects a sink that never consumes the PWM CV",
    },
    {
        # ENDPOINT CONTROL 2 — WRONG SIDE / INVERTED POLARITY. The sink consumes the source every
        # frame (so the same-frame check stays green) but with the polarity flipped, which is the
        # wrong-side failure in this family: positive CV LOWERS the duty. At the -5 V endpoint the
        # emitted waveform is then the 0.999 waveform while the independent reference predicts the
        # 0.001 one, so the endpoint criterion must reject it on its own.
        "id": "acc-endpoint-inverted-pwm-polarity",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_RT: [("""        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        vcA_.setPwCv(pwm);""",
                               """        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        vcA_.setPwCv(-pwm);  // MUTANT: wrong side / inverted PWM polarity""")]},
        "expect": "negativeCvSaturatesAtDutyMin",
        "claim": "the endpoint criterion rejects a wrong-side (inverted-polarity) consumer",
    },
    {
        # ENDPOINT CONTROL 3 — ONE FRAME LATE. The sink consumes the PREVIOUS frame's CV. Where the
        # published CV is moving this is visible; at a settled DC endpoint it is not, and that is a
        # property of the stimulus, not of the criterion — so this arm is graded where the lag can
        # actually be seen, on the changing-CV same-frame criterion, and the endpoint criterion is
        # additionally required to reject it if the lag ever shows up in the endpoint window.
        "id": "acc-endpoint-cv-one-sample-late",
        "target": "test_gh19_s0_morph_pwm_acceptance",
        "edits": {MACHINE_RT: [
            ("""        double pwm = 0.0;
        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        vcA_.setPwCv(pwm);""",
             """        const double prevA = pwmLatchedA_;
        double pwm = 0.0;
        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));
        pwmLatchedA_ = pwm;
        vcA_.setPwCv(prevA);"""),
            ("  JackId pwmInA_{0};",
             "  JackId pwmInA_{0}; double pwmLatchedA_ = 0.0;"),
        ]},
        "expect": "pwmCvIsConsumedInTheSameFrameItIsPublished",
        "claim": "the endpoint/consumption surface rejects a one-frame-late PWM consumer",
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
        return "INVALID", ("failed on a DIFFERENT check than this mutant targets; want %r, got %s" %
                           (expect, names[:4]))
    return "RED", (red.group(0), hit[0], len(names))


def apply_edits(root, edits, shadow_root):
    """Copy each touched header into the shadow tree with the mutant edits applied.

    The shadow tree mirrors `core/include`, i.e. it is the INCLUDE ROOT (so `-I <shadow>` shadows
    `<lunar24/...>` and every untouched header still resolves from the real tree). Every replacement
    is asserted to land exactly once, so a silently-stale mutation cannot be graded as a red."""
    for rel, subs in edits.items():
        src = os.path.join(root, rel)
        with open(src) as fh:
            text = fh.read()
        for old, new in subs:
            if text.count(old) != 1:
                raise RuntimeError("mutation anchor found %d times (want 1) in %s:\n%r" %
                                   (text.count(old), rel, old[:160]))
            text = text.replace(old, new)
        inner = rel[len("core/include/"):] if rel.startswith("core/include/") else rel
        dst = os.path.join(shadow_root, inner)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as fh:
            fh.write(text)
    return shadow_root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="repo root")
    ap.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--only", default="", help="comma-separated mutant ids")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    want = [m for m in MUTANTS if not args.only or m["id"] in args.only.split(",")]
    if args.only:
        unknown = set(args.only.split(",")) - {m["id"] for m in MUTANTS}
        if unknown:
            print("unknown mutant id(s): %s" % ", ".join(sorted(unknown)))
            return 1

    td = tempfile.mkdtemp(prefix="gh19-s0-mut-")
    invalid, rows = [], []
    try:
        # ---------------- reference arm, per target actually exercised: must be rc 0 + 0 fail.
        ref_bins = {}
        for target in sorted({m["target"] for m in want}):
            out = os.path.join(td, "bin_ref_" + target)
            b = build(root, target, None, out, args.compiler)
            if b.returncode != 0:
                print("[REFERENCE %s] BUILD FAIL — aborting; the real product must build" % target)
                print((b.stderr or "").strip()[-800:])
                return 1
            r = run(out)
            verdict, detail = grade(target, r, require_zero=True)
            print("[REFERENCE %-22s] %s  %s" % (target, verdict, detail if verdict == "GREEN"
                                                else detail))
            if verdict != "GREEN":
                invalid.append("reference %s not clean: %s" % (target, detail))
            ref_bins[target] = out

        # ---------------- mutant arms.
        for m in want:
            tag = m["id"]
            if not m["edits"]:
                print("[MUTANT %-24s] INVALID  no mutation defined (restore path not yet wired)" % tag)
                invalid.append("%s: mutation not defined" % tag)
                continue
            shadow = os.path.join(td, "inc_" + tag)
            os.makedirs(shadow, exist_ok=True)
            try:
                apply_edits(root, m["edits"], shadow)
            except RuntimeError as exc:
                print("[MUTANT %-24s] INVALID  %s" % (tag, exc))
                invalid.append("%s: %s" % (tag, exc))
                continue
            out = os.path.join(td, "bin_" + tag)
            b = build(root, m["target"], shadow, out, args.compiler)
            if b.returncode != 0:
                tail = (b.stderr or "").strip().splitlines()[-1:] or [""]
                print("[MUTANT %-24s] INVALID  BUILD FAIL — a compile error is not a red (%s)" %
                      (tag, tail[0][:200]))
                invalid.append("%s: build failure (not counted as red)" % tag)
                continue
            r = run(out)
            verdict, detail = grade(m["target"], r, expect=m["expect"])
            print("[MUTANT %-24s] %-7s %s" % (tag, verdict,
                                              detail if verdict == "RED" else detail))
            if verdict == "RED":
                rows.append((tag, m["target"], detail[0], detail[1], detail[2]))
            else:
                invalid.append("%s: %s" % (tag, detail))
            if args.verbose:
                print("      claim: %s" % m["claim"])
    finally:
        shutil.rmtree(td, ignore_errors=True)

    print()
    print("%-24s %-24s %s" % ("mutant", "target", "named failing check"))
    for tag, target, summary, name, n in rows:
        print("%-24s %-24s %-8s %s  (%s)" % (tag, target, summary, name, "1 of %d failures" % n))
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
