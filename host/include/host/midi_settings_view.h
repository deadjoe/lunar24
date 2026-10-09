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
inline constexpr Box kInput{1350, 1128, 1816, 1164};
inline constexpr Box profileRow(int row) { return {832, 1205.f + row * 50, 1966, 1253.f + row * 50}; }
inline constexpr Box profileAction(int i) { return {832.f + i * 168, 1436, 988.f + i * 168, 1472}; }
inline constexpr Box kNameEntry{846, 1208, 1668, 1248};
inline constexpr Box kLearn{434, 1364, 788, 1404};
inline constexpr Box kPrevious{1722, 1430, 1770, 1470};
inline constexpr Box kNext{1918, 1430, 1966, 1470};
inline constexpr Box kVersion{1540, 1436, 1712, 1472};  // plugins: two lines, version and build
inline constexpr int kVisibleRows = 4;
inline constexpr float kTableY = 1204, kRowH = 50;
inline constexpr int kSettingCount = 4;  // CHANNEL, TRANSPOSE, VELOCITY, SPLIT
inline constexpr Box setting(int i) { return {434, 1186.f + i * 44, 788, 1226.f + i * 44}; }
inline constexpr Box decrement(int i) {
  const auto r = setting(i);
  return {594, r.t + 2, 638, r.b - 2};
}
inline constexpr Box increment(int i) {
  const auto r = setting(i);
  return {742, r.t + 2, 786, r.b - 2};
}
inline constexpr Box mode(int row) {
  const float y = kTableY + row * kRowH;
  return {1708, y + 5, 1848, y + 45};
}
inline constexpr Box remove(int row) {
  const float y = kTableY + row * kRowH;
  return {1914, y + 5, 1958, y + 45};
}
inline constexpr Box action(int i) { return {832.f + i * 140, 1436, 962.f + i * 140, 1472}; }
inline constexpr core::MidiAction kActions[] = {
    core::MidiAction::cartridge_prev, core::MidiAction::preset_load_a, core::MidiAction::preset_load_b,
    core::MidiAction::preset_load_c, core::MidiAction::preset_load_d};
inline constexpr const char* kActionLabels[] = {"CART PREV", "PRESET A", "PRESET B", "PRESET C", "PRESET D"};
inline constexpr std::uint32_t kPaper = 0xe9e0d2, kCard = 0xf5eee3, kInk = 0x24211e, kMuted = 0x655e54,
                               kRule = 0xc8bdad, kTeal = 0x005e7a, kRed = 0xcb2026, kWhite = 0xfffaf2;
// Disabled buttons are outlined ghosts: visible on paper, cards and alternate rows alike.
// Armed status text is a deeper kRed, readable at small sizes on paper (WCAG AA).
inline constexpr std::uint32_t kDisabledEdge = 0xb5a998, kDisabledText = 0x8a8174, kArmedText = 0xa81b20;

