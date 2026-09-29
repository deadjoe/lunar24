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
  machine state saved on exit / restored on launch. The panel follows the official
  Solar 42N panel drawing: same module frames, labels and control positions (taken from
  the PDF by `tools/gen_panel_art.py`), branded Lunar 24. `panel_preview > panel.svg`
  renders it without building the app. CI attaches a downloadable app to
  every run (GitHub → Actions → the run → Artifacts).
- **Listening without the app**: `lunar24_render` renders the engine to a WAV file.
- **Tests**: ~53 unit/engine tests, about 10 seconds.

## How to play
- Knobs: drag up/down (Shift = fine), mouse wheel, double-click resets.
- Round buttons: click to toggle (amber ring = on). Lever switches: click above the
  centre to move the lever up, below to move it down.
- Cables: drag from one jack to another; drag a cable off an input to unplug it.
- Touch plates: click (lower on the plate = more pressure), or the computer keyboard
  like a piano: `A W S E D F T G Y H U J K O L P ;`. Octave: the arrow buttons next to
  the encoder, or `Z` / `X`; the display shows it.
- Keyboard settings (play mode, scale, arp, sequencer, glide, vibrato, ...): click the
  red encoder to open the KEYBOARD MENU over the plates, click it again to close.
- MIDI keyboard: plugged-in devices are picked up automatically. Mod wheel / CC74 =
  filter cutoff, CC71 = resonance, CC91 = effector blend, CC7 = master.
- Effector: click the cartridge slot (or the button below it) to load the next cartridge
  (right-click = previous; hover shows both program names); the L / R switches beside
  the button pick program 1-2-3 per side.
- DRONE VOICES keys 1-6 switch each drone voice on/off (LED lit = on).

## Known gaps / next steps
1. Tune the sound by ear (owner listening sessions) — every curve is a first guess.
2. Plugging/unplugging a cable recompiles the patch graph on the audio thread (a few
   small allocations, only at that moment). Move it off the audio thread later.
3. Windows: build is tested in CI; real audio/MIDI device testing still to do.
4. Panel parts shown but not functional yet: the classic drones' CV amount knob, photo
   sensor, the NEW drones' LFO-out / CV-in jacks and the headphone socket. The VCOs'
   separate wave outputs and the envelopes' VCA-CV outputs exist in the engine but are
   not on the official panel, so they are not patchable from the UI.

## Known limits (by design)
- No hardware is available, so sound is tuned by ear, not measured against a real unit.
- Only features the hardware has; no extra modulation matrix, no multi-patch library.
