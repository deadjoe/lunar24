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

int main() {
  pool_primitives_are_rt_clean();
  publication_in_rt_is_caught();
  last_release_is_reclaimed_off_rt();
  last_release_in_callback_is_caught();
  return ::test::finish("state_publish");
}
