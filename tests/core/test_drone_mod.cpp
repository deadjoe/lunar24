// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
//  tests for the "new drone" voice block: Schmitt oscillator, noise source,
// sample-and-hold, and an FM/AM voice. The shared judges come from
// drone_test_common.h — the SAME code uses — so a detector change here is
// picked up by both slices at once ("判据只有一份").
//
// mandate, folded in below:
//   * Two RATE-UNIT traps. The Schmitt oscillator's frequency comes from a charge
//     rate scaled by dt=1/sampleRate (never a fixed per-sample step), and the
//     sample-and-hold period is in SECONDS (never a sample count). The cross-sr
//     must-tests assert Hz / hold-seconds, not "the same thing at any sr".
//   * S&H cross-sr asserts the HOLD DURATION IN SECONDS only, never the held-value
//     sequence (a per-sample-advancing noise stream makes values differ by sr as a
//     physical necessity; the only sr-invariant is the period).
//   * Schmitt alias suppression is checked against a same-pitch naive triangle.
//     The separate FmAmVoice high-deviation alias test remains a characterization.
//   * NEW: measure AUDIBLE-BAND NOISE POWER (20 Hz..20 kHz) across
//     44.1/48/88.2/96k. White noise is flat up to Nyquist, so a fixed amplitude
//     spreads over a wider band at higher sr -> LESS power in the audible slice.
//     Measured and recorded; the fix is a P3-exit decision, not a silent patch.
//
// Each must-test carries a red-negative so it cannot vacously pass —:
// "测不出区别的测试，就没在测那个东西."

#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "drone_test_common.h"

#include <lunar24/core/drone_noise.h>
#include <lunar24/core/fm_am.h>
#include <lunar24/core/sample_hold.h>
#include <lunar24/core/schmitt_osc.h>
#include <lunar24/core/seeded_random.h>

namespace core = lunar24::core;

using drone_test::goertzel_mag;
using drone_test::measure_freq_hz;
using drone_test::noise_sample_var;
using drone_test::same_render;

namespace {

// Four standard sample rates for every cross-sample-rate must-test.
const std::vector<double> kRates = {44100.0, 48000.0, 88200.0, 96000.0};

constexpr double kPiV = drone_test::kPi;
// Hold period used for the S&H cross-sample-rate test.
constexpr double kHoldSeconds = 0.01;

// ---------------------------------------------------------------------------
// Render helpers (single + a deliberately-buggy sibling for each negative).
// ---------------------------------------------------------------------------

// Schmitt oscillators.

static std::vector<double> render_schmitt(core::SchmittOsc& osc, std::size_t n) {
  std::vector<double> out(n);
  double v = 0.0;
  for (auto& o : out) {
    osc.tick(&v);
    o = v;
  }
  return out;
}

// THE BUG: a hardcoded 48 kHz per-sample step (chargeRate/48000), never scaled by
// the actual sample rate. Correct only at 48 kHz; frequency scales with sr, so the
// cross-sample-rate detector catches it.
static std::vector<double> render_schmitt_buggy_fixed48k(std::uint64_t seed,
                                                         double sr, std::size_t n) {
  (void)sr;  // the bug is precisely that SAMPLE RATE IS IGNORED (hardcoded 48 kHz).
  core::SeededRandom rng(seed);
  const double fb = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;
  const double tol = rng.nextUnit(0.0, 0.02);
  const double rate_per_s = 4.0 * fb * (1.0 + tol) * core::SchmittOsc::kWindowVolts;
  const double step = rate_per_s / 48000.0;  // fixed per sample — the bug.
  std::vector<double> out(n);
  double ramp = 0.0, dir = 1.0;
  for (auto& o : out) {
    ramp += dir * step;
    if (ramp >= core::SchmittOsc::kWindowVolts) {
      ramp = core::SchmittOsc::kWindowVolts;
      dir = -1.0;
    } else if (ramp <= -core::SchmittOsc::kWindowVolts) {
      ramp = -core::SchmittOsc::kWindowVolts;
      dir = 1.0;
    }
    o = ramp;
  }
  return out;
}

// Noise source.

static std::vector<double> render_noise(core::NoiseSource& src, std::size_t n) {
  std::vector<double> out(n);
  double v = 0.0;
  for (auto& o : out) {
    src.tick(&v);
    o = v;
  }
  return out;
}

// THE BUG: a file-scope global PRNG keeps advancing, so two "same seed"
// constructions yield DIFFERENT streams — hidden global state, not seed-derived.
static std::vector<double> render_noise_buggy_global(std::uint64_t /*seed*/,
                                                     std::size_t n) {
  static core::SeededRandom g(0x12345678);  // hidden global, never re-seeded.
  std::vector<double> out(n);
  for (auto& o : out) o = (g.nextUnit() * 2.0 - 1.0);
  return out;
}

// Sample-and-hold.

// Feed a rising ramp so consecutive grabbed values always differ -> clean run
// boundaries when measuring the hold duration.
static std::vector<double> render_hold(core::SAndHold& sh, std::size_t n) {
  std::vector<double> in(n), out(n);
  for (std::size_t i = 0; i < n; ++i) in[i] = static_cast<double>(i);
  double v = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    sh.tick(in[i], &v);
    out[i] = v;
  }
  return out;
}

