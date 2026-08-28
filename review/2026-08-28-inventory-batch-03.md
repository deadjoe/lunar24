# Codex inventory audit — batch 03

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b` (`feat/p0-full-registry`)

Scope: crosswalk the complete designed capability set through registry, core
implementation, core runtime, standalone host, behavior tests, and plan exit
criteria. No product code was changed.

## Crosswalk

| Capability | Plan assignment | Registry | Core implementation | Core runtime | Standalone host | Behavioral verdict |
|---|---|---:|---|---|---|---|
| VCO A/B | P3 | yes | `Vco` | consumed | not consumed | core-runtime complete; product incomplete |
| Dual VCF | P3 | yes | partial: linear filter, no unit calibration/nonlinear state | consumed | not consumed | partial; #6 |
| Sensor keyboard | P4 | yes | behavior/state components exist | not composed into `SynthRuntime` | not consumed | partial; #8 plus state application gap |
| Envelope A/B | **none** | yes | absent | absent | absent | **plan gap + missing implementation** |
| LFO A/B | **none** | yes | absent | absent | absent | **plan gap + missing implementation** |
| Joystick | **none** | yes | absent | absent | absent | **plan gap + missing implementation** |
| Preamp | P3 | yes | `Preamp` | consumed | not consumed | core-runtime complete; product incomplete |
| Envelope follower | P3 | yes | `EnvelopeFollower` | consumed | not consumed | core-runtime complete; product incomplete |
| Physical five-step sequencer | **none** | yes | reserved state only | absent | absent | **plan gap + missing implementation** |
| Voice mixer | P3 | yes | `VoiceMixer` | consumed | not consumed | core-runtime complete; product incomplete |
| Dual effector | P6 | yes | not yet implemented | absent | absent | plan-assigned future work, not current debt |
| Classic drones 1/2/4/5 | P3 | yes | oscillator/control subset | consumed | not consumed | partial; #5 |
| New drones 3/6 | P3 | yes | Schmitt/FM/AM/noise/S&H subset | consumed | not consumed | core-runtime complete; product incomplete |
| DeviceState save/restore | P0/P2/P8 | schema exists | encode/decode exists | no apply/restore path | absent | partial; new finding A11 |
| Four logical outputs / adapter | P1/P3 | declared | `RuntimeOutput` exists; device-layout helpers synthesize test signals | produced by core runtime | host bypasses both runtime and adapter | partial; #4/#10 |
| macOS/Windows standalone | P1 | n/a | macOS bootstrap only | n/a | macOS `ProcessBlock` is pass-through/silence; no Windows product target | missing product implementation; #4/#10 |

`generated/lunar24/registry.hpp` is descriptive data only. It contains no
factory or constructor. `SynthRuntime::FixedChainRole` and `step_()` form a
closed dispatch over VCO A/B, drones, external input, preamp, envelope follower,
mixer, VCF, and distortion; there is no generic module executor.

The real standalone boundary is still disconnected: `host/plugin.cpp:83-96`
does not instantiate `SynthRuntime`; it copies connected inputs to outputs and
otherwise emits silence. The stronger core runtime test therefore proves a core
path, not a runnable product path.

## Confirmed findings

| ID | Severity | Finding | Disposition |
|---|---|---|---|
| A10 | High | The master plan declares LFO A/B, Envelope A/B, joystick, and the physical five-step sequencer, but assigns none to a P0–P8 implementation phase | New GitHub issue |
| A11 | High | Decoded `DeviceStateV1` and keyboard preset scalar fields are never applied to the product runtime | New GitHub issue |
| A03 | High | The standalone host does not instantiate `SynthRuntime` or a real `DeviceAdapter`; the composed core runtime remains test-only at the application boundary | Add exact-head evidence to #4 |
| A04 | High | Classic drone gate/HOLD/ATT/RLS/CV/envelope-out and parts of the dynamic model remain absent | Already retained as #5 partial |
| A05 | High | VCF identity/calibration and level-dependent nonlinear behavior remain absent | Already retained as #6 open |
| A07 | High | Keyboard note/touch pipeline defects remain; separate state-application debt is tracked by A11 | Already retained as #8 open |

## Five-step versus keyboard sequencer correction

The physical five-step sequencer and keyboard 16-step sequencer are distinct in
`design/01-architecture.md:30-40,61-66`, `design/06-master-plan.md:41-44`, and
the registry (`ModuleId::sequencer` versus `ModuleId::keyboard`). The current
state schema also keeps them distinct: `KeyboardSeq` is separate from the
reserved `SequencerSettings` block.

The defect at this head is narrower but real: comments in
`device_state.h:326-327` and `device_capacities.h:63` incorrectly call the
keyboard sequence a continuation of the five-step run. Those comments should be
corrected when A10 assigns the physical sequencer to a phase; there is no
evidence that the serialized fields themselves are currently merged.

## Exit judgment

P3/P4 status cannot be inferred from registry completeness or isolated class
tests. At this head the composed core audio subset exists, but the whole-machine
factory, missing control generators, state application, keyboard composition,
device adapter, and host consumption do not. The next implementation work must
not resume feature expansion until the inventory is finished and the repair
order is explicitly set.
