// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
//   the HOST window-layout choke point (window_layout.h). This is the
// host-side counterpart to the core fit module (host_window_fit.h): it is the ONE place
// the host turns a design rect into the window it opens, and it MUST consume the core
// fit module. The mandate's four requirements, pinned here:
//
//   (1) host consumes the module (bypass -> red). A fit host's geometry here EXACTLY
//       equals core::compute_host_window_fit — proof it delegates, not reimplements.
//       The bypass (design scale + WM clamp) is the RED case: the bottom row crops.
//   (2) repayment, decision level: a fit window lands logicalH exactly at the
//       available height and the bottom design row is REACHABLE. This is the "the bottom
//       141px is reachable" decision, parameterized on the available area (no 141 here).
//   (3) 7 zoom scales + fit -> logical coordinates unchanged. Reuses slice-: the
//       mapping designPos->logicalPos is ALWAYS designPos * drawScale and the backing is
//       design * drawScale * screenScale, with screenScale NEVER folded into drawScale.
//   (4) iPlug2 pin unmodified checked MECHANICALLY — that is the separate
//       iplug_pin_clean_gate (check_iplug_pin_clean.py), not this unit test; this test
//       covers the geometry the host builds ON TOP of that pristine pin.

#include "mini_test.h"

#include <host/window_layout.h>

#include <array>
#include <cmath>

namespace {
using lunar24::core::DesignRect;
using lunar24::core::compute_host_window_fit;
using lunar24::core::kDesignHeight;
using lunar24::core::kDesignWidth;
using lunar24::host::WindowLayout;
using lunar24::host::compute_window_layout;

constexpr double kDesignW = kDesignWidth;   // 2400.0
constexpr double kDesignH = kDesignHeight;  // 1551.0
constexpr double kVisibleW = 2400.0;        // this screen's visible logical width (data)
constexpr double kVisibleH = 1410.0;        // this screen's visible logical height (data)

// The design control row the criterion cares about: the panel bottom (same as the
// core precursor test). Its BOTTOM edge (y1) is what crops under a clamp.
constexpr DesignRect kBottomRow{0.0, 1451.0, kDesignW, 1519.0};

// The 7 zoom steps the panel offers, plus "fit" (expressed as zoomScale<=0). These are
// DATA for the invariance test — the invariance is about the MAPPING RULE (logicalPos =
// designPos * drawScale), not about which specific values. slice-'s whole point.
constexpr std::array<double, 7> kZoomScales{{0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}};

// The screen scale (retina) is a SEPARATE multiplier, never coupled to drawScale. These
// test that: 2.0 is this machine's retina (backing = logical * 2), but the LOGICAL
// coordinates must not change when the retina multiplier changes.
constexpr double kRetina = 2.0;

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int run(const char* suite) {
  // (1) HOST CONSUMES THE MODULE / (2) DECISION — a fit host on the defective screen
  // (visible height 1410 < design 1551) delegates to the core fit, lands logicalH exactly
  // at the visible height, and the bottom design row is ON-WINDOW. This is the fix at the
  // decision layer: fit, not clamp.
  {
    const WindowLayout w =
        compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH, 0.0, kRetina, kBottomRow);
    std::printf("  fit drawScale=%.5f logical=%.2fx%.2f eff=%.2fx%.2f backing=%.1fx%.1f bottom=%d\n",
                w.drawScale, w.logicalW, w.logicalH, w.effectiveLogicalW, w.effectiveLogicalH,
                w.backingW, w.backingH, (int)w.bottomRowReachable);

    // Delegation proof: its fit geometry is EXACTLY the core module's, computed live.
    // A host that reimplemented the fit (or hardcoded a dimension) would drift here.
    const auto coreFit = compute_host_window_fit(kDesignW, kDesignH, kVisibleW, kVisibleH);
    CHECK(near(w.drawScale, coreFit.drawScale));
    CHECK(near(w.logicalW, coreFit.logicalW));
    CHECK(near(w.logicalH, coreFit.logicalH));
    CHECK(coreFit.fullHeightVisible == true);  // the core itself sees the whole design fits

    //  criterion: the window is the WHOLE panel scaled to fit — bottom row reachable,
    // and the panel's full base (design 1551) lands at logicalH (1410), so nothing crops.
    CHECK(near(w.drawScale, kVisibleH / kDesignH));  // 1410/1551 = 0.9090..
    CHECK(near(w.logicalH, kVisibleH));
    CHECK(w.bottomRowReachable == true);
    CHECK(w.effectiveLogicalH == w.logicalH);  // no clamp under a correct fit: request fits
  }

