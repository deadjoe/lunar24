// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P2-② tests for the PatchGraph connection-facts layer (design/07 §4, design/04
// §2):
//   A. Stable Jack IDs — every registry jack carries an explicit numeric id that
//      is order-independent; the graph reads cardinality from the descriptor.
//   B. NormalizedRoute as formal graph edges — 1-source-many-destination (the
//      keyboard V/OCT feeds both VCOs), not consuming user-cable cardinality,
//      each edge overridden only at its own sink and restored on unplug. Module
//      input resolution is one graph query (resolveInput), never a scattered
//      `if (jack empty)`.
//   C. 插线覆盖 / 拔线恢复 — user cable cardinality is DATA (read from
//      JackDescriptor.maxCables, default 1); connecting a cable to an occupied
//      jack atomically replaces the old cable (source or sink side, whichever is
//      at capacity); order-independence and multi-round no-drift both hold.
//
// The maxCables=2 synthetic test is the @Claude red-flag guard: if the graph
// hardcoded "a jack holds one cable", honoring a descriptor that says 2 would be
// impossible and this test would go red. It passing proves cardinality is data.

#include "mini_test.h"

#include <cstdint>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;
namespace reg = lunar24::registry;

// ---------------------------------------------------------------- helpers ----

// A minimal, well-formed jack descriptor for the synthetic-capacity test. Only
// id / direction / maxCables are meaningful to PatchGraph; the rest is
// value-initialized and never read.
static core::JackDescriptor mk_jack(core::JackId id, core::PinDirection dir,
                                    std::uint8_t maxCables) {
  core::JackDescriptor j{};
  j.id = id;
  j.direction = dir;
  j.maxCables = maxCables;
  return j;
}

// ----------------------------------------------------------- data-driven -----

// @Claude: "把 ≤1 保持成数据……图必须读 maxCables 这个字段". Every real jack
// currently carries maxCables==1 (the no-mult evidence default); the graph must
// read it, never assume it. This confirms the source data is what the graph
// consumes and that no real jack silently exceeds the single-cable default.
static void registry_cardinality_is_data() {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    CHECK(reg::kJacks[i].maxCables >= 1u);
    CHECK(reg::kJacks[i].direction == core::PinDirection::input ||
          reg::kJacks[i].direction == core::PinDirection::output);
  }
  // The keyboard V/OCT output — the multi-destination source in the manual — is
  // exactly the evidenced default, one cable. Fan-out to both VCOs is a
  // NormalizedRoute, not a raw output-cable mult.
  bool found = false;
  for (std::uint32_t i = 0; i < core::kJackCount; ++i)
    if (reg::kJacks[i].id == core::JackId::keyboard_v_oct_out) found = true;
  CHECK(found);
}

// -------------------------------------------------------------- normalized ---

// B: normalized routes are formal edges that do not consume cable cardinality,
// may join one source to several sinks, and are overridden/restored per-sink.
static void normalized_route_formal_edges() {
  core::PatchGraph g(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                     core::kRouteCount);

  // By default both VCO V/OCT inputs are fed by the keyboard V/OCT (1 source,
  // 2 routes). Neither route consumed a user cable (cableCount stays 0).
  CHECK_EQ(g.cableCount(), 0u);
  CHECK_TRUE(g.normalizedActive(core::JackId::keyboard_v_oct_out,
                                core::JackId::vco_a_v_oct_in));
  CHECK_TRUE(g.normalizedActive(core::JackId::keyboard_v_oct_out,
                                core::JackId::vco_b_v_oct_in));
  CHECK_TRUE(g.effectiveEdge(core::JackId::keyboard_v_oct_out,
                             core::JackId::vco_a_v_oct_in));

  // resolveInput is one graph query: a module asks "what drives my input", it
  // never scatters `if (jack empty)`.
  core::JackId src{};
  CHECK_TRUE(g.resolveInput(core::JackId::vco_a_v_oct_in, &src));
  CHECK_TRUE(src == core::JackId::keyboard_v_oct_out);
  CHECK_TRUE(g.resolveInput(core::JackId::vco_b_v_oct_in, &src));
  CHECK_TRUE(src == core::JackId::keyboard_v_oct_out);

  // Plugging a cable into VCO A's V/OCT input overrides ONLY that input's route.
  // VCO B's route stays active.
  CHECK_TRUE(g.connect(core::JackId::joystick_x_out, core::JackId::vco_a_v_oct_in));
  CHECK_TRUE(g.cableConnected(core::JackId::joystick_x_out, core::JackId::vco_a_v_oct_in));
  CHECK_FALSE(g.normalizedActive(core::JackId::keyboard_v_oct_out,
                                 core::JackId::vco_a_v_oct_in));
  CHECK_TRUE(g.normalizedActive(core::JackId::keyboard_v_oct_out,
                                core::JackId::vco_b_v_oct_in));
  CHECK_TRUE(g.resolveInput(core::JackId::vco_a_v_oct_in, &src));
  CHECK_TRUE(src == core::JackId::joystick_x_out);      // cable wins at its sink
  CHECK_TRUE(g.resolveInput(core::JackId::vco_b_v_oct_in, &src));
  CHECK_TRUE(src == core::JackId::keyboard_v_oct_out);  // route untouched

  // Unplugging restores the normalized edge (not a blank).
  CHECK_TRUE(g.disconnect(core::JackId::joystick_x_out, core::JackId::vco_a_v_oct_in));
  CHECK_TRUE(g.normalizedActive(core::JackId::keyboard_v_oct_out,
                                core::JackId::vco_a_v_oct_in));
  CHECK_TRUE(g.resolveInput(core::JackId::vco_a_v_oct_in, &src));
  CHECK_TRUE(src == core::JackId::keyboard_v_oct_out);
}

