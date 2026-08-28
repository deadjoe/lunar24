// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-① tests for the three time semantics design/07 §3 distinguishes, and the
// sample-rate / buffer invariance §5 requires:
//   - DISCRETE EVENTS (event_timebase.h): absolute-sample scheduler. The same
//     event script must fire at the same absolute sample under any block
//     partition — 64/128/256 uniform AND a mixed non-divisible sequence that a
//     block-index * blockSize accumulator would get wrong.
//   - CONTINUOUS SMOOTHING (parameter_smoothing.h): a seconds time constant that
//     converges at the SAME wall-clock time across 44.1k / 48k / 96k, not at a
//     fixed sample count.
//   - AUDIO-RATE MODULATION (audio_rate_modulation.h): evaluated per sample; its
//     waveform swing must never be flattened by the smoothing path.
//
// Every property has a negative control that genuinely goes red: A (block-relative
// / non-cumulative placement), B (per-block staircase pretending to be
// sample-accurate), C (host pre-interprets so kind gets lost), D (audio-rate
// routed through a smoothing curve), E (fixed-sample-count convergence that
// secretly treats 48k as constant).

#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <lunar24/core/audio_rate_modulation.h>
#include <lunar24/core/control_event.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/parameter_smoothing.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static core::TimedControlEvent mk_timed(core::ControlEventKind k, std::uint64_t sample,
                                        core::SignalSample value, std::uint32_t source,
                                        std::uint64_t seq) {
  core::TimedControlEvent t;
  t.event.kind = k;
  t.event.value = value;
  t.event.source = source;
  t.event.producerSequence = seq;
  t.sample = sample;
  return t;
}

// A continuous parameter event carrying a stable ParameterId (design/07 §3: the one
// thing that may coalesce under pressure — only parameter targets, only the latest).
static core::TimedControlEvent mk_param(std::uint32_t pid, core::SignalSample value,
                                        std::uint32_t source, std::uint64_t seq,
                                        std::uint64_t sample) {
  core::TimedControlEvent t = mk_timed(core::ControlEventKind::parameter, sample, value,
                                       source, seq);
  t.event.parameter = core::ParameterId{pid};
  return t;
}

// The dispatch-invariant comparison: absolute sample + kind + source + sequence.
// The block-relative sampleOffset is intentionally NOT compared — it is a
// partition-dependent view, whereas §5 requires the absolute sample to be stable.
struct Delivery {
  std::uint64_t sample;
  core::ControlEventKind kind;
  std::uint32_t source;
  std::uint64_t seq;
};

static bool same_delivery(const std::vector<Delivery>& a, const std::vector<Delivery>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].sample != b[i].sample) return false;
    if (a[i].kind != b[i].kind) return false;
    if (a[i].source != b[i].source) return false;
    if (a[i].seq != b[i].seq) return false;
  }
  return true;
}

// Drive the real EventTimebase across a partition; assert nothing was dropped and
// nothing was late. Returns the delivered sequence (absolute sample keyed).
static std::vector<Delivery> run_blocks(const std::vector<core::TimedControlEvent>& script,
                                        const std::vector<std::uint32_t>& partition) {
  core::EventTimebase tb;
  for (const auto& e : script) tb.enqueue(e);
  std::vector<Delivery> deliv;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  for (std::uint32_t frames : partition) {
    const std::uint32_t n = tb.processBlock(frames, out, core::kEventTimebaseCapacity);
    for (std::uint32_t i = 0; i < n; ++i) {
      deliv.push_back({out[i].sample, out[i].event.kind, out[i].event.source,
                       out[i].event.producerSequence});
    }
  }
  CHECK_EQ(tb.pending(), 0u);
  CHECK_FALSE(tb.lateSeen());
  return deliv;
}

