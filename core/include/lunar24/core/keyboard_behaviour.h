// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-③ per-note keyboard behaviours (design/00 §2d/§2e, design/06 §P4, design/07
// §3 §6; @Claude Go msg 37db4aa5): pressure output modes/rise-fall, portamento,
// vibrato, note quantiser scale+root.
//
// Design/07 §3 separates EXTERNAL events (timestamped ControlEvent) from INTERNAL
// continuous signals (per-sample CV/gate/clock). The P4-① InputStateMachine is the
// single place a normalized PerformanceInput becomes a canonical ControlEvent.
// These behaviours are the layer DOWNSTREAM of that: they take those ControlEvents
// and turn them into the per-sample control signals a keyboard voice consumes
// (design/07 §3 semantics 1 = discrete event, 2 = seconds-smoothing). This layer
// only ever produces control signals — never audio.
//
// Side-context rule (design/07 §6, design/00 §2d): a parameter is identified by
// ParameterId (the one physical knob) and its VALUE by which side's bank is read.
// Every scalar behaviour parameter is read through read_side_scalar (the side-
// resolution choke point in keyboard_mode.h), never from the global parameters[]
// directly. The one non-scalar behaviour parameter — the quantiser's scale-editor
// 12-bit note mask — has no ParameterId, so it is read through the same
// side_bank() resolution from the per-side `_r` mirror, not read_side_scalar.
//
// Honest boundaries (design/00 §3, §5) — do NOT treat these as evidence:
//   * norm -> seconds/amount laws for portamento/vibrato/pressure are PROVISIONAL
//     linear maps. The manual gives raw 0-255 / 0-127 and the registry freezes
//     them as norm 0..1; no numeric curve is evidenced, so a documented linear
//     ceiling is used pending measurement (design/00 §5 "先量后签").
//   * ASR/AD/LOOP sustain+peak level = the pressure at press is PROVISIONAL (the
//     manual enumerates the modes but not the segment levels).
//   * The 19 named preset scales (manual L970-984): the 7 modes / pentatonic x2 /
//     whole-tone / semitones have unambiguous 12-TET note sets (standard music
//     theory). The 8 style scales (blues-major, blues-minor, folk, japanese,
//     gamelan, gypsy, arabian, flamenco) have NO single 12-TET pattern in the
//     manual — they are left UNRESOLVED (preset_scale_mask returns kScaleUnresolved)
//     and MUST NOT be filled in by guessing a pattern.

#pragma once

#include <cmath>
#include <cstdint>
#include <utility>

#include <lunar24/core/control_event.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/parameter_smoothing.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// ------------------------------------------------------------------ scale + root -

// The scale-editor 12-bit note mask: bit i (0=C .. 11=B) set means that semitone is
// an allowed note of the quantised scale. All bits clear = microtonal keyboard
// (manual L972-978: "when all notes are disabled, the keyboard functions as a
// microtonal keyboard" — the quantiser passes the pitch through untouched).
inline constexpr std::uint16_t kMicrotonalScaleMask = 0x0000u;
inline constexpr std::uint16_t kChromaticScaleMask   = 0x0FFFu;  // all 12 ("semitones")

// Unambiguous 12-TET mode/pentatonic/whole-tone note sets (standard theory).
inline constexpr std::uint16_t kScaleIonian          = (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<11);
inline constexpr std::uint16_t kScaleDorian          = (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10);
inline constexpr std::uint16_t kScalePhrygian        = (1u<<0)|(1u<<1)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10);
inline constexpr std::uint16_t kScaleLydian          = (1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<9)|(1u<<11);
inline constexpr std::uint16_t kScaleMixolydian      = (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10);
inline constexpr std::uint16_t kScaleAeolian         = (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10);
inline constexpr std::uint16_t kScaleLocrian         = (1u<<0)|(1u<<1)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<10);
inline constexpr std::uint16_t kScalePentatonicMajor = (1u<<0)|(1u<<2)|(1u<<4)|(1u<<7)|(1u<<9);
inline constexpr std::uint16_t kScalePentatonicMinor = (1u<<0)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<10);
inline constexpr std::uint16_t kScaleWholeTone        = (1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8)|(1u<<10);

// Sentinel for a preset scale whose 12-TET note set is NOT evidenced by the manual.
inline constexpr std::uint16_t kScaleUnresolved = 0xFFFFu;

