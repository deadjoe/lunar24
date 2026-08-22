// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// AudioBlockView: a framework-free, preallocated block of the four logical
// outputs the core always produces (design/07 §5):

#pragma once

#include <cstdint>

#include <lunar24/core/signal.h>

namespace lunar24::core {

// Core always emits WET L/R + DRY A (VCO A) + DRY B (VCO B). Physical 2/4-channel
// selection belongs to the DeviceAdapter, never to the core. Pointers must remain
// valid for the whole block; no allocation happens while rendering.
struct AudioBlockView {
  SignalSample* wetL = nullptr;
  SignalSample* wetR = nullptr;
  SignalSample* dryA = nullptr;  // dry VCO A output
  SignalSample* dryB = nullptr;  // dry VCO B output
  std::uint32_t frames = 0;      // must equal the host block size
};

}  // namespace lunar24::core
