// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Default-state + validator + migration oracle (task #75 revision 3 — Codex BLOCK bd53b76a).
//
// Revision 3 adds/replaces the revision-2 oracle:
//   * validate_device_state() no longer takes a caller-supplied schema version — it
//     requires kDeviceStorageSchemaVersion directly. default_is_valid_and_identity_faithful
//     forms a VALID state by construction (identitySeed == caller seed exactly).
//   * ROUNDTRIP: migrate(default, out) produces a candidate that validates ok.
//   * ALL-FAILURE ZERO-MUTATION: for every family poison, the validator and the failing
//     migration leave the caller bitwise unchanged (observed via a byte snapshot).
//   * SIX DETACHED OLD-ERROR RED->GREEN categories that revision 2 accepted (false-green):
//       (A) older schema with a caller-supplied "expected v5"      -> wrong_schema_version
//       (B) calibration trim == 0 (finite but not >0)              -> calibration_invalid
//       (C) fractional selector (in-range but not integer)         -> parameter_out_of_range
//       (D) keyboard mirror drift (params vs KeyboardSettings)     -> keyboard_live_invalid
//       (E) route override whose sink has no user cable (lookup)   -> route_incoherent
//       (F) no-cable slot carrying a non-zero source jon           -> cable_incoherent
//   Each is asserted RED below (the correct, not false-green, result).

#include "mini_test.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <lunar24/core/state_default.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/core/state_migration.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/core/state_validation.h>
#include <lunar24/core/unit_identity_profile.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;
namespace vd = core::validate_detail;

// A landed INPUT jack, a landed OUTPUT jack, and a HOLE jack id (not landed).
// second_input_jack is the next landed input (for the source-aggregate cardinality
// test); cable_route_sink is a landed input jack that is a route's sink (for the
// legal cable+route positive); first_bad_program_id is a stable find_program()==nullptr.
static core::JackId first_input_jack = core::JackId{0};
static core::JackId second_input_jack = core::JackId{0};
static core::JackId first_output_jack = core::JackId{0};
static core::JackId first_hole_jack = core::JackId{0};
static core::JackId cable_route_sink = core::JackId{0};
static std::uint32_t landed_route_id = 0;
static std::uint32_t unlanded_route_id = 0;
static core::ProgramId first_bad_program_id = core::ProgramId::program_cathedral_1;

static constexpr std::uint32_t kInvalidField = 0xFFFFFFFFu;

// The frozen wire-record size, used as a constexpr array bound (the serializer's
// totalBytesHint is a constant-expression member of the constexpr storage schema).
static constexpr std::size_t kWireBytes = core::kDeviceStorageSchema.totalBytesHint;

static void discover_ids_for_poison() {
  bool found_in = false, found_in2 = false, found_out = false, found_hole = false;
  for (std::uint32_t id = 0; id < core::kDevicePatchCapacity; ++id) {
    const core::JackDescriptor* d = vd::find_jack(id);
    if (d == nullptr) {
      if (!found_hole) { first_hole_jack = static_cast<core::JackId>(id); found_hole = true; }
      continue;
    }
    if (d->direction == core::PinDirection::input && !found_in) {
      first_input_jack = static_cast<core::JackId>(id); found_in = true;
    } else if (d->direction == core::PinDirection::input && !found_in2) {
      second_input_jack = static_cast<core::JackId>(id); found_in2 = true;
    } else if (d->direction == core::PinDirection::output && !found_out) {
      first_output_jack = static_cast<core::JackId>(id); found_out = true;
    }
    if (found_in && found_in2 && found_out && found_hole) break;
  }
  CHECK(found_in);
  CHECK(found_in2);
  CHECK(found_out);
  CHECK(found_hole);
  for (std::uint32_t i = 0; i < core::kDeviceRouteCapacity; ++i)
    if (vd::find_route(i) != nullptr) { landed_route_id = i; break; }
  for (std::uint32_t i = 0; i < core::kDeviceRouteCapacity; ++i)
    if (vd::find_route(i) == nullptr) { unlanded_route_id = i; break; }
  CHECK(vd::find_route(landed_route_id) != nullptr);
  CHECK(vd::find_route(unlanded_route_id) == nullptr);
  // A landed route whose sink is a landed INPUT jack — plant a real cable on its sink
  // and declare the override to build a legal (non-poisoned) cable+route state.
  for (std::uint32_t i = 0; i < core::kDeviceRouteCapacity; ++i) {
    const core::NormalizedRoute* r = vd::find_route(i);
    if (r == nullptr) continue;
    const std::uint32_t sink = static_cast<std::uint32_t>(r->sinkJack);
    if (sink >= core::kDevicePatchCapacity) continue;
    const core::JackDescriptor* sd = vd::find_jack(sink);
    if (sd != nullptr && sd->direction == core::PinDirection::input) {
      cable_route_sink = r->sinkJack;
      break;
    }
  }
  CHECK(vd::find_jack(static_cast<std::uint32_t>(cable_route_sink)) != nullptr);
  // A stable ProgramId with no descriptor. We SEARCH for it (find_program()==nullptr)
  // rather than assume the program index space is dense (revision-3 assumed
  // kProgramCount == the first bad id, which only holds under density).
  bool found_bad_program = false;
  for (std::uint32_t v = 0; v <= core::kProgramCount; ++v) {
    if (core::find_program(static_cast<core::ProgramId>(v)) == nullptr) {
      first_bad_program_id = static_cast<core::ProgramId>(v);
      found_bad_program = true;
      break;
    }
  }
  CHECK(found_bad_program);
}

