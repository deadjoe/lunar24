// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/midi_map.h>
#include <lunar24/core/state_edit.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace lunar24::host::midi_ui {

// One geometry for rendering, hit testing and the offline preview. Design units
// scale with the main panel; primary text is 18-22, interactive rows are 44-50 high.
struct Box {
  float l, t, r, b;
  bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
};
inline constexpr Box kBounds{410, 1112, 1990, 1482};
inline constexpr Box kClose{1846, 1126, 1966, 1166};
inline constexpr Box kProfile{832, 1126, 1332, 1166};
// The tabs: BINDINGS (Learn and the profile's table), DEVICES (the inputs and each one's
// settings), PROFILES (the profile list, also opened by the PROFILE box).
inline constexpr int kTabCount = 3;
inline constexpr Box tab(int i) { return {1350.f + i * 156, 1128, 1500.f + i * 156, 1164}; }
inline constexpr const char* kTabLabels[kTabCount] = {"BINDINGS", "DEVICES", "PROFILES"};
// BINDINGS: the devices that are on, under INPUTS (a click opens DEVICES).
inline constexpr Box kInputsList{434, 1204, 790, 1360};
inline constexpr Box inputLine(int i) { return {434, 1208.f + i * 38, 790, 1242.f + i * 38}; }
// The bindings / profiles table (x 1020..1966) leaves room on its left for the PROFILE
// SWITCH KEYS card (x 812..984); the GLOBAL MIDI SETTINGS heading spans the settings and it.
inline constexpr float kTableL = 1020;
inline constexpr Box profileRow(int row) { return {kTableL, 1205.f + row * 50, 1966, 1253.f + row * 50}; }
inline constexpr Box profileAction(int i) { return {832.f + i * 168, 1436, 988.f + i * 168, 1472}; }
inline constexpr Box kNameEntry{1034, 1208, 1668, 1248};
inline constexpr Box kLearn{434, 1368, 788, 1404};
inline constexpr Box kRetrySave{434, 1368, 604, 1404};
inline constexpr Box kReload{614, 1368, 788, 1404};
// PROFILE SWITCH KEYS: one triangle per keybed triangle (0 = PREV, 1 = NEXT). A click while
// Learn waits for a target picks it; x clears its key.
inline constexpr Box kSwitchCard{812, 1208, 984, 1404};
inline constexpr float switchCenterX(int i) { return i == 0 ? 855.f : 941.f; }
inline constexpr Box switchTarget(int i) { return {switchCenterX(i) - 40, 1230, switchCenterX(i) + 40, 1292}; }
inline constexpr Box switchRemove(int i) { return {switchCenterX(i) - 16, 1342, switchCenterX(i) + 16, 1374}; }
// The confirmation before a key another profile uses becomes a switch key (over the table).
inline constexpr Box kConfirmCard{kTableL, 1204, 1966, 1404};
inline constexpr Box kConfirmBind{1620, 1356, 1780, 1394};
inline constexpr Box kConfirmCancel{1792, 1356, 1952, 1394};
inline constexpr Box unavailableRemove(int row) { return {1858, 1210.f + row * 50, 1958, 1250.f + row * 50}; }
inline constexpr Box kPrevious{1722, 1430, 1770, 1470};
inline constexpr Box kNext{1918, 1430, 1966, 1470};
inline constexpr Box kVersion{1540, 1436, 1712, 1472};  // plugins: two lines, version and build
inline constexpr int kVisibleRows = 4;
inline constexpr float kTableY = 1204, kRowH = 50;
inline constexpr int kSettingCount = 4;  // CHANNEL, TRANSPOSE, VELOCITY, SPLIT
inline constexpr Box mode(int row) {
  const float y = kTableY + row * kRowH;
  return {1708, y + 5, 1848, y + 45};
}
inline constexpr Box remove(int row) {
  const float y = kTableY + row * kRowH;
  return {1914, y + 5, 1958, y + 45};
}
inline constexpr Box action(int i) { return {832.f + i * 140, 1436, 962.f + i * 140, 1472}; }
// DEVICES: one row per port: ON / OFF, the name and its state, then its four settings.
inline constexpr Box deviceToggle(int row) {
  const float y = kTableY + row * kRowH;
  return {434, y + 7, 524, y + 43};
}
inline constexpr Box deviceSetting(int row, int i) {
  const float y = kTableY + row * kRowH, l = 914.f + i * 263;
  return {l, y + 5, l + 250, y + 45};
}
inline constexpr Box deviceDecrement(int row, int i) {
  const auto c = deviceSetting(row, i);
  return {c.l, c.t, c.l + 40, c.b};
}
inline constexpr Box deviceIncrement(int row, int i) {
  const auto c = deviceSetting(row, i);
  return {c.r - 40, c.t, c.r, c.b};
}
inline constexpr core::MidiAction kActions[] = {
    core::MidiAction::cartridge_prev, core::MidiAction::preset_load_a, core::MidiAction::preset_load_b,
    core::MidiAction::preset_load_c, core::MidiAction::preset_load_d};
