// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// PatchGraph: the real signal network (NOT a modulation-matrix). The on-screen
// cable is only a visualization of this graph (design/07 §4).
//
// This header implements the P2-② CONNECTION-FACTS layer (the identity and
// mutation semantics of the topology): stable JackIds, normalized-route edges,
// and patch/unpatch with the override/restore rule. It deliberately does NOT
// implement the topology/execution compiler (SCC decomposition, z^-1 feedback
// ordering, per-path causal delay) — that is the P2-③ deliverable, and this
// header only forward-declares its input, the immutable CompiledGraph.
//
// Invariants enforced here (design/07 §4, design/04 §2):
//   * User cable cardinality is DATA, not a hardcoded count. cardinality is read
//     from JackDescriptor.maxCables (default 1) and JackDescriptor.direction.
//     Nothing in this file writes the assumption "a jack can hold one cable" —
//     a jack holding more than one works iff its descriptor says so, which is
//     exactly what P2-② wants to make cheap to relax when evidence arrives.
//   * Connecting to a jack already at capacity atomically replaces the old cable.
//   * NormalizedRoute edges never consume user-cable cardinality, may be
//     1-source-many-destination (e.g. keyboard V/OCT feeds VCO A and VCO B), and
//     are overridden only at their own sink by a cable plugged there; unplugging
//     restores them.
//   * Restoring the same patch facts yields the same effective edge set
//     regardless of the order in which cables were connected.
//
// Framework-free: no heap, no locks, fixed-capacity cable bank.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/descriptors.h>
#include <lunar24/core/id_types.h>

namespace lunar24::core {

// A single user patch cable.
struct PatchConnection {
  JackId source;
  JackId sink;
};

// Fixed capacity of the user-cable bank. Bounded by the machine patch capacity
// (kDevicePatchCapacity is gated against the registry jack id-space).
inline constexpr std::uint32_t kPatchCableBankCapacity = kDevicePatchCapacity;

// Monotonic graph epoch — bumped on every topology change so the audio thread
// can cheaply detect an out-of-date snapshot without locking (P2-③ uses this for
// the SCC recompile trigger).
using GraphEpoch = std::uint64_t;

// An effective directed signal edge in the graph. The compiler (P2-③) consumes
// edges only; it never needs to know whether an edge was a user cable or a
// normalized route. Invariant: source != sink; source is an output jack, sink an
// input jack.
struct PatchEdge {
  JackId source;
  JackId sink;
};

// Canonical edge order (source id, then sink id) so the effective edge set can be
// serialized identically whatever order cables were connected in. This is how
// "restore the same state -> the same topology" is made observable.
inline bool patch_edge_before(const PatchEdge& a, const PatchEdge& b) {
  if (a.source != b.source)
    return static_cast<std::uint32_t>(a.source) < static_cast<std::uint32_t>(b.source);
  return static_cast<std::uint32_t>(a.sink) < static_cast<std::uint32_t>(b.sink);
}

// Opaque handle to an immutable compiled graph snapshot. The audio thread holds
// only a non-owning handle/epoch; old snapshots are reclaimed on a non-audio
// thread. P0 declares the handle; P2-③ defines the compiler and the concrete type.
struct CompiledGraph;

// Mutable connection-facts layer. Holds non-owning pointers to the jack and
// normalized-route descriptors (this is the registry in production; tests pass
// synthetic tables). All cardinality decisions read those descriptors.
class PatchGraph {
 public:
  PatchGraph(const JackDescriptor* jacks, std::uint32_t jackCount,
             const NormalizedRoute* routes, std::uint32_t routeCount)
      : jacks_(jacks), jackCount_(jackCount), routes_(routes), routeCount_(routeCount) {}

  // ---------------------------------------------------------------------------
  // User-cable mutation (design/07 §4 line 96)
  // ---------------------------------------------------------------------------

  // Connect a user cable source -> sink. Returns false (and changes nothing)
  // only when the connection is invalid: an unknown/self jack, or a direction
  // mismatch (source must be an output jack, sink an input jack). Type/range are
  // merely advisory (the spec says core rejects by nothing but direction), so a
  // CV output may drive a gate input. When a source or sink is already at its
  // maxCables capacity, one existing cable is atomically replaced (the old cable
  // on that jack is displaced — for maxCables==1 that is the jack's only cable).
  // Connecting an already-present cable is an idempotent no-op.
  bool connect(JackId source, JackId sink) {
    const JackDescriptor* src = findJack(source);
    const JackDescriptor* snk = findJack(sink);
    if (src == nullptr || snk == nullptr) return false;
    if (src->direction != PinDirection::output) return false;
    if (snk->direction != PinDirection::input) return false;
    if (source == sink) return false;
    if (cableConnected(source, sink)) return true;

    // Cardinality is read from the descriptor, never a hardcoded capacity. A
    // source at capacity displaces its lowest-id sink; a sink at capacity
    // displaces its lowest-id source. Both are atomic with the add below.
    if (countOutOf(source) >= src->maxCables)
      removeCable(source, lowestSinkConnectedTo(source));
    if (countInto(sink) >= snk->maxCables)
      removeCable(lowestSourceConnectedTo(sink), sink);

    addCable(source, sink);
    ++epoch_;
    return true;
  }

