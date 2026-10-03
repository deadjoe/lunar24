// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// keyboard_menu_view.h — the KEYBOARD MENU overlay (opened by the red keyboard encoder).
// Same pattern as midi_settings_view.h: one table of boxes drives drawing, hit testing,
// the offline preview and the layout test. The 36 settings are grouped by function into
// six tabs; each setting appears exactly once. The editor (panel_editor.h) only maps a hit
// to the same EditorShared / engine calls as before, so values and routing are unchanged.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include <host/midi_settings_view.h>
#include <host/panel_art.h>
#include <host/panel_format.h>
#include <lunar24/core/arp_sequencer.h>
#include <lunar24/registry.hpp>

namespace lunar24::host::kb_ui {

using midi_ui::Box;
using midi_ui::kCard, midi_ui::kDisabledEdge, midi_ui::kDisabledText, midi_ui::kArmedText, midi_ui::kInk;
using midi_ui::kMuted, midi_ui::kPaper, midi_ui::kRed, midi_ui::kRule, midi_ui::kTeal, midi_ui::kWhite;
using P = core::ParameterId;

enum Tab : int { kPlay, kExpression, kArp, kSeq, kSteps, kService, kTabCount };
inline constexpr const char* kTabLabels[kTabCount] = {"PLAY", "EXPRESSION", "ARP", "SEQ", "SEQ STEPS", "SERVICE"};
// Segmented: every option visible, one click picks it. Latch: on/off with an LED.
// Knob: continuous value (drag). Trimmer: small service knob. Stepper: clamped < value >.
// RootKeys: one octave of keys, click the root note.
enum class Kind : std::uint8_t { Segmented, VSegmented, Latch, Knob, Trimmer, Stepper, RootKeys };

// ---- frame -------------------------------------------------------------------------------------
inline constexpr Box kBounds{410, 1112, 1990, 1482};
inline constexpr Box kTitleBar{414, 1116, 1986, 1172};
inline constexpr Box kClose{1846, 1126, 1966, 1166};
inline constexpr Box kSide{1612, 1129, 1826, 1163};  // EDITING LEFT | RIGHT (PLAY = SPLIT only)
inline constexpr Box tab(int i) { return {652.f + i * 138.f, 1124, 786.f + i * 138.f, 1172}; }
inline constexpr Box sideHalf(int i) {
  return {kSide.l + 1 + 106.f * i + (i ? 1.f : 0.f), kSide.t + 1, kSide.l + 1 + 106.f * (i + 1), kSide.b - 1};
}
// ---- footer: presets A-D (manual p.19) and RESET PANEL ------------------------------------------
inline constexpr float kFooterRule = 1424;
inline constexpr Box kPresetSlots{1100, 1434, 1280, 1470};  // A | B | C | D
inline constexpr Box kPresetLoad{1292, 1434, 1374, 1470};
inline constexpr Box kPresetSave{1382, 1434, 1464, 1470};
inline constexpr Box kPresetInit{1472, 1434, 1554, 1470};
inline constexpr Box kReset{1786, 1434, 1966, 1470};

// ---- cards -------------------------------------------------------------------------------------
// Under SPLIT a card shows whether its settings follow EDITING (LEFT / RIGHT) or are GLOBAL.
enum class Tag : std::uint8_t { None, Side, Global };
struct Card {
  int tab;
  Box box;
  const char* title;
  Tag tag;
  float tagL = 0;  // 0 = tag at the card's top right; else its left edge (header holds a control)
};
inline constexpr float kCardT = 1184, kCardB = 1416;
inline constexpr Card kCards[] = {
    {kPlay, {434, kCardT, 884, kCardB}, "KEYBOARD", Tag::None},  // PLAY global, MODE per side: badge on MODE
    {kPlay, {900, kCardT, 1500, kCardB}, "QUANTISER", Tag::Side},
    {kPlay, {1516, kCardT, 1966, kCardB}, "CLOCK", Tag::Global},
    {kExpression, {434, kCardT, 754, kCardB}, "PORTAMENTO", Tag::Side},
    {kExpression, {770, kCardT, 1366, kCardB}, "VIBRATO", Tag::Side},
    {kExpression, {1382, kCardT, 1966, kCardB}, "PRESSURE OUTPUT", Tag::Side},
    {kArp, {434, kCardT, 1100, kCardB}, "ARPEGGIATOR", Tag::Side},
    {kArp, {1116, kCardT, 1966, kCardB}, "ARP RHYTHM", Tag::Side, 1252},
    {kSeq, {434, kCardT, 1100, kCardB}, "SEQUENCER", Tag::Side},
    {kSeq, {1116, kCardT, 1966, kCardB}, "SEQ RHYTHM", Tag::Side, 1252},
    {kSteps, {434, kCardT, 1966, kCardB}, "", Tag::None},
    {kService, {434, 1232, 904, kCardB}, "OUTPUT CALIBRATION", Tag::Global},
    {kService, {920, 1232, 1420, kCardB}, "TOUCH SENSOR", Tag::Global},
    {kService, {1436, 1232, 1766, kCardB}, "MPR121", Tag::Global},
    {kService, {1782, 1232, 1966, kCardB}, "ENCODER", Tag::Global},
};

// ---- the 36 settings, one entry each -------------------------------------------------------------
// Knob / Trimmer boxes are the whole block (name, knob, value): the drag target.
struct Item {
  P id;
  int tab;
  Kind kind;
  Box box;
  const char* label;
};
inline constexpr Box knobBlock(float cx, float cy) { return {cx - 62, cy - 62, cx + 62, cy + 70}; }
inline constexpr Box trimBlock(float cx, float cy) { return {cx - 52, cy - 50, cx + 52, cy + 56}; }
inline constexpr Item kItems[] = {
    // PLAY: keyboard behaviour, quantiser, clocks
    {P::keyboard_behaviour, kPlay, Kind::Segmented, {454, 1240, 864, 1282}, "PLAY"},
    {P::keyboard_mode, kPlay, Kind::Segmented, {454, 1322, 864, 1364}, "MODE"},
    {P::keyboard_quantise_load_scale, kPlay, Kind::Stepper, {920, 1240, 1480, 1282}, "SCALE"},
    {P::keyboard_root_note, kPlay, Kind::RootKeys, {920, 1322, 1480, 1404}, "ROOT"},
    {P::keyboard_clock_bpm, kPlay, Kind::Knob, {1536, 1222, 1700, 1404}, "TEMPO"},
    {P::sequencer_clock, kPlay, Kind::Segmented, {1720, 1262, 1946, 1304}, "5-STEP SEQ CLOCK"},
    // EXPRESSION: portamento, vibrato, pressure output
    {P::keyboard_portamento_speed, kExpression, Kind::Knob, knobBlock(522, 1312), "GLIDE"},
    {P::keyboard_portamento_legato, kExpression, Kind::Latch, {612, 1262, 734, 1304}, "LEGATO"},
    {P::keyboard_vibrato_speed, kExpression, Kind::Knob, knobBlock(845, 1312), "RATE"},
    {P::keyboard_vibrato_depth, kExpression, Kind::Knob, knobBlock(994, 1312), "DEPTH"},
    {P::keyboard_vibrato_delay, kExpression, Kind::Knob, knobBlock(1143, 1312), "DELAY"},
    {P::keyboard_vibrato_pressure, kExpression, Kind::Knob, knobBlock(1292, 1312), "PRESSURE"},
    {P::keyboard_pressure_output, kExpression, Kind::Segmented, {1402, 1222, 1946, 1262}, ""},
    {P::keyboard_pressure_rise, kExpression, Kind::Knob, knobBlock(1476, 1340), "RISE"},
    {P::keyboard_pressure_fall, kExpression, Kind::Knob, knobBlock(1618, 1340), "FALL"},
    // ARP (its rhythm pattern sits beside it)
    {P::keyboard_arp_hold, kArp, Kind::Latch, {454, 1240, 574, 1282}, "HOLD"},
    {P::keyboard_arp_direction, kArp, Kind::Segmented, {594, 1240, 1080, 1282}, "DIRECTION"},
    {P::keyboard_arp_variation, kArp, Kind::Segmented, {454, 1322, 754, 1364}, "VARIATION"},
    {P::keyboard_arp_interval, kArp, Kind::Stepper, {774, 1322, 1080, 1364}, "INTERVAL"},
    {P::keyboard_arp_length, kArp, Kind::Stepper, {1782, 1192, 1946, 1230}, "LENGTH"},
    // SEQ (its rhythm pattern sits beside it; the 16 steps have their own tab)
    {P::keyboard_seq_run, kSeq, Kind::Segmented, {454, 1240, 744, 1282}, "RUN"},
    {P::keyboard_seq_cv_output, kSeq, Kind::Segmented, {764, 1240, 1080, 1282}, "CV OUTPUT"},
    {P::keyboard_seq_direction, kSeq, Kind::Segmented, {454, 1322, 894, 1364}, "DIRECTION"},
    {P::keyboard_seq_length, kSeq, Kind::Stepper, {914, 1322, 1080, 1364}, "LENGTH"},
    {P::keyboard_seq_rhythm_length, kSeq, Kind::Stepper, {1782, 1192, 1946, 1230}, "LENGTH"},
    // SERVICE: the hardware calibration menu (manual p.20), stored only
    {P::keyboard_calibration_v_oct, kService, Kind::Trimmer, trimBlock(510, 1322), "V/OCT OUT"},
    {P::keyboard_calibration_pressure, kService, Kind::Trimmer, trimBlock(624, 1322), "PRESS OUT"},
    {P::keyboard_dac_vref, kService, Kind::VSegmented, {700, 1292, 884, 1372}, "DAC VREF"},
    {P::keyboard_touch_threshold, kService, Kind::Trimmer, trimBlock(982.5f, 1322), "TOUCH"},
    {P::keyboard_release_threshold, kService, Kind::Trimmer, trimBlock(1107.5f, 1322), "RELEASE"},
    {P::keyboard_pressure_min, kService, Kind::Trimmer, trimBlock(1232.5f, 1322), "P MIN"},
    {P::keyboard_pressure_max, kService, Kind::Trimmer, trimBlock(1357.5f, 1322), "P MAX"},
    {P::keyboard_mpr121_charge, kService, Kind::Trimmer, trimBlock(1491, 1322), "CHARGE"},
    {P::keyboard_mpr121_discharge, kService, Kind::Trimmer, trimBlock(1601, 1322), "DISCHARGE"},
    {P::keyboard_debounce, kService, Kind::Trimmer, trimBlock(1711, 1322), "DEBOUNCE"},
    {P::keyboard_encoder_direction, kService, Kind::VSegmented, {1802, 1292, 1946, 1372}, "DIRECTION"},
};
inline constexpr int kItemCount = int(sizeof(kItems) / sizeof(kItems[0]));
// A stepper in a card header has its name to its left; elsewhere names sit above the control.
inline constexpr bool inHeader(const Item& it) { return it.box.t < kCardT + 30; }

// ---- control parts (shared by drawing and hit testing) ------------------------------------------
inline int optionCount(P id) {
  const auto* d = core::find_parameter(id);
  return d != nullptr && d->optionCount > 0 ? int(d->optionCount) : 2;
}
inline Box segment(Box b, int n, int i, bool vertical) {
  const float span = vertical ? b.b - b.t - 2 : b.r - b.l - 2;
  const float a = span * float(i) / float(n), z = span * float(i + 1) / float(n);
  if (vertical) return {b.l + 1, b.t + 1 + a + (i ? 1.f : 0.f), b.r - 1, b.t + 1 + z};
  return {b.l + 1 + a + (i ? 1.f : 0.f), b.t + 1, b.l + 1 + z, b.b - 1};
}
inline Box stepDown(Box b) { return {b.l, b.t, b.l + (b.b - b.t), b.b}; }
inline Box stepUp(Box b) { return {b.r - (b.b - b.t), b.t, b.r, b.b}; }
struct Dial {
  float cx, cy, r;
};
inline Dial dial(const Item& it) {
  const float cx = (it.box.l + it.box.r) / 2;
  if (it.kind == Kind::Trimmer) return {cx, it.box.t + 50, 20};
  if (it.id == P::keyboard_clock_bpm) return {cx, 1296, 36};
  return {cx, it.box.t + 62, 30};
}
inline Box tempoNudge(const Item& it) {  // < 120 BPM > below the TEMPO knob
  const Dial d = dial(it);
  return {it.box.l, d.cy + d.r + 14, it.box.r, d.cy + d.r + 50};
}
inline constexpr int kWhiteKeys[7] = {0, 2, 4, 5, 7, 9, 11};
inline constexpr int kBlackKeys[5] = {1, 3, 6, 8, 10};
inline constexpr float kBlackAt[5] = {1, 2, 4, 5, 6};  // in white-key widths from the left
inline Box rootKey(Box b, int semitone) {
  const float ww = (b.r - b.l) / 7;
  for (int k = 0; k < 7; ++k)
    if (kWhiteKeys[k] == semitone) return {b.l + ww * float(k) + 1.5f, b.t + 1.5f, b.l + ww * float(k + 1) - 1.5f, b.b - 1.5f};
  for (int k = 0; k < 5; ++k)
    if (kBlackKeys[k] == semitone) return {b.l + kBlackAt[k] * ww - 24, b.t + 1.5f, b.l + kBlackAt[k] * ww + 24, b.t + 50};
  return {0, 0, 0, 0};
}
// Rhythm pads (ARP tab: arpeggiator pattern, SEQ tab: sequencer pattern, same place) and the
// 16 sequencer step columns (fader above, gate button below).
inline constexpr int kRhythmSteps = 8, kSeqSteps = 16, kSeqMaxNote = 24;
inline constexpr Box rhythmPad(int i) { return {1156.f + i * 98.f, 1250, 1240.f + i * 98.f, 1334}; }
inline constexpr float kStepX0 = 510, kStepW = 90, kFaderTop = 1230, kFaderBottom = 1356;
inline constexpr float kGateT = 1372, kGateB = 1406;
inline constexpr Box stepColumn(int i) { return {kStepX0 + i * kStepW, 1192, kStepX0 + (i + 1) * kStepW, 1408}; }
inline constexpr Box stepGate(int i) { return {kStepX0 + i * kStepW + 12, kGateT, kStepX0 + (i + 1) * kStepW - 12, kGateB}; }
inline int noteAt(float y) {
  const double t = (double(kFaderBottom) - double(y)) / double(kFaderBottom - kFaderTop);
  return int(std::lround(std::clamp(t, 0.0, 1.0) * kSeqMaxNote));
}

// ---- values ------------------------------------------------------------------------------------
// Count settings are stored as 0..1 and decoded by the core (rounding); a step writes the
// exact value for the next count, so the stepper and the engine always agree.
struct Count {
  P id;
  int lo, hi;
};
inline constexpr Count kCounts[] = {{P::keyboard_arp_interval, 1, 12},
                                    {P::keyboard_arp_length, 1, 8},
                                    {P::keyboard_seq_length, 2, 16},
                                    {P::keyboard_seq_rhythm_length, 1, 8}};
inline const Count* countOf(P id) {
  for (const auto& c : kCounts)
    if (c.id == id) return &c;
  return nullptr;
}
inline int countValue(P id, double v) {
  switch (id) {
    case P::keyboard_arp_interval: return int(core::arp_interval_semitones(v));
    case P::keyboard_arp_length: return int(core::arp_length_steps(v));
    case P::keyboard_seq_length: return int(core::seq_length_steps(v));
    case P::keyboard_seq_rhythm_length: return int(core::seq_rhythm_length_steps(v));
    default: return 0;
  }
}
inline double countNorm(const Count& c, int k) { return double(std::clamp(k, c.lo, c.hi) - c.lo) / double(c.hi - c.lo); }
// TEMPO: 10..300 BPM (formatParam); the < > buttons move it by one whole BPM.
inline int bpmValue(double v) { return int(std::lround(10.0 + 290.0 * std::clamp(v, 0.0, 1.0))); }
inline double bpmNorm(int bpm) { return double(std::clamp(bpm, 10, 300) - 10) / 290.0; }
inline int rootValue(double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 11.0)); }
inline double rootNorm(int semitone) { return double(std::clamp(semitone, 0, 11)) / 11.0; }
inline int optionIndex(P id, double v) {
  const auto* d = core::find_parameter(id);
  if (d == nullptr) return 0;
  return std::clamp(int(std::lround((v - d->min) / (d->step > 0 ? d->step : 1.0))), 0, optionCount(id) - 1);
}
// Option names in full, upper case (x1..x3 stay as printed in the manual).
inline std::string optionText(P id, int i) {
  const auto* d = core::find_parameter(id);
  if (d == nullptr || i < 0 || i >= int(d->optionCount)) return "?";
  std::string s(d->options[i]);
  if (s.size() == 2 && s[0] == 'x') return s;
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// ---- hit testing -------------------------------------------------------------------------------
enum class HitKind : std::uint8_t { None, Close, Tab, Side, Slot, Load, Save, Init, Reset, Item, Pad, Fader, Gate };
// Item: index = kItems index; sub = option (segmented), -1 / +1 (stepper arrows, TEMPO
// nudge; 0 = the value between them), semitone (root keys, -1 = between keys), 0 = knob.
struct Hit {
  HitKind kind = HitKind::None;
  int index = 0, sub = 0;
};
inline Hit hitTest(int page, bool split, float x, float y) {
  if (kClose.contains(x, y)) return {HitKind::Close};
  for (int i = 0; i < kTabCount; ++i)
    if (tab(i).contains(x, y)) return {HitKind::Tab, i};
  if (split)
    for (int i = 0; i < 2; ++i)
      if (sideHalf(i).contains(x, y)) return {HitKind::Side, i};
  for (int i = 0; i < 4; ++i)
    if (segment(kPresetSlots, 4, i, false).contains(x, y)) return {HitKind::Slot, i};
  if (kPresetLoad.contains(x, y)) return {HitKind::Load};
  if (kPresetSave.contains(x, y)) return {HitKind::Save};
  if (kPresetInit.contains(x, y)) return {HitKind::Init};
  if (kReset.contains(x, y)) return {HitKind::Reset};
  for (int i = 0; i < kItemCount; ++i) {
    const Item& it = kItems[i];
    if (it.tab != page || !it.box.contains(x, y)) continue;
    switch (it.kind) {
      case Kind::Segmented:
      case Kind::VSegmented: {
        const int n = optionCount(it.id);
        for (int k = 0; k < n; ++k)
          if (segment(it.box, n, k, it.kind == Kind::VSegmented).contains(x, y)) return {HitKind::Item, i, k};
        return {};
      }
      case Kind::Stepper:
        return {HitKind::Item, i, stepDown(it.box).contains(x, y) ? -1 : stepUp(it.box).contains(x, y) ? 1 : 0};
      case Kind::RootKeys:
        for (int k : kBlackKeys)  // black keys lie on top of the white ones
          if (rootKey(it.box, k).contains(x, y)) return {HitKind::Item, i, k};
        for (int k : kWhiteKeys)
          if (rootKey(it.box, k).contains(x, y)) return {HitKind::Item, i, k};
        return {HitKind::Item, i, -1};
      case Kind::Knob:
        if (it.id == P::keyboard_clock_bpm && tempoNudge(it).contains(x, y)) {
          const Box n = tempoNudge(it);
          if (stepDown(n).contains(x, y)) return {HitKind::Item, i, -1};
          if (stepUp(n).contains(x, y)) return {HitKind::Item, i, 1};
        }
        return {HitKind::Item, i, 0};
      default: return {HitKind::Item, i, 0};
    }
  }
  if (page == kArp || page == kSeq)
    for (int i = 0; i < kRhythmSteps; ++i)
      if (rhythmPad(i).contains(x, y)) return {HitKind::Pad, i};
  if (page == kSteps)
    for (int i = 0; i < kSeqSteps; ++i) {
      if (stepGate(i).contains(x, y)) return {HitKind::Gate, i};
      if (stepColumn(i).contains(x, y) && y < kGateT) return {HitKind::Fader, i};
    }
  return {};
}

// ---- drawing -----------------------------------------------------------------------------------
struct SeqStep {
  int note = 0;
  bool gate = true;
};
struct State {
  std::function<double(P)> value;  // the value shown: per-side settings read the edited side
  int tab = kPlay;
  bool split = false;
  int side = 0, presetSlot = 0;
  bool resetArmed = false, initArmed = false;
  int presetDone = -1;  // 0 LOAD, 1 SAVE, 2 INIT: briefly shows what happened
  SeqStep steps[kSeqSteps];
  unsigned arpMask = 0, seqMask = 0;  // bit set = silent beat (engine convention)
};
inline constexpr std::uint32_t kLedOn = 0xeb3c32, kLedOff = 0x9c9284, kHover = 0xd9cdbc, kAltCard = 0xe2d8c9;
inline constexpr std::uint32_t kKnobCap = 0xd9362b, kTick = 0x343434, kBarText = 0xc8bdad;

// Sink: fillRect, fillCircle, circle/moveTo/lineTo + fillGrad/strokePath (panel_art.h), and
// label(box, size, rgb, text, bold, align) with align 0 = left, 1 = centre, 2 = right.
template <class Sink>
void draw(Sink& s, const State& st, float mouseX = -1, float mouseY = -1) {
  auto rect = [&](Box r, std::uint32_t c, float radius = 6) { s.fillRect(r.l, r.t, r.r, r.b, c, radius); };
  auto framed = [&](Box r, std::uint32_t edge, std::uint32_t face, float radius = 6) {
    rect(r, edge, radius);
    rect({r.l + 1, r.t + 1, r.r - 1, r.b - 1}, face, radius - 1);
  };
  auto label = [&](Box r, float size, std::uint32_t c, const std::string& t, bool bold = false, int align = 0) {
    s.label(r, size, c, t.c_str(), bold, align);
  };
  auto hot = [&](Box r) { return r.contains(mouseX, mouseY); };
  // Push button: white face, rule edge; ghost outline when disabled; accent = solid fill.
  auto button = [&](Box r, const std::string& t, bool enabled, std::uint32_t accent = 0, float size = 16) {
    if (!enabled) {
      framed(r, kDisabledEdge, kPaper);
      label(r, size, kDisabledText, t, true, 1);
    } else if (accent != 0) {
      rect(r, accent);
      label(r, size, kWhite, t, true, 1);
    } else {
      framed(r, kRule, hot(r) ? kHover : kWhite);
      label(r, size, kInk, t, true, 1);
    }
  };
  auto led = [&](float cx, float cy, float r, bool on) {  // red when on, like the panel LEDs
    if (on) {
      s.circle(cx, cy, r * 2.4f);
      s.fillGrad(art::radialGrad(cx, cy, r * 0.8f, r * 2.4f, kLedOn, 0.45f, kLedOn, 0.f));
    }
    s.fillCircle(cx, cy, r + 1.5f, on ? 0x7a1a14 : 0x6f665a);
    s.fillCircle(cx, cy, r, on ? kLedOn : kLedOff);
    s.fillCircle(cx - r * 0.3f, cy - r * 0.35f, r * 0.35f, on ? 0xffb0a0 : 0xbdb3a5);
  };
  auto segmented = [&](Box r, int n, int sel, bool vertical, const std::function<std::string(int)>& text) {
    rect(r, kRule, 6);
    for (int i = 0; i < n; ++i) {
      const Box c = segment(r, n, i, vertical);
      const bool on = i == sel;
      rect(c, on ? kTeal : (hot(c) ? kHover : kWhite), 5);
      label({c.l + 4, c.t, c.r - 4, c.b}, n >= 5 ? 14.f : 15.f, on ? kWhite : kInk, text(i), true, 1);
    }
  };
  auto stepper = [&](Box r, const std::string& value, bool canDown, bool canUp, float size) {
    rect(r, kRule, 6);
    rect({r.l + 1, r.t + 1, r.r - 1, r.b - 1}, kCard, 5);
    button(stepDown(r), "<", canDown);
    button(stepUp(r), ">", canUp);
    label({stepDown(r).r + 4, r.t, stepUp(r).l - 4, r.b}, size, kInk, value, true, 1);
  };
  auto nameAbove = [&](Box r, const char* text) { label({r.l, r.t - 24, r.r, r.t - 4}, 14, kMuted, text, true); };
  auto valueBox = [&](Box r, const std::string& t, float size) {
    framed(r, kRule, kWhite, 5);
    label(r, size, kInk, t, true, 1);
  };
  auto norm = [](P id, double v) {
    const auto* d = core::find_parameter(id);
    return d != nullptr && d->max > d->min ? (v - d->min) / (d->max - d->min) : 0.0;
  };
  auto angle = [](double n) { return float(-150.0 + std::clamp(n, 0.0, 1.0) * 300.0); };
  const char* sideName = st.side == 0 ? "LEFT" : "RIGHT";

  // Frame, title bar, tabs, EDITING switch, CLOSE.
  rect(kBounds, kInk, 12);
  rect({412, 1114, 1988, 1480}, kPaper, 10);
  rect(kTitleBar, kInk, 8);
  label({434, 1122, 640, 1166}, 22, kWhite, "KEYBOARD MENU", true);
  for (int i = 0; i < kTabCount; ++i) {
    const Box t = tab(i);
    if (st.tab == i) {
      rect({t.l, t.t, t.r, t.b + 4}, kPaper, 8);
      label(t, 16, kInk, kTabLabels[i], true, 1);
    } else {
      if (hot(t)) rect({t.l, t.t + 4, t.r, t.b - 6}, 0x3d3833, 6);
      label({t.l, t.t, t.r, t.b - 2}, 15, kBarText, kTabLabels[i], true, 1);
    }
  }
  if (st.split) {
    label({1500, 1129, 1604, 1163}, 13, kBarText, "EDITING", true, 2);
    rect(kSide, 0x4a443d, 6);
    const char* sides[] = {"LEFT C-F", "RIGHT F#-B"};
    for (int i = 0; i < 2; ++i) {
      const Box h = sideHalf(i);
      rect(h, st.side == i ? kTeal : (hot(h) ? 0x3d3833 : 0x2e2a26), 5);
      label(h, 14, st.side == i ? kWhite : kBarText, sides[i], true, 1);
    }
  }
  button(kClose, "CLOSE", true);

  // Cards.
  for (const Card& c : kCards) {
    if (c.tab != st.tab) continue;
    rect(c.box, kCard, 8);
    if (c.title[0] != '\0') label({c.box.l + 18, c.box.t + 6, c.box.r - 18, c.box.t + 28}, 14, kInk, c.title, true);
    if (!st.split || c.tag == Tag::None) continue;
    const bool perSide = c.tag == Tag::Side;
    const float w = perSide ? 64.f : 72.f;
    const Box t = c.tagL > 0 ? Box{c.tagL, c.box.t + 8, c.tagL + w, c.box.t + 28}
                             : Box{c.box.r - 16 - w, c.box.t + 8, c.box.r - 16, c.box.t + 28};
    rect(t, perSide ? kTeal : kRule, 10);
    label(t, 12, perSide ? kWhite : kInk, perSide ? sideName : "GLOBAL", true, 1);
  }

  // Settings.
  for (const Item& it : kItems) {
    if (it.tab != st.tab) continue;
    const double v = st.value(it.id);
    const Box b = it.box;
    switch (it.kind) {
      case Kind::Segmented:
      case Kind::VSegmented: {
        if (it.label[0] != '\0') nameAbove(b, it.label);
        if (st.split && it.id == P::keyboard_mode) {  // MODE follows EDITING; PLAY is global
          const Box t{b.l + 56, b.t - 24, b.l + 116, b.t - 6};
          rect(t, kTeal, 9);
          label(t, 11, kWhite, sideName, true, 1);
        }
        segmented(b, optionCount(it.id), optionIndex(it.id, v), it.kind == Kind::VSegmented,
                  [&](int i) { return optionText(it.id, i); });
        break;
      }
      case Kind::Latch: {
        nameAbove(b, it.label);
        const bool on = v > 0.5;
        framed(b, kRule, hot(b) ? kHover : kWhite);
        led(b.l + 24, (b.t + b.b) / 2, 7, on);
        label({b.l + 40, b.t, b.r - 8, b.b}, 16, kInk, on ? "ON" : "OFF", true, 1);
        break;
      }
      case Kind::Knob: {
        const Dial d = dial(it);
        label({b.l, d.cy - d.r - 30, b.r, d.cy - d.r - 10}, 14, kMuted, it.label, true, 1);
        art::drawKnob(s, d.cx, d.cy, d.r, kKnobCap, true, angle(norm(it.id, v)), kTick, -150.f, 150.f, hot(b));
        if (it.id == P::keyboard_clock_bpm) {
          const int bpm = bpmValue(v);
          stepper(tempoNudge(it), formatParam(std::uint32_t(it.id), v), bpm > 10, bpm < 300, 16);
        } else {
          valueBox({d.cx - 48, d.cy + d.r + 12, d.cx + 48, d.cy + d.r + 38}, formatParam(std::uint32_t(it.id), v), 16);
        }
        break;
      }
      case Kind::Trimmer: {  // small black service knob with a printed scale
        const Dial d = dial(it);
        label({b.l, b.t, b.r, b.t + 20}, 13, kMuted, it.label, true, 1);
        for (int k = 0; k <= 10; ++k) {
          float x0, y0, x1, y1;
          art::polarPoint(d.cx, d.cy, 25.f, -150.f + 30.f * float(k), x0, y0);
          art::polarPoint(d.cx, d.cy, 30.f, -150.f + 30.f * float(k), x1, y1);
          s.moveTo(x0, y0);
          s.lineTo(x1, y1);
          s.strokePath(kRule, 2.f);
        }
        art::drawKnob(s, d.cx, d.cy, d.r, 0x262626, false, angle(norm(it.id, v)), kTick, -150.f, 150.f, hot(b));
        valueBox({d.cx - 40, d.cy + 32, d.cx + 40, d.cy + 54}, formatParam(std::uint32_t(it.id), v), 14);
        break;
      }
      case Kind::Stepper: {
        if (it.id == P::keyboard_quantise_load_scale) {
          const int i = optionIndex(it.id, v), n = optionCount(it.id);
          nameAbove(b, it.label);
          char idx[16];
          std::snprintf(idx, sizeof idx, "%d / %d", i + 1, n);
          label({b.r - 120, b.t - 24, b.r, b.t - 4}, 14, kMuted, idx, false, 2);
          stepper(b, optionText(it.id, i), i > 0, i < n - 1, 19);
        } else if (const Count* c = countOf(it.id)) {
          const int k = countValue(it.id, v);
          if (inHeader(it)) label({b.l - 90, b.t, b.l - 10, b.b}, 14, kMuted, it.label, true, 2);
          else nameAbove(b, it.label);
          stepper(b, formatParam(std::uint32_t(it.id), v), k > c->lo, k < c->hi, inHeader(it) ? 16.f : 18.f);
        }
        break;
      }
      case Kind::RootKeys: {
        static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int root = rootValue(v);
        nameAbove(b, it.label);
        label({b.r - 200, b.t - 24, b.r, b.t - 4}, 14, kInk, names[root], true, 2);
        const Hit over = hitTest(st.tab, st.split, mouseX, mouseY);
        const int hoverKey = over.kind == HitKind::Item && kItems[over.index].kind == Kind::RootKeys ? over.sub : -1;
        rect(b, kInk, 6);
        for (int k : kWhiteKeys) {
          const Box key = rootKey(b, k);
          const bool on = k == root;
          rect(key, on ? kTeal : (k == hoverKey ? kHover : kWhite), 4);
          label({key.l, key.b - 26, key.r, key.b - 4}, 15, on ? kWhite : kMuted, names[k], true, 1);
        }
        for (int k : kBlackKeys) {
          const Box key = rootKey(b, k);
          const bool on = k == root;
          rect(key, on ? kTeal : (k == hoverKey ? 0x4a443d : kInk), 4);
          if (on) label({key.l, key.b - 24, key.r, key.b - 4}, 13, kWhite, names[k], true, 1);
        }
        break;
      }
    }
  }

  // Tab furniture: rhythm pads, captions, the step editor, the service banner.
  if (st.tab == kArp || st.tab == kSeq) {
    const bool arp = st.tab == kArp;
    const P lengthId = arp ? P::keyboard_arp_length : P::keyboard_seq_rhythm_length;
    const int length = countValue(lengthId, st.value(lengthId));
    const unsigned mask = arp ? st.arpMask : st.seqMask;
    for (int i = 0; i < kRhythmSteps; ++i) {
      const Box p = rhythmPad(i);
      const bool inUse = i < length, plays = ((mask >> i) & 1u) == 0;
      char n[4];
      std::snprintf(n, sizeof n, "%d", i + 1);
      if (!inUse) {  // past LENGTH: skipped
        framed(p, kDisabledEdge, kPaper, 10);
        label({p.l, p.t + 34, p.r, p.b - 8}, 22, kDisabledText, n, true, 1);
        continue;
      }
      s.fillRect(p.l + 2, p.t + 4, p.r + 2, p.b + 4, 0xcfc3b2, 10);  // pad shadow
      framed(p, hot(p) ? kTeal : kRule, plays ? kWhite : kAltCard, 10);
      if (hot(p)) framed({p.l + 1, p.t + 1, p.r - 1, p.b - 1}, kTeal, plays ? kWhite : kAltCard, 9);
      led((p.l + p.r) / 2, p.t + 20, 7, plays);
      label({p.l, p.t + 34, p.r, p.b - 8}, 22, plays ? kInk : kMuted, n, true, 1);
    }
    label({1136, 1350, 1946, 1374}, 14, kMuted,
          arp ? "Lit steps let the arpeggiator clock through; dark steps are silent beats."
              : "Lit steps let the sequencer clock through; dark steps are silent beats.");
    label({1136, 1374, 1946, 1398}, 14, kMuted, "Steps past LENGTH are skipped. Click a pad to switch it.");
    label({454, 1378, 1080, 1402}, 14, kMuted,
          arp ? "VARIATION repeats the pattern, transposed up by INTERVAL." : "Notes and gates of the 16 steps: SEQ STEPS tab.");
  }
  if (st.tab == kPlay) {
    label({454, 1374, 864, 1398}, 13, kMuted, "TWIN / SPLIT: plates C-F play left, F#-B right.");
    label({1720, 1312, 1946, 1334}, 13, kMuted, "Panel 5-step sequencer: EXT");
    label({1720, 1332, 1946, 1354}, 13, kMuted, "waits for its EXT CLOCK jack.");
    label({1720, 1366, 1946, 1388}, 13, kMuted, "Keyboard CLOCK jack overrides");
    label({1720, 1386, 1946, 1408}, 13, kMuted, "TEMPO until TEMPO is moved.");
  }
  if (st.tab == kExpression) {
    label({612, 1312, 744, 1332}, 13, kMuted, "ON: glide only when");
    label({612, 1330, 744, 1350}, 13, kMuted, "plates overlap.");
    label({790, 1384, 1346, 1406}, 13, kMuted, "PRESSURE: how much plate pressure scales the depth (0 = off).");
    const int mode = optionIndex(P::keyboard_pressure_output, st.value(P::keyboard_pressure_output));
    static const char* l1[] = {"PRESSURE", "ASR ENVELOPE", "AD ENVELOPE", "LOOPING AD", "RANDOM"};
    static const char* l2[] = {"Follows the plate.", "Attack, sustain, release.", "Attack, decay.", "AD envelope, repeating.",
                               "New level per press."};
    static const char* l3[] = {"RISE / FALL smooth", "RISE = attack time", "RISE = attack time", "RISE = attack time",
                               "RISE / FALL smooth"};
    static const char* l4[] = {"the voltage.", "FALL = release time", "FALL = decay time", "FALL = decay time", "the voltage."};
    const char* lines[] = {l1[mode], l2[mode], l3[mode], l4[mode]};
    for (int i = 0; i < 4; ++i)
      label({1712, 1290.f + 22.f * float(i), 1950, 1312.f + 22.f * float(i)}, 13, i == 0 ? kInk : kMuted, lines[i], i == 0);
  }
  if (st.tab == kSteps) {
    const int len = countValue(P::keyboard_seq_length, st.value(P::keyboard_seq_length));
    for (int n : {0, 12, 24}) {
      const float y = kFaderBottom - (kFaderBottom - kFaderTop) * float(n) / float(kSeqMaxNote);
      s.fillRect(500, y - 0.5f, 1950, y + 0.5f, kRule, 0);
      char t[8];
      std::snprintf(t, sizeof t, n != 0 ? "+%d" : "0", n);
      label({446, y - 10, 494, y + 10}, 13, kMuted, t, true, 2);
    }
    label({446, 1194, 506, 1214}, 13, kMuted, "STEP", true);
    label({446, kGateT, 506, kGateB}, 13, kMuted, "GATE", true);
    for (int i = 0; i < kSeqSteps; ++i) {
      const Box col = stepColumn(i);
      const float cx = (col.l + col.r) / 2;
      const bool used = i < len, over = hot(col);
      const SeqStep& q = st.steps[i];
      char t[8];
      std::snprintf(t, sizeof t, "%d", i + 1);
      label({col.l, 1194, col.r, 1214}, 15, used ? (over ? kTeal : kInk) : kDisabledText, t, true, 1);
      const float y = kFaderBottom - (kFaderBottom - kFaderTop) * float(std::clamp(q.note, 0, kSeqMaxNote)) / float(kSeqMaxNote);
      s.fillRect(cx - 4, kFaderTop - 6, cx + 4, kFaderBottom + 6, used ? 0x4a443d : kDisabledEdge, 4);  // slot
      if (q.gate && used) s.fillRect(cx - 4, y, cx + 4, kFaderBottom + 6, kTeal, 4);                  // level
      const Box cap{cx - 26, y - 12, cx + 26, y + 12};
      if (used) {
        s.fillRect(cap.l + 1, cap.t + 3, cap.r + 1, cap.b + 3, 0xb8ac9a, 4);
        rect(cap, over ? 0x3d3833 : kInk, 4);
      } else {
        framed(cap, kDisabledEdge, kPaper, 4);
      }
      char nt[8];
      std::snprintf(nt, sizeof nt, q.note != 0 ? "+%d" : "0", q.note);
      label(cap, 14, used ? kWhite : kDisabledText, nt, true, 1);
      const Box g = stepGate(i);
      if (used) {
        framed(g, kRule, hot(g) ? kHover : (q.gate ? kWhite : kAltCard), 6);
        led(cx, (g.t + g.b) / 2, 6, q.gate);
      } else {  // past LENGTH, still editable: a dim dot shows the gate
        framed(g, kDisabledEdge, kPaper, 6);
        if (q.gate) s.fillCircle(cx, (g.t + g.b) / 2, 5, kDisabledText);
      }
    }
  }
  if (st.tab == kService) {
    rect({434, 1184, 1966, 1220}, kCard, 8);
    rect({434, 1184, 440, 1220}, kRed, 2);
    label({456, 1184, 810, 1220}, 15, kInk, "HARDWARE CALIBRATION  (manual p.20)", true);
    label({820, 1184, 1950, 1220}, 14, kMuted,
          "Lunar 24 saves these values but does not use them yet: changing them has no effect on the sound.");
  }

  // Footer: hint, presets, RESET PANEL.
  rect({434, kFooterRule, 1966, kFooterRule + 2}, kRule, 0);
  static const char* hints[kTabCount] = {
      "Drag knobs up / down (Shift = fine), wheel, double-click = default.",
      "Drag knobs up / down (Shift = fine), wheel, double-click = default.",
      "Hold plates with MODE = ARPEGGIATOR (PLAY tab).",
      "Hold a plate with MODE = SEQUENCER (PLAY tab).",
      "",
      "Mirrors the Solar 42N calibration menu (stored only).",
  };
  if (st.tab == kSteps) {
    label({434, 1430, 1000, 1452}, 13, kMuted, "Drag a fader: note above the held plate (double-click = 0).");
    label({434, 1451, 1000, 1473}, 13, kMuted, "Dim steps are past LENGTH (SEQ tab); GATE off = a rest.");
  } else {
    label({434, 1434, 1000, 1470}, 14, kMuted, hints[std::clamp(st.tab, 0, kTabCount - 1)]);
  }
  label({1010, 1434, 1092, 1470}, 14, kMuted, "PRESET", true, 2);
  segmented(kPresetSlots, 4, st.presetSlot, false, [](int i) { return std::string(1, char('A' + i)); });
  button(kPresetLoad, st.presetDone == 0 ? "LOADED" : "LOAD", true, st.presetDone == 0 ? kTeal : 0, 15);
  button(kPresetSave, st.presetDone == 1 ? "SAVED" : "SAVE", true, st.presetDone == 1 ? kTeal : 0, 15);
  button(kPresetInit, st.presetDone == 2 ? "CLEARED" : st.initArmed ? "SURE?" : "INIT", true,
         st.presetDone == 2 ? kTeal : st.initArmed ? kRed : 0, 15);
  rect({1700, 1434, 1702, 1470}, kRule, 0);
  if (st.resetArmed) {
    button(kReset, "CLICK TO CONFIRM", true, kRed, 15);
  } else {
    framed(kReset, kRed, hot(kReset) ? 0xf3dcd6 : kWhite);
    label(kReset, 15, kArmedText, "RESET PANEL", true, 1);
  }
}

}  // namespace lunar24::host::kb_ui
