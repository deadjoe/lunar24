# Third-party licenses — Lunar 24

Lunar 24's own code is **Apache License 2.0** (see `LICENSE` at repo root).

This directory records every third-party dependency Lunar 24 links or vendors, its
role, its license, and the source of that license determination. Per the master plan
(`design/06-master-plan.md` §2) dependencies are required to be zlib/Zlib-like/MIT; if
a dependency's license deviates we surface it here rather than silently adopting it.

**SPDX header rule (third-party not affected):** every source file Lunar 24 authors
carries, at the top of the file:

```cpp
// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
```

Lunar 24 never rewrites or re-licenses a third-party dependency's own license. Each
vendored dependency keeps its original license file and, if it embeds modifications,
a `LOCAL-MODIFICATIONS.md` note as required by that dependency's license.

## Dependency inventory

| Dependency | Role in Lunar 24 | License | Where pinned | Verification |
|---|---|---|---|---|
| (none — P0) | P0 core is framework-free; no third-party code is linked at P0. | — | — | — |
| iPlug2 (main repo, pinned commit) | Standalone app + device I/O + MIDI adapter (P1 onward, GH#10). | **zlib-like 3-clause** (permissive), reviewed from `iPlug2/LICENSE.txt` at the pinned commit. | `third_party/iPlug2` — **pinned git submodule** (GH#10), commit `d54f6905` (full `d54f69050f517e43b941d88c2a170f0a840b9ee4`). | OK — submodule is an unmodified pinned commit, verified by the `iplug_pin_clean_gate` CTest. |
| IGraphics (part of iPlug2) | UI only (P1 onward). | falls under iPlug2's own zlib-like `LICENSE.txt` at the same pin. | same `third_party/iPlug2` submodule pin. | OK — same pin, unmodified. |

> **iPlug2 dependency boundary (at pin `d54f6905`):** iPlug2's own license is zlib-like,
> and the components it carries are each permissive — WDL (zlib-like), NanoVG (zlib),
> NanoSVG (zlib), MetalNanoVG (MIT), RTAudio (MIT), RTMidi (MIT), optional Skia (BSD),
> WebView (MIT). None of these is copyleft. Lunar 24 does **not** separately vendor or pin
> any of them; the Lunar24Host CMake builds RTAudio (`RtAudio.cpp`), RTMidi (`RtMidi.cpp`)
> and the mac SWELL sources **directly out of the pinned iPlug2 tree** as transitive portions
> of that submodule, so they are compiled from the pin and remain subject to their own
> upstream licenses recorded here. Boundary is clean w.r.t. the design/06 master plan's
> zlib/Zlib-like/MIT requirement.
| CMake | Build system (host tool, not linked). | BSD-3-Clause (host tool) | not vendored | OK |
| C++17 runtime / STL | Language runtime, shipped by the platform toolchain, not a Lunar dependency. | platform / toolchain | not vendored | OK |

> Note: `iPlug2OOS` is used in design as a **structural reference only** (its top-level
> license was not clearly established). It is **not** copied, not depended upon, and
> never linked. This does not change Lunar's dependency boundary.
