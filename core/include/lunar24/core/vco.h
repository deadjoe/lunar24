// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vco — P3-③: a single triangle-core analog VCO (AS3340 family) that models BOTH
// VCO A and VCO B. This is the DSP core. The A/B *module shape* (which jacks
// exist) is a registry fact already landed: vco_a has a SYNC input, vco_b has a
// VCO output, and VCO A's output is normalised into VCO B's CV input (CV AMT knob)
// — the latter is P2-②'s NormalizedRoute, re-presented here as a fact, NOT re-coded
// in this header. The core therefore carries the shared oscillator DSP; the
// sync-only-on-A asymmetry is guarded by test (see syncPulse below).
//
// PROVENANCE — the registry (generated/lunar24/registry.hpp) is the frozen
// implementation basis; every claim below mirrors it and marks evidence strength
// precisely (@Claude GO 604ed080). "Confirmed" == all relevant field evidence
// confirmed; "provisional" == position/value is evidenced in name but the exact
// number is not written in the manual; "unverified" == the field evidence is
// unverified and it may conflict with the manual's literal wording.
//
//   * V/OCT (v_oct_in) — SignalType::cv, polarity unipolar, range 0..8 V,
//     SignalTransfer::exponential, direction/type/transfer CONFIRMED.
//     pitch = fBase * 2^vOct ; every 1 V exactly doubles across the whole 0..8 V.
//   * Octave selector (oct_sel) — 3 positions "low"/"0"/"+3", default index 1="0"
//     (all six field evidence PROVISIONAL). The position NAMES are evidenced; the
//     exact octave COUNT per position is not (manual writes only "+3 / low"; what
//     "low" gives is not in the manual). Encoded here as {low:-1, "0":0, "+3":+3}
//     octaves — PROVISIONAL, and the value for "low" is a standing guess, not fact.
//   * Sub selector (sub_sel) — 2 positions "0"/"-1", default index 1="-1"
//     (PROVISIONAL). Sub is one octave down.
//   * Tune (tune) — unit "oct", range -1..+1, default 0, but ALL six field evidence
//     are UNVERIFIED and this conflicts with the manual literal "tune knob controls
//     the pitch continuously over ONE octave" (registry reads -1..+1 = two octaves).
//     This conflict is recorded in FINDINGS as a MUST-RESOLVE item and is NOT
//     silently resolved here. Implemented per registry (oct, -1..+1).
//   * CV input lin/exp (lin_exp) — selector "lin"/"exp", default index 1="exp"
//     (PROVISIONAL). The generic CV input is a modulation input with UNKNOWN
//     transfer (cv_in is -5..+5). The linear-vs-exponential law below is a
//     PROVISIONAL modeling choice. V/OCT (confirmed) is the calibrated 1 V/oct
//     pitch input; the generic CV input is an uncalibrated modulation input.
//   * SHAPE (pw / pulse width) — 0..1, default 0.5 (unverified). Duty, for kPulse.
//
// WAVEFORM PROVENANCE — the "traditional four" (saw / triangle / sine / pulse) is
// an INFERENCE from AS3340 convention. The manual says only "6 waveforms, two
// morphing (saw<->inverted saw, sine<->triangle)" and never enumerates the four.
// This is recorded in FINDINGS as inference, NOT manual evidence. The two morphing
// waveforms are evidenced by the manual's own parenthetical.
//
// SUB PHASE-LOCK — the sub is derived from the SAME unwrapped pitch accumulator
// (subPhase = frac(cumPitch/2)), so it is exactly one octave down AND phase-locked
// to the parent with a fixed, deterministic relationship. This is the OPPOSITE of
// the drone-bank requirement (voices must be independent); an independent
// free-running sub is the negative control for the phase-lock test.
//
// HARD SYNC — syncPulse() resets the unwrapped phase to 0 (the slave restarts).
// This is the sync input that exists on VCO A only. VCO B has NO sync input, so a
// correct B path never calls syncPulse(); the test asserts B's phase is continuous
// and that a B given a sync pulse shows a discontinuity the continuity detector
// fires on. Hard sync is a prime aliasing source (measured in FINDINGS).
//
// Value source = control inputs only (no seed / no hidden randomness); identical
// inputs + identical sample rate + identical buffer partition => bit-identical
// output. Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

