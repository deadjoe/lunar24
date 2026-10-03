# Settled decisions (and why)

Keep these unless a listening test or a real bug says otherwise.

- **Panel = the official Solar 42N drawing.** Labels, frames and tabs are extracted by
  `tools/gen_panel_art.py` into `host/include/host/panel_art.generated.h`; control positions
  live in `host/include/host/panel_ui_layout.h`. Branded "Lunar 24" (non-commercial; the
  owner confirmed no conflict).
- **Name plates** are set in Saira (OFL), expanded width, stored as outlines by
  `tools/gen_panel_logo.py` — no font file ships, and the preview matches the app. No moon
  or other emblem: like the hardware, the name alone. Static art is drawn once through
  `host/include/host/panel_art.h` by both the app and `panel_preview`.
- **Label text uses an embedded Noto Sans** (OFL, `tools/gen_ui_font.py`), loaded from
  memory. Looking up a system font by name failed on macOS and left the panel with no text.
- **Controls are drawn with depth, not bitmaps.** Knobs, jacks, switches, buttons and the
  joystick are vector-drawn in `panel_art.h` with top-left light: drop shadows, top-lit
  gradients, highlights (amounts tuned by eye). Stays sharp at any window size and the SVG
  preview shows exactly what the app draws. A control's drawing area is padded beyond its
  hit box so shadows and scale ticks are not clipped.
- **Mac window is a black metal case** with the panel set into it (`LunarCaseView` in
  `host/main.mm`): transparent title bar that is part of the case, window buttons on the
  case, drag the case to move, proportions locked. A native window underneath (not a
  borderless one), so move / resize / full screen / shadow stay standard. Panel placement
  is `place_panel` (`window_layout.h`), shared by the case drawing and the resize code.
  In full screen the case shrinks to a thin rim (no screws) so the panel fills the screen.
  Windows keeps a plain window.
- **The window scales the whole panel to fit** (`LunarHostPlugin::OnParentWindowResize`);
  iPlug2's default resets the zoom to 1 on every resize, which cropped the panel.
- **Keyboard menu** (36 settings in 6 function tabs, `host/include/host/keyboard_menu_view.h`)
  is an overlay opened by the red encoder; values are shown in the manual's units
  (`host/include/host/panel_format.h`). Count settings use steppers that stop at both ends
  (no wrap); presets A-D are picked directly. While open it covers the encoder, so CLOSE or
  Esc closes it. Its footer holds RESET PANEL (two clicks): the reset publishes the power-on
  default at a stopped-stream boundary (the audio stream is briefly reopened), the same path
  as a startup restore, so it covers the stored machine state, not just knobs. MIDI bindings and CHANNEL /
  TRANSPOSE / VELOCITY belong to the separate controller configuration and are not reset.
- **MUTE button** (right of DRONE VOICES, not on the hardware): an app-level output mute with a
  10 ms fade; the machine keeps running, nothing is saved, the app starts unmuted. This is
  not a panic / note reset: notes and effects continue while muted, so unmuting can reveal
  a held note or its remaining tail. The hardware
  has no front-panel power switch to model (only a POWER / 12 V DC socket on the top edge), so
  quitting the app is the "power off".
- **Indicator LEDs**: 20 of the 22 printed LEDs are lit from the engine (drone and envelope
  levels, LFOs, the 5-step position, preamp clip, follower level / gate, S&H steps). The engine
  writes one snapshot per audio block (relaxed atomics) and each LED redraws only when it
  changes. The joystick's two LEDs stay dark: the on-screen stick already shows its position.
- **Keyboard clock**: the internal clock is 16th notes at 10-300 BPM (steps-per-beat tuned
  by ear); a CLOCK jack or MIDI clock takes over until BPM is changed again (manual p.19).
  MIDI START restarts the arpeggio / sequence from its first step like the RESET jack;
  STOP / CONTINUE do nothing extra. Transport never stops held notes (`MidiClockFollower`).
- **Note quantiser (SCALE / ROOT)**: picking a SCALE loads its notes into the scale editor,
  the mask the quantiser reads (`scale_editor_for_selector`); keys snap to the nearest scale
  note (the manual warns several plates may then play the same note), and arp / sequencer
  output is quantised too. 0 V is A3 on every pitch source, so the quantiser counts from C
  9 semitones below it. Exactly between two notes it goes down (the manual does not say).
  SEMITONES and the 8 scales the manual only names (blues, folk, japanese, gamelan, gypsy,
  arabian, flamenco) pass notes through; the menu marks the latter NOT MODELLED rather than
  inventing intervals. ROOT shows B where the manual prints the German H.
- **MIDI bindings on panel switches** (`midi_parameter_drive` in `core/midi_map.h`): knobs follow
  the controller (absolute with pickup, or relative). Switches and levers with 2-4 positions
  are stepped by a press instead: a pad moves to the next position (wrapping), and a CC
  button flips an on/off switch when it rises past 64, like the MUTE / DRONE actions. So a
  CC button in toggle mode (127, then 0 on the next press) needs two presses per flip; set
  such buttons to momentary. Pickup engages when the controller is within one step of the
  current value, including on its first message, or has moved across it.
- **Relative knobs are detected at Learn** (`MidiRelativeDetector` in `core/midi_map.h`).
  Controllers disagree on how a relative encoder says "one step" (64 +/- n, two's complement
  1 / 127, or sign bit + amount), and the MPK manual does not say (measured: two's
  complement, REL 2). After a learn, the knob's next values decide: an absolute knob never
  repeats a value except 0 / 127 at an end stop, an encoder turned slowly does; the repeated
  value picks the dialect. Only a binding still on ABS is switched; MODE can override.
