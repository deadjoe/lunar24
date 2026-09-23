#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh19_s3_over_negatives.py — task #118 (GH #19 S3): NEGATIVE CONTROLS FOR THE OUT-OF-DOMAIN
# (dt > kPolyblepMaxDt) AND REACHABLE-BOX CHECKS ADDED IN THIS REVISION.
#
# WHY THIS SCRIPT EXISTS. run_gh19_s3_kernel_sweep.py asserts a set of claims about what happens
# PAST the kernel's proven domain: that a non-positive step is still bit-identical to the naive
# pulse, that past dt = 0.5 the emitted waveform is the BOUNDED CONTINUOUS EXTENSION obtained by
# capping the kernel width (and therefore exactly the dt = 0.5 output, sample for sample), that this
# cap is the IDENTITY in domain so no in-domain sample moved, that the regime LABEL is unchanged,
# and that the reachable box measured through the real Vco object agrees with the pitch law it claims
# to be measuring. A claim of that shape is only evidence if the checks producing it can FAIL. Each
# control below breaks exactly one thing that the corresponding claim depends on and requires the
# named criterion to go RED — and requires other predictions to stay green, because a mutation that
# breaks everything tells you nothing about which claim saw it.
#
# WHAT CHANGED WITH THE CAP (and why one control had to be replaced, not repaired). The previous
# revision's `residual_domain_shrink` confined polyblepResidual's `t < dt` branch to `dt <=
# kPolyblepMaxDt`. Under the width cap that edit is a NO-OP: the residual is now only ever handed
# w = min(dt, cap) <= 0.5, so the condition is unconditionally true and the arm would come out GREEN
# — i.e. INVALID, a dead control. That fact is itself the structural change this revision makes, and
# it is measured from the other side by `no_cap` (take the cap away: the pre-cap header, and every
# out-of-domain claim fails) and `cap_always` (cap unconditionally: the in-domain samples move). A
# control that can no longer be constructed is not replaced by a weaker one; it is replaced by the
# two that the new structure admits.
#
# WHERE THE MUTATION HAPPENS. Never in this worktree. `core/include` is COPIED to a scratch tree,
# patched there, compiled from there (the runner's `--include-dir` override), and thrown away. The
# candidate worktree's own headers are hashed before and after the whole run and the run fails if
# any of them moved. The second control group does not touch C++ at all: it plants a call site in a
# scratch directory and points the runner's FM-caller scan (`--fm-scan-root`) at that directory,
# which is the only way to show that scan is not blind.
#
# WHAT COUNTS AS RED. Same ruling as run_gh19_s3_negatives.py (#116, carried through #117): RED
# requires the tree to BUILD, the driver to EXIT 0, and the NAMED criterion to fail. A compile error
# is an INVALID control, not a red one — it proves the compiler reacted, not that the measurement
# sees the defect. So is a driver that dies, a criterion that could not be evaluated, and — the case
# this script exists to make impossible — a red that arrived from a check other than the named one.
# The runner's positive control (the refuted revision's 0.5625 jump must still reproduce) is inside
# every arm: if a mutation kills it, the arm is INVALID rather than red, because a driver that can no
# longer see the refutation is not measuring anything.
#
# THE NAMED-CRITERION MATCH IS ON THE *RED* FORM OF THE MESSAGE. The runner prints both outcomes of
# every check with the same label; only the failure carries "*** <label>: measured". Matching the
# bare label would accept an arm that went red somewhere else while this check passed, so each
# `criterion`/`reach` entry below is the failure text, and a control whose expected failure text is
# absent is reported as INVALID (red for the wrong reason), never as a hit.

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "tools", "run_gh19_s3_kernel_sweep.py")
INCLUDE_SRC = os.path.join(ROOT, "core", "include")
SWEEP_SRC = "tools/gh19_s3_pulse_kernel_sweep.cpp"

# The production headers under test. Hashed before and after: the whole point of copying them is that
# the candidate tree must be untouched, and "I reverted it afterwards" is one crash away from being
# false.
TRACKED = [
    "core/include/lunar24/core/polyblep_kernel.h",
    "core/include/lunar24/core/pulse_blep_kernel.h",
    "core/include/lunar24/core/vco.h",
    "core/include/lunar24/core/machine_runtime.h",
    "core/include/lunar24/core/vco_wave_map.h",
    "tools/gh19_s3_pulse_kernel_sweep.cpp",
    "tools/run_gh19_s3_kernel_sweep.py",
]

