// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1: the single whole-device current state (design/07 §6).
//
// It is the ONLY "last-exit state" for the whole machine and stays strictly
// separate from the keyboard's own four presets (which it nevertheless stores,
// because they are keyboard-owned, not whole-device presets).
//
// IMPORTANT — capacities are PROVISIONAL UPPER BOUNDS, not a frozen schema.
// They are not declared final until the full P0 inventory proves them, and the
// on-disk wire format is the separately-declared DeviceStorageSchema (a
// versioned, named record), never a memcpy of this raw C++ layout.

#pragma once

#include <cstddef>
#include <cstdint>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/id_types.h>

namespace lunar24::core {

// ---------------------------------------------------------------------------
// Portable storage schema (design/07 §6) — SEPARATE from the raw C++ layout.
//
// The on-disk format is a VERSIONED set of NAMED, TYPED, ENCODED fields. It is
// never a `memcpy` of the C++ struct across macOS/Windows: the in-memory
// DeviceStateV1 below is a framework-free working copy, and a device-
// authoritative serialization layer maps it to/from this declared schema by
// field name (that layer is a later slice — P0 declares the schema only).
//
// Storage field widths here are fixed-width and host-layout-independent (no
// `sizeof`, no padding dependence), which is what makes the record portable.
// Capacities are PROVISIONAL upper bounds until the full P0 inventory.
// ---------------------------------------------------------------------------

// Fixed-width item storage type (no host type re-use; all wire widths explicit).
enum class StorageFieldType : std::uint8_t {
  u8 = 0, u16 = 1, u32 = 2, u64 = 3, f32 = 4, f64 = 5,
};

// Wire format for a field's value. `binary` is fixed-width little-endian (the
// writer normalises); `text` is a human-readable token.
enum class StorageEncoding : std::uint8_t {
  binary = 0, text = 1,
};

// How a named field is laid out. `record` = `count` fixed-size items of
// `itemBytes`; `reserved` = `count` bytes held for a subtype still to be
// designed (declared by name, value semantics deferred). No range/value is
// invented for a `reserved` field.
enum class StorageFieldKind : std::uint8_t {
  scalar = 0,   // one value of `type`
  array = 1,    // `count` contiguous values of `type`
  record = 2,   // `count` fixed-size records of `itemBytes`
  reserved = 3, // `count` bytes reserved until the subtype lands
};

// One named, typed field *inside* a record, at an explicit byte offset. Fixed
// width only (no host `sizeof`/padding dependence), which is what keeps the wire
// record portable. Declared so a future serializer can name-encode a record by
// its own field names instead of treating it as an opaque byte block.
struct StorageRecordField {
  const char* name;
  StorageFieldType type;
  StorageEncoding encoding;
  std::uint32_t offset;      // byte offset within the record's itemBytes
  std::uint32_t sizeBytes;   // fixed wire width of this field
  std::uint32_t versionFrom;
};

// A record's machine-readable interior. `fields`/`fieldCount` describe exactly
// which named fields a `StorageFieldKind::record` carries and where they sit. A
// record with `fieldCount==0` is an opaque byte block at this layer (value
// semantics deferred), matching how `reserved` works for whole records.
struct StorageRecordLayout {
  const StorageRecordField* fields = nullptr;
  std::uint32_t fieldCount = 0;
};

// One NAME + TYPE + ENCODING + CARDINALITY of the persisted device record.
struct StorageField {
  const char* name;
  StorageFieldKind kind;
  StorageFieldType type;
  StorageEncoding encoding;
  std::uint32_t count;        // scalar=1, array/record=item count, reserved=bytes
  std::uint32_t itemBytes;    // record only: fixed width per record
  std::uint32_t versionFrom;  // first schema version the field appears in
  StorageRecordLayout record; // record only: machine-readable interior (opaque if fieldCount==0)
};

// The named, versioned "deviceState-v1" record. `fields` points at the declared
// field table; a future serializer walks it by name to emit/read the record.
struct DeviceStorageSchema {
  std::uint32_t schemaVersion = 1;
  std::uint32_t revision = 0;             // non-breaking additions
  std::uint32_t fieldCount = 0;
  const StorageField* fields = nullptr;   // -> kDeviceStorageFields[]
  std::uint32_t totalBytesHint = 0;       // canonical fixed-width byte size
};

// Fixed widths for the storage record, independent of C++ layout. PROVISIONAL
// until the full P0 inventory proves them, and never a memcpy target.
inline constexpr std::uint32_t kKeyboardPresetRecordBytes = 8u;  // id(4) + behaviour(1) + output(1) + pad(2)
inline constexpr std::uint32_t kKeyboardSettingsRecordBytes = 2u;  // pressure behaviour(1) + pressure output(1)
inline constexpr std::uint32_t kSequencerPhysicalBytes = 16u;    // reserved until the sequencer lands
inline constexpr std::uint32_t kDeviceStorageSchemaVersion = 1u;
inline constexpr std::uint32_t kDeviceStorageInitialRevision = 0u;

// A KeyboardPreset's machine-readable interior (offset within the 8-byte record).
inline constexpr StorageRecordField kKeyboardPresetFields[] = {
    {"id",                 StorageFieldType::u32, StorageEncoding::binary, 0u, 4u, 1u},
    {"pressure_behaviour", StorageFieldType::u8,  StorageEncoding::binary, 4u, 1u, 1u},
    {"pressure_output",    StorageFieldType::u8,  StorageEncoding::binary, 5u, 1u, 1u},
    {"reserved",           StorageFieldType::u8,  StorageEncoding::binary, 6u, 2u, 1u},
};
inline constexpr StorageRecordField kKeyboardSettingsFields[] = {
    {"pressure_behaviour", StorageFieldType::u8, StorageEncoding::binary, 0u, 1u, 1u},
    {"pressure_output",    StorageFieldType::u8, StorageEncoding::binary, 1u, 1u, 1u},
};

// The map-able record layouts referenced by the two keyboard records below.
inline constexpr StorageRecordLayout kKeyboardPresetLayout{kKeyboardPresetFields, 4u};
inline constexpr StorageRecordLayout kKeyboardSettingsLayout{kKeyboardSettingsFields, 2u};

inline constexpr StorageField kDeviceStorageFields[] = {
    {"schema_version",           StorageFieldKind::scalar,  StorageFieldType::u32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"identity_model_version",   StorageFieldKind::scalar,  StorageFieldType::u32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"identity_seed",            StorageFieldKind::scalar,  StorageFieldType::u64, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"calibration_vcf_left",     StorageFieldKind::scalar,  StorageFieldType::f32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"calibration_vcf_right",    StorageFieldKind::scalar,  StorageFieldType::f32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"parameters",               StorageFieldKind::array,   StorageFieldType::f64, StorageEncoding::binary, kDeviceParamCapacity, 0u, 1u, {}},
    {"input_cable",              StorageFieldKind::array,   StorageFieldType::u8,  StorageEncoding::binary, kDevicePatchCapacity, 0u, 1u, {}},
    {"cable_source",             StorageFieldKind::array,   StorageFieldType::u32, StorageEncoding::binary, kDevicePatchCapacity, 0u, 1u, {}},
    {"route_overridden",         StorageFieldKind::array,   StorageFieldType::u8,  StorageEncoding::binary, kDeviceRouteCapacity, 0u, 1u, {}},
    {"keyboard_presets",         StorageFieldKind::record,  StorageFieldType::u8,  StorageEncoding::binary, kDeviceKeyboardPresetCount, kKeyboardPresetRecordBytes, 1u, kKeyboardPresetLayout},
    {"keyboard_settings",        StorageFieldKind::record,  StorageFieldType::u8,  StorageEncoding::binary, 1u, kKeyboardSettingsRecordBytes, 1u, kKeyboardSettingsLayout},
    {"effector_left_program",    StorageFieldKind::scalar,  StorageFieldType::u32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"effector_right_program",   StorageFieldKind::scalar,  StorageFieldType::u32, StorageEncoding::binary, 1u, 0u, 1u, {}},
    {"sequencer_physical",       StorageFieldKind::reserved, StorageFieldType::u8, StorageEncoding::binary, kSequencerPhysicalBytes, 0u, 1u, {}},
};

inline constexpr std::uint32_t kDeviceStorageFieldCount =
    static_cast<std::uint32_t>(sizeof(kDeviceStorageFields) / sizeof(kDeviceStorageFields[0]));

// The declared storage record. `totalBytesHint` is the canonical fixed-width
// byte size (recomputed in a test so it can never silently diverge from the
// field table). It rose 3506 -> 3511 when kDevicePatchCapacity went 64 -> 65
// (DRONE 6, Codex msg cc68ab2b — Option A narrow release): the `input_cable` u8
// array grows +1 and the `cable_source` u32 array grows +4. It rose 3511 -> 3831
// in the parameter-capacity freeze (Codex msg deab14b7) when kDeviceParamCapacity
// went 384 -> 424: the `parameters` f64 array grows +40 (40 x 8 = 320 bytes).
inline constexpr DeviceStorageSchema kDeviceStorageSchema{
    kDeviceStorageSchemaVersion,
    kDeviceStorageInitialRevision,
    kDeviceStorageFieldCount,
    kDeviceStorageFields,
    3831u,
};

// Fixed per-unit constitution, not re-randomized per launch (design/07 §7).
struct UnitIdentitySeed {
  std::uint64_t seed = 0;
};

// Persistent per-unit calibration. Left/right channel normalization and
// level-dependent gain staging are independent.
struct CalibrationState {
  float vcfLeftTrim = 1.0f;
  float vcfRightTrim = 1.0f;
};

// Keyboard's current live settings. The manual names 'Behaviour' and 'Pressure
// output' as the two parameters steering the PRESSURE jack; the concrete value
// model is keyboard-subsystem-owned and completed later. PROVISIONAL.
struct KeyboardSettings {
  std::uint8_t pressureBehaviour = 0;  // PROVISIONAL decode
  std::uint8_t pressureOutput = 0;     // PROVISIONAL decode
};

// A KeyboardPreset is owned by the keyboard subsystem only; it is never
// serialized as a whole-device preset. Each of the kDeviceKeyboardPresetCount
// native presets carries its own keyboard-owned state. The manual names
// 'Behaviour' and 'Pressure output' as the two parameters steering the PRESSURE
// jack, so a preset holds them; the remaining keyboard-owned state lands when
// the keyboard subsystem is designed (PROVISIONAL decode). The wire record width
// is the schema-declared kKeyboardPresetRecordBytes, never sizeof here.
struct KeyboardPreset {
  std::uint32_t id = 0u;            // stable sys-selected preset id, never renumbered
  std::uint8_t pressureBehaviour = 0u;  // manual 'Behaviour' (PROVISIONAL decode)
  std::uint8_t pressureOutput = 0u;     // manual 'Pressure output' (PROVISIONAL decode)
};

// Dual-effector selection. Each slot selects a single processor idiom by its
// STABLE ProgramId (cartridge × slot × X/Y/Z are all fully identified by it) —
// no unstable registry array index, and no separate cartridge registry is
// invented. Left/right instances and their non-shared state never merge.
struct EffectorSelection {
  ProgramId program = ProgramId{0};  // stable program idiom (>= cathedral.1 is valid)
};

// Sequencer PHYSICAL settings only — NOT transient gate/playhead/envelope/delay
// state (design/07 §6 excludes those). The sequencer subsystem is a later slice,
// so P0 only reserves a fixed, NAMED byte block (declared as `sequencer_physical`
// in the storage schema, kind==reserved) to keep the wire format versioned and
// stable. No range/value is invented here.
struct SequencerSettings {
  std::uint8_t reserved[kSequencerPhysicalBytes] = {};
};

struct DeviceStateV1 {
  std::uint32_t schemaVersion = 1;
  std::uint32_t identityModelVersion = 1;
  UnitIdentitySeed identitySeed;
  CalibrationState calibration;

  // All physical knob/switch parameters, indexed by ParameterId. No allocation.
  double parameters[kDeviceParamCapacity] = {};

  // Patch facts. `inputCable[i]` is nonzero if a user cable is plugged into the
  // i-th input jack; `cableSource[i]` is that cable's source jack id.
  std::uint8_t inputCable[kDevicePatchCapacity] = {};
  JackId cableSource[kDevicePatchCapacity] = {};

  // Normalized-route override per RouteId. Indexed by a stable RouteId (design/07
  // §7), never by array position — kDeviceRouteCapacity is gated against the
  // generated kRouteIdSpace so a route id can never exceed this bank.
  std::uint8_t routeOverridden[kDeviceRouteCapacity] = {};

  // Keyboard current settings + its native four presets.
  KeyboardSettings keyboardSettings;
  KeyboardPreset keyboardPresets[kDeviceKeyboardPresetCount] = {};

  // Dual-effector cartridge/program selection (left and right slots).
  EffectorSelection leftEffector;
  EffectorSelection rightEffector;

  // Sequencer physical settings (transient state is deliberately absent).
  SequencerSettings sequencer;
};

}  // namespace lunar24::core
