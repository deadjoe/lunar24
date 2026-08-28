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
//
// design/07 §3 separates a CONTROL-LANE (note/gate/clock/sync/reset) from a
// CONTINUOUS lane (parameter/pitch/pressure). The two lanes must never share an
// arbitrary drop policy: critical edges are never coalesced — on genuine overflow
// they raise a single reconcile failsafe (a canonical reset) rather than vanishing
// silently. This implementation keeps two fixed-capacity, no-heap sorted queues
// (one per lane) and merges them at dispatch in the global absolute order, so the
// §3 phase ordering and the §5 buffer invariance are both preserved.

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

// Bounded, no-heap pending queues (design/07 §5: no allocation on the audio
// thread). The continuous lane is sized for the declared max MIDI/CC burst of a
// single block. The critical lane has its OWN declared capacity, preserved at the
// original single-queue value of 64 (independent of continuous pressure) so
// note/clock edges are never crowded out by parameter pressure. This is the prior
// burst guarantee kept intact, NOT a value claimed to be backed by new external
// evidence. kEventDispatchCapacity is the product's per-block output buffer:
// enough for every continuous + critical event of one block plus a single
// reconcile failsafe.
inline constexpr std::uint32_t kEventTimebaseCapacity = 64;  // continuous (parameter/pitch/pressure)
inline constexpr std::uint32_t kEventCriticalCapacity = 64;  // critical (note/gate/clock/sync/reset); original 64, independent of continuous
inline constexpr std::uint32_t kEventDispatchCapacity =
    kEventTimebaseCapacity + kEventCriticalCapacity + 1;  // +1 for the reconcile reset

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
//
// Pressure policy (design/07 §3):
//   - continuous lane, within capacity: every event is kept whole, in order;
//   - continuous lane, at capacity: only `parameter` events coalesce by stable
//     ParameterId into the newest unconsumed same-target event (re-sorted); pitch/
//     pressure never coalesce; a parameter with no mergeable target returns false;
//   - critical lane: independent capacity, never dropped under continuous pressure;
//     on genuine overflow it raises a SINGLE reconcile request (counted every time)
//     instead of dropping an edge, and the next safe boundary emits a canonical
//     reset (all-gates-off / clock-resync) and deterministically clears the
//     now-unsafe critical batch;
//   - dispatch: only events actually written to `out` are removed; when the output
//     buffer is full the rest stay pending (never silently dropped).
class EventTimebase {
 public:
  // Insert into the sorted pending queue of the event's lane. Returns false (and
  // records the matching overflow diagnostic) only when that lane genuinely has no
  // room — the audio thread is never blocked.
  bool enqueue(TimedControlEvent e) {
    if (e.lane() == ControlLane::critical) {
      if (critPending_ >= kEventCriticalCapacity) {
        // Critical overflow: never silently drop an edge. Raise a single reconcile
        // request (multiple overflows merge into one request) and count each one.
        ++criticalOverflow_;
        reconcileRequested_ = true;
        return false;
      }
      critPending_ = insert_sorted(crit_, critPending_, e);
      return true;
    }
    // Continuous lane (parameter/pitch/pressure).
    if (contPending_ >= kEventTimebaseCapacity) {
      // Pressure. Only `parameter` may coalesce by stable ParameterId; pitch and
      // pressure are never coalesced, and a parameter with no mergeable same-target
      // is rejected rather than silently admitted.
      if (e.event.kind == ControlEventKind::parameter) {
        if (coalesce_parameter(e)) return true;
      }
      ++continuousOverflow_;
      return false;
    }
    contPending_ = insert_sorted(cont_, contPending_, e);
    return true;
  }

  // Process one block of `frames` starting at the accumulated absolute position.
  // Copies each due event into `out` (up to `capacity`) with its block-relative
  // offset resolved into event.sampleOffset; event.sample stays the absolute
  // sample it fired at. Returns the number handed out. Advances the accumulated
  // block start by `frames`.
  //
  // A pending reconcile (critical-overflow failsafe) is emitted FIRST at offset 0
  // when there is output space, then the unsafed critical batch is flushed; if the
  // caller gave no output space, the request stays pending until it can be
  // delivered. After that, the due continuous + critical events are merged into
  // `out` in global absolute order. Events that do not fit are LEFT PENDING and
  // counted in the dispatch-capacity diagnostic — they are never dropped.
  std::uint32_t processBlock(std::uint32_t frames, TimedControlEvent* out,
                             std::uint32_t capacity) {
    const std::uint64_t blockEnd = blockStart_ + frames;
    std::uint32_t n = 0;

    // Reconcile: an all-gates-off / clock-resync request must precede any event of
    // this block, so it is emitted at offset 0 and only when there is room. The
    // whole pending critical batch has lost trusted integrity, so it is
    // deterministically cleared (counted) rather than partially delivered.
    if (reconcileRequested_ && n < capacity) {
      TimedControlEvent r;
      r.event = ControlEvent{};
      r.event.kind = ControlEventKind::reset;
      r.event.sampleOffset = 0u;
      r.sample = blockStart_;
      out[n++] = r;
      reconcileRequested_ = false;
      ++reconcileCount_;
      criticalFlushed_ += critPending_;
      critPending_ = 0;
    }

    // Merge-deliver the two lanes in global absolute order. Only events actually
    // written to `out` are consumed; running out of output space leaves the rest
    // pending and counts them as the dispatch-capacity observation.
    std::uint32_t ci = 0, ki = 0;
    while (true) {
      const bool contDue = ci < contPending_ && cont_[ci].sample < blockEnd;
      const bool critDue = ki < critPending_ && crit_[ki].sample < blockEnd;
      if (!contDue && !critDue) break;
      const bool pickCont =
          !critDue || (contDue && timed_event_before(cont_[ci], crit_[ki]));
      TimedControlEvent& pick = pickCont ? cont_[ci] : crit_[ki];
      if (n < capacity) {
        const bool late = pick.sample < blockStart_;
        if (late) {
          lateSeen_ = true;
          ++lateCount_;
        }
        TimedControlEvent d = pick;
        d.event.sampleOffset = late ? 0u
                                    : static_cast<std::uint32_t>(pick.sample - blockStart_);
        out[n++] = d;
        if (pickCont) ++ci; else ++ki;
      } else {
        dispatchCapacity_ += count_due_(cont_, ci, contPending_, blockEnd) +
                             count_due_(crit_, ki, critPending_, blockEnd);
        break;
      }
    }

    // Compact the consumed prefix of each lane.
    if (ci > 0) {
      for (std::uint32_t i = ci; i < contPending_; ++i) cont_[i - ci] = cont_[i];
      contPending_ -= ci;
    }
    if (ki > 0) {
      for (std::uint32_t i = ki; i < critPending_; ++i) crit_[i - ki] = crit_[i];
      critPending_ -= ki;
    }

    blockStart_ += frames;
    return n;
  }

