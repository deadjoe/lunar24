// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/window_metrics_win.cpp — the Windows half of the host geometry probes.
//
// host/plugin.cpp (shared across platforms) calls four extern "C" probes to get the
// screen's LOGICAL visible area and the DPI factor, then feeds them into the single
// framework-free geometry choke point (lunar24::host::compute_window_layout in
// window_layout.h). On macOS those probes live in main.mm (NSScreen.visibleFrame /
// backingScaleFactor). On Windows the bootstrap differs: the stock IPlugAPP_main.cpp
// (compiled from the pinned iPlug2::APP) owns WinMain, gHINSTANCE, gHWND and
// SaveWindowScreenshot, so THIS file defines only the four geometry probes and nothing
// else — redefining any of those symbols would be a duplicate-definition link error.
//
// Units follow the standard Windows logical-pixel model, mirrored from macOS:
//   * avail*     = LOGICAL window units (macOS: points; here: physical pixels / DPI).
//   * screenScale= logical -> physical (macOS: backingScaleFactor; here: DPI / 96).
//   * backing    = logical * screenScale  (compute_window_layout multiplies this itself).
// This file reads REAL values from the Win32 work area + system DPI, never a hardcoded
// desktop size (the P5-① mandate forbids a fixed 2400x1551 here).
//
// DPI model: the pinned IPlugAPP_main.cpp (compiled from iPlug2::APP) publishes a
// PER_MONITOR_AWARE_V2 DPI context via SetProcessDpiAwarenessContext before it calls
// CreateDialog on the host window — i.e. awareness is established by the stock bootstrap,
// not by a DPI manifest embedded in the .rc. GetSystemMetrics reports the primary physical
// work area and GetDeviceCaps(LOGPIXELSX)/96 is the system DPI scale factor, so we divide
// by the factor to hand the layout choke point LOGICAL units, matching macOS points. This
// shim still derives from the primary physical work area + the system DPI; exact per-monitor
// DPI / multi-monitor virtualization (and whether the NanoVG/GL2 window consumes these as
// logical-then-scaled or as physical) is a runtime detail to confirm on a real Windows host.
// The GH#10 scope here is that the target COMPILES and LINKS (actual-target CI) and that it
// feeds real work area + DPI numbers, not constants.
//
// Framework-free on purpose: this TU only needs <windows.h> + the C stdlib probes; it
// pulls no iPlug2 header, so it stays compilable on any Windows toolchain and never
// drags SWELL/IGNanoVG into the probe boundary.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdlib>
#include <cstring>

// --- geometry probes (extern "C" so plugin.cpp can resolve them by name) ----------------

// DPI factor (logical -> physical). Clamped to >= 1.0: a 96-DPI desktop or a task where
// the DC cannot be obtained must never report a fractional scale.
static double lunar_win_scale()
{
  const HDC dc = GetDC(nullptr);
  const int dpi = (dc != nullptr) ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc != nullptr) {
    ReleaseDC(nullptr, dc);
  }
  const double scale = static_cast<double>(dpi) / 96.0;
  return (scale < 1.0) ? 1.0 : scale;
}

// Primary work area in PHYSICAL pixels. GetSystemMetrics has NO SM_CXWORKAREA /
// SM_CYWORKAREA; the Win32 contract for the visible desktop rectangle is
// SystemParametersInfo(SPI_GETWORKAREA, ..., RECT*). We try that first and only fall back
// to the full screen size when it fails, so the geometry choke point always gets a real
// work-area rect and never a hardcoded desktop size. SPI_GETWORKAREA returns the work area
// RECT as COORDINATES, so the visible size is the SPAN (right-left x bottom-top), never the
// absolute right/bottom corner — when the taskbar is docked left or top, left/top are
// non-zero and the absolute corner would OVERESTIMATE the available space. We therefore
// always reduce to width/height via the span, for the SPI path AND the SM_CXSCREEN fallback
// path alike (both produce a RECT that goes through the same span math).
static RECT lunar_work_area()
{
  RECT rect = {0, 0, 0, 0};
  BOOL ok = SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0);
  if (!ok) {
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    rect.left = 0;
    rect.top = 0;
    rect.right = (w > 0) ? w : 0;
    rect.bottom = (h > 0) ? h : 0;
  }
  return rect;
}

// LOGICAL span width = physical work-area WIDTH / DPI factor (mirrors macOS points).
// The visible desktop is coordinates, not a size, so we must subtract left from right.
static double lunar_work_area_span_w()
{
  const RECT r = lunar_work_area();
  const LONG w = (r.right > r.left) ? (r.right - r.left) : 0;
  return static_cast<double>(w) / lunar_win_scale();
}

// LOGICAL span height = physical work-area HEIGHT / DPI factor.
static double lunar_work_area_span_h()
{
  const RECT r = lunar_work_area();
  const LONG h = (r.bottom > r.top) ? (r.bottom - r.top) : 0;
  return static_cast<double>(h) / lunar_win_scale();
}

extern "C" double lunar_host_avail_logical_w()
{
  return lunar_work_area_span_w();
}

extern "C" double lunar_host_avail_logical_h()
{
  return lunar_work_area_span_h();
}

extern "C" double lunar_host_screen_scale()
{
  return lunar_win_scale();
}

extern "C" bool lunar_host_force_clamp()
{
  const char* v = std::getenv("LUNAR_HOST_FORCE_CLAMP");
  return (v != nullptr) && (std::strcmp(v, "1") == 0);
}
