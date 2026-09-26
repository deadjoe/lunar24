#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s3_negatives.py — task #118 (GH #19 S3): the DIRECTED, ISOLATED SOURCE NEGATIVE CONTROLS.
#
# WHAT THIS IS FOR. A measurement that reports an improvement has to be shown to be capable of
# reporting the absence of one. This script takes the candidate's real production headers, breaks
# exactly one thing in them, rebuilds the real probe against the broken source, and requires the
# named criterion for that break to FAIL. If a break that must destroy the improvement does not, the
# improvement was not coming from the code under test.
#
# WHERE THE MUTATION HAPPENS. Never in the candidate worktree. The candidate's three headers are
# COPIED into a scratch worktree (the pinned baseline tree, which is not the candidate's tree and
# holds no other work), patched there, built there, and reverted there. The candidate tree's own
# files are hashed before and after and the run fails if they moved at all. Mutating the tree you are
# measuring from and restoring it afterwards is one crash away from leaving the measurement self-
# consistent and wrong.
#
# WHAT COUNTS AS RED. Per the #116 ruling carried into #117: a mutation is RED only when the tree
# BUILDS, the probe EXITS 0, and the NAMED CRITERION for that mutation fails. A compile error is not
# a red control, it is an INVALID one — it proves the toolchain reacted, not that the measurement
# sees the defect. Same for a probe that dies, or a criterion that could not be evaluated because a
# cell is missing. Those are reported as INVALID and the run fails; they are never counted as hits.
#
# THE FOUR MUTATIONS, and why each is the one that tests what it tests:
#
#   bypass          pulseBlepWeight_() returns 0.0, i.e. the correction is computed nowhere. This is
#                   the strongest control available: the mutated arm must become INDISTINGUISHABLE
#                   FROM THE BASELINE on every cell. It separates "the correction caused the measured
#                   improvement" from "something else in this arm caused it".
#   wrong_sign      the B edge's residual is ADDED instead of subtracted. A pulse is not a phase-
#                   shifted saw, and the two edges' signs are the whole content of that difference.
#                   The arm must be WORSE THAN THE BASELINE, not merely less improved.
#   wrong_position  the B edge is corrected at phase (1 - duty) instead of duty. Tests the POSITION
#                   of the moving edge specifically, which the static phase formula cannot establish:
#                   with duty = 0.5 the two positions coincide, so this mutation is invisible on every
#                   pw50 cell and can only be caught where duty != 0.5.
#   pwm_late        the PWM CV is consumed ONE FRAME LATE, at the runtime site where the product
#                   actually consumes it (`machine_runtime.h`: vco_a.pwm_in -> Vco::setPwCv, and B's
#                   mirror). This is the control the static matrix structurally cannot see: with a
#                   stationary duty the stale value equals the current one, so static cells must be
#                   UNCHANGED while moving-duty cells must RESPOND. A criterion that only looked at
#                   static cells would call this mutation green, which is exactly the blind spot the
#                   dynamic matrix exists to close.
#
#                   WHY IT IS PATCHED AT THE RUNTIME SITE AND NOT IN THE KERNEL (@Codex e144b61
#                   review, item 4). The previous revision of this control delayed the duty the
#                   CORRECTION reads (`vco.h` pulseBlepCorr_) while the NAIVE pulse went on reading
#                   the current value. That is not a product timing error: it is a formula that
#                   disagrees with itself, and the arm's movement could be produced by the
#                   inconsistency alone. It also advanced its state once per CALL of the correction
#                   rather than once per frame, and one frame may call it more than once. Delaying at
#                   the consumption site makes every downstream reader -- naive pulse, correction,
#                   ring-mix pulse node -- see the same stale duty, which is what "the CV is consumed
#                   one frame late" means.
#
#                   AND IT IS JUDGED AGAINST A PRODUCT ASSERTION, NOT A SENSITIVITY NUMBER. A
#                   residual that "moved" is a sensitivity experiment; it cannot stand in for the
#                   product's own same-frame reconciliation. The criterion below therefore requires
#                   the CANDIDATE to pass `align_check` (product output == the model sequence rebuilt
#                   from the recorded source frames, unexplained == 0) and the MUTANT to FAIL it with
#                   a NAMED first frame on the stepping-duty cells. That is green-then-red on one
#                   assertion, on the independent source frames, exactly as asked.
#
#                   THE FIRST PREDICTION OF THIS CONTROL WAS FALSIFIED BY MEASUREMENT, and the
#                   criterion was restated rather than tuned. It originally required every moving-duty
#                   cell to get WORSE. Measured: the movement splits by LFO rate -- slow-LFO cells
#                   worse by 0.20..0.25 dB, fast-LFO cells BETTER by 0.24..1.59 dB. So the residual
#                   half of the criterion asserts only what is falsifiable and true (bit-inert on
#                   static, not blind on moving) and the sign split is printed as a finding. See the
#                   long comment in judge().

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gh19_s3_pulse_analyze as A  # noqa: E402
from _gh19_textio import read_text, write_text

