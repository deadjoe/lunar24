#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_app_state_negatives.py — task #105 (GH #12) NEGATIVE CONTROLS for the APP state-persistence
# acceptance (tests/host/test_app_state_store.cpp).
#
# The acceptance must DISCRIMINATE: GREEN on the real product and RED when a production source is
# mutated away from the contract. Each mutation is applied to an ISOLATED shadow include tree that
# shadows the real header via -I ordering; nothing in the committed tree is ever modified.
#
# TRUST MODEL (the same one @Codex b9d8ff9f accepted for the preset-engine controls). A control is
# only evidence if the run it observed was a COMPLETE, NORMAL red run of THIS acceptance. So every
# observation is parsed AND cross-checked before it is judged, and an observation is discarded as
# untrustworthy when any of these hold:
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
# Required controls (@Codex b2af0b4, "负控(隔离源)"):
#   1. skip_startup_apply       — publishPending reports Accepted without handing the candidate to
#                                 the engine: the startup restore never reaches the canonical state.
#   2. reread_every_boundary    — the explicit session latch is removed, so a device reopen reads
#                                 the disk again (and observes a replaced file).
#   3. drop_pending             — captureCanonical() stops retaining the transfer payload, so the
#                                 session config is lost at the next stopped-stream boundary.
#   4. bad_file_overwritten_on_exit — saveImpl() ignores the lifecycle-save gate, so the exit save
#                                 overwrites a file that was present but unusable.
#   5. short_write_lies         — the REAL write adapter writes half the bytes and still reports a
#                                 complete write (the mandated interface-violation boundary).
#   6. delete_old_file_first    — the REAL replace adapter removes the destination BEFORE the
#                                 rename, so a failed replace has already destroyed the live file.
#   7. save_result_swallowed    — the typed SaveResult is collapsed into "Saved", so a failed save
#                                 is reported as success to the host.
#   8. save_writes_default_without_legal_config — with no committed canonical and no retained legal
#                                 config the store writes the member bank instead of reporting
#                                 SkippedNoConfig (mandate §5 "no successful legal config -> don't
#                                 write").
#
# HELD controls (NOT run here; reported, never counted as a pass):
#   9. exit_call_missing / exit_call_before_closeaudio — both are structural mutations of
#      host/iPlug_app_host_override.cpp (~IPlugAPPHost) and are judged by the W16 static gate.
#      That file's upstream->fork diff is pinned whole by tools/check_host_override_drift.py
#      (PIN + EXPECTED_DIFF_HASH["host"]), and the gate's own rule reserves regeneration of that
#      hash to an @Codex ruling on a curated hunk. The hunk is written and ready; it is HELD
#      pending that ruling (msg 1bc54368). `--require-all` turns the held controls fatal so the
#      complete state can be enforced once the ruling lands.
#
# POSIX-only (the driver invokes the compiler); on Windows the acceptance test still runs.
#
# Reproduction (from the repo root):
#   python3 tools/run_app_state_negatives.py
#   python3 tools/run_app_state_negatives.py --verbose
#   python3 tools/run_app_state_negatives.py --require-all
#
import argparse
import os
import re
import subprocess
import sys
import tempfile

TEST_SRC = os.path.join("tests", "host", "test_app_state_store.cpp")

# (repo-relative source, include-relative shadow path) for each mutated production source.
STORE = ("host/include/host/app_state_store.h", "host/app_state_store.h")

FAIL_SUMMARY_RE = re.compile(r"^\[app state store\] (\d+)/(\d+) checks FAILED", re.M)
OK_SUMMARY_RE = re.compile(r"^\[app state store\] (\d+) checks OK", re.M)


def _replace(text, needle, replacement, name):
    """Replace exactly one occurrence, and FAIL LOUDLY if the needle drifted away.

    A silently missing needle would produce an unmutated shadow tree — a control that "passes"
    without testing anything. So a missing or ambiguous needle is a hard error, not a warning."""
    if text.count(needle) != 1:
        raise SystemExit(
            "control '%s': the mutation anchor appears %d times (expected exactly 1); the "
            "production source drifted — fix the anchor before trusting this control"
            % (name, text.count(needle)))
    return text.replace(needle, replacement)


