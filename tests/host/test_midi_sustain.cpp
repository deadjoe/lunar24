// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/midi_sustain.h>
#include <host/standalone_audio_engine.h>

using namespace lunar24;

int main() {
  // Exercise the production sustain helper, translator and audio runtime together.
  for (unsigned channel = 0; channel < 16; ++channel) {
    host::MidiSustain sustain;
    host::StandaloneAudioEngine engine;
    CHECK(engine.prepare(1, 48000, 64, 0, 2));
    core::InputStateMachine input{nullptr, 0};
    std::uint64_t sequence = 0;
    auto send = [&](core::PerformanceInput in) {
      in.seq = ++sequence;
      core::ControlEvent events[3];
      const auto count = input.translate(in, events, 3);
      CHECK(count > 0);
      for (unsigned i = 0; i < count; ++i) CHECK(engine.enqueueEventFromAudioThread(events[i]));
    };
    auto gate = [&]() {
      double left[64]{}, right[64]{};
      double* out[] = {left, right};
      CHECK(engine.processBlock(nullptr, out, 0, 2, 64) == host::StandaloneAudioEngine::Status::Rendered);
      return engine.runtime()->controlVoltageAt(core::JackId::keyboard_gate_left_main_out) > 1.0;
    };
    core::PerformanceInput note{};
    note.kind = core::PerfInputKind::note_on;
    note.source = 3;
    note.channel = static_cast<std::uint8_t>(channel);
    note.noteId = 61;
    note.value = 0.8f;
    sustain.noteOn(channel, 60);
    send(note);
    CHECK(gate());
    sustain.pedal(channel, true, 3, send);
    CHECK(sustain.deferNoteOff(channel, 60));
    CHECK(gate());
    sustain.pedal(channel, false, 3, send);
    CHECK(!gate());
  }
  // Same note on two channels: releasing one pedal leaves the other held.
  host::MidiSustain sustain;
  unsigned releases = 0;
  auto release = [&](core::PerformanceInput in) {
    CHECK(in.kind == core::PerfInputKind::note_off);
    CHECK_EQ(in.source, 3u);
    CHECK_EQ(in.noteId, 61u);
    CHECK_EQ(in.channel, releases == 0 ? 1u : 9u);
    ++releases;
  };
  sustain.pedal(1, true, 3, release);
  sustain.pedal(9, true, 3, release);
  CHECK(sustain.deferNoteOff(1, 60));
  CHECK(sustain.deferNoteOff(9, 60));
  CHECK(!sustain.deferNoteOff(0, 60));
  sustain.pedal(1, false, 3, release);
  CHECK_EQ(releases, 1u);
  sustain.pedal(1, false, 3, release);
  CHECK_EQ(releases, 1u);
  sustain.pedal(9, false, 3, release);
  CHECK_EQ(releases, 2u);
  // Re-pressing a sustained note keeps it down when the pedal is released.
  sustain.pedal(1, true, 3, release);
  CHECK(sustain.deferNoteOff(1, 60));
  sustain.noteOn(1, 60);
  sustain.pedal(1, false, 3, release);
  CHECK_EQ(releases, 2u);
  CHECK(!sustain.deferNoteOff(1, 60));
  // reset() (the audio-stream boundary) clears pedal and deferred-note state: a pedal
  // that was down when the stream was replaced neither releases phantom notes nor
  // defers the next stream's note-offs.
  {
    host::MidiSustain s2;
    unsigned n = 0;
    auto count = [&](core::PerformanceInput) { ++n; };
    s2.noteOn(2, 60);
    s2.pedal(2, true, 3, count);
    CHECK(s2.deferNoteOff(2, 60));
    s2.reset();
    s2.pedal(2, false, 3, count);              // pedal was never down after the reset
    CHECK_EQ(n, 0u);
    CHECK(!s2.deferNoteOff(2, 60));            // nothing is deferred any more
    s2.noteOn(2, 61);
    CHECK(!s2.deferNoteOff(2, 61));            // a fresh note releases normally
  }
  return test::finish("test_midi_sustain");
}
