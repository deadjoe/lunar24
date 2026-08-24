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
