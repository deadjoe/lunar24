// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// StateSerializer (design/07 §6): DeviceStateV1 <-> the declared, frozen storage
// schema (kDeviceStorageSchema). This is the mapping device_state.h calls "a
// later slice": a serializer maps the framework-free working copy to/from the
// named, versioned, fixed-width wire record by field name.
//
// Scope (P2-⑤ @Claude Q1): this is the MINIMAL round-trip-required mapping, not a
// generic schema framework. It walks kDeviceStorageFields for ordinal + width
// (so it can never silently diverge from the declared order), and writes each
// field by name. It is deliberately not an auto-walking "schema engine".
//
// Wire format (per StorageEncoding::binary): fixed-width little-endian, exactly
// kDeviceStorageSchema.totalBytesHint bytes. Reserved bytes (the sequencer's
// opaque block, and the keyboard-preset 2-byte pad) are carried verbatim — never
// cleared, because a different version writing there must not be destroyed
// (P2-⑤ @Claude Q2: unknown bytes are preserved, not zeroed).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <lunar24/core/device_state.h>
#include <lunar24/core/id_types.h>

namespace lunar24::core {

// Byte width of one top-level storage field, computed from the schema table.
// Mirrors the recompute in tests/core/test_core_contract.cpp so the serializer
// totals agree with the frozen totalBytesHint.
inline std::uint32_t storage_field_bytes(const StorageField& f) {
  switch (f.kind) {
    case StorageFieldKind::scalar: {
      switch (f.type) {
        case StorageFieldType::u8:  return 1u;
        case StorageFieldType::u16: return 2u;
        case StorageFieldType::u32: return 4u;
        case StorageFieldType::u64: return 8u;
        case StorageFieldType::f32: return 4u;
        case StorageFieldType::f64: return 8u;
      }
      return 0u;
    }
    case StorageFieldKind::array: {
      std::uint32_t per = 0u;
      switch (f.type) {
        case StorageFieldType::u8:  per = 1u; break;
        case StorageFieldType::u16: per = 2u; break;
        case StorageFieldType::u32: per = 4u; break;
        case StorageFieldType::u64: per = 8u; break;
        case StorageFieldType::f32: per = 4u; break;
        case StorageFieldType::f64: per = 8u; break;
      }
      return per * f.count;
    }
    case StorageFieldKind::record: return f.itemBytes * f.count;
    case StorageFieldKind::reserved: return f.count;
  }
  return 0u;
}

// Little-endian fixed-width helpers. f32/f64 are emitted by their IEEE-754 bit
// pattern so the wire is host- and endian-independent (no `sizeof`, no layout
// dependence — that is what makes the record portable).
inline void put_u8(std::uint8_t* p, std::uint8_t v) { p[0] = v; }
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}
inline void put_u64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void put_f32(std::uint8_t* p, float v) {
  std::uint32_t u;
  std::memcpy(&u, &v, 4u);
  put_u32(p, u);
}
inline void put_f64(std::uint8_t* p, double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, 8u);
  put_u64(p, u);
}
inline std::uint8_t get_u8(const std::uint8_t* p) { return p[0]; }
inline std::uint32_t get_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t get_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}
inline float get_f32(const std::uint8_t* p) {
  std::uint32_t u = get_u32(p);
  float v;
  std::memcpy(&v, &u, 4u);
  return v;
}
inline double get_f64(const std::uint8_t* p) {
  std::uint64_t u = get_u64(p);
  double v;
  std::memcpy(&v, &u, 8u);
  return v;
}

