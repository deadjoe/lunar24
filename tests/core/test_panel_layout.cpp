// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-② criterion 5 (task #33): measured coords from the panel reference figure, and
// those measured coords are consistent with the registry's panelSite region labels.
//
// The measured dataset lives in panel_layout.h (one DesignRect per distinct panelSite
// label, read off the 2400x1551 reference figure). This test pins four things:
//   (1) MEASURED GEOMETRY SANITY — every region rect is non-empty and fully inside the
//       design space [0,2400]x[0,1551]; the by-label lookup round-trips.
//   (2) PANELSITE CONSISTENCY (the conflict check @Claude mandated) — the region's
//       measured center must fall inside the ZONE that its panelSite label's leading
//       token names (中上/左上/中部/右上/右中/下排/底部/右下). A label whose measured
//       position contradicts the zone it names is exactly "必须顶回来" class. A negative
//       forces one region's label to a wrong zone and proves the guard FIRES.
//   (3) TRANSFORM BRIDGE — each measured design point round-trips design->screen->design
//       to ~0 through a zoom+retina transform. The measured coords are stable under the
//       SAME transform geometry/hit-testing use, at every zoom preset and both retina
//       1x and 2x (the sampled-at-non-100% rule from criterion 1: 100%/1x hides the bug).
//   (4) HIT TEST PATH — a set of disjoint design rects, hit-tested through a zoom+retina
//       transform, returns the CORRECT rect for a screen point that inverts into each of
//       them. This is the criterion-1 bridge on the actual hit path: a hit-tester that
//       recomputes a scale of its own (rather than inverting via the transform) drifts off
//       at non-100% and goes red here.
//
// Out of scope (recorded in FINDINGS): pixel-exact per-control placement — the P5 full
// panel-layout slice. These are region-level rects, the panelSite granularity.

#include "mini_test.h"

