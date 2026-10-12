// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/keyboard_focus.mm — does the panel's window get the keyboard now (macOS)? Shared by
// the app and the plugins: when it does not, no key-ups reach the panel, so it releases the
// computer keys it holds (plugin.cpp OnIdle).

#import <Cocoa/Cocoa.h>

// Not when another app is in front, or another window (a dialog, the DAW's own window) is.
extern "C" bool lunar_host_view_has_keyboard(void* view)
{
  NSView* v = (NSView*)view;
  return v != nil && [NSApp isActive] && v.window != nil && v.window.isKeyWindow;
}
