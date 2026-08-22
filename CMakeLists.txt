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

# Strict warnings-as-errors for Lunar-24-owned code.
add_compile_options(-Wall -Wextra -Wpedantic)

# Minimal bootstrap skeleton. Targets (framework-free lunar_core, tests, and the
# generator-driven registry) are added on the P0 foundation branch.
