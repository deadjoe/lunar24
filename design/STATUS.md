# Lunar 24 — status

_Last updated: 2026-10-14._ Module-by-module comparison with the Solar 42N manual:
`design/ARCHITECTURE.md`. Manual tests and results: `design/MANUAL_TESTS.md`. Sound-source
review and listening notes: `design/SOURCE_REVIEW.md`.

## What works
- **Engine (core/)**: every module of the Solar 42N panel. 4 classic drones (negistor
  generators, drift, VOLT FM, CV MOD, photo sensor), 2 Papa Srapa noise / S&H drones,
  VCO A/B with envelopes and VCAs, 10-channel mixer, dual Polivoks-style filter,
  distortion, dual effector (13 cartridges x 3 programs, one per side), 2 LFOs, joystick,
  5-step sequencer, preamp + envelope follower, touch keyboard (single / twin / split, arp,
  16-step sequencer, glide, vibrato, pressure modes, quantiser with scale editor, per-plate
  tuning, presets A-D). Patch cables, normalled connections, feedback loops.
- **App (host/)**: macOS / Windows standalone with the official panel layout, up to four MIDI
  inputs at once, learn and named profiles, REC to WAV, machine state restored on launch;
  plugging in an unrelated audio device does not interrupt the sound.
- **Plugins**: VST3 (Mac, Windows) and AUv2 (Mac) from the same code. The machine and drone
  keys are saved in the DAW project; MIDI profiles are shared with the app; REC does not
  respond. Plan: `design/PLUGIN_PLAN.md`; hand tests: MANUAL_TESTS sections 15 and 17.
- **CI**: every run builds downloadable Artifacts (Lunar24-macOS, Lunar24-Windows,
  Lunar24-Plugins-macOS, Lunar24-Plugins-Windows), checks the plugins with Steinberg's
  validator and auval, and runs the portable tests, also under ASan + UBSan and TSan.
- **Version**: 1.0.0. About Lunar 24 (plugins: bottom right of the MIDI panel) shows the
  build number and commit.
- **Diagnostics log**: bounded session files in `<settings>/logs` (MANUAL_TESTS section 18).
- **Tools**: `lunar24_render` (engine to WAV), `panel_preview` (panel to SVG).
- **Checked by hand on the Mac**: everything in MANUAL_TESTS except its ⏳ items.

## How to play
- Knobs: drag (Shift = fine), wheel, double-click resets. Cables: drag jack to jack; drag
  off an input to unplug.
- Plates: click (lower = more pressure) or keys `A W S E D F T G Y H U J K O L P ;`;
  octave with the arrow buttons, `Z` / `X`, or the wheel over the red encoder.
- Plate tuning: hold plates + wheel over the red encoder, or Command + wheel over a plate
  (10 cents a notch, a semitone with Option); encoder click / Command-click = back to 0.
- Red encoder: KEYBOARD MENU (PLAY, EXPRESSION, ARP, SEQ, SEQ STEPS, SERVICE); CLOSE or Esc
  closes it. Footer PANEL swatches recolor the face (kept across launches).
- DRONE VOICES 1-6 latch each voice; all start off (launch, RESET PANEL). RESET PANEL keeps
  keyboard presets A-D and the MIDI setup.
- Default panel (launch, RESET PANEL): a dark, wide D-minor film drone with a long shimmer
  hall; open drone keys swell in over ~3 s (DECISIONS "Default panel", MANUAL_TESTS T0.12).
- Photo sensor (white dome, drones 1/2/4/5): press = hand over it, drag up = darker,
  sideways = fingers; bends only MOD-lit generators.
- Effector: click the cartridge slot to pick a cartridge (right-click = previous); a
  side's 1-2-3 switch loads it on that side and picks the program.
- MIDI: notes play the plates; mod wheel / CC74 cutoff, CC71 resonance, CC91 blend, CC7
  master; pitch bend, sustain, MIDI clock. The MIDI button opens BINDINGS (Learn), DEVICES
  and PROFILES. DEVICES turns ports on (up to four at once, any of them plays) and holds each
  device's CHANNEL / TRANSPOSE / VELOCITY / SPLIT. A profile holds bindings; the two switch
  keys are global. The keybed triangles, `[` / `]` or the switch keys step profiles while
  playing; the display's second line names the profile.
- MIDI in a plugin: bindings ignore the channel unless the DAW keeps it (in Live, route the
  pads' track to channel 10, T15.15); otherwise a pad and a key on the same note are one
  key, so give bound pads notes outside the playing range or switch them to CC.
- REC (headphone corner): WET, DRY or ALL to 24-bit WAVs in Music/Lunar 24. `M` = log MARK.

## Next steps
1. Several MIDI devices: hand-tested with the MPK + iPad (2026-10-13). Still to test with a
   second device that sends notes (an M-VAVE SMC-Mixer is on its way), in the app and the
   plugins: the same note from two devices, a binding that answers one device only, and a held
   photo pad unplugged (MANUAL_TESTS T14.8-T14.11).
2. Hand-test the Windows app and plugins together, the VST3 in Live for Windows (MANUAL_TESTS
   ⏳; when the owner has a Windows PC and Live).
3. A sound-polishing round when the owner schedules it: "调音待办" and the open tail issue #9.
4. Plugin phases 2/3: options analysed in PLUGIN_PLAN section 9; not started.

## Decided not to do (for now)
- Keyboard pushbutton offsets (hold arrow + encoder; TWIN / SPLIT per-side offset).
- Arp HOLD replacing the chord on a new press: new plates join the held chord (max 12).
- Effector tails across a program switch (the hardware cuts them too).
- Photo-eye LED lit by CV MOD / LFO: CV acts on pitch directly.
- AUv3 and CLAP: Ableton does not load CLAP; AUv3 needs an app extension and a paid
  Apple developer account.

## Known limits
- No hardware here: sound is tuned by ear, not measured against a real unit.
- The manual gives no arp / seq clock multiply / divide ratios (the setting is 1:1) and no
  notes for 8 named scales (Blues x2, Folk, Japanese, Gamelan, Gypsy, Arabian, Flamenco:
  marked NOT MODELLED; the scale editor can build them by hand).
- Only what the hardware has, plus MIDI and the WAV recorder: no mod matrix, no patch
  library.
