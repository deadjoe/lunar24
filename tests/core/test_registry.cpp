// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P0 tests for the generated machine registry. Verifies the constexpr descriptor
// arrays are internally consistent, every id it references exists, every
// parameter range/journal is valid, every route endpoint resolves, and every
// descriptor carries evidence + a valid status. Also locks the frozen P0 counts.

#include "mini_test.h"

#include <string_view>

#include <lunar24/core/device_state.h>
#include <lunar24/registry.hpp>

namespace core = lunar24::core;
namespace reg = lunar24::registry;

static int find_jack(core::JackId id) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

static int find_module(core::ModuleId id) {
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    if (reg::kModules[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

static bool owner_exists(std::string_view owner) {
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    if (reg::kModules[i].stable_id == owner) return true;
  }
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    if (reg::kPrograms[i].stable_id == owner) return true;
  }
  return false;
}

static bool valid_status(core::EvidenceStatus s) {
  return s == core::EvidenceStatus::confirmed ||
         s == core::EvidenceStatus::unverified ||
         s == core::EvidenceStatus::provisional;
}

static bool valid_fe(const core::FieldEvidence& fe) {
  return valid_status(fe.nominalRange) && valid_status(fe.toleratedRange) &&
         valid_status(fe.threshold) && valid_status(fe.saturation) &&
         valid_status(fe.transfer);
}

static bool valid_pfe(const core::ParameterFieldEvidence& pfe) {
  return valid_status(pfe.range) && valid_status(pfe.unit) &&
         valid_status(pfe.initial) && valid_status(pfe.step) &&
         valid_status(pfe.smoothing) && valid_status(pfe.persistence);
}

static void frozen_counts() {
  // Locked P0 baseline — the audit target. These are the actual vertical-slice
  // counts in spec/machine/lunar24.json at the P0 lock.
  CHECK_EQ(core::kModuleCount, 5u);
  CHECK_EQ(core::kParameterCount, 63u);  // Phase B: fixed 75; moved to honest gap the 8 non-scalar
                                         // keyboard params (seq_steps, quantise_scale_editor,
                                         // plate_tune, pushbutton_value, preset_a..d) AND the 4
                                         // un-evidenced selector params (arp_clock, seq_clock,
                                         // arp_rhythm, seq_rhythm — no enumerated manual set)
  CHECK_EQ(core::kJackCount, 18u);       // Phase B dropped rogue jack vcf.audio_in
  CHECK_EQ(core::kProgramCount, 2u);
  CHECK_EQ(core::kRouteCount, 3u);
  // kNormalizedRoutes[] still holds exactly the frozen route count (kRouteCount
  // generated; the array is sized by that count).
  CHECK_EQ(sizeof(reg::kNormalizedRoutes) / sizeof(reg::kNormalizedRoutes[0]), core::kRouteCount);
}

static void module_ranges_are_contiguous_and_disjoint() {
  std::uint32_t paramNext = 0, jackNext = 0;
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    const auto& m = reg::kModules[i];
    CHECK(m.paramBegin == paramNext);
    CHECK(m.paramBegin + m.paramCount <= core::kParameterCount);
    CHECK(m.jackBegin == jackNext);
    CHECK(m.jackBegin + m.jackCount <= core::kJackCount);
    paramNext += m.paramCount;
    jackNext += m.jackCount;
  }
  // kParameterCount also counts program params (module params come first), so
  // module ranges only need to fit within it; prog contiguity is checked below.
  CHECK(paramNext <= core::kParameterCount);
  CHECK(jackNext == core::kJackCount);
}

static void program_ranges_within_params() {
  // Program params are contiguous and start immediately after the module range.
  std::uint32_t moduleParams = 0;
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i)
    moduleParams += reg::kModules[i].paramCount;

  std::uint32_t next = moduleParams;
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    const auto& p = reg::kPrograms[i];
    CHECK(p.paramBegin == next);
    CHECK(p.paramBegin + p.paramCount <= core::kParameterCount);
    CHECK(p.slot >= 1u && p.slot <= 3u);
    CHECK(!p.stable_id.empty());
    CHECK(!p.name.empty());
    next += p.paramCount;
  }
  CHECK(next == core::kParameterCount);
}

static void parameters_are_valid() {
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& p = reg::kParameters[i];
    CHECK(!p.stable_id.empty());
    CHECK(owner_exists(p.owner));
    CHECK(p.min <= p.max);
    CHECK(p.initial >= p.min && p.initial <= p.max);
    CHECK(p.step >= 0.0);
    CHECK(valid_status(p.status));
    CHECK(valid_pfe(p.fieldEvidence));  // per-field provenance split (07 §10, Codex 03848819)
    CHECK(!p.evidence.source.empty());
  }
}