// The manual's LOAD SCALE list (L970-984) has 19 entries, index 0..18 matching the
// frozen registry selector 0..18. Returns the 12-bit note mask for the scales with
// an unambiguous note set; kScaleUnresolved for the style scales the manual only
// NAMES without enumerating their intervals (blues-major, blues-minor, folk,
// japanese, gamelan, gypsy, arabian, flamenco) — a real evidence gap we refuse to
// invent a pattern for.
inline constexpr std::uint16_t preset_scale_mask(std::uint8_t selector) noexcept {
  switch (selector) {
    case  0: return kChromaticScaleMask;      // semitones (all 12: no quantise)
    case  1: return kScaleIonian;
    case  2: return kScaleDorian;
    case  3: return kScalePhrygian;
    case  4: return kScaleLydian;
    case  5: return kScaleMixolydian;
    case  6: return kScaleAeolian;
    case  7: return kScaleLocrian;
    case 10: return kScalePentatonicMajor;
    case 11: return kScalePentatonicMinor;
    case 18: return kScaleWholeTone;
    // 8, 9, 12..17 (blues-major, blues-minor, folk, japanese, gamelan, gypsy,
    // arabian, flamenco): no evidenced 12-TET pattern -> unresolved.
    default: return kScaleUnresolved;
  }
}

// ROOT NOTE (manual L992-993): "sets the root note of the scale applied by the
// note quantiser from C to H." norm 0..1 -> semitone 0..11 (C..B). The exact
// norm->step split is PROVISIONAL (manual gives nominal C..H only, no numeric).
inline std::uint8_t root_note_semitone(double norm) noexcept {
  if (norm <= 0.0) return 0;
  if (norm >= 1.0) return 11;
  const long s = std::lround(norm * 11.0);
  return static_cast<std::uint8_t>(s < 0 ? 0 : (s > 11 ? 11 : s));
}

// The note quantiser (manual L970-993): snap a pitch CV (1 V/oct = 12 semitones/V)
// to the nearest in-scale note, rooted at root_semitone. The scale is the
// scale-editor 12-bit note mask. All-off -> microtonal passthrough. Discrete,
// stateless, deterministic (per-note decision, design/07 §3 semantics 1).
inline double quantize_pitch(double pitch_cv, std::uint16_t scale_mask,
                             std::uint8_t root_semitone) noexcept {
  if (scale_mask == kMicrotonalScaleMask) return pitch_cv;
  const double rel = pitch_cv * 12.0 - static_cast<double>(root_semitone);
  double best_cand = rel;
  double best_dist = -1.0;
  for (int oct = -2; oct <= 2; ++oct) {
    for (int note = 0; note < 12; ++note) {
      if ((scale_mask & (1u << note)) == 0u) continue;
      const double cand = static_cast<double>(note) + 12.0 * static_cast<double>(oct);
      const double d = std::fabs(rel - cand);
      if (best_dist < 0.0 || d < best_dist) {
        best_dist = d;
        best_cand = cand;
      }
    }
  }
  return (static_cast<double>(root_semitone) + best_cand) / 12.0;
}

// ------------------------------------------------------------ parameter reading -

// The scalar params a per-note behaviour reads, resolved for ONE side. Scalars are
// read through read_side_scalar (the side-context choke point); the non-scalar
// scale editor is read through the same side_bank() resolution from the per-side
// `_r` mirror. Structuring the read this way is what makes "never read global
// parameters[] for a side" structural rather than a convention.
struct KeyboardBehaviourParams {
  // scale selector + root (norm); scale editor mask (non-scalar side path).
  std::uint16_t scaleEditor = kMicrotonalScaleMask;
  std::uint8_t  quantiseLoadScale = 0;  // 0..18
  double        rootNote = 0.0;         // norm 0..1
  // pressure
  std::uint8_t  pressureOutput = 0;     // 0..4 (pressure/asr/ad/loop/random)
  double        pressureRise = 0.0;     // norm 0..1
  double        pressureFall = 0.0;     // norm 0..1
  // portamento
  double        portamentoSpeed = 0.0;  // norm 0..1
  std::uint8_t  portamentoLegato = 0;   // 0/1
  // vibrato
  double        vibratoSpeed = 0.0;     // norm 0..1
  double        vibratoDepth = 0.0;     // norm 0..1
  double        vibratoDelay = 0.0;     // norm 0..1
  double        vibratoPressure = 0.0;  // norm 0..1 (0 = pressure control off, >0 = on)
};