  std::uint64_t blockStart() const { return blockStart_; }
  // Total events still pending (both lanes); the drain tests use this to prove the
  // event set was fully delivered.
  std::uint32_t pending() const { return contPending_ + critPending_; }

  // True only if an event was delivered whose absolute sample fell in a block
  // already passed — a scheduling-integrity violation the good paths never do.
  bool lateSeen() const { return lateSeen_; }

  // ---- Fixed-size, no-log/no-alloc diagnostics (design/07 §3, §5) ----
  // Number of parameter events coalesced away under continuous pressure.
  std::uint32_t parameterCoalesced() const { return parameterCoalesced_; }
  // Continuous events rejected for lack of a mergeable same-target.
  std::uint32_t continuousOverflow() const { return continuousOverflow_; }
  // Critical events rejected because the critical lane was genuinely full.
  std::uint32_t criticalOverflow() const { return criticalOverflow_; }
  // Critical events deterministically cleared by a reconcile (lost trust).
  std::uint32_t criticalFlushed() const { return criticalFlushed_; }
  // Due events left pending because the caller's output buffer was full.
  std::uint32_t dispatchCapacity() const { return dispatchCapacity_; }
  // Late deliveries (absolute sample in an already-passed block).
  std::uint32_t lateCount() const { return lateCount_; }
  // Canonical reset failsafe emissions from a reconcile request.
  std::uint32_t reconcileCount() const { return reconcileCount_; }
  // True if a reconcile reset is still queued (awaiting output space).
  bool reconcilePending() const { return reconcileRequested_; }

 private:
  // Sorted insert into a fixed lane; returns the new pending count.
  static std::uint32_t insert_sorted(TimedControlEvent* q, std::uint32_t pending,
                                     TimedControlEvent e) {
    std::uint32_t i = pending;
    while (i > 0 && timed_event_before(e, q[i - 1])) {
      q[i] = q[i - 1];
      --i;
    }
    q[i] = e;
    return pending + 1;
  }

  // Under continuous pressure, coalesce the incoming parameter into the newest
  // unconsumed same-ParameterId parameter event: drop the stale same-target entries
  // and re-insert the incoming as their sole, latest representative. Returns false
  // if there is no mergeable same-target (the caller then records overflow).
  bool coalesce_parameter(TimedControlEvent e) {
    const ParameterId pid = e.event.parameter;
    std::uint32_t w = 0, removed = 0;
    for (std::uint32_t i = 0; i < contPending_; ++i) {
      if (cont_[i].event.kind == ControlEventKind::parameter &&
          cont_[i].event.parameter == pid) {
        ++removed;
      } else {
        cont_[w++] = cont_[i];
      }
    }
    if (removed == 0) return false;
    contPending_ = insert_sorted(cont_, w, e);
    parameterCoalesced_ += removed;
    return true;
  }

  static std::uint32_t count_due_(const TimedControlEvent* q, std::uint32_t from,
                                  std::uint32_t pending, std::uint64_t blockEnd) {
    std::uint32_t c = 0;
    for (std::uint32_t i = from; i < pending; ++i)
      if (q[i].sample < blockEnd) ++c;
    return c;
  }

  // Continuous lane (parameter/pitch/pressure).
  TimedControlEvent cont_[kEventTimebaseCapacity];
  std::uint32_t contPending_ = 0;
  // Critical lane (note/gate/clock/sync/reset).
  TimedControlEvent crit_[kEventCriticalCapacity];
  std::uint32_t critPending_ = 0;

  std::uint64_t blockStart_ = 0;
  bool lateSeen_ = false;

  // Pressure policy state.
  std::uint32_t parameterCoalesced_ = 0;
  std::uint32_t continuousOverflow_ = 0;
  std::uint32_t criticalOverflow_ = 0;
  std::uint32_t criticalFlushed_ = 0;
  std::uint32_t dispatchCapacity_ = 0;
  std::uint32_t lateCount_ = 0;
  std::uint32_t reconcileCount_ = 0;
  bool reconcileRequested_ = false;
};

}  // namespace lunar24::core
