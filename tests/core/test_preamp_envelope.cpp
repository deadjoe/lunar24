// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P3-④ must-test suite for the PREAMP (core/include/lunar24/core/preamp.h) and the
// ENVELOPE FOLLOWER (core/include/lunar24/core/envelope_follower.h). @Claude's
// mandate (msg 351ef73b) requires five must-tests, each carrying a REAL red
// negative — a true degradation, not a flag flip. Judges are the shared detectors
// in drone_test_common.h (判据只有一份).
//
//   ①  attack/release are SECONDS, not a sample count. Judge: the same A/R reaches
//       the same envelope value at the same wall-clock time at 44.1k/48k/88.2k/96k.
//       Negative: a fixed-per-sample coefficient (independent of sr) → red.
//   ②  preamp soft-saturation nonlinearity folds harmonics below Nyquist. Measured
//       (Goertzel fundamental vs folded 3rd, dB) and recorded in FINDINGS — NOT
//       fixed here. The probe is a synthetic test point (f0=10 kHz, gain norm 1).
//   ③  EXT SOURCE unconnected (input 0) → no noise, no DC, no NaN. Negative: the
//       caller reads an uninitialized (junk) buffer instead of 0 → red; plus tiny
//       DC-offset and NaN provokes to show all three clauses bite.
//   ④  envelope follower on silence → converges to 0 AND holds, no residual drift.
//       Negative: a buggy floor that parks the envelope above zero (leaky return) → red.
//   ⑤  cross-sr + cross-buffer reuse (drone_test_common.h). Judge: the env follower
//       renders bit-identically in one pass vs 64-sample frames (same_render), and
//       the envelope value at the same wall-clock time agrees across the four rates.
//
// Test-only (tests/core), license-gated, NOT subject to the framework-free scan.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "drone_test_common.h"
#include "lunar24/core/envelope_follower.h"
#include "lunar24/core/preamp.h"
#include "mini_test.h"

using lunar24::core::EnvelopeFollower;
using lunar24::core::Preamp;

namespace {

const std::vector<double> kRates = {44100.0, 48000.0, 88200.0, 96000.0};

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

// First sample index (converted to seconds) where v[i] >= target, or -1.0 if never.
double reach_time_seconds(const std::vector<double>& v, double sr, double target) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] >= target) return static_cast<double>(i) / sr;
  }
  return -1.0;
}

double min_of(const std::vector<double>& v) {
  double m = v.front();
  for (const double x : v) if (x < m) m = x;
  return m;
}

double max_of(const std::vector<double>& v) {
  double m = v.front();
  for (const double x : v) if (x > m) m = x;
  return m;
}

double mean_of(const std::vector<double>& v) {
  double s = 0.0;
  for (const double x : v) s += x;
  return s / static_cast<double>(v.size());
}

// Fold a (possibly above-Nyquist) harmonic back into [0, sr/2].
double fold_to_baseband(double f_hz, double sr) {
  const double nyq = sr / 2.0;
  double r = std::fmod(f_hz, sr);
  if (r < 0.0) r += sr;
  if (r > nyq) r = sr - r;
  return r;
}

// Smallest odd harmonic >= 3 whose order*f0 exceeds Nyquist; returns the folded
// baseband frequency and sets *order.
double first_folded_harmonic(double f0, double sr, int* order) {
  const double nyq = sr / 2.0;
  for (int n = 3;; n += 2) {
    const double h = n * f0;
    if (h > nyq) {
      if (order != nullptr) *order = n;
      return fold_to_baseband(h, sr);
    }
  }
}

// ----------------------------------------------------------------------------
// ① attack/release are seconds, cross-sr wall-clock reach time
// ----------------------------------------------------------------------------
void test_attack_release_are_seconds() {
  const double attackSec = 0.01, releaseSec = 0.02;
  const double amp = 10.0, target = 0.7 * amp;

  std::vector<double> real_times, buggy_times;
  for (const double sr : kRates) {
    const std::size_t n = static_cast<std::size_t>(sr * 0.1);

    // Real: A/R in seconds -> reach time must be the same wall-clock across rates.
    EnvelopeFollower ef(sr);
    ef.setAttackSeconds(attackSec);
    ef.setReleaseSeconds(releaseSec);
    std::vector<double> env(n);
    for (std::size_t i = 0; i < n; ++i) env[i] = ef.tick(amp);
    real_times.push_back(reach_time_seconds(env, sr, target));

    // Negative: a fixed per-sample coefficient, i.e. "attack = so many samples",
    // which scales reach time by 1/sr regardless of sample rate.
    const double buggy_coeff = 1.0 / 240.0;
    double base = 0.0;
    std::vector<double> bug(n);
    for (std::size_t i = 0; i < n; ++i) {
      base += buggy_coeff * (amp - base);
      bug[i] = base;
    }
    buggy_times.push_back(reach_time_seconds(bug, sr, target));
  }

  const double real_spread = max_of(real_times) - min_of(real_times);
  const double buggy_spread = max_of(buggy_times) - min_of(buggy_times);
  std::printf("① seconds reach: real spread %.6f s (%g..%g), buggy spread %.6f s\n",
              real_spread, min_of(real_times), max_of(real_times), buggy_spread);

  CHECK(real_times[0] > 0.0 && real_times[0] < 0.05);
  CHECK(real_spread < 0.001);      // sr-invariant: same wall-clock reach.
  CHECK(buggy_spread > 0.001);     // the fixed-sample negative scales with 1/sr.
}