// The block-relative flaw §5 warns about: an event is placed by
// blockIdx * frames (the CURRENT block's size) instead of an accumulated absolute
// position. Because every block multiplies its own size, this is only correct on a
// uniform partition; a mixed sequence leaves gaps and overlaps.
static std::vector<Delivery> buggy_dispatch(const std::vector<core::TimedControlEvent>& script,
                                            const std::vector<std::uint32_t>& partition) {
  std::vector<core::TimedControlEvent> sorted = script;
  std::stable_sort(sorted.begin(), sorted.end(), core::timed_event_before);
  std::vector<Delivery> deliv;
  std::uint32_t block_idx = 0;
  for (std::uint32_t frames : partition) {
    const std::uint64_t blk_start = static_cast<std::uint64_t>(block_idx) * frames;  // BUG
    const std::uint64_t blk_end = blk_start + frames;
    for (const auto& e : sorted) {
      if (e.sample >= blk_start && e.sample < blk_end) {
        deliv.push_back({e.sample, e.event.kind, e.event.source, e.event.producerSequence});
      }
    }
    ++block_idx;
  }
  return deliv;
}

static std::vector<std::uint32_t> uniform(std::uint32_t size, std::uint32_t count) {
  std::vector<std::uint32_t> v;
  v.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) v.push_back(size);
  return v;
}

static std::vector<std::uint32_t> mixed_partition() {
  // Sum 683 per repetition — deliberately non-divisible and not a power of two so
  // an accumulator bug cannot quietly land on the right sample.
  static const std::uint32_t kPattern[] = {64u, 100u, 37u, 128u, 7u, 256u, 91u};
  std::vector<std::uint32_t> v;
  v.reserve(7u * 30u);
  for (int rep = 0; rep < 30; ++rep) {
    for (std::uint32_t p : kPattern) v.push_back(p);
  }
  return v;
}

static const std::vector<core::TimedControlEvent>& invariance_script() {
  static const std::vector<core::TimedControlEvent> script = [] {
    std::vector<core::TimedControlEvent> s;
    s.push_back(mk_timed(core::ControlEventKind::reset, 0ull, 0.0f, 1u, 0u));
    s.push_back(mk_timed(core::ControlEventKind::parameter, 1ull, 0.5f, 4u, 0u));
    s.push_back(mk_timed(core::ControlEventKind::gate_on, 64ull, 0.0f, 3u, 0u));   // block end boundary
    s.push_back(mk_timed(core::ControlEventKind::gate_off, 100ull, 0.0f, 2u, 0u));  // same-sample cluster
    s.push_back(mk_timed(core::ControlEventKind::clock, 100ull, 0.0f, 1u, 0u));
    s.push_back(mk_timed(core::ControlEventKind::sync, 100ull, 0.0f, 2u, 5u));
    s.push_back(mk_timed(core::ControlEventKind::reset, 101ull, 0.0f, 1u, 1u));
    s.push_back(mk_timed(core::ControlEventKind::gate_on, 10000ull, 0.0f, 3u, 0u));  // ween-ahead
    s.push_back(mk_timed(core::ControlEventKind::clock, 20000ull, 0.0f, 1u, 0u));
    return s;
  }();
  return script;
}

// ---------------------------------------------------------------------------
// Negative control A: block-relative / non-cumulative placement is NOT invariant
// ---------------------------------------------------------------------------

static void buffer_on_invariant() {
  const auto& script = invariance_script();
  const std::vector<Delivery> d64 = run_blocks(script, uniform(64u, 320u));
  const std::vector<Delivery> d128 = run_blocks(script, uniform(128u, 160u));
  const std::vector<Delivery> d256 = run_blocks(script, uniform(256u, 80u));
  const std::vector<Delivery> dmix = run_blocks(script, mixed_partition());

  CHECK_EQ(d64.size(), script.size());
  CHECK_EQ(d128.size(), script.size());
  CHECK_EQ(d256.size(), script.size());
  CHECK_EQ(dmix.size(), script.size());

  // The same event set under the same patch MUST give the same absolute trigger
  // sample across all four block schemes (design/07 §5).
  CHECK(same_delivery(d64, d128));
  CHECK(same_delivery(d128, d256));
  CHECK(same_delivery(d256, dmix));

  // A (detector bites): a non-cumulative blockStart is MASKED on the uniform
  // partition — it reproduces the correct timing there, so a uniform-only check
  // would pass it. The same bug is EXPOSED by the mixed sequence. This proves the
  // invariance probe is not vacuous.
  const std::vector<Delivery> b64 = buggy_dispatch(script, uniform(64u, 320u));
  const std::vector<Delivery> bmix = buggy_dispatch(script, mixed_partition());
  CHECK(same_delivery(b64, d64));           // bug hidden under uniform
  CHECK_FALSE(same_delivery(bmix, dmix));   // bug exposed under mixed
}