static std::uint32_t pid_index(core::ParameterId id) { return static_cast<std::uint32_t>(id); }

static void expect_family(const core::DeviceStateV1& st,
                          core::ValidationFamily want_family,
                          std::uint32_t want_field = kInvalidField) {
  const core::StateValidationResult r = core::validate_device_state(st);
  CHECK_FALSE(r.ok);
  CHECK(r.family == want_family);
  if (want_field != kInvalidField) CHECK_EQ(r.field, want_field);
}

// Default is valid, identitySeed equals the caller seed, and the default converges the
// #57 keyboard mirrors (so it is valid by construction).
static void default_is_valid_and_identity_faithful() {
  const core::DeviceStateV1 st = core::make_default_device_state(0xBEEFU);
  const core::StateValidationResult r = core::validate_device_state(st);
  CHECK_TRUE(r.ok);
  CHECK(r.family == core::ValidationFamily::ok);
  CHECK_EQ(st.identitySeed.seed, 0xBEEFU);
  // Mirrors converge (revision-3 requirement): params[behaviour]==pressureBehaviour,
  // params[pressure_output]==pressureOutput.
  CHECK(st.parameters[pid_index(core::ParameterId::keyboard_behaviour)] ==
        static_cast<double>(st.keyboardSettings.pressureBehaviour));
  CHECK(st.parameters[pid_index(core::ParameterId::keyboard_pressure_output)] ==
        static_cast<double>(st.keyboardSettings.pressureOutput));
  // ProgramId by descriptor lookup: the default selects a LANDED program by stable id
  // (cathedral.1 == ProgramId{0}), never a dense 0 sentinel or an id with no descriptor.
  CHECK(st.leftEffector.program == core::ProgramId::program_cathedral_1);
  CHECK(st.rightEffector.program == core::ProgramId::program_cathedral_1);
  CHECK(core::find_program(st.leftEffector.program) != nullptr);
}

// ROUNDTRIP (revision-3 requirement 7): migrate(current) produces a candidate that
// validates. The source is unchanged (const) and the out candidate is valid.
static void migration_roundtrip_produces_valid_candidate() {
  const core::DeviceStateV1 st = core::make_default_device_state(0x0D00UL);
  std::uint8_t before[sizeof(core::DeviceStateV1)];
  std::memcpy(before, &st, sizeof(st));
  core::DeviceStateV1 out{};
  const core::MigrationResult m = core::migrate_device_state(st, out);
  CHECK_TRUE(m.ok);
  CHECK(m.status == core::MigrationStatus::current_version);
  CHECK_EQ(m.fromVersion, core::kDeviceStorageSchemaVersion);
  CHECK(std::memcmp(before, &st, sizeof(st)) == 0);  // source bitwise unchanged
  CHECK_TRUE(core::validate_device_state(out).ok);   // the produced candidate validates
  // Current-version migration is a real passthrough: the out candidate must be
  // BITWISE identical to the source (not merely source-unchanged + out-validates).
  CHECK(std::memcmp(&out, &st, sizeof(st)) == 0);
}

