// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ParameterSmoothing: the real-time continuous-parameter semantic (design/07 §3).
// This is distinct from a sample-accurate discrete event (event_timebase.h) and
// from an audio-rate modulation signal (audio_rate_modulation.h) — a smoothed
// parameter moves toward a target over a time constant, it is not evaluated per
// sample from a waveform.
//
// §5: core is configured by the host sample rate — there is no fixed 48k constant.
// Time constants are expressed in SECONDS; the per-sample coefficient is
// recomputed whenever the sample rate changes. A good smoothing must reach the
// same value at the SAME WALL-CLOCK time across sample rates, so the coefficient
// must be derived from the seconds constant and the live rate, not from a fixed
// sample count and not from a coefficient frozen at one assumption.

#pragma once

#include <cmath>

namespace lunar24::core {

// One-pole smoother toward a target, with a seconds-based time constant.
//
//   y[n] = y[n-1] + a * (target - y[n-1])
//   a    = 1 - exp(-1 / (fs * tau_seconds))
//
// The exact choice of `a` makes the decay independent of sample rate in
// WALL-CLOCK time: fraction `f` of the step is reached at
//   t = -tau * ln(1 - f)   seconds,   for any fs.
// That is the cross-sample-rate property the tests hold it to.
class ParameterSmoother {
 public:
  // Snapshot the starting value so a step from an arbitrary level to a new target
  // is measured from that level.
  void reset(double value) {
    current_ = value;
    target_ = value;
    resetLevel_ = value;
    recompute();
  }

  void setSampleRate(double sample_rate) {
    sample_rate_ = sample_rate;
    recompute();
  }

  void setTimeConstantSeconds(double seconds) {
    tau_seconds_ = seconds;
    recompute();
  }

  void setTarget(double value) { target_ = value; }

  // Advance one sample and return the new smoothed value.
  double next() {
    current_ += coefficient_ * (target_ - current_);
    return current_;
  }

  double current() const { return current_; }
  double target() const { return target_; }
  double sampleRate() const { return sample_rate_; }

  // Fraction of the current step that `current` has travelled from the reset
  // level. Used by the cross-sample-rate reach-time check.
  double progress() const {
    const double span = target_ - resetLevel_;
    if (std::abs(span) < 1e-12) return 1.0;
    return (current_ - resetLevel_) / span;
  }

 private:
  void recompute() {
    if (sample_rate_ > 0.0) {
      // tau <= 0 means ZERO smoothing: snap to the target on the next sample
      // (coefficient 1.0) — the keyboard portamento contract. A portamentoSpeed
      // of 0 is NOT a freeze: it must jump immediately (the one-pole limit
      // a = 1 - exp(-1/(fs*tau)) is 1.0 as tau -> 0+). Negative tau is treated
      // the same way defensively. coefficient 1.0 is the ONLY legal snap value.
      //
      // Contrast this with a sample rate that is still 0 (not yet configured,
      // below): there coefficient 0.0 IS appropriate — it is an inert not-yet-live
      // state that returns the held value until the host sets the rate. Do not
      // carry that 0.0 into a live-rate tau=0 case; that is the freeze bug.
      if (tau_seconds_ <= 0.0) {
        coefficient_ = 1.0;
      } else {
        coefficient_ = 1.0 - std::exp(-1.0 / (sample_rate_ * tau_seconds_));
      }
    } else {
      coefficient_ = 0.0;  // no sample rate yet: inert
    }
  }

  double current_ = 0.0;
  double target_ = 0.0;
  double resetLevel_ = 0.0;
  double sample_rate_ = 0.0;
  double tau_seconds_ = 0.0;
  double coefficient_ = 0.0;
};

}  // namespace lunar24::core
