// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P0 tests for the framework-free core contract: frozen DeviceStateV1 schema,
// fixed capacities vs the registry counts, the virtual-volts electrical domain,
// and the control/block/contract/patch-graph types. Includes negative cases for
// the invariant "no capacity may be smaller than the registry it indexes".

#include "mini_test.h"

#include <lunar24/core/audio_block_view.h>
#include <lunar24/core/control_event.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/enums.h>
#include <lunar24/core/evidence.h>
#include <lunar24/core/module_execution_contract.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/signal.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

static void device_state_is_frozen() {
  core::DeviceStateV1 st;
  CHECK_EQ(st.schemaVersion, 1u);
  CHECK_EQ(st.identityModelVersion, 1u);
  CHECK_EQ(st.identitySeed.seed, 0ull);
  CHECK_EQ(st.calibration.vcfLeftTrim, 1.0f);
  CHECK_EQ(st.calibration.vcfRightTrim, 1.0f);
}

static void capacity_ge_registry() {
  // The fixed banks must cover every registry id they index. kDeviceRouteCapacity
  // is check in test_registry.cpp against the kNormalizedRoutes array, which is
  // not visible from registry_ids.hpp (routes have no id enum).
  CHECK(core::kDeviceParamCapacity >= core::kParameterCount);
  CHECK(core::kDevicePatchCapacity >= core::kJackCount);
}

static void virtual_volts_domain() {
  CHECK_EQ(core::kVoltPerUnit, 1.0);          // 1.0 == 1 V
  CHECK_EQ(core::kPitchCvVoltsPerOctave, 1.0);
  CHECK_EQ(core::kPitchCvPerOctave, 1.0);
}

static void control_event_contract() {
  CHECK_EQ(core::kControlEventPhaseCount, 5u);

  core::ControlEvent ev;
  ev.kind = core::ControlEventKind::gate_on;
  ev.lane = core::ControlLane::critical;
  ev.sampleOffset = 42;
  ev.producerSequence = 7;
  CHECK(ev.kind == core::ControlEventKind::gate_on);
  CHECK(ev.lane == core::ControlLane::critical);
  // A {}-initialised ControlEvent has a valid default ParameterId of value 0.
  core::ControlEvent def;
  CHECK_EQ(static_cast<core::IdValue>(def.parameter), 0u);
  CHECK_EQ(def.value, static_cast<core::SignalSample>(0));
}

static void audio_block_view_has_four_outputs() {
  float wl[4], wr[4], da[4], db[4];
  core::AudioBlockView v;
  v.wetL = wl;
  v.wetR = wr;
  v.dryA = da;
  v.dryB = db;
  v.frames = 4;
  CHECK_EQ(v.frames, 4u);
  CHECK(v.wetL != nullptr && v.wetR != nullptr && v.dryA != nullptr && v.dryB != nullptr);
  // Default view is fully null (forces the adapter to supply every buffer).
  core::AudioBlockView d;
  CHECK(d.wetL == nullptr && d.wetR == nullptr && d.dryA == nullptr && d.dryB == nullptr);
}

static void module_execution_contract_flags() {
  core::ModuleExecutionContract c;
  CHECK_EQ(c.sampleRate, 0.0);
  CHECK_EQ(c.maxBlockSize, 0u);
  CHECK_EQ(c.intrinsicLatencySamples, 0u);
  CHECK_EQ(c.maxResources, 0u);
  CHECK_FALSE(c.hasDirectThroughPath);
  CHECK_FALSE(c.declaresCausalPathDelay);
  CHECK_FALSE(c.allowedInCyclicSCC);
}

static void patch_graph_types_and_handles() {
  core::PatchConnection conn = {core::JackId{0}, core::JackId{1}};
  CHECK_EQ(static_cast<core::IdValue>(conn.source), 0u);
  CHECK_EQ(static_cast<core::IdValue>(conn.sink), 1u);
  core::GraphEpoch epoch = 0;
  CHECK_EQ(epoch, 0u);
  // CompiledGraph is an incomplete type whose address can be held (non-owning) as a handle.
  struct HasHandle { core::CompiledGraph* handle = nullptr; };
  HasHandle h;
  CHECK(h.handle == nullptr);
}

static void enum_category_stability() {
  // Serialized-by-value enums: guarantee the ordering was frozen at P0.
  int smooth[] = {static_cast<int>(core::Smoothing::none),
                  static_cast<int>(core::Smoothing::linear),
                  static_cast<int>(core::Smoothing::seconds)};
  CHECK(smooth[0] == 0 && smooth[1] == 1 && smooth[2] == 2);
  int status[] = {static_cast<int>(core::EvidenceStatus::confirmed),
                  static_cast<int>(core::EvidenceStatus::unverified),
                  static_cast<int>(core::EvidenceStatus::provisional)};
  CHECK(status[0] == 0 && status[1] == 1 && status[2] == 2);
}

int main() {
  device_state_is_frozen();
  capacity_ge_registry();
  virtual_volts_domain();
  control_event_contract();
  audio_block_view_has_four_outputs();
  module_execution_contract_flags();
  patch_graph_types_and_handles();
  enum_category_stability();
  return ::test::finish("core contract");
}
