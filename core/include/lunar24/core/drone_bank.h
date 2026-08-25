// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DroneBank — P3-①: a classic drone bank of free-running oscillators.
//
// Model: kMaxVoices independent, phase-accumulated oscillators. The only source
// of value is the seed: every per-voice constant (base frequency, static
// tolerance, amplitude, and the drift/noise terms) is derived once at
// construction from a single SeededRandom, so a given seed => exactly one
// deterministic signal, with no hidden randomness anywhere in the audio path.
//
// FREQUENCY MODEL (two parts, deliberately kept separable so the "tolerance vs
// drift" must-test can tell them apart):
//   effectiveFreq(t) = freqBase * (1 + tolerance) + drift(t)
//     * freqBase   — the per-voice nominal frequency (seeded).
//     * tolerance  — a STATIC fraction of the base, set once per-voice from the
//                    seed. It does not change over time. This is the analog of a
//                    part's hard-tolerance component tolerance.
//     * drift(t)   — a DYNAMIC, time-varying offset in Hz, itself a sum of two
//                    low-frequency (sub-acoustic) sines with seeded rates/phase.
//                    Because it is a sum of fixed-rate sines its per-sample rate
//                    of change is bounded — it is SLOW DOUBLING-DOWN, not
//                    per-sample white noise. This is what the "drift is bounded
//                    rate" must-test asserts: max |drift(t+1)-drift(t)| < eps.
//
// WAVEFORM: sin() is a documented PLACEHOLDER. The real P3 non-linearity is the
// negistor junction; that is a later, separate implementation. Here the
// non-linearity seam is `nonlinear_`, defaulting to the identity — so the exact
// seam exists now but is inert, and swapping in a negistor curve later cannot
// change the phase-accumulator / frequency-model under test.
//
// NOTE on mutual distinctness: production NEVER dedup-corrects frequencies. If
// two voices happen to derive the same base frequency the bank keeps them as-is
// (free-running and independent is the whole point; "distinct enough" is a test
// assertion, not a production constraint). The test asserts that the derived
// frequencies are pairwise distinct for the chosen seed; if a future seed makes
// a collision, the failure belongs to the test fixture, not to production.
//
// Framework-free, header-only, no heap, no locks, realtime-safe (tick() does not
// allocate or block), SPDX.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class DroneBank {
 public:
  static constexpr std::size_t kMaxVoices = 20;

  // A single voice's full state. Split into three conceptual layers so the
  // tolerance/drift separation is inspectable at the struct boundary:
  //   * static terms (set once, never changed by tick) -> tolerance/noise
  //     separation is decided by WHICH term a test reads.
  //   * accumulator (the only per-oscillator state tick() advances).
  //   * driftNow (the current drifted frequency for this block, recomputed each
  //     tick from the analytic drift model — not a random draw).
  //
  // This is deliberately a member struct (accessible by index) so a test can
  // inspect phase/drift per voice without a private-accessor hole.
  struct Voice {
    double freqBaseHz;   // nominal frequency, seeded, static.
    double tolerance;    // STATIC fractional tolerance of freqBase, seeded.
    double amplitude;    // output gain, seeded.
    double driftF1Hz;    // drift sine-1 rate, seeded (Hz of the sub-drift osc).
    double driftF2Hz;    // drift sine-2 rate, seeded.
    double driftA1;      // drift sine-1 amplitude, seeded (fraction of base).
    double driftA2;      // drift sine-2 amplitude, seeded (fraction of base).
    double driftPh1;     // drift sine-1 phase, seeded (radians).
    double driftPh2;     // drift sine-2 phase, seeded (radians).
    double phase;        // phase accumulator, in radians, advances each tick.
    double driftNow;     // current drifted Hertz offset for this block.
  };

  // sampleRate must be > 0. voiceCount is clamped to [1, kMaxVoices].
  // driftEnabled=false makes the whole dynamic drift model inert (tolerance only
  // remains) — used by the tolerance/drift separation must-test's drift-OFF arm.
  DroneBank(std::uint64_t seed, double sampleRate,
            std::size_t voiceCount = kMaxVoices, bool driftEnabled = true)
      : sampleRate_(sampleRate),
        voiceCount_(voiceCount == 0 ? 1 : (voiceCount > kMaxVoices ? kMaxVoices : voiceCount)),
        driftEnabled_(driftEnabled) {
    SeededRandom rng(seed);
    for (std::size_t i = 0; i < voiceCount_; ++i) {
      // Spread base freqs across a wide, musically-useful band. The exact wrap
      // range is a tuning choice; what matters is that it is seeded and static.
      Voice& v = voices_[i];
      v.freqBaseHz = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;   // 20 .. 2000 Hz
      v.tolerance = rng.nextUnit(0.0, 0.02);                    // 0 .. 2%
      v.amplitude = rng.nextUnit(0.05, 1.0);                    // 0.05 .. 1.0
      // Drift sines are sub-acoustic: well below the audible band, so drift
      // reads as "slowly wandering pitch" not "added tone".
      v.driftF1Hz = rng.nextUnit(0.01, 0.3);
      v.driftF2Hz = rng.nextUnit(0.01, 0.3);
      v.driftA1 = rng.nextUnit(0.0005, 0.01);
      v.driftA2 = rng.nextUnit(0.0005, 0.01);
      v.driftPh1 = rng.nextUnit(0.0, 6.283185307179586);
      v.driftPh2 = rng.nextUnit(0.0, 6.283185307179586);
      v.phase = 0.0;
      // Consistent with the first tick()'s t=0 computation, so there is no
      // constructor->first-sample jump (a startup click). At construction time
      // blockSample_ is still 0, so the drift phases are exactly the seeded ph1/ph2.
      v.driftNow =
          driftEnabled_
              ? v.freqBaseHz *
                    (v.driftA1 * std::sin(v.driftPh1) + v.driftA2 * std::sin(v.driftPh2))
              : 0.0;
    }
  }

  // Advance every voice by one sample and write its (scaled) sample into out[i].
  // out must have room for voiceCount_ values. Realtime-safe.
  void tick(double* out) {
    for (std::size_t i = 0; i < voiceCount_; ++i) {
      Voice& v = voices_[i];
      // Drift is an analytic function of the seeded constants; per-sample change
      // is bounded because each sine is fixed-rate. When drift is disabled we
      // hold it at zero (tolerance-only model).
      // driftA1/driftA2 are fractions of the BASE frequency, so the whole drift
      // term is in Hz and is meaningful (up to ~2% of the base at the std seed),
      // while still being a slow bounded-rate analytic function of time.
      v.driftNow = driftEnabled_
                       ? v.freqBaseHz *
                             (v.driftA1 * std::sin(driftPhase1_(v)) +
                              v.driftA2 * std::sin(driftPhase2_(v)))
                       : 0.0;
      const double effFreq = v.freqBaseHz * (1.0 + v.tolerance) + v.driftNow;
      out[i] = v.amplitude * nonlinear_(std::sin(v.phase));
      v.phase += twoPi_ * effFreq / sampleRate_;
      if (v.phase >= twoPi_) v.phase -= twoPi_;
    }
    ++blockSample_;  // advance the (reconstructed) drift time by one sample.
  }

  // Inspectors, for tests and (read-only) later DSP supervision.
  double phaseOf(std::size_t i) const { return voices_[i].phase; }
  double driftOf(std::size_t i) const { return voices_[i].driftNow; }
  double freqBaseHz(std::size_t i) const { return voices_[i].freqBaseHz; }
  double toleranceOf(std::size_t i) const { return voices_[i].tolerance; }
  double effectiveFreqHz(std::size_t i) const {
    return voices_[i].freqBaseHz * (1.0 + voices_[i].tolerance) + voices_[i].driftNow;
  }
  std::size_t voiceCount() const { return voiceCount_; }
  bool driftEnabled() const { return driftEnabled_; }

 private:
  static constexpr double twoPi_ = 6.283185307179586;

  // Drift sine arguments, each a fixed rate times the (reconstructed) block
  // time. Because we never store absolute samples we track the drift sine phase
  // incrementally is impossible for a single "effFreq" model without extra state,
  // so we reconstruct from a running block-sample counter. This is a bounded
  // analytic function and is exactly reproducible across runs from the seed.
  double driftPhase1_(const Voice& v) const {
    const double t = blockSample_ / sampleRate_;
    return twoPi_ * v.driftF1Hz * t + v.driftPh1;
  }
  double driftPhase2_(const Voice& v) const {
    const double t = blockSample_ / sampleRate_;
    return twoPi_ * v.driftF2Hz * t + v.driftPh2;
  }

  // The negistor non-linearity seam. Identity for now — a documented placeholder
  // (see file comment). A later negistor implementation replaces the lambda
  // state, not the frequency/phase model.
  static double nonlinear_(double x) { return x; }

  double sampleRate_;
  std::size_t voiceCount_;
  bool driftEnabled_;
  double blockSample_ = 0.0;  // running sample count used to reconstruct drift t.
  Voice voices_[kMaxVoices];
};

}  // namespace lunar24::core
