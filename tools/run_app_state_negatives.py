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
# Added for @Codex 0c9ea88d / d5f6a520 (the three ordered revisions):
#   9. save_gate_reopened_by_session_publish — the sticky adoption verdict is replaced by "any
#                                 Accepted publish re-opens the gate", which is exactly the reported
#                                 hole: a later successful device reopen (default config) un-protects a
#                                 file this session refused, and the exit save overwrites it.
#  10. oversized_allocated_before_size_gate — the fstat size gate is disabled, so an oversized record
#                                 is read before the length verdict instead of being refused first.
#  11. trailing_byte_accepted  — the read boundary stops probing for a byte beyond the record, so a
#                                 file that grew after the (snapshot) size gate is accepted as exact.
#  12. temp_reserve_not_exclusive — the temp name is still created but no longer claimed EXCLUSIVELY,
#                                 so a colliding name is silently shared with another process.
#
# Structural controls (2, judged by the W16b wiring invariants — NOT by the C++ acceptance):
#  13. exit_call_missing        — ~IPlugAPPHost never calls the plugin's exit save, so the lifecycle
#                                 save silently disappears from the host.
#  14. exit_call_before_closeaudio — the save is moved BEFORE CloseAudio(), i.e. it runs while the
#                                 audio callback may still be live (mandate §5 order).
# Both mutate host/iPlug_app_host_override.cpp. That file's whole upstream->fork diff is pinned by
# tools/check_host_override_drift.py, so a control NEVER edits the product file: it builds a shadow
# repo (a real copy of tools/check_host_engine_wiring.py + a symlink farm to the rest of the tree,
# with the mutated override as the only real content) and runs the gate there. @Codex msg 97d9f1a2
# authorized the two hunks and the matching drift-hash update, which un-HELD these controls.
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
import shutil
import subprocess
import sys
import tempfile

TEST_SRC = os.path.join("tests", "host", "test_app_state_store.cpp")

# (repo-relative source, include-relative shadow path) for each mutated production source.
STORE = ("host/include/host/app_state_store.h", "host/app_state_store.h")

# The structural controls mutate the APP host TU and are judged by the W16b wiring invariants. The
# shadow repo needs the gate itself (a REAL copy: Path(__file__).resolve() must stay inside the
# shadow root, or the gate would re-read the product tree and the mutation would be invisible) and
# every file the gate reads (symlinks: the gate only reads text).
HOST_OVR = os.path.join("host", "iPlug_app_host_override.cpp")
GATE_TOOL = os.path.join("tools", "check_host_engine_wiring.py")
GATE_INPUTS = [
    "host/plugin.h",
    "host/plugin.cpp",
    "host/include/host/standalone_audio_engine.h",
    "host/iPlug_app_override.cpp",
    "host/config.h",
    "host/include/host/stream_plan.h",
    "host/include/host/app_state_store.h",
]

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
    "    if (result != SaveResult::ok) {\n"
    "      // The backend's discardFile is the backend's own cleanup; this only drops the reservation "
    "WE\n"
    "      // took, and only for the exact path we reserved (never a pattern, never another "
    "instance's).\n"
    "      app_state_file_ops::realDiscardFile(nullptr, temp.c_str());\n"
    "    }\n"
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
                    "    if (result != SaveResult::ok) {\n"
                    "      app_state_file_ops::realDiscardFile(nullptr, temp.c_str());\n"
                    "    }\n"
                    "    (void)result;  // MUTATION: save_result_swallowed — always Saved.\n"
                    "    return StateSaveOutcome::Saved;\n",
                    name)


STICKY_ADOPTION = ("      if (origin == PendingOrigin::FromFile && !fileUnadopted_) {\n"
                   "        saveAllowed_ = true;  // the file was adopted by the REAL candidate\n"
                   "      }\n")