# The failure texts the runner prints, so the controls below can name one without transcribing the
# runner's phrasing from memory. `red()` is for the checks that report through the runner's `need()`
# helper (whose failure form is "*** <label>: measured ..."); the checks that print their own prose
# failure are named by a literal fragment of that prose, written out in full at each use site.
#
# THE STALENESS HAZARD, AND THE PREFLIGHT THAT CLOSES IT. A `criterion`/`reach`/`forbidden` string
# that no longer exists in the runner is a SILENT no-op: `forbidden` in particular would still "hold"
# while guarding nothing. So every declared string is checked against the runner's and driver's own
# source before any arm runs (see label_preflight below) and a string that is not present there is
# INVALID. That turns "this check was renamed and this control stopped covering anything" from an
# invisible decay into a loud failure.
def red(label):
    return "*** %s: measured" % label

# ---- control group A: the header mutations --------------------------------------------------------
#
# Each entry names the file it patches, the exact source text it replaces (which must occur EXACTLY
# once -- a mutation that applies to the wrong site or to several sites is INVALID, not red), what
# the break is, the criterion that must go red, and the additional predictions that must hold. Those
# extra predictions are the reason these are controls rather than demonstrations: a break that
# reddens everything cannot tell you which claim detected it.
MUTATIONS = [
    {
        "name": "pristine_copy",
        "expect": "green",
        "why": "THE REFERENCE ARM. Nothing is mutated: this is the candidate's own headers, reached "
               "through the scratch copy. Every RED below is measured against this arm, so if the "
               "copying/mutating machinery itself disturbed a check, this is where it shows up as a "
               "red arm that no mutation explains.",
        "file": None,
        "edits": [],
    },
    {
        "name": "c3a_fallback",
        "expect": "red",
        "why": "the C3a candidate, i.e. the strategy @Codex's ruling REJECTED: the pulse kernel "
               "adopts the TRIANGLE path's out-of-domain fallback verbatim "
               "(`if (polyblepSupportReachesHalfPeriod(dt)) return 0.0;`). This control does double "
               "duty. As a control it shows the out-of-domain claims are anchored to the correction "
               "path rather than to the naive pulse. As a MEASUREMENT it is the effect the rejected "
               "candidate would have had on the product: the reviewer's own argument against it was "
               "that switching the correction off reintroduces a hard discontinuity at the switch "
               "point, and the VCO-CROSSOVER check measures exactly that on the real Vco object "
               "(1.81 in one 2.5e-7 grid step). The cap is the alternative that does not do this, and "
               "the pair -- this arm red, cap_always green out of domain -- is the comparison.",
        "file": "pulse_blep_kernel.h",
        "edits": [(
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;\n",
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;\n"
            "  if (polyblepSupportReachesHalfPeriod(dt)) return 0.0;  // NEGCTL c3a_fallback\n",
        )],
        # A PREDICTION FROM THE PREVIOUS REVISION OF THIS CONTROL WAS WRONG AND IS RECORDED HERE.
        # It was written that zeroing the correction out of domain would redden
        # OVER-STRUCT-EXTENSION ("out(dt) IS out(0.5)"). It does not, and the reason is worth
        # keeping: the inserted guard is true at dt = 0.5 as well, so BOTH sides of that identity
        # collapse to the naive pulse and the check passes TRIVIALLY. An identity that two sides can
        # satisfy by both being degenerate is not measuring the extension. The measurement that does
        # see the defect is the t-gap discriminator, so that is the criterion now.
        #
        # WHAT THE ARM ACTUALLY FIRES (measured, full set): every out-of-domain dt in OVER-GRID-CAP;
        # OVER-REPRO-X0 (dt = 0.5 itself); OVER-STRUCT-CAPISIDENTITY with 359476 bit-differences --
        # because the guard also fires AT dt = 0.5, so this candidate is not an out-of-domain fallback
        # only, it moves the boundary cell too; and the real Vco's max step across the crossover,
        # 1.81 for a 2.5e-7 grid spacing, which is the hard discontinuity at the switch point.
        "criterion": "*** OVER-GRID-CAP dt=",
        "reach": ["*** OVER-STRUCT-CAPISIDENTITY: measured",
                  "*** OVER-REPRO-X0 cap == unc at dt=0.5: measured",
                  "*** VCO-CROSSOVER: the largest step across the cap boundary"],
        # The two arms of the out-of-domain section that are computed by the DRIVER, not by the
        # header (uncappedPulse and guardedPulse), and the wiring identity, which compares the object
        # against the header it was compiled with -- none of them may notice this edit. A firing here
        # would mean the control's red is not attributable to the correction path.
        "forbidden": ["*** OVER-REPRO-UNC lo", "*** OVER-RESIDUAL-UNC dt=",
                      "*** OVER-GRID-UNC dt=", red("VCO-WIRING emitted == kernel"),
                      red("VCO-CROSSOVER far side == out(dt=cap)"),
                      "*** OVER-STRUCT-EXTENSION: measured"],
    },
    {
        "name": "no_cap",
        "expect": "red",
        "why": "the cap is removed (`w = dt`), which is exactly the PRE-CAP HEADER: the residual's "
               "own step at t = dt re-enters the emitted waveform past dt = 0.5. In the previous "
               "revision this arm was the header under test; here it is the control that says the "
               "in-domain samples were never the thing the cap changed. The criterion is the t-gap "
               "discriminator, because that check measures the emitted WAVEFORM (a jump's t-gap is "
               "flat in the t resolution, a continuous function's falls as 1/nT) rather than any "
               "single coordinate.",
        "file": "pulse_blep_kernel.h",
        "edits": [(
            "  const double w = (dt < kPolyblepMaxDt) ? dt : kPolyblepMaxDt;\n",
            "  const double w = dt;  // NEGCTL no_cap\n",
        )],
        "criterion": "*** OVER-GRID-CAP dt=",
        "reach": ["*** OVER-STRUCT-EXTENSION: measured",
                  "*** OVER-REPRO-CAP: the capped arm is BIT-IDENTICAL",
                  "*** OVER-RESIDUAL-CAP dt=",
                  "*** VCO-CROSSOVER far side == out(dt=cap): measured"],
        # THE ISOLATION PREDICTION THAT MATTERS. min(dt, cap) is dt for dt <= 0.5 either way, so the
        # in-domain grid must be BIT-IDENTICAL under this edit. If it moved, the cap was doing
        # something in domain and the "in-domain evidence is unchanged" claim is false.
        "forbidden": ["*** OVER-STRUCT-CAPISIDENTITY: measured",
                      "*** OVER-RESIDUAL-UNC dt=", "*** OVER-GRID-UNC dt="],
    },
    {
        "name": "cap_always",
        "expect": "red",
        "why": "the cap is applied UNCONDITIONALLY (`w = kPolyblepMaxDt`), a plausible misreading of "
               "'cap the width'. Past dt = 0.5 this changes nothing -- w = 0.5 is what the cap gives "
               "there anyway -- so every out-of-domain claim must stay green, and the arm isolates "
               "the one claim it does break: that the cap is the IDENTITY in domain, i.e. that the "
               "static evidence for 0 < step <= 0.5 is bit-unchanged.",
        "file": "pulse_blep_kernel.h",
        "edits": [(
            "  const double w = (dt < kPolyblepMaxDt) ? dt : kPolyblepMaxDt;\n",
            "  const double w = kPolyblepMaxDt;  // NEGCTL cap_always\n",
        )],
        "criterion": "*** OVER-STRUCT-CAPISIDENTITY: measured",
        "reach": [],
        # The out-of-domain half of the section is untouched by construction: for dt >= 0.5 this edit
        # computes the same w as the header. Stated so that a red out there is reported as a broken
        # isolation claim rather than silently absorbed into an arm that already went red.
        "forbidden": ["*** OVER-STRUCT-EXTENSION: measured", "*** OVER-GRID-CAP dt=",
                      "*** VCO-CROSSOVER far side == out(dt=cap): measured"],
    },
    {
        "name": "label_drift",
        "expect": "red",
        "why": "polyblepPulseWindowsDisjoint -- the regime LABEL, which no code path branches on -- "
               "is widened by 0.01. The label's whole reason for existing is that a caller can "
               "interpret a cell by it, so a drift has to be visible somewhere; the sweep checks the "
               "shipped predicate against the honest `m >= 2*min(dt, cap)` form cell by cell. This "
               "control is the only thing in the file that shows that check is connected to the "
               "predicate rather than to a restatement of it.",
        "file": "pulse_blep_kernel.h",
        "edits": [(
            "  return m >= 2.0 * dt;\n",
            "  return m >= 2.0 * dt + 0.01;  // NEGCTL label_drift\n",
        )],
        "criterion": red("OVER-STRUCT-LABEL"),
        "reach": [],
        # The label gates nothing, so no numerical claim may move with it. This is the strongest
        # isolation prediction in the file and it is also a check on the HEADER's own claim that the
        # predicate is reporting-only.
        "forbidden": ["*** OVER-STRUCT-CAPISIDENTITY: measured",
                      "*** OVER-STRUCT-EXTENSION: measured", "*** OVER-GRID-CAP dt=",
                      "*** OVER-RESIDUAL-CAP dt=", red("VCO-WIRING emitted == kernel")],
    },
    {
        "name": "stop_regressed",
        "expect": "red",
        "why": "the correction's dt <= 0 exit returns a constant instead of zero. The kernel's first "
               "claim -- a non-positive step leaves the naive pulse BIT-IDENTICAL -- is the one the "
               "stop/reverse strategy rests on, and this is the smallest edit that breaks it. The "
               "constant is deliberately not a function of t: `R(t) - R(b)` with a t-independent "
               "constant would cancel and the arm would stay green, i.e. the control would be dead. "
               "Returning it from the correction's own early exit makes the output naive + 0.25 on "
               "every sample of a stopped oscillator.",
        "file": "pulse_blep_kernel.h",
        "edits": [(
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.0;\n",
            "inline double polyblepPulseCorrection(double t, double duty, double dt) {\n"
            "  if (!(dt > 0.0)) return 0.25;  // NEGCTL stop_regressed\n",
        )],
        "criterion": red("OVER-STOP bit-identical naive"),
        # The stop path is the only thing this edit can reach: every dt in the out-of-domain section
        # is strictly positive, so those claims must be untouched. If they moved, the arm is not
        # isolated and its red would not be attributable to the stopped-oscillator claim.
        "reach": [],
        "forbidden": ["*** OVER-RESIDUAL-CAP dt=", "*** OVER-GRID-CAP dt=",
                      "*** OVER-STRUCT-EXTENSION: measured",
                      "*** OVER-STRUCT-CAPISIDENTITY: measured"],
    },
    {
        "name": "box_law_change",
        "expect": "red",
        "why": "the V/OCT exponent in the production pitch law is doubled in a COPY of vco.h. The box "
               "section claims its numbers come from the real Vco object driven through the real law, "
               "and cross-checks the bisected threshold against log2(0.5*sr/panel_max). Doubling the "
               "exponent moves the two apart while leaving the panel maximum (V/OCT = 0) exactly "
               "where it was -- so the arm cannot pass by accident on a different sample rate or by "
               "the panel cell having moved.",
        "file": "vco.h",
        "edits": [(
            "  p *= std::pow(2.0, vOct_);                       // V/OCT: confirmed, 1 V = 1 oct.\n",
            "  p *= std::pow(2.0, 2.0 * vOct_);                // NEGCTL box_law_change\n",
        )],
        # Both box checks that hang off the law must move: the measured threshold halves, and the
        # declared-box maximum (which is V/OCT = +5) moves with it. The threshold is the criterion
        # because it is the one measured ON the object; the declared maximum is the cross-check.
        "criterion": red("BOX V/OCT threshold sr=44100"),
        "reach": [red("BOX declared max CV exp")],
        # The stopped-oscillator cell drives vOct = 0 through the same law, so it must not move; the
        # FM scan does not touch the pitch law either.
        "forbidden": [red("BOX stop emitted constant"), "*** FM SETTER GAINED A CALLER"],
    },
]

