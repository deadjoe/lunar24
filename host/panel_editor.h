// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_editor.h — the Lunar 24 panel, drawn with iPlug2 IGraphics.
//
// Every control comes from host/include/host/panel_ui_layout.h (positions) and draws with
// the colours in panel_theme.h. Controls do not use iPlug parameters: they read the
// machine state from the engine and send changes through its live-control queue
// (StandaloneAudioEngine::postParameter / postConnect / postEvent ...), so the saved
// state always matches what the user sees and hears.
//
// Playing: click the touch plates, or use the computer keyboard like a piano
// (A W S E D F T G Y H U J K O L P ; — Z / X shift the octave).
// Patching: drag from any jack to another; drag a cable off an input to unplug it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "IControl.h"
#include "IGraphics.h"

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

inline IColor col(theme::Rgb c, int alpha = 255) { return IColor(alpha, c.r, c.g, c.b); }
inline IText txt(float size, theme::Rgb c, bool bold = false, EAlign align = EAlign::Center) {
  return IText(size, col(c), bold ? kFontBold : kFont, align, EVAlign::Middle);
}

class CableLayer;
class PlatesControl;
class JackControl;

// Shared by every control of one editor instance.
struct EditorShared {
  explicit EditorShared(StandaloneAudioEngine& e) : engine(e) {}
  StandaloneAudioEngine& engine;
  std::vector<JackControl*> jacks;
  std::map<std::uint32_t, IRECT> jackRects;  // JackId -> rect (for cable drawing)
  CableLayer* cables = nullptr;
  PlatesControl* plates = nullptr;
  core::InputStateMachine input{nullptr, 0};
  std::uint64_t seq = 0;
  std::uint64_t seenStateVersion = ~0ull;

  const core::DeviceStateV1* state() const { return engine.canonicalState(); }
  double value(std::uint32_t id) const { return engine.parameterValue(static_cast<ParameterId>(id)); }
  void set(std::uint32_t id, double v) { engine.postParameter(static_cast<ParameterId>(id), v); }

