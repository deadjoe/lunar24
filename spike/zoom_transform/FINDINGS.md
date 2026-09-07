# P1 slice ④ — zoom + fit-to-window + retina on the Design Coordinate Space

Disposable spike. Verdict: **PASS** — every requirement is proven GOOD(0) /
BAD(fires), the 2× retina is applied **explicitly** in the transform (the failure
mode flagged in ①b), and one real-render measurement confirms the renderer
genuinely honors the 2×. Scope is macOS-only (Windows suspended per bearbone
8-24). Drives the P1 `06-master-plan.md` transformation line (which node uses).

Two artifacts here:

- `zoom_transform.cpp` — **self-verifying** harness. It is pure math, not a
  build; it derives the transform verbatim from the pinned iPlug2 IGraphics
  source and then proves the required properties hold and that the negatives
  ALARM. It never touches core/ or generated/.
- `FINDINGS.md` — this file.

## The transform (derived verbatim from pinned iPlug2 IGraphics)

The mandate text (from `design/06-master-plan.md`):

> 以 2400×1551 Design Coordinate Space 统一变换，验证 50/67/75/100/125/150/200%
> 与 fit-to-window。  (06-master-plan.md:77)
> 缩放不能改变逻辑坐标或状态。  (03-ui-framework.md:119)
> geometry、hit testing、touch plate、patch point/cable 共用一个可逆 transform。
>  (06-master-plan.md:117)

The design coordinate space is **2400×1551** (aspect 1.5474:1). The transform is
read from the framework ①b proved renders on this machine
(`spike/third_party/iPlug2/IGraphics/IGraphics.h` / `IGraphicsIOS.mm`):

```
WindowWidth()          = mWidth * mDrawScale                     (IGraphics.h:1106)
GetBackingPixelScale() = GetScreenScale() * GetDrawScale()       (IGraphics.h:1803)
GetTotalScale()        = mDrawScale * mScreenScale               (IGraphics.h:1130)
GetScaleForScreen(w,h) = min(w/designW, h/designH)  (fit-to-window; IGraphicsIOS.mm:37)
```

So:

```
design ->[drawScale (zoom/fit)]-> logical window ->[mScreenScale (retina)]-> backing
backing = design * drawScale * mScreenScale
```

**The '2× retina explicit' rule** (the point of this slice): `mScreenScale` is a
**separate multiplier**. It is never folded into `drawScale`, and never dropped
from the forward or the inverse (hit-test) path. Because the two scales are
distinct, a hit-test that forgets the retina lands a design space away and is
immediately visible — that asymmetry is what makes the bugs in the negative
controls catchable.

## Requirement coverage (this machine, 2026-08-24)

Build + run:

```
cd spike/zoom_transform
clang++ -std=c++17 -O2 -Wall -Wextra zoom_transform.cpp -o zoom_transform
./zoom_transform
```

Raw output (abbreviated to the verdict lines; the full per-level trace is in
the terminal run):

```
Design Coordinate Space = 2400 x 1551 (aspect 1.5474:1); retina = 2x (this Mac).
zoom = {50,67,75,100,125,150,200}%; SetScaleConstraints(0.50, 2.00).

Req A — '2x retina explicit' in the FORWARD transform (design -> backing)
   z=1.00: corner backing = (4800, 3102)  ok
  forward applies retina 2x (drop-2x -> RED)           good=0   bad=7    PASS

Req B — hit-test inverts through BOTH zoom and retina (round-trip)
   z=0.50: probe(600,300) -> design (600.000, 300.000)  ok
   z=2.00: probe(600,300) -> design (600.000, 300.000)  ok
  hit-test inverts through zoom AND retina (off-by-half -> RED) good=0   bad=7    PASS

Req C — one unified transform: panel aspect invariant (no non-uniform stretch)
  unified transform preserves panel aspect (stretch -> RED) good=0   bad=7    PASS
  cable midpoint maps affinely (geometry == hit-test)  actual(expect 0)=0    PASS

Req D — zoom is VIEW-ONLY: design state (logical coord) invariant across levels
  zoom does not mutate design state (state*zoom -> RED) good=0   bad=6    PASS

Req E — fit-to-window (host backing 3840 x 2160, retina 2x)
   fit drawScale = 0.6963 -> content 3342 x 2160 backing, overflow=0
  fit content fits without overflow (retina explicit)  actual(expect 0)=0    PASS
  fit dropping retina overflows (2x too big) -> RED    actual(expect >0)=1    PASS

== slice ④ verdict: PASS ==
```

### Req A — forward applies the 2× retina

`z=1.00` corner backs at **4800×3102 = 2× 2400×1551**. Per level the forward is
`design * z * 2`; the BAD control (drop the retina, `s=1`) makes every level
half of the correct target → all 7 alarm. **good=0, bad=7 (PASS).**

### Req B — hit-test inverts through BOTH zoom and retina

