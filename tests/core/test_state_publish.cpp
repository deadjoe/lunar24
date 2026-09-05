// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-⑤ Half 1 tests for the snapshot publication path (design/07 §5, master plan
// line 93). The StateSnapshotPool is a fixed preallocated pool whose publish/
// retire/current primitives are lock-free and heap-free; a superseded snapshot is
// reclaimed by a worker OFF the audio thread. The RtGuard detector (tests/, never
// core/) proves the two RT invariants @Claude mandated:
//   * publish-in-RT is caught (a degraded heap-allocating publication is red);
//   * the last reference to a retired snapshot is reclaimed OFF the RT thread
//     (a reclaim inside the callback is red).

#include "mini_test.h"
#include "rt_guard_test.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include <lunar24/core/state_snapshot.h>

namespace core = lunar24::core;

namespace rt {
namespace {
// A second, test-local counter for "snapshot released in the RT window". It is
// deliberately separate from g_heap_in_rt etc.: the pool reclaims a slot by
// resetting its Snapshot, where a non-trivial destructor is the thing that must
// not run on the audio thread. This counter is the sensor for that.
std::atomic<long> g_release_in_rt{0};

inline void reset_release() { g_release_in_rt.store(0); }
}  // namespace
}  // namespace rt

// A snapshot with a non-trivial destructor that reports if it ran inside the RT
// window. This is the "the destructor must not run on the audio thread" probe.
struct TestSnapshot {
  int value = 0;
  ~TestSnapshot() {
    if (rt::g_rt_window) rt::g_release_in_rt.fetch_add(1, std::memory_order_relaxed);
  }
};

using Pool = core::StateSnapshotPool<TestSnapshot, 4>;

// --------------------------------------------------------- positive: RT-clean --

// The reader path (current()/snapshot()) and the publish primitive are lock-free
// and heap-free: driving them INSIDE the RT window must not bump any RT-forbidden
// counter. This is what makes the pool safe to use on the audio thread.
static void pool_primitives_are_rt_clean() {
  rt::reset();
  rt::reset_release();

  Pool pool;
  CHECK_EQ(pool.current(), Pool::kNoSlot);

  TestSnapshot* a = pool.acquire();
  CHECK(a != nullptr);
  a->value = 41;
  CHECK_EQ(pool.publish(0), Pool::kNoSlot);  // first publish: no prior current
  CHECK_EQ(pool.current(), 0u);

  {
    rt::RtGuard guard;  // the audio callback reads the current snapshot
    std::uint32_t cur = pool.current();
    CHECK_EQ(cur, 0u);
    CHECK_EQ(pool.snapshot(cur).value, 41);
  }

  CHECK_EQ(rt::g_heap_in_rt.load(), 0L);
  CHECK_EQ(rt::g_mutex_in_rt.load(), 0L);
  CHECK_EQ(rt::g_file_in_rt.load(), 0L);
  CHECK_EQ(rt::g_log_in_rt.load(), 0L);
}

// ------------------------------------------- negative: publish-in-RT is caught --

// A degraded publication path that constructs a fresh snapshot via heap on the
// audio thread (the snapshot/payload construction that must live on the control
// thread). The pool itself never allocates; this returns the symptom @Claude
// named — a heap-on-publication regression inside the RT window.
static void publication_in_rt_is_caught() {
  rt::reset();
  rt::reset_release();

  Pool pool;
  {
    rt::RtGuard guard;
    TestSnapshot* s = new TestSnapshot();  // naive in-callback construction
    s->value = 9;
    pool.publish(0);
    delete s;
  }

  CHECK(rt::g_heap_in_rt.load() > 0L);  // red symptom
}

// ------------------------------------- positive: last release reclaimed off RT --

// The reader retires a superseded slot to the reclaim ring and the WORKER recycles
// it on a non-audio thread: the destructor runs OFF the RT thread (counter 0).
static void last_release_is_reclaimed_off_rt() {
  rt::reset();
  rt::reset_release();

  Pool pool;
  TestSnapshot* a = pool.acquire();
  CHECK(a != nullptr);
  a->value = 1;
  std::uint32_t prior = pool.publish(0);  // first publish: no prior current
  CHECK_EQ(prior, Pool::kNoSlot);

  TestSnapshot* b = pool.acquire();
  CHECK(b != nullptr);
  b->value = 2;
  prior = pool.publish(1);
  CHECK_EQ(prior, 0u);  // slot 0 was superseded

  // The reader parks the retired slot (no destructor here, even in RT).
  CHECK(pool.retire(prior));
  CHECK_EQ(pool.pendingReclaim(), 1u);

  // The reclaim worker recycles it on a NON-audio thread — no RtGuard.
  CHECK(pool.recycleOne());
  CHECK_EQ(rt::g_release_in_rt.load(), 0L);  // destructor ran OFF the RT thread
  CHECK_EQ(pool.pendingReclaim(), 0u);
  CHECK_EQ(pool.current(), 1u);
  CHECK_EQ(pool.snapshot(pool.current()).value, 2);
}