  // Keyboard notes (plates, computer keys): through the same input state machine as MIDI.
  void note(bool on, int semitoneFromC3, core::NoteId id, double pressure, core::ControlSourceId source) {
    core::PerformanceInput in{};
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
  void pressure(core::NoteId id, double p, core::ControlSourceId source) {
    core::PerformanceInput in{};
    in.kind = core::PerfInputKind::aftertouch;
    in.value = static_cast<core::SignalSample>(p);
    in.noteId = id;
    in.source = source;
    in.seq = ++seq;
    core::ControlEvent ev[1];
    if (input.translate(in, ev, 1) == 1) engine.postEvent(ev[0]);
  }
};

inline const core::ParameterDescriptor* desc(std::uint32_t id) {
  return core::find_parameter(static_cast<ParameterId>(id));
}

inline std::string formatValue(const core::ParameterDescriptor* d, double v) {
  char b[48];
  if (d == nullptr) return "";
  const std::string unit(d->unit);
  if (unit == "norm") std::snprintf(b, sizeof b, "%.0f%%", 100.0 * (v - d->min) / (d->max - d->min));
  else if (unit == "seconds") std::snprintf(b, sizeof b, v < 1.0 ? "%.0f ms" : "%.2f s", v < 1.0 ? v * 1000.0 : v);
  else if (unit == "hz") std::snprintf(b, sizeof b, "%.2f Hz", v);
  else if (unit == "oct") std::snprintf(b, sizeof b, "%+.2f oct", v);
  else if (unit == "volts") std::snprintf(b, sizeof b, "%.2f V", v);
  else std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

// ---------------------------------------------------------------------------------------------
// Static background: module cards and the name plate.
class BackgroundControl : public IControl {
 public:
  BackgroundControl(const IRECT& r, std::vector<Widget> cards) : IControl(r), cards_(std::move(cards)) {
    SetIgnoreMouse(true);
  }
  void Draw(IGraphics& g) override {
    for (const Widget& w : cards_) {
      const theme::Rgb a = theme::accent(w.accent);
      const IRECT r(static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(w.x + w.w),
                    static_cast<float>(w.y + w.h));
      if (w.kind == WidgetKind::Title) {
        g.FillCircle(col({205, 202, 194}), r.L + 70, r.T + 75, 52);
        g.FillCircle(col(theme::kBackground), r.L + 92, r.T + 62, 52);
        g.DrawText(txt(64, theme::kText, false, EAlign::Near), "LUNAR 24", r.L + 150, r.T + 72);
        g.DrawText(txt(16, theme::kTextDim, false, EAlign::Near), "AMBIENT  DRONE  MACHINE", r.L + 154, r.T + 118);
        continue;
      }
      g.FillRoundRect(col(theme::kCard), r.GetPadded(-1), 10);
      g.DrawRoundRect(col(theme::kCardEdge), r.GetPadded(-1), 10, nullptr, 1.5f);
      g.DrawText(txt(13, a, true, EAlign::Near), w.label.c_str(), r.L + 12, r.T + 15);
      g.DrawLine(col(a, 90), r.L + 12, r.T + 28, r.R - 12, r.T + 28, nullptr, 1.5f);
    }
  }

 private:
  std::vector<Widget> cards_;
};

// ---------------------------------------------------------------------------------------------
class KnobControl : public IControl {
 public:
  KnobControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {}

  void Draw(IGraphics& g) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    const float dia = mRECT.W(), cx = mRECT.MW(), cy = mRECT.T + dia / 2, rad = dia / 2 - 5;
    const double v = s_.value(w_.id);
    const double n = std::clamp((v - d->min) / (d->max - d->min), 0.0, 1.0);
    const float ang = float(theme::kKnobMinDeg + n * (theme::kKnobMaxDeg - theme::kKnobMinDeg));
    g.DrawArc(col(theme::kKnobTrack), cx, cy, rad + 2, float(theme::kKnobMinDeg), float(theme::kKnobMaxDeg), nullptr, 3);
    if (n > 0.001) g.DrawArc(col(theme::accent(w_.accent)), cx, cy, rad + 2, float(theme::kKnobMinDeg), ang, nullptr, 3);
    g.FillCircle(col(theme::kKnobBody), cx, cy, rad - 3);
    g.DrawCircle(col(mMouseIsOver || dragging_ ? theme::kTextDim : theme::kCardEdge), cx, cy, rad - 3, nullptr, 1.5f);
    const float t = (ang - 90.f) * 3.14159265f / 180.f;
    g.DrawLine(col(theme::kText), cx + std::cos(t) * (rad - 14) * 0.2f, cy + std::sin(t) * (rad - 14) * 0.2f,
               cx + std::cos(t) * (rad - 5), cy + std::sin(t) * (rad - 5), nullptr, 2.5f);
    const std::string label = dragging_ || mMouseIsOver ? formatValue(d, v) : w_.label;
    g.DrawText(txt(w_.small ? 9.5f : 10.5f, dragging_ ? theme::kText : theme::kTextDim), label.c_str(),
               mRECT.MW(), mRECT.B - 8);
  }
  void OnMouseDown(float, float, const IMouseMod&) override { dragging_ = true; SetDirty(false); }
  void OnMouseUp(float, float, const IMouseMod&) override { dragging_ = false; SetDirty(false); }
  void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override { nudge(-dY / (mod.S ? 2000.0 : 250.0)); }
  void OnMouseWheel(float, float, const IMouseMod& mod, float d) override { nudge(d / (mod.S ? 500.0 : 60.0)); }
  void OnMouseDblClick(float, float, const IMouseMod&) override {
    if (const core::ParameterDescriptor* d = desc(w_.id)) s_.set(w_.id, d->initial);
    SetDirty(false);
  }

 private:
  void nudge(double fractionOfRange) {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    s_.set(w_.id, s_.value(w_.id) + fractionOfRange * (d->max - d->min));
    SetDirty(false);
  }
  EditorShared& s_;
  Widget w_;
  bool dragging_ = false;
};

// ---------------------------------------------------------------------------------------------
class SelectorControl : public IControl {
 public:
  SelectorControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {}

