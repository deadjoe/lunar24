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

# The host (host/) depends on iPlug2 as an UNMODIFIED fixed-commit library. This gate
# verifies the local clone is pinned to exactly that commit AND its working tree is
# clean (a locally-edited pin is a secret fork). The clone is a local, non-vendored
# dependency, so on CI it is absent and the gate SKIPs loudly — never a silent absent
# guard. Same register-and-loudly-skip shape as evidence_layout_guard and the CoreAudio
# output test above.
add_test(NAME iplug_pin_clean_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_iplug_pin_clean.py)

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

function(lunar_add_test name src)
  add_executable(${name} ${src})
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
lunar_add_test(test_state_persistence tests/core/test_state_persistence.cpp)
lunar_add_test(test_drone_bank tests/core/test_drone_bank.cpp)
lunar_add_test(test_drone_mod tests/core/test_drone_mod.cpp)
lunar_add_test(test_vco tests/core/test_vco.cpp)
lunar_add_test(test_preamp_envelope tests/core/test_preamp_envelope.cpp)
lunar_add_test(test_mix_filter_distortion tests/core/test_mix_filter_distortion.cpp)
lunar_add_test(test_device_layout tests/core/test_device_layout.cpp)
lunar_add_test(test_executor_realpath tests/core/test_executor_realpath.cpp)
lunar_add_test(test_input_equivalence tests/core/test_input_equivalence.cpp)
lunar_add_test(test_keyboard_presets tests/core/test_keyboard_presets.cpp)
lunar_add_test(test_keyboard_mode tests/core/test_keyboard_mode.cpp)
lunar_add_test(test_keyboard_behaviour tests/core/test_keyboard_behaviour.cpp)
lunar_add_test(test_arp_sequencer tests/core/test_arp_sequencer.cpp)
lunar_add_test(test_keyboard_calibration tests/core/test_keyboard_calibration.cpp)
lunar_add_test(test_keyboard_menu tests/core/test_keyboard_menu.cpp)
lunar_add_test(test_host_window_fit tests/core/test_host_window_fit.cpp)
lunar_add_test(test_panel_transform tests/core/test_panel_transform.cpp)
lunar_add_test(test_panel_layout tests/core/test_panel_layout.cpp)
lunar_add_test(test_host_window_layout tests/host/test_host_window_layout.cpp)
target_link_libraries(test_host_window_layout PRIVATE lunar_host)
target_include_directories(test_host_window_layout PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")

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
# P5-① host STANDALONE (host/): a self-authored iPlug2 APP bootstrap that opens a
# real macOS window. Guarded to macOS + the pinned iPlug2 clone being present.
# iPlug2 is gitignored/absent on CI (so this is a no-op there — same
# register-and-loudly-skip shape as the iPlug pin gate), and it is never built on
# the cross-platform Linux/Windows test matrix (an OBJCXX/framework target).
# Its target is Lunar24Host (host/CMakeLists.txt); the host geometry is the pure
# lunar_host/lunar_core headers it links.
# ---------------------------------------------------------------------------
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/iPlug2")
  add_subdirectory(host)
endif()
