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
// P4-② (Decision A, msg 6a366ebb): kKeyboardPresetRecordBytes grew 8 -> 247 when
// the preset carried the full keyboard_params_minus_clock payload (31 params) —
// the 8-byte shell (id/behaviour/output + 2-byte pad) is PRESERVED verbatim and
// 29 fields are appended, never reordered.
//
// P4-③ (per-side, msg 60df2e43 + e7ad49ec): split runs two independent half-banks,
// and the frozen manifest gives the SAME keyboard_params_minus_clock params to both
// sides (behaviour is the only global). So each preset now carries TWO banks: the
// left half stays exactly where the v2 layout put it (offsets 5, 8..246) — never
// reordered — and a right half is appended as a contiguous 240-byte region at
// offsets 247..486. wire: 247 + 240 = 487 bytes.
inline constexpr std::uint32_t kKeyboardSideBankBytes = 240u;      // 30 per-side params (frozen 31 minus behaviour)
inline constexpr std::uint32_t kKeyboardPresetRecordBytes = 487u;  // 247 (v2, left) + 240 (right)
inline constexpr std::uint32_t kKeyboardSettingsRecordBytes = 2u;  // pressure behaviour(1) + pressure output(1)
inline constexpr std::uint32_t kSequencerPhysicalBytes = 16u;    // reserved until the sequencer lands
inline constexpr std::uint32_t kKeyboardSeqRecordBytes = kKeyboardSeqBytes;  // 16 steps x 6 bytes
// P4-③ (per-side scalar bank, @Claude msg c0d9e9be — Decis B): the LIVE right-bank
// scalar count. The 22 keyboard scalars that carry a ParameterId and are per-side
// (the frozen 30 keyboard_params_minus_clock minus behaviour, minus the four
// no-domain clock/rhythm selectors, minus the four non-scalars — their right side
// is the `_r` mirror block, already at schema v4). The LEFT/shared side reads
// parameters[ParameterId]; the RIGHT side reads keyboardScalarRight[index(id)].
inline constexpr std::uint32_t kKeyboardScalarRightCount = 22u;
inline constexpr std::uint32_t kDeviceStorageSchemaVersion = 5u;
inline constexpr std::uint32_t kDeviceStorageInitialRevision = 0u;

// One KeyboardSeqStep's machine-readable interior (component of a seq record).
// Packed note(u8) + value(f32) + gate(u8) = 6 bytes, fixed-width; the interior is
// described so a future serializer can name-encode a step. versionFrom=2 because
// the seq record first appears in schema version 2.
inline constexpr StorageRecordField kKeyboardSeqStepFields[] = {
    {"note",  StorageFieldType::u8,  StorageEncoding::binary, 0u, 1u, 2u},
    {"value", StorageFieldType::f32, StorageEncoding::binary, 1u, 4u, 2u},
    {"gate",  StorageFieldType::u8,  StorageEncoding::binary, 5u, 1u, 2u},
};
inline constexpr StorageRecordLayout kKeyboardSeqStepLayout{kKeyboardSeqStepFields, 3u};