# ---- mutations (each one is a defect the acceptance MUST observe) ------------------------------

PUBLISH_APPLY = ("    const auto status = engine.applyDeviceState(candidate, sampleRate, "
                 "maxBlockSize, inputCapability,\n"
                 "                                                outputCapability);")


def mut_skip_startup_apply(text, name):
    """The restore is never handed to the engine: the boundary reports Accepted anyway."""
    return _replace(text, PUBLISH_APPLY,
                    "    (void)engine;  // MUTATION: skip_startup_apply — no real candidate.\n"
                    "    const auto status = StandaloneAudioEngine::StateApplyStatus::Accepted;\n",
                    name)


def mut_reread_every_boundary(text, name):
    """The session latch is gone: every boundary reads the disk again."""
    return _replace(text, "    if (restoreAttempted_) return loadOutcome_;",
                    "    // MUTATION: reread_every_boundary — the session latch is removed.\n"
                    "    if (false) return loadOutcome_;",
                    name)


CAPTURE_BODY = ("    const DeviceStateV1* canonical = engine.canonicalState();\n"
                "    if (canonical == nullptr) return;\n"
                "    pending_ = *canonical;\n"
                "    pendingOrigin_ = PendingOrigin::FromSession;\n"
                "    pendingValid_ = true;\n")


def mut_drop_pending(text, name):
    """The transfer payload is never captured, so a reopen loses the session config."""
    return _replace(text, CAPTURE_BODY,
                    "    (void)engine;  // MUTATION: drop_pending — nothing is retained.\n",
                    name)


def mut_bad_file_overwritten_on_exit(text, name):
    """The lifecycle-save gate is ignored: an unusable file gets overwritten on exit."""
    return _replace(text, "    if (!saveAllowed_) return StateSaveOutcome::SkippedFileUnhealthy;",
                    "    // MUTATION: bad_file_overwritten_on_exit — the gate is ignored.\n",
                    name)


REAL_WRITE_BODY = ("  const std::size_t wrote = (n == 0) ? 0u : std::fwrite(bytes, 1u, n, f);\n"
                   "  const int closed = std::fclose(f);\n"
                   "  return wrote == n && closed == 0;\n")


def mut_short_write_lies(text, name):
    """The REAL adapter writes half the bytes and reports a complete write (interface violation)."""
    return _replace(text, REAL_WRITE_BODY,
                    "  // MUTATION: short_write_lies — half the bytes, still reported as complete.\n"
                    "  const std::size_t wrote = (n == 0) ? 0u : std::fwrite(bytes, 1u, n / 2u, f);\n"
                    "  const int closed = std::fclose(f);\n"
                    "  return closed == 0;\n",
                    name)


REAL_REPLACE_BODY = ("  std::error_code ec;\n"
                     "  std::filesystem::rename(std::filesystem::path(from), "
                     "std::filesystem::path(to), ec);\n"
                     "  return !ec;\n")


def mut_delete_old_file_first(text, name):
    """The REAL replace removes the destination first, so a failed replace destroys the old file."""
    return _replace(text, REAL_REPLACE_BODY,
                    "  std::error_code ec;\n"
                    "  // MUTATION: delete_old_file_first — the destination is removed up front.\n"
                    "  std::filesystem::remove(std::filesystem::path(to), ec);\n"
                    "  std::filesystem::rename(std::filesystem::path(from), "
                    "std::filesystem::path(to), ec);\n"
                    "  return !ec;\n",
                    name)


SAVE_SOURCE = ("    const DeviceStateV1* source = engine.canonicalState();\n"
               "    if (source == nullptr) source = pendingValid_ ? &pending_ : nullptr;\n"
               "    if (source == nullptr) return StateSaveOutcome::SkippedNoConfig;\n")


