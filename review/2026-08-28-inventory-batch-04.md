# Codex inventory audit — batch 04

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b` (`feat/p0-full-registry`)

Scope: production reachability, duplicate/dead execution paths, silent fallback,
test-oracle quality, and evidence-backed simplicity risks. No product code was
changed.

## Accepted findings

| ID | Severity | Finding | Disposition |
|---|---|---|---|
| A12 | High | `SynthRuntime` silently truncates a valid compiled feedback plan after 16 edges while leaving `graphValid()==true` | [#13](https://github.com/deadjoe/lunar24/issues/13) |
| A03 | High | The runtime test uses synthetic jacks/modules/fixed edges and manual role binding; no canonical whole-machine factory or host consumer exists | Add to #4 acceptance |
| A03 | Medium | `wet_dry::SignalPath`/`WetDryOutput` duplicate the composition and output record now implemented by `SynthRuntime`; only isolated tests consume the old path | Consolidate under #4, without a separate refactor project |
| A03/A09 | Medium | `device_layout::logical_signal()` and `render_device_output()` generate synthetic test signals rather than adapt `RuntimeOutput`; the real host/CI path remains untested | Add to #4/#10 acceptance |
| A06 | High | `module_contract_is_valid()` is not a `compile_graph()` admission gate; tests validate contracts separately while compilation consumes unchecked fields | Existing #7 |
| A11 | High | State/preset tests are serializer round trips and struct-level moves; none restores into runtime and proves an audible/product behavior change | Existing #12 |

## Canonical feedback reproduction

The new A12 finding was reproduced using only `registry::kJacks` and legal
one-cable cardinality. Eighteen module-local output-to-input self-loops are
possible across twelve canonical modules:

- VCO A: 2; VCO B: 3; keyboard: 2;
- Envelope A/B: 1 each; physical five-step sequencer: 1;
- classic drones 1/2/4/5: 1 each; new drones 3/6: 2 each.

All 18 connections were accepted. `compile_graph()` returned a plan with 12
cyclic regions and 18 feedback edges. `SynthRuntime::rebuild()` returned true,
`graphValid()` remained true, but `feedbackCount()` was 16. The cause is
`machine_runtime.h:547`: once `kMaxFeedback` is reached, the runtime executes
`continue` despite the adjacent claim that it never truncates silently.

An earlier synthetic reproduction with out-of-domain Jack IDs was rejected and
is not evidence. A second analysis incorrectly compared the per-module maximum
of three with the global capacity; that conclusion was also rejected. The
canonical 18/16/true reproduction above is the accepted evidence.

## Oracle judgment

- `test_machine_runtime` is a strong core-runtime oracle, but its hand-built
  graph fixture is not a canonical product factory and the standalone host can
  bypass it while every core test remains green.
- Serializer round trips detect asymmetric corruption but can preserve a shared
  encode/decode error. State exit requires restore-into-runtime assertions with
  observable parameter, patch, keyboard, identity/calibration, and sound effects.
- Host geometry and device-layout tests exercise decision helpers. They do not
  build or run the real product callback, instantiate the runtime, or route its
  four outputs to a device.
- Registry/codegen/source-scan tests are valuable structural gates, but cannot
  close sound, state-application, or host-product exits.

## Simplicity disposition

Delete or fold the superseded `SignalPath` only while making the canonical
runtime/factory path authoritative; do not start a broad refactor. Move
synthetic signal generation to test support when the real `DeviceAdapter` lands.
Do not delete setters merely because the unfinished host has no caller: many are
required future control surfaces. Conditional probes and read-only inspectors
remain legitimate test seams absent measured realtime cost.

No issue was created for repeated constants, zero-caller APIs, capacity checks
that are unreachable under the canonical registry, or test hooks. Those become
defects only with an observable contract violation, not by grep count alone.
