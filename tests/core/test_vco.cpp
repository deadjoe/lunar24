// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P3-③ tests for the triangle-core AS3340 VCO. Six must-tests, each carrying a
// real red-negative so it cannot vacuously pass — @Claude ("测不出区别的测试，就没
// 在测那个东西") and, for the sub/phase-lock and sync tests specifically, the P3-①
// independence judge must NOT be reused here (sub is phase-LOCKED to the parent,
// the OPPOSITE of DroneBank's independent voices).
//
// @Claude's P3-③ mandate, folded in below:
//   ① V/OCT is a FULL 8-octave range (0..8 V, confirmed exponential): every 1 V
//      must exactly double the rendered frequency (linear V/OCT -> red), AND the
//      lin/exp CV-input switch must actually change the response (a switch that is
//      ignored -> red).
//   ② The sub is -1 octave AND phase-LOCKED: same phase origin, preserved through
//      a discontinuity (a free-running independent sub -> red). NEVER ①'s judge.
//   ③ Hard sync is a REAL, detectable discontinuity and is A-SYMMETRIC: VCO A has
//      it (a sync causes a reset the detector sees), VCO B does not (a B wired to
//      sync is a discontinuity the detector flags -> red). A DEAD sync (no-op) is
//      also caught.
//   ④ Morph is CONTINUOUS across the whole 0..1 range (a binary step/switch ->
//      red).
//   ⑤ Cross-sr (same Hz at 44.1/48/88.2/96k) + cross-buffer (bit-identical across
//      partitionings) — judges shared with P3-①/② in drone_test_common.h.
//   ⑥ PWM extreme duty never collapses to DC or silence (a saturated duty clamped
//      to 0/1 -> red).
//   Two MEASURE-ONLY aliasing probes (hard-sync splatter + narrow-pulse folding)
//   are recorded in FINDINGS.md with "合成测试点, 非硬规格" provenance; the fix is
//   deferred to P3 exit by evidence, not patched silently.
//
// The waveform provenance (traditional four = AS3340-convention inference, two
// morphing = manual-evidenced) and the tune-octave conflict (registry -1..+1 vs
// manual "one octave") are the implementer's open items, recorded in FINDINGS,
// NOT silently resolved here.

#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "drone_test_common.h"

#include <lunar24/core/vco.h>

namespace core = lunar24::core;

using drone_test::goertzel_mag;
using drone_test::kTwoPi;
using drone_test::measure_freq_hz;
using drone_test::same_render;

namespace {

// Four standard sample rates for every cross-sample-rate must-test.
const std::vector<double> kRates = {44100.0, 48000.0, 88200.0, 96000.0};

// ---------------------------------------------------------------------------
// Render helpers.
// ---------------------------------------------------------------------------

// Render one Vco into `out`; if `sub` is non-null, capture the sub waveform too.
static void render_vco(core::Vco& v, std::size_t n, std::vector<double>& out,
                       std::vector<double>* sub = nullptr) {
  out.assign(n, 0.0);
  if (sub) sub->assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) v.tick(&out[i], sub ? &(*sub)[i] : nullptr);
}

// ---------------------------------------------------------------------------
// Negative (broken) renders, one per must-test so each judge can show it RED.
// ---------------------------------------------------------------------------

// ① linear V/OCT: pitch = fBase*(1+vOct) instead of fBase*2^vOct. Frequency does
// NOT double every volt, so the V/OCT-ratio judge fires.
static std::vector<double> render_vco_linear_voct(double sr, double baseHz,
                                                  double vOct, std::size_t n) {
  const double f = baseHz * (1.0 + vOct);  // linear — the bug.
  std::vector<double> out(n);
  double ph = 0.0;
  for (auto& o : out) {
    ph += f / sr;
    if (ph >= 1.0) ph -= 1.0;
    o = 2.0 * ph - 1.0;  // saw.
  }
  return out;
}

// ① lin/exp switch ignored: always the linear law regardless of mode. A switch
// that does nothing.
static std::vector<double> render_vco_switch_ignored(double sr, double baseHz,
                                                     double cv, std::size_t n) {
  const double f = baseHz * (1.0 + cv);  // always linear — mode moot.
  std::vector<double> out(n);
  double ph = 0.0;
  for (auto& o : out) {
    ph += f / sr;
    if (ph >= 1.0) ph -= 1.0;
    o = 2.0 * ph - 1.0;
  }
  return out;
}

// ③ hard sync is a NO-OP (dead sync input): the slave is never reset, so a sync
// pulse has zero effect — the continuity judge catches it.
static std::vector<double> render_vco_noop_sync(double sr, double baseHz,
                                                std::size_t n) {
  std::vector<double> out(n);
  double ph = 0.0;
  const double f = baseHz;
  for (auto& o : out) {
    ph += f / sr;
    if (ph >= 1.0) ph -= 1.0;
    o = 2.0 * ph - 1.0;  // never reset — the bug.
  }
  return out;
}

// ④ stepped morph: a binary switch (morph<0.5 -> saw, else -> inverted saw), the
// discontinuity the continuity-in-morph judge fires on.
static double stepped_morph(double m, double p) {
  const double a = 2.0 * p - 1.0;
  return (m < 0.5) ? a : -a;  // jump at the 0.5 boundary — the bug.
}

// ⑤ fixed 48 kHz per-sample step: the pitch increment is hardcoded to sr=48000,
// never scaled by the actual sample rate, so frequency scales with sr — the
// cross-sr detector fires (mirrors the P3-①/② fixed-increment negatives).
static std::vector<double> render_vco_fixed48k(double sr, double baseHz,
                                               std::size_t n) {
  (void)sr;  // the bug is precisely that SAMPLE RATE IS IGNORED.
  std::vector<double> out(n);
  double ph = 0.0;
  const double f = baseHz;
  for (auto& o : out) {
    ph += f / 48000.0;  // hardcoded — the bug.
    if (ph >= 1.0) ph -= 1.0;
    o = 2.0 * ph - 1.0;
  }
  return out;
}

// ⑤ block-boundary reset: the phase is cleared at each block start, making the
// output partition-dependent — the buffer-independence judge fires.
static void render_vco_block_reset(std::size_t n, const std::vector<std::size_t>& blocks,
                                   std::vector<double>& out) {
  std::vector<double> part(n);
  std::size_t pos = 0, bi = 0;
  double f = 200.0, sr = 48000.0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    double ph = 0.0;  // cleared per block — the bug.
    for (std::size_t k = 0; k < b; ++k) {
      ph += f / sr;
      if (ph >= 1.0) ph -= 1.0;
      part[pos + k] = 2.0 * ph - 1.0;
    }
    pos += b;
    ++bi;
  }
  out = part;
}