- **MIDI notes under PLAY = TWIN / SPLIT** (owner's choice, 2026-10-04): split by note
  range, so one controller plays both halves. Notes below the MIDI settings' SPLIT note
  (default C4, range C1..C7, the key played before TRANSPOSE) go to the left side, the rest
  to the right. The side is fixed at note-on (`MidiNoteSides`), so a release, a pedal
  release or poly aftertouch reaches the half that holds the note; channel aftertouch
  follows the channel's latest note. Single merges both sides as before.
- **DRONE VOICES start closed** (owner, 2026-10-04): the app opens, and RESET PANEL
  leaves the machine, with all six keys off. Each new runtime starts with its drones
  sounding and then closes them, so opening the app, RESET PANEL and every audio reopen
  play a short drone swell that fades over the voices' RLS (2-5 s). The owner likes it
  as a start-up sound; keep it.
- **Cable edits are real-time safe** (2026-10-04): compiling the patch plan allocates, so a
  live cable edit is compiled on the UI thread against its own copy of the patch
  (`SynthRuntime::planGraph`), sent with the edit, and swapped in by the audio thread
  (`installGraphPlan`); the replaced plan goes back to the UI thread to be freed. The
  audio callback that applies an edit does no heap work (`test_live_cable_rt`). If a plan
  ever does not match the live patch, the audio thread falls back to a full rebuild.
- **REC lives in the headphone corner** (owner's choice, 2026-10-04): software has no
  headphone output, so the socket's place is the REC button and the PHONE knob's place a
  WET / DRY / ALL selector (PHONE keeps its stored value, without a control). The engine
  taps every frame's four outputs after MUTE (DRY too on a 2-channel device) into a
  preallocated ring (`WavRecorder`); a writer thread writes 24-bit stereo WAVs (DRY: A
  left, B right) to Music/Lunar 24. A full ring drops and counts frames instead of
  blocking the audio thread; a sample-rate change stops the recording.
- **MIDI timing**: events are timestamped on arrival and placed inside the next block
  (`host/include/host/midi_timing.h`) — one block of constant latency instead of jitter.
- **Distortion uses first-order ADAA, not oversampling.** Oversampling would delay the wet
  path against the dry blend (comb filtering) unless the dry path were delayed too.
- **Drone ATT/RLS span 1 ms..10 s on a cubic taper** (`DroneBank::mapAttSeconds`, shared by
  drones 1-6; the manual gives no range). The old 1 s ceiling was too short for drone swells;
  the cubic keeps the lower half fine for short times (0.5 -> 1.25 s). Tuned by ear.
- **VCO waveform knob follows the panel icons** (`core/include/lunar24/core/vco_wave_map.h`):
  pointing at an icon gives that shape (sine, triangle, saw, pulse, inverted saw), neighbours
  crossfade, and the last stretch morphs sine -> triangle. Icon positions measured from the
  panel drawing. Replaced an equal-spaced order that did not match the icons (owner's test).
- **VCO B OUT carries volts and SYNC triggers at 1 V.** The jack publishes the waveform on its
  ±5 V scale (like the pressure out), and VCO A SYNC rises at 1 V (hysteresis 0.2 V; tuned by
  ear). The old 5 V threshold was never crossed by a VCO, so the manual's B -> A sync patch was
  silent.
- **Distortion GAIN is a fixed drive, not a level-dependent one** (`distortion.h`): tanh at a
  drive of 1 + 40*GAIN^2 with a make-up gain, so GAIN changes the character (clean -> heavy
  fuzz) while the level stays within a few dB. The old law scaled drive with the signal level
  and peaked at ~3% distortion, so the knobs mostly changed the volume. Tuned by ear.
- **5-step sequencer outputs have real widths** (`five_step_sequencer.h`): GATE holds for half a
  step (capped at 1 s, with a one-sample drop between back-to-back gates) and CLOCK OUT is a
  50% square. One-sample pulses could not open an envelope. A cable in EXT. CLOCK takes over
  from the PULSER (as on the hardware), and the input rises at 1.2 V so a 0..10 V LFO clocks it.
- **The computer's audio input starts silent**: PREAMP GAIN (the manual's way to mute the
  preamp) and the mixer's EXT.AUDIO VOL both default to minimum. Input channel 1 feeds
  EXT.AUDIO and channel 2 the preamp, so with either at mid a mic left selected in Preferences
  put room noise (and claps, through the reverb) under the drones. A one-channel input (a
  laptop's built-in mic) feeds both, so it reaches the preamp like the hardware's contact mic.
- **Envelope follower ATTACK / RELEASE are exponential, 1 ms .. 1 s** (centre ~32 ms; RELEASE
  defaults to ~180 ms). The old linear law put 0.5 s at the centre, too slow for a clap to
  reach the gate detector. The gate detector has its own fast peak follower (1 ms / 50 ms,
  independent of the knobs) and opens at 5 V, closes below 3 V: a clap with GAIN at mid nears
  the preamp's 10 V clip, room noise stays far below. Gating on the knob-smoothed envelope
  either missed claps or held open on room noise. Tuned by ear.
- **Level controls glide** inside the DSP blocks; loading a whole state snaps them
  (`SynthRuntime::snapSmoothedLevels`). Pitch knobs are not smoothed (phase-continuous).
- **Effector program switch** fades out, resets and fades in (~15 ms) — the hardware also
  reloads the program and cuts the tail (manual p.22).
- **Per-sample constants are memoised**; the render stayed within 2e-15 of the old output.