// FULL default oracle (Revision 4 point 5): not spot-checks, but every landed
// parameter equals its registry initial, every hole/slack slot is exactly 0, the
// right bank mirrors the left fully, all four presets carry a valid id + full
// in-range L/R scalar payload, and both L/R ProgramIds are landed by lookup.
static void default_full_oracle() {
  const std::uint64_t seed = 0xFEEDUL;
  const core::DeviceStateV1 st = core::make_default_device_state(seed);
  CHECK_EQ(st.identitySeed.seed, seed);
  CHECK_EQ(st.identityModelVersion, core::kIdentityModelVersionSupported);
  CHECK_EQ(st.schemaVersion, core::kDeviceStorageSchemaVersion);
  // Landed params == registry initial; holes/slack == 0 (every parameter slot).
  for (std::uint32_t i = 0; i < core::kDeviceParamCapacity; ++i) {
    const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(i));
    if (d != nullptr)
      CHECK(st.parameters[i] == d->initial);
    else
      CHECK(st.parameters[i] == 0.0);
  }
  // Right bank fully mirrors the left (kKeyboardScalarRightCount entries).
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j)
    CHECK(st.keyboardScalarRight[j] ==
          st.parameters[static_cast<std::uint32_t>(core::kKeyboardScalarParameterIds[j])]);
  // All four presets: valid id + full in-range L/R scalar payload.
  for (std::uint32_t k = 0; k < core::kDeviceKeyboardPresetCount; ++k) {
    const core::KeyboardPreset& p = st.keyboardPresets[k];
    CHECK_EQ(p.id, k);
    for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
      const core::ParameterId pid = core::kKeyboardScalarParameterIds[j];
      const core::ParameterDescriptor* d = core::find_parameter(pid);
      CHECK(d != nullptr);
      double left = 0.0, right = 0.0;
      core::read_preset_scalar_pair(p, pid, &left, &right);
      CHECK_TRUE(vd::check_scalar_value(left, *d, 0u).ok);
      CHECK_TRUE(vd::check_scalar_value(right, *d, 0u).ok);
    }
  }
  // L/R ProgramId both landed (by descriptor lookup, never a dense index).
  CHECK(core::find_program(st.leftEffector.program) != nullptr);
  CHECK(core::find_program(st.rightEffector.program) != nullptr);
  CHECK(st.leftEffector.program == core::ProgramId::program_cathedral_1);
  CHECK(st.rightEffector.program == core::ProgramId::program_cathedral_1);
}