namespace lunar24::core {

// The six selectable waveforms. The traditional four are an AS3340-convention
// inference (see PROVENANCE); the two morphing are manual-evidenced.
enum class VcoWaveform : std::uint8_t {
  kSaw,               // traditional (AS3340 inference)
  kTriangle,          // traditional (AS3340 inference; the triangle core itself)
  kSine,              // traditional (AS3340 inference)
  kPulse,             // traditional; duty from SHAPE (pw)
  kMorphSawInvSaw,    // manual-evidenced: saw <-> inverted saw
  kMorphSineTriangle  // manual-evidenced: sine <-> triangle
};

// CV input lin/exp mode (lin_exp selector). Default index 1 = kExponential.
enum class VcoControlMode : std::uint8_t { kLinear, kExponential };

class Vco {
 public:
  // sampleRate must be > 0. Reproducible: no seed, value comes only from inputs.
  explicit Vco(double sampleRate) : sr_(sampleRate) {}

  // ---------------------------------------------------------------- frequency --
  // Baseline oscillator frequency (the f0 the whole chain is referenced to).
  void setBaseHz(double hz) { baseHz_ = hz; }

  // V/OCT jack: 0..8 V, exponential (CONFIRMED). Every 1 V doubles.
  void setVoct(double volts) { vOct_ = volts; }

  // Generic CV input with a lin/exp mode switch (cv_in -5..+5). The scaling is a
  // PROVISIONAL modeling choice (cv_in transfer is UNKNOWN). cvAmt scales it.
  void setCvInput(double volts, VcoControlMode mode) { cv_ = volts; cvMode_ = mode; }
  void setCvAmt(double amt) { cvAmt_ = amt; }  // CV AMT knob, 0..1, default 1.

  // Octave selector: 0="low", 1="0", 2="+3". Octave COUNTS are PROVISIONAL.
  void setOctaveSelect(int index);
  // Tune knob: unit "oct", range -1..+1 (UNVERIFIED + conflicts with manual's
  // "one octave"; see FINDINGS — must-resolve, NOT silently resolved here).
  void setTune(double oct) { tune_ = oct; }
  // Sub selector: 0="0" (off), 1="-1" (one octave down). PROVISIONAL.
  void setSubSelect(int index) { subSelect_ = index; }

  // ---------------------------------------------------------------- waveform --
  void setWaveform(VcoWaveform w) { wave_ = w; }
  // Morph 0..1; only meaningful for the two morphing waveforms (clamped).
  void setMorph(double m);
  // SHAPE = pulse-width duty for kPulse. Clamped into a small (0,1) window so an
  // extreme setting can never collapse the pulse to a flat DC line / silence break
  // (the two-rail swing is always present; the must-test verifies it).
  void setShape(double duty) {
    constexpr double kMinDuty = 1e-3, kMaxDuty = 1.0 - 1e-3;
    duty_ = duty < kMinDuty ? kMinDuty : (duty > kMaxDuty ? kMaxDuty : duty);
  }

  // ----------------------------------------------------------- linear FM input --
  // instFreq = pitch + fmDevHz * fmCv. fDev is the Hz depth, fmCv the FM CV (-5..+5).
  void setFmCv(double volts) { fmCv_ = volts; }
  void setFmDepth(double fDevHz) { fmDevHz_ = fDevHz; }

  // HARD SYNC (VCO A only): reset the unwrapped pitch phase to 0. VCO B has no
  // sync input and must never call this; test guards the asymmetry.
  void syncPulse() { cumPitch_ = 0.0; }

  // ---------------------------------------------------------------- render --
  // Advance one sample. Writes the main waveform into *out; if subOut is non-null,
  // the sub waveform (one octave down, phase-locked) into *subOut.
  void tick(double* out, double* subOut = nullptr);