// A KeyboardPreset's machine-readable interior. The 8-byte shell (id / behaviour
// / output / reserved-pad) is unchanged; the 29 remaining fields of the frozen
// keyboard_params_minus_clock payload are appended, never reordered. Composite
// fields (seq_steps, plate_tune, pushbutton_value) are declared as their fixed
// wire byte-region here; the serializer maps them to/from the structured
// working-copy members by name.
inline constexpr StorageRecordField kKeyboardPresetFields[] = {
    {"id",                    StorageFieldType::u32, StorageEncoding::binary, 0u,   4u,  1u},
    {"pressure_behaviour",    StorageFieldType::u8,  StorageEncoding::binary, 4u,   1u,  1u},
    {"pressure_output",       StorageFieldType::u8,  StorageEncoding::binary, 5u,   1u,  1u},
    {"reserved",              StorageFieldType::u8,  StorageEncoding::binary, 6u,   2u,  1u},
    {"mode",                  StorageFieldType::u8,  StorageEncoding::binary, 8u,   1u,  2u},
    {"arp_hold",              StorageFieldType::u8,  StorageEncoding::binary, 9u,   1u,  2u},
    {"arp_clock",             StorageFieldType::u8,  StorageEncoding::binary, 10u,  1u,  2u},
    {"arp_direction",         StorageFieldType::u8,  StorageEncoding::binary, 11u,  1u,  2u},
    {"arp_variation",         StorageFieldType::u8,  StorageEncoding::binary, 12u,  1u,  2u},
    {"arp_interval",          StorageFieldType::f32, StorageEncoding::binary, 13u,  4u,  2u},
    {"arp_rhythm",            StorageFieldType::u8,  StorageEncoding::binary, 17u,  1u,  2u},
    {"arp_length",            StorageFieldType::f32, StorageEncoding::binary, 18u,  4u,  2u},
    {"seq_run",               StorageFieldType::u8,  StorageEncoding::binary, 22u,  1u,  2u},
    {"seq_length",            StorageFieldType::f32, StorageEncoding::binary, 23u,  4u,  2u},
    {"seq_clock",             StorageFieldType::u8,  StorageEncoding::binary, 27u,  1u,  2u},
    {"seq_direction",         StorageFieldType::u8,  StorageEncoding::binary, 28u,  1u,  2u},
    {"seq_cv_output",         StorageFieldType::u8,  StorageEncoding::binary, 29u,  1u,  2u},
    {"seq_rhythm",            StorageFieldType::u8,  StorageEncoding::binary, 30u,  1u,  2u},
    {"seq_rhythm_length",     StorageFieldType::f32, StorageEncoding::binary, 31u,  4u,  2u},
    {"seq_steps",             StorageFieldType::u8,  StorageEncoding::binary, 35u,  96u, 2u},
    {"portamento_speed",      StorageFieldType::f32, StorageEncoding::binary, 131u, 4u,  2u},
    {"portamento_legato",     StorageFieldType::u8,  StorageEncoding::binary, 135u, 1u,  2u},
    {"vibrato_speed",         StorageFieldType::f32, StorageEncoding::binary, 136u, 4u,  2u},
    {"vibrato_depth",         StorageFieldType::f32, StorageEncoding::binary, 140u, 4u,  2u},
    {"vibrato_delay",         StorageFieldType::f32, StorageEncoding::binary, 144u, 4u,  2u},
    {"vibrato_pressure",      StorageFieldType::f32, StorageEncoding::binary, 148u, 4u,  2u},
    {"pressure_rise",         StorageFieldType::f32, StorageEncoding::binary, 152u, 4u,  2u},
    {"pressure_fall",         StorageFieldType::f32, StorageEncoding::binary, 156u, 4u,  2u},
    {"quantise_scale_editor", StorageFieldType::u16, StorageEncoding::binary, 160u, 2u,  2u},
    {"quantise_load_scale",   StorageFieldType::u8,  StorageEncoding::binary, 162u, 1u,  2u},
    {"root_note",             StorageFieldType::f32, StorageEncoding::binary, 163u, 4u,  2u},
    {"plate_tune",            StorageFieldType::u8,  StorageEncoding::binary, 167u, 48u, 2u},
    {"pushbutton_value",      StorageFieldType::u8,  StorageEncoding::binary, 215u, 32u, 2u},
    // P4-③ per-side RIGHT half-bank (contiguous 240B region, offsets 247..486,
    // versionFrom=3). The frozen manifest gives both sides the same 30 params
    // (keyboard_params_minus_clock minus the global 'behaviour'), so the right
    // bank is the same field set with an "_r" suffix and a base offset of 247.
    {"pressure_output_r",       StorageFieldType::u8,  StorageEncoding::binary, 247u, 1u,  3u},
    {"mode_r",                  StorageFieldType::u8,  StorageEncoding::binary, 248u, 1u,  3u},
    {"arp_hold_r",              StorageFieldType::u8,  StorageEncoding::binary, 249u, 1u,  3u},
    {"arp_clock_r",             StorageFieldType::u8,  StorageEncoding::binary, 250u, 1u,  3u},
    {"arp_direction_r",         StorageFieldType::u8,  StorageEncoding::binary, 251u, 1u,  3u},
    {"arp_variation_r",         StorageFieldType::u8,  StorageEncoding::binary, 252u, 1u,  3u},
    {"arp_interval_r",          StorageFieldType::f32, StorageEncoding::binary, 253u, 4u,  3u},
    {"arp_rhythm_r",            StorageFieldType::u8,  StorageEncoding::binary, 257u, 1u,  3u},
    {"arp_length_r",            StorageFieldType::f32, StorageEncoding::binary, 258u, 4u,  3u},
    {"seq_run_r",               StorageFieldType::u8,  StorageEncoding::binary, 262u, 1u,  3u},
    {"seq_length_r",            StorageFieldType::f32, StorageEncoding::binary, 263u, 4u,  3u},
    {"seq_clock_r",             StorageFieldType::u8,  StorageEncoding::binary, 267u, 1u,  3u},
    {"seq_direction_r",         StorageFieldType::u8,  StorageEncoding::binary, 268u, 1u,  3u},
    {"seq_cv_output_r",         StorageFieldType::u8,  StorageEncoding::binary, 269u, 1u,  3u},
    {"seq_rhythm_r",            StorageFieldType::u8,  StorageEncoding::binary, 270u, 1u,  3u},
    {"seq_rhythm_length_r",     StorageFieldType::f32, StorageEncoding::binary, 271u, 4u,  3u},
    {"seq_steps_r",             StorageFieldType::u8,  StorageEncoding::binary, 275u, 96u, 3u},
    {"portamento_speed_r",      StorageFieldType::f32, StorageEncoding::binary, 371u, 4u,  3u},
    {"portamento_legato_r",     StorageFieldType::u8,  StorageEncoding::binary, 375u, 1u,  3u},
    {"vibrato_speed_r",         StorageFieldType::f32, StorageEncoding::binary, 376u, 4u,  3u},
    {"vibrato_depth_r",         StorageFieldType::f32, StorageEncoding::binary, 380u, 4u,  3u},
    {"vibrato_delay_r",         StorageFieldType::f32, StorageEncoding::binary, 384u, 4u,  3u},
    {"vibrato_pressure_r",      StorageFieldType::f32, StorageEncoding::binary, 388u, 4u,  3u},
    {"pressure_rise_r",         StorageFieldType::f32, StorageEncoding::binary, 392u, 4u,  3u},
    {"pressure_fall_r",         StorageFieldType::f32, StorageEncoding::binary, 396u, 4u,  3u},
    {"quantise_scale_editor_r", StorageFieldType::u16, StorageEncoding::binary, 400u, 2u,  3u},
    {"quantise_load_scale_r",   StorageFieldType::u8,  StorageEncoding::binary, 402u, 1u,  3u},
    {"root_note_r",             StorageFieldType::f32, StorageEncoding::binary, 403u, 4u,  3u},
    {"plate_tune_r",            StorageFieldType::u8,  StorageEncoding::binary, 407u, 48u, 3u},
    {"pushbutton_value_r",      StorageFieldType::u8,  StorageEncoding::binary, 455u, 32u, 3u},
};
inline constexpr StorageRecordField kKeyboardSettingsFields[] = {
    {"pressure_behaviour", StorageFieldType::u8, StorageEncoding::binary, 0u, 1u, 1u},
    {"pressure_output",    StorageFieldType::u8, StorageEncoding::binary, 1u, 1u, 1u},
};

