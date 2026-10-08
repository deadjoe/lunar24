// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_face_pref.h — the panel face the user picked, remembered across launches.
//
// It is a look preference, not the machine: RESET PANEL, keyboard presets and the
// state file do not touch it. The app and the plugins share one file in the settings
// folder (a sibling of lunar24-state.bin and lunar24-midi-map.bin). UI thread only.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <host/app_state_store.h>
#include <lunar24/core/state_persistence.h>

namespace lunar24::host {

inline constexpr const char* kPanelFaceFileName = "lunar24-panel-face.txt";
inline constexpr const char* kPanelFaceTempStem = "lunar24-panel-face.txt.tmp-";

// 0 when the folder is missing, the file is absent, or the bytes are not one index.
// A legal file is a single digit in 0..count-1, optionally followed by LF or CRLF.
inline int loadPanelFaceIndex(const std::string& directory, int count) {
  if (directory.empty() || count <= 0 || count > 10) return 0;
  const std::string live = app_state_file_ops::joinUtf8(directory, kPanelFaceFileName);
  std::FILE* f = app_state_file_ops::openNative(live, "rb");
  if (f == nullptr) return 0;
  char buf[4] = {};
  const std::size_t n = std::fread(buf, 1, sizeof buf, f);
  const bool ioError = std::ferror(f) != 0;
  const int extra = n < sizeof buf ? std::fgetc(f) : 0;
  std::fclose(f);
  if (ioError || n == 0 || n == sizeof buf || extra != EOF) return 0;
  if (buf[0] < '0' || buf[0] > '9') return 0;
  const int index = buf[0] - '0';
  if (index >= count) return 0;
  if (n == 2 && buf[1] != '\n') return 0;
  if (n == 3 && !(buf[1] == '\r' && buf[2] == '\n')) return 0;
  return index;
}

inline bool savePanelFaceIndex(const std::string& directory, int index, int count) {
  if (directory.empty() || index < 0 || index >= count || count > 10) return false;
  static std::atomic<std::uint64_t> counter{0u};
  std::string temp;
  for (int attempt = 0; attempt < 16 && temp.empty(); ++attempt) {
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1u, std::memory_order_relaxed);
    const std::string candidate = app_state_file_ops::joinUtf8(
        directory, std::string(kPanelFaceTempStem) + std::to_string(ticks) + "-" + std::to_string(seq));
    if (app_state_file_ops::reserveExclusiveCreate(candidate)) temp = candidate;
  }
  if (temp.empty()) return false;
  const char bytes[2] = {static_cast<char>('0' + index), '\n'};
  const std::string live = app_state_file_ops::joinUtf8(directory, kPanelFaceFileName);
  return core::save_state_atomic(reinterpret_cast<const std::uint8_t*>(bytes), 2, temp.c_str(), live.c_str(),
                                 app_state_file_ops::realOps(), nullptr) == core::SaveResult::ok;
}

}  // namespace lunar24::host