// ----------------------------------- negative: release in the callback is caught --

// Dropping the last reference (recycling a retired slot) INSIDE the audio callback
// runs the snapshot destructor in the RT window — exactly the sneak-run the
// deferred-reclamation pattern exists to prevent.
static void last_release_in_callback_is_caught() {
  rt::reset();
  rt::reset_release();

  Pool pool;
  TestSnapshot* a = pool.acquire();
  a->value = 1;
  pool.publish(0);
  TestSnapshot* b = pool.acquire();
  b->value = 2;
  std::uint32_t prior = pool.publish(1);
  CHECK_EQ(prior, 0u);
  CHECK(pool.retire(prior));

  {
    rt::RtGuard guard;
    CHECK(pool.recycleOne());  // reclaim runs the destructor inside the callback
  }

  CHECK(rt::g_release_in_rt.load() > 0L);  // red symptom
}

// --------------------------------------------------------- GH#3 A02 (repair ①) --
//
// The defect @Claude's audit flagged: the reader could NOT pin a slot. It read
// `current()` then `snapshot(cur)` with no guarantee against a concurrent
// publish(B) → retire(A) → recycleOne() resetting A underneath it — the audio
// thread read a destroyed object. The repair gives the reader pinCurrent()/
// unpin(), and makes recycleOne() DEFER (return false) rather than reset a slot
// any reader still holds.
//
// These two tests prove it END-TO-END and deterministically (@Claude: never "跑
// 一万遍撞运气" — force the interleaving with synchronized points so EVERY run
// hits the window):
//   * The positive drives the FIXED pool and asserts a recycle over a pinned
//     reader is DEFERRED (recycleOne returns false) and the reader still reads the
//     held 41.
//   * The negative drives a faithful replica of the PRE-repair pool and asserts it
//     really has the bug (recycleOne returns true over a parked reader, the reader
//     reads the reset 0) — so the harness is sensitive, not vacuous.
//
// The sync points use acquire/release atomics: they create a happens-before edge
// so the FUNCTIONAL value assertions are deterministic (no torn read in the
// observation), which also keeps this test TSan-clean in the fixed build; the
// TSan detector's job on the pre-repair race is shown by the repro in the audit
// record (read in the reader thread vs the write in recycleOne resetting the
// snapshot the reader is parked on).

// A faithful replica of the PRE-repair pool (GH#3 A02). The reader cannot pin a
// slot and recycleOne() resets a retired slot with no guard for a parked reader.
// Byte-for-byte the publish/retire/recycleOne/current/snapshot shape of the
// header BEFORE the repair. Values are plain (non-atomic) so an unguarded reset
// is a real read/write overlap, exactly the defect.
struct LegacyPool {
  static constexpr std::uint32_t kSlots = 4;
  static constexpr std::uint32_t kNoSlot = static_cast<std::uint32_t>(-1);

  std::atomic<std::uint32_t> current_{kNoSlot};
  std::atomic<std::uint32_t> head_{0};
  std::atomic<std::uint32_t> tail_{0};
  std::atomic<std::uint32_t> ring_[kSlots]{};
  int values_[kSlots]{0, 0, 0, 0};