// ---------------------------------------------------------------------------
// Boundary cases: block edges, tie-break at one sample, peek-ahead, late safety
// ---------------------------------------------------------------------------

static void boundary_block_edges() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];

  // A single event exactly at sample 64. Blocks are half-open [start, end): an
  // event on a boundary belongs to the block that STARTS there, not the one that
  // ended there.
  tb.enqueue(mk_timed(core::ControlEventKind::gate_on, 64ull, 0.0f, 3u, 0u));
  CHECK_EQ(tb.processBlock(64u, out, core::kEventTimebaseCapacity), 0u);  // [0,64): no
  CHECK_EQ(tb.processBlock(64u, out, core::kEventTimebaseCapacity), 1u);  // [64,128): yes
  CHECK_EQ(out[0].sample, 64ull);
  CHECK_EQ(out[0].event.sampleOffset, 0u);
  CHECK_EQ(tb.blockStart(), 128u);
}

static void same_sample_tiebreak() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  tb.enqueue(mk_timed(core::ControlEventKind::gate_off, 100ull, 0.0f, 2u, 2u));
  tb.enqueue(mk_timed(core::ControlEventKind::reset, 100ull, 0.0f, 1u, 9u));
  tb.enqueue(mk_timed(core::ControlEventKind::sync, 100ull, 0.0f, 2u, 5u));
  tb.enqueue(mk_timed(core::ControlEventKind::gate_on, 100ull, 0.0f, 3u, 3u));
  tb.enqueue(mk_timed(core::ControlEventKind::parameter, 100ull, 0.0f, 4u, 1u));
  tb.enqueue(mk_timed(core::ControlEventKind::clock, 100ull, 0.0f, 1u, 7u));
  const std::uint32_t n = tb.processBlock(256u, out, core::kEventTimebaseCapacity);
  CHECK_EQ(n, 6u);
  // Same absolute sample, deterministic order: phase (reset<parameter<gate_off<
  // sync/clock<gate_on), then source within phase (clock src1 < sync src2).
  const core::ControlEventKind expect[] = {
      core::ControlEventKind::reset, core::ControlEventKind::parameter,
      core::ControlEventKind::gate_off, core::ControlEventKind::clock,
      core::ControlEventKind::sync, core::ControlEventKind::gate_on};
  for (std::uint32_t i = 0; i < n; ++i) {
    CHECK(out[i].event.kind == expect[i]);
    CHECK_EQ(out[i].event.sampleOffset, 100u);
    CHECK_EQ(out[i].sample, 100ull);
  }
}

static void peek_ahead() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  // Enqueued far ahead: it stays queued (peek-ahead) and is never dropped.
  tb.enqueue(mk_timed(core::ControlEventKind::clock, 6000ull, 0.0f, 1u, 0u));
  for (std::uint32_t i = 0; i < 46; ++i) {
    CHECK_EQ(tb.processBlock(128u, out, core::kEventTimebaseCapacity), 0u);
  }
  CHECK_EQ(tb.pending(), 1u);
  CHECK_EQ(tb.blockStart(), 46u * 128u);  // 5888, still before 6000
  const std::uint32_t nd = tb.processBlock(128u, out, core::kEventTimebaseCapacity);  // [5888,6016)
  CHECK_EQ(nd, 1u);
  CHECK_EQ(out[0].sample, 6000ull);
  CHECK_EQ(out[0].event.sampleOffset, 112u);
  CHECK_EQ(tb.pending(), 0u);
}

static void late_event_safety() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  tb.processBlock(64u, out, core::kEventTimebaseCapacity);  // blockStart -> 64
  CHECK_FALSE(tb.lateSeen());
  // Handed in for a block that has already passed: detected, still delivered at
  // the current block start (no unsigned cycle), true sample retained.
  tb.enqueue(mk_timed(core::ControlEventKind::clock, 10ull, 0.0f, 1u, 0u));
  const std::uint32_t n = tb.processBlock(64u, out, core::kEventTimebaseCapacity);  // [64,128)
  CHECK_EQ(n, 1u);
  CHECK(tb.lateSeen());
  CHECK_EQ(out[0].event.sampleOffset, 0u);
  CHECK_EQ(out[0].sample, 10ull);
}

