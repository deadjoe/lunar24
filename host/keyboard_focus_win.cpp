// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/keyboard_focus_win.cpp — does the panel's window get the keyboard now (Windows)?
// Shared by the app and the plugins: when it does not, no key-ups reach the panel, so it
// releases the computer keys it holds (plugin.cpp OnIdle).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// Not when another app is in front, or another top-level window (a dialog, the DAW's own
// window) is.
extern "C" bool lunar_host_view_has_keyboard(void* view)
{
  HWND child = static_cast<HWND>(view);
  if (child == nullptr) return false;
  return GetForegroundWindow() == GetAncestor(child, GA_ROOT);
}
