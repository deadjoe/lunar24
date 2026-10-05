// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The panel layout: every panel parameter has exactly one control, every jack on the
// official panel has exactly one socket, controls stay on the panel and do not overlap,
// and the keyboard menu's tabs keep each setting once, inside its card, without overlaps,
// with clicks landing on the control drawn there. The static panel art (panel_art.h) draws
// with every path inside the panel.

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "mini_test.h"
#include <host/panel_art.h>
#include <host/keyboard_menu_view.h>
#include <host/midi_settings_view.h>
#include <host/panel_ui_layout.h>
#include <lunar24/core/keyboard_behaviour.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/state_disposition.h>

using namespace lunar24;
using host::Widget;
using host::WidgetKind;

namespace {
bool overlaps(const Widget& a, const Widget& b) {
  return a.x() < b.x() + b.w && b.x() < a.x() + a.w && a.y() < b.y() + b.h && b.y() < a.y() + a.h;
}
bool onPanel(const Widget& w) {
  return w.x() >= 0 && w.y() >= 0 && w.x() + w.w <= 2400 && w.y() + w.h <= 1552;
}
bool inMenu(const Widget& w) {
  return w.x() >= host::kMenuX0 && w.y() >= host::kMenuY0 && w.x() + w.w <= host::kMenuX1 &&
         w.y() + w.h + 30 <= host::kMenuY1;  // + label line
}
bool notOnPanel(std::uint32_t jack) {
  for (auto j : host::kJacksNotOnPanel)
    if (static_cast<std::uint32_t>(j) == jack) return true;
  return false;
}
// Counts what the panel art draws and flags points off the panel or unbalanced paths.
struct CheckSink {
  int paths = 0, texts = 0, bad = 0;
  bool open = false;
  void pt(float x, float y) { if (x < -1 || y < -1 || x > 2401 || y > 1553) ++bad; }
  void fillRect(float x0, float y0, float x1, float y1, std::uint32_t, float) { pt(x0, y0); pt(x1, y1); }
  void fillCircle(float cx, float cy, float, std::uint32_t) { pt(cx, cy); }
  void moveTo(float x, float y) { pt(x, y); open = true; }
  void lineTo(float x, float y) { pt(x, y); if (!open) ++bad; }
  void closePath() {}
  void markHole() { if (!open) ++bad; }
  void fillPath(std::uint32_t, bool) { ++paths; open = false; }
  void strokePath(std::uint32_t, float) { ++paths; open = false; }
  void text(float x, float y, float, std::uint32_t, bool, const char*) { pt(x, y); ++texts; }
  void circle(float cx, float cy, float) { pt(cx, cy); open = true; }
  void fillGrad(const host::art::Grad&) { if (!open) ++bad; ++paths; open = false; }
  void strokeGrad(const host::art::Grad&, float) { if (!open) ++bad; ++paths; open = false; }
};
}  // namespace