HDR_VCO = "core/include/lunar24/core/vco.h"
HDR_MAP = "core/include/lunar24/core/vco_wave_map.h"
HDR_KERNEL = "core/include/lunar24/core/pulse_blep_kernel.h"
# The PWM CV is CONSUMED here (vco_a.pwm_in / vco_b.pwm_in -> Vco::setPwCv), once per frame per VCO,
# through the one control-sink resolver the other CV consumers use. The `pwm_late` control has to
# patch THIS file rather than the kernel: patching the kernel's correction argument only made the
# correction read a stale duty while the naive pulse still read the current one, which is not a
# product timing error at all but a self-inconsistent formula -- and a per-function-call counter can
# be advanced several times by one `sync` frame, so it did not even model "one frame". See the
# pwm_late entry in MUTATIONS and the criterion comment in judge().
HDR_RUNTIME = "core/include/lunar24/core/machine_runtime.h"
# The probe is copied too, and for the same reason the headers are: it is candidate code that the
# scratch tree does not have in this revision. A stale copy would not see --peak-hi and would abort
# every wrong-edge arm as `over-scale`, i.e. the control would silently fail closed. It is NOT
# reverted after each mutation: the probe is arm-independent, so leaving it in place is correct.
PROBE = "tests/probes/gh19_s3_pulse_probe.cpp"

# ---- the declared criterion cell set. Small ON PURPOSE and declared rather than sampled: these
# negative controls answer "did the break move the number in the predicted direction", and a
# comparative criterion between two arms measured with the SAME reference cannot be strengthened by
# adding cells, only made slower. The absolute dB from this set is never quoted -- only the sign and
# rough size of the movement between arms.
STATIC_CELLS = [
    "vco_a_pulse_44100_220_pw50",
    "vco_a_pulse_44100_440_pw50",
    "vco_a_pulse_44100_880_pw50",
    "vco_a_pulse_44100_220_pw10",
    "vco_a_pulse_44100_440_pw10",
    "vco_a_pulse_44100_880_pw90",
    "vco_b_pulse_44100_220_pw50",
    "vco_a_pulsehi_44100_3520_pw50",
]
# duty != 0.5. ONLY these can see a wrong B-edge position: at duty = 0.5 the correct position `duty`
# and the wrong one `1 - duty` are the same number, so the mutation is inert there BY CONSTRUCTION.
# That asymmetry is not an inconvenience, it is the sharpest available statement of what the position
# mutation tests: the criterion requires the duty != 0.5 cells to worsen AND the duty = 0.5 cells to
# be untouched, which cannot both hold unless the edge really is being corrected at `duty`.
ASYMMETRIC_CELLS = [
    "vco_a_pulse_44100_220_pw10",
    "vco_a_pulse_44100_440_pw10",
    "vco_a_pulse_44100_880_pw90",
]
# The smooth-PWM cells that carry the S3 improvement claim itself: triangle LFO, cable written into
# the DeviceState, rendered through encode -> decode -> owner restore. They are NOT the pwm_late
# criterion set (see SLEW_MIN) -- they are the boundary that criterion has to be stated against.
DYNAMIC_CELLS = [
    "vco_a_dynpwm_44100_220_tri_r2_b50_d50",
    "vco_a_dynpwm_44100_220_tri_r20_b50_d50",
    "vco_a_dynpwm_44100_220_tri_r2_b25_d50",
]
RINGMIX_CELLS = [
    "vco_a_ringmix_44100_440_pw050_m87500",
    "vco_a_ringmix_44100_440_pw050_m93750",
]

MUTATIONS = [
    {
        "name": "bypass",
        "file": HDR_VCO,
        "why": "the correction is computed nowhere; the arm must equal the baseline exactly",
        "edits": [(
            "      case VcoWaveform::kMorphRing:\n"
            "        return wave_map::pulseWeight(wave_map::kRingEqual, morph_);",
            "      case VcoWaveform::kMorphRing:\n"
            "        return 0.0;  // NEGCTL bypass",
        )],
        "criterion": "bypass_must_be_indistinguishable_from_baseline",
    },
    {
        "name": "wrong_sign",
        "file": HDR_KERNEL,
        "why": "the -2 B edge is added instead of subtracted",
        # THE ANCHOR SPELLS OUT THE CAPPED-WIDTH FORM (`w`, not `dt`). It has to: revision 4 rewrote
        # this return to hand the residual the capped width, and a control still looking for the old
        # `dt` text anchors zero times. That is caught (the apply loop requires exactly one
        # occurrence) but it is caught LATE -- after a full build-and-run, as an INVALID arm a reader
        # has to diagnose. `declaration_preflight` below now checks every anchor against the real
        # sources before anything is built, so this class of drift is reported as a declaration
        # problem instead of as a broken measurement.
        "edits": [(
            "  return polyblepResidual(t, w) - polyblepResidual(b, w);",
            "  return polyblepResidual(t, w) + polyblepResidual(b, w);  // NEGCTL wrong sign",
        )],
        "criterion": "wrong_sign_must_worsen_vs_baseline",
    },
    {
        "name": "wrong_position",
        "file": HDR_KERNEL,
        "why": "the moving edge is corrected at 1-duty instead of duty",
        "edits": [(
            "  double b = t - duty;",
            "  double b = t - (1.0 - duty);  // NEGCTL wrong edge position",
        )],
        "criterion": "wrong_position_must_worsen_iff_duty_differs_from_half",
    },
    {
        "name": "pwm_late",
        "file": HDR_RUNTIME,
        "why": "the PWM CV is consumed one frame late, at the runtime site where the product consumes it",
        # THREE edits, all in machine_runtime.h: the two consumption sites and the member that holds
        # the delayed sample. The member is a member and not a function-local `static` on purpose --
        # the probe builds a fresh engine per cell, and a header-scope `static` would carry one cell's
        # duty into the next, i.e. the control would leak state between cells and its own verdict
        # would depend on cell order.
        "edits": [
            (
                "  JackId pwmInB_{0};          bool pwmInBoundB_ = false;\n",
                "  JackId pwmInB_{0};          bool pwmInBoundB_ = false;\n"
                "  double negctlPwmPrevA_ = 0.0;  // NEGCTL one-frame-late PWM CV (A)\n"
                "  double negctlPwmPrevB_ = 0.0;  // NEGCTL one-frame-late PWM CV (B)\n",
            ),
            (
                "        double pwm = 0.0;\n"
                "        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));\n"
                "        vcA_.setPwCv(pwm);\n",
                "        double pwm = 0.0;\n"
                "        if (pwmInBoundA_) static_cast<void>(resolveControlSink_(pwmInA_, pwm, driveGraph));\n"
                "        // NEGCTL: hand this frame the PREVIOUS frame's value.\n"
                "        { const double q = negctlPwmPrevA_; negctlPwmPrevA_ = pwm; vcA_.setPwCv(q); }\n",
            ),
            (
                "        double pwm = 0.0;\n"
                "        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInB_, pwm, driveGraph));\n"
                "        vcB_.setPwCv(pwm);\n",
                "        double pwm = 0.0;\n"
                "        if (pwmInBoundB_) static_cast<void>(resolveControlSink_(pwmInB_, pwm, driveGraph));\n"
                "        // NEGCTL: hand this frame the PREVIOUS frame's value.\n"
                "        { const double q = negctlPwmPrevB_; negctlPwmPrevB_ = pwm; vcB_.setPwCv(q); }\n",
            ),
        ],
        "criterion": "late_cv_consume_must_break_the_same_frame_reconciliation_on_stepping_cells_only",
    },
]