// Read one side's behaviour params through the side-context choke point. `bank` is
// the value-bank reader read_side_scalar expects ((bankIndex, id) -> double); the
// left/shared side reads parameters[id] and the right side reads keyboardScalarRight.
// `scaleEditor` reads the non-scalar mask for a bank index ((bankIndex) -> u16).
template <typename BankReader, typename ScaleEditor>
KeyboardBehaviourParams read_behaviour_params(BankReader&& bank, ScaleEditor&& scaleEditor,
                                              KeyboardMode mode, KeyboardSide side) {
  KeyboardBehaviourParams p{};
  p.scaleEditor = std::forward<ScaleEditor>(scaleEditor)(side_bank(mode, side));
  const auto rd = [&](ParameterId id) { return read_side_scalar(bank, mode, side, id); };
  p.quantiseLoadScale = static_cast<std::uint8_t>(rd(ParameterId::keyboard_quantise_load_scale));
  p.rootNote          = rd(ParameterId::keyboard_root_note);
  p.pressureOutput    = static_cast<std::uint8_t>(rd(ParameterId::keyboard_pressure_output));
  p.pressureRise      = rd(ParameterId::keyboard_pressure_rise);
  p.pressureFall      = rd(ParameterId::keyboard_pressure_fall);
  p.portamentoSpeed   = rd(ParameterId::keyboard_portamento_speed);
  p.portamentoLegato  = static_cast<std::uint8_t>(rd(ParameterId::keyboard_portamento_legato));
  p.vibratoSpeed      = rd(ParameterId::keyboard_vibrato_speed);
  p.vibratoDepth      = rd(ParameterId::keyboard_vibrato_depth);
  p.vibratoDelay      = rd(ParameterId::keyboard_vibrato_delay);
  p.vibratoPressure   = rd(ParameterId::keyboard_vibrato_pressure);
  return p;
}

// ------------------------------------------------- PROVISIONAL duration/depth laws -

// Linear norm -> seconds/amount ceilings. These are the MEASURABLE free parameters
// the manual only bounds by raw 0-255 / 0-127; no curve is evidenced, hence a
// documented linear map pending measurement. Do not cite these as the real law.
inline constexpr double kPortamentoMaxSeconds  = 2.5;
inline constexpr double kVibratoMaxHz          = 15.0;
inline constexpr double kVibratoMaxDepthCv     = 2.0 / 12.0;  // 2 semitones as volts
inline constexpr double kVibratoMaxDelaySec    = 2.5;
inline constexpr double kPressureMaxSeconds    = 2.5;

// ------------------------------------------------------------------- pressure --

// The manual's pressure OUTPUT modes (L951-965).
enum class PressureOutput : std::uint8_t {
  Pressure = 0,  // voltage follows plate pressure, slew on edges
  Asr      = 1,  // attack / sustain / release
  Ad       = 2,  // attack / decay
  Loop     = 3,  // looped AD
  Random   = 4,  // random voltage on plate press
};

