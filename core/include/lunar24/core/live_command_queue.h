// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// live_command_queue.h — a fixed-size, lock-free single-producer / single-consumer queue
// that carries live commands (knob moves, notes, effector program changes) from the UI or
// MIDI thread to the audio thread. No allocation, no locks.

#pragma once

#include <atomic>
#include <cstdint>

#include <lunar24/core/control_event.h>
#include <lunar24/core/id_types.h>

namespace lunar24::core {

struct LiveCommand {
  enum class Kind : std::uint8_t { Parameter, Event, Connect, Disconnect, EffectorProgram, DroneKey, SeqStep,
                                  KeyboardRight,    // parameter / value of the right bank
                                  KeyboardPreset,    // side = action (0 load, 1 save, 2 clear), index = slot
                                  KeyboardSelector };  // side 0/1, index = clock selector 0..3, value
  Kind kind = Kind::Parameter;
  ParameterId parameter = ParameterId{0};
  double value = 0.0;
  ControlEvent event{};           // Kind::Event
  std::uint32_t side = 0;         // EffectorProgram / SeqStep: 0 = left, 1 = right; DroneKey: voice 0..5
  std::uint32_t index = 0;        // SeqStep: step 0..15 (value = note, program id unused, hadOld = gate)
  ProgramId program = ProgramId{0};
  JackId source = JackId{0};      // Kind::Connect / Disconnect
  JackId sink = JackId{0};
  JackId oldSource = JackId{0};   // Kind::Connect: cable being replaced (if hadOld)
  bool hadOld = false;
};

template <std::uint32_t Capacity = 1024>
class SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  // Producer thread only. With a single producer, a free slot stays available
  // until its next push: the consumer can only free more space. This lets a
  // caller reject an edit before mutating its saved state.
  bool canPush() const noexcept {
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    return h - t < Capacity;
  }

  bool push(const LiveCommand& c) noexcept {
    const std::uint32_t h = head_.load(std::memory_order_relaxed);
    const std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= Capacity) return false;  // full: drop (the UI will send the next value)
    buf_[h & (Capacity - 1)] = c;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(LiveCommand& out) noexcept {
    const std::uint32_t t = tail_.load(std::memory_order_relaxed);
    const std::uint32_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = buf_[t & (Capacity - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  void clear() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  LiveCommand buf_[Capacity];
  std::atomic<std::uint32_t> head_{0};
  std::atomic<std::uint32_t> tail_{0};
};

}  // namespace lunar24::core
