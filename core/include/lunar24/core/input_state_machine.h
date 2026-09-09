// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unified performance-input state machine (design/06 §P4, design/07 §1, §3).
//
// The SINGLE interpretation choke point. Pointer (panel touch), computer
// keyboard and MIDI are three thin adapters that only translate a native
// message into a normalized PerformanceInput — none of them interpret. The
// InputStateMachine is the only place a normalized input becomes a canonical
// ControlEvent, so the same control sequence reaching the SAME state machine
// from any source yields bit-identical internal CV/gate/clock. That is the P4
// exit condition, and it is structural, not a coincidence a test has to chase.
//
// The mapping rules encoded here ARE the design contract (design/07 §3):
//   * note_on    -> gate_on + pitch event; velocity folds into the original's
//                   single pressure dimension (design/06 §L4/§P4: the keyboard
//                   has pressure, not a separate velocity target). A sounding
//                   keyboard voice needs the note latched: gate goes high with
//                   the pitch, not downstream.
//   * note_off   -> gate_off event
//   * aftertouch -> pressure event (pressure/aftertouch only ever touch pressure)
//   * cc         -> parameter event, ONLY if the controller is in the data-driven
//                   ccLearn map; otherwise ignored, never fabricates a target
//   * clock      -> clock event (external clock / MIDI clock)
// Enforcing "no phantom capability" is the whole point: MIDI is an entry, never
// a new feature. CC learn is data (a controller -> existing ParameterId map),
// never a hardcoded branch.

#pragma once

#include <cstdint>

#include <lunar24/core/control_event.h>
#include <lunar24/core/id_types.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/signal.h>

namespace lunar24::core {

// The framework-independent vocabulary every source translates INTO. A sources
// only ever produces these; it never constructs a ControlEvent on its own.
enum class PerfInputKind : std::uint8_t {
  note_on = 0,     // pitch (1 V/oct) + velocity -> pressure fold
  note_off = 1,
  aftertouch = 2,  // pressure/aftertouch value -> pressure
  cc = 3,          // controller -> existing ParameterId (data-driven map)
  clock = 4,       // external clock edge
};

// One normalized performance input. The adapter stamps the absolute host sample
// at which the native message arrived; the whole (kind, value, timing) sequence
// is exactly what the P4 exit condition compares across sources. channel/source/
// seq give a stable, deterministic same-sample ordering (design/07 §3).
struct PerformanceInput {
  PerfInputKind kind;
  std::uint64_t sample = 0;                // absolute host sample of this message
  SignalSample pitch = SignalSample{0};    // volts (1 V/oct) for note_on
  SignalSample value = SignalSample{0};    // velocity / pressure / cc value
  std::uint16_t controller = 0;            // cc number for kind==cc
  std::uint8_t channel = 0;                // source sub-id (MIDI channel etc.)
  ControlSourceId source = 0;              // stable producer id (pointer/key/midi)
  NoteId noteId = 0;                       // note/touch press identity (GH#8)
  std::uint64_t seq = 0;                   // deterministic same-source tiebreak
  // GH#12 task#101: which physical performance side produced this input. An explicit
  // internal metadata field carried through translate() onto every ControlEvent it
  // emits; a source adapter states the side it read, it is never inferred from the
  // channel/pitch/noteId. Defaults to Left so every pre-#101 caller is unchanged.
  KeyboardSide side = KeyboardSide::Left;
};

// Data-driven CC-learn binding: a controller number -> the ParameterId of an
// EXISTING control. Config read from the machine definition / user learn map,
// never a branch. A controller with no binding is ignored by translate().
struct CcBinding {
  std::uint16_t controller;
  ParameterId target;
};

// The input state machine. Network-free, fixed-size, no allocation (design/07
// §5). translate() turns one normalized input into the canonical ControlEvents
// it implies; at most 3 (note_on emits gate_on + pitch + pressure). Returns 0
// for a cc with no valid binding — it is dropped, not fabricated.
class InputStateMachine {
 public:
  InputStateMachine(const CcBinding* bindings, std::uint32_t bindingsCount)
      : bindings_(bindings), bindingsCount_(bindingsCount) {}

  std::uint32_t translate(const PerformanceInput& in, ControlEvent* out,
                          std::uint32_t capacity) const;

 private:
  const ParameterId* resolve(std::uint16_t controller) const {
    for (std::uint32_t i = 0; i < bindingsCount_; ++i)
      if (bindings_[i].controller == controller) return &bindings_[i].target;
    return nullptr;
  }

  const CcBinding* bindings_;
  std::uint32_t bindingsCount_;
};

inline std::uint32_t InputStateMachine::translate(const PerformanceInput& in,
                                                  ControlEvent* out,
                                                  std::uint32_t capacity) const {
  // GH#8: note_on is a WHOLE TRANACTION. It emits exactly three events (pitch for
  // the target, pressure for the velocity fold, gate_on to latch the voice). It is
  // all-or-none: if the caller's out buffer cannot hold all three, return 0 and
  // write NOTHING — never a partial note (a latch without its target, or a pitch
  // without a gate). Every other kind needs exactly one event, so a capacity of 0
  // on those also returns 0 with no partial write.
  std::uint32_t need = (in.kind == PerfInputKind::note_on) ? 3u : 1u;
  if (capacity < need) return 0;

  std::uint32_t n = 0;
  auto push = [&](ControlEventKind kind, SignalSample value,
                  ParameterId parameter = ParameterId{0}) {
    out[n].kind = kind;
    out[n].parameter = parameter;
    out[n].value = value;
    out[n].sampleOffset = 0;  // resolved by EventTimebase at dispatch
    out[n].source = in.source;
    out[n].channel = in.channel;
    out[n].noteId = in.noteId;   // the SAME press identity every event of this note carries
    out[n].producerSequence = in.seq;
    out[n].side = in.side;       // GH#12 task#101: the SAME side every event of this input carries
    ++n;
  };

  switch (in.kind) {
    case PerfInputKind::note_on:
      // Canonical dependency order (design/07 §3): the pitch/pressure target is
      // stated first (phase 1), then the gate latches high (phase 4) using the
      // pitch already arrived. pitch is a 1 V/oct-equivalent CV; velocity folds
      // into the original's single pressure dimension.
      push(ControlEventKind::pitch, in.pitch);
      push(ControlEventKind::pressure, in.value);
      push(ControlEventKind::gate_on, SignalSample{1});
      break;
    case PerfInputKind::note_off:
      push(ControlEventKind::gate_off, SignalSample{0});
      break;
    case PerfInputKind::aftertouch:
      // pressure/aftertouch only ever maps to pressure (no other target exists).
      push(ControlEventKind::pressure, in.value);
      break;
    case PerfInputKind::cc: {
      const ParameterId* target = resolve(in.controller);
      if (target == nullptr) return 0;  // no valid binding: drop, never fabricate
      push(ControlEventKind::parameter, in.value, *target);
      break;
    }
    case PerfInputKind::clock:
      push(ControlEventKind::clock, in.value);
      break;
  }
  return n;
}

}  // namespace lunar24::core