def mut_save_gate_reopened_by_session_publish(text, name):
    """The adoption verdict stops being sticky: any Accepted publish re-opens the exit save. This is
    @Codex 0c9ea88d's reproduced hole — the file is refused, the gate closes, a device reopen publishes
    the DEFAULT config as a successful FromSession publish, and the exit save overwrites the file."""
    return _replace(text, STICKY_ADOPTION,
                    "      // MUTATION: save_gate_reopened_by_session_publish — a successful publish\n"
                    "      // of ANY origin lifts the protection again.\n"
                    "      if (loadOutcome_ == StateLoadOutcome::Ok) {\n"
                    "        saveAllowed_ = true;\n"
                    "      }\n",
                    name)


SIZE_GATE = ("    if (static_cast<std::uintmax_t>(st.st_size) !=\n"
             "        static_cast<std::uintmax_t>(kAppStateWireBytes)) {\n")


def mut_oversized_allocated_before_size_gate(text, name):
    """The pre-allocation size gate is gone: an oversized record is read (and allocated) in full
    before the length verdict, instead of being refused by the fstat snapshot first."""
    return _replace(text, SIZE_GATE,
                    "    // MUTATION: oversized_allocated_before_size_gate — the size gate is "
                    "disabled.\n"
                    "    if (false) {\n",
                    name)


TRAILING_PROBE = ("    const int extra = (got == out->size()) ? std::fgetc(f) : EOF;\n"
                  "    *ioError = std::ferror(f) != 0;\n"
                  "    return !*ioError && got == out->size() && extra == EOF;\n")


def mut_trailing_byte_accepted(text, name):
    """The read boundary stops probing past the record: a file that grew after the fstat snapshot is
    accepted as an exact record instead of being refused."""
    return _replace(text, TRAILING_PROBE,
                    "    const int extra = (got == out->size()) ? std::fgetc(f) : EOF;\n"
                    "    *ioError = std::ferror(f) != 0;\n"
                    "    // MUTATION: trailing_byte_accepted — the extra-byte probe is dropped.\n"
                    "    (void)extra;\n"
                    "    return !*ioError && got == out->size();\n",
                    name)


EXCL_POSIX = "  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);\n"
EXCL_WIN = ("  if (_sopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, "
            "_SH_DENYRW,\n")


def mut_temp_reserve_not_exclusive(text, name):
    """The temp name is still created, but not claimed EXCLUSIVELY: a colliding name is silently
    shared with another process instead of being refused by the kernel."""
    text = _replace(text, EXCL_POSIX,
                    "  // MUTATION: temp_reserve_not_exclusive — no O_EXCL.\n"
                    "  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0666);\n",
                    name)
    return _replace(text, EXCL_WIN,
                    "  // MUTATION: temp_reserve_not_exclusive — no _O_EXCL.\n"
                    "  if (_sopen_s(&fd, path.c_str(), _O_CREAT | _O_WRONLY | _O_BINARY, "
                    "_SH_DENYRW,\n",
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
    "save_gate_reopened_by_session_publish": (STORE[0], STORE[1],
                                              mut_save_gate_reopened_by_session_publish),
    "oversized_allocated_before_size_gate": (STORE[0], STORE[1],
                                             mut_oversized_allocated_before_size_gate),
    "trailing_byte_accepted": (STORE[0], STORE[1], mut_trailing_byte_accepted),
    "temp_reserve_not_exclusive": (STORE[0], STORE[1], mut_temp_reserve_not_exclusive),
}

# ---- structural mutations of the APP host TU (judged by the W16b wiring invariants) -------------
#
# The two anchors are the delivered product text: the exit-save call line, and the destructor's
# `mExiting = true;` / `CloseAudio();` opening. `_replace` fails LOUDLY if either drifts, so a control
# can never silently become a no-op.
EXIT_SAVE_LINE = "  static_cast<LunarHostPlugin*>(GetPlug())->saveDeviceState();\n"
DTOR_OPEN = "  mExiting = true;\n  \n  CloseAudio();\n"


