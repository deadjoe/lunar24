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
  return valid_status(fe.range) && valid_status(fe.threshold) &&
         valid_status(fe.saturation) && valid_status(fe.transfer);
}

static void frozen_counts() {
  // Locked P0 baseline — the audit target. These are the actual vertical-slice
  // counts in spec/machine/lunar24.json at the P0 lock.
  CHECK_EQ(core::kModuleCount, 5u);
  CHECK_EQ(core::kParameterCount, 30u);
  CHECK_EQ(core::kJackCount, 19u);
  CHECK_EQ(core::kProgramCount, 2u);
  // Routes live in kNormalizedRoutes[] (no id enum).
  CHECK_EQ(sizeof(reg::kNormalizedRoutes) / sizeof(reg::kNormalizedRoutes[0]), 3u);
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
    CHECK(valid_status(p.rangeEvidence));  // numeric-range provenance split (07 §10)
    CHECK(!p.evidence.source.empty());
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
  const std::uint32_t routeCount =
      static_cast<std::uint32_t>(sizeof(reg::kNormalizedRoutes) / sizeof(reg::kNormalizedRoutes[0]));
  for (std::uint32_t i = 0; i < routeCount; ++i) {
    const auto& r = reg::kNormalizedRoutes[i];
    CHECK(find_jack(r.sourceJack) >= 0);     // source jack must exist
    CHECK(find_jack(r.sinkJack) >= 0);       // sink jack must exist
    CHECK(!r.stable_id.empty());
    CHECK(valid_status(r.status));
    CHECK(!r.evidence.source.empty());
    // A sink is the target of at most one route.
    for (std::uint32_t j = i + 1; j < routeCount; ++j) {
      CHECK_FALSE(reg::kNormalizedRoutes[j].sinkJack == r.sinkJack);
    }
  }
}

static void devices_capacity_matches_routes() {
  const std::uint32_t routeCount =
      static_cast<std::uint32_t>(sizeof(reg::kNormalizedRoutes) / sizeof(reg::kNormalizedRoutes[0]));
  CHECK(core::kDeviceRouteCapacity >= routeCount);
}

static void id_string_lookup_is_complete() {
  // Every enum value must round-trip through the generated id-string switch.
  for (std::uint32_t i = 0; i < core::kModuleCount; ++i) {
    CHECK(core::module_id_string(core::ModuleId{i}) == reg::kModules[i].stable_id);
  }
  for (std::uint32_t i = 0; i < core::kParameterCount; ++i) {
    CHECK(core::parameter_id_string(core::ParameterId{i}) == reg::kParameters[i].stable_id);
  }
}

int main() {
  frozen_counts();
  module_ranges_are_contiguous_and_disjoint();
  program_ranges_within_params();
  parameters_are_valid();
  jacks_are_valid();
  routes_resolve_and_sink_cardinality();
  devices_capacity_matches_routes();
  id_string_lookup_is_complete();
  return ::test::finish("registry");
}
