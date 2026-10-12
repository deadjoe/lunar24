// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include <host/midi_sustain.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/midi_map.h>

namespace lunar24::host {

// MIDI from several input devices at once (the app; a plugin's DAW is one input). Each
// device's 16 channels are a "lane" of their own (device * 16 + channel), so two devices
// playing the same note on the same channel never touch each other's notes, pedals or
// lights. Lanes 0-15 are device 0: with one input everything is as it always was.
inline constexpr unsigned kMidiInputDevices = core::kMidiMaxInputDevices;
inline constexpr unsigned kMidiLanes = kMidiInputDevices * 16;
constexpr unsigned midi_lane(unsigned device, unsigned channel) { return device * 16u + channel; }
constexpr unsigned midi_lane_device(unsigned lane) { return lane / 16u; }
constexpr unsigned midi_lane_channel(unsigned lane) { return lane % 16u; }
static_assert(kMidiSustainLanes == kMidiLanes, "the sustain pedal ledger has a lane per device and channel");

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
      overflows_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[h % kCapacity] = {message, epoch_.load(std::memory_order_acquire)};
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // Messages lost to a full queue (each one also reset the input's held notes). Any thread.
  std::uint32_t overflows() const { return overflows_.load(std::memory_order_relaxed); }
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
  std::atomic<std::uint32_t> head_{0}, tail_{0}, epoch_{0}, overflows_{0};
  std::uint32_t seen_ = 0;  // audio only
};

// One MidiInputQueue per input device: each device's driver callback is that queue's only
// producer, and closing one device resets only its own notes. drain() takes the devices in
// turn; the engine orders events by time, so their order across devices does not matter.
class MidiInputQueues {
 public:
  bool push(unsigned device, MidiInputQueue::Message message) {
    return device < kMidiInputDevices && queues_[device].push(message);
  }
  void invalidate(unsigned device) {
    if (device < kMidiInputDevices) queues_[device].invalidate();
  }
  void invalidateAll() { for (auto& q : queues_) q.invalidate(); }
  std::uint32_t overflows() const {
    std::uint32_t n = 0;
    for (const auto& q : queues_) n += q.overflows();
    return n;
  }
  // reset(device) runs when that device's input was closed, switched or overflowed;
  // receive(device, message) for each of its messages.
  template <class Reset, class Receive>
  void drain(Reset reset, Receive receive) {
    for (unsigned d = 0; d < kMidiInputDevices; ++d)
      queues_[d].drain([&] { reset(d); }, [&](MidiInputQueue::Message m) { receive(d, m); });
  }

 private:
  MidiInputQueue queues_[kMidiInputDevices];
};

// Track the disposition at note-on, independently of later Learn/filter edits.
// Keyed by lane (device * 16 + channel).
class MidiNoteOwnership {
 public:
  void played(unsigned lane, unsigned note) {
    if (lane < kMidiLanes && note < 128) played_[lane][note] = true;
  }
  bool release(unsigned lane, unsigned note) {
    if (lane >= kMidiLanes || note >= 128) return false;
    const bool wasPlayed = played_[lane][note];
    played_[lane][note] = false;
    return wasPlayed;
  }
  void reset() { for (auto& lane : played_) for (auto& note : lane) note = false; }
  // Forget every played note, calling release(lane, note) for each.
  template <class Release>
  void releaseAll(Release release) {
    for (unsigned lane = 0; lane < kMidiLanes; ++lane) releaseLane_(lane, release);
  }
  // Forget the notes one input device played (its lanes only).
  template <class Release>
  void releaseDevice(unsigned device, Release release) {
    if (device >= kMidiInputDevices) return;
    for (unsigned ch = 0; ch < 16; ++ch) releaseLane_(midi_lane(device, ch), release);
  }
 private:
  template <class Release>
  void releaseLane_(unsigned lane, Release& release) {
    for (unsigned note = 0; note < 128; ++note)
      if (played_[lane][note]) {
        played_[lane][note] = false;
        release(lane, note);
      }
  }
  bool played_[kMidiLanes][128] = {};
};

