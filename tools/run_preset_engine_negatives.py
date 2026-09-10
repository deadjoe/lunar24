#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_preset_engine_negatives.py — task #103 (GH #12) NEGATIVE CONTROLS for the engine-layer
# preset LOAD / SAVE / INITIALISE acceptance (tests/host/test_preset_engine_actions.cpp).
#
# The acceptance must DISCRIMINATE: GREEN on the real product and RED when a production source is
# mutated away from the contract. Each mutation is applied to an ISOLATED shadow include tree that
# shadows the real header via -I ordering; nothing in the committed tree is ever modified.
#
# TRUST MODEL (@Codex msg b9d8ff9f). A control is only evidence if the run it observed was a
# COMPLETE, NORMAL red run of THIS acceptance. So every observation is parsed AND cross-checked
# before it is judged, and an observation is discarded as untrustworthy when any of these hold:
#   * the acceptance did not build              — a compile failure is NOT a red control;
#   * the exit code is not exactly 1            — a signal / abort / crash is NOT a red control;
#   * there is no complete termination summary  — the run may have stopped early;
#   * the summary disagrees with the printed check lines (a truncated / partial run);
#   * a label that must HOLD never printed a PASS line — "no FAIL line mentions it" is not
#     evidence that the criterion ran at all (the residual-FAIL trap);
#   * the expected RED label never printed a FAIL line.
# `--self-check` (run first, always) feeds synthetic observations to the SAME judge so a broken
# judge is caught before it can bless a broken control.
#
# Required controls (@Codex msg 0a602a4f):
#   1. no_behaviour_mirror      — drop the D-1 fix: write the canonical behaviour but not its
#                                 compatibility mirror parameters[keyboard_behaviour].
#   2. no_right_transfer        — drop the right scalar-bank write in load_live_side_bank.
#   3. slot_crosswire           — load_preset_to_live reads the WRONG slot (slot ^ 1).
#      bank_crosswire           — load_live_side_bank writes bank0/bank1 swapped (L/R cross).
#   4. save_reads_earlier_state — the SAVE branch captures a copy of an EARLIER committed state.
#   5. initialise_steals_load   — INITIALISE secretly also LOADs the target slot into live.
#   6. skip_real_apply          — report Accepted without going through applyDeviceState.
#      false_success_on_rejection — report Accepted even when the candidate was rejected
#                                 (paired with the --fixture below; alone it is a no-op because
#                                 no legal call rejects, which is exactly the point).
#
# Plus the constructed downstream-failure FIXTURE (not a defect — it makes a legal call fail):
#   load_produces_illegal_candidate — the recall writes an out-of-range scale editor, so the
#   candidate the owner hands to applyDeviceState is invalid. The D1 criterion must then observe
#   RejectedState + full atomicity, and the paired controls below must go RED on the named labels.
#   failure_still_commits — paired with the same fixture: the caller is told RejectedState but the
#   canonical state changed anyway (the ONE existing commit path is re-run on a repaired candidate).
#   This is the "失败仍 commit" symptom; D1's atomicity assertion must go RED.
#   runtime_mutated_on_rejection — paired with the same fixture: the rejection path mutates the
#   RUNNING owner (definition_->runtime().setVcoBaseHz), so the audio-preservation label must go
#   RED while the identity / trace / liveness labels stay PASS — the control is specific.
#
# POSIX-only (the driver invokes the compiler); on Windows the acceptance test still runs.
#
# Reproduction (from the repo root):
#   python3 tools/run_preset_engine_negatives.py
#   python3 tools/run_preset_engine_negatives.py --verbose
#
import argparse
import os
import re
import subprocess
import sys
import tempfile

TEST_SRC = "tests/host/test_preset_engine_actions.cpp"