A probe point round-trips `design -> backing -> design` to its original design
coordinate at every level. The BAD control (inverse forgets the retina, `s=1`)
lands half a design space away → all 7 alarm. This is the "off-by-half with a
stylus / cable endpoint" bug ①b flagged. **good=0, bad=7 (PASS).**

### Req C — one unified transform, aspect invariant

Because geometry, hit-testing, touch plate and patch/cable all share one affine
transform, the panel aspect (2400/1551) is preserved under every zoom; the
degree of non-uniformity is exactly 0. A non-uniform fit (x scales 1.0×, y
scales 1.5×) breaks the aspect → all 7 alarm. A **cable midpoint** (the affine
property a patch segment relies on) maps to the midpoint of its endpoints'
backings at all 7 levels → 0 breaks. **good=0, bad=7 and 0 breaks (PASS).**

### Req D — zoom is view-only (state invariant)

The design/logical coordinate is invariant across zoom levels — zoom changes the
view, not the state (03-ui-framework.md:119). BAD control writes `state *= zoom`
back into the logical coordinate → drifts with the level. **good=0, bad=6. The
`bad=6` (not 7) is a real observation, not a failure:** at the identity level
`z=1.00` the `state*zoom` bug leaves state unchanged, so it is not flagged there
(1× is a no-op); the other 6 levels alarm. That is the correct, expected count —
the identity level genuinely cannot expose a pure multiply-by-zoom bug.

### Req E — fit-to-window, retina explicit in the fit

`GetScaleForScreen(w,h) = min(w/designW, h/designH)` in **logical** pixels. For a
4K-ish host backing (3840×2160), retina 2×, the fit scale = `min(3840/2/2400,
2160/2/1551) = 0.6963`, content 3342×2160 backing, **overflow=0**. BAD control
(fit takes the backing as if it were logical, i.e. forgot to divide by the
retina) → drawScale 2× too big → content overflows → alarms with 1. **good=0,
bad(>0)=1 (PASS).**

## Real-render confirmation (off-screen FBO == 4800×3102 both axes; on-screen host CLAMPS)

The harness is pure math; the following checks the *actual* renderer — mirroring
①b's live-render discipline. This 2026-08-25 run supersedes the earlier
"real-render confirmation" that measured only the on-screen PNG.

### Off-screen framebuffer backing (the decisive datum)

The renderer draws into an off-screen framebuffer (`mMainFrameBuffer`, a NanoVG
Metal-backed image), then blits to the window. Reading that framebuffer's Metal
texture proves the *actual* backing size the transform produces. Instrumented
`IGraphicsNanoVG.cpp::DrawResize()` with an env-gated readback of
`mnvgImageHandle(mVG, mMainFrameBuffer->image)` → `id<MTLTexture>` width/height,
and ran with `config.h PLUG_WIDTH 2400 / PLUG_HEIGHT 1551`
(`LUNAR_DUMP_FBO=1`). Logged:

```
LUNAR_FBO_TEXTURE: 4800x3102 (requested 4800x3102; logical 2400x1551)
LUNAR_FBO_TEXTURE: 4800x3102 (requested 4800x3102; logical 2400x1551)
LUNAR_FBO_TEXTURE: 4800x2756 (requested 4800x2756; logical 2400x1378)
```

- **First two (initial DrawResize): backing = 4800×3102 = 2400×1551 × 2.** This is
  the off-screen framebuffer at the full design size, **both axes exact** — the
  design→backing transform produces 4800×3102 (not clamped). The "2× retina
  explicit" rule is what **executes**: the FBO is 2× the logical design on X and Y.
- **Third (a resize event): backing = 4800×2756 (logical 2400×1378).** The live
  window was clamped by the OS to height ≤ the screen's visible area, and
  `DrawResize()` re-allocated the FBO to follow the window (width stays 2400,
  height shrinks 1551→1378).

So the off-screen framebuffer *is* full-height at the design size on both axes;
clamping only appears after the OS/screen height limit bites.

### On-screen window (the host CLAMPS, it does NOT fit-to-window)

`--screenshot /tmp/zoom_2400.png --no-io` produced a **4800 × 2820** PNG. That is
the on-screen window: logical 2400×1410 (2×), i.e. the window height is clamped
to the screen's visible logical height 1410 (= NSScreen visible frame
`2560×1440 @ scale 2.0`, minus the 30pt menu bar → 1410), not 1551. Width stays
the full 2400.

**This is a genuine CLAMP, not a fit-to-window, and it is a real defect against
the design intent.** The mandate says fit-to-window (`GetScaleForScreen(w,h) =
min(w/designW, h/designH)`, in logical px) — the whole panel should be visible,
scaled to fit. But the real macOS host never calls that fit: it renders the panel
at design scale (drawScale = design) and lets the window-manager CLAMP the height
to the screen. Result: at the design's full scale, the bottom of the panel is
**cut off** (1551−1410 = 141 logical px = 282 backing px lost at the screen
bottom) instead of being scaled to fit. A fit-to-window would show the entire
1551-tall panel scaled down; the actual host crops it.

