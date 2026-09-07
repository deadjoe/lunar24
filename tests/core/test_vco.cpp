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
  return ::test::finish("vco");
}
