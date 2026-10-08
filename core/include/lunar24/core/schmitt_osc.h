// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SchmittOsc —: a Schmitt-trigger relaxation oscillator. This is a FIRST-
// CLASS oscillator, NOT the CV→gate comparator latch in sink_interpret.h. The
// distinction matters (and is why the two must coexist, each written on its own):
//
//   * sink_interpret.h's hysteresis is a COMPARATOR LATCH: an input CV and a
//     high/low threshold (gateThresholdVolts + hysteresisVolts) yield a 0/1 gate.
//     It has no integrator, no charge rate, no timing, and produces no frequency.
//   * SchmittOsc here is a FEEDBACK INTEGRATOR: a bounded ramp is charged toward
//     +/-vT at a fixed rate per unit time; when it crosses a threshold the sign
//     flips, so the window vT and the charge rate jointly set the PERIOD. It is a
//     genuine oscillator with a well-defined frequency.
//
// FREQUENCY MODEL — the "rate-unit trap" named. The ramp advances by
//
//     ramp += sign * chargeRate / sampleRate (per sample)
//
// i.e. chargeRate is a per-SECOND rate scaled by dt = 1/sampleRate. It is NOT a
// fixed per-sample increment. A per-sample constant step would make the frequency
// scale with the sample rate (double sr => double Hz), which the cross-sample-rate
// must-test rejects. For a symmetric triangle ramping -vT..+vT, one half-period is
// the time to swing the full window 2*vT at rate chargeRate, so
//
//     freq = chargeRate / (4 * vT)
//
// and construction sets chargeRate = 4 * vT * freqBase*(1+tolerance) so that the
// measured frequency equals the nominal seeded frequency exactly in the
// continuous-time limit.
//
// Discrete integration retains the fractional time after every threshold crossing.
// A wrapped phase expresses the same symmetric charge/discharge ramp without
// rounding each half-period up to whole samples. This matters for both audio pitch
// and the LF square that clocks FM/AM and sample-and-hold in drones 3/6.
//
// The single source of value is the seed: freqBase (20..2000 Hz) and a STATIC
// tolerance are derived once from one SeededRandom, giving a deterministic,
// repeatable oscillator with no hidden randomness. Like DroneBank, the seed is
// consumed only at construction (constants), never per sample. The waveform is a
// triangle (ramp_) in [-vT, +vT]. No negistor here; that seam is DroneBank's.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

