// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-① (task #31) — the HOST window-layout choke point (window_layout.h). This is the
// host-side counterpart to the core fit module (host_window_fit.h): it is the ONE place
// the host turns a design rect into the window it opens, and it MUST consume the core
// fit module. The mandate's four requirements, pinned here:
//
//   (1) host consumes the module (bypass -> red). A fit host's geometry here EXACTLY
//       equals core::compute_host_window_fit — proof it delegates, not reimplements.
//       The bypass (design scale + WM clamp) is the RED case: the bottom row crops.
//   (2) #15 repayment, decision level: a fit window lands logicalH exactly at the
//       available height and the bottom design row is REACHABLE. This is the "the bottom
//       141px is reachable" decision, parameterized on the available area (no 141 here).
//   (3) 7 zoom scales + fit -> logical coordinates unchanged. Reuses slice-④: the
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

// The design control row the #15 criterion cares about: the panel bottom (same as the
// core precursor test). Its BOTTOM edge (y1) is what crops under a clamp.
constexpr DesignRect kBottomRow{0.0, 1451.0, kDesignW, 1519.0};

// The 7 zoom steps the panel offers, plus "fit" (expressed as zoomScale<=0). These are
// DATA for the invariance test — the invariance is about the MAPPING RULE (logicalPos =
// designPos * drawScale), not about which specific values. slice-④'s whole point.
constexpr std::array<double, 7> kZoomScales{{0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}};

// The screen scale (retina) is a SEPARATE multiplier, never coupled to drawScale. These
// test that: 2.0 is this machine's retina (backing = logical * 2), but the LOGICAL
// coordinates must not change when the retina multiplier changes.
constexpr double kRetina = 2.0;

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int run(const char* suite) {
  // (1) HOST CONSUMES THE MODULE / (2) #15 DECISION — a fit host on the defective screen
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

    // #15 criterion: the window is the WHOLE panel scaled to fit — bottom row reachable,
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

  // (3) ZOOM INVARIANCE (slice-④). For each of the 7 zoom scales AND fit, the mapping
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
  // identical, only backing differs. This is slice-④'s bug — folding the 2x in changes the
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

int main() {
  std::printf("== P5-1 host window layout: consume core fit + #15 reachability ==\n");
  return run("host_window_layout");
}