// REAL wire round-trip (Revision 4 point 1): default -> encode -> decode -> migrate
// -> validate -> re-encode, with the wire bytes pinned stable. This is NOT the
// revision-3 "default -> migrate -> validate" — encode/decode are actually invoked.
// It also proves the decoder is layer-agnostic: it never runs semantic validation (a
// wire-encodable but semantically-invalid state decodes OK, and ONLY validate_device_state
// rejects it — catching a blocker that smuggles semantic validation into decode()).
static void wire_roundtrip_is_stable_and_decoder_is_agnostic() {
  const core::DeviceStateV1 st = core::make_default_device_state(0xBEEF0001UL);
  std::uint8_t buf1[kWireBytes];
  std::size_t w1 = 0;
  CHECK_TRUE(core::encode_device_state(st, buf1, sizeof(buf1), &w1));
  CHECK_EQ(w1, core::kDeviceStorageSchema.totalBytesHint);

  core::DeviceStateV1 decoded{};
  CHECK_TRUE(core::decode_device_state(buf1, w1, &decoded));

  core::DeviceStateV1 migrated{};
  CHECK_TRUE(core::migrate_device_state(decoded, migrated).ok);
  CHECK_TRUE(core::validate_device_state(migrated).ok);
  // current-version migration is a pure passthrough: migrated == decoded bitwise.
  CHECK(std::memcmp(&migrated, &decoded, sizeof(decoded)) == 0);

  std::uint8_t buf2[kWireBytes];
  std::size_t w2 = 0;
  CHECK_TRUE(core::encode_device_state(migrated, buf2, sizeof(buf2), &w2));
  CHECK_EQ(w2, core::kDeviceStorageSchema.totalBytesHint);
  CHECK(std::memcmp(buf1, buf2, sizeof(buf1)) == 0);  // wire bytes stable

  // Decoder is agnostic: an encodable-but-semantically-INVALID state still encodes
  // + decodes; only validate rejects it (field = the offending landed param).
  core::DeviceStateV1 bad = core::make_default_device_state(0xBEEF0002UL);
  bad.parameters[pid_index(core::ParameterId::lfo_a_rate)] = 1000.0;
  std::uint8_t bbuf[kWireBytes];
  std::size_t bw = 0;
  CHECK_TRUE(core::encode_device_state(bad, bbuf, sizeof(bbuf), &bw));
  core::DeviceStateV1 bdecoded{};
  CHECK_TRUE(core::decode_device_state(bbuf, bw, &bdecoded));
  const core::StateValidationResult r = core::validate_device_state(bdecoded);
  CHECK_FALSE(r.ok);
  CHECK(r.family == core::ValidationFamily::parameter_out_of_range);
  CHECK_EQ(r.field, pid_index(core::ParameterId::lfo_a_rate));
}

// Expanded BAD-VALUE MATRIX (Revision 4 point 2). Revision-3 only spot-checked a pid
// hole (4), NaN, and one fractional selector. This adds Inf, off-grid step (via a
// synthetic descriptor), capacity slack (pid 412), the RIGHT bank, preset L/R scalar
// pairs, live/preset mask+gate, the source-aggregate maxCables cardinality, and a
// positive legal cable+route state. Each guarded deletion would turn the test red.
static void expanded_bad_value_matrix() {
  // Inf in a landed parameter (not just NaN).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA1UL);
    st.parameters[0] = std::numeric_limits<double>::infinity();
    expect_family(st, core::ValidationFamily::non_finite_state, 0u);
  }
  // Off-grid STEP. No real registry parameter has optionCount==0 && step>0 (every
  // step>0 param is a selector), so this exercises check_scalar_value's step grid
  // with a synthetic descriptor: 0.25 is in [0,1] but off the 0.5 grid.
  {
    core::ParameterDescriptor d{};
    d.min = 0.0; d.max = 1.0; d.step = 0.5;
    CHECK_FALSE(vd::check_scalar_value(0.25, d, 0u).ok);
    CHECK(vd::check_scalar_value(0.25, d, 0u).family == core::ValidationFamily::parameter_out_of_range);
    CHECK_TRUE(vd::check_scalar_value(0.5, d, 0u).ok);
  }
  // Capacity SLACK: pid 412 (> last landed 411) with a non-zero value.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA2UL);
    st.parameters[412] = 1.0;
    expect_family(st, core::ValidationFamily::invalid_parameter_slot, 412u);
  }
  // RIGHT bank: a live right-scalar out of range (field = 424 + j, j=0 = keyboard_mode).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA3UL);
    st.keyboardScalarRight[0] = 1e9;
    expect_family(st, core::ValidationFamily::parameter_out_of_range,
                  core::kDeviceParamCapacity + 0u);
  }
  // Preset LEFT scalar pair out of range (f32-backed index 4 = arp_interval, field 10000+j).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA4UL);
    st.keyboardPresets[0].arpInterval = 1e9f;
    expect_family(st, core::ValidationFamily::parameter_out_of_range, 10000u + 4u);
  }
  // Preset RIGHT scalar pair out of range (field 20000+j).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA5UL);
    st.keyboardPresets[0].arpIntervalR = 1e9f;
    expect_family(st, core::ValidationFamily::parameter_out_of_range, 20000u + 4u);
  }
  // Live RIGHT-bank quantise mask overflow (> 0x0FFF). LEFT mask is covered in
  // each_family_is_reachable; this pins the right _R mirror path.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA6UL);
    st.keyboardScaleEditorR = static_cast<std::uint16_t>(vd::kScaleEditorMaskLimit + 1u);
    expect_family(st, core::ValidationFamily::keyboard_live_invalid, 1u);
  }
  // Live seq GATE not boolean (>1).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA7UL);
    st.keyboardSeqCurrent.steps[0].gate = 2u;
    expect_family(st, core::ValidationFamily::keyboard_live_invalid, 6000u + 0u);
  }
  // Preset seq GATE not boolean (>1).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA8UL);
    st.keyboardPresets[0].seqSteps.steps[0].gate = 2u;
    expect_family(st, core::ValidationFamily::preset_invalid, 30070u + 0u);
  }
  // Preset global pressureBehaviour selector at/over its optionCount.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xA9UL);
    const core::ParameterDescriptor* b = core::find_parameter(core::ParameterId::keyboard_behaviour);
    CHECK(b != nullptr);
    st.keyboardPresets[0].pressureBehaviour = static_cast<std::uint8_t>(b->optionCount);
    expect_family(st, core::ValidationFamily::preset_invalid, 30000u + 0u);
  }
  // Source-AGGREGATE maxCables cardinality: one output jack sourced by two landed
  // inputs overflows the (default 1u) aggregate share.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xAAUL);
    const std::uint32_t a = static_cast<std::uint32_t>(first_input_jack);
    const std::uint32_t b = static_cast<std::uint32_t>(second_input_jack);
    st.inputCable[a] = 1; st.cableSource[a] = first_output_jack;
    st.inputCable[b] = 1; st.cableSource[b] = first_output_jack;
    expect_family(st, core::ValidationFamily::cable_cardinality,
                  static_cast<std::uint32_t>(first_output_jack));
  }
  // POSITIVE legal cable+route: one real cable on a route's sink + the override
  // declared. The whole state validates (the inverse of every poison above).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0xABUL);
    const std::uint32_t sink = static_cast<std::uint32_t>(cable_route_sink);
    st.inputCable[sink] = 1;
    st.cableSource[sink] = first_output_jack;
    for (std::uint32_t i = 0; i < core::kDeviceRouteCapacity; ++i) {
      const core::NormalizedRoute* r = vd::find_route(i);
      if (r != nullptr && static_cast<std::uint32_t>(r->sinkJack) == sink)
        st.routeOverridden[i] = 1;
    }
    CHECK_TRUE(core::validate_device_state(st).ok);
  }
}