  // Remove a user cable source -> sink. Returns true if a cable was removed;
  // false if none was present (also a no-op).
  bool disconnect(JackId source, JackId sink) {
    if (removeCable(source, sink)) {
      ++epoch_;
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // User-cable queries
  // ---------------------------------------------------------------------------

  // Is a user cable source -> sink currently patched?
  bool cableConnected(JackId source, JackId sink) const {
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].source == source && cables_[i].sink == sink) return true;
    return false;
  }

  // Number of user cables leaving a source jack.
  std::uint32_t countOutOf(JackId source) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].source == source) ++n;
    return n;
  }

  // Number of user cables entering a sink jack.
  std::uint32_t countInto(JackId sink) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].sink == sink) ++n;
    return n;
  }

  std::uint32_t cableCount() const { return cableCount_; }
  GraphEpoch epoch() const { return epoch_; }

  // ---------------------------------------------------------------------------
  // NormalizedRoute edges (design/07 §4 line 97)
  // ---------------------------------------------------------------------------

  // Is there a normalized route source -> sink declared in the registry?
  bool routeExists(JackId source, JackId sink) const {
    for (std::uint32_t i = 0; i < routeCount_; ++i)
      if (routes_[i].sourceJack == source && routes_[i].sinkJack == sink) return true;
    return false;
  }

  // A normalized route is ACTIVE iff its sink has no user cable. Plugging a
  // cable into the sink overrides only that sink's route; unplugging restores
  // it. This is the "override / restore" rule, derived rather than stored, so a
  // route never consumes user-cable cardinality.
  bool normalizedActive(JackId source, JackId sink) const {
    return routeExists(source, sink) && countInto(sink) == 0u;
  }

  // ---------------------------------------------------------------------------
  // Effective edges (what the compiler consumes)
  // ---------------------------------------------------------------------------

  // An effective edge is a user cable or an active normalized route. This is the
  // union the execution layer sees.
  bool effectiveEdge(JackId source, JackId sink) const {
    return cableConnected(source, sink) || normalizedActive(source, sink);
  }

  std::uint32_t effectiveEdgeCount() const {
    std::uint32_t n = cableCount_;
    for (std::uint32_t i = 0; i < routeCount_; ++i)
      if (countInto(routes_[i].sinkJack) == 0u) ++n;
    return n;
  }

  // Enumerate the effective edges in canonical (source, sink) order into `out`
  // (up to `capacity`). Returns the count written. This canonical form is the
  // observable "same state -> same topology" fingerprint used by the order- and
  // drift-regressions.
  std::uint32_t effectiveEdges(PatchEdge* out, std::uint32_t capacity) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < cableCount_ && n < capacity; ++i)
      out[n++] = PatchEdge{cables_[i].source, cables_[i].sink};
    for (std::uint32_t i = 0; i < routeCount_ && n < capacity; ++i)
      if (countInto(routes_[i].sinkJack) == 0u)
        out[n++] = PatchEdge{routes_[i].sourceJack, routes_[i].sinkJack};
    std::sort(out, out + n, patch_edge_before);
    return n;
  }

  // Resolve what actually drives an input jack. Priority: a user cable into the
  // input; else an active normalized route. Returns false if nothing drives it.
  // This is the single graph query a module consults for its input, so module
  // code never scatters `if (jack empty)` checks (design/07 §4 line 100).
  bool resolveInput(JackId sink, JackId* outSource) const {
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].sink == sink) {
        if (outSource != nullptr) *outSource = cables_[i].source;
        return true;
      }
    for (std::uint32_t i = 0; i < routeCount_; ++i)
      if (routes_[i].sinkJack == sink) {
        if (outSource != nullptr) *outSource = routes_[i].sourceJack;
        return true;
      }
    return false;
  }

 private:
  const JackDescriptor* findJack(JackId id) const {
    for (std::uint32_t i = 0; i < jackCount_; ++i)
      if (jacks_[i].id == id) return &jacks_[i];
    return nullptr;
  }

  void addCable(JackId source, JackId sink) {
    if (cableCount_ >= kPatchCableBankCapacity) return;  // unreachable: cardinality is enforced above
    cables_[cableCount_++] = PatchConnection{source, sink};
  }

  bool removeCable(JackId source, JackId sink) {
    for (std::uint32_t i = 0; i < cableCount_; ++i) {
      if (cables_[i].source == source && cables_[i].sink == sink) {
        cables_[i] = cables_[cableCount_ - 1];
        --cableCount_;
        return true;
      }
    }
    return false;
  }

  // Callers only invoke these once countOutOf/countInto has shown >= 1 cable.
  JackId lowestSinkConnectedTo(JackId source) const {
    JackId best{std::numeric_limits<std::uint32_t>::max()};
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].source == source &&
          static_cast<std::uint32_t>(cables_[i].sink) < static_cast<std::uint32_t>(best))
        best = cables_[i].sink;
    return best;
  }

  JackId lowestSourceConnectedTo(JackId sink) const {
    JackId best{std::numeric_limits<std::uint32_t>::max()};
    for (std::uint32_t i = 0; i < cableCount_; ++i)
      if (cables_[i].sink == sink &&
          static_cast<std::uint32_t>(cables_[i].source) < static_cast<std::uint32_t>(best))
        best = cables_[i].source;
    return best;
  }

  const JackDescriptor* jacks_ = nullptr;
  std::uint32_t jackCount_ = 0;
  const NormalizedRoute* routes_ = nullptr;
  std::uint32_t routeCount_ = 0;

  PatchConnection cables_[kPatchCableBankCapacity];
  std::uint32_t cableCount_ = 0;
  GraphEpoch epoch_ = 0;
};

}  // namespace lunar24::core