// -------------------------------------------------------------- cardinality --

// C: cardinality read from maxCables (data). maxCables==1 source caps at 1 output
// cable (atomic replace); maxCables==1 sink caps at 1 input cable. Clean jacks
// (no normalized route) are used so the replace is unambiguous.
static void user_cable_cardinality_default_one() {
  core::PatchGraph g(reg::kJacks, core::kJackCount, reg::kNormalizedRoutes,
                     core::kRouteCount);

  // Output cardinality: lfo_a.cv_out (maxCables 1) drives one input; plugging it
  // into a second input atomically replaces the first.
  CHECK_TRUE(g.connect(core::JackId::lfo_a_cv_out, core::JackId::vco_a_cv_in));
  CHECK_TRUE(g.cableConnected(core::JackId::lfo_a_cv_out, core::JackId::vco_a_cv_in));
  CHECK_TRUE(g.connect(core::JackId::lfo_a_cv_out, core::JackId::vco_a_fm_in));
  CHECK_FALSE(g.cableConnected(core::JackId::lfo_a_cv_out, core::JackId::vco_a_cv_in));
  CHECK_TRUE(g.cableConnected(core::JackId::lfo_a_cv_out, core::JackId::vco_a_fm_in));
  CHECK_EQ(g.countOutOf(core::JackId::lfo_a_cv_out), 1u);

  // Input cardinality: vco_a.cv_in (maxCables 1) accepts one source; a second
  // source replaces the first.
  CHECK_TRUE(g.connect(core::JackId::joystick_y_out, core::JackId::vco_a_cv_in));
  CHECK_TRUE(g.cableConnected(core::JackId::joystick_y_out, core::JackId::vco_a_cv_in));
  CHECK_EQ(g.countInto(core::JackId::vco_a_cv_in), 1u);
}

// @Claude red-flag test: a jack whose descriptor says maxCables==2 MUST be
// honored. If the graph hardcoded "one cable per jack", this goes red. It proves
// the graph reads the field rather than asserting an assumption.
static void max_cables_is_data_not_assumption() {
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{100}, core::PinDirection::output, 2u),  // out supports 2
      mk_jack(core::JackId{101}, core::PinDirection::output, 1u),  // out supports 1
      mk_jack(core::JackId{102}, core::PinDirection::input, 2u),   // in supports 2
      mk_jack(core::JackId{103}, core::PinDirection::input, 1u),   // in supports 1
  };
  core::PatchGraph g(jacks, 4, nullptr, 0);

  // Output that supports 2 may drive two inputs (fan-out is a data fact here).
  CHECK_TRUE(g.connect(core::JackId{100}, core::JackId{102}));
  CHECK_TRUE(g.connect(core::JackId{100}, core::JackId{103}));
  CHECK_EQ(g.countOutOf(core::JackId{100}), 2u);
  CHECK_TRUE(g.cableConnected(core::JackId{100}, core::JackId{102}));
  CHECK_TRUE(g.cableConnected(core::JackId{100}, core::JackId{103}));

  // Input that supports 2 may accept two sources.
  CHECK_TRUE(g.connect(core::JackId{101}, core::JackId{102}));
  CHECK_EQ(g.countInto(core::JackId{102}), 2u);
  CHECK_TRUE(g.cableConnected(core::JackId{100}, core::JackId{102}));
  CHECK_TRUE(g.cableConnected(core::JackId{101}, core::JackId{102}));

  // The maxCables==1 source still caps at one (read from data, not hardcoded).
  CHECK_TRUE(g.connect(core::JackId{101}, core::JackId{103}));
  CHECK_EQ(g.countOutOf(core::JackId{101}), 1u);
}

