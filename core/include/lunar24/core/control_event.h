// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ControlEvent: a timestamped external discrete event or parameter command,
// carrying a sample offset within the current block (design/07 §3).

#pragma once

#include <cstdint>

#include <lunar24/core/id_types.h>
#include <lunar24/core/signal.h>

namespace lunar24::core {

// Dispatch lane. Continuous events (knob/joystick/CC) may coalesce under queue
// pressure; critical edges (gate/clock/sync/reset) must never be dropped silently.
enum class ControlLane : std::uint8_t {
  continuous,
  critical,
};

enum class ControlEventKind : std::uint8_t {
  parameter,   // a continuous parameter target change
  pitch,       // pitch CV target (note on pitch)
  pressure,    // pressure/aftertouch target
  gate_on,
  gate_off,
  clock,       // clock edge
  sync,        // arp/sequencer sync/reset edge
  reset,       // failsafe: all-gates-off / clock resync
};

struct ControlEvent {
  ControlEventKind kind;
  ControlLane lane;
  ParameterId parameter = ParameterId{0};  // for kind == parameter
  SignalSample value = SignalSample{0};    // target in virtual volts / index / bool
  std::uint32_t sampleOffset = 0;          // within the current block
  std::uint64_t producerSequence = 0;      // stable tiebreak for same-sample ordering
};

// Same-sample deterministic phase order (design/07 §3). Reset/failsafe first,
// then parameter/pitch/pressure, then off-edges, then sync/clock, then on-edges.
// Within a phase, ordering by lane and producerSequence is stable.
constexpr std::uint32_t kControlEventPhaseCount = 5;

}  // namespace lunar24::core
