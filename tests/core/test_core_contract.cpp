// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P0 tests for the framework-free core contract: frozen DeviceStateV1 schema,
// fixed capacities vs the registry counts, the virtual-volts electrical domain,
// and the control/block/contract/patch-graph types. Includes negative cases for
// the invariant "no capacity may be smaller than the registry it indexes".

#include "mini_test.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

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
  CHECK_EQ(st.schemaVersion, core::kDeviceStorageSchemaVersion);
  CHECK_EQ(st.identityModelVersion, 1u);
  CHECK_EQ(st.identitySeed.seed, 0ull);
  CHECK_EQ(st.calibration.vcfLeftTrim, 1.0f);
  CHECK_EQ(st.calibration.vcfRightTrim, 1.0f);
}

static void capacity_ge_registry() {
  // The fixed banks must cover the SERIALIZED ID SPACE they are indexed by (not
  // merely the count). id-space is one-past-the-last id, so a sparse id (a hole
  // between two ids) is still covered. registry_ids.hpp static_asserts the same
  // invariant at compile time; this test re-checks it at runtime against the
  // current spec.
  CHECK(core::kDeviceParamCapacity >= core::kParameterIdSpace);
  CHECK(core::kDevicePatchCapacity >= core::kJackIdSpace);
  CHECK(core::kDeviceRouteCapacity >= core::kRouteIdSpace);
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
  ev.sampleOffset = 42;
  ev.source = 5;
  ev.producerSequence = 7;
  CHECK(ev.kind == core::ControlEventKind::gate_on);
  CHECK(ev.lane() == core::ControlLane::critical);  // lane is DERIVED from kind (07 §3)
  CHECK(ev.source == 5u);
  // Same-sample ordering tiebreak (07 §3): later producerSequence is later.
  core::ControlEvent later = ev;
  later.producerSequence = 8;
  CHECK(later.producerSequence > ev.producerSequence);
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
  CHECK_FALSE(c.allowedInCyclicSCC);
  // Per-path causal facts (07 §2, §4): module-wide latency alone never grants
  // cycle-breaking credit — paths are named and each has its own min delay.
  CHECK_EQ(c.pathDelayCount, 0u);
  CHECK_EQ(c.pathDelays[0].inPort, core::JackId{0});
  CHECK_EQ(c.pathDelays[0].outPort, core::JackId{0});
  CHECK_EQ(c.pathDelays[0].minCausalDelaySamples, 0.0);
  CHECK_FALSE(c.pathDelays[0].canDirectThrough);
  CHECK_FALSE(c.pathDelays[0].directThroughExactZeroGain);
  // Each scheduling fact carries its OWN provenance (path semantics, not a
  // reused jack voltage field). All default to unverified until prepared.
  CHECK(c.pathDelays[0].evidence.minDelay == core::EvidenceStatus::unverified);
  CHECK(c.pathDelays[0].evidence.canDirectThrough == core::EvidenceStatus::unverified);
  CHECK(c.pathDelays[0].evidence.exactZeroGain == core::EvidenceStatus::unverified);
}

