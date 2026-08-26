// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// TSan-only memory-model probe for the StateSnapshotPool (task #37, GH#3 A02).
//
// This is COMPILED AND RUN ONLY under -fsanitize=thread (gated in CMakeLists.txt,
// never built/run in the normal build) — deliberately, because it has no CTest
// assertion and is meaningful only as a ThreadSanitizer detector: a correct pool
// exits 0, a pool whose guard is removed or whose memory_order is weakened exits
// non-zero (a race). Its job is not to assert a value but to make "the reader can
// read a slot the recycler resets" impossible under TSan.
//
// HOW IT DETECTS THE RACE: the reader PINS the one published snapshot, then HOLDS
// that pin across a suspend (sleep_for) while the recycler publishes a new current,
// retires the pinned slot, and drains it. Pre-fix recycleOne drains on top of the
// reader's pin, so it WRITES slots_[slot] while the reader is still about to READ
// slots_[slot].value. There is NO happens-before edge between the reader and the
// recycler except through the pool's own atomics, so TSan reports the
// read-after-write as a data race. Post-fix recycleOne CASes 0→kRetiring and
// DEFERS while the reader holds the pin, so the read stays clean.
//
// WHY ONE ROUND (and a suspend, not a free-running stress loop): a many-round
// free-running version does race, but a guard-less pool then CORRUPTS its own state
// (the reader's unpin-after-reset drives a slot's state to a negative, never-idle
// value), which makes a later pinCurrent() livelock — an intermittent hang that
// makes the probe unreliable. ONE round fires the single race deterministically and
// terminates, so the probe is a stable memory-model guard. sleep_for() and the
// volatile sink are NOT synchronizers — neither establishes a happens-before edge,
// so TSan still sees the reset as racing the read.
//
// RUN RECIPE: `TSAN_OPTIONS=abort_on_error=0:exitcode=66 ./test_state_publish_tsan`
// (pre-fix exits 66 with a data-race report, post-fix exits 0). The deterministic
// test_state_publish.cpp is the primary observable-behaviour judge; THIS probe is
// the memory-model guard that stays green only while the pin/retire ordering holds.

#include <chrono>
#include <cstdint>
#include <thread>

#include <lunar24/core/state_snapshot.h>

namespace core = lunar24::core;

namespace {
struct ProbeSnapshot {
  int value = 0;
};

// One round. The reader pins the single published snapshot and holds it across
// kPinHold; a guard-less recycler drains it in that window (→ race), a guarded
// one defers (→ clean). See the file-header comment for why a multi-round
// free-running version is deliberately NOT used (it livelocks a guard-less pool).
constexpr int kRounds = 1;
constexpr auto kPinHold = std::chrono::microseconds(50);
using Pool = core::StateSnapshotPool<ProbeSnapshot, 4>;
}  // namespace

int main() {
  Pool pool;
  {
    ProbeSnapshot* s = pool.acquire();
    if (s != nullptr) {
      s->value = 0;
      pool.publish(pool.slotOf(s));  // establish a current snapshot to pin
    }
  }

  // Reader: pin the one current snapshot, HOLD it across kPinHold, then read.
  // The only cross-thread channel is the pool's own atomics — there is no sync
  // flag, so the read is exactly the memory the recycleOne guard must protect.
  std::thread reader([&pool] {
    // A volatile sink forces the load of `.value` to actually happen (a discarded
    // pure read like `(void)pool.snapshot(cur).value` is elided by the optimizer,
    // so TSan would never see a competing access). Volatile here is NOT a sync
    // primitive — it only prevents dead-code elimination; it introduces no
    // happens-before edge, so a guard-less reset still races.
    volatile int sink = 0;
    for (int i = 0; i < kRounds; ++i) {
      std::uint32_t cur = pool.pinCurrent();
      if (cur != Pool::kNoSlot) {
        std::this_thread::sleep_for(kPinHold);  // hold the pin (not a sync edge)
        sink += pool.snapshot(cur).value;
        pool.unpin(cur);
      }
    }
  });

  // Recycler/control: acquire → write → publish → retire → drain. In this one
  // round it publishes a NEW current and retires the slot the reader just pinned;
  // the drain is where a guard-less recycleOne overwrites it (the race), while a
  // guarded recycleOne DEFERS (returns false) because the reader is still pinned.
  std::thread recycler([&pool] {
    for (int i = 0; i < kRounds; ++i) {
      ProbeSnapshot* ns = pool.acquire();
      if (ns != nullptr) {
        ns->value = i;
        std::uint32_t prior = pool.publish(pool.slotOf(ns));
        if (prior != Pool::kNoSlot) pool.retire(prior);
      }
      while (pool.recycleOne()) {}  // drain; defers while a reader is pinned
    }
  });

  reader.join();
  recycler.join();
  return 0;
}
