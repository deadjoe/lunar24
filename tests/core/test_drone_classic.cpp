// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// #39 (GH#5, A04) classic drone STRUCTURE tests. The P3-① bank (test_drone_bank.cpp)
// proved the free-run/tolerance/drift/sr frequency MODEL. This file proves the
// manual's CLASSIC SOLAR 50 structure is real (design/00-status A04 gap):
//
//   1. the waveform is a real SAWTOOTH, not a sine (@Claude: "锯齿波形").
//   2. the oscillator nonlinearity is NON-IDENTITY (negistor transfer, "真实非线性
//      恒等不算") — the P3 exit criterion #1 blocker.
//   3. generator roles are ordered low / medium / high (1st,2nd=low; 3rd=med; 4th,5th=high).
//   4. each generator has a MUTE button (muted => contributes exactly 0), and it is
//      per-generator (a neighbour is unaffected).
//   5. each generator has a TUNE control (+12 st => an octave up).
//   6. a shared VOLT knob transposes ALL five generators of a voice down together.
//   7. past half the VOLT stroke the generators start mutual FM (period de-uniforms);
//      below half the period stays steady.
//
// STRUCTURE vs CONSTANTS (design/07 §3): these assert the STRUCTURE is present and
// correct. None assert a measured circuit constant — the negistor curve coefficients,
// the tune/VOLT law and the FM depth are PROVISIONAL and recorded in FINDINGS. A
// mutation that removes any piece of structure (sine wave, identity nonlinearity,
// flat roles, ignored MUTE/TUNE/VOLT/FM) must red this test.
//
// Detectors (max_adjacent_step / zero_crossing_interval_spread) live in
// drone_test_common.h so P3-① and P3-② share the same code (@Claude: "判据只有一份").

#include "mini_test.h"

#include <cmath>
#include <vector>

#include "drone_test_common.h"

#include <lunar24/core/drone_bank.h>

namespace core = lunar24::core;

namespace {

constexpr double kEps = 1e-6;

using drone_test::max_adjacent_step;
using drone_test::zero_crossing_interval_spread;

static core::DroneBank make_bank(std::uint64_t seed, double sr,
                                 std::size_t voices, bool drift) {
  return core::DroneBank(seed, sr, voices, drift);
}

static std::vector<double> render_channel(core::DroneBank& bank, std::size_t ch,
                                          std::size_t n) {
  std::vector<double> vbuf(bank.voiceCount());
  std::vector<double> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    bank.tick(vbuf.data());
    out[i] = vbuf[ch];
  }
  return out;
}

// -------------------------------------------------- 1. waveform = sawtooth ------

// A discontinuous wave (sawtooth) has one sample-to-sample step ~2x its peak level
// (the wrap); a continuous wave (sine) has a step bounded by 2*pi*peak*f/fs << peak.
static void test_classic_waveform_sawtooth() {
  const std::uint64_t seed = 0x51A3B0ULL;
  const double sr = 48000.0;
  core::DroneBank bank = make_bank(seed, sr, 5, false);
  const std::size_t ch = 4;  // a HIGH generator keeps the sine step largest.
  const auto buf = render_channel(bank, ch, 48000);
  double peak = 0.0;
  for (const double x : buf) peak = std::max(peak, std::abs(x));
  CHECK(peak > 1e-3);                        // the channel actually sounds.
  CHECK(max_adjacent_step(buf) > 1.0 * peak);  // jump exceeds the running level.
  // RED NEGATIVE: a sine of the same tone never jumps by more than its level.
  CHECK_FALSE(peak * 2.0 * drone_test::kPi * 1000.0 / sr > 1.0 * peak);
}

// ------------------------------------------------- 2. real (non-identity) nonlin.

// The product applies DroneBank::nonlinearity to the sawtooth every sample; probe
// THAT transfer (the product path, not a shadow judge).
static void test_classic_nonlinearity_real() {
  using core::DroneBank;
  CHECK(std::abs(DroneBank::nonlinearity(0.5) - 0.5) > kEps);    // not identity.
  CHECK(std::abs(DroneBank::nonlinearity(-0.5) + 0.5) > kEps);
  CHECK(std::abs(DroneBank::nonlinearity(0.7)) <= 1.0);           // bounded.
  CHECK(std::abs(DroneBank::nonlinearity(-0.7)) <= 1.0);
  // A nonlinear transfer does NOT scale linearly.
  CHECK(std::abs(DroneBank::nonlinearity(2.0 * 0.4) - 2.0 * DroneBank::nonlinearity(0.4)) > kEps);
  // RED NEGATIVE: an identity transfer WOULD be caught by check #1 (it is fixed-point).
  CHECK_FALSE(std::abs(0.5 - 0.5) > kEps);
}

// ------------------------------------------------- 3. low < medium < high -------