  void Draw(IGraphics& g) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    const theme::Rgb a = theme::accent(w_.accent);
    const IRECT box(mRECT.L + 2, mRECT.T + 2, mRECT.R - 2, mRECT.B - 18);
    const int idx = index_(d);
    const bool on = idx > 0 && count_(d) == 2;
    g.FillRoundRect(on ? col(a, 70) : col(theme::kKnobBody), box, 5);
    g.DrawRoundRect(col(a, mMouseIsOver ? 255 : 200), box, 5, nullptr, 1.2f);
    std::string opt = (d->optionCount > 0 && idx >= 0 && idx < int(d->optionCount)) ? d->options[idx]
                                                                                   : std::to_string(idx + 1);
    for (auto& ch : opt) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const float size = opt.size() > 6 ? std::max(7.5f, 10.f - float(opt.size() - 6) * 0.6f) : 10.f;
    if (opt.size() > 10) opt = opt.substr(0, 10);
    g.DrawText(txt(size, a, true), opt.c_str(), box);
    g.DrawText(txt(9.5f, theme::kTextDim), w_.label.c_str(), mRECT.MW(), mRECT.B - 8);
  }
  void OnMouseDown(float, float, const IMouseMod& mod) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    if (d == nullptr) return;
    const int n = count_(d);
    const int idx = (index_(d) + ((mod.R || mod.S) ? n - 1 : 1)) % n;
    s_.set(w_.id, d->min + idx * (d->step > 0 ? d->step : 1.0));
    SetDirty(false);
  }

 private:
  int count_(const core::ParameterDescriptor* d) const {
    if (d->optionCount > 0) return int(d->optionCount);
    return std::max(2, int(std::lround((d->max - d->min) / (d->step > 0 ? d->step : 1.0))) + 1);
  }
  int index_(const core::ParameterDescriptor* d) const {
    return int(std::lround((s_.value(w_.id) - d->min) / (d->step > 0 ? d->step : 1.0)));
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// Draws every patch cable (and the one being dragged) on top of the panel.
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
        drawCable(g, a->second.MW(), a->second.T + 17, b->second.MW(), b->second.T + 17,
                  theme::kCables[sink % 6]);
      }
    }
    if (drag_) drawCable(g, dx0_, dy0_, dx1_, dy1_, {235, 235, 235});
  }

 private:
  static void drawCable(IGraphics& g, float x0, float y0, float x1, float y1, theme::Rgb c) {
    const float dist = std::hypot(x1 - x0, y1 - y0);
    const float sag = 30.f + dist * 0.22f;
    for (int pass = 0; pass < 2; ++pass) {
      g.PathClear();
      g.PathMoveTo(x0, y0);
      g.PathCubicBezierTo(x0, y0 + sag, x1, y1 + sag, x1, y1);
      g.PathStroke(pass == 0 ? IPattern(IColor(110, 0, 0, 0)) : IPattern(col(c, 225)), pass == 0 ? 8.f : 5.f);
    }
    g.FillCircle(col(c), x0, y0, 7);
    g.FillCircle(col(c), x1, y1, 7);
    g.FillCircle(IColor(255, 20, 20, 22), x0, y0, 3);
    g.FillCircle(IColor(255, 20, 20, 22), x1, y1, 3);
  }
  EditorShared& s_;
  bool drag_ = false;
  float dx0_ = 0, dy0_ = 0, dx1_ = 0, dy1_ = 0;
};

// ---------------------------------------------------------------------------------------------
class JackControl : public IControl {
 public:
  JackControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {
    for (const auto& d : registry::kJacks)
      if (static_cast<std::uint32_t>(d.id) == w.id) desc_ = &d;
  }
  JackId id() const { return static_cast<JackId>(w_.id); }
  bool isOutput() const { return desc_ != nullptr && desc_->direction == core::PinDirection::output; }
  float cx() const { return mRECT.MW(); }
  float cy() const { return mRECT.T + 17; }