// ---------------------------------------------------------------------------
// Negative control C: interpretation must happen INSIDE core, not at the host
// ---------------------------------------------------------------------------

struct ConsumeState {
  double gate = 0.0;
  std::uint64_t clock_edges = 0;
  double param_cv = 0.0;
};

// Canonical: core reads kind+value and derives behaviour AT CONSUME TIME.
static void interpret_in_core(ConsumeState& s, const core::TimedControlEvent& e) {
  switch (e.event.kind) {
    case core::ControlEventKind::gate_on: s.gate = 1.0; break;
    case core::ControlEventKind::gate_off: s.gate = 0.0; break;
    case core::ControlEventKind::clock: ++s.clock_edges; break;
    case core::ControlEventKind::sync: ++s.clock_edges; break;
    case core::ControlEventKind::reset: s.gate = 0.0; ++s.clock_edges; break;
    case core::ControlEventKind::parameter:
    case core::ControlEventKind::pitch:
    case core::ControlEventKind::pressure:
      s.param_cv = static_cast<double>(e.event.value); break;
  }
}

// A consumer that received only a pre-collapsed {value, edge}: it cannot tell a
// gate_on from a clock, so the true gate+clock semantics are unrecoverable.
struct CollapsedConsumer {
  double gate = 0.0;
  std::uint64_t clock_edges = 0;
  void apply(double value, bool edge) {
    if (edge) {
      gate = value > 0.0 ? 1.0 : 0.0;
      ++clock_edges;
    }
  }
};

static bool kind_semantics_preserved(bool collapse_at_boundary) {
  const core::TimedControlEvent g =
      mk_timed(core::ControlEventKind::gate_on, 5ull, 0.0f, 1u, 0u);
  const core::TimedControlEvent c = mk_timed(core::ControlEventKind::clock, 5ull, 0.0f, 1u, 0u);
  if (!collapse_at_boundary) {
    ConsumeState s;
    interpret_in_core(s, g);
    interpret_in_core(s, c);
    return s.gate == 1.0 && s.clock_edges == 1u;  // gate_on lifts gate; clock ticks once
  }
  // Host boundary pre-interprets: both become "edge, value 0". The collapsed
  // consumer overwrites gate each edge and counts both as clock ticks — the
  // semantic truth (exactly one clock, gate lifted) is lost.
  CollapsedConsumer s;
  s.apply(static_cast<double>(g.event.value), true);
  s.apply(static_cast<double>(c.event.value), true);
  return s.gate == 1.0 && s.clock_edges == 1u;  // false: gate 0, clock_edges 2
}

static void external_event_interpreted_in_core() {
  CHECK(kind_semantics_preserved(false));  // interpretation inside core -> distinct
  CHECK_FALSE(kind_semantics_preserved(true));  // host pre-interprets -> kind lost
}

// ---------------------------------------------------------------------------
// Negative controls B / D / E: smoothing vs sample-accurate vs audio-rate
// ---------------------------------------------------------------------------

static std::uint32_t value_runs_within_block(const std::vector<double>& samples) {
  if (samples.empty()) return 0u;
  std::uint32_t runs = 1u;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    if (samples[i] != samples[i - 1]) ++runs;
  }
  return runs;
}

static constexpr std::uint32_t kBlock = 64u;

static void per_block_staircase_is_not_sample_accurate() {
  // GOOD: real per-sample smoothing changes value within a block.
  core::ParameterSmoother sm;
  sm.setSampleRate(48000.0);
  sm.setTimeConstantSeconds(0.05);
  sm.reset(0.0);
  sm.setTarget(1.0);
  std::vector<double> smooth_block;
  smooth_block.reserve(kBlock);
  for (std::uint32_t i = 0; i < kBlock; ++i) smooth_block.push_back(sm.next());
  CHECK(value_runs_within_block(smooth_block) > 1u);  // moves inside the block

  // BAD: a per-block staircase (value held constant all block, jumped at the
  // boundary) is NOT sample-accurate smoothing — the probe sees one flat run.
  std::vector<double> flat_block;
  flat_block.assign(kBlock, 0.0);  // constant throughout
  CHECK_EQ(value_runs_within_block(flat_block), 1u);
  CHECK_FALSE(value_runs_within_block(flat_block) > 1u);
}

