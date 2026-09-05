// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SeededRandom — a deterministic, seedable PRNG (splitmix64 variant). There is
// NO global/static state: each instance owns its state and feeds it one seed,
// so two instances constructed from the same seed produce the same sequence and
// an entire render is bit-reproducible. This is a deliberate backstop for the
// P3-① "no hidden randomness" must-test — it removes Rand()/time-seed/
// uninitialized-memory/container-iteration-order as failure paths that would
// otherwise poison every later "bit-identical" comparison.
//
// The P3-① requirement is met by using this ONLY to derive the per-voice
// constants at construction time (a fixed seed => fixed constants => one
// deterministic signal), never as the per-sample random source for the audio
// path. The drift/noise is a deterministic, analytic function of the seeded
// constants (see drone_bank.h), not a stream consumed one value per sample.
//
// Framework-free, header-only, no heap, no locks, SPDX.

#pragma once

#include <cstdint>

namespace lunar24::core {

class SeededRandom {
 public:
  // The splitmix64 additive constant; also a fine default seed.
  static constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ULL;
  static constexpr std::uint64_t kDefaultSeed = kGoldenGamma;

  explicit SeededRandom(std::uint64_t seed = kDefaultSeed) : state_(seed) {}

  // Next 64-bit value. Deterministic: same seed -> same sequence, forever.
  std::uint64_t next() {
    std::uint64_t z = (state_ += kGoldenGamma);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, 1): top 53 bits, so no double rounding bias.
  double nextUnit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

  // Uniform in [lo, hi).
  double nextUnit(double lo, double hi) { return lo + nextUnit() * (hi - lo); }

 private:
  std::uint64_t state_;
};

}  // namespace lunar24::core
