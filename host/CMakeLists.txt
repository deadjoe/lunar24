# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# host/CMakeLists.txt — the Lunar24Host STANDALONE executable (P5-①, task #31; GH#10).
#
# ONE cross-platform target, two trusted entries:
#   macOS   — a SELF-AUTHORED, nib-free bootstrap (host/main.mm provides main() +
#             SWELLAppMain), mac SWELL, Metal, bundle/PkgInfo. Assembled manually because
#             we deliberately EXCLUDE iPlug2's stock IPlugAPP_main.cpp (that would bring a
#             second main() and, worse, edit iPlug2). The bootstrap replicates the nib
#             lifecycle a CLT-only machine cannot drive with ibtool.
#   Windows — the SAME plugin.cpp, driven by the pinned iPlug2::APP + iplug_configure_app
#             (stock IPlugAPP_main.cpp, RtAudio/RtMidi, ASIO/DS/MM sources, WIN32_EXECUTABLE)
#             plus a repo-owned Win32 window-metrics shim (host/window_metrics_win.cpp) that
#             feeds the geometry choke point REAL work-area / system-DPI values, and the
#             repo-owned host/resources/main.rc (dialog/menu/accelerator/version, using the
#             existing host/resource.h). The mac .rc_mac_* files never stand in for a Win
#             resource.
#
# Renderer is platform-appropriate (NanoVG/Metal on Apple, NanoVG/GL2 on Windows) and is
# set EXPLICITLY per platform so a stale cache value from a mac configure can never inject
# a METAL renderer into a Windows build (GH#10 requirement: renderer is not the mac cache).
#
# third_party/iPlug2 stays an UNMODIFIED pinned submodule. The ONLY Lunar-authored
# translation units here are plugin.cpp, main.mm (mac), window_metrics_win.cpp (win), and
# the .rc resources.
#
# Linux is never reached here: root CMakeLists.txt only add_subdirectory(host) on
# APPLE OR WIN32, and FATALs if the iPlug2 submodule is missing (GH#10, no EXISTS skip).

# Backend / renderer selection must precede find_package(iPlug2). These are FORCED, not
# merely cached: the renderer is fully determined by the platform, and a stale cache value
# from a macOS configure (METAL) must never be able to leak into a Windows build (GL2) in
# the same build tree — GH#10 requires the renderer not to be polluted by the mac cache.
set(IGRAPHICS_BACKEND "NANOVG" CACHE STRING "IGraphics drawing backend" FORCE)
if(APPLE)
  set(IGRAPHICS_RENDERER "METAL" CACHE STRING "IGraphics renderer" FORCE)
elseif(WIN32)
  set(IGRAPHICS_RENDERER "GL2" CACHE STRING "IGraphics renderer" FORCE)
endif()

if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

if(NOT EXISTS "${IPLUG2_DIR}/iPlug2.cmake")
  message(FATAL_ERROR
    "Lunar24Host: the pinned iPlug2 submodule is not at ${IPLUG2_DIR}. "
    "Run `git submodule update --init --recursive` (GH#10 requires the submodule "
    "on macOS and Windows).")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)
# IGRAPHICS_LIB is now set (e.g. iPlug2::IGraphics::NanoVG::Metal / NanoVG).

set(HOST_APP_NAME "Lunar24Host")
set(HOST_OUT_DIR "${CMAKE_BINARY_DIR}/out")

function(lunar_host_common target)
  # plugin.cpp (shared, framework-free-ish C++) links lunar_host -> lunar_core for the
  # geometry headers, plus the selected IGraphics backend. config.h/resource.h resolve
  # from the host/ directory (their own directory is the include root).
  target_link_libraries(${target} PRIVATE lunar_host ${IGRAPHICS_LIB})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# Strict warnings are applied ONLY to the Lunar-authored translation units, never to the
