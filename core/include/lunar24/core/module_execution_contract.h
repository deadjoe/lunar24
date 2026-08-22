// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ModuleExecutionContract: the scheduling facts a module declares after its
// real algorithm has been prepared (design/07 §2, §4). These are audited
// scheduling facts, NOT user-visible PDC parameters.

#pragma once

#include <cstdint>

#include <lunar24/core/signal.h>

namespace lunar24::core {

struct ModuleExecutionContract {
  double sampleRate = 0.0;             // prepared-for host sample rate
  std::uint32_t maxBlockSize = 0;      // prepared-for maximum block size (frames)
  std::uint32_t intrinsicLatencySamples = 0;  // documented output latency after prepare

  // Whether SOME input→output path can be zero-delay direct-through from some
  // parameter position. If true, the compiler must treat that path as latency 0
  // unless direct-through is provably exact-zero gain (and even then only after
  // a topology recompile). This never by itself grants cycle-breaking credit.
  bool hasDirectThroughPath = false;

  // A declared, input→output-path minimum causal delay that MAY be used to break
  // the specific cycle it actually lies on (replacing an extra z^-1). Module-level
  // latency numbers never grant break eligibility by themselves.
  bool declaresCausalPathDelay = false;

  // Upper bound on live DSP resources (delay lines, FFT plans, filter counts).
  std::uint32_t maxResources = 0;

  // Whether this module may enter a cyclic SCC, or must be rejected there
  // (required for window/FFT/reverse modules until they expose a cycle-safe
  // interface).
  bool allowedInCyclicSCC = false;
};

}  // namespace lunar24::core