// Which keyboard side (PLAY = TWIN / SPLIT) each MIDI note plays: notes below the split
// note go left, the rest right. The side is fixed at note-on, so the note-off, a pedal
// release and poly aftertouch reach the side holding the note even if the split note
// changed meanwhile. Channel aftertouch follows the channel's latest note. Audio thread.
// Keyed by lane (device * 16 + channel).
class MidiNoteSides {
 public:
  core::KeyboardSide noteOn(unsigned lane, unsigned note, int splitNote) {
    const bool right = static_cast<int>(note) >= splitNote;
    if (lane < kMidiLanes && note < 128) {
      right_[lane][note] = right;
      lastRight_[lane] = right;
    }
    return right ? core::KeyboardSide::Right : core::KeyboardSide::Left;
  }
  core::KeyboardSide of(unsigned lane, unsigned note) const {
    return lane < kMidiLanes && note < 128 && right_[lane][note] ? core::KeyboardSide::Right
                                                                 : core::KeyboardSide::Left;
  }
  core::KeyboardSide latest(unsigned lane) const {
    return lane < kMidiLanes && lastRight_[lane] ? core::KeyboardSide::Right : core::KeyboardSide::Left;
  }

 private:
  bool right_[kMidiLanes][128] = {};
  bool lastRight_[kMidiLanes] = {};
};

// Which keyboard plates MIDI is playing, for the panel: one bit per note name (bit 0 = C ..
// bit 11 = B), set while at least one MIDI note of that name is held down. Notes are counted
// per name across octaves, channels and devices; a release clears the name the note-on lit, even if
// TRANSPOSE changed in between. The audio thread writes, the UI thread reads mask().
class MidiPlateLights {
 public:
  void on(unsigned lane, unsigned note, int semitone) {
    if (lane >= kMidiLanes || note >= 128) return;
    off(lane, note);  // a repeated note-on replaces the old one
    const auto name = static_cast<std::uint8_t>(((semitone % 12) + 12) % 12);
    name_[lane][note] = static_cast<std::uint8_t>(name + 1);
    ++count_[name];
    publish();
  }
  void off(unsigned lane, unsigned note) {
    if (lane >= kMidiLanes || note >= 128 || name_[lane][note] == 0) return;
    const unsigned name = name_[lane][note] - 1u;
    name_[lane][note] = 0;
    if (count_[name] > 0) --count_[name];
    publish();
  }
  void reset() {
    for (auto& lane : name_) for (auto& n : lane) n = 0;
    for (auto& c : count_) c = 0;
    publish();
  }
  // One input device went away: put out only the plates its notes lit.
  void resetDevice(unsigned device) {
    if (device >= kMidiInputDevices) return;
    for (unsigned ch = 0; ch < 16; ++ch)
      for (unsigned note = 0; note < 128; ++note) off(midi_lane(device, ch), note);
  }
  std::uint16_t mask() const { return mask_.load(std::memory_order_relaxed); }

 private:
  void publish() {
    std::uint16_t m = 0;
    for (unsigned i = 0; i < 12; ++i)
      if (count_[i] > 0) m = static_cast<std::uint16_t>(m | (1u << i));
    mask_.store(m, std::memory_order_relaxed);
  }
  std::uint8_t name_[kMidiLanes][128] = {};   // by lane; 0 = not lit, else note name + 1
  std::uint16_t count_[12] = {};
  std::atomic<std::uint16_t> mask_{0};
};

// The MIDI input was closed, switched or overflowed: release exactly the notes MIDI is
// still holding (pressed, or kept by a sustain pedal). Notes played with the mouse or the
// computer keyboard, and an arpeggio they hold, are left alone. `release` receives a
// note_off PerformanceInput (source = the MIDI producer, channel = the lane, noteId = MIDI
// note + 1).
template <class Release>
void release_midi_notes(MidiNoteOwnership& notes, MidiSustain& sustain, core::ControlSourceId source,
                        Release release) {
  notes.releaseAll([&](unsigned lane, unsigned note) {
    core::PerformanceInput in{};
    in.kind = core::PerfInputKind::note_off;
    in.source = source;
    in.channel = static_cast<std::uint8_t>(lane);
    in.noteId = note + 1;
    release(in);
  });
  sustain.releaseAll(source, release);
}
// The same for one input device only: the other devices' notes and pedals carry on.
template <class Release>
void release_midi_device_notes(unsigned device, MidiNoteOwnership& notes, MidiSustain& sustain,
                               core::ControlSourceId source, Release release) {
  notes.releaseDevice(device, [&](unsigned lane, unsigned note) {
    core::PerformanceInput in{};
    in.kind = core::PerfInputKind::note_off;
    in.source = source;
    in.channel = static_cast<std::uint8_t>(lane);
    in.noteId = note + 1;
    release(in);
  });
  sustain.releaseDevice(device, source, release);
}
}  // namespace lunar24::host
