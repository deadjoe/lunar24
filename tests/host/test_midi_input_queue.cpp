// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/midi_input_queue.h>
#include <host/standalone_audio_engine.h>
#include <limits>
#include <utility>
#include <vector>
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
  ownership.played(0, 0);
  ownership.played(15, 127);
  // Keyed by lane (input device * 16 + channel): 64 lanes, anything past them is ignored.
  for (unsigned channel : {0u, 15u, 16u, lunar24::host::kMidiLanes, std::numeric_limits<unsigned>::max()}) {
    for (unsigned note : {0u, 127u, 128u, std::numeric_limits<unsigned>::max()}) {
      if (channel < lunar24::host::kMidiLanes && note < 128) continue;
      ownership.played(channel, note);
      CHECK(!ownership.release(channel, note));
    }
  }
  CHECK(ownership.release(0, 0));
  CHECK(ownership.release(15, 127));
  CHECK(!ownership.release(15, 127));
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
  // MidiNoteSides: below the split note is left, from it up right; fixed at note-on.
  {
    lunar24::host::MidiNoteSides sides;
    using lunar24::core::KeyboardSide;
    CHECK(sides.noteOn(0, 59, 60) == KeyboardSide::Left);
    CHECK(sides.noteOn(0, 60, 60) == KeyboardSide::Right);
    CHECK(sides.latest(0) == KeyboardSide::Right);
    // The split moves while both are held: their releases keep the note-on sides.
    CHECK(sides.of(0, 59) == KeyboardSide::Left);
    CHECK(sides.of(0, 60) == KeyboardSide::Right);
    CHECK(sides.noteOn(1, 60, 72) == KeyboardSide::Left);  // per channel
    CHECK(sides.of(0, 60) == KeyboardSide::Right);
    CHECK(sides.latest(1) == KeyboardSide::Left);
    CHECK(sides.of(lunar24::host::kMidiLanes, 0) == KeyboardSide::Left);  // out of range: the default side
  }
  // Several input devices: each has its own lanes and its own queue, and closing one device
  // releases only its notes, pedals and plate lights.
  {
    using lunar24::host::midi_lane;
    lunar24::host::MidiNoteOwnership owned;
    lunar24::host::MidiSustain sustain;
    lunar24::host::MidiPlateLights lights;
    // The same channel 1 note 60 on device 0 and device 1 are two notes.
    owned.played(midi_lane(0, 0), 60);
    owned.played(midi_lane(1, 0), 60);
    lights.on(midi_lane(0, 0), 60, 60);
    lights.on(midi_lane(1, 0), 64, 64);
    CHECK_EQ(lights.mask(), static_cast<std::uint16_t>((1u << 0) | (1u << 4)));
    CHECK(owned.release(midi_lane(1, 0), 60));          // device 1's note-off...
    CHECK(!owned.release(midi_lane(1, 0), 60));
    owned.played(midi_lane(1, 0), 60);
    // A pedal held on device 1 holds back device 1's notes only.
    sustain.pedal(midi_lane(1, 0), true, 3, [](lunar24::core::PerformanceInput) {});
    CHECK(sustain.deferNoteOff(midi_lane(1, 0), 62));
    CHECK(!sustain.deferNoteOff(midi_lane(0, 0), 62));
    // Device 1 goes away: its note and its pedal's note are released, device 0's stays.
    std::vector<std::pair<unsigned, unsigned>> released;
    lunar24::host::release_midi_device_notes(1, owned, sustain, 3, [&](lunar24::core::PerformanceInput in) {
      CHECK(in.kind == lunar24::core::PerfInputKind::note_off);
      released.emplace_back(in.channel, in.noteId - 1);
    });
    CHECK_EQ(released.size(), 2u);
    for (const auto& r : released) CHECK_EQ(r.first, midi_lane(1, 0));
    CHECK(!sustain.deferNoteOff(midi_lane(1, 0), 62));   // its pedal was lifted
    lights.resetDevice(1);
    CHECK_EQ(lights.mask(), static_cast<std::uint16_t>(1u << 0));  // device 0's C still lit
    CHECK(owned.release(midi_lane(0, 0), 60));          // device 0's note was untouched
    // Out-of-range devices are ignored.
    lunar24::host::release_midi_device_notes(lunar24::host::kMidiInputDevices, owned, sustain, 3,
                                             [&](lunar24::core::PerformanceInput) { CHECK(false); });

    // One queue per device: invalidating device 2 resetDevices device 2 only, and its stale
    // messages are dropped while the other devices' arrive.
    lunar24::host::MidiInputQueues queues;
    CHECK(queues.push(0, {1, 0x90, 60, 100}));
    CHECK(queues.push(2, {2, 0x90, 61, 100}));
    CHECK(!queues.push(lunar24::host::kMidiInputDevices, {3, 0x90, 62, 100}));
    queues.invalidate(2);
    CHECK(queues.push(2, {4, 0x90, 63, 100}));
    std::vector<unsigned> resetDevices;
    std::vector<std::pair<unsigned, int>> got;
    queues.drain([&](unsigned d) { resetDevices.push_back(d); },
                 [&](unsigned d, lunar24::host::MidiInputQueue::Message m) { got.emplace_back(d, m.data1); });
    CHECK(resetDevices == std::vector<unsigned>{2u});
    CHECK(got == (std::vector<std::pair<unsigned, int>>{{0u, 60}, {2u, 63}}));
    resetDevices.clear();
    queues.invalidateAll();
    queues.drain([&](unsigned d) { resetDevices.push_back(d); }, [&](unsigned, lunar24::host::MidiInputQueue::Message) {});
    CHECK_EQ(resetDevices.size(), static_cast<std::size_t>(lunar24::host::kMidiInputDevices));
  }
  return test::finish("test_midi_input_queue");
}
