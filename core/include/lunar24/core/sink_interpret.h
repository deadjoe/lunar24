// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SinkInterpret (design/07 §4 跨类型 + §5): the sink-side interpretation of a
// continuous input stream into gate/clock semantics. A real output may drive any
// real input; type/range are advisory, never a hard connection filter (PatchGraph
// rejects a cable only by direction). But when the SINK jack is a gate/clock, the
// incoming stream must be *interpreted* there: a continuous CV crossing the jack's
// threshold produces gate behaviour. It is never isolated off by a signal-type
// match. This is the P2-④ deliverable.
//
// The shaper is stateful and per-sample (audio-rate). A hysteresis latch holds the
// gate high until the input drops below (threshold - hysteresis) and low until it
// rises above (threshold + hysteresis), so a signal dithering near the threshold
// does not retrigger. An inverting (bipolar) gate sink presents the NEGATED gate,
// swapping high/low and therefore rising/falling. Because everything is per-sample
// and the only state is the latch, the edge lands at a deterministic ABSOLUTE
// sample independent of how the host partitions blocks — which is exactly why a
// block-lazy implementation (re-evaluate only at a block boundary) must fail the
// partition-invariance check.
//
// Framework-free, no heap, no locks. One GateClockSinkState per sink edge; the
// caller owns the bank (keyed by sink JackId). The actual gate/clock numbers
// (gateThresholdVolts, hysteresisVolts, polarity) come from the JackDescriptor —
// the only source of truth, never a hardcoded value.

#pragma once

#include <cstdint>

#include <lunar24/core/descriptors.h>

namespace lunar24::core {

// A gate transition observed between two consecutive interpreted samples.
enum class GateEdge : std::uint8_t { none = 0, rising = 1, falling = 2 };

// The result of interpreting one continuous input sample at a gate/clock sink.
struct SinkSample {
  bool gateHigh = false;
  GateEdge edge = GateEdge::none;
};

// Per-sink gate/clock interpretation state. One per sink edge; caller-owned.
// Value-initialise ({}) for a fresh state; see sink_gate_reset().
struct GateClockSinkState {
  bool high = false;     // raw hysteresis latch (pre-inversion)
  bool prevOut = false;  // previous OUTPUT (post-inversion), for edge detection
  bool primed = false;   // false until the first sample: no edge on sample 0
};

inline void sink_gate_reset(GateClockSinkState& st) {
  st.high = false;
  st.prevOut = false;
  st.primed = false;
}

// Interpret one continuous input sample `inputVolts` at a gate/clock sink. The
// sink descriptor supplies gateThresholdVolts / hysteresisVolts / polarity. The
// return value is the post-inversion gate level and the transition from the
// previous sample. This is the sink-side "continuous CV -> gate" shaper.
//
// Modelling note (since the machine's `Polarity` describes the signal rail, not
// an explicit gate-direction flag, and this is the only inversion-bearing field
// a JackDescriptor carries): a `bipolar` sink is treated as an inverting gate —
// its output gate is the negation of the raw level, so high/low and rising/falling
// both swap versus a `unipolar` sink on the same input. `unknown` polarity is
// treated as non-inverting (unipolar behaviour): no evidence, no inversion. The
// polarity-effective test drives an explicit unipolar vs bipolar pair to prove the
// field is genuinely consulted, and its negative control (a shaper that ignores
// polarity) must go red.
inline SinkSample sink_gate_interpret(const JackDescriptor& sink, GateClockSinkState& st,
                                      double inputVolts) {
  const double thr = sink.gateThresholdVolts;
  const double hyst = sink.hysteresisVolts >= 0.0 ? sink.hysteresisVolts : 0.0;

  // Raw hysteresis latch: rise clears the upper edge, hold stays while above the
  // lower edge, fall drops below the lower edge. A signal dithering inside the
  // band cannot retrigger.
  bool rawHigh;
  if (!st.high)
    rawHigh = inputVolts >= (thr + hyst);
  else
    rawHigh = inputVolts > (thr - hyst);
  st.high = rawHigh;

  // Polarity: an inverting (bipolar) gate presents the negated gate.
  const bool inverted = (sink.polarity == Polarity::bipolar);
  const bool out = inverted ? !rawHigh : rawHigh;

  // Edge = output transition between the previous sample and this one.
  GateEdge edge = GateEdge::none;
  if (st.primed) {
    if (!st.prevOut && out)
      edge = GateEdge::rising;
    else if (st.prevOut && !out)
      edge = GateEdge::falling;
  }
  st.prevOut = out;
  st.primed = true;

  return SinkSample{out, edge};
}

}  // namespace lunar24::core
