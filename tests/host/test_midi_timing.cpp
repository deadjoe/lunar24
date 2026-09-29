// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Live MIDI: arrival stamps map to jitter-free positions inside the audio block; clock events
// (MIDI clock) step the arpeggiator and close each gate half a pulse later; pitch bend moves
// the keyboard V/OCT output.

#include <memory>

#include "mini_test.h"
#include <host/midi_timing.h>
#include <lunar24/core/machine_definition.h>

using namespace lunar24;
namespace reg = lunar24::registry;

static void block_offsets() {
  const double sr = 48000.0;
  const int frames = 480;  // 10 ms
  const int now = 1000000;
  CHECK_EQ(host::midiBlockOffset(now, now, sr, frames), frames - 1);         // just arrived: end
  CHECK_EQ(host::midiBlockOffset(now - 5000, now, sr, frames), frames - 1 - 240);  // 5 ms ago
  CHECK_EQ(host::midiBlockOffset(now - 20000, now, sr, frames), 0);          // older than a block
  // Wrap-around of the 30-bit stamp keeps the right age.
  CHECK_EQ(host::midiBlockOffset(0x3FFFFFFF - 999, 1000, sr, frames), frames - 1 - 96);
  CHECK_EQ(host::midiBlockOffset(5, 5, sr, 1), 0);
}

static int gate(core::SynthRuntime& rt) {
  return rt.controlVoltageAt(reg::JackId::keyboard_gate_left_main_out) > 5.0 ? 1 : 0;
}
static void render(core::SynthRuntime& rt, int n) {
  core::RuntimeInputs z{};
  core::RuntimeOutput o;
  for (int i = 0; i < n; ++i) rt.processBlock(&z, 1, &o);
}

static void clock_events_and_bend() {
  auto def = std::make_unique<core::MachineRuntimeDefinition>(0x5EEDu, 48000.0);
  core::SynthRuntime& rt = def->runtime();
  std::uint64_t seq = 1;
  auto ev = [&](core::ControlEventKind k, double v, std::uint64_t at, core::ParameterId p = {}) {
    core::ControlEvent e{};
    e.kind = k;
    e.parameter = p;
    e.value = static_cast<core::SignalSample>(v);
    e.source = 3;
    e.noteId = 1;
    e.producerSequence = seq++;
    rt.enqueueControlEvent(core::TimedControlEvent{e, at});
  };
  ev(core::ControlEventKind::parameter, 1.0, 0, reg::ParameterId::keyboard_mode);  // arpeggiator
  ev(core::ControlEventKind::pitch, 0.5, 1);
  ev(core::ControlEventKind::gate_on, 1.0, 1);
  // Three clock pulses 6000 samples apart (a steady external clock).
  for (int k = 0; k < 3; ++k) ev(core::ControlEventKind::clock, 1.0, 100 + 6000u * k);
  render(rt, 100 + 6000 * 2 + 10);  // just after the 3rd pulse
  CHECK_EQ(gate(rt), 1);
  render(rt, 3100);                 // past half the measured period: the gate closed
  CHECK_EQ(gate(rt), 0);
  // No internal clock while an external clock is in charge: nothing re-opens by itself.
  render(rt, 48000);
  CHECK_EQ(gate(rt), 0);

  // Pitch bend: +1 semitone on the V/OCT output.
  const double before = rt.controlVoltageAt(reg::JackId::keyboard_v_oct_out);
  rt.setKeyboardBendVolts(1.0 / 12.0);
  render(rt, 1);
  CHECK(std::fabs(rt.controlVoltageAt(reg::JackId::keyboard_v_oct_out) - before - 1.0 / 12.0) < 1e-9);
}

int main() {
  block_offsets();
  clock_events_and_bend();
  return ::test::finish("midi_timing");
}