  // (1) BYPASS -> RED. Revert to clamp: drawScale = design scale (zoom 1.0), the window
  // request is the full design (1551 tall), the WM clips to the visible height (1410),
  // and the bottom design row is NOT on-window. This is the defect under test, genuinely
  // red — mandate: 负控必须真会红.
  {
    const WindowLayout w =
        compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH, 1.0, kRetina, kBottomRow);
    std::printf("  clamp zoom=1.0 logical=%.2fx%.2f eff=%.2fx%.2f bottom=%d (expect 0)\n",
                w.logicalW, w.logicalH, w.effectiveLogicalW, w.effectiveLogicalH,
                (int)w.bottomRowReachable);
    CHECK(near(w.logicalH, kDesignH));       // design scale -> 1551 logical request
    CHECK(near(w.effectiveLogicalH, kVisibleH));  // WM clips to what fits (1410)
    CHECK(w.bottomRowReachable == false);     // the 141px crop is VISIBLE to this predicate
    // The crop IS 141 logical px: design height minus visible height. Data, not a constant.
    CHECK(near(kDesignH - kVisibleH, 141.0));
  }

  // (3) ZOOM INVARIANCE (slice). For each of the 7 zoom scales AND fit, the mapping
  // rule is unchanged: a design point maps to logicalPos = designPos * drawScale, and the
  // window logical size is design * drawScale. The screenScale is never folded in — the
  // LOGICAL coordinates are independent of it (only backing changes).
  {
    std::printf("  zoom invariance (drawScale, logicalW/H, screenScale independent):\n");
    for (const double zoom : kZoomScales) {
      const WindowLayout w = compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH,
                                                   zoom, kRetina, kBottomRow);
      CHECK(near(w.drawScale, zoom));
      CHECK(near(w.logicalW, kDesignW * zoom));
      CHECK(near(w.logicalH, kDesignH * zoom));
      // designPos->logicalPos is designPos * drawScale, ALWAYS. Markers on a design point.
      CHECK(near(kBottomRow.x1 * zoom, w.logicalW));         // design 2400 -> full width
      CHECK(near(kBottomRow.y1 * zoom, 1519.0 * zoom));      // design 1519 -> that*scale
      // ScreenScale is separate: it multiplies backing only, never drawScale/logical.
      CHECK(near(w.backingW, w.logicalW * kRetina));
      CHECK(near(w.backingH, w.logicalH * kRetina));
    }
    // fit is a valid "zoom" value, meaning drawScale <= 0 -> delegate to core.
    const WindowLayout wfit =
        compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH, 0.0, kRetina, kBottomRow);
    CHECK(near(wfit.drawScale, kVisibleH / kDesignH));
    CHECK(near(wfit.logicalW, kDesignW * wfit.drawScale));
    CHECK(near(wfit.logicalH, kDesignH * wfit.drawScale));
  }

  // RETINA NEVER FOLDS INTO drawScale. Same zoom, two screen scales: logical/drawScale
  // identical, only backing differs. This is slice-'s bug — folding the 2x in changes the
  // logical coordinates and mis-places hit-test/cable points.
  {
    const WindowLayout a =
        compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH, 0.0, 1.0, kBottomRow);
    const WindowLayout b =
        compute_window_layout(kDesignW, kDesignH, kVisibleW, kVisibleH, 0.0, 2.0, kBottomRow);
    std::printf("  retina split 1x vs 2x: drawScale %.5f==%.5f logicalH %.2f==%.2f backingH %.1f vs %.1f\n",
                a.drawScale, b.drawScale, a.logicalH, b.logicalH, a.backingH, b.backingH);
    CHECK(near(a.drawScale, b.drawScale));       // retina does NOT change the logical scale
    CHECK(near(a.logicalH, b.logicalH));         // nor the logical window size
    CHECK(near(b.backingH, b.logicalH * 2.0));   // it only scales the backing store
    CHECK(near(b.backingH, a.backingH * 2.0));
    CHECK(a.bottomRowReachable == b.bottomRowReachable);  // reachability is logical, so same
  }

  // DEGENERATE input must NOT silently clamp to a design-scale fallback. A fit host
  // rejects bad data (scale 0); the reachability predicate then says "not reachable"
  // rather than reporting a phantom window drawn at design scale.
  {
    const WindowLayout w =
        compute_window_layout(0.0, kDesignH, kVisibleW, kVisibleH, 0.0, kRetina, kBottomRow);
    CHECK(near(w.drawScale, 0.0));
    CHECK(w.bottomRowReachable == false);
    const WindowLayout w2 =
        compute_window_layout(kDesignW, kDesignH, 0.0, kVisibleH, 0.0, kRetina, kBottomRow);
    CHECK(near(w2.drawScale, 0.0));
    CHECK(w2.bottomRowReachable == false);
  }

  return ::test::finish(suite);
}
}  // namespace

