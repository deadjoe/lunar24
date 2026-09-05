## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

`StateSnapshotPool` can recycle a retired slot while the audio thread still reads that same snapshot.

The audio path performs two non-owning operations: `current()` followed by `snapshot(slot)` (`state_snapshot.h:83-87`). A control thread may then `publish()` a replacement, immediately `retire(prior)`, and a worker may run `recycleOne()`, which assigns `Snapshot{}` into the old slot (`state_snapshot.h:74-80,89-114`). There is no reader epoch, hazard/lease, quiescent-state acknowledgement, generation check, or triple-buffer grace period.

Interleaving:

1. audio loads slot A from `current()`;
2. control publishes B and retires A;
3. worker resets A in `recycleOne()`;
4. audio reads or continues reading A.

That is a concurrent read/write data race and can expose a reset or partially destroyed snapshot to the callback. `test_state_publish.cpp` is sequential and verifies only allocation/destructor thread placement (`:45-163`); it never holds a reader while publish/reclaim proceeds.

This violates the safe-quiescence requirement in `design/07-core-contract.md:113-118`.

## Required correction

Use an explicit safe-reclamation protocol (reader epoch/RCU, hazard/lease acknowledgement, or a proven triple-buffer handoff) and add a deterministic concurrent test that blocks a reader on the prior slot while publish+retire+reclaim attempt to advance. Reclamation must remain off the audio thread and must not mutate a slot until no audio reader can reference it.

## Classification

Implementation defect; critical realtime correctness issue; P2 exit gate regression.
