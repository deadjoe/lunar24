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
core/        framework-free synth core (public contract headers + implementation)
app/         standalone app / device / MIDI / UI adapter  (P1)
spec/machine/  canonical machine registry source of truth (JSON)
generated/   committed C++ headers generated from spec/machine/ (regenerate via tools/)
tools/       stdlib-only gates (generate_registry.py, check_core_headers.py, check_registry_negative.py)
tests/core/  framework-free unit + validation + regression tests
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

**P0 is closed out and P1/P2 have exited MET; the work in flight is the GH#19 anti-aliasing
slices.** P0 established the canonical machine registry, the framework-free core contract,
`DeviceStateV1`, a field-evidence policy, and the gates that keep them consistent and
switch-clean. This is a reviewed basis to build on, **not** a frozen/locked final
implementation; the concrete P0–P8 plan is in `design/06-master-plan.md`, and the live
reconnection point is [`design/00-status.md`](design/00-status.md).
