// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// StateSnapshotPool (design/07 §5, master plan line 93): the graph/state update
// publication path. A control thread builds an immutable snapshot and publishes
// it atomically; the audio thread reads a non-owning current-slot handle. A
// superseded (retired) snapshot is reclaimed by a worker on a NON-audio thread,
// never inside the audio callback.
//
// This is the P2-⑤ Half 1 deliverable, a heap-free, lock-free port of the proven
// P1-② audio-thread-invariants mechanism: the RT thread must never drop the last
// reference to a retired snapshot, because a snapshot may own a non-trivial
// destructor that must not run in the callback. The detector that proves this is
// a test concern (RtGuard, tests/, never core/), so this header stays framework-
// and probe-free.
//
// RT contract:
//   * publish()/retire()/current() are lock-free (atomics only) and never allocate.
//   * A snapshot's destructor runs ONLY in recycleOne(), which is the worker's
//     job — the audio thread must never call recycleOne().
//   * No heap, no mutex, no file, no log on any path reachable in a callback.
//
// Framework-free, no heap, no locks, header-only. `Snapshot` must be
// default-constructible and assignable (the pool embeds a fixed array of them).

#pragma once

#include <atomic>
#include <cstdint>

namespace lunar24::core {

// A fixed-capacity pool of preallocated snapshot slots. The current slot is
// published atomically; the previously-current slot is handed back to the caller
// to retire(), which parks it on a bounded SPSC reclaim ring drained by
// recycleOne() off the audio thread.
template <typename Snapshot, std::uint32_t kSlots>
class StateSnapshotPool {
 public:
  // A "no slot" sentinel returned when there is no current snapshot yet.
  static constexpr std::uint32_t kNoSlot = static_cast<std::uint32_t>(-1);
  static constexpr std::uint32_t kSlotCount = kSlots;

  StateSnapshotPool() { reset(); }

  // Reset every slot to idle and clear the current/ring. NOT RT-safe; the control
  // thread rebuilds the pool before audio starts.
  void reset() {
    current_.store(kNoSlot, std::memory_order_relaxed);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kSlots; ++i) {
      slotState_[i].store(SlotState::idle, std::memory_order_relaxed);
      ring_[i].store(kNoSlot, std::memory_order_relaxed);
    }
  }

  // --- control thread ---
  // Reserve an idle slot to write a fresh snapshot into. Returns nullptr if no
  // slot is free (every slot is in flight). The returned snapshot is MUTABLE
  // until publish(). Construction of the snapshot payload happens on the control
  // thread; nothing in this path allocates or locks.
  Snapshot* acquire() {
    for (std::uint32_t i = 0; i < kSlots; ++i) {
      SlotState expect = SlotState::idle;
      if (slotState_[i].compare_exchange_strong(expect, SlotState::writing,
                                                std::memory_order_acq_rel)) {
        return &snapshots_[i];
      }
    }
    return nullptr;
  }

  // Atomically publish `slot` as the current snapshot. The previously-current
  // slot (if any) is superseded and returned so the caller can retire() it for
  // off-RT reclamation. Returns kNoSlot if there was no prior current.
  std::uint32_t publish(std::uint32_t slot) {
    std::uint32_t prior = current_.exchange(slot, std::memory_order_acq_rel);
    slotState_[slot].store(SlotState::current, std::memory_order_release);
    return prior;
  }

  // --- reader (audio) thread ---
  // The current published slot index, or kNoSlot if none. Non-owning.
  std::uint32_t current() const { return current_.load(std::memory_order_acquire); }
  // Read the snapshot in `slot` (valid only while slot == current()).
  const Snapshot& snapshot(std::uint32_t slot) const { return snapshots_[slot]; }

  // --- SPSC reclaim (producer = the thread that retires a slot) ---
  // Park a retired slot for off-audio-thread reclamation. Single producer at a
  // time; returns false if the ring is full (caller must not reset the slot
  // itself). No destructor runs here.
  bool retire(std::uint32_t slot) {
    std::uint32_t h = head_.load(std::memory_order_relaxed);
    std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= kSlots) return false;  // ring full
    ring_[h % kSlots].store(slot, std::memory_order_relaxed);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // --- SPSC reclaim (consumer = the reclaim worker) ---
  // Recycle one retired slot: reset its Snapshot (the destructor + default ctor
  // run HERE, on the caller's thread — never on the audio thread). Returns true
  // if a slot was recycled.
  bool recycleOne() {
    std::uint32_t t = tail_.load(std::memory_order_relaxed);
    std::uint32_t h = head_.load(std::memory_order_acquire);
    if (t >= h) return false;  // empty
    std::uint32_t slot = ring_[t % kSlots].load(std::memory_order_relaxed);
    snapshots_[slot] = Snapshot{};                                  // off-RT release
    slotState_[slot].store(SlotState::idle, std::memory_order_release);
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Test helper: how many retired slots are parked but not yet reclaimed.
  std::uint32_t pendingReclaim() const {
    return static_cast<std::uint32_t>(
        head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
  }

 private:
  enum class SlotState : std::uint8_t { idle, writing, current };

  Snapshot snapshots_[kSlots];
  std::atomic<SlotState> slotState_[kSlots];
  std::atomic<std::uint32_t> ring_[kSlots];
  std::atomic<std::uint32_t> current_;
  std::atomic<std::uint32_t> head_;
  std::atomic<std::uint32_t> tail_;
};

}  // namespace lunar24::core