def mut_exit_call_missing(text, name):
    """The host never calls the plugin's exit save: the lifecycle save is gone from the product."""
    return _replace(text, EXIT_SAVE_LINE, "", name)


def mut_exit_call_before_closeaudio(text, name):
    """The save is moved BEFORE CloseAudio(): it runs while the audio callback may still be live."""
    text = _replace(text, EXIT_SAVE_LINE, "", name)
    return _replace(text, DTOR_OPEN,
                    "  mExiting = true;\n"
                    "\n"
                    "  // MUTATION: exit_call_before_closeaudio — the exit save runs before the\n"
                    "  // audio callback is quiesced.\n"
                    + EXIT_SAVE_LINE +
                    "  CloseAudio();\n",
                    name)


# name -> (transform, must_fail, must_pass). `must_fail` = the W16b invariants this mutation is
# DESIGNED to break; `must_pass` = invariants that must still hold (specificity: the mutation is one
# ordering/presence defect, not a gate crash).
STRUCTURAL = {
    "exit_call_missing": (
        mut_exit_call_missing,
        ["W16b exit save is a plugin delegate",
         "W16b exit save runs AFTER CloseAudio() returns",
         "W16b exit save runs before the remaining member teardown",
         "W16b the host override calls saveDeviceState exactly once"],
        ["W16b ~IPlugAPPHost body present",
         "W16b no file IO in the destructor body"],
    ),
    "exit_call_before_closeaudio": (
        mut_exit_call_before_closeaudio,
        ["W16b exit save runs AFTER CloseAudio() returns"],
        ["W16b ~IPlugAPPHost body present",
         "W16b exit save is a plugin delegate",
         "W16b exit save runs before the remaining member teardown",
         "W16b no file IO in the destructor body",
         "W16b the host override calls saveDeviceState exactly once"],
    ),
}