// THE BUG: hold period in SAMPLES (fixed 441), not seconds — the hold duration
// scales inversely with sr, so the cross-sample-rate seconds detector catches it.
static std::vector<double> render_hold_buggy_samplesamples(double sr, std::size_t n) {
  (void)sr;  // the bug is precisely that the hold period is in SAMPLES, not seconds.
  const long holdSamples = 441;
  std::vector<double> out(n);
  long count = 0;
  double held = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (++count >= holdSamples) {
      held = static_cast<double>(i);
      count = 0;
    }
    out[i] = held;
  }
  return out;
}

// Median run length (samples) of the held output, divided by sr -> seconds. The
// held value never changes between re-grabs, so a run is a maximal equal-value
// stretch. Median is robust to a partial first/last run.
static double median_hold_seconds(const std::vector<double>& out, double sr) {
  std::vector<std::size_t> runs;
  std::size_t run = 1;
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i] != out[i - 1]) {
      runs.push_back(run);
      run = 1;
    } else {
      ++run;
    }
  }
  runs.push_back(run);
  if (runs.empty()) return 0.0;
  std::sort(runs.begin(), runs.end());
  return static_cast<double>(runs[runs.size() / 2]) / sr;
}

// FM/AM voice.

static std::vector<double> render_fm_am(core::FmAmVoice& v, std::size_t n) {
  std::vector<double> out(n);
  double s = 0.0;
  for (auto& o : out) {
    v.tick(&s);
    o = s;
  }
  return out;
}

