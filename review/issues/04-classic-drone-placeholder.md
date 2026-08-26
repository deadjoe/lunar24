## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

The classic drone source is still a clean-sine, structurally unbound placeholder with no actual nonlinearity although P3 was marked `MET`.

- The official manual describes each classic voice as five sawtooth negistor oscillators with low/medium/high roles, per-generator TUNE/MUTE/MOD, shared VOLT behavior, photo-sensitive/CV modulation, mutual FM after half travel, and voice envelope/gate behavior (manual text around lines 264-318; derived source documented in `design/reference/SOURCES.md`).
- `DroneBank` instead assigns each oscillator a random 20–2000 Hz base and amplitude, emits `sin(phase)`, and calls an identity `nonlinear_()` (`drone_bank.h:26-30,83-113,116-137,168-171`). Its own comments label both the waveform and negistor seam placeholders.
- It has no binding for the panel's per-generator TUNE/MUTE/MOD controls, shared VOLT/mutual-FM behavior, photo-sensitive/CV modulation, or voice envelope/gate.
- Dynamic variation is only two deterministic LFO sines; the required small-noise/correlated-environment variation in `design/07-core-contract.md:150-158` is absent.

This is not a demand for unmeasurable analog tone matching. It is a structural mismatch with explicit hardware behavior and with the project's stronger core contract: classic drones must support independent free-running oscillators, fixed tolerance, slow drift, and nonlinearity from the first audible version; a clean waveform+detune structure is explicitly forbidden (`design/06-master-plan.md:97-105`; `design/07-core-contract.md:150-158`).

## Required correction

Keep unknown circuit constants provisional, but replace the placeholder waveform/frequency layout with a structurally faithful classic-drone model, implement a real non-identity nonlinear stage, and bind every confirmed panel behavior. Add absolute behavioral tests for saw/nonlinear structure, fixed pitch ordering/roles, per-generator controls, shared VOLT/mutual-FM transition, modulation/gate-envelope behavior, and identity-seed reproducibility. The tests must prove the nonlinear structure exists without claiming unmeasured circuit constants are authoritative.

## Classification

Confirmed implementation defect; high severity; P3 exit gate not satisfied. `design/07-core-contract.md`'s “must support nonlinearity” requirement controls over the weaker “reserve the seam” wording in `design/06-master-plan.md`; this still does not authorize invented precision.
