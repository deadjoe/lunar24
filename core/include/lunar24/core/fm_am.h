// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// FmAmVoice — P3-②: a sample-accurate FM+AM voice. One carrier sine with a
// modulation LFO applied two ways:
//
//     mod(t)   = sin(phaseMod)                        // modulation LFO, -1..+1
//     instHz(t)= fCarrier + fDevHz * mod(t)           // FREQUENCY modulation
//     amp(t)   = baseAmp  * (1 + depth  * mod(t))     // AMPLITUDE modulation
//     out(t)   = amp(t) * sin(phaseCarrier)
//
// where fDevHz is the peak FM deviation in Hz (the carrier swings by +/-fDevHz as
// mod sweeps -1..+1), and depth is the AM index in [0,1) (at depth=1, mod=-1 zeroes
// the amplitude). BOTH modulations are applied per sample: every tick advances the
// carrier phase by 2*pi*instHz/sampleRate and the modulator phase by 2*pi*fMod/sr
// using the SAME sampleRate, so the output is bit-identical regardless of how the
// render is partitioned (buffer-independence must-test) and deterministic for a
// seed (no hidden randomness).
//
// When fDevHz is large enough that the instantaneous frequency fCarrier+fDevHz
// exceeds Nyquist, the per-sample phase advance itself aliases: the waveform folds
// back into the band at the mirror of the overshoot. That is the FM aliasing
// @Claude asked to MEASURE (measure-only this slice; the fix is a P3-exit
// decision). See test_drone_mod.cpp + FINDINGS.md.
//
// Value from seed: fCarrier, fModHz (the LFO rate), and baseAmp derive once from
// one SeededRandom at construction. fDevHz and depth are passed in because they are
// the quantities a caller/ test sets (the peak deviation and the AM index). A seed
// => fully deterministic.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class FmAmVoice {
 public:
  // sampleRate > 0, fDevHz >= 0, depth in [0,1].
  FmAmVoice(std::uint64_t seed, double sampleRate, double fDevHz, double depth)
      : sampleRate_(sampleRate),
        fDevHz_(fDevHz < 0.0 ? 0.0 : fDevHz),
        depth_(depth < 0.0 ? 0.0 : (depth > 1.0 ? 1.0 : depth)),
        carrierPhase_(0.0),
        modPhase_(0.0) {
    SeededRandom rng(seed);
    fCarrierHz_ = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;  // 20 .. 2000 Hz
    fModHz_ = 0.5 + rng.nextUnit(0.0, 1.0) * 20.0;         // LFO: 0.5 .. 20.5 Hz
    baseAmp_ = 0.05 + rng.nextUnit(0.0, 1.0) * 0.95;       // 0.05 .. 1.0
  }

  // Advance one sample. Realtime-safe.
  void tick(double* out) {
    const double mod = std::sin(modPhase_);
    const double instHz = fCarrierHz_ + fDevHz_ * mod;
    const double amp = baseAmp_ * (1.0 + depth_ * mod);
    *out = amp * std::sin(carrierPhase_);
    carrierPhase_ += twoPi_ * instHz / sampleRate_;
    if (carrierPhase_ >= twoPi_) carrierPhase_ -= twoPi_;
    modPhase_ += twoPi_ * fModHz_ / sampleRate_;
    if (modPhase_ >= twoPi_) modPhase_ -= twoPi_;
  }

  double carrierHz() const { return fCarrierHz_; }
  double fModHz() const { return fModHz_; }
  double fDevHz() const { return fDevHz_; }
  double depth() const { return depth_; }
  double baseAmp() const { return baseAmp_; }

 private:
  static constexpr double twoPi_ = 6.283185307179586;

  double sampleRate_;
  double fCarrierHz_;
  double fModHz_;
  double fDevHz_;
  double depth_;
  double baseAmp_;
  double carrierPhase_;
  double modPhase_;
};

}  // namespace lunar24::core