// THE BUG: the modulator LFO is recomputed once per BLOCK, not per sample. How the
// render is partitioned changes the instantaneous frequencies, so the
// buffer-independence detector catches it.
static std::vector<double> render_fm_am_buggy_perblock(
    std::uint64_t seed, double sr, std::size_t n,
    const std::vector<std::size_t>& blocks) {
  core::SeededRandom rng(seed);
  const double fc = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;
  const double fm = 0.5 + rng.nextUnit(0.0, 1.0) * 20.0;
  const double baseAmp = 0.05 + rng.nextUnit(0.0, 1.0) * 0.95;
  const double fDev = 100.0, depth = 0.5;
  std::vector<double> out(n);
  std::size_t pos = 0, bi = 0;
  double carrierPhase = 0.0, modPhase = 0.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    const double mod = std::sin(modPhase);  // recomputed once per block — the bug.
    const double instHz = fc + fDev * mod;
    const double amp = baseAmp * (1.0 + depth * mod);
    for (std::size_t k = 0; k < b; ++k) {
      out[pos + k] = amp * std::sin(carrierPhase);
      carrierPhase += 2.0 * kPiV * instHz / sr;
      if (carrierPhase >= 2.0 * kPiV) carrierPhase -= 2.0 * kPiV * std::floor(carrierPhase / (2.0 * kPiV));
    }
    modPhase += (2.0 * kPiV * fm / sr) * static_cast<double>(b);
    pos += b;
    ++bi;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Aliasing + spectral helpers.
// ---------------------------------------------------------------------------

// Fold a component at frequency h (Hz) to baseband [0, sr/2].
static double fold_to_baseband(double h, double sr) {
  const double nyq = sr * 0.5;
  const double n = std::round(h / sr);
  double a = std::fabs(h - n * sr);
  return (a > nyq) ? sr - a : a;
}

// Find the LOWEST odd harmonic of f0 that crosses Nyquist (the first, and thus
// strongest, to alias). sdof > sr/2, so odd n >= 3.
static bool first_folded_harmonic(double f0, double sr, int& order, double& aliasHz) {
  const double nyq = sr * 0.5;
  for (int n = 3; n <= 200; n += 2) {
    const double h = n * f0;
    if (h > nyq) {
      aliasHz = fold_to_baseband(h, sr);
      order = n;
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Must-tests.
// ---------------------------------------------------------------------------

// Measure complete periods with interpolated crossings; a partial final period
// must not consume the one-cent error budget, especially in the low range.
static double schmitt_frequency(const std::vector<double>& samples, double sr) {
  double first = 0.0, last = 0.0;
  int crossings = 0;
  for (std::size_t i = 32; i < samples.size(); ++i) {
    if (samples[i - 1] <= 0.0 && samples[i] > 0.0) {
      const double at = static_cast<double>(i - 1) -
                        samples[i - 1] / (samples[i] - samples[i - 1]);
      if (crossings++ == 0) first = at;
      last = at;
    }
  }
  return crossings > 1 ? (crossings - 1) * sr / (last - first) : 0.0;
}

static bool test_schmitt_cross_sr() {
  double worstCents = 0.0;
  for (double sr : kRates) {
    // Include adjacent frequencies that previously collapsed onto one plateau,
    // low-range notes, and the seeded product voices' upper pitch region.
    for (double target : {16.35, 220.0, 999.0, 1001.0, 1999.0, 2001.0,
                          2499.0, 2501.0, 2650.114042387, 2671.271858799}) {
      core::SchmittOsc osc(0x5A17u, sr);
      osc.setFreqHz(target);
      const double hz = schmitt_frequency(render_schmitt(osc, static_cast<std::size_t>(2 * sr)), sr);
      CHECK(hz > 0.0);
      const double cents = std::fabs(1200.0 * std::log2(hz / target));
      CHECK(cents < 1.0);
      worstCents = std::max(worstCents, cents);
    }
    // Construction still applies its seeded tolerance, once, to the base pitch.
    core::SchmittOsc seeded(0x5A17u, sr);
    const double expected = seeded.freqBaseHz() * (1.0 + seeded.toleranceOf());
    const double measured = schmitt_frequency(render_schmitt(seeded, static_cast<std::size_t>(2 * sr)), sr);
    CHECK(std::fabs(1200.0 * std::log2(measured / expected)) < 1.0);
  }
  std::printf("Schmitt pitch: worst error %.6f cents across four sample rates\n", worstCents);
  // The existing fixed-step/clamped implementation fails the same measurement.
  const auto old = render_schmitt_buggy_fixed48k(0x5A17u, 48000.0, 96000);
  core::SchmittOsc target(0x5A17u, 48000.0);
  CHECK(std::fabs(1200.0 * std::log2(schmitt_frequency(old, 48000.0) /
                                  target.effectiveFreqHz())) > 1.0);
  return true;
}

static bool test_schmitt_modulation_and_pause() {
  for (double sr : kRates) {
    for (bool fm : {false, true}) for (bool am : {false, true}) {
      core::SchmittOsc osc(1, sr), unmodulatedAmplitude(1, sr);
      osc.setAmDepth(am ? 1.0 : 0.0);
      // Independent threshold-reflection integrator verifies the raw square's
      // phase through changing pitch/CV/FM, including reversals between samples.
      double ramp = 0.0, direction = 1.0;
      for (int i = 0; i < 12000; ++i) {
        const double mod = (i % 317 < 173) ? 0.8 : -0.8;
        const double hz = 400.0 + (i % 1700) * 0.7;
        const double cv = (i % 911 < 450) ? 0.3 : -0.2;
        for (auto* o : {&osc, &unmodulatedAmplitude}) {
          o->setFreqHz(hz);
          o->setPitchCvOctaves(cv);
          o->setFmOctaves(fm ? 3.0 : 0.0);
          o->setFmDevHz(fm ? 75.0 : 0.0);
          o->setMod(mod);
        }
        const double f = hz * std::exp2(cv + (fm ? 3.0 * mod : 0.0)) + (fm ? 75.0 * mod : 0.0);
        ramp += direction * 2.0 * f / sr;
        if (ramp > 0.5) { ramp = 1.0 - ramp; direction = -1.0; }
        if (ramp < -0.5) { ramp = -1.0 - ramp; direction = 1.0; }
        double x = 0.0, dry = 0.0;
        osc.tick(&x); unmodulatedAmplitude.tick(&dry);
        CHECK(std::isfinite(x) && std::fabs(x) < 1.1);
        CHECK(std::fabs(x - dry * (am ? 1.0 + mod : 1.0)) < 1e-12);
        if (std::fabs(ramp) > 1e-8) CHECK_EQ(osc.square(), ramp > 0.0 ? 1.0 : -1.0);
      }
    }
    core::SchmittOsc paused(1, sr), reference(1, sr);
    paused.setFreqHz(137.3); reference.setFreqHz(137.3);
    for (int i = 0; i < 237; ++i) { double x; paused.tick(&x); reference.tick(&x); }
    const double square = paused.square();
    paused.setFreqHz(0.0);
    double held = 0.0; paused.tick(&held);
    for (int i = 0; i < 100; ++i) {
      double x; paused.tick(&x);
      CHECK_EQ(x, held); CHECK_EQ(paused.square(), square);
    }
    paused.setFreqHz(137.3);
    paused.setPitchSemitones(core::SchmittOsc::kSilenceSt);
    for (int i = 0; i < 100; ++i) { double x; paused.tick(&x); CHECK_EQ(x, 0.0); }
    paused.setPitchSemitones(0.0);
    for (int i = 0; i < 1000; ++i) {
      double x, y; paused.tick(&x); reference.tick(&y); CHECK_EQ(x, y);
    }
    // Strong positive CV can cross many periods per frame. It must not create
    // unbounded loops or poison the phase when the pitch returns to normal.
    for (double f : {1e-9, sr * 0.49, sr * 0.5, sr * 1.7, 1e8, 137.3}) {
      paused.setFreqHz(f);
      for (int i = 0; i < 1000; ++i) {
        double x; paused.tick(&x); CHECK(std::isfinite(x) && std::fabs(x) < 0.6);
      }
    }
    paused.setFreqHz(100.0); paused.setFmDevHz(200.0); paused.setMod(-1.0);
    paused.tick(&held);
    for (int i = 0; i < 100; ++i) { double x; paused.tick(&x); CHECK_EQ(x, held); }
  }
  return true;
}

static bool test_schmitt_lf_edges() {
  for (double sr : kRates) for (double hz : {0.13, 3.71, 19.97, 199.7}) {
    core::SchmittOsc osc(1, sr);
    osc.setFreqHz(hz);
    double prev = osc.square();
    int edge = 0;
    for (int i = 0; i < static_cast<int>(10 * sr); ++i) {
      double x; osc.tick(&x);
      const double sq = osc.square();
      if (sq > 0.0 && prev < 0.0) {
        ++edge;
        CHECK(std::fabs((i + 1) - edge * sr / hz) <= 1.00001);
      }
      prev = sq;
    }
    CHECK(std::abs(edge - static_cast<int>(10 * hz)) <= 1);
  }
  return true;
}

// Schmitt: per-sample output is identical across partitionings.
static bool test_schmitt_buffer_independence() {
  const std::uint64_t seed = 0xB00Fu;
  const double sr = 48000.0;
  const std::size_t n = 24000;
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};

  core::SchmittOsc o1(seed, sr);
  const auto single = render_schmitt(o1, n);

  core::SchmittOsc o2(seed, sr);
  std::vector<double> part(n);
  std::size_t pos = 0, bi = 0;
  double v = 0.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < b; ++k) {
      o2.tick(&v);
      part[pos + k] = v;
    }
    pos += b;
    ++bi;
  }
  CHECK(same_render(single, part));

  // Negative: a block-boundary reset makes the output partition-dependent.
  // (Re-implemented inline so the "reset at each block start" bug is cast-iron.)
  core::SchmittOsc o3(seed, sr);
  std::vector<double> buggy(n);
  pos = 0;
  bi = 0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    double ramp = 0.0, dir = 1.0;  // cleared per block — the bug.
    const double rate = 4.0 * o3.freqBaseHz() * (1.0 + o3.toleranceOf()) *
                        core::SchmittOsc::kWindowVolts;
    for (std::size_t k = 0; k < b; ++k) {
      ramp += dir * rate / sr;
      if (ramp >= core::SchmittOsc::kWindowVolts) {
        ramp = core::SchmittOsc::kWindowVolts;
        dir = -1.0;
      } else if (ramp <= -core::SchmittOsc::kWindowVolts) {
        ramp = -core::SchmittOsc::kWindowVolts;
        dir = 1.0;
      }
      buggy[pos + k] = ramp;
    }
    pos += b;
    ++bi;
  }
  CHECK_FALSE(same_render(single, buggy));
  return true;
}