// ----------------------------------------------------------------------------
// ② preamp soft-saturation aliasing (measure -> FINDINGS, do not fix)
// ----------------------------------------------------------------------------
void test_preamp_aliasing() {
  const double sr = 48000.0, f0 = 10000.0, amp = 1.0;
  const std::size_t n = static_cast<std::size_t>(sr);  // 1 s.

  Preamp p(sr);
  p.setGainNorm(1.0);  // x100, saturates hard -> rich odd harmonics.

  std::vector<double> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    x[i] = p.tick(amp * std::sin(drone_test::kTwoPi * f0 * t));
  }

  int order = 0;
  const double alias = first_folded_harmonic(f0, sr, &order);
  const double fund = drone_test::goertzel_mag(x, f0, sr);
  const double folded = drone_test::goertzel_mag(x, alias, sr);
  const double db = 20.0 * std::log10((folded / fund) + 1e-12);

  std::printf("② preamp aliasing: %dth harmonic fold %g Hz -> %g Hz, "
              "%.2f dB vs fundamental\n", order, order * f0, alias, db);
  CHECK(fund > 0.001);
  CHECK(folded > 0.0);
  CHECK(db > -50.0);  // the fold is a REAL measured component, not a ghost.
}

// ----------------------------------------------------------------------------
// ③ EXT source unconnected -> no noise / DC / NaN
// ----------------------------------------------------------------------------
void test_ext_unconnected() {
  const double sr = 48000.0;
  const std::size_t n = static_cast<std::size_t>(sr * 0.1);

  // Real: the source is unpatched -> input is 0 V -> zero output.
  Preamp p(sr);
  p.setGainNorm(1.0);
  std::vector<double> out(n);
  bool finite = true;
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = p.tick(0.0);
    if (!std::isfinite(out[i])) finite = false;
  }
  const double mean = std::fabs(mean_of(out));
  const double var = drone_test::noise_sample_var(out);
  std::printf("③ unconnected: mean %g, var %.3g, finite %s\n",
              mean, var, finite ? "yes" : "NO");
  CHECK(finite);                 // no NaN.
  CHECK(mean < 1e-9);            // no DC.
  CHECK(var < 1e-9);             // no noise.

  // Negative: the caller reads an uninitialized (junk) buffer instead of 0.
  std::uint32_t seed = 0x12345678u;
  Preamp pb(sr);
  pb.setGainNorm(1.0);
  std::vector<double> junk(n), bad(n);
  for (std::size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    junk[i] = (static_cast<double>((seed >> 8) & 0xFFFFu) / 32768.0) - 1.0;
    bad[i] = pb.tick(junk[i]);
  }
  const double junk_var = drone_test::noise_sample_var(bad);
  std::printf("③ negative (uninit junk): var %.3g\n", junk_var);
  CHECK(junk_var > 1e-6);  // the real (0 V) case had var ~ 0; junk has noise.

  // Clause provokes: a DC-offset stage and a NaN-carrying read, each must trip
  // its own clause so "no DC" / "no NaN" are genuinely exercised, not just "var".
  Preamp pc(sr);
  std::vector<double> dc(n);
  for (std::size_t i = 0; i < n; ++i) dc[i] = pc.tick(0.0) + 0.05;  // leaky +DC.
  CHECK(std::fabs(mean_of(dc)) > 0.01);       // the "no DC" judge would fire.

  Preamp pn(sr);
  pn.setGainNorm(1.0);
  const double nan = std::nan("");
  const double nan_out = pn.tick(nan);
  CHECK(!std::isfinite(nan_out));             // the "no NaN" judge would fire.
}

