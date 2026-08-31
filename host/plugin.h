// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/plugin.h — the Lunar 24 host standalone plugin (blank editor). This is the
// IPlugAPP plugin class the host opens. Its editor is intentionally empty for
// P5-①: the mandate is to prove the self-authored host bootstrap opens a real
// macOS window sized by the geometry choke point, not to render controls yet.
// The window-sizing logic lives in the mMakeGraphicsFunc lambda (below) — that is
// the one place the host consumes lunar24::host::compute_window_layout.

#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include <host/standalone_audio_engine.h>

using namespace iplug;

class LunarHostPlugin final : public Plugin
{
public:
  LunarHostPlugin(const InstanceInfo& info);

#if IPLUG_DSP
  // GH#4 8B2 lifecycle gate: OnReset() runs at the stopped-stream boundary (CloseAudio
  // callbacks done -> SetBlockSize/SetSampleRate -> OnReset -> openStream/startStream). It is
  // the ONE place the host (re)prepares the runtime owner for the REAL device format.
  void OnReset() override;
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;

  // GH#4 8B3 (task#73): install the ACTUAL connected channel plan with fail-closed ADMISSION.
  // A derived class is the ONLY place the real host can drive the protected
  // IPlugProcessor::SetChannelConnections (the iPlug2 host is not a friend of IPlugProcessor), so
  // the app host calls this via a static_cast<LunarHostPlugin*> before OnReset(). It disconnects
  // ALL declared max channels (config.h APP branch "0-2 1-2 2-2 0-4 1-4 2-4" -> MaxNChannels 2/4)
  // then re-connects only [0,inCh)/[0,outCh), so OnReset()/AppProcess read the REAL count
  // (NInChansConnected/NOutChansConnected) and a 2-out device never re-asserts the 4-channel max.
  // An ILLEGAL plan (inCh not in {0,1,2}, outCh not in {0,2,4}, or exceeding the declared max) is
  // rejected: the host installs a 0-in/0-out sentinel and returns false so the owner is NOT-READY
  // (never "a device channel plan silently clamped/truncated"). The host MUST check the return.
  // 0/0 is itself legal (the fail-closed NOT-READY sentinel the invalidation helper installs).
  bool setActualChannelPlan(int inCh, int outCh);
#endif

private:
  // The framework-free runtime owner, held BY VALUE. It owns the address-stable
  // MachineRuntimeDefinition (heap) + the single DeviceAdapter (task#71). ProcessBlock is a
  // PURE delegate to it.
  lunar24::host::StandaloneAudioEngine engine_;
};
