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

// Opaque stable identity of the control/event producer (note on/off source,
// knob, joystick, MIDI channel, UI widget). Used for deterministic same-sample
// ordering (design/07 §3) — it is NOT a pointer or an address.
using ControlSourceId = std::uint32_t;

// Dispatch lane. Continuous events (knob/joystick/CC) may coalesce under queue
// pressure; critical edges (note/gate/clock/sync/reset) must never be dropped
// silently. The lane is DERIVED from kind (design/07 §3), so it can never
// contradict the kind.
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

// The dispatch lane an event kind belongs to (design/07 §3: note/gate/clock/
// sync/reset edges are critical; parameter/pitch/pressure are continuous).
constexpr ControlLane control_event_lane(ControlEventKind k) {
  switch (k) {
    case ControlEventKind::parameter:
    case ControlEventKind::pitch:
    case ControlEventKind::pressure:
      return ControlLane::continuous;
    case ControlEventKind::gate_on:
    case ControlEventKind::gate_off:
    case ControlEventKind::clock:
    case ControlEventKind::sync:
    case ControlEventKind::reset:
      return ControlLane::critical;
  }
  return ControlLane::critical;
}

// Same-sample deterministic phase order (design/07 §3): Reset/failsafe →
// Parameter/Pitch/Pressure target → Note/Gate Off → Sync/Clock edge → Note/Gate
// On. Within a phase, ordering by stable source id and producer sequence.
constexpr std::uint32_t kControlEventPhaseCount = 5;

constexpr std::uint32_t control_event_phase(ControlEventKind k) {
  switch (k) {
    case ControlEventKind::reset:           return 0;
    case ControlEventKind::parameter:
    case ControlEventKind::pitch:
    case ControlEventKind::pressure:        return 1;
    case ControlEventKind::gate_off:        return 2;
    case ControlEventKind::sync:
    case ControlEventKind::clock:           return 3;
    case ControlEventKind::gate_on:         return 4;
  }
  return kControlEventPhaseCount;  // defensive: out of range marks an invalid kind
}

struct ControlEvent {
  ControlEventKind kind;
  ParameterId parameter = ParameterId{0};  // for kind == parameter
  SignalSample value = SignalSample{0};    // target in virtual volts / index / bool
  std::uint32_t sampleOffset = 0;          // within the current block
  ControlSourceId source = 0;              // stable producer id
  std::uint64_t producerSequence = 0;      // stable tiebreak for same-source ordering

  ControlLane lane() const { return control_event_lane(kind); }
};

}  // namespace lunar24::core
