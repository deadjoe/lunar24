// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/midi_map.h>
#include <lunar24/core/state_edit.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace lunar24::host::midi_ui {

// One geometry for rendering, hit testing and the offline preview. Design units
// scale with the main panel; primary text is 18-22, interactive rows are 44-50 high.
struct Box {
  float l, t, r, b;
  bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
};
inline constexpr Box kBounds{410, 1112, 1990, 1482};
inline constexpr Box kClose{1846, 1126, 1966, 1166};
inline constexpr Box kLearn{434, 1364, 788, 1404};
inline constexpr Box kPrevious{1722, 1430, 1770, 1470};
inline constexpr Box kNext{1918, 1430, 1966, 1470};
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

struct State {
  const core::MidiMap* map = nullptr;
  std::string device;
  int channel = 0, octave = 0, curve = 0, offset = 0;
  int split = core::kMidiDefaultSplitNote;  // MIDI note: TWIN / SPLIT right side starts here
  bool armed = false, awaitTarget = false, editable = true;
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
  label({832, 1128, 904, 1164}, 14, 0xc8bdad, "INPUT", true);
  label({914, 1128, 1816, 1164}, 19, kWhite,
        st.device.empty() ? "Choose an input in Preferences" : asciiText(st.device), true);
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
    button(decrement(i), "<", st.editable);
    button(increment(i), ">", st.editable);
    label({640, b.t, 740, b.b}, 20, kInk, values[i], true, true);
  }
  button(kLearn, st.armed ? "CANCEL LEARN" : "+ LEARN A CONTROL", st.editable, true);
  label({844, 1180, 1198, 1202}, 14, kMuted, "CONTROLLER", true);
  label({1220, 1180, 1688, 1202}, 14, kMuted, "PANEL TARGET", true);
  label({1708, 1180, 1848, 1202}, 14, kMuted, "MODE", true, true);
  const int count = st.map ? static_cast<int>(st.map->count()) : 0;
  const int offset = pageOffset(st.offset, count);
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
  rect({434, 1412, 1966, 1414}, kRule, 0);
  const char *line1 = "Link a panel control to a knob,", *line2 = "button or pad on your controller.";
  if (!st.editable) {
    line1 = "Bindings file cannot be read.";
    line2 = "Editing is disabled to protect it.";
  } else if (st.armed && st.awaitTarget) {
    line1 = "1. Choose a panel control above";
    line2 = "or an EXTRA LEARN TARGET.";
  } else if (st.armed) {
    line1 = "2. Move a knob or press a pad.";
    line2 = "Esc or CANCEL LEARN to cancel.";
  }
  label({434, 1424, 802, 1448}, 16, st.armed ? kArmedText : kMuted, line1, true);
  label({434, 1449, 802, 1474}, 15, kMuted, line2);
  label({832, 1416, 1532, 1435}, 12, kMuted, "EXTRA LEARN TARGETS", true);
  for (int i = 0; i < 5; ++i) button(action(i), kActionLabels[i], st.editable && st.armed && st.awaitTarget);
  button(kPrevious, "<", offset > 0);
  button(kNext, ">", offset + kVisibleRows < count);
  char page[32];
  std::snprintf(page, sizeof(page), "%d / %d", offset / kVisibleRows + 1,
                std::max(1, (count + kVisibleRows - 1) / kVisibleRows));
  label({1776, 1428, 1912, 1453}, 18, kInk, page, true, true);
  char total[32];
  std::snprintf(total, sizeof(total), "%d %s", count, count == 1 ? "binding" : "bindings");
  label({1776, 1452, 1912, 1475}, 13, kMuted, total, false, true);
}
}  // namespace lunar24::host::midi_ui
