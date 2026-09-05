// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// TSan-only memory-model PROBE for the StateSnapshotPool (task #37, GH#3 A02).
//
// COMPILED AND RUN ONLY under -fsanitize=thread (gated in CMakeLists.txt; the
// normal build never builds it) — deliberately, because it has no CTest value
// assertion and is meaningful only as a ThreadSanitizer detector. Its job is to
// make "the reader reads a slot the recycler resets" a TSan data race; it does
// not assert a numeric outcome.
//
// TWO tests are registered from THIS ONE file:
//   * test_state_publish_tsan           (no macro)  — the GUARDED pool.
//       Acceptance: exit 0, no race. A correct pool never resets a slot a reader
//       is pinned on (recycleOne CASes 0→kRetiring and defers), so the read stays
//       clean. If the guard is removed or the memory_order weakened, this goes
//       red (exit non-zero) — the regression guard.
//   * test_state_publish_tsan_selftest  (-DLUNAR24_PROBE_SELFTEST) — the SAME probe
//       with recycleOne's pin guard short-circuited in state_snapshot.h.
//       Acceptance: MUST exit 66 (a TSan data-race report). This PROVES the
//       committed probe itself is capable of failing — a detector that has never
//       been shown to fire is not a detector. The selftest CTest passes only when
//       the race fires; if the probe stops detecting (exit 0) it fails, catching a
//       dead probe. (A probe compiled WITHOUT the macro uses the guarded pool and
//       is the clean half.)
//
// HOW IT DETECTS THE RACE — free-running but BOUNDED, @Claude's required shape.
// Both threads spin `kRounds` times in a tight loop; the ONLY cross-thread channel
// is the pool's own atomics, so there is no happens-before edge between the reader
// and the recycler except through those atomics, and TSan reports competing access.
// The reader pins the current slot, reads snapshots_[cur].value, unpins. The
// recycler acquires a fresh slot, publishes it as the new current, retires the
// previous current, and drains. A guard-less recycleOne resets the retired slot by
// WRITING snapshots_[slot] — a write that races the reader's READ if the reader is
// still holding that slot (pinned before retirement). A guarded recycleOne DEFERS
// instead, so the read is never racing a reset.
//
// WHY BOUNDED, NOT a `while(true)` spin: free-running never terminates on its own,
// and a guard-less pool under heavy collision does degrade (a reader's unpin after
// a reset drives a slot's state to a negative non-idle value that is never reused),
// so an unbounded spin can livelock. Bounding to kRounds guarantees the process
// exits in all cases, while 50k rounds still give the race far more than enough
// collisions to fire. Measured: guard present exit 0 10/10; guard short-circuited
// exit 66 10/10, both without a hang.
//
// A volatile sink forces the `snapshot(cur).value` load to actually happen (a
// discarded pure read is elided, so TSan would never see a competing access).
// volatile here is NOT a synchronization primitive — it only prevents dead-code
// elimination; it introduces no happens-before edge, so a guard-less reset still
// races the read.
//
// RUN RECIPE (local TSan build): `TSAN_OPTIONS=abort_on_error=0:exitcode=66`.

#include <cstdint>
#include <cstdio>
#include <thread>

#include <lunar24/core/state_snapshot.h>

namespace core = lunar24::core;

namespace {
struct ProbeSnapshot {
  int value = 0;
};

// Bounded round count. The reader and the recycler each loop this many times; the
// process always terminates, but 50k rounds fire the race deterministically-in-
// practice (measured 10/10 under a guard-less pool). No sleep between pin and read
// is used — a suspend would force the collision every time but DRAMATICALLY shorten
// the pool's healthy lifetime (a guard-less pool degrades under that rate) and —
// worse — make the probe's "red" depend on the author choosing the right sleep, the
// exact shape @Claude rejected. Free-running + bounded is both terminable and, per
// the measurement, reliably red when the guard is gone.
constexpr int kRounds = 50000;
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

  // Reader: pin the current slot, read its value, unpin — kRounds times, no
  // suspend. The volatile sink prevents the read from being dropped.
  volatile int sink = 0;
  std::thread reader([&pool, &sink] {
    for (int i = 0; i < kRounds; ++i) {
      std::uint32_t cur = pool.pinCurrent();
      if (cur != Pool::kNoSlot) {
        sink += pool.snapshot(cur).value;
        pool.unpin(cur);
      }
    }
  });

  // Recycler/control: acquire → write → publish → retire → drain, kRounds times.
  // The drain is where a guard-less recycleOne overwrites a pinned slot (the race);
  // a guarded recycleOne DEFERS (returns false) because the reader is pinned.
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
  std::printf("probe finished, sink=%d\n", static_cast<int>(sink));
  return 0;
}
