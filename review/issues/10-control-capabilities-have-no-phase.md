# A10 — Required control capabilities have no implementation phase

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b`

## Finding

The master plan declares LFO A/B, Envelope A/B, joystick, and the physical
five-step sequencer as required product capabilities, but none is assigned to a
concrete P0–P8 DSP implementation or exit criterion.

- `design/06-master-plan.md:41-44` lists all four in the L2 capability layer.
- `design/01-architecture.md:30-40,47,61-66` independently identifies their
  signals and physical controls.
- The canonical registry contains separate module IDs 4–8 and 11 with parameters
  and patchable jacks.
- P0 only inventories the descriptors. P1–P8 never schedules their DSP behavior.
  The P3 list covers drones, VCOs, preamp/follower, mixer, VCF, distortion, and
  outputs; P4 covers the touch keyboard, not these four control generators.

The implementation mirrors the planning hole: there is no Envelope A/B, LFO
A/B, joystick, or physical five-step production class; no `FixedChainRole` or
generic executor can run one; and no behavior test exercises one. Registry and
patch-identity tests are structural evidence only.

The physical five-step sequencer is distinct from the keyboard 16-step
sequencer. Current comments in `device_state.h:326-327` and
`device_capacities.h:63` incorrectly describe the keyboard sequence as an
extension of the five-step run, although the actual schema keeps `KeyboardSeq`
and `SequencerSettings` separate.

## Required correction

Assign these four capabilities to explicit implementation phases and exit
criteria before scheduling their code. Preserve the physical five-step and
keyboard 16-step sequencers as separate subsystems. Then implement product DSP,
runtime/factory consumption, virtual-voltage behavior, and old-error negative
tests for each capability without inventing unevidenced transfer constants.

## Classification

Design-plan gap that has already produced missing product implementation; high
severity because P3's “playable and patchable whole chain” status otherwise
hides four required control sources.
