// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0

#include "about.h"

#include "build_info.h"
#include "lunar24_version.h"

#ifdef _WIN32
#include <windows.h>
#include <string>
#endif

namespace lunar24::host {

const char* aboutVersion() { return LUNAR24_VERSION; }

const char* aboutBuild() {
  return "Build " LUNAR24_BUILD_NUMBER " \xC2\xB7 " LUNAR24_BUILD_TIME " \xC2\xB7 " LUNAR24_BUILD_COMMIT;
}

const char* aboutCopyright() {
  return "\xC2\xA9 " LUNAR24_COPYRIGHT_YEAR " " LUNAR24_COPYRIGHT_HOLDER;
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? static_cast<size_t>(n) : 0u, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty()) w.pop_back();  // the terminating NUL
  return w;
}
}  // namespace

bool showAboutBox() {
  const std::string text = std::string("Lunar 24\nVersion ") + aboutVersion() + " (" + aboutBuild() +
                           ")\n\n" + aboutCopyright();
  MessageBoxW(GetActiveWindow(), widen(text).c_str(), L"About Lunar 24", MB_OK | MB_ICONINFORMATION);
  return true;
}
#endif

}  // namespace lunar24::host