# ---- control group B: the instrument controls -----------------------------------------------------
#
# These do not claim a defect. They answer "is the check capable of seeing anything at all", which is
# a different question from "does the header pass it", and mixing the two verdicts is how a vacuous
# check survives review. Both are GREEN-expected and are labelled as instrument controls in the
# output so they are never counted as defect controls.

PLANTED_CALLER = "negctl_planted/planted_caller.cpp"
PLANTED_CALLER_TEXT = (
    "// NEGCTL planted by run_gh19_s3_over_negatives.py -- NOT product code.\n"
    "// This file exists to be FOUND by the runner's FM-caller scan.\n"
    "inline void negctlPlantedCaller(lunar24::core::Vco& v) {\n"
    "  v.setFmCv(0.5);\n"          # member call: must be counted
    "}\n"
)
PLANTED_MENTION = "negctl_planted/planted_mention.h"
PLANTED_MENTION_TEXT = (
    "// NEGCTL planted by run_gh19_s3_over_negatives.py -- NOT product code.\n"
    "// This file only NAMES setFmCv and setFmDepth in prose, with no member access and no call.\n"
    "// A scan that reported a caller here would be matching text rather than call sites.\n"
    "// setFmCv( setFmDepth(\n"
)


def declared_strings():
    """Every criterion / reach / forbidden string this file declares, with the control it belongs
    to, in the order the controls are listed."""
    out = []
    for m in MUTATIONS:
        entries = []
        if m.get("criterion"):
            entries.append(("criterion", m["criterion"]))
        entries += [("reach", s) for s in m.get("reach", ())]
        entries += [("forbidden", s) for s in m.get("forbidden", ())]
        out += [(m["name"], kind, s) for kind, s in entries]
    return out