struct ProfileRow {
  std::string name;
  std::uint32_t bindings = 0;
  bool current = false, readable = true;
};
struct State {
  const core::MidiMap* map = nullptr;
  std::string device;
  std::string profileName = "Untitled", error, nameText;
  std::vector<ProfileRow> profiles;
  bool profileBrowser = false, deleteArmed = false, dirty = false;
  int nameAction = -1;
  int channel = 0, octave = 0, curve = 0, offset = 0;
  int split = core::kMidiDefaultSplitNote;  // MIDI note: TWIN / SPLIT right side starts here
  bool armed = false, awaitTarget = false, editable = true;
  const char* noDevice = "Choose an input in Preferences";  // shown when `device` is empty
  // The plugins have no About box: their version and build are shown here (empty: not shown).
  std::string version, build;
};
// MIDI note name with octave, middle C (60) = C4.
inline std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}
inline int pageOffset(int offset, int count) {
  return std::clamp(offset / kVisibleRows, 0, std::max(0, (count - 1) / kVisibleRows)) * kVisibleRows;
}
inline const char* actionName(core::MidiAction a) {
  static const char* names[] = {"DRONE 1",       "DRONE 2",       "DRONE 3",        "DRONE 4",
                                "DRONE 5",       "DRONE 6",       "CARTRIDGE NEXT", "CARTRIDGE PREV",
                                "LOAD PRESET A", "LOAD PRESET B", "LOAD PRESET C",  "LOAD PRESET D",
                                "MUTE",          "DRONE 1 PHOTO", "DRONE 2 PHOTO",  "DRONE 4 PHOTO",
                                "DRONE 5 PHOTO"};
  const auto i = static_cast<unsigned>(a);
  return i < core::kMidiActionCount ? names[i] : "?";
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
  auto label = [&](Box b, float size, std::uint32_t c, const std::string& text, bool bold = false,
                   bool center = false) { s.label(b, size, c, text.c_str(), bold, center); };
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
  label({kInput.l, kInput.t, 1410, kInput.b}, 12, 0xc8bdad, "INPUT", true);
  label({1420, kInput.t, kInput.r, kInput.b}, 17, kWhite,
        st.device.empty() ? std::string(st.noDevice) : asciiText(st.device), true);
  button(kClose, "CLOSE", true);
  rect({810, 1186, 812, 1404}, kRule, 0);
  const char* labels[] = {"CHANNEL", "TRANSPOSE", "VELOCITY", "SPLIT"};
  const char* curves[] = {"LINEAR", "SOFT", "HARD"};
  char values[kSettingCount][24];
  if (st.channel == 0)
    std::snprintf(values[0], sizeof(values[0]), "ANY");
  else
    std::snprintf(values[0], sizeof(values[0]), "%d", st.channel);
  std::snprintf(values[1], sizeof(values[1]), "%+d st", st.octave);
  std::snprintf(values[2], sizeof(values[2]), "%s", curves[std::clamp(st.curve, 0, 2)]);
  std::snprintf(values[3], sizeof(values[3]), "%s", noteName(st.split).c_str());
  for (int i = 0; i < kSettingCount; ++i) {
    const auto b = setting(i);
    rect(b, kCard);
    label({b.l + 14, b.t, 590, b.b}, 16, kMuted, labels[i], true);
    button(decrement(i), "<", st.editable && !st.profileBrowser);
    button(increment(i), ">", st.editable && !st.profileBrowser);
    label({640, b.t, 740, b.b}, 20, kInk, values[i], true, true);
  }
  const bool retry = st.dirty && !st.error.empty();
  button(kLearn, retry ? "RETRY SAVE" : st.profileBrowser ? "BACK TO BINDINGS" :
                 st.armed ? "CANCEL LEARN" : "+ LEARN A CONTROL", st.editable || st.profileBrowser, !st.profileBrowser);
  const int count = st.profileBrowser ? static_cast<int>(st.profiles.size()) :
                    st.map ? static_cast<int>(st.map->count()) : 0;
  const int offset = pageOffset(st.offset, count);
  if (st.nameAction >= 0) {
    const char* titles[] = {"NEW PROFILE NAME", "COPY PROFILE AS", "RENAME CURRENT PROFILE"};
    label({844, 1180, 1640, 1202}, 14, kMuted, titles[std::clamp(st.nameAction, 0, 2)], true);
    rect({832, 1205, 1966, 1403}, kCard);
    rect(kNameEntry, kWhite, 4);
    label(kNameEntry, 19, kInk, st.nameText, true);
    label({846, 1270, 1948, 1310}, 18, kMuted, "English names, up to 39 characters.");
    label({846, 1314, 1948, 1354}, 16, kMuted, "Enter to confirm. Esc to cancel.");
  } else if (st.profileBrowser) {
    label({844, 1180, 1640, 1202}, 14, kMuted, "SAVED PROFILES", true);
    label({1720, 1180, 1950, 1202}, 14, kMuted, "BINDINGS", true, true);
    for (int i = 0; i < kVisibleRows; ++i) {
      const auto box = profileRow(i);
      const int row = offset + i;
      rect(box, i % 2 == 0 ? kCard : 0xe2d8c9, 5);
      if (row >= count) continue;
      const auto& profile = st.profiles[static_cast<std::size_t>(row)];
      if (profile.readable && box.contains(mouseX, mouseY)) rect(box, 0xd9cdbc, 5);
      if (profile.current) rect({box.l, box.t + 5, box.l + 4, box.b - 5}, kTeal, 1);
      label({850, box.t, 1620, box.b}, 19, profile.readable ? kInk : kMuted, profile.name, true);
      if (profile.current) label({1628, box.t, 1732, box.b}, 12, kTeal, "CURRENT", true, true);
      label({1750, box.t, 1950, box.b}, 17, kMuted,
            profile.readable ? std::to_string(profile.bindings) : "UNAVAILABLE", false, true);
    }
  } else {
    label({844, 1180, 1198, 1202}, 14, kMuted, "CONTROLLER", true);
    label({1220, 1180, 1688, 1202}, 14, kMuted, "PANEL TARGET", true);
    label({1708, 1180, 1848, 1202}, 14, kMuted, "MODE", true, true);
    for (int i = 0; i < kVisibleRows; ++i) {
      const float y = kTableY + i * kRowH;
      rect({832, y + 1, 1966, y + 49}, i % 2 == 0 ? kCard : 0xe2d8c9, 5);
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
      label({846, y + 2, 1196, y + 27}, 18, kInk, source, true);
      label({846, y + 26, 1196, y + 47}, 14, kMuted, b.key.device[0] ? asciiText(b.key.device) : "Any device");
      label({1220, y + 4, 1688, y + 46}, 18, kInk, targetText(b), true);
      const auto drive = core::midi_parameter_drive(b);
      if (b.targetKind != core::MidiTargetKind::parameter && core::midi_action_is_continuous(b.action)) {
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
      rect({832, 1204, 1966, 1404}, kCard);
      label({866, 1240, 1932, 1282}, 22, kInk, "No controller bindings yet", true, true);
      label({866, 1284, 1932, 1320}, 18, kMuted,
            "Click LEARN, choose a panel control, then move a knob or press a pad.", false, true);
      label({866, 1326, 1932, 1360}, 16, kMuted, "Bindings are saved separately from your sound settings.",
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
    line1 = "1. Choose a panel control above";
    line2 = "or an EXTRA LEARN TARGET.";
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
  if (!st.version.empty()) {
    label({kVersion.l, kVersion.t, kVersion.r, kVersion.t + 18}, 13, kMuted, st.version, true);
    label({kVersion.l, kVersion.t + 18, kVersion.r, kVersion.b}, 12, kMuted, st.build);
  }
  button(kPrevious, "<", offset > 0);
  button(kNext, ">", offset + kVisibleRows < count);
  char page[32];
  std::snprintf(page, sizeof(page), "%d / %d", offset / kVisibleRows + 1,
                std::max(1, (count + kVisibleRows - 1) / kVisibleRows));
  label({1776, 1428, 1912, 1453}, 18, kInk, page, true, true);
  char total[32];
  std::snprintf(total, sizeof(total), "%d %s", count, st.profileBrowser ? (count == 1 ? "profile" : "profiles") : (count == 1 ? "binding" : "bindings"));
  label({1776, 1452, 1912, 1475}, 13, kMuted, total, false, true);
}
}  // namespace lunar24::host::midi_ui