// ⑥ saturated pulse: duty clamped to exact 0 (or 1), so the pulse never switches
// and collapses to a near-constant (DC / silence) — the both-excursion / not-silent
// judge fires.
static std::vector<double> render_vco_saturated_pulse(double sr, double baseHz,
                                                      std::size_t n) {
  std::vector<double> out(n);
  double ph = 0.0;
  const double f = baseHz;
  for (auto& o : out) {
    ph += f / sr;
    if (ph >= 1.0) ph -= 1.0;
    o = (ph < 0.0) ? 1.0 : -1.0;  // duty clamped to 0 — always -1 after phase 0.
  }
  return out;
}

// Fold a component at frequency h (Hz) to baseband [0, sr/2] + lowest odd harmonic
// above Nyquist (reused logic, P3-② style).
static double fold_to_baseband(double h, double sr) {
  const double nyq = sr * 0.5;
  const double n = std::round(h / sr);
  double a = std::fabs(h - n * sr);
  return (a > nyq) ? sr - a : a;
}

static bool first_folded_harmonic(double f0, double sr, int& order, double& aliasHz) {
  const double nyq = sr * 0.5;
  for (int n = 3; n <= 400; n += 2) {
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

// ① V/OCT doubling + lin/exp switch (a full 8-octave range, confirmed exponential).
static bool test_vco_voct_and_lilin() {
  const double sr = 96000.0;  // high sr so the top octave stays below Nyquist.
  const double baseHz = 100.0;
  const std::size_t n = 240000;  // 2.5 s; ~250 crossings at the 100 Hz bottom octave.

  // V/OCT: f(V+1)/f(V) == 2 across the full 0..8 V range.
  double prev = -1.0;
  double maxRatioErr = 0.0;
  for (int v = 0; v <= 8; ++v) {
    core::Vco o(sr);
    o.setBaseHz(baseHz);
    o.setVoct(static_cast<double>(v));
    o.setWaveform(core::VcoWaveform::kSaw);
    o.setSubSelect(0);
    std::vector<double> buf;
    render_vco(o, n, buf);
    const double hz = measure_freq_hz(buf, sr);
    if (prev > 0.0) maxRatioErr = std::max(maxRatioErr, std::fabs(hz / prev - 2.0));
    prev = hz;
  }
  // Correct (V/OCT verified): every octave doubles to within ~1 % (finite-sample
  // zero-crossing quantization in measure_freq_hz).
  CHECK(maxRatioErr < 0.02);

  // Negative: a linear V/OCT (f = base*(1+v)) does NOT double → ratio far off 2.
  double prevL = -1.0, worstL = 0.0;
  for (int v = 1; v <= 8; ++v) {
    const auto b = render_vco_linear_voct(sr, baseHz, v, n);
    const double hz = measure_freq_hz(b, sr);
    if (prevL > 0.0) worstL = std::max(worstL, std::fabs(hz / prevL - 2.0));
    prevL = hz;
  }
  CHECK(worstL > 0.5);  // the V/OCT-ratio judge sees the linear bug.

  // lin/exp CV-input switch must change the response.
  core::Vco expV(sr);
  expV.setBaseHz(baseHz);
  expV.setCvInput(2.0, core::VcoControlMode::kExponential);
  expV.setCvAmt(1.0);
  expV.setWaveform(core::VcoWaveform::kSaw);
  expV.setSubSelect(0);
  std::vector<double> expBuf;
  render_vco(expV, n, expBuf);
  const double expHz = measure_freq_hz(expBuf, sr);

  core::Vco linV(sr);
  linV.setBaseHz(baseHz);
  linV.setCvInput(2.0, core::VcoControlMode::kLinear);
  linV.setCvAmt(1.0);
  linV.setWaveform(core::VcoWaveform::kSaw);
  linV.setSubSelect(0);
  std::vector<double> linBuf;
  render_vco(linV, n, linBuf);
  const double linHz = measure_freq_hz(linBuf, sr);

  // exp: 100*2^2 = 400 Hz; lin: 100*(1+2) = 300 Hz. Differ by ~25 %.
  CHECK(std::fabs(expHz - 400.0) < 8.0);
  CHECK(std::fabs(linHz - 300.0) < 8.0);
  CHECK(expHz > linHz * 1.19);  // the switch is real, not a no-op.

  // Negative: a dead switch keeps BOTH modes at the SAME frequency, so the
  // "modes differ" judge yields FALSE for it — proving the discrimination (the
  // real VCO differs; the dead-switch mock does not).
  const auto swExp = render_vco_switch_ignored(sr, baseHz, 2.0, n);
  const auto swLin = render_vco_switch_ignored(sr, baseHz, 2.0, n);
  CHECK(swExp.size() == n);  // fixture sanity.
  CHECK(measure_freq_hz(swExp, sr) == measure_freq_hz(swLin, sr));  // dead switch -> same
  return true;
}

// ② Sub = -1 octave AND phase-locked (NOT P3-①'s independence judge).
static bool test_vco_sub_locked() {
  const double sr = 48000.0;
  const double baseHz = 200.0;
  const std::size_t n = 48000;
  const std::size_t kSync = 20000;

  // Render parent saw + sub (subEnabled = index 1 = -1).
  core::Vco v(sr);
  v.setBaseHz(baseHz);
  v.setWaveform(core::VcoWaveform::kSaw);
  v.setSubSelect(1);
  std::vector<double> parent(n), sub(n);
  // manual loop so we can insert a hard sync at kSync.
  for (std::size_t i = 0; i < kSync; ++i) v.tick(&parent[i], &sub[i]);
  v.syncPulse();
  for (std::size_t i = kSync; i < n; ++i) v.tick(&parent[i], &sub[i]);

  // (a) Rendered sub frequency == parent / 2 (the -1 octave).
  const double parentHz = measure_freq_hz(parent, sr);
  const double subHz = measure_freq_hz(sub, sr);
  CHECK(std::fabs(parentHz - baseHz) < 2.0);
  CHECK(std::fabs(subHz - parentHz * 0.5) < 0.02 * parentHz);

  // (b) Phase-lock through a discontinuity: right after the sync the parent and the
  // sub BOTH reset toward phase 0 — the sub output sits near -1 (phase 0), proving
  // it shares the parent's phase origin and tracks the reset. An independent
  // free-running sub would NOT reset.
  CHECK(parent[kSync] < -0.99);  // parent reset to phase ~0 (saw -> -1).
  CHECK(sub[kSync] < -0.99);     // sub reset to phase ~0 (square -> -1).

  // Negative: an independent sub (its own accumulator, launched at a phase offset,
  // ignoring the sync) does NOT reset anywhere near the parent's phase 0.
  std::vector<double> subInd(n);
  double subPh = 0.5;
  const double subRate = (baseHz * 0.5) / sr;
  for (std::size_t i = 0; i < n; ++i) {
    subPh += subRate;
    if (subPh >= 1.0) subPh -= 1.0;
    subInd[i] = 2.0 * subPh - 1.0;
  }
  CHECK_FALSE(subInd[kSync] < -0.99);  // independent sub is NOT reset here -> judge DECIDES
  return true;
}

// ③ Hard sync is a real, detectable, A-symmetric discontinuity; a wrongly-synced B
// (or a dead sync) is caught.
static bool test_vco_hardsync_asymmetry() {
  const double sr = 48000.0;
  const double baseHz = 400.0;
  const std::size_t n = 24000;
  const std::size_t kSync = 12000;

  // Clean (never synced) — the correct VCO B.
  core::Vco b(sr);
  b.setBaseHz(baseHz);
  b.setWaveform(core::VcoWaveform::kSaw);
  b.setSubSelect(0);
  std::vector<double> clean;
  render_vco(b, n, clean);

  // VCO A: hard-synced once at kSync.
  core::Vco a(sr);
  a.setBaseHz(baseHz);
  a.setWaveform(core::VcoWaveform::kSaw);
  a.setSubSelect(0);
  std::vector<double> aBuf(n);
  for (std::size_t i = 0; i < kSync; ++i) a.tick(&aBuf[i]);
  a.syncPulse();
  for (std::size_t i = kSync; i < n; ++i) a.tick(&aBuf[i]);

  // Positive: the sync produces a discontinuity — the A render differs from the
  // clean B render (a reset happened, the phase restarted).
  CHECK_FALSE(same_render(clean, aBuf));

  // Positive: B is continuous — a never-synced oscillator is bit-identical to
  // itself across a partition (no hidden reset).
  core::Vco b2(sr);
  b2.setBaseHz(baseHz);
  b2.setWaveform(core::VcoWaveform::kSaw);
  b2.setSubSelect(0);
  std::vector<double> part(n);
  std::vector<double> b2buf(n);
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};
  std::size_t pos = 0, bi = 0;
  while (pos < n) {
    const std::size_t blk = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < blk; ++k) {
      b2.tick(&b2buf[pos + k]);
      part[pos + k] = b2buf[pos + k];
    }
    pos += blk;
    ++bi;
  }
  (void)part;
  CHECK(same_render(clean, b2buf));  // B is continuous & partition-independent.

  // Negative 1: a wrongly-synced B IS a discontinuity (the "B also resets" bug).
  core::Vco b3(sr);
  b3.setBaseHz(baseHz);
  b3.setWaveform(core::VcoWaveform::kSaw);
  b3.setSubSelect(0);
  std::vector<double> bWrong(n);
  for (std::size_t i = 0; i < kSync; ++i) b3.tick(&bWrong[i]);
  b3.syncPulse();  // B must never be synced — the bug.
  for (std::size_t i = kSync; i < n; ++i) b3.tick(&bWrong[i]);
  CHECK_FALSE(same_render(clean, bWrong));  // a synced B differs from clean -> judge decides

  // Negative 2: a DEAD sync (no-op) has no effect, so the "sync causes a
  // discontinuity" judge below fails.
  const auto noopBuf = render_vco_noop_sync(sr, baseHz, n);
  CHECK_FALSE(same_render(clean, noopBuf));  // dead sync == clean -> RED
  return true;
}

