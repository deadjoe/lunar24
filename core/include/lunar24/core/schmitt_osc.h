// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SchmittOsc — P3-②: a Schmitt-trigger relaxation oscillator. This is a FIRST-
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
// FREQUENCY MODEL — the "rate-unit trap" @Claude named. The ramp advances by
//
//     ramp += sign * chargeRate / sampleRate        (per sample)
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
// DISCRETE QUANTIZATION (honest, inherent, documented): the ramp can only flip
// direction on an integer sample, so the real period is
//       period = 2 * ceil(1 / step) samples,  step = chargeRate / sampleRate,
// i.e. the frequency is `sr / (2 * ceil(sr/(2*vT*freqBase*(1+tolerance))))` and
// is NEVER slower than the continuous value by more than one sub-sample per half
// swing (bounded by step, typically a few %). This is a genuine relaxation-
// oscillator property, not a fixed-increment bug: across 44.1/48/88.2/96 kHz the
// frequency stays within that bound of the continuous value and is sr-invariant to
// within it, whereas a fixed per-sample increment would scale WITH sr.
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

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class SchmittOsc {
 public:
  // Threshold window: the ramp swings between -kWindowVolts and +kWindowVolts.
  static constexpr double kWindowVolts = 0.5;

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
    direction_ = 1.0;  // charge from 0 toward +vT first; sign flips on threshold.
  }

  // Advance one sample and write the oscillator waveform into *out. Realtime-safe.
  void tick(double* out) {
    // dt-scaled rate. NEVER a fixed per-sample step (see file comment).
    ramp_ += direction_ * chargeRate_ / sampleRate_;
    if (ramp_ >= kWindowVolts) {
      ramp_ = kWindowVolts;  // clamp so the next half-period starts exactly at +vT.
      direction_ = -1.0;
    } else if (ramp_ <= -kWindowVolts) {
      ramp_ = -kWindowVolts;
      direction_ = 1.0;
    }
    *out = ramp_;
  }

  double freqBaseHz() const { return freqBaseHz_; }
  double toleranceOf() const { return tolerance_; }
  double effectiveFreqHz() const { return freqBaseHz_ * (1.0 + tolerance_); }

 private:
  double sampleRate_;
  double freqBaseHz_;
  double tolerance_;
  double chargeRate_;  // per-second ramp rate; freq = chargeRate/(4*vT).
  double ramp_;
  double direction_;  // +1 charging up, -1 charging down.
};

}  // namespace lunar24::core
