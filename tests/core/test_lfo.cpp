// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) LFO A/B sound-core strong-oracle suite for
// core/include/lunar24/core/lfo.h. This new Lfo is a SEPARATE free-running
// per-sample oscillator — NOT a wrapper over any audio-rate modulation helper —
// and A/B are two real independent instances. The tests pin behaviour / trend /
// determinism and never fake a measured hardware curve (see the header's
// PROVISIONAL modelling constants). The 0..+10V unipolar CV rail is grounded on
// the REAL reg::kJacks lfo_a/b_cv_out descriptor, not a test-side shadow.
//
// @Codex mandate (msg 2ed673a6) must-tests:
//   ① square endpoint / duty trend + triangle linear ramp
//   ② mid morph is neither endpoint and is a per-sample crossfade
//   ③ 0..+10V bounded / finite
//   ④ RATE trend (higher baseHz -> more cycles)
//   ⑤ ×1/×6/×10 multiplier ratio
//   ⑥ four sample-rates wall-clock (44.1/48/88.2/96k, NO fixed 48k)
//   ⑦ block partition per-sample bit-identical
//   ⑧ A/B config + phase isolation (one side reset / change RATE leaves the
//      other's per-sample trace bit-identical)
//   ⑨ reset preserves config
//   ⑩ invalid config fail-closed (a rejected value must NOT pollute the later
//      phase/output trace, and must keep the FULL prior config)
//   ⑪ real registry lfo_a/b_cv_out descriptor (direction/CV-type/unipolar/0..10V)
//
// Negative controls (each narrow old-error RED->revert GREEN) are run separately
// in a detached /tmp worktree: ① bypass WAVE (constant square), ② multiplier
// ignored, ③ phase step fixed 48k, ④ output still bipolar (not mapped to 0..10V),
// ⑤ A/B shared phase, ⑥ invalid-config guard. Detectors read real per-sample
// behaviour — never source grep / a read-only inspector / a self-copied trace.

#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "lunar24/core/lfo.h"
#include "lunar24/registry.hpp"

namespace core = lunar24::core;
namespace reg = lunar24::registry;

namespace {

using core::Lfo;
using core::LfoSpeedMult;
using core::kLfoOutputPeakVolt;

int find_jack(core::JackId id) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::vector<double> run_sig(Lfo& l, int n) {
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) v.push_back(l.tick());
  return v;
}

// Rising-edge count of the square-biased 0..10V signal, crossing up through 5V.
int count_rising_edges(const std::vector<double>& sig) {
  int e = 0;
  for (std::size_t i = 1; i < sig.size(); ++i) {
    if (sig[i - 1] < 5.0 && sig[i] >= 5.0) ++e;
  }
  return e;
}

// One wall-clock second of a square LFO, returning the number of rising edges.
int edges_for(double sr, double baseHz, LfoSpeedMult m) {
  Lfo l(sr);
  l.setBaseHz(baseHz);
  l.setWave(0.0);  // square gives clean rail transitions
  l.setSpeedMult(m);
  auto sig = run_sig(l, static_cast<int>(sr));
  return count_rising_edges(sig);
}

void test_square_endpoint_and_duty() {
  const double sr = 48000.0;
  Lfo l(sr);
  l.setBaseHz(4.0);
  l.setWave(0.0);  // square
  l.setSpeedMult(LfoSpeedMult::x1);
  // square fundamental is exactly +1 (phase<0.5) -> 10V, or -1 (phase>=0.5) -> 0V.
  // 1 second at 4 Hz = 4 whole cycles, so duty (high samples) ~ 0.5 and ~4 edges.
  const int N = static_cast<int>(sr);
  auto sig = run_sig(l, N);
  int hi = 0;
  for (double v : sig) {
    CHECK(std::isfinite(v));
    CHECK(v >= 0.0 && v <= kLfoOutputPeakVolt + 1e-9);
    // square only ever takes the two rail values (no intermediate morph)
    CHECK(v < 1e-6 || std::fabs(v - kLfoOutputPeakVolt) < 1e-6);
    if (v >= 5.0) ++hi;
  }
  const double duty = static_cast<double>(hi) / N;
  CHECK(duty > 0.45 && duty < 0.55);
  const int edges = count_rising_edges(sig);
  CHECK(edges >= 3 && edges <= 5);
}