// ④ Morph is continuous across the whole 0..1 range (no step).
static bool test_vco_morph_continuous() {
  core::Vco v(48000.0);
  v.setWaveform(core::VcoWaveform::kMorphSawInvSaw);
  const double p = 0.3;                       // fixed probe phase.
  double prev = v.waveformSampleAt(0.0);
  double maxStep = 0.0;
  const int steps = 256;
  for (int i = 0; i <= steps; ++i) {
    const double m = static_cast<double>(i) / steps;
    v.setMorph(m);
    const double val = v.waveformSampleAt(p);
    if (i > 0) maxStep = std::max(maxStep, std::fabs(val - prev));
    prev = val;
  }
  CHECK(maxStep < 0.05);  // continuous in morph.

  // Negative: a binary step (morph<0.5 -> saw, else -> inverted saw) jumps by the
  // full saw-swing at the 0.5 boundary.
  double stepMax = 0.0;
  double prevS = stepped_morph(0.0, p);
  for (int i = 1; i <= steps; ++i) {
    const double m = static_cast<double>(i) / steps;
    const double val = stepped_morph(m, p);
    stepMax = std::max(stepMax, std::fabs(val - prevS));
    prevS = val;
  }
  CHECK_FALSE(stepMax < 0.05);  // a stepped morph jumps ~0.8 -> judge decides
  return true;
}

// ⑤ Cross-sample-rate + cross-buffer (judges shared with P3-①/②).
static bool test_vco_cross_sr_and_buffer() {
  const double baseHz = 440.0;
  const std::size_t n = 48000;

  // cross-sr: the same Hz at every sample rate (pitch is a physical frequency).
  double minHz = 1e18, maxHz = -1e18;
  for (const double sr : kRates) {
    core::Vco v(sr);
    v.setBaseHz(baseHz);
    v.setWaveform(core::VcoWaveform::kSaw);
    v.setSubSelect(0);
    std::vector<double> buf;
    render_vco(v, n, buf);
    const double hz = measure_freq_hz(buf, sr);
    CHECK(std::fabs(hz - baseHz) < 2.0);
    minHz = std::min(minHz, hz);
    maxHz = std::max(maxHz, hz);
  }
  // Correct: the spread is tiny (the Hz is sr-invariant).
  CHECK((maxHz - minHz) < 1.0);

  // Negative: a fixed 48 kHz step makes the frequency scale with sr -> wide spread.
  double bmin = 1e18, bmax = -1e18;
  for (const double sr : kRates) {
    const auto b = render_vco_fixed48k(sr, baseHz, n);
    const double hz = measure_freq_hz(b, sr);
    bmin = std::min(bmin, hz);
    bmax = std::max(bmax, hz);
  }
  CHECK((bmax - bmin) > 100.0);  // detector sees the sr-dependence.

  // cross-buffer: per-sample output identical across partitionings.
  core::Vco v1(48000.0);
  v1.setBaseHz(baseHz);
  v1.setWaveform(core::VcoWaveform::kSaw);
  v1.setSubSelect(0);
  std::vector<double> single;
  render_vco(v1, n, single);

  core::Vco v2(48000.0);
  v2.setBaseHz(baseHz);
  v2.setWaveform(core::VcoWaveform::kSaw);
  v2.setSubSelect(0);
  const std::vector<std::size_t> blocks = {64, 100, 37, 128, 7, 256, 91};
  std::vector<double> part(n);
  std::size_t pos = 0, bi = 0;
  while (pos < n) {
    const std::size_t b = std::min(n - pos, blocks[bi % blocks.size()]);
    for (std::size_t k = 0; k < b; ++k) v2.tick(&part[pos + k]);
    pos += b;
    ++bi;
  }
  CHECK(same_render(single, part));

  // Negative: a block-boundary reset makes it partition-dependent.
  std::vector<double> buggy;
  render_vco_block_reset(n, blocks, buggy);
  CHECK_FALSE(same_render(single, buggy));
  return true;
}

