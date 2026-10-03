// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include <host/midi_sustain.h>

namespace lunar24::host {

// Driver -> audio SPSC queue. Closing the port advances the epoch AFTER its
// callback has stopped and BEFORE another port opens. Queued messages from the
// previous port can then never restart a note after the audio-side reset.
class MidiInputQueue {
 public:
  struct Message {
    int stamp = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
  };
  void invalidate() { epoch_.fetch_add(1, std::memory_order_acq_rel); }
  bool push(Message message) {
    const auto h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= kCapacity) {
      invalidate();  // a lost note-off makes the old stream untrustworthy
      return false;
    }
    slots_[h % kCapacity] = {message, epoch_.load(std::memory_order_acquire)};
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  template <class Reset, class Receive>
  void drain(Reset reset, Receive receive) {
    // One epoch and one bounded backlog per audio block. A switch during this
    // block takes effect at the next block, including when no messages arrive.
    const auto epoch = epoch_.load(std::memory_order_acquire);
    if (epoch != seen_) { seen_ = epoch; reset(); }
    auto t = tail_.load(std::memory_order_relaxed);
    const auto end = head_.load(std::memory_order_acquire);
    while (t != end) {
      const auto entry = slots_[t % kCapacity];
      // A switch during drain belongs to the NEXT block's reset. Leave its
      // messages queued rather than consuming a new port's first note here.
      if (epoch_.load(std::memory_order_acquire) != epoch) break;
      ++t;
      tail_.store(t, std::memory_order_release);
      if (entry.epoch == epoch) receive(entry.message);
    }
  }
 private:
  static constexpr std::uint32_t kCapacity = 1024;
  struct Entry { Message message; std::uint32_t epoch = 0; };
  Entry slots_[kCapacity];
  std::atomic<std::uint32_t> head_{0}, tail_{0}, epoch_{0};
  std::uint32_t seen_ = 0;  // audio only
};

// Track the disposition at note-on, independently of later Learn/filter edits.
class MidiNoteOwnership {
 public:
  void played(unsigned channel, unsigned note) { played_[channel][note] = true; }
  bool release(unsigned channel, unsigned note) {
    const bool wasPlayed = played_[channel][note];
    played_[channel][note] = false;
    return wasPlayed;
  }
  void reset() { for (auto& channel : played_) for (auto& note : channel) note = false; }
  // Forget every played note, calling release(channel, note) for each.
  template <class Release>
  void releaseAll(Release release) {
    for (unsigned channel = 0; channel < 16; ++channel)
      for (unsigned note = 0; note < 128; ++note)
        if (played_[channel][note]) {
          played_[channel][note] = false;
          release(channel, note);
        }
  }
 private:
  bool played_[16][128] = {};
};

// The MIDI input was closed, switched or overflowed: release exactly the notes MIDI is
// still holding (pressed, or kept by a sustain pedal). Notes played with the mouse or the
// computer keyboard, and an arpeggio they hold, are left alone. `release` receives a
// note_off PerformanceInput (source = the MIDI producer, noteId = MIDI note + 1).
template <class Release>
void release_midi_notes(MidiNoteOwnership& notes, MidiSustain& sustain, core::ControlSourceId source,
                        Release release) {
  notes.releaseAll([&](unsigned channel, unsigned note) {
    core::PerformanceInput in{};
    in.kind = core::PerfInputKind::note_off;
    in.source = source;
    in.channel = static_cast<std::uint8_t>(channel);
    in.noteId = note + 1;
    release(in);
  });
  sustain.releaseAll(source, release);
}
}  // namespace lunar24::host
