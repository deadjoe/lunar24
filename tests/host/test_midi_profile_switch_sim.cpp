// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Profile switch keys, simulated: thousands of random edits a player might make (Learn into a
// profile, Learn a switch key, unbind, step with the keybed triangles or a pad, pick a profile,
// make a new one, open / close the MIDI panel, play the bound keys) run against the real
// profile store and audio engine, and against a small reference model of the rules:
//   * one key, one meaning: the later bind wins (profile row <-> switch key, PREV <-> NEXT);
//   * a switch key outranks rows other profiles still hold on it (they stay, greyed);
//   * stepping walks the profiles in name order, wrapping, and never changes the startup choice;
//   * the global settings and switch keys are the same in every profile;
//   * while the MIDI panel is open a switch key is swallowed and steps nothing;
//   * a pad learned with no device (a plugin) and the same pad learned with the app's device
//     name are one key (core::midi_keys_overlap);
//   * a second instance (the app next to a plugin) changing the global settings is merged,
//     never overwritten, by this instance's next global change.
// After every operation the store, the files on disk and the engine's snapshot must agree
// with the model.

#include "mini_test.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <host/midi_map_store.h>
#include <host/standalone_audio_engine.h>

using namespace lunar24;

namespace {

constexpr const char* kDevice = "MPK mini IV";

// The keys a player has: pads (notes, channel 10) and knobs / buttons (CCs, channel 1).
// Key 7 is pad 0 as a plugin learns it: no device, any channel.
constexpr int kPlayableKeys = 7;
core::MidiBindingKey makeKey(int i) {
  core::MidiBindingKey k;
  std::snprintf(k.device, core::kMidiBindingDeviceCapacity, "%s", kDevice);
  if (i == kPlayableKeys) {
    k.device[0] = '\0';
    k.kind = core::MidiBindingKind::note;
    k.channel = 0;
    k.number = 36;
  } else if (i < 4) {
    k.kind = core::MidiBindingKind::note;
    k.channel = 10;
    k.number = static_cast<std::uint8_t>(36 + i);
  } else {
    k.kind = core::MidiBindingKind::cc;
    k.channel = 1;
    k.number = static_cast<std::uint8_t>(20 + i);
  }
  return k;
}
constexpr int kKeys = 8;
bool overlap(int a, int b) { return a == b || (a == 0 && b == kPlayableKeys) || (a == kPlayableKeys && b == 0); }

// Panel targets: a few knobs and a drone key.
core::MidiBinding makeBinding(int key, int target) {
  core::MidiBinding b;
  b.key = makeKey(key);
  static const core::ParameterId params[] = {core::ParameterId::vcf_l_freq, core::ParameterId::effector_blend,
                                             core::ParameterId::vco_a_tune};
  if (target < 3) {
    b.parameter = params[target];
  } else {
    b.targetKind = core::MidiTargetKind::action;
    b.action = core::MidiAction::drone_key_2;
  }
  return b;
}
constexpr int kTargets = 4;

std::string folded(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::uint8_t> readBytes(const std::string& path) {
  std::vector<std::uint8_t> bytes;
  (void)host::midi_profile_files::read(path, bytes, 1u << 20);
  return bytes;
}

struct Model {
  std::map<std::string, std::map<int, int>> profiles;  // name -> key index -> target
  std::string current, selected;
  int switchKey[2] = {-1, -1};
  int octave = 0;      // the global TRANSPOSE
  bool panelOpen = false;
  bool aSynced = true;  // this instance has seen the second instance's last global change

  std::vector<std::string> order() const {  // name order ignoring case (names unique that way)
    std::vector<std::string> names;
    for (const auto& p : profiles) names.push_back(p.first);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return folded(a) < folded(b); });
    return names;
  }
  void step(int direction) {
    const auto names = order();
    if (names.size() < 2) return;
    const auto at = std::find(names.begin(), names.end(), current) - names.begin();
    const auto n = static_cast<std::ptrdiff_t>(names.size());
    current = names[static_cast<std::size_t>(((at + direction) % n + n) % n)];
  }
  void bindProfile(int key, int target) {
    profiles[current][key] = target;
    for (int& k : switchKey)
      if (k >= 0 && overlap(k, key)) k = -1;
  }
  void bindSwitch(int direction, int key) {
    switchKey[direction] = key;
    if (switchKey[1 - direction] >= 0 && overlap(switchKey[1 - direction], key)) switchKey[1 - direction] = -1;
    auto& rows = profiles[current];
    for (auto it = rows.begin(); it != rows.end();) it = overlap(it->first, key) ? rows.erase(it) : std::next(it);
  }
};

int keyIndex(const core::MidiBindingKey& k) {
  for (int i = 0; i < kKeys; ++i)
    if (makeKey(i).equals(k)) return i;
  return -1;
}