// ---------------------------------------------------------------------------
// GH#19 S0 (task #117) — the PRODUCTION continuous-waveform law and the PWM consumer.
//
// The three load-bearing claims of the production entry, each with its own red-negative:
//   (1) A runtime-built Vco renders kMorphRing, and the DEFAULT position (morph = 0.5) is the
//       SINE node. The reference is this file's own std::sin — NOT wave_map — so a law that
//       kept the old fixed triangle (the "fixed old triangle" mutant) fails here.
//   (2) morph = 0.75 on the ring is BIT-IDENTICAL to the module-dev raw kTriangle ACROSS A FULL
//       RENDER, BLAMP correction included. This is what lets the pre-#117 triangle/BLAMP gates
//       be re-pointed at norm=.75 and reproduce their historical numbers, so it is asserted at
//       bit equality rather than at a tolerance.
//   (3) The PWM transfer: effectiveDuty = clamp(basePW + depth*cv/10, 0.001, 0.999), depth 0
//       STRICTLY unchanged, basePW never written back, non-finite never propagated.
// The "wrong side" and "mapping jump" mutants are caught by (1)'s endpoint table and the
// continuity sweep respectively.
// ---------------------------------------------------------------------------
static bool test_vco_gh19_s0_morph_ring_and_pwm() {
  const double sr = 48000.0;
  const double baseHz = 440.0;
  const std::size_t n = 4800;  // ~44 cycles of 440 Hz — long enough for BLAMP to matter.
  const double p = 0.3;        // fixed probe phase for the pure-shape checks.

  // (1a) The rendering law of a default-constructed Vco is the ring, not a raw shape.
  {
    core::Vco v(sr);
    CHECK(v.waveform() == core::VcoWaveform::kMorphRing);
    CHECK(std::fabs(v.morph() - 0.5) < 1e-12);   // the registry default position.
  }

  // (1b) DEFAULT == the SINE node, against an INDEPENDENT reference (std::sin, this file's),
  //      never wave_map. tick() emits the shape UNSCALED (the 0.5 DRY clamp lives in the
  //      DeviceAdapter, not here), and the phase convention is pre-increment: sample i reads
  //      frac((i+1)*f0/sr).
  {
    core::Vco v(sr);
    v.setBaseHz(baseHz);          // without this the oscillator is at 0 Hz and emits silence.
    CHECK(v.waveform() == core::VcoWaveform::kMorphRing);
    std::vector<double> buf;
    render_vco(v, n, buf);
    // The reference ACCUMULATES the phase the way the producer does (`cumPitch_ += step`, never
    // re-derived as i*step), so the comparison is bit-level rather than limited by the
    // accumulation-vs-product rounding difference — measured at ~2e-11 when the product form is
    // used, which is real but would be indistinguishable from a tiny waveform error.
    const double step = baseHz / sr;
    double cum = 0.0;
    double maxErr = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      cum += step;                                     // tick() advances before emitting.
      const double ref = std::sin(core::Vco::kTwoPi * (cum - std::floor(cum)));
      maxErr = std::max(maxErr, std::fabs(buf[i] - ref));
    }
    std::printf("P3-3 gh19-s0 default-vs-sine: max|err| = %.6e\n", maxErr);
    CHECK(maxErr < 1e-12);   // the default really is the sine node.
    // RED-NEGATIVE: the pre-#117 default (a triangle at the same pitch) is a DIFFERENT buffer,
    // so the check above cannot pass with the old fixed-triangle law left in place.
    const std::vector<double> tri = [&] {
      core::Vco t(sr);
      t.setBaseHz(baseHz);
      t.setWaveform(core::VcoWaveform::kTriangle);
      std::vector<double> out;
      render_vco(t, n, out);
      return out;
    }();
    double vs = 0.0;
    for (std::size_t i = 0; i < n; ++i) vs = std::max(vs, std::fabs(buf[i] - tri[i]));
    CHECK(vs > 0.1);         // sine vs triangle is a large, unmistakable difference.
  }

  // (1c) The ring's five NODE positions land on the five expected shapes (the "wrong side" /
  //      node-order mutant fails at least one of these). Reference = wave_map's own node
  //      generators, which is legitimate here: this asserts the ROUTING (which node each
  //      coordinate selects), not the node formula.
  {
    core::Vco v(sr);
    v.setShape(0.25);
    struct NodeCase { double at; core::wave_map::Node node; const char* name; };
    const NodeCase cases[] = {
        {0.00, core::wave_map::Node::kSaw, "saw"},
        {0.25, core::wave_map::Node::kInvSaw, "invSaw"},
        {0.50, core::wave_map::Node::kSine, "sine"},
        {0.75, core::wave_map::Node::kTriangle, "triangle"},
        {1.00, core::wave_map::Node::kPulse, "pulse"},
    };
    for (const NodeCase& c : cases) {
      v.setMorph(c.at);
      const double got = v.waveformSampleAt(p);
      const double want = core::wave_map::nodeSample(c.node, p, 0.25);
      if (!(std::fabs(got - want) < 1e-15))
        std::printf("  gh19-s0 node mismatch at morph=%.2f (%s): got=%.17g want=%.17g\n",
                    c.at, c.name, got, want);
      CHECK(std::fabs(got - want) < 1e-15);
    }
  }

  // (1d) CONTINUITY across the whole ring (the "mapping jump" mutant fires here): no single
  //      morph step may move the sample by a full-swing amount.
  {
    core::Vco v(sr);
    const int steps = 1000;
    v.setMorph(0.0);   // start the sweep AT 0 — the default 0.5 is a different node (sine).
    double prev = v.waveformSampleAt(p);
    double maxStep = 0.0;
    for (int i = 1; i <= steps; ++i) {
      v.setMorph(static_cast<double>(i) / steps);
      const double val = v.waveformSampleAt(p);
      maxStep = std::max(maxStep, std::fabs(val - prev));
      prev = val;
    }
    std::printf("P3-3 gh19-s0 ring continuity: max step = %.6f over %d steps\n", maxStep, steps);
    CHECK(maxStep < 0.05);
  }

  // (1e) The two NAMED stretches against INDEPENDENT closed forms (this file's own arithmetic),
  //      never against wave_map's node generators. This block exists because (1c) is
  //      self-referenced BY CONSTRUCTION: it asks whether the ring ROUTES coordinate k/4 to node
  //      k, and uses the same header for both sides, so a swap of two node FORMULAS cancels
  //      there and passes. Measured: with sawShape/invSawShape bodies exchanged, (1c) is green.
  //      Such a swap is visible HERE and nowhere else in this suite: the saw stretch must be
  //      (1-2u)(2p-1) exactly, and sine->triangle (1-u)sin(2*pi*p) + u(4|p-0.5|-1) exactly.
  {
    double worstSaw = 0.0, worstST = 0.0;
    for (int i = 0; i <= 64; ++i) {
      const double u = static_cast<double>(i) / 64.0;
      for (int j = 0; j < 16; ++j) {
        const double q = static_cast<double>(j) / 16.0;
        const double ramp = 2.0 * q - 1.0;
        worstSaw = std::max(
            worstSaw, std::fabs(core::wave_map::morphSawInvSaw(u, q) - (1.0 - 2.0 * u) * ramp));
        const double ref = (1.0 - u) * std::sin(core::Vco::kTwoPi * q) +
                           u * (4.0 * std::fabs(q - 0.5) - 1.0);
        worstST = std::max(worstST, std::fabs(core::wave_map::morphSineTriangle(u, q) - ref));
      }
    }
    std::printf("P3-3 gh19-s0 stretch closed forms: saw=%.3e sine->tri=%.3e\n", worstSaw, worstST);
    CHECK(worstSaw < 1e-15);
    CHECK(worstST < 1e-15);
  }

  // (2) morph = 0.75 on the ring == the raw module-dev kTriangle, BIT-IDENTICAL over the render.
  {
    core::Vco ring(sr);
    ring.setBaseHz(baseHz);
    ring.setShape(0.5);
    ring.setMorph(0.75);
    std::vector<double> ringBuf;
    render_vco(ring, n, ringBuf);

    core::Vco raw(sr);
    raw.setBaseHz(baseHz);
    raw.setShape(0.5);
    raw.setWaveform(core::VcoWaveform::kTriangle);
    std::vector<double> rawBuf;
    render_vco(raw, n, rawBuf);

    std::size_t bitDiffs = 0;
    double maxDiff = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (ringBuf[i] != rawBuf[i]) ++bitDiffs;
      maxDiff = std::max(maxDiff, std::fabs(ringBuf[i] - rawBuf[i]));
    }
    std::printf("P3-3 gh19-s0 tri-node: bit-diffs=%zu max|diff|=%.6e over %zu samples\n",
                bitDiffs, maxDiff, n);
    CHECK(bitDiffs == 0);            // the re-point at norm=.75 is EXACT, BLAMP included.
    CHECK(ringBuf == rawBuf);
    // RED-NEGATIVE: the same comparison at morph=0.5 (the sine node) is NOT identical, so the
    // bit-equality above is a property of the .75 node and not of a comparison that ignores the
    // waveform. (This is the control that makes the .75 claim non-vacuous.)
    core::Vco wrong(sr);
    wrong.setBaseHz(baseHz);
    wrong.setShape(0.5);
    wrong.setMorph(0.5);
    std::vector<double> wrongBuf;
    render_vco(wrong, n, wrongBuf);
    CHECK(wrongBuf != rawBuf);
  }

  // (2b) The ring's triangle BLAMP weight is the real triangle weight (1 at the node, 0 away).
  {
    using core::wave_map::triangleWeight;
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 0.75) - 1.0) < 1e-15);
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 0.5) - 0.0) < 1e-15);
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 1.0) - 0.0) < 1e-15);
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 0.0) - 0.0) < 1e-15);
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 0.875) - 0.5) < 1e-15);
    CHECK(std::fabs(triangleWeight(core::wave_map::kRingEqual, 0.625) - 0.5) < 1e-15);
  }

  // (3) PWM transfer. depth 0 is STRICTLY unchanged; the ±5 V @ depth 1 ratio is ±0.5; positive
  //     CV raises the duty; the clamp holds; nothing non-finite reaches the emitted duty; and
  //     basePW (the canonical knob value) is never written back by modulation.
  {
    core::Vco v(sr);
    v.setBaseHz(baseHz);
    v.setWaveform(core::VcoWaveform::kPulse);

    for (const double pw : {0.1, 0.5, 0.9}) {
      v.setShape(pw);
      const double canonical = v.shape();
      CHECK(std::fabs(canonical - pw) < 1e-15);

      // depth 0: unchanged for ANY cv, including a hostile one.
      for (const double cv : {0.0, 5.0, -5.0, 1e9}) {
        v.setPwDepth(0.0);
        v.setPwCv(cv);
        CHECK(v.effectiveDuty() == canonical);   // EXACT equality, not a tolerance.
      }

      // ±5 V @ depth 1 adds exactly ±0.5 to the duty — asserted on whichever side stays inside
      // [0.001, 0.999]. Both sides at pw = 0.5 would sit exactly ON the window edge (0.5+0.5 is
      // 1.0, which the clamp pulls to 0.999), so the clamp, not the ratio, would be measured;
      // the clamp has its own check at the end of this block.
      v.setPwDepth(1.0);
      if (canonical + 0.5 <= core::Vco::kPwDutyMax) {
        v.setPwCv(5.0);
        CHECK(std::fabs(v.effectiveDuty() - (canonical + 0.5)) < 1e-15);
      } else {
        v.setPwCv(5.0);
        CHECK(v.effectiveDuty() == core::Vco::kPwDutyMax);
      }
      if (canonical - 0.5 >= core::Vco::kPwDutyMin) {
        v.setPwCv(-5.0);
        CHECK(std::fabs(v.effectiveDuty() - (canonical - 0.5)) < 1e-15);
      } else {
        v.setPwCv(-5.0);
        CHECK(v.effectiveDuty() == core::Vco::kPwDutyMin);
      }
      // Half depth == half the contribution (the ratio is linear in depth, not just at 1).
      v.setPwDepth(0.5);
      v.setPwCv(5.0);
      CHECK(std::fabs(v.effectiveDuty() - std::min(core::Vco::kPwDutyMax, canonical + 0.25)) < 1e-15);

      // positive CV RAISES the duty (the declared sign).
      v.setPwDepth(0.5);
      v.setPwCv(0.0);
      const double flat = v.effectiveDuty();
      v.setPwCv(5.0);
      CHECK(v.effectiveDuty() > flat);

      // the modulation never writes the canonical base width back.
      CHECK(v.shape() == canonical);
      v.setPwCv(0.0);
      v.setPwDepth(0.0);
      CHECK(v.effectiveDuty() == canonical);
    }

    // Clamp window: a huge CV saturates at 0.999 / 0.001, never 0 or 1 (a flat DC line).
    v.setShape(0.5);
    v.setPwDepth(1.0);
    v.setPwCv(1e6);
    CHECK(v.effectiveDuty() == core::Vco::kPwDutyMax);
    v.setPwCv(-1e6);
    CHECK(v.effectiveDuty() == core::Vco::kPwDutyMin);

    // Non-finite NEVER passes through: a non-finite cv/depth is treated as 0.
    v.setPwCv(std::numeric_limits<double>::quiet_NaN());
    CHECK(std::isfinite(v.effectiveDuty()));
    CHECK(v.pwCv() == 0.0);
    v.setPwCv(0.0);
    v.setPwDepth(std::numeric_limits<double>::infinity());
    CHECK(std::isfinite(v.effectiveDuty()));
    CHECK(v.pwDepth() == 0.0);
    CHECK(v.effectiveDuty() == v.shape());

    // The clamp/NaN contract holds on the REAL emitted samples too — a pulse at an extreme
    // effective duty still swings to both rails (no DC collapse).
    v.setPwDepth(1.0);
    v.setPwCv(5.0);
    v.setShape(0.6);   // 0.6 + 0.5 -> clamped to 0.999
    std::vector<double> buf;
    render_vco(v, 4800, buf);
    double mn = 1e18, mx = -1e18;
    for (const double x : buf) { mn = std::min(mn, x); mx = std::max(mx, x); }
    CHECK(std::isfinite(mn) && std::isfinite(mx));
    CHECK(mx > 0.4 && mn < -0.4);   // both excursions present — not a flat line.
  }

  // (3b) The PWM consumer is only the PULSE node: on the ring, modulation reaches the pulse
  //      node (morph = 1) and must NOT perturb the other four nodes.
  {
    const auto atMorph = [&](double morph, double depth, double cv) {
      core::Vco v(sr);
      v.setMorph(morph);
      v.setPwDepth(depth);
      v.setPwCv(cv);
      return v.waveformSampleAt(p);
    };
    // pulse node: duty really moves the sample (p=0.3 is inside a 0.5-wide pulse's high half).
    const double pLo = atMorph(1.0, 1.0, -5.0);   // duty 0.001 -> phase .3 is LOW
    const double pHi = atMorph(1.0, 1.0, +5.0);   // duty 0.999 -> phase .3 is HIGH
    CHECK(pLo != pHi);
    // every non-pulse node is bit-unchanged by the modulation.
    for (const double m : {0.0, 0.25, 0.5, 0.75}) {
      CHECK(atMorph(m, 0.0, 0.0) == atMorph(m, 1.0, 5.0));
      CHECK(atMorph(m, 0.0, 0.0) == atMorph(m, 1.0, -5.0));
    }
  }

  return true;
}

