// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host window-layout choke point (P5-①, task #31). This is the ONE place the host
// turns a Design Coordinate Space rect into the window (logical) space it actually
// opens. It DELEGATES the fit decision and the reachability predicate to the
// framework-free geometry module
//   lunar24::core::compute_host_window_fit / design_rect_fully_on_window
// (host_window_fit.h), so it never reimplements the transform and never hardcodes a
// dimension. A host that computes its own size WITHOUT this choke point is exactly the
// clamp defect under test (mandate: bypass -> red): it reverts to design scale and lets
// the window-manager crop the bottom, which the reachability predicate then sees.
//
// Transform provenance (proven by slice ④, kept in the hammered-down chain):
//   design ->[drawScale (fit or zoom)]-> logical window ->[screenScale (retina)]-> backing.
//   backing = design * drawScale * screenScale.
// FIT / ZOOM is a LOGICAL-space decision (drawScale maps design->logical). RETINA maps
// logical->backing and is a SEPARATE multiplier, NEVER folded into drawScale and never
// dropped. This header takes the screenScale it is given and multiplies it at the end;
// it never derives a screen scale from a drawScale (that is ④'s bug).
//
// Reachability is tested against the window the host ACTUALLY opens, i.e. its request
// CLAMPED to the available logical area (the window-manager clips a request larger than
// the screen). Under a correct fit the request lands exactly at the available height, so
// nothing crops; under a clamp/oversized zoom the request exceeds it, so the bottom of
// the panel is not reachable and the negative fires.
//
// Header-only, no heap, no locks, realtime-safe. This header is pure (links only
// lunar24::core); it is the PUBLIC geometry surface of lunar_host. The GUI bootstrap
// (host/main.cpp) is separate and is where platform/framework code lives — never here,
// so core/ and this header both stay framework-free.

#pragma once

#include <algorithm>  // std::min

#include <lunar24/core/host_window_fit.h>

namespace lunar24::host {

// Window geometry a fit-to-screen / zoom host should open. `drawScale` maps
// design->logical; `screenScale` maps logical->backing (retina). The two are ALWAYS
// separate (slice-④). Backing is the on-screen buffer size.
struct WindowLayout {
  double drawScale = 0.0;         // design->logical (fit or zoom)
  double screenScale = 1.0;       // logical->backing (retina multiplier, never coupled)
  double logicalW = 0.0;          // designW * drawScale
  double logicalH = 0.0;          // designH * drawScale (the window's requested logical height)
  double backingW = 0.0;          // logicalW * screenScale
  double backingH = 0.0;          // logicalH * screenScale
  double effectiveLogicalW = 0.0; // logicalW clamped to available width (what the WM shows)
  double effectiveLogicalH = 0.0; // logicalH clamped to available height
  bool bottomRowReachable = false; // the design bottom-row rect fully on the open window
};

// Choke point. Derive the host window geometry for a design space (designW/designH), an
// available LOGICAL area (availLogicalW/availLogicalH), and the current zoom. `bottomRow`
// is the design-space control rect the #15 criterion cares about (the panel bottom).
//
// `zoomScale`:
//   <= 0.0 -> fit-to-window: delegate to lunar24::core::compute_host_window_fit.
//   >  0.0 -> explicit zoom drawScale (e.g. 1.0 = design scale; >1 zooms in).
// Passing <=0 is the only way to get a fit and it MUST come from the core module — a
// host that passes 1.0 (design scale) is the clamp/bypass and goes red on reachability.
//
// `screenScale` (retina) is passed in and used ONLY at the backing step; it is never
// folded into drawScale and never recomputed from it.
inline WindowLayout compute_window_layout(double designW, double designH,
                                          double availLogicalW, double availLogicalH,
                                          double zoomScale, double screenScale,
                                          const core::DesignRect& bottomRow) {
  WindowLayout out;

  // The fit draw scale ALWAYS comes from the core module. An explicit zoom is the same
  // design*scale rule, just a different scale value — this header never invents a scale,
  // it only either delegates to core (fit) or applies an already-chosen zoom.
  const double fitScale =
      core::fit_draw_scale(designW, designH, availLogicalW, availLogicalH);
  out.drawScale = (zoomScale <= 0.0) ? fitScale : zoomScale;

  // Window logical size = design * drawScale, by the core rule.
  core::fit_window_logical_size(designW, designH, out.drawScale, out.logicalW, out.logicalH);

  // Retina maps logical->backing and is separate (slice-④). Never fold, never drop.
  out.screenScale = screenScale;
  out.backingW = out.logicalW * out.screenScale;
  out.backingH = out.logicalH * out.screenScale;

  // The reachable area is what the WM actually shows: the request CLAMPED to the
  // available logical area. Under fit the request is <= available, so this is a no-op;
  // under a clamp/oversized zoom the request exceeds it and the panel bottom crops.
  out.effectiveLogicalW = std::min(out.logicalW, availLogicalW);
  out.effectiveLogicalH = std::min(out.logicalH, availLogicalH);

  // Reachability is ALWAYS the core predicate — this header only supplies the rect and
  // the window that was actually opened. This is what makes a bypass visible: pass the
  // design scale and a clamped window and the bottom row goes red.
  out.bottomRowReachable = core::design_rect_fully_on_window(
      bottomRow, out.drawScale, out.effectiveLogicalW, out.effectiveLogicalH);

  return out;
}

}  // namespace lunar24::host