// ----------------------------------------------------------------------------
// ④ envelope follower on silence converges to 0 and holds
// ----------------------------------------------------------------------------
void test_silence_converges_zero() {
  const double sr = 44100.0;
  const std::size_t onset = static_cast<std::size_t>(sr * 0.05);  // loud 50ms.
  const std::size_t n = static_cast<std::size_t>(sr * 1.5) + onset;

  EnvelopeFollower ef(sr);
  ef.setAttackSeconds(0.01);
  ef.setReleaseSeconds(0.05);
  std::vector<double> env(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double in = (i < onset) ? 10.0 : 0.0;  // onset then silence.
    env[i] = ef.tick(in);
  }

  const double peak = max_of(env);
  const double tail = env.back();
  // no upward rebound in the silent region: non-increasing from the onset peak.
  bool monotone = true;
  for (std::size_t i = onset + 1; i < n; ++i) {
    if (env[i] > env[i - 1] + 1e-12) monotone = false;
  }
  // holds near zero through the final quarter.
  bool holds = true;
  for (std::size_t i = n - n / 4; i < n; ++i) {
    if (env[i] >= 1e-6) holds = false;
  }
  const bool gate_off = (ef.gate() == 0.0);
  std::printf("④ silence: peak %g, tail %.3g, monotone %s, holds %s, gate_off %s\n",
              peak, tail, monotone ? "yes" : "NO", holds ? "yes" : "NO",
              gate_off ? "yes" : "NO");
  CHECK(peak > 5.0);          // the onset really engaged the follower.
  CHECK(tail < 1e-6);         // converged to ~0.
  CHECK(monotone);            // no residual drift upward.
  CHECK(holds);               // stays at 0 (does not float back up).
  CHECK(gate_off);            // gate dropped off.

  // Negative: a leaky return that parks above zero (a fixed floor).
  const double floor = 0.01;
  double base = 0.0;
  const std::size_t on = static_cast<std::size_t>(sr * 0.05);
  std::vector<double> floor_env(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double in = (i < on) ? 10.0 : 0.0;
    const double r = std::fabs(in);
    const double c = 1.0 - std::exp(-1.0 / (sr * (r >= base ? 0.01 : 0.05)));
    base += c * (r - base);
    if (base < floor) base = floor;  // buggy clamp: never reaches zero.
    floor_env[i] = base;
  }
  const double floor_tail = floor_env.back();
  std::printf("④ negative (floor): tail %.3g\n", floor_tail);
  CHECK(floor_tail >= 0.01);  // the "converge to 0" judge would fire.
}

// ----------------------------------------------------------------------------
// ⑤ cross-sr + cross-buffer reuse
// ----------------------------------------------------------------------------
void test_cross_sr_cross_buffer() {
  // A slow, always-positive amplitude wave (volts in [1.8, 9.0]); the env follower
  // rectifies then smooths it, so the same wall-clock envelope is sr-invariant.
  const auto sig = [](std::size_t i, double sr) {
    const double t = static_cast<double>(i) / sr;
    return 9.0 * (0.6 + 0.4 * std::sin(drone_test::kTwoPi * 0.7 * t));
  };

  // Cross-buffer: one pass vs 64-sample frames must be bit-identical.
  const double sr = 48000.0;
  const std::size_t n = static_cast<std::size_t>(sr * 0.4);
  std::vector<double> full(n), part(n);
  EnvelopeFollower fa(sr);
  fa.setAttackSeconds(0.01);
  fa.setReleaseSeconds(0.05);
  for (std::size_t i = 0; i < n; ++i) full[i] = fa.tick(sig(i, sr));
  EnvelopeFollower fb(sr);
  fb.setAttackSeconds(0.01);
  fb.setReleaseSeconds(0.05);
  for (std::size_t base = 0; base < n; base += 64) {
    const std::size_t end = (base + 64 < n) ? base + 64 : n;
    for (std::size_t i = base; i < end; ++i) part[i] = fb.tick(sig(i, sr));
  }
  CHECK(drone_test::same_render(full, part));

  // Cross-sr: the envelope value at the same wall-clock time agrees across rates.
  std::vector<std::vector<double>> per_rate;
  for (const double r : kRates) {
    const std::size_t nn = static_cast<std::size_t>(r * 0.3);
    EnvelopeFollower e(r);
    e.setAttackSeconds(0.01);
    e.setReleaseSeconds(0.05);
    std::vector<double> ev(nn);
    for (std::size_t i = 0; i < nn; ++i) ev[i] = e.tick(sig(i, r));
    per_rate.push_back(ev);
  }
  for (double probe : {0.1, 0.2, 0.25}) {
    const std::size_t i0 = static_cast<std::size_t>(probe * kRates[0]);
    const double ref = per_rate[0][i0];
    for (std::size_t k = 1; k < kRates.size(); ++k) {
      const std::size_t ik = static_cast<std::size_t>(probe * kRates[k]);
      const double val = per_rate[k][ik];
      CHECK(std::fabs(val - ref) < 0.02);
    }
  }
  std::printf("⑤ cross-buffer bit-identical + cross-sr envelope agrees; "
              "ref@0.1s %.4f V\n", per_rate[0][static_cast<std::size_t>(0.1 * kRates[0])]);
}

}  // namespace

int main() {
  test_attack_release_are_seconds();
  test_preamp_aliasing();
  test_ext_unconnected();
  test_silence_converges_zero();
  test_cross_sr_cross_buffer();
  return test::finish("preamp_envelope");
}