static void module_contract_validation() {
  // Invariants the scheduler relies on (design/07 §2, §4). A default contract is
  // valid; the negative cases must each reject one specific violation.
  core::ModuleExecutionContract c;
  CHECK(core::module_contract_is_valid(c));

  core::ModuleExecutionContract over;
  over.pathDelayCount = core::kMaxModulePathDelays + 1;
  CHECK_FALSE(core::module_contract_is_valid(over));  // count > path-array capacity

  core::ModuleExecutionContract self;
  self.pathDelayCount = 1;
  self.pathDelays[0].inPort = core::JackId{3};
  self.pathDelays[0].outPort = core::JackId{3};  // degenerate self-path
  CHECK_FALSE(core::module_contract_is_valid(self));

  core::ModuleExecutionContract zeroGain;
  zeroGain.pathDelayCount = 1;
  zeroGain.pathDelays[0].directThroughExactZeroGain = true;  // requires canDirectThrough
  CHECK_FALSE(core::module_contract_is_valid(zeroGain));

  core::ModuleExecutionContract delayedThrough;
  delayedThrough.pathDelayCount = 1;
  delayedThrough.pathDelays[0].minCausalDelaySamples = 16.0;
  delayedThrough.pathDelays[0].canDirectThrough = true;  // >0 min delay can't be zero-delay through
  CHECK_FALSE(core::module_contract_is_valid(delayedThrough));

  // NaN / Inf are not schedulable delays: `NaN < 0` is false so a plain <0 check
  // would let them through. Both must be rejected as non-finite.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  core::ModuleExecutionContract nanDelay;
  nanDelay.pathDelayCount = 1;
  nanDelay.pathDelays[0].minCausalDelaySamples = nan;
  CHECK_FALSE(core::module_contract_is_valid(nanDelay));
  core::ModuleExecutionContract infDelay;
  infDelay.pathDelayCount = 1;
  infDelay.pathDelays[0].minCausalDelaySamples = inf;
  CHECK_FALSE(core::module_contract_is_valid(infDelay));

  // The module-wide hasDirectThroughPath flag must equal the derived result of
  // the per-path canDirectThrough facts — it may never contradict them. Each
  // case gives the path a distinct in/out pair so it is the FLAG MISMATCH, not a
  // degenerate self-path, that governs the rejection.
  core::ModuleExecutionContract flagOverclaim;
  flagOverclaim.pathDelayCount = 1;
  flagOverclaim.pathDelays[0].inPort = core::JackId{1};
  flagOverclaim.pathDelays[0].outPort = core::JackId{2};
  flagOverclaim.hasDirectThroughPath = true;  // claims a direct-through ... but no path declares one
  CHECK_FALSE(core::module_contract_is_valid(flagOverclaim));
  core::ModuleExecutionContract flagUnderclaim;
  flagUnderclaim.pathDelayCount = 1;
  flagUnderclaim.pathDelays[0].inPort = core::JackId{1};
  flagUnderclaim.pathDelays[0].outPort = core::JackId{2};
  flagUnderclaim.pathDelays[0].canDirectThrough = true;  // path can direct-through ...
  flagUnderclaim.hasDirectThroughPath = false;            // ... but the flag denies it
  CHECK_FALSE(core::module_contract_is_valid(flagUnderclaim));
  core::ModuleExecutionContract flagConsistent;
  flagConsistent.pathDelayCount = 1;
  flagConsistent.pathDelays[0].inPort = core::JackId{1};
  flagConsistent.pathDelays[0].outPort = core::JackId{2};
  flagConsistent.pathDelays[0].canDirectThrough = true;
  flagConsistent.hasDirectThroughPath = true;  // flag matches the per-path facts
  CHECK(core::module_contract_is_valid(flagConsistent));
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

static core::ControlEvent mk_event(core::ControlEventKind k, std::uint32_t offset,
                                   std::uint32_t src, std::uint64_t seq) {
  core::ControlEvent e;
  e.kind = k;
  e.sampleOffset = offset;
  e.source = src;
  e.producerSequence = seq;
  return e;
}

static void control_event_ordering() {
  // Deterministic same-block order (07 §3): sampleOffset → phase → source →
  // producerSequence. Provide a shuffled set; the sorted result must match the
  // canonical order below.
  std::vector<core::ControlEvent> v = {
      mk_event(core::ControlEventKind::gate_on,    2u, 9u, 5u),
      mk_event(core::ControlEventKind::reset,      0u, 1u, 1u),
      mk_event(core::ControlEventKind::gate_off,   1u, 2u, 2u),
      mk_event(core::ControlEventKind::parameter,  0u, 4u, 3u),
      mk_event(core::ControlEventKind::sync,       1u, 2u, 9u),
      mk_event(core::ControlEventKind::pitch,      0u, 4u, 6u),
  };
  std::stable_sort(v.begin(), v.end(), core::control_event_before);

  const core::ControlEventKind expect[] = {
      core::ControlEventKind::reset,      // phase 0, offset 0
      core::ControlEventKind::parameter,  // phase 1, offset 0, source 4, seq 3
      core::ControlEventKind::pitch,      // phase 1, offset 0, source 4, seq 6
      core::ControlEventKind::gate_off,   // phase 2, offset 1, source 2, seq 2
      core::ControlEventKind::sync,       // phase 3, offset 1, source 2, seq 9
      core::ControlEventKind::gate_on,    // phase 4, offset 2
  };
  for (int i = 0; i < 6; ++i) {
    CHECK(v[static_cast<std::size_t>(i)].kind == expect[i]);
  }
}

static std::uint32_t storage_type_bytes(core::StorageFieldType t) {
  switch (t) {
    case core::StorageFieldType::u8:  return 1u;
    case core::StorageFieldType::u16: return 2u;
    case core::StorageFieldType::u32: return 4u;
    case core::StorageFieldType::u64: return 8u;
    case core::StorageFieldType::f32: return 4u;
    case core::StorageFieldType::f64: return 8u;
  }
  return 0u;
}

static void device_storage_schema() {
  const auto& s = core::kDeviceStorageSchema;
  CHECK_EQ(s.schemaVersion, core::kDeviceStorageSchemaVersion);
  CHECK_EQ(s.revision, core::kDeviceStorageInitialRevision);
  CHECK(s.fields != nullptr);
  CHECK(s.fieldCount >= 8u);
  CHECK(s.totalBytesHint > 0u);

  // Recompute the canonical width from the declared field table so the hint can
  // never silently diverge from the schema.
  std::uint32_t recomputed = 0;
  for (std::uint32_t i = 0; i < s.fieldCount; ++i) {
    const auto& f = s.fields[i];
    CHECK(f.name != nullptr && f.name[0] != '\0');
    CHECK(f.versionFrom >= 1u && f.versionFrom <= s.schemaVersion);
    CHECK(f.count > 0u);
    if (f.kind == core::StorageFieldKind::scalar) {
      CHECK_EQ(f.count, 1u);
      recomputed += storage_type_bytes(f.type);
    } else if (f.kind == core::StorageFieldKind::array) {
      recomputed += storage_type_bytes(f.type) * f.count;
    } else if (f.kind == core::StorageFieldKind::record) {
      CHECK(f.itemBytes > 0u);
      recomputed += f.itemBytes * f.count;
    } else {  // reserved: fixed byte block, no value invented
      recomputed += f.count;
    }
  }
  CHECK_EQ(recomputed, s.totalBytesHint);

  // A record's machine-readable interior (if declared) must be self-consistent:
  // every sub-field has a name, sits inside the record's itemBytes, and does not
  // overlap a sibling. This is what lets a later serializer name-encode the record.
  for (std::uint32_t i = 0; i < s.fieldCount; ++i) {
    const auto& f = s.fields[i];
    if (f.kind != core::StorageFieldKind::record) continue;
    const auto& rec = f.record;
    if (rec.fieldCount == 0u) continue;  // opaque record block (no interior declared)
    CHECK(rec.fields != nullptr);
    std::uint32_t cursor = 0u;
    for (std::uint32_t j = 0; j < rec.fieldCount; ++j) {
      const auto& rf = rec.fields[j];
      CHECK(rf.name != nullptr && rf.name[0] != '\0');
      CHECK(rf.sizeBytes > 0u);
      CHECK(rf.versionFrom >= 1u && rf.versionFrom <= s.schemaVersion);
      CHECK(rf.offset >= cursor);  // ordered + non-overlapping as the wire writes them
      CHECK(rf.offset + rf.sizeBytes <= f.itemBytes);
      cursor = rf.offset + rf.sizeBytes;
    }
  }

  // The named fields a future serialization layer walks by name.
  bool found_params = false, found_presets = false, found_settings = false, found_effector = false;
  bool found_seq_current = false, found_scale_editor = false, found_plate_tune = false;
  bool found_pushbutton = false, found_clock_selectors = false;
  for (std::uint32_t i = 0; i < s.fieldCount; ++i) {
    std::string_view n = s.fields[i].name;
    if (n == "parameters") found_params = true;
    if (n == "keyboard_presets") found_presets = true;
    if (n == "keyboard_settings") found_settings = true;
    if (n == "effector_left_program" || n == "effector_right_program") found_effector = true;
    if (n == "keyboard_seq_current") found_seq_current = true;
    if (n == "keyboard_scale_editor") found_scale_editor = true;
    if (n == "keyboard_plate_tune") found_plate_tune = true;
    if (n == "keyboard_pushbutton") found_pushbutton = true;
    if (n == "keyboard_clock_selectors") found_clock_selectors = true;
  }
  CHECK(found_params && found_presets && found_settings && found_effector);
  // P4-②: the live keyboard non-scalar / no-domain-selector fields are present.
  CHECK(found_seq_current && found_scale_editor && found_plate_tune &&
        found_pushbutton && found_clock_selectors);

  // Effector selects a STABLE ProgramId, not a registry array index.
  core::EffectorSelection l;
  l.program = core::ProgramId{1};
  CHECK(l.program == core::ProgramId{1});

  // A keyboard preset carries its own keyboard-owned state (not a bare id) — the
  // frozen keyboard_params_minus_clock payload (P4-②). The shell (id / behaviour
  // / output) is preserved; the full 33-sub-field interior is name-encodable.
  core::KeyboardPreset p;
  p.id = 3u;
  p.pressureBehaviour = 1u;
  p.pressureOutput = 2u;
  p.reserved[0] = 0xEAu;
  p.reserved[1] = 0xF5u;  // a "version-N wrote here" byte; preserved, never zeroed
  CHECK_EQ(p.id, 3u);
  CHECK_EQ(p.pressureBehaviour, 1u);
  CHECK_EQ(p.pressureOutput, 2u);
  CHECK_EQ(p.reserved[0], 0xEAu);
  CHECK_EQ(p.reserved[1], 0xF5u);
  CHECK_EQ(core::kKeyboardPresetRecordBytes, 247u);  // 8-byte shell + 29 appended fields
  CHECK_EQ(static_cast<std::uint32_t>(core::kKeyboardPresetLayout.fieldCount), 33u);

  // The live keyboard state carries the non-scalars (design/07 §6) as structured
  // fields, never flattened into a scalar descriptor.
  core::KeyboardSeq seq;
  CHECK_EQ(core::kKeyboardSeqStepCount, 16u);
  CHECK_EQ(core::kKeyboardSeqStepBytes, 6u);
  CHECK_EQ(core::kKeyboardSeqBytes, 96u);
  // A seq step is note(u8) + value(f32) + gate(u8), packed.
  seq.steps[3].note = 7u;
  seq.steps[3].value = 0.5f;
  seq.steps[3].gate = 1u;
  CHECK_EQ(seq.steps[3].note, 7u);
  CHECK_EQ(seq.steps[3].value, 0.5f);
  CHECK_EQ(seq.steps[3].gate, 1u);

  // Sequencer holds a named reserved block — not a fake opaque packed decode.
  core::SequencerSettings sequ;
  CHECK_EQ(static_cast<std::uint32_t>(sizeof(sequ.reserved)),
           core::kSequencerPhysicalBytes);
  CHECK(core::kSequencerPhysicalBytes > 0u);
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
  int transfer[] = {static_cast<int>(core::SignalTransfer::linear),
                    static_cast<int>(core::SignalTransfer::exponential),
                    static_cast<int>(core::SignalTransfer::none),
                    static_cast<int>(core::SignalTransfer::unknown)};
  CHECK(transfer[0] == 0 && transfer[1] == 1 && transfer[2] == 2 && transfer[3] == 3);
  int sat[] = {static_cast<int>(core::SaturationType::none),
               static_cast<int>(core::SaturationType::hard),
               static_cast<int>(core::SaturationType::soft),
               static_cast<int>(core::SaturationType::unknown)};
  CHECK(sat[0] == 0 && sat[1] == 1 && sat[2] == 2 && sat[3] == 3);
}

int main() {
  device_state_is_frozen();
  capacity_ge_registry();
  virtual_volts_domain();
  control_event_contract();
  control_event_ordering();
  audio_block_view_has_four_outputs();
  module_execution_contract_flags();
  module_contract_validation();
  patch_graph_types_and_handles();
  device_storage_schema();
  enum_category_stability();
  return ::test::finish("core contract");
}
