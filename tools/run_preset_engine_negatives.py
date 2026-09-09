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
#   RejectedState + full atomicity, and the paired false_success_on_rejection control must go RED.
#
# POSIX-only (the driver invokes the compiler); on Windows the acceptance test still runs.
#
# Reproduction (from the repo root):
#   python3 tools/run_preset_engine_negatives.py
#   python3 tools/run_preset_engine_negatives.py --verbose
#
import argparse
import os
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
    "load_produces_illegal_candidate": (PRESETS[0], PRESETS[1], fixture_load_produces_illegal_candidate),
}

# Each control must terminate NORMALLY (no crash / no timeout) and hit its named assertion.
# label -> a substring that must appear on a FAIL line.
EXPECTED = {
    "no_behaviour_mirror":
        "A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)",
    "no_right_transfer":
        "A2 the recalled live config",
    "slot_crosswire":
        "B4 LOAD installs the slot payload into the live config",
    "bank_crosswire":
        "A2 the recalled live config",
    "save_reads_earlier_state":
        "B5 slot 1 holds config B",
    "initialise_steals_load":
        "B6 INITIALISE did NOT touch the live config (no implicit LOAD)",
    "skip_real_apply":
        "B4 LOAD installs the slot payload into the live config",
}

# Paired experiment: the fixture makes the recalled candidate illegal; the paired control then
# reports success without a commit. D1 must go RED on the "no false success" assertion.
PAIRED = {
    "fixture_candidate_failure_atomic": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": None,
        # With a correct owner the D1 failure branch holds: the run is RED overall (other LOAD
        # criteria fail) but the two D1 labels must PASS.
        "must_pass_labels": [
            "D1 a downstream candidate failure after a legal preset action is reported as "
            "RejectedState",
            "D1 the rejected preset action is atomic",
        ],
    },
    "false_success_on_rejected_candidate": {
        "fixture": "load_produces_illegal_candidate",
        "mutation": "false_success_on_rejection",
        "must_fail_labels": [
            "D1 an accepted LOAD really committed the slot payload (no false success)",
        ],
    },
}


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


def failed_labels(res):
    labels = []
    for line in res.get("out", "").splitlines():
        if "[FAIL]" in line:
            labels.append(line.split("[FAIL]", 1)[1].strip())
    return labels


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

    print("== RETENTION POSITIVE CONTROL (no mutation) ==")
    real = build_and_run(root, compiler, {})
    if "compile_error" in real:
        check(False, "real build failed:\n%s" % real["compile_error"])
        return 1
    check(real["rc"] == 0, "real product is GREEN (rc=0)")
    check("checks OK" in real["out"], "real product reports all checks OK")

    print("\n== SINGLE-SOURCE NEGATIVE CONTROLS ==")
    for name in EXPECTED:
        res = build_and_run(root, compiler, apply_chain(root, [name]))
        if "compile_error" in res:
            check(False, "%s: RED — compile failed:\n%s" % (name, res["compile_error"]))
            continue
        labels = failed_labels(res)
        hit = [l for l in labels if EXPECTED[name] in l]
        check(res["rc"] != 0, "%s: terminated normally with a non-zero exit (%d)" % (name, res["rc"]))
        check(bool(hit), "%s: hit its assertion -> %s" % (name, EXPECTED[name]))
        if args.verbose:
            for l in labels:
                print("        FAIL: %s" % l)
        elif not hit:
            for l in labels:
                print("        observed FAIL: %s" % l)

    print("\n== CONSTRUCTED DOWNSTREAM-FAILURE ATOMICITY ==")
    for name, spec in PAIRED.items():
        res = build_and_run(root, compiler, apply_chain(root, [spec["fixture"], spec["mutation"]]))
        if "compile_error" in res:
            check(False, "%s: build failed:\n%s" % (name, res["compile_error"]))
            continue
        labels = failed_labels(res)
        check(res["rc"] != 0, "%s: the run is RED (rc=%d)" % (name, res["rc"]))
        for want in spec.get("must_pass_labels", []):
            check(not any(want in l for l in labels),
                  "%s: '%s' holds" % (name, want))
        for want in spec.get("must_fail_labels", []):
            check(any(want in l for l in labels),
                  "%s: '%s' goes RED" % (name, want))
        if args.verbose:
            for l in labels:
                print("        FAIL: %s" % l)

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
