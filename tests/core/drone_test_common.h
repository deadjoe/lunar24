// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared measurement + judgment helpers for the P3 drone tests. Both
// P3-① test_drone_bank.cpp and P3-② test_drone_mod.cpp include this so there is
// exactly ONE copy of every judge — @Claude: "判据只有一份". Any change to a
// detector here is picked up by both slices at once, which is the point: if a
// measure stops being able to tell a real signal from a broken one, neither test
// can hide it.
//
// This is a TEST-only header (tests/core), so it is license-gated but NOT subject
// to the framework-free forbidden-include scan — it may use <vector>/<cstring>.
//
// Helpers:
//   kPi / kTwoPi      — shared constants.
//   measure_freq_hz   — positive-going zero-crossing frequency of a buffer (Hz).
//   wrap_pi           — wrap a phase difference to (-pi, pi].
//   same_render       — bit-identical per-sample render comparison.
//   goertzel_mag      — single-frequency DFT magnitude (Goertzel); for the
//                       aliasing measurement (folded harmonic vs fundamental).
//   noise_sample_var  — sample variance of a buffer; for the audible-band noise
//                       power measurement (multi-rate).

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace drone_test {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// Positive-going zero-crossing frequency (Hz) of a single-voice buffer. Counts
// one period per positive-going crossing; the final window divisor is the last
// crossing interval, so a partial trailing period does not bias the estimate.
inline double measure_freq_hz(const std::vector<double>& buf, double sr) {
  if (buf.size() < 2) return 0.0;
  long up = 0;
  for (std::size_t i = 1; i < buf.size(); ++i)
    if (buf[i - 1] <= 0.0 && buf[i] > 0.0) ++up;
  return static_cast<double>(up) * sr / static_cast<double>(buf.size() - 1);
}

// Wrap an angle (radians) to (-pi, pi]. Phase-difference compare helper.
inline double wrap_pi(double a) {
  while (a <= -kPi) a += kTwoPi;
  while (a > kPi) a -= kTwoPi;
  return a;
}

// Are two per-sample buffers bit-identical?
inline bool same_render(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

// Single-frequency magnitude via Goertzel. Returns an un-normalized amplitude in
// the range [0, N]; because the aliasing test always TAKES A RATIO between a
// folded harmonic and the fundamental, the /N denominator cancels and is omitted.
// `freq` must lie in [0, sr/2]. buf length must be >= 1.
inline double goertzel_mag(const std::vector<double>& buf, double freq, double sr) {
  const double w = kTwoPi * freq / sr;
  const double coeff = 2.0 * std::cos(w);
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  for (const double x : buf) {
    s0 = x + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  // Power at the bin, then magnitude (sqrt). s1/s2 are the y(n-1)/y(n-2) states.
  const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return std::sqrt(power < 0.0 ? 0.0 : power);
}

// Sample variance (biased, /N) of a buffer. This is the total noise power of the
// rendered signal. For the multi-rate audible-band test we measure the variance
// of the actual noise and multiply by the analytic rectangular-band fraction —
// the noise is (per-sample) white, so its one-sided power is flat up to Nyquist.
inline double noise_sample_var(const std::vector<double>& buf) {
  if (buf.empty()) return 0.0;
  double sum = 0.0;
  for (const double x : buf) sum += x;
  const double mean = sum / static_cast<double>(buf.size());
  double acc = 0.0;
  for (const double x : buf) {
    const double d = x - mean;
    acc += d * d;
  }
  return acc / static_cast<double>(buf.size());
}

// Largest sample-to-sample step (|s[n]-s[n-1]|). A DISCONTINUOUS waveform (sawtooth)
// has one step ~2x its peak level (the wrap), while any continuous waveform (sine)
// has a step bounded by 2*pi*peak*f/sr << peak. Used to prove the classic drone wave
// is a real sawtooth, not a sine (A04).
inline double max_adjacent_step(const std::vector<double>& buf) {
  if (buf.size() < 2) return 0.0;
  double m = 0.0;
  for (std::size_t i = 1; i < buf.size(); ++i)
    m = std::max(m, std::abs(buf[i] - buf[i - 1]));
  return m;
}

// Spread (max-min in samples) between consecutive positive-going zero-crossings.
// A fixed-frequency oscillator yields ~uniform intervals (spread ~0); a frequency-
// modulated one yields unequal intervals (spread > 0). This isolates MUTUAL FM from
// the steady transpose of the VOLT knob (transpose shifts the interval length, not
// its uniformity). For a real (unbounded-output) signal a NaN/empty buffer gives 0.
inline double zero_crossing_interval_spread(const std::vector<double>& buf) {
  std::vector<std::size_t> ups;
  for (std::size_t i = 1; i < buf.size(); ++i)
    if (buf[i - 1] <= 0.0 && buf[i] > 0.0) ups.push_back(i);
  if (ups.size() < 2) return 0.0;
  double lo = 1e30, hi = -1e30;
  for (std::size_t k = 1; k < ups.size(); ++k) {
    const double d = static_cast<double>(ups[k] - ups[k - 1]);
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  return hi - lo;
}

}  // namespace drone_test