// Continuous pressure output (design/07 §3 semantics 2: seconds-smoothing envelope).
// RISE = attack / slew-on-rising-edge, FALL = decay/release / slew-on-falling-edge.
// Asr/Ad/Loop sustain+peak level = the pressure at press (PROVISIONAL; the manual
// enumerates the modes but not the segment levels).
class PressureOutlet {
 public:
  PressureOutlet() { rand_.seed(0x9e3779b9u); }
  void setSampleRate(double fs) { fs_ = fs; }
  void setMode(PressureOutput m) { mode_ = m; }
  void setTimes(double riseNorm, double fallNorm) {  // PROVISIONAL linear law
    riseSec_ = riseNorm * kPressureMaxSeconds;
    fallSec_ = fallNorm * kPressureMaxSeconds;
  }
  void gate(bool high) {
    if (high && !held_) {
      held_ = true;
      stage_ = Stage::Attack;
      captured_ = false;
      if (mode_ == PressureOutput::Random) random_ = rand_.next01();
    } else if (!high && held_) {
      held_ = false;
      // One-shot AD/Loop decay on their own; ASR releases on gate-off.
      if (mode_ == PressureOutput::Asr) stage_ = Stage::Release;
    }
  }
  double tick(double pressure_in) {
    livePressure_ = pressure_in;
    switch (mode_) {
      case PressureOutput::Pressure: {
        const double tau = (pressure_in >= current_) ? riseSec_ : fallSec_;
        current_ = approach(current_, pressure_in, tau);
        break;
      }
      case PressureOutput::Asr: {
        if (stage_ == Stage::Attack) {
          capture_if_needed();
          current_ = approach(current_, sustain_, riseSec_);
          if (close(current_, sustain_)) stage_ = Stage::Sustain;
        } else if (stage_ == Stage::Sustain) {
          current_ = sustain_;
        } else {  // Release / Idle
          current_ = approach(current_, 0.0, fallSec_);
          if (close(current_, 0.0)) stage_ = Stage::Idle;
        }
        break;
      }
      case PressureOutput::Ad: {
        if (stage_ == Stage::Attack) {
          capture_if_needed();
          current_ = approach(current_, peak_, riseSec_);
          if (close(current_, peak_)) stage_ = Stage::Decay;
        } else {  // Decay / Idle
          current_ = approach(current_, 0.0, fallSec_);
          if (close(current_, 0.0)) stage_ = Stage::Idle;
        }
        break;
      }
      case PressureOutput::Loop: {
        if (stage_ == Stage::Attack) {
          capture_if_needed();
          current_ = approach(current_, peak_, riseSec_);
          if (close(current_, peak_)) stage_ = Stage::Decay;
        } else {  // Decay / Idle
          current_ = approach(current_, 0.0, fallSec_);
          if (close(current_, 0.0)) {
            stage_ = held_ ? Stage::Attack : Stage::Idle;  // loop back while held
          }
        }
        break;
      }
      case PressureOutput::Random: {
        const double tau = (random_ >= current_) ? riseSec_ : fallSec_;
        current_ = approach(current_, random_, tau);
        break;
      }
    }
    return current_;
  }
  double current() const { return current_; }
  void setRandomSeed(std::uint32_t s) { rand_.seed(s); }

 private:
  enum class Stage : std::uint8_t { Idle, Attack, Sustain, Release, Decay };
  // Deterministic LCG (std::rand is implementation-defined; a fixed seed must give
  // a fixed stream so the Random mode is reproducible in tests).
  struct Lcg {
    std::uint32_t s = 0x9e3779b9u;
    void seed(std::uint32_t x) { s = x ? x : 0x9e3779b9u; }
    double next01() {
      s = s * 1664525u + 1013904223u;
      return static_cast<double>((s >> 8) & 0x00FFFFFFu) / 16777216.0;
    }
  };
  void capture_if_needed() {
    if (!captured_) {
      sustain_ = peak_ = livePressure_;  // PROVISIONAL: level = pressure at press
      captured_ = true;
    }
  }
  double approach(double cur, double target, double tau) {
    if (tau > 0.0 && fs_ > 0.0) {
      const double a = 1.0 - std::exp(-1.0 / (fs_ * tau));
      return cur + a * (target - cur);
    }
    return target;
  }
  static bool close(double a, double b) { return std::fabs(a - b) < 1e-4; }

  PressureOutput mode_ = PressureOutput::Pressure;
  Stage stage_ = Stage::Idle;
  double fs_ = 0.0, current_ = 0.0, livePressure_ = 0.0;
  double riseSec_ = 0.0, fallSec_ = 0.0, sustain_ = 0.0, peak_ = 0.0, random_ = 0.0;
  bool held_ = false, captured_ = false;
  Lcg rand_;
};

// ------------------------------------------------------------------ portamento --