static double peak_to_peak(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double lo = v[0], hi = v[0];
  for (double x : v) {
    if (x < lo) lo = x;
    if (x > hi) hi = x;
  }
  return hi - lo;
}

static void audio_rate_must_not_be_smoothed() {
  // GOOD: the audio-rate source is evaluated per sample and keeps its swing.
  core::AudioRateModulation mod(0.001, 48000.0);  // 1 kHz triangle at 48k
  std::vector<double> raw;
  raw.reserve(512u);
  for (int i = 0; i < 512; ++i) raw.push_back(mod.next());
  const double raw_ptp = peak_to_peak(raw);
  CHECK(raw_ptp > 1.5);  // near full -1..+1 swing

  // D (BAD): the SAME signal routed through a smoothing curve loses its audio-rate
  // information — the waveform swing is low-passed away, so the output swing is a
  // small fraction of the input (the audio-rate path keeps ~all of it).
  core::ParameterSmoother sm;
  sm.setSampleRate(48000.0);
  sm.setTimeConstantSeconds(0.05);
  sm.reset(raw[0]);
  std::vector<double> smoothed;
  smoothed.reserve(raw.size());
  for (double x : raw) {
    sm.setTarget(x);
    smoothed.push_back(sm.next());
  }
  const double sm_ptp = peak_to_peak(smoothed);
  CHECK(sm_ptp < raw_ptp * 0.5);         // >half the swing destroyed by smoothing
  CHECK_FALSE(sm_ptp > 1.5);             // "audio-rate swing retained" probe fails
}

static double wall_clock_reach(double fs, double tau, double fraction) {
  core::ParameterSmoother s;
  s.setSampleRate(fs);
  s.setTimeConstantSeconds(tau);
  s.reset(0.0);
  s.setTarget(1.0);
  std::uint64_t n = 0;
  while (n < 1000000ull) {
    s.next();
    ++n;
    if (s.progress() >= fraction) break;
  }
  return static_cast<double>(n) / fs;
}

static bool cross_sr_consistent(double r44, double r48, double r96) {
  const double tol = 1e-3;  // < 1ms; one-pole should be exact to sub-sample
  const double a = std::abs(r44 - r48);
  const double b = std::abs(r48 - r96);
  return a < tol && b < tol;
}

static double fixed_sample_reach(double fs, double fraction) {
  // E (BAD): converges in a FIXED sample count (2400 = 0.05s * 48k) regardless of
  // fs — this secretly treats 48k as the constant. Wall-clock reach then depends
  // on fs, so a cross-sample-rate check on it is red.
  static constexpr double kSamples = 2400.0;
  std::uint64_t n = 0;
  while (n < 1000000ull) {
    const double prog = (static_cast<double>(n) + 1.0) / kSamples;
    ++n;
    if (prog >= fraction) break;
  }
  return static_cast<double>(n) / fs;
}

static void smoothing_is_wall_clock_invariant() {
  constexpr double kTau = 0.05;
  constexpr double kFrac = 0.6321205588;  // 1 - 1/e, reached at t == tau

  // GOOD: the seconds-time-constant smoother reaches the same fraction at the
  // same wall-clock time under 44.1k / 48k / 96k.
  const double r44 = wall_clock_reach(44100.0, kTau, kFrac);
  const double r48 = wall_clock_reach(48000.0, kTau, kFrac);
  const double r96 = wall_clock_reach(96000.0, kTau, kFrac);
  CHECK(std::abs(r44 - kTau) < 1e-6);
  CHECK(std::abs(r48 - kTau) < 1e-6);
  CHECK(std::abs(r96 - kTau) < 1e-6);
  CHECK(cross_sr_consistent(r44, r48, r96));

  // E (detector bites): a fixed-sample-count smoother is NOT wall-clock invariant;
  // the reach time differs by ~2x across 44.1k and 96k.
  const double m44 = fixed_sample_reach(44100.0, kFrac);
  const double m48 = fixed_sample_reach(48000.0, kFrac);
  const double m96 = fixed_sample_reach(96000.0, kFrac);
  CHECK_FALSE(cross_sr_consistent(m44, m48, m96));
  CHECK(std::abs(m44 - m96) > 1e-3);
}

