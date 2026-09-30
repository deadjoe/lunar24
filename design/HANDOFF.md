# Handoff — where the last session stopped (2026-09-30)

Read `design/STATUS.md` first (what works, how to play). This page adds what only lived in
the chat: the open backlog, decisions and their reasons, and how the owner likes to work.

## State
- `main` @ PR #50 (refocus: official-panel UI, dual effector, keyboard arp/seq clock, MIDI,
  autosave, 40% less render CPU). Only `main` exists; CI green on Linux / macOS / Windows.
- The app is downloadable from GitHub → Actions → latest `ci` run on `main` → Artifacts.
- The owner is now listening by ear; sound feedback from them comes first (CLAUDE.md rule 1-3).

## Open backlog (objective, not sound tuning) — in suggested order
1. Effector cartridge switch: `EffectorSlot::reset()` clears ~1 MB of delay lines inside one
   sample (CPU spike) and cuts the old program's tail after a 15 ms fade. Clear lazily /
   let the tail ring out on a second slot.
2. Before any UI for keyboard presets A-D: `StandaloneAudioEngine::applyDeviceState` /
   `applyPresetAction` swap the whole definition with no audio-thread sync. Today they only
   run at `OnReset` (stream stopped), which is safe. Route them through the live queue or a
   stop-swap-start before exposing them.
3. Cable patching recompiles the graph on the audio thread (`drainLive_` → `rebuild()`,
   measured ~12 µs avg / 159 µs worst, small allocations). Compile on the UI thread and swap.
4. Knob hover calls `SetAllControlsDirty()` (full redraw); dirty only the readout.
5. When features settle: split `core/include/lunar24/core/machine_runtime.h` (~4000 lines)
   by module, strip the old process jargon from comments (GH#, @Codex, task#…), and slim
   the tests (~34k lines, some oracle/allocator suites overlap).

## Missing because the manual does not specify them
Arp/seq clock multiply/divide ratios and RHYTHM patterns (not applied: 1 step per clock),
the 16-step sequencer's per-step note editor (steps are all 0; it only transposes by the held
plate), the note sets of the Folk / Japanese / Gamelan / Gypsy / Arabian / Flamenco scales.
Also shown but not functional: classic-drone CV amount knob, photo sensor, drone 3/6
LFO-out / CV-in jacks, headphone socket.

## Decisions worth keeping (and why)
- Panel = the official Solar 42N drawing: labels/frames/tabs extracted by
  `tools/gen_panel_art.py` into `host/include/host/panel_art.generated.h`; positions in
  `panel_ui_layout.h`. Branded "Lunar 24" (non-commercial; the owner confirmed no conflict).
- Keyboard menu (35 settings) is an overlay opened by the encoder; values shown in the
  manual's units (`host/include/host/panel_format.h`).
- Internal keyboard clock = 16th notes at 10-300 BPM (steps-per-beat tuned by ear); a CLOCK
  jack or MIDI clock takes over until BPM is changed again (manual p.19).
- MIDI is timestamped on arrival and placed inside the next block
  (`host/include/host/midi_timing.h`): one block of constant latency instead of jitter.
- Distortion uses first-order ADAA, not oversampling: oversampling would delay the wet path
  against the dry blend (comb filtering) unless the dry path is delayed too.
- Level controls glide inside the DSP blocks; a whole-state load snaps them
  (`SynthRuntime::snapSmoothedLevels`). Pitch knobs are not smoothed (phase-continuous).
- Per-sample constants are memoised; the render stayed within 2e-15 of the old output.

## Reference material (not in the repo — ELTA copyright)
The official panel PDF and a converted user manual (manual.md + cartridges.json + figures,
from `solar42N_instruct_08_04_v15.pdf`) were uploaded by the owner. Ask them to upload the
zip again when a manual check is needed. The effector program table is already in
`spec/machine/lunar24.json`.

## Working with the owner
- Software engineer, new to audio: explain domain terms plainly; they write in Chinese.
- They judge by ear: for sound changes render a WAV (`lunar24_render`) and say what changed.
- Work on a branch, open a PR, merge yourself when CI is green (owner's standing permission);
  merge commits, not squash. Keep docs short (CLAUDE.md rules).