#include <lunar24/core/panel_layout.h>
#include <lunar24/core/panel_transform.h>

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {
using namespace lunar24::core;
double near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// The zone a panelSite label's leading token (before the first space) names, with the
// MEASURED placement of that zone on the panel. Independent of the per-module label so
// the consistency check is non-tautological.
const char* zone_prefix(const char* site) {
  // return a pointer to the first space position (prefix = leading token).
  while (*site) {
    if (*site == ' ') return site;  // prefix ends here (caller slices by length)
    ++site;
  }
  return site;  // no space -> whole label is the prefix
}
// Exact-match the leading token (up to first space) to a known zone rect.
bool zone_for(const char* site, double& zx0, double& zy0, double& zx1, double& zy1) {
  // prefix slice
  const char* p = site;
  const char* end = zone_prefix(site);
  int len = int(end - p);
  // Compare the leading token against the measured zone table.
  // zone rects measured from the reference figure (the coarse zone the token names).
  struct Z { const char* name; double x0, y0, x1, y1; };
  static const Z zones[] = {
    {"左上", 0, 180, 780, 700},
    {"左中", 0, 560, 480, 880},
    {"中上", 400, 180, 2000, 880},
    {"中部", 400, 880, 2100, 1035},
    {"右上", 1560, 180, 2400, 561},
    {"右中", 1560, 561, 2400, 880},
    {"下排", 0, 880, 2400, 1090},
    {"底部", 0, 1090, 2400, 1520},
    {"右下", 1200, 1090, 2400, 1520},
  };
  for (const Z& z : zones) {
    // match the token by content
    int i = 0;
    while (i < len && z.name[i] && z.name[i] == p[i]) ++i;
    if (i == len && z.name[i] == '\0' && p[i] == ' ') {
      zx0 = z.x0; zy0 = z.y0; zx1 = z.x1; zy1 = z.y1;
      return true;
    }
  }
  return false;
}

double rect_cx(const DesignRect& r) { return (r.x0 + r.x1) * 0.5; }
double rect_cy(const DesignRect& r) { return (r.y0 + r.y1) * 0.5; }
bool rect_hits(const DesignRect& r, double px, double py) {
  return px >= r.x0 && px < r.x1 && py >= r.y0 && py < r.y1;
}

int run(const char* suite) {
  // ---- (1) measured-geometry sanity + label round-trip -----------------------
  for (int i = 0; i < kPanelRegionCount; ++i) {
    const DesignRect& r = kPanelRegions[i].rect;
    CHECK(r.x0 < r.x1);  // non-empty / non-degenerate
    CHECK(r.y0 < r.y1);
    CHECK(r.x0 >= kDesignArea.x0 && r.y0 >= kDesignArea.y0);  // inside design space
    CHECK(r.x1 <= kDesignArea.x1 && r.y1 <= kDesignArea.y1);
    CHECK(panel_region_by_site(kPanelRegions[i].site) == i);  // by-label round-trip
  }
  CHECK_EQ(panel_region_by_site("NO SUCH REGION"), -1);  // a miss is a real mismatch

  // ---- (2) panelSite consistency conflict check ------------------------------
  for (int i = 0; i < kPanelRegionCount; ++i) {
    const char* site = kPanelRegions[i].site;
    double zx0, zy0, zx1, zy1;
    CHECK(zone_for(site, zx0, zy0, zx1, zy1));  // every label's prefix is a known zone
    const double cx = rect_cx(kPanelRegions[i].rect);
    const double cy = rect_cy(kPanelRegions[i].rect);
    // The measured position must fall in the zone the label's prefix names. This is the
    // conflict check: a label whose measured coords contradict its zone is escalated.
    const bool inside = cx >= zx0 && cx < zx1 && cy >= zy0 && cy < zy1;
    if (!inside) {
      std::printf("  CONFLICT [%s] measured center (%.0f,%.0f) outside zone (%s)\n",
                  site, cx, cy, site);
    }
    CHECK(inside);
  }

  // Negative: force one region's label to a clearly-wrong zone and confirm the guard
  // FIRES (负控必须真会红). drone_6 (right-middle) mislabelled 左上 -> measured center
  // (2186,720) is NOT in the 左上 zone (x<=780), so it must red.
  {
    double zx0, zy0, zx1, zy1;
    const int d6 = panel_region_by_site("右中 DRONE 6");
    CHECK(d6 >= 0);
    CHECK(zone_for("左上 DRONE 1/2" /* wrong zone for DRONE 6 */, zx0, zy0, zx1, zy1));
    const double cx = rect_cx(kPanelRegions[d6].rect);
    const double cy = rect_cy(kPanelRegions[d6].rect);
    const bool inside_wrong_zone = cx >= zx0 && cx < zx1 && cy >= zy0 && cy < zy1;
    CHECK_FALSE(inside_wrong_zone);  // DRONE 6 measured at right-middle is NOT top-left
    // and the CORRECT zone for DRONE 6 must contain it (so the failure is the label,
    // not the measurement).
    double wx0, wy0, wx1, wy1;
    CHECK(zone_for("右中 DRONE 6", wx0, wy0, wx1, wy1));
    CHECK(rect_hits(DesignRect{wx0, wy0, wx1, wy1}, cx, cy));
  }

  // ---- (3) transform bridge on measured coords ------------------------------
  for (int iz = 0; iz < kZoomScaleCount; ++iz) {
    const double ds = kZoomScales[iz];
    for (double ss : {1.0, 2.0}) {  // retina 1x and 2x
      PanelTransform t = make_zoom_transform(ds, ss, 40.0, 25.0);  // a non-zero pan
      CHECK(t.valid());
      for (int i = 0; i < kPanelRegionCount; ++i) {
        const double dx = rect_cx(kPanelRegions[i].rect);
        const double dy = rect_cy(kPanelRegions[i].rect);
        double bx, by, rx, ry;
        design_to_screen(t, dx, dy, bx, by);
        screen_to_design(t, bx, by, rx, ry);  // exact inverse
        // measured coords round-trip to the same design point (reversibility on the
        // measured dataset, at every zoom/retina).
        CHECK(near(rx, dx));
        CHECK(near(ry, dy));
      }
    }
  }

  // ---- (4) hit-testing uses the SAME transform (not a re-derived scale) -----
  // A disjoint set of design rects across the panel; each is hit-tested after the
  // screen point is produced by the SAME transform. Non-100% + retina 2x means a
  // hit-tester that recomputes its own scale (ignores retina / re-derives fit / drops
  // the zoom) lands on the WRONG rect and goes red.
  const DesignRect cells[] = {
    DesignRect{50, 50, 400, 400},     // top-left
    DesignRect{900, 50, 1500, 400},   // top-center
    DesignRect{1900, 50, 2350, 400},  // top-right
    DesignRect{50, 900, 500, 1300},   // bottom-left
    DesignRect{1000, 900, 1600, 1300},  // bottom-center
    DesignRect{1900, 900, 2350, 1300},  // bottom-right
  };
  const int nCells = 6;
  for (int iz = 0; iz < kZoomScaleCount; ++iz) {
    const double ds = kZoomScales[iz];
    for (double ss : {1.0, 2.0, 1.5}) {
      PanelTransform t = make_zoom_transform(ds, ss, 10.0, 7.0);
      for (int c = 0; c < nCells; ++c) {
        double bx, by;
        design_to_screen(t, rect_cx(cells[c]), rect_cy(cells[c]), bx, by);
        const int hit = hit_test_rects(t, bx, by, cells, nCells);
        if (hit != c) {
          std::printf("  HIT MISMATCH zoom=%.2f retina=%.1f cell=%d got=%d\n",
                      ds, ss, c, hit);
        }
        CHECK_EQ(hit, c);
      }
    }
  }

  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== task #33 P5-2 criterion 5: measured panel region layout + panelSite consistency ==\n");
  return run("panel_layout");
}