static void selector_options_are_well_formed() {
  // A selector parameter's discrete value domain is carried IN the descriptor so UI/MIDI never
  // receive a bare 0/1/2 integer without knowing what it means (Codex 03848819 Root 2). Verify the
  // option table is exact and the pointer/count wiring is consistent: a continuous parameter has
  // optionCount==0 with options==nullptr; a selector has optionCount>=2, a non-null options pointer,
  // non-empty labels, no duplicate label within one selector's slice, and every option slice counted
  // exactly once (sum(optionCount) == kParameterOptionLabelCount).
  std::uint32_t totalOptions = 0;
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& p = reg::kParameters[i];
    if (p.optionCount == 0) {
      CHECK(p.options == nullptr);
      continue;
    }
    CHECK(p.optionCount >= 2u);
    CHECK(p.options != nullptr);
    totalOptions += p.optionCount;
    for (std::uint32_t k = 0; k < p.optionCount; ++k) {
      CHECK(p.options[k] != nullptr);
      CHECK(p.options[k][0] != '\0');  // non-empty label
      for (std::uint32_t m = k + 1; m < p.optionCount; ++m)
        CHECK_FALSE(p.options[k] == p.options[m]);  // no duplicate within one selector's slice
    }
  }
  CHECK_EQ(totalOptions, reg::kParameterOptionLabelCount);
  for (std::uint32_t k = 0; k < reg::kParameterOptionLabelCount; ++k) {
    CHECK(reg::kParameterOptionLabels[k] != nullptr);
    CHECK(reg::kParameterOptionLabels[k][0] != '\0');
  }
}

static void jacks_are_valid() {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    const auto& j = reg::kJacks[i];
    CHECK(!j.stable_id.empty());
    CHECK(find_module(j.module) >= 0);      // owning module must exist
    CHECK(j.nominalMin <= j.nominalMax);
    CHECK(j.toleratedMin <= j.toleratedMax);
    CHECK(j.maxCables >= 1u);
    CHECK(j.modulationDepthPerVolt >= 0.0);
    CHECK(j.gateThresholdVolts >= 0.0);
    CHECK(j.hysteresisVolts >= 0.0);
    CHECK(valid_status(j.status));
    CHECK(valid_fe(j.fieldEvidence));       // per-field provenance split (07 §3, §10)
    CHECK(!j.evidence.source.empty());
  }
}

static void routes_resolve_and_sink_cardinality() {
  const std::uint32_t routeCount = core::kRouteCount;
  for (std::uint32_t i = 0; i < routeCount; ++i) {
    const auto& r = reg::kNormalizedRoutes[i];
    CHECK(find_jack(r.sourceJack) >= 0);     // source jack must exist
    CHECK(find_jack(r.sinkJack) >= 0);       // sink jack must exist
    CHECK(!r.stable_id.empty());
    CHECK(valid_status(r.status));
    CHECK(!r.evidence.source.empty());
    // Route is identified by its own stable numeric id, and that id round-trips
    // through the generated string switch (order-independent identity, 07 §7).
    CHECK(core::route_id_string(r.id) == r.stable_id);
    // A sink is the target of at most one route.
    for (std::uint32_t j = i + 1; j < routeCount; ++j) {
      CHECK_FALSE(reg::kNormalizedRoutes[j].sinkJack == r.sinkJack);
    }
  }
}

static void devices_capacity_matches_routes() {
  // The route-override bank is indexed by RouteId, so it must cover the id-space
  // (one-past-the-last id), not merely the count — a sparse route id is caught.
  CHECK(core::kDeviceRouteCapacity >= core::kRouteIdSpace);
  CHECK(core::kDeviceRouteCapacity >= core::kRouteCount);
}

static void id_string_lookup_is_complete() {
  // Every enum value must round-trip through the generated id-string switch, keyed
  // by each descriptor's OWN explicit numeric id (order-independent identity,
  // design/07 §7) — never by its JSON/array index. index==id is an accident to be
  // caught by keys-by-id, not an ordering guarantee to rely on.
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    const auto& d = reg::kModules[i];
    CHECK(core::module_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    const auto& d = reg::kParameters[i];
    CHECK(core::parameter_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    const auto& d = reg::kJacks[i];
    CHECK(core::jack_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kProgramCount; ++i) {
    const auto& d = reg::kPrograms[i];
    CHECK(core::program_id_string(d.id) == d.stable_id);
  }
  for (std::uint32_t i = 0; i < core::kRouteCount; ++i) {
    const auto& d = reg::kNormalizedRoutes[i];
    CHECK(core::route_id_string(d.id) == d.stable_id);
  }
}

int main() {
  frozen_counts();
  module_ranges_are_contiguous_and_disjoint();
  program_ranges_within_params();
  parameters_are_valid();
  selector_options_are_well_formed();
  jacks_are_valid();
  routes_resolve_and_sink_cardinality();
  devices_capacity_matches_routes();
  id_string_lookup_is_complete();
  return ::test::finish("registry");
}
