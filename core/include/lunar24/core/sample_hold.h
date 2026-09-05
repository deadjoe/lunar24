// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SAndHold — P3-②: a sample-and-hold with a hold period specified in SECONDS.
// The trigger ("grab a new value") fires every holdSeconds; between triggers the
// output is held constant. It is a unit delay on the "re-clock" side: it does not
// invent any timestamp, it just freezes the incoming level for a wall-clock
// interval and re-captures it when the timer elapses.
//
// THE "RATE-UNIT TRAP" @Claude named: the hold duration must be in SECONDS, never
// in samples. We accumulate
//
//     elapsed += 1 / sampleRate ;  if (elapsed >= holdSeconds) grab & subtract
//
// so the period is holdSeconds in wall-clock time, and a hold of, say, 10 ms is
// 441 samples at 44.1 kHz and 960 samples at 96 kHz — the SAME 10 ms. A
// sample-count hold (trigger at a fixed N) would change the hold duration with the
// sample rate, which the cross-sample-rate must-test rejects.
//
// @Claude's P3-② amendment: cross-sample-rate asserts the HOLD DURATION IN SECONDS
// only, NEVER the held-value sequence. If the input is a per-sample-advancing
// signal (e.g. NoiseSource), the sample captured at the same wall-clock trigger
// differs by sample rate as a physical necessity, so "same values across sr" would
// be wrong. The only sr-invariant is the period in seconds.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

namespace lunar24::core {

class SAndHold {
 public:
  // sampleRate > 0, holdSeconds > 0. `initial` is the held level until the first
  // trigger elapses.
  SAndHold(double sampleRate, double holdSeconds, double initial = 0.0)
      : sampleRate_(sampleRate),
        holdSeconds_(holdSeconds > 0.0 ? holdSeconds : 1e-6),
        elapsed_(0.0),
        held_(initial) {}

  // Clock-triggered form — what the product path uses. The manual (Papa Srapa):
  // "send any LFO or modulator source to the clock socket, which will set the speed
  // of operation." A new value is captured on a RISING edge of `clock` (a level
  // thresholded at kClockOn); between edges the output is held. An UNCLOCKED clock
  // (constant level, never crossing the threshold) re-captures nothing, so the
  // output does NOT self-run — the "未接 clock 时不自走" acceptance. The seconds-based
  // self-timed form below is kept for the module's legacy standalone tests.
  void tick(double input, double clock, double* out) {
    const bool rising = (clock >= kClockOn) && (prevClock_ < kClockOn);
    if (rising) held_ = input;
    prevClock_ = clock;
    *out = held_;
  }

  // Consume one input sample and write the held level into *out (legacy self-timed
  // form). When the timer elapses, capture the current input and (critically)
  // subtract the full period so the next trigger is exactly holdSeconds later — no
  // per-block or per-sample cumulative drift, so partitioning the render can never
  // shift a grab.
  void tick(double input, double* out) {
    elapsed_ += 1.0 / sampleRate_;
    if (elapsed_ >= holdSeconds_) {
      held_ = input;
      elapsed_ -= holdSeconds_;
    }
    *out = held_;
  }

  double holdSeconds() const { return holdSeconds_; }

 private:
  // Clock edge threshold (a clock level >= kClockOn counts as "high").
  static constexpr double kClockOn = 0.5;

  double sampleRate_;
  double holdSeconds_;
  double elapsed_;
  double held_;
  double prevClock_ = 0.0;  // last clock level, for rising-edge detection.
};

}  // namespace lunar24::core
