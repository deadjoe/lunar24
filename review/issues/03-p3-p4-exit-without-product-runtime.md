## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

P3 and P4 are marked `MET` without a product runtime that composes the implemented modules and executes the compiled patch graph.

Evidence:

- `design/00-status.md:76-83` marks P3 and P4 `MET`.
- P3 exit requires the complete non-effector chain to be playable, patchable, and produce four logical outputs (`design/06-master-plan.md:97-105`).
- `SignalPath` only accepts a caller-preassembled 10-channel sample array and composes mixer → VCF → distortion (`wet_dry.h:52-99`). It does not own sources, consume `PatchGraph`/`CompiledGraph`, dispatch `ControlEvent`s, or build the whole machine.
- `compile_graph()` has no product executor consumer under `core/` or `host/`; every call is in tests. `test_executor_realpath.cpp` implements local `std::vector` simulation functions (`:95-212`), not a runtime executor.
- The standalone host deliberately renders passthrough/silence (`host/plugin.cpp:83-96`), exposes only a blank panel, and has MIDI disabled (`host/config.h:36-47`).
- P4's three-source test explicitly calls its adapters framework-free stubs and says app-side readers are out of scope (`test_input_equivalence.cpp:29-34`).

The pieces are useful unit-tested components, but their existence does not satisfy the phase exit claims. There is currently no executable path on which the declared P3/P4 behavior can occur.

## Required correction

Change P3/P4 status from `MET` to “modules complete, integration unverified” until a framework-free machine runtime composes sources, routing/execution, state/event dispatch, keyboard behavior, and four outputs. The runtime must be programmable without P5's finished panel UI so the original phase order remains intact.

Acceptance must exercise the production runtime, not a second executor or adapter implemented only inside tests.

## Classification

Confirmed implementation/status-contract divergence; high severity. Engineering-director disposition: product runtime is required here; real host panel/MIDI adapters remain outside this correction so P3/P4 do not depend on P5.