def mut_save_writes_default_without_legal_config(text, name):
    """No committed canonical and no retained legal config still writes SOMETHING (the member
    default bank), instead of reporting SkippedNoConfig and leaving the disk alone."""
    return _replace(text, SAVE_SOURCE,
                    "    const DeviceStateV1* source = engine.canonicalState();\n"
                    "    // MUTATION: save_writes_default_without_legal_config — a null source "
                    "falls back\n"
                    "    // to the member bank instead of refusing to write.\n"
                    "    if (source == nullptr) source = &pending_;\n",
                    name)


SAVE_RESULT_SWITCH = (
    "    const SaveResult result = lunar24::core::save_state_atomic(wire.data(), written, "
    "temp.c_str(),\n"
    "                                                              live.c_str(), ops_, opsCtx_);\n"
    "    switch (result) {\n"
    "      case SaveResult::ok: return StateSaveOutcome::Saved;\n"
    "      case SaveResult::flush_failed: return StateSaveOutcome::FlushFailed;\n"
    "      case SaveResult::replace_failed: return StateSaveOutcome::ReplaceFailed;\n"
    "      case SaveResult::temp_write_failed: break;\n"
    "    }\n"
    "    return StateSaveOutcome::TempWriteFailed;\n")


def mut_save_result_swallowed(text, name):
    """Every typed failure is collapsed into Saved: the host can no longer see a failed save."""
    return _replace(text, SAVE_RESULT_SWITCH,
                    "    const SaveResult result = lunar24::core::save_state_atomic(wire.data(), "
                    "written, temp.c_str(),\n"
                    "                                                              live.c_str(), "
                    "ops_, opsCtx_);\n"
                    "    (void)result;  // MUTATION: save_result_swallowed — always Saved.\n"
                    "    return StateSaveOutcome::Saved;\n",
                    name)


# name -> (repo source path, include-relative shadow path, transform).
MUTATIONS = {
    "skip_startup_apply": (STORE[0], STORE[1], mut_skip_startup_apply),
    "reread_every_boundary": (STORE[0], STORE[1], mut_reread_every_boundary),
    "drop_pending": (STORE[0], STORE[1], mut_drop_pending),
    "bad_file_overwritten_on_exit": (STORE[0], STORE[1], mut_bad_file_overwritten_on_exit),
    "short_write_lies": (STORE[0], STORE[1], mut_short_write_lies),
    "delete_old_file_first": (STORE[0], STORE[1], mut_delete_old_file_first),
    "save_result_swallowed": (STORE[0], STORE[1], mut_save_result_swallowed),
    "save_writes_default_without_legal_config": (STORE[0], STORE[1],
                                                 mut_save_writes_default_without_legal_config),
}

# Criteria that MUST have actually run in the retention positive control — each is one exact label
# the unmutated acceptance prints as a PASS line. A criterion that silently stopped executing can
# never be registered by a mutation going red, so the positive control proves the matrix is live:
# C1 real round trip, C2 the one-read latch + transfer payload, C3 startup restore, C4 failure
# semantics, C5 the typed save matrix, C6 multi-instance, C7 the clean audio path.
REQUIRED_GREEN = [
    "C1.1 the saved file is EXACTLY the wire size",
    "C1.2 the saved bytes decode back to the saved state",
    "C1.5 a new store reads the file",
    "C2.1 the second boundary performed NO disk read",
    "C2.2 the second boundary still uses the session's file state (no re-read)",
    "C2.5 the boundary captured the last committed session config",
    "C2.3 the pending survives the failed prepare (clearState_ cannot lose the session)",
    "C3.1 a fresh instance restores the file state",
    "C4.9 the exit save did not overwrite the bad file",
    "C5.5 a save failure is reported, not swallowed",
    "C5.4 a failed replace did not remove the destination first",
    "C6.1 interleaved saves never expose a half-written live file",
    "C7.1 rendering performed no file operation at all",
    "C8.1 the retained legal config is the save source when no owner exists",
    "C8.2 no successful legal config means NO write",
]