// Continuous pitch glide (manual L910-923): "slew limiting effect on the CV output".
// LEGATO off = always on; on = gliding only when >=2 plates touched (no effect with
// arp). Reuses ParameterSmoother (design/07 §3: don't reinvent the seconds-smoother).
class PortamentoGlide {
 public:
  PortamentoGlide() { smoother_.reset(0.0); }
  void setSampleRate(double fs) { smoother_.setSampleRate(fs); }
  void setNorm(double speedNorm, double legato) {  // PROVISIONAL linear law
    smoother_.setTimeConstantSeconds(speedNorm * kPortamentoMaxSeconds);
    legato_ = legato != 0.0;
  }
  // A note target arrived. legato on and NOT a multi-touch -> instant, else glide.
  void noteOn(double pitch_cv, bool multi_touch) {
    if (!legato_ || multi_touch) smoother_.setTarget(pitch_cv);
    else smoother_.reset(pitch_cv);  // single note, legato: jump (no glide)
  }
  void setTarget(double pitch_cv) { smoother_.setTarget(pitch_cv); }
  // GH#8 reset: clear the glide back to 0 so a later note in the SAME sample can
  // re-open afresh (design/07 §3 phase 0 reset precedes a same-sample re-note-on).
  void reset() { smoother_.reset(0.0); }
  double tick() { return smoother_.next(); }
  double current() const { return smoother_.current(); }

 private:
  ParameterSmoother smoother_;
  bool legato_ = false;
};

// --------------------------------------------------------------------- vibrato --

// Continuous pitch-mod oscillator (manual L934-950): an LFO on the pitch (V/oct)
// output. SPEED = frequency, DEPTH = amount, DELAY = ramp time to full strength,
// PRESSURE = when on, amount scaled by key pressure.
class Vibrato {
 public:
  void setSampleRate(double fs) { fs_ = fs; }
  void setNorm(double speedNorm, double depthNorm, double delayNorm, double pressureNorm) {
    speedHz_ = speedNorm * kVibratoMaxHz;        // PROVISIONAL linear law
    depthCv_ = depthNorm * kVibratoMaxDepthCv;   // PROVISIONAL
    delaySec_ = delayNorm * kVibratoMaxDelaySec; // PROVISIONAL
    pressureCtrl_ = pressureNorm > 0.0;
    pressureAmount_ = pressureNorm;              // PROVISIONAL: pressure scales depth
  }
  void gate(bool high) {
    if (high && !running_) {
      running_ = true;
      phase_ = 0.0;
      delayElapsed_ = 0.0;
    } else if (!high) {
      running_ = false;  // vibrato follows the note; gate-off stops it
    }
  }
  // Returns a pitch-CV delta (volts) at this sample.
  double tick(double pressure) {
    if (!running_ || fs_ <= 0.0) return 0.0;
    const double ramp = delaySec_ > 0.0 ? std::min(1.0, delayElapsed_ / delaySec_) : 1.0;
    delayElapsed_ += 1.0 / fs_;
    phase_ += kTwoPi * speedHz_ / fs_;
    if (phase_ > kTwoPi) phase_ -= kTwoPi;
    double amt = depthCv_ * ramp;
    if (pressureCtrl_) amt *= (1.0 + pressureAmount_ * pressure);  // PROVISIONAL
    return std::sin(phase_) * amt;
  }

 private:
  double fs_ = 0.0, speedHz_ = 0.0, depthCv_ = 0.0, delaySec_ = 0.0;
  double delayElapsed_ = 0.0, phase_ = 0.0, pressureAmount_ = 0.0;
  bool running_ = false, pressureCtrl_ = false;
  static constexpr double kTwoPi = 6.283185307179586476925287;
};

// ---------------------------------------------------- per-side behaviour engine --

// Composes the four behaviours for ONE keyboard side into a single control-signal
// engine. It is fed the canonical ControlEvents produced by InputStateMachine (the
// P4-① choke point) and produces the per-sample pitch / pressure / gate control
// signals a voice would consume. It never constructs a ControlEvent from raw input
// itself — that is translate()'s job — so the "single interpretation choke point"
// (design/07 §1) holds structurally.
class KeyboardBehaviour {
 public:
  // Decode the side params into the running configuration (reads via the choke
  // point are done externally in read_behaviour_params, then applied here).
  void configure(const KeyboardBehaviourParams& p, double sample_rate) {
    fs_ = sample_rate;
    pressure_.setSampleRate(sample_rate);
    vibrato_.setSampleRate(sample_rate);
    portamento_.setSampleRate(sample_rate);
    pressure_.setMode(static_cast<PressureOutput>(p.pressureOutput));
    pressure_.setTimes(p.pressureRise, p.pressureFall);
    portamento_.setNorm(p.portamentoSpeed, p.portamentoLegato);
    vibrato_.setNorm(p.vibratoSpeed, p.vibratoDepth, p.vibratoDelay, p.vibratoPressure);
    scaleMask_ = p.scaleEditor != kScaleUnresolved ? p.scaleEditor : kMicrotonalScaleMask;
    rootSemitone_ = root_note_semitone(p.rootNote);
  }

