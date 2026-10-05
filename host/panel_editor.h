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
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "IControl.h"
#include "IGraphics.h"

#include <host/panel_art.h>
#include <host/ui_font.generated.h>
#include <host/keyboard_menu_view.h>
#include <host/midi_map_store.h>
#include <host/midi_settings_view.h>
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
inline IRECT drawRectOf(const Widget& w) {
  const float pad = std::max(14.f, float(std::max(w.w, w.h)) * 0.3f);
  return rectOf(w).GetPadded(pad);
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
  std::vector<IControl*> menuControls;       // the keyboard menu overlay (KeyboardMenuControl)
  int menuPage = 0;                          // keyboard menu tab: kb_ui::Tab (0 = PLAY)
  int seqSide = 0;                           // menu side being edited under SPLIT: 0 = left, 1 = right
  int presetSlot = 0;                        // keyboard preset the LOAD / SAVE / INIT buttons act on: 0..3 = A..D
  std::function<void()> factoryReset;        // the plugin's requestFactoryReset (RESET PANEL)
  std::vector<IControl*> plates;
  bool menuOpen = false;
  int octave = 0;
  std::set<int> lit;                         // plates currently sounding (0..11)
  std::map<int, int> heldKeys;               // computer key semitone -> sounding semitone
  int mousePlate = -1, mouseSemi = 0;        // the plate the mouse holds (-1 = none) and its semitone
  int tunePlate = -1;                        // plate last tuned while held (the display shows it)
  double tuneWheelAcc = 0.0;                 // wheel travel not yet a whole notch
  // Value readout for the knob under the mouse (drawn by the top layer).
  std::string readout;
  float readoutX = 0, readoutY = 0;
  core::InputStateMachine input{nullptr, 0};
  std::uint64_t seq = 0;
  std::uint64_t seenStateVersion = ~0ull;
  bool seenReady = false;
  bool seenMuted = false;
  std::uint16_t seenMidiPlates = 0;  // the plates MIDI was lighting at the last display tick

  static constexpr core::NoteId kMouseId = 1000;
  static constexpr core::NoteId kKeyIdBase = 2000;
  static constexpr core::ControlSourceId kMouseSource = 1;
  static constexpr core::ControlSourceId kKeySource = 2;

  const core::DeviceStateV1* state() const { return engine.canonicalState(); }
  // ---- MIDI settings overlay (the panel's MIDI button) -----------------------------
  // The store is the plugin's; the bridges keep this header free of plugin types.
  host::MidiMapStore* midiStore = nullptr;
  struct MidiUi {
    std::function<std::string()> inputDeviceName;
    std::function<std::uint64_t()> messageSeq;        // bumps per incoming note/CC
    std::function<std::uint32_t()> lastMessage;       // packed value/kind/channel/number
    std::function<std::uint16_t()> litPlates;         // plates MIDI notes hold down (bit 0 = C)
    std::function<int()> channelFilter;
    std::function<int()> octaveShift;
    std::function<int()> velocityCurve;
    std::function<int()> splitNote;
    std::function<void(int, int, int, int)> setRigSettings;
    std::function<void()> bindingsChanged;            // save + republish
  };
  MidiUi midi;
  // REC (the plugin owns the recorder; the source choice lives here for the session).
  struct RecordUi {
    std::function<bool()> recording;
    std::function<double()> seconds;
    std::function<void(int)> toggle;  // start with this source (0 WET, 1 DRY, 2 ALL), or stop
  };
  RecordUi rec;
  int recordSource = 0;
  IControl* recordControl = nullptr;
  int recordShownSecond = -1;
  bool isRecording() const { return rec.recording && rec.recording(); }
  std::vector<IControl*> midiControls;
  bool midiOpen = false;
  void showMidi(bool open) {
    midiOpen = open;
    for (IControl* c : midiControls) c->Hide(!open);
    if (open) showMenu(false);
    if (!open) setLearnArmed(false, false);
  }
  // Learn: armed while the user picks a panel target and then moves a hardware control.
  IControl* learnCapture = nullptr;  // the full-panel click catcher (mouse-ignored unless picking)
  bool learnArmed = false;
  bool learnAwaitTarget = false;
  bool learnTargetIsAction = false;
  core::ParameterId learnParameter = core::ParameterId{0};
  core::MidiAction learnAction = core::MidiAction::master_mute;
  std::uint64_t learnArmSeq = 0;
  int midiListOffset = 0;  // bindings table paging
  void setLearnArmed(bool armed, bool awaitTarget) {
    learnArmed = armed;
    learnAwaitTarget = awaitTarget;
    if (learnCapture != nullptr) learnCapture->SetIgnoreMouse(!(armed && awaitTarget));
  }
  // Called from the display tick: a hardware message arrived while awaiting one.
  void pollLearn() {
    if (!learnArmed || learnAwaitTarget || midiStore == nullptr) return;
    if (!midi.messageSeq || midi.messageSeq() == learnArmSeq) return;
    const std::uint32_t m = midi.lastMessage();
    core::MidiBinding b{};
    b.key.kind = ((m >> 20) & 1u) ? core::MidiBindingKind::cc : core::MidiBindingKind::note;
    b.key.channel = static_cast<std::uint8_t>((m >> 8) & 0x1Fu);
    b.key.number = static_cast<std::uint8_t>(m & 0xFFu);
    if (b.key.kind == core::MidiBindingKind::cc && b.key.number == 64) return;
    const std::string dev = midi.inputDeviceName ? midi.inputDeviceName() : "";
    std::snprintf(b.key.device, core::kMidiBindingDeviceCapacity, "%s", dev.c_str());
    b.targetKind = learnTargetIsAction ? core::MidiTargetKind::action : core::MidiTargetKind::parameter;
    b.parameter = learnParameter;
    b.action = learnAction;
    if (midiStore->bind(b)) {
      const int row = midiStore->map().find(b.key);
      midiListOffset = midi_ui::pageOffset(row, static_cast<int>(midiStore->map().count()));
      if (midi.bindingsChanged) midi.bindingsChanged();
      // A learned knob: watch its next values for a relative encoder (MPK KnobM = Rel).
      detectActive = b.key.kind == core::MidiBindingKind::cc && row >= 0 &&
                     core::midi_parameter_drive(midiStore->map().at(static_cast<std::uint32_t>(row))) ==
                         core::MidiParameterDrive::follow;
      detectKey = b.key;
      detector = core::MidiRelativeDetector{};
      detectSeq = midi.messageSeq();
      if (detectActive) (void)detector.feed(static_cast<int>((m >> 21) & 0x7Fu));
    }
    setLearnArmed(false, false);
  }
  // Display tick after a learn: switch the new binding to the relative mode its values show.
  // Returns true when the binding changed.
  bool detectActive = false;
  core::MidiBindingKey detectKey{};
  core::MidiRelativeDetector detector;
  std::uint64_t detectSeq = 0;
  bool pollRelativeDetect() {
    if (!detectActive || midiStore == nullptr || !midi.messageSeq) return false;
    const std::uint64_t seqNow = midi.messageSeq();
    if (seqNow == detectSeq) return false;
    detectSeq = seqNow;
    const std::uint32_t m = midi.lastMessage();
    if (((m >> 20) & 1u) == 0 || ((m >> 8) & 0x1Fu) != detectKey.channel || (m & 0xFFu) != detectKey.number)
      return false;
    const core::MidiInputMode mode = detector.feed(static_cast<int>((m >> 21) & 0x7Fu));
    if (mode == core::MidiInputMode::absolute) {
      if (detector.exhausted()) detectActive = false;
      return false;
    }
    detectActive = false;
    const int row = midiStore->map().find(detectKey);
    if (row < 0) return false;
    auto edited = midiStore->map().at(static_cast<std::uint32_t>(row));
    if (edited.mode != core::MidiInputMode::absolute) return false;  // the user already chose one
    edited.mode = mode;
    if (!midiStore->bind(edited)) return false;
    if (midi.bindingsChanged) midi.bindingsChanged();
    return true;
  }
  // Widgets a click can bind while learning (built once at panel build).
  struct MidiBindable {
    double x0, y0, x1, y1;
    bool isAction;
    core::ParameterId param;
    core::MidiAction action;
  };
  std::vector<MidiBindable> midiBindables;
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
    // Route by parameter ownership, not by admission success: a rejected
    // right-side edit must never fall through and change the left bank.
    if (editSide() == 1 && core::keyboard_scalar_index(static_cast<ParameterId>(id)) >= 0) {
      engine.postKeyboardRightParameter(static_cast<ParameterId>(id), v);
      return;
    }
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
    in.plate = static_cast<std::uint8_t>(((plate % 12) + 12) % 12);  // the upper computer keys are plates 0-4
    core::ControlEvent ev[3];
    const std::uint32_t n = input.translate(in, ev, 3);
    for (std::uint32_t i = 0; i < n; ++i) engine.postEvent(ev[i]);
  }

  // ---- per-plate tuning (PLATE EDITOR, manual p.15): hold plates, turn the encoder -----------
  // The plates held by the mouse or the computer keys right now: {plate, sounding semitone, id, source}.
  struct HeldPlate { int plate, semi; core::NoteId id; core::ControlSourceId source; };
  std::vector<HeldPlate> heldPlates() const {
    std::vector<HeldPlate> h;
    if (mousePlate >= 0) h.push_back({mousePlate, mouseSemi, kMouseId, kMouseSource});
    for (const auto& [key, semi] : heldKeys)
      h.push_back({key % 12, semi, static_cast<core::NoteId>(kKeyIdBase + static_cast<core::NoteId>(key)), kKeySource});
    return h;
  }
  // Add `delta` semitones to one plate's tuning (reset = true sets it to 0), show it on the
  // display, and send the pitch of any held note on it again so it moves at once.
  void tunePlateBy(int plate, double delta, bool reset = false) {
    const double t = reset ? 0.0 : engine.keyboardPlateTune(plate) + delta;
    engine.postKeyboardPlateTune(plate, std::round(t * 100.0) / 100.0);  // whole cents
    tunePlate = plate;
    for (const HeldPlate& h : heldPlates()) {
      if (h.plate != plate) continue;
      core::ControlEvent ev{};
      ev.kind = core::ControlEventKind::pitch;
      ev.value = static_cast<core::SignalSample>((h.semi - 9) / 12.0);
      ev.source = h.source;
      ev.noteId = h.id;
      ev.side = sideOf(h.plate);
      ev.plate = static_cast<std::uint8_t>(h.plate);
      engine.postEvent(ev);
    }
  }
  // The same for every held plate. False when no plate is held.
  bool tuneHeldPlates(double delta, bool reset = false) {
    std::set<int> plates;
    for (const HeldPlate& h : heldPlates()) plates.insert(h.plate);
    for (int p : plates) tunePlateBy(p, delta, reset);
    return !plates.empty();
  }
  // Wheel travel -> semitones: 10 cents a notch, a semitone when `coarse` (Option, or Shift:
  // macOS may turn Shift+wheel sideways); ENCODER DIRECTION reverses it. A trackpad's small
  // steps add up to notches. // tuned by ear
  double wheelTuneSteps(float d, bool coarse) {
    tuneWheelAcc += d;
    const int notches = int(tuneWheelAcc);
    tuneWheelAcc -= notches;
    const double dir = engine.parameterValue(ParameterId::keyboard_encoder_direction) > 0.5 ? -1.0 : 1.0;
    return dir * notches * (coarse ? 1.0 : 0.1);
  }
  // A wheel turn while plates are held (computer keys, or the mouse): tunes them. False when no
  // plate is held.
  bool wheelTuneHeldPlates(float d, bool coarse) {
    if (heldPlates().empty()) {
      tuneWheelAcc = 0.0;
      return false;
    }
    const double steps = wheelTuneSteps(d, coarse);
    if (steps != 0.0) tuneHeldPlates(steps);
    return true;
  }
  // The display shows the plate being tuned until every plate is let go.
  void platesMaybeReleased() {
    if (heldPlates().empty()) tunePlate = -1;
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
    if (open) showMidi(false);
    menuOpen = open;
    for (IControl* c : menuControls) c->Hide(!open);
  }
  const core::KeyboardSeqStep* seqStep(int i) const {
    const core::DeviceStateV1* st = state();
    if (st == nullptr || i < 0 || i >= kb_ui::kSeqSteps) return nullptr;
    return &(editSide() == 0 ? st->keyboardSeqCurrent : st->keyboardSeqCurrentR).steps[static_cast<std::size_t>(i)];
  }

  // Computer keyboard: returns true if the key was used.
  bool key(const IKeyPress& k, bool up) {
    static const char kKeys[] = "awsedftgyhujkolp;";
    if (k.VK == kVK_ESCAPE && midiOpen) {
      if (!up) showMidi(false);  // also cancels an armed learn
      return true;
    }
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
      platesMaybeReleased();
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
                hexOf(theme::kSkirt), float(theme::kKnobMinDeg), float(theme::kKnobMaxDeg), hover);
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
    drawKnob(g, w_, core::value_to_knob(*d, s_.value(w_.id)), mMouseIsOver || dragging_);
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
    // Move the knob, not the value: time and rate knobs are tapered (knob_taper.h).
    s_.set(w_.id, core::knob_to_value(*d, core::value_to_knob(*d, s_.value(w_.id)) + fractionOfRange));
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
// lever up, the lower half to move it down.
class ToggleControl : public IControl {
 public:
  ToggleControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    const core::ParameterDescriptor* d = desc(w_.id);
    const int n = positionsOf(d), idx = std::clamp(s_.index(w_.id), 0, n - 1);
    const int pos = leverPos(idx, n);
    GraphicsSink sink{g};
    art::drawToggle(sink, float(w_.cx), float(w_.cy), n <= 1 ? 0.f : float(pos) / float(n - 1), mMouseIsOver);
  }
  void OnMouseDown(float, float y, const IMouseMod&) override {
    const int n = positionsOf(desc(w_.id)), idx = s_.index(w_.id);
    const int pos = std::clamp(leverPos(idx, n) + (y < float(w_.cy) ? -1 : 1), 0, n - 1);
    // An effector 1-2-3 switch: flipping it loads the slot's cartridge into its side first, and
    // the cartridge slot's L / R lines redraw.
    const bool fx = w_.id == static_cast<std::uint32_t>(ParameterId::effector_select_l) ||
                    w_.id == static_cast<std::uint32_t>(ParameterId::effector_select_r);
    if (fx) s_.engine.loadSlotCartridge(w_.id == static_cast<std::uint32_t>(ParameterId::effector_select_l) ? 0 : 1);
    s_.setIndex(w_.id, w_.leverIndex[pos]);
    if (fx) GetUI()->SetAllControlsDirty();
    else SetDirty(false);
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
      // The box grows with its text (a cartridge's two program names can be long), never
      // narrower than a knob value's, and stays inside the panel.
      const IText style = txt(16, theme::kMenuText);
      IRECT measured;
      g.MeasureText(style, s_.readout.c_str(), measured);
      const float half = std::max(52.f, 0.5f * measured.W() + 12.f);
      const float cx = std::clamp(s_.readoutX, half + 4.f, 2400.f - half - 4.f);
      const IRECT r(cx - half, s_.readoutY - 14, cx + half, s_.readoutY + 14);
      g.FillRoundRect(col(theme::kMenuBg, 235), r, 5.f);
      g.DrawText(style, s_.readout.c_str(), r);
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
    if (!overlay_hides_point(s_.menuOpen || s_.midiOpen, x, y))
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
    const bool midiLit = ((s_.seenMidiPlates >> w_.id) & 1u) != 0;  // played on a MIDI keyboard
    art::drawPlate(sink, mRECT.L, mRECT.T, mRECT.R, mRECT.B, s_.lit.count(int(w_.id)) > 0 || midiLit);
  }
  void OnMouseDown(float, float y, const IMouseMod& mod) override {
    if (mod.R || mod.C) {  // Command-click (Ctrl-click on Windows): this plate's tuning back to 0
      s_.tunePlateBy(int(w_.id), 0.0, true);
      GetUI()->SetAllControlsDirty();
      return;
    }
    semi_ = int(w_.id) + 12 * s_.octave;
    s_.note(true, semi_, EditorShared::kMouseId, pressureAt(y), EditorShared::kMouseSource, int(w_.id));
    s_.lit.insert(int(w_.id));
    s_.mousePlate = int(w_.id);
    s_.mouseSemi = semi_;
    SetDirty(false);
  }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override {
    if (semi_ == kNone) return;
    if (!mRECT.Contains(x, y)) release();
    else s_.pressure(EditorShared::kMouseId, pressureAt(y), EditorShared::kMouseSource, int(w_.id));
  }
  void OnMouseUp(float, float, const IMouseMod&) override { release(); }
  // Command (Ctrl on Windows) + wheel over a plate tunes that plate, held or not: a trackpad
  // cannot scroll while it holds a click. Without Command the wheel tunes the held plates.
  // (iPlug2 reports Command as mod.R on macOS.)
  void OnMouseWheel(float, float, const IMouseMod& mod, float d) override {
    if (mod.R || mod.C) {
      const double steps = s_.wheelTuneSteps(d, mod.A || mod.S);
      if (steps != 0.0) s_.tunePlateBy(int(w_.id), steps);
      GetUI()->SetAllControlsDirty();
      return;
    }
    if (s_.wheelTuneHeldPlates(d, mod.A || mod.S)) GetUI()->SetAllControlsDirty();
  }
  void OnMouseOut() override {
    IControl::OnMouseOut();
    if (s_.tunePlate >= 0 && s_.heldPlates().empty()) {  // done tuning by hovering
      s_.tunePlate = -1;
      GetUI()->SetAllControlsDirty();
    }
  }

 private:
  static constexpr int kNone = -1000;
  void release() {
    if (semi_ == kNone) return;
    s_.note(false, semi_, EditorShared::kMouseId, 0.0, EditorShared::kMouseSource, int(w_.id));
    s_.lit.erase(int(w_.id));
    semi_ = kNone;
    s_.mousePlate = -1;
    const bool wasTuning = s_.tunePlate >= 0;
    s_.platesMaybeReleased();
    if (wasTuning) GetUI()->SetAllControlsDirty();  // the display goes back to OCT
    else SetDirty(false);
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
    art::drawJoystick(sink, cx, cy, 55.f, x, y, mMouseIsOver);
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
// The effector cartridge slot (id 0) and its button (id 1). As on the hardware, clicking either
// puts the next cartridge in the slot (right-click or Shift = previous) without loading it;
// flipping a side's 1-2-3 switch loads the slot's cartridge into that side (ToggleControl).
// The slot shows the cartridge in it on the label, and under it what each side is running.
class CartridgeControl : public IControl {
 public:
  CartridgeControl(EditorShared& s, const Widget& w) : IControl(rectOf(w)), s_(s), w_(w) {}
  void Draw(IGraphics& g) override {
    if (w_.id == 1) {
      g.FillCircle(col(theme::kInk), float(w_.cx), float(w_.cy), 16);
      g.FillCircle(col(mMouseIsOver ? theme::kAmber : theme::Rgb{60, 60, 60}), float(w_.cx), float(w_.cy), 10);
      return;
    }
    const IRECT r = rectOf(w_);
    g.FillRect(col(theme::kInk), r);
    const IRECT label(r.L + 7, r.T + 5, r.R - 7, r.T + 23);
    g.FillRect(col(mMouseIsOver ? theme::Rgb{190, 142, 58} : theme::Rgb{160, 118, 46}), label);
    g.DrawText(txt(12, theme::kInk), cartridgeName(s_.engine.slotCartridge()).c_str(), label);
    const theme::Rgb lit{235, 190, 110};
    g.DrawText(txt(11, lit, false), sideLine(0).c_str(), IRECT(r.L, r.T + 27, r.R, r.T + 43));
    g.DrawText(txt(11, lit, false), sideLine(1).c_str(), IRECT(r.L, r.T + 44, r.R, r.T + 60));
  }
  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    IControl::OnMouseOver(x, y, mod);
    std::string tip = sideTip(0) + "    " + sideTip(1);
    const int slot = s_.engine.slotCartridge();
    if (slot != sideCartridge(0) || slot != sideCartridge(1))
      tip += "    SLOT: " + cartridgeName(slot) + " (flip L / R to load)";
    s_.readout = tip;
    s_.readoutX = float(w_.cx);
    s_.readoutY = float(w_.y() - 22);
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseOut() override {
    IControl::OnMouseOut();
    s_.readout.clear();
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    s_.engine.stepSlotCartridge((mod.R || mod.S) ? -1 : 1);
    OnMouseOver(x, y, mod);
  }

 private:
  static std::string cartridgeName(int cart) {
    const core::ProgramDescriptor* p = core::find_program(static_cast<core::ProgramId>(cart * 3));
    return p ? upper(std::string(p->cartridge)) : std::string();
  }
  int sideCartridge(int side) const {
    const core::DeviceStateV1* st = s_.state();
    if (st == nullptr) return 0;
    return int(static_cast<std::uint32_t>((side == 0 ? st->leftEffector : st->rightEffector).program) / 3u);
  }
  int sideSelect(int side) const {
    return s_.index(static_cast<std::uint32_t>(side == 0 ? ParameterId::effector_select_l
                                                          : ParameterId::effector_select_r));
  }
  // "L  CATHEDRAL 2": the side, its cartridge and its 1-2-3 switch.
  std::string sideLine(int side) const {
    return std::string(side == 0 ? "L  " : "R  ") + cartridgeName(sideCartridge(side)) + " " +
           std::to_string(sideSelect(side) + 1);
  }
  // "L: CATHEDRAL 2 - <program name>".
  std::string sideTip(int side) const {
    const core::ProgramDescriptor* p = core::find_program(
        static_cast<core::ProgramId>(sideCartridge(side) * 3 + sideSelect(side)));
    return std::string(side == 0 ? "L: " : "R: ") + cartridgeName(sideCartridge(side)) + " " +
           std::to_string(sideSelect(side) + 1) + " - " + (p ? std::string(p->name) : "?");
  }
  EditorShared& s_;
  Widget w_;
};

// ---------------------------------------------------------------------------------------------
// Keyboard encoder (opens the keyboard menu), octave arrows, display. While the menu is open
// it covers the encoder and takes its clicks: CLOSE or Esc closes it.
class EncoderControl : public IControl {
 public:
  EncoderControl(EditorShared& s, const Widget& w) : IControl(drawRectOf(w)), s_(s), w_(w) { SetTargetRECT(rectOf(w)); }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawEncoder(sink, float(w_.cx), float(w_.cy), s_.menuOpen || mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    // Holding plates: a click sets their tuning back to 0. Otherwise it opens the menu.
    if (!s_.tuneHeldPlates(0.0, true)) s_.showMenu(!s_.menuOpen);
    GetUI()->SetAllControlsDirty();
  }
  void OnMouseWheel(float, float, const IMouseMod& mod, float d) override {
    if (s_.wheelTuneHeldPlates(d, mod.A || mod.S)) {  // holding plates: the wheel tunes them
      GetUI()->SetAllControlsDirty();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double gap = std::chrono::duration<double>(now - lastWheel_).count();
    lastWheel_ = now;
    const int step = wheel_.step(d, gap, s_.engine.parameterValue(ParameterId::keyboard_encoder_direction));
    if (step == 0) return;
    s_.shiftOctave(step);
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
  Widget w_;
  EncoderWheel wheel_;
  std::chrono::steady_clock::time_point lastWheel_{};
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
    else if (s_.tunePlate >= 0) {  // the plate being tuned: its name and offset in semitones
      static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      std::snprintf(b, sizeof b, "%s %+.2f", kNames[s_.tunePlate % 12], s_.engine.keyboardPlateTune(s_.tunePlate));
    } else std::snprintf(b, sizeof b, "OCT %+d", s_.octave);
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
    s_.engine.toggleMuted();
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// The MIDI button below MUTE: opens the MIDI settings overlay. Amber ring while open.
class MidiSettingsControl : public IControl {
 public:
  MidiSettingsControl(EditorShared& s, const Widget& w)
      : IControl(rectOf(w).GetPadded(14.f).Union(IRECT(float(w.cx - 40), float(w.cy - 56), float(w.cx + 40),
                                                       float(w.cy)))),
        s_(s), w_(w) {
    SetTargetRECT(rectOf(w));
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawButton(sink, float(w_.cx), float(w_.cy), float(w_.w / 2), s_.midiOpen, mMouseIsOver);
    g.DrawText(txt(15, theme::kInk), "MIDI", float(w_.cx), float(w_.cy - 42));
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    s_.showMidi(!s_.midiOpen);
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// REC, in the headphone socket's place: starts / stops recording the chosen source to WAV.
// The label above shows the elapsed time while recording. Not on the hardware.
class RecordControl : public IControl {
 public:
  RecordControl(EditorShared& s, const Widget& w)
      : IControl(rectOf(w).GetPadded(14.f).Union(IRECT(float(w.cx - 30), float(w.cy - 68), float(w.cx + 30),
                                                       float(w.cy)))),
        s_(s), w_(w) {
    SetTargetRECT(rectOf(w));
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    const bool on = s_.isRecording();
    art::drawRecordButton(sink, float(w_.cx), float(w_.cy), float(w_.w / 2), on,
                          on && s_.rec.seconds ? int(s_.rec.seconds()) : 0, mMouseIsOver);
  }
  void OnMouseDown(float, float, const IMouseMod&) override {
    if (s_.rec.toggle) s_.rec.toggle(s_.recordSource);
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// REC's source, in the PHONE knob's place: click for the next of WET / DRY / ALL (right-click:
// previous). Fixed while recording.
class RecordSourceControl : public IControl {
 public:
  RecordSourceControl(EditorShared& s, const Widget& w)
      : IControl(drawRectOf(w).Union(IRECT(float(w.cx - 44), float(w.cy - w.h / 2 - 26), float(w.cx + 44),
                                           float(w.cy)))),
        s_(s), w_(w) {
    SetTargetRECT(rectOf(w).Union(IRECT(float(w.cx - 40), float(w.cy - w.h / 2 - 22), float(w.cx + 40),
                                        float(w.cy - w.h / 2))));
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    art::drawRecordSource(sink, float(w_.cx), float(w_.cy), float(w_.w / 2), s_.recordSource,
                          hexOf(theme::cap(w_.cap)), mMouseIsOver && !s_.isRecording());
  }
  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (s_.isRecording()) return;
    s_.recordSource = (s_.recordSource + ((mod.R || mod.S) ? 2 : 1)) % 3;
    SetDirty(false);
  }

 private:
  EditorShared& s_;
  Widget w_;
};

// A classic drone's photo sensor, played with the mouse as a hand: press to bring the hand
// over the eye, drag up to bring it closer (darker) and down to lift it, drag sideways to
// sweep the fingers across it (the light flickers between them); release to take the hand
// away. The dome shows what the sensor sees. The light model is core/photo_sensor.h.
class PhotoSensorControl : public IControl {
 public:
  PhotoSensorControl(EditorShared& s, const Widget& w)
      : IControl(drawRectOf(w)), s_(s), group_(static_cast<int>(w.id)), cx_(float(w.cx)), cy_(float(w.cy)),
        r_(float(w.w / 2)) {
    SetTargetRECT(rectOf(w));
  }
  bool IsDirty() override {
    return std::fabs(s_.engine.photoLight(group_) - shown_) > 0.01f || IControl::IsDirty();
  }
  void Draw(IGraphics& g) override {
    shown_ = s_.engine.photoLight(group_);
    GraphicsSink sink{g};
    art::drawSensor(sink, cx_, cy_, r_, shown_, pressed_ ? 0.4f + 0.6f * depth_ : 0.f, handX_, handY_);
  }
  void OnMouseDown(float x, float y, const IMouseMod&) override {
    pressed_ = true;
    startX_ = x;
    startY_ = y;
    depth_ = kStartDepth;
    move_(x, y);
  }
  void OnMouseDrag(float x, float y, float, float, const IMouseMod&) override {
    depth_ = std::clamp(kStartDepth + (startY_ - y) / kDepthTravel, 0.f, 1.f);
    move_(x, y);
  }
  void OnMouseUp(float, float, const IMouseMod&) override {
    pressed_ = false;
    (void)s_.engine.postPhotoShade(group_, 0.0);
    SetDirty(false);
  }

 private:
  static constexpr float kStartDepth = 0.55f;   // a press: the hand hovering over the eye // tuned by ear
  static constexpr float kDepthTravel = 120.f;  // drag this far up for a full cover
  static constexpr float kFingerSpacing = 26.f;  // sideways travel from one finger to the next
  void move_(float x, float y) {
    handX_ = std::clamp(x, cx_ - r_, cx_ + r_);
    handY_ = std::clamp(y, cy_ - r_, cy_ + r_);
    // Fingers: moving sideways passes gaps between them over the eye.
    const float fingers = 0.6f + 0.4f * std::cos(6.2831853f * (x - startX_) / kFingerSpacing);  // tuned by ear
    (void)s_.engine.postPhotoShade(group_, double(depth_ * fingers));
    SetDirty(false);
  }

  EditorShared& s_;
  int group_;
  float cx_, cy_, r_;
  float shown_ = -1.f;
  bool pressed_ = false;
  float startX_ = 0.f, startY_ = 0.f, depth_ = 0.f, handX_ = 0.f, handY_ = 0.f;
};

// A classic drone's OSC STATUS: one lamp per generator, as bright as that generator sounds
// right now (MUTE, the voice's envelope, its own level), pulsing with the voice's beating.
// The engine writes the levels once per block; this only reads them.
class OscStatusControl : public IControl {
 public:
  OscStatusControl(EditorShared& s, const Widget& w)
      : IControl(drawRectOf(w)), s_(s), group_(static_cast<int>(w.id)), w_(w) {
    SetIgnoreMouse(true);
  }
  bool IsDirty() override {
    for (int i = 0; i < 5; ++i)
      if (std::fabs(s_.engine.oscLamp(group_, i) - shown_[i]) > 0.02f) return true;
    return IControl::IsDirty();
  }
  void Draw(IGraphics& g) override {
    GraphicsSink sink{g};
    for (int i = 0; i < 5; ++i) {
      shown_[i] = s_.engine.oscLamp(group_, i);
      // Spread the sounding range (about 0.3..0.9) over the whole lamp, so the pulse reads.
      const float b = std::pow(std::clamp((shown_[i] - 0.3f) / 0.6f, 0.f, 1.f), 1.3f);  // tuned by eye
      const float x0 = float(w_.x() + 5 + i * 12.5);
      art::drawBarLed(sink, x0, float(w_.y() + 8), x0 + 9.f, float(w_.y() + 38), hexOf(theme::kLedOn),
                      shown_[i] > 0.01f ? std::max(b, 0.08f) : 0.f);
    }
  }

 private:
  EditorShared& s_;
  int group_;
  Widget w_;
  float shown_[5] = {-1.f, -1.f, -1.f, -1.f, -1.f};
};

// Text for the overlays: each label has a box and an alignment (0 left, 1 centre, 2 right);
// long text is ellipsized at the chosen size (midi_ui::fitText), never shrunk.
struct OverlaySink : GraphicsSink {
  explicit OverlaySink(IGraphics& graphics) : GraphicsSink{graphics} {}
  void label(midi_ui::Box b, float size, std::uint32_t color, const char* value, bool bold, int align) {
    IText style = txt(size, theme::rgb(color), bold);
    style.mAlign = align == 1 ? EAlign::Center : align == 2 ? EAlign::Far : EAlign::Near;
    const auto fitted = midi_ui::fitText(value, b.r - b.l, [&](const char* text) {
      IRECT measured;
      g.MeasureText(style, text, measured);
      return measured.W();
    });
    g.DrawText(style, fitted.c_str(), IRECT(b.l, b.t, b.r, b.b));
  }
};

// The KEYBOARD MENU: six tabs of keyboard settings, the 16-step editor, the rhythm patterns,
// presets A-D and RESET PANEL. Drawing and hit testing come from keyboard_menu_view.h; every
// edit goes through the same EditorShared / engine calls as before (per-side routing under
// SPLIT included). It covers the whole menu area, so the plates, encoder and keyboard jacks
// under it take no clicks while it is open.
class KeyboardMenuControl : public IControl {
 public:
  explicit KeyboardMenuControl(EditorShared& s)
      : IControl(IRECT(kb_ui::kBounds.l, kb_ui::kBounds.t, kb_ui::kBounds.r, kb_ui::kBounds.b)), s_(s) {}
  void Draw(IGraphics& g) override {
    OverlaySink sink(g);
    kb_ui::State st;
    st.value = [this](ParameterId id) { return s_.value(static_cast<std::uint32_t>(id)); };
    st.tab = s_.menuPage;
    st.split = s_.split();
    st.side = s_.seqSide;
    st.presetSlot = s_.presetSlot;
    const auto now = Clock::now();
    st.resetArmed = now < resetUntil_;
    st.initArmed = now < initUntil_;
    st.presetDone = now < doneUntil_ ? doneKind_ : -1;
    for (int i = 0; i < kb_ui::kSeqSteps; ++i)
      if (const core::KeyboardSeqStep* q = s_.seqStep(i)) st.steps[i] = {int(q->note), q->gate != 0};
    st.arpMask = s_.engine.keyboardRhythm(s_.editSide(), false);
    st.seqMask = s_.engine.keyboardRhythm(s_.editSide(), true);
    st.scaleMask = s_.engine.keyboardScaleEditor(s_.editSide());
    kb_ui::draw(sink, st, hoverX_, hoverY_);
  }
  // Redraw once more when a confirm or "done" label runs out.
  bool IsDirty() override {
    const auto now = Clock::now();
    const int shown = (now < resetUntil_ ? 1 : 0) | (now < initUntil_ ? 2 : 0) | (now < doneUntil_ ? 4 : 0);
    const bool changed = shown != shownLast_;
    shownLast_ = shown;
    return IControl::IsDirty() || changed;
  }
  void OnMouseOver(float x, float y, const IMouseMod&) override {
    hoverX_ = x;
    hoverY_ = y;
    SetDirty(false);
  }
  void OnMouseOut() override {
    hoverX_ = hoverY_ = -1;
    SetDirty(false);
  }
  // Every close path (CLOSE, Esc, RESET PANEL, opening the MIDI overlay) hides the menu
  // without a mouse-out, so drop the hover here; it must not light up on reopen.
  void Hide(bool hide) override {
    if (hide) {
      hoverX_ = hoverY_ = -1;
      dragItem_ = dragStep_ = -1;
    }
    IControl::Hide(hide);  // marks dirty
  }
  void OnMouseDown(float x, float y, const IMouseMod&) override { press(x, y, false); }
  // iPlug2 reports a quick second click as a double-click: on a knob it restores the
  // factory value and on a step fader note 0, as before; everywhere else it is a click.
  void OnMouseDblClick(float x, float y, const IMouseMod&) override { press(x, y, true); }
  void OnMouseUp(float, float, const IMouseMod&) override {
    dragItem_ = dragStep_ = -1;
    SetDirty(false);
  }
  void OnMouseDrag(float, float y, float, float dY, const IMouseMod& mod) override {
    if (dragItem_ >= 0) nudge(kb_ui::kItems[dragItem_].id, -dY / (mod.S ? 2000.0 : 250.0));
    else if (dragStep_ >= 0 && y < kb_ui::kGateT) setStep(dragStep_, kb_ui::noteAt(y), std::nullopt);
  }
  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override {
    const kb_ui::Hit h = kb_ui::hitTest(s_.menuPage, s_.split(), x, y);
    if (h.kind != kb_ui::HitKind::Item || h.sub != 0) return;
    const kb_ui::Item& it = kb_ui::kItems[h.index];
    if (it.kind == kb_ui::Kind::Knob || it.kind == kb_ui::Kind::Trimmer) nudge(it.id, d / (mod.S ? 500.0 : 60.0));
  }

 private:
  using Clock = std::chrono::steady_clock;
  void press(float x, float y, bool dbl) {
    using kb_ui::HitKind;
    const kb_ui::Hit h = kb_ui::hitTest(s_.menuPage, s_.split(), x, y);
    switch (h.kind) {
      case HitKind::None: return;
      case HitKind::Close: s_.showMenu(false); break;
      case HitKind::Tab:
        s_.menuPage = h.index;
        s_.showMenu(true);
        break;
      case HitKind::Side: s_.seqSide = h.index; break;  // only under SPLIT (hitTest)
      case HitKind::Slot: s_.presetSlot = h.index; break;  // LOAD / SAVE / INIT act on it
      case HitKind::Load:
      case HitKind::Save:
      case HitKind::Init: preset(h.kind); break;
      case HitKind::Reset: reset(); break;
      case HitKind::Item: item(h, dbl); return;
      case HitKind::Pad: {  // ARP tab: arpeggiator pattern, SEQ tab: sequencer pattern
        const bool seqRow = s_.menuPage == kb_ui::kSeq;
        const std::uint8_t mask = s_.engine.keyboardRhythm(s_.editSide(), seqRow);
        s_.engine.postKeyboardRhythm(s_.editSide(), seqRow, static_cast<std::uint8_t>(mask ^ (1u << h.index)));
        SetDirty(false);
        return;
      }
      case HitKind::Fader:
        setStep(h.index, dbl ? 0 : kb_ui::noteAt(y), std::nullopt);
        if (!dbl) dragStep_ = h.index;
        return;
      case HitKind::Gate:
        if (const core::KeyboardSeqStep* q = s_.seqStep(h.index)) setStep(h.index, q->note, q->gate == 0);
        return;
      case HitKind::Note: {  // SCALE EDITOR: switch one note of the scale (bits count from ROOT)
        const int root = kb_ui::rootValue(s_.value(static_cast<std::uint32_t>(ParameterId::keyboard_root_note)));
        const std::uint16_t mask = s_.engine.keyboardScaleEditor(s_.editSide());
        s_.engine.postKeyboardScaleEditor(s_.editSide(),
                                          static_cast<std::uint16_t>(mask ^ (1u << ((h.index - root + 12) % 12))));
        break;
      }
    }
    GetUI()->SetAllControlsDirty();
  }
  void item(const kb_ui::Hit& h, bool dbl) {
    const kb_ui::Item& it = kb_ui::kItems[h.index];
    const auto id = static_cast<std::uint32_t>(it.id);
    switch (it.kind) {
      case kb_ui::Kind::Segmented:
      case kb_ui::Kind::VSegmented: s_.setIndex(id, h.sub); break;
      case kb_ui::Kind::Latch: s_.setIndex(id, s_.index(id) > 0 ? 0 : 1); break;
      case kb_ui::Kind::Stepper:  // clamped at both ends (the arrow greys out), no wrap
        if (const kb_ui::Count* c = kb_ui::countOf(it.id)) {
          const int k = kb_ui::countValue(it.id, s_.value(id)), next = std::clamp(k + h.sub, c->lo, c->hi);
          if (next != k) s_.set(id, kb_ui::countNorm(*c, next));
        } else {
          const int k = s_.index(id), next = std::clamp(k + h.sub, 0, kb_ui::optionCount(it.id) - 1);
          if (next != k) s_.setIndex(id, next);
        }
        break;
      case kb_ui::Kind::RootKeys:
        if (h.sub >= 0) s_.set(id, kb_ui::rootNorm(h.sub));
        break;
      case kb_ui::Kind::Knob:
      case kb_ui::Kind::Trimmer:
        if (h.sub != 0) {  // TEMPO < >: one whole BPM
          s_.set(id, kb_ui::bpmNorm(kb_ui::bpmValue(s_.value(id)) + h.sub));
        } else if (dbl) {
          if (const core::ParameterDescriptor* d = desc(id)) s_.set(id, d->initial);
        } else {
          dragItem_ = h.index;
        }
        break;
    }
    // PLAY changes which side the menu edits and whether EDITING shows: redraw all.
    if (it.id == ParameterId::keyboard_behaviour) GetUI()->SetAllControlsDirty();
    SetDirty(false);
  }
  void nudge(ParameterId pid, double fractionOfRange) {
    const auto id = static_cast<std::uint32_t>(pid);
    const core::ParameterDescriptor* d = desc(id);
    if (d == nullptr) return;
    s_.set(id, s_.value(id) + fractionOfRange * (d->max - d->min));
    SetDirty(false);
  }
  // One sequencer step: note (0..24 semitones above the held plate) and gate; an absent gate
  // keeps the step's current one.
  void setStep(int step, int note, std::optional<bool> gate) {
    const core::KeyboardSeqStep* q = s_.seqStep(step);
    if (q == nullptr) return;
    s_.engine.postSeqStep(s_.editSide(), step, note, gate.value_or(q->gate != 0));
    SetDirty(false);
  }
  void preset(kb_ui::HitKind k) {
    using Action = StandaloneAudioEngine::PresetAction;
    const auto now = Clock::now();
    if (k == kb_ui::HitKind::Init && now >= initUntil_) {  // first click arms, a second within 4 s clears
      initUntil_ = now + std::chrono::seconds(4);
      return;
    }
    if (k == kb_ui::HitKind::Init) initUntil_ = {};
    const Action a = k == kb_ui::HitKind::Load ? Action::Load : k == kb_ui::HitKind::Save ? Action::Save : Action::Initialise;
    if (s_.engine.postKeyboardPreset(a, static_cast<std::uint32_t>(s_.presetSlot))) {
      doneUntil_ = now + std::chrono::milliseconds(1200);
      doneKind_ = k == kb_ui::HitKind::Load ? 0 : k == kb_ui::HitKind::Save ? 1 : 2;
    }
  }
  void reset() {  // first click arms, a second within 4 s resets everything
    const auto now = Clock::now();
    if (now >= resetUntil_) {
      resetUntil_ = now + std::chrono::seconds(4);
      return;
    }
    resetUntil_ = {};
    for (int v = 0; v < 6; ++v) s_.engine.postDroneKey(v, true);
    s_.octave = 0;
    s_.seqSide = 0;
    s_.presetSlot = 0;
    s_.menuPage = 0;
    if (s_.factoryReset) s_.factoryReset();
    s_.showMenu(false);
  }
  EditorShared& s_;
  float hoverX_ = -1, hoverY_ = -1;
  int dragItem_ = -1, dragStep_ = -1;  // knob or step fader being dragged
  Clock::time_point resetUntil_{}, initUntil_{}, doneUntil_{};
  int doneKind_ = -1, shownLast_ = 0;
};

// MIDI uses the same measured rectangles for drawing and hit testing. All labels
// have explicit alignment and bounds; long names are ellipsized at a fixed font size.
class MidiOverlayControl : public IControl {
 public:
  explicit MidiOverlayControl(EditorShared& s)
      : IControl(IRECT(midi_ui::kBounds.l, midi_ui::kBounds.t, midi_ui::kBounds.r, midi_ui::kBounds.b)),
        s_(s) {}
  void Draw(IGraphics& g) override {
    OverlaySink sink(g);
    midi_ui::State state;
    state.map = s_.midiStore ? &s_.midiStore->map() : nullptr;
    state.device = s_.midi.inputDeviceName ? s_.midi.inputDeviceName() : "";
    state.channel = s_.midi.channelFilter ? s_.midi.channelFilter() : 0;
    state.octave = s_.midi.octaveShift ? s_.midi.octaveShift() : 0;
    state.curve = s_.midi.velocityCurve ? s_.midi.velocityCurve() : 0;
    state.split = s_.midi.splitNote ? s_.midi.splitNote() : core::kMidiDefaultSplitNote;
    state.offset = s_.midiListOffset;
    state.armed = s_.learnArmed;
    state.awaitTarget = s_.learnAwaitTarget;
    state.editable = editable();
    midi_ui::draw(sink, state, hoverX_, hoverY_);
  }
  void OnMouseOver(float x, float y, const IMouseMod&) override {
    hoverX_ = x;
    hoverY_ = y;
    SetDirty(false);
  }
  void OnMouseOut() override {
    hoverX_ = hoverY_ = -1;
    SetDirty(false);
  }
  // Every close path (CLOSE, Esc, the MIDI button, opening the keyboard menu) hides the
  // overlay without a mouse-out, so drop the hover here; it must not light up on reopen.
  void Hide(bool hide) override {
    if (hide) hoverX_ = hoverY_ = -1;
    IControl::Hide(hide);  // marks dirty
  }
  void OnMouseDown(float x, float y, const IMouseMod&) override {
    if (midi_ui::kClose.contains(x, y)) {
      s_.showMidi(false);
      GetUI()->SetAllControlsDirty();
      return;
    }
    const int count = s_.midiStore ? static_cast<int>(s_.midiStore->map().count()) : 0;
    s_.midiListOffset = midi_ui::pageOffset(s_.midiListOffset, count);
    if (midi_ui::kPrevious.contains(x, y)) {
      s_.midiListOffset = midi_ui::pageOffset(s_.midiListOffset - midi_ui::kVisibleRows, count);
      SetDirty(false);
      return;
    }
    if (midi_ui::kNext.contains(x, y)) {
      s_.midiListOffset = midi_ui::pageOffset(s_.midiListOffset + midi_ui::kVisibleRows, count);
      SetDirty(false);
      return;
    }
    if (!editable()) return;
    if (midi_ui::kLearn.contains(x, y)) {
      s_.setLearnArmed(!s_.learnArmed, !s_.learnArmed);
      GetUI()->SetAllControlsDirty();
      return;
    }
    for (int i = 0; i < midi_ui::kSettingCount; ++i) {
      const int delta = midi_ui::decrement(i).contains(x, y)   ? -1
                        : midi_ui::increment(i).contains(x, y) ? 1
                                                               : 0;
      if (!delta || !s_.midi.setRigSettings) continue;
      int values[] = {s_.midi.channelFilter(), s_.midi.octaveShift(), s_.midi.velocityCurve(),
                      s_.midi.splitNote()};
      constexpr int low[] = {0, -36, 0, core::kMidiSplitNoteLow},
                    high[] = {16, 36, 2, core::kMidiSplitNoteHigh};
      values[i] += delta;
      if (values[i] < low[i]) values[i] = high[i];
      if (values[i] > high[i]) values[i] = low[i];
      s_.midi.setRigSettings(values[0], values[1], values[2], values[3]);
      SetDirty(false);
      return;
    }
    if (s_.learnArmed && s_.learnAwaitTarget) {
      for (int i = 0; i < 5; ++i)
        if (midi_ui::action(i).contains(x, y)) {
          s_.learnTargetIsAction = true;
          s_.learnAction = midi_ui::kActions[i];
          s_.setLearnArmed(true, false);
          s_.learnArmSeq = s_.midi.messageSeq ? s_.midi.messageSeq() : 0;
          GetUI()->SetAllControlsDirty();
          return;
        }
    }
    for (int i = 0; i < midi_ui::kVisibleRows && s_.midiListOffset + i < count; ++i) {
      const auto binding = s_.midiStore->map().at(static_cast<std::uint32_t>(s_.midiListOffset + i));
      bool changed = false;
      if (midi_ui::remove(i).contains(x, y))
        changed = s_.midiStore->unbind(binding.key);
      else if (midi_ui::mode(i).contains(x, y) &&
               ((binding.targetKind == core::MidiTargetKind::parameter &&
                 core::midi_parameter_drive(binding) == core::MidiParameterDrive::follow) ||
                (binding.targetKind == core::MidiTargetKind::action && binding.key.kind == core::MidiBindingKind::cc &&
                 core::midi_action_is_continuous(binding.action)))) {
        auto edited = binding;
        edited.mode = static_cast<core::MidiInputMode>((static_cast<int>(binding.mode) + 1) % 4);
        changed = s_.midiStore->bind(edited);
      }
      if (changed) {
        s_.midiListOffset =
            midi_ui::pageOffset(s_.midiListOffset, static_cast<int>(s_.midiStore->map().count()));
        if (s_.midi.bindingsChanged) s_.midi.bindingsChanged();
        GetUI()->SetAllControlsDirty();
        return;
      }
    }
  }

 private:
  bool editable() const {
    return s_.midiStore && s_.midiStore->loadOutcome() != host::MidiMapLoadOutcome::Malformed &&
           s_.midiStore->loadOutcome() != host::MidiMapLoadOutcome::Unreadable;
  }
  EditorShared& s_;
  float hoverX_ = -1, hoverY_ = -1;
};

// Full-panel click capture, mouse-enabled only while learn awaits a target (driven by
// EditorShared::setLearnArmed): a click on a bindable panel control picks it as the
// target; a click elsewhere cancels. Draws nothing.
class LearnCaptureControl : public IControl {
 public:
  explicit LearnCaptureControl(EditorShared& s, const IRECT& all) : IControl(all), s_(s) {
    SetIgnoreMouse(true);
  }
  void Draw(IGraphics&) override {}
  void OnMouseDown(float x, float y, const IMouseMod&) override {
    for (const auto& b : s_.midiBindables) {
      if (x >= float(b.x0) && x <= float(b.x1) && y >= float(b.y0) && y <= float(b.y1)) {
        s_.learnTargetIsAction = b.isAction;
        s_.learnParameter = b.param;
        s_.learnAction = b.action;
        s_.setLearnArmed(true, false);  // target picked: now wait for the hardware message
        s_.learnArmSeq = s_.midi.messageSeq ? s_.midi.messageSeq() : 0;
        GetUI()->SetAllControlsDirty();
        return;
      }
    }
    s_.setLearnArmed(false, false);  // clicked empty panel: cancel
    GetUI()->SetAllControlsDirty();
  }

 private:
  EditorShared& s_;
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
      case WidgetKind::MidiSettings: g->AttachControl(new MidiSettingsControl(shared, w)); break;
      case WidgetKind::Record:
        shared.recordControl = new RecordControl(shared, w);
        g->AttachControl(shared.recordControl);
        break;
      case WidgetKind::RecordSource: g->AttachControl(new RecordSourceControl(shared, w)); break;
      case WidgetKind::Encoder: g->AttachControl(new EncoderControl(shared, w)); break;
      case WidgetKind::OctaveKey: g->AttachControl(new OctaveKeyControl(shared, w)); break;
      case WidgetKind::Display: g->AttachControl(new DisplayControl(shared, w)); break;
      case WidgetKind::PhotoSensor: g->AttachControl(new PhotoSensorControl(shared, w)); break;
      case WidgetKind::OscStatus: g->AttachControl(new OscStatusControl(shared, w)); break;
    }
  }
  // The bindable widget rects for MIDI learn (panel click picks the target).
  shared.midiBindables.clear();
  for (const Widget& w : widgets) {
    if (w.menu) continue;
    switch (w.kind) {
      case WidgetKind::Knob:
      case WidgetKind::Button:
      case WidgetKind::Toggle:
        shared.midiBindables.push_back({w.x(), w.y() - 24, w.x() + w.w, w.y() + w.h, false,
                                        static_cast<core::ParameterId>(w.id), core::MidiAction::master_mute});
        break;
      case WidgetKind::DroneKey:
        shared.midiBindables.push_back({w.x(), w.y(), w.x() + w.w, w.y() + w.h, true,
                                        core::ParameterId::keyboard_behaviour,
                                        static_cast<core::MidiAction>(static_cast<int>(core::MidiAction::drone_key_1) + int(w.id))});
        break;
      case WidgetKind::MasterMute:
        shared.midiBindables.push_back({w.x(), w.y() - 24, w.x() + w.w, w.y() + w.h, true,
                                        core::ParameterId::keyboard_behaviour, core::MidiAction::master_mute});
        break;
      case WidgetKind::PhotoSensor:
        shared.midiBindables.push_back({w.x(), w.y(), w.x() + w.w, w.y() + w.h, true,
                                        core::ParameterId::keyboard_behaviour,
                                        static_cast<core::MidiAction>(static_cast<int>(core::MidiAction::photo_drone_1) + int(w.id))});
        break;
      case WidgetKind::Cartridge:
        shared.midiBindables.push_back({w.x(), w.y(), w.x() + w.w, w.y() + w.h, true,
                                        core::ParameterId::keyboard_behaviour, core::MidiAction::cartridge_next});
        break;
      default:
        break;
    }
  }
  // The indicator LEDs the engine drives, each over its printed LED (same position and colour).
  static_assert(sizeof(kPanelLedPos) / sizeof(kPanelLedPos[0]) == StandaloneAudioEngine::kPanelLedCount,
                "one panel position per engine LED");
  for (int i = 0; i < StandaloneAudioEngine::kPanelLedCount; ++i)
    for (const art::Led& l : art::kLeds)
      if (std::fabs(l.x - kPanelLedPos[i].x) < 1.0 && std::fabs(l.y - kPanelLedPos[i].y) < 1.0)
        g->AttachControl(new LedControl(shared, i, l.x, l.y, l.r, l.rgb));
  shared.cables = new CableLayer(shared, all);
  g->AttachControl(shared.cables);

  // The keyboard menu on top of the plates and the cables (controls draw in attach order),
  // hidden until the encoder opens it. The cable layer ignores the mouse, so clicks are unchanged.
  auto* menu = new KeyboardMenuControl(shared);
  g->AttachControl(menu);
  shared.menuControls.push_back(menu);
  shared.showMenu(false);

  // MIDI settings overlay: the learn-capture layer sits UNDER the overlay controls (the
  // overlay wins its own rect; the panel's widgets answer clicks anywhere else).
  auto* capture = new LearnCaptureControl(shared, all);
  g->AttachControl(capture);
  shared.learnCapture = capture;
  shared.midiControls.clear();
  auto addMidi = [&](IControl* c) { g->AttachControl(c); shared.midiControls.push_back(c); };
  addMidi(new MidiOverlayControl(shared));
  shared.showMidi(false);

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
    if (shared.midi.litPlates && shared.midi.litPlates() != shared.seenMidiPlates) {  // MIDI notes
      shared.seenMidiPlates = shared.midi.litPlates();
      for (IControl* p : shared.plates) p->SetDirty(false);
    }
    if (shared.engine.muted() != shared.seenMuted) {  // MUTE toggled, also by a MIDI binding
      shared.seenMuted = shared.engine.muted();
      g->SetAllControlsDirty();
    }
    if (shared.engine.stateVersion() != shared.seenStateVersion) {
      shared.seenStateVersion = shared.engine.stateVersion();
      g->SetAllControlsDirty();
    }
    if (shared.learnArmed && !shared.learnAwaitTarget) {  // a hardware message completes a learn
      shared.pollLearn();
      if (!shared.learnArmed) g->SetAllControlsDirty();
    }
    if (shared.pollRelativeDetect()) g->SetAllControlsDirty();
    // REC: redraw the elapsed time once a second, and once more when recording stops.
    const int recSecond = shared.isRecording() && shared.rec.seconds ? int(shared.rec.seconds()) : -1;
    if (recSecond != shared.recordShownSecond && shared.recordControl != nullptr) {
      shared.recordShownSecond = recSecond;
      shared.recordControl->SetDirty(false);
    }
  });
}

}  // namespace lunar24::host::ui
