# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# host/CMakeLists.txt — the Lunar 24 HOST STANDALONE executable (P5-①, task #31).
#
# This is deliberately NOT iplug_configure_app(): that helper injects
# IPlugAPP_main.cpp (the STOCK entry point). The whole point of P5-① is a
# SELF-AUTHORED bootstrap — our host/main.mm provides main() + SWELLAppMain — so we
# assemble the same APP machinery manually and compile iPlug2's library sources
# unmodified. third_party/iPlug2 stays a pinned, clean, fixed-commit library.
#
# iPlug2 is an UNMODIFIED library; the only Lunar-authored translation units are
# our own plugin.cpp + main.mm.

# Backend / renderer selection must precede find_package(iPlug2).
if(NOT DEFINED IGRAPHICS_BACKEND)
  set(IGRAPHICS_BACKEND "NANOVG" CACHE STRING "IGraphics drawing backend")
endif()
if(NOT DEFINED IGRAPHICS_RENDERER)
  set(IGRAPHICS_RENDERER "METAL" CACHE STRING "IGraphics renderer")
endif()

if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

if(NOT EXISTS "${IPLUG2_DIR}/iPlug2.cmake")
  message(FATAL_ERROR
    "Lunar24Host: iPlug2 not found at ${IPLUG2_DIR}. "
    "It is gitignored/absent on CI (the pin gate loud-skips there). "
    "On a dev machine, clone iPlug2 at commit d54f6905 into third_party/iPlug2.")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)
# IGRAPHICS_LIB is now set (e.g. iPlug2::IGraphics::NanoVG::Metal).

set(HOST_APP_NAME "Lunar24Host")

add_executable(${HOST_APP_NAME}
  plugin.cpp
  main.mm
)

# --- our includes + the iPlug2 APP/SWELL include paths -------------------------
target_include_directories(${HOST_APP_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${IPLUG2_DIR}/IPlug/APP"
  "${IPLUG2_DIR}/WDL/swell"
  "${IPLUG2_DIR}/Dependencies/IPlug/RTAudio"
  "${IPLUG2_DIR}/Dependencies/IPlug/RTMidi"
)

# --- compile definitions the standalone APP needs ------------------------------
target_compile_definitions(${HOST_APP_NAME} PRIVATE
  APP_API
  IPLUG_EDITOR=1
  IPLUG_DSP=1
  __MACOSX_CORE__
  SWELL_COMPILED
)

# --- iPlug2 library targets (IGraphics + IPlug + the selected drawing backend) --
# lunar_host -> lunar_core gives plugin.cpp the lunar24/core + lunar24/host include
# paths (the geometry headers are header-only; this only wires their locations).
target_link_libraries(${HOST_APP_NAME} PRIVATE
  lunar_host
  ${IGRAPHICS_LIB}
  "-framework AppKit"
  "-framework Carbon"
  "-framework CoreMIDI"
  "-framework CoreAudio"
)

# --- APP-side sources. IPLUGD2 IPlugAPP_main.cpp is EXCLUDED on purpose. --------
set(APP_SRC_LIST
  "${IPLUG2_DIR}/IPlug/APP/IPlugAPP.cpp"
  "${IPLUG2_DIR}/IPlug/APP/IPlugAPP_dialog.cpp"
  "${IPLUG2_DIR}/IPlug/APP/IPlugAPP_host.cpp"
)
target_sources(${HOST_APP_NAME} PRIVATE
  ${APP_SRC_LIST}
  "${IPLUG2_DIR}/Dependencies/IPlug/RTAudio/RtAudio.cpp"
  "${IPLUG2_DIR}/Dependencies/IPlug/RTMidi/RtMidi.cpp"
)

# --- SWELL sources (mac) -------------------------------------------------------
set(SWELL_DIR "${IPLUG2_DIR}/WDL/swell")
set(SWELL_SRC_LIST
  "${SWELL_DIR}/swell-appstub.mm"
  "${SWELL_DIR}/swellappmain.mm"
  "${SWELL_DIR}/swell-ini.cpp"
  "${SWELL_DIR}/swell-dlg.mm"
  "${SWELL_DIR}/swell-kb.mm"
  "${SWELL_DIR}/swell-miscdlg.mm"
  "${SWELL_DIR}/swell-menu.mm"
  "${SWELL_DIR}/swell-wnd.mm"
  "${SWELL_DIR}/swell.cpp"
  "${SWELL_DIR}/swell-misc.mm"
  "${SWELL_DIR}/swell-gdi.mm"
)
target_sources(${HOST_APP_NAME} PRIVATE ${SWELL_SRC_LIST})

# --- On Apple: APP sources compile as ObjC++; SWELL sources need Cocoa prefixed --
set_source_files_properties(${APP_SRC_LIST}
  TARGET_DIRECTORY ${HOST_APP_NAME}
  PROPERTIES LANGUAGE OBJCXX
)
set_source_files_properties(${SWELL_SRC_LIST}
  TARGET_DIRECTORY ${HOST_APP_NAME}
  PROPERTIES
  LANGUAGE OBJCXX
  COMPILE_FLAGS "-Wno-deprecated-declarations -include ${IPLUG2_DIR}/IPlug/IPlugOBJCPrefix.pch"
)

# --- bundle ------------------------------------------------------------
set_target_properties(${HOST_APP_NAME} PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Lunar24Host-macOS-Info.plist"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
  OUTPUT_NAME "${HOST_APP_NAME}"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
)

# PkgInfo for non-Xcode generators (mirrors iPlug2's APP.cmake).
if(NOT XCODE)
  set(HOST_PKGINFO_SRC "${CMAKE_CURRENT_BINARY_DIR}/PkgInfo_${HOST_APP_NAME}")
  file(WRITE "${HOST_PKGINFO_SRC}" "APPL????")
  add_custom_command(TARGET ${HOST_APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:${HOST_APP_NAME}>/Contents"
    COMMAND ${CMAKE_COMMAND} -E copy "${HOST_PKGINFO_SRC}" "$<TARGET_BUNDLE_DIR:${HOST_APP_NAME}>/Contents/PkgInfo"
    COMMENT "Creating PkgInfo for ${HOST_APP_NAME}.app"
  )
endif()