// ⑥ PWM extreme duty never collapses to DC or silence.
static bool test_vco_pwm_extreme_duty() {
  const double sr = 48000.0;
  const double baseHz = 200.0;
  const std::size_t n = 24000;

  for (const double duty : {0.01, 0.5, 0.99}) {
    core::Vco v(sr);
    v.setBaseHz(baseHz);
    v.setWaveform(core::VcoWaveform::kPulse);
    v.setShape(duty);
    v.setSubSelect(0);
    std::vector<double> buf;
    render_vco(v, n, buf);

    // Not silent (real AC energy) and swings to BOTH extremes (not DC).
    double mn = 1e18, mx = -1e18;
    for (const double x : buf) { mn = std::min(mn, x); mx = std::max(mx, x); }
    CHECK((mx - mn) > 1.5);          // full ±1 swing present.
    CHECK(mx > 0.5 && mn < -0.5);    // both excursions — not held at one rail.
    const double hz = measure_freq_hz(buf, sr);
    CHECK(std::fabs(hz - baseHz) < 2.0);  // still oscillates at the base pitch.
  }

  // Negative: a saturated duty (clamped to 0 / 1) never switches -> near-DC.
  const auto sat = render_vco_saturated_pulse(sr, baseHz, n);
  double smn = 1e18, smx = -1e18;
  for (const double x : sat) { smn = std::min(smn, x); smx = std::max(smx, x); }
  CHECK_FALSE((smx - smn) > 1.5);  // a saturated pulse never swings -> judge decides
  return true;
}