# (repo-relative source path, include-relative shadow path). The shadow tree mirrors the INCLUDE
# path (what `#include <...>` names), because that is what the -I search resolves against.
PRESETS = ("core/include/lunar24/core/keyboard_presets.h", "lunar24/core/keyboard_presets.h")
SIDE_BANK = ("core/include/lunar24/core/keyboard_side_bank.h", "lunar24/core/keyboard_side_bank.h")
ENGINE = ("host/include/host/standalone_audio_engine.h", "host/standalone_audio_engine.h")

# The canonical/mirror write the D-1 fix added (task #102 §1.3).
D1_FIX = ("  live.keyboardSettings.pressureBehaviour = p.pressureBehaviour;\n"
          "  live.parameters[static_cast<IdValue>(ParameterId::keyboard_behaviour)] =\n"
          "      static_cast<double>(p.pressureBehaviour);\n")

SAVE_CASE = ("    case PresetAction::Save:\n"
             "      ok = lunar24::core::save_live_to_preset(candidate, slot);\n"
             "      break;\n")

INIT_CASE = ("    case PresetAction::Initialise:\n"
             "      ok = lunar24::core::initialise_preset(candidate, slot);\n"
             "      break;\n")

APPLY_TAIL = ("  const StateApplyStatus st =\n"
              "      applyDeviceState(candidate, sampleRate_, blockSize_, inputCapability_, "
              "outputCapability_);\n"
              "  return (st == StateApplyStatus::Accepted) ? PresetActionStatus::Accepted\n"
              "                                            : PresetActionStatus::RejectedState;\n")

SCALE_LOAD = "  live.keyboardScaleEditor = p.quantiseScaleEditor;\n"

BANK_LOAD_PAIR = ("    live.parameters[static_cast<IdValue>(id)] = left;\n"
                  "    live.keyboardScalarRight[i] = right;\n")

RIGHT_WRITE = "    live.keyboardScalarRight[i] = right;\n"

# The acceptance's own termination summaries — the only two shapes it may end with.
FAIL_SUMMARY_RE = re.compile(r"^\[preset engine actions\] (\d+)/(\d+) checks FAILED$", re.M)
OK_SUMMARY_RE = re.compile(r"^\[preset engine actions\] (\d+) checks OK$", re.M)


def _replace(text, needle, replacement, name):
    assert needle in text, "%s: needle not found" % name
    return text.replace(needle, replacement, 1)


def mut_no_behaviour_mirror(text, _name):
    return _replace(text, D1_FIX,
                    "  live.keyboardSettings.pressureBehaviour = p.pressureBehaviour;\n"
                    "  // MUTATION: the compatibility mirror is never written (pre-D-1 source).\n",
                    "no_behaviour_mirror")


def mut_no_right_transfer(text, _name):
    return _replace(text, RIGHT_WRITE, "    // MUTATION: the right scalar bank is never loaded.\n",
                    "no_right_transfer")


def mut_slot_crosswire(text, _name):
    return _replace(text, "  const KeyboardPreset& p = live.keyboardPresets[slot];",
                    "  const KeyboardPreset& p = live.keyboardPresets[slot ^ 1u];  // MUTATION: slot cross",
                    "slot_crosswire")


def mut_bank_crosswire(text, _name):
    return _replace(text, BANK_LOAD_PAIR,
                    "    live.parameters[static_cast<IdValue>(id)] = right;  // MUTATION: L/R cross\n"
                    "    live.keyboardScalarRight[i] = left;\n",
                    "bank_crosswire")


def mut_save_reads_earlier_state(text, _name):
    stale = ("    case PresetAction::Save: {\n"
             "      // MUTATION: SAVE reads a copy of an EARLIER committed state, not the live one.\n"
             "      static DeviceStateV1 staleState;\n"
             "      static bool haveStale = false;\n"
             "      if (!haveStale) { staleState = candidate; haveStale = true; }\n"
             "      DeviceStateV1 work = staleState;\n"
             "      ok = lunar24::core::save_live_to_preset(work, slot);\n"
             "      candidate = work;\n"
             "      break;\n"
             "    }\n")
    return _replace(text, SAVE_CASE, stale, "save_reads_earlier_state")