  std::uint32_t current() const { return current_.load(std::memory_order_acquire); }
  int snapshot(std::uint32_t slot) const { return values_[slot]; }
  std::uint32_t publish(std::uint32_t slot, int value) {
    std::uint32_t prior = current_.exchange(slot, std::memory_order_acq_rel);
    values_[slot] = value;
    return prior;
  }
  bool retire(std::uint32_t slot) {
    std::uint32_t h = head_.load(std::memory_order_relaxed);
    std::uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= kSlots) return false;
    ring_[h % kSlots].store(slot, std::memory_order_relaxed);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool recycleOne() {
    std::uint32_t t = tail_.load(std::memory_order_relaxed);
    std::uint32_t h = head_.load(std::memory_order_acquire);
    if (t >= h) return false;
    std::uint32_t slot = ring_[t % kSlots].load(std::memory_order_relaxed);
    values_[slot] = 0;  // BUG: resets a slot a reader may still be parked on
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
};

// Positive: the pin protocol DEFENDS the reader. The reader pins the old slot,
// the control supersedes+parks+recycles it — but the recycle DEFERS because the
// reader still holds the pin, so the reader reads the held 41 (never the reset 0),
// and only after the unpin is the slot reclaimed.
static void reader_pinned_slot_survives_publish_retire_recycle() {
  Pool pool;
  TestSnapshot* a = pool.acquire();
  CHECK(a != nullptr);
  a->value = 41;
  CHECK_EQ(pool.publish(0), Pool::kNoSlot);  // slot 0 is the snapshot the reader pins

  std::atomic<bool> readerPinned{false};
  std::atomic<bool> gorun{false};
  std::atomic<int> readerSlot{-1};
  std::atomic<int> readerValue{-1};

  std::thread reader([&] {
    std::uint32_t cur = pool.pinCurrent();  // pin the current slot (0)
    readerSlot.store(static_cast<int>(cur), std::memory_order_release);
    readerPinned.store(true, std::memory_order_release);
    while (!gorun.load(std::memory_order_acquire)) {}  // wait for the recycle attempt
    readerValue.store(pool.snapshot(cur).value, std::memory_order_release);
    pool.unpin(cur);
  });

  // Wait until the reader is actually pinned BEFORE we supersede+recycle. This
  // forces the exact window that used to read a reset snapshot.
  while (!readerPinned.load(std::memory_order_acquire)) {}

  TestSnapshot* b = pool.acquire();
  CHECK(b != nullptr);
  b->value = 42;
  std::uint32_t prior = pool.publish(1);  // supersede slot 0
  CHECK_EQ(prior, 0u);
  CHECK(pool.retire(prior));               // park slot 0 on the reclaim ring
  const bool recycled = pool.recycleOne(); // MUST defer: reader holds the pin on 0

  gorun.store(true, std::memory_order_release);
  reader.join();

  CHECK_EQ(readerSlot.load(std::memory_order_acquire), 0);   // pinned the superseded slot
  CHECK_EQ(readerValue.load(std::memory_order_acquire), 41); // never touched while pinned
  CHECK_FALSE(recycled);                                     // the pin blocked the recycle
  // after the reader unpinned, the same slot IS reclaimable.
  CHECK(pool.recycleOne());
  CHECK_EQ(pool.pendingReclaim(), 0u);
}

// Negative control: the PRE-repair pool really has the bug. Same interleaving,
// but the legacy reader path (current()+snapshot(), no pin) parks on the old slot
// and recycleOne() resets it underneath — recycleOne returns true and the reader
// reads the reset 0, not the held 41. This is the "will-red" proof that the
// harness above is sensitive: swap the legacy path in and it both fails
// functionally and was the exact shape TSan flagged on the pre-repair header.
static void legacy_pool_recycle_over_parked_reader_has_the_bug() {
  LegacyPool pool;
  pool.publish(0, 41);

  std::atomic<bool> readerParked{false};
  std::atomic<bool> gorun{false};
  std::atomic<int> readerValue{-1};

  std::thread reader([&] {
    std::uint32_t cur = pool.current();  // legacy: NO pin — the unsafe read path
    readerParked.store(true, std::memory_order_release);
    while (!gorun.load(std::memory_order_acquire)) {}
    readerValue.store(pool.snapshot(cur), std::memory_order_release);
  });

  while (!readerParked.load(std::memory_order_acquire)) {}
  pool.publish(1, 42);
  pool.retire(0);
  const bool recycled = pool.recycleOne();  // BUG: resets slot 0 under the parked reader
  gorun.store(true, std::memory_order_release);
  reader.join();

  CHECK(recycled);                              // the bug: recycled over a parked reader
  CHECK_EQ(readerValue.load(std::memory_order_acquire), 0);  // reader saw the reset, not 41
}

int main() {
  pool_primitives_are_rt_clean();
  publication_in_rt_is_caught();
  last_release_is_reclaimed_off_rt();
  last_release_in_callback_is_caught();
  reader_pinned_slot_survives_publish_retire_recycle();
  legacy_pool_recycle_over_parked_reader_has_the_bug();
  return ::test::finish("state_publish");
}