// ---------------------------------------------------------------------------
// Audio-rate cross-sample-rate check (the analogue of the smoothing one, but on
// measured FREQUENCY instead of wall-clock reach time). The old design hardcoded
// 48000 as the default rate; a host running 96k that omitted the rate got a phase
// step computed from 48k, so a "1 kHz" triangle actually ran at 2 kHz — silently,
// with tests still green (the only audio-rate call site always passed 48000, and
// the cross-sample-rate path was built on smoothing, which did not need it).
// ---------------------------------------------------------------------------

// Measure the actual frequency of an AudioRateModulation by counting rising
// zero-crossings of its triangle over a fixed WALL-CLOCK window (0.05 s), so the
// sample count scales with fs rather than being a fixed number. A good audio-rate
// source with period P reads ~1/P Hz at ANY fs. The measurement is quantized by the
// crossing window (resolution = 1/window = 20 Hz), so tolerate a narrow band.
static double audio_rate_measured_hz(double period_seconds, double fs) {
  core::AudioRateModulation m(period_seconds, fs);
  double prev = m.next();
  std::uint64_t crossings = 0;
  const std::uint64_t n = static_cast<std::uint64_t>(fs * 0.05);
  for (std::uint64_t i = 1; i < n; ++i) {
    const double cur = m.next();
    if (prev < 0.0 && cur >= 0.0) ++crossings;  // one rising zero-cross per period
    prev = cur;
  }
  return static_cast<double>(crossings) / (static_cast<double>(n) / fs);
}

// The original "hardcoded 48k default" defect reproduced as a negative control: the
// phase advance uses a FIXED 48k rate (step = 1/(48000·P)) whatever the host fs.
// The wave then completes the same number of samples per period at every fs, so the
// measured frequency scales with fs (48k→1000 Hz, 96k→2000 Hz). This is exactly what
// the constructor/member `48000.0` default produced before the fix.
static double buggy_audio_rate_measured_hz(double period_seconds, double fs) {
  double phase = 0.0;
  const double step = 1.0 / (48000.0 * period_seconds);  // BUG: fixed 48k
  auto tri = [](double p) { return (p < 0.5) ? (4.0 * p - 1.0) : (3.0 - 4.0 * p); };
  double prev = tri(phase);
  std::uint64_t crossings = 0;
  const std::uint64_t n = static_cast<std::uint64_t>(fs * 0.05);
  for (std::uint64_t i = 1; i < n; ++i) {
    phase += step;
    if (phase >= 1.0) phase -= 1.0;
    const double cur = tri(phase);
    if (prev < 0.0 && cur >= 0.0) ++crossings;
    prev = cur;
  }
  return static_cast<double>(crossings) / (static_cast<double>(n) / fs);
}

// Hz measurements can be off by the crossing-window resolution (20 Hz), so compare
// RELATIVE spread, not absolute ms. A genuine 2x defect (48k default at a 96k
// session) is ~100% off and is caught by this generous 8% band; legitimate
// quantisation noise (~2-4%) passes.
static bool audio_cross_sr_consistent(double h44, double h48, double h96) {
  const double mean = (h44 + h48 + h96) / 3.0;
  const double max_dev =
      std::max(std::abs(h44 - mean),
               std::max(std::abs(h48 - mean), std::abs(h96 - mean)));
  return max_dev / mean < 0.08;
}

