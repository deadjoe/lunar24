// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The panel-face preference file: a missing file is the default cream, a saved
// index comes back, and a bad file is ignored rather than adopted as a face.

#include "mini_test.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <host/app_state_store.h>
#include <host/panel_face_pref.h>

using namespace lunar24;

static std::string makeTempDir(const char* tag) {
  static std::uint64_t seq = 0u;
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  const std::string name =
      std::string("lunar24-panel-face-") + tag + "-" + std::to_string(static_cast<unsigned long long>(seq++));
  const auto dir = base / std::filesystem::u8path(name);
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.u8string();
}

static void removeTree(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
}

static void writeBytes(const std::string& path, const char* bytes, std::size_t n) {
  std::FILE* f = host::app_state_file_ops::openNative(path, "wb");
  CHECK(f != nullptr);
  if (f == nullptr) return;
  CHECK_EQ(std::fwrite(bytes, 1, n, f), n);
  std::fclose(f);
}

int main() {
  constexpr int kCount = 10;
  CHECK_EQ(host::loadPanelFaceIndex("", kCount), 0);
  CHECK(!host::savePanelFaceIndex("", 3, kCount));

  const std::string dir = makeTempDir("roundtrip");
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 0);
  CHECK(host::savePanelFaceIndex(dir, 4, kCount));
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 4);
  CHECK(host::savePanelFaceIndex(dir, 0, kCount));
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 0);
  CHECK(!host::savePanelFaceIndex(dir, 10, kCount));
  CHECK(!host::savePanelFaceIndex(dir, -1, kCount));

  const std::string live = host::app_state_file_ops::joinUtf8(dir, host::kPanelFaceFileName);
  writeBytes(live, "7\r\n", 3);
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 7);
  writeBytes(live, "hello", 5);
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 0);
  writeBytes(live, "10\n", 3);
  CHECK_EQ(host::loadPanelFaceIndex(dir, kCount), 0);

  removeTree(dir);
  return 0;
}