def mut_initialise_steals_load(text, _name):
    steal = ("    case PresetAction::Initialise:\n"
             "      ok = lunar24::core::initialise_preset(candidate, slot);\n"
             "      // MUTATION: a hidden LOAD makes the reset immediately audible.\n"
             "      if (ok) ok = lunar24::core::load_preset_to_live(candidate, slot);\n"
             "      break;\n")
    return _replace(text, INIT_CASE, steal, "initialise_steals_load")


def mut_skip_real_apply(text, _name):
    return _replace(text, APPLY_TAIL,
                    "  (void)candidate;  // MUTATION: the candidate/commit path is skipped.\n"
                    "  return PresetActionStatus::Accepted;\n",
                    "skip_real_apply")


def mut_false_success_on_rejection(text, _name):
    return _replace(text, APPLY_TAIL,
                    "  const StateApplyStatus st =\n"
                    "      applyDeviceState(candidate, sampleRate_, blockSize_, inputCapability_, "
                    "outputCapability_);\n"
                    "  (void)st;  // MUTATION: a rejected candidate is still reported accepted.\n"
                    "  return PresetActionStatus::Accepted;\n",
                    "false_success_on_rejection")


def mut_failure_still_commits(text, _name):
    """The observable 'failure still commits' defect: the caller is told RejectedState, but the
    canonical state changed anyway. Expressed on the ONE existing commit path (no second path is
    added): on a rejection the action re-applies a repaired candidate through that same path."""
    tail = ("  const StateApplyStatus st =\n"
            "      applyDeviceState(candidate, sampleRate_, blockSize_, inputCapability_, "
            "outputCapability_);\n"
            "  if (st != StateApplyStatus::Accepted) {\n"
            "    // MUTATION: a failed action still commits — the same single path is re-run on a\n"
            "    // repaired candidate, so the reported failure left the canonical state changed.\n"
            "    DeviceStateV1 repaired = candidate;\n"
            "    repaired.keyboardScaleEditor = 0u;\n"
            "    (void)applyDeviceState(repaired, sampleRate_, blockSize_, inputCapability_,\n"
            "                           outputCapability_);\n"
            "    return PresetActionStatus::RejectedState;\n"
            "  }\n"
            "  return PresetActionStatus::Accepted;\n")
    return _replace(text, APPLY_TAIL, tail, "failure_still_commits")


def mut_runtime_mutated_on_rejection(text, _name):
    """The 'the failed action still disturbed the running owner' defect (@Codex b9d8ff9f): the
    canonical state is untouched, so the old D1 could not see it, but the LIVE runtime is written
    through. Paired with the illegal-candidate fixture, so the rejection branch is the one taken."""
    tail = ("  const StateApplyStatus st =\n"
            "      applyDeviceState(candidate, sampleRate_, blockSize_, inputCapability_, "
            "outputCapability_);\n"
            "  if (st != StateApplyStatus::Accepted) {\n"
            "    // MUTATION: the rejection is reported, but the running owner was mutated anyway.\n"
            "    definition_->runtime().setVcoBaseHz(123.0);\n"
            "    return PresetActionStatus::RejectedState;\n"
            "  }\n"
            "  return PresetActionStatus::Accepted;\n")
    return _replace(text, APPLY_TAIL, tail, "runtime_mutated_on_rejection")


def fixture_load_produces_illegal_candidate(text, _name):
    return _replace(text, SCALE_LOAD,
                    SCALE_LOAD +
                    "  live.keyboardScaleEditor = 0xFFFFu;  // FIXTURE: force an illegal candidate.\n",
                    "load_produces_illegal_candidate")


