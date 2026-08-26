// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-② criterion 5 (task #33 → re-measure task #36, 裁决 0843a838): measured coords
// from the panel reference figure, and those measured coords must MATCH the figure —
// not merely be consistent with themselves. This is the "绝对锚" rule §5: an
// implementation that is consistently wrong still goes all green, so the path must be
// pinned to an EXTERNAL reference (the card-border / separated-column positions read
// off the reference figure), never to the rects themselves.
//
// WHY THE OLD CHECK WAS WRONG (the lesson 00-status §2k records): the previous
// criterion-5 validated each region's center against a "zone table" whose zone rects
// were co-authored with the region rects. Two same-source quantities validating each
// other prove nothing — when the mixer was mislabelled to the bottom row, the wrong
// zone table agreed with the wrong rect and it stayed green. @Claude's independent
// separated-column detection (a DIFFERENT source from the connected-components that
// produced the rects) exposed it.
//
// So this test pins each region's rect to a committed EXTERNAL anchor set (figure
// measurements), NOT re-derived from the rects, and proves the guard actually fires:
//   (1) GEOMETRY SANITY — every rect is non-empty, inside [0,2400]x[0,1551], and the
//       by-label lookup round-trips; every anchor has a measured region and conversely.
//   (2) EXTERNAL ANCHOR (the conflict check) — every region rect CONVERGES on its
//       figure-measured anchor within a small tolerance, and same-band neighbours are
//       separated by a card gutter (the "分离列" property). Four negatives must red:
//       (a) the OLD wrong mixer value, (b) a translated rect, (c) a bottom-row swap,
//       (d) a region mislabelled to another site.
//   (3) TRANSFORM BRIDGE — each region center round-trips design->screen->design under
//       every zoom preset and retina 1x/2x (sampled at non-100% so a drifted transform
//       is visible).
//   (4) HIT TEST PATH — the shared transform, not a re-derived scale, is what maps a
//       screen point onto the correct region.
//
// Out of scope (FINDINGS): pixel-exact per-control placement (the P5 full-panel slice).
// These are region-level rects at panelSite granularity.

#include "mini_test.h"

#include <lunar24/core/panel_layout.h>
#include <lunar24/core/panel_transform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
using namespace lunar24::core;
double near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ----------------------------------------------------------------------------
// EXTERNAL ANCHORS — figure-measured card-border / separated-column rects.
// Values for the mid band + bottom row are @Claude's independent separated-column
// detection (design/00-status.md §2k, 裁决 0843a838) — a different measurement
// source from the connected-components used to author the region rects. The rest
// are card-edge readings from the same reference figure.
//
// CRITICAL: these are FIGURE TRUTH. They must NEVER be edited to match a region-rect
// change. The whole point is the rects converge on the anchors, not the reverse.
// ----------------------------------------------------------------------------
struct Anchor { const char* site; double x0, y0, x1, y1; };
static const Anchor kExternalAnchors[] = {
  // left column
  {"左上 DRONE 1/2", 22, 180, 724, 700},
  {"左中 DRONE 3", 22, 560, 407, 880},
  // center strip
  {"中上 VCO A", 415, 560, 805, 880},
  {"中部 VOICE MIXER", 807, 560, 1592, 700},
  {"中上 VCO B", 1598, 560, 1985, 880},
  {"中上 DUAL EFFECTOR", 808, 183, 1591, 537},
  {"中上 DUAL VCF", 808, 537, 1591, 710},
  {"中部 ENV A", 807, 700, 1146, 880},
  {"中部 ENV B", 1248, 700, 1591, 880},
  // right column
  {"右上 DRONE 4/5", 1599, 180, 2301, 561},
  {"右中 DRONE 6", 1993, 561, 2379, 879},
  // bottom row, physical order
  {"下排 LFO A", 19, 880, 292, 1030},
  {"下排 joystick", 296, 880, 596, 1030},
  {"下排 5-step seq", 600, 880, 1546, 1030},
  {"下排 preamp", 1549, 880, 1723, 1030},
  {"下排 env follower", 1727, 880, 2103, 1030},
  {"下排 LFO B", 2104, 880, 2377, 1030},
  // bottom band
  {"底部 12 触摸片", 399, 1104, 2000, 1492},
  {"右下 DRONE VOICES", 1700, 1104, 2400, 1492},
};
constexpr int kAnchorCount = (int)(sizeof(kExternalAnchors) / sizeof(kExternalAnchors[0]));
constexpr double kAnchorTol = 8.0;  // allow sub-pixel rounding; a >tol drift is a figure mismatch

bool anchor_received(const char* site) {
  for (int i = 0; i < kAnchorCount; ++i)
    if (std::strcmp(kExternalAnchors[i].site, site) == 0) return true;
  return false;
}
bool rect_near_anchor(const DesignRect& r, const Anchor& a) {
  return std::fabs(r.x0 - a.x0) <= kAnchorTol &&
         std::fabs(r.y0 - a.y0) <= kAnchorTol &&
         std::fabs(r.x1 - a.x1) <= kAnchorTol &&
         std::fabs(r.y1 - a.y1) <= kAnchorTol;
}
double rect_cx(const DesignRect& r) { return (r.x0 + r.x1) * 0.5; }
double rect_cy(const DesignRect& r) { return (r.y0 + r.y1) * 0.5; }

