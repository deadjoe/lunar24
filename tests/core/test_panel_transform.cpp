// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-② transform + hit-testing slice (task #33) — the single reversible transform that
// geometry, hit testing, touch plate and patch point/cable all share (design/06 §P5
// first line). This test pins the five mandate criteria and, critically, drives the
// negatives at NON-100% because the real bug @Claude flagged is invisible at design scale.
//
//   C1  ONE transform. geometry (design_to_screen) and hit-testing (screen_to_design)
//       must agree: a design control drawn at screen point S is hit when we ask
//       hit-testing at S. NEGATIVE: a renegade hit-test that recomputes its own scale
//       diverges at non-100% (and is GREEN at 100% — the trap). We assert both.
//   C2  Reversible. screen_to_design(design_to_screen(p)) == p within float tolerance at
//       every zoom + fit, with and without pan, with retina != 1. NEGATIVE: a one-way
//       lossy step (integer truncation of screen coords) loses precision > 1e-3 px.
//   C3  7 zoom levels (50/67/75/100/125/150/200%) + fit-to-window, logical coords &
//       state unchanged (reuse the P1-④ invariant — the design point is the state and it
//       round-trips to itself at every zoom; logical size = design * drawScale).
//   C4  retina = SEPARATE multiplier, never folded into drawScale. design_to_logical
//       depends only on drawScale; logical_to_backing depends only on screenScale.
//       NEGATIVE: a renegade that folds retina into drawScale (design*drawScale*screenScale
//       in the logical step) diverges — and is invisible at retina==1.
//   C5  (measured coords — handled in test_panel_layout.cpp once panel_layout.h lands.)
//
// All mappings are exact inverses (multiply then divide by the SAME fields). A value
// differs from another only when a renegade introduces an independent scale.

#include "mini_test.h"

#include <lunar24/core/host_window_fit.h>
#include <lunar24/core/panel_transform.h>

#include <cmath>
#include <cstdio>

