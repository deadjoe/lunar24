// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_editor.h — the Lunar 24 panel, drawn with iPlug2 IGraphics.
//
// The panel follows the official Solar 42N panel drawing: the static artwork (labels, frames,
// printed marks, name plates) and the keyboard-area controls come from panel_art.h, control
// positions from panel_ui_layout.h, colours from panel_theme.h. Controls do not use iPlug parameters: they read the machine state
// from the engine and send changes through its live-control queue
// (StandaloneAudioEngine::postParameter / postConnect / postEvent ...), so the saved state
// always matches what the user sees and hears.
//
// Playing: click the touch plates (lower on a plate = more pressure), or use the computer
// keyboard like a piano (A W S E D F T G Y H U J K O L P ; — Z / X or the arrow keys on the
// panel shift the octave). The encoder opens the KEYBOARD MENU with the keyboard settings.
// Patching: drag from any jack to another; drag a cable off an input to unplug it.
// Knobs: drag up/down (Shift = fine), mouse wheel, double-click = factory value.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "IControl.h"
#include "IGraphics.h"

#include <host/panel_art.h>
#include <host/ui_font.generated.h>
#include <host/panel_format.h>
#include <host/panel_theme.h>
#include <host/panel_ui_layout.h>
#include <host/standalone_audio_engine.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/state_edit.h>

