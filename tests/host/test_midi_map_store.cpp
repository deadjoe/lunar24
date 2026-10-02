// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The MIDI bindings file: load/save round-trip in a real temp directory, the
// first-run and no-path outcomes, and the never-overwrite-a-foreign-file gate.

#include "mini_test.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <host/midi_map_store.h>

using namespace lunar24;

// Same UTF-8 temp-directory discipline as test_app_state_store.
static std::string makeTempDir(const char* tag) {
  static std::uint64_t seq = 0u;
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  const std::string name =
      std::string("lunar24-midi-map-") + tag + "-" + std::to_string(static_cast<unsigned long long>(seq++));
  const auto dir = base / std::filesystem::u8path(name);
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.u8string();
}

static void removeTree(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
}

static core::MidiBinding ccBinding(const char* dev, std::uint8_t ch, std::uint8_t num,
                                   core::ParameterId p) {
  core::MidiBinding b{};
  std::snprintf(b.key.device, core::kMidiBindingDeviceCapacity, "%s", dev);
  b.key.channel = ch;
  b.key.kind = core::MidiBindingKind::cc;
  b.key.number = num;
  b.parameter = p;
  return b;
}

static std::vector<std::uint8_t> readFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(bytes.data(), 1u, bytes.size(), f);
  std::fclose(f);
  bytes.resize(got);
  return bytes;
}

int main() {
  // First run: no file -> empty map, save allowed; save -> load round-trips.
  {
    const std::string dir = makeTempDir("roundtrip");
    host::MidiMapStore store;
    store.setDirectory(dir);
    CHECK(store.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK_EQ(store.map().count(), 0u);
    CHECK(store.bind(ccBinding("MPK mini IV", 1, 74, core::ParameterId::vcf_l_freq)));
    CHECK(store.bind(ccBinding("MPK mini IV", 1, 71, core::ParameterId::vcf_l_res)));
    CHECK(store.save());

    host::MidiMapStore again;
    again.setDirectory(dir);
    CHECK(again.load() == host::MidiMapLoadOutcome::Ok);
    CHECK_EQ(again.map().count(), 2u);
    CHECK(again.map().at(0).parameter == core::ParameterId::vcf_l_freq);
    CHECK(again.map().at(1).key.channel == 1);
    removeTree(dir);
  }
  // A malformed file: load rejects it, save refuses to overwrite it, bytes preserved.
  {
    const std::string dir = makeTempDir("malformed");
    host::MidiMapStore store;
    store.setDirectory(dir);
    const std::string live = store.livePath();
    const char garbage[17] = "not a midi map!!";
    std::FILE* f = std::fopen(live.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK_EQ(std::fwrite(garbage, 1u, sizeof(garbage), f), sizeof(garbage));
    std::fclose(f);

    CHECK(store.load() == host::MidiMapLoadOutcome::Malformed);
    CHECK_EQ(store.map().count(), 0u);
    CHECK(!store.save());  // never overwrite a file this session could not adopt
    const auto after = readFile(live);
    CHECK_EQ(after.size(), sizeof(garbage));
    CHECK(std::memcmp(after.data(), garbage, sizeof(garbage)) == 0);
    removeTree(dir);
  }
  // No directory: no path, no save.
  {
    host::MidiMapStore store;
    CHECK(store.load() == host::MidiMapLoadOutcome::NoPath);
    CHECK(!store.save());
  }
  // An oversized file is rejected by the size gate without a full read.
  {
    const std::string dir = makeTempDir("oversize");
    host::MidiMapStore store;
    store.setDirectory(dir);
    const std::string live = store.livePath();
    std::FILE* f = std::fopen(live.c_str(), "wb");
    CHECK(f != nullptr);
    std::vector<std::uint8_t> big(core::midi_map_wire_bytes(core::kMidiMapCapacity) + 1, 0x5A);
    CHECK_EQ(std::fwrite(big.data(), 1u, big.size(), f), big.size());
    std::fclose(f);
    CHECK(store.load() == host::MidiMapLoadOutcome::Malformed);
    CHECK(!store.save());
    removeTree(dir);
  }
  return test::finish("test_midi_map_store");
}