# Every W16b/W17b invariant that MUST have actually run in the structural positive control (the
# unmutated shadow tree). A mutation going red is only evidence if the neighbouring invariants really
# executed; this is the structural analogue of REQUIRED_GREEN.
STRUCTURAL_REQUIRED_GREEN = [
    "W16b ~IPlugAPPHost body present",
    "W16b exit save is a plugin delegate",
    "W16b exit save runs AFTER CloseAudio() returns",
    "W16b exit save runs before the remaining member teardown",
    "W16b no file IO in the destructor body",
    "W16b the host override calls saveDeviceState exactly once",
    "W17b InitState body present",
    "W17b handoff happens after the platform directory resolution",
    'W17b handoff happens BEFORE Append("settings.ini") mutates mINIPath',
    "W17b handoff passes the resolved mINIPath",
    "W17b host calls setStateDirectory exactly once",
    "W17b host never names the state file",
]


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
    # @Codex 0c9ea88d / d5f6a520: the sticky adoption verdict, the pre-allocation size gate, the
    # extra-byte read boundary and the EXCLUSIVE temp claim.
    "C4.11 the refusal closed the lifecycle save gate",
    "C4.12 a successful FromSession publish did NOT re-open the gate",
    "C4.12 the refused file is preserved byte-for-byte across device reopens",
    "C4.13 NOTHING was read: the size gate precedes allocation",
    "C4.14 a byte BEYOND the record is rejected, not accepted",
    "C6.3 exclusive creation refuses a path another owner already holds",
    "C6.5 the temp was ALREADY reserved (empty file) when the backend was asked to write",
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
    # @Codex 0c9ea88d's reproduced hole: refusal -> gate closed -> device reopen publishes the
    # DEFAULT config successfully -> the exit save overwrites the refused file.
    "save_gate_reopened_by_session_publish": {
        "must_fail": ["C4.12 a successful FromSession publish did NOT re-open the gate"],
        "also_fail": ["C4.12 the refused file is preserved byte-for-byte across device reopens",
                      "C4.11 the exit save refused to touch the refused file"],
        # The refusal itself still closes the gate; only the later reopen re-opens it.
        "must_pass": ["C4.11 the refusal closed the lifecycle save gate"],
    },
    # The size gate is the ONLY pre-allocation refusal for an oversized record; the bounded read
    # still turns the file into a typed LengthMismatch, so the mutation is observed by the ordering
    # criterion alone (that is precisely the @Codex d5f6a520 requirement).
    "oversized_allocated_before_size_gate": {
        "must_fail": ["C4.13 NOTHING was read: the size gate precedes allocation"],
        "must_pass": ["C4.13 an oversized record is a length mismatch",
                      "C4.13 the oversized file stays protected"],
    },
    # fstat is a snapshot, not a lock: the extra-byte probe is the only thing that catches a file
    # that grew after the size gate (the oversized case is already refused by that gate).
    "trailing_byte_accepted": {
        "must_fail": ["C4.14 a byte BEYOND the record is rejected, not accepted"],
        "must_pass": ["C4.14 an exact-size record is accepted and exactly that many bytes were read",
                      "C4.14 a SHORT read is rejected, not accepted"],
    },
    # The temp is still created (so the save path still works) but no longer claimed exclusively:
    # the collision with another owner is the defect.
    "temp_reserve_not_exclusive": {
        "must_fail": ["C6.3 exclusive creation refuses a path another owner already holds"],
        "must_pass": ["C6.3 exclusive creation succeeds on a fresh path",
                      "C6.3 the reservation really created the file",
                      "C6.4 each reserved temp name exists on disk (claimed in the kernel)",
                      "C6.5 the temp was ALREADY reserved (empty file) when the backend was asked "
                      "to write"],
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


# ---- structural controls: the W16b wiring gate as the behaviour detector -------------------------

GATE_HEADER = "check_host_engine_wiring"
GATE_FAIL_SUMMARY_RE = re.compile(r"^  (\d+) wiring invariant\(s\) FAILED$", re.M)
GATE_OK_SUMMARY_RE = re.compile(r"^  all host wiring invariants PASS$", re.M)


class GateObservation:
    """One run of tools/check_host_engine_wiring.py on a shadow tree, parsed AND cross-checked.

    Same trust model as `Observation`: a control is evidence only if the gate ran to a COMPLETE
    termination on a tree that really contained the mutation. A traceback (unreadable/missing file),
    a signal exit, or a summary that disagrees with the printed lines is NOT a red control."""

    def __init__(self, res):
        self.rc = res.get("rc")
        self.passed = []
        self.failed = []
        self.summary = None
        self.failed_count = None
        self.problems = []

        if res.get("error") is not None:
            self.problems.append("the wiring gate could not be launched (%s)" % res["error"])
            return

        out = res.get("out", "")
        if GATE_HEADER not in out:
            self.problems.append("the wiring gate never printed its header — it did not run")
        for line in out.splitlines():
            if line.startswith("  PASS  "):
                self.passed.append(line[len("  PASS  "):].strip())
            elif line.startswith("  FAIL  "):
                self.failed.append(line[len("  FAIL  "):].strip())

        m = GATE_FAIL_SUMMARY_RE.search(out)
        if m:
            self.summary = "failed"
            self.failed_count = int(m.group(1))
        elif GATE_OK_SUMMARY_RE.search(out):
            self.summary = "ok"
            self.failed_count = 0
        else:
            self.problems.append("the wiring gate printed NO complete termination summary "
                                 "(neither 'N wiring invariant(s) FAILED' nor "
                                 "'all host wiring invariants PASS')")

        if self.summary == "failed" and self.failed_count != len(self.failed):
            self.problems.append("the summary claims %d failed invariants but %d FAIL lines were "
                                 "printed (truncated / interleaved output)"
                                 % (self.failed_count, len(self.failed)))
        if self.summary == "ok" and self.failed:
            self.problems.append("the gate reports all invariants PASS but also printed FAIL lines")

        if self.rc is None:
            self.problems.append("no exit code was observed")
        elif self.rc < 0:
            self.problems.append("the gate was killed by signal %d (NOT a red control)" % -self.rc)
        elif self.rc > 1:
            self.problems.append("the gate exited %d (a normal red gate exits exactly 1; a crash is "
                                 "NOT a red control)" % self.rc)

    def trusted(self):
        return not self.problems


def judge_gate_red(obs, must_fail, must_pass):
    """A red structural control must be TRUSTED, exit exactly 1, and hit exactly the named labels."""
    problems = list(obs.problems)
    if obs.rc != 1:
        problems.append("exit code is %s, not exactly 1" % (obs.rc,))
    if obs.summary != "failed":
        problems.append("no 'N wiring invariant(s) FAILED' termination summary")
    for want in must_fail:
        if not any(want in l for l in obs.failed):
            problems.append("the expected RED invariant did not appear as a FAIL line: '%s'" % want)
    for want in must_pass:
        if not any(want in l for l in obs.passed):
            problems.append("the invariant that must still HOLD never printed a PASS line: '%s'"
                            % want)
    return problems


def judge_gate_green(obs, must_pass):
    problems = list(obs.problems)
    if obs.rc != 0:
        problems.append("exit code is %s, not 0" % (obs.rc,))
    if obs.summary != "ok":
        problems.append("no 'all host wiring invariants PASS' termination summary")
    if obs.failed:
        problems.append("%d FAIL lines in the structural positive control" % len(obs.failed))
    for want in must_pass:
        if not any(want in l for l in obs.passed):
            problems.append("invariant did not run (no PASS line): '%s'" % want)
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


def _synth_gate(rc, passed, failed, summary="failed", failed_count=None, header=True):
    """Build a synthetic wiring-gate run for the structural judge's own self-check."""
    out = (GATE_HEADER + "\n") if header else ""
    out += "".join("  PASS  %s\n" % l for l in passed)
    out += "".join("  FAIL  %s\n" % l for l in failed)
    if summary == "failed":
        out += "  %d wiring invariant(s) FAILED\n" % (
            len(failed) if failed_count is None else failed_count)
    elif summary == "ok":
        out += "  all host wiring invariants PASS\n"
    return {"rc": rc, "out": out, "err": ""}


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

# The structural judge gets its own self-check: every entry is a way it could bless a broken
# structural control (or reject a good one).
SELF_CHECK_GATE = [
    ("red", "a clean red gate run that hits its invariant is TRUSTED",
     _synth_gate(1, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"]),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], False),
    ("red", "a gate that exits 0 is not a red control",
     _synth_gate(0, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"], summary="ok"),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a gate killed by a signal is rejected",
     _synth_gate(-11, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"]),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a gate that crashed (exit >1) is rejected",
     _synth_gate(2, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"]),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a gate run with no termination summary is rejected",
     _synth_gate(1, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"], summary=None),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a summary that disagrees with the printed FAIL count is rejected",
     _synth_gate(1, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"], failed_count=4),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a gate that never printed its header (did not run) is rejected",
     _synth_gate(1, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"], header=False),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "a missing expected RED invariant is rejected",
     _synth_gate(1, ["W16b no file IO in the destructor body"], ["something else"]),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("red", "residual-only FAIL (the must-hold invariant never ran) is rejected",
     _synth_gate(1, [], ["W16b exit save runs AFTER CloseAudio() returns"]),
     ["W16b exit save runs AFTER CloseAudio() returns"],
     ["W16b no file IO in the destructor body"], True),
    ("green", "a clean green gate run is accepted",
     _synth_gate(0, ["W16b no file IO in the destructor body"], [], summary="ok"),
     [], ["W16b no file IO in the destructor body"], False),
    ("green", "a green gate run with a FAIL line is rejected",
     _synth_gate(0, ["W16b no file IO in the destructor body"],
                 ["W16b exit save runs AFTER CloseAudio() returns"], summary="ok"),
     [], ["W16b no file IO in the destructor body"], True),
    ("green", "a green gate run whose invariant never ran is rejected",
     _synth_gate(0, ["something"], [], summary="ok"), [], ["W16b no file IO in the destructor body"],
     True),
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


def build_shadow_repo(root, td, override_text):
    """Build a shadow repo the wiring gate can run in: a REAL copy of the gate (so
    Path(__file__).resolve() stays inside the shadow root — a symlinked gate would resolve back to
    the product tree and the mutation would be invisible), the mutated override as the only real
    content, and symlinks for every other file the gate reads."""
    shadow = os.path.join(td, "repo")
    root = os.path.abspath(root)  # symlink targets must be absolute (a relative target resolves
                                  # against the link's own directory, not the process cwd)
    os.makedirs(os.path.join(shadow, "tools"))
    shutil.copy2(os.path.join(root, GATE_TOOL), os.path.join(shadow, GATE_TOOL))
    dest = os.path.join(shadow, HOST_OVR)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w") as fh:
        fh.write(override_text)
    for rel in GATE_INPUTS:
        dst = os.path.join(shadow, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.symlink(os.path.join(root, rel), dst)
    return shadow


def run_wiring_gate(root, mutate):
    """Run the wiring gate on a shadow tree whose host override is (optionally) mutated."""
    try:
        with open(os.path.join(root, HOST_OVR)) as fh:
            text = fh.read()
    except OSError as exc:
        return {"error": str(exc)}
    if mutate is not None:
        text = mutate(text, "structural")
    with tempfile.TemporaryDirectory() as td:
        shadow = build_shadow_repo(root, td, text)
        try:
            proc = subprocess.run([sys.executable, os.path.join(shadow, GATE_TOOL)],
                                  capture_output=True, text=True, timeout=300)
        except OSError as exc:
            return {"error": str(exc)}
        return {"rc": proc.returncode, "out": proc.stdout, "err": proc.stderr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--compiler", default=None)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--require-all", action="store_true",
                    help="fail if any control is HELD (all 14 controls now run: 12 acceptance + 2 "
                         "structural)")
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
    for kind, name, res, must_fail, must_pass, want_problems in SELF_CHECK_GATE:
        obs = GateObservation(res)
        problems = judge_gate_red(obs, must_fail, must_pass) if kind == "red" \
            else judge_gate_green(obs, must_pass)
        got = bool(problems)
        detail = "" if got == want_problems else "  -> got %s" % (problems or "no problems")
        check(got == want_problems, "self-check (gate %s): %s%s" % (kind, name, detail))
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

    print("\n== STRUCTURAL POSITIVE CONTROL (unmutated shadow tree) ==")
    base = GateObservation(run_wiring_gate(root, None))
    problems = judge_gate_green(base, STRUCTURAL_REQUIRED_GREEN)
    check(not problems, "the unmutated shadow tree is GREEN under the wiring gate (rc=0, complete "
                        "summary, every W16b/W17b invariant printed a PASS line)")
    for p in problems:
        print("        %s" % p)
    if problems:
        print("\nOVERALL: FAIL (the structural baseline is not green; its controls are meaningless)")
        return 1

    print("\n== STRUCTURAL NEGATIVE CONTROLS (shadow copy of the wiring gate) ==")
    for name, (mutate, must_fail, must_pass) in STRUCTURAL.items():
        obs = GateObservation(run_wiring_gate(root, mutate))
        problems = judge_gate_red(obs, must_fail, must_pass)
        check(not problems, "%s: the wiring gate is RED for the named reason (rc=1, complete "
                            "summary, expected FAIL invariant present)" % name)
        for p in problems:
            print("        %s" % p)
        if args.verbose:
            for l in obs.failed:
                print("        FAIL: %s" % l)

    if args.require_all:
        # No control may be silently skipped: 12 acceptance controls + 2 structural controls.
        check(len(SINGLE) == 12 and len(STRUCTURAL) == 2,
              "--require-all: all 14 controls ran (%d acceptance + %d structural)"
              % (len(SINGLE), len(STRUCTURAL)))

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
