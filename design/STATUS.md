# Lunar 24 — status

_Last updated: 2026-10-04._ How each module compares with the Solar 42N manual:
`design/ARCHITECTURE.md`. Manual test steps: `design/MANUAL_TESTS.md`.

## What works
- **Engine (core/)**: every module of the Solar 42N panel. 4 classic drones (negistor
  generators, drift, VOLT FM, CV MOD, photo sensor), 2 Papa Srapa noise / S&H drones,
  VCO A/B with envelopes and VCAs, 10-channel mixer, dual Polivoks-style filter,
  distortion, dual effector (13 cartridges x 3 programs), 2 LFOs, joystick, 5-step
  sequencer, preamp + envelope follower, touch keyboard (single / twin / split, arp,
  16-step sequencer, glide, vibrato, pressure modes, quantiser, presets A-D).
  Patch cables, normalled connections and feedback loops.
- **App (host/)**: macOS / Windows standalone with the official panel layout, MIDI input
  and MIDI learn, REC to WAV, machine state restored on launch. CI builds a downloadable
  app on every run (Actions → run → Artifacts).
- **Tools**: `lunar24_render` (engine to WAV), `panel_preview` (panel to SVG).
- **Tests**: 64 unit / engine tests (~30 s), also under ASan + UBSan in CI.

## How to play
- Knobs: drag (Shift = fine), wheel, double-click resets. Cables: drag jack to jack; drag
  off an input to unplug.
- Plates: click (lower = more pressure) or keys `A W S E D F T G Y H U J K O L P ;`;
  octave with the arrows or `Z` / `X`.
- Red encoder opens the KEYBOARD MENU (PLAY, EXPRESSION, ARP, SEQ, SEQ STEPS, SERVICE);
  CLOSE or Esc closes it. SERVICE is stored only, except ENCODER DIRECTION.
- DRONE VOICES keys 1-6 latch each voice on / off; all start off (launch, RESET PANEL).
- Photo sensor (white dome, drones 1/2/4/5): press = hand over it, drag up = darker,
  sideways = fingers; bends only MOD-lit generators.
- Effector: click the cartridge slot to put the next cartridge in it (right-click = previous);
  flipping a side's L / R switch loads it there and picks program 1-2-3.
- MIDI: notes play the plates; mod wheel / CC74 cutoff, CC71 resonance, CC91 blend, CC7
  master; pitch bend, sustain, MIDI clock. The MIDI button opens learn and controller
  settings (channel, transpose, velocity curve, SPLIT note).
- REC (headphone corner): WET, DRY or ALL to 24-bit WAVs in Music/Lunar 24.

## Next steps (in order; owner-approved after the manual check)
1. ~~Keyboard fixes: arp HOLD off drops released plates; 16-step ping-pong and random.~~
2. ~~Knob tapers: envelope A/B A, D, R cubic; LFO A/B rate exponential.~~
3. ~~RESET PANEL keeps keyboard presets A-D, as it keeps MIDI bindings.~~
4. ~~Effector: a different cartridge per side, as on the hardware; taller cartridge display.~~
5. Per-plate tuning (hold a plate + encoder, any pitch; `keyboardPlateTune` is stored but
   unread), then order the arpeggio by plate number as the manual does (press order now).
6. Scale editor UI: switch single notes of the quantiser scale on / off
   (`keyboardScaleEditor` exists; only LOAD SCALE sets it now).
7. Small cleanups: stale comments in `machine_runtime.h` (header route ledger, drone ENV
   OUT range).
8. Check by hand: the keyboard menu (T12.18), SERVICE values after a restart, the Windows
   build, sustain-pedal items (need a pedal).
9. Tune sounds from listening (drone level, modulation depth, S&H, mix, VCO B top).
10. Later: AU / VST3, panel tweaks.

## Decided not to do (for now)
- Keyboard pushbutton offsets (hold arrow + encoder; TWIN / SPLIT per-side offset).
- Arp HOLD replacing the chord on a new press: new plates join the held chord (max 12).
- Effector tails across a program switch (the hardware cuts them too).

## Known limits
- No hardware here: sound is tuned by ear, not measured against a real unit.
- The manual gives no arp / seq clock multiply / divide ratios (the setting is 1:1) and no
  notes for 8 named scales (Blues x2, Folk, Japanese, Gamelan, Gypsy, Arabian, Flamenco:
  marked NOT MODELLED, notes pass through).
- Only what the hardware has, plus MIDI and the WAV recorder: no mod matrix, no patch
  library.
