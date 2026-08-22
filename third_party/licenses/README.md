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
| iPlug2 (main repo, pinned commit) | Standalone app + device I/O + MIDI adapter (P1 onward). | **to be confirmed from the pinned commit** — iPlug2 is a permissive fork; record its exact terms + commit SHA when the submodule is added at P1. | `third_party/iplug2` (fixed commit submodule, P1) | `unverified` until pinned |
| IGraphics (part of iPlug2) | UI only (P1 onward). | falls under iPlug2's own license, confirm at pin. | same | `unverified` until pinned |
| CMake | Build system (host tool, not linked). | BSD-3-Clause (host tool) | not vendored | OK |
| C++17 runtime / STL | Language runtime, shipped by the platform toolchain, not a Lunar dependency. | platform / toolchain | not vendored | OK |

> Note: `iPlug2OOS` is used in design as a **structural reference only** (its top-level
> license was not clearly established). It is **not** copied, not depended upon, and
> never linked. This does not change Lunar's dependency boundary.