# name -> (repo source path, include-relative shadow path, transform).
MUTATIONS = {
    "no_behaviour_mirror": (PRESETS[0], PRESETS[1], mut_no_behaviour_mirror),
    "no_right_transfer": (SIDE_BANK[0], SIDE_BANK[1], mut_no_right_transfer),
    "slot_crosswire": (PRESETS[0], PRESETS[1], mut_slot_crosswire),
    "bank_crosswire": (SIDE_BANK[0], SIDE_BANK[1], mut_bank_crosswire),
    "save_reads_earlier_state": (ENGINE[0], ENGINE[1], mut_save_reads_earlier_state),
    "initialise_steals_load": (ENGINE[0], ENGINE[1], mut_initialise_steals_load),
    "skip_real_apply": (ENGINE[0], ENGINE[1], mut_skip_real_apply),
    "false_success_on_rejection": (ENGINE[0], ENGINE[1], mut_false_success_on_rejection),
    "failure_still_commits": (ENGINE[0], ENGINE[1], mut_failure_still_commits),
    "runtime_mutated_on_rejection": (ENGINE[0], ENGINE[1], mut_runtime_mutated_on_rejection),
    "load_produces_illegal_candidate": (PRESETS[0], PRESETS[1], fixture_load_produces_illegal_candidate),
}

# Criteria that MUST have actually run in the retention positive control — each is one exact
# label the unmutated acceptance prints as a PASS line. A criterion that silently stopped
# executing can never be registered by a mutation going red, so the positive control proves the
# whole matrix is live (A: D-1, B: engine API, C1: 4 slots x 3 modes, C2: recalled scale,
# C3: clock-driven recalled seq, D1: accepted path).
REQUIRED_GREEN = [
    "A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)",
    "B4 the RIGHT side plays the recalled microtonal bank (0.04 V passes through)",
    "C1 slot 0 x Single: the recalled payload reaches the consumer",
    "C2 the recalled 0x0F0F mask quantises 0.30 V to 3/12 V",
    "C3 each clock edge publishes the recalled step (0, 7/12, 0, 7/12 V)",
    "D1 an accepted LOAD really committed the slot payload (no false success)",
]

# Single-source controls. `must_fail` = the label this mutation is DESIGNED to break;
# `also_fail` = labels from the newer criteria (task #103 C-matrix) that must ALSO observe it,
# which is how the new criteria are registered as evidence rather than decoration;
# `must_pass` = labels that must still hold (specificity, not a shotgun).
# The C-matrix payload carries a per-side CONSUMED scalar (the quantiser root: left C, right F),
# so the two per-side scalar-bank mutations (no_right_transfer, bank_crosswire) are observed by
# C1's published outputs too — the right bank's root reaches pressure_out in Split, and a crossed
# bank reaches vOct in Single.
SINGLE = {
    "no_behaviour_mirror": {
        "must_fail": ["A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)"],
        "also_fail": ["C1 "],
    },
    "no_right_transfer": {
        "must_fail": ["A2 the recalled live config"],
        "also_fail": ["C1 "],
    },
    "slot_crosswire": {
        "must_fail": ["B4 LOAD installs the slot payload into the live config"],
        "also_fail": ["C1 "],
    },
    "bank_crosswire": {
        "must_fail": ["A2 the recalled live config"],
        "also_fail": ["C1 "],
    },
    "save_reads_earlier_state": {
        "must_fail": ["B5 slot 1 holds config B"],
    },
    "initialise_steals_load": {
        "must_fail": ["B6 INITIALISE did NOT touch the live config (no implicit LOAD)"],
    },
    "skip_real_apply": {
        "must_fail": ["B4 LOAD installs the slot payload into the live config"],
        "also_fail": ["C1 "],
    },
}

