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
#include <type_traits>
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
// GH#4 8B2: the ProcessBlock bridge below casts sample** <-> double** . That relabeling is only
// valid under iPlug2's DEFAULT `sample = double`. A SAMPLE_TYPE_FLOAT build would reinterpret the
// buffers with the wrong element type -> UB. This is a compile-time hard gate, not a runtime or
// generated-check: the host must never silently compile such a bridge. (The wiring gate greps for
// this guard so removing it fails loudly too.)
static_assert(std::is_same_v<sample, double>,
              "Lunar24 host ProcessBlock bridge requires iPlug2 sample == double");

void LunarHostPlugin::OnReset()
{
  // GH#4 8B2: the stopped-stream boundary. Rebuild the runtime owner for the REAL device
  // format the host is about to open. The physical connector counts are read from the host
  // (NOT hardcoded): with GH#4 8B3 the plan is negotiated from the device capability and
  // installed via setActualChannelPlan() BEFORE this runs, so NInChansConnected()/
  // NOutChansConnected() are the actual open counts. A failure (e.g. <2 outputs, or the 0/0
  // failure sentinel) leaves the engine not-ready and ProcessBlock fail-silent.
  engine_.prepare(lunar24::host::kLunarStartupSeed, GetSampleRate(), GetBlockSize(),
                  NInChansConnected(), NOutChansConnected());
}

bool LunarHostPlugin::setActualChannelPlan(int inCh, int outCh)
{
  // GH#4 8B3 (task#73): disconnect ALL declared max channels first, then connect only
  // [0,inCh)/[0,outCh). Called at the stopped-stream boundary (InitAudio) BEFORE OnReset, so the
  // engine prepares for the REAL plan and AppProcess attaches by the same count.
  //
  // FAIL-CLOSED ADMISSION. (inCh == 0 && outCh == 0) is the NOT-READY sentinel (LunarInvalidateAudio
  // installs it), NOT an iPlug2 IOConfig, so it is allowed as-is. Otherwise the ONLY accepted pairs
  // are one of the six APP configs declared in PLUG_CHANNEL_IO ("0-2 1-2 2-2 0-4 1-4 2-4") — tested
  // by the AUTHORITATIVE parsed-config admission IPlugProcessor::LegalIO(in,out), not by a hand-
  // written mirror. LegalIO returns true iff (in,out) is exactly one of those configs, so it rejects
  // (1,0)/(2,0) (no config has 0 outputs), (3,2), (2,3), and any out-of-domain pair — a per-direction
  // in{0,1,2} x out{0,2,4} check would wrongly accept an input-only plan with no outputs. Also cap at
  // the declared max so setActualChannelPlan never silently clamps/truncates. An ill-formed plan
  // REJECTS: it installs the 0-in/0-out NOT-READY sentinel and returns false; the host (InitAudio)
  // must check the bool and abort the open rather than proceeding with a truncated channel set.
  const int maxIn = MaxNChannels(ERoute::kInput);
  const int maxOut = MaxNChannels(ERoute::kOutput);
  const bool sentinel = (inCh == 0 && outCh == 0);
  if (!sentinel && (!LegalIO(inCh, outCh) || inCh > maxIn || outCh > maxOut)) {
    inCh = 0;
    outCh = 0;  // failure sentinel: the owner's <2 output fail-path turns it NOT-READY.
    SetChannelConnections(ERoute::kInput, 0, maxIn, false);
    SetChannelConnections(ERoute::kOutput, 0, maxOut, false);
    return false;
  }
  SetChannelConnections(ERoute::kInput, 0, maxIn, false);
  SetChannelConnections(ERoute::kOutput, 0, maxOut, false);
  if (inCh > 0) SetChannelConnections(ERoute::kInput, 0, inCh, true);
  if (outCh > 0) SetChannelConnections(ERoute::kOutput, 0, outCh, true);
  return true;
}

void LunarHostPlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // GH#4 8B2: a PURE delegate to the framework-free owner. The owner either renders through
  // the production DeviceAdapter::renderBlock (task#71) or returns a dropped status after
  // writing deterministic silence into the outputs. There is NO frame loop / scale / mapping /
  // pass-through / second output bank in the host — that would be a wiring defect.
  //
  // sample==double, so the pointer casts into the owner's framework-free surface are a
  // same-representation reinterpret_to_const (double** -> const double* const*): it only
  // adds const / re-types at the compile-time layer, and the host touches no data itself.
  engine_.processBlock(reinterpret_cast<const double* const*>(inputs),
                       reinterpret_cast<double* const*>(outputs),
                       NInChansConnected(), NOutChansConnected(), nFrames);
}
#endif
