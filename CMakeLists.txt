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
# Tests. Keep this list lean: one unit/behaviour test per module, plus a few
# end-to-end engine tests. No per-task probe/mutation/acceptance machinery.
# ---------------------------------------------------------------------------
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# The committed generated headers must match spec/machine/lunar24.json.
add_test(NAME registry_regen_zero_diff
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_registry.py --check)
# core/ stays framework-free (no iPlug2 / platform headers).
add_test(NAME core_headers_gate
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core_headers.py)

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

# Engine tests that also link a separate allocator-probe TU (checks the audio path never allocates).
lunar_add_test(test_machine_runtime tests/core/test_machine_runtime.cpp
               tests/core/test_machine_runtime_allocator.cpp)
lunar_add_test(test_machine_definition tests/core/test_machine_definition.cpp
               tests/core/test_machine_definition_allocator.cpp)
lunar_add_test(test_machine_control_sources tests/core/test_machine_control_sources.cpp
               tests/core/test_machine_control_sources_allocator.cpp)
lunar_add_test(test_device_adapter_oracle tests/core/test_device_adapter_oracle.cpp
               tests/core/test_device_adapter_oracle_allocator.cpp)

# These tests build large fixed-capacity runtimes on the stack; give them room.
if(MSVC)
  target_link_options(test_machine_runtime PRIVATE /STACK:4194304)
  target_link_options(test_machine_definition PRIVATE /STACK:4194304)
elseif(APPLE)
  target_link_options(test_machine_runtime PRIVATE -Wl,-stack_size,0x1000000)
endif()

foreach(t IN ITEMS
    core_contract
    registry
    time_semantics
    patch_graph
    graph_compiler
    sink_interpret
    state_publish
    state_persistence
    drone_bank
    polyblep_kernel
    drone_classic
    drone_mod
    vco
    preamp_envelope
    envelope_generator
    lfo
    joystick_cv
    five_step_sequencer
    mix_filter_distortion
    device_layout
    executor_realpath
    input_equivalence
    keyboard_presets
    keyboard_mode
    keyboard_behaviour
    arp_sequencer
    keyboard_pipeline
    keyboard_calibration
    keyboard_menu
    host_window_fit
    panel_transform
    panel_layout
    state_disposition
    state_validation
    effector
)
  lunar_add_test(test_${t} tests/core/test_${t}.cpp)
endforeach()

# Host-side engine tests (framework-free StandaloneAudioEngine; no iPlug2 needed).
function(lunar_add_host_test name)
  lunar_add_test(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lunar_host)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/core")
endfunction()
lunar_add_host_test(test_host_engine_oracle tests/host/test_host_engine_oracle.cpp tests/host/test_host_engine_oracle_allocator.cpp)
lunar_add_host_test(test_state_apply_oracle tests/host/test_state_apply_oracle.cpp tests/host/test_state_apply_oracle_allocator.cpp)
lunar_add_host_test(test_state_apply_oracle_169 tests/host/test_state_apply_oracle_169.cpp)
lunar_add_host_test(test_machine_control_outputs tests/host/test_machine_control_outputs.cpp)
lunar_add_host_test(test_machine_audio_families tests/host/test_machine_audio_families.cpp)
lunar_add_host_test(test_machine_cable_restore tests/host/test_machine_cable_restore.cpp)
lunar_add_host_test(test_vco_normal_source tests/host/test_vco_normal_source.cpp)
lunar_add_host_test(test_preset_engine_actions tests/host/test_preset_engine_actions.cpp)
lunar_add_host_test(test_app_state_store tests/host/test_app_state_store.cpp)
lunar_add_host_test(test_host_stream_plan tests/host/test_host_stream_plan.cpp)
lunar_add_host_test(test_host_window_layout tests/host/test_host_window_layout.cpp)
lunar_add_host_test(test_panel_ui_layout tests/host/test_panel_ui_layout.cpp)

# Offline renderer: listen to the engine without the app (see CLAUDE.md).
add_executable(lunar24_render tools/lunar24_render.cpp)
target_link_libraries(lunar24_render PRIVATE lunar_host)
lunar_enable_warnings(lunar24_render)
# SVG preview of the panel layout (review the UI without building the app).
add_executable(panel_preview tools/panel_preview.cpp)
target_link_libraries(panel_preview PRIVATE lunar_host)
lunar_enable_warnings(panel_preview)
if(MSVC)
  # Plain fopen/sscanf are fine for these command-line tools.
  target_compile_definitions(lunar24_render PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(panel_preview PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(APPLE)
  lunar_add_test(test_coreaudio_output tests/core/test_coreaudio_output.cpp)
  target_link_libraries(test_coreaudio_output PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
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