inline constexpr const char* kActionLabels[] = {"CART PREV", "PRESET A", "PRESET B", "PRESET C", "PRESET D"};
inline constexpr std::uint32_t kPaper = 0xe9e0d2, kCard = 0xf5eee3, kInk = 0x24211e, kMuted = 0x655e54,
                               kRule = 0xc8bdad, kTeal = 0x005e7a, kRed = 0xcb2026, kWhite = 0xfffaf2;
// Disabled buttons are outlined ghosts: visible on paper, cards and alternate rows alike.
// Armed status text is a deeper kRed, readable at small sizes on paper (WCAG AA).
inline constexpr std::uint32_t kDisabledEdge = 0xb5a998, kDisabledText = 0x8a8174, kArmedText = 0xa81b20;
// Activity lights: a message just arrived / connected and quiet.
inline constexpr std::uint32_t kActive = 0x2e9e4f, kIdle = 0x7d756a;

struct ProfileRow {
  std::string name;
  std::uint32_t bindings = 0;
  bool current = false, readable = true, deleteArmed = false;
};
// One DEVICES row. The plugins have one, the DAW track (its settings: the every-device ones).
struct DeviceRow {
  std::string name;
  bool on = false, connected = false, failed = false, isVirtual = false;
  bool active = false;  // a message arrived just now (the activity light)
  core::MidiRigSettings rig;
  int sameName = 1;     // ports with this name (only the first is used)
};
enum class Tab { bindings, devices, profiles };
struct State {
  const core::MidiMap* map = nullptr;
  core::MidiGlobalSettings global;  // switch keys shown in their card; their rows are greyed
  // A key another profile uses, waiting for BIND / CANCEL: "<profile>: <target>" lines.
  bool confirmSwitch = false;
  int confirmDirection = 0;
  std::vector<std::string> confirmUses;
  std::string profileName = "Untitled", error, nameText;
  std::vector<ProfileRow> profiles;
  bool profileBrowser = false, deleteArmed = false, dirty = false, reloadArmed = false, recovery = false;
  int nameAction = -1;
  int offset = 0;
  bool armed = false, awaitTarget = false, editable = true;
  bool devicesTab = false;  // DEVICES (profileBrowser: PROFILES; neither: BINDINGS)
  std::vector<DeviceRow> devices;
  std::string deviceNote;   // DEVICES: why the last click did nothing (e.g. four already on)
  bool fromDaw = false;     // a plugin: MIDI comes from the DAW track
  // The plugins have no About box: their version and build are shown here (empty: not shown).
  std::string version, build;
};
inline Tab currentTab(const State& st) {
  return st.profileBrowser ? Tab::profiles : st.devicesTab ? Tab::devices : Tab::bindings;
}
// MIDI note name with octave, middle C (60) = C4.
inline std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}
// A DEVICES setting's value: 0 CHANNEL, 1 TRANSPOSE, 2 VELOCITY, 3 SPLIT.
inline std::string settingText(const core::MidiRigSettings& rig, int i) {
  static const char* const curves[] = {"LINEAR", "SOFT", "HARD"};
  char text[24];
  switch (i) {
    case 0:
      if (rig.channelFilter == 0) return "ANY";
      return std::to_string(int(rig.channelFilter));
    case 1: std::snprintf(text, sizeof(text), "%+d st", int(rig.octaveShift)); return text;
    case 2: return curves[std::clamp(int(rig.velocityCurve), 0, 2)];
    default: return noteName(rig.splitNote);
  }
}
// One click on a DEVICES setting's < (-1) or > (+1): wraps at either end.
inline core::MidiRigSettings stepSetting(core::MidiRigSettings rig, int i, int delta) {
  constexpr int low[] = {0, -36, 0, core::kMidiSplitNoteLow}, high[] = {16, 36, 2, core::kMidiSplitNoteHigh};
  int v = i == 0 ? rig.channelFilter : i == 1 ? rig.octaveShift : i == 2 ? int(rig.velocityCurve) : rig.splitNote;
  v += delta;
  if (v < low[i]) v = high[i];
  if (v > high[i]) v = low[i];
  if (i == 0) rig.channelFilter = static_cast<std::uint8_t>(v);
  else if (i == 1) rig.octaveShift = static_cast<std::int8_t>(v);
  else if (i == 2) rig.velocityCurve = static_cast<core::MidiVelocityCurve>(v);
  else rig.splitNote = static_cast<std::uint8_t>(v);
  return rig;
}
inline int pageOffset(int offset, int count) {
  return std::clamp(offset / kVisibleRows, 0, std::max(0, (count - 1) / kVisibleRows)) * kVisibleRows;
}
inline const char* actionName(core::MidiAction a) {
  static const char* names[] = {"DRONE 1",       "DRONE 2",       "DRONE 3",        "DRONE 4",
                                "DRONE 5",       "DRONE 6",       "CARTRIDGE NEXT", "CARTRIDGE PREV",
                                "LOAD PRESET A", "LOAD PRESET B", "LOAD PRESET C",  "LOAD PRESET D",
                                "MUTE",          "DRONE 1 PHOTO", "DRONE 2 PHOTO",  "DRONE 4 PHOTO",
                                "DRONE 5 PHOTO", "PROFILE PREV",  "PROFILE NEXT"};
  const auto i = static_cast<unsigned>(a);
  return i < core::kMidiActionCount ? names[i] : "?";
}
inline std::string keyText(const core::MidiBindingKey& k) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s %u", k.kind == core::MidiBindingKind::cc ? "CC" : "NOTE", unsigned(k.number));
  return text;
}
inline std::string channelText(const core::MidiBindingKey& k) {
  return k.channel == 0 ? std::string("ANY CH") : "CH " + std::to_string(unsigned(k.channel));
}
// A profile row on a key that is now a switch key (or overlaps one): the switch key outranks it.
inline bool shadowedBySwitch(const core::MidiGlobalSettings& g, const core::MidiBindingKey& k) {
  for (int i = 0; i < 2; ++i)
    if (g.switchBound[i] && core::midi_keys_overlap(g.switchKey[i], k)) return true;
  return false;
}
inline const char* modeText(const core::MidiBinding& b) {
  switch (b.mode) {
    case core::MidiInputMode::relativeBinOffset:
      return "REL 1";
    case core::MidiInputMode::relativeTwosComplement:
      return "REL 2";
    case core::MidiInputMode::relativeSignMagnitude:
      return "REL 3";
    default:
      return "ABS";
  }
}
inline std::string targetText(const core::MidiBinding& b) {
  if (b.targetKind == core::MidiTargetKind::action) return actionName(b.action);
  const auto* d = core::find_parameter(b.parameter);
  if (!d) return "?";
  std::string owner(d->owner);
  for (auto& ch : owner) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return owner + " / " + std::string(d->name);
}
// The embedded font subset is ASCII only: show each non-ASCII code point (one whole
// UTF-8 sequence, or a stray byte) as '?' rather than a missing-glyph box.
inline std::string asciiText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      continue;
    }
    out += '?';
    if ((c & 0xc0) == 0x80) continue;  // stray continuation byte
    while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) ++i;
  }
  return out;
}
// UI-only text fitting. Keep the chosen font size; shorten long device/target names
// on UTF-8 boundaries, so one row can never paint over its neighbour.
template <class Measure>
std::string fitText(std::string text, float width, Measure measure) {
  if (measure(text.c_str()) <= width) return text;
  do {
    if (text.empty()) return "...";
    std::size_t n = text.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
    text.resize(n);
  } while (measure((text + "...").c_str()) > width);
  return text + "...";
}