int targetIndex(const core::MidiBinding& b) {
  for (int t = 0; t < kTargets; ++t) {
    const auto m = makeBinding(0, t);
    if (m.targetKind == b.targetKind && (b.targetKind == core::MidiTargetKind::action ? m.action == b.action
                                                                                       : m.parameter == b.parameter))
      return t;
  }
  return -1;
}

// The plugin's republishMidiMap, as the app does it after every change.
void publish(host::StandaloneAudioEngine& e, const host::MidiMapStore& store, bool panelOpen) {
  const core::MidiMap switches = core::midi_switch_map(store.global());
  e.publishMidiMap(store.map(), kDevice, store.settings(), true, &switches, !panelOpen);
}

bool checkAgainstModel(const Model& m, host::MidiMapStore& store, const std::string& selectedPath,
                       std::map<std::string, std::string>& idOf, int seed, int op) {
  bool ok = true;
  ok &= store.name() == m.current;
  const auto& cur = m.profiles.at(m.current);
  ok &= store.map().count() == cur.size();
  for (std::uint32_t i = 0; i < store.map().count(); ++i) {
    const auto& b = store.map().at(i);
    const auto it = cur.find(keyIndex(b.key));
    ok &= it != cur.end() && it->second == targetIndex(b);
  }
  if (m.aSynced) {
    for (int d = 0; d < 2; ++d) {
      ok &= store.global().switchBound[d] == (m.switchKey[d] >= 0);
      if (m.switchKey[d] >= 0) ok &= store.global().switchKey[d].equals(makeKey(m.switchKey[d]));
    }
    ok &= store.settings().octaveShift == m.octave;
  }
  // The startup choice on disk is the last profile picked or made, never a stepped one.
  const auto sel = readBytes(selectedPath);
  ok &= std::string(sel.begin(), sel.end()) == idOf[m.selected];
  // A fresh instance reads the same profiles and global settings from disk.
  host::MidiMapStore fresh;
  fresh.setDirectory(std::filesystem::u8path(store.libraryPath()).parent_path().u8string());
  ok &= fresh.load() == host::MidiMapLoadOutcome::Ok;
  ok &= fresh.name() == m.selected;
  for (int d = 0; d < 2; ++d) ok &= fresh.global().switchBound[d] == (m.switchKey[d] >= 0);
  ok &= fresh.settings().octaveShift == m.octave;
  if (!ok) std::printf("  model mismatch: seed %d, operation %d (current %s)\n", seed, op, m.current.c_str());
  CHECK(ok);
  return ok;
}

}  // namespace

