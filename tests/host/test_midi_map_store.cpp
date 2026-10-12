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
  CHECK(!store.needsRecovery());
  CHECK(!store.rename(""));
  CHECK(!store.needsRecovery());
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
  CHECK(store.needsRecovery());
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
    const auto local = ccBinding("Controller B", 5, 30, core::ParameterId::effector_blend);
    CHECK(b.bind(local)); CHECK(!b.save());
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
    // Reload needs no writes, even when selected.txt cannot be replaced.
    b.setFileOps(preferenceFailure, nullptr);
    CHECK(b.reload()); CHECK(!b.dirty()); CHECK(b.error().empty());
    b.setFileOps(real, nullptr);
    CHECK_EQ(b.map().count(), 1u); CHECK_EQ(b.settings().channelFilter, 0);
    CHECK(b.generation() > generation);
    CHECK(b.setSettings(settings)); CHECK(b.save()); // editing works again
    CHECK_EQ(a.settings().channelFilter, 0);
    CHECK(a.refresh()); CHECK_EQ(a.settings().channelFilter, 3);  // global: seen on refresh
    CHECK(a.create("Second", false));
    const auto alternate = a.id();
    const auto selectedPath = host::app_state_file_ops::joinUtf8(a.libraryPath(), "selected.txt");
    const auto selectedBytes = readFile(selectedPath);
    CHECK(b.reload()); CHECK_EQ(b.id(), initial);
    CHECK(readFile(selectedPath) == selectedBytes); // another instance's startup choice is unchanged
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
    CHECK(!starting.removeCurrent()); CHECK(!starting.needsRecovery());
    const auto beforeReload = b.generation();
    const auto beforeSelection = readFile(selectedPath);
    CHECK(!b.reload()); // a missing current file must not switch to another profile
    CHECK_EQ(b.id(), initial); CHECK_EQ(b.generation(), beforeReload);
    CHECK_EQ(b.map().count(), 1u); CHECK(b.needsRecovery());
    CHECK(readFile(selectedPath) == beforeSelection);
    CHECK(b.select(alternate)); // only an explicit selection switches profiles
    CHECK(b.bind(local));
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
  // Global settings, profile switch keys and performance stepping.
  {
    const auto gdir = makeTempDir("global");
    // An older library (no global.bin): the selected profile's own settings seed it.
    {
      host::MidiMapStore seed;
      seed.setDirectory(gdir);
      CHECK(seed.load() == host::MidiMapLoadOutcome::NoFile);
      CHECK(seed.rename("Bravo"));
      core::MidiMap m;
      const core::MidiRigSettings legacy{7, -12, core::MidiVelocityCurve::soft, 50};
      CHECK(real.writeFile(nullptr, seed.livePath().c_str(),
                           host::midi_profile_files::encode("Bravo", m, legacy).data(),
                           host::midi_profile_files::encode("Bravo", m, legacy).size()));
      std::error_code ec;
      std::filesystem::remove(std::filesystem::u8path(seed.globalPath()), ec);
    }
    host::MidiMapStore s1;
    s1.setDirectory(gdir);
    CHECK(s1.load() == host::MidiMapLoadOutcome::Ok);
    CHECK_EQ(s1.settings().channelFilter, 7);
    CHECK_EQ(s1.settings().splitNote, 50);
    CHECK(!readFile(s1.globalPath()).empty());  // the seed is written at once
    const auto bravo = s1.id();
    CHECK(s1.create("alpha", false));
    const auto alpha = s1.id();
    CHECK(s1.create("Charlie", false));
    const auto charlie = s1.id();
    CHECK_EQ(s1.settings().channelFilter, 7);  // global: same in every profile
    CHECK(s1.select(bravo));
    const auto selectedPath = host::app_state_file_ops::joinUtf8(s1.libraryPath(), "selected.txt");
    const auto selected = readFile(selectedPath);
    // Name order ignoring case: alpha, Bravo, Charlie; both ways wrap; startup choice kept.
    CHECK_EQ(s1.step(1), 1); CHECK_EQ(s1.id(), charlie);
    CHECK_EQ(s1.step(1), 1); CHECK_EQ(s1.id(), alpha);
    CHECK_EQ(s1.step(-1), 1); CHECK_EQ(s1.id(), charlie);
    CHECK_EQ(s1.step(-1), 1); CHECK_EQ(s1.id(), bravo);
    CHECK(readFile(selectedPath) == selected);
    // Pending edits are saved before stepping away.
    const auto k1 = ccBinding("MPK", 1, 70, core::ParameterId::effector_blend);
    CHECK(s1.bind(k1));
    CHECK_EQ(s1.step(1), 1);
    CHECK(s1.select(bravo));
    CHECK_EQ(s1.map().count(), 1u);
    // An unreadable profile is skipped.
    corrupt(host::app_state_file_ops::joinUtf8(s1.libraryPath(), charlie + ".bin"));
    CHECK_EQ(s1.step(1), 1); CHECK_EQ(s1.id(), alpha);
    CHECK_EQ(s1.step(-1), 1); CHECK_EQ(s1.id(), bravo);
    // Switch keys: one key per triangle, and the later bind wins everywhere it is visible.
    CHECK(s1.bindSwitch(1, k1.key));                 // k1 was BLEND in Bravo: now NEXT
    CHECK(s1.global().switchBound[1]);
    CHECK_EQ(s1.map().count(), 0u);
    CHECK(s1.dirty());
    CHECK(s1.save());
    CHECK(s1.bindSwitch(0, k1.key));                 // the same key on PREV leaves NEXT
    CHECK(s1.global().switchBound[0] && !s1.global().switchBound[1]);
    auto k2 = k1.key;
    k2.number = 71;
    CHECK(s1.bindSwitch(0, k2));                     // PREV takes a new key: k1 is free
    CHECK(s1.global().switchKey[0].equals(k2));
    CHECK(s1.bindSwitch(1, k1.key));
    auto blendOnK2 = k1;
    blendOnK2.key = k2;
    CHECK(s1.bind(blendOnK2));                       // k2 back to a profile row: PREV unbound
    CHECK(!s1.global().switchBound[0] && s1.global().switchBound[1]);
    core::MidiBinding sw{};
    sw.targetKind = core::MidiTargetKind::action;
    sw.action = core::MidiAction::profile_next;
    CHECK(!s1.bind(sw));                             // never into a profile
    CHECK(s1.unbindSwitch(1));
    CHECK(!s1.unbindSwitch(1));
    CHECK(s1.bindSwitch(1, k1.key));
    // Another profile's row on the switch key is reported by name, and left in place.
    CHECK(s1.save());
    CHECK(s1.select(alpha));
    auto freq = ccBinding("MPK", 1, 72, core::ParameterId::vcf_l_freq);
    CHECK(s1.bind(freq));
    CHECK(s1.save());
    CHECK(s1.select(bravo));
    const auto uses = s1.otherProfilesUsing(freq.key);
    CHECK_EQ(uses.size(), 1u);
    if (!uses.empty()) {
      CHECK_EQ(uses[0].profile, "alpha");
      CHECK(uses[0].binding.parameter == core::ParameterId::vcf_l_freq);
    }
    CHECK(s1.otherProfilesUsing(k2).empty());        // Bravo is current, not "other"
    CHECK(s1.bindSwitch(0, freq.key));
    CHECK(s1.select(alpha));
    CHECK_EQ(s1.map().count(), 1u);                  // alpha's row stays (greyed by the UI)
    // A second instance sees the global file on load and on refresh.
    host::MidiMapStore s2;
    s2.setDirectory(gdir);
    CHECK(s2.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(s2.global().switchBound[0] && s2.global().switchKey[0].equals(freq.key));
    CHECK(s1.setSettings(core::MidiRigSettings{2, 0, core::MidiVelocityCurve::linear, 60}));
    CHECK_EQ(s2.settings().channelFilter, 7);
    CHECK(s2.refresh());
    CHECK_EQ(s2.settings().channelFilter, 2);
    // A profile file holding a switch action is not a profile this version wrote: unreadable.
    {
      core::MidiMap odd;
      CHECK(odd.bind(sw));
      const auto bytes = host::midi_profile_files::encode("Odd", odd, core::MidiRigSettings{});
      std::string name; core::MidiMap map; core::MidiRigSettings rs;
      CHECK(!host::midi_profile_files::decode(bytes, name, map, rs));
    }
    // Only one readable profile: nothing to step to.
    const auto odir = makeTempDir("one");
    host::MidiMapStore one;
    one.setDirectory(odir);
    CHECK(one.load() == host::MidiMapLoadOutcome::NoFile);
    const auto only = one.id();
    CHECK_EQ(one.step(1), 0); CHECK_EQ(one.step(-1), 0);
    CHECK_EQ(one.id(), only);
    {
      host::midi_profile_files::Lock lock(host::app_state_file_ops::joinUtf8(s1.libraryPath(), "library.lock"));
      CHECK_EQ(s1.step(1), -1);  // busy: the caller retries
    }
    removeTree(odir);
    removeTree(gdir);
  }
  // Review fixes (#148): several instances share global.bin; damaged or missing files.
  {
    auto key = [](const char* dev, std::uint8_t ch, std::uint8_t n) {
      core::MidiBindingKey k;
      std::snprintf(k.device, core::kMidiBindingDeviceCapacity, "%s", dev);
      k.channel = ch;
      k.kind = core::MidiBindingKind::note;
      k.number = n;
      return k;
    };
    // Another instance's switch key survives this instance changing CHANNEL (merge, not overwrite).
    const auto rdir = makeTempDir("review-merge");
    host::MidiMapStore a, b;
    a.setDirectory(rdir); b.setDirectory(rdir);
    CHECK(a.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK(b.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(b.bindSwitch(1, key("MPK", 10, 40)));
    core::MidiRigSettings rs = a.settings();
    rs.channelFilter = 3;
    CHECK(a.setSettings(rs));
    CHECK(a.global().switchBound[1]);               // merged into this instance too
    host::MidiMapStore c;
    c.setDirectory(rdir);
    CHECK(c.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(c.global().switchBound[1]);
    CHECK_EQ(c.settings().channelFilter, 3);
    CHECK(a.unbindSwitch(1));                       // and the reverse: A's later unbind wins
    CHECK(b.bindSwitch(0, key("MPK", 10, 41)));
    CHECK(c.refresh());
    CHECK(!c.global().switchBound[1] && c.global().switchBound[0]);
    // Live: the other instance's change shows up here without reopening anything.
    CHECK(!c.reloadGlobalIfChanged());              // nothing new since c refreshed
    CHECK(b.bindSwitch(1, key("MPK", 10, 42)));
    CHECK(c.reloadGlobalIfChanged());
    CHECK(c.global().switchBound[1] && c.global().switchKey[1].number == 42);
    CHECK(!c.reloadGlobalIfChanged());              // once only
    // A global.bin this build cannot read is neither replaced by a seed nor emptied in memory.
    const auto globalPath = c.globalPath();
    {
      auto* f = host::app_state_file_ops::openNative(globalPath, "wb");
      CHECK(f != nullptr);
      if (f) { std::fputs("L24G-future", f); std::fclose(f); }
    }
    const auto future = readFile(globalPath);
    CHECK(c.refresh());
    CHECK(c.global().switchBound[0]);
    CHECK(readFile(globalPath) == future);
    host::MidiMapStore d;
    d.setDirectory(rdir);
    CHECK(d.load() == host::MidiMapLoadOutcome::Ok);
    CHECK(readFile(globalPath) == future);          // a starting instance does not overwrite it either
    removeTree(rdir);

    // Every profile unreadable at startup: global.bin is still read, so a later edit keeps it.
    const auto mdir = makeTempDir("review-malformed");
    host::MidiMapStore m1;
    m1.setDirectory(mdir);
    CHECK(m1.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK(m1.bindSwitch(1, key("MPK", 10, 40)));
    corrupt(m1.livePath());
    host::MidiMapStore m2;
    m2.setDirectory(mdir);
    CHECK(m2.load() == host::MidiMapLoadOutcome::Malformed);
    CHECK(m2.global().switchBound[1]);
    CHECK(!m2.unbindSwitch(1));                     // not editable: nothing changes
    CHECK(m2.create("New", false));
    rs = m2.settings();
    rs.channelFilter = 2;
    CHECK(m2.setSettings(rs));
    host::MidiMapStore m3;
    m3.setDirectory(mdir);
    (void)m3.load();
    CHECK(m3.global().switchBound[1]);
    CHECK_EQ(m3.settings().channelFilter, 2);
    removeTree(mdir);

    // A key learned in a plugin (any device) and the same pad learned in the app (named device)
    // are one key: the warning names the other profile, the row is cleared, and vice versa.
    const auto wdir = makeTempDir("review-wildcard");
    host::MidiMapStore w;
    w.setDirectory(wdir);
    CHECK(w.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK(w.rename("A"));
    core::MidiBinding anyDevice;
    anyDevice.key = key("", 10, 40);
    anyDevice.parameter = core::ParameterId::effector_blend;
    CHECK(w.bind(anyDevice));
    core::MidiBinding anyChannel = anyDevice;
    anyChannel.key = key("MPK", 0, 41);
    CHECK(w.bind(anyChannel));
    CHECK(w.save());
    CHECK(w.create("B", false));
    CHECK_EQ(w.otherProfilesUsing(key("MPK", 10, 40)).size(), 1u);
    CHECK_EQ(w.otherProfilesUsing(key("MPK", 7, 41)).size(), 1u);
    CHECK(w.otherProfilesUsing(key("MPK", 10, 42)).empty());
    CHECK(w.select(w.profiles()[0].id));            // back to A
    CHECK_EQ(w.name(), "A");
    CHECK(w.bindSwitch(1, key("MPK", 10, 40)));     // the any-device row on the same pad goes
    CHECK_EQ(w.map().count(), 1u);
    core::MidiBinding specific = anyDevice;
    specific.key = key("MPK", 10, 40);
    CHECK(w.bindSwitch(0, key("", 0, 40)));         // an any-device, any-channel switch key...
    CHECK(!w.global().switchBound[1]);              // ...takes the pad from NEXT (later wins)
    CHECK(w.bind(specific));                        // and a profile row on that pad frees it again
    CHECK(!w.global().switchBound[0]);
    removeTree(wdir);
  }
  host::MidiMapStore noPath;
  CHECK(noPath.load() == host::MidiMapLoadOutcome::NoPath);
  CHECK(!noPath.save());
  return test::finish("test_midi_map_store");
}
