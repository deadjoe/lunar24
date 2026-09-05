# P1 slice-①b — iPlug2 + IGraphics bootstrap: findings

Disposable artifact. Verdict: **PASS** — IGraphics (NanoVG/Metal) compiles,
opens a window, AND renders pixels on this machine (Darwin 25 / Apple Silicon).
All three framework-risk axes retired: 编得过 / 开得出窗 / 画得出像素.

## What was tested

`iPlug2` fixed commit `d54f6905` cloned to `spike/third_party/iPlug2` (gitignored,
license gate skips `third_party/` by name). Thin wrapper
`spike/igraphics_smoke/CMakeLists.txt` `add_subdirectory`'s only
`Examples/IPlugVisualizer` (FORMATS APP; no Tests/VST3/CLAP). Configure selected
`IGraphics: NANOVG/METAL`. Full chain (IGraphics/NanoVG/Metal/SWELL/CoreText)
compiled + linked 100%; product `build/out/IPlugVisualizer.app`.

## The bootstrap gap (NOT a framework defect)

The stock iPlug2 macOS standalone needs full Xcode's `ibtool` to compile
`MainMenu.xib` → `.nib`. This machine has only Command Line Tools
(`xcode-select -p = /Library/Developer/CommandLineTools`, no `/Applications/Xcode.app`),
so the stock `.app` cannot build (`tool 'ibtool' requires Xcode`). Circumvention:
delete the example `MainMenu.xib` + drop `NSMainNibFile` from `Info.plist` → app
builds and launches, **but no window appears**. 

**Root cause (source-pinned, confirming @Claude's diagnosis da035c51):** removing
the nib meant `SWELLAppController` was never instantiated from `awakeFromNib`
(`swellappmain.mm`). No controller → no NSApp delegate → `applicationDidFinishLaunching:`
never fires → `SWELLAPP_LOADED` never dispatched (`swellappmain.mm` L222) →
`CreateDialog` never runs → no editor window. The "no window" was a **bootstrap
artifact**, not an IGraphics failure. An earlier caveat — "we only proved the window
won't open under an incomplete toolchain, not that IGraphics can't open one" — is now
resolved in the affirmative direction.

## Path C — nib-free bootstrap (what made it PASS)

Edited `spike/third_party/iPlug2/IPlug/APP/IPlugAPP_main.cpp` (replace
`NSApplicationMain` in `main()` with a manual replication of the nib lifecycle):

```
@autoreleasepool {
  NSApplication* app = [NSApplication sharedApplication];
  Class ctrlClass = NSClassFromString(@"SWELLAppController");
  id ctrl = ctrlClass ? [ctrlClass new] : nil;
  [app setDelegate:ctrl];
  [ctrl performSelector:@selector(awakeFromNib)];   // dispatches SWELLAPP_ONLOAD
  [app activateIgnoringOtherApps:YES];
  [app run];                                         // -> SWELLAPP_LOADED -> window
}
```

The `--screenshot` / `--no-io` arg parsing and `AppIsSandboxed()` check were kept.

## Evidence (window is open AND rendered, double-proved)

1. **On-screen window** — CoreGraphics `CGWindowList` (`.optionOnScreenOnly`, no
   TCC needed) shows **1 content window id=2631, 600×332** owned by the app.
2. **Pixel-level render** — the app's own `--screenshot /tmp/iplug_shot.png --no-io`
   mode produced a **real 51 KB PNG** at 19:52 with clean stderr and clean exit:
   title bar, "Spectrum" panel, 0→−90 dB Y axis, 50 Hz→10 kHz log X axis, gridlines,
   and a −90 dB green silence trace (silence because `--no-io`). This is NanoVG/Metal
   actually drawing, not just "a window exists".

## Free discovery for slice-④ (retina 2×)

The screenshot PNG is **1200×664**, but the reported window is **600×332 — exactly
2×**. Retina backing store is the logical size times two. When ④ does the
**2400×1551 Design Coordinate Space** transform, the 2× must be applied **explicitly**
(not assumed 1:1), or hit-test regions and cable landing points will be off by half.

## ⚠️ Architecture discipline (MUST carry forward)

Path C was implemented by editing `spike/third_party/iPlug2/IPlug/APP/IPlugAPP_main.cpp`.
**This is PROOF-ONLY, it is NOT the implementation plan.** In the real product that
file is someone else's repository code; editing it would be a **secret fork of iPlug2** —
on the next pin-commit update our changes silently vanish, and no gate reports what was
lost. 

**Real host bootstrap must live in OUR OWN code**: the `SWELLAppController`
instantiation / `setDelegate` / dispatch `SWELLAPP_ONLOAD`+`SWELLAPP_LOADED` / build
window sequence is written as **our own entry point**. iPlug2 stays an **unmodified
fixed commit**, used only as a library, not edited. (This is the same principle as
slice-①: we don't use the stock host's hardcoded channel handling — now the startup
path is also ours.)

The spike's modification approach **≠** implementation approach; implementation =
self-authored entry point.
