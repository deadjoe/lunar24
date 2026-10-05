// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/about.h — the About Lunar 24 box: version, build stamp, copyright.
//   Lunar 24
//   Version 1.0.0 (Build 57 · 2026-10-05 16:40 UTC · abc1234)
//   [Bearbone.Studio logo]
//   © 2026 Bearbone.Studio
// The build stamp tells which CI run (Actions run number), when, and which commit made this app.

#ifndef HOST_ABOUT_H
#define HOST_ABOUT_H

namespace lunar24::host {

const char* aboutVersion();    // "1.0.0"
const char* aboutBuild();      // "Build 57 · 2026-10-05 16:40 UTC · abc1234" (UTF-8)
const char* aboutCopyright();  // "© 2026 Bearbone.Studio" (UTF-8)

// Shows the About box (macOS: main.mm, the standard about panel with the studio logo;
// Windows: about.cpp, a message box). Returns true when shown.
bool showAboutBox();

}  // namespace lunar24::host

#endif  // HOST_ABOUT_H
