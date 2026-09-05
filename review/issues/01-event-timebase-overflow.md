## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

`EventTimebase` silently loses control events on both overflow surfaces, including critical note/gate/clock/sync/reset edges.

- `enqueue()` returns `false` as soon as the single 64-entry queue is full (`event_timebase.h:52-63`). It neither distinguishes the critical lane nor coalesces only parameter events under pressure.
- `processBlock()` removes every due event from the queue even after the caller-provided output buffer is full (`event_timebase.h:71-93`). Due events beyond `capacity` are therefore discarded without a failure signal.
- The only diagnostic is `lateSeen_`; there is no overflow counter, authoritative-state reconcile, all-gates-off, or clock resync path.

This violates `design/07-core-contract.md:87-91`, which permits parameter coalescing only under pressure and requires critical edges to survive or trigger deterministic reconciliation. The current behavior can leave a stuck note/gate or a desynchronized clock while all tests remain green.

## Required correction

Implement and test a bounded pressure policy that:

1. preserves critical-lane events independently of parameter pressure;
2. coalesces only pending parameter targets by stable `ParameterId`;
3. reports fixed-size overflow diagnostics; and
4. performs deterministic reconcile/all-gates-off/clock resync when critical capacity is genuinely exceeded.

Add negative tests for queue-full admission and `processBlock(..., outCapacity < dueCount)` so neither path can silently discard a critical edge.

## Classification

Implementation defect; high severity; P2 exit gate regression.
