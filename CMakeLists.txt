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

# task #98 (GH #15 D3 divider): REAL-RESTORE directional acceptance (the @Codex dd57c783 gap-fill).
# The live-lane d3 acceptance (test_machine_runtime::d3_div_actual_timing_acceptance) drives rate +
# divider through a controlled SynthRuntime fixture; it does NOT exercise the codec -> owner ->
# StandaloneAudioEngine restore path, and its capture frames are chunk-tail-approximate. This test
# restores a LEGAL DeviceState's RATE/DIVIDER through encode -> decode -> applyDeviceState and observes
# the real S&H at the harness's PER-SAMPLE seam (renderSampled: one processBlock(1) per frame + an
# onSample hook), so each capture is recorded at its absolute frame with its held value. It asserts the
# ratio against an INDEPENDENT LF-edge reference round(win*rateHz/sr) from the live rate getter (not the
# divider getter), integer N and default N=8.5 (intervals strictly alternating 8/9, mean 8.5), asymmetric
# two-lane no-cross-talk, restore-twice determinism (same state two loads -> same frame/held sequence)
# and one-render(all) vs two-render(half) block-split at the same per-sample seam. Tagged `slow` (it
# renders ~800k frames per-sample; PR/merge probe-gate only).
lunar_add_test(test_d3_divider_restore tests/host/test_d3_divider_restore.cpp)
target_link_libraries(test_d3_divider_restore PRIVATE lunar_host)
target_include_directories(test_d3_divider_restore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task #85 (GH #19): REAL-PRODUCT anti-aliasing probe. Renders the actual product DSP (through the
# agreed encode->decode->applyDeviceState->processBlock chain) for a matrix of path x sample-rate x
# frequency/level cells and dumps the samples + metadata for the verified Python analyzer. This is
# the measurement guard: it proves the baseline samples come from the product and not an ideal() or a
# silent buffer. Links lunar_host (the engine lives in host/include/host) and needs tests/host for the
# shared EngineHarness. Registered as a CTest; the *measurement pass/fail* decision lives in the
# Python analyzer (gh19_alias_analyze.py), which this target feeds.
lunar_add_test(gh19_alias_probe tests/probes/gh19_alias_probe.cpp)
target_link_libraries(gh19_alias_probe PRIVATE lunar_host)
target_include_directories(gh19_alias_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/host")

# task #86 (GH #19): RED->GREEN acceptance gate for the VCO-triangle BLAMP. Builds nothing
# itself: it runs the REAL product probe (already built as gh19_alias_probe) through the
# verified analyzer and asserts the deliverable criterion against the committed NAIVE
# baseline (tools/gh19_naive_baseline.tsv): every 880 Hz A/B isolated triangle cell improves
# the full-band residual by >= 6 dB, and every 220/440 Hz cell does not worsen beyond 0.5 dB.
# Running this gate on the naive triangle is RED (imp=0 dB at 880); on the BLAMP triangle it
# is GREEN (~8.6 dB). It is the runnable, CI-enforced form of the committed matrix.
add_test(NAME gh19_blamp_acceptance
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_gh19_blamp_pipeline.py
    --probe $<TARGET_FILE:gh19_alias_probe>
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tools/gh19_manifest.tsv
    --baseline ${CMAKE_CURRENT_SOURCE_DIR}/tools/gh19_naive_baseline.tsv
    --analyzer ${CMAKE_CURRENT_SOURCE_DIR}/tools/gh19_alias_analyze.py
    --gate ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_gh19_blamp_acceptance.py)

# CI time: the REAL-product probe (gh19_alias_probe) and the criterion gate that re-runs it
# (gh19_blamp_acceptance) render the actual DSP through the full chain and take 25-60 min per leg.
# They must not gate every feature push (that made CI ~1 hr and, across the 4-OS matrix, multiplied it).
# They are tagged `slow`, excluded from the every-push `build-and-test` matrix (`ctest --label-exclude
# slow`), and run once, on ubuntu, at PR/merge time in the `probe-gate` job (`ctest --label-regex slow`).
# Both stay as separate CTest entries on purpose: the probe is the per-cell measurement guard, the
# acceptance is the criterion-level gate — they are different assertions, not a redundant re-run.
set_tests_properties(gh19_alias_probe gh19_blamp_acceptance PROPERTIES LABELS "slow")

# task #86 (GH #19): the PRODUCTION BLAMP LUT must match the generator's analytic g.
# @Codex (msg 1a8ed7f2) flagged the generator's self-consistency as self-referential
# (it re-parsed its OWN emitted text). This gate reads the ACTUAL production vco.h,
# deduces its L/N config from its own symbols and cross-checks its kLut values against
# the analytic g; a hand-copied / hand-edited production coefficient goes RED. This is
# the runnable, CI-enforced form of "changing any production coefficient must show up".
add_test(NAME gh19_blamp_lut_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_gh19_blamp.py --check-prod
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include/lunar24/core/blamp_kernel.h)

# task #87 (GH #20): REAL-PRODUCT VCF cross-sample-rate response probe. Renders the actual product
# DSP (encode->decode->applyDeviceState->processBlock, via EngineHarness) for a sr x mode x res x
# norm x freq matrix and records the steady-state WET amplitude + per-cell noise floor. This is the
# measurement guard: it proves the response samples come from the product VCF and not an analytic
# re-derivation. The *measurement/finding* decision lives in the Python analyzer (gh20_vcf_analyze.py),
# which this target feeds.
lunar_add_test(gh20_vcf_probe tests/probes/gh20_vcf_probe.cpp)
target_link_libraries(gh20_vcf_probe PRIVATE lunar_host)
target_include_directories(gh20_vcf_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/host")

# task #88 (GH #20): the runnable, CI-enforced gate. Runs the REAL-PRODUCT probe (the same
# codec->owner->render path), then the verified analyzer (--check) on its output: coverage must match
# the committed manifest (no missing / silent / over-scale / non-finite cell) AND the product WET
# response must match the INDEPENDENT bilinear reference to <= 0.1 dB (the @Codex numeric contract,
# msg 8320798f), with a sample-rate-uniform 100 Hz-normalised 8 kHz endpoint (<= 1 dB across
# 44.1/48/88.2/96k). A product reproducing the OLD sr/8-capped Chamberlin response is RED, so the
# criterion is not self-fulfilling. No production DSP is changed to make it green.
add_test(NAME gh20_vcf_acceptance
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_gh20_vcf_gate.py
    --probe $<TARGET_FILE:gh20_vcf_probe>
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/tools/gh20_manifest.tsv
    --analyzer ${CMAKE_CURRENT_SOURCE_DIR}/tools/gh20_trap_analyze.py)

# CI time: the REAL-product probe (gh20_vcf_probe) and the criterion gate that re-runs it
# (gh20_vcf_acceptance) render the actual DSP through the full sr x mode x res x norm grid
# (15518 cells) and take ~25-60 min per leg. They must not gate every feature push, so they
# carry the same `slow` label that PR #28 (chore/ci-probe-gating) introduced for the gh19
# pair; the PR-only probe-gate runs them. gh20_vcf_negative is a fast header-only shadow build
# and deliberately stays in the fast set.
set_tests_properties(gh20_vcf_probe gh20_vcf_acceptance PROPERTIES LABELS "slow")

# task (GH #12): KEYBOARD-OWNER red-first acceptance. Renders the REAL canonical
# MachineRuntimeDefinition (default power-on state via the public seed ctor) and injects a
# canonical note triple (pitch + pressure + gate_on, the exact set InputStateMachine::translate
# emits for a note_on) through the genuine EventTimebase -> applyControlEvent_ -> processFrame
# -> step_ dispatch. On the pre-fix head the keyboard is kUnsupported / deferred and
# applyControlEvent_ drops non-parameter events, so AC-1 (pitch doubling), AC-2 (gate->EG) and
# AC-4b (legato re-note) MUST be red — that is the "will-turn-red" evidence @Kimi required
# before the implementation starts. Links only lunar_core (no host engine, no seam), exactly
# like the core-probe family. Tagged `slow` so the PR-only probe-gate can run it once.
lunar_add_test(gh12_keyboard_owner_probe tests/probes/gh12_keyboard_owner_probe.cpp)
set_tests_properties(gh12_keyboard_owner_probe PROPERTIES LABELS "slow")

# task #101 (GH #12 side restore): acceptance for the per-side keyboard performance state. It
# enters through the real canonical chain (encode -> decode -> validate -> candidate -> runtime)
# and renders tens of thousands of frames per criterion, so it carries the `slow` label the
# probe-gating (PR #28) introduced; the PR-only probe-gate runs it once. Links lunar_host and
# tests/host for the shared EngineHarness: the task#101 review requires the mode/restore/failure
# facts to ALSO be pinned on the real host single-commit entry (applyDeviceState -> processBlock),
# not only at buildMachineRuntimeCandidate.
lunar_add_test(gh12_keyboard_side_restore_probe tests/probes/gh12_keyboard_side_restore_probe.cpp)
target_link_libraries(gh12_keyboard_side_restore_probe PRIVATE lunar_host)
target_include_directories(gh12_keyboard_side_restore_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/host")
set_tests_properties(gh12_keyboard_side_restore_probe PROPERTIES LABELS "slow")

# task #103 (GH #12): ENGINE-LAYER preset LOAD / SAVE / INITIALISE acceptance. Section A pins the
# D-1 defect on the PRE-EXISTING public surface only (canonicalState -> load_preset_to_live ->
# the owner's single-commit apply), so it is genuinely RED on the unfixed keyboard_presets.h with
# RejectedInvalidState / keyboard_live_invalid field 9002; A3 re-asserts that rejection directly so
# A1 can never pass vacuously. Section B drives the new owner API through the shared EngineHarness
# (encode -> decode -> applyDeviceState -> applyPresetAction -> processBlock) and asserts explicit
# failure results, real two-sided recall, LAST-COMMITTED SAVE semantics, INITIALISE scope,
# byte-exact slot round-trip, re-commit timebase reset, partition invariance and a recalled
# consumed scalar reaching the host audio. ~25k rendered frames total, so it stays in the fast set.
lunar_add_test(test_preset_engine_actions tests/host/test_preset_engine_actions.cpp)
target_link_libraries(test_preset_engine_actions PRIVATE lunar_host)
target_include_directories(test_preset_engine_actions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task #103 (GH #12): SOURCE NEGATIVE CONTROLS for the preset action acceptance. Each required
# control mutates ONE production source in an isolated shadow include tree (missing behaviour
# mirror / missing right-bank transfer / slot cross-wire / L-R bank cross-wire / SAVE reading an
# earlier committed state / INITIALISE stealing a LOAD / skipping the real apply) and asserts the
# acceptance terminates normally and turns RED on its named criterion; a retention positive control
# proves the unmutated build is GREEN. The constructed downstream-failure pair (an isolated fixture
# that makes the recalled candidate illegal, +/- a false-success owner) proves the D1 atomicity
# criterion is reachable and discriminating. POSIX-only (the driver invokes the compiler); on
# Windows the acceptance test still runs. Fast (header-only shadow builds), so no `slow` label.
if(UNIX)
  add_test(NAME preset_engine_actions_negative
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_preset_engine_negatives.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --compiler ${CMAKE_CXX_COMPILER})
endif()

# task #105 (GH #12): the APP one-shot startup restore / device-reopen retention / exit atomic save
# acceptance. It drives the REAL file adapter in a REAL temporary directory and the REAL engine
# candidate path (exact-length gate -> decode -> migrate -> validate -> pending -> applyDeviceState
# -> single stopped-stream publish; save -> encode -> save_state_atomic). Framework-free: it links
# only lunar_core + lunar_host's headers, so a plain CTest target is enough. ~64 rendered blocks, so
# it stays in the fast set.
lunar_add_test(test_app_state_store tests/host/test_app_state_store.cpp)
target_link_libraries(test_app_state_store PRIVATE lunar_host)
target_include_directories(test_app_state_store PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

# task #105 (GH #12): SOURCE NEGATIVE CONTROLS for the APP state acceptance. Each required control
# mutates ONE production source in an isolated shadow include tree (skip the startup apply / re-read
# every boundary / drop the pending transfer / overwrite a bad file on exit / let a short write report
# success / delete the live file before replace / swallow the typed SaveResult / drop or misplace the
# exit call) and asserts the acceptance terminates normally and turns RED on its NAMED criterion; a
# retention positive control proves the unmutated build is GREEN. POSIX-only (the driver invokes the
# compiler); on Windows the acceptance test still runs. Fast (header-only shadow builds).
if(UNIX)
  add_test(NAME app_state_store_negative
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_app_state_negatives.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --compiler ${CMAKE_CXX_COMPILER})
endif()

# task #98 (GH #15 D3): the real-restore divider acceptance renders the actual DSP per-sample over
# ~800k frames (a short run is under-sampled for the N=16 discriminator). It must not gate every
# feature push, so it carries the `slow` label the probe-gating (PR #28) introduced; the PR-only
# probe-gate runs it once.
set_tests_properties(test_d3_divider_restore PROPERTIES LABELS "slow")

# task #88 (GH #20): SOURCE NEGATIVE CONTROLS. The acceptance gate must DISCRIMINATE: GREEN on the
# real trapezoidal product and RED on a production PolivoksFilter source mutated away from the @Codex
# numeric contract (old cap / old recursion / wrong damp / wrong LP-BP / L-R cross / skip setter /
# wrong update order). This builds the header-only negative driver (gh20_trap_neg_driver.cpp) against
# an isolated shadowed header and asserts the reference comparison turns RED on each, with the real
# control GREEN. POSIX-only (the mutation rebuild invokes the compiler); on Windows this is
# skipped — the real acceptance gate still runs there.
if(UNIX)
  add_test(NAME gh20_vcf_negative
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_gh20_trap_negatives.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --compiler ${CMAKE_CXX_COMPILER})
endif()

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