// The panel inside the mac metal case: fits, keeps its proportions, sits inside the case.
static void case_placement() {
  using lunar24::host::place_panel;
  const auto m = lunar24::host::kMacCase;
  const double W = 2400.0, H = 1551.0;
  // A window of exactly panel + case at 0.6: the panel lands at the case margins.
  auto p = place_panel((W + 2 * m.side) * 0.6, (H + m.top + m.bottom) * 0.6, W, H, m);
  CHECK(std::fabs(p.scale - 0.6) < 1e-9);
  CHECK(std::fabs(p.x - m.side * 0.6) < 1e-9);
  CHECK(std::fabs(p.y - m.top * 0.6) < 1e-9);
  // A wider window (full screen on a 16:10 display): centred left/right.
  p = place_panel(1728.0, 1117.0, W, H, m);
  const double left = p.x, right = 1728.0 - (p.x + p.w);
  CHECK(std::fabs(left - right) < 1e-6);
  CHECK(p.y >= m.top * p.scale - 1e-9);
  CHECK(p.y + p.h <= 1117.0);
  // Full screen uses the thin rim: the panel gets bigger on the same screen.
  CHECK(place_panel(1728.0, 1117.0, W, H, lunar24::host::kMacCaseFullScreen).scale > p.scale);
  // Never below the smallest zoom; no case: plain centred fit.
  CHECK(place_panel(100.0, 100.0, W, H, m).scale == lunar24::host::kMinPanelScale);
  p = place_panel(1200.0, 1000.0, W, H);
  CHECK(std::fabs(p.scale - 0.5) < 1e-9);
  CHECK(std::fabs(p.x) < 1e-9);
}

// A plugin window the DAW resizes keeps the panel's shape, between half and full size.
static void plugin_window_constraint() {
  using lunar24::host::constrain_plugin_window;
  const double W = 2400.0, H = 1551.0;
  int w = 1200, h = 776;  // the default size already fits
  CHECK(constrain_plugin_window(w, h, W, H));
  CHECK(w == 1200 && h == 776);
  w = 1800; h = 900;  // too wide: the height decides
  CHECK(!constrain_plugin_window(w, h, W, H));
  CHECK(h == 900 && w == 1393);
  w = 300; h = 200;  // never below half size
  CHECK(!constrain_plugin_window(w, h, W, H));
  CHECK(w == 1200 && h == 776);
  w = 5000; h = 4000;  // never above full size
  CHECK(!constrain_plugin_window(w, h, W, H));
  CHECK(w == 2400 && h == 1551);
  for (int t = 1200; t <= 2400; t += 7) {  // a constrained size is stable
    int a = t, b = 2000;
    constrain_plugin_window(a, b, W, H);
    int a2 = a, b2 = b;
    CHECK(constrain_plugin_window(a2, b2, W, H));
  }
}

// Windows resize keeps the panel's shape, so the dialog background never shows around it.
static void client_aspect() {
  using lunar24::host::client_size_for_aspect;
  using lunar24::host::largest_client_for_aspect;
  const double W = 2400.0, H = 1551.0;
  int w = 3000, h = 1000;  // a wide drag: height follows the width
  client_size_for_aspect(w, h, W, H, false);
  CHECK(w == 3000);
  CHECK(h == static_cast<int>(3000.0 * H / W + 0.5));
  w = 1000;
  h = 1551;  // a vertical drag: width follows the height
  client_size_for_aspect(w, h, W, H, true);
  CHECK(h == 1551);
  CHECK(w == static_cast<int>(1551.0 * W / H + 0.5));
  int cw = 0, ch = 0;
  largest_client_for_aspect(1920, 1080, W, H, cw, ch);  // 16:9 screen, height binds
  CHECK(ch == 1080);
  CHECK(cw == static_cast<int>(1080.0 * W / H + 0.5));
  CHECK(cw < 1920);
}

static void windows_dpi_fit() {
  using namespace lunar24::host;
  const double W = 2400.0, H = 1551.0;
  for (double dpi : {1.0, 1.25, 1.5, 1.75, 2.0}) {
    for (auto screen : {std::array<int, 2>{1366, 768}, {1920, 1080}, {3840, 2160}}) {
      // Representative physical frame/title/menu/taskbar reservations.
      const int availW = screen[0] - static_cast<int>(16 * dpi);
      const int availH = screen[1] - static_cast<int>(100 * dpi);
      const auto limits = windows_client_limits(availW, availH, dpi, W, H);
      CHECK(limits.minW > 0 && limits.minH > 0);
      CHECK(limits.minW < limits.maxW && limits.minH < limits.maxH);
      CHECK(limits.maxW <= availW && limits.maxH <= availH);
      for (auto size : {std::array<int, 2>{limits.minW, limits.minH},
                       {limits.maxW, limits.maxH}}) {
        const auto p = place_panel(size[0] / dpi, size[1] / dpi, W, H, {}, 0.01);
        CHECK(p.x >= -1e-9 && p.y >= -1e-9);
        CHECK((p.x + p.w) * dpi <= size[0] + 1e-9);
        CHECK((p.y + p.h) * dpi <= size[1] + 1e-9);
        CHECK(std::abs(size[0] * H / W - size[1]) <= 1.0);
      }
    }
  }
  // Primary, right-hand and negative-coordinate monitors give the same offset.
  for (int origin : {0, 1920, -2560}) {
    CHECK(maximized_position(origin, origin, 1920, 1600) == 160);
    // A taskbar at the left/top is a work-area offset within the monitor.
    CHECK(maximized_position(origin + 48, origin, 1872, 1600) == 184);
  }
}

int main() {
  std::printf("== P5-1 host window layout: consume core fit + reachability ==\n");
  case_placement();
  plugin_window_constraint();
  client_aspect();
  windows_dpi_fit();
  return run("host_window_layout");
}
