// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared profiles: persistence, switching, failed writes and concurrent instance conflicts.

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
  // The product's native-path boundary (wide on Windows); plain std::fopen would trip
  // MSVC's secure-CRT deprecation (C4996) under /WX.
  std::FILE* f = host::app_state_file_ops::openNative(path, "rb");
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

static void corrupt(const std::string& path, std::size_t size = 17) {
  auto* f = host::app_state_file_ops::openNative(path, "wb");
  CHECK(f != nullptr);
  if (f) { const std::vector<std::uint8_t> bytes(size, 0x5a); std::fwrite(bytes.data(), 1, size, f); std::fclose(f); }
}

int main() {
  const auto dir = makeTempDir("profiles-UTF8-\xc3\xa9");
  const auto legacyPath = host::app_state_file_ops::joinUtf8(dir, "lunar24-midi-map.bin");
  corrupt(legacyPath);
  const auto legacyBytes = readFile(legacyPath);
  host::MidiMapStore store;
  store.setDirectory(dir);
  CHECK(store.load() == host::MidiMapLoadOutcome::NoFile);
  const auto first = store.id();
  CHECK_EQ(store.map().count(), 0u);
  CHECK(readFile(legacyPath) == legacyBytes);
  CHECK(store.rename("Studio"));
  auto binding = ccBinding("Controller A", 3, 24, core::ParameterId::vcf_l_freq);
  binding.mode = core::MidiInputMode::relativeTwosComplement;
  CHECK(store.bind(binding));
  core::MidiRigSettings settings{3, 12, core::MidiVelocityCurve::soft, 48};
  CHECK(store.setSettings(settings));
  CHECK(store.save());
  CHECK(!store.dirty());
  CHECK(store.create("Live", true));
  const auto second = store.id();
  CHECK(first != second);
  CHECK(store.map().at(0).mode == core::MidiInputMode::relativeTwosComplement);
  CHECK_EQ(store.settings().splitNote, 48);
  CHECK(store.unbind(binding.key));
  CHECK(store.save());
  CHECK_EQ(store.map().count(), 0u);
  CHECK_EQ(store.name(), "Live");
  CHECK_EQ(store.settings().channelFilter, 3);
  CHECK(store.select(first));
  CHECK_EQ(store.map().count(), 1u);
  CHECK(!store.create("studio", false));
  CHECK(!store.rename(""));
  CHECK(!store.rename(" leading"));
  CHECK(!store.rename(std::string(40, 'a')));
  CHECK(!store.rename("\xe4\xb8\xad\xe6\x96\x87"));
  CHECK_EQ(store.name(), "Studio");
  CHECK(!store.select("../../unrelated"));
  CHECK_EQ(store.id(), first);

  // The shared library remembers selection but does not change an open instance.
  host::MidiMapStore other;
  other.setDirectory(dir);
  CHECK(other.load() == host::MidiMapLoadOutcome::Ok);
  CHECK_EQ(other.id(), first);
  CHECK(store.select(second));
  CHECK_EQ(other.id(), first);
  CHECK(other.select(second));
  CHECK_EQ(other.map().count(), 0u);
  CHECK(other.select(first));
  CHECK(other.rename("Studio renamed"));
  CHECK(store.refresh());
  CHECK_EQ(store.profiles().size(), 2u);
  CHECK(store.select(first));
  CHECK_EQ(store.name(), "Studio renamed");

  // Independent instances must not silently overwrite a newer edit of the same profile.
  CHECK(other.bind(ccBinding("Controller B", 10, 21, core::ParameterId::effector_blend)));
  CHECK(other.save());
  CHECK(store.unbind(binding.key));
  CHECK(!store.save());
  CHECK(store.dirty());
  CHECK(!store.select(second));
  CHECK(store.create("Recovered", true));
  CHECK_EQ(store.map().count(), 0u);
  CHECK(other.select(first));
  CHECK_EQ(other.map().count(), 2u);

  // Save faults leave both the original file and local edits intact; retry succeeds.
  CHECK(store.select(second));
  const auto original = readFile(store.livePath());
  CHECK(store.bind(binding));
  const auto real = host::app_state_file_ops::realOps();
  for (int stage = 0; stage < 3; ++stage) {
    auto broken = real;
    if (stage == 0) broken.writeFile = [](void*, const char*, const std::uint8_t*, std::size_t) { return false; };
    if (stage == 1) broken.flushFile = [](void*, const char*) { return false; };
    if (stage == 2) broken.atomicReplace = [](void*, const char*, const char*) { return false; };
    store.setFileOps(broken, nullptr);
    CHECK(!store.save());
    CHECK(store.dirty());
    CHECK(readFile(store.livePath()) == original);
    CHECK(!store.select(first));
    CHECK_EQ(store.id(), second);
  }
  store.setFileOps(real, nullptr);
  CHECK(store.save());
  CHECK(store.error().empty());
  {
    host::midi_profile_files::Lock lock(host::app_state_file_ops::joinUtf8(store.libraryPath(), "library.lock"));
    CHECK(static_cast<bool>(lock));
    CHECK(!other.refresh());
    CHECK(!store.save());
  }
  CHECK(store.save());

  // A failed last-selection write must not half-switch or leave a new orphan profile.
  auto preferenceFailure = real;
  preferenceFailure.atomicReplace = [](void*, const char* from, const char* to) {
    if (std::filesystem::u8path(to).filename() == "selected.txt") return false;
    return host::app_state_file_ops::realOps().atomicReplace(nullptr, from, to);
  };
  store.setFileOps(preferenceFailure, nullptr);
  CHECK(!store.select(first));
  CHECK_EQ(store.id(), second);
  CHECK(store.refresh());
  const auto profileCount = store.profiles().size();
  CHECK(!store.create("Failed creation", true));
  CHECK_EQ(store.id(), second);
  CHECK(store.refresh());
  CHECK_EQ(store.profiles().size(), profileCount);
  store.setFileOps(real, nullptr);

  // A bad target never replaces the working map; bad records are not overwritten at startup.
  const auto live = store.livePath();
  corrupt(live);
  CHECK(!store.select(second));
  CHECK_EQ(store.map().count(), 1u);
  CHECK(!store.save());
  CHECK_EQ(readFile(live).size(), 17u);
  CHECK(store.select(first));
  CHECK(store.removeCurrent());
  CHECK(!store.removeCurrent());  // one remaining readable profile (plus the corrupt file)
  CHECK(store.create("Fresh", false));
  CHECK_EQ(store.map().count(), 0u);
  CHECK_EQ(store.settings().octaveShift, 12);  // NEW keeps the current playing setup
  CHECK(store.removeCurrent());
  removeTree(dir);

  const auto badDir = makeTempDir("all-bad");
  host::MidiMapStore bad;
  bad.setDirectory(badDir);
  CHECK(bad.load() == host::MidiMapLoadOutcome::NoFile);
  corrupt(bad.livePath(), host::kMidiProfileMaxBytes + 1);
  host::MidiMapStore reopened;
  reopened.setDirectory(badDir);
  CHECK(reopened.load() == host::MidiMapLoadOutcome::Malformed);
  CHECK(!reopened.editable());
  CHECK(!reopened.save());
  CHECK(reopened.create("New setup", false));
  CHECK(reopened.editable());
  removeTree(badDir);
  // Recovery never overwrites another instance or drops edits before a successful read.
  const auto recoveryDir = makeTempDir("recovery");
  {
    host::MidiMapStore a, b;
    a.setDirectory(recoveryDir); b.setDirectory(recoveryDir);
    CHECK(a.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK(a.rename("First"));
    const auto initial = a.id();
    CHECK(b.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(a.bind(binding)); CHECK(a.save());
    CHECK(b.setSettings(settings)); CHECK(!b.save());
    CHECK(b.dirty());
    const auto generation = b.generation();
    {
      host::midi_profile_files::Lock lock(host::app_state_file_ops::joinUtf8(a.libraryPath(), "library.lock"));
      CHECK(!b.reload());
      CHECK(b.dirty()); CHECK_EQ(b.generation(), generation);
      host::MidiMapStore starting;
      starting.setDirectory(recoveryDir);
      CHECK(starting.load() == host::MidiMapLoadOutcome::Unreadable);
      CHECK(!starting.editable()); CHECK(!starting.error().empty());
    }
    b.setFileOps(preferenceFailure, nullptr);
    CHECK(!b.reload()); CHECK(b.dirty()); CHECK_EQ(b.generation(), generation);
    b.setFileOps(real, nullptr);
    CHECK(b.reload()); CHECK(!b.dirty()); CHECK(b.error().empty());
    CHECK_EQ(b.map().count(), 1u); CHECK_EQ(b.settings().channelFilter, 0);
    CHECK(b.generation() > generation);
    CHECK(b.setSettings(settings)); CHECK(b.save()); // editing works again
    CHECK(a.reload()); CHECK_EQ(a.settings().channelFilter, 3);
    CHECK(a.create("Second", false));
    const auto alternate = a.id();
    CHECK(a.select(initial));
    const auto damaged = a.livePath();
    corrupt(damaged);
    host::MidiMapStore starting;
    starting.setDirectory(recoveryDir);
    CHECK(starting.load() == host::MidiMapLoadOutcome::Ok); // selected corrupt: use another
    CHECK_EQ(starting.id(), alternate);
    CHECK(!starting.removeUnavailable(alternate)); // last good profile is protected
    CHECK(!starting.removeUnavailable("../../unrelated"));
    CHECK(starting.removeUnavailable(initial));
    CHECK_EQ(starting.profiles().size(), 1u);
    CHECK_EQ(starting.id(), alternate);
    CHECK(!starting.removeCurrent());
    CHECK(b.reload()); // another instance deleted our file: use a readable replacement
    CHECK_EQ(b.id(), alternate);
    CHECK(b.setSettings(settings));
    corrupt(b.livePath());
    CHECK(!b.reload()); CHECK(b.dirty()); CHECK_EQ(b.settings().channelFilter, 3);
    CHECK(!b.removeUnavailable(alternate)); // preserve the current in-memory rescue copy
    CHECK(b.create("Rescued", true));
    CHECK(b.removeUnavailable(alternate));
    CHECK_EQ(b.settings().channelFilter, 3);
    // A repaired profile must not be removed using stale UNAVAILABLE UI state.
    CHECK(b.create("Repair", false));
    const auto repairId = b.id();
    const auto repairPath = b.livePath();
    const auto repairBytes = readFile(repairPath);
    CHECK(starting.refresh());
    corrupt(repairPath); CHECK(starting.refresh());
    CHECK(real.writeFile(nullptr, repairPath.c_str(), repairBytes.data(), repairBytes.size()));
    CHECK(!starting.removeUnavailable(repairId));
    CHECK(readFile(repairPath) == repairBytes);
  }
  {
    host::MidiMapStore retry;
    retry.setDirectory(recoveryDir);
    {
      host::midi_profile_files::Lock lock(host::app_state_file_ops::joinUtf8(retry.libraryPath(), "library.lock"));
      CHECK(retry.load() == host::MidiMapLoadOutcome::Unreadable);
    }
    CHECK(retry.refresh()); CHECK(!retry.error().empty()); // keep the RETRY LOAD affordance
    CHECK(retry.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(retry.editable()); CHECK(retry.error().empty());
  }
  removeTree(recoveryDir);
  host::MidiMapStore noPath;
  CHECK(noPath.load() == host::MidiMapLoadOutcome::NoPath);
  CHECK(!noPath.save());
  return test::finish("test_midi_map_store");
}