static void audio_rate_is_wall_clock_invariant() {
  constexpr double kPeriod = 0.001;       // a 1 kHz triangle
  constexpr double kTargetHz = 1000.0;

  // GOOD: the same period_seconds read the same Hz at 44.1k / 48k / 96k.
  const double h44 = audio_rate_measured_hz(kPeriod, 44100.0);
  const double h48 = audio_rate_measured_hz(kPeriod, 48000.0);
  const double h96 = audio_rate_measured_hz(kPeriod, 96000.0);
  CHECK(h44 > kTargetHz - 60.0 && h44 < kTargetHz + 60.0);
  CHECK(h48 > kTargetHz - 60.0 && h48 < kTargetHz + 60.0);
  CHECK(h96 > kTargetHz - 60.0 && h96 < kTargetHz + 60.0);
  CHECK(audio_cross_sr_consistent(h44, h48, h96));

  // The original 48k-default defect (detector bites): a fixed-48k phase step makes
  // the measured Hz track fs, so the cross-rate check is red and 96k is 2x 48k.
  const double m44 = buggy_audio_rate_measured_hz(kPeriod, 44100.0);
  const double m48 = buggy_audio_rate_measured_hz(kPeriod, 48000.0);
  const double m96 = buggy_audio_rate_measured_hz(kPeriod, 96000.0);
  CHECK_FALSE(audio_cross_sr_consistent(m44, m48, m96));
  CHECK(std::abs(m96 - m48) > 600.0);  // 2000 vs 1000
}

static void unconfigured_rate_is_inert_not_wrong() {
  // A default-constructed modulation (rate unset = 0.0, matching
  // ParameterSmoother's unset 0.0) must be INERT, not a lurching 48k: phaseStep_
  // stays 0 so the phase never advances and next() is a constant, not a wrong-rate
  // wave. There is no implicit sample rate anywhere in core (requirement 3).
  core::AudioRateModulation unset;
  const double a = unset.next();
  const double b = unset.next();
  CHECK(a == b);                       // never advances -> constant DC
  CHECK_EQ(unset.sampleRate(), 0.0);   // no hidden 48000
  // Reconfiguring activates it: rate now set, phase steps, output changes.
  unset.setSampleRate(48000.0);
  unset.setPeriodSeconds(0.001);
  const double c = unset.next();
  const double d = unset.next();
  CHECK(c != d);
}

// ---------------------------------------------------------------------------
// design/07 §3 pressure policy (GH#2): the critical lane must be independent of
// continuous pressure; only parameter events coalesce (by stable ParameterId, under
// pressure, never across kinds); critical overflow yields a deterministic reset +
// flush; and an undersized dispatch buffer delays rather than drops an edge.
// ---------------------------------------------------------------------------

// A (negative control): continuous lane full must NOT block the critical lane.
// A critical edge is admitted on its own reservation and still delivered.
static void critical_reserved_when_continuous_full() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  // Fill the continuous lane to its cap with far-future parameters (distinct ids so
  // no earlier coalesce is possible and nothing is due yet).
  for (std::uint32_t i = 0; i < core::kEventTimebaseCapacity; ++i)
    CHECK(tb.enqueue(mk_param(i, core::SignalSample{0.0f}, 1u, i, 100000ull + i)));
  CHECK_EQ(tb.pending(), core::kEventTimebaseCapacity);  // continuous lane full
  // An edge on the critical reservation is still admitted and delivered.
  CHECK(tb.enqueue(mk_timed(core::ControlEventKind::gate_on, 5ull, core::SignalSample{0.0f},
                            9u, 0u)));
  const std::uint32_t n = tb.processBlock(100u, out, core::kEventTimebaseCapacity);
  CHECK_EQ(n, 1u);
  CHECK(out[0].event.kind == core::ControlEventKind::gate_on);
  CHECK_EQ(out[0].sample, 5ull);
  CHECK_EQ(tb.criticalOverflow(), 0u);
}

// B (negative control): under continuous pressure only `parameter` coalesces, by
// stable ParameterId into the latest target; pitch/pressure never coalesce.
static void parameter_only_coalescing_under_pressure() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  for (std::uint32_t i = 0; i < core::kEventTimebaseCapacity; ++i)
    CHECK(tb.enqueue(mk_param(i, core::SignalSample{0.0f}, 1u, i, 100000ull + i)));
  CHECK_EQ(tb.pending(), core::kEventTimebaseCapacity);
  // A same-ParameterId parameter under pressure coalesces into the LATEST target.
  CHECK(tb.enqueue(mk_param(7u, core::SignalSample{0.75f}, 8u, 999u, 200000ull)));
  CHECK(tb.parameterCoalesced() >= 1u);
  CHECK_EQ(tb.pending(), core::kEventTimebaseCapacity);      // coalesced, still at cap
  CHECK_EQ(tb.continuousOverflow(), 0u);
  // A continuous-but-non-parameter event (pitch) NEVER coalesces under pressure.
  CHECK_FALSE(tb.enqueue(mk_timed(core::ControlEventKind::pitch, 300000ull,
                                  core::SignalSample{0.4f}, 2u, 1u)));
  CHECK(tb.continuousOverflow() >= 1u);
  CHECK_EQ(tb.pending(), core::kEventTimebaseCapacity);
}