- **The transform itself is correct** — the off-screen FBO is exactly 4800×3102,
  and the fit scale math (Req E) proves a fit would overflow if the retina were
  dropped. The defect is in the **host's window height clamping**, which iPlug2
  on macOS does not reconcile with a fit-to-window path when the design exceeds
  the screen. This is the "host clamps while design intends fit" case @Claude
  flagged as a candidate real defect; the measurement confirms it is real, not a
  transform artifact. On the actual Studio Display (the P1 device target) the
  same 2400×1551 design will exceed the visible height there too, so this is a
  defect the P1 host must either fit or make scrollable — not leave cropped.

### How the measurement was made (reproducible)

1. Set `Examples/IPlugVisualizer/config.h` to `PLUG_WIDTH 2400` / `PLUG_HEIGHT 1551`.
2. Added the env-gated readback in `IGraphicsNanoVG.cpp::DrawResize()` (guarded by
   `getenv("LUNAR_DUMP_FBO")`, and `#include <cstdlib>`), compiled via the
   `igraphics_smoke` build (Unix Makefiles, NANOVG/METAL, IPlugVisualizer only).
3. Ran `LUNAR_DUMP_FBO=1 ./IPlugVisualizer --screenshot /tmp/zoom_2400.png --no-io`;
   read the `LUNAR_FBO_TEXTURE:` lines from stderr and `sips`'d the PNG.
4. `config.h` has been **reverted to the ①b baseline** `PLUG_WIDTH 600` /
   `PLUG_HEIGHT 300`. The build dir and auto-deployed
   `<local>/Applications/IPlugVisualizer.app` are gitignored disposable spike
   artifacts, not tracked source.

### ⚠️ Architecture discipline (MUST carry forward — same as ①b / igraphics_smoke)

The off-screen FBO readback (`IGraphicsNanoVG.cpp`) and the 2400×1551 measurement
(`config.h`) were implemented by editing pinned iPlug2 source files. **This is
PROOF-ONLY, it is NOT the implementation plan.** In the real product those files
are someone else's repository code; editing them would be a **secret fork of
iPlug2** — on the next pin-commit update our changes silently vanish, and no gate
reports what was lost.

**Real host bootstrap — including fit-to-window / window sizing, which is where
task #15 lives — must be in OUR OWN code.** iPlug2 stays an **unmodified fixed
commit**, used only as a library, not edited. The spike's modification approach
**≠** implementation approach.

Note the distinction here: "env-gated / inert by default" is **not the same as
"not present"**. The readback was still an edit to a pinned file; once the pin is
bumped it disappears silently and no gate reports it. The correct end state is
that the edit is **removed**, not merely disabled — so the readback block has been
reverted, not just left inert.

### What this answers for the mandate

- **Req A (forward applies 2× retina) is confirmed real, not just modeled**: the
  off-screen FBO is 4800×3102 = 2400×1551 × 2 on both axes.
- **The "fit-to-window" intent is NOT honored on macOS when design > screen**:
  the host clamps height (crops the panel bottom) rather than scaling to fit.
  This is a defect recorded for the P1 host layer.

## Negative-control evidence (the mandate's "every BAD control must ALARM" rule)

Each negative is a genuine bug injected into the transform, and each ALARMS
instead of silently passing:

- **Forward drops the retina** (`s=1`): every level targets HALF the correct
  backing → 7 alarm. (Req A)
- **Inverse drops the retina** (`s=1` in the hit-test): a probe lands half a
  design space away → 7 alarm. (Req B)
- **Non-uniform fit** (x 1.0×, y 1.5×): panel aspect breaks → 7 alarm. (Req C)
- **Zoom leaks into state** (`state *= zoom`): coordinate drifts with the level →
  6 alarm (1 at identity is a no-op, see Req D). (Req D)
- **Fit forgets the retina** (backing treated as logical): drawScale 2× too big →
  content overflows → 1 alarm. (Req E)

## Real-vs-modeled boundary (honest)

- **Real**: the renderer actually draws and produces pixels; the 4800=2×2400
  width is measured off a produced PNG, not computed.
- **Modeled**: the zoom/fit/retina transform math itself. This is a
  transform-correctness spike; the real device-level application of the
  transform on the actual Studio Display / Mac mini outputs is the P1 device
  layer (slice ③ + the P3 real-output-stream debt), out of scope here.

## Notes

- Reuses no shared code — it is a single self-contained source file. Not
  referenced by `core/` or `generated/`; not wired into any build. Cleanup: file
  is `spike/`-scoped, binary gitignored.
- The transform is read from the **pinned** iPlug2 commit, not re-derived from
  memory — so if the pin ever moves this spike carries a recorded, checkable
  source of truth.