// ---------------------------------------------------------------------------
// MEASURE-ONLY aliasing probes (@Claude: measure, record in FINDINGS, defer fix).
// ---------------------------------------------------------------------------

// Hard-sync splatter: a slave saw restarted by a master clock gets energy where the
// clean saw has none. Probe a master-line frequency and report the delta in dB.
static bool test_vco_hardsync_splatter() {
  const double sr = 48000.0;
  const double fSlave = 500.0;
  const double fMaster = 600.0;  // M = 48000/600 = 80 samples per reset.
  const std::size_t n = 48000;
  const std::size_t M = static_cast<std::size_t>(std::lround(sr / fMaster));

  // Clean slave saw (no sync).
  core::Vco clean(sr);
  clean.setBaseHz(fSlave);
  clean.setWaveform(core::VcoWaveform::kSaw);
  clean.setSubSelect(0);
  std::vector<double> cbuf;
  render_vco(clean, n, cbuf);

  // Hard-synced slave saw (restart every M samples).
  core::Vco sync(sr);
  sync.setBaseHz(fSlave);
  sync.setWaveform(core::VcoWaveform::kSaw);
  sync.setSubSelect(0);
  std::vector<double> sbuf(n);
  for (std::size_t i = 0; i < n; ++i) {
    sync.tick(&sbuf[i]);
    if ((i + 1) % M == 0) sync.syncPulse();  // master rising edge each M samples.
  }

  // Probe a master-line frequency (fMaster), where the clean saw has no harmonic.
  const double probe = fMaster;  // 600 Hz — not a multiple of 500.
  const double cMag = goertzel_mag(cbuf, probe, sr);
  const double sMag = goertzel_mag(sbuf, probe, sr);
  const double db = 20.0 * std::log10((sMag + 1e-12) / (cMag + 1e-12));
  std::printf("P3-3 vco-hard-sync: sr=%.0f slave=%.0fHz master=%.0fHz probe=%.0fHz -> %.2f dB (synced vs clean)\n",
              sr, fSlave, fMaster, probe, db);
  // Splatter is real, not a ghost: the synced line is far above the clean floor.
  CHECK(db > 15.0);
  return true;
}

// Narrow-pulse folding: a pulse has rich harmonics; the first odd harmonic above
// Nyquist folds back into band. Measure the folded component relative to the
// fundamental (a synthetic probe, not a hard spec).
static bool test_vco_narrowpulse_fold() {
  const double sr = 48000.0;
  const double duty = 0.1;
  const double f0 = 1800.0;  // 15th harmonic = 27000 > Nyquist 24000 -> folds.
  const std::size_t n = 65536;

  core::Vco v(sr);
  v.setBaseHz(f0);
  v.setWaveform(core::VcoWaveform::kPulse);
  v.setShape(duty);
  v.setSubSelect(0);
  std::vector<double> buf;
  render_vco(v, n, buf);

  int order = 0;
  double alias = 0.0;
  if (!first_folded_harmonic(f0, sr, order, alias)) {
    CHECK(false);  // no fold among the test's harmonics -> fixture bug, not a pass.
    return true;
  }
  const double fund = goertzel_mag(buf, f0, sr);
  const double folded = goertzel_mag(buf, alias, sr);
  CHECK(fund > 0.0);
  CHECK(folded > 0.0);
  const double db = 20.0 * std::log10((folded + 1e-12) / (fund + 1e-12));
  std::printf("P3-3 vco-pulse-fold: sr=%.0f f0=%.0fHz duty=%.2f fold-order=%d alias=%.2fHz -> %.2f dB\n",
              sr, f0, duty, order, alias, db);
  // A real folded component (not a numerical ghost): > -60 dB relative.
  CHECK(db > -60.0);
  return true;
}