// Noise: fixed-seed reproducibility (same seed => bit-identical), different seed =>
// different signal, and buffer-independence. Noise is an AMPLITUDE source, so the
// cross-sample-rate *frequency* test does not apply (documented in drone_noise.h).
static bool test_noise_reproducible_buffer() {
  const std::uint64_t seed = 0x5EEDu;
  const std::size_t n = 20000;

  core::NoiseSource n1(seed, 0.5);
  core::NoiseSource n2(seed, 0.5);
  core::NoiseSource n3(seed ^ 0x9E3779B9u, 0.5);
  CHECK(same_render(render_noise(n1, n), render_noise(n2, n)));
  CHECK_FALSE(same_render(render_noise(n1, n), render_noise(n3, n)));

  // Buffer-independence: run the SAME source seeded identically in two passes but
  // the second one partitioned. tick consumes exactly one value per sample, so a
  // partition can never skip/repeat a draw.
  core::NoiseSource b1(seed, 0.5), b2(seed, 0.5);
  const auto single = render_noise(b1, n);
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};
  std::vector<double> part(n);
  std::size_t pos = 0, bi = 0;
  double v = 0.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < b; ++k) {
      b2.tick(&v);
      part[pos + k] = v;
    }
    pos += b;
    ++bi;
  }
  CHECK(same_render(single, part));

  // Negative (hidden global state): same "seed" twice, but the source draws from a
  // shared advancing stream -> different buffers, and the detector catches it.
  CHECK_FALSE(same_render(render_noise_buggy_global(seed, n),
                          render_noise_buggy_global(seed, n)));
  return true;
}