  void Draw(IGraphics& g) override {
    const theme::Rgb a = theme::accent(w_.accent);
    if (isOutput()) g.FillRoundRect(col(a, 90), IRECT(cx() - 17, cy() - 17, cx() + 17, cy() + 17), 6);
    g.FillCircle(col(mMouseIsOver ? theme::kText : theme::kJackRing), cx(), cy(), 13);
    g.FillCircle(col(theme::kJackHole), cx(), cy(), 7);
    g.DrawText(txt(9, isOutput() ? a : theme::kTextDim, isOutput()), w_.label.c_str(), mRECT.MW(), mRECT.B - 7);
  }

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
      if (j->GetRECT().Contains(x, y)) target = j;
    if (target != nullptr && target != origin_ && target->isOutput() != origin_->isOutput()) {
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
// The 12 touch plates. Click/drag to play (lower on the plate = more pressure); the computer
// keyboard plays them too.
class PlatesControl : public IControl {
 public:
  PlatesControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s) {}

  void Draw(IGraphics& g) override {
    static const char* kNote[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const float pw = mRECT.W() / 12.f;
    for (int i = 0; i < 12; ++i) {
      const IRECT p(mRECT.L + i * pw + 4, mRECT.T, mRECT.L + (i + 1) * pw - 4, mRECT.B);
      const bool lit = lit_.count(i) > 0;
      g.FillRoundRect(col(lit ? theme::kPlateLit : theme::kPlate), p, 8);
      g.DrawRoundRect(col(lit ? theme::accent(Accent::Keyboard) : theme::kCardEdge), p, 8, nullptr, lit ? 2.f : 1.f);
      g.DrawText(txt(14, lit ? theme::kText : theme::kTextDim), kNote[i], p.MW(), p.B - 16);
    }
    char oct[32];
    std::snprintf(oct, sizeof oct, "OCTAVE %+d   (Z / X)", octave_);
    g.DrawText(txt(11, theme::kTextDim), oct, mRECT.MW(), mRECT.T + 14);
  }

  void OnMouseDown(float x, float y, const IMouseMod&) override { mouseNote(plateAt(x), y); }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override {
    const int p = plateAt(x);
    if (p != mousePlate_) mouseNote(p, y);
    else if (mousePlate_ >= 0) s_.pressure(kMouseId, pressureAt(y), kMouseSource);
  }
  void OnMouseUp(float, float, const IMouseMod&) override { mouseNote(-1, 0); }

  // Computer keyboard: returns true if the key was used.
  bool key(const IKeyPress& k, bool up) {
    static const char kKeys[] = "awsedftgyhujkolp;";
    const char c = char(std::tolower(static_cast<unsigned char>(k.utf8[0])));
    if (!up && (c == 'z' || c == 'x')) {
      octave_ = std::clamp(octave_ + (c == 'x' ? 1 : -1), -3, 3);
      SetDirty(false);
      return true;
    }
    const char* p = c ? std::strchr(kKeys, c) : nullptr;
    if (p == nullptr) return false;
    const int semi = int(p - kKeys);
    const core::NoteId id = static_cast<core::NoteId>(kKeyIdBase + static_cast<core::NoteId>(semi));
    if (!up && heldKeys_.count(semi) == 0) {
      heldKeys_[semi] = semi + 12 * octave_;
      s_.note(true, heldKeys_[semi], id, 0.8, kKeySource);
      lit_.insert(semi % 12);
    } else if (up && heldKeys_.count(semi) > 0) {
      s_.note(false, heldKeys_[semi], id, 0.0, kKeySource);
      heldKeys_.erase(semi);
      lit_.erase(semi % 12);
    }
    SetDirty(false);
    return true;
  }

 private:
  static constexpr core::NoteId kMouseId = 1000;
  static constexpr core::NoteId kKeyIdBase = 2000;
  static constexpr core::ControlSourceId kMouseSource = 1;
  static constexpr core::ControlSourceId kKeySource = 2;

  int plateAt(float x) const {
    if (x < mRECT.L || x >= mRECT.R) return -1;
    return std::clamp(int((x - mRECT.L) / (mRECT.W() / 12.f)), 0, 11);
  }
  double pressureAt(float y) const { return std::clamp(double((y - mRECT.T) / mRECT.H()), 0.1, 1.0); }
  void mouseNote(int plate, float y) {
    if (mousePlate_ >= 0) {
      s_.note(false, mousePlate_ + 12 * octave_, kMouseId, 0.0, kMouseSource);
      lit_.erase(mousePlate_);
    }
    mousePlate_ = plate;
    if (plate >= 0) {
      s_.note(true, plate + 12 * octave_, kMouseId, pressureAt(y), kMouseSource);
      lit_.insert(plate);
    }
    SetDirty(false);
  }
  EditorShared& s_;
  int mousePlate_ = -1;
  int octave_ = 0;
  std::map<int, int> heldKeys_;  // key semitone -> sounding semitone (octave at press time)
  std::set<int> lit_;
};

// ---------------------------------------------------------------------------------------------
class JoystickControl : public IControl {
 public:
  JoystickControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    const theme::Rgb a = theme::accent(w_.accent);
    g.FillRoundRect(col(theme::kKnobBody), mRECT, 8);
    g.DrawRoundRect(col(a, 180), mRECT, 8, nullptr, 1.5f);
    g.DrawLine(col(theme::kCardEdge), mRECT.MW(), mRECT.T + 6, mRECT.MW(), mRECT.B - 6);
    g.DrawLine(col(theme::kCardEdge), mRECT.L + 6, mRECT.MH(), mRECT.R - 6, mRECT.MH());
    const float x = mRECT.L + float(s_.value(w_.id)) * mRECT.W();
    const float y = mRECT.B - float(s_.value(w_.id2)) * mRECT.H();
    g.FillCircle(col(a), x, y, 9);
  }
  void OnMouseDown(float x, float y, const IMouseMod&) override { move(x, y); }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override { move(x, y); }
  void OnMouseDblClick(float, float, const IMouseMod&) override {
    s_.set(w_.id, 0.5);
    s_.set(w_.id2, 0.5);
    SetDirty(false);
  }

 private:
  void move(float x, float y) {
    s_.set(w_.id, std::clamp(double((x - mRECT.L) / mRECT.W()), 0.0, 1.0));
    s_.set(w_.id2, std::clamp(double((mRECT.B - y) / mRECT.H()), 0.0, 1.0));
    SetDirty(false);
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// An effector cartridge slot: the arrows swap the cartridge, the PROGRAM switch picks 1-2-3.
class CartridgeControl : public IControl {
 public:
  CartridgeControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    const theme::Rgb a = theme::accent(w_.accent);
    g.FillRoundRect(col(theme::kKnobBody), mRECT, 8);
    g.DrawRoundRect(col(a, 200), mRECT, 8, nullptr, 1.5f);
    const core::ProgramDescriptor* p = core::find_program(static_cast<core::ProgramId>(cartridge() * 3 + select()));
    g.DrawText(txt(10, theme::kTextDim), (w_.label + " CARTRIDGE").c_str(), mRECT.MW(), mRECT.T + 18);
    g.DrawText(txt(18, a, true), p ? std::string(p->cartridge).c_str() : "?", mRECT.MW(), mRECT.T + 48);
    g.DrawText(txt(12, theme::kText), p ? std::string(p->name).c_str() : "", mRECT.MW(), mRECT.T + 74);
    g.DrawText(txt(18, mMouseIsOver ? theme::kText : theme::kTextDim), "<", mRECT.L + 14, mRECT.T + 48);
    g.DrawText(txt(18, mMouseIsOver ? theme::kText : theme::kTextDim), ">", mRECT.R - 14, mRECT.T + 48);
  }
  void OnMouseDown(float x, float, const IMouseMod&) override {
    const int step = x < mRECT.MW() ? -1 : 1;
    const int next = (cartridge() + step + 13) % 13;
    s_.engine.postEffectorProgram(int(w_.id), static_cast<core::ProgramId>(next * 3));
    SetDirty(false);
  }

 private:
  int cartridge() const {
    const core::DeviceStateV1* st = s_.state();
    if (st == nullptr) return 0;
    return int(static_cast<std::uint32_t>(w_.id == 0 ? st->leftEffector.program : st->rightEffector.program) / 3u);
  }
  int select() const {
    return int(std::lround(s_.value(static_cast<std::uint32_t>(
        w_.id == 0 ? ParameterId::effector_select_l : ParameterId::effector_select_r))));
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
class DroneKeyControl : public IControl {
 public:
  DroneKeyControl(EditorShared& s, const Widget& w)
      : IControl(IRECT(float(w.x), float(w.y), float(w.x + w.w), float(w.y + w.h))), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    const theme::Rgb a = theme::accent(w_.accent);
    const bool open = s_.engine.droneKey(int(w_.id));
    g.FillRoundRect(open ? col(a, 80) : col(theme::kPlate), mRECT, 8);
    g.DrawRoundRect(col(a, open ? 255 : 120), mRECT, 8, nullptr, open ? 2.f : 1.f);
    g.DrawText(txt(20, open ? theme::kText : a, true), w_.label.c_str(), mRECT);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.engine.postDroneKey(int(w_.id), !s_.engine.droneKey(int(w_.id)));
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// Build the whole panel into `g`. `shared` must outlive the editor.
inline void BuildPanel(IGraphics* g, EditorShared& shared) {
#if defined(OS_WIN)
  const char* face = "Segoe UI";
#else
  const char* face = "Helvetica Neue";
#endif
  g->LoadFont(kFont, face, ETextStyle::Normal);
  g->LoadFont(kFontBold, face, ETextStyle::Bold);
  g->AttachPanelBackground(col(theme::kBackground));

  const std::vector<Widget> widgets = build_panel_layout();
  std::vector<Widget> cards;
  for (const Widget& w : widgets)
    if (w.kind == WidgetKind::Module || w.kind == WidgetKind::Title) cards.push_back(w);
  const IRECT all = g->GetBounds();
  g->AttachControl(new BackgroundControl(all, cards));

  shared.jacks.clear();
  shared.jackRects.clear();
  for (const Widget& w : widgets) {
    switch (w.kind) {
      case WidgetKind::Knob: g->AttachControl(new KnobControl(shared, w)); break;
      case WidgetKind::Selector: g->AttachControl(new SelectorControl(shared, w)); break;
      case WidgetKind::Jack: {
        auto* j = new JackControl(shared, w);
        shared.jacks.push_back(j);
        shared.jackRects[w.id] = j->GetRECT();
        g->AttachControl(j);
        break;
      }
      case WidgetKind::Plates: {
        auto* p = new PlatesControl(shared, w);
        shared.plates = p;
        g->AttachControl(p);
        break;
      }
      case WidgetKind::Joystick: g->AttachControl(new JoystickControl(shared, w)); break;
      case WidgetKind::Cartridge: g->AttachControl(new CartridgeControl(shared, w)); break;
      case WidgetKind::DroneKey: g->AttachControl(new DroneKeyControl(shared, w)); break;
      case WidgetKind::Module:
      case WidgetKind::Title: break;
    }
  }
  shared.cables = new CableLayer(shared, all);
  g->AttachControl(shared.cables);

  g->SetKeyHandlerFunc([&shared](const IKeyPress& key, bool isUp) {
    return shared.plates != nullptr && shared.plates->key(key, isUp);
  });
  // Redraw everything when a whole new machine state lands (startup restore, preset load).
  g->SetDisplayTickFunc([&shared, g]() {
    if (shared.engine.stateVersion() != shared.seenStateVersion) {
      shared.seenStateVersion = shared.engine.stateVersion();
      g->SetAllControlsDirty();
    }
  });
}

}  // namespace lunar24::host::ui