void test_triangle_linear_ramp() {
  // sr=16, baseHz=1, x1 -> phaseStep=0.0625; one cycle = 16 samples. The triangle
  // has a constant slope magnitude, so every adjacent-sample delta is
  // |4 * phaseStep * 5| = 1.25V (turning points included, since samples straddle
  // them); the peak is exactly 10V and the trough exactly 0V.
  const double sr = 16.0;
  const double stepVolt = 1.25;
  Lfo l(sr);
  l.setBaseHz(1.0);
  l.setWave(1.0);  // triangle
  l.setSpeedMult(LfoSpeedMult::x1);
  auto sig = run_sig(l, 32);  // two cycles
  double mx = -1e9, mn = 1e9;
  for (double v : sig) {
    CHECK(std::isfinite(v));
    CHECK(v >= 0.0 && v <= kLfoOutputPeakVolt + 1e-9);
    mx = std::max(mx, v);
    mn = std::min(mn, v);
  }
  CHECK(std::fabs(mx - kLfoOutputPeakVolt) < 1e-6);
  CHECK(std::fabs(mn - 0.0) < 1e-6);
  for (int i = 1; i < 32; ++i) {
    CHECK(std::fabs(std::fabs(sig[i] - sig[i - 1]) - stepVolt) < 1e-6);
  }
}

void test_wave_morph_crossfade() {
  const double sr = 48000.0, base = 5.0;
  Lfo a(sr), b(sr), c(sr);
  a.setBaseHz(base); a.setWave(0.0);  a.setSpeedMult(LfoSpeedMult::x1);
  b.setBaseHz(base); b.setWave(1.0);  b.setSpeedMult(LfoSpeedMult::x1);
  c.setBaseHz(base); c.setWave(0.5);  c.setSpeedMult(LfoSpeedMult::x1);
  // identical baseHz/sr/mult -> all three stay phase-synchronized (wave does not
  // affect the phase advance), so at each sample their phases are equal and the
  // mid-wave fundamental is the exact linear mean of the two endpoints.
  for (int i = 0; i < 1000; ++i) {
    a.tick(); b.tick(); c.tick();
    const double fa = a.fundamental(), fb = b.fundamental(), fc = c.fundamental();
    CHECK(std::fabs(a.phase() - b.phase()) < 1e-12);
    CHECK(std::fabs(b.phase() - c.phase()) < 1e-12);
    // endpoints differ
    CHECK(std::fabs(fa - fb) > 1e-9);
    // mid is strictly between the endpoints (excludes pure square / pure triangle)
    CHECK(fc > std::min(fa, fb) + 1e-9 && fc < std::max(fa, fb) - 1e-9);
    // and is the per-sample linear crossfade
    CHECK(std::fabs(fc - 0.5 * (fa + fb)) < 1e-9);
  }
}

void test_output_bounded_0_10_finite() {
  const double sr = 44100.0;
  for (double wave : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    Lfo l(sr);
    l.setBaseHz(3.0);
    l.setWave(wave);
    l.setSpeedMult(LfoSpeedMult::x10);
    for (int i = 0; i < static_cast<int>(sr); ++i) {
      const double v = l.tick();
      CHECK(std::isfinite(v));
      CHECK(v >= 0.0 && v <= kLfoOutputPeakVolt + 1e-9);
    }
  }
}

