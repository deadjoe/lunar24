// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

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
 private:
  bool played_[16][128] = {};
};
}  // namespace lunar24::host