  // ---------------------------------------------------------------- inspect --
  double frequencyHz() const;   // core pitch (before FM), at current inputs.
  double subFrequencyHz() const { return frequencyHz() * 0.5; }
  double phase() const { return frac(cumPitch_); }       // parent phase [0,1).
  double subPhase() const { return frac(cumPitch_ * 0.5); }  // sub phase [0,1).
  bool subEnabled() const { return subSelect_ == 1; }    // index 1 = "-1".
  double waveformSampleAt(double p) const;  // shape at a phase, for morph checks.

  static constexpr double kLowOctave = -1.0;  // oct_sel "low" (PROVISIONAL).
  static constexpr double kZeroOctave = 0.0;  // oct_sel "0".
  static constexpr double kPlus3Octave = 3.0; // oct_sel "+3" (approximately +3 oct).
  static constexpr double kTwoPi = 6.28318530717958647692528676655900577;

 private:
  static double frac(double v) { return v - std::floor(v); }

  double sr_;
  double baseHz_ = 0.0;
  double vOct_ = 0.0;
  double cv_ = 0.0;
  double cvAmt_ = 1.0;
  int octSelect_ = 1;                  // default "0".
  double tune_ = 0.0;
  int subSelect_ = 1;                  // default "-1".
  VcoControlMode cvMode_ = VcoControlMode::kExponential;  // default index 1.
  VcoWaveform wave_ = VcoWaveform::kTriangle;
  double morph_ = 0.5;
  double duty_ = 0.5;
  double fmCv_ = 0.0;
  double fmDevHz_ = 0.0;
  double cumPitch_ = 0.0;              // unwrapped pitch phase (cycles).
};

inline void Vco::setOctaveSelect(int index) {
  octSelect_ = index < 0 ? 0 : (index > 2 ? 2 : index);
}

inline void Vco::setMorph(double m) { morph_ = m < 0.0 ? 0.0 : (m > 1.0 ? 1.0 : m); }

inline double Vco::frequencyHz() const {
  const double octs[3] = {kLowOctave, kZeroOctave, kPlus3Octave};
  double p = baseHz_ * std::pow(2.0, octs[octSelect_] + tune_);
  p *= std::pow(2.0, vOct_);                       // V/OCT: confirmed, 1 V = 1 oct.
  const double cvEff = cv_ * cvAmt_;
  if (cvMode_ == VcoControlMode::kExponential) {
    p *= std::pow(2.0, cvEff);                     // PROVISIONAL exp scaling.
  } else {
    p *= (1.0 + cvEff);                            // PROVISIONAL linear scaling.
  }
  return p > 0.0 ? p : 0.0;
}

inline void Vco::tick(double* out, double* subOut) {
  const double pitch = frequencyHz();
  const double instHz = pitch + fmDevHz_ * fmCv_;  // linear FM.
  cumPitch_ += instHz / sr_;
  *out = waveformSampleAt(phase());
  if (subOut) {
    if (subEnabled()) {
      *subOut = 2.0 * subPhase() - 1.0;   // sub = square of subPhase (phase-locked).
    } else {
      *subOut = 0.0;
    }
  }
}

// Waveform level at a given phase p in [0,1). Morphing waveforms crossfade in
// phase-locked space so the morph is continuous by construction.
inline double Vco::waveformSampleAt(double p) const {
  const double a = 2.0 * p - 1.0;                     // saw (rising).
  switch (wave_) {
    case VcoWaveform::kSaw:
      return a;
    case VcoWaveform::kTriangle:
      return 4.0 * std::fabs(p - 0.5) - 1.0;          // p=0->+1 .. p=0.5->-1.
    case VcoWaveform::kSine:
      return std::sin(kTwoPi * p);
    case VcoWaveform::kPulse:
      return (p < duty_) ? 1.0 : -1.0;                // bipolar pulse, duty in (0,1).
    case VcoWaveform::kMorphSawInvSaw: {
      // crossfade saw <-> inverted saw; continuous (passes through 0 at mid).
      const double inv = -a;
      return (1.0 - morph_) * a + morph_ * inv;
    }
    case VcoWaveform::kMorphSineTriangle: {
      const double s = std::sin(kTwoPi * p);
      const double t = 4.0 * std::fabs(p - 0.5) - 1.0;
      return (1.0 - morph_) * s + morph_ * t;
    }
  }
  return 0.0;
}

}  // namespace lunar24::core
