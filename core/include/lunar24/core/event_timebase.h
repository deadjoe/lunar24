// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// EventTimebase: the absolute-sample scheduler that turns external ControlEvents,
// once they have entered core, into deterministic block-relative dispatch. This is
// the sample-accurate timebase the contract distinguishes from continuous
// smoothing and audio-rate modulation (design/07 §3, §5).
//
// The event is keyed by an ABSOLUTE sample on the host audio timeline. The block
// partition is only a delivery detail: it changes the block-relative offset an
// event is handed out at, never the absolute sample it fires at. Same event set +
// same patch must yield the same sample timing under any 64/128/256 (or mixed,
// non-divisible) block sequence — that is the buffer-invariant property §5 names.

#pragma once

#include <cstdint>

#include <lunar24/core/control_event.h>

namespace lunar24::core {

// An external event as it enters core: a ControlEvent payload tagged with the
// ABSOLUTE sample it is meant to fire at. This is the canonical admission point —
// core interprets the kind+value at consume time, never pre-interpreted at a host
// boundary. ControlEvent::sampleOffset is meaningless here; it is the value
// processBlock() resolves at dispatch time.
struct TimedControlEvent {
  ControlEvent event;        // kind/value/parameter/source/producerSequence (uninterpreted)
  std::uint64_t sample = 0;  // absolute sample on the host audio timeline

  ControlLane lane() const { return event.lane(); }
};

// Bounded, no-heap pending queue (design/07 §5: no allocation on the audio
// thread). Sized for the declared max MIDI/clock burst of a single block.
inline constexpr std::uint32_t kEventTimebaseCapacity = 64;

// Same deterministic comparator the block-level scheduler uses (sampleOffset →
// phase → source → producerSequence), but keyed by absolute sample first: a later
// absolute sample is always later, regardless of how blocks are partitioned.
inline bool timed_event_before(const TimedControlEvent& a, const TimedControlEvent& b) {
  if (a.sample != b.sample) return a.sample < b.sample;
  return control_event_before(a.event, b.event);
}

// Sample-accurate event scheduler. Framework-free, fixed-capacity, no locks and
// no allocation on the audio thread. Enqueued events are held on an absolute
// timeline and released exactly when their block arrives.
class EventTimebase {
 public:
  // Insert into the sorted pending queue. Returns false (and does nothing) only
  // when the fixed queue is full — the audio thread is never blocked.
  bool enqueue(TimedControlEvent e) {
    if (pending_ >= kEventTimebaseCapacity) return false;
    std::uint32_t i = pending_;
    while (i > 0 && timed_event_before(e, queued_[i - 1])) {
      queued_[i] = queued_[i - 1];
      --i;
    }
    queued_[i] = e;
    ++pending_;
    return true;
  }

  // Process one block of `frames` starting at the accumulated absolute position.
  // Copies each due event into `out` (up to `capacity`) with its block-relative
  // offset resolved into event.sampleOffset; event.sample stays the absolute
  // sample it fired at. Returns the number handed out. Advances the accumulated
  // block start by `frames`.
  std::uint32_t processBlock(std::uint32_t frames, TimedControlEvent* out,
                             std::uint32_t capacity) {
    const std::uint64_t blockEnd = blockStart_ + frames;
    std::uint32_t n = 0;
    while (pending_ > 0 && queued_[0].sample < blockEnd) {
      if (queued_[0].sample < blockStart_) {
        // Handed in after its block was passed: a scheduling-integrity violation.
        // Never cycle on a negative offset; apply at this block's start (all-gates
        // safety direction) and keep the true absolute sample for diagnostics.
        lateSeen_ = true;
        if (n < capacity) {
          TimedControlEvent d = queued_[0];
          d.event.sampleOffset = 0;
          out[n++] = d;
        }
      } else if (n < capacity) {
        TimedControlEvent d = queued_[0];
        d.event.sampleOffset = static_cast<std::uint32_t>(queued_[0].sample - blockStart_);
        out[n++] = d;
      }
      for (std::uint32_t i = 0; i + 1 < pending_; ++i) queued_[i] = queued_[i + 1];
      --pending_;
    }
    blockStart_ += frames;
    return n;
  }

  std::uint64_t blockStart() const { return blockStart_; }
  std::uint32_t pending() const { return pending_; }

  // True only if an event was delivered whose absolute sample fell in a block
  // already passed — a scheduling-integrity violation the good paths never do.
  bool lateSeen() const { return lateSeen_; }

 private:
  TimedControlEvent queued_[kEventTimebaseCapacity];
  std::uint32_t pending_ = 0;
  std::uint64_t blockStart_ = 0;
  bool lateSeen_ = false;
};

}  // namespace lunar24::core
