# A12 — Runtime silently truncates a valid compiled feedback plan

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b`

## Finding

`SynthRuntime::rebuildChainExec_()` silently discards every compiled feedback
edge after the fixed `kMaxFeedback == 16` limit and still leaves the graph valid.

At `machine_runtime.h:545-557`, the runtime iterates every cyclic region and its
feedback plan. Once `feedbackCount_` reaches 16, line 547 executes `continue`.
The comment on that line says “never truncate silently,” but the code does
exactly that. `rebuild()` does not compare the compiled plan count with runtime
capacity and sets `graphValid_ = true` before rebuilding the execution data.

This is reachable inside the canonical machine topology. A disposable
exact-head reproduction used only `registry::kJacks`, original ModuleId/jack
ownership, and legal one-cable cardinality. It connected 18 distinct
module-local output-to-input self-loops across VCO A/B, keyboard, Envelope A/B,
the physical sequencer, and all six drone modules.

Observed result:

```text
connections accepted          18 / 18
compile/rebuild                ok / true
compiled cyclic regions       12
compiled feedback edges       18
runtime feedbackCount         16
graphValid                    true
silently discarded edges      2
```

The audio executor consequently does not realize the compiled break plan it
claims to have accepted. This violates the deterministic feedback and no-silent-
truncation requirements in `design/06-master-plan.md:86-95` and
`design/07-core-contract.md:102-111,115-118`.

## Required correction

Count the complete compiled feedback plan before committing it to the runtime.
Either provision a fixed capacity proven sufficient for every canonical valid
patch and gate that relationship, or reject an oversized plan off the audio
thread with a fixed diagnostic. Never publish `graphValid()==true` with fewer
runtime feedback lines than the compiled plan.

Add the canonical 18-self-loop regression. The old behavior must fail because
`feedbackCount()!=planFeedback` while rebuild reports success; the corrected
behavior must either preserve all 18 or reject the plan deterministically.

## Classification

Confirmed implementation defect; high severity graph/runtime correctness issue.