static void each_family_is_reachable() {
  // wrong_schema_version — a schema other than the current one (no caller-parameterization).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0AUL);
    st.schemaVersion = core::kDeviceStorageSchemaVersion - 1u;
    expect_family(st, core::ValidationFamily::wrong_schema_version, st.schemaVersion);
  }
  // identity_invalid — unsupported identity model version (via isSupportedIdentityVersion).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0BUL);
    st.identityModelVersion = core::kIdentityModelVersionSupported + 1u;
    expect_family(st, core::ValidationFamily::identity_invalid, st.identityModelVersion);
  }
  // calibration_invalid — non-positive calibration trim (finite but <=0 is still bad).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0CUL);
    st.calibration.vcfLeftTrim = 0.0f;  // finite, but must be >0
    expect_family(st, core::ValidationFamily::calibration_invalid, 0u);
  }
  // non_finite_state — a NaN in a landed parameter (finite is tested before range).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0DUL);
    st.parameters[0] = std::nan("");
    expect_family(st, core::ValidationFamily::non_finite_state, 0u);
  }
  // parameter_out_of_range — a continuous landed parameter above its max.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0EUL);
    st.parameters[pid_index(core::ParameterId::lfo_a_rate)] = 1000.0;
    expect_family(st, core::ValidationFamily::parameter_out_of_range,
                  pid_index(core::ParameterId::lfo_a_rate));
  }
  // invalid_parameter_slot — a non-zero value in an ID hole (pid 4 is a hole).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x0FUL);
    st.parameters[4] = 1.0;
    expect_family(st, core::ValidationFamily::invalid_parameter_slot, 4u);
  }
  // illegal_jack_id — a cable occupying a hole jack slot (no such jack).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x10UL);
    st.inputCable[static_cast<std::uint32_t>(first_hole_jack)] = 1;
    expect_family(st, core::ValidationFamily::illegal_jack_id,
                  static_cast<std::uint32_t>(first_hole_jack));
  }
  // cable_direction — a cable planted into an OUTPUT jack (dest is not input).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x11UL);
    st.inputCable[static_cast<std::uint32_t>(first_output_jack)] = 1;
    expect_family(st, core::ValidationFamily::cable_direction,
                  static_cast<std::uint32_t>(first_output_jack));
  }
  // cable_cardinality — the presence bit is not a count (must be exactly 0 or 1).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x12UL);
    st.inputCable[static_cast<std::uint32_t>(first_input_jack)] = 2;
    expect_family(st, core::ValidationFamily::cable_cardinality,
                  static_cast<std::uint32_t>(first_input_jack));
  }
  // cable_incoherent — a cable whose source jack id is absent (a hole).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x13UL);
    const std::uint32_t i = static_cast<std::uint32_t>(first_input_jack);
    st.inputCable[i] = 1;
    st.cableSource[i] = first_hole_jack;
    expect_family(st, core::ValidationFamily::cable_incoherent, i);
  }
  // route_incoherent — an override on a non-landed RouteId (lookup, not dense index).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x14UL);
    st.routeOverridden[unlanded_route_id] = 1;
    expect_family(st, core::ValidationFamily::route_incoherent, unlanded_route_id);
  }
  // illegal_program_id — a program selection with no descriptor. We poison with a
  // DISCOVERED find_program()==nullptr value (never assume a dense index space).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x15UL);
    st.leftEffector.program = first_bad_program_id;
    expect_family(st, core::ValidationFamily::illegal_program_id, 0u);
  }
  // keyboard_live_invalid — the 12-bit scale-editor mask overflows mask limit.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x16UL);
    st.keyboardScaleEditor = static_cast<std::uint16_t>(vd::kScaleEditorMaskLimit + 1u);
    expect_family(st, core::ValidationFamily::keyboard_live_invalid, 0u);
  }
  // preset_invalid — a preset slot whose id does not equal its index (pinned).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x17UL);
    st.keyboardPresets[0].id = 1u;
    expect_family(st, core::ValidationFamily::preset_invalid, 0u);
  }
}