  void handleControlEvent(const ControlEvent& ev) {
    switch (ev.kind) {
      case ControlEventKind::pitch:
        handlePitch_(ev, quantize_pitch(ev.value, scaleMask_, rootSemitone_));
        break;
      case ControlEventKind::pressure:
        handlePressure_(ev);
        break;
      case ControlEventKind::gate_on:
        handleGateOn_(ev);
        break;
      case ControlEventKind::gate_off:
        handleGateOff_(ev);
        break;
      case ControlEventKind::reset:
        resetAll_();
        break;
      default:
        break;  // parameter / clock / sync are not per-note behaviour
    }
  }

  // Advance one sample. pitch_cv is the glided (then vibrato-modulated) pitch CV;
  // pressure_cv is the pressure-output envelope; gate is the live gate state.
  // GH#8: the glide MUST advance via portamento_.tick() every sample — using
  // current() (the old path) left the glide frozen at its starting value.
  void tick(double* pitch_cv, double* pressure_cv) {
    const double glide = portamento_.tick();
    const double vib = vibrato_.tick(livePressure_);
    *pitch_cv = glide + vib;
    *pressure_cv = pressure_.tick(livePressure_);
  }
  bool gate() const { return engagedCount_() > 0; }
  double rootSemitone() const { return static_cast<double>(rootSemitone_); }
  std::uint16_t scaleMask() const { return scaleMask_; }

  // Random-mode seed: forwarded so a test makes the Random output deterministic.
  void setRandomSeed(std::uint32_t s) { pressure_.setRandomSeed(s); }

  // GH#8 over-capacity note/identity rejections (deterministic + observable).
  std::uint32_t overCapacity() const { return overCapacity_; }

 private:
  // One note/touch press of a side's keyboard. `engaged` means a gate_on has
  // latched it (the voice is sounding it or has it held-under); a not-engaged
  // record is a pre-latch pitch/pressure that arrived in phase 1 before its
  // phase-4 gate_on. Identity = (source, channel, noteId) — the three fields a
  // press carries through the whole pipeline (GH#8).
  struct HeldNote {
    ControlSourceId source = 0;
    std::uint8_t channel = 0;
    NoteId noteId = 0;
    double pitch = 0.0;        // quantized CV
    double pressure = 0.0;
    bool hasPitch = false;
    bool engaged = false;
    std::uint32_t order = 0;   // gate-on activation order (fallback selection)
  };
  static constexpr std::uint32_t kMaxHeld = 12;  // the touch plates

