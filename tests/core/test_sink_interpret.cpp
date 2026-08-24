// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-④ tests for the sink-side gate/clock interpretation layer
// (design/07 §4 跨类型 + §5) — the counterpart to test_patch_graph's
// connection-facts tests. Cross-type connection lives there (connect is
// direction-only, signal type is advisory); here we prove the INTERPRETATION:
// a continuous stream fed into a gate/clock sink is shaped into gate semantics
// by threshold / hysteresis / polarity / edge, at the sink, never isolated by a
// signal-type match.
//
// @Claude's guards, each with a negative control proving the detector is live:
//   * hysteresis works — a signal dithering in the threshold band does NOT
//     retrigger. The negative (a shaper that ignores hysteresis) retriggers on
//     the very same input.
//   * edge is sample-accurate and partition-invariant — the rising edge lands on
//     the same absolute sample under 64/128/256 AND the mixed non-uniform
//     partition (legacy P2-① partition). The negative (a block-lazy shaper that
//     re-evaluates only at a block boundary) lets the edge drift.
//   * Polarity is deliberately NOT exercised here: the field describes the signal
//     rail, not a gate direction, and there is no evidence of an inverting gate —
//     so the shaper reads only threshold/hysteresis (see sink_interpret.h).

#include "mini_test.h"

#include <cstdint>
#include <vector>

#include <lunar24/core/descriptors.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/sink_interpret.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------- helpers ----

// Build a gate/clock jack descriptor with the fields the shaper reads.
static core::JackDescriptor gate_jack(core::SignalType st, core::Polarity pol,
                                      double thr, double hyst) {
  core::JackDescriptor j{};
  j.signalType = st;
  j.polarity = pol;
  j.gateThresholdVolts = thr;
  j.hysteresisVolts = hyst;
  return j;
}

// Run the real per-sample shaper over a whole signal, returning the sample
// sequence (gate level + edge) — the absolute-sample trace.
static std::vector<core::SinkSample> run_all(const core::JackDescriptor& jk,
                                             const std::vector<double>& in) {
  core::GateClockSinkState st;
  std::vector<core::SinkSample> out;
  out.reserve(in.size());
  for (double v : in) out.push_back(core::sink_gate_interpret(jk, st, v));
  return out;
}

// Run the real shaper over `in` chunked into `blocks`, returning the absolute
// sample of the FIRST rising edge. Honest framing: because the shaper is
// per-sample and carries only the latch, this is independent of the chunking.
static int real_first_rising_abs(const core::JackDescriptor& jk,
                                 const std::vector<double>& in,
                                 const std::vector<std::uint32_t>& blocks) {
  core::GateClockSinkState st;
  std::uint32_t i = 0, b = 0, n = static_cast<std::uint32_t>(in.size());
  while (i < n) {
    std::uint32_t blk = blocks[b % blocks.size()];
    for (std::uint32_t k = 0; k < blk && i < n; ++k, ++i) {
      auto s = core::sink_gate_interpret(jk, st, in[i]);
      if (s.edge == core::GateEdge::rising) return static_cast<int>(i);
    }
    ++b;
  }
  return -1;
}

// --------------------------------------------------------------------
// @Claude guard: hysteresis "proves it works".
// --------------------------------------------------------------------

// A signal that dithers INSIDE the threshold band after one clean rise: with
// hysteresis the latch holds it high the whole time (no retrigger); the same
// values under a single threshold would retrigger on every crossing.
static const std::vector<double> kJitter = {3.0, 7.0, 5.5, 4.5, 5.2,
                                            4.3, 5.6, 4.1, 3.0};

