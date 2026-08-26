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

using namespace iplug;

class LunarHostPlugin final : public Plugin
{
public:
  LunarHostPlugin(const InstanceInfo& info);

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
#endif
};
