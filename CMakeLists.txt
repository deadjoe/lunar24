# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# Build for Lunar 24 — the independent, unaffiliated desktop emulation of the
# ELTA Solar 42N panel/behaviour. See design/ and README.md for scope.

cmake_minimum_required(VERSION 3.24)

project(Lunar24
  VERSION 0.0.1
  DESCRIPTION "Independent, unaffiliated desktop study of the ELTA Solar 42N panel and behaviour"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# lunar_core: the framework-free synth core (header-only at P0: descriptors,
# contract types, DeviceStateV1). Never links realtime-unsafe or HAL code.
# ---------------------------------------------------------------------------
add_library(lunar_core INTERFACE)
target_include_directories(lunar_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_compile_features(lunar_core INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# lunar_host: the host (window) layer. NOT framework-free at the GUI bootstrap (its
# main.cpp will drive iPlug2's SWELL), so it lives OUTSIDE core/ — which keeps the
# core framework-free gate green. Its public geometry header
# host/include/host/window_layout.h is PURE (links only lunar_core) and is the ONE
# place the host consumes core::compute_host_window_fit. The GUI main.cpp is separate
# and is registered as its own (non-core) target later.
# ---------------------------------------------------------------------------
add_library(lunar_host INTERFACE)
target_include_directories(lunar_host INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/host/include")
target_link_libraries(lunar_host INTERFACE lunar_core)
target_compile_features(lunar_host INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# Tests: the "regenerate == commit" and "gate" checks run as CTest.
# ---------------------------------------------------------------------------
enable_testing()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Regeneration must be zero-diff: the committed generated headers reflect the
# committed spec. Fails if `generate_registry.py` would rewrite them.
add_test(NAME registry_regen_zero_diff
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_registry.py --check)

# The inventory-manifest builder must also reproduce the committed manifest exactly
# (Codex msg 064cfa6e): a hand-edited manifest that the builder would erase/rewrite
# is a drift bug, not a silent "fix".
add_test(NAME manifest_regen_zero_diff
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/p0_regions_build.py --check)

# Static gates: every authored code file carries a leading SPDX Apache-2.0 header
# (recursive scan), and core public headers stay framework-free (forbidden-include
# scan restricted to the core boundary).
add_test(NAME core_headers_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core_headers.py)

# The produced tree must never depend on the disposable P1 spike/ dir: core/ and
# generated/ are framework-free and spike-free, so a later "delete spike/ and
# ship" cannot break the build. The boundary is one-directional (spike/ may
# reference anything; core/ + generated/ may not reference spike/).
add_test(NAME spike_clean_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_spike_clean.py)

# Negative registry fixtures must each be REJECTED by the validator FOR THEIR
# INTENDED REASON (a message-asserting wrapper, so a fixture that fails for an
# accidental reason no longer passes).
add_test(NAME reg_negative
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_registry_negative.py)

# Order-independent identity regression: serialized ids = explicit numeric id,
# never a JSON-order index. Reordering the spec must not renumber the enums.
add_test(NAME registry_id_stability
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_id_stability.py)

# Cross-edit JackId stability (P2-② add-on 1): the current stable_id -> numeric
# JackId map is frozen as a committed baseline and a freshly-built registry must
# reproduce it exactly. A mid-registry insert that renumbers an existing JackId
# shifts the map, so this goes red; the negative control proves the shift-detector
# actually fires.
add_test(NAME jack_id_baseline
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_jack_id_baseline.py)
add_test(NAME jack_id_baseline_negative
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_jack_id_baseline_negative.py)

# Id-space / state-bank gate: the banks are indexed by the serialized id, so they
# must be sized for the id-space (one-past-the-last id), not the count — a sparse
# id (a hole) is caught here and by the generated static_assert.
add_test(NAME test_id_space_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_id_space_gate.py)

# Full-machine completeness gate (Phase A): the independently-transcribed
# p0_inventory_manifest.json is the target; the registry is the implementation.
# Always-on (not --require-full) so a commit may be <100% but never regress the
# mustComplete baseline or be self-incoherent. The CI step for the PR #2 exact
# head runs the same tool with --require-full (see .github/workflows/ci.yml).
add_test(NAME registry_complete_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_registry_complete.py)

# The gate must reject 重件 (per-category duplicate) / incomplete evidence /
# dangling-or-misdirected route endpoint / present-but-empty module /
# mustComplete-not-equal-landed-set / NON-REGRESSION / --require-full for their
# intended reasons, not by accident.
add_test(NAME registry_complete_negative
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_registry_complete_negative.py)

# Every EvidenceRef emitted into the generated registry must be grounded: its
# lineStart/lineEnd must stay inside the cited document AND land on non-blank lines.
# This is a MECHANICAL grounding check — it does NOT verify a ref's relevance (that
# is an adjudicator judgement, deliberately out of scope here).
add_test(NAME evidence_refs_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_evidence_refs.py)

# The gate must reject each EvidenceRef defect (out-of-range, lineStart>lineEnd,
# blank boundary, unknown source) for its intended reason, and go red when a single
# ref is flipped to point at a blank line — not stumble onto it by accident.
add_test(NAME evidence_refs_negative
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_evidence_refs_negative.py)

# Evidence-layout drift-guard: the committed fingerprint must reproduce the live
# manual exactly (regen==commit). Registered so it is visible on every runner —
# on a dev machine it really checks; on CI the manual is gitignored and it SKIPs
# loudly (never a silent absent guard). Same register-and-loudly-skip principle
# @Claude mandated for the real CoreAudio output test below.
add_test(NAME evidence_layout_guard
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_evidence_layout.py --check)

# Panel-region anchor drift-guard (task #36, msg 9c660e3d): the region anchors are
# MACHINE-generated from the panel reference figure (tools/measure_panel_regions.py),
# never transcribed by hand. Re-running the generator must reproduce the committed
# panel_regions.json AND panel_anchors.generated.h byte-identically, so the C++ test
# consumes a set that provably comes from the image. On CI the PNG is gitignored, so
# the tool SKIPs loudly (never a silent absent guard) — same register-and-loudly-skip
# shape as evidence_layout_guard. The anchor-mutation NEGATIVE lives in the C++ test
# (test_panel_layout NEGATIVE 1): a single edited anchor value must red it.
add_test(NAME panel_regions_guard
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/measure_panel_regions.py --check)

# The host (host/) depends on iPlug2 as an UNMODIFIED fixed-commit library. Since GH#10
# iPlug2 is a real pinned git submodule. This gate is FAIL-LOUD: it verifies .gitmodules
# registers the pinned path+URL, the index gitlink is the exact pin, the submodule is
# initialized (source present), its HEAD is the pin, and its worktree is clean. A missing,
# uninitialized, wrong-pin, or dirty submodule FAILS — there is no longer a skip-if-absent
# path. The CI checkout must pull it recursively (see .github/workflows/ci.yml).
add_test(NAME iplug_pin_clean_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_iplug_pin_clean.py)

# The Windows Lunar24Host target is compiled by the hosted MSVC runner, not the mac dev
# machine, so CMake can't be its gate. This oracle statically asserts the shape of the
# Windows configuration that would otherwise only be proven at hosted link time: the WIN32
# branch genuinely assembles the target (iplug_configure_app + the repo-owned window-metrics
# shim + resources/main.rc), does NOT leak the mac bootstrap main.mm, FORCEs the GL2
# renderer (no mac-cache pollution), compiles the resource script with an /I path, and the
# shim does not redefine the stock APP symbols. Deterministic and platform-independent, so a
# structural regression is caught on ANY CI OS, not just Windows.
add_test(NAME host_windows_target_oracle
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_host_windows_target.py)

# GH#4 8B3 (task#73): the repo IPlugAPP overrides are forks of the pinned submodule TUs that differ
# ONLY by the allowlisted hunks. This gate proves override == upstream + those hunks, so the host
# never silently carries two divergent truths. FAILS if the submodule is not at the exact pin.
add_test(NAME host_override_drift
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_host_override_drift.py)

# G5 negative: the drift gate must flag a mutation INSIDE an allowlisted function (re-connecting all
# MaxNChannels() in AppProcess — the anti-pattern that would read a 2-out device out of bounds). The
# old anchor-based gate passed any same-name body; the exact-hash gate must not, or this breaks.
add_test(NAME host_override_drift_negative
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_host_override_drift_negative.py)

# Strict warnings, applied PRIVATE to a Lunar-owned compiled target only. Never
# set globally: a future third-party build unit (iPlug2, P1+) must not be forced
# warning-clean by a global add_compile_options, and lunar_core (header-only
# INTERFACE) has no compilation unit of its own to set them on.
function(lunar_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

function(lunar_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lunar_core)
  lunar_enable_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lunar_add_test(test_core_contract tests/core/test_core_contract.cpp)
lunar_add_test(test_registry tests/core/test_registry.cpp)
lunar_add_test(test_time_semantics tests/core/test_time_semantics.cpp)
lunar_add_test(test_patch_graph tests/core/test_patch_graph.cpp)
lunar_add_test(test_graph_compiler tests/core/test_graph_compiler.cpp)
lunar_add_test(test_sink_interpret tests/core/test_sink_interpret.cpp)
lunar_add_test(test_state_publish tests/core/test_state_publish.cpp)
lunar_add_test(test_machine_runtime tests/core/test_machine_runtime.cpp
               tests/core/test_machine_runtime_allocator.cpp)
lunar_add_test(test_machine_definition tests/core/test_machine_definition.cpp
               tests/core/test_machine_definition_allocator.cpp)
lunar_add_test(test_machine_control_sources tests/core/test_machine_control_sources.cpp
               tests/core/test_machine_control_sources_allocator.cpp)
lunar_add_test(test_device_adapter_oracle tests/core/test_device_adapter_oracle.cpp
               tests/core/test_device_adapter_oracle_allocator.cpp)

# task#55 (GH#5) Windows-only CI diagnostic, stack A/B, do NOT remove without the
# intent documented below. This test repeatedly stack-constructs ~149 KB of
# fixed-capacity SynthRuntime fixtures from one large harness, so Windows' 1 MB
# default process stack is a TEST-HARNESS budget, not a product runtime contract
# (the product target, and every other test, is untouched). Reserve 4 MB for this
# one test executable, and only under MSVC, so the change cannot affect any other
# target or platform.
if(MSVC)
  target_link_options(test_machine_runtime PRIVATE /STACK:4194304)
  # task#65 (GH#11) same Windows test-harness budget for the canonical
  # MachineRuntimeDefinition test. `test_machine_definition` stack-constructs and
  # nests several ~161 KB definitions (MachineRuntimeDefinition = 161,168 bytes,
  # SynthRuntime = 151,920) from one large `main` (static frame 510,592 B) plus a
  # lambda frame 483,728 B (~994,320 B combined), so Windows' 1 MB default process
  # stack is a TEST-HARNESS budget, not a product runtime contract (the product
  # target and every other test/platform is untouched). Reserve 4 MB for this one
  # test executable, only under MSVC, mirroring test_machine_runtime above.
  target_link_options(test_machine_definition PRIVATE /STACK:4194304)
elseif(APPLE)
  # task#65 (GH#11) same harness budget on the Apple LINKER (ld64): this one test stack-constructs
  # many ~149 KB fixed-capacity SynthRuntime fixtures from one large `main`, and under
  # -fsanitize=address the main-thread guard is tight enough that the inlined fixture
  # lambdas overflow it (Release/Debug are unaffected). Reserve a larger main-thread
  # stack for THIS ONE test executable, mirroring the MSVC branch above; scoped so no
  # other target or platform is affected. -Wl,-stack_size is a standard ld64 flag, so the
  # guard is Apple-linker (not AppleClang-only) and is independent of the compiler frontend.
  target_link_options(test_machine_runtime PRIVATE -Wl,-stack_size,0x1000000)
endif()

# #37 close-out: the TSan-only memory-model PROBE. It has no CTest assertion and is
# meaningful only as a ThreadSanitizer detector — a correct pool exits 0, a pool
# whose recycleOne guard is removed or whose memory_order is weakened races and
# exits non-zero. It must NEVER run in the normal build (it has no happens-before
# edge between reader and recycler, so it would be a meaningless busy loop there);
# it is compiled and registered ONLY under -fsanitize=thread. The deterministic
# test_state_publish.cpp above is the primary observable-behaviour judge; this
# probe is the memory-model guard that stays green only while the pin/retire
# ordering holds (guards against future removal of the guard / weakening of the
# memory_order). Both stay.
if(CMAKE_CXX_FLAGS MATCHES "fsanitize=thread")
  lunar_add_test(test_state_publish_tsan tests/core/test_state_publish_tsan.cpp)
  set_tests_properties(test_state_publish_tsan PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=abort_on_error=0:exitcode=66")

  # #37 close-out selftest: the SAME probe rebuilt with its pin guard short-circuited
  # (-DLUNAR24_PROBE_SELFTEST in state_snapshot.h). It MUST report a TSan data race
  # (exit 66) — the proof that the committed probe can actually fail. A detector that
  # has never been shown to fire is not a detector. CTest inverts the expected
  # non-zero exit via the expect_exit wrapper, so a race actually firing passes; a
  # probe that stops detecting (exit 0) or a crash (any other code) fails, so a dead
  # detector is caught rather than staying green. The selftest source is built as its
  # own target because lunar_add_test does not accept compile definitions.
  add_executable(test_state_publish_tsan_selftest
    tests/core/test_state_publish_tsan.cpp)
  target_link_libraries(test_state_publish_tsan_selftest PRIVATE lunar_core)
  target_compile_definitions(test_state_publish_tsan_selftest PRIVATE
    LUNAR24_PROBE_SELFTEST)
  lunar_enable_warnings(test_state_publish_tsan_selftest)
  add_test(NAME test_state_publish_tsan_selftest
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/expect_exit.py 66
      $<TARGET_FILE:test_state_publish_tsan_selftest>)
  set_tests_properties(test_state_publish_tsan_selftest PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=abort_on_error=0:exitcode=66")
endif()

lunar_add_test(test_state_persistence tests/core/test_state_persistence.cpp)
lunar_add_test(test_drone_bank tests/core/test_drone_bank.cpp)
lunar_add_test(test_drone_classic tests/core/test_drone_classic.cpp)
lunar_add_test(test_drone_mod tests/core/test_drone_mod.cpp)
lunar_add_test(test_vco tests/core/test_vco.cpp)
lunar_add_test(test_preamp_envelope tests/core/test_preamp_envelope.cpp)
lunar_add_test(test_envelope_generator tests/core/test_envelope_generator.cpp)
lunar_add_test(test_lfo tests/core/test_lfo.cpp)
lunar_add_test(test_joystick_cv tests/core/test_joystick_cv.cpp)
lunar_add_test(test_five_step_sequencer tests/core/test_five_step_sequencer.cpp)
lunar_add_test(test_mix_filter_distortion tests/core/test_mix_filter_distortion.cpp)
lunar_add_test(test_device_layout tests/core/test_device_layout.cpp)
lunar_add_test(test_executor_realpath tests/core/test_executor_realpath.cpp)
lunar_add_test(test_input_equivalence tests/core/test_input_equivalence.cpp)
lunar_add_test(test_keyboard_presets tests/core/test_keyboard_presets.cpp)
lunar_add_test(test_keyboard_mode tests/core/test_keyboard_mode.cpp)
lunar_add_test(test_keyboard_behaviour tests/core/test_keyboard_behaviour.cpp)
lunar_add_test(test_arp_sequencer tests/core/test_arp_sequencer.cpp)
lunar_add_test(test_keyboard_pipeline tests/core/test_keyboard_pipeline.cpp)
lunar_add_test(test_keyboard_calibration tests/core/test_keyboard_calibration.cpp)
lunar_add_test(test_keyboard_menu tests/core/test_keyboard_menu.cpp)
lunar_add_test(test_host_window_fit tests/core/test_host_window_fit.cpp)
lunar_add_test(test_panel_transform tests/core/test_panel_transform.cpp)
lunar_add_test(test_panel_layout tests/core/test_panel_layout.cpp)
lunar_add_test(test_host_window_layout tests/host/test_host_window_layout.cpp)
target_link_libraries(test_host_window_layout PRIVATE lunar_host)
target_include_directories(test_host_window_layout PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# GH#12 9A foundation (task#75 r2): disposition table + default-state + migration/validate.
lunar_add_test(test_state_disposition tests/core/test_state_disposition.cpp)
lunar_add_test(test_state_validation tests/core/test_state_validation.cpp)

# GH#4 8B2: the framework-free host audio-engine owner (task#72) behaviour oracle. It links
# LUNAR_HOST (not lunar_core) because the engine lives in host/include/host and depends on
# lunar_core for the DeviceAdapter / MachineRuntimeDefinition. The isolated operator-new
# allocator probe is built as a SECOND source of the SAME target (its replaceable new/delete
# must live in a different TU from the call sites, or -Wmismatched-new-delete misjudges the
# malloc/free implementation). The wiring SHAPE (ProcessBlock/OnReset delegate) is a separate
# python CTest below: ProcessBlock is not reachable from a unit test (an iPlug virtual), so a
# python static gate is the permanent second line for "is the host actually wired".
lunar_add_test(test_host_engine_oracle
  tests/host/test_host_engine_oracle.cpp
  tests/host/test_host_engine_oracle_allocator.cpp)
target_link_libraries(test_host_engine_oracle PRIVATE lunar_host)
target_include_directories(test_host_engine_oracle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# GH#12 9B (task#76): the state-aware runtime candidate apply oracle. Drives the framework-free
# StandaloneAudioEngine::applyDeviceState (a validated DeviceStateV1 -> buildMachineRuntimeCandidate
# -> single stopped-stream commit -> processBlock -> DeviceAdapter::renderBlock) plus the safe-boot
# prepare(seed)==make_default_device_state(seed) equality, the distinct calibration/seed trace
# discriminators, the atomic rejected-candidate leaves-prior-unchanged contract, and the
# render-path allocator probe (separate TU, mirroring test_host_engine_oracle_allocator).
lunar_add_test(test_state_apply_oracle
  tests/host/test_state_apply_oracle.cpp
  tests/host/test_state_apply_oracle_allocator.cpp)
target_link_libraries(test_state_apply_oracle PRIVATE lunar_host)
target_include_directories(test_state_apply_oracle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task#78 commit ①/②: the FULL-169 applied_to_DSP product oracle. Where the target above pins the
# 35 control-source params + GH#6 identity (9B), this one gates the whole applied_to_DSP set
# (exactly 169), reading back the REAL DSP members the render path consumes — no shadow bank — and
# the A/B, L/R, group/gen, mixer vol/pan anti-cross-talk + default==restore + typed-reject carry.
lunar_add_test(test_state_apply_oracle_169
  tests/host/test_state_apply_oracle_169.cpp)
target_link_libraries(test_state_apply_oracle_169 PRIVATE lunar_host)
target_include_directories(test_state_apply_oracle_169 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task#78 9C rework (@Codex 1e34b7bb): the control-family + audio-family behavior harnesses. BOTH
# drive the AGREED entry — encode -> decode -> StandaloneAudioEngine.applyDeviceState -> processBlock
# — through the shared tests/host/test_engine_harness.h (no direct buildMachineRuntimeCandidate /
# processFrame route). The control harness asserts on the PUBLISHED CV/gate via owner.runtime()
# (read-only) for the five control-source families; the audio harness asserts on the REAL output
# buffers (WET_L/DRY_A/DRY_B) for VCO init + six audio families, incl. the NEW drone_3/6 applied
# params. No product topology change; verification only.
lunar_add_test(test_machine_control_outputs tests/host/test_machine_control_outputs.cpp)
target_link_libraries(test_machine_control_outputs PRIVATE lunar_host)
target_include_directories(test_machine_control_outputs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

lunar_add_test(test_machine_audio_families tests/host/test_machine_audio_families.cpp)
target_link_libraries(test_machine_audio_families PRIVATE lunar_host)
target_include_directories(test_machine_audio_families PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task#80 (GH#12 9D C3): the DeviceState user-cable atomic restore into the real PatchGraph, driven
# through the agreed entry (encode -> decode -> StandaloneAudioEngine.applyDeviceState -> processBlock)
# and asserted blindly on the published cable bank (cableCount / countInto / cableConnected) + the
# real output buffers. Covers @Codex 7ce76a8b (a)-(f): no-cable default == original; a user cable
# overrides an active normalized route and unplug restores it; a supported asymmetric cable yields the
# expected control + audio difference; same state restored twice (and a positional re-arrangement)
# gives the same wire set/plan/output; multi-cable no loss; and an unsupported/deferred graph fails
# as a WHOLE candidate with the prior accepted owner preserved by the single-commit guard.
lunar_add_test(test_machine_cable_restore tests/host/test_machine_cable_restore.cpp)
target_link_libraries(test_machine_cable_restore PRIVATE lunar_host)
target_include_directories(test_machine_cable_restore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task #83 (GH #18): RED-first product oracle + regression net for correcting the default VCO-B
# normalized-route SOURCE to the published VCO-A signal (acyclic A->B). Drives the agreed entry and
# asserts the GH #18 contract: default DRY A/B non-constant + no 50ms flat window at 4 sample rates,
# cvAmt 0..1 A-only asymmetric propagation (isolating at 0), B-change never swaps the A source, a user
# cable overrides/restores the A->B route, and the A->B edge is acyclic (no artificial z^-1 delay).
lunar_add_test(test_vco_normal_source tests/host/test_vco_normal_source.cpp)
target_link_libraries(test_vco_normal_source PRIVATE lunar_host)
target_include_directories(test_vco_normal_source PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# GH#4 8B3 (task#73): the shared channel-plan oracle. The host override and this test call the SAME
# negotiate_stream_plan() function, so the host and the oracle cannot disagree on how many channels
# the stream actually opens. No allocator probe: the negotiation is pure ints.
lunar_add_test(test_host_stream_plan tests/host/test_host_stream_plan.cpp)
target_link_libraries(test_host_stream_plan PRIVATE lunar_host)
target_include_directories(test_host_stream_plan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# GH#4 8B2 wiring-shape oracle (task#72): ProcessBlock/OnReset are iPlug virtuals, not
# reachable from a unit test, so a python gate is the permanent second line for "is the host
# actually wired". Deterministic and platform-independent — a structural regression (old
# passthrough/silence stub, OnReset-not-prepare, hardcoded 48k, host-side frame loop) is
# caught on ANY CI OS, mirroring check_host_windows_target.py at line 183.
add_test(NAME host_engine_wiring_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_host_engine_wiring.py)

# GH#4 8B3 (task#73): codec portability. The hosted CI run surfaced two Windows gate failures
# caused by Path.read_text() with no encoding= (Windows Py3.14 defaults to cp1252). This gate pins
# the permanent invariant that every .read_text(...) in the two host gate scripts carries a constant
# encoding="utf-8", includes a disposable-copy negative (removing any one encoding flips RED), and
# runs both gates under LC_ALL=C LANG=C PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 to prove their file reads
# do not depend on the process default encoding. Product / override / pinned drift hash untouched.
add_test(NAME host_script_codec
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_host_script_codec.py)

# P3-⑥ Debt 1 (platform half): the four logic outputs against the REAL machine's
# output layout, read via the framework authority (AudioBufferList). Apple-only: the
# .cpp is #ifdef __APPLE__-guarded so the non-macOS matrix never compiles it, and even
# on macOS it loud-skips (no default device / <4 outs) rather than faking a pass. This
# is registered but never exercises on CI (headless, no device) — recorded in FINDINGS.
if(APPLE)
  lunar_add_test(test_coreaudio_output tests/core/test_coreaudio_output.cpp)
  target_link_libraries(test_coreaudio_output PRIVATE
    "-framework CoreAudio" "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------------------
# P5-① host STANDALONE (host/): the Lunar24Host executable. Two trusted entries share
# ONE cross-platform target:
#   * macOS uses a repo-owned, nib-free bootstrap (host/main.mm) + mac SWELL + Metal.
#   * Windows uses the pinned iPlug2::APP + iplug_configure_app (stock IPlugAPP_main.cpp,
#     RtAudio/RtMidi, ASIO/DS/MM sources, WIN32_EXECUTABLE) + a repo-owned Win32
#     window-metrics shim + host/resources/main.rc.
# GH#10 makes iPlug2 a REAL pinned submodule, so on APPLE/WIN32 the submodule MUST be
# present and initialized AT CONFIGURE TIME — a missing submodule is a config error
# (fail-loud), never an EXISTS silent-skip. Linux builds the core only (no host target)
# and still sees the submodule via the recursive CI checkout + the fail-loud pin gate.
# The host never touches SynthRuntime/DeviceAdapter (that is GH#4); it only sizes a
# real window via the geometry choke point.
# ---------------------------------------------------------------------------
if(APPLE OR WIN32)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/iPlug2/iPlug2.cmake")
    message(FATAL_ERROR
      "Lunar24Host: the pinned iPlug2 submodule is missing at "
      "third_party/iPlug2. Run `git submodule update --init --recursive` "
      "(GH#10 requires the submodule for the hosts on macOS and Windows).")
  endif()
  add_subdirectory(host)
endif()