TOL = 0.05   # dB: "the same number", well below the ~7.5 dB the correction actually buys
WORSE = 0.50  # dB: "materially large", well above measurement noise
SENS = 0.10  # dB: the instrument's response floor. The static cells move by exactly 0.0000 dB, so
             # anything above this on a moving-duty cell is the instrument responding to the stale
             # read and not to its own noise.

# The cell-validity peak envelope handed to the MUTATED arms only. See the probe's gPeakHi comment:
# a wrong-sign or wrong-position edge correction drives the +-1 step to +-2 in the unit domain, i.e.
# ~1.0 in the device domain, and no bounded formulation of that error exists. The candidate arms are
# always measured at the probe's own default of 0.55.
MUT_PEAK_HI = 4.0

# THE DUTY-SLEW RULE for the `pwm_late` control, and why it is a RULE and not a list of cells.
#
# A one-frame-stale duty read moves edge B by exactly one frame's worth of duty, and one frame's worth
# of duty is only a defect of a knowable size if you say what it is relative to: the correction's own
# support, dt = f0 / sr, which is one window wide. The ratio (one-frame duty movement) / dt is
# therefore the predicted size of the error in units of the thing being corrected, and it is a
# property of the STIMULUS alone -- it is computed from the recorded source trace of the BASELINE arm
# and touches no residual, so choosing cells by it cannot select for the answer.
#
# The measurement separates cleanly and is why this rule exists at all: triangle-LFO cells slew at
# most 0.182 windows/frame (median 0.023), so a stale read moves the edge by ~2% of a window and the
# residual barely reacts -- an honest fact about the smooth-PWM regime the S3 improvement claim rests
# on, and it is REPORTED below rather than asserted away. Square-LFO cells step the duty by up to 0.75
# in a single frame, i.e. hundreds of windows, which is the regime where "one frame late" is
# unambiguously a large, named error. The criterion is declared on the cells where the error is
# predicted to be large; asserting it on the cells where it is predicted to be 2% would be a control
# chosen for its weakness.
SLEW_MIN = 100.0   # correction windows traversed by one frame of duty movement

# THE PREDICTED-DISAGREEMENT RULE for the pwm_late RECONCILIATION half of the criterion, and why it
# is a rule rather than a list of cells.
#
# `align_check` compares the product's output to the naive model rebuilt from the recorded source
# frames, and PERMITS disagreement inside two classes: `wrap` (the model phase within 1e-9 of 0/1,
# where the accumulator and the closed form legitimately differ by one step) and `edge` (within dt of
# an edge, which is where a floor-limited candidate is ALLOWED to differ from the naive sequence and
# therefore where the candidate's own mismatches live). The load-bearing number is `unexplained`.
#
# A one-frame-late duty read can only move that number on a frame where the naive sign under the
# STALE duty differs from the naive sign under the CURRENT duty AND the frame is far enough from
# every edge phase to fall outside the permitted classes. Both halves are properties of the stimulus
# alone -- the recorded source trace plus the phase law `phi(n) = frac((n+1)*f0/sr)` that align_check
# itself uses -- so the set of frames the mutation MUST flag is computable BEFORE the mutant is built,
# from no rendered sample, no residual and no knowledge of the candidate. That is the rule:
#
#     P2(cell) = { n in the reconciled window :
#                    (phi(n) < d[n]) != (phi(n) < d[n-1])
#                    AND min(|phi|, |phi-1|, |phi-d[n]|, |phi-1+d[n]|) >= dt }
#
# and the judged cells are those with P2 non-empty. The criterion is then exact rather than
# statistical: on each judged cell the mutant's first unexplained frame must BE min(P2), i.e. the
# frame the stimulus said would be flagged first.
#
# WHAT THIS RULE DELIBERATELY DOES NOT DO, stated here because it is the honest limit of the control.
# On this grid only 2 of the 13 slew-rule cells have a non-empty P2; on the others the duty steps land
# at phases where the two duties agree on the naive sign, or inside the edge class, so the mutation is
# predicted to leave `unexplained` at zero -- and measured, it does. Those cells are NOT silently
# dropped: their step counts and the mutant's measured verdict are PRINTED below, and the residual
# half of the criterion still covers all 13. The edge class is a declared blind spot and this control
# hides inside it on most of the grid; the report says so rather than implying the assertion covers
# every stepping cell.
PRED_MIN = 1       # predicted unexplained frames required to put a cell under the criterion

