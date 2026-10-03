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

// Which keyboard plates MIDI is playing, for the panel: one bit per note name (bit 0 = C ..
// bit 11 = B), set while at least one MIDI note of that name is held down. Notes are counted
// per name across octaves and channels; a release clears the name the note-on lit, even if
// TRANSPOSE changed in between. The audio thread writes, the UI thread reads mask().
class MidiPlateLights {
 public:
  void on(unsigned channel, unsigned note, int semitone) {
    if (channel >= 16 || note >= 128) return;
    off(channel, note);  // a repeated note-on replaces the old one
    const auto name = static_cast<std::uint8_t>(((semitone % 12) + 12) % 12);
    name_[channel][note] = static_cast<std::uint8_t>(name + 1);
    ++count_[name];
    publish();
  }
  void off(unsigned channel, unsigned note) {
    if (channel >= 16 || note >= 128 || name_[channel][note] == 0) return;
    const unsigned name = name_[channel][note] - 1u;
    name_[channel][note] = 0;
    if (count_[name] > 0) --count_[name];
    publish();
  }
  void reset() {
    for (auto& channel : name_) for (auto& n : channel) n = 0;
    for (auto& c : count_) c = 0;
    publish();
  }
  std::uint16_t mask() const { return mask_.load(std::memory_order_relaxed); }

 private:
  void publish() {
    std::uint16_t m = 0;
    for (unsigned i = 0; i < 12; ++i)
      if (count_[i] > 0) m = static_cast<std::uint16_t>(m | (1u << i));
    mask_.store(m, std::memory_order_relaxed);
  }
  std::uint8_t name_[16][128] = {};   // 0 = not lit, else note name + 1
  std::uint16_t count_[12] = {};
  std::atomic<std::uint16_t> mask_{0};
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
