# A09 — CI does not compile the declared macOS/Windows standalone product host

Severity: High

Audit base: `11e3bb2b3ebcf331985f5a1765b2abc44994c86b`

## Finding

The design requires a fixed-commit iPlug2 submodule and a macOS/Windows standalone build, but the repository has no `.gitmodules` entry. `.gitignore:40-44` instead excludes a manually cloned `third_party/iPlug2/` directory.

CI checks out only the Lunar repository and never obtains iPlug2. Root `CMakeLists.txt:274-284` explicitly turns the product host into a no-op when that ignored clone is absent and restricts it to Apple. The Windows matrix job therefore compiles only the framework-free core/tests, not a Windows standalone host, device adapter, MIDI adapter, or IGraphics UI.

This diverges from:

- `design/03-ui-framework.md:7-13`: fixed-commit iPlug2 submodule and macOS/Windows standalone;
- `design/03-ui-framework.md:115-122`: both platform shells must build before DSP implementation;
- `design/06-master-plan.md:15-17,75-83`: pinned submodule, standalone-only product, and Windows CI compile as a P1 exit condition;
- `README.md:30-39`: macOS/Windows standalone and pinned iPlug2 submodule.

P1 is nevertheless marked MET in `design/00-status.md:85`.

## Required correction

Make the pinned iPlug2 dependency reproducibly available to CI using the declared submodule contract (or formally amend the design to an equally reproducible, license-preserving mechanism). Add hosted compile jobs for the actual macOS and Windows standalone targets. Missing iPlug2 may loudly skip hardware-only runtime tests, but it must not silently remove the product build target while satisfying the platform compile gate.

Windows real-device audio/MIDI behavior remains deferred as documented; this finding concerns compilation of the actual product surface, not hardware access.