namespace {
using lunar24::core::DesignRect;
using lunar24::core::PanelTransform;
using lunar24::core::fit_draw_scale;
using lunar24::core::kDesignHeight;
using lunar24::core::kDesignWidth;
using lunar24::core::kZoomScaleCount;
using lunar24::core::kZoomScales;

constexpr double kDesignW = kDesignWidth;   // 2400.0
constexpr double kDesignH = kDesignHeight;  // 1551.0

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// ---------------------------------------------------------------------------
// Renegades — each is a DIFFERENT way a host could "compute its own scale". They exist to
// prove the canonical transform is single and that these variants are detectable. Each
// diverges from the canonical result at non-100% and (critically) matches at 100%/retina1.
// ---------------------------------------------------------------------------

// Renegade A (hit-test, criterion 1): drop retina — treat the backing point as if it were
// a logical point, dividing only by drawScale.
inline void renegade_drop_retina(const PanelTransform& t, double bx, double by,
                                 double& dx, double& dy) {
  dx = (bx - t.originX) / t.drawScale;
  dy = (by - t.originY) / t.drawScale;
}

// Renegade B (hit-test, criterion 1): re-derive a fit scale (ignoring the current zoom)
// from a window area, instead of using the transform's drawScale. Only the zoom!=fit case
// exposes it.
inline void renegade_rederive_fit(double fitScale, const PanelTransform& t, double bx,
                                  double by, double& dx, double& dy) {
  double lx = bx / t.screenScale, ly = by / t.screenScale;
  dx = (lx - t.originX) / fitScale;
  dy = (ly - t.originY) / fitScale;
}

// Renegade C (forward, criterion 4): fold retina into drawScale — the logical step
// multiplies by drawScale*screenScale instead of drawScale.
inline void renegade_fold_retina(const PanelTransform& t, double dx, double dy,
                                 double& lx, double& ly) {
  lx = dx * (t.drawScale * t.screenScale) + t.originX;
  ly = dy * (t.drawScale * t.screenScale) + t.originY;
}

void run_roundtrip(const char* label, PanelTransform t, double dx0, double dy0) {
  double bx, by, dx1, dy1;
  lunar24::core::design_to_screen(t, dx0, dy0, bx, by);
  lunar24::core::screen_to_design(t, bx, by, dx1, dy1);
  std::printf("  %-28s design(%.4f,%.4f) -> screen(%.3f,%.3f) -> back(%.7f,%.7f)\n", label,
              dx0, dy0, bx, by, dx1, dy1);
  CHECK(near(dx1, dx0));
  CHECK(near(dy1, dy0));
}

int run(const char* suite) {
  // ---- C1 POSITIVE: geometry & hit-test agree (one transform). A design control drawn at
  // its screen position is hit when we ask hit-testing at that same screen position. ----
  {
    const PanelTransform t =
        lunar24::core::make_zoom_transform(1.5, 2.0, 120.0, 60.0);  // zoom+retina+pan
    // A design control region (measured-anchor style) at the centre of the panel.
    const DesignRect control{1150.0, 725.0, 1250.0, 825.0};
    double cx = (control.x0 + control.x1) / 2.0, cy = (control.y0 + control.y1) / 2.0;
    double bx, by;
    lunar24::core::design_to_screen(t, cx, cy, bx, by);
    // The geometry drew the control centre at (bx,by); hit-testing that pixel must find it.
    const DesignRect rects[1] = {control};
    const int hit = lunar24::core::hit_test_rects(t, bx, by, rects, 1);
    std::printf("C1+ geometry centre (%0.f,%0.f) -> screen(%.2f,%.2f) hit=%d\n", cx, cy, bx,
                by, hit);
    CHECK_EQ(hit, 0);
    // A point just outside the control region is NOT hit (region is design-space test).
    const int miss =
        lunar24::core::hit_test_rects(t, bx + 400.0, by - 400.0, rects, 1);
    CHECK_EQ(miss, -1);
  }

  // ---- C1 NEGATIVE: the renegade is GREEN at 100%/retina1 (the invisible trap) and RED
  // (diverges) at non-100%. We assert BOTH, so the test documents that a second scale
  // introduced by a future hit-tester would be caught only at non-100%. ----
  {
    // Trap: at design scale + retina 1.0, drop-retina agrees with canonical (invisible).
    const PanelTransform t100 = lunar24::core::make_zoom_transform(1.0, 1.0, 0.0, 0.0);
    double bx, by, cx, cy, rx, ry;
    const double px = 1200.0, py = 775.0;
    lunar24::core::design_to_screen(t100, px, py, bx, by);
    lunar24::core::screen_to_design(t100, bx, by, cx, cy);
    renegade_drop_retina(t100, bx, by, rx, ry);
    std::printf("C1- trap: at 100%% retina1 canonical=(%0.f,%0.f) renegade=(%0.f,%0.f)\n", cx,
                cy, rx, ry);
    CHECK(near(cx, rx));   // green here — this is why the bug goes unnoticed at 100%
    CHECK(near(cy, ry));

    // At zoom 1.5 + retina 2.0 the renegade diverges — this is where it must be caught.
    const PanelTransform tzo = lunar24::core::make_zoom_transform(1.5, 2.0, 0.0, 0.0);
    lunar24::core::design_to_screen(tzo, px, py, bx, by);
    lunar24::core::screen_to_design(tzo, bx, by, cx, cy);
    renegade_drop_retina(tzo, bx, by, rx, ry);
    std::printf("C1- fire: zoom1.5 retina2 canonical=(%.3f,%.3f) renegade=(%.3f,%.3f)\n", cx,
                cy, rx, ry);
    CHECK_FALSE(near(cx, rx));
    CHECK_FALSE(near(cy, ry));
  }

  // ---- C1 NEGATIVE (re-derive-fit variant): only fires when the current zoom != fit. ----
  {
    const PanelTransform t = lunar24::core::make_zoom_transform(1.5, 2.0, 0.0, 0.0);
    // A small window whose fit scale is ~0.5 (width-binds) — clearly not the zoom 1.5.
    const double fitScale = fit_draw_scale(kDesignW, kDesignH, 1200.0, 800.0);
    double bx, by, cx, cy, rx, ry;
    const double px = 600.0, py = 775.0;
    lunar24::core::design_to_screen(t, px, py, bx, by);
    lunar24::core::screen_to_design(t, bx, by, cx, cy);
    renegade_rederive_fit(fitScale, t, bx, by, rx, ry);
    std::printf("C1- fire: zoom1.5 fit=%.3f canonicalX=%.3f renegadeX=%.3f\n", fitScale, cx,
                rx);
    CHECK_FALSE(near(cx, rx));
  }

  // ---- C2 POSITIVE: reversibility design->screen->design at every zoom + fit, with and
  // without pan, with retina != 1. ----
  {
    const double px = 1200.0, py = 775.0;
    for (int i = 0; i < kZoomScaleCount; ++i) {
      char label[48];
      std::snprintf(label, sizeof(label), "zoom %.2f retina2 pan0", kZoomScales[i]);
      run_roundtrip(label, lunar24::core::make_zoom_transform(kZoomScales[i], 2.0, 0.0, 0.0),
                    px, py);
    }
    // With a pan AND retina (both nonzero) — the composite must still invert exactly.
    run_roundtrip("zoom1.5 retina3 pan(120,60)",
                  lunar24::core::make_zoom_transform(1.5, 3.0, 120.0, 60.0), px, py);
    // A corner design point (0,0) and the far corner — the extremes of the space.
    run_roundtrip("corner(0,0) retina2 pan0",
                  lunar24::core::make_zoom_transform(1.5, 2.0, 0.0, 0.0), 0.0, 0.0);
    run_roundtrip("corner(2400,1551) retina2 pan0",
                  lunar24::core::make_zoom_transform(1.5, 2.0, 0.0, 0.0), kDesignW, kDesignH);
  }

  // ---- C2 FIT: the fit path uses the existing P1-④ rule (criterion 3: reuse, don't
  // reinvent) and is also reversible. ----
  {
    const double availW = 1600.0, availH = 900.0;
    const PanelTransform t =
        lunar24::core::make_fit_transform(kDesignW, kDesignH, availW, availH, 2.0);
    std::printf("C3 fit drawScale=%.5f logical=%.2fx%.2f\n", t.drawScale,
                kDesignW * t.drawScale, kDesignH * t.drawScale);
    CHECK(near(t.drawScale, fit_draw_scale(kDesignW, kDesignH, availW, availH)));
    // logical size = design * drawScale (the P1-④ invariant), which fits the window.
    // For 1600x900, HEIGHT binds (900/1551=0.580 < 1600/2400=0.667), so logicalH == availH.
    const double hBind = availH / kDesignH;  // 0.58027
    CHECK(near(t.drawScale, hBind));
    CHECK(near(kDesignH * t.drawScale, availH));              // height binds -> logicalH == availH
    CHECK(near(kDesignW * t.drawScale, kDesignW * hBind));    // 2400 * 0.58027 = 1392.65
    // Uniform scale never distorts: aspect ratio of the logical window == design aspect.
    CHECK(near((kDesignW * t.drawScale) / (kDesignH * t.drawScale), kDesignW / kDesignH));
    run_roundtrip("fit(1600x900) retina2",
                  lunar24::core::make_fit_transform(kDesignW, kDesignH, availW, availH, 2.0),
                  1200.0, 775.0);
  }

  // ---- C3: logical coords & STATE (the design point/region) unchanged across zoom. The
  // design coords are the invariant state; only the screen mapping scales. Verify the
  // round-trip lands on the SAME design point at every zoom (state invariant) and the
  // design->logical map is exactly design*drawScale (no origin drift in the logical size).
  {
    const double px = 1180.0, py = 1300.0;  // a bottom-of-panel design point (state)
    for (int i = 0; i < kZoomScaleCount; ++i) {
      const PanelTransform t = lunar24::core::make_zoom_transform(kZoomScales[i], 2.0, 0.0, 0.0);
      double bx, by, rx, ry;
      lunar24::core::design_to_screen(t, px, py, bx, by);
      lunar24::core::screen_to_design(t, bx, by, rx, ry);
      // State (design point) unchanged — the zoom is only a view transform.
      CHECK(near(rx, px));
      CHECK(near(ry, py));
      // Logical size = design * drawScale (P1-④ invariant reused).
      CHECK(near(kDesignW * t.drawScale, kDesignW * kZoomScales[i]));
    }
  }

  // ---- C4 POSITIVE: retina is a SEPARATE multiplier. design_to_logical depends only on
  // drawScale (not screenScale); logical_to_backing depends only on screenScale. ----
  {
    const PanelTransform t = lunar24::core::make_zoom_transform(1.5, 2.0, 30.0, 40.0);
    double lx = 0.0, ly = 0.0;
    lunar24::core::design_to_logical(t, 1000.0, 500.0, lx, ly);
    std::printf("C4+ design_to_logical(1000,500)=(%.2f,%.2f) drawScale=1.5 origin=(30,40)\n",
                lx, ly);
    CHECK(near(lx, 1000.0 * 1.5 + 30.0));
    CHECK(near(ly, 500.0 * 1.5 + 40.0));
    // design_to_logical is INDEPENDENT of screenScale: changing it must not move the result.
    const PanelTransform t2 = lunar24::core::make_zoom_transform(1.5, 7.0, 30.0, 40.0);
    double lx2 = 0.0, ly2 = 0.0;
    lunar24::core::design_to_logical(t2, 1000.0, 500.0, lx2, ly2);
    CHECK(near(lx2, lx));  // identical despite screenScale 2.0 -> 7.0
    CHECK(near(ly2, ly));
    // logical_to_backing depends only on screenScale.
    double bx = 0.0, by = 0.0;
    lunar24::core::logical_to_backing(t, lx, ly, bx, by);
    CHECK(near(bx, lx * 2.0));
    CHECK(near(by, ly * 2.0));
    // The composed design->screen == design->logical THEN logical->backing (retina applied
    // AFTER drawScale, never folded before it).
    double sx = 0.0, sy = 0.0;
    lunar24::core::design_to_screen(t, 1000.0, 500.0, sx, sy);
    CHECK(near(sx, bx));
    CHECK(near(sy, by));
  }

  // ---- C4 NEGATIVE: a renegade that folds retina into drawScale (design*drawScale*
  // screenScale in the LOGICAL step) diverges from design_to_logical at retina != 1, and
  // is green (invisible) at retina == 1. ----
  {
    const PanelTransform t = lunar24::core::make_zoom_transform(1.5, 2.0, 30.0, 40.0);
    double lxCanon, lyCanon, lxFold, lyFold;
    lunar24::core::design_to_logical(t, 1000.0, 500.0, lxCanon, lyCanon);
    renegade_fold_retina(t, 1000.0, 500.0, lxFold, lyFold);
    std::printf("C4- fire: retina2 canonicalLx=%.2f foldedLx=%.2f\n", lxCanon, lxFold);
    CHECK_FALSE(near(lxCanon, lxFold));  // folding retina corrupts logical position
    // And at retina==1 the fold is invisible (green) — the trap.
    const PanelTransform t1 = lunar24::core::make_zoom_transform(1.5, 1.0, 30.0, 40.0);
    double lx1c, ly1c, lx1f, ly1f;
    lunar24::core::design_to_logical(t1, 1000.0, 500.0, lx1c, ly1c);
    renegade_fold_retina(t1, 1000.0, 500.0, lx1f, ly1f);
    CHECK(near(lx1c, lx1f));  // green here — the fold only shows at retina != 1
  }

  // ---- C2 NEGATIVE: a one-way LOSSY step (integer truncation of screen coords) does NOT
  // round-trip — the exact inverse we provide recovers the point, but a rasterizer that
  // floors the backing pixels loses > 1e-3 design px. This proves our inverse is truly
  // reversible and a lossy one would be caught. ----
  {
    const PanelTransform t = lunar24::core::make_zoom_transform(1.5, 2.0, 0.0, 0.0);
    const double px = 1200.7, py = 775.3;
    double bx, by, dxc, dyc, dxl, dyl;
    lunar24::core::design_to_screen(t, px, py, bx, by);
    lunar24::core::screen_to_design(t, bx, by, dxc, dyc);       // exact inverse
    const double bxT = std::floor(bx), byT = std::floor(by);    // lossy screen truncation
    lunar24::core::screen_to_design(t, bxT, byT, dxl, dyl);
    std::printf("C2- lossy: exact=(%.6f,%.6f) truncated=(%.4f,%.4f)\n", dxc, dyc, dxl, dyl);
    CHECK(near(dxc, px));     // our inverse is exact
    CHECK(near(dyc, py));
    CHECK_FALSE(near(dxl, dxc));  // truncation loses sub-pixel — detectable
    CHECK(std::fabs(dxl - dxc) > 1e-3);
  }

  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== P5-② single reversible panel transform + hit-testing ==\n");
  return run("panel_transform");
}
