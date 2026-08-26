// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/plugin.cpp — the Lunar 24 host standalone plugin implementation.
//
// P5-① mandate (the geometry choke point): the host MUST size its window by
// consuming lunar24::host::compute_window_layout, NOT by hardcoding a size or
// reverting to design scale. A host that bypasses this is exactly the clamp
// defect under test (it lets the window-manager crop the panel bottom). So:
//
//   * zoomScale come from the LUNAR_HOST_FORCE_CLAMP env var: set to "1" -> the
//     negative (clamp, design scale) path; otherwise -> the fit path.
//   * drawScale + logicalW/H are the geometry module's answer, never computed here.
//   * the open window = SetEditorSize(logicalW, logicalH), which drives the
//     ClientResize() in the stock IPlugAPP_dialog MainDlgProc (WM_INITDIALOG).
//
// iPlug2's MakeGraphics(*this, designW, designH, fps, drawScale) is called with
// the DESIGN dims + drawScale so that the IGraphics internal view (
// WindowWidth() = mWidth * mDrawScale) equals the logical window the dialog
// opens — the design space is drawn at drawScale into the logical window, and
// retina is a separate SetScreenScale() multiplier (slice-④), never folded in.

#include "plugin.h"
#include "IPlug_include_in_plug_src.h"

#include <cstdlib>
#include <lunar24/core/host_window_fit.h>
#include <host/window_layout.h>

using namespace iplug::igraphics;

// Platform geometry probes implemented in host/main.mm (the ObjC++ bootstrap).
// The plugin (framework-free-ish C++) asks the host for the screen's LOGICAL
// area + retina scale; the layout math is all done here via the core module.
extern "C" double lunar_host_avail_logical_w();
extern "C" double lunar_host_avail_logical_h();
extern "C" double lunar_host_screen_scale();
extern "C" bool lunar_host_force_clamp();

LunarHostPlugin::LunarHostPlugin(const InstanceInfo& info)
    : Plugin(info, MakeConfig(0, 0))
{
#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    // zoomScale <= 0 -> fit (delegate to core); zoomScale > 0 -> explicit zoom.
    // The env var is the ONLY place the fit-vs-clamp decision is injected, so the
    // negative path (clamp) is reproducible on demand.
    const double zoomScale = lunar_host_force_clamp() ? 1.0 : 0.0;

    const double designW = lunar24::core::kDesignWidth;
    const double designH = lunar24::core::kDesignHeight;
    const double availW = lunar_host_avail_logical_w();
    const double availH = lunar_host_avail_logical_h();
    const double screenScale = lunar_host_screen_scale();

    // The design-space bottom row is the reachability predicate input; P5-① does
    // not make a control-reachability claim, but the choke point still computes it
    // from the window it actually opens, which is exactly the bug-detector shape.
    const lunar24::core::DesignRect fullDesign{0.0, 0.0, designW, designH};

    const auto layout = lunar24::host::compute_window_layout(
        designW, designH, availW, availH, zoomScale, screenScale, fullDesign);

    // THIS is the size the host opens: the geometry module's logical W/H. It runs
    // inside OpenWindow (before ClientResize), so GetEditorWidth/Height return it.
    SetEditorSize(static_cast<int>(layout.logicalW), static_cast<int>(layout.logicalH));

    // Design-space IGraphics. WindowWidth() = designW * drawScale == logicalW,
    // so the internal view fills the dialog the host just sized.
    return MakeGraphics(*this, static_cast<int>(designW), static_cast<int>(designH),
                        PLUG_FPS, static_cast<float>(layout.drawScale));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    if (pGraphics->NControls() > 0) {
      return;  // already laid out; don't re-attach on a resize relayout
    }
    pGraphics->AttachPanelBackground(COLOR_GRAY);
  };
#endif
}

#if IPLUG_DSP
void LunarHostPlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // P5-① is the window bootstrap only; no audio is produced. This runs as --no-io
  // in the probe, so it is never invoked, but satisfying the DSP surface keeps the
  // class concrete. Silence output deterministically.
  const int nIn = NInChansConnected();
  const int nOut = NOutChansConnected();
  for (int s = 0; s < nFrames; s++) {
    for (int c = 0; c < nOut; c++) {
      outputs[c][s] = (nIn > 0) ? inputs[c % nIn][s] : sample(0.0);
    }
  }
}
#endif