// C (negative control): a genuine critical overflow raises a deterministic failsafe —
// the next safe boundary emits a canonical reset FIRST, then clears the lost-trust
// critical batch (no authoritative performance state exists yet, so this is the
// design/07 fallback; downstream gate identity is still #8/#4).
static void critical_overflow_reconciles_with_failsafe() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  for (std::uint32_t i = 0; i < core::kEventCriticalCapacity; ++i)
    CHECK(tb.enqueue(mk_timed(core::ControlEventKind::gate_on, 100000ull + i,
                              core::SignalSample{0.0f}, 1u, i)));
  CHECK_EQ(tb.pending(), core::kEventCriticalCapacity);
  // One more edge overflows the critical lane: rejected, counted, one reconcile set.
  CHECK_FALSE(tb.enqueue(mk_timed(core::ControlEventKind::sync, 100000ull +
                                core::kEventCriticalCapacity, core::SignalSample{0.0f}, 2u, 0u)));
  CHECK(tb.criticalOverflow() >= 1u);
  CHECK(tb.reconcilePending());
  // Next safe boundary: reset FIRST at offset 0, then flush the unsafed batch.
  const std::uint32_t n = tb.processBlock(4096u, out, core::kEventTimebaseCapacity);
  CHECK(n >= 1u);
  CHECK(out[0].event.kind == core::ControlEventKind::reset);
  CHECK_EQ(out[0].event.sampleOffset, 0u);
  CHECK(tb.reconcileCount() >= 1u);
  CHECK(tb.criticalFlushed() >= core::kEventCriticalCapacity);
  CHECK_FALSE(tb.reconcilePending());
}

// D (negative control): an undersized dispatch buffer must DELAY a critical edge,
// never drop it — capacity==1 with two same-sample edges keeps the second pending
// and delivers it at offset 0 on the next block.
static void output_capacity_never_drops_critical() {
  core::EventTimebase tb;
  core::TimedControlEvent out[core::kEventTimebaseCapacity];
  tb.enqueue(mk_timed(core::ControlEventKind::gate_on, 5ull, core::SignalSample{0.0f}, 1u, 0u));
  tb.enqueue(mk_timed(core::ControlEventKind::clock, 5ull, core::SignalSample{0.0f}, 2u, 0u));
  const std::uint32_t n = tb.processBlock(16u, out, 1u);  // capacity 1
  CHECK_EQ(n, 1u);
  CHECK(tb.dispatchCapacity() >= 1u);
  CHECK_EQ(tb.pending(), 1u);  // the second critical is NOT dropped
  // Next block delivers the leftover edge as a late event at offset 0.
  const std::uint32_t m = tb.processBlock(16u, out, core::kEventTimebaseCapacity);
  CHECK_EQ(m, 1u);
  CHECK_EQ(out[0].event.sampleOffset, 0u);
  CHECK_EQ(out[0].sample, 5ull);
  CHECK_EQ(tb.pending(), 0u);
  CHECK(tb.lateSeen());
}

int main() {
  buffer_on_invariant();
  boundary_block_edges();
  same_sample_tiebreak();
  peek_ahead();
  late_event_safety();
  external_event_interpreted_in_core();
  per_block_staircase_is_not_sample_accurate();
  audio_rate_must_not_be_smoothed();
  smoothing_is_wall_clock_invariant();
  audio_rate_is_wall_clock_invariant();
  unconfigured_rate_is_inert_not_wrong();
  critical_reserved_when_continuous_full();      // A
  parameter_only_coalescing_under_pressure();    // B
  critical_overflow_reconciles_with_failsafe();  // C
  output_capacity_never_drops_critical();        // D
  return ::test::finish("time semantics");
}