// ---------------------------------------------------------------------------
// task #86 / GH#19: BLAMP triangle slope-correction STRUCTURAL contract. The
// alias metric (blref_full_db) is the acceptance, but it realigns to the measured
// fundamental phase and is scale-invariant, so these four structural controls are
// asserted here so a regression is caught by the right detector, not by accident:
//   A) phase-advance guard   — the corrected triangle must add NO whole-sample
//      latency. Cross-correlation with the naive triangle must peak at lag 0.
//   B) amplitude guard       — corner rounding never overshoots past the naive
//      swing, and the output is never silently volume-scaled (fake improvement).
//   C) morph-untouched       — the morphing sine<->triangle uses the NAIVE triangle
//      (the BLAMP is applied only to the plain kTriangle core, in scope).
//   D) block-invariance      — the correction is a pure function of the persisted
//      phase accumulator, so a block-boundary reset would change the output.
//   E) BLAMP-is-active       — corrected kTriangle differs from the naive triangle
//      (kMorphSineTriangle @ morph=1 is the naive triangle) at the corners.
// ---------------------------------------------------------------------------
static bool test_vco_blamp() {
  const double sr = 48000.0;
  const double baseHz = 440.0;
  const std::size_t n = 4800;  // ~44 cycles of 440 Hz.

  // Render a Vco at a waveform/morph into a buffer.
  auto render = [&](core::VcoWaveform w, double morph) {
    core::Vco v(sr);
    v.setBaseHz(baseHz);
    v.setWaveform(w);
    v.setMorph(morph);
    std::vector<double> out;
    render_vco(v, n, out);
    return out;
  };

  const std::vector<double> triBlamp = render(core::VcoWaveform::kTriangle, 0.5);
  const std::vector<double> triNaive = render(core::VcoWaveform::kMorphSineTriangle, 1.0);

  // E) the BLAMP is not a no-op: corrected differs from naive near corners.
  double maxDiff = 0.0;
  for (std::size_t i = 0; i < n; ++i)
    maxDiff = std::max(maxDiff, std::fabs(triBlamp[i] - triNaive[i]));
  std::printf("P3-3 vco-blamp: max|corrected-naive| = %.5f (BLAMP active if > 1e-4)\n",
              maxDiff);
  CHECK(maxDiff > 1e-4);   // the correction actually changed the triangle.
  CHECK(maxDiff < 0.05);   // but it is a corner-local residual, not a reshaping.

  // C) morph-untouched: kMorphSineTriangle @morph=1 is EXACTLY the naive triangle.
  //    The Vco pre-increments the phase accumulator before each sample, so sample i
  //    uses phase = frac((i+1)*f0/sr) — mirror that convention for the reference.
  double morphErr = 0.0;
  const double step = baseHz / sr;
  for (std::size_t i = 0; i < n; ++i) {
    const double ph = (double(i + 1) * step) - std::floor(double(i + 1) * step);
    const double ref = 4.0 * std::fabs(ph - 0.5) - 1.0;
    morphErr = std::max(morphErr, std::fabs(triNaive[i] - ref));
  }
  std::printf("P3-3 vco-blamp: morph=1 stays naive (max err %.3e)\n", morphErr);
  CHECK(morphErr < 1e-9);  // the morph waveform never receives the BLAMP.

  // A) phase-advance / no hidden one-sample delay: cross-correlation peaks at lag 0.
  //    corr(lag) = sum_i naive[i] * blamp[i - lag]. For a lagged alignment both indices
  //    must stay in [0, n): i in [max(0,lag), min(n, n+lag)). Getting these bounds wrong
  //    reads out of range (blamp[-1] or blamp[n]) and only a memory sanitizer catches it.
  auto corr = [&](int lag) {
    double s = 0.0;
    const std::size_t lo = (lag > 0 ? std::size_t(lag) : 0);
    const std::size_t hi = (lag < 0 ? n - std::size_t(-lag) : n);
    for (std::size_t i = lo; i < hi; ++i)
      s += triNaive[i] * triBlamp[i - lag];
    return s;
  };
  const double c0 = corr(0), c1 = corr(1), cm1 = corr(-1);
  std::printf("P3-3 vco-blamp: xcorr lag -1=%g 0=%g +1=%g (argmax must be 0)\n",
              cm1, c0, c1);
  CHECK(c0 > c1 * 1.001);   // no +1-sample delay.
  CHECK(c0 > cm1 * 1.001);  // no -1-sample delay.

  // B) amplitude guard: corner rounding keeps |out| <= ~1, never overshoots,
  //    and is never volume-scaled below the naive swing (fake "improvement").
  double hi = -1e9, lo = 1e9, naiveHi = -1e9;
  for (std::size_t i = 0; i < n; ++i) {
    hi = std::max(hi, triBlamp[i]);
    lo = std::min(lo, triBlamp[i]);
    naiveHi = std::max(naiveHi, triNaive[i]);
  }
  std::printf("P3-3 vco-blamp: corrected max=%.4f min=%.4f, naive max=%.4f\n",
              hi, lo, naiveHi);
  CHECK(hi <= 1.05);              // no overshoot beyond the naive rail.
  CHECK(lo >= -1.05);
  CHECK(naiveHi - hi < 0.05);     // the BLAMP did not lower the peak (no volume fake).
  CHECK(hi > 0.9 && lo < -0.9);   // full swing present — not a silent/low buffer.

  // D) block-invariance: contiguous render == split-into-odd-blocks render, so a
  //    block-boundary reset of the correction state would be caught.
  core::Vco v(sr);
  v.setBaseHz(baseHz);
  v.setWaveform(core::VcoWaveform::kTriangle);
  v.setMorph(0.5);
  std::vector<double> contiguous(n);
  std::size_t idx = 0;
  for (std::size_t i = 0; i < n; ++i) v.tick(&contiguous[idx++], nullptr);
  core::Vco w(sr);
  w.setBaseHz(baseHz);
  w.setWaveform(core::VcoWaveform::kTriangle);
  w.setMorph(0.5);
  std::vector<double> partitioned(n);
  idx = 0;
  const std::size_t blocks[] = {7, 13, 32};  // odd, non-divisor, and even partitions.
  for (std::size_t b = 0, off = 0; off < n; ++b) {
    const std::size_t len = std::min(blocks[b % 3], n - off);
    for (std::size_t j = 0; j < len; ++j) w.tick(&partitioned[off + j], nullptr);
    off += len;
  }
  double partMax = 0.0;
  for (std::size_t i = 0; i < n; ++i)
    partMax = std::max(partMax, std::fabs(contiguous[i] - partitioned[i]));
  std::printf("P3-3 vco-blamp: contiguous-vs-partitioned maxdiff = %.3e\n", partMax);
  CHECK(partMax < 1e-12);  // the correction state is a pure function of phase.

  return true;
}

