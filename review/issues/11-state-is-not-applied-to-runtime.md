# A11 — Persisted state is decoded but never applied to the product runtime

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b`

## Finding

`DeviceStateV1` can round-trip through the serializer, but there is no production
path that applies a decoded state to `SynthRuntime` or the standalone host.

- `decode_device_state()` exists in `state_serializer.h`, but production code has
  no caller; its consumers are tests.
- `SynthRuntime` neither owns `DeviceStateV1` nor exposes a canonical whole-state
  apply/restore operation. Its timed parameter dispatch currently recognizes
  only eight drone 3/6 parameters.
- `host/plugin.h` has no runtime/state member, and `host/plugin.cpp:83-96`
  bypasses the runtime entirely.
- `keyboard_presets.h:56-59,83-85` explicitly leaves the preset's scalar
  parameters untouched on both load and save. The landed right-side scalar bank
  therefore does not make a preset behaviorally restorable.

The current serializer and preset tests prove byte preservation and isolated
state movement. They do not prove startup restore, preset recall, or a changed
state reaching generated parameters and audible product behavior.

## Required correction

Define one canonical, off-audio-thread state-application path from validated
`DeviceStateV1` into the complete product runtime. It must apply all physical
parameters, patches/normalized-route overrides, keyboard current state and all
four preset payloads including scalar fields, effector selections, identity,
and calibration at a safe boundary. Add product-path tests whose negative
controls omit or misroute one representative value in each state family and
therefore fail observably after restore.

## Classification

Implementation/integration defect; high severity; P2 state-restoration and P4
keyboard-preset exits are not satisfied by serialization-only tests.