void test_rate_trend() {
  const double sr = 48000.0;
  const int e_lo = edges_for(sr, 2.0, LfoSpeedMult::x1);  // ~2 cycles
  const int e_hi = edges_for(sr, 8.0, LfoSpeedMult::x1);  // ~8 cycles
  // 8 / 2 = 4: higher baseHz must move the wall-clock frequency up
  CHECK(e_hi > e_lo);
  const double ratio = static_cast<double>(e_hi) / static_cast<double>(e_lo);
  CHECK(ratio > 3.0 && ratio < 5.0);
}

void test_speed_multiplier_ratio() {
  const double sr = 48000.0;
  const int e1 = edges_for(sr, 5.0, LfoSpeedMult::x1);   // ~5 cycles
  const int e6 = edges_for(sr, 5.0, LfoSpeedMult::x6);   // ~30 cycles
  const int e10 = edges_for(sr, 5.0, LfoSpeedMult::x10); // ~50 cycles
  CHECK(e6 > e1 && e10 > e6);
  const double r6 = static_cast<double>(e6) / static_cast<double>(e1);
  const double r10 = static_cast<double>(e10) / static_cast<double>(e1);
  CHECK(r6 > 5.0 && r6 < 7.0);
  CHECK(r10 > 9.0 && r10 < 11.0);
}

void test_four_sample_rates_wall_clock() {
  // Same baseHz setting must give the same wall-clock frequency (~5 Hz) at every
  // real sample rate. A fixed 48 kHz phase step would skew 88.2/96k (and 44.1k).
  for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
    const int edges = edges_for(sr, 5.0, LfoSpeedMult::x1);
    CHECK(edges >= 4 && edges <= 6);
  }
}

void test_block_partition_bit_identical() {
  const double sr = 48000.0;
  Lfo a(sr);
  a.setBaseHz(3.0); a.setWave(0.5); a.setSpeedMult(LfoSpeedMult::x6);
  const auto full = run_sig(a, 1000);

  Lfo b(sr);
  b.setBaseHz(3.0); b.setWave(0.5); b.setSpeedMult(LfoSpeedMult::x6);
  std::vector<double> part;
  part.reserve(1000);
  const int chunks[] = {1, 7, 13, 5, 64, 128, 3};
  int done = 0;
  for (int c : chunks) {
    for (int i = 0; i < c; ++i) part.push_back(b.tick());
    done += c;
  }
  while (done < 1000) { part.push_back(b.tick()); ++done; }
  CHECK(part.size() == 1000);
  for (int i = 0; i < 1000; ++i) CHECK(full[i] == part[i]);  // bit-identical
}

void test_ab_isolation() {
  const double sr = 48000.0;
  Lfo a(sr), b(sr);
  a.setBaseHz(5.0); a.setWave(0.0); a.setSpeedMult(LfoSpeedMult::x1);
  b.setBaseHz(5.0); b.setWave(1.0); b.setSpeedMult(LfoSpeedMult::x1);

  // advance A a lot; that must not move B's phase (B was never ticked).
  for (int i = 0; i < 5000; ++i) a.tick();

  const int M = 500;
  const auto trace1 = run_sig(b, M);  // B's actual next M samples (still at phase 0)

  // act on A ONLY: reset + change RATE + advance
  a.reset(); a.setBaseHz(80.0); a.setSpeedMult(LfoSpeedMult::x10);
  for (int i = 0; i < 100; ++i) a.tick();

  // B must be completely unaffected: reproduce its trace from a reset and compare
  // bit-identical. (A shared phase would leave B starting at A's leftover phase, so
  // trace1 (pre-reset) would differ from trace2.)
  b.reset();
  const auto trace2 = run_sig(b, M);
  CHECK(trace1 == trace2);
  // B config untouched, A config changed
  CHECK(b.baseHz() == 5.0 && b.wave() == 1.0 && b.speedMult() == LfoSpeedMult::x1);
  CHECK(a.baseHz() == 80.0 && a.wave() == 0.0 && a.speedMult() == LfoSpeedMult::x10);
}