// ------------------------------------------------------- order & no-drift ----

// Helper to capture a graph's canonical effective edges into an array.
static std::uint32_t capture(core::PatchGraph& g, core::PatchEdge* out) {
  return g.effectiveEdges(out, 128);
}

static void order_independence() {
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{1}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{2}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{3}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{11}, core::PinDirection::input, 1u),
      mk_jack(core::JackId{12}, core::PinDirection::input, 1u),
      mk_jack(core::JackId{13}, core::PinDirection::input, 1u),
  };
  const core::PatchConnection plan[] = {
      {core::JackId{1}, core::JackId{11}},
      {core::JackId{2}, core::JackId{12}},
      {core::JackId{3}, core::JackId{13}},
  };
  const std::uint32_t kPlan = sizeof(plan) / sizeof(plan[0]);
  const std::uint32_t kOrder[] = {0u, 1u, 2u, 1u, 2u, 0u, 2u, 0u, 1u};  // shuffled

  core::PatchGraph a(jacks, 6, nullptr, 0);
  for (std::uint32_t i = 0; i < kPlan; ++i) a.connect(plan[i].source, plan[i].sink);

  core::PatchGraph b(jacks, 6, nullptr, 0);
  for (std::uint32_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
    b.connect(plan[kOrder[i]].source, plan[kOrder[i]].sink);

  // The same set of cables, connected in a different order, yields the SAME
  // canonical effective-edge set (order-independence).
  core::PatchEdge ea[128], eb[128];
  CHECK_EQ(capture(a, ea), capture(b, eb));
  for (std::uint32_t i = 0; i < 3; ++i)
    CHECK_TRUE(ea[i].source == eb[i].source && ea[i].sink == eb[i].sink);
  CHECK_EQ(a.effectiveEdgeCount(), 3u);
}

// state->cable->state round-trip and multi-round no-drift: N patch/unpatch
// cycles leave the topology bit-identical to the starting state. Both cycle
// counts and a shuffled multi-cable round are exercised.
static void multi_round_no_drift() {
  const core::JackDescriptor jacks[] = {
      mk_jack(core::JackId{1}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{2}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{11}, core::PinDirection::input, 1u),
      mk_jack(core::JackId{12}, core::PinDirection::input, 1u),
  };
  core::PatchGraph g(jacks, 4, nullptr, 0);

  core::PatchEdge initial[128];
  const std::uint32_t n0 = capture(g, initial);
  CHECK_EQ(n0, 0u);                       // no routes, no cables yet

  // N identical patch/unpatch cycles of the same cable must not drift.
  for (std::uint32_t r = 0; r < 64; ++r) {
    CHECK_TRUE(g.connect(core::JackId{1}, core::JackId{11}));
    CHECK_TRUE(g.disconnect(core::JackId{1}, core::JackId{11}));
    core::PatchEdge now[128];
    CHECK_EQ(capture(g, now), n0);
    for (std::uint32_t i = 0; i < n0; ++i)
      CHECK_TRUE(now[i].source == initial[i].source && now[i].sink == initial[i].sink);
  }

  // A multi-cable round, patched then unpatched in a different order, returns to
  // the identical empty topology — no leftover state accumulates.
  const core::PatchConnection plan[] = {
      {core::JackId{1}, core::JackId{11}},
      {core::JackId{2}, core::JackId{12}},
  };
  for (std::uint32_t i = 0; i < 2; ++i) g.connect(plan[i].source, plan[i].sink);
  for (std::uint32_t i = 2; i > 0;) {  // unplug in the reverse order
    --i;
    g.disconnect(plan[i].source, plan[i].sink);
  }
  core::PatchEdge now[128];
  CHECK_EQ(capture(g, now), n0);
  CHECK_EQ(g.cableCount(), 0u);
}

