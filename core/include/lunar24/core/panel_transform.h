// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-② — the single reversible panel transform + hit-testing, task #33.
//
// This is the ONE transform the whole panel hangs on. design/06 §P5 first line is the
// mandate verbatim: "geometry、hit testing、touch plate、patch point/cable 共用一个可逆
// transform"。 The reason it must be singular is the failure mode @Claude called out
// (mandate, criterion 1): if geometry and hit-testing each keep their own scale, the
// panel LOOKS right but clicks are off — and that is INVISIBLE at 100% zoom (design
// scale), only surfacing at any other scale. So this slice builds the transform as an
// object that BOTH paths take, and the test drives the invariant at non-100% so the
// divergence would actually be caught.
//
// Transform provenance (locked by slice ④ / #31, kept in host_window_fit.h, never
// re-derived here):
//   design ->[drawScale (fit or zoom) + origin pan]-> logical ->[screenScale (retina)]-> backing
//   backing = design * drawScale * screenScale
// FIT / ZOOM is a LOGICAL-space decision (drawScale maps design->logical). RETINA maps
// logical->backing and is a SEPARATE multiplier (screenScale), NEVER folded into
// drawScale and NEVER dropped. This header keeps the two as distinct fields and exposes
// design_to_logical / logical_to_backing as separate steps so the separation is directly
// testable — folding retina into drawScale shows up as logical_to_backing depending on
// drawScale, which is the bug slice ④ proved out and which criterion 4 forbids.
//
// Coordinates are MEASURED (design/03 §3: the reference PNG/PDF is measurement-only, not
// a background). Design space is the canonical 2400×1551 (host_window_fit.h); measured
// control/module positions live in panel_layout.h as DESIGN rects — the invariant state.
// Geometry draws by mapping a design rect to screen; hit-testing maps a screen point back
// to design (the exact inverse) and answers point-in-design-rect. Both use the SAME
// transform, so a control is always hit where it is drawn.
//
// Reversibility: design->screen->design round-trips to the original design point within
// float tolerance. This is the algebraic inverse (multiply then divide by the SAME
// fields), so it is exact up to ~1 ULP; it is NOT a one-way projection or an integer
// truncation.
//
// Header-only, no heap, no locks, realtime-safe. Pure geometry: no windowing, no platform
// API, no iPlug2/IGraphics type.

#pragma once

#include <lunar24/core/host_window_fit.h>  // DesignRect, kDesignWidth/Height, fit_draw_scale

#include <cmath>

namespace lunar24::core {

// The discrete zoom draw-scales the UI offers (design/03 §6.3, mandate criterion 3):
// 50/67/75/100/125/150/200%. A percentage / 100. These are DATA (the product's zoom
// presets), not a branch in the logic.
inline constexpr double kZoomScales[] = {
    0.50, 0.67, 0.75, 1.00, 1.25, 1.50, 2.00,
};
inline constexpr int kZoomScaleCount = 7;

// The single panel transform. drawScale maps design->logical (fit or an explicit zoom);
// screenScale maps logical->backing (retina) and is ALWAYS separate; originX/originY are
// a LOGICAL-space pan (where design-space (0,0) lands). All four paths (geometry,
// hit-testing, touch plate, patch point) reference one instance of this.
struct PanelTransform {
  double drawScale = 1.0;    // design -> logical (zoom/fit). NEVER includes retina.
  double screenScale = 1.0;  // logical -> backing (retina multiplier). ALWAYS separate.
  double originX = 0.0;      // logical x of design-space (0,0) — canvas pan.
  double originY = 0.0;      // logical y of design-space (0,0) — canvas pan.

