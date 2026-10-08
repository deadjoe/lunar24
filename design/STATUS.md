# Lunar 24 — status

_Last updated: 2026-10-05._ Module-by-module comparison with the Solar 42N manual:
`design/ARCHITECTURE.md`. Manual test steps and results: `design/MANUAL_TESTS.md`.
Sound-source review and the 2026-10-08 listening notes: `design/SOURCE_REVIEW.md`.

## What works
- **Engine (core/)**: every module of the Solar 42N panel. 4 classic drones (negistor
  generators, drift, VOLT FM, CV MOD, photo sensor), 2 Papa Srapa noise / S&H drones,
  VCO A/B with envelopes and VCAs, 10-channel mixer, dual Polivoks-style filter,
  distortion, dual effector (13 cartridges x 3 programs, a different cartridge per side),
  2 LFOs, joystick, 5-step sequencer, preamp + envelope follower, touch keyboard (single /
  twin / split, arp in plate order, 16-step sequencer, glide, vibrato, pressure modes,
  quantiser with scale editor, per-plate tuning, presets A-D). Patch cables, normalled
  connections and feedback loops.
- **App (host/)**: macOS / Windows standalone with the official panel layout, MIDI input
  and MIDI learn, REC to WAV, machine state restored on launch. CI builds a downloadable
  app on every run (Actions → run → Artifacts: Lunar24-macOS, Lunar24-Windows).
- **Plugins**: VST3 (Mac, Windows) and AUv2 (Mac) instruments built from the same code.
  The machine and drone keys are saved in the DAW project; MIDI bindings are shared with
  the app; REC is shown but does not respond. CI checks them with Steinberg's validator and
  auval (Artifacts: Lunar24-Plugins-macOS, Lunar24-Plugins-Windows). Install and test steps:
  MANUAL_TESTS section 17. Plan and decisions: `design/PLUGIN_PLAN.md`.
- **Version**: 1.0.0; About Lunar 24 shows the build number, time and commit (the plugins:
  bottom right of the MIDI panel, build number and commit).
- **Tools**: `lunar24_render` (engine to WAV), `panel_preview` (panel to SVG).
- **Tests**: 67 unit / engine tests (~70 s), also under ASan + UBSan in CI; the threaded
  ones under ThreadSanitizer too.
- **Checked by hand on the Mac**: everything in MANUAL_TESTS except the ⏳ items below.

## How to play
- Knobs: drag (Shift = fine), wheel, double-click resets. Envelope A / D / R and LFO rate
  knobs are fine at the slow / short end. Cables: drag jack to jack; drag off an input to
  unplug.
- Plates: click (lower = more pressure) or keys `A W S E D F T G Y H U J K O L P ;`;
  octave with the arrows, `Z` / `X`, or the wheel over the red encoder.
- Plate tuning: hold plates (keys) + wheel over the red encoder, or Command + wheel over a
  plate; 10 cents a notch, a semitone with Option; encoder click / Command-click = back to 0.
- Red encoder opens the KEYBOARD MENU (PLAY, EXPRESSION, ARP, SEQ, SEQ STEPS, SERVICE);
  CLOSE or Esc closes it. PLAY → NOTES switches single notes of the scale. SERVICE values
  are stored only, except ENCODER DIRECTION.
- DRONE VOICES keys 1-6 latch each voice on / off; all start off (launch, RESET PANEL).
- RESET PANEL keeps keyboard presets A-D and MIDI settings.
- Photo sensor (white dome, drones 1/2/4/5): press = hand over it, drag up = darker,
  sideways = fingers; bends only MOD-lit generators.
- Effector: click the cartridge slot to pick a cartridge (right-click = previous); flipping
  a side's 1-2-3 switch loads it on that side and picks the program.
- MIDI: notes play the plates; mod wheel / CC74 cutoff, CC71 resonance, CC91 blend, CC7
  master; pitch bend, sustain, MIDI clock. The MIDI button opens learn and controller
  settings (channel, transpose, velocity curve, SPLIT note).
- REC (headphone corner): WET, DRY or ALL to 24-bit WAVs in Music/Lunar 24.

## Next steps
1. Check the Windows build by hand (MANUAL_TESTS ⏳; the owner schedules it separately).
2. Later, when the owner asks: tune sounds from listening (MANUAL_TESTS "调音待办":
   drone level, modulation depth, S&H, mix, VCO B top).
3. Plugins: phase 1 passed in Ableton Live on the Mac (VST3 and AU, MANUAL_TESTS 17).
   Next, when the owner asks: phase 2 (automation, DAW tempo, extra outputs). Later: panel tweaks.

## Decided not to do (for now)
- Keyboard pushbutton offsets (hold arrow + encoder; TWIN / SPLIT per-side offset).
- Arp HOLD replacing the chord on a new press: new plates join the held chord (max 12).
- Effector tails across a program switch (the hardware cuts them too).
- Photo-eye LED lit by CV MOD / LFO: CV acts on pitch directly.

## Known limits
- No hardware here: sound is tuned by ear, not measured against a real unit.
- The manual gives no arp / seq clock multiply / divide ratios (the setting is 1:1) and no
  notes for 8 named scales (Blues x2, Folk, Japanese, Gamelan, Gypsy, Arabian, Flamenco:
  marked NOT MODELLED, notes pass through; the scale editor can build them by hand).
- Only what the hardware has, plus MIDI and the WAV recorder: no mod matrix, no patch
  library.