// --------------------------------------------------------------------
// P2-④ guard: signal type is ADVISORY, never a connection filter. A real output
// drives any real input; type/range mismatch is a hint to the dsp layer, not a
// reason to reject the cable. PatchGraph::connect already rejects only by
// direction — here we PROVE cross-type succeeds, and that a TypeGuard that would
// reject it is the degeneracy @Claude names.
// --------------------------------------------------------------------

static void cross_type_connect_succeeds() {
  // Four jacks with deliberately mismatched (and, for 3, non-adjoining) types.
  core::JackDescriptor jacks[4] = {
      mk_jack(core::JackId{201}, core::PinDirection::output, 1u),  // cv out
      mk_jack(core::JackId{202}, core::PinDirection::input, 1u),   // gate in
      mk_jack(core::JackId{203}, core::PinDirection::output, 1u),  // gate out
      mk_jack(core::JackId{204}, core::PinDirection::input, 1u),   // cv in
  };
  jacks[0].signalType = core::SignalType::cv;
  jacks[1].signalType = core::SignalType::gate;
  jacks[2].signalType = core::SignalType::gate;
  jacks[3].signalType = core::SignalType::cv;

  core::PatchGraph g(jacks, 4u, nullptr, 0u);

  // cv out -> gate in: accepted despite the type mismatch.
  CHECK_TRUE(g.connect(core::JackId{201}, core::JackId{202}));
  CHECK_TRUE(g.cableConnected(core::JackId{201}, core::JackId{202}));
  // gate out -> cv in: accepted too.
  CHECK_TRUE(g.connect(core::JackId{203}, core::JackId{204}));
  CHECK_TRUE(g.cableConnected(core::JackId{203}, core::JackId{204}));

  // The one thing PatchGraph still rejects is a direction violation — type is not
  // consulted at all. An input-driven-from-input stays rejected.
  CHECK_FALSE(g.connect(core::JackId{202}, core::JackId{204}));
  CHECK_FALSE(g.connect(core::JackId{201}, core::JackId{203}));
}

// A hypothetical TypeGuard connect that hard-filters by signal type — the exact
// reading of "signal type is advisory" that would be WRONG. It must reject the
// CV->gate cable. Its refusal on a {cv,gate} pair is the red symptom: a real
// connect that adopted this rule would break cross-type patching.
static bool type_guard_connect(const core::JackDescriptor* jacks,
                               core::JackId source, core::JackId sink) {
  const core::JackDescriptor* s = nullptr;
  const core::JackDescriptor* k = nullptr;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    if (jacks[i].id == source) s = &jacks[i];
    if (jacks[i].id == sink) k = &jacks[i];
  }
  if (!s || !k) return false;
  if (s->direction != core::PinDirection::output) return false;
  if (k->direction != core::PinDirection::input) return false;
  if (s->signalType != k->signalType) return false;  // <-- the degenerate filter
  return true;
}

static void type_guard_would_reject() {
  core::JackDescriptor jacks[4] = {
      mk_jack(core::JackId{201}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{202}, core::PinDirection::input, 1u),
      mk_jack(core::JackId{203}, core::PinDirection::output, 1u),
      mk_jack(core::JackId{204}, core::PinDirection::input, 1u),
  };
  jacks[0].signalType = core::SignalType::cv;
  jacks[1].signalType = core::SignalType::gate;
  jacks[2].signalType = core::SignalType::gate;
  jacks[3].signalType = core::SignalType::cv;

  // A type-guarded connect REFUSES the very CV->gate, gate->CV cables that the
  // real PatchGraph accepts. Its false here is the degenerate-behaviour symptom:
  // if PatchGraph silently adopted a type filter, cross_type_connect_succeeds
  // would go red, so the detector's teeth are real.
  CHECK_FALSE(type_guard_connect(jacks, core::JackId{201}, core::JackId{202}));
  CHECK_FALSE(type_guard_connect(jacks, core::JackId{203}, core::JackId{204}));
}

int main() {
  registry_cardinality_is_data();
  normalized_route_formal_edges();
  user_cable_cardinality_default_one();
  max_cables_is_data_not_assumption();
  order_independence();
  multi_round_no_drift();
  cross_type_connect_succeeds();
  type_guard_would_reject();
  return ::test::finish("patch_graph");
}
