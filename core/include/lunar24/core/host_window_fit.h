// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host window fit + control reachability — task #15 PRECURSOR (design/00 §2i +
// @Claude msg dac2a86f). This is the DECISION LAYER for how the host must size its
// window; it is NOT the host. frame-free pure geometry: no windowing, no platform
// API, no iPlug2/IGraphics type.
//
// ⚠️ This slice does NOT close task #15. @Claude's ruling (msg 52a001a3): #15's
// 债务 is "面板底部 141 逻辑 px 不可达", and "真正不可达与否，要等 P5 的宿主真开出窗口才
// 成立"。 A correct module only says "IF the host sizes to fit, the bottom is reachable".
// #15 stays OPEN until P5's real host window proves the bottom row reachable. The P5
// mandate (recorded here + in memory) is: the host MUST consume this module to set
// window height; NEGATIVE = host computes its own size bypassing it -> red. The
// already-testable != already-true gap is exactly P2-③'s real_path lesson.
//
// The defect (measured 2026-08-25 by the zoom-transform probe; its FINDINGS are
// recorded in memory, not here, to keep this tree spike-free): the macOS host rendered
// at DESIGN scale (drawScale = design) and let the window-manager CLAMP the height,
// cropping the bottom (1551 - 1410 = 141 logical px on that machine). The design intent
// is fit-to-window (GetScaleForScreen = min(w/h ratios), in LOGICAL px), which shows the
// whole panel scaled to fit. This header captures that decision so a future host consumes
// it instead of clamping.
//
// Transform provenance (from pinned iPlug2, proven by slice ④): design ->[drawScale
// (zoom/fit)]-> logical window ->[mScreenScale (retina)]-> backing. backing =
// design * drawScale * mScreenScale. FIT is a LOGICAL-space decision (drawScale maps
// design->logical); RETINA maps logical->backing and is a SEPARATE multiplier, never
// folded into drawScale, never dropped (slice ④'s whole point). So this header takes
// only LOGICAL areas and does NOT see retina — folding retina in here would re-introduce
// the bug slice ④ proved out. The off-screen FBO being exactly 4800x3102 = 2x logical
// across BOTH axes is the proof the transform itself is correct; the defect is purely
// the host's height choice, which this module fixes at the decision level.
//
// ALL dimensions / mappings are parameters. `141` is NEVER hardcoded — it is the
// visibleFrame of one machine (NSScreen visible height 1410, design 1551). Another
// machine produces a different number; writing 141 would make my machine the spec.
//
// Header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// The single Design Coordinate Space the whole panel is laid out in (design/06 L20,
// design/07 L162: "2400×1551 是唯一 Design Coordinate Space"). This is the product's
// design space, NOT a default physical window size; every function below takes it (or
// an arbitrary design space) as a parameter so the host passes what it needs and no
// dimension is hardwired into a branch. These are conveniences for callers, not a
// branch in the logic.
inline constexpr double kDesignWidth = 2400.0;
inline constexpr double kDesignHeight = 1551.0;

// Tolerance for boundary reachability. Fit is exact in ideal math (designH * fit ==
// availH when height binds), but floating point can land epsilon below/above; treat a
// rect whose edge is within this of the window edge as reachable. Small, relative to a
// 1e3-scale coordinate space.
inline constexpr double kFitEps = 1e-6;

// A rectangle in Design Coordinate Space. Axis-aligned; (x0,y0) top-left,
// (x1,y1) bottom-right.
struct DesignRect {
  double x0 = 0.0, y0 = 0.0;
  double x1 = 0.0, y1 = 0.0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Fit-to-window draw scale, in LOGICAL px. Mirrors the framework's
// GetScaleForScreen(w,h) = min(w/designW, h/designH) (IGraphicsIOS.mm:37): the largest
// scale that keeps the WHOLE design inside the available logical area. This is the
// number the host must use for drawScale. A host that instead uses design-scale (1.0)
// and lets the window-manager clamp is exactly the bug under test.
//
// Pure data: both the design space and the available logical area are passed in.
// The fit scale is the min of the two aspect ratios, so the panel aspect is preserved
// (never non-uniformly stretched) and the whole design fits without cropping.
inline double fit_draw_scale(double designW, double designH,
                             double availLogicalW, double availLogicalH) {
  if (designW <= 0.0 || designH <= 0.0 || availLogicalW <= 0.0 || availLogicalH <= 0.0) {
    return 0.0;  // degenerate input -> no scale; a host must reject this, not clamp
  }
  const double sx = availLogicalW / designW;
  const double sy = availLogicalH / designH;
  const double s = sx < sy ? sx : sy;
  return s < 0.0 ? 0.0 : s;
}

// The window size (in logical px) a fit host should open: design scaled by `drawScale`.
// Draws the panel at design * fit, so it fits the available area rather than being
// clamped. Backing is this times the screen scale (retina), applied by the host layer.
inline void fit_window_logical_size(double designW, double designH, double drawScale,
                                    double& logicalW, double& logicalH) {
  logicalW = designW * drawScale;
  logicalH = designH * drawScale;
}

// Reachability predicate — the heart of the criterion. Is `rect` (in design space)
// fully on-window when rendered at `drawScale` into a window whose actual logical size
// is (logicalW, logicalH)? A design point (x,y) maps to (x*drawScale, y*drawScale); the
// rect is reachable iff the mapped rect lies within [0, logicalW] x [0, logicalH] (with
// epsilon). This is what "can the criterion SEE the bottom 141px is reachable" means:
// it takes the window the HOST ACTUALLY OPENED, so a clamp host (logicalH = availH,
// smaller than designH*drawScale) reports the bottom row as NOT reachable and the
// negative fires, while a fit host (logicalH = designH*fit) reports it reachable.
inline bool design_rect_fully_on_window(const DesignRect& rect, double drawScale,
                                        double logicalW, double logicalH) {
  if (rect.empty() || drawScale <= 0.0 || logicalW <= 0.0 || logicalH <= 0.0) {
    return false;
  }
  const double lx0 = rect.x0 * drawScale, ly0 = rect.y0 * drawScale;
  const double lx1 = rect.x1 * drawScale, ly1 = rect.y1 * drawScale;
  return lx0 >= -kFitEps && ly0 >= -kFitEps &&
         lx1 <= logicalW + kFitEps && ly1 <= logicalH + kFitEps;
}

// Convenience: for a design space and an available logical area, compute the fit
// drawScale and confirm the BOTTOM design row is reachable. This is the single answer a
// host should consult before opening its window. `fullHeightVisible` is true iff the
// whole design height (0..designH) fits — under a correct fit always true; under a
// clamp (designH > availLogicalH) false, which is the red the negative drives at.
struct HostWindowFit {
  double drawScale = 0.0;        // fit scale the host must use
  double logicalW = 0.0;         // designW * drawScale (window logical width)
  double logicalH = 0.0;         // designH * drawScale (window logical height)
  bool fullHeightVisible = false; // designH edge maps within availLogicalH
};

inline HostWindowFit compute_host_window_fit(double designW, double designH,
                                             double availLogicalW, double availLogicalH) {
  HostWindowFit out;
  out.drawScale = fit_draw_scale(designW, designH, availLogicalW, availLogicalH);
  fit_window_logical_size(designW, designH, out.drawScale, out.logicalW, out.logicalH);
  const DesignRect full{0.0, 0.0, designW, designH};
  out.fullHeightVisible =
      design_rect_fully_on_window(full, out.drawScale, availLogicalW, availLogicalH);
  return out;
}

}  // namespace lunar24::core
