cmake_minimum_required(VERSION 3.24)

project(Lunar24
  VERSION 0.0.1
  DESCRIPTION "Open-source desktop emulation of the ELTA Solar 42N"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Strict warnings on Lunar-24-owned code. -Werror is applied per-target below so
# third-party build units (iPlug2, P1+) are never forced to be warning-clean.
# MSVC uses /W4 + /WX; GCC/Clang use -Wall -Wextra -Wpedantic -Werror.
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
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

# Static gates: every core public header carries the SPDX Apache-2.0 header and
# stays framework-free (forbidden-include scan).
add_test(NAME core_headers_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core_headers.py)

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

function(lunar_add_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lunar_core)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lunar_add_test(test_core_contract tests/core/test_core_contract.cpp)
lunar_add_test(test_registry tests/core/test_registry.cpp)
