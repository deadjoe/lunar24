# Lunar 24 — status (plain language)

_Last updated: 2026-10-03._

## What works
- **Sound engine (core/)**
  - 4 classic drones (5 oscillators each) modelled on the negistor relaxation oscillator:
    capacitor-charge ramp, slow random-walk drift, per-cycle jitter; VOLT past half its
    stroke makes the oscillators frequency-modulate each other. A cable into CV MOD bends
    the MOD-on oscillators by up to an octave; the CV knob sets how much.
  - 2 "Papa Srapa" noise / S&H drones (LFO OUT, CV IN at 1 V/oct, S&H IN / CLOCK / OUT
    patchable; unpatched, the S&H samples the voice's noise on the divided LFO),
    VCO A/B (morph, PWM, sync, sub) with their VCAs
    driven by Envelope A/B, 10-channel mixer, dual Polivoks-style filter, distortion.
  - **Dual effector: all 13 cartridges x 3 programs** (reverbs, shimmer, pitch/reverse
    delays, chorus/flanger/phaser, filters, ring mod, bit-crush, mini synths).
  - 2 LFOs, 2 envelopes, joystick, 5-step sequencer, envelope follower, keyboard
    (single/twin/split, arp, 16-step, rhythm patterns, glide, vibrato, quantiser, 4 presets).
  - Patch cables, normalled connections and feedback loops.
- **App (host/)**: macOS/Windows standalone with the full panel UI, MIDI input, and the
  machine state restored on launch (saved on exit and every 30 s after an edit). The panel follows the official
  Solar 42N panel drawing: same module frames, labels, printed icons, LEDs and control
  positions (taken from the PDF by `tools/gen_panel_art.py`), branded Lunar 24; the printed
  indicator LEDs are lit from the engine's state. `panel_preview > panel.svg`
  renders it without building the app. CI attaches a downloadable app to
  every run (GitHub → Actions → the run → Artifacts).
- **Listening without the app**: `lunar24_render` renders the engine to a WAV file.
- **Tests**: 61 unit/engine tests, about 30 seconds. Manual test steps (by ear, in the app):
  `design/MANUAL_TESTS.md`.

## How to play
- Knobs: drag up/down (Shift = fine), mouse wheel, double-click resets.
- Round buttons: click to toggle (amber ring = on). Lever switches: click above the
  centre to move the lever up, below to move it down.
- Cables: drag from one jack to another; drag a cable off an input to unplug it.
- Touch plates: click (lower on the plate = more pressure), or the computer keyboard
  like a piano: `A W S E D F T G Y H U J K O L P ;`. Octave: the arrow buttons next to
  the encoder, or `Z` / `X`; the display shows it.
- Keyboard settings (play mode, scale, arp, sequencer, glide, vibrato, ...): click the
  red encoder to open the KEYBOARD MENU over the plates, click it again to close. Values
  show in the manual's units (BPM, note, steps, 0-255 / 0-127) and apply at once.
- Arpeggiator / 16-step sequencer: MODE = ARPEGGIATOR or SEQUENCER, hold plates. The
  internal clock runs at BPM (10-300, 16th notes); a cable into the keyboard CLOCK jack
  takes over until BPM is changed again; RESET restarts the pattern. Edit the 16 steps
  on the menu's SEQUENCER page: drag a slider for the note (0..+24 semitones above the
  held plate), click the round button to turn the step's gate on/off (off = a rest).
- MIDI keyboard: plugged-in devices are picked up automatically. Mod wheel / CC74 =
  filter cutoff, CC71 = resonance, CC91 = effector blend, CC7 = master; pitch bend +/-2
  semitones; sustain pedal; MIDI clock drives the arpeggiator / sequencer (START / STOP).
  The MIDI button (under MUTE, right of DRONE VOICES) opens the MIDI settings: learn a
  panel control onto any CC or pad note (parameters, drone keys, cartridge, presets,
  mute), per-binding absolute/relative pickup, channel filter, octave shift and velocity
  curve. Bindings live in their own file, not in the machine state.
- Effector: click the cartridge slot (or the button below it) to load the next cartridge
  (right-click = previous; hover shows both program names); the L / R switches beside
  the button pick program 1-2-3 per side.
- DRONE VOICES keys 1-6 switch each drone voice on/off (LED lit = on).

## Checked against the official manual (v15)
Mixer channel order, the 39 effector programs and their X/Y/Z roles, output voltage
ranges, normalled connections and the keyboard ranges all match the manual. Not given by
the manual, so still guesses: arp/seq clock multiply/divide ratios (not applied yet) and the
exact note patterns of the Folk / Japanese / Gamelan / Gypsy / Arabian / Flamenco scales.

## Next steps (in order)
1. Verify the MIDI correctness fixes in PR #89 with the controller (MANUAL_TESTS T15.5–T15.6).
   Fix reproduced product failures in small PRs; audit findings are leads, not an automatic backlog.
2. Complete external MIDI support for the owner's MPK MINI IV: pressure, clock and reliable note
   handling; decide SINGLE / TWIN / SPLIT routing, then test with the actual controller.
3. Continue the remaining manual checks alongside MIDI work: 5-STEP CLK, device DRY outputs,
   PREAMP input override/unplug, and saved calibration settings. Untested does not mean broken;
   finishing every manual check is not a prerequisite for MIDI work. Windows device tests remain pending.
4. After that foundation, tune sounds from listening feedback: drone level, modulation depth,
   S&H rate/range and the combined mix. Revisit uncertain hardware curves only when needed.
5. Later: inactive decorative controls, REC (WET L/R; optional DRY A/B), panel tweaks and AU/VST3.
   BlackHole recording remains available. Do not expand these into prerequisites for MIDI.

Known stability work: cable edits compile the patch graph on the audio thread with small allocations.
Assess a focused fix separately; avoid turning it into a broad architecture rewrite.

Only when the related feature is touched: knob hover/drag redraws the whole panel — dirty
only the readout if the UI feels slow. Not planned: letting the old effector tail ring out on a cartridge switch
(the hardware reloads and cuts it too), splitting `machine_runtime.h`.

The VCOs' separate wave outputs and the envelopes' VCA-CV outputs are engine-only (not on the panel).

## Known limits (by design)
- No hardware is available, so sound is tuned by ear, not measured against a real unit.
- Only features the hardware has (plus the WAV recorder); no mod matrix, no patch library.