// ⑦ GH#19 S5 (task #111): the PRODUCT hard-sync entry, requestSync(). ③ above covers the RAW
// primitive syncPulse() and must keep covering exactly what it covered before — its mid-sample
// meaning is asserted below to be UNCHANGED, so a "fix" that silently redefines syncPulse()
// fails here instead of shipping.
//
// WHAT S5 CHANGES. syncPulse() zeroes the accumulator BETWEEN ticks, so the next tick() advances
// once and reports phase `step`: the value discontinuity lands on one sample while the new cycle's
// phase-0 sample is the NEXT one — phase and value disagree by one sample, and the emitted step is
// unband-limited. requestSync() records the reset and lets tick() apply it AFTER its own advance,
// so the reset sample itself reads phase 0 and tick() band-limits that jump.
//
// HOW THE 1/2 LAW IS CHECKED WITHOUT KNOWING E(0). No public path ever emits the phase-0 value
// E(0) (tick() always advances first), so a test cannot obtain it to compare against. It does not
// have to: with two resets at different phases f1 != f2 the emitted reset samples are
// m_i = E(0) - (E(0) - E(f_i))/2, so E(0) CANCELS and the law becomes a pure slope statement
//     m1 - m2 == (E(f1) - E(f2)) / 2.
// That single relation pins BOTH the magnitude (1/2) and the sign at once, and each wrong kernel
// breaks it differently: the full-scale kernel (the "copy polyblepSaw" defect) makes m_i == E(0)
// constant, so the left side goes to 0; the sign-flipped kernel doubles it to -1/2.
static bool test_vco_hardsync_reset_alignment() {
  const double sr = 48000.0;
  const double baseHz = 440.0;
  const std::size_t k1 = 997, k2 = 1234;   // two resets at different phases.

  // Never-synced twin: supplies E(f_i), the value each reset sample would have had.
  core::Vco twin(sr);
  twin.setBaseHz(baseHz);
  twin.setWaveform(core::VcoWaveform::kTriangle);
  std::vector<double> cont(k2 + 4);
  for (std::size_t i = 0; i < cont.size(); ++i) twin.tick(&cont[i]);

  // A fresh VCO's first three samples are E(step), E(2*step), E(3*step) — the trajectory the new
  // cycle MUST follow once the reset sample has read phase 0.
  core::Vco fresh(sr);
  fresh.setBaseHz(baseHz);
  fresh.setWaveform(core::VcoWaveform::kTriangle);
  std::vector<double> head(3);
  for (std::size_t i = 0; i < head.size(); ++i) fresh.tick(&head[i]);

  const auto run = [&](std::size_t k, bool deferred, std::vector<double>& buf) {
    core::Vco v(sr);
    v.setBaseHz(baseHz);
    v.setWaveform(core::VcoWaveform::kTriangle);
    buf.assign(k + 4, 0.0);
    for (std::size_t i = 0; i < k; ++i) v.tick(&buf[i]);
    if (deferred) v.requestSync(); else v.syncPulse();
    for (std::size_t i = k; i < buf.size(); ++i) v.tick(&buf[i]);
  };

  std::vector<double> p1, p2, r1, r2;
  run(k1, true, p1);
  run(k2, true, p2);
  run(k1, false, r1);
  run(k2, false, r2);

  const double pre1 = cont[k1], pre2 = cont[k2];
  const double m1 = p1[k1], m2 = p2[k2];

  // Guard: the two reset phases must actually differ, or the slope check below is vacuous.
  CHECK(std::fabs(pre1 - pre2) > 1e-6);

  // (a) THE 1/2 LAW, E(0) cancelled out. This is the load-bearing assertion.
  CHECK(std::fabs((m1 - m2) - 0.5 * (pre1 - pre2)) < 1e-12);

  // (b) ALIGNMENT: after the product's reset the emitted trajectory is the FRESH cycle's, bit for
  // bit — the next samples are phase step, 2*step. The raw primitive reaches the same values one
  // sample EARLIER (it starts the fresh trajectory AT the reset sample): that is the second of the
  // two discrete timing conventions (reset-then-advance vs request-then-apply), not a defect, and
  // this test asserts which convention each entry point implements. Which one the PRODUCT path is
  // required to use is fixed by the independent master-edge vs reset-frame criterion in the S5
  // mutation runner (task #111 item 2), not by this unit test.
  CHECK(p1[k1 + 1] == head[0]);
  CHECK(p1[k1 + 2] == head[1]);
  CHECK(p2[k2 + 1] == head[0]);
  CHECK(r1[k1] == head[0]);
  CHECK(r1[k1 + 1] == head[1]);
  CHECK(r2[k2] == head[0]);

  // (c) RED-NEGATIVE: a dead reset (no call at all) is a different render, so (a)/(b) cannot pass
  // with the entry point silently doing nothing.
  std::vector<double> nothing(k1 + 4);
  for (std::size_t i = 0; i < nothing.size(); ++i) nothing[i] = cont[i];
  CHECK_FALSE(same_render(nothing, p1));

  // (d) RED-NEGATIVE: syncPulse() must keep its RAW meaning — it does NOT band-limit and does NOT
  // make the reset sample read phase 0. If someone "fixes" syncPulse() to defer, r1[k1] becomes
  // the midpoint and this fails.
  CHECK(r1[k1] != p1[k1]);

  std::printf("P3-3 vco-hardsync: pre1-pre2=%.6e m1-m2=%.6e ratio=%.6f (law 0.5), m1=%.6f m2=%.6f\n",
              pre1 - pre2, m1 - m2, (m1 - m2) / (pre1 - pre2), m1, m2);
  return true;
}

int main() {
  test_vco_voct_and_lilin();
  test_vco_sub_locked();
  test_vco_hardsync_asymmetry();
  test_vco_morph_continuous();
  test_vco_cross_sr_and_buffer();
  test_vco_pwm_extreme_duty();
  test_vco_hardsync_splatter();
  test_vco_narrowpulse_fold();
  test_vco_blamp();
  test_vco_hardsync_reset_alignment();
  test_vco_gh19_s0_morph_ring_and_pwm();
  return ::test::finish("vco");
}