# Set in main() from the baseline arm's traces; the pwm_late criterion is declared over it.
LATE_CELLS = []
PRED_FRAMES = {}   # id -> {"steps": [...], "pred": P2, "slew": windows/frame}, from pred_frames()


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def tree_hashes(root, rels):
    return {r: sha256(os.path.join(root, r)) for r in rels if os.path.exists(os.path.join(root, r))}


def residuals(arm, cells_wanted, sr_filter=None):
    """{id: res_db} for the requested cell ids, computed with the SAME reference construction the
    main analyzer uses. Comparison between two arms is therefore common-mode in any reference error,
    which is why the criterion can be a comparative one and still be trustworthy."""
    rows = A.load_manifest(arm)
    by_id = {r["id"]: r for r in rows}
    out = {}
    for cid in cells_wanted:
        if cid not in by_id:
            continue
        c = A.Cell(by_id[cid], arm)
        x = c.window()
        if c.path in ("vco_a_dynpwm", "vco_b_dynpwm"):
            if c.trace is None:
                continue
            dwt = [A.duty_from_cv(v, c.duty_param, c.pw_depth) for v in c.trace]
            h = A.design_decimator(501, 8, 0.06)
            r = A.ref_dynamic(c.warm, len(x), c.f0, c.sr,
                              lambda n, t=dwt: t[n] if 0 <= n < len(t) else t[-1], 8, h)
        elif c.path == "vco_a_ringmix":
            r = A.ref_mix_static(c.warm, len(x), c.f0, c.sr, c.duty_param,
                                 A.pulse_node_weight(c.morph))
        else:
            r = A.ref_static(c.warm, len(x), c.f0, c.sr, c.duty_param)
        if r is None:
            continue
        st = A.residual_stats(x, r)
        if st is None:
            continue
        out[cid] = st["res_db"]
    return out


def slew_cells(arm, min_ratio=SLEW_MIN):
    """[(id, ratio)] for every dynamic cell whose measured per-frame duty movement is at least
    `min_ratio` correction windows. See the SLEW_MIN comment: this reads the STIMULUS only."""
    out = []
    for r in A.load_manifest(arm):
        if r["path"] not in ("vco_a_dynpwm", "vco_b_dynpwm"):
            continue
        c = A.Cell(r, arm)
        if c.trace is None or len(c.trace) < 2:
            continue
        dwt = [A.duty_from_cv(v, c.duty_param, c.pw_depth) for v in c.trace]
        slew = max(abs(dwt[i + 1] - dwt[i]) for i in range(len(dwt) - 1))
        dt = c.f0 / c.sr
        if dt > 0.0 and slew / dt >= min_ratio:
            out.append((c.id, slew / dt))
    out.sort()
    return out


def peaks(arm, cells_wanted):
    """{id: peak} from the manifest, for the cells that were produced. Used only to DISCLOSE how far
    a mutated arm leaves the candidate's envelope -- never to judge it."""
    out = {}
    for r in A.load_manifest(arm):
        if r["id"] in cells_wanted:
            try:
                out[r["id"]] = float(r["peak"])
            except (KeyError, TypeError, ValueError):
                pass
    return out


def pred_frames(arm):
    """{id: {"steps": [frames], "pred": [frames]}} for every dynamic cell, from the stimulus only.

    `steps` is every frame in `align_check`'s analysis window where the duty moved at all (reported,
    to show why a cell with many steps can still predict nothing). `pred` is P2 from the PRED_MIN
    comment: the frames where the stale-duty naive sign differs from the current-duty naive sign AND
    the frame is outside both permitted classes, which is exactly the set `unexplained` must contain.

    The frame indexing is `align_check`'s own: it evaluates frame n as `n0 + i` over the window and
    reads the duty as `dwt[n]`, and `phi_at` is the same phase law it uses. If those two disagreed the
    criterion would be naming a frame align_check never looks at, and would pass vacuously."""
    out = {}
    for r in A.load_manifest(arm):
        if r["path"] not in ("vco_a_dynpwm", "vco_b_dynpwm"):
            continue
        c = A.Cell(r, arm)
        if c.trace is None:
            continue
        dwt = [A.duty_from_cv(v, c.duty_param, c.pw_depth) for v in c.trace]
        dt = c.f0 / c.sr
        steps, pred = [], []
        for n in range(c.warm, c.warm + c.win):
            if n >= len(dwt) or n - 1 < 0:
                break
            d, dprev = dwt[n], dwt[n - 1]
            if d != dprev:
                steps.append(n)
                p = A.phi_at(n, c.f0, c.sr)
                if (p < d) != (p < dprev):
                    near = min(abs(p), abs(p - 1.0), abs(p - d), abs(p - 1.0 + d))
                    if near >= dt:
                        pred.append(n)
        out[c.id] = {"steps": steps, "pred": pred,
                     "slew": (max(abs(dwt[i + 1] - dwt[i]) for i in range(len(dwt) - 1))
                              if len(dwt) > 1 else 0.0)}
    return out