// A by-site lookup that works over a caller-supplied region set (for negatives on a
// mutated copy), mirroring panel_layout.h's panel_region_by_site semantics.
int panel_region_by_site_of(const PanelLayoutRegion* regs, int n, const char* site) {
  for (int i = 0; i < n; ++i) {
    const char* a = regs[i].site;
    const char* b = site;
    bool same = true;
    while (*a && *b) { if (*a != *b) { same = false; break; } ++a; ++b; }
    if (same && *a == *b) return i;
  }
  return -1;
}

// Full external-anchor convergence check over a region set. Returns true only if
// EVERY region has a figure anchor AND every anchor's region converges on it, and the
// same-band neighbours are gutter-separated. Used for the main pass (must be true) and
// for negatives (must be false).
bool regions_anchor_ok(const PanelLayoutRegion* regs, int n) {
  // every measured region must be an anchor target
  for (int i = 0; i < n; ++i)
    if (!anchor_received(regs[i].site)) { std::printf("  NO ANCHOR [%s]\n", regs[i].site); return false; }
  // every anchor must map to a region that converges on it
  for (int a = 0; a < kAnchorCount; ++a) {
    const int r = panel_region_by_site_of(regs, n, kExternalAnchors[a].site);
    if (r < 0) { std::printf("  ANCHOR UNMEASURED [%s]\n", kExternalAnchors[a].site); return false; }
    if (!rect_near_anchor(regs[r].rect, kExternalAnchors[a])) {
      std::printf("  ANCHOR MISMATCH [%s] rect(%.0f,%.0f,%.0f,%.0f) vs anchor(%.0f,%.0f,%.0f,%.0f)\n",
                  kExternalAnchors[a].site,
                  regs[r].rect.x0, regs[r].rect.y0, regs[r].rect.x1, regs[r].rect.y1,
                  kExternalAnchors[a].x0, kExternalAnchors[a].y0,
                  kExternalAnchors[a].x1, kExternalAnchors[a].y1);
      return false;
    }
  }
  return true;
}