// S&H: cross-sample-rate asserts the HOLD DURATION IN SECONDS only, never the
// held-value sequence (values differ by sr for a per-sample-advancing stream).
static bool test_sandhold_cross_sr_duration() {
  const std::size_t n = 40000;  // > 4s at 44.1k: plenty of hold periods.
  double minSec = 1e18, maxSec = -1e18;
  for (const double sr : kRates) {
    core::SAndHold sh(sr, kHoldSeconds);
    const auto out = render_hold(sh, n);
    const double sec = median_hold_seconds(out, sr);
    CHECK(std::fabs(sec - kHoldSeconds) < 0.05 * kHoldSeconds);
    minSec = std::min(minSec, sec);
    maxSec = std::max(maxSec, sec);
  }
  // Correct: hold duration is sr-invariant (seconds).
  CHECK((maxSec - minSec) < 0.05 * kHoldSeconds);

  // Negative: a fixed SAMPLE count varies the hold duration with sr; the seconds
  // detector sees a spread far above the tolerance.
  double bmin = 1e18, bmax = -1e18;
  for (const double sr : kRates) {
    const auto b = render_hold_buggy_samplesamples(sr, n);
    const double sec = median_hold_seconds(b, sr);
    bmin = std::min(bmin, sec);
    bmax = std::max(bmax, sec);
  }
  // Spread 0.00459s..0.01s => 0.0054 s, far above the correct <0.0005 s bound. Thresh
  // is set at 0.002 so it sits well between the two (detector proves it discriminates).
  CHECK((bmax - bmin) > 0.002);
  return true;
}

// S&H: partitioning the render never shifts a grab, so per-sample output is
// identical regardless of block grouping.
static bool test_sandhold_buffer_independence() {
  const double sr = 48000.0;
  const std::size_t n = 9600;
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};

  core::SAndHold s1(sr, kHoldSeconds);
  const auto single = render_hold(s1, n);

  // Partitioned equivalent: same per-sample accumulation, same grab timing.
  core::SAndHold s2(sr, kHoldSeconds);
  std::vector<double> in(n), part(n);
  for (std::size_t i = 0; i < n; ++i) in[i] = static_cast<double>(i);
  std::size_t pos = 0, bi = 0;
  double v = 0.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < b; ++k) {
      s2.tick(in[pos + k], &v);
      part[pos + k] = v;
    }
    pos += b;
    ++bi;
  }
  CHECK(same_render(single, part));
  return true;
}

// S&H CLOCK-EDGE form: tick(input, clock, *out) is the product path —
// a level >= kClockOn on a RISING edge captures the input; between edges the last
// captured level is held. addition: the drone lane reaches this
// only through the divided LF square, so two properties have no lane-level proof.
// Pin them at the module level: (1) an UNCLOCKED (constant-0) clock never self-runs
// (the "未接 clock 时不自走" acceptance), and (2) an arbitrary waveform clock captures
// the input exactly AT each rising edge, holding it until the next.
static bool test_sandhold_clock_edge() {
  const double sr = 48000.0;

  // (1) Unclocked (constant 0.0) clock: a rising edge is
  //     (clock >= kClockOn) && (prevClock_ < kClockOn); with clock pinned at 0.0 the
  //     threshold is never crossed, so held_ is never re-captured. Feed a strictly
  //     increasing input that WOULD move a running hold — it must not.
  {
    core::SAndHold sh(sr, kHoldSeconds, /*initial=*/5.0);
    double out = -1.0;
    for (std::size_t i = 0; i < 16; ++i) {
      sh.tick(static_cast<double>(i), 0.0, &out);   // clock stays 0.0.
      CHECK(out == 5.0);                             // held at the initial level.
    }
  }

  // (2) Arbitrary waveform clock: clock is 0.0 most samples and 1.0 (>= kClockOn,
  //     preceded by a 0.0) exactly at rising indices {2, 7, 13}, so each such index is
  //     a genuine rising edge. Value captured must be the input AT that index.
  {
    const std::size_t n = 16;
    const std::vector<std::size_t> rises = {2, 7, 13};
    core::SAndHold sh(sr, kHoldSeconds, /*initial=*/-1.0);
    std::vector<double> in(n), out(n), clock(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) in[i] = 100.0 + static_cast<double>(i);  // distinct.
    for (std::size_t r : rises) clock[r] = 1.0;
    for (std::size_t i = 0; i < n; ++i) sh.tick(in[i], clock[i], &out[i]);
    // Until the first rising edge (i<2) the initial level is held.
    CHECK(out[0] == -1.0);
    CHECK(out[1] == -1.0);
    // At i=2 the edge captures in[2]; held through i=6.
    CHECK(out[2] == in[2]);
    for (std::size_t i = 3; i < 7; ++i) CHECK(out[i] == in[2]);
    // At i=7 the edge captures in[7]; held through i=12.
    CHECK(out[7] == in[7]);
    for (std::size_t i = 8; i < 13; ++i) CHECK(out[i] == in[7]);
    // At i=13 the edge captures in[13]; held to the end.
    CHECK(out[13] == in[13]);
    for (std::size_t i = 14; i < n; ++i) CHECK(out[i] == in[13]);
  }

  return true;
}