// Deterministic FIRST-failure ordering: the walk reports the earliest family, never a
// cascade. With schema failing first it must beat a later non-finite parameter.
static void first_failure_is_deterministic() {
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x30UL);
    st.schemaVersion = core::kDeviceStorageSchemaVersion - 1u;  // wrong_schema_version
    st.parameters[0] = std::nan("");                            // non_finite_state (later)
    expect_family(st, core::ValidationFamily::wrong_schema_version);
  }
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x31UL);
    st.identityModelVersion = core::kIdentityModelVersionSupported;  // valid (not poisoned)
    st.parameters[0] = std::nan("");                                 // params (step 5) wins
    expect_family(st, core::ValidationFamily::non_finite_state);
  }
}

// ALL-FAILURE ZERO-MUTATION (revision-3 requirement 8): every family poison leaves the
// candidate bitwise unchanged after validate_device_state. The validator is const& and
// fixed-heap, so a rejected candidate is never half-written.
static void validator_never_mutates_the_candidate() {
  core::DeviceStateV1 cases[14];
  std::uint32_t n = 0;
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x40UL); st.schemaVersion = core::kDeviceStorageSchemaVersion - 1u; cases[n++] = st;
    st = core::make_default_device_state(0x41UL); st.identityModelVersion = core::kIdentityModelVersionSupported + 1u; cases[n++] = st;
    st = core::make_default_device_state(0x42UL); st.calibration.vcfLeftTrim = 0.0f; cases[n++] = st;
    st = core::make_default_device_state(0x43UL); st.parameters[0] = std::nan(""); cases[n++] = st;
    st = core::make_default_device_state(0x44UL); st.parameters[pid_index(core::ParameterId::lfo_a_rate)] = 1000.0; cases[n++] = st;
    st = core::make_default_device_state(0x45UL); st.parameters[4] = 1.0; cases[n++] = st;
    st = core::make_default_device_state(0x46UL); st.inputCable[static_cast<std::uint32_t>(first_hole_jack)] = 1; cases[n++] = st;
    st = core::make_default_device_state(0x47UL); st.inputCable[static_cast<std::uint32_t>(first_output_jack)] = 1; cases[n++] = st;
    st = core::make_default_device_state(0x48UL); st.inputCable[static_cast<std::uint32_t>(first_input_jack)] = 2; cases[n++] = st;
    st = core::make_default_device_state(0x49UL); { const std::uint32_t i = static_cast<std::uint32_t>(first_input_jack); st.inputCable[i] = 1; st.cableSource[i] = first_hole_jack; } cases[n++] = st;
    st = core::make_default_device_state(0x4AUL); st.routeOverridden[unlanded_route_id] = 1; cases[n++] = st;
    st = core::make_default_device_state(0x4BUL); st.leftEffector.program = first_bad_program_id; cases[n++] = st;
    st = core::make_default_device_state(0x4CUL); st.keyboardScaleEditor = static_cast<std::uint16_t>(vd::kScaleEditorMaskLimit + 1u); cases[n++] = st;
    st = core::make_default_device_state(0x4DUL); st.keyboardPresets[0].id = 1u; cases[n++] = st;
  }
  CHECK_EQ(n, 14u);
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint8_t before[sizeof(core::DeviceStateV1)];
    std::memcpy(before, &cases[i], sizeof(cases[i]));
    const core::StateValidationResult r = core::validate_device_state(cases[i]);
    CHECK_FALSE(r.ok);
    CHECK(std::memcmp(before, &cases[i], sizeof(cases[i])) == 0);
  }
}