static void hysteresis_holds_through_band_jitter() {
  // band = [4.0, 6.0] (thr 5.0, hyst 1.0), unipolar.
  auto jk = gate_jack(core::SignalType::gate, core::Polarity::unipolar, 5.0, 1.0);
  auto seq = run_all(jk, kJitter);

  int rising = 0, falling = 0, inBandEdges = 0;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq[i].edge == core::GateEdge::rising) ++rising;
    if (seq[i].edge == core::GateEdge::falling) ++falling;
    // The in-band dithering samples (index 2..7) must produce NO edge.
    if (i >= 2 && i <= 7 && seq[i].edge != core::GateEdge::none) ++inBandEdges;
  }
  // A clean single gate: exactly one rise (after the initial below-band low) and
  // one fall (drops below the lower edge). The dither inside the band is silent.
  CHECK_EQ(rising, 1);
  CHECK_EQ(falling, 1);
  CHECK_EQ(inBandEdges, 0);
  CHECK_EQ(seq[1].edge, core::GateEdge::rising);
  CHECK_EQ(seq[8].edge, core::GateEdge::falling);
}

// A shaper that IGNORES hysteresis (single threshold) — the exact "退化成单阈值"
// degradation @Claude names. Same kJitter input must retrigger on every crossing.
static core::SinkSample single_threshold_interpret(const core::JackDescriptor& jk,
                                                   core::GateClockSinkState& st,
                                                   double v) {
  double thr = jk.gateThresholdVolts;
  bool raw = (v >= thr);
  bool out = (jk.polarity == core::Polarity::bipolar) ? !raw : raw;
  core::GateEdge edge = core::GateEdge::none;
  if (st.primed) {
    if (!st.prevOut && out) edge = core::GateEdge::rising;
    else if (st.prevOut && !out) edge = core::GateEdge::falling;
  }
  st.prevOut = out;
  st.primed = true;
  return core::SinkSample{out, edge};
}

static void single_threshold_retriggers() {
  // A no-hysteresis shaper (reads only the single threshold) on the SAME jitter
  // that the hysteresis shaper held silent: it must retrigger, in-band, many
  // times. This is the red-worthy symptom — the no-retrigger property would
  // forbid it, so a shaper that silently dropped hysteresis would be caught.
  auto jk = gate_jack(core::SignalType::gate, core::Polarity::unipolar, 5.0, 1.0);
  core::GateClockSinkState st;
  int inBand = 0;
  for (std::size_t i = 0; i < kJitter.size(); ++i) {
    auto s = single_threshold_interpret(jk, st, kJitter[i]);
    if (i >= 2 && i <= 7 && s.edge != core::GateEdge::none) ++inBand;
  }
  CHECK(inBand > 1);  // multiple false triggers without hysteresis
}

// --------------------------------------------------------------------
// @Claude guard: edge sample-accurate + partition-invariant.
// --------------------------------------------------------------------

// A mono-rising CV ramp, val = 0.1 * sample. Reaches the upper edge (thr 5.0 +
// hyst 0.5 = 5.5) at sample 55.
static std::vector<double> ramp(std::uint32_t n) {
  std::vector<double> v;
  v.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) v.push_back(0.1 * static_cast<double>(i));
  return v;
}

static void edge_absolute_sample_partition_invariant() {
  auto jk = gate_jack(core::SignalType::gate, core::Polarity::unipolar, 5.0, 0.5);
  auto in = ramp(512);

  std::vector<std::uint32_t> b64 = {64};
  std::vector<std::uint32_t> b128 = {128};
  std::vector<std::uint32_t> b256 = {256};
  std::vector<std::uint32_t> mixed = {64, 100, 37, 128, 7, 256, 91};  // legacy P2-① partition

  // The rising edge must land at the SAME absolute sample under every partition.
  CHECK_EQ(real_first_rising_abs(jk, in, b64), 55);
  CHECK_EQ(real_first_rising_abs(jk, in, b128), 55);
  CHECK_EQ(real_first_rising_abs(jk, in, b256), 55);
  CHECK_EQ(real_first_rising_abs(jk, in, mixed), 55);

  // Per-sample-identical reference (P2-① judge): the mixed partition's full
  // sequence equals a plain sequential partition's sequence.
  auto reference = run_all(jk, in);
  core::GateClockSinkState st;
  std::uint32_t i = 0, b = 0, n = static_cast<std::uint32_t>(in.size());
  std::size_t idx = 0;
  while (i < n) {
    std::uint32_t blk = mixed[b % mixed.size()];
    for (std::uint32_t k = 0; k < blk && i < n; ++k, ++i, ++idx) {
      auto s = core::sink_gate_interpret(jk, st, in[i]);
      CHECK_EQ(s.gateHigh, reference[idx].gateHigh);
      CHECK_EQ(s.edge, reference[idx].edge);
    }
    ++b;
  }
}