# Single-source controls. `must_fail` = the label this mutation is DESIGNED to break;
# `also_fail` = further labels that must ALSO observe it, which is how the neighbouring criteria
# are registered as evidence rather than decoration; `must_pass` = labels that must still hold
# (specificity, not a shotgun — the mutation is one defect, not a broken run).
SINGLE = {
    "skip_startup_apply": {
        "must_fail": ["C3.1 a fresh instance restores the file state"],
        "also_fail": ["C2.6 the engine canonical is now the single authority"],
        # The mutation still clears the pending, so the control is "the apply was skipped",
        # not "publishPending became a no-op".
        "must_pass": ["C2.6 the pending is cleared once published"],
    },
    "reread_every_boundary": {
        "must_fail": ["C2.2 the second boundary still uses the session's file state (no re-read)"],
        "also_fail": ["C2.1 the second boundary performed NO disk read"],
        "must_pass": ["C2.1 the first read adopted the file state"],
    },
    "drop_pending": {
        "must_fail": ["C2.5 the boundary captured the last committed session config"],
        "also_fail": ["C2.3 the boundary captured the config before the illegal format"],
        "must_pass": ["C2.5 the session committed a changed config"],
    },
    "bad_file_overwritten_on_exit": {
        "must_fail": ["C4.9 a failed load blocks the lifecycle save"],
        "also_fail": ["C4.9 the exit save did not overwrite the bad file"],
        # The load verdict itself is untouched: only the save path stopped consulting it.
        "must_pass": ["C4.7 a failed load blocks the lifecycle save gate"],
    },
    "short_write_lies": {
        "must_fail": ["C1.1 the saved file is EXACTLY the wire size"],
        "also_fail": ["C1.2 the saved bytes decode back to the saved state"],
        # The honest boundary: the store reports the lie as a save, because the complete-write
        # check is writeFile's contract, not a second store-side truth.
        "must_pass": ["C1.1 the lifecycle save wrote the file"],
    },
    "delete_old_file_first": {
        "must_fail": ["C5.4 a failed replace did not remove the destination first"],
        "also_fail": ["C5.4 the real backend reported the replace failure"],
        # Normal saves are unaffected: the mutation targets only the replace failure path.
        "must_pass": ["C1.1 the saved file is EXACTLY the wire size"],
    },
    "save_result_swallowed": {
        "must_fail": ["C5.5 a save failure is reported, not swallowed"],
        "also_fail": ["C5.1 a temp write failure leaves the live file"],
        # The backend really did fail and really did leave the live bytes alone; only the
        # reporting was swallowed.
        "must_pass": ["C5 the failed save left the prior live bytes untouched",
                      "C5 the first save succeeded"],
    },
    "save_writes_default_without_legal_config": {
        "must_fail": ["C8.2 no legal config means SkippedNoConfig, not a written default"],
        "also_fail": ["C8.2 no successful legal config means NO write"],
        # The retained-config source is untouched: with a pending it still writes THAT.
        "must_pass": ["C8.1 the retained legal config is the save source when no owner exists"],
    },
}