_ALIGN_CACHE = {}


def align_all(arm):
    """{id: align_check result} for every cell align_check applies to. Cached per arm: the candidate's
    side of the criterion does not depend on the mutation, and recomputing a 16384-sample Python loop
    four times would only make the run slower, not the verdict stronger.

    A cell that align_check cannot evaluate is reported as such and is NOT silently dropped: an empty
    result would read as "nothing failed", which is the one reading that must never be available."""
    if arm in _ALIGN_CACHE:
        return _ALIGN_CACHE[arm]
    out = {}
    for r in A.load_manifest(arm):
        c = A.Cell(r, arm)
        if c.path == "vco_a_ringmix":
            continue
        out[c.id] = A.align_check(c, c.f0, c.sr)
    _ALIGN_CACHE[arm] = out
    return out


def build_and_run(scratch, out_dir, label, peak_hi):
    """Build the probe in the scratch tree and run it.

    The caller has already copied the candidate headers in and applied this mutation's patch; this
    function must NOT copy anything, or it would silently overwrite the patch with the pristine
    candidate source and every mutation would report the candidate's own numbers.

    Returns (ok, detail). ok is False when the BUILD failed or the probe did not exit 0 -- both are
    INVALID rather than red, and the caller must say so rather than count them."""
    b = subprocess.run(["cmake", "--build", "build-base-rel", "--target", "gh19_s3_pulse_probe",
                        "-j", "8"], cwd=scratch, capture_output=True, text=True)
    if b.returncode != 0:
        return False, "BUILD FAILED (invalid, not red):\n%s" % b.stdout[-1500:]
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    p = subprocess.run([os.path.join(scratch, "build-base-rel", "gh19_s3_pulse_probe"),
                        "--out", out_dir, "--peak-hi", str(peak_hi)],
                       cwd=scratch, capture_output=True, text=True)
    if p.returncode != 0:
        return False, "PROBE EXIT %d (invalid, not red):\n%s" % (p.returncode, p.stderr[-1500:])
    return True, label


def declaration_preflight(cand):
    """Every mutation anchor must exist EXACTLY ONCE in the candidate file it names.

    WHY THIS EXISTS, AND HOW THIS RUNNER EARNED IT. The apply loop below already refuses a mutation
    whose anchor is not unique, so a stale anchor can never produce a silent no-op -- but it is
    discovered only after that arm has been built and run, and what the reader sees is an INVALID arm
    with an "anchor count 0" line, which reads like a broken measurement rather than like a stale
    declaration. That is exactly what happened when revision 4 rewrote the kernel's return statement:
    `wrong_sign` anchored on the old `polyblepResidual(t, dt) - polyblepResidual(b, dt)` text, matched
    zero times, and the whole suite went to `3/4 red, 1 invalid`. The measurement was fine; the
    CONTROL was out of date. Checking the anchors against the real sources up front turns that into a
    named declaration problem, reported before any build, and it is the same discipline the
    out-of-domain suite (`run_gh19_s3_over_negatives.py`) applies to its criterion strings.

    This is not a weaker substitute for the apply-time check -- it is the same check, moved earlier.
    Both stay: the preflight reports it clearly, the apply loop remains the enforcement point."""
    problems = []
    for mut in MUTATIONS:
        path = os.path.join(cand, mut["file"])
        if not os.path.exists(path):
            problems.append("%s: %s does not exist in the candidate tree" % (mut["name"], mut["file"]))
            continue
        text = read_text(path)
        for find, _repl in mut["edits"]:
            n = text.count(find)
            if n != 1:
                problems.append("%s: anchor occurs %d times in %s: %r"
                                % (mut["name"], n, mut["file"], find.strip()))
    return problems


def restore(scratch):
    subprocess.run(["git", "checkout", "--", HDR_VCO, HDR_MAP, HDR_RUNTIME], cwd=scratch,
                   capture_output=True, text=True)
    k = os.path.join(scratch, HDR_KERNEL)
    if os.path.exists(k):
        os.remove(k)


def deltas(base, arm, cells):
    """{id: arm - base} over the cells present in both."""
    d = {}
    for cid in cells:
        if cid in base and cid in arm:
            d[cid] = arm[cid] - base[cid]
    return d


