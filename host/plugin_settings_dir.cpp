// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0

#include "plugin_settings_dir.h"

#include "config.h"

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace lunar24::host {

std::string plugin_settings_dir()
{
#if defined(_WIN32)
  PWSTR wide = nullptr;
  std::string dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide))) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      dir.resize(static_cast<std::size_t>(n - 1));
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, dir.data(), n, nullptr, nullptr);
      dir += "\\" BUNDLE_NAME "\\";
    }
  }
  CoTaskMemFree(wide);  // also required when the call failed
  return dir;
#else
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::string();
  return std::string(home) + "/Library/Application Support/" BUNDLE_NAME "/";
#endif
}

}  // namespace lunar24::host
