// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// PatchGraph: the real signal network (NOT a modulation-matrix). The on-screen
// cable is only a visualization of this graph (design/07 §4).
//
// P0 freezes the types and invariants; the topology/SCC/feedback-edge compiler
// is a P2 deliverable. Any real output may connect to any real input; type and
// range are advisory. User cable cardinality is fixed: an input takes at most
// one cable, an output drives at most one cable (no fan-out until hardware
// evidence supports stackable mults). NormalizedRoute edges do not consume
// user-cable cardinality and are overridden by a cable plugged at their sink.

#pragma once

#include <cstdint>

#include <lunar24/core/id_types.h>

namespace lunar24::core {

// A single user patch cable.
struct PatchConnection {
  JackId source;
  JackId sink;
};

// Monotonic graph epoch — bumped on every topology change so the audio thread
// can cheaply detect an out-of-date snapshot without locking (P2 uses this for
// the SCC recompile trigger).
using GraphEpoch = std::uint64_t;

// Opaque handle to an immutable compiled graph snapshot. The audio thread holds
// only a non-owning handle/epoch; old snapshots are reclaimed on a non-audio
// thread. P0 declares the handle; P2 defines the compiler and the concrete type.
struct CompiledGraph;

}  // namespace lunar24::core
