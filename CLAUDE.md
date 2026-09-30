# Working on Lunar 24 — rules for AI agents

Lunar 24 is a software instrument inspired by the ELTA Solar 42N (ambient / drone / cinematic).
The owner judges the result **by ear**. Everything here serves one goal: a playable,
good-sounding instrument with a usable panel UI.

## Priorities (in order)
1. It builds, runs, and makes sound without glitches (real-time safety is non-negotiable).
2. It is playable: panel UI, mouse, computer keyboard, MIDI.
3. It sounds good for ambient/drone: drones, filter, distortion, and the dual effector.
4. Fidelity details (exact curves, anti-aliasing refinements) come last, and only when a
   listening test shows a problem.

## Rules
- **Every task must change the product** (code under `core/`, `host/`, or the UI) or produce a
  listening result. Pure measurement/verification tasks need explicit owner approval.
- **Tests are proportional.** One focused unit test file per module; heavy tests only for
  real-time safety, state save/load, and crash-prone code. No mutation-testing harnesses,
  no pre-registered acceptance criteria files, no evidence directories, no per-task reports.
- **Sound parameters are tuned by ear.** Numbers that the manual does not give are just
  reasonable defaults; mark them `// tuned by ear` and move on. Do not build machinery to
  "prove" a guess.
- **Docs stay short.** `README.md` (what/how to build), `design/STATUS.md` (what works, what's
  next — plain language, kept under ~80 lines), and the design notes in `design/`. No
  governance logs, role rosters, or message IDs in the repo.
- **Commit messages are plain English**: what changed and why, no internal ticket jargon.
- Before finishing: `cmake --build build && ctest --test-dir build` must pass, and for audio
  changes render a WAV with `lunar24_render` and describe what changed in the sound.

## Starting a session
Read `design/STATUS.md` (what works, how to play) and `design/HANDOFF.md` (open backlog,
decisions and their reasons, how the owner likes to work) before changing anything.

## Architecture (short)
- `core/` — framework-free C++17 synth engine (header-only). No iPlug2/platform includes
  (enforced by `tools/check_core_headers.py`). Audio thread: no allocation, no locks, no I/O.
- `host/` — iPlug2 standalone app (macOS/Windows): audio device, MIDI, IGraphics UI.
  The panel layout is framework-free (`host/include/host/panel_ui_layout.h`, tested);
  `host/panel_editor.h` draws it. UI/MIDI changes go through the engine's live queue
  (`StandaloneAudioEngine::post*`), never by touching the runtime directly.
- `spec/machine/lunar24.json` → `tools/generate_registry.py` → `generated/` (parameter,
  jack and program IDs). Edit the JSON, regenerate, commit both.
- Signal flow: 6 drones + VCO A/B + ext/preamp → 10-ch mixer → dual VCF → distortion →
  dual effector → WET L/R; VCO A/B also go to DRY outs.

## Handy commands
```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
./build/lunar24_render --seconds 20 --out wet.wav      # listen to the engine offline
./build/panel_preview > panel.svg                      # look at the panel layout
```