void test_reset_preserves_config() {
  const double sr = 44100.0;
  Lfo l(sr);
  l.setBaseHz(7.0); l.setWave(0.75); l.setSpeedMult(LfoSpeedMult::x6);
  for (int i = 0; i < 100; ++i) l.tick();
  CHECK(l.phase() > 0.0);
  l.reset();
  CHECK(l.phase() == 0.0);
  CHECK(l.sampleRate() == sr && l.baseHz() == 7.0 && l.wave() == 0.75 &&
        l.speedMult() == LfoSpeedMult::x6);
  // after reset the first sample is again the deterministic phase-step start
  Lfo fresh(sr);
  fresh.setBaseHz(7.0); fresh.setWave(0.75); fresh.setSpeedMult(LfoSpeedMult::x6);
  CHECK(l.tick() == fresh.tick());
}

void test_invalid_config_fail_closed() {
  const double sr = 48000.0;
  const int N = 1000;
  Lfo ctl(sr);
  ctl.setBaseHz(5.0); ctl.setWave(0.25); ctl.setSpeedMult(LfoSpeedMult::x6);
  const auto tctl = run_sig(ctl, N);

  Lfo tampered(sr);
  tampered.setBaseHz(5.0); tampered.setWave(0.25); tampered.setSpeedMult(LfoSpeedMult::x6);
  // every invalid call must return false
  CHECK(tampered.setSampleRate(0.0) == false);
  CHECK(tampered.setSampleRate(std::nan("")) == false);
  CHECK(tampered.setSampleRate(-1.0) == false);
  CHECK(tampered.setBaseHz(-1.0) == false);
  CHECK(tampered.setBaseHz(std::nan("")) == false);
  CHECK(tampered.setWave(std::numeric_limits<double>::infinity()) == false);
  CHECK(tampered.setWave(std::nan("")) == false);
  CHECK(tampered.setSpeedMult(static_cast<LfoSpeedMult>(99)) == false);
  // ... and leave the FULL prior config intact
  CHECK(tampered.sampleRate() == sr && tampered.baseHz() == 5.0 &&
        tampered.wave() == 0.25 && tampered.speedMult() == LfoSpeedMult::x6);
  // ... and must NOT pollute the later phase/output trace
  const auto ttampered = run_sig(tampered, N);
  CHECK(tctl == ttampered);
}

void test_registry_jack_descriptors() {
  const int ia = find_jack(core::JackId::lfo_a_cv_out);
  const int ib = find_jack(core::JackId::lfo_b_cv_out);
  CHECK(ia >= 0 && ib >= 0);
  if (ia >= 0) {
    const auto& j = reg::kJacks[ia];
    CHECK(j.direction == core::PinDirection::output);
    CHECK(j.signalType == core::SignalType::cv);
    CHECK(j.polarity == core::Polarity::unipolar);
    CHECK(j.nominalMin == 0.0 && j.nominalMax == kLfoOutputPeakVolt);
  }
  if (ib >= 0) {
    const auto& j = reg::kJacks[ib];
    CHECK(j.direction == core::PinDirection::output);
    CHECK(j.signalType == core::SignalType::cv);
    CHECK(j.polarity == core::Polarity::unipolar);
    CHECK(j.nominalMin == 0.0 && j.nominalMax == kLfoOutputPeakVolt);
  }
}

}  // namespace

int main() {
  test_square_endpoint_and_duty();
  test_triangle_linear_ramp();
  test_wave_morph_crossfade();
  test_output_bounded_0_10_finite();
  test_rate_trend();
  test_speed_multiplier_ratio();
  test_four_sample_rates_wall_clock();
  test_block_partition_bit_identical();
  test_ab_isolation();
  test_reset_preserves_config();
  test_invalid_config_fail_closed();
  test_registry_jack_descriptors();
  return test::finish("lfo");
}