#include "lunar24/core/blamp_kernel.h"
#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class SchmittOsc {
 public:
  // Threshold window: the ramp swings between -kWindowVolts and +kWindowVolts.
  static constexpr double kWindowVolts = 0.5;
  // PITCH at or below this (semitones) gates the tone off — the "PITCH to zero =>
  // clean noise" recipe. Provisional (the manual gives no numeric pitch floor).
  static constexpr double kSilenceSt = -60.0;

  // sampleRate must be > 0. Derives freqBase + a static tolerance from `seed`.
  SchmittOsc(std::uint64_t seed, double sampleRate)
      : sampleRate_(sampleRate) {
    SeededRandom rng(seed);
    const double freqBaseHz = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;  // 20 .. 2000 Hz
    const double tolerance = rng.nextUnit(0.0, 0.02);                  // 0 .. 2% (static)
    freqBaseHz_ = freqBaseHz;
    tolerance_ = tolerance;
    // Solve chargeRate for the target nominal frequency: freq = chargeRate/(4*vT).
    chargeRate_ = 4.0 * freqBaseHz * (1.0 + tolerance) * kWindowVolts;
    ramp_ = 0.0;
  }

  // NEW-voice PITCH control (provisional mapping). Scales the per-
  // second charge rate by 2^(st/12); because freq = chargeRate/(4*vT), the measured
  // frequency scales by 2^(st/12) at every sample rate. The rate-unit property
  // (dt = 1/sr scaling, NOT a fixed per-sample step) is preserved — a pure fixed
  // multiplier cannot break the cross-sample-rate invariance.
  //
  // PITCH-at-floor silence: at or below kSilenceSt the oscillator is gated silent
  // (toneGate_ = 0). The manual's "PITCH to zero => clean noise sound" recipe needs
  // the tone gone so the added noise is all that remains behind the signal. The
  // runtime maps the panel PITCH position (0..1) so its minimum lands here.
  void setPitchSemitones(double semitones) {
    if (semitones <= kSilenceSt) { toneGate_ = 0.0; return; }
    toneGate_ = 1.0;
    pitchScale_ = std::pow(2.0, semitones / 12.0);
  }

  // NEW-voice FM/AM modulation. One external modulation source
  // (set via setMod, typically the LF square) drives BOTH kinds of modulation, the
  // two "factory" FM/AM switches choosing which are engaged (see the 4-combo
  // acceptance):
  //   * FM — fmDevHz is the PEAK deviation in Hz: the instant frequency swings by
  //     +/-fmDevHz as mod sweeps -1..+1 (freq = chargeRate/(4vT) => an absolute
  //     fDevHz adds 4*vT*fDevHz*mod to the per-second charge rate).
  //   * AM — amDepth in [0,1] is the amplitude index: amp = (1 + amDepth*mod).
  void setMod(double m) { mod_ = m; }
  void setFmDevHz(double hz) { fmDevHz_ = (hz < 0.0 ? 0.0 : hz); }
  // Exponential FM: the frequency is multiplied by 2^(fmOctaves * mod).
  void setFmOctaves(double oct) { fmOct_ = oct < 0.0 ? 0.0 : oct; }
  // An external pitch CV in octaves (1 V/oct at the jack): frequency x 2^oct.
  void setPitchCvOctaves(double oct) {
    if (oct != pitchCvOct_) {
      pitchCvOct_ = oct;
      pitchCvScale_ = std::exp2(oct);
    }
  }
  void setAmDepth(double ad) { amDepth_ = (ad < 0.0 ? 0.0 : (ad > 1.0 ? 1.0 : ad)); }
  // Override the nominal frequency (Hz) from a control such as RATE. Resets the
  // pitch scale to neutral so the target frequency is absolute.
  void setFreqHz(double hz) {
    const double f = hz < 0.0 ? 0.0 : hz;
    chargeRate_ = 4.0 * f * kWindowVolts;
    pitchScale_ = 1.0;
  }

  // The LF oscillator is "used as a square wave modulator" (manual, Papa Srapa):
  // a +/-1 bipolar square at the oscillator frequency, taken from the ramp sign.
  // This is what the audio oscillator consumes as its mod source.
  double square() const { return ramp_ >= 0.0 ? 1.0 : -1.0; }

  // Advance one sample and write the oscillator waveform into *out. Realtime-safe.
  void tick(double* out) {
    if (toneGate_ <= 0.0) { *out = 0.0; return; }  // PITCH-at-floor: tone silent.
    // dt-scaled rate. NEVER a fixed per-sample step (see file comment).
    const double expFm = fmOct_ > 0.0 ? std::exp2(fmOct_ * mod_) : 1.0;
    const double instRate = chargeRate_ * pitchScale_ * pitchCvScale_ * expFm +
                            4.0 * kWindowVolts * fmDevHz_ * mod_;
    const double rate = instRate > 0.0 ? instRate : 0.0;  // per-second ramp rate.
    const double step = rate / (4.0 * kWindowVolts * sampleRate_);
    if (!std::isfinite(step)) { *out = 0.0; return; }
    // Fold in constant time, even when a large pitch CV crosses multiple periods
    // in one sample. Reduce the step first so it cannot erase the existing phase.
    phase_ += step - std::floor(step);
    phase_ -= std::floor(phase_);
    ramp_ = kWindowVolts * (4.0 * std::fabs(phase_ - 0.5) - 1.0);
    const double corr = triangleBlampCorr(step);
    // AM index: amp = ramp * (1 + amDepth*mod), clamped at 0 (never inverts).
    const double am = 1.0 + amDepth_ * mod_;
    *out = (ramp_ + corr) * (am > 0.0 ? am : 0.0);
  }

  double freqBaseHz() const { return freqBaseHz_; }
  double toleranceOf() const { return tolerance_; }
  double effectiveFreqHz() const { return chargeRate_ * pitchScale_ / (4.0 * kWindowVolts); }

 private:
  // Round the slope reversals at the actual fractional-sample corners. The
  // shared kernel already includes its factor of 8; for a +/-A triangle the
  // correction scale is A * step. Peaks are at integer phase, valleys at n+0.5.
  // Sum overlapping corners too: at high audio rates their supports overlap.
  // This is a local-rate correction, not full band-limiting of abrupt FM or AM.
  double triangleBlampCorr(double step) const {
    if (!(step > 0.0) || step > 0.5) return 0.0;
    // Above Nyquist retain the bounded ramp; this does not remove its aliasing.
    const double mag = kWindowVolts * step;
    const int radius = static_cast<int>(std::ceil(8.0 * step)) + 1;
    double corr = 0.0;
    for (int n = -radius; n <= radius; ++n) {
      double d = std::fabs(static_cast<double>(n) - phase_) / step;
      if (d <= 8.0) corr -= mag * blampKernel(d);
      d = std::fabs(static_cast<double>(n) + 0.5 - phase_) / step;
      if (d <= 8.0) corr += mag * blampKernel(d);
    }
    return corr;
  }

  double sampleRate_;
  double freqBaseHz_;
  double tolerance_;
  double chargeRate_;   // per-second ramp rate; freq = chargeRate/(4*vT).
  double pitchScale_ = 1.0;  // PITCH semitone factor (2^(st/12)); neutral = 1.0.
  double fmDevHz_ = 0.0;     // FM peak deviation in Hz (0 = no FM).
  double fmOct_ = 0.0;       // exponential FM depth in octaves per unit of mod (0 = off).
  double pitchCvOct_ = 0.0, pitchCvScale_ = 1.0;  // external pitch CV (octaves, factor)
  double amDepth_ = 0.0;     // AM index [0,1] (0 = no AM).
  double mod_ = 0.0;         // external modulation source, set per frame via setMod.
  double toneGate_ = 1.0;    // 0 = PITCH-at-floor silence, 1 = on.
  double ramp_;
  double phase_ = 0.75;  // ramp starts at zero, charging toward +kWindowVolts.
};

}  // namespace lunar24::core
