# Codex inventory audit — batch 02

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b` (`feat/p0-full-registry`)

Scope: exact-head revalidation of existing GitHub issues #2–#8 against the
normative design contract, current production paths, behavior tests, and narrow
negative controls. No product code was changed.

## Disposition

| Issue | Classification | Exact-head evidence | Action |
|---|---|---|---|
| #2 | **OPEN** | `EventTimebase::enqueue()` still rejects every event when the single queue is full. There is no critical lane, parameter coalescing, fixed diagnostic, or authoritative reconcile. This contradicts `design/07-core-contract.md:87-91`. | Keep open; repair the queue contract and add overflow/reconcile behavior tests. Do not weaken the contract to permit arbitrary critical-edge loss. |
| #3 | **FIXED** | `StateSnapshotPool::pinCurrent()`/`unpin()` now lease slots and `recycleOne()` first transitions an unpinned slot to `kRetiring`. The production behavior test holds a reader while publish/retire/recycle advances. A disposable mutation that removed the pin deferral made three assertions fail; restoring it returned 38/38 green. | Close with the exact-head evidence. |
| #4 | **PARTIAL** | The original statement that no composed product runtime exists is no longer true: `SynthRuntime::rebuild()` calls `compile_graph()` and the render path consumes its plan. A mutation that bypassed compilation made six runtime assertions fail. However, the runtime consumes only the current fixed audio roles and dispatches only eight `ParameterId`s; it consumes no keyboard subsystem, preset behavior, or persisted unit/calibration state. The test also uses a hand-built descriptor/role fixture rather than instantiating the canonical generated whole-machine registry. | Keep open and narrow the remaining acceptance to a canonical, complete product runtime rather than repeating the superseded “no runtime” claim. |
| #5 | **PARTIAL** | The clean-sine/identity-nonlinearity and unbound TUNE/MUTE/MOD/VOLT/mutual-FM subclaims are repaired: `DroneBank` now renders saw oscillators with a non-identity cubic transfer and is consumed by `SynthRuntime`. Replacing the transfer with identity made three nonlinearity assertions fail. But the original issue also requires classic-voice gate/HOLD/ATT/RLS/CV/envelope-out behavior and the `design/07` dynamic model includes small noise, oscillator-specific variation, and a correlatable environmental term; these remain absent. | Keep open; update the issue to distinguish the repaired oscillator/control/nonlinearity subset from the missing envelope/gate/output/dynamic subset. |
| #6 | **OPEN** | The VCF remains a linear Chamberlin-style update. Persisted `UnitIdentitySeed`/calibration data has no audio-path consumer, and left/right VCF calibration/nonlinear state is absent. This contradicts `design/07-core-contract.md:152-158`. | Keep open; add product-path identity/calibration and level-dependent behavior with tests, without inventing measured constants. |
| #7 | **OPEN** | Feedback-delay selection still follows one recorded parent edge/input path and `compile_graph()` does not gate compilation through `module_contract_is_valid()`. A parallel reachable direct path can therefore leave an algebraic cycle while another branch grants real-delay credit, contrary to `design/07-core-contract.md:102-110`. | Keep open; add the parallel-direct-path regression before repairing the compiler. |
| #8 | **OPEN** | The event and performance input records still carry no note/touch identity; note-on can be partially enqueued; reset is ignored by keyboard behavior; partial note-off disables modulation state; and portamento reads `current()` without advancing `tick()`. The canonical phase order remains authoritative. | Keep open; preserve note/touch identity through the common input pipeline and add deterministic overlap/release/reset/portamento replay tests. |

## Runtime consumption boundary for #4

`SynthRuntime` currently consumes VCO A/B, preamp, classic `DroneBank`, new
Papa voices 3/6, envelope follower, ten-channel mixer, Polivoks filter,
distortion, `EventTimebase`, and parameter `ControlEvent`s. Its parameter event
switch handles only drone 3/6 pitch, noise, FM, and AM. It does not consume the
keyboard behavior/state machine or presets, persisted identity/calibration, LFO
A/B, Envelope A/B, joystick, or the distinct physical five-step sequencer.

The result is a real but incomplete composed runtime. Class existence, registry
presence, isolated tests, and actual runtime consumption must continue to be
reported separately.

## Systemic conclusions

1. Local component coverage is substantially stronger than canonical product-path coverage; the latter is the governing exit criterion.
2. Only #3 has production repair, positive behavior coverage, and an independently exercised old-error negative control for the whole issue.
3. #5's negative control proves one repaired subclaim, not the envelope/gate/output and dynamic-model requirements bundled into the original issue.
4. #2, #6, #7, and #8 are contract violations, not invitations to weaken the design in lieu of implementation evidence.
5. The incomplete runtime map exposes a separate design-to-phase planning gap for LFO A/B, Envelope A/B, joystick, and the physical five-step sequencer; that gap requires a full capability crosswalk before a new issue is filed.
