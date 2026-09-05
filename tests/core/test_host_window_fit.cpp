// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// task #15 PRECURSOR — host height clamp -> fit-to-window decision module.
//
// The defect (measured by the zoom-transform probe on 2026-08-25; FINDINGS kept in
// memory, not this tree, to stay spike-free): the macOS host rendered
// at DESIGN scale and let the window-manager CLAMP the height. On that machine the
// visible logical height was 1410 (design 1551), so the bottom 141 logical px
// (282 backing px) were cropped — a genuine CLAMP, not a fit-to-window, against the
// design intent (design/00 §2i + @Claude msg dac2a86f). The off-screen FBO measured
// exactly 4800x3102 = 2x2400x1551 both axes, proving the transform is correct and ONLY
// the host's height choice is wrong.
//
// This test pins the DECISION layer: a host that sizes to fit makes the bottom design
// row reachable; a host that reverts to clamp does NOT, and the reachability predicate
// must SEE that crop (fire RED). Scenarios are parameterized on the available logical
// area, so no machine-specific 141 leaks into the module — a different screen yields a
// different fit without touching the code.
//
//   GOOD  fit (drawScale = min ratios) -> whole design, incl. the bottom row at
//         design y~1451..1551, is fully on-window.
//   RED   clamp (drawScale = design, WM crops to visible height) -> the bottom row is
//         NOT on-window. This is the negative mandate #4: 负控必须真会红.
//   DATA  the module follows whatever logical area is passed (tall/short screens both
//         produce a correct fit, no hardcoded 141).
//   ASPECT uniform scale -> logical window keeps designW/designH (never stretched).
//
// ⚠️ This slice does NOT close task #15 (see host_window_fit.h). #15 stays OPEN until
// P5's real host window proves the bottom row reachable; an already-green decision
// module is not the "already-true" host (P2-③ real_path lesson).

#include "mini_test.h"

#include <lunar24/core/host_window_fit.h>

#include <cmath>

