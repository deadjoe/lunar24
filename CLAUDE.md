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
  next — plain language, kept under ~80 lines), `design/HARDWARE.md`, `design/DECISIONS.md`,
  `design/ARCHITECTURE.md` (module-by-module mechanism vs the Solar 42N manual, in Chinese:
  same / software adaptation / not stated by the manual / manual contradiction) and
  `design/MANUAL_TESTS.md` (the owner's step-by-step manual test guide, in Chinese, using the
  exact panel names; add or update the test case whenever a fix changes what the owner should
  check). No governance logs, role rosters, or message IDs in the repo.
- **Commit messages are plain English**: what changed and why, no internal ticket jargon.
- Before finishing: `cmake --build build && ctest --test-dir build` must pass, and for audio
  changes render a WAV with `lunar24_render` and describe what changed in the sound.

## Starting a session
Read `design/STATUS.md` (what works, how to play, what's next), `design/HARDWARE.md` (what
the manual says the machine is) and `design/DECISIONS.md` (settled technical choices and
why) before changing anything.

## Working with the owner
- Software engineer, new to audio: explain domain terms plainly. They write in Chinese.
- They judge by ear: for sound changes render a WAV (`lunar24_render`) and say what changed.
- Work on a branch and keep each PR focused. Run the relevant local tests and wait for CI.
  When native testing is needed, give the owner the Actions app download and precise steps,
  observations and expected results. Fix their feedback in the same PR. The owner verifies
  and merges the PR; do not merge it yourself. Start the next small PR after that merge.
- The official manual and panel PDF are ELTA copyright and stay out of the repo. Ask the
  owner to upload them (panel PDF + `solar42n_to_agent.zip`: manual.md, cartridges.json,
  figures) when a manual check is needed; keep them in the session scratchpad. The same
  goes for the owner's controller manual (AKAI MPK mini IV User Guide); what it tells us
  is summarised under MANUAL_TESTS "MIDI 待办".

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