# Controls that cannot run yet, with the exact reason. They are REPORTED, never counted as passes.
HELD = {
    "exit_call_missing": (
        "structural mutation of host/iPlug_app_host_override.cpp (~IPlugAPPHost) judged by the W16 "
        "static gate; that file's whole upstream->fork diff is pinned by "
        "tools/check_host_override_drift.py (PIN + EXPECTED_DIFF_HASH[\"host\"]), whose own rule "
        "reserves regeneration to an @Codex ruling — HELD on msg 1bc54368"),
    "exit_call_before_closeaudio": (
        "same file / same pinned-diff blocker as exit_call_missing: moving the save call before "
        "CloseAudio() is a second hunk in the same pinned diff"),
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
                                     "('[app state store] N/M checks FAILED' or "
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
        err = "[app state store] %d/%d checks FAILED\n" % (
            len(failed) if failed_count is None else failed_count,
            (len(passed) + len(failed)) if total is None else total)
    elif summary == "ok":
        out += "[app state store] %d checks OK\n" % (
            len(passed) if total is None else total)
    return {"rc": rc, "out": out, "err": err}


# (kind, name, synthetic result, must_fail, must_pass, the judge MUST report problems).
# Every entry is a way a broken judge could bless a broken control.
SELF_CHECK = [
    ("red", "a clean red run that hits its label is TRUSTED",
     _synth(1, ["C1.1 ok"], ["C2.5 the label"]), ["C2.5 the label"], ["C1.1 ok"], False),
    ("red", "a negative-signal exit is rejected",
     _synth(-6, ["C1.1 ok"], ["C2.5 the label"]), ["C2.5 the label"], ["C1.1 ok"], True),
    ("red", "an abort exit (>1) is rejected",
     _synth(134, ["C1.1 ok"], ["C2.5 the label"]), ["C2.5 the label"], ["C1.1 ok"], True),
    ("red", "a missing termination summary is rejected",
     _synth(1, ["C1.1 ok"], ["C2.5 the label"], summary=None), ["C2.5 the label"], ["C1.1 ok"],
     True),
    ("red", "a summary that disagrees with the printed check lines is rejected",
     _synth(1, ["C1.1 ok"], ["C2.5 the label"], total=99), ["C2.5 the label"], ["C1.1 ok"], True),
    ("red", "a summary that disagrees with the printed FAIL count is rejected",
     _synth(1, ["C1.1 ok"], ["C2.5 the label"], failed_count=3), ["C2.5 the label"], ["C1.1 ok"],
     True),
    ("red", "residual-only FAIL (the must-hold label never ran) is rejected",
     _synth(1, [], ["C2.5 the label"]), ["C2.5 the label"], ["C1.1 ok"], True),
    ("red", "a missing expected RED label is rejected",
     _synth(1, ["C1.1 ok"], ["something else"]), ["C2.5 the label"], ["C1.1 ok"], True),
    ("red", "a compile failure is never a red control",
     _synth(1, [], [], compile_error="error: no member named 'canonicalState'"),
     ["C2.5 the label"], [], True),
    ("green", "a clean green run is accepted",
     _synth(0, ["C1.1 ok"], [], summary="ok"), [], ["C1.1 ok"], False),
    ("green", "a green run with a FAIL line is rejected",
     _synth(0, ["C1.1 ok"], ["C2.5 the label"], summary="ok"), [], ["C1.1 ok"], True),
    ("green", "a green run whose criterion never ran is rejected",
     _synth(0, ["something"], [], summary="ok"), [], ["C1.1 ok"], True),
]


def apply_chain(root, names):
    """Return {include-relative header path: mutated text} after applying `names` in order."""
    texts = {}
    for name in names:
        if name is None:
            continue
        src, dst, transform = MUTATIONS[name]
        if dst not in texts:
            with open(os.path.join(root, src)) as fh:
                texts[dst] = fh.read()
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
        binpath = os.path.join(td, "test_app_state_store")
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
    ap.add_argument("--require-all", action="store_true",
                    help="treat the HELD controls as fatal (use once the drift ruling lands)")
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

    print("\n== SINGLE-SOURCE NEGATIVE CONTROLS (isolated shadow tree) ==")
    for name, spec in SINGLE.items():
        obs = Observation(build_and_run(root, compiler, apply_chain(root, [name])))
        problems = judge_red(obs, spec["must_fail"], spec.get("must_pass", []))
        check(not problems, "%s: RED for the named reason (rc=1, complete summary, expected FAIL "
                            "line present)" % name)
        for p in problems:
            print("        %s" % p)
        for want in spec.get("also_fail", []):
            check(any(want in l for l in obs.failed),
                  "%s: the neighbouring criterion '%s' also observes the mutation" % (name, want))
        if args.verbose:
            for l in obs.failed:
                print("        FAIL: %s" % l)

    print("\n== HELD CONTROLS (reported, never counted as passes) ==")
    for name, why in HELD.items():
        print("  HELD %s — %s" % (name, why))
    if args.require_all:
        check(False, "every control must run (--require-all): %d control(s) are still HELD"
              % len(HELD))

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
