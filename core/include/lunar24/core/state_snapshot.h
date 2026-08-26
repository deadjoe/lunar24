// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// StateSnapshotPool (design/07 §5, master plan line 93): the graph/state update
// publication path. A control thread builds an immutable snapshot and publishes
// it atomically; the audio thread reads a non-owning current-slot handle. A
// superseded (retired) snapshot is reclaimed by a worker on a NON-audio thread,
// never inside the audio callback.
//
// This is the P2-⑤ Half 1 deliverable (audit-repair ① — GH#3 A02). The defect
// being repaired: the reader could NOT pin a slot. It read `current()` then
// `snapshot(cur)` with no guarantee against a concurrent publish(B)→retire(A)→
// recycleOne() resetting A underneath it — the audio thread read a destroyed
// object. See tests/core/test_state_publish.cpp for the deterministic repro.
//
// PIN CONTRACT (the repair): the audio reader brackets its read with
// pinCurrent()/unpin(). A pinned slot is retired out of the reclaim path:
// recycleOne() DEFERS — returns false — rather than reset a slot any reader
// still holds. Only after every reader unpins does a later recycleOne() reclaim
// it. pinCurrent() and recycleOne() race on one atomic (the slot state seesaw
// between "current + N readers" and "retiring"): only ONE wins, the loser
// retries/deferts. There is no ABA because any recycle passes through kWriting
// (< 0), so a reader's CAS(expect >= 0) must fail.
//
// DELIBERATE STALE-GENERATION ACCEPTANCE (a decision, not a bug): if a reader
// loads current_=A, and A is then retired → recycled → REUSED as the new current
// before the reader's pin CAS executes, pinCurrent() succeeds (A is `current`
// again, state >= 0) and the reader reads the NEWER generation's content, not
// the snapshot it first saw. For audio that is benign — fresher state, never
// torn data — but it is an intentional semantic, not an accident: do not "fix"
// it by stalling the recycle, which would break the lock-free progress
// guarantee.
//
// RT contract:
//   * publish()/retire()/current()/pinCurrent()/unpin() are lock-free (atomics
//     only) and never allocate. recycleOne() is the worker's job, never the
//     audio thread.
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
// recycleOne() off the audio thread. The reader pins the current slot (so a
// recycle cannot reclaim it out from under a read) before it uses it.
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
      slotState_[i].store(kIdle, std::memory_order_relaxed);
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
      int expect = kIdle;
      if (slotState_[i].compare_exchange_strong(expect, kWriting,
                                                std::memory_order_acq_rel)) {
        return &snapshots_[i];
      }
    }
    return nullptr;
  }

  // Atomically publish `slot` as the current snapshot, transitioning it
  // writing→current-with-0-readers. The previously-current slot (if any) is
  // superseded and returned so the caller can retire() it for off-RT
  // reclamation. Returns kNoSlot if there was no prior current.
  std::uint32_t publish(std::uint32_t slot) {
    std::uint32_t prior = current_.exchange(slot, std::memory_order_acq_rel);
    slotState_[slot].store(0, std::memory_order_release);  // current + 0 readers
    return prior;
  }

  // --- reader (audio) thread ---
  // The current published slot index, or kNoSlot if none. INFORMATIONAL — a safe
  // concurrent read MUST go through pinCurrent()/snapshot()/unpin(), never a bare
  // `snapshot(current())`, which is exactly the GH#3 A02 the repair removes.
  std::uint32_t current() const { return current_.load(std::memory_order_acquire); }
  // Pin the current snapshot for reading: returns the pinned slot index (>= 0) or
  // kNoSlot if there is none. While a slot is pinned, recycleOne() will not
  // reclaim it. Retries on a fresh current_ if the target slot stopped being
  // current (a retire began); this never pins a slot being reclaimed.
  std::uint32_t pinCurrent() {
    for (;;) {
      std::uint32_t cur = current_.load(std::memory_order_acquire);
      if (cur == kNoSlot) return kNoSlot;
      int s = slotState_[cur].load(std::memory_order_acquire);
      if (s < 0) continue;  // writing/retiring/idle — not current; re-read current_
      // s >= 0: current with s readers. CAS s → s+1 to pin. A concurrent
      // recycle CASes 0 → kRetiring; both start from the same state on the same
      // atomic, so only one wins — if the recycle took 0 first we read a != 0 here
      // and retry; if we took it, the recycle defers.
      if (slotState_[cur].compare_exchange_strong(s, s + 1, std::memory_order_acq_rel))
        return cur;
      // CAS failed only because the slot left `current`-with-`s`-readers (a
      // recycle retired it, or another reader re-counted). Re-evaluate.
    }
  }
  // Read the snapshot in `slot`. Valid only while `slot` is pinned (pinCurrent())
  // on the concurrent path; a single-threaded caller may read the current slot.
  const Snapshot& snapshot(std::uint32_t slot) const { return snapshots_[slot]; }
  // Release a pin taken by pinCurrent(); call exactly once per successful pin.
  void unpin(std::uint32_t slot) {
    slotState_[slot].fetch_sub(1, std::memory_order_release);
  }

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
  // Recycle ONE retired slot: reset its Snapshot (the destructor + default ctor
  // run HERE, on the caller's thread — never on the audio thread). DEFERS —
  // returns false — if a reader is still pinned on the slot, so the destructor is
  // never run on a slot a reader is reading; the slot stays in the ring for a
  // later attempt. Returns true if a slot was actually recycled.
  bool recycleOne() {
    std::uint32_t t = tail_.load(std::memory_order_relaxed);
    std::uint32_t h = head_.load(std::memory_order_acquire);
    if (t >= h) return false;  // empty
    std::uint32_t slot = ring_[t % kSlots].load(std::memory_order_relaxed);
    int expect = 0;  // current + 0 readers
    if (!slotState_[slot].compare_exchange_strong(expect, kRetiring,
                                                  std::memory_order_acq_rel))
      return false;  // a reader is pinned (count>0) — defer, never reset it
    snapshots_[slot] = Snapshot{};                                  // off-RT release
    slotState_[slot].store(kIdle, std::memory_order_release);
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Test helper: how many retired slots are parked but not yet reclaimed.
  std::uint32_t pendingReclaim() const {
    return static_cast<std::uint32_t>(
        head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
  }

 private:
  // slotState_ per-slot state. Negative values are state codes; a NON-NEGATIVE
  // value means the slot is CURRENT and carries the count of readers pinned on
  // it (>= 0). Exactly one meaning holds at a time:
  //   kIdle      (-3): free, not yet acquired
  //   kWriting   (-2): a control thread is writing a fresh snapshot
  //   kRetiring  (-1): a recycle was decided; NEW readers must not pin
  //   N          (>=0): current slot with N active readers pinned
  static constexpr int kIdle = -3;
  static constexpr int kWriting = -2;
  static constexpr int kRetiring = -1;

  Snapshot snapshots_[kSlots];
  std::atomic<int> slotState_[kSlots];
  std::atomic<std::uint32_t> ring_[kSlots];
  std::atomic<std::uint32_t> current_;
  std::atomic<std::uint32_t> head_;
  std::atomic<std::uint32_t> tail_;
};

}  // namespace lunar24::core
