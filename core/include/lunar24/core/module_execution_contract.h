// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ModuleExecutionContract: the scheduling facts a module declares after its
// real algorithm has been prepared (design/07 §2, §4). These are audited
// scheduling facts, NOT user-visible PDC parameters.
//
// Per design/07 §2/§4 the contract must name each specific input→output path's
// minimum causal delay and whether that path can be zero-delay direct-through.
// Module-wide latency numbers alone never grant cycle-breaking eligibility.

#pragma once

#include <cstdint>

#include <lunar24/core/evidence_policy.h>
#include <lunar24/core/id_types.h>
#include <lunar24/core/signal.h>

namespace lunar24::core {

// Upper bound on the number of distinct input→output paths a module may declare.
// Kept bounded so the contract is a fixed no-heap struct.
inline constexpr std::uint32_t kMaxModulePathDelays = 16;

// Per-claim provenance for one path's scheduling facts (design/07 §10). Each of
// min-delay, direct-through eligibility, and exact-zero-gain is its own audited
// fact and carries its own status — they are NOT three faces of one jack voltage
// field. Unverified until the real algorithm is prepared.
struct PathEvidence {
  EvidenceStatus minDelay = EvidenceStatus::unverified;        // minCausalDelaySamples
  EvidenceStatus canDirectThrough = EvidenceStatus::unverified; // canDirectThrough
  EvidenceStatus exactZeroGain = EvidenceStatus::unverified;    // directThroughExactZeroGain
};

// A specific input→output path and its scheduling facts.
struct ModulePathDelay {
  JackId inPort;
  JackId outPort;

  // Minimum causal delay along this path, in samples. 0 == the path may be
  // zero-latency (only then is it cycle-break eligible, and only if it actually
  // lies on the cycle in question).
  double minCausalDelaySamples = 0.0;

  // Whether SOME reachable parameter position makes this path a zero-delay
  // direct/mix through. If true, the compiler treats that path as latency 0
  // unless direct-through is provably exact-zero-gain (per design/07 §4).
  bool canDirectThrough = false;

  // Whether the direct-through path can prove its gain is exactly zero when
  // toggled (which would force a topology recompile). Only meaningful if
  // canDirectThrough is true.
  bool directThroughExactZeroGain = false;

  // Each scheduling fact's own provenance (design/07 §10).
  PathEvidence evidence;
};

struct ModuleExecutionContract {
  double sampleRate = 0.0;             // prepared-for host sample rate
  std::uint32_t maxBlockSize = 0;      // prepared-for maximum block size (frames)
  std::uint32_t intrinsicLatencySamples = 0;  // documented output latency after prepare

  // Whether SOME input→output path can be zero-delay direct-through from some
  // parameter position. Keep for module-wide convenience; the authoritative
  // per-path facts live in pathDelays[]. This never by itself grants cycle-
  // breaking credit.
  bool hasDirectThroughPath = false;

  // Upper bound on live DSP resources (delay lines, FFT plans, filter counts).
  std::uint32_t maxResources = 0;

  // Whether this module may enter a cyclic SCC, or must be rejected there
  // (required for window/FFT/reverse modules until they expose a cycle-safe
  // interface).
  bool allowedInCyclicSCC = false;

  // Declared per-path causal facts. `pathDelays[i]` for i in [0, pathDelayCount)
  // is valid; no allocation happens on the audio thread.
  std::uint32_t pathDelayCount = 0;
  ModulePathDelay pathDelays[kMaxModulePathDelays] = {};
};

// Invariants the scheduler relies on (design/07 §2, §4). A prepared contract that
// violates any of them is rejected at prepare time, before it reaches the audio
// thread. This is the single gate for the per-path causal facts.
inline bool module_contract_is_valid(const ModuleExecutionContract& c) {
  if (c.pathDelayCount > kMaxModulePathDelays) return false;
  for (std::uint32_t i = 0; i < c.pathDelayCount; ++i) {
    const auto& p = c.pathDelays[i];
    if (p.inPort == p.outPort) return false;              // degenerate self-path
    if (p.minCausalDelaySamples < 0.0) return false;
    if (p.directThroughExactZeroGain && !p.canDirectThrough) return false;  // exact-zero-gain requires a direct-through path
    if (p.canDirectThrough && p.minCausalDelaySamples > 0.0) return false;  // a >0 min-delay path is never zero-delay through
  }
  return true;
}

}  // namespace lunar24::core
