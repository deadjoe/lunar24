# Lunar 24

An open-source desktop software musical instrument: an **independent, unaffiliated study**
of the ELTA Music **Solar 42N** — the analogue microtonal ambient drone machine and
semi-modular stand-alone synthesizer. Lunar 24 is not affiliated with, endorsed by, or a
product of ELTA Music / Analogue Solutions; it is a clean-room engineering study built from
the freely published manual and the project's own fact mapping.

Lunar 24 aims to reproduce the Solar 42N **panel layout and operating behaviour** as
documented. The product boundary is strict: it does **not** add sound sources, modulators,
general-purpose routing matrices, macros, plugin/DAW hosts, or a multi-preset library
beyond the hardware's own four keyboard presets. The goal is architectural and behavioural, **not** a claim of calibrated or
identical sonic fidelity, which would need the hardware as a reference. Sound is tuned by ear.

> This repository tracks its own source and design documents only. The ELTA Solar 42N
> reference manual and panel drawing are copyrighted third-party material and are
> intentionally **not** committed — see `design/reference/SOURCES.md` for their sources.

## License

**Apache-2.0.** See [LICENSE](LICENSE). Lunar 24's own code is Apache-2.0; third-party
dependencies keep their own licenses (inventory: `third_party/licenses/`).

## Target platforms

- **macOS** and **Windows** (standalone desktop app). No iPad/iOS/Android, and no Linux
  desktop product support (Linux may only supplement core CI builds).

## Tech stack

- C++17 w/ warnings-as-errors, CMake.
- Standalone only; audio device + MIDI via a pinned iPlug2 submodule; iPlug2's
  IGraphics provides the UI. No AU/VST/CLAP/AAX/WAM, no WebView.
- The **synth core** is a framework-free pure C++ library — it never includes iPlug2,
  IGraphics, CoreAudio/WASAPI, window, or filesystem types.

## Project layout

```
core/          framework-free synth engine (header-only C++17)
host/          macOS/Windows standalone app (iPlug2: audio, MIDI, UI)
spec/machine/  machine registry (modules, parameters, jacks, programs) — source of truth
generated/     C++ headers generated from spec/ (tools/generate_registry.py)
tests/         unit and engine tests
design/        STATUS.md (progress), HARDWARE.md (the manual's facts), DECISIONS.md
```

## Build

```sh
git submodule update --init --recursive   # iPlug2 (needed for the mac/win app)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI builds the macOS and Windows apps and attaches them to each run as downloadable artifacts.
The mac app is signed ad hoc, not notarized: after downloading, clear the quarantine flag
once with `xattr -cr Lunar24Host.app` (otherwise macOS reports it as damaged).

Audio: the app plays through the system's current output device (headphones, AirPods)
unless another one is chosen in Preferences (app menu > Preferences…, or ⌘,); the audio
input (microphone) stays off until it is switched on there. Devices open at the rate they already run at. When the output
device changes or drops out (AirPods connected or put away), the app reopens on the current
one. If the saved setup cannot open, it falls back to one that can; if nothing opens, the
keyboard display reads NO AUDIO.

## Status

See [`design/STATUS.md`](design/STATUS.md).