  HeldNote* findNote_(ControlSourceId s, std::uint8_t ch, NoteId id) {
    for (std::uint32_t i = 0; i < kMaxHeld; ++i)
      if (notes_[i].source == s && notes_[i].channel == ch && notes_[i].noteId == id)
        return &notes_[i];
    return nullptr;
  }
  // Reserve a wholly-free slot (not latched and not holding a pending pitch).
  // Returns null on over-capacity: the note is REJECTED (counted, observable),
  // never allowed to overwrite an older held note (which would leave a stuck gate).
  HeldNote* createNote_(ControlSourceId s, std::uint8_t ch, NoteId id) {
    for (std::uint32_t i = 0; i < kMaxHeld; ++i)
      if (!notes_[i].engaged && !notes_[i].hasPitch) {
        notes_[i] = HeldNote{};
        notes_[i].source = s;
        notes_[i].channel = ch;
        notes_[i].noteId = id;
        return &notes_[i];
      }
    ++overCapacity_;
    return nullptr;
  }
  std::uint32_t engagedCount_() const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < kMaxHeld; ++i) if (notes_[i].engaged) ++c;
    return c;
  }
  int highestOrderEngaged_() const {
    int best = -1;
    std::uint32_t bestOrder = 0;
    for (std::uint32_t i = 0; i < kMaxHeld; ++i)
      if (notes_[i].engaged && (best < 0 || notes_[i].order > bestOrder)) {
        best = static_cast<int>(i);
        bestOrder = notes_[i].order;
      }
    return best;
  }

  void handlePitch_(const ControlEvent& ev, double q) {
    HeldNote* note = findNote_(ev.source, ev.channel, ev.noteId);
    if (note) {
      // Re-pitch a known note: update its stored pitch; if it is the SOUNDING
      // note, glide to the new target (a held chord member being re-pitched).
      note->pitch = q;
      note->hasPitch = true;
      if (note->engaged && currentIndex_ >= 0 && &notes_[currentIndex_] == note)
        portamento_.setTarget(q);
      return;
    }
    HeldNote* created = createNote_(ev.source, ev.channel, ev.noteId);
    if (!created) return;  // over-capacity (already counted in createNote_)
    created->pitch = q;
    created->hasPitch = true;
  }
  void handlePressure_(const ControlEvent& ev) {
    HeldNote* note = findNote_(ev.source, ev.channel, ev.noteId);
    if (!note) return;  // defensive: a pressure always follows its pitch/gate
    note->pressure = ev.value;
    if (note->engaged && currentIndex_ >= 0 && &notes_[currentIndex_] == note)
      livePressure_ = ev.value;  // only the SOUNDING note drives the live pressure
  }
  void handleGateOn_(const ControlEvent& ev) {
    HeldNote* note = findNote_(ev.source, ev.channel, ev.noteId);
    if (!note) {
      // No prior pitch (a direct test may latch first); use the default pitch.
      note = createNote_(ev.source, ev.channel, ev.noteId);
      if (!note) return;
    }
    note->engaged = true;
    note->order = ++orderCounter_;
    currentIndex_ = static_cast<int>(note - notes_);  // newest note becomes current
    std::uint32_t engaged = 0;
    for (std::uint32_t i = 0; i < kMaxHeld; ++i) if (notes_[i].engaged) ++engaged;
    multiTouch_ = engaged >= 2;
    // The pitch already arrived in phase 1; latch with it (NOT "wait for the
    // next pitch after gate-on" — the old nextPitchIsNewNote_ protocol).
    portamento_.noteOn(note->pitch, multiTouch_);
    livePressure_ = note->pressure;
    pressure_.gate(true);
    vibrato_.gate(true);
  }
  void handleGateOff_(const ControlEvent& ev) {
    HeldNote* note = findNote_(ev.source, ev.channel, ev.noteId);
    if (!note || !note->engaged) return;  // unknown/idempotent release
    note->engaged = false;
    if (currentIndex_ >= 0 && &notes_[currentIndex_] == note) {
      // The SOUNDING note was released: deterministically return to the still-held
      // note with the largest activation order (the last note seen). Any other
      // release leaves pitch/gate/modulation untouched (GH#8 partial-release rule).
      const int fb = highestOrderEngaged_();
      if (fb >= 0) {
        currentIndex_ = fb;
        livePressure_ = notes_[fb].pressure;
        portamento_.setTarget(notes_[fb].pitch);
      } else {
        currentIndex_ = -1;
        livePressure_ = 0.0;
      }
    }
    *note = HeldNote{};  // free the slot (pitch is no longer a voice state)
    if (engagedCount_() == 0) {
      // Only the LAST release closes pressure/vibrato (partial release holds them).
      pressure_.gate(false);
      vibrato_.gate(false);
    }
  }
  void resetAll_() {
    for (std::uint32_t i = 0; i < kMaxHeld; ++i) notes_[i] = HeldNote{};
    currentIndex_ = -1;
    orderCounter_ = 0;
    multiTouch_ = false;
    livePressure_ = 0.0;
    pressure_.gate(false);
    vibrato_.gate(false);
    portamento_.reset();
  }

  double fs_ = 0.0;
  std::uint16_t scaleMask_ = kMicrotonalScaleMask;
  std::uint8_t rootSemitone_ = 0;
  HeldNote notes_[kMaxHeld];
  int currentIndex_ = -1;      // index of the SOUNDING note, or -1 when none
  std::uint32_t orderCounter_ = 0;
  bool multiTouch_ = false;
  std::uint32_t overCapacity_ = 0;
  double livePressure_ = 0.0;
  PressureOutlet pressure_;
  PortamentoGlide portamento_;
  Vibrato vibrato_;
};

}  // namespace lunar24::core
