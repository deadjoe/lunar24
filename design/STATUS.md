# Lunar 24 — status (plain language)

_Last updated: 2026-09-29 (project refocus)._

## What works
- **Sound engine (core/)**: 4 classic drones (5 oscillators each), 2 "Papa Srapa" noise/S&H
  drones, VCO A/B (morphing waveforms, PWM, sync, sub), 10-channel mixer, dual Polivoks-style
  filter, distortion, 2 LFOs, 2 envelopes, joystick, 5-step sequencer, envelope follower,
  keyboard logic (single/twin/split, arp, 16-step seq, portamento, vibrato, quantiser,
  4 presets). Patch cables and normalled (default) connections work in the engine.
- **App shell (host/)**: macOS/Windows standalone opens an audio device, renders the engine,
  saves the machine state on exit and restores it on launch.
- **Tests**: ~50 unit/behaviour tests, run in seconds.

## What is missing (the refocus plan, in order)
1. **Listening tool** — render the engine to a WAV file offline.
2. **Dual effector** — the 13 cartridges × 3 programs (reverb, delays, pitch, modulation,
   filter, ring mod, bit-crush, mini-synths). This is where most of the ambient character lives.
3. **Drone character** — classic drones need more organic drift/instability; nicer default patch.
4. **Playing** — MIDI and computer-keyboard input into the keyboard.
5. **Panel UI** — knobs, switches, jacks, patch cables and the touch keyboard on the
   2400×1551 panel.
6. Later: Windows real-device testing, tuning by ear against Solar 42N demo recordings.

## Known limits (by design)
- No hardware is available, so sound is tuned by ear, not measured against a real unit.
- Only features the hardware has; no extra modulation matrix, no multi-patch library.