# Paired experiment: the fixture makes the recalled candidate illegal, so the D1 failure branch is
# the one the paired mutation acts on.
PAIRED = {
    "fixture_candidate_failure_atomic": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": None,
        # With a correct owner the D1 failure branch holds. These labels must print as PASS lines
        # — not merely be absent from the FAIL set.
        "must_pass": [
            "D1 a downstream candidate failure after a legal preset action is reported as "
            "RejectedState",
            "D1 the rejected preset action is atomic",
            "D1 the rejected action keeps the SAME runtime object",
            "D1 the rejected action preserves the pending-event progress",
            "D1 the rejected action preserves all four audio channels",
        ],
        "must_fail": [],
    },
    "false_success_on_rejected_candidate": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": "false_success_on_rejection",
        "must_fail": [
            "D1 an accepted LOAD really committed the slot payload (no false success)",
        ],
        # No must_pass here on purpose: this mutation makes the owner take the ACCEPTED branch on
        # an illegal candidate, so the whole failure branch (including "reported as RejectedState")
        # is unreachable. Requiring that label to PASS would be requiring the mutation to be a
        # no-op; the earlier label is what catches it.
        "must_pass": [],
    },
    "failure_still_commits_on_rejected_candidate": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": "failure_still_commits",
        "must_fail": [
            "D1 the rejected preset action is atomic",
        ],
        "must_pass": [
            "D1 a downstream candidate failure after a legal preset action is reported as "
            "RejectedState",
        ],
    },
    "runtime_mutated_on_rejected_candidate": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": "runtime_mutated_on_rejection",
        "must_fail": [
            "D1 the rejected action preserves all four audio channels",
        ],
        # The rejection is still reported, the runtime OBJECT is the same, and the control trace is
        # untouched — the mutation is caught by exactly one criterion, not by a shotgun.
        "must_pass": [
            "D1 a downstream candidate failure after a legal preset action is reported as "
            "RejectedState",
            "D1 the rejected action keeps the SAME runtime object",
            "D1 the rejected action preserves the pending-event progress",
        ],
    },
}


class Observation:
    """One build-and-run of the acceptance, parsed AND cross-checked.

    `problems` records why this observation cannot be trusted as a control; the judge refuses to
    draw a conclusion from an observation that has any."""

    def __init__(self, res):
        self.rc = res.get("rc")
        self.passed = []
        self.failed = []
        self.summary = None        # "failed" | "ok" | None
        self.failed_count = None
        self.total_count = None
        self.problems = []

        if res.get("compile_error") is not None:
            self.problems.append("the acceptance did not BUILD (a compile failure is NOT a red "
                                 "control)")
            return

        out = res.get("out", "")
        err = res.get("err", "")
        for line in out.splitlines():
            if "[PASS]" in line:
                self.passed.append(line.split("[PASS]", 1)[1].strip())
            elif "[FAIL]" in line:
                self.failed.append(line.split("[FAIL]", 1)[1].strip())

        m = FAIL_SUMMARY_RE.search(err) or FAIL_SUMMARY_RE.search(out)
        if m:
            self.summary = "failed"
            self.failed_count, self.total_count = int(m.group(1)), int(m.group(2))
        else:
            m = OK_SUMMARY_RE.search(out)
            if m:
                self.summary = "ok"
                self.total_count = int(m.group(1))
            else:
                self.problems.append("the run printed NO complete termination summary "
                                     "('[preset engine actions] N/M checks FAILED' or "
                                     "'... N checks OK')")

        if self.rc is None:
            self.problems.append("no exit code was observed")
        elif self.rc < 0:
            self.problems.append("the run was killed by signal %d (a negative-signal exit is NOT "
                                 "a red control)" % -self.rc)
        elif self.rc > 1:
            self.problems.append("the run exited %d (a normal red run exits exactly 1; a crash is "
                                 "NOT a red control)" % self.rc)

        printed = len(self.passed) + len(self.failed)
        if self.total_count is not None and self.total_count != printed:
            self.problems.append("the summary claims %d checks but %d check lines were printed "
                                 "(the run did not reach the end)" % (self.total_count, printed))
        if self.failed_count is not None and self.failed_count != len(self.failed):
            self.problems.append("the summary claims %d failed checks but %d FAIL lines were "
                                 "printed (truncated / interleaved output)"
                                 % (self.failed_count, len(self.failed)))
        if self.summary == "ok" and self.failed:
            self.problems.append("the run reports OK but also printed FAIL lines")

    def trusted(self):
        return not self.problems