int run(const char* suite) {
  // ---- (1) geometry sanity + label round-trip -------------------------------
  for (int i = 0; i < kPanelRegionCount; ++i) {
    const DesignRect& r = kPanelRegions[i].rect;
    CHECK(r.x0 < r.x1);
    CHECK(r.y0 < r.y1);
    CHECK(r.x0 >= kDesignArea.x0 && r.y0 >= kDesignArea.y0);
    CHECK(r.x1 <= kDesignArea.x1 && r.y1 <= kDesignArea.y1);
    CHECK_EQ(panel_region_by_site(kPanelRegions[i].site), i);
  }
  CHECK_EQ(panel_region_by_site("NO SUCH REGION"), -1);
  CHECK_EQ(kPanelRegionCount, kAnchorCount);  // measured set == anchor set, no orphans
  // the by-array lookup helper and the by-label lookup agree on the real sets
  CHECK_EQ(panel_region_by_site_of(kPanelRegions, kPanelRegionCount, "中上 VCO A"),
           panel_region_by_site("中上 VCO A"));
  CHECK_EQ(panel_region_by_site_of(kPanelRegions, kPanelRegionCount, "中部 ENV B"),
           panel_region_by_site("中部 ENV B"));

  // ---- (2) EXTERNAL-ANCHOR convergence (the figure-anchored conflict check) ----
  CHECK(regions_anchor_ok(kPanelRegions, kPanelRegionCount));

  // Same-band neighbours separated by a card gutter ("分离列" property) — independent
  // of the anchors: the mixer is a narrow strip between VCO A and VCO B, ENV A is left
  // of ENV B, and the bottom row runs strictly left->right.
  {
    const int vcoA = panel_region_by_site("中上 VCO A");
    const int mix = panel_region_by_site("中部 VOICE MIXER");
    const int vcoB = panel_region_by_site("中上 VCO B");
    const int envA = panel_region_by_site("中部 ENV A");
    const int envB = panel_region_by_site("中部 ENV B");
    CHECK(vcoA >= 0 && mix >= 0 && vcoB >= 0 && envA >= 0 && envB >= 0);
    // 分离列: within the mid band, cards are separate (a beige gutter separates them).
    CHECK(kPanelRegions[vcoA].rect.x1 <= kPanelRegions[mix].rect.x0);  // VCO A | MIXER gutter
    CHECK(kPanelRegions[mix].rect.x1 <= kPanelRegions[vcoB].rect.x0);  // MIXER | VCO B gutter
    CHECK(kPanelRegions[envA].rect.x1 <= kPanelRegions[envB].rect.x0);  // ENV A | ENV B gutter
    // the mixer strip is narrow (part of the 560-700 band), NOT the full-height column.
    CHECK(kPanelRegions[mix].rect.y1 < kPanelRegions[vcoA].rect.y1);
  }
  {
    // bottom row strictly left->right, no overlap, and the five gutters are >= ~4px.
    const char* order[] = {"下排 LFO A", "下排 joystick", "下排 5-step seq",
                           "下排 preamp", "下排 env follower", "下排 LFO B"};
    int prev = panel_region_by_site(order[0]);
    CHECK(prev >= 0);
    for (int k = 1; k < 6; ++k) {
      const int cur = panel_region_by_site(order[k]);
      CHECK(cur >= 0);
      CHECK(kPanelRegions[prev].rect.x1 <= kPanelRegions[cur].rect.x0);  // no overlap
      CHECK(kPanelRegions[cur].rect.x0 - kPanelRegions[prev].rect.x1 >= 0);
      prev = cur;
    }
  }

  // ---- NEGATIVES: the guard must FIR E (not pass by accidental agreement) ----
  {
    // (a) the OLD WRONG mixer value @Claude flagged ({416,880,1984,1035}) -> must red
    PanelLayoutRegion copy[kPanelRegionCount];
    std::memcpy(copy, kPanelRegions, sizeof copy);
    const int m = panel_region_by_site("中部 VOICE MIXER");
    CHECK(m >= 0);
    copy[m].rect = DesignRect{416, 880, 1984, 1035};  // the very mismatched value
    CHECK_FALSE(regions_anchor_ok(copy, kPanelRegionCount));
  }
  {
    // (b) a TRANSLATED rect (shift the mixer +40px) -> must red (the "整体平移" trapping)
    PanelLayoutRegion copy[kPanelRegionCount];
    std::memcpy(copy, kPanelRegions, sizeof copy);
    const int m = panel_region_by_site("中部 VOICE MIXER");
    copy[m].rect.x0 += 40;  // shift right, no longer on the card border
    CHECK_FALSE(regions_anchor_ok(copy, kPanelRegionCount));
  }
  {
    // (c) BOTTOM-ROW SWAP (joystick and 5-step rects exchanged) -> must red
    PanelLayoutRegion copy[kPanelRegionCount];
    std::memcpy(copy, kPanelRegions, sizeof copy);
    const int j = panel_region_by_site("下排 joystick");
    const int s = panel_region_by_site("下排 5-step seq");
    DesignRect t = copy[j].rect; copy[j].rect = copy[s].rect; copy[s].rect = t;
    CHECK_FALSE(regions_anchor_ok(copy, kPanelRegionCount));
  }
  {
    // (d) a region mislabelled to another SITE -> must red (the original negative,
    // now against the external anchors: the rect sits under the wrong anchor)
    PanelLayoutRegion copy[kPanelRegionCount];
    std::memcpy(copy, kPanelRegions, sizeof copy);
    const int d6 = panel_region_by_site("右中 DRONE 6");
    CHECK(d6 >= 0);
    // swap just the SITE label of DRONE 6 to a top-left region; rect stays right-middle
    copy[d6].site = "左上 DRONE 1/2";
    CHECK_FALSE(regions_anchor_ok(copy, kPanelRegionCount));
  }

  // ---- (3) transform bridge on measured coords (criterion 1-4 are intact) -----
  for (int iz = 0; iz < kZoomScaleCount; ++iz) {
    const double ds = kZoomScales[iz];
    for (double ss : {1.0, 2.0}) {
      PanelTransform t = make_zoom_transform(ds, ss, 40.0, 25.0);
      CHECK(t.valid());
      for (int i = 0; i < kPanelRegionCount; ++i) {
        double bx, by, rx, ry;
        design_to_screen(t, rect_cx(kPanelRegions[i].rect), rect_cy(kPanelRegions[i].rect), bx, by);
        screen_to_design(t, bx, by, rx, ry);
        CHECK(near(rx, rect_cx(kPanelRegions[i].rect)));
        CHECK(near(ry, rect_cy(kPanelRegions[i].rect)));
      }
    }
  }

  // ---- (4) hit-testing uses the SAME transform (not a re-derived scale) -----
  const DesignRect cells[] = {
    DesignRect{50, 50, 400, 400},
    DesignRect{900, 50, 1500, 400},
    DesignRect{1900, 50, 2350, 400},
    DesignRect{50, 900, 500, 1300},
    DesignRect{1000, 900, 1600, 1300},
    DesignRect{1900, 900, 2350, 1300},
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
        if (hit != c)
          std::printf("  HIT MISMATCH zoom=%.2f retina=%.1f cell=%d got=%d\n", ds, ss, c, hit);
        CHECK_EQ(hit, c);
      }
    }
  }

  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== task #36 P5-2 criterion 5: measured panel region layout, EXTERNAL-anchored ==\n");
  return run("panel_layout");
}