def _probe_re(probe):
    """A matcher for `probe` tolerating FORMAT SPECIFIERS where the declaration has a number.

    Some criteria name a label the runner builds at runtime, e.g. "BOX V/OCT threshold sr=44100"
    against `need("BOX V/OCT threshold sr=%d" % sr, ...)`. The literal therefore never appears in the
    source, and a verbatim probe would call a live control stale -- which is exactly the false alarm
    that would push someone to weaken this preflight until it stopped catching the real thing. So a
    digit run in the declaration matches either the same digits or a printf specifier. Everything
    else is matched literally, which is what keeps a RENAMED label failing."""
    parts = re.split(r"(\d+)", probe)
    pat = ""
    for i, p in enumerate(parts):
        if i % 2 == 1:
            pat += r"(?:%[-+ #0-9.]*[A-Za-z]|\d+)"
        else:
            pat += re.escape(p)
    return re.compile(pat)


def label_preflight():
    """Check that every declared string can actually appear in the output it is matched against.

    The runner and the driver are the producers of these strings, so their sources are the authority
    on whether a declaration is still live. `"*** X: measured"` is the runner's `need()` failure
    form, which is built at runtime -- so the part that must exist in the source is the label `X`
    between the wrapper, and the wrapper is stripped before matching. Anything else is matched whole.
    Returns (checked_count, [problems])."""
    src = ""
    for rel in (os.path.join("tools", "run_gh19_s3_kernel_sweep.py"), SWEEP_SRC):
        with open(os.path.join(ROOT, rel)) as fh:
            src += fh.read()
    problems = []
    checked = 0
    for name, kind, s in declared_strings():
        checked += 1
        # Strip the runtime-built wrapper first, then the "*** " printer prefix the runner/driver
        # writes literally. What remains must be findable in one of the two sources.
        probe = s
        if probe.startswith("*** "):
            probe = probe[4:]
        if probe.endswith(": measured"):
            probe = probe[: -len(": measured")]
        if not _probe_re(probe).search(src):
            problems.append("[%s] %s %r: this text does not occur in the runner or the driver, so "
                            "the check it names cannot fire -- the declaration is stale and guards "
                            "nothing" % (name, kind, s))
    return checked, problems


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def tree_hashes():
    return {r: sha256(os.path.join(ROOT, r)) for r in TRACKED}


