## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

`GraphCompiler` can grant a feedback edge `real_path` delay while another zero-delay path leaves the same cycle algebraic.

- For a DFS back edge, the compiler chooses the input port from the single DFS `parentEdge[v]` and calls `decide_feedback_delay()` once (`graph_compiler.h:351-378`).
- It does not inspect every input→output path that can participate in the selected cycle.
- Reproduction: module B has `slow_in→out = 3 samples` and `direct_in→out = 0/direct`; module A feeds both B inputs and B feeds A. The compiler reports `real_path 3` for the B→A feedback edge, although the A→B-direct→A cycle still has zero minimum delay.

This violates `design/07-core-contract.md:104-108`: a real delay may replace `z⁻¹` only when the specific cycle's minimum reachable delay is positive; any reachable direct branch makes that minimum zero. Existing graph tests use simple one-input paths and do not cover this multipath topology.

`compile_graph()` also accepts supplied contracts without invoking `module_contract_is_valid()`. The validator's documented prepare-time rejection (`module_execution_contract.h:86-104`) is therefore not enforced by the compiler API.

## Required correction

Compute feedback delay eligibility over every reachable input→output path for each selected cycle (or conservatively choose `z⁻¹` when path identity is ambiguous), and reject invalid/missing contracts before compiling a cyclic region. Add a regression topology with one delayed and one direct branch feeding the same module; the selected feedback edge must remain `z⁻¹`.

## Classification

Implementation defect; critical graph-causality issue; P2 exit gate regression.
