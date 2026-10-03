// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/midi_input_queue.h>
#include <host/standalone_audio_engine.h>
#include <memory>
#include <thread>

int main() {
  using lunar24::host::MidiInputQueue;
  MidiInputQueue queue;
  int resets = 0, notes = 0;
  auto reset = [&] { ++resets; notes = 0; };
  auto receive = [&](MidiInputQueue::Message) { ++notes; };
  CHECK(queue.push({0, 0x90, 60, 100}));
  queue.invalidate();  // close before the queued note-on reaches audio
  queue.drain(reset, receive);
  CHECK_EQ(resets, 1);
  CHECK_EQ(notes, 0);
  CHECK(queue.push({0, 0x90, 62, 100}));
  queue.drain(reset, receive);
  CHECK_EQ(notes, 1);
  queue.invalidate();  // closing an idle port still releases its playing notes
  queue.drain(reset, receive);
  CHECK_EQ(resets, 2);
  CHECK_EQ(notes, 0);
  CHECK(queue.push({0, 0x90, 60, 100}));
  queue.invalidate();
  CHECK(queue.push({0, 0x90, 64, 100}));  // a newly opened port can play immediately
  queue.drain(reset, receive);
  CHECK_EQ(resets, 3);
  CHECK_EQ(notes, 1);
  for (int i = 0; i < 1024; ++i) CHECK(queue.push({0, 0x90, 60, 100}));
  CHECK(!queue.push({0, 0x80, 60, 0}));  // losing note-off invalidates the backlog
  queue.drain(reset, receive);
  CHECK_EQ(resets, 4);
  CHECK_EQ(notes, 0);

  // A switch while draining must preserve the new port's first message.
  CHECK(queue.push({0, 0x90, 60, 100}));
  CHECK(queue.push({0, 0x90, 61, 100}));
  queue.drain(reset, [&](MidiInputQueue::Message) {
    queue.invalidate();
    CHECK(queue.push({0, 0x90, 64, 100}));
  });
  int lastNote = 0;
  queue.drain(reset, [&](MidiInputQueue::Message m) { lastNote = m.data1; });
  CHECK_EQ(lastNote, 64);

  // Real driver/audio overlap; also exercise this target under ThreadSanitizer.
  MidiInputQueue concurrent;
  std::atomic<bool> finished{false};
  std::thread driver([&] {
    for (int i = 1; i <= 20000; ++i) (void)concurrent.push({i, 0x90, 60, 100});
    finished.store(true, std::memory_order_release);
  });
  int lastStamp = 0;
  auto consume = [&](MidiInputQueue::Message m) {
    CHECK(m.stamp > lastStamp);
    lastStamp = m.stamp;
  };
  while (!finished.load(std::memory_order_acquire)) concurrent.drain([] {}, consume);
  driver.join();
  concurrent.drain([] {}, consume);

  lunar24::host::MidiNoteOwnership ownership;
  ownership.played(0, 60);
  // A Learn/filter edit does not change the accepted note's ownership.
  CHECK(ownership.release(0, 60));
  CHECK(!ownership.release(0, 60));
  CHECK(!ownership.release(1, 60));  // a pad / filtered note never owned a played note
  ownership.played(1, 60);
  ownership.reset();
  CHECK(!ownership.release(1, 60));
  // Closing the MIDI input releases only what MIDI holds: a pressed note and a note kept
  // by the sustain pedal stop, a note held with the mouse keeps sounding at its own pitch.
  {
    namespace core = lunar24::core;
    auto engine = std::make_unique<lunar24::host::StandaloneAudioEngine>();
    CHECK(engine->prepare(1, 48000.0, 64, 0, 2));
    core::InputStateMachine input{nullptr, 0};
    std::uint64_t seq = 0;
    auto send = [&](core::PerformanceInput in) {
      in.seq = ++seq;
      core::ControlEvent events[3];
      const auto n = input.translate(in, events, 3);
      for (unsigned i = 0; i < n; ++i) CHECK(engine->enqueueEventFromAudioThread(events[i]));
    };
    auto play = [&](core::ControlSourceId source, core::NoteId id, double semitones) {
      core::PerformanceInput in{};
      in.kind = core::PerfInputKind::note_on;
      in.source = source;
      in.noteId = id;
      in.pitch = static_cast<core::SignalSample>(semitones / 12.0);
      in.value = 0.8f;
      send(in);
    };
    auto render = [&] {
      double l[64]{}, r[64]{};
      double* out[] = {l, r};
      for (int i = 0; i < 8; ++i) engine->processBlock(nullptr, out, 0, 2, 64);
    };
    const core::JackId gate = core::JackId::keyboard_gate_left_main_out;
    const core::JackId vOct = core::JackId::keyboard_v_oct_out;
    lunar24::host::MidiNoteOwnership held;
    lunar24::host::MidiSustain sustain;
    play(1, 1000, 0.0);  // the mouse holds A
    render();
    // MIDI: note 64 held down; note 67 released under the pedal (the pedal keeps it).
    held.played(0, 64);
    sustain.noteOn(0, 64);
    play(3, 65, 7.0);
    sustain.pedal(0, true, 3, send);
    held.played(0, 67);
    sustain.noteOn(0, 67);
    play(3, 68, 10.0);
    CHECK(held.release(0, 67));
    CHECK(sustain.deferNoteOff(0, 67));
    render();
    CHECK(engine->runtime()->controlVoltageAt(gate) > 1.0);
    CHECK(std::fabs(engine->runtime()->controlVoltageAt(vOct) * 12.0 - 10.0) < 1e-3);
    int released = 0;
    lunar24::host::release_midi_notes(held, sustain, 3, [&](core::PerformanceInput in) {
      CHECK(in.kind == core::PerfInputKind::note_off && in.source == 3);
      ++released;
      send(in);
    });
    CHECK_EQ(released, 2);
    render();
    CHECK(engine->runtime()->controlVoltageAt(gate) > 1.0);  // the mouse note still sounds
    CHECK(std::fabs(engine->runtime()->controlVoltageAt(vOct) * 12.0) < 1e-3);  // at A
    CHECK(!held.release(0, 64));            // nothing left to release
    CHECK(!sustain.deferNoteOff(0, 70));     // the pedal is up again
  }
  // Plate lights: by note name across octaves and channels; a release clears the name its
  // note-on lit even when TRANSPOSE moved in between; reset clears everything.
  {
    lunar24::host::MidiPlateLights lights;
    CHECK_EQ(lights.mask(), 0u);
    lights.on(0, 60, 60);              // C4 -> C
    lights.on(9, 40, 40);              // pad E on channel 10 -> E
    CHECK_EQ(lights.mask(), (1u << 0) | (1u << 4));
    lights.on(0, 72, 72);              // another C, one octave up
    lights.off(0, 60);
    CHECK_EQ(lights.mask(), (1u << 0) | (1u << 4));  // C still held by 72
    lights.off(0, 72);
    CHECK_EQ(lights.mask(), 1u << 4);
    lights.on(0, 61, 61 - 13);         // TRANSPOSE -13 st: C#4 sounds as C3 -> lights C
    CHECK_EQ(lights.mask(), (1u << 0) | (1u << 4));
    lights.off(0, 61);                 // released after TRANSPOSE changed: still clears C
    CHECK_EQ(lights.mask(), 1u << 4);
    lights.off(5, 50);                 // a note that never lit: no change
    lights.on(9, 40, 40);              // the same note again without a release: counted once
    lights.off(9, 40);
    CHECK_EQ(lights.mask(), 0u);
    lights.on(3, 69, 69);
    lights.reset();
    CHECK_EQ(lights.mask(), 0u);
  }
  return test::finish("test_midi_input_queue");
}