// SIX DETACHED OLD-ERROR RED->GREEN categories (revision-2 false-greens, now red).
static void detached_old_error_categories() {
  // (A) older schema with a caller-supplied "expected v5": revision-2 accepted it because
  // the caller told the validator the schema it expected. Now the schema must match
  // directly -> wrong_schema_version.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x50UL);
    st.schemaVersion = core::kDeviceStorageSchemaVersion - 1u;
    expect_family(st, core::ValidationFamily::wrong_schema_version);
  }
  // (B) calibration trim == 0 (finite): revision-2 checked only isfinite -> green. now red.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x51UL);
    st.calibration.vcfRightTrim = 0.0f;
    expect_family(st, core::ValidationFamily::calibration_invalid, 1u);
  }
  // (C) fractional selector (in [0,optionCount) but not an integer): revision-2 checked
  // only [min,max] -> green. now red.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x52UL);
    st.parameters[pid_index(core::ParameterId::keyboard_behaviour)] = 1.5;
    expect_family(st, core::ValidationFamily::parameter_out_of_range,
                  pid_index(core::ParameterId::keyboard_behaviour));
  }
  // (D) keyboard mirror drift: params[keyboard_pressure_output] != KeyboardSettings.pressureOutput
  // (the #57 canonical LEFT). revision-2 had no coherence check -> green. now red.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x53UL);
    st.keyboardSettings.pressureOutput = 1;  // canonical LEFT drift (left still 0)
    expect_family(st, core::ValidationFamily::keyboard_live_invalid, 9003u);
  }
  // (D2) the behaviour compatibility mirror must equal KeyboardSettings.pressureBehaviour:
  // params[keyboard_behaviour] (an in-range selector) is left behind at 0 while the
  // canonical is untouched — a coherence FALSE-GREEN for a value that passes range alone.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x56UL);
    st.parameters[pid_index(core::ParameterId::keyboard_behaviour)] = 2;  // in-range selector
    expect_family(st, core::ValidationFamily::keyboard_live_invalid, 9002u);
  }
  // (E) route override whose sink has no user cable. revision-2 checked only the dense
  // i<kRouteCount index + v<=1 -> green. now red via kNormalizedRoutes lookup.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x54UL);
    st.routeOverridden[landed_route_id] = 1;  // this route's sink has no cable in the default
    expect_family(st, core::ValidationFamily::route_incoherent, landed_route_id);
  }
  // (F) a no-cable slot carrying a non-zero source: revision-2 skipped the cableSource when
  // inputCable==0 -> green (the dangling source went unnoticed). now red (presence-bit coherence).
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x55UL);
    const std::uint32_t i = static_cast<std::uint32_t>(first_input_jack);
    st.cableSource[i] = first_output_jack;  // no cable, but a source is recorded
    expect_family(st, core::ValidationFamily::cable_incoherent, i);
  }
}

