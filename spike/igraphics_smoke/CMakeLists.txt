# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# ①b — iPlug2 + IGraphics smoke test. Proves that the framework's GUI stack
# (IGraphics, here the NanoVG/Metal backend the design chose) compiles and can
# open a standalone window on this machine, WITHOUT drawing any Lunar 42N panel
# content. This is a disposable P1 spike artifact; it is NOT the Lunar thin
# wrapper (that comes later). It reuses the stock, known-good iPlug2
# IPlugVisualizer example source as the smoke target.
#
# The example dir (in the disposable iPlug2 clone) does its own `include(iPlug2.cmake)`
# + `find_package(iPlug2)`; we only set IPLUG2_DIR and add it as a subproject.
# The IGRAPHICS_BACKEND=NANOVG / IGRAPHICS_RENDERER=METAL cache vars are supplied
# at configure time (mirroring the iPlug2 macos preset).

cmake_minimum_required(VERSION 3.21)
project(iPlug2GraphicsSmoke VERSION 0.0.1)

set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/iPlug2"
    CACHE PATH "iPlug2 root directory"
    FORCE)

# Build ONLY the standalone/APP target (the example CMakeLists was edited to
# FORMATS APP) — never AU/VST3/CLAP/AAX.
add_subdirectory(${IPLUG2_DIR}/Examples/IPlugVisualizer IPlugVisualizer)
