// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Windows window geometry and lifecycle. The stock bootstrap owns WinMain;
// our per-window subclass owns sizing, DPI changes and monitor-relative maximize.
// Screen probes return logical units; Win32 rectangles and track limits are physical.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>  // IGraphics/WDL declarations with WIN32_LEAN_AND_MEAN
#include "IGraphics.h"
#include <host/panel_theme.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <host/window_layout.h>

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

// Put the panel view at (x, y) client pixels (a maximised window is wider or taller than the panel).
extern "C" void lunar_host_place_view(void* view, double x, double y)
{
  HWND child = static_cast<HWND>(view);
  if (child == nullptr) {
    return;
  }
  SetWindowPos(child, nullptr, static_cast<int>(x), static_cast<int>(y), 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// No case round the panel on Windows.
extern "C" void lunar_host_case_margins(void* view, double* side, double* top, double* bottom)
{
  static_cast<void>(view);
  *side = 0.0;
  *top = 0.0;
  *bottom = 0.0;
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

// --- standalone window sizing ----------------------------------------------------------
namespace {
using iplug::igraphics::IGraphics;
constexpr UINT_PTR kWindowSubclass = 1;

struct WindowState {
  IGraphics* graphics;
  double designW, designH;
  bool firstShow = true;
};

UINT window_dpi(HWND hwnd) {
  // Same Windows 10 API used by iPlug2; keep a fallback for older systems.
  using GetDpi = UINT(WINAPI*)(HWND);
  static const auto getDpi = reinterpret_cast<GetDpi>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  const UINT dpi = getDpi ? getDpi(hwnd) : 0;
  return dpi ? dpi : static_cast<UINT>(96.0 * lunar_win_scale());
}

struct WindowGeometry {
  MONITORINFO monitor{};
  int ncX = 0, ncY = 0;
  lunar24::host::ClientLimits client{};
};

WindowGeometry geometry(HWND hwnd, const WindowState& state, UINT dpi, const RECT* proposed = nullptr) {
  WindowGeometry out;
  out.monitor.cbSize = sizeof(out.monitor);
  const HMONITOR monitor = proposed ? MonitorFromRect(proposed, MONITOR_DEFAULTTONEAREST)
                                   : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(monitor, &out.monitor)) {
    out.monitor.rcWork = lunar_work_area();
    out.monitor.rcMonitor = out.monitor.rcWork;
  }
  RECT frame{};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
  static const auto adjust = reinterpret_cast<AdjustForDpi>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
  // Derive the normal sizing frame, even while maximized or changing monitors.
  if (adjust) adjust(&frame, style & ~WS_MAXIMIZE, GetMenu(hwnd) != nullptr, exStyle, dpi);
  else AdjustWindowRectEx(&frame, style & ~WS_MAXIMIZE, GetMenu(hwnd) != nullptr, exStyle);
  out.ncX = frame.right - frame.left;
  out.ncY = frame.bottom - frame.top;
  const RECT& work = out.monitor.rcWork;
  out.client = lunar24::host::windows_client_limits(
      work.right - work.left - out.ncX, work.bottom - work.top - out.ncY,
      dpi / 96.0, state.designW, state.designH);
  return out;
}

void write_client(RECT& rect, int edge, int ncX, int ncY, int clientW, int clientH) {
  const int winW = clientW + ncX, winH = clientH + ncY;
  const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
  const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
  if (left) rect.left = rect.right - winW;
  else rect.right = rect.left + winW;
  if (top) rect.top = rect.bottom - winH;
  else rect.bottom = rect.top + winH;
}

void fit_rect(RECT& rect, const WindowGeometry& g, const WindowState& state, bool maximized) {
  int w = std::clamp<int>(rect.right - rect.left - g.ncX, g.client.minW, g.client.maxW);
  int h = std::clamp<int>(rect.bottom - rect.top - g.ncY, g.client.minH, g.client.maxH);
  if (maximized) { w = g.client.maxW; h = g.client.maxH; }
  lunar24::host::largest_client_for_aspect(w, h, state.designW, state.designH, w, h);
  const RECT& work = g.monitor.rcWork;
  const int x = maximized ? work.left + (work.right - work.left - w - g.ncX) / 2
                         : std::clamp<int>(rect.left, work.left, work.right - w - g.ncX);
  const int y = maximized ? work.top + (work.bottom - work.top - h - g.ncY) / 2
                         : std::clamp<int>(rect.top, work.top, work.bottom - h - g.ncY);
  rect = {x, y, x + w + g.ncX, y + h + g.ncY};
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR id, DWORD_PTR data) {
  auto& state = *reinterpret_cast<WindowState*>(data);
  if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, window_proc, id);
    delete &state;
    return DefSubclassProc(hwnd, msg, wp, lp);
  }
  if (msg == WM_ERASEBKGND) {
    // Integer logical/physical conversion can leave a one-pixel strip.
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto color = lunar24::host::theme::windowGap();
    HDC dc = reinterpret_cast<HDC>(wp);
    const COLORREF previous = SetDCBrushColor(dc, RGB(color.r, color.g, color.b));
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, previous);
    return TRUE;
  }
  if (msg == WM_SHOWWINDOW && wp && state.firstShow) {
    state.firstShow = false;
    const auto g = geometry(hwnd, state, window_dpi(hwnd));
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    // Stock ClientResize centers only the client against the full primary screen.
    // Center the complete framed window inside this monitor's taskbar-free area.
    fit_rect(rect, g, state, false);
    const RECT& work = g.monitor.rcWork;
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                 work.top + (work.bottom - work.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (msg == WM_DPICHANGED) {
    const UINT dpi = HIWORD(wp);
    RECT rect = *reinterpret_cast<RECT*>(lp);
    const auto g = geometry(hwnd, state, dpi, &rect);
    // Update the backing buffer before WM_SIZE fits the panel in logical units.
    // EditorResize acknowledges this without changing the native parent rectangle.
    state.graphics->SetScreenScale(static_cast<float>(dpi) / 96.f);
    fit_rect(rect, g, state, IsZoomed(hwnd) != FALSE);
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }
  if (msg == WM_SIZING) {
    const auto g = geometry(hwnd, state, window_dpi(hwnd));
    auto& rect = *reinterpret_cast<RECT*>(lp);
    int w = std::clamp<int>(rect.right - rect.left - g.ncX, g.client.minW, g.client.maxW);
    int h = std::clamp<int>(rect.bottom - rect.top - g.ncY, g.client.minH, g.client.maxH);
    lunar24::host::client_size_for_aspect(w, h, state.designW, state.designH,
                                         wp == WMSZ_TOP || wp == WMSZ_BOTTOM);
    write_client(rect, static_cast<int>(wp), g.ncX, g.ncY, w, h);
    return TRUE;
  }
  const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
  if (msg == WM_GETMINMAXINFO) {
    const auto g = geometry(hwnd, state, window_dpi(hwnd));
    auto& mmi = *reinterpret_cast<MINMAXINFO*>(lp);
    mmi.ptMinTrackSize = {g.client.minW + g.ncX, g.client.minH + g.ncY};
    mmi.ptMaxTrackSize = {g.client.maxW + g.ncX, g.client.maxH + g.ncY};
    mmi.ptMaxSize = mmi.ptMaxTrackSize;
    const RECT& work = g.monitor.rcWork;
    mmi.ptMaxPosition.x = lunar24::host::maximized_position(
        work.left, g.monitor.rcMonitor.left, work.right - work.left, mmi.ptMaxSize.x);
    mmi.ptMaxPosition.y = lunar24::host::maximized_position(
        work.top, g.monitor.rcMonitor.top, work.bottom - work.top, mmi.ptMaxSize.y);
  }
  return result;
}
}  // namespace

// Run once, before the stock dialog's initial ClientResize/ShowWindow. Its integer
// screenScale truncates 125/150/175%; correct it before the first visible frame.
extern "C" void lunar_host_init_window(IGraphics* graphics, double designW, double designH) {
  HWND child = static_cast<HWND>(graphics->GetWindow());
  HWND root = GetAncestor(child, GA_ROOT);
  if (!root) return;
  auto* state = new WindowState{graphics, designW, designH};
  if (!SetWindowSubclass(root, window_proc, kWindowSubclass, reinterpret_cast<DWORD_PTR>(state))) {
    delete state;
    return;
  }
  const UINT dpi = window_dpi(root);
  const auto g = geometry(root, *state, dpi);
  graphics->SetScreenScale(static_cast<float>(dpi) / 96.f);
  // Leave room for title, menu, sizing border and taskbar at the first open too.
  graphics->GetDelegate()->OnParentWindowResize(g.client.maxW, g.client.maxH);
}

// --- REC: the recordings folder (Music\Lunar 24) ------------------------------------------
extern "C" bool lunar_host_recordings_dir(char* out, size_t capacity)
{
  PWSTR music = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Music, KF_FLAG_CREATE, nullptr, &music))) return false;
  std::wstring dir = std::wstring(music) + L"\\Lunar 24";
  CoTaskMemFree(music);
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
  return WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out, static_cast<int>(capacity), nullptr, nullptr) > 0;
}

// REC: show the recordings folder in Explorer after a recording stops.
extern "C" void lunar_host_reveal_dir(const char* utf8Path)
{
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, nullptr, 0);
  if (n <= 0) return;
  std::wstring path(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, &path[0], n);
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
