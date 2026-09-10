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
    direction_ = 1.0;  // charge from 0 toward +vT first; sign flips on threshold.
  }

  // NEW-voice PITCH control (design/01 §3, provisional mapping). Scales the per-
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

  // NEW-voice FM/AM modulation (design/01 §3, #45). One external modulation source
  // (set via setMod, typically the LF square) drives BOTH kinds of modulation, the
  // two "factory" FM/AM switches choosing which are engaged (see the 4-combo
  // acceptance):
  //   * FM — fmDevHz is the PEAK deviation in Hz: the instant frequency swings by
  //     +/-fmDevHz as mod sweeps -1..+1 (freq = chargeRate/(4vT) => an absolute
  //     fDevHz adds 4*vT*fDevHz*mod to the per-second charge rate).
  //   * AM — amDepth in [0,1) is the amplitude index: amp = (1 + amDepth*mod).
  // With fmDevHz = amDepth = 0 and the gate on, tick() is bit-identical to the
  // pre-#45 oscillator, so every existing SchmittOsc test stays green.
  void setMod(double m) { mod_ = m; }
  void setFmDevHz(double hz) { fmDevHz_ = (hz < 0.0 ? 0.0 : hz); }
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
    const double instRate = chargeRate_ * pitchScale_ +
                            4.0 * kWindowVolts * fmDevHz_ * mod_;
    const double rate = instRate > 0.0 ? instRate : 0.0;  // per-second ramp rate.
    // NOTE: this ramp update keeps the pre-existing association
    // (direction * rate) / sampleRate. Re-associating it to direction * (rate /
    // sampleRate) is a different floating-point expression and would break the
    // bit-identical-default guarantee; `inc` below is for the CORRECTION only.
    ramp_ += direction_ * rate / sampleRate_;
    int rail = 0;  // which rail this sample was clamped to, if any.
    if (ramp_ >= kWindowVolts) {
      ramp_ = kWindowVolts;  // clamp so the next half-period starts exactly at +vT.
      direction_ = -1.0;
      rail = +1;
    } else if (ramp_ <= -kWindowVolts) {
      ramp_ = -kWindowVolts;
      direction_ = 1.0;
      rail = -1;
    }
    const double corr = railBlampCorr(rate / sampleRate_, rail);
    // AM index: amp = ramp * (1 + amDepth*mod), clamped at 0 (never inverts).
    const double am = 1.0 + amDepth_ * mod_;
    *out = (ramp_ + corr) * (am > 0.0 ? am : 0.0);
  }

  double freqBaseHz() const { return freqBaseHz_; }
  double toleranceOf() const { return tolerance_; }
  double effectiveFreqHz() const { return chargeRate_ * pitchScale_ / (4.0 * kWindowVolts); }

 private:
  // ---------------------------------------------------------------------------
  // Rail-clamp slope correction (task #109 / GH#19 S1).
  //
  // The ramp's SLOPE jumps by 2r at every rail clamp (from +r to -r at +vT, and
  // back at -vT, where r = rate/sampleRate is the per-sample ramp increment), so the
  // right band-limiting kernel is the SHARED BLAMP in blamp_kernel.h -- the same
  // kernel the VCO triangle uses. What is NOT shared is the shape and the scale,
  // both derived here rather than copied from the VCO:
  //
  //   * PHASE. `blampPhase_` is a CORNER LATTICE in cycles, not the oscillator's
  //     analytic phase: the two rails are half a cycle apart, the +vT clamp is an
  //     INTEGER lattice point and the -vT clamp a HALF-INTEGER one -- the corner
  //     lattice the kernel assumes (peaks at integers, valleys at half-integers).
  //     Each clamp SNAPS the phase onto its lattice point, which is what absorbs the
  //     oscillator's discrete-quantization overshoot (the clamp fires an integer
  //     number of samples after the ideal crossing, not at it).
  //     The lattice step is 0.5 / ceil(1/r) -- half a cycle per ACTUAL (integer)
  //     half-period -- NOT the analytic r/2. That distinction is load-bearing: with
  //     the analytic step every half-integer corner lands `a` samples away from the
  //     corner the waveform actually has, and the measured cost of that misplacement
  //     was up to 7 dB (two cells came out WORSE than the uncorrected naive render).
  //   * SCALE. mag = slope_jump/8 = 2r/8 = r/4 (the kernel already carries the 8,
  //     corner 8/pi^2). Cross-check by the paper's A*phi law: A = kWindowVolts = 0.5
  //     and phi = r/2, giving the same r/4. Two independent routes, one number.
  //   * FALLBACK. The M <= 8 gate is the SHARED predicate expressed in the phase
  //     domain: 8*step >= 0.5 <=> 4/ceil(1/r) >= 0.5 <=> ceil(1/r) <= 8 <=> M <= 8.
  //     Same physical fact as the VCO's, so both callers leave the bounded naive
  //     waveform there. That range is reported as FALLBACK, never as improvement.
  //
  // PROVENANCE OF THE 1/2 IN `delta = 0.5 * a` -- read this before "simplifying":
  //   a = ceil(1/r) - 1/r is DERIVED: it is the sub-sample distance from the clamp
  //   sample back to where the unclamped ramp would have crossed the rail, and it is
  //   a closed form in r alone (verified: M == ceil(1/r) and the discarded overshoot
  //   == M*r-1 held on 12/12 required cells). THE 1/2 IS NOT DERIVED. It is the
  //   MEASURED OPTIMUM, not a geometric identity: the ideal crossing sits a whole
  //   `a` before the clamp sample, but the empirically best kernel centre is half
  //   that. Evidence: a least-squares slope through the origin of -0.5113 over the
  //   12 required cells, and per-cell optima/a = 0.443..0.569 on the 9 cells whose a
  //   is resolved by the 0.05 delta grid. The three excluded cells are the SMALLEST-a
  //   ones, where grid/a = 0.92, 0.46 and 0.38 -- the sweep step is simply too coarse
  //   to locate the optimum there, so they are grid-resolution artifacts, not
  //   counterexamples. This constant is named PROVISIONAL for exactly that reason
  //   and is a single GLOBAL value: it is NOT fitted per cell or per sample rate, and
  //   must not be.
  //   `delta = 0` (kernel centred ON the clamp sample) and `delta = a` (centred on
  //   the ideal crossing) were both measured, and both are far worse: -45.5 dB and
  //   -110.9 dB of aggregate gain against +92.6 dB for 0.5*a. A tempting "geometric"
  //   story -- that the clamp splits the 2r slope jump into a small one (-a*r at c-1)
  //   and a big one (-(2-a)*r at c), whose centroid is exactly c - a/2 -- was also
  //   tested as a TWO-kernel construction and is WORSE STILL (-76.7 dB): splitting
  //   the jump is not what the naive waveform does. The fitted constant is therefore
  //   not a disguised physical quantity; see the slice report.
  //   The correction is applied BEFORE the AM multiply, so it is exact only for
  //   amDepth = 0 (i.e. it does not scale with the AM index); see the slice report.
  double railBlampCorr(double inc, int rail) {
    if (!(inc > 0.0) || !std::isfinite(inc)) return 0.0;  // zero/NaN rate: none.
    // `r` is the per-sample ramp increment IN VOLTS (== inc), i.e. the SLOPE of the
    // ramp between clamps. It must not be confused with the slope JUMP, which is 2r:
    // mag = jump/8 = 2r/8 = r/4, and the ideal half-period is M = 1/r. (Writing
    // r = inc + inc here would scale mag 2x too large AND halve M, i.e. make `a`
    // meaningless -- both routes below are stated in terms of r, so r is inc.)
    const double r = inc;                       // per-sample ramp increment (volts).
    const double mIdeal = 1.0 / r;              // IDEAL half-period, in samples.
    // The ACTUAL half-period is an integer: the clamp discards the overshoot, so the
    // ramp leaves -vT exactly and re-clamps exactly ceil(1/r) samples later (every
    // half period, unconditionally). The lattice step must be 0.5 cycles per ACTUAL
    // half period, not per ideal one -- with the ideal step `every half-integer
    // (-rail) corner sat `a` samples away from the corner the waveform really has`,
    // which cost up to 7 dB of the correction on real cells. Cross-checked against
    // the analyzer: this step reproduces the offline corner-anchored model's per-cell
    // dB exactly (see the slice report).
    const double mActual = std::ceil(mIdeal);   // integer half-period the ramp HAS.
    const double step = 0.5 / mActual;          // cycles/sample; exact at every clamp.
    if (blampSupportReachesHalfPeriod(step)) return 0.0;    // M <= 8: fallback.
    if (rail != 0) {
      // This sample IS the rail clamp: SNAP the phase onto the kernel's corner
      // lattice (+vT = integer, -vT = half-integer). The snap is what absorbs the
      // discrete-quantization overshoot -- the clamp fires an integer number of
      // samples after the ideal crossing, and without the snap that error would
      // accumulate and walk the lattice away from the actual corners.
      blampPhase_ = (rail > 0) ? 0.0 : 0.5;
      blampAnchored_ = true;
    } else {
      blampPhase_ += step;                      // interior sample: advance.
    }
    // Before the first clamp there is no corner in the signal's past to correct.
    if (!blampAnchored_) return 0.0;
    const double a = mActual - mIdeal;          // DERIVED sub-sample offset.
    const double delta = 0.5 * a;               // PROVISIONAL 1/2 -- see above.
    const double mag = 0.25 * r;                // slope jump 2r / 8; kernel carries 8.
    const double frac = blampPhase_ - std::floor(blampPhase_);
    const int radius = static_cast<int>(std::ceil(8.0 * step)) + 1;
    double corr = 0.0;
    for (int n = -radius; n <= radius; ++n) {
      // Signed distance from the corner at phase n to this sample is
      // (frac - n)/step samples; the kernel centre sits `delta` samples BEFORE the
      // clamp sample, so the distance to it is (frac-n)/step + delta, which is the
      // same as |(n-frac)/step - delta| below.
      double d = std::fabs(((double)n - frac) / step - delta);
      if (d <= 8) corr -= mag * blampKernel(d);   // -2r slope jump at +vT.
      d = std::fabs(((double)n + 0.5 - frac) / step - delta);
      if (d <= 8) corr += mag * blampKernel(d);   // +2r slope jump at -vT.
    }
    return corr;
  }

  double sampleRate_;
  double freqBaseHz_;
  double tolerance_;
  double chargeRate_;   // per-second ramp rate; freq = chargeRate/(4*vT).
  double pitchScale_ = 1.0;  // PITCH semitone factor (2^(st/12)); neutral = 1.0.
  double fmDevHz_ = 0.0;     // FM peak deviation in Hz (0 = no FM).
  double amDepth_ = 0.0;     // AM index [0,1] (0 = no AM).
  double mod_ = 0.0;         // external modulation source, set per frame via setMod.
  double toneGate_ = 1.0;    // 0 = PITCH-at-floor silence, 1 = on.
  double ramp_;
  double direction_;  // +1 charging up, -1 charging down.
  double blampPhase_ = 0.0;      // correction phase, cycles; corner lattice anchor.
  bool blampAnchored_ = false;   // false until the first rail clamp sets the lattice.
};

}  // namespace lunar24::core
