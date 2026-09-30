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
- **The window scales the whole panel to fit** (`LunarHostPlugin::OnParentWindowResize`);
  iPlug2's default resets the zoom to 1 on every resize, which cropped the panel.
- **Keyboard menu** (35 settings) is an overlay opened by the red encoder; values are shown
  in the manual's units (`host/include/host/panel_format.h`).
- **Keyboard clock**: the internal clock is 16th notes at 10-300 BPM (steps-per-beat tuned
  by ear); a CLOCK jack or MIDI clock takes over until BPM is changed again (manual p.19).
- **MIDI timing**: events are timestamped on arrival and placed inside the next block
  (`host/include/host/midi_timing.h`) — one block of constant latency instead of jitter.
- **Distortion uses first-order ADAA, not oversampling.** Oversampling would delay the wet
  path against the dry blend (comb filtering) unless the dry path were delayed too.
- **Level controls glide** inside the DSP blocks; loading a whole state snaps them
  (`SynthRuntime::snapSmoothedLevels`). Pitch knobs are not smoothed (phase-continuous).
- **Effector program switch** fades out, resets and fades in (~15 ms) — the hardware also
  reloads the program and cuts the tail (manual p.22).
- **Per-sample constants are memoised**; the render stayed within 2e-15 of the old output.
