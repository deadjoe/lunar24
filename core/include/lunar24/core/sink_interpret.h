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
// The gate level is a pure threshold comparison against the sink's
// gateThresholdVolts with a hysteresis band: the gate rises once the input clears
// (threshold + hysteresis) and stays high until it drops below (threshold -
// hysteresis), so a signal dithering near the threshold does not retrigger. The
// path deliberately does NOT consult Polarity: that field describes the signal
// RAIL (unipolar 0..+V, bipolar -V..+V), not a gate direction, and the hardware
// gate jacks are all 0..10V single-polarity — there is no evidence of an inverting
// gate. Per design discipline ("字段被读了不是目标，行为符合证据才是") a field with no
// role on a path is honestly left unused, never granted an invented behaviour
// (see enums.h). Because everything is per-sample and the only state is the latch,
// the edge lands at a deterministic ABSOLUTE sample independent of how the host
// partitions blocks — which is exactly why a block-lazy implementation
// (re-evaluate only at a block boundary) must fail the partition-invariance check.
//
// Framework-free, no heap, no locks. One GateClockSinkState per sink edge; the
// caller owns the bank (keyed by sink JackId). The actual gate/clock numbers
// (gateThresholdVolts, hysteresisVolts) come from the JackDescriptor — the only
// source of truth, never a hardcoded value.

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
// sink descriptor supplies gateThresholdVolts / hysteresisVolts. The return value
// is the gate level and the transition from the previous sample — the sink-side
// "continuous CV -> gate" shaper. The gate is a straight threshold comparison
// (with hysteresis); Polarity is deliberately not consulted (no evidence of an
// inverting gate; see the module comment).
inline SinkSample sink_gate_interpret(const JackDescriptor& sink, GateClockSinkState& st,
                                      double inputVolts) {
  const double thr = sink.gateThresholdVolts;
  const double hyst = sink.hysteresisVolts >= 0.0 ? sink.hysteresisVolts : 0.0;

  // Raw hysteresis latch: rise clears the upper edge, hold stays while above the
  // lower edge, fall drops below the lower edge. A signal dithering inside the
  // band cannot retrigger.
  if (!st.high)
    st.high = inputVolts >= (thr + hyst);
  else
    st.high = inputVolts > (thr - hyst);

  // The gate level is the raw latch, unmodified: no polarity inversion on this
  // path (the field describes the rail, not a gate direction we can act on here).
  const bool out = st.high;

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