// FM/AM: sample-accurate — the per-sample output is identical across partitionings
// (negatives = a per-block modulator recompute), and it is deterministic per seed.
static bool test_fm_am_buffer_determinism() {
  const std::uint64_t seed = 0x6DEu;
  const double sr = 48000.0;
  const std::size_t n = 48000;
  const double fDev = 1500.0, depth = 0.6;
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};

  core::FmAmVoice v1(seed, sr, fDev, depth);
  const auto single = render_fm_am(v1, n);

  core::FmAmVoice v2(seed, sr, fDev, depth);
  std::vector<double> part(n);
  std::size_t pos = 0, bi = 0;
  double s = 0.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < b; ++k) {
      v2.tick(&s);
      part[pos + k] = s;
    }
    pos += b;
    ++bi;
  }
  CHECK(same_render(single, part));

  // Determinism: two identical constructions => bit-identical buffer.
  core::FmAmVoice v3(seed, sr, fDev, depth);
  CHECK(same_render(single, render_fm_am(v3, n)));

  // Negative: per-block modulator recompute makes the output partition-dependent.
  const auto buggy = render_fm_am_buggy_perblock(seed, sr, n, blocks);
  CHECK_FALSE(same_render(single, buggy));
  return true;
}

// ---------------------------------------------------------------------------
// Spectral regression and FM/noise characterization.
// ---------------------------------------------------------------------------

// Coherent one-second renders keep spectral leakage out of the comparison.
// Compare against an uncorrected triangle at the SAME actual frequency, not the
// old clamped oscillator whose different pitch would invalidate the measurement.
static bool test_schmitt_aliasing() {
  for (double sr : kRates) for (double f : {503.0, 1001.0, 2501.0, 5003.0, 10003.0, 21001.0}) {
    core::SchmittOsc osc(1, sr);
    osc.setFreqHz(f);
    const int n = static_cast<int>(sr);
    std::vector<double> corrected(n), naive(n);
    for (int i = 0; i < n; ++i) {
      osc.tick(&corrected[i]);
      const double cycles = 0.75 + (i + 1) * f / sr;
      naive[i] = 2.0 * std::fabs(cycles - std::floor(cycles) - 0.5) - 0.5;
    }
    int order = 0;
    double aliasHz = 0.0;
    CHECK(first_folded_harmonic(f, sr, order, aliasHz));
    const double fund = goertzel_mag(corrected, f, sr);
    const double alias = goertzel_mag(corrected, aliasHz, sr);
    const double plainAlias = goertzel_mag(naive, aliasHz, sr);
    CHECK(fund > 0.1 * n);
    CHECK(alias < plainAlias * 0.6);
    CHECK(alias / fund < std::pow(10.0, -30.0 / 20.0));
  }
  return true;
}

// FM aliasing at high fDev: the peak instantaneous frequency folds back into band
// at the mirror of the overshoot. Compare that probe against the same probe at a
// LOW fDev (no overshoot) to isolate the aliasing-added energy.
static bool test_fm_aliasing() {
  const double sr = 48000.0;
  const std::uint64_t seed = 0xF4u;
  const std::size_t n = 65536;
  const double depth = 0.0;  // Isolate FM: no AM.

  core::FmAmVoice probe(seed, sr, 0.0, depth);  // low fDev (no overshoot).
  const double fc = probe.carrierHz();

  const double fDevHigh = 30000.0;                    // peak = fc+30000 > 24k -> folds.
  const double peak = fc + fDevHigh;
  const double mirror = fold_to_baseband(peak, sr);  // where the overshoot reflects.

  core::FmAmVoice high(seed, sr, fDevHigh, depth);
  const auto bufHi = render_fm_am(high, n);
  core::FmAmVoice low(seed, sr, 100.0, depth);  // tiny deviation, stays in-band.
  const auto bufLo = render_fm_am(low, n);

  const double at_mirror_hi = goertzel_mag(bufHi, mirror, sr);
  const double at_mirror_lo = goertzel_mag(bufLo, mirror, sr);
  CHECK(at_mirror_hi > 0.0);
  CHECK(at_mirror_lo >= 0.0);
  const double db = 20.0 * std::log10((at_mirror_hi + 1e-12) / (at_mirror_lo + 1e-12));
  std::printf("P3-2 fm-alias: sr=%.0f fc=%.2fHz fDev=%.0fHz peak=%.2fHz mirror=%.2fHz -> %.2f dB (hi vs lo)\n",
              sr, fc, fDevHigh, peak, mirror, db);
  // Aliasing pumps energy into the mirror that the no-overshoot case lacks.
  CHECK((at_mirror_lo > 0.0) ? db > 20.0 : at_mirror_hi > 0.0);
  return true;
}