def judge_red(obs, must_fail, must_pass):
    """A red control must be TRUSTED, exit exactly 1, and hit exactly the labels named."""
    problems = list(obs.problems)
    if obs.rc != 1:
        problems.append("exit code is %s, not exactly 1" % (obs.rc,))
    for want in must_fail:
        if not any(want in l for l in obs.failed):
            problems.append("the expected RED label did not appear as a FAIL line: '%s'" % want)
    for want in must_pass:
        if not any(want in l for l in obs.passed):
            problems.append("the label that must still HOLD never printed a PASS line: '%s'" % want)
    return problems


def judge_green(obs, must_pass):
    problems = list(obs.problems)
    if obs.rc != 0:
        problems.append("exit code is %s, not 0" % (obs.rc,))
    if obs.summary != "ok":
        problems.append("no 'checks OK' termination summary")
    if obs.failed:
        problems.append("%d FAIL lines in the retention positive control" % len(obs.failed))
    for want in must_pass:
        if not any(want in l for l in obs.passed):
            problems.append("criterion did not run (no PASS line): '%s'" % want)
    return problems


def _synth(rc, passed, failed, summary="failed", total=None, failed_count=None,
           compile_error=None):
    """Build a synthetic build-and-run result for the judge's own self-check."""
    if compile_error is not None:
        return {"compile_error": compile_error}
    out = "".join("  [PASS] %s\n" % l for l in passed)
    out += "".join("  [FAIL] %s\n" % l for l in failed)
    err = ""
    if summary == "failed":
        err = "[preset engine actions] %d/%d checks FAILED\n" % (
            len(failed) if failed_count is None else failed_count,
            (len(passed) + len(failed)) if total is None else total)
    elif summary == "ok":
        out += "[preset engine actions] %d checks OK\n" % (
            len(passed) if total is None else total)
    return {"rc": rc, "out": out, "err": err}


# (kind, name, synthetic result, must_fail, must_pass, the judge MUST report problems).
# Every entry is a way the old driver could have blessed a broken control.
SELF_CHECK = [
    ("red", "a clean red run that hits its label is TRUSTED",
     _synth(1, ["A1 ok"], ["B4 the label"]), ["B4 the label"], ["A1 ok"], False),
    ("red", "a negative-signal exit is rejected",
     _synth(-6, ["A1 ok"], ["B4 the label"]), ["B4 the label"], ["A1 ok"], True),
    ("red", "an abort exit (>1) is rejected",
     _synth(134, ["A1 ok"], ["B4 the label"]), ["B4 the label"], ["A1 ok"], True),
    ("red", "a missing termination summary is rejected",
     _synth(1, ["A1 ok"], ["B4 the label"], summary=None), ["B4 the label"], ["A1 ok"], True),
    ("red", "a summary that disagrees with the printed check lines is rejected",
     _synth(1, ["A1 ok"], ["B4 the label"], total=99), ["B4 the label"], ["A1 ok"], True),
    ("red", "a summary that disagrees with the printed FAIL count is rejected",
     _synth(1, ["A1 ok"], ["B4 the label"], failed_count=3), ["B4 the label"], ["A1 ok"], True),
    ("red", "residual-only FAIL (the must-hold label never ran) is rejected",
     _synth(1, [], ["B4 the label"]), ["B4 the label"], ["A1 ok"], True),
    ("red", "a missing expected RED label is rejected",
     _synth(1, ["A1 ok"], ["something else"]), ["B4 the label"], ["A1 ok"], True),
    ("red", "a compile failure is never a red control",
     _synth(1, [], [], compile_error="error: no member named 'runtime'"), ["B4 the label"], [],
     True),
    ("green", "a clean green run is accepted",
     _synth(0, ["A1 ok"], [], summary="ok"), [], ["A1 ok"], False),
    ("green", "a green run with a FAIL line is rejected",
     _synth(0, ["A1 ok"], ["B4 the label"], summary="ok"), [], ["A1 ok"], True),
    ("green", "a green run whose criterion never ran is rejected",
     _synth(0, ["something"], [], summary="ok"), [], ["A1 ok"], True),
]


