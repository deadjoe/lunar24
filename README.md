# Lunar 24

An open-source desktop software musical instrument: a faithful software emulation of the
ELTA Music **Solar 42N** — the analogue microtonal ambient drone machine and semi-modular
stand-alone synthesizer.

Lunar 24 keeps the Solar 42N sound topology, panel layout, and operating relationships, and
remaps its visual aesthetic through a restrained "moon" theme. The product boundary is
strict: it does **not** add sound sources, modulators, general-purpose routing matrices,
macros, plugin/DAW hosts, or a multi-preset library beyond the hardware's own four keyboard
presets.

> This is the official repository only. The ELTA Solar 42N reference manual, panel render and
> effector catalog images are copyrighted third-party material and are intentionally **not**
> committed — see `design/reference/SOURCES.md` for their source URLs and checksums.

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
core/        framework-free synth core (public contract headers + implementation)
app/         standalone app / device / MIDI / UI adapter  (P1)
spec/machine/  canonical machine registry source of truth (JSON)
generated/   committed C++ headers generated from spec/machine/ (regenerate via tools/)
tools/       stdlib-only generators (generate_registry.py)
tests/core/  framework-free unit + validation tests
design/      design specifications and reference index
third_party/ dependency license inventory
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Governance

Design/engineering direction is set in the design docs. Development flows through feature
branches and PRs; commits follow Conventional Commits and each commit is a buildable,
testable logical unit.

## Status

P0 — lock an auditable baseline (canonical machine registry, framework-free core contract,
`DeviceStateV1`, evidence + tests). See `design/06-master-plan.md` for the full P0–P8 plan.
