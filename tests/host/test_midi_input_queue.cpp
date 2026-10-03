// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/midi_input_queue.h>

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

  lunar24::host::MidiNoteOwnership ownership;
  ownership.played(0, 60);
  // A Learn/filter edit does not change the accepted note's ownership.
  CHECK(ownership.release(0, 60));
  CHECK(!ownership.release(0, 60));
  CHECK(!ownership.release(1, 60));  // a pad / filtered note never owned a played note
  ownership.played(1, 60);
  ownership.reset();
  CHECK(!ownership.release(1, 60));
  return test::finish("test_midi_input_queue");
}