def apply_chain(root, names):
    """Return {include-relative header path: mutated text} after applying `names` in order."""
    texts = {}
    for name in names:
        if name is None:
            continue
        src, dst, transform = MUTATIONS[name]
        if dst not in texts:
            texts[dst] = open(os.path.join(root, src)).read()
        texts[dst] = transform(texts[dst], name)
    return texts


def build_and_run(root, compiler, shadow_texts):
    with tempfile.TemporaryDirectory() as td:
        shadow = os.path.join(td, "shadow")
        for header, text in shadow_texts.items():
            dest = os.path.join(shadow, header)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "w") as fh:
                fh.write(text)
        binpath = os.path.join(td, "test_preset_engine_actions")
        cmd = [compiler, "-O1", "-std=c++17",
               "-I", shadow,
               "-I", os.path.join(root, "core/include"),
               "-I", os.path.join(root, "generated"),
               "-I", os.path.join(root, "host/include"),
               "-I", os.path.join(root, "tests/host"),
               os.path.join(root, TEST_SRC), "-o", binpath]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return {"compile_error": (proc.stdout + proc.stderr)[-4000:]}
        run = subprocess.run([binpath], capture_output=True, text=True, timeout=900)
        return {"rc": run.returncode, "out": run.stdout, "err": run.stderr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--compiler", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    compiler = args.compiler or os.environ.get("CXX", "c++")
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("  PASS " if cond else "  FAIL ") + msg)
        ok = ok and cond

    print("== TOOL SELF-CHECK (the judge must reject untrustworthy observations) ==")
    for kind, name, res, must_fail, must_pass, want_problems in SELF_CHECK:
        obs = Observation(res)
        problems = judge_red(obs, must_fail, must_pass) if kind == "red" \
            else judge_green(obs, must_pass)
        got = bool(problems)
        detail = "" if got == want_problems else "  -> got %s" % (problems or "no problems")
        check(got == want_problems, "self-check (%s): %s%s" % (kind, name, detail))
    if not ok:
        print("\nOVERALL: FAIL (the judge itself is not trustworthy; no control was run)")
        return 1

    print("\n== RETENTION POSITIVE CONTROL (no mutation) ==")
    real = Observation(build_and_run(root, compiler, {}))
    problems = judge_green(real, REQUIRED_GREEN)
    check(not problems, "the unmutated acceptance is GREEN (rc=0, complete summary, every "
                        "required criterion printed a PASS line)")
    for p in problems:
        print("        %s" % p)
    if problems:
        print("\nOVERALL: FAIL (the product baseline is not green; controls are meaningless)")
        return 1

    print("\n== SINGLE-SOURCE NEGATIVE CONTROLS ==")
    for name, spec in SINGLE.items():
        obs = Observation(build_and_run(root, compiler, apply_chain(root, [name])))
        problems = judge_red(obs, spec["must_fail"], spec.get("must_pass", []))
        check(not problems, "%s: RED for the named reason (rc=1, complete summary, expected FAIL "
                            "line present)" % name)
        for p in problems:
            print("        %s" % p)
        for want in spec.get("also_fail", []):
            check(any(want in l for l in obs.failed),
                  "%s: the newer criterion family '%s' also observes the mutation" % (name, want))
        if args.verbose:
            for l in obs.failed:
                print("        FAIL: %s" % l)

    print("\n== CONSTRUCTED DOWNSTREAM-FAILURE ATOMICITY (paired) ==")
    for name, spec in PAIRED.items():
        obs = Observation(build_and_run(root, compiler,
                                        apply_chain(root, [spec["fixture"], spec["mutation"]])))
        problems = judge_red(obs, spec["must_fail"], spec["must_pass"])
        check(not problems, "%s: RED for the named reason (rc=1, complete summary, every "
                            "must-hold label printed a PASS line)" % name)
        for p in problems:
            print("        %s" % p)
        if args.verbose:
            for l in obs.failed:
                print("        FAIL: %s" % l)

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
