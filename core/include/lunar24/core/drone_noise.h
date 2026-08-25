// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// NoiseSource — P3-②: a seeded, per-sample white-noise generator for the drone
// voice. It is an AMPLITUDE source (a signal), not a frequency source, so the
// cross-sample-rate *frequency* must-test does not apply to it — instead it is
// covered by the fixed-seed reproducibility and buffer-independence must-tests
// (identical per-sample values across partitionings because each tick draws
// exactly one seeded value, none skipped nor repeated).
//
// The value source is a SeededRandom instance owned by NoiseSource, advanced once
// per sample. This is the ONE place the P3 audio path consumes a seeded stream per
// sample (per-sample white noise); DroneBank deliberately consumes the seed only at
// construction. Both are deterministic: a seed => an exactly reproducible buffer.
//
// NOTE on the multi-rate "audible-band noise power" measurement (a P3-② must-test
// that is MEASURE-ONLY, fix deferred): per-sample white noise has a flat one-sided
// spectrum up to Nyquist, so at a higher sample rate the SAME total power spreads
// over a wider band and a fixed 20 Hz..20 kHz audible slice captures LESS of it.
// NoiseSource holds a constant amplitude /sr-independent per-sample distribution;
// the sr-dependence lives entirely in the band, not in the generator. It is
// measured (see test_drone_mod.cpp) and recorded in FINDINGS.md, not "fixed" here,
// because the physically-correct treatment (band-limit/oversample, or normalize by
// bandwidth) is a P3-exit decision to make on evidence, not a silent patch now.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cstdint>

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class NoiseSource {
 public:
  // noise amplitude: each per-sample value is uniform in [-amp, +amp). amp < 0 is
  // taken as |amp| (an amplitude, not a gain sign). Default is unity.
  NoiseSource(std::uint64_t seed, double amplitude = 1.0)
      : rng_(seed), amplitude_(amplitude < 0.0 ? -amplitude : amplitude) {}

  // Advance one sample and write a uniform noise value into *out. Realtime-safe.
  void tick(double* out) {
    // Uniform in [-1, +1) via nextUnit's [0,1), scaled by amplitude.
    *out = (rng_.nextUnit() * 2.0 - 1.0) * amplitude_;
  }

  double amplitude() const { return amplitude_; }

 private:
  SeededRandom rng_;
  double amplitude_;
};

}  // namespace lunar24::core