int main() {
  const std::vector<Widget> ws = host::build_panel_layout();
  // The plugins' panel: the hardware's PHONE knob where the app has REC and its source selector,
  // everything else the same.
  {
    const std::vector<Widget> plug = host::build_panel_layout(false);
    CHECK_EQ(plug.size() + 1, ws.size());
    int rec = 0, phone = 0;
    for (const Widget& w : plug) {
      if (w.kind == WidgetKind::Record || w.kind == WidgetKind::RecordSource) ++rec;
      if (w.kind == WidgetKind::Knob && w.id == static_cast<std::uint32_t>(core::ParameterId::effector_phone)) ++phone;
    }
    CHECK_EQ(rec, 0);
    CHECK_EQ(phone, 1);
  }

  std::map<std::uint32_t, int> params, jacks;
  for (const Widget& w : ws) {
    if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::Button || w.kind == WidgetKind::Toggle) ++params[w.id];
    if (w.kind == WidgetKind::Jack) ++jacks[w.id];
    if (w.kind == WidgetKind::Joystick) { ++params[w.id]; ++params[w.id2]; }
  }
  int missingP = 0, dupP = 0;
  for (const auto& p : registry::kParameters) {
    if (p.owner.substr(0, 8) == "program.") continue;  // per-program labels, not panel controls
    if (p.id == core::ParameterId::effector_phone) continue;  // its place holds REC's source selector
    const int n = params[static_cast<std::uint32_t>(p.id)];
    if (n == 0) { ++missingP; std::printf("  no control for %s\n", std::string(p.stable_id).c_str()); }
    if (n > 1) { ++dupP; std::printf("  %d controls for %s\n", n, std::string(p.stable_id).c_str()); }
  }
  CHECK_EQ(missingP, 0);
  CHECK_EQ(dupP, 0);

  int missingJ = 0, dupJ = 0;
  for (const auto& j : registry::kJacks) {
    const auto id = static_cast<std::uint32_t>(j.id);
    const int n = jacks[id];
    if (notOnPanel(id)) { CHECK_EQ(n, 0); continue; }
    if (n == 0) { ++missingJ; std::printf("  no socket for %s\n", std::string(j.stable_id).c_str()); }
    if (n > 1) ++dupJ;
  }
  CHECK_EQ(missingJ, 0);
  CHECK_EQ(dupJ, 0);

  int off = 0, clash = 0, menuOut = 0;
  for (std::size_t i = 0; i < ws.size(); ++i) {
    const Widget& a = ws[i];
    if (!onPanel(a)) { ++off; std::printf("  off panel: kind %d id %u\n", int(a.kind), a.id); }
    if (a.menu && !inMenu(a)) { ++menuOut; std::printf("  outside menu: %s\n", a.label.c_str()); }
    for (std::size_t k = i + 1; k < ws.size(); ++k) {
      const Widget& b = ws[k];
      if (a.menu || b.menu) continue;  // the menu covers the plates; its tabs are checked below
      if (overlaps(a, b)) {
        ++clash;
        std::printf("  overlap: kind %d id %u @(%.0f,%.0f) / kind %d id %u @(%.0f,%.0f)\n", int(a.kind), a.id, a.cx, a.cy,
                    int(b.kind), b.id, b.cx, b.cy);
      }
    }
  }
  CHECK_EQ(off, 0);
  CHECK_EQ(menuOut, 0);
  CHECK_EQ(clash, 0);

  CheckSink art;
  host::art::drawPanelArt(art);
  CHECK_EQ(art.bad, 0);
  CHECK(art.paths > 500);   // frames, printed marks, name plates
  CHECK(art.texts > 300);   // panel labels
  // The app-level MUTE / MIDI pair sits right of DRONE VOICES as a vertical pair whose
  // centre is the six-key block's centre (y = 1296.5), wide enough apart for the labels.
  {
    const Widget* mute = nullptr;
    const Widget* midi = nullptr;
    for (const Widget& w : ws) {
      if (w.kind == WidgetKind::MasterMute) mute = &w;
      if (w.kind == WidgetKind::MidiSettings) midi = &w;
    }
    CHECK(mute != nullptr);
    CHECK(midi != nullptr);
    if (mute != nullptr && midi != nullptr) {
      CHECK(std::fabs((mute->cy + midi->cy) / 2 - 1296.5) < 1.0);
      CHECK(std::fabs(mute->cx - midi->cx) < 1.0);
      CHECK(midi->cy - mute->cy >= 100.0);  // the label under MUTE stays clear of MIDI
    }
  }
  // Every control, drawn at its place: all paths are started before they are painted.
  CheckSink ctl;
  for (const Widget& w : ws) {
    if (w.menu) continue;
    const float cx = float(w.cx), cy = float(w.cy);
    if (w.kind == WidgetKind::Knob) host::art::drawKnob(ctl, cx, cy, float(w.w / 2), 0x006080, w.cap != host::Cap::Black, 30.f, 0x333333, -150.f, 150.f, true);
    if (w.kind == WidgetKind::Button) host::art::drawButton(ctl, cx, cy, float(w.w / 2), true, true);
    if (w.kind == WidgetKind::Toggle) host::art::drawToggle(ctl, cx, cy, 0.f, true);
    if (w.kind == WidgetKind::Jack) host::art::drawJack(ctl, cx, cy, float(w.w / 2), true);
    if (w.kind == WidgetKind::Joystick) host::art::drawJoystick(ctl, cx, cy, 55.f, cx + 90.f, cy - 90.f, true);
  }
  CHECK_EQ(ctl.bad, 0);
  CHECK(ctl.paths > 1000);
  for (const auto& sh : host::art::kDecorShapes)
    CHECK(sh.firstSub + sh.subCount <= sizeof(host::art::kDecorSubPaths) / sizeof(host::art::kDecorSubPaths[0]));
  for (const auto& sp : host::art::kDecorSubPaths)
    CHECK(2 * (sp.first + sp.count) <= sizeof(host::art::kDecorPoints) / sizeof(float));
  for (const auto& sp : host::art::kLogoSubPaths)
    CHECK(2 * (sp.first + sp.count) <= sizeof(host::art::kLogoPoints) / sizeof(float));
    // Every lit indicator LED sits exactly on a printed LED, each on a different one, and its order
  // matches the engine's PanelLed list (20 LEDs; the joystick's two are not driven).
  {
    std::map<int, int> used;
    const int n = int(sizeof(host::kPanelLedPos) / sizeof(host::kPanelLedPos[0]));
    CHECK_EQ(n, 20);
    for (int i = 0; i < n; ++i) {
      int hit = -1;
      for (int k = 0; k < int(sizeof(host::art::kLeds) / sizeof(host::art::kLeds[0])); ++k) {
        const auto& l = host::art::kLeds[k];
        if (std::fabs(l.x - host::kPanelLedPos[i].x) < 1.0 && std::fabs(l.y - host::kPanelLedPos[i].y) < 1.0) hit = k;
      }
      CHECK(hit >= 0);
      CHECK(used.count(hit) == 0);
      used[hit] = i;
    }
    CHECK(host::art::litLed(0xff2308u, 1.f) == 0xff2308u);
    CHECK(host::art::litLed(0xff2308u, 0.f) == host::art::unlit(0xff2308u));
  }
  {  // TWIN / SPLIT: C..F are the left side, F#..B the right, in every octave.
    for (int s = 0; s < 6; ++s) CHECK(!host::plate_is_right_side(s));
    for (int s = 6; s < 12; ++s) CHECK(host::plate_is_right_side(s));
    CHECK(!host::plate_is_right_side(12) && host::plate_is_right_side(18));
    CHECK(host::plate_is_right_side(-1) && !host::plate_is_right_side(-12));
  }
  { // MIDI: deleting the last row must return to a populated page.
    using namespace host::midi_ui;
    CHECK_EQ(pageOffset(4, 5), 4);
    CHECK_EQ(pageOffset(4, 4), 0);
    CHECK_EQ(pageOffset(-4, 0), 0);
    CHECK_EQ(pageOffset(128, 128), 124);
    for (int i=0; i<kSettingCount; ++i) {
      CHECK(!decrement(i).contains(setting(i).l+20, setting(i).t+20));
      CHECK(!increment(i).contains(setting(i).l+20, setting(i).t+20));
      CHECK(decrement(i).r <= increment(i).l);
      if (i + 1 < kSettingCount) CHECK(setting(i).b < setting(i + 1).t);
    }
    CHECK(setting(kSettingCount - 1).b < kLearn.t);  // the rows clear LEARN
    CHECK(kLearn.b < 1412);                          // and LEARN clears the footer rule
    CHECK_EQ(noteName(60), std::string("C4"));
    CHECK_EQ(noteName(24), std::string("C1"));
    CHECK_EQ(noteName(61), std::string("C#4"));
    // A truncated UTF-8 name must end on a character boundary.
    auto bytes = [](const char* text) { return static_cast<float>(std::string(text).size()); };
    CHECK_EQ(fitText("ab\xc3\xa9" "cdef", 6, bytes), std::string("ab..."));
    CHECK_EQ(fitText("short", 20, bytes), std::string("short"));
    // Non-ASCII device names: one '?' per code point, ASCII untouched.
    CHECK_EQ(asciiText("MPK mini IV"), std::string("MPK mini IV"));
    CHECK_EQ(asciiText("Caf\xc3\xa9 \xe9\x94\xae\xe7\x9b\x98 \xf0\x9f\x8e\xb9!"), std::string("Caf? ?? ?!"));
    CHECK_EQ(asciiText("a\xe9\x94"), std::string("a?"));       // truncated sequence
    CHECK_EQ(asciiText("\x80\x80" "b"), std::string("??b"));   // one per stray byte
    CHECK_EQ(asciiText(""), std::string());
  }
  { // Keyboard menu: each of the 36 settings on exactly one tab, inside a card of that tab,
    // nothing overlapping on a tab (names included), clicks land on what is drawn there.
    namespace kb = host::kb_ui;
    using kb::Box;
    const auto inside = [](Box a, Box o) { return a.l >= o.l && a.t >= o.t && a.r <= o.r && a.b <= o.b; };
    const auto overlap = [](Box a, Box b) { return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b; };
    const auto centre = [](Box b, float& x, float& y) { x = (b.l + b.r) / 2; y = (b.t + b.b) / 2; };
    const auto extent = [](const kb::Item& it) {  // the box plus the name drawn above it
      Box b = it.box;
      if (it.kind != kb::Kind::Knob && it.kind != kb::Kind::Trimmer && it.label[0] && !kb::inHeader(it)) b.t -= 24;
      return b;
    };
    CHECK(host::kMenuX0 == kb::kBounds.l && host::kMenuY0 == kb::kBounds.t && host::kMenuX1 == kb::kBounds.r &&
          host::kMenuY1 == kb::kBounds.b);
    CHECK_EQ(kb::kItemCount, 36);
    // Both overlays cover the same area: a cable dropped there while either is open must not
    // reach a keyboard jack hidden under it; closed, every jack takes cables as before.
    CHECK(host::kMenuX0 == host::midi_ui::kBounds.l && host::kMenuY0 == host::midi_ui::kBounds.t &&
          host::kMenuX1 == host::midi_ui::kBounds.r && host::kMenuY1 == host::midi_ui::kBounds.b);
    int hiddenJacks = 0;
    for (const Widget& w : ws) {
      if (w.kind != WidgetKind::Jack) continue;
      const bool covered = host::overlay_hides_point(true, w.cx, w.cy);
      if (covered) ++hiddenJacks;
      CHECK(!host::overlay_hides_point(false, w.cx, w.cy));
    }
    CHECK_EQ(hiddenJacks, 6);  // keyboard CLOCK, RESET, GATE L/R, PRESSURE, V/OCT
    // Encoder wheel: one mouse notch = one octave; a whole trackpad swipe (many small deltas,
    // zero deltas, momentum after lifting) = one octave; SERVICE > ENCODER DIRECTION flips it.
    {
      host::EncoderWheel w;
      CHECK_EQ(w.step(1.0, 1.0, 0.0), 1);    // a notch up
      CHECK_EQ(w.step(-1.0, 1.0, 0.0), -1);  // a later notch down
      CHECK_EQ(w.step(1.0, 1.0, 1.0), -1);   // reversed
      CHECK_EQ(w.step(-1.0, 1.0, 1.0), 1);
      CHECK_EQ(w.step(0.0, 1.0, 0.0), 0);    // zero delta (sideways swipe, gesture end)
      int total = 0;
      for (int i = 0; i < 60; ++i) total += w.step(i < 40 ? 0.3 : 0.05, 0.016, 0.0);  // swipe + momentum
      total += w.step(0.0, 0.016, 0.0);
      CHECK_EQ(total, 1);
      CHECK_EQ(w.step(0.3, 0.016, 0.0), 0);  // still the same gesture
      CHECK_EQ(w.step(-0.6, 0.5, 0.0), -1);  // a new swipe after a pause
      CHECK_EQ(w.step(0.2, 0.5, 0.0), 0);    // a tiny brush stays below the threshold
    }
    std::map<std::uint32_t, int> onTabs;
    for (const auto& it : kb::kItems) ++onTabs[static_cast<std::uint32_t>(it.id)];
    int menuWidgets = 0;
    for (const Widget& w : ws)
      if (w.menu) {
        ++menuWidgets;
        CHECK_EQ(onTabs[w.id], 1);
      }
    CHECK_EQ(menuWidgets, kb::kItemCount);
    const Box body{412, 1176, 1988, 1422};
    int bad = 0;
    for (int i = 0; i < kb::kItemCount; ++i) {
      const kb::Item& a = kb::kItems[i];
      bool inCard = false;
      for (const auto& c : kb::kCards) inCard = inCard || (c.tab == a.tab && inside(extent(a), c.box));
      if (!inside(extent(a), body) || !inCard) { ++bad; std::printf("  menu item outside its card: %s\n", a.label); }
      for (int k = i + 1; k < kb::kItemCount; ++k)
        if (kb::kItems[k].tab == a.tab && overlap(extent(a), extent(kb::kItems[k]))) {
          ++bad;
          std::printf("  menu items overlap: %s / %s\n", a.label, kb::kItems[k].label);
        }
      for (int p = 0; p < kb::kRhythmSteps; ++p)
        if ((a.tab == kb::kArp || a.tab == kb::kSeq) && overlap(kb::rhythmPad(p), extent(a))) ++bad;
      // Per-side settings sit in LEFT / RIGHT cards, global ones in GLOBAL cards (SPLIT tags).
      for (const auto& c : kb::kCards)
        if (c.tab == a.tab && c.tag != kb::Tag::None && inside(extent(a), c.box) &&
            (core::keyboard_scalar_index(a.id) >= 0) != (c.tag == kb::Tag::Side)) {
          ++bad;
          std::printf("  wrong side tag: %s in %s\n", a.label, c.title);
        }
      // A click at the centre of each part hits that part.
      float x, y;
      const auto hits = [&](Box b, int sub) {
        centre(b, x, y);
        const kb::Hit h = kb::hitTest(a.tab, true, x, y);
        return h.kind == kb::HitKind::Item && h.index == i && h.sub == sub;
      };
      switch (a.kind) {
        case kb::Kind::Segmented:
        case kb::Kind::VSegmented:
          for (int o = 0; o < kb::optionCount(a.id); ++o)
            if (!hits(kb::segment(a.box, kb::optionCount(a.id), o, a.kind == kb::Kind::VSegmented), o)) ++bad;
          break;
        case kb::Kind::Stepper:
          if (!hits(kb::stepDown(a.box), -1) || !hits(kb::stepUp(a.box), 1)) ++bad;
          break;
        case kb::Kind::RootKeys:
          for (int k = 0; k < 12; ++k) {
            Box key = kb::rootKey(a.box, k);
            if (key.b - key.t > 60) key.t = a.box.t + 56;  // white keys: below the black ones
            if (!hits(key, k)) ++bad;
          }
          break;
        default:
          if (a.id == core::ParameterId::keyboard_clock_bpm) {
            if (!hits(kb::stepDown(kb::tempoNudge(a)), -1) || !hits(kb::stepUp(kb::tempoNudge(a)), 1)) ++bad;
            const kb::Dial d = kb::dial(a);
            if (!hits({d.cx - 1, d.cy - 1, d.cx + 1, d.cy + 1}, 0)) ++bad;
          } else if (!hits(a.box, 0)) {
            ++bad;
          }
      }
    }
    CHECK_EQ(bad, 0);
    // Title bar, footer, pads and steps: inside the overlay, apart, and hit where drawn.
    float x, y;
    for (int i = 0; i < kb::kTabCount; ++i) {
      CHECK(inside(kb::tab(i), kb::kTitleBar));
      CHECK(!overlap(kb::tab(i), kb::kSide) && !overlap(kb::tab(i), kb::kClose));
      if (i > 0) CHECK(!overlap(kb::tab(i), kb::tab(i - 1)));
      centre(kb::tab(i), x, y);
      CHECK(kb::hitTest(kb::kPlay, false, x, y).kind == kb::HitKind::Tab && kb::hitTest(kb::kPlay, false, x, y).index == i);
    }
    CHECK(inside(kb::kSide, kb::kTitleBar) && !overlap(kb::kSide, kb::kClose));
    for (int i = 0; i < 2; ++i) {
      centre(kb::sideHalf(i), x, y);
      CHECK(kb::hitTest(kb::kPlay, true, x, y).kind == kb::HitKind::Side && kb::hitTest(kb::kPlay, true, x, y).index == i);
      CHECK(kb::hitTest(kb::kPlay, false, x, y).kind == kb::HitKind::None);  // no side switch outside SPLIT
    }
    const Box footer[] = {kb::kPresetSlots, kb::kPresetLoad, kb::kPresetSave, kb::kPresetInit, kb::kReset};
    const kb::HitKind footerKind[] = {kb::HitKind::Slot, kb::HitKind::Load, kb::HitKind::Save, kb::HitKind::Init,
                                      kb::HitKind::Reset};
    for (int i = 0; i < 5; ++i) {
      CHECK(inside(footer[i], kb::kBounds) && footer[i].t > kb::kFooterRule);
      for (int k = i + 1; k < 5; ++k) CHECK(!overlap(footer[i], footer[k]));
      centre(i == 0 ? kb::segment(footer[0], 4, 0, false) : footer[i], x, y);
      for (int t = 0; t < kb::kTabCount; ++t) CHECK(kb::hitTest(t, true, x, y).kind == footerKind[i]);
    }
    for (int i = 0; i < 4; ++i) {
      centre(kb::segment(kb::kPresetSlots, 4, i, false), x, y);
      CHECK(kb::hitTest(kb::kPlay, false, x, y).index == i);
    }
    // SCALE EDITOR notes: inside the QUANTISER card, clear of SCALE and ROOT, hit where drawn,
    // and only on the PLAY tab.
    for (int n = 0; n < 12; ++n) {
      const Box b = kb::scaleNote(n);
      CHECK(inside(b, kb::kCards[1].box));
      for (const auto& it : kb::kItems)
        if (it.tab == kb::kPlay) CHECK(!overlap(b, it.box));
      if (n > 0) CHECK(!overlap(b, kb::scaleNote(n - 1)));
      centre(b, x, y);
      CHECK(kb::hitTest(kb::kPlay, false, x, y).kind == kb::HitKind::Note && kb::hitTest(kb::kPlay, false, x, y).index == n);
      CHECK(kb::hitTest(kb::kArp, false, x, y).kind != kb::HitKind::Note);
    }
    centre(kb::kClose, x, y);
    CHECK(kb::hitTest(kb::kSteps, false, x, y).kind == kb::HitKind::Close);
    for (int i = 0; i < kb::kRhythmSteps; ++i) {
      const Box p = kb::rhythmPad(i);
      CHECK(inside(p, kb::kCards[7].box) && inside(p, kb::kCards[9].box));
      centre(p, x, y);
      CHECK(kb::hitTest(kb::kArp, false, x, y).kind == kb::HitKind::Pad && kb::hitTest(kb::kArp, false, x, y).index == i);
      CHECK(kb::hitTest(kb::kSeq, false, x, y).kind == kb::HitKind::Pad);
      CHECK(kb::hitTest(kb::kPlay, false, x, y).kind != kb::HitKind::Pad);
    }
    for (int i = 0; i < kb::kSeqSteps; ++i) {
      CHECK(inside(kb::stepColumn(i), kb::kCards[10].box) && inside(kb::stepGate(i), kb::stepColumn(i)));
      if (i > 0) CHECK(!overlap(kb::stepColumn(i), kb::stepColumn(i - 1)));
      centre(kb::stepGate(i), x, y);
      CHECK(kb::hitTest(kb::kSteps, false, x, y).kind == kb::HitKind::Gate && kb::hitTest(kb::kSteps, false, x, y).index == i);
      x = (kb::stepColumn(i).l + kb::stepColumn(i).r) / 2;
      CHECK(kb::hitTest(kb::kSteps, false, x, kb::kFaderTop).kind == kb::HitKind::Fader);
      CHECK(kb::hitTest(kb::kSteps, false, x, kb::kFaderTop).index == i);
    }
    CHECK_EQ(kb::noteAt(kb::kFaderBottom), 0);
    CHECK_EQ(kb::noteAt(kb::kFaderTop), 24);
    CHECK_EQ(kb::noteAt(kb::kFaderTop - 50), 24);
    // Values written by steppers, root keys and TEMPO decode back exactly in the core.
    for (const auto& c : kb::kCounts)
      for (int k = c.lo; k <= c.hi; ++k) CHECK_EQ(kb::countValue(c.id, kb::countNorm(c, k)), k);
    CHECK_EQ(int(core::arp_interval_semitones(kb::countNorm(*kb::countOf(core::ParameterId::keyboard_arp_interval), 7))), 7);
    for (int k = 0; k < 12; ++k) {
      CHECK_EQ(int(core::root_note_semitone(kb::rootNorm(k))), k);
      CHECK_EQ(kb::rootValue(kb::rootNorm(k)), k);
    }
    for (int bpm = 10; bpm <= 300; ++bpm) {
      CHECK_EQ(kb::bpmValue(kb::bpmNorm(bpm)), bpm);
      CHECK_EQ(host::formatParam(static_cast<std::uint32_t>(core::ParameterId::keyboard_clock_bpm), kb::bpmNorm(bpm)),
               std::to_string(bpm) + " BPM");
    }
    CHECK_EQ(kb::bpmValue(kb::bpmNorm(301)), 300);
    CHECK_EQ(kb::optionText(core::ParameterId::keyboard_mode, 1), std::string("ARPEGGIATOR"));  // full names
    CHECK_EQ(kb::optionText(core::ParameterId::keyboard_arp_variation, 1), std::string("x1"));
  }
  { // MIDI learn targets: the 202 panel controls, never a keyboard menu setting.
    int targets = 0;
    for (const Widget& w : ws) {
      if (w.menu) continue;
      switch (w.kind) {
        case WidgetKind::Knob:
        case WidgetKind::Button:
        case WidgetKind::Toggle:
        case WidgetKind::DroneKey:
        case WidgetKind::MasterMute:
        case WidgetKind::Cartridge: ++targets; break;
        default: break;
      }
    }
    CHECK_EQ(targets, 202);  // 203 before the PHONE knob gave its place to REC
  }
  return test::finish("test_panel_ui_layout");
}