  bool valid() const {
    return std::isfinite(drawScale) && std::isfinite(screenScale) && drawScale > 0.0 &&
           screenScale > 0.0 && std::isfinite(originX) && std::isfinite(originY);
  }
};

// design -> logical: uses ONLY drawScale + origin (never screenScale). This is the step
// slice ④ pins retina OUT of; a renegade that multiplies screenScale in here is the bug.
inline void design_to_logical(const PanelTransform& t, double dx, double dy,
                              double& lx, double& ly) {
  lx = dx * t.drawScale + t.originX;
  ly = dy * t.drawScale + t.originY;
}

// logical -> backing: uses ONLY screenScale (retina), the separate multiplier. It never
// sees drawScale.
inline void logical_to_backing(const PanelTransform& t, double lx, double ly,
                               double& bx, double& by) {
  bx = lx * t.screenScale;
  by = ly * t.screenScale;
}

// design -> backing: the full single forward mapping. Composition of the two steps above;
// both step functions are public so the retina separation is observable/testable.
inline void design_to_screen(const PanelTransform& t, double dx, double dy,
                             double& bx, double& by) {
  double lx, ly;
  design_to_logical(t, dx, dy, lx, ly);
  logical_to_backing(t, lx, ly, bx, by);
}

// backing -> logical: exact inverse of logical_to_backing.
inline void backing_to_logical(const PanelTransform& t, double bx, double by,
                               double& lx, double& ly) {
  lx = bx / t.screenScale;
  ly = by / t.screenScale;
}

// logical -> design: exact inverse of design_to_logical.
inline void logical_to_design(const PanelTransform& t, double lx, double ly,
                              double& dx, double& dy) {
  dx = (lx - t.originX) / t.drawScale;
  dy = (ly - t.originY) / t.drawScale;
}

// backing -> design: the single inverse of design_to_screen. This is what hit-testing
// MUST call; a hit-tester that recomputes a scale of its own (drops or folds retina, or
// re-derives fit instead of using the current zoom) is exactly the renegade under test.
inline void screen_to_design(const PanelTransform& t, double bx, double by,
                             double& dx, double& dy) {
  double lx, ly;
  backing_to_logical(t, bx, by, lx, ly);
  logical_to_design(t, lx, ly, dx, dy);
}

// Rect variants: forward maps a design rect to a screen rect via the SAME transform;
// inverse maps a screen rect back to design. They use design_to_screen/screen_to_design
// on the two corners, so they never carry their own scale.
inline DesignRect design_rect_to_screen(const PanelTransform& t, const DesignRect& d) {
  double x0, y0, x1, y1;
  design_to_screen(t, d.x0, d.y0, x0, y0);
  design_to_screen(t, d.x1, d.y1, x1, y1);
  // Normalise (a pan or a negative-ish scale would otherwise flip the corners).
  return DesignRect{x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 : x0,
                    y0 < y1 ? y1 : y0};
}

inline DesignRect screen_rect_to_design(const PanelTransform& t, const DesignRect& s) {
  double x0, y0, x1, y1;
  screen_to_design(t, s.x0, s.y0, x0, y0);
  screen_to_design(t, s.x1, s.y1, x1, y1);
  return DesignRect{x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 : x0,
                    y0 < y1 ? y1 : y0};
}

// A design rect contains a design point (using half-open right/bottom, consistent with a
// click region). This is the DESIGN-space test hit-testing does after the inverse.
inline bool design_rect_hit(const DesignRect& r, double px, double py) {
  return !r.empty() && px >= r.x0 && px < r.x1 && py >= r.y0 && py < r.y1;
}

// Highest-priority hit test: given a screen point, invert it to design, then return the
// index of the first design rect that contains it, or -1. This is the ONE hit path —
// geometry and hit-testing both go through the transform, so a control is always hit
// where it is drawn.
inline int hit_test_rects(const PanelTransform& t, double bx, double by,
                          const DesignRect* rects, int n) {
  double dx, dy;
  screen_to_design(t, bx, by, dx, dy);
  for (int i = 0; i < n; ++i) {
    if (design_rect_hit(rects[i], dx, dy)) return i;
  }
  return -1;
}

// Fit-to-window transform: drawScale = fit_draw_scale (delegated to the existing P1-④ /
// #15 module — we do NOT reinvent the fit rule). screenScale (retina) stays separate;
// origin defaults to 0 (no pan on a fit). This is the construction the host uses when
// it wants the whole panel visible.
inline PanelTransform make_fit_transform(double designW, double designH,
                                         double availLogicalW, double availLogicalH,
                                         double screenScale) {
  PanelTransform t;
  t.drawScale = fit_draw_scale(designW, designH, availLogicalW, availLogicalH);
  t.screenScale = screenScale;
  t.originX = 0.0;
  t.originY = 0.0;
  return t;
}

// Explicit-zoom transform: drawScale = the given zoom (a kZoomScales[] entry). origin is
// the pan; defaults to 0. screenScale (retina) separate.
inline PanelTransform make_zoom_transform(double drawScale, double screenScale,
                                          double originX, double originY) {
  PanelTransform t;
  t.drawScale = drawScale;
  t.screenScale = screenScale;
  t.originX = originX;
  t.originY = originY;
  return t;
}

}  // namespace lunar24::core