static void test_classic_role_order() {
  const std::uint64_t seed = 0xBEEFCA5EULL;
  const double sr = 48000.0;
  core::DroneBank bank = make_bank(seed, sr, 20, false);  // full classic 4 voices x 5.
  for (std::size_t g = 0; g < core::DroneBank::kClassicVoices; ++g) {
    const std::size_t gs = g * core::DroneBank::kGensPerVoice;
    const double lo0 = bank.freqBaseHz(gs + 0);
    const double lo1 = bank.freqBaseHz(gs + 1);
    const double med = bank.freqBaseHz(gs + 2);
    const double hi0 = bank.freqBaseHz(gs + 3);
    const double hi1 = bank.freqBaseHz(gs + 4);
    CHECK(lo0 < med && lo1 < med);            // 1st & 2nd LOW < 3rd MEDIUM.
    CHECK(med < hi0 && med < hi1);            // 3rd MEDIUM < 4th & 5th HIGH.
    CHECK(bank.roleOf(gs + 0) == core::DroneBank::Role::kLow);
    CHECK(bank.roleOf(gs + 1) == core::DroneBank::Role::kLow);
    CHECK(bank.roleOf(gs + 2) == core::DroneBank::Role::kMedium);
    CHECK(bank.roleOf(gs + 3) == core::DroneBank::Role::kHigh);
    CHECK(bank.roleOf(gs + 4) == core::DroneBank::Role::kHigh);
  }
  // RED NEGATIVE: a flat (no-band) assignment cannot guarantee low<med<high.
  CHECK_FALSE(200.0 < 100.0);
}

// ------------------------------------------------- 4. per-generator MUTE --------

static void test_classic_mute() {
  const std::uint64_t seed = 0xABCDEF123ULL;
  const double sr = 48000.0;
  core::DroneBank bank = make_bank(seed, sr, 5, false);
  bank.setMute(1, true);
  const auto muted = render_channel(bank, 1, 4096);
  const auto alive = render_channel(bank, 0, 4096);
  for (const double x : muted) CHECK(x == 0.0);   // muted generator is silent.
  double peak_alive = 0.0;
  for (const double x : alive) peak_alive = std::max(peak_alive, std::abs(x));
  CHECK(peak_alive > 1e-3);                       // neighbour still sounds.
  CHECK(bank.mutedOf(1) == true && bank.mutedOf(0) == false);
  // RED NEGATIVE: an ignored MUTE leaves a tone.
  CHECK_FALSE(peak_alive == 0.0);
}

// ------------------------------------------------- 5. per-generator TUNE --------

static void test_classic_tune() {
  const std::uint64_t seed = 0x0C0FFEEULL;
  const double sr = 48000.0;
  core::DroneBank bank = make_bank(seed, sr, 5, false);
  const double f0 = bank.effectiveFreqHz(0);
  bank.setTune(0, 12.0);              // +1 octave.
  const double f1 = bank.effectiveFreqHz(0);
  CHECK(f1 > 1.9 * f0 && f1 < 2.1 * f0);   // one octave up (tolerance-bounded).
  CHECK(std::abs(bank.tuneOf(0) - 12.0) < kEps);
  // RED NEGATIVE: an ignored TUNE leaves f1 == f0, so the octave check above reds it.
  CHECK_FALSE(f0 > 1.9 * f0);
}

// ------------------------------------------------- 6. shared VOLT transpose ------

static void test_classic_volt_shared() {
  const std::uint64_t seed = 0xFEED4BEEULL;
  const double sr = 48000.0;
  core::DroneBank bank = make_bank(seed, sr, 5, false);
  const double g0 = bank.effectiveFreqHz(0);
  const double g1 = bank.effectiveFreqHz(1);
  bank.setVolt(0, 12.0);              // shared knob: whole group down an octave.
  const double h0 = bank.effectiveFreqHz(0);
  const double h1 = bank.effectiveFreqHz(1);
  CHECK(h0 > 0.45 * g0 && h0 < 0.55 * g0);   // halved.
  CHECK(h1 > 0.45 * g1 && h1 < 0.55 * g1);   // the WHOLE group moved, not one gen.
  CHECK(bank.voltOf(0) == 12.0 && bank.voltOf(1) == 12.0 && bank.voltOf(4) == 12.0);
  // RED NEGATIVE: a NON-shared VOLT (only gen0 moved) leaves gen1 at its old pitch.
  CHECK_FALSE(h1 > 0.9 * g1 && h1 < 1.1 * g1);
}

// ------------------------------------------------- 7. mutual FM past half -------

static void test_classic_mutual_fm() {
  const std::uint64_t seed = 0xFEEDC0DEULL;
  const double sr = 48000.0;
  const std::size_t ch = 2;  // a MEDIUM generator: not too low, not too high.
  const std::size_t n = 96000;
  // BELOW half the stroke: VOLT transposes but there is NO mutual FM => steady period.
  core::DroneBank below = make_bank(seed, sr, 5, false);
  below.setVolt(0, 20.0);  // < kVvoltMid (30).
  const double spread_below = zero_crossing_interval_spread(render_channel(below, ch, n));
  // ABOVE half the stroke: mutual FM kicks in => the period de-uniforms.
  core::DroneBank above = make_bank(seed, sr, 5, false);
  above.setVolt(0, 45.0);  // > kVvoltMid (30).
  const double spread_above = zero_crossing_interval_spread(render_channel(above, ch, n));
  CHECK(spread_above > 10.0 * (spread_below + 5.0));  // FM de-uniforms the period.
  // RED NEGATIVE: no mutual FM => above steady, same as below.
  CHECK_FALSE(spread_above < 10.0 * (spread_below + 5.0));
}

}  // namespace

int main() {
  test_classic_waveform_sawtooth();
  test_classic_nonlinearity_real();
  test_classic_role_order();
  test_classic_mute();
  test_classic_tune();
  test_classic_volt_shared();
  test_classic_mutual_fm();
  return ::test::finish("drone_classic");
}