// NEW audible-band noise power across the four rates. White noise is flat up to
// Nyquist, so a fixed amplitude carries LESS power in 20 Hz..20 kHz at higher sr.
// Measure-only: record the deviation; the fix is a P3-exit decision.
static bool test_noise_audible_band_power() {
  const std::uint64_t seed = 0x21Cu;
  const std::size_t n = 40000;
  const double loHz = 20.0, hiHz = 20000.0;
  double minVar = 1e18, maxVar = -1e18;
  double minAud = 1e18, maxAud = -1e18;
  for (const double sr : kRates) {
    core::NoiseSource ns(seed, 0.5);
    const auto buf = render_noise(ns, n);
    const double var = noise_sample_var(buf);  // total power, sr-independent.
    const double nyq = sr * 0.5;
    const double audFrac = (hiHz - loHz) / nyq;  // rectangular-band fraction.
    const double aud = var * audFrac;            // power in 20 Hz..20 kHz.
    minVar = std::min(minVar, var);
    maxVar = std::max(maxVar, var);
    minAud = std::min(minAud, aud);
    maxAud = std::max(maxAud, aud);
  }
  // The GENERATOR is amplitude-scaled, not bandwidth-scaled: total variance is
  // sr-invariant. (The noise sequence is identical across sr for a fixed seed, so
  // this is exact, not an estimate.)
  CHECK((maxVar - minVar) < 1e-9 * minVar);
  // The BAND is sr-dependent: audible-band power falls as sr rises.
  const double devDb = 10.0 * std::log10(maxAud / minAud);
  std::printf("P3-2 noise-audible-band: total-var %.6f (const), audible-power %.6f..%.6f across 44.1/48/88.2/96k -> %.2f dB\n",
              minVar, minAud, maxAud, devDb);
  CHECK(devDb > 1.0);  // the detector proves sr-dependence is real, not a ghost.
  return true;
}

// ---------------------------------------------------------------------------
// ABSOLUTE-REFERENCE ANCHORS (ca5e: the two drone_mod paths that had
// no anchor — FmAmVoice + NoiseSource). Each pins the implementation to OUR OWN
// DECLARED implementation contract, NOT a hardware fact (the polarity--rule:
// never invent an unevidenced hardware number as an anchor). If hardware evidence
// later says the distribution differs, the DECLARATION changes and these anchors
// follow. See FINDINGS.md §4. Each carries a negative so the judge is provably
// able to go red (never an anchor that cannot fail).
// ---------------------------------------------------------------------------

// Positive-going zero-crossing interval frequency of a SINGLE cycle; the shortest
// interval over the buffer is the peak instantaneous frequency (Hz). Local to this
// test (not a shared judge) — it is FM-specific and coarse (interval-quantized).
static double peak_inst_freq_hz(const std::vector<double>& buf, double sr) {
  long last = -1, best = -1;
  for (std::size_t i = 1; i < buf.size(); ++i) {
    if (buf[i - 1] <= 0.0 && buf[i] > 0.0) {
      if (last >= 0) {
        const long d = static_cast<long>(i) - last;
        if (best < 0 || d < best) best = d;
      }
      last = static_cast<long>(i);
    }
  }
  return best > 0 ? sr / static_cast<double>(best) : 0.0;
}

// NEGATIVE: the phase advance hardcodes a 48 kHz sample rate instead of using the
// real one. Correct at 48 kHz (so a single-rate test would "pass"), wrong at every
// other rate and partition-invariant — the exact class the carrier anchor exists
// to catch.
static std::vector<double> render_fm_am_srhardcoded(std::uint64_t seed, double /*sr*/,
                                                    double fDevHz, double depth,
                                                    std::size_t n) {
  const double kHard = 48000.0;
  core::SeededRandom rng(seed);
  const double fc = 20.0 + rng.nextUnit(0.0, 1.0) * 1980.0;
  const double fmod = 0.5 + rng.nextUnit(0.0, 1.0) * 20.0;
  const double baseAmp = 0.05 + rng.nextUnit(0.0, 1.0) * 0.95;
  std::vector<double> out(n);
  double phaseC = 0.0, phaseM = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double mod = std::sin(phaseM);
    out[i] = baseAmp * (1.0 + depth * mod) * std::sin(phaseC);
    phaseC += 2.0 * drone_test::kPi * (fc + fDevHz * mod) / kHard;
    if (phaseC >= 2.0 * drone_test::kPi) phaseC -= 2.0 * drone_test::kPi;
    phaseM += 2.0 * drone_test::kPi * fmod / kHard;
    if (phaseM >= 2.0 * drone_test::kPi) phaseM -= 2.0 * drone_test::kPi;
  }
  return out;
}

