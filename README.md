# Lunar 24

An open-source desktop software musical instrument: an **independent, unaffiliated study**
of the ELTA Music **Solar 42N** — the analogue microtonal ambient drone machine and
semi-modular stand-alone synthesizer. Lunar 24 is not affiliated with, endorsed by, or a
product of ELTA Music / Analogue Solutions; it is a clean-room engineering study built from
the freely published manual and the project's own fact mapping.

Lunar 24 aims to reproduce the Solar 42N **panel layout and operating behaviour** as
documented, and remaps its visual aesthetic through a restrained "moon" theme. The product
boundary is strict: it does **not** add sound sources, modulators, general-purpose routing
matrices, macros, plugin/DAW hosts, or a multi-preset library beyond the hardware's own four
keyboard presets. The goal is architectural and behavioural, **not** a claim of calibrated or
identical sonic fidelity — that would require the hardware as a reference and would
over-claim beyond the evidence discipline in `design/07-core-contract.md`. Public audio may
inform perceptual tuning only; it is never presented as strict calibration.

> This repository tracks its own source and design documents only. The ELTA Solar 42N
> reference manual, panel render and effector catalog images are copyrighted third-party
> material and are intentionally **not** committed — see `design/reference/SOURCES.md` for
> their source URLs and checksums.

## License

**Apache-2.0.** See [LICENSE](LICENSE). Lunar 24's own code is Apache-2.0; third-party
dependencies keep their own licenses (inventory: `third_party/licenses/`).

## Target platforms

- **macOS** and **Windows** (standalone desktop app). No iPad/iOS/Android, and no Linux
  desktop product support (Linux may only supplement core CI builds).

## Tech stack

- C++17 w/ warnings-as-errors, CMake.
- Standalone only; audio device + MIDI via a pinned iPlug2 submodule (**P1**); iPlug2's
  IGraphics provides the UI. No AU/VST/CLAP/AAX/WAM, no WebView.
- The **synth core** is a framework-free pure C++ library — it never includes iPlug2,
  IGraphics, CoreAudio/WASAPI, window, or filesystem types. See `design/07-core-contract.md`.

## Project layout

```
core/          framework-free synth engine (header-only C++17)
host/          macOS/Windows standalone app (iPlug2: audio, MIDI, UI)
spec/machine/  machine registry (modules, parameters, jacks, programs) — source of truth
generated/     C++ headers generated from spec/ (tools/generate_registry.py)
tests/         unit and engine tests
design/        design notes; design/STATUS.md is the plain-language progress page
```

## Build

```sh
git submodule update --init --recursive   # iPlug2 (needed for the mac/win app)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI builds the macOS and Windows apps and attaches them to each run as downloadable artifacts.

## Status

See [`design/STATUS.md`](design/STATUS.md).