// Encode `state` into the canonical schema record at `out`. `out` must hold at
// least kDeviceStorageSchema.totalBytesHint bytes; returns false (and writes
// nothing) if `capacity` is too small. On success `*written` (if given) is the
// exact byte count.
inline bool encode_device_state(const DeviceStateV1& state, std::uint8_t* out,
                                std::size_t capacity, std::size_t* written = nullptr) {
  const std::uint32_t total = kDeviceStorageSchema.totalBytesHint;
  if (capacity < total) return false;

  std::uint32_t off = 0u;
  for (std::uint32_t i = 0; i < kDeviceStorageSchema.fieldCount; ++i) {
    const StorageField& f = kDeviceStorageSchema.fields[i];
    std::uint8_t* p = out + off;
    const std::string_view n = f.name;
    if (n == "schema_version") {
      put_u32(p, state.schemaVersion);
    } else if (n == "identity_model_version") {
      put_u32(p, state.identityModelVersion);
    } else if (n == "identity_seed") {
      put_u64(p, state.identitySeed.seed);
    } else if (n == "calibration_vcf_left") {
      put_f32(p, state.calibration.vcfLeftTrim);
    } else if (n == "calibration_vcf_right") {
      put_f32(p, state.calibration.vcfRightTrim);
    } else if (n == "parameters") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_f64(out + off + k * 8u, state.parameters[k]);
    } else if (n == "input_cable") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_u8(out + off + k, state.inputCable[k]);
    } else if (n == "cable_source") {
      for (std::uint32_t k = 0; k < f.count; ++k)
        put_u32(out + off + k * 4u, static_cast<IdValue>(state.cableSource[k]));
    } else if (n == "route_overridden") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_u8(out + off + k, state.routeOverridden[k]);
    } else if (n == "keyboard_presets") {
      const std::uint32_t stride = kKeyboardPresetRecordBytes;
      for (std::uint32_t k = 0; k < f.count; ++k) {
        const KeyboardPreset& rec = state.keyboardPresets[k];
        std::uint8_t* q = out + off + k * stride;
        put_u32(q + 0u, rec.id);
        put_u8(q + 4u, rec.pressureBehaviour);
        put_u8(q + 5u, rec.pressureOutput);
        put_u8(q + 6u, rec.reserved[0]);
        put_u8(q + 7u, rec.reserved[1]);
      }
    } else if (n == "keyboard_settings") {
      put_u8(out + off + 0u, state.keyboardSettings.pressureBehaviour);
      put_u8(out + off + 1u, state.keyboardSettings.pressureOutput);
    } else if (n == "effector_left_program") {
      put_u32(out + off, static_cast<IdValue>(state.leftEffector.program));
    } else if (n == "effector_right_program") {
      put_u32(out + off, static_cast<IdValue>(state.rightEffector.program));
    } else if (n == "sequencer_physical") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_u8(out + off + k, state.sequencer.reserved[k]);
    }
    off += storage_field_bytes(f);
  }

  if (written) *written = total;
  return true;
}

// Decode the canonical schema record at `in` (exactly kDeviceStorageSchema
// totalBytesHint bytes) into `out`. Returns false if `size` is too small.
inline bool decode_device_state(const std::uint8_t* in, std::size_t size,
                                DeviceStateV1* out) {
  if (!out || size < kDeviceStorageSchema.totalBytesHint) return false;
  DeviceStateV1 state;  // value-initialised; only the wire fields are overwritten

  std::uint32_t off = 0u;
  for (std::uint32_t i = 0; i < kDeviceStorageSchema.fieldCount; ++i) {
    const StorageField& f = kDeviceStorageSchema.fields[i];
    const std::uint8_t* p = in + off;
    const std::string_view n = f.name;
    if (n == "schema_version") {
      state.schemaVersion = get_u32(p);
    } else if (n == "identity_model_version") {
      state.identityModelVersion = get_u32(p);
    } else if (n == "identity_seed") {
      state.identitySeed.seed = get_u64(p);
    } else if (n == "calibration_vcf_left") {
      state.calibration.vcfLeftTrim = get_f32(p);
    } else if (n == "calibration_vcf_right") {
      state.calibration.vcfRightTrim = get_f32(p);
    } else if (n == "parameters") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.parameters[k] = get_f64(in + off + k * 8u);
    } else if (n == "input_cable") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.inputCable[k] = get_u8(in + off + k);
    } else if (n == "cable_source") {
      for (std::uint32_t k = 0; k < f.count; ++k)
        state.cableSource[k] = static_cast<JackId>(get_u32(in + off + k * 4u));
    } else if (n == "route_overridden") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.routeOverridden[k] = get_u8(in + off + k);
    } else if (n == "keyboard_presets") {
      const std::uint32_t stride = kKeyboardPresetRecordBytes;
      for (std::uint32_t k = 0; k < f.count; ++k) {
        const std::uint8_t* q = in + off + k * stride;
        KeyboardPreset& rec = state.keyboardPresets[k];
        rec.id = get_u32(q + 0u);
        rec.pressureBehaviour = get_u8(q + 4u);
        rec.pressureOutput = get_u8(q + 5u);
        rec.reserved[0] = get_u8(q + 6u);
        rec.reserved[1] = get_u8(q + 7u);
      }
    } else if (n == "keyboard_settings") {
      state.keyboardSettings.pressureBehaviour = get_u8(in + off + 0u);
      state.keyboardSettings.pressureOutput = get_u8(in + off + 1u);
    } else if (n == "effector_left_program") {
      state.leftEffector.program = static_cast<ProgramId>(get_u32(in + off));
    } else if (n == "effector_right_program") {
      state.rightEffector.program = static_cast<ProgramId>(get_u32(in + off));
    } else if (n == "sequencer_physical") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.sequencer.reserved[k] = get_u8(in + off + k);
    }
    off += storage_field_bytes(f);
  }

  *out = state;
  return true;
}

}  // namespace lunar24::core