def apply_edits(path, edits, name):
    """Apply exact-text edits under a copy. Returns None on success or a reason string. Each `old`
    must occur EXACTLY once: an edit that matched nowhere means the header moved under this control,
    and one that matched twice means the control cannot say which site it broke. Both are INVALID."""
    with open(path) as fh:
        txt = fh.read()
    for old, new in edits:
        n = txt.count(old)
        if n != 1:
            return "%s: the edit anchor occurs %d times (need exactly 1) in %s" % (name, n,
                                                                                   os.path.basename(path))
        txt = txt.replace(old, new, 1)
    with open(path, "w") as fh:
        fh.write(txt)
    return None


def run_arm(name, include_dir, scan_root, out_path, binary, timeout=900):
    """Run the production runner against this arm. Returns (verdict_kind, rc, stdout, detail)."""
    cmd = [sys.executable, RUNNER,
           "--include-dir", include_dir,
           "--fm-scan-root", scan_root,
           "--out", out_path,
           "--binary", binary]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "invalid", -1, "", "the runner did not finish within %ds" % timeout
    return None, p.returncode, p.stdout, (p.stderr or "")


def judge(name, expect, rc, out, err, criterion=None, reach=(), forbidden=()):
    """The verdict for one arm. Returns (verdict, reasons)."""
    if rc == 2:
        return "INVALID", ["the runner reported a TOOL failure (rc=2): %s" % err.strip()[-400:]]
    if rc not in (0, 1):
        return "INVALID", ["the runner exited %d, which is neither green (0) nor a check failure (1)"
                           % rc]
    if "POSITIVE CONTROL" in out and ("DEAD" in out or "MISSING" in out or "UNPARSEABLE" in out):
        # Caught here rather than trusted to rc, because an arm whose driver can no longer reproduce
        # the refuted revision is INVALID even when the arm's own check happens to fail: it is not
        # evidence about the header.
        return "INVALID", ["the driver's positive control died in this arm, so its verdict about "
                           "the current header means nothing"]
    if expect == "green":
        if rc != 0:
            hits = [ln for ln in out.splitlines() if ln.startswith("***")]
            return "INVALID", ["expected GREEN but the runner failed: %s" % (hits[:3] or ["?"])]
        return "GREEN-OK", []
    # expect == "red"
    if rc == 0:
        return "INVALID", ["expected RED but the runner passed: the mutation is DEAD (the check "
                           "cannot see the break it was built for)"]
    reasons = []
    if criterion and criterion not in out:
        return "INVALID", ["red, but not on the named criterion: %r is absent from the output, so "
                           "some other check caught this mutation" % criterion]
    for r in reach:
        if r not in out:
            reasons.append("PREDICTION MISSED: %r did not fire (the mutation's reach is not what "
                           "was declared)" % r)
    for f in forbidden:
        if f in out:
            reasons.append("ISOLATION BROKEN: %r fired, but this mutation must not reach that "
                           "check" % f)
    return ("RED" if not reasons else "INVALID"), reasons


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--scratch", default="/tmp/gh19-s3/over-negctl",
                    help="scratch root for the header copies, the planted scan tree and the "
                         "captured runner output (never inside the worktree)")
    ap.add_argument("--out", default=os.path.join("report", "gh19-s3-pulse-aa",
                                                  "over_negctl.txt"),
                    help="where to write the transcript as evidence")
    ap.add_argument("--keep", action="store_true", help="do not delete the scratch tree")
    args = ap.parse_args(argv)
    os.chdir(ROOT)

    if not os.path.isdir(INCLUDE_SRC):
        sys.stderr.write("TOOL FAILURE: %s is missing\n" % INCLUDE_SRC)
        return 2

    before = tree_hashes()
    before_status = subprocess.run(["git", "status", "--short"], capture_output=True, text=True).stdout

    scratch = os.path.abspath(args.scratch)
    if os.path.exists(scratch):
        shutil.rmtree(scratch)
    os.makedirs(scratch)
    out_dir = os.path.join(scratch, "out")
    os.makedirs(out_dir)

    transcript = []
    verdicts = []
    results = []

    def say(line=""):
        print(line)
        transcript.append(line)

    say("task #118 (GH #19 S3) — negative controls for the OUT-OF-DOMAIN and REACHABLE-BOX checks")
    say("scratch              %s" % scratch)
    say("candidate worktree   %s" % ROOT)
    say("runner under test    tools/run_gh19_s3_kernel_sweep.py")
    say("")
    say("verdict rule: RED requires the arm to BUILD, the driver to EXIT 0 and the NAMED criterion")
    say("to fail. A compile error, a dead driver, a missing criterion, a missed prediction or a")
    say("broken isolation prediction is INVALID and is never counted as a hit.")
    say("")

    # ---- declaration preflight -------------------------------------------------------------------
    # Before any arm runs: every string this file matches on must exist in the runner or the driver,
    # or the declaration is stale and the control it belongs to covers nothing. Done first because a
    # stale `forbidden` is invisible in the verdict -- it simply never fires, and the control reads
    # as isolated when it is not.
    say("-- declaration preflight: every criterion/reach/forbidden string must exist in the "
        "producer --")
    try:
        n_decl, decl_problems = label_preflight()
    except OSError as e:
        n_decl, decl_problems = 0, ["the runner/driver source could not be read: %s" % e]
    if decl_problems:
        for p in decl_problems:
            say("  *** %s ***" % p)
    else:
        say("  all %d declared strings present in tools/run_gh19_s3_kernel_sweep.py or %s"
            % (n_decl, SWEEP_SRC))
    say("")

    # ---- control group A -------------------------------------------------------------------------
    say("-- group A: header mutations (each on its own COPY; no tracked file is touched) --")
    for m in MUTATIONS:
        print("\n[%s] %s" % (m["name"], "RED expected" if m["expect"] == "red" else "GREEN expected"))
        inc = os.path.join(scratch, m["name"], "include")
        os.makedirs(os.path.dirname(inc), exist_ok=True)
        shutil.copytree(INCLUDE_SRC, inc)
        detail = ""
        if m["file"]:
            p = os.path.join(inc, "lunar24", "core", m["file"])
            if not os.path.exists(p):
                detail = "%s is not in the include copy" % m["file"]
            else:
                detail = apply_edits(p, m["edits"], m["name"]) or ""
        out_path = os.path.join(out_dir, m["name"] + ".txt")
        binary = os.path.join(scratch, "sweep_" + m["name"])
        if detail:
            kind, rc, out, err = "INVALID", -1, "", detail
        else:
            _, rc, out, err = run_arm(m["name"], inc, ROOT, out_path, binary)
            kind, reasons = judge(m["name"], m["expect"], rc, out, err,
                                  m.get("criterion"), m.get("reach", ()), m.get("forbidden", ()))
            detail = "; ".join(reasons)
        verdicts.append((m["name"], kind))
        results.append((m, kind, out, detail))
        say("")
        say("[%s] expect=%s -> %s  rc=%d" % (m["name"], m["expect"].upper(), kind, rc))
        say("  why: %s" % m["why"].replace("\n", " "))
        if detail:
            say("  %s" % detail)
        # The arm's own failure lines, verbatim: the verdict above is a summary of them, and the
        # evidence for WHICH check fired has to travel with the summary rather than live only in a
        # scratch directory that is deleted at the end of the run.
        fired = [ln for ln in out.splitlines() if ln.startswith("***")]
        for ln in fired[:6]:
            say("  | %s" % ln)
        if len(fired) > 6:
            say("  | ... and %d more *** lines (full stdout is the runner's own --out capture)"
                % (len(fired) - 6))

    # ---- control group B: instrument controls ----------------------------------------------------
    say("")
    say("-- group B: INSTRUMENT controls (these do NOT claim a defect; they ask whether the check "
        "can see anything) --")
    scan_dir = os.path.join(scratch, "scan")
    os.makedirs(os.path.join(scan_dir, "negctl_planted"), exist_ok=True)
    with open(os.path.join(scan_dir, PLANTED_CALLER), "w") as fh:
        fh.write(PLANTED_CALLER_TEXT)
    with open(os.path.join(scan_dir, PLANTED_MENTION), "w") as fh:
        fh.write(PLANTED_MENTION_TEXT)

    # B1: the scan sees a planted CALL. RED on the FM criterion is the correct outcome for this arm
    # -- it is an instrument control standing on a deliberately broken input, and it is the only
    # thing in this file that shows `fm_scan` is not returning [] unconditionally.
    inc_pristine = os.path.join(scratch, "pristine_copy", "include")
    if not os.path.isdir(inc_pristine):
        # Explicit rather than relying on group A having run first: the instrument controls must be
        # readable on their own, and an implicit ordering dependency is exactly the kind of thing
        # that turns one arm's failure into a different arm's INVALID.
        shutil.copytree(INCLUDE_SRC, inc_pristine)
    out_b1 = os.path.join(out_dir, "fm_caller_planted.txt")
    _, rc_b1, out_b1_text, err_b1 = run_arm("fm_caller_planted", inc_pristine, scan_dir, out_b1,
                                            os.path.join(scratch, "sweep_fm_caller_planted"))
    b1_kind, b1_reasons = judge("fm_caller_planted", "red", rc_b1, out_b1_text, err_b1,
                               "*** FM SETTER GAINED A CALLER")
    # The mention-only file must be REPORTED (as a mention) and must NOT be counted as a call: that
    # is the whole distinction the check rests on, and a scan that collapsed the two would fire this
    # criterion on the production tree, where the setters are named in prose.
    b1_mentions = [ln for ln in out_b1_text.splitlines() if ln.startswith("BOX-SRC fm")]
    if not any("planted_mention.h" in ln for ln in b1_mentions):
        b1_kind = "INVALID"
        b1_reasons.append("the mention-only planted file was not reported as a mention, so the scan "
                          "is not walking what it claims to walk")
    if not any("planted_caller.cpp" in ln for ln in b1_mentions):
        b1_kind = "INVALID"
        b1_reasons.append("the planted CALL file was not reported at all")
    verdicts.append(("fm_caller_planted", b1_kind))
    say("")
    say("[fm_caller_planted] expect=RED (instrument) -> %s  rc=%d" % (b1_kind, rc_b1))
    say("  why: a scratch tree with one planted `.setFmCv(` member call and one prose-only mention. "
        "The scan must find the call (which is what makes the production run's empty result a "
        "measurement rather than a silence) and must NOT count the mention as a call.")
    say("  planted entries seen: %s" % (b1_mentions or ["none"]))
    for r in b1_reasons:
        say("  %s" % r)

    # B2: the scan pointed at a directory with nothing in it must report nothing -- and that is
    # precisely why the production arm's empty result had to be checked against the real tree, not
    # against an empty one.
    empty_dir = os.path.join(scratch, "scan_empty")
    os.makedirs(empty_dir, exist_ok=True)
    out_b2 = os.path.join(out_dir, "fm_scan_blind.txt")
    _, rc_b2, out_b2_text, err_b2 = run_arm("fm_scan_blind", inc_pristine, empty_dir, out_b2,
                                            os.path.join(scratch, "sweep_fm_scan_blind"))
    b2_mentions = [ln for ln in out_b2_text.splitlines() if ln.startswith("BOX-SRC fm")]
    b2_calls = "FM SETTER GAINED A CALLER" in out_b2_text
    if rc_b2 != 0 or b2_mentions or b2_calls:
        b2_kind = "INVALID"
        b2_reasons = ["an empty scan root must produce rc=0 with no mention lines and no call; got "
                      "rc=%d, %d mention line(s), call_fired=%s" % (rc_b2, len(b2_mentions), b2_calls)]
    else:
        b2_kind, b2_reasons = "INSTRUMENT-OK", []
    verdicts.append(("fm_scan_blind", b2_kind))
    say("")
    say("[fm_scan_blind] expect=GREEN (instrument) -> %s  rc=%d" % (b2_kind, rc_b2))
    say("  why: the same pristine headers scanned against an EMPTY root. This is the arm that proves "
        "B1's result comes from the planted file and not from the scan firing unconditionally.")
    for r in b2_reasons:
        say("  %s" % r)

    # B3: the production scan. No planted caller, the real tree: the arm the FM claim is actually
    # about. It is the reference the whole FM argument rests on, so it is printed here as a
    # measurement rather than left implicit in the group-A runs.
    prod_calls = []
    for m, kind, out, detail in results:
        if m["name"] != "pristine_copy":
            continue
        prod_calls = [ln for ln in out.splitlines() if ln.startswith("BOX-SRC fm")]
        prod_fired = "FM SETTER GAINED A CALLER" in out
        if kind != "GREEN-OK" or prod_fired:
            verdicts.append(("fm_scan_production", "INVALID"))
            say("")
            say("[fm_scan_production] -> INVALID: the pristine arm did not come out clean on the FM "
                "check (kind=%s, call_fired=%s)" % (kind, prod_fired))
        else:
            verdicts.append(("fm_scan_production", "INSTRUMENT-OK"))
            say("")
            say("[fm_scan_production] -> INSTRUMENT-OK: the real tree reports %d setter "
                "mention(s) and 0 callers" % len(prod_calls))
            for ln in prod_calls:
                say("  %s" % ln)

    # ---- the worktree guard -----------------------------------------------------------------------
    say("")
    say("-- worktree guard --")
    after = tree_hashes()
    moved = [r for r in TRACKED if before.get(r) != after.get(r)]
    after_status = subprocess.run(["git", "status", "--short"], capture_output=True, text=True).stdout
    guard_ok = True
    if moved:
        guard_ok = False
        say("*** A TRACKED FILE MOVED DURING THIS RUN: %s. The mutations were supposed to happen "
            "only in the scratch copy. ***" % ", ".join(moved))
    else:
        say("all %d candidate files byte-identical before and after (%s)"
            % (len(TRACKED), ", ".join(os.path.basename(r) for r in TRACKED)))
    if before_status != after_status:
        guard_ok = False
        say("*** `git status --short` changed during this run:\nbefore:\n%s\nafter:\n%s ***"
            % (before_status, after_status))
    else:
        say("`git status --short` unchanged (no tracked file created, modified or removed)")
    say("HEAD %s" % subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                                   text=True).stdout.strip())

    # ---- verdict ---------------------------------------------------------------------------------
    say("")
    say("-- verdict --")
    hits = [n for n, k in verdicts if k == "RED"]
    ok_kinds = {"GREEN-OK", "INSTRUMENT-OK"}
    clean = [n for n, k in verdicts if k in ok_kinds]
    bad = [(n, k) for n, k in verdicts if k == "INVALID"]
    if decl_problems:
        # Not a per-arm verdict: a stale declaration invalidates the file's coverage claim, so it is
        # reported in the same list and counted in the same total.
        bad = [("declaration_preflight", "INVALID")] + bad
    for n, k in verdicts:
        say("  %-22s %s" % (n, k))
    say("")
    say("RED %d / clean %d / INVALID %d" % (len(hits), len(clean), len(bad)))
    if bad:
        say("*** %d control(s) did not land in their declared class: %s. An INVALID control is a "
            "TOOL problem and is never counted as a hit. ***"
            % (len(bad), ", ".join("%s=%s" % (n, k) for n, k in bad)))
    if not guard_ok:
        say("*** the worktree guard failed; nothing measured here is attributable. ***")

    if args.out:
        d = os.path.dirname(args.out)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(args.out, "w") as fh:
            fh.write("\n".join(transcript) + "\n")
        print("captured to %s" % args.out)

    if not args.keep:
        shutil.rmtree(scratch, ignore_errors=True)

    return 0 if (not bad and guard_ok and hits) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