# iPlug2 / third-party sources that the INTERFACE libraries or this target also compile
# (those are not ours to force warning-clean). `TARGET` is the consumed executable.
#
# The call sites pass each Lunar TU as a SEPARATE argument (mac: "plugin.cpp" "main.mm";
# win: "plugin.cpp" "window_metrics_win.cpp"), so the trailing ones land in ARGN and must be
# merged with the named `owned_srcs` (the first one) — otherwise the later TU silently gets
# no warning flags. `set_source_files_properties` applies compile options per source FILE,
# so scoping is per Lunar-authored TU and never bleeds onto third-party sources.
function(lunar_host_owned_warnings target owned_srcs)
  set(_lunar_srcs "${owned_srcs}")
  list(APPEND _lunar_srcs ${ARGN})
  if(MSVC)
    set_source_files_properties(${_lunar_srcs} TARGET_DIRECTORY ${target} PROPERTIES
      COMPILE_OPTIONS "/W4")
  else()
    set_source_files_properties(${_lunar_srcs} TARGET_DIRECTORY ${target} PROPERTIES
      COMPILE_OPTIONS "-Wall;-Wextra")
  endif()
endfunction()

if(APPLE)
  # -------------------------------------------------------------------------
  # macOS: self-authored bootstrap (manual assembly, no iPlug_configure_app).
  # -------------------------------------------------------------------------
  add_executable(${HOST_APP_NAME}
    plugin.cpp
    main.mm
  )

  target_include_directories(${HOST_APP_NAME} PRIVATE
    "${IPLUG2_DIR}/IPlug/APP"
    "${IPLUG2_DIR}/WDL/swell"
    "${IPLUG2_DIR}/Dependencies/IPlug/RTAudio"
    "${IPLUG2_DIR}/Dependencies/IPlug/RTMidi"
  )

  # --- compile definitions the standalone APP needs --------------------------
  target_compile_definitions(${HOST_APP_NAME} PRIVATE
    APP_API
    IPLUG_EDITOR=1
    IPLUG_DSP=1
    __MACOSX_CORE__
    SWELL_COMPILED
  )

  lunar_host_common(${HOST_APP_NAME})
  lunar_host_owned_warnings(${HOST_APP_NAME} "plugin.cpp" "main.mm")

  target_link_libraries(${HOST_APP_NAME} PRIVATE
    "-framework AppKit"
    "-framework Carbon"
    "-framework CoreMIDI"
    "-framework CoreAudio"
  )

  # --- APP-side sources. Stock IPlugAPP_main.cpp is EXCLUDED on purpose. ----
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

  # --- SWELL sources (mac) ----------------------------------------------------
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

  # --- On Apple: APP sources compile as ObjC++; SWELL sources need Cocoa prefix.
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
    RUNTIME_OUTPUT_DIRECTORY "${HOST_OUT_DIR}"
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

elseif(WIN32)
  # -------------------------------------------------------------------------
  # Windows: pinned iPlug2::APP + iplug_configure_app (stock main.cpp), a repo-owned
  # window-metrics shim, and the repo-owned main.rc.
  # -------------------------------------------------------------------------
  add_executable(${HOST_APP_NAME}
    plugin.cpp
    window_metrics_win.cpp
    resources/main.rc
  )
  # Tell the RC compiler where to find resource.h (the .rc includes it by name).
  set_source_files_properties(resources/main.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}\""
  )

  # iPlug2::APP supplies APP_API / IPLUG_EDITOR / IPLUG_DSP, the stock IPlugAPP_main.cpp
  # (WinMain), RtAudio/RtMidi, ASIO/DS/MM sources, and WIN32_EXECUTABLE -> out/...exe.
  iplug_configure_app(${HOST_APP_NAME} "${HOST_APP_NAME}")

  lunar_host_common(${HOST_APP_NAME})
  lunar_host_owned_warnings(${HOST_APP_NAME} "plugin.cpp" "window_metrics_win.cpp")
else()
  message(FATAL_ERROR "Lunar24Host only supports APPLE and WIN32 (root CMake gates this).")
endif()