// A block-LAZY shaper: it re-evaluates the threshold only at a block boundary and
// holds the level for the whole block. This is the degradation that lets the
// edge drift with the partition.
static int lazy_first_rising_abs(const core::JackDescriptor& jk,
                                 const std::vector<double>& in,
                                 const std::vector<std::uint32_t>& blocks) {
  double thr = jk.gateThresholdVolts;
  std::uint32_t i = 0, b = 0, n = static_cast<std::uint32_t>(in.size());
  bool prev = false;
  while (i < n) {
    std::uint32_t blk = blocks[b % blocks.size()];
    bool level = in[i] >= thr;  // block-start-only decision, held for the block
    bool edge = (b > 0 && level != prev);
    prev = level;
    if (edge && level) return static_cast<int>(i);  // rising at this block's start
    i += blk;
    ++b;
  }
  return -1;
}

static void block_lazy_edge_moves() {
  auto jk = gate_jack(core::SignalType::gate, core::Polarity::unipolar, 5.0, 0.5);
  auto in = ramp(512);
  std::vector<std::uint32_t> b64 = {64};
  std::vector<std::uint32_t> b256 = {256};

  int a = lazy_first_rising_abs(jk, in, b64);
  int bv = lazy_first_rising_abs(jk, in, b256);
  // The lazy impl's edge sits at the first block start past the threshold, which
  // differs by block size. The one-sample-accurate shaper would not move. So this
  // is the symptom that a block-lazy implementation is NOT partition-invariant
  // (and therefore a real shaper that silently became block-lazy would be caught).
  CHECK(a != bv);
  CHECK(a != 55);  // and it differs from the sample-accurate answer on partition 64 too
  CHECK(bv != 55);
}

// --------------------------------------------------------------------
// Composite: cross-type connect accepts a CV -> gate sink, and the sink
// INTERPRETS it (not isolated off by signal type).
// --------------------------------------------------------------------

static void cv_into_gate_sink_is_interpreted() {
  core::JackDescriptor cv{};
  cv.id = core::JackId{1};
  cv.direction = core::PinDirection::output;
  cv.maxCables = 1u;
  cv.signalType = core::SignalType::cv;

  core::JackDescriptor g{};
  g.id = core::JackId{2};
  g.direction = core::PinDirection::input;
  g.maxCables = 1u;
  g.signalType = core::SignalType::gate;
  g.polarity = core::Polarity::unipolar;
  g.gateThresholdVolts = 5.0;
  g.hysteresisVolts = 1.0;

  const core::JackDescriptor jacks[] = {cv, g};
  core::PatchGraph graph(jacks, 2u, nullptr, 0u);

  // A CV output driving a gate input is accepted — signal type is advisory, not a
  // connection filter.
  CHECK(graph.connect(core::JackId{1}, core::JackId{2}));
  core::JackId src{0};
  CHECK(graph.resolveInput(core::JackId{2}, &src));
  CHECK_EQ(src, cv.id);

  // And once driving it, the gate sink INTERPRETS the CV: a CV ramp crossing the
  // threshold produces gate edges. It is not isolated by the type mismatch.
  std::vector<double> rampVals = {1.0, 2.0, 3.0, 4.0, 6.0, 7.0, 6.0, 0.0};
  auto seq = run_all(g, rampVals);
  int rising = 0;
  for (auto& s : seq)
    if (s.edge == core::GateEdge::rising) ++rising;
  CHECK(rising >= 1);
}

int main() {
  hysteresis_holds_through_band_jitter();
  single_threshold_retriggers();
  edge_absolute_sample_partition_invariant();
  block_lazy_edge_moves();
  cv_into_gate_sink_is_interpreted();
  return ::test::finish("sink_interpret");
}