namespace {
using lunar24::core::DesignRect;
using lunar24::core::HostWindowFit;
using lunar24::core::compute_host_window_fit;
using lunar24::core::design_rect_fully_on_window;
using lunar24::core::fit_draw_scale;
using lunar24::core::fit_window_logical_size;
using lunar24::core::kDesignHeight;
using lunar24::core::kDesignWidth;

// The Design Coordinate Space is a product constant; the AVAILABLE logical area is
// machine data. The defective host's visible height (1410) is treated as DATA here,
// NOT as a constant in the module.
constexpr double kDesignW = kDesignWidth;   // 2400.0
constexpr double kDesignH = kDesignHeight;  // 1551.0
constexpr double kVisibleW = 2400.0;        // this screen's visible logical width
constexpr double kVisibleH = 1410.0;        // this screen's visible logical height (data)

// The design-control row the P1/P5 criterion cares about: the bottom of the panel.
// The interesting bound is its BOTTOM edge (y1) — under clamp it is what crops.
constexpr DesignRect kBottomRow{0.0, 1451.0, kDesignW, 1519.0};

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int run(const char* suite) {
  // GOOD — a fit host on the defective screen: the bottom row is reachable and the whole
  // design height fits the visible area. This is the fix at the decision level.
  {
    const HostWindowFit w = compute_host_window_fit(kDesignW, kDesignH, kVisibleW, kVisibleH);
    std::printf("  fit drawScale=%.5f logical=%.2fx%.2f fullHeightVisible=%d\n",
                w.drawScale, w.logicalW, w.logicalH, (int)w.fullHeightVisible);
    CHECK(near(w.drawScale, kVisibleH / kDesignH));  // height binds: 1410/1551 = 0.9090..
    CHECK(near(w.logicalW, kDesignW * w.drawScale));
    CHECK(near(w.logicalH, kDesignH * w.drawScale));
    // Fit on a height-BOUND screen lands logicalH exactly at visible height (1410), so
    // nothing crops — this is the whole point (see RED below for the contrast).
    CHECK(near(w.logicalH, kVisibleH));
    CHECK(w.fullHeightVisible == true);
    CHECK(design_rect_fully_on_window(kBottomRow, w.drawScale, kVisibleW, kVisibleH) == true);
    // Bottom edge (design y=1519, but the full panel base y=kDesignH) must land at log
    // height <= visible height. The full design base 1551 * fit == 1410 exactly.
    CHECK(design_rect_fully_on_window(DesignRect{0.0, 0.0, kDesignW, kDesignH},
                                      w.drawScale, kVisibleW, kVisibleH) == true);
  }

  // RED — revert to clamp: drawScale = design (1.0), WM crops the window to the visible
  // height. The bottom row is NOT on-window. mandate #4 negative, must genuinely fire.
  {
    const double clampScale = 1.0;              // design scale, no fit
    const double winW = kDesignW;               // WM keeps full 2400 wide
    const double winH = kVisibleH;              // WM clamps height to visible (data)
    const bool bottomReach = design_rect_fully_on_window(kBottomRow, clampScale, winW, winH);
    const bool fullReach = design_rect_fully_on_window(
        DesignRect{0.0, 0.0, kDesignW, kDesignH}, clampScale, winW, winH);
    std::printf("  clamp drawScale=1.0 bottom=%d full=%d (expect 0 0) | would-crop logH %.1f/%0.f\n",
                (int)bottomReach, (int)fullReach, kDesignH * clampScale, winH);
    CHECK_FALSE(bottomReach);
    CHECK_FALSE(fullReach);
    // The crop IS the 141px: design base (1551) minus visible height (1410) = 141.
    CHECK(near(kDesignH - kVisibleH, 141.0));
  }

  // DATA — the module adapts to whatever logical area it is given; no machine constant.
  // A TALL screen (visible height > design) expands to full design and stays reachable.
  {
    const HostWindowFit w = compute_host_window_fit(kDesignW, kDesignH, kVisibleW, 2000.0);
    std::printf("  tall fit drawScale=%.5f logical=%.2fx%.2f fullHeightVisible=%d\n",
                w.drawScale, w.logicalW, w.logicalH, (int)w.fullHeightVisible);
    CHECK(near(w.drawScale, 1.0));              // height not binding -> full design scale
    CHECK(w.fullHeightVisible == true);
    CHECK(design_rect_fully_on_window(kBottomRow, w.drawScale, kVisibleW, 2000.0) == true);
  }
  // A SHORT screen must still fit the whole panel by shrinking (height binds).
  {
    const HostWindowFit w = compute_host_window_fit(kDesignW, kDesignH, 2400.0, 1000.0);
    std::printf("  short fit drawScale=%.5f logical=%.2fx%.2f fullHeightVisible=%d\n",
                w.drawScale, w.logicalW, w.logicalH, (int)w.fullHeightVisible);
    CHECK(near(w.drawScale, 1000.0 / kDesignH));  // 0.6447..
    CHECK(near(w.logicalH, 1000.0));
    CHECK(w.fullHeightVisible == true);
    CHECK(design_rect_fully_on_window(kBottomRow, w.drawScale, 2400.0, 1000.0) == true);
  }

  // A NARROW screen where WIDTH binds: the fit is width-driven, and even then the panel
  // height fits (height is not the limiter). This also guards against the min-ratio
  // picking the wrong axis.
  {
    const HostWindowFit w = compute_host_window_fit(kDesignW, kDesignH, 1000.0, 1410.0);
    std::printf("  narrow fit drawScale=%.5f logical=%.2fx%.2f fullHeightVisible=%d\n",
                w.drawScale, w.logicalW, w.logicalH, (int)w.fullHeightVisible);
    CHECK(near(w.drawScale, 1000.0 / kDesignW));  // 0.4166.. width binds
    CHECK(near(w.logicalW, 1000.0));
    CHECK(w.fullHeightVisible == true);
  }

  // ASPECT — uniform scale never distorts; logical window preserves designW/designH.
  {
    const HostWindowFit w = compute_host_window_fit(kDesignW, kDesignH, 1000.0, 1410.0);
    CHECK(near(w.logicalW / w.logicalH, kDesignW / kDesignH));
  }

  // fit_window_logical_size is just design * scale (used to derive the logical window).
  {
    double lw = 0.0, lh = 0.0;
    fit_window_logical_size(kDesignW, kDesignH, 0.5, lw, lh);
    CHECK(near(lw, kDesignW * 0.5));
    CHECK(near(lh, kDesignH * 0.5));
  }

  // DEGENERATE input must NOT silently clamp; a fit host must reject it (scale 0). The
  // point is a host never falls back to a design-scale clamp on bad data — it refuses.
  {
    CHECK(near(fit_draw_scale(0.0, kDesignH, kVisibleW, kVisibleH), 0.0));
    CHECK(near(fit_draw_scale(kDesignW, 0.0, kVisibleW, kVisibleH), 0.0));
    CHECK(near(fit_draw_scale(kDesignW, kDesignH, 0.0, kVisibleH), 0.0));
    CHECK(near(fit_draw_scale(kDesignW, kDesignH, kVisibleW, 0.0), 0.0));
  }

  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== task #15 precursor: host window fit + control reachability ==\n");
  return run("host_window_fit");
}
