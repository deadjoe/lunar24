# Lunar 24 — status (plain language)

_Last updated: 2026-09-29 (project refocus)._

## What works
- **Sound engine (core/)**
  - 4 classic drones (5 oscillators each) modelled on the negistor relaxation oscillator:
    capacitor-charge ramp, slow random-walk drift, per-cycle jitter; VOLT past half its
    stroke makes the oscillators frequency-modulate each other.
  - 2 "Papa Srapa" noise / S&H drones, VCO A/B (morph, PWM, sync, sub) with their VCAs
    driven by Envelope A/B, 10-channel mixer, dual Polivoks-style filter, distortion.
  - **Dual effector: all 13 cartridges x 3 programs** (reverbs, shimmer, pitch/reverse
    delays, chorus/flanger/phaser, filters, ring mod, bit-crush, mini synths).
  - 2 LFOs, 2 envelopes, joystick, 5-step sequencer, envelope follower, keyboard
    (single/twin/split, arp, 16-step, glide, vibrato, quantiser, 4 presets).
  - Patch cables, normalled connections and feedback loops.
- **App (host/)**: macOS/Windows standalone with the full panel UI, MIDI input, and the
  machine state saved on exit / restored on launch. CI attaches a downloadable app to
  every run (GitHub → Actions → the run → Artifacts).
- **Listening without the app**: `lunar24_render` renders the engine to a WAV file.
- **Tests**: ~53 unit/engine tests, about 10 seconds.

## How to play
- Knobs: drag up/down (Shift = fine), mouse wheel, double-click resets.
- Switches: click to step (right-click or Shift steps back).
- Cables: drag from one jack to another; drag a cable off an input to unplug it.
- Touch plates: click (lower on the plate = more pressure), or the computer keyboard
  like a piano: `A W S E D F T G Y H U J K O L P ;`, `Z` / `X` = octave down / up.
- MIDI keyboard: plugged-in devices are picked up automatically. Mod wheel / CC74 =
  filter cutoff, CC71 = resonance, CC91 = effector blend, CC7 = master.
- Effector: the arrows on a cartridge slot swap cartridges; PROGRAM L/R pick 1-2-3.
- DRONE VOICES keys 1-6 switch each drone voice on/off.

## Known gaps / next steps
1. Tune the sound by ear (owner listening sessions) — every curve is a first guess.
2. MIDI CC moves are heard but not yet shown on the panel knobs or saved.
3. Plugging/unplugging a cable recompiles the patch graph on the audio thread (a few
   small allocations, only at that moment). Move it off the audio thread later.
4. Windows: build is tested in CI; real audio/MIDI device testing still to do.
5. The panel layout is our own arrangement inside the measured module areas, not a
   copy of the original panel artwork.

## Known limits (by design)
- No hardware is available, so sound is tuned by ear, not measured against a real unit.
- Only features the hardware has; no extra modulation matrix, no multi-patch library.