// Sink::label fits text into a rectangle, left aligned unless center=true.
// All drawing primitives below are shared by the app and panel_preview --midi.
template <class Sink>
void draw(Sink& s, const State& st, float mouseX = -1, float mouseY = -1) {
  auto rect = [&](Box b, std::uint32_t c, float radius = 6) { s.fillRect(b.l, b.t, b.r, b.b, c, radius); };
  // align: 0 left, 1 (true) centre, 2 right.
  auto label = [&](Box b, float size, std::uint32_t c, const std::string& text, bool bold = false,
                   int align = 0) { s.label(b, size, c, text.c_str(), bold, align); };
  auto button = [&](Box b, const char* text, bool enabled, bool accent = false) {
    const bool hover = enabled && b.contains(mouseX, mouseY);
    rect(b, enabled ? (accent ? (st.armed ? kRed : kTeal) : kRule) : kDisabledEdge);
    if (!enabled || !accent)
      rect({b.l + 1, b.t + 1, b.r - 1, b.b - 1}, enabled ? (hover ? 0xd9cdbc : kWhite) : kPaper, 5);
    label({b.l + 7, b.t, b.r - 7, b.b}, 16, enabled ? (accent ? kWhite : kInk) : kDisabledText, text, true, true);
  };
  rect(kBounds, kInk, 12);
  rect({412, 1114, 1988, 1480}, kPaper, 10);
  rect({414, 1116, 1986, 1176}, kInk, 8);
  label({434, 1124, 798, 1168}, 24, kWhite, "MIDI CONTROL", true);
  rect(kProfile, kProfile.contains(mouseX, mouseY) ? 0x504940 : 0x39342e, 5);
  label({846, 1128, 922, 1164}, 12, 0xc8bdad, "PROFILE", true);
  label({930, 1128, 1288, 1164}, 19, kWhite, st.profileName + (st.dirty ? " *" : ""), true);
  s.moveTo(1298, st.profileBrowser ? 1150.f : 1142.f);
  s.lineTo(1304, st.profileBrowser ? 1142.f : 1150.f);
  s.lineTo(1310, st.profileBrowser ? 1150.f : 1142.f);
  s.closePath(); s.fillPath(kWhite, false);
  const Tab current = currentTab(st);
  for (int i = 0; i < kTabCount; ++i) {
    const Box b = tab(i);
    const bool on = static_cast<int>(current) == i;
    rect(b, on ? kPaper : b.contains(mouseX, mouseY) ? 0x504940 : 0x39342e, 5);
    label(b, 15, on ? kInk : 0xc8bdad, kTabLabels[i], true, true);
  }
  button(kClose, "CLOSE", true);
  // Paging and the count, bottom right (every tab).
  auto pager = [&](int offset, int count, const char* one, const char* many) {
    button(kPrevious, "<", offset > 0);
    button(kNext, ">", offset + kVisibleRows < count);
    char page[32];
    std::snprintf(page, sizeof(page), "%d / %d", offset / kVisibleRows + 1,
                  std::max(1, (count + kVisibleRows - 1) / kVisibleRows));
    label({1776, 1428, 1912, 1453}, 18, kInk, page, true, true);
    char total[32];
    std::snprintf(total, sizeof(total), "%d %s", count, count == 1 ? one : many);
    label({1776, 1452, 1912, 1475}, 13, kMuted, total, false, true);
  };
  auto version = [&] {
    if (st.version.empty()) return;
    label({kVersion.l, kVersion.t, kVersion.r, kVersion.t + 18}, 13, kMuted, st.version, true);
    label({kVersion.l, kVersion.t + 18, kVersion.r, kVersion.b}, 12, kMuted, st.build);
  };
  if (current == Tab::devices) {
    const char* names[] = {"CHANNEL", "TRANSPOSE", "VELOCITY", "SPLIT"};
    label({434, 1180, 524, 1202}, 14, kMuted, st.fromDaw ? "" : "ON", true, true);
    label({544, 1180, 900, 1202}, 14, kMuted, st.fromDaw ? "MIDI INPUT" : "DEVICE", true);
    for (int i = 0; i < kSettingCount; ++i) {
      const Box c = deviceSetting(0, i);
      label({c.l, 1180, c.r, 1202}, 14, kMuted, names[i], true, true);
    }
    const int count = static_cast<int>(st.devices.size());
    const int offset = pageOffset(st.offset, count);
    for (int i = 0; i < kVisibleRows; ++i) {
      const float y = kTableY + i * kRowH;
      rect({434, y + 1, 1966, y + 49}, i % 2 == 0 ? kCard : 0xe2d8c9, 5);
      if (offset + i >= count) continue;
      const DeviceRow& d = st.devices[static_cast<std::size_t>(offset + i)];
      if (!st.fromDaw) {  // ON / OFF: a teal pill when on
        const Box t = deviceToggle(i);
        const bool hover = t.contains(mouseX, mouseY);
        rect(t, d.on ? kTeal : kRule, 18);
        if (!d.on) rect({t.l + 1, t.t + 1, t.r - 1, t.b - 1}, hover ? 0xd9cdbc : kWhite, 17);
        label(t, 15, d.on ? kWhite : kInk, d.on ? "ON" : "OFF", true, true);
      }
      s.circle(554, y + 25, 6);
      s.fillPath(d.active ? kActive : d.on && d.connected && !d.failed ? kIdle : kDisabledEdge, false);
      const bool live = d.connected && !d.failed;
      label({570, y + 2, 900, y + 27}, 18, live || st.fromDaw ? kInk : kMuted,
            st.fromDaw ? "DAW track" : asciiText(d.name), true);
      const char* status = st.fromDaw ? "Every MIDI message the track sends"
                           : d.failed ? "CANNOT OPEN (in use by another app?)"
                           : !d.connected ? "NOT CONNECTED"
                           : d.isVirtual ? "Virtual input for other apps"
                           : d.sameName > 1 ? "CONNECTED - NAME SHARED, FIRST PORT USED"
                           : d.on ? "CONNECTED" : "Connected, off";
      label({570, y + 26, 900, y + 47}, 13, d.failed ? kArmedText : kMuted, status, d.failed || !d.connected);
      for (int k = 0; k < kSettingCount; ++k) {
        const Box c = deviceSetting(i, k);
        // The DAW chooses the plugins' channel; an OFF device's settings wait until it is on.
        const bool editable = st.editable && (st.fromDaw ? k != 0 : d.on);
        if (editable) {
          rect(c, kWhite, 5);
          button(deviceDecrement(i, k), "<", true);
          button(deviceIncrement(i, k), ">", true);
        }
        const std::string value = st.fromDaw && k == 0 ? std::string("DAW") : settingText(d.rig, k);
        label({c.l + 42, c.t, c.r - 42, c.b}, 19, editable ? kInk : kDisabledText, value, true, true);
      }
    }
    if (count == 0) {
      rect({434, 1204, 1966, 1404}, kCard);
      label({440, 1250, 1960, 1290}, 22, kInk, "No MIDI device found", true, true);
      label({440, 1292, 1960, 1328}, 18, kMuted, "Plug in a controller; it appears here within a second.", false, true);
    }
    rect({434, 1412, 1966, 1414}, kRule, 0);
    std::string line1 = st.fromDaw ? "MIDI comes from the DAW track." : "Turn on the devices you play,";
    std::string line2 = st.fromDaw ? "Route a controller to the track." : "up to four at once.";
    if (!st.deviceNote.empty()) { line1 = st.deviceNote; line2.clear(); }
    label({434, 1424, 802, 1448}, 15, st.deviceNote.empty() ? kMuted : kArmedText, line1, true);
    label({434, 1449, 802, 1474}, 14, kMuted, line2);
    label({832, 1416, 1532, 1435}, 12, kMuted, st.fromDaw ? "" : "EACH DEVICE KEEPS ITS OWN SETTINGS", true);
    if (!st.fromDaw)
      label({832, 1436, 1532, 1474}, 14, kMuted, "Any device can play the plates and use the bindings.");
    version();
    pager(offset, count, "device", "devices");
    return;
  }
  rect({1000, 1186, 1002, 1404}, kRule, 0);
  label({434, 1182, 790, 1202}, 14, kTeal, "INPUTS", true);
  {  // PROFILE SWITCH KEYS: raised triangles standing for the keybed's two
    rect(kSwitchCard, kCard);
    label({kSwitchCard.l, 1210, kSwitchCard.r, 1228}, 12, kMuted, "PROFILE SWITCH KEYS", true, true);
    const bool pick = st.editable && st.armed && st.awaitTarget && !st.profileBrowser && !st.confirmSwitch;
    const char* names[2] = {"PREV", "NEXT"};
    for (int i = 0; i < 2; ++i) {
      const float cx = switchCenterX(i), tip = 1238, base = 1266, half = 16;
      const bool hover = pick && switchTarget(i).contains(mouseX, mouseY);
      auto tri = [&](float dx, float dy, float inset, std::uint32_t c) {
        s.moveTo(cx + dx, tip + dy + inset * 1.6f);
        s.lineTo(cx + dx + half - inset, base + dy - inset * 0.6f);
        s.lineTo(cx + dx - half + inset, base + dy - inset * 0.6f);
        s.closePath();
        s.fillPath(c, false);
      };
      const std::uint32_t face = pick ? (hover ? 0x1f8aa8 : kTeal) : 0x3a3633;
      tri(1.5f, 3.f, 0, 0xb5a998);                    // shadow on the card
      tri(0, 0, 0, pick ? 0x003e52 : 0x1c1a18);       // the key's dark lower edge
      tri(0, -1.5f, 1.2f, pick ? 0x4fb0c8 : 0x6b6560);  // light catching the upper edges
      tri(0, 0, 2.6f, face);                          // the face
      label({cx - 40, 1272, cx + 40, 1292}, 13, pick ? kTeal : kInk, names[i], true, true);
      if (st.global.switchBound[i]) {
        const auto& k = st.global.switchKey[i];
        label({cx - 43, 1296, cx + 43, 1316}, 14, kInk, keyText(k), true, true);
        label({cx - 43, 1316, cx + 43, 1334}, 12, kMuted, channelText(k), false, true);
        button(switchRemove(i), "x", st.editable && !st.confirmSwitch);
      } else {
        label({cx - 43, 1296, cx + 43, 1316}, 14, kMuted, "NOT SET", true, true);
      }
    }
  }
  {  // INPUTS: the devices that are on, each with its activity light
    int shown = 0;
    for (const auto& d : st.devices) {
      if (!d.on || shown >= 4) continue;
      const Box b = inputLine(shown++);
      rect(b, kInputsList.contains(mouseX, mouseY) ? 0xece3d5 : kCard);
      s.circle(b.l + 18, (b.t + b.b) / 2, 6);
      s.fillPath(d.active ? kActive : d.connected ? kIdle : kDisabledEdge, false);
      label({b.l + 34, b.t, d.connected ? b.r - 10 : b.r - 130, b.b}, 16, d.connected ? kInk : kMuted,
            asciiText(d.name), true);
      if (!d.connected) label({b.r - 126, b.t, b.r - 10, b.b}, 12, kMuted, "NOT CONNECTED", true, 2);
    }
    if (shown == 0) {
      rect({kInputsList.l, kInputsList.t + 4, kInputsList.r, kInputsList.t + 80}, kCard);
      label({448, 1212, 780, 1240}, 16, kInk, "No device is on.", true);
      label({448, 1240, 780, 1266}, 14, kMuted, "Open DEVICES to turn one on.");
    }
  }
  if (st.recovery && st.editable) {
    button(kRetrySave, st.dirty ? "RETRY SAVE" : "RETRY", true);
    button(kReload, st.reloadArmed ? "RELOAD?" : "RELOAD", true);
  } else {
    button(kLearn, st.recovery ? (st.editable ? "RETRY" : "RETRY LOAD") :
                   st.profileBrowser ? "BACK TO BINDINGS" : st.armed ? "CANCEL LEARN" : "+ LEARN A CONTROL",
           st.editable || st.profileBrowser || st.recovery, !st.profileBrowser);
  }
  const int count = st.profileBrowser ? static_cast<int>(st.profiles.size()) :
                    st.map ? static_cast<int>(st.map->count()) : 0;
  const int offset = pageOffset(st.offset, count);
  if (st.nameAction >= 0) {
    const char* titles[] = {"NEW PROFILE NAME", "COPY PROFILE AS", "RENAME CURRENT PROFILE"};
    label({1032, 1180, 1640, 1202}, 14, kMuted, titles[std::clamp(st.nameAction, 0, 2)], true);
    rect({kTableL, 1205, 1966, 1403}, kCard);
    rect(kNameEntry, kWhite, 4);
    label(kNameEntry, 19, kInk, st.nameText, true);
    label({1034, 1270, 1948, 1310}, 18, kMuted, "English names, up to 39 characters.");
    label({1034, 1314, 1948, 1354}, 16, kMuted, "Enter or click outside to confirm. Esc to cancel.");
  } else if (st.confirmSwitch) {
    label({1032, 1180, 1950, 1202}, 14, kMuted, st.confirmDirection == 0 ? "PREV SWITCH KEY" : "NEXT SWITCH KEY", true);
    rect(kConfirmCard, kCard);
    label({1040, 1212, 1950, 1240}, 19, kInk, "This key is also bound in other profiles:", true);
    const int shown = std::min<int>(3, static_cast<int>(st.confirmUses.size()));
    for (int i = 0; i < shown; ++i)
      label({1056, 1244.f + i * 26, 1950, 1268.f + i * 26}, 16, kInk, st.confirmUses[static_cast<std::size_t>(i)]);
    if (static_cast<int>(st.confirmUses.size()) > shown)
      label({1056, 1322, 1950, 1344}, 15, kMuted,
            "and " + std::to_string(st.confirmUses.size() - static_cast<std::size_t>(shown)) + " more");
    label({1040, 1356, 1600, 1376}, 14, kMuted, "Bound, it switches profiles there too;");
    label({1040, 1376, 1600, 1396}, 14, kMuted, "those rows stay, greyed. Cancel to check first.");
    button(kConfirmBind, "BIND", true, true);
    button(kConfirmCancel, "CANCEL", true);
  } else if (st.profileBrowser) {
    int readable = 0;
    for (const auto& p : st.profiles) if (p.readable) ++readable;
    label({1032, 1180, 1640, 1202}, 14, kMuted,
          "SAVED PROFILES / " + std::to_string(readable) + " AVAILABLE", true);
    label({1720, 1180, 1950, 1202}, 14, kMuted, "BINDINGS", true, true);
    for (int i = 0; i < kVisibleRows; ++i) {
      const auto box = profileRow(i);
      const int row = offset + i;
      rect(box, i % 2 == 0 ? kCard : 0xe2d8c9, 5);
      if (row >= count) continue;
      const auto& profile = st.profiles[static_cast<std::size_t>(row)];
      if (profile.readable && box.contains(mouseX, mouseY)) rect(box, 0xd9cdbc, 5);
      if (profile.current) rect({box.l, box.t + 5, box.l + 4, box.b - 5}, kTeal, 1);
      label({1038, box.t, 1620, box.b}, 19, profile.readable ? kInk : kMuted, profile.name, true);
      if (profile.current) label({1628, box.t, 1732, box.b}, 12, kTeal, "CURRENT", true, true);
      if (profile.readable) label({1750, box.t, 1950, box.b}, 17, kMuted, std::to_string(profile.bindings), false, true);
      else {
        label({1690, box.t, 1850, box.b}, 14, kMuted, "UNAVAILABLE", false, true);
        button(unavailableRemove(i), profile.deleteArmed ? "DELETE?" : "DELETE", true);
      }
    }
  } else {
    label({1032, 1180, 1226, 1202}, 14, kMuted, "CONTROLLER", true);
    label({1236, 1180, 1700, 1202}, 14, kMuted, "PANEL TARGET", true);
    label({1708, 1180, 1848, 1202}, 14, kMuted, "MODE", true, true);
    for (int i = 0; i < kVisibleRows; ++i) {
      const float y = kTableY + i * kRowH;
      rect({kTableL, y + 1, 1966, y + 49}, i % 2 == 0 ? kCard : 0xe2d8c9, 5);
      if (offset + i >= count) continue;
      const auto& b = st.map->at(static_cast<std::uint32_t>(offset + i));
      char source[48];
      if (b.key.channel == 0)
        std::snprintf(source, sizeof(source), "%s %u / ANY CH",
                      b.key.kind == core::MidiBindingKind::cc ? "CC" : "NOTE", unsigned(b.key.number));
      else
        std::snprintf(source, sizeof(source), "%s %u / CH %u",
                      b.key.kind == core::MidiBindingKind::cc ? "CC" : "NOTE", unsigned(b.key.number),
                      unsigned(b.key.channel));
      const bool shadowed = shadowedBySwitch(st.global, b.key);  // the switch key outranks it
      label({1034, y + 2, 1226, y + 27}, 18, shadowed ? kMuted : kInk, source, true);
      label({1034, y + 26, 1226, y + 47}, 14, kMuted, b.key.device[0] ? asciiText(b.key.device) : "Any device");
      label({1236, y + 4, 1700, y + 46}, 18, shadowed ? kDisabledText : kInk, targetText(b), true);
      const auto drive = core::midi_parameter_drive(b);
      if (shadowed)
        label(mode(i), 13, kMuted, "SWITCH KEY", true, true);
      else if (b.targetKind != core::MidiTargetKind::parameter && core::midi_action_is_continuous(b.action)) {
        if (b.key.kind == core::MidiBindingKind::note)
          label(mode(i), 15, kMuted, "PRESS", true, true);  // a pad: hit, pressure, release
        else
          button(mode(i), modeText(b), st.editable);
      } else if (b.targetKind != core::MidiTargetKind::parameter)
        label(mode(i), 15, kMuted, "TRIGGER", true, true);
      else if (drive == core::MidiParameterDrive::toggleOnPress)
        label(mode(i), 15, kMuted, "TOGGLE", true, true);  // a press flips the switch
      else if (drive == core::MidiParameterDrive::stepOnPress)
        label(mode(i), 15, kMuted, "STEP", true, true);    // a hit moves to the next position
      else
        button(mode(i), modeText(b), st.editable);
      button(remove(i), "x", st.editable);
    }
    if (count == 0) {
      rect({kTableL, 1204, 1966, 1404}, kCard);
      label({1040, 1240, 1932, 1282}, 22, kInk, "No controller bindings yet", true, true);
      label({1040, 1284, 1932, 1320}, 18, kMuted,
            "Click LEARN, choose a panel control, then move a knob or press a pad.", false, true);
      label({1040, 1326, 1932, 1360}, 16, kMuted, "Bindings are saved separately from your sound settings.",
            false, true);
    }
  }
  rect({434, 1412, 1966, 1414}, kRule, 0);
  const char *line1 = "Link a panel control to a knob,", *line2 = "button or pad on your controller.";
  if (st.profileBrowser) {
    line1 = "Choose a profile to switch.";
    line2 = "Edits are saved automatically.";
  } else if (!st.editable) {
    line1 = "No editable profile.";
    line2 = "Open PROFILE to create one.";
  } else if (st.armed && st.awaitTarget) {
    line1 = "1. Choose a panel control above,";
    line2 = "a switch key or an EXTRA TARGET.";
  } else if (st.armed) {
    line1 = "2. Move a knob or press a pad.";
    line2 = "Esc or CANCEL LEARN to cancel.";
  }
  std::string error1, error2;
  if (!st.error.empty()) {
    auto cut = st.error.find(". ");
    if (cut == std::string::npos) cut = st.error.find("; ");
    if (cut != std::string::npos && cut < 38) {
      error1 = st.error.substr(0, cut + 1);
      error2 = st.error.substr(cut + 2);
    } else {
      cut = st.error.size() > 38 ? st.error.rfind(' ', 38) : std::string::npos;
      error1 = st.error.substr(0, cut);
      if (cut != std::string::npos) error2 = st.error.substr(cut + 1);
    }
    line1 = error1.c_str(); line2 = error2.c_str();
  }
  if (st.reloadArmed) { line1 = st.dirty ? "Discard local edits?" : "Reload current profile?"; line2 = "Click RELOAD? to read this profile."; }
  label({434, 1424, 802, 1448}, 15, !st.error.empty() ? kRed : st.armed ? kArmedText : kMuted, line1, true);
  label({434, 1449, 802, 1474}, 14, kMuted, line2);
  if (st.profileBrowser) {
    label({832, 1416, 1532, 1435}, 12, kMuted, "MANAGE CURRENT PROFILE", true);
    button(profileAction(0), "NEW", st.nameAction < 0);
    button(profileAction(1), "COPY", st.editable && st.nameAction < 0);
    button(profileAction(2), "RENAME", st.editable && st.nameAction < 0);
    int readable = 0;
    for (const auto& p : st.profiles) if (p.readable) ++readable;
    button(profileAction(3), st.deleteArmed ? "DELETE?" : "DELETE", st.editable && readable > 1 && st.nameAction < 0);
  } else {
    label({832, 1416, 1532, 1435}, 12, kMuted, "EXTRA LEARN TARGETS", true);
    for (int i = 0; i < 5; ++i) button(action(i), kActionLabels[i], st.editable && st.armed && st.awaitTarget);
  }
  version();
  if (st.profileBrowser) pager(offset, count, "profile", "profiles");
  else pager(offset, count, "binding", "bindings");
}
}  // namespace lunar24::host::midi_ui
