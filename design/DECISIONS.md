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
- **Keyboard menu** (35 settings) is an overlay opened by the red encoder; values are shown
  in the manual's units (`host/include/host/panel_format.h`).
  Its title row also holds CLOSE and RESET PANEL (two clicks): the reset publishes the power-on
  default at a stopped-stream boundary (the audio stream is briefly reopened), the same path
  as a startup restore, so it covers every stored field, not just knobs.
- **Keyboard clock**: the internal clock is 16th notes at 10-300 BPM (steps-per-beat tuned
  by ear); a CLOCK jack or MIDI clock takes over until BPM is changed again (manual p.19).
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
- **Level controls glide** inside the DSP blocks; loading a whole state snaps them
  (`SynthRuntime::snapSmoothedLevels`). Pitch knobs are not smoothed (phase-continuous).
- **Effector program switch** fades out, resets and fades in (~15 ms) — the hardware also
  reloads the program and cuts the tail (manual p.22).
- **Per-sample constants are memoised**; the render stayed within 2e-15 of the old output.