// The map-able record layouts referenced by the keyboard records below.
// P4-③: the preset layout grew 33 -> 63 sub-fields (the 30 right-bank "_r" fields).
inline constexpr StorageRecordLayout kKeyboardPresetLayout{kKeyboardPresetFields, 63u};
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
    // P4-② live keyboard non-scalar + no-domain-selector state (appended, never
    // reordered). The current run-time values of the Deferred-to-P4 non-scalars
    // (design/07 §6) and the four no-domain clock/rhythm selectors (which have no
    // ParameterId — the frozen id-space gapped them) live here, separate from the
    // four saved preset slots.
    {"keyboard_seq_current",     StorageFieldKind::record,  StorageFieldType::u8,  StorageEncoding::binary, kKeyboardSeqStepCount, kKeyboardSeqStepBytes, 2u, kKeyboardSeqStepLayout},
    {"keyboard_scale_editor",    StorageFieldKind::scalar,  StorageFieldType::u16, StorageEncoding::binary, 1u,  0u, 2u, {}},
    {"keyboard_plate_tune",      StorageFieldKind::array,   StorageFieldType::f32, StorageEncoding::binary, kKeyboardPlateTuneCount, 0u, 2u, {}},
    {"keyboard_pushbutton",      StorageFieldKind::array,   StorageFieldType::f32, StorageEncoding::binary, kKeyboardPushbuttonCount, 0u, 2u, {}},
    {"keyboard_clock_selectors", StorageFieldKind::array,   StorageFieldType::u8,  StorageEncoding::binary, 4u,  0u, 2u, {}},
    // P4-③ right-bank mirrors of the five live fields above (L1, versionFrom=4).
    // Appended, never reordered. Same shape/size as the left field; consumed only
    // under split, when the right-side performance surface reads its own bank.
    {"keyboard_seq_current_r",    StorageFieldKind::record, StorageFieldType::u8,  StorageEncoding::binary, kKeyboardSeqStepCount, kKeyboardSeqStepBytes, 4u, kKeyboardSeqStepLayout},
    {"keyboard_scale_editor_r",   StorageFieldKind::scalar, StorageFieldType::u16, StorageEncoding::binary, 1u,  0u, 4u, {}},
    {"keyboard_plate_tune_r",     StorageFieldKind::array,  StorageFieldType::f32, StorageEncoding::binary, kKeyboardPlateTuneCount, 0u, 4u, {}},
    {"keyboard_pushbutton_r",     StorageFieldKind::array,  StorageFieldType::f32, StorageEncoding::binary, kKeyboardPushbuttonCount, 0u, 4u, {}},
    {"keyboard_clock_selectors_r",StorageFieldKind::array,  StorageFieldType::u8,  StorageEncoding::binary, 4u,  0u, 4u, {}},
    // P4-③ live per-side SCALAR bank (Decis B, versionFrom=5). Appended, never
    // reordered. The 22 keyboard scalars that carry a ParameterId, flattened into
    // one f64 array indexed by keyKeyboardScalarIndexOf(id). bank[0] (left/shared)
    // stays in `parameters[ParameterId]` — this is ONLY the right bank. Choosing
    // f64 (not f32) keeps it type-consistent with `parameters[]` so the side-bank
    // reader can return one Value type for both banks.
    {"keyboard_scalar_right",       StorageFieldKind::array,  StorageFieldType::f64, StorageEncoding::binary, kKeyboardScalarRightCount, 0u, 5u, {}},
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
// It rose 3831 -> 3841 for the vco_b registry correction (task #24, append-only
// per @Claude "只追加，绝不重排" — manual L411 mirror rule — and the later fm_in
// revert, task #32) when kDevicePatchCapacity reached 67: `input_cable` u8 grows
// +2 and `cable_source` u32 grows +8 (2 x 4 = 8) = +10 bytes. The +10 is keyed to
// the id-space (67 = max id 66 + 1, where 66 is vco_b.vca_ctl), not to the count
// of vco_b appends: vco_b.fm_in (id 65) was removed as over-recorded, but the
// capacity stays 67 because the id-space is unchanged. It rose 3841 -> 4979
// for P4-② (Decision A, msg 6a366ebb): the keyboard preset grew 8 -> 247 bytes
// (4 presets: 4 x 239 = 956) and the five live keyboard non-scalar fields were
// appended (96 + 2 + 48 + 32 + 4 = 182). It rose 4979 -> 5939 for P4-③ (per-side,
// msg 60df2e43 + e7ad49ec): each preset grew 247 -> 487 bytes (4 x 240 = 960),
// the right half-bank being appended as a contiguous 240-byte region. It rose
// 5939 -> 6121 for P4-③ live-state L1 (design/00 §2d, msg 695564a7): the five
// live non-scalar / no-domain-selector fields gained a right-bank `_r` mirror
// (the same 96 + 2 + 48 + 32 + 4 = 182 bytes again, appended, versionFrom=4).
// It rose 6121 -> 6297 for P4-③ per-side scalar bank (Decis B, msg c0d9e9be): the
// 22 keyboard scalars' right bank is appended as one f64 array (22 x 8 = 176 bytes).
inline constexpr DeviceStorageSchema kDeviceStorageSchema{
    kDeviceStorageSchemaVersion,
    kDeviceStorageInitialRevision,
    kDeviceStorageFieldCount,
    kDeviceStorageFields,
    6297u,
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

// One step of the keyboard's own 16-step sequencer run (P4 keyboard performance
// system; design/06 §4). The physical 5-step sequencer is a SEPARATE P3 control
// source (ModuleId 11, STAGES 3/4/5) and does NOT extend into this 16-step run.
// Physical 5-step's full persistent state lives in parameters[] (pulser rate,
// clock selector, stage count, 5×step CV, 5×gate-enable — 13 persistent
// controls); NOT persisted are only its transient playhead and the currently
// generated gate / clock phase / output. Gate-ENABLE params are persistent; a
// transient gate LEVEL is not. note is a semitone, value a CV in volts,
// gate a boolean. Fixed-width 6-byte wire record (see kKeyboardSeqStepFields).
struct KeyboardSeqStep {
  std::uint8_t note = 0;
  float value = 0.0f;
  std::uint8_t gate = 0;
};

// The full 16-step sequencer run, as structured keyboard-state. One KeyboardSeq
// is kKeyboardSeqBytes (96) on the wire.
struct KeyboardSeq {
  KeyboardSeqStep steps[kKeyboardSeqStepCount] = {};
};

// A KeyboardPreset is owned by the keyboard subsystem only; it is never
// serialized as a whole-device preset. Each of the kDeviceKeyboardPresetCount
// native presets carries its own keyboard-owned state — the frozen
// keyboard_params_minus_clock payload (31 params), which the manual names
// 'Behaviour' and 'Pressure output' among. The wire record width is the
// schema-declared kKeyboardPresetRecordBytes (487), never sizeof here; the C++
// struct is a framework-free working copy the serializer maps field-by-field.
//
// P4-② (Decision A): the 8-byte shell (id@0 / pressure_behaviour@4 /
// pressure_output@5 / reserved@6) is PRESERVED and 29 fields are appended, never
// reordered. `reserved` stays a 2-byte region at offset 6; a re-encode must
// preserve it verbatim rather than zero it (P2-⑤ @Claude Q2). The `seqSteps`,
// `plateTune`, `pushbuttonValue` members mirror the composite wire regions
// (offsets 35/167/215) as structured working copies.
//
// P4-③ (per-side): the LEFT half-bank stays exactly where v2 put it (the members
// from `pressureOutput` down to `pushbuttonValue` — offsets 5/8..246, never
// reordered), and the RIGHT half-bank is the same 30 fields with an "R" suffix
// (offsets 247..486). C++ member naming uses CamelCase + a trailing "R"
// (e.g. `seqStepsR`) matching the wire "_r" suffix. The single global
// `pressureBehaviour` is NOT copied per-side (it is the single/twin/split
// selector); everything else about the two halves is identical in kind.
struct KeyboardPreset {
  std::uint32_t id = 0u;            // stable sys-selected preset id, never renumbered
  std::uint8_t pressureBehaviour = 0u;  // manual 'Behaviour' — GLOBAL single/twin/split selector
  std::uint8_t pressureOutput = 0u;     // manual 'Pressure output' (PROVISIONAL decode)
  std::uint8_t reserved[2] = {};        // bytes 6-7 of the shell, preserved verbatim

  // LEFT half-bank (appended keyboard_params_minus_clock payload, offsets 8..246).
  std::uint8_t mode = 0u;
  std::uint8_t arpHold = 0u;
  std::uint8_t arpClock = 0u;        // no-domain selector (no ParameterId)
  std::uint8_t arpDirection = 0u;
  std::uint8_t arpVariation = 0u;
  float arpInterval = 0.0f;
  std::uint8_t arpRhythm = 0u;       // no-domain selector
  float arpLength = 0.0f;
  std::uint8_t seqRun = 0u;
  float seqLength = 0.0f;
  std::uint8_t seqClock = 0u;        // no-domain selector
  std::uint8_t seqDirection = 0u;
  std::uint8_t seqCvOutput = 0u;
  std::uint8_t seqRhythm = 0u;       // no-domain selector
  float seqRhythmLength = 0.0f;
  KeyboardSeq seqSteps;              // wire: 96-byte region at offset 35
  float portamentoSpeed = 0.0f;
  std::uint8_t portamentoLegato = 0u;
  float vibratoSpeed = 0.0f;
  float vibratoDepth = 0.0f;
  float vibratoDelay = 0.0f;
  float vibratoPressure = 0.0f;
  float pressureRise = 0.0f;
  float pressureFall = 0.0f;
  std::uint16_t quantiseScaleEditor = 0u;  // 12-bit scale-editor mask (u16)
  std::uint8_t quantiseLoadScale = 0u;
  float rootNote = 0.0f;
  float plateTune[kKeyboardPlateTuneCount] = {};      // wire: 48-byte region at 167
  float pushbuttonValue[kKeyboardPushbuttonCount] = {};  // wire: 32-byte region at 215

  // RIGHT half-bank (P4-③, contiguous region at offsets 247..486) — same 30 fields.
  std::uint8_t pressureOutputR = 0u;
  std::uint8_t modeR = 0u;
  std::uint8_t arpHoldR = 0u;
  std::uint8_t arpClockR = 0u;
  std::uint8_t arpDirectionR = 0u;
  std::uint8_t arpVariationR = 0u;
  float arpIntervalR = 0.0f;
  std::uint8_t arpRhythmR = 0u;
  float arpLengthR = 0.0f;
  std::uint8_t seqRunR = 0u;
  float seqLengthR = 0.0f;
  std::uint8_t seqClockR = 0u;
  std::uint8_t seqDirectionR = 0u;
  std::uint8_t seqCvOutputR = 0u;
  std::uint8_t seqRhythmR = 0u;
  float seqRhythmLengthR = 0.0f;
  KeyboardSeq seqStepsR;             // wire: 96-byte region at offset 275
  float portamentoSpeedR = 0.0f;
  std::uint8_t portamentoLegatoR = 0u;
  float vibratoSpeedR = 0.0f;
  float vibratoDepthR = 0.0f;
  float vibratoDelayR = 0.0f;
  float vibratoPressureR = 0.0f;
  float pressureRiseR = 0.0f;
  float pressureFallR = 0.0f;
  std::uint16_t quantiseScaleEditorR = 0u;  // 12-bit scale-editor mask (u16)
  std::uint8_t quantiseLoadScaleR = 0u;
  float rootNoteR = 0.0f;
  float plateTuneR[kKeyboardPlateTuneCount] = {};
  float pushbuttonValueR[kKeyboardPushbuttonCount] = {};
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
  std::uint32_t schemaVersion = kDeviceStorageSchemaVersion;
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

  // Keyboard's own LIVE non-scalar + no-domain-selector state (design/07 §6:
  // non-scalars belong in DeviceState structured fields, never flattened into a
  // scalar ParameterDescriptor). These are the run-time values, separate from the
  // four saved preset slots above. The four no-domain clock/rhythm selectors have
  // no ParameterId (the frozen id-space gapped them — no evidenced value domain),
  // so their current value lives here, not in parameters[].
  KeyboardSeq keyboardSeqCurrent;
  std::uint16_t keyboardScaleEditor = 0;   // 12-bit scale-editor mask
  float keyboardPlateTune[kKeyboardPlateTuneCount] = {};
  float keyboardPushbutton[kKeyboardPushbuttonCount] = {};
  std::uint8_t keyboardClockSelectors[4] = {};  // {arp_clock, arp_rhythm, seq_clock, seq_rhythm}

  // P4-③ RIGHT half-bank mirror of the five live non-scalar / no-domain-selector
  // fields above (design/00 §2d L1: the v2 field is the left/base, the right bank
  // appends `_r` — the whole 182-byte copy is NOT a separate struct, which would be
  // a second source of truth). Under single/twin these are never read (both sides
  // use the left bank); under split the right-side performance surface reads these.
  // The five no-domain selectors and the non-scalars intentionally NEVER carry a
  // ParameterId, so they cannot be a per-side *scalar* bank — they are mirrored
  // whole, exactly like the preset right half-bank.
  KeyboardSeq keyboardSeqCurrentR;
  std::uint16_t keyboardScaleEditorR = 0;   // 12-bit scale-editor mask
  float keyboardPlateTuneR[kKeyboardPlateTuneCount] = {};
  float keyboardPushbuttonR[kKeyboardPushbuttonCount] = {};
  std::uint8_t keyboardClockSelectorsR[4] = {};  // {arp_clock, arp_rhythm, seq_clock, seq_rhythm}

  // P4-③ LIVE per-side SCALAR bank (Decis B, @Claude msg c0d9e9be). The 22 keyboard
  // scalars that carry a ParameterId have their RIGHT-side value here; the LEFT /
  // shared side value lives in `parameters[ParameterId]` (bank 0) untouched, so
  // there is never a second copy that could drift. Indexed by ParameterId via
  // keyboard_side_bank.h's kKeyboardScalarParameterIds / keyboard_scalar_index;
  // f64 matches `parameters[]` so a side-bank reader returns one Value type. Never
  // a scalar ParameterId grows for the right — split is two banks under one id.
  double keyboardScalarRight[kKeyboardScalarRightCount] = {};

  // Dual-effector cartridge/program selection (left and right slots).
  EffectorSelection leftEffector;
  EffectorSelection rightEffector;

  // Sequencer physical settings (transient state is deliberately absent).
  SequencerSettings sequencer;
};

}  // namespace lunar24::core