def judge(mut, base, cand, m, cand_arm, mutant_arm):
    """Apply the named criterion. Returns (passed, detail_lines). Every criterion is written as the
    prediction the mutation makes, so PASS means the measurement saw the break.

    `cand_arm`/`mutant_arm` are the two probe output directories. The reconciliation half of the
    pwm_late criterion is asserted on the rendered arms themselves, not on the residual dicts, so it
    needs them by path."""
    name = mut["step"]
    s = deltas(base, m, STATIC_CELLS)
    y = deltas(base, m, DYNAMIC_CELLS)
    g = deltas(base, m, RINGMIX_CELLS)
    lines = []
    if name == "bypass":
        worst = max([abs(v) for v in list(s.values()) + list(y.values()) + list(g.values())] or [99.0])
        lines.append("max |mutation - baseline| over %d static + %d dynamic + %d mixed cells = %.4f dB"
                     % (len(s), len(y), len(g), worst))
        return worst <= TOL, lines
    if name == "wrong_position":
        # Two-sided on purpose: worsen where duty != 0.5, be INERT where duty == 0.5.
        #
        # INERT MEANS IDENTICAL TO THE CANDIDATE, NOT TO THE BASELINE. At duty = 0.5 the correct
        # position `duty` and the wrong one `1 - duty` are the same number, so the mutated arm runs
        # the candidate's own arithmetic there and must reproduce it byte-for-byte. Comparing those
        # cells to the baseline would instead ask the mutation to give up the candidate's improvement
        # -- true of any working arm and therefore no evidence at all.
        allc = dict(deltas(cand, m, STATIC_CELLS), **deltas(cand, m, RINGMIX_CELLS))
        asym = {k: v for k, v in allc.items() if k in ASYMMETRIC_CELLS}
        sym = {k: v for k, v in allc.items() if k not in ASYMMETRIC_CELLS}
        for k, v in sorted(allc.items()):
            lines.append("  %-38s %+7.2f dB vs candidate  (duty %s 0.5)" %
                         (k, v, "!=" if k in ASYMMETRIC_CELLS else "=="))
        ok = (len(asym) > 0 and all(v > WORSE for v in asym.values())
              and len(sym) > 0 and all(abs(v) <= TOL for v in sym.values()))
        return ok, lines
    if name == "wrong_sign":
        for k, v in sorted(list(s.items()) + list(g.items())):
            lines.append("  %-38s %+7.2f dB vs baseline" % (k, v))
        vals = list(s.values()) + list(g.values())
        return (len(s) > 0 and all(v > WORSE for v in vals)), lines
    if name == "pwm_late":
        # Same rule: a stale duty read is invisible where the duty is stationary, so those cells must
        # equal the CANDIDATE; the baseline comparison would again only restate that the candidate
        # improves.
        dc = deltas(cand, m, STATIC_CELLS)
        ds = max([abs(v) for v in dc.values()] or [99.0])
        lines.append("  max |static movement vs candidate| = %.4f dB (must stay <= %.2f: a stationary "
                     "duty cannot see a stale read)" % (ds, TOL))
        # Judged: the cells whose stimulus slews the duty by >= SLEW_MIN windows per frame, where the
        # stale edge is unambiguously far from where the correction belongs.
        #
        # WHAT THIS CRITERION DOES *NOT* SAY, AND WHY IT IS WRITTEN THIS WAY. The first version of it
        # predicted that the stale duty would make every one of these cells WORSE. Measured, that
        # prediction is FALSIFIED: on the 13 cells above the slew rule the movement splits by LFO rate
        # -- every slow-LFO cell gets worse (+0.20..+0.25 dB) and every fast-LFO cell gets BETTER
        # (-0.24..-1.59 dB). A criterion asserting "worsen" would have to be either abandoned or
        # tuned until it passed, and both would hide a real property of the instrument. The property
        # is this: in the stepping-duty regime the residual is not a monotone function of duty-timing
        # error, because the dynamic reference holds one duty per frame while the product reads the CV
        # per sample, so the reference's own model error is largest exactly where the duty steps.
        # The criterion is therefore restated as the two predictions the physics DOES support and the
        # measurement can falsify: bit-inert where the duty is stationary, and NOT blind where it
        # moves. The sign split is printed rather than asserted.
        dy = deltas(cand, m, LATE_CELLS)
        n_better = sum(1 for v in dy.values() if v < -SENS)
        n_worse = sum(1 for v in dy.values() if v > SENS)
        for k, v in sorted(dy.items()):
            lines.append("  %-50s %+7.2f dB vs candidate" % (k, v))
        lines.append("  sign split: %d cells better, %d cells worse -- the 'must worsen' prediction "
                     "is FALSIFIED (see the criterion comment); the judged claim is sensitivity, not "
                     "direction" % (n_better, n_worse))
        # Reported, not judged: the smooth-PWM cells the S3 improvement claim rests on. Their slew is
        # <= 0.182 windows/frame, so this mutation is predicted small there; printing it states the
        # sensitivity boundary instead of hiding it.
        sy = deltas(cand, m, DYNAMIC_CELLS)
        lines.append("  smooth-PWM cells carrying the improvement claim (slew <= 0.182 windows/frame),"
                     " reported NOT judged:")
        for k, v in sorted(sy.items()):
            lines.append("    %-48s %+7.2f dB vs candidate" % (k, v))
        ok = (len(LATE_CELLS) > 0 and len(dy) == len(LATE_CELLS)
              and all(abs(v) > SENS for v in dy.values())
              and max(abs(v) for v in dy.values()) > WORSE
              and ds <= TOL)
        # ---- and the PRODUCT TIMING half of the same control (@Codex e144b61 review, item 4) ----
        #
        # Everything above is a sensitivity measurement: "the residual moved". @Codex's objection is
        # that a residual moving is not a product assertion, and that the previous revision of this
        # control was not even a product timing error. So the criterion is restated around the
        # analyzer's own same-frame reconciliation, which compares the rendered output against the
        # naive model rebuilt from the RECORDED SOURCE FRAMES -- an independent trace, unchanged by
        # the mutation -- and requires the disagreement to be zero outside the permitted wrap/edge
        # classes. GREEN FIRST: the candidate must pass it. RED SECOND: the mutant, which exits 0 and
        # builds, must fail it, and its FIRST failing sample must be NAMED, at a frame index, on a
        # frame where the stimulus actually steps the duty.
        ca = align_all(cand_arm)
        ma = align_all(mutant_arm)
        judged = [i for i in LATE_CELLS if len(PRED_FRAMES.get(i, {}).get("pred", [])) >= PRED_MIN]
        lines.append("  same-frame reconciliation (product output vs the model of the recorded source"
                     " frames):")
        lines.append("    candidate arm: %d/%d cells align (unexplained == 0)"
                     % (sum(1 for v in ca.values() if v["ok"]), len(ca)))
        cand_green = all(v["ok"] for v in ca.values())
        if not cand_green:
            bad = [k for k, v in ca.items() if not v["ok"]]
            lines.append("    *** THE CANDIDATE ITSELF FAILS THIS ASSERTION on %s -- the control's"
                         " 'green first' half is not established, so no verdict is available ***"
                         % ", ".join(sorted(bad)[:4]))
        # The inert half. Exact rather than probabilistic: a stationary duty's stale value IS the
        # current value, so these cells must reconcile exactly as the candidate does. A static cell
        # going red would mean the mutation is not a timing error at all.
        st_red = [k for k in STATIC_CELLS if k in ma and not ma[k]["ok"]]
        lines.append("    mutant arm, static cells (a stationary duty cannot see a stale read):"
                     " %d/%d still align%s"
                     % (len(STATIC_CELLS) - len(st_red), len(STATIC_CELLS),
                        "" if not st_red else "  *** RED: %s ***" % ", ".join(sorted(st_red))))
        # The judged half: the frames the STIMULUS predicted must be flagged, and the first one.
        named_ok = True
        red_judged = []
        for cid in judged:
            mres = ma.get(cid)
            pred = PRED_FRAMES[cid]["pred"]
            if mres is None:
                lines.append("    %-46s NOT MEASURED (invalid)" % cid)
                named_ok = False
                continue
            if mres["ok"]:
                lines.append("    %-46s *** still aligns; predicted %d unexplained frame(s) "
                             "starting at %d ***" % (cid, len(pred), pred[0]))
                named_ok = False
                continue
            red_judged.append(cid)
            fn_ok = (mres["first_n"] == pred[0])
            cnt_ok = (mres["unexplained"] >= len(pred))
            named_ok = named_ok and fn_ok and cnt_ok
            lines.append("    %-46s RED  unexplained=%-3d (predicted >=%d) first_bad frame=%d "
                         "(predicted %d, %s) i=%d phase=%.9f duty=%.9f err=%.9f"
                         % (cid, mres["unexplained"], len(pred), mres["first_n"], pred[0],
                            "MATCH" if fn_ok else "*** MISMATCH ***", mres["first_i"],
                            mres["first_phase"], mres["first_duty"], mres["first_err"]))
        # Reported, not judged: the slew-rule cells whose stimulus predicts NO unexplained frame. This
        # is the control's coverage boundary, printed rather than implied -- see the PRED_MIN comment.
        below = [(i, PRED_FRAMES.get(i, {})) for i in LATE_CELLS if i not in judged]
        if below:
            lines.append("    predicted to leave `unexplained` at ZERO (steps land where the two "
                         "duties agree, or inside the permitted edge class), reported NOT judged:")
            for i, pf in sorted(below):
                v = ma.get(i, {}).get("unexplained")
                lines.append("      %-48s %2d step frame(s), predicted 0, mutant unexplained=%s"
                             % (i, len(pf.get("steps", [])), "n/a" if v is None else v))
        ok = (ok and cand_green and not st_red and len(judged) > 0
              and len(red_judged) == len(judged) and named_ok)
        return ok, lines
    return False, ["no criterion implemented for %s" % name]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cand", required=True, help="the candidate worktree (never written to)")
    ap.add_argument("--scratch", required=True, help="a pinned-baseline worktree used as the mutation host")
    ap.add_argument("--base-arm", required=True, help="probe output dir of the UNMODIFIED baseline arm")
    ap.add_argument("--cand-arm", required=True, help="probe output dir of the candidate arm")
    ap.add_argument("--work", default="/tmp/gh19-s3/negctl", help="where mutated arms are written")
    args = ap.parse_args(argv)

    rels = (HDR_VCO, HDR_MAP, HDR_KERNEL, HDR_RUNTIME)
    before = tree_hashes(args.cand, rels)
    print("candidate tree hashes before (these must not move):")
    for k, v in before.items():
        print("  %s %s" % (v[:16], k))

    # The pwm_late criterion's cell set, selected from the BASELINE arm's stimulus traces by the
    # declared slew rule. Printed with the ratios it selected on, so the rule is auditable from the
    # log rather than trusted.
    LATE_CELLS[:] = [i for i, _ in slew_cells(args.base_arm)]
    print("duty-slew rule: %d dynamic cells move the duty by >= %.0f correction windows per frame "
          "(dt = f0/sr); these carry the pwm_late criterion:" % (len(LATE_CELLS), SLEW_MIN))
    for i, r_ in slew_cells(args.base_arm):
        print("    %-50s %.2f windows/frame" % (i, r_))
    # ... and the sub-set of those where the error is GUARANTEED to be visible: the frames inside
    # align_check's own analysis window at which the stale and current duties disagree about the naive
    # sign, far enough from every edge that the mismatch cannot be absorbed by the permitted edge
    # class. Same stimulus-only rule, declared before the mutant is built -- see the PRED_MIN comment.
    PRED_FRAMES.clear()
    PRED_FRAMES.update(pred_frames(args.base_arm))
    judged = [i for i in LATE_CELLS if len(PRED_FRAMES.get(i, {}).get("pred", [])) >= PRED_MIN]
    print("predicted-visibility rule: %d of those have >= %d frame(s) where the stale read must flip "
          "the naive sign OUTSIDE the permitted edge class; these are the cells the reconciliation "
          "half is asserted on:" % (len(judged), PRED_MIN))
    for i in sorted(PRED_FRAMES):
        if i in LATE_CELLS:
            pf = PRED_FRAMES[i]
            tail = ("" if i in judged else
                    "   (predicted 0 unexplained frames: reported, not judged -- see the PRED_MIN "
                    "comment)")
            if pf["pred"]:
                tail = "   predicted first_bad frame=%d (+,%d more)" % (pf["pred"][0],
                                                                       len(pf["pred"]) - 1)
            print("    %-50s %2d step frame(s), %2d predicted unexplained%s"
                  % (i, len(pf["steps"]), len(pf["pred"]), tail))
    cells_all = STATIC_CELLS + DYNAMIC_CELLS + RINGMIX_CELLS + LATE_CELLS

    print("\nreading the two reference arms with the criterion reference construction...")
    base = residuals(args.base_arm, cells_all)
    cand = residuals(args.cand_arm, cells_all)
    print("  baseline: %d cells   candidate: %d cells" % (len(base), len(cand)))
    claimed = deltas(base, cand, cells_all)
    if not claimed:
        print("FATAL: no comparable cells between the two arms; nothing to control")
        return 4
    n_imp = sum(1 for v in claimed.values() if v < -WORSE)
    print("  the candidate's own claim, on these cells: %d of %d improved by > %.2f dB "
          "(median %+.2f dB)" % (n_imp, len(claimed),
                                 WORSE, sorted(claimed.values())[len(claimed) // 2]))

    results = []
    decl_problems = declaration_preflight(args.cand)
    print("\n-- declaration preflight: every anchor must occur exactly once in the candidate tree --")
    if decl_problems:
        for p in decl_problems:
            print("  *** %s" % p)
        print("  %d stale declaration(s): the control set no longer describes this revision, so any "
              "verdict below would be about a mutation that never happened" % len(decl_problems))
        results.append(("declaration_preflight", "-", "INVALID", decl_problems[0]))
    else:
        print("  all %d anchors present exactly once across %d mutations"
              % (sum(len(m["edits"]) for m in MUTATIONS), len(MUTATIONS)))
    try:
        for mut in MUTATIONS:
            print("\n=== NEGCTL %s — %s ===" % (mut["name"], mut["why"]))
            for rel in (HDR_VCO, HDR_MAP, HDR_KERNEL, HDR_RUNTIME, PROBE):
                shutil.copyfile(os.path.join(args.cand, rel), os.path.join(args.scratch, rel))
            path = os.path.join(args.scratch, mut["file"])
            text = read_text(path)
            for find, repl in mut["edits"]:
                n = text.count(find)
                if n != 1:
                    print("  *** PATCH ANCHOR NOT UNIQUE (%d occurrences) — invalid ***" % n)
                    results.append((mut["name"], mut["criterion"], "INVALID", "anchor count %d" % n))
                    break
                text = text.replace(find, repl)
            else:
                write_text(path, text)
                mut["step"] = mut["name"]
                ok, detail = build_and_run(args.scratch,
                                           os.path.join(args.work, mut["name"]), mut["name"],
                                           MUT_PEAK_HI)
                if not ok:
                    print("  " + detail.replace("\n", "\n  "))
                    results.append((mut["name"], mut["criterion"], "INVALID", detail.splitlines()[0]))
                else:
                    arm = os.path.join(args.work, mut["name"])
                    m = residuals(arm, cells_all)
                    # Disclosed, not judged: how far this arm left the candidate's envelope. The
                    # wrong-edge mutations are predicted to leave it, so the reader must be able to
                    # see that the arm's magnitude is an excursion and that its criterion is a
                    # direction test rather than a magnitude claim.
                    pk = peaks(arm, cells_all)
                    if pk:
                        print("  envelope: max peak over the criterion cells = %.4f (candidate "
                              "default bound 0.55, widened to %.2f for this arm)"
                              % (max(pk.values()), MUT_PEAK_HI))
                    ok2, lines = judge(mut, base, cand, m, args.cand_arm, arm)
                    for l in lines:
                        print("  " + l)
                    print("  criterion %s: %s" % (mut["criterion"], "FAILS AS PREDICTED (red, ok)"
                                                  if ok2 else "*** DID NOT FAIL ***"))
                    results.append((mut["name"], mut["criterion"],
                                    "RED" if ok2 else "GREEN", ""))
            restore(args.scratch)
    finally:
        restore(args.scratch)

    after = tree_hashes(args.cand, rels)
    moved = [k for k in before if before[k] != after.get(k)]
    print("\n=== NEGCTL SUMMARY ===")
    for name, crit, verdict, note in results:
        print("  %-16s %-52s %s %s" % (name, crit, verdict, note))
    if moved:
        print("  *** CANDIDATE TREE WAS MODIFIED: %s ***" % ", ".join(moved))
        return 5
    print("  candidate tree unchanged (all %d headers byte-identical)" % len(before))
    n_invalid = sum(1 for r in results if r[2] == "INVALID")
    n_green = sum(1 for r in results if r[2] == "GREEN")
    print("  %d/%d red as predicted, %d invalid, %d unexpectedly green"
          % (len(results) - n_invalid - n_green, len(results), n_invalid, n_green))
    return 0 if (n_invalid == 0 and n_green == 0 and not moved
                 and len(results) == len(MUTATIONS)) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