int main() {
  using E = host::StandaloneAudioEngine;
  for (int seed = 1; seed <= 6; ++seed) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("lunar24-switch-sim-" + std::to_string(seed));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    host::MidiMapStore store;
    store.setDirectory(dir.u8string());
    CHECK(store.load() == host::MidiMapLoadOutcome::NoFile);
    CHECK(store.rename("Alpha"));
    const std::string selectedPath = host::app_state_file_ops::joinUtf8(store.libraryPath(), "selected.txt");
    Model m;
    m.profiles["Alpha"];
    m.current = m.selected = "Alpha";
    std::map<std::string, std::string> idOf{{"Alpha", store.id()}};
    auto heap = std::make_unique<E>();
    E& e = *heap;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    publish(e, store, false);
    host::MidiMapStore other;  // a second instance on the same library (global settings only)
    other.setDirectory(dir.u8string());
    CHECK(other.load() == host::MidiMapLoadOutcome::Ok);
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint32_t>(n)); };
    static const char* kNames[] = {"bravo", "Charlie", "delta", "Echo", "foxtrot"};
    int made = 0, steps = 0, swallowed = 0, conflictsSeen = 0, merges = 0;
    auto sync = [&]() {  // what opening PROFILE does: refresh (the app then republishes)
      if (!m.aSynced) {
        CHECK(store.refresh());
        m.aSynced = true;
      }
    };
    for (int op = 0; op < 600; ++op) {
      const int what = pick(100);
      if (what < 25) {                       // Learn a panel target into the current profile
        const int key = pick(kKeys), target = pick(kTargets);
        if (!m.aSynced) ++merges;
        CHECK(store.bind(makeBinding(key, target)));
        CHECK(store.save());
        m.bindProfile(key, target);
        m.aSynced = true;  // bind() rereads global.bin
      } else if (what < 37) {                // Learn a switch key (BIND after any warning)
        const int direction = pick(2), key = pick(kKeys);
        if (!store.otherProfilesUsing(makeKey(key)).empty()) ++conflictsSeen;
        // The warning names exactly the other profiles holding the key.
        std::size_t expected = 0;
        for (const auto& p : m.profiles)
          if (p.first != m.current)
            for (const auto& row : p.second) expected += overlap(row.first, key) ? 1u : 0u;
        CHECK_EQ(store.otherProfilesUsing(makeKey(key)).size(), expected);
        if (!m.aSynced) ++merges;
        CHECK(store.bindSwitch(direction, makeKey(key)));
        CHECK(store.save());
        m.bindSwitch(direction, key);
        m.aSynced = true;
      } else if (what < 42) {                // x under a triangle
        const int direction = pick(2);
        sync();
        CHECK_EQ(store.unbindSwitch(direction), m.switchKey[direction] >= 0);
        m.switchKey[direction] = -1;
      } else if (what < 48) {                // x on a profile row
        const int key = pick(kKeys);
        const bool had = m.profiles[m.current].count(key) != 0;
        CHECK_EQ(store.unbind(makeKey(key)), had);
        CHECK(store.save());
        m.profiles[m.current].erase(key);
      } else if (what < 62) {                // keybed triangle or [ / ] (ignored while the panel is open)
        const int direction = pick(2) ? 1 : -1;
        if (!m.panelOpen) {
          const int r = store.step(direction);
          CHECK(r >= 0);
          m.step(direction);
          ++steps;
        }
      } else if (what < 80) {                // play a key on the controller
        const int key = pick(kPlayableKeys);
        const auto k = makeKey(key);
        sync();
        publish(e, store, m.panelOpen);
        const int row = e.midiBindingRow(k.channel, k.kind, k.number);
        const bool isSwitch = (m.switchKey[0] >= 0 && overlap(m.switchKey[0], key)) ||
                              (m.switchKey[1] >= 0 && overlap(m.switchKey[1], key));
        if (isSwitch) {
          CHECK(row >= E::kMidiSwitchRowBase);
          e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(row), 100, k.channel);
          if (k.kind == core::MidiBindingKind::cc)  // release, so the next press is a new press
            e.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(e.midiBindingRow(k.channel, k.kind, k.number)), 0);
          e.syncParametersFromAudioThread();
          const int keySteps = e.takeProfileSteps();
          const int direction = m.switchKey[1] >= 0 && overlap(m.switchKey[1], key) ? 1 : -1;
          if (m.panelOpen) {
            CHECK_EQ(keySteps, 0);
            ++swallowed;
          } else {
            CHECK_EQ(keySteps, direction);
            CHECK(store.step(direction) >= 0);  // the plugin's OnIdle
            m.step(direction);
            ++steps;
          }
        } else {
          const auto& cur = m.profiles[m.current];
          auto it = cur.find(key);  // the exact row outranks the any-device one (midi_map_find)
          if (it == cur.end() && key == 0) it = cur.find(kPlayableKeys);
          if (it == cur.end()) {
            CHECK_EQ(row, -1);
          } else {
            CHECK(row >= 0 && row < E::kMidiSwitchRowBase);
            if (row >= 0 && row < E::kMidiSwitchRowBase)
              CHECK_EQ(targetIndex(store.map().at(static_cast<std::uint32_t>(row))), it->second);
          }
        }
      } else if (what < 86) {                // pick a profile in the PROFILE list (startup choice too)
        const auto names = m.order();
        const auto& name = names[static_cast<std::size_t>(pick(static_cast<int>(names.size())))];
        CHECK(store.select(idOf[name]));
        m.current = m.selected = name;
      } else if (what < 90) {                // NEW / COPY
        if (made < 5) {
          const bool copy = pick(2) == 1;
          const std::string name = kNames[made++];
          CHECK(store.create(name, copy));
          idOf[name] = store.id();
          m.profiles[name] = copy ? m.profiles[m.current] : std::map<int, int>{};
          m.current = m.selected = name;
        }
      } else if (what < 93) {                // open / close the MIDI panel
        m.panelOpen = !m.panelOpen;
      } else if (what < 97) {                // the other instance changes the global settings
        if (pick(2) == 0 || m.switchKey[0] < 0) {
          core::MidiRigSettings s = other.settings();
          s.octaveShift = static_cast<std::int8_t>(pick(7) * 12 - 36);
          CHECK(other.setSettings(s));
          m.octave = s.octaveShift;
        } else {
          CHECK(other.refresh());
          CHECK(other.unbindSwitch(0));
          m.switchKey[0] = -1;
        }
        m.aSynced = false;
      } else {                               // global settings: the same in every profile
        if (!m.aSynced) ++merges;
        core::MidiRigSettings s = store.settings();
        s.octaveShift = static_cast<std::int8_t>(pick(7) * 12 - 36);
        CHECK(store.setSettings(s));
        m.octave = s.octaveShift;
        m.aSynced = true;
        const auto before = store.settings().octaveShift;
        if (!m.panelOpen && store.step(1) > 0) {
          m.step(1);
          ++steps;
        }
        CHECK_EQ(store.settings().octaveShift, before);
      }
      if (!checkAgainstModel(m, store, selectedPath, idOf, seed, op)) break;  // one report per seed
    }
    // Every seed exercises the interesting paths.
    CHECK(steps > 20);
    CHECK(swallowed > 0);
    CHECK(conflictsSeen > 0);
    CHECK(merges > 0);
    heap.reset();
    std::filesystem::remove_all(dir, ec);
  }
  return test::finish("test_midi_profile_switch_sim");
}
