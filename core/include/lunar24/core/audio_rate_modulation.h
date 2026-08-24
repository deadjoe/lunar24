// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// AudioRateModulation: the audio-rate modulation-signal semantic (design/07 §3).
// This is distinct from a discrete event (event_timebase.h) and from a smoothed
// continuous parameter (parameter_smoothing.h). An audio-rate signal carries
// per-sample information about the waveform itself — it MUST be evaluated once
// per sample and arrive at the processor as its raw instantaneous value. Routing
// it through a smoothing curve destroys that audio-rate information.
//
// §5: period/frequency in seconds/Hz (no fixed 48k constant). Deterministic,
// headless, per-sample evaluation gate.

#pragma once

namespace lunar24::core {

// Deterministic normalized triangle oscillator returning one sample per call.
// Runs between -1.0 (inclusive) and +1.0 (inclusive): triangle(0)= -1,
// triangle(0.5)=+1, triangle(1)= -1. `period_seconds` is the waveform cycle
// length.
class AudioRateModulation {
 public:
  AudioRateModulation(double period_seconds = 0.01, double sample_rate = 48000.0) {
    setPeriodSeconds(period_seconds);
    setSampleRate(sample_rate);
  }

  void setSampleRate(double sample_rate) {
    sample_rate_ = sample_rate;
    recompute();
  }

  void setPeriodSeconds(double period_seconds) {
    period_seconds_ = period_seconds;
    recompute();
  }

  // Return the current sample, then advance one sample.
  double next() {
    const double v = triangle(phase_);
    phase_ += phaseStep_;
    if (phase_ >= 1.0) phase_ -= 1.0;
    return v;
  }

  void reset(double phase = 0.0) { phase_ = phase; }

 private:
  static double triangle(double p) {  // p in [0,1)
    return (p < 0.5) ? (4.0 * p - 1.0) : (3.0 - 4.0 * p);
  }

  void recompute() {
    phaseStep_ = (sample_rate_ > 0.0 && period_seconds_ > 0.0)
                     ? 1.0 / (sample_rate_ * period_seconds_)
                     : 0.0;
  }

  double sample_rate_ = 48000.0;
  double period_seconds_ = 0.01;
  double phaseStep_ = 0.0;
  double phase_ = 0.0;
};

}  // namespace lunar24::core
