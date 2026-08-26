## Audit base

`16496295cf1fdeb68b0c83c8549f7a2905927610` (`feat/p0-full-registry`)

## Finding

The classic drone source is still an explicit clean-sine placeholder although P3 is marked `MET`.

- The official manual describes each classic voice as five sawtooth negistor oscillators with low/medium/high roles, per-generator TUNE/MUTE/MOD, shared VOLT behavior, photo-sensitive/CV modulation, mutual FM after half travel, and voice envelope/gate behavior (manual text around lines 264-318; derived source documented in `design/reference/SOURCES.md`).
- `DroneBank` instead assigns each oscillator a random 20–2000 Hz base and amplitude, emits `sin(phase)`, and calls an identity `nonlinear_()` (`drone_bank.h:26-30,83-113,116-137,168-171`). Its own comments label both the waveform and negistor seam placeholders.
- It has no binding for the panel's per-generator TUNE/MUTE/MOD controls, shared VOLT/mutual-FM behavior, photo-sensitive/CV modulation, or voice envelope/gate.
- Dynamic variation is only two deterministic LFO sines; the required small-noise/correlated-environment variation in `design/07-core-contract.md:150-158` is absent.

This is not a demand for unmeasurable analog tone matching. It is a structural mismatch with explicit hardware behavior and with the project's own rule that classic drones include independent free-running oscillators, tolerance, drift, and nonlinearity from the first audible version (`design/06-master-plan.md:97-105`; `design/07-core-contract.md:150-158`).

## Required correction

Keep unknown circuit constants provisional, but replace the placeholder with a structurally faithful classic-drone model and bind every confirmed panel behavior. Add absolute behavioral tests for saw/negistor nonlinearity, fixed pitch ordering/roles, per-generator controls, shared VOLT/mutual-FM transition, modulation/gate-envelope behavior, and identity-seed reproducibility.

## Classification

Implementation defect; high severity; P3 exit gate not satisfied.