namespace lunar24::host::ui {

using namespace iplug;
using namespace iplug::igraphics;
using core::JackId;
using core::ParameterId;

constexpr const char* kFont = "lunar-regular";
constexpr const char* kFontBold = "lunar-bold";
constexpr float kPi = 3.14159265f;

inline IColor col(theme::Rgb c, int alpha = 255) { return IColor(alpha, c.r, c.g, c.b); }
inline IText txt(float size, theme::Rgb c, bool bold = true, float angle = 0.f) {
  return IText(size, col(c), bold ? kFontBold : kFont, EAlign::Center, EVAlign::Middle, angle);
}
inline IRECT rectOf(const Widget& w) {
  return IRECT(float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h));
}
// Controls draw a little beyond their hit box (scale ticks, drop shadows): the drawing area
// is padded, the mouse target stays the hit box.
// Menu overlay controls also draw their name and value below (at cy+42 and cy+58), so their
// drawing area reaches down past that text; otherwise the names were clipped in half and the
// values hidden.
inline IRECT drawRectOf(const Widget& w) {
  const float pad = std::max(14.f, float(std::max(w.w, w.h)) * 0.3f);
  IRECT r = rectOf(w).GetPadded(pad);
  if (w.menu) r = r.Union(IRECT(float(w.cx - 64), float(w.cy), float(w.cx + 64), float(w.cy + 70)));
  return r;
}
inline std::uint32_t hexOf(theme::Rgb c) { return (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b; }
// Point at `r` from (cx, cy) in direction `deg` (0 = 12 o'clock, clockwise).
inline void polar(float cx, float cy, float r, float deg, float& x, float& y) {
  const float t = (deg - 90.f) * kPi / 180.f;
  x = cx + r * std::cos(t);
  y = cy + r * std::sin(t);
}

inline const core::ParameterDescriptor* desc(std::uint32_t id) {
  return core::find_parameter(static_cast<ParameterId>(id));
}
inline int positionsOf(const core::ParameterDescriptor* d) {
  if (d == nullptr) return 2;
  if (d->optionCount > 0) return int(d->optionCount);
  return std::max(2, int(std::lround((d->max - d->min) / (d->step > 0 ? d->step : 1.0))) + 1);
}
inline std::string upper(std::string s) {
  for (auto& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

class CableLayer;
class JackControl;

// Shared by every control of one editor instance.
struct EditorShared {
  explicit EditorShared(StandaloneAudioEngine& e) : engine(e) {}
  StandaloneAudioEngine& engine;
  std::vector<JackControl*> jacks;
  std::map<std::uint32_t, IRECT> jackRects;  // JackId -> socket rect (for cable drawing)
  CableLayer* cables = nullptr;
  std::vector<IControl*> menuControls;       // SETTINGS page of the keyboard menu
  std::vector<IControl*> seqControls;        // SEQUENCER page (16-step editor)
  std::vector<IControl*> rhythmControls;     // RHYTHM page (arp / seq step patterns)
  std::vector<IControl*> menuChrome;         // background and page tabs: whenever the menu is open
  int menuPage = 0;                          // 0 = SETTINGS, 1 = SEQUENCER, 2 = RHYTHM
  int seqSide = 0;                           // menu side being edited under SPLIT: 0 = left, 1 = right
  int presetSlot = 0;                        // keyboard preset the LOAD / SAVE / INIT buttons act on: 0..3 = A..D
  std::function<void()> factoryReset;        // the plugin's requestFactoryReset (RESET PANEL)
  std::vector<IControl*> plates;
  bool menuOpen = false;
  int octave = 0;
  std::set<int> lit;                         // plates currently sounding (0..11)
  std::map<int, int> heldKeys;               // computer key semitone -> sounding semitone
  // Value readout for the knob under the mouse (drawn by the top layer).
  std::string readout;
  float readoutX = 0, readoutY = 0;
  core::InputStateMachine input{nullptr, 0};
  std::uint64_t seq = 0;
  std::uint64_t seenStateVersion = ~0ull;
  bool seenReady = false;

  static constexpr core::NoteId kMouseId = 1000;
  static constexpr core::NoteId kKeyIdBase = 2000;
  static constexpr core::ControlSourceId kMouseSource = 1;
  static constexpr core::ControlSourceId kKeySource = 2;

  const core::DeviceStateV1* state() const { return engine.canonicalState(); }
  // Under PLAY = SPLIT the keyboard menu edits either side (EDIT: LEFT / RIGHT); a per-side
  // keyboard setting then reads and writes that side's bank. Everything else has one value.
  bool split() const { return engine.parameterValue(ParameterId::keyboard_behaviour) > 1.5; }
  int editSide() const { return split() ? seqSide : 0; }
  double value(std::uint32_t id) const {
    const std::int32_t idx = core::keyboard_scalar_index(static_cast<ParameterId>(id));
    const core::DeviceStateV1* st = state();
    if (idx >= 0 && editSide() == 1 && st != nullptr) return st->keyboardScalarRight[static_cast<std::size_t>(idx)];
    return engine.parameterValue(static_cast<ParameterId>(id));
  }
  void set(std::uint32_t id, double v) {
    if (editSide() == 1 && engine.postKeyboardRightParameter(static_cast<ParameterId>(id), v)) return;
    engine.postParameter(static_cast<ParameterId>(id), v);
  }
  int index(std::uint32_t id) const {
    const core::ParameterDescriptor* d = desc(id);
    if (d == nullptr) return 0;
    return int(std::lround((value(id) - d->min) / (d->step > 0 ? d->step : 1.0)));
  }
  void setIndex(std::uint32_t id, int idx) {
    const core::ParameterDescriptor* d = desc(id);
    if (d == nullptr) return;
    idx = std::clamp(idx, 0, positionsOf(d) - 1);
    set(id, d->min + idx * (d->step > 0 ? d->step : 1.0));
  }

  // Keyboard notes (plates, computer keys): through the same input state machine as MIDI.
  // `plate` (0..11, the plate pressed) picks the keyboard side under TWIN / SPLIT.
  static core::KeyboardSide sideOf(int plate) {
    return plate_is_right_side(plate) ? core::KeyboardSide::Right : core::KeyboardSide::Left;
  }
  void note(bool on, int semitoneFromC3, core::NoteId id, double pressure, core::ControlSourceId source, int plate) {
    core::PerformanceInput in{};
    in.side = sideOf(plate);
    in.kind = on ? core::PerfInputKind::note_on : core::PerfInputKind::note_off;
    in.pitch = static_cast<core::SignalSample>((semitoneFromC3 - 9) / 12.0);  // A3 = 0 V = 220 Hz
    in.value = static_cast<core::SignalSample>(pressure);
    in.noteId = id;
    in.source = source;
    in.seq = ++seq;
    core::ControlEvent ev[3];
    const std::uint32_t n = input.translate(in, ev, 3);
    for (std::uint32_t i = 0; i < n; ++i) engine.postEvent(ev[i]);
  }
  void pressure(core::NoteId id, double p, core::ControlSourceId source, int plate) {
    core::PerformanceInput in{};
    in.side = sideOf(plate);
    in.kind = core::PerfInputKind::aftertouch;
    in.value = static_cast<core::SignalSample>(p);
    in.noteId = id;
    in.source = source;
    in.seq = ++seq;
    core::ControlEvent ev[1];
    if (input.translate(in, ev, 1) == 1) engine.postEvent(ev[0]);
  }
  void shiftOctave(int d) { octave = std::clamp(octave + d, -3, 3); }
  void showMenu(bool open) {
    menuOpen = open;
    for (IControl* c : menuChrome) c->Hide(!open);
    for (IControl* c : menuControls) c->Hide(!(open && menuPage == 0));
    for (IControl* c : seqControls) c->Hide(!(open && menuPage == 1));
    for (IControl* c : rhythmControls) c->Hide(!(open && menuPage == 2));
  }
  const core::KeyboardSeqStep* seqStep(int i) const {
    const core::DeviceStateV1* st = state();
    if (st == nullptr || i < 0 || i >= kSeqSteps) return nullptr;
    return &(editSide() == 0 ? st->keyboardSeqCurrent : st->keyboardSeqCurrentR).steps[static_cast<std::size_t>(i)];
  }

  // Computer keyboard: returns true if the key was used.
  bool key(const IKeyPress& k, bool up) {
    static const char kKeys[] = "awsedftgyhujkolp;";
    if (k.VK == kVK_ESCAPE && menuOpen) {
      if (!up) showMenu(false);
      return true;
    }
    const char c = char(std::tolower(static_cast<unsigned char>(k.utf8[0])));
    if (!up && (c == 'z' || c == 'x')) {
      shiftOctave(c == 'x' ? 1 : -1);
      return true;
    }
    const char* p = c ? std::strchr(kKeys, c) : nullptr;
    if (p == nullptr) return false;
    const int semi = int(p - kKeys);
    const core::NoteId id = static_cast<core::NoteId>(kKeyIdBase + static_cast<core::NoteId>(semi));
    if (!up && heldKeys.count(semi) == 0) {
      heldKeys[semi] = semi + 12 * octave;
      note(true, heldKeys[semi], id, 0.8, kKeySource, semi);
      lit.insert(semi % 12);
    } else if (up && heldKeys.count(semi) > 0) {
      note(false, heldKeys[semi], id, 0.0, kKeySource, semi);
      heldKeys.erase(semi);
      lit.erase(semi % 12);
    }
    return true;
  }
};

// ---------------------------------------------------------------------------------------------
// Draws host/panel_art.h artwork with IGraphics.
struct GraphicsSink {
  IGraphics& g;
  static IColor hex(std::uint32_t c) { return IColor(255, int((c >> 16) & 0xff), int((c >> 8) & 0xff), int(c & 0xff)); }
  void fillRect(float x0, float y0, float x1, float y1, std::uint32_t c, float radius) {
    if (radius > 0.f) g.FillRoundRect(hex(c), IRECT(x0, y0, x1, y1), radius);
    else g.FillRect(hex(c), IRECT(x0, y0, x1, y1));
  }
  void fillCircle(float cx, float cy, float r, std::uint32_t c) { g.FillCircle(hex(c), cx, cy, r); }
  void moveTo(float x, float y) {
    if (!open_) g.PathClear();
    open_ = true;
    g.PathMoveTo(x, y);
  }
  void lineTo(float x, float y) { g.PathLineTo(x, y); }
  void closePath() { g.PathClose(); }
  void markHole() { g.PathSetWinding(true); }
  void fillPath(std::uint32_t c, bool) {
    g.PathFill(IPattern(hex(c)), IFillOptions(false, EFillRule::Preserve));  // per-sub-path windings
    open_ = false;
  }
  void strokePath(std::uint32_t c, float width) {
    g.PathStroke(IPattern(hex(c)), width);
    open_ = false;
  }
  void text(float x, float y, float size, std::uint32_t c, bool vertical, const char* s) {
    g.DrawText(txt(size, theme::rgb(c), true, vertical ? -90.f : 0.f), s, x, y);
  }
  void circle(float cx, float cy, float r) {
    if (!open_) g.PathClear();
    open_ = true;
    g.PathCircle(cx, cy, r);
  }
  static IPattern pattern(const art::Grad& p) {
    auto c = [](std::uint32_t rgb, float a) {
      return IColor(int(std::lround(std::clamp(a, 0.f, 1.f) * 255.f)), int((rgb >> 16) & 0xff), int((rgb >> 8) & 0xff),
                    int(rgb & 0xff));
    };
    if (p.c0 == p.c1 && p.a0 == p.a1 && !p.radial) return IPattern(c(p.c0, p.a0));
    if (p.radial)
      return IPattern::CreateRadialGradient(p.x0, p.y0, p.y1,
                                            {IColorStop(c(p.c0, p.a0), p.y1 > 0 ? p.x1 / p.y1 : 0.f),
                                             IColorStop(c(p.c1, p.a1), 1.f)});
    return IPattern::CreateLinearGradient(p.x0, p.y0, p.x1, p.y1,
                                          {IColorStop(c(p.c0, p.a0), 0.f), IColorStop(c(p.c1, p.a1), 1.f)});
  }
  void fillGrad(const art::Grad& p) {
    g.PathFill(pattern(p), IFillOptions(false, EFillRule::Preserve));
    open_ = false;
  }
  void strokeGrad(const art::Grad& p, float width) {
    IStrokeOptions o;
    o.mCapOption = ELineCap::Round;
    o.mJoinOption = ELineJoin::Round;
    g.PathStroke(pattern(p), width, o);
    open_ = false;
  }
  bool open_ = false;
};

// Static panel art: panel colour, keybed, frames, tabs, printed marks, labels, name plates.
// Drawn once into a cached layer.
class BackgroundControl : public IControl {
 public:
  explicit BackgroundControl(const IRECT& r) : IControl(r) { SetIgnoreMouse(true); }
  void Draw(IGraphics& g) override {
    if (!g.CheckLayer(layer_)) {
      g.StartLayer(this, mRECT);
      GraphicsSink sink{g};
      art::drawPanelArt(sink);
      layer_ = g.EndLayer();
    }
    g.DrawLayer(layer_);
  }

 private:
  ILayerPtr layer_;
};

// ---------------------------------------------------------------------------------------------
inline void drawKnob(IGraphics& g, const Widget& w, double norm, bool hover) {
  GraphicsSink sink{g};
  const float ang = float(theme::kKnobMinDeg + std::clamp(norm, 0.0, 1.0) * (theme::kKnobMaxDeg - theme::kKnobMinDeg));
  art::drawKnob(sink, float(w.cx), float(w.cy), float(w.w / 2), hexOf(theme::cap(w.cap)), w.cap != Cap::Black, ang,
                hexOf(w.menu ? theme::kMenuText : theme::kSkirt), float(theme::kKnobMinDeg), float(theme::kKnobMaxDeg),
                hover);
}

inline void drawJack(IGraphics& g, float cx, float cy, float r, bool hover) {
  GraphicsSink sink{g};
  art::drawJack(sink, cx, cy, r, hover);
}

class KnobControl : public IControl {
 public:
  KnobControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }

  void Draw(IGraphics& g) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    drawKnob(g, w_, (s_.value(w_.id) - d->min) / (d->max - d->min), mMouseIsOver || dragging_);
    if (w_.menu) {
      g.DrawText(txt(12, theme::kMenuText), w_.label.c_str(), float(w_.cx), float(w_.cy + 42));
      g.DrawText(txt(11, theme::kAmber, false), formatParam(w_.id, s_.value(w_.id)).c_str(), float(w_.cx),
                 float(w_.cy + 58));
    }
  }
  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    IControl::OnMouseOver(x, y, mod);
    showReadout();
  }
  void OnMouseOut() override {
    IControl::OnMouseOut();
    if (!dragging_) s_.readout.clear();
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseDown(float, float, const IMouseMod&) override { dragging_ = true; showReadout(); }
  void OnMouseUp(float, float, const IMouseMod&) override { dragging_ = false; SetDirty(false); }
  void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override { nudge(-dY / (mod.S ? 2000.0 : 250.0)); }
  void OnMouseWheel(float, float, const IMouseMod& mod, float d) override { nudge(d / (mod.S ? 500.0 : 60.0)); }
  void OnMouseDblClick(float, float, const IMouseMod&) override {
    if (const core::ParameterDescriptor* d = desc(w_.id)) s_.set(w_.id, d->initial);
    showReadout();
  }

 private:
  void nudge(double fractionOfRange) {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    s_.set(w_.id, s_.value(w_.id) + fractionOfRange * (d->max - d->min));
    showReadout();
  }
  void showReadout() {
    s_.readout = formatParam(w_.id, s_.value(w_.id));
    s_.readoutX = float(w_.cx);
    s_.readoutY = float(w_.y() - 16);
    GetUI()->SetAllControlsDirty();
  }
  EditorShared& s_;
  Widget w_;
  bool dragging_ = false;
};

// Round latching push button (2-position parameter): amber ring when on.
class ButtonControl : public IControl {
 public:
  ButtonControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawButton(sink, float(w_.cx), float(w_.cy), float(w_.w / 2), s_.index(w_.id) > 0, mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.setIndex(w_.id, s_.index(w_.id) > 0 ? 0 : 1);
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// Lever switch (2 or 3 positions, pointing at the panel labels). Click the upper half to move the
// lever up, the lower half to move it down. On the menu overlay: a box showing the option.
class ToggleControl : public IControl {
 public:
  ToggleControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    const int n = positionsOf(d), idx = std::clamp(s_.index(w_.id), 0, n - 1);
    if (w_.menu) {
      const IRECT box = rectOf(w_);
      g.DrawRoundRect(col(mMouseIsOver ? theme::kAmber : theme::kMenuText), box, 5.f, nullptr, 1.5f);
      std::string opt = d && d->optionCount > 0 ? upper(std::string(d->options[idx])) : std::to_string(idx + 1);
      if (opt.size() > 10) opt = opt.substr(0, 10);
      g.DrawText(txt(11, theme::kMenuText), opt.c_str(), box);
      g.DrawText(txt(12, theme::kMenuText), w_.label.c_str(), float(w_.cx), float(w_.cy + 42));
      return;
    }
    const int pos = leverPos(idx, n);
    GraphicsSink sink{g};
    art::drawToggle(sink, float(w_.cx), float(w_.cy), n <= 1 ? 0.f : float(pos) / float(n - 1), mMouseIsOver);
  }
  void OnMouseDown(float, float y, const IMouseMod& mod) override {
    const int n = positionsOf(desc(w_.id)), idx = s_.index(w_.id);
    if (w_.menu) {
      s_.setIndex(w_.id, (idx + ((mod.R || mod.S) ? n - 1 : 1)) % n);
    } else {
      const int pos = std::clamp(leverPos(idx, n) + (y < float(w_.cy) ? -1 : 1), 0, n - 1);
      s_.setIndex(w_.id, w_.leverIndex[pos]);
    }
    // PLAY changes which side the menu edits and whether EDIT: LEFT / RIGHT shows: redraw all.
    if (w_.id == static_cast<std::uint32_t>(ParameterId::keyboard_behaviour)) GetUI()->SetAllControlsDirty();
    SetDirty(false);
  }

  // Lever position (0 = top) that shows parameter index `idx`.
  int leverPos(int idx, int n) const {
    for (int p = 0; p < n && p < 3; ++p)
      if (w_.leverIndex[p] == idx) return p;
    return 0;
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// Draws every patch cable (and the one being dragged) and the knob value readout on top.
class CableLayer : public IControl {
 public:
  CableLayer(EditorShared& s, const IRECT& r) : IControl(r), s_(s) { SetIgnoreMouse(true); }

  void setDrag(bool on, float x0 = 0, float y0 = 0, float x1 = 0, float y1 = 0) {
    drag_ = on; dx0_ = x0; dy0_ = y0; dx1_ = x1; dy1_ = y1;
    SetDirty(false);
  }
  void Draw(IGraphics& g) override {
    const core::DeviceStateV1* st = s_.state();
    if (st != nullptr) {
      for (std::uint32_t sink = 0; sink < core::kDevicePatchCapacity; ++sink) {
        if (st->inputCable[sink] == 0u) continue;
        auto a = s_.jackRects.find(static_cast<std::uint32_t>(st->cableSource[sink]));
        auto b = s_.jackRects.find(sink);
        if (a == s_.jackRects.end() || b == s_.jackRects.end()) continue;
        drawCable(g, a->second.MW(), a->second.MH(), b->second.MW(), b->second.MH(), theme::kCables[sink % 6]);
      }
    }
    if (drag_) drawCable(g, dx0_, dy0_, dx1_, dy1_, {235, 235, 235});
    if (!s_.readout.empty()) {
      const IRECT r(s_.readoutX - 52, s_.readoutY - 13, s_.readoutX + 52, s_.readoutY + 13);
      g.FillRoundRect(col(theme::kMenuBg, 235), r, 5.f);
      g.DrawText(txt(15, theme::kMenuText), s_.readout.c_str(), r);
    }
  }

 private:
  static void drawCable(IGraphics& g, float x0, float y0, float x1, float y1, theme::Rgb c) {
    const float dist = std::hypot(x1 - x0, y1 - y0);
    const float sag = 30.f + dist * 0.22f;
    for (int pass = 0; pass < 2; ++pass) {
      g.PathClear();
      g.PathMoveTo(x0, y0);
      g.PathCubicBezierTo(x0, y0 + sag, x1, y1 + sag, x1, y1);
      g.PathStroke(pass == 0 ? IPattern(IColor(110, 0, 0, 0)) : IPattern(col(c, 225)), pass == 0 ? 10.f : 7.f);
    }
    g.FillCircle(col(c), x0, y0, 10);
    g.FillCircle(col(c), x1, y1, 10);
    g.FillCircle(IColor(255, 20, 20, 22), x0, y0, 4);
    g.FillCircle(IColor(255, 20, 20, 22), x1, y1, 4);
  }
  EditorShared& s_;
  bool drag_ = false;
  float dx0_ = 0, dy0_ = 0, dx1_ = 0, dy1_ = 0;
};

// ---------------------------------------------------------------------------------------------
class JackControl : public IControl {
 public:
  JackControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) {
    SetTargetRECT(rectOf(w));
    for (const auto& d : registry::kJacks)
      if (static_cast<std::uint32_t>(d.id) == w.id) desc_ = &d;
  }
  JackId id() const { return static_cast<JackId>(w_.id); }
  bool isOutput() const { return desc_ != nullptr && desc_->direction == core::PinDirection::output; }
  float cx() const { return float(w_.cx); }
  float cy() const { return float(w_.cy); }

  void Draw(IGraphics& g) override { drawJack(g, cx(), cy(), float(w_.w / 2), mMouseIsOver); }

  void OnMouseDown(float, float, const IMouseMod&) override {
    const core::DeviceStateV1* st = s_.state();
    origin_ = this;
    const auto sink = static_cast<std::uint32_t>(w_.id);
    // Pulling a cable off an input: unplug it and keep dragging from its source.
    if (!isOutput() && st != nullptr && sink < core::kDevicePatchCapacity && st->inputCable[sink] != 0u) {
      const JackId src = st->cableSource[sink];
      s_.engine.postDisconnect(id());
      for (JackControl* j : s_.jacks)
        if (j->id() == src) origin_ = j;
    }
    s_.cables->setDrag(true, origin_->cx(), origin_->cy(), origin_->cx(), origin_->cy());
  }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override {
    s_.cables->setDrag(true, origin_->cx(), origin_->cy(), x, y);
  }
  void OnMouseUp(float x, float y, const IMouseMod&) override {
    s_.cables->setDrag(false);
    JackControl* target = nullptr;
    for (JackControl* j : s_.jacks)
      if (j->GetTargetRECT().Contains(x, y)) target = j;
    if (origin_ != nullptr && target != nullptr && target != origin_ && target->isOutput() != origin_->isOutput()) {
      JackControl* out = origin_->isOutput() ? origin_ : target;
      JackControl* in = origin_->isOutput() ? target : origin_;
      connect(out, in);
    }
    origin_ = nullptr;
    GetUI()->SetAllControlsDirty();
  }

 private:
  void connect(JackControl* out, JackControl* in) {
    const core::DeviceStateV1* st = s_.state();
    if (st == nullptr) return;
    // An output takes at most `maxCables` cables (usually one, as on the hardware):
    // re-patching an output moves its cable.
    const std::uint8_t maxCables = out->desc_ != nullptr && out->desc_->maxCables > 0 ? out->desc_->maxCables : 1;
    int used = 0;
    for (std::uint32_t k = 0; k < core::kDevicePatchCapacity; ++k)
      if (st->inputCable[k] != 0u && st->cableSource[k] == out->id() && static_cast<JackId>(k) != in->id()) ++used;
    for (std::uint32_t k = 0; k < core::kDevicePatchCapacity && used >= maxCables; ++k)
      if (st->inputCable[k] != 0u && st->cableSource[k] == out->id() && static_cast<JackId>(k) != in->id()) {
        s_.engine.postDisconnect(static_cast<JackId>(k));
        --used;
      }
    s_.engine.postConnect(out->id(), in->id());
  }
  EditorShared& s_;
  Widget w_;
  const core::JackDescriptor* desc_ = nullptr;
  JackControl* origin_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// One touch plate (semitone id from C). Lower on the plate = more pressure.
class PlateControl : public IControl {
 public:
  PlateControl(EditorShared& s, const Widget& w) : IControl(rectOf(w)), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawPlate(sink, mRECT.L, mRECT.T, mRECT.R, mRECT.B, s_.lit.count(int(w_.id)) > 0);
  }
  void OnMouseDown(float, float y, const IMouseMod&) override {
    semi_ = int(w_.id) + 12 * s_.octave;
    s_.note(true, semi_, EditorShared::kMouseId, pressureAt(y), EditorShared::kMouseSource, int(w_.id));
    s_.lit.insert(int(w_.id));
    SetDirty(false);
  }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override {
    if (semi_ == kNone) return;
    if (!mRECT.Contains(x, y)) release();
    else s_.pressure(EditorShared::kMouseId, pressureAt(y), EditorShared::kMouseSource, int(w_.id));
  }
  void OnMouseUp(float, float, const IMouseMod&) override { release(); }

 private:
  static constexpr int kNone = -1000;
  void release() {
    if (semi_ == kNone) return;
    s_.note(false, semi_, EditorShared::kMouseId, 0.0, EditorShared::kMouseSource, int(w_.id));
    s_.lit.erase(int(w_.id));
    semi_ = kNone;
    SetDirty(false);
  }
  double pressureAt(float y) const { return std::clamp(double((y - mRECT.T) / mRECT.H()), 0.1, 1.0); }
  EditorShared& s_;
  Widget w_;
  int semi_ = kNone;
};

// ---------------------------------------------------------------------------------------------
// The joystick: drag the stick inside its gate; double-click centres it.
class JoystickControl : public IControl {
 public:
  JoystickControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    const float cx = float(w_.cx), cy = float(w_.cy);
    const float x = cx + float(s_.value(w_.id) * 2.0 - 1.0) * kTravel;
    const float y = cy - float(s_.value(w_.id2) * 2.0 - 1.0) * kTravel;
    GraphicsSink sink{g};
    art::drawJoystick(sink, cx, cy, 55.f, kTravel, x, y, mMouseIsOver);
  }
  void OnMouseDown(float x, float y, const IMouseMod&) override { move(x, y); }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override { move(x, y); }
  void OnMouseDblClick(float, float, const IMouseMod&) override {
    s_.set(w_.id, 0.5);
    s_.set(w_.id2, 0.5);
    SetDirty(false);
  }

 private:
  static constexpr float kTravel = 90.f;
  void move(float x, float y) {
    s_.set(w_.id, std::clamp(0.5 + double((x - float(w_.cx)) / (2 * kTravel)), 0.0, 1.0));
    s_.set(w_.id2, std::clamp(0.5 - double((y - float(w_.cy)) / (2 * kTravel)), 0.0, 1.0));
    SetDirty(false);
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// The effector cartridge slot (id 0, shows the cartridge and both programs) and its button
// (id 1). Clicking either loads the next cartridge into both effector sides (right-click or
// Shift = previous); the L / R switches pick the program 1-2-3 on each side.
class CartridgeControl : public IControl {
 public:
  CartridgeControl(EditorShared& s, const Widget& w) : IControl(rectOf(w)), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    if (w_.id == 1) {
      g.FillCircle(col(theme::kInk), float(w_.cx), float(w_.cy), 16);
      g.FillCircle(col(mMouseIsOver ? theme::kAmber : theme::Rgb{60, 60, 60}), float(w_.cx), float(w_.cy), 10);
      return;
    }
    g.FillRect(col(theme::kInk), IRECT(1143, 238, 1257, 289));
    g.FillRect(col(mMouseIsOver ? theme::Rgb{190, 142, 58} : theme::Rgb{160, 118, 46}), IRECT(1150, 247, 1250, 280));
    const core::ProgramDescriptor* p = core::find_program(static_cast<core::ProgramId>(cartridge() * 3));
    g.DrawText(txt(12, theme::kInk), p ? upper(std::string(p->cartridge)).c_str() : "", 1200, 256);
    char progs[64];
    std::snprintf(progs, sizeof progs, "L%d  R%d", s_.index(static_cast<std::uint32_t>(ParameterId::effector_select_l)) + 1,
                  s_.index(static_cast<std::uint32_t>(ParameterId::effector_select_r)) + 1);
    g.DrawText(txt(10, theme::kInk, false), progs, 1200, 271);
  }
  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    IControl::OnMouseOver(x, y, mod);
    const core::DeviceStateV1* st = s_.state();
    if (st == nullptr) return;
    const core::ProgramDescriptor* l = core::find_program(st->leftEffector.program);
    const core::ProgramDescriptor* r = core::find_program(st->rightEffector.program);
    s_.readout = (l ? std::string(l->name) : "?") + " | " + (r ? std::string(r->name) : "?");
    s_.readoutX = 1200;
    s_.readoutY = 214;
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseOut() override {
    IControl::OnMouseOut();
    s_.readout.clear();
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    const int next = (cartridge() + ((mod.R || mod.S) ? 12 : 1)) % 13;
    s_.engine.postEffectorProgram(0, static_cast<core::ProgramId>(next * 3));
    s_.engine.postEffectorProgram(1, static_cast<core::ProgramId>(next * 3));
    OnMouseOver(x, y, mod);
  }

 private:
  int cartridge() const {
    const core::DeviceStateV1* st = s_.state();
    return st == nullptr ? 0 : int(static_cast<std::uint32_t>(st->leftEffector.program) / 3u);
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// Keyboard encoder (opens / closes the keyboard menu), octave arrows, display.
class EncoderControl : public IControl {
 public:
  EncoderControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawEncoder(sink, float(w_.cx), float(w_.cy), s_.menuOpen || mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.showMenu(!s_.menuOpen);
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseWheel(float, float, const IMouseMod&, float d) override {
    s_.shiftOctave(d > 0 ? 1 : -1);
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
  Widget w_;
};

class OctaveKeyControl : public IControl {
 public:
  OctaveKeyControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawOctaveKey(sink, float(w_.cx), float(w_.cy), mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.shiftOctave(w_.id == 1 ? 1 : -1);
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
  Widget w_;
};

class DisplayControl : public IControl {
 public:
  // The bezel and its shadow reach past the screen, so the control covers them too.
  DisplayControl(EditorShared& s, const Widget& w) : IControl(rectOf(w).GetPadded(12.f)), s_(s), screen_(rectOf(w)) {
    SetIgnoreMouse(true);
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    const IRECT& r = screen_;
    art::drawDisplay(sink, r.L, r.T, r.R, r.B);
    char b[32];
    if (!s_.engine.isReady()) std::snprintf(b, sizeof b, "NO AUDIO");  // see Preferences > audio device
    else if (s_.menuOpen) std::snprintf(b, sizeof b, "MENU");
    else std::snprintf(b, sizeof b, "OCT %+d", s_.octave);
    g.DrawText(txt(18, {235, 240, 255}), b, r);
    art::drawDisplayGlass(sink, r.L, r.T, r.R, r.B);
  }

 private:
  EditorShared& s_;
  IRECT screen_;
};

// ---------------------------------------------------------------------------------------------
// DRONE VOICES key: opens / closes drone 1..6 (the LED is lit while the voice is open).
class DroneKeyControl : public IControl {
 public:
  // The key's shadow falls outside the key, so the control covers it; clicks stay on the key.
  DroneKeyControl(EditorShared& s, const Widget& w) : IControl(rectOf(w).GetPadded(12.f)), s_(s), w_(w) {
    SetTargetRECT(rectOf(w));
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    const IRECT r = rectOf(w_);
    art::drawDroneKey(sink, r.L, r.T, r.R, r.B, s_.engine.droneKey(int(w_.id)), mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.engine.postDroneKey(int(w_.id), !s_.engine.droneKey(int(w_.id)));
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// A printed indicator LED lit from the engine (see StandaloneAudioEngine::PanelLed). The unlit
// LED is part of the static art; this draws the lit lens and its spill (art::drawLed) over it,
// and asks to be redrawn only when its brightness changes visibly.
class LedControl : public IControl {
 public:
  LedControl(EditorShared& s, int led, float cx, float cy, float r, std::uint32_t rgb)
      : IControl(IRECT(cx - 3 * r, cy - 3 * r, cx + 3 * r, cy + 3 * r)), s_(s), led_(led), cx_(cx), cy_(cy),
        r_(r), rgb_(rgb) {
    SetIgnoreMouse(true);
  }
  bool IsDirty() override {
    return std::fabs(s_.engine.panelLed(led_) - shown_) > 0.03f || IControl::IsDirty();
  }
  void Draw(IGraphics& g) override {
    shown_ = s_.engine.panelLed(led_);
    if (shown_ <= 0.01f) return;
    GraphicsSink sink{g};
    art::drawLed(sink, cx_, cy_, r_, rgb_, shown_);
  }

 private:
  EditorShared& s_;
  int led_;
  float cx_, cy_, r_;
  std::uint32_t rgb_;
  float shown_ = -1.f;
};

// MUTE: silences every output (the engine keeps running, so unmuting picks up where the sound
// is). Amber ring while muted, like the panel's latching buttons. Not on the hardware.
class MasterMuteControl : public IControl {
 public:
  MasterMuteControl(EditorShared& s, const Widget& w)
      : IControl(rectOf(w).GetPadded(14.f).Union(IRECT(float(w.cx - 40), float(w.cy - 56), float(w.cx + 40),
                                                       float(w.cy)))),
        s_(s), w_(w) {
    SetTargetRECT(rectOf(w));
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawButton(sink, float(w_.cx), float(w_.cy), float(w_.w / 2), s_.engine.muted(), mMouseIsOver);
    g.DrawText(txt(15, theme::kInk), "MUTE", float(w_.cx), float(w_.cy - 42));
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.engine.setMuted(!s_.engine.muted());
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// Non-interactive panel hardware: photo sensor, the drone LED bar (lit per unmuted tone),
// and jacks that have no function in Lunar 24.
class DecorControl : public IControl {
 public:
  DecorControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetIgnoreMouse(true); }
  void Draw(IGraphics& g) override {
    switch (w_.id) {
      case 0: {
        GraphicsSink sink{g};
        art::drawSensor(sink, float(w_.cx), float(w_.cy), float(w_.w / 2));
        break;
      }
      case 1:
        for (std::uint32_t i = 0; i < 5; ++i) {
          const bool muted = s_.value(w_.id2 + i) > 0.5;
          g.FillRect(col(muted ? theme::kLedOff : theme::kLedOn),
                     IRECT(float(w_.x() + 5 + i * 12.5), float(w_.y() + 8), float(w_.x() + 14 + i * 12.5), float(w_.y() + 38)));
        }
        break;
      default: drawJack(g, float(w_.cx), float(w_.cy), float(w_.w / 2), false); break;
    }
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// The keyboard menu background: covers the plates (and swallows their clicks) while open.
class MenuBackground : public IControl {
 public:
  explicit MenuBackground(EditorShared& s)
      : IControl(IRECT(float(kMenuX0), float(kMenuY0), float(kMenuX1), float(kMenuY1))), s_(s) {}
  void Draw(IGraphics& g) override {
    g.FillRoundRect(col(theme::kMenuBg), mRECT, 10.f);
    g.DrawText(txt(18, theme::kMenuText), "KEYBOARD MENU", float(kMenuTitleX), mRECT.T + 22);
    if (s_.menuPage == 2) {
      // Row names with the pattern length (set by ARP RHYTHM / SEQ RHYTHM on SETTINGS).
      const auto id = [](ParameterId p) { return static_cast<std::uint32_t>(p); };
      const int arpLen = int(core::arp_length_steps(s_.value(id(ParameterId::keyboard_arp_length))));
      const int seqLen = int(core::seq_rhythm_length_steps(s_.value(id(ParameterId::keyboard_seq_rhythm_length))));
      char buf[24];
      g.DrawText(txt(14, theme::kMenuText), "ARP", 520, float(kRhythmArpY) - 8.f);
      std::snprintf(buf, sizeof buf, "%d step%s", arpLen, arpLen == 1 ? "" : "s");
      g.DrawText(txt(11, theme::kMenuText), buf, 520, float(kRhythmArpY) + 14.f);
      g.DrawText(txt(14, theme::kMenuText), "SEQ", 520, float(kRhythmSeqY) - 8.f);
      std::snprintf(buf, sizeof buf, "%d step%s", seqLen, seqLen == 1 ? "" : "s");
      g.DrawText(txt(11, theme::kMenuText), buf, 520, float(kRhythmSeqY) + 14.f);
      g.DrawText(txt(11, theme::kMenuText),
                 "amber = plays   dark = silent beat   outline = not used (set the length: ARP RHYTHM / SEQ RHYTHM on SETTINGS)",
                 mRECT.MW(), float(kRhythmSeqY) + 66.f);
    }
    if (s_.menuPage == 1) {
      g.DrawText(txt(12, theme::kMenuText, true, -90.f), "NOTE", 440, float(kSeqSliderTop + kSeqSliderBottom) / 2);
      g.DrawText(txt(12, theme::kMenuText), "GATE", 440, float(kSeqGateY));
    }
  }

 private:
  EditorShared& s_;
};

// Menu title-row buttons: page tabs (SETTINGS / SEQUENCER) and the left/right side switch.
// The side switch only shows under PLAY = SPLIT (in SINGLE / TWIN both halves share the
// left bank); it applies to both pages.
class MenuTabControl : public IControl {
 public:
  enum Kind { kSettings, kSequencer, kRhythm, kSide, kClose, kReset };
  MenuTabControl(EditorShared& s, const Rect& r, Kind k)
      : IControl(IRECT(float(r.x0), float(r.y0), float(r.x1), float(r.y1))), s_(s), k_(k) {}
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    if (k_ == kSide && !s_.split()) return;  // nothing to choose outside SPLIT
    const bool armed = k_ == kReset && armedNow();
    const char* label = k_ == kSettings ? "SETTINGS" : k_ == kSequencer ? "SEQUENCER" : k_ == kRhythm ? "RHYTHM"
                        : k_ == kClose ? "CLOSE"
                        : k_ == kReset ? (armed ? "CLICK TO CONFIRM" : "RESET PANEL")
                        : s_.seqSide == 0 ? "EDIT: LEFT" : "EDIT: RIGHT";
    const bool active = (k_ == kSettings && s_.menuPage == 0) || (k_ == kSequencer && s_.menuPage == 1) ||
                        (k_ == kRhythm && s_.menuPage == 2) || armed;
    art::drawMenuTab(sink, mRECT.L, mRECT.T, mRECT.R, mRECT.B, label, active, mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    if (k_ == kClose) {
      s_.showMenu(false);
      GetUI()->SetAllControlsDirty();
      return;
    }
    if (k_ == kReset) {  // first click arms, a second within a few seconds resets everything
      if (!armedNow()) {
        armedAt_ = std::chrono::steady_clock::now();
        SetDirty(false);
        return;
      }
      armedAt_ = {};
      for (int v = 0; v < 6; ++v) s_.engine.postDroneKey(v, true);
      s_.octave = 0;
      s_.seqSide = 0;
      s_.presetSlot = 0;
      s_.menuPage = 0;
      if (s_.factoryReset) s_.factoryReset();
      s_.showMenu(false);
      GetUI()->SetAllControlsDirty();
      return;
    }
    if (k_ == kSide) {
      if (!s_.split()) return;
      s_.seqSide = 1 - s_.seqSide;
    }
    else s_.menuPage = k_ == kSettings ? 0 : k_ == kSequencer ? 1 : 2;
    s_.showMenu(true);
    GetUI()->SetAllControlsDirty();
  }

 private:
  bool armedNow() const {
    return armedAt_ != std::chrono::steady_clock::time_point{} &&
           std::chrono::steady_clock::now() - armedAt_ < std::chrono::seconds(4);
  }
  EditorShared& s_;
  Kind k_;
  std::chrono::steady_clock::time_point armedAt_{};
};

// Keyboard presets A-D: the slot button (click = next, right-click = previous) and LOAD /
// SAVE / INIT for that slot. A preset holds every keyboard menu setting of both sides and
// both 16-step sequences, but not the tempo. INIT (back to factory settings) needs a second
// click to confirm. The button briefly shows what happened.
class PresetControl : public IControl {
 public:
  enum Kind { kSlot, kLoad, kSave, kInit };
  PresetControl(EditorShared& s, const Rect& r, Kind k)
      : IControl(IRECT(float(r.x0), float(r.y0), float(r.x1), float(r.y1))), s_(s), k_(k) {}
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    const auto now = std::chrono::steady_clock::now();
    const bool armed = now < armedUntil_, flash = now < doneUntil_;
    char slot[16];
    std::snprintf(slot, sizeof slot, "PRESET %c", char('A' + s_.presetSlot));
    const char* label = k_ == kSlot ? slot
                        : flash     ? (k_ == kLoad ? "LOADED" : k_ == kSave ? "SAVED" : "CLEARED")
                        : armed     ? "SURE?"
                        : k_ == kLoad ? "LOAD" : k_ == kSave ? "SAVE" : "INIT";
    art::drawMenuTab(sink, mRECT.L, mRECT.T, mRECT.R, mRECT.B, label, armed || flash, mMouseIsOver);
  }
  // Redraw once more when the confirm or "done" label runs out.
  bool IsDirty() override {
    const auto now = std::chrono::steady_clock::now();
    const bool shown = now < armedUntil_ || now < doneUntil_;
    const bool changed = shown != shownLast_;
    shownLast_ = shown;
    return IControl::IsDirty() || changed;
  }
  void OnMouseDown(float, float, const IMouseMod& mod) override {
    using Action = StandaloneAudioEngine::PresetAction;
    const auto now = std::chrono::steady_clock::now();
    if (k_ == kSlot) {
      s_.presetSlot = (s_.presetSlot + ((mod.R || mod.S) ? 3 : 1)) % 4;
      GetUI()->SetAllControlsDirty();  // the other buttons act on the new slot
      return;
    }
    if (k_ == kInit && now >= armedUntil_) {  // first click arms, a second within 4 s clears
      armedUntil_ = now + std::chrono::seconds(4);
      SetDirty(false);
      return;
    }
    armedUntil_ = {};
    const Action a = k_ == kLoad ? Action::Load : k_ == kSave ? Action::Save : Action::Initialise;
    if (s_.engine.postKeyboardPreset(a, static_cast<std::uint32_t>(s_.presetSlot)))
      doneUntil_ = now + std::chrono::milliseconds(1200);
    GetUI()->SetAllControlsDirty();  // a load changes the menu settings and maybe PLAY
  }

 private:
  EditorShared& s_;
  Kind k_;
  std::chrono::steady_clock::time_point armedUntil_{}, doneUntil_{};
  bool shownLast_ = false;
};

// One step of the 16-step keyboard sequencer: drag the slider for the note (semitones above
// the held plate), click the round button for the step's gate. Double-click resets the note.
class SeqStepControl : public IControl {
 public:
  SeqStepControl(EditorShared& s, int step)
      : IControl(IRECT(float(seq_step_rect(step).x0), float(seq_step_rect(step).y0), float(seq_step_rect(step).x1),
                       float(seq_step_rect(step).y1))),
        s_(s), step_(step) {}
  void Draw(IGraphics& g) override {
    const core::KeyboardSeqStep* st = s_.seqStep(step_);
    if (st == nullptr) return;
    GraphicsSink sink{g};
    art::drawSeqStep(sink, mRECT.L, mRECT.R, float(kSeqSliderTop), float(kSeqSliderBottom), float(kSeqGateY), step_,
                     st->note, StandaloneAudioEngine::kSeqStepMaxNote, st->gate != 0, mMouseIsOver);
  }
  void OnMouseDown(float, float y, const IMouseMod&) override {
    const core::KeyboardSeqStep* st = s_.seqStep(step_);
    if (st == nullptr) return;
    if (y > float(kSeqGateY) - 20.f) post(st->note, st->gate == 0);
    else post(noteAt(y), st->gate != 0);
  }
  void OnMouseDrag(float, float y, float, float, const IMouseMod&) override {
    const core::KeyboardSeqStep* st = s_.seqStep(step_);
    if (st != nullptr && y <= float(kSeqGateY) - 20.f) post(noteAt(y), st->gate != 0);
  }
  void OnMouseDblClick(float, float y, const IMouseMod&) override {
    const core::KeyboardSeqStep* st = s_.seqStep(step_);
    if (st != nullptr && y <= float(kSeqGateY) - 20.f) post(0, st->gate != 0);
  }

 private:
  static int noteAt(float y) {
    const double t = (kSeqSliderBottom - double(y)) / (kSeqSliderBottom - kSeqSliderTop);
    return int(std::lround(std::clamp(t, 0.0, 1.0) * StandaloneAudioEngine::kSeqStepMaxNote));
  }
  void post(int note, bool gate) {
    s_.engine.postSeqStep(s_.editSide(), step_, note, gate);
    SetDirty(false);
  }
  EditorShared& s_;
  int step_;
};

// One step of a RHYTHM pattern (row 0 = arpeggiator, 1 = sequencer). Lit = this clock edge
// reaches the arp / sequencer; dark = muted. Steps past the pattern length are drawn faint.
class RhythmStepControl : public IControl {
 public:
  RhythmStepControl(EditorShared& s, int row, int step)
      : IControl(IRECT(float(rhythm_step_rect(row, step).x0), float(rhythm_step_rect(row, step).y0),
                       float(rhythm_step_rect(row, step).x1), float(rhythm_step_rect(row, step).y1))),
        s_(s), row_(row), step_(step) {}
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    const bool on = ((mask() >> step_) & 1u) == 0;
    const bool inUse = step_ < length();
    char buf[4];
    std::snprintf(buf, sizeof buf, "%d", step_ + 1);
    const float cx = mRECT.MW(), cy = mRECT.MH();
    sink.text(cx, cy - 32, 13, inUse ? (mMouseIsOver ? art::kMenuAmberRgb : art::kMenuTextRgb) : art::kMenuDimRgb,
              false, buf);
    if (!inUse) {  // past the pattern length: an empty outline
      sink.fillCircle(cx, cy + 6, 16, 0x3a393f);
      sink.fillCircle(cx, cy + 6, 14, 0x1e1e22);
    } else if (on) {  // this beat plays
      sink.fillCircle(cx, cy + 6, 16, art::kMenuAmberRgb);
    } else {  // a silent beat: dark with a grey rim
      sink.fillCircle(cx, cy + 6, 16, 0x8a898e);
      sink.fillCircle(cx, cy + 6, 12, 0x1e1e22);
    }
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.engine.postKeyboardRhythm(s_.editSide(), row_ == 1, static_cast<std::uint8_t>(mask() ^ (1u << step_)));
    SetDirty(false);
  }

 private:
  std::uint8_t mask() const { return s_.engine.keyboardRhythm(s_.editSide(), row_ == 1); }
  int length() const {
    const auto id = [](ParameterId p) { return static_cast<std::uint32_t>(p); };
    return row_ == 0 ? int(core::arp_length_steps(s_.value(id(ParameterId::keyboard_arp_length))))
                     : int(core::seq_rhythm_length_steps(s_.value(id(ParameterId::keyboard_seq_rhythm_length))));
  }
  EditorShared& s_;
  int row_, step_;
};

// ---------------------------------------------------------------------------------------------
// Build the whole panel into `g`. `shared` must outlive the editor.
inline void BuildPanel(IGraphics* g, EditorShared& shared) {
  // The label font is built into the app (a system font looked up by name can be missing,
  // and IGraphics then draws no text at all).
  g->LoadFont(kFont, const_cast<unsigned char*>(font::kRegular), static_cast<int>(font::kRegularSize));
  g->LoadFont(kFontBold, const_cast<unsigned char*>(font::kBold), static_cast<int>(font::kBoldSize));
  g->AttachPanelBackground(col(theme::kPanel));

  const std::vector<Widget> widgets = build_panel_layout();
  const IRECT all = g->GetBounds();
  g->AttachControl(new BackgroundControl(all));

  shared.jacks.clear();
  shared.jackRects.clear();
  shared.menuControls.clear();
  shared.plates.clear();
  for (const Widget& w : widgets) {
    if (w.menu) continue;
    switch (w.kind) {
      case WidgetKind::Knob: g->AttachControl(new KnobControl(shared, w)); break;
      case WidgetKind::Button: g->AttachControl(new ButtonControl(shared, w)); break;
      case WidgetKind::Toggle: g->AttachControl(new ToggleControl(shared, w)); break;
      case WidgetKind::Jack: {
        auto* j = new JackControl(shared, w);
        shared.jacks.push_back(j);
        shared.jackRects[w.id] = j->GetTargetRECT();
        g->AttachControl(j);
        break;
      }
      case WidgetKind::Plate: {
        auto* p = new PlateControl(shared, w);
        shared.plates.push_back(p);
        g->AttachControl(p);
        break;
      }
      case WidgetKind::Joystick: g->AttachControl(new JoystickControl(shared, w)); break;
      case WidgetKind::Cartridge: g->AttachControl(new CartridgeControl(shared, w)); break;
      case WidgetKind::DroneKey: g->AttachControl(new DroneKeyControl(shared, w)); break;
      case WidgetKind::MasterMute: g->AttachControl(new MasterMuteControl(shared, w)); break;
      case WidgetKind::Encoder: g->AttachControl(new EncoderControl(shared, w)); break;
      case WidgetKind::OctaveKey: g->AttachControl(new OctaveKeyControl(shared, w)); break;
      case WidgetKind::Display: g->AttachControl(new DisplayControl(shared, w)); break;
      case WidgetKind::Decor: g->AttachControl(new DecorControl(shared, w)); break;
    }
  }
  // The indicator LEDs the engine drives, each over its printed LED (same position and colour).
  static_assert(sizeof(kPanelLedPos) / sizeof(kPanelLedPos[0]) == StandaloneAudioEngine::kPanelLedCount,
                "one panel position per engine LED");
  for (int i = 0; i < StandaloneAudioEngine::kPanelLedCount; ++i)
    for (const art::Led& l : art::kLeds)
      if (std::fabs(l.x - kPanelLedPos[i].x) < 1.0 && std::fabs(l.y - kPanelLedPos[i].y) < 1.0)
        g->AttachControl(new LedControl(shared, i, l.x, l.y, l.r, l.rgb));
  // The keyboard menu on top of the plates, hidden until the encoder opens it.
  shared.menuChrome.clear();
  shared.seqControls.clear();
  shared.rhythmControls.clear();
  auto* menuBg = new MenuBackground(shared);
  g->AttachControl(menuBg);
  shared.menuChrome.push_back(menuBg);
  for (auto k : {MenuTabControl::kSettings, MenuTabControl::kSequencer, MenuTabControl::kRhythm}) {
    auto* t = new MenuTabControl(shared, k == MenuTabControl::kSettings    ? kMenuTabSettings
                                         : k == MenuTabControl::kSequencer ? kMenuTabSequencer
                                                                           : kMenuTabRhythm, k);
    g->AttachControl(t);
    shared.menuChrome.push_back(t);
  }
  auto* close = new MenuTabControl(shared, kMenuClose, MenuTabControl::kClose);
  g->AttachControl(close);
  shared.menuChrome.push_back(close);
  auto* reset = new MenuTabControl(shared, kMenuReset, MenuTabControl::kReset);
  g->AttachControl(reset);
  shared.menuChrome.push_back(reset);
  auto* side = new MenuTabControl(shared, kSeqSideSwitch, MenuTabControl::kSide);
  g->AttachControl(side);
  shared.menuChrome.push_back(side);
  for (auto [r, k] : {std::pair{kPresetSlot, PresetControl::kSlot}, std::pair{kPresetLoad, PresetControl::kLoad},
                      std::pair{kPresetSave, PresetControl::kSave}, std::pair{kPresetInit, PresetControl::kInit}}) {
    auto* c = new PresetControl(shared, r, k);
    g->AttachControl(c);
    shared.menuChrome.push_back(c);
  }
  for (int row = 0; row < 2; ++row)
    for (int i = 0; i < kRhythmSteps; ++i) {
      auto* c = new RhythmStepControl(shared, row, i);
      g->AttachControl(c);
      shared.rhythmControls.push_back(c);
    }
  for (int i = 0; i < kSeqSteps; ++i) {
    auto* c = new SeqStepControl(shared, i);
    g->AttachControl(c);
    shared.seqControls.push_back(c);
  }
  for (const Widget& w : widgets) {
    if (!w.menu) continue;
    IControl* c = w.kind == WidgetKind::Knob ? static_cast<IControl*>(new KnobControl(shared, w))
                                             : static_cast<IControl*>(new ToggleControl(shared, w));
    g->AttachControl(c);
    shared.menuControls.push_back(c);
  }
  shared.showMenu(false);

  shared.cables = new CableLayer(shared, all);
  g->AttachControl(shared.cables);

  g->EnableMouseOver(true);  // hover highlights and knob value readouts
  g->SetKeyHandlerFunc([&shared, g](const IKeyPress& key, bool isUp) {
    const bool used = shared.key(key, isUp);
    if (used) g->SetAllControlsDirty();
    return used;
  });
  // Redraw when MIDI CC moved knobs, or a whole new machine state lands (startup restore).
  g->SetDisplayTickFunc([&shared, g]() {
    if (shared.engine.syncParametersFromAudioThread() > 0) g->SetAllControlsDirty();  // MIDI CC
    if (shared.engine.isReady() != shared.seenReady) {  // audio started or stopped
      shared.seenReady = shared.engine.isReady();
      g->SetAllControlsDirty();
    }
    if (shared.engine.stateVersion() != shared.seenStateVersion) {
      shared.seenStateVersion = shared.engine.stateVersion();
      g->SetAllControlsDirty();
    }
  });
}

}  // namespace lunar24::host::ui