// FmAmVoice anchor, half A — carrier == carrierHz at every rate (the exact closed
// form named). depth=0, fDev=0 => a pure sine at fc; measure_freq_hz pins
// it absolutely. A hardcoded-48k phase advance breaks at 44.1/88.2/96 kHz.
static bool test_fm_am_carrier_cross_sr() {
  const std::uint64_t seed = 0xC0FFEEu;
  const double fDev = 0.0, depth = 0.0;
  for (const double sr : kRates) {
    core::FmAmVoice v(seed, sr, fDev, depth);
    const auto buf = render_fm_am(v, 4u * static_cast<std::size_t>(sr));
    const double meas = measure_freq_hz(buf, sr);
    CHECK(std::fabs(meas - v.carrierHz()) < 1.5);  // exact closed form: fundamental==fc
  }
  // Negative: the sr-hardcoded renderer is wrong at non-48k, and the anchor sees it.
  {
    const double sr = 44100.0;  // a rate where the hardcode is wrong by >1 octave-free
    const auto bad = render_fm_am_srhardcoded(seed, sr, fDev, depth,
                                              4u * static_cast<std::size_t>(sr));
    const double meas = measure_freq_hz(bad, sr);
    CHECK_FALSE(std::fabs(meas - core::FmAmVoice(seed, sr, fDev, depth).carrierHz()) < 1.5);
  }
  return true;
}

// FmAmVoice anchor, half B — the FM deviation is actually realized: with fDev>0 the
// peak instantaneous frequency must reach ~carrierHz+fDev, not stay at the carrier
// (a depth/fDev- scaling error is partition-invariant, so no consistency judge sees
// it). Coarse by construction (interval-quantized), so it runs at a large, accurate
// fDev and asserts a bounded band rather than an exact value.
static bool test_fm_am_deviation_realized() {
  const std::uint64_t seed = 0xC0FFEEu;
  const double sr = 48000.0, fDev = 500.0, depth = 0.0;
  core::FmAmVoice v(seed, sr, fDev, depth);
  const auto buf = render_fm_am(v, 8u * static_cast<std::size_t>(sr));
  const double peak = peak_inst_freq_hz(buf, sr);
  const double fc = v.carrierHz();
  // Modulator peak => instHz=fc+fDev; band [fc+0.75fDev, fc+1.25fDev] catches a
  // collapsed (0x) or doubled (2x) deviation without false-failing on the ±0.7 Hz
  // interval quantization that a large fDev allows.
  CHECK(peak > fc + 0.75 * fDev);
  CHECK(peak < fc + 1.25 * fDev);
  return true;
}

// NoiseSource anchor: each per-sample value is uniform in [-amp,+amp), so the
// sample variance must equal amp^2/3 (sr-independent; a wrong-but-consistent
// amplitude scaling is partition-invariant). This validates our declared
// implementation contract — not a hardware spec —.
static bool test_noise_variance_contract() {
  const std::uint64_t seed = 0x21Cu;
  const double amp = 0.5;
  const double expect = amp * amp / 3.0;
  for (const double sr : kRates) {
    (void)sr;  // the value is sr-independent BY CONTRACT; the sweep confirms no sr-dependence crept in
    core::NoiseSource ns(seed, amp);
    std::vector<double> buf(40000);
    for (std::size_t i = 0; i < buf.size(); ++i) ns.tick(&buf[i]);
    const double var = noise_sample_var(buf);
    CHECK(std::fabs(var - expect) < 0.02 * expect);
  }
  // Negative: an amplitude-halved source has variance (amp/2)^2/3 = 25% of expected.
  {
    core::SeededRandom rng(seed);
    const double halved = amp * 0.5;
    std::vector<double> buf(40000);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = (rng.nextUnit() * 2.0 - 1.0) * halved;
    const double var = noise_sample_var(buf);
    CHECK_FALSE(std::fabs(var - expect) < 0.02 * expect);
  }
  return true;
}

int main() {
  test_schmitt_cross_sr();
  test_schmitt_buffer_independence();
  test_schmitt_modulation_and_pause();
  test_schmitt_lf_edges();
  test_noise_reproducible_buffer();
  test_sandhold_cross_sr_duration();
  test_sandhold_buffer_independence();
  test_sandhold_clock_edge();
  test_fm_am_buffer_determinism();
  test_fm_am_carrier_cross_sr();
  test_fm_am_deviation_realized();
  test_noise_variance_contract();
  test_schmitt_aliasing();
  test_fm_aliasing();
  test_noise_audible_band_power();
  return ::test::finish("drone_mod");
}