// A deferred/unavailable parameter is NOT masqueraded as an applied-to-DSP live control,
// yet a valid in-range stored value is still a legal (landed) parameter. So its
// disposition must be transfer_unavailable (not applied_to_dsp) while validate returns ok.
static void deferred_unavailable_not_masqueraded_as_applied() {
  core::DeviceStateV1 st = core::make_default_device_state(0x60UL);
  st.parameters[pid_index(core::ParameterId::vco_a_pwm)] = 0.5;  // valid in-range, no consumer
  CHECK_TRUE(core::validate_device_state(st).ok);
  CHECK(core::disposition_of(core::ParameterId::vco_a_pwm) == core::StateDisposition::transfer_unavailable);
  CHECK(core::disposition_of(core::ParameterId::program_orche_3_z) == core::StateDisposition::preserved_deferred_p6_p8);
}

// Migration hook: current passes through (out = copy, ok), an older/newer version fails
// closed typed, and the out slot is bitwise unchanged on the rejected path.
static void migration_hook_fails_closed_zero_mutation() {
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x2AUL);
    core::DeviceStateV1 out{};
    const core::MigrationResult r = core::migrate_device_state(st, out);
    CHECK_TRUE(r.ok);
    CHECK(r.status == core::MigrationStatus::current_version);
    CHECK_EQ(r.fromVersion, core::kDeviceStorageSchemaVersion);
    CHECK_TRUE(core::validate_device_state(out).ok);
  }
  // Newer-than-known -> requires_newer_codec; out slot bitwise unchanged.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x2BUL);
    st.schemaVersion = core::kDeviceStorageSchemaVersion + 1u;
    core::DeviceStateV1 out{};
    std::uint8_t before[sizeof(core::DeviceStateV1)];
    std::memcpy(before, &out, sizeof(out));
    const core::MigrationResult r = core::migrate_device_state(st, out);
    CHECK_FALSE(r.ok);
    CHECK(r.status == core::MigrationStatus::requires_newer_codec);
    CHECK_EQ(r.fromVersion, core::kDeviceStorageSchemaVersion + 1u);
    CHECK(std::memcmp(before, &out, sizeof(out)) == 0);  // out unchanged
  }
  // Older/unrecognized -> unsupported_version; out slot bitwise unchanged.
  {
    core::DeviceStateV1 st = core::make_default_device_state(0x2CUL);
    st.schemaVersion = core::kDeviceStorageSchemaVersion - 1u;
    core::DeviceStateV1 out{};
    std::uint8_t before[sizeof(core::DeviceStateV1)];
    std::memcpy(before, &out, sizeof(out));
    const core::MigrationResult r = core::migrate_device_state(st, out);
    CHECK_FALSE(r.ok);
    CHECK(r.status == core::MigrationStatus::unsupported_version);
    CHECK_EQ(r.fromVersion, core::kDeviceStorageSchemaVersion - 1u);
    CHECK(std::memcmp(before, &out, sizeof(out)) == 0);  // out unchanged
  }
}

int main() {
  discover_ids_for_poison();
  default_is_valid_and_identity_faithful();
  default_full_oracle();
  wire_roundtrip_is_stable_and_decoder_is_agnostic();
  migration_roundtrip_produces_valid_candidate();
  each_family_is_reachable();
  first_failure_is_deterministic();
  validator_never_mutates_the_candidate();
  detached_old_error_categories();
  deferred_unavailable_not_masqueraded_as_applied();
  migration_hook_fails_closed_zero_mutation();
  expanded_bad_value_matrix();
  return ::test::finish("state validation (task #75 r4)");
}
