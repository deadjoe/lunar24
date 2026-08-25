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
inline void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
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
inline std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
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

// --- KeyboardPreset record mapping (P4-②) ------------------------------------
//
// A KeyboardPreset's wire record is described by kKeyboardPresetFields (name ->
// offset + width); these map each named field to/from the structured working
// copy. The layout table is the single source of truth for offset + width, so a
// serializer change can never silently diverge from the declared schema. The
// composite fields (seq_steps, plate_tune, pushbutton_value) iterate their inner
// array at the declared offset.

inline void write_keyboard_preset(std::uint8_t* q, const StorageRecordField& rf,
                                  const KeyboardPreset& p) {
  const std::uint32_t o = rf.offset;
  const std::string_view n = rf.name;
  if (n == "id") {
    put_u32(q + o, p.id);
  } else if (n == "pressure_behaviour") {
    put_u8(q + o, p.pressureBehaviour);
  } else if (n == "pressure_output") {
    put_u8(q + o, p.pressureOutput);
  } else if (n == "reserved") {
    put_u8(q + o, p.reserved[0]);
    put_u8(q + o + 1u, p.reserved[1]);
  } else if (n == "mode") {
    put_u8(q + o, p.mode);
  } else if (n == "arp_hold") {
    put_u8(q + o, p.arpHold);
  } else if (n == "arp_clock") {
    put_u8(q + o, p.arpClock);
  } else if (n == "arp_direction") {
    put_u8(q + o, p.arpDirection);
  } else if (n == "arp_variation") {
    put_u8(q + o, p.arpVariation);
  } else if (n == "arp_interval") {
    put_f32(q + o, p.arpInterval);
  } else if (n == "arp_rhythm") {
    put_u8(q + o, p.arpRhythm);
  } else if (n == "arp_length") {
    put_f32(q + o, p.arpLength);
  } else if (n == "seq_run") {
    put_u8(q + o, p.seqRun);
  } else if (n == "seq_length") {
    put_f32(q + o, p.seqLength);
  } else if (n == "seq_clock") {
    put_u8(q + o, p.seqClock);
  } else if (n == "seq_direction") {
    put_u8(q + o, p.seqDirection);
  } else if (n == "seq_cv_output") {
    put_u8(q + o, p.seqCvOutput);
  } else if (n == "seq_rhythm") {
    put_u8(q + o, p.seqRhythm);
  } else if (n == "seq_rhythm_length") {
    put_f32(q + o, p.seqRhythmLength);
  } else if (n == "seq_steps") {
    for (std::uint32_t s = 0; s < kKeyboardSeqStepCount; ++s) {
      std::uint8_t* sp = q + o + s * kKeyboardSeqStepBytes;
      put_u8(sp + 0u, p.seqSteps.steps[s].note);
      put_f32(sp + 1u, p.seqSteps.steps[s].value);
      put_u8(sp + 5u, p.seqSteps.steps[s].gate);
    }
  } else if (n == "portamento_speed") {
    put_f32(q + o, p.portamentoSpeed);
  } else if (n == "portamento_legato") {
    put_u8(q + o, p.portamentoLegato);
  } else if (n == "vibrato_speed") {
    put_f32(q + o, p.vibratoSpeed);
  } else if (n == "vibrato_depth") {
    put_f32(q + o, p.vibratoDepth);
  } else if (n == "vibrato_delay") {
    put_f32(q + o, p.vibratoDelay);
  } else if (n == "vibrato_pressure") {
    put_f32(q + o, p.vibratoPressure);
  } else if (n == "pressure_rise") {
    put_f32(q + o, p.pressureRise);
  } else if (n == "pressure_fall") {
    put_f32(q + o, p.pressureFall);
  } else if (n == "quantise_scale_editor") {
    put_u16(q + o, p.quantiseScaleEditor);
  } else if (n == "quantise_load_scale") {
    put_u8(q + o, p.quantiseLoadScale);
  } else if (n == "root_note") {
    put_f32(q + o, p.rootNote);
  } else if (n == "plate_tune") {
    for (std::uint32_t t = 0; t < kKeyboardPlateTuneCount; ++t) put_f32(q + o + t * 4u, p.plateTune[t]);
  } else if (n == "pushbutton_value") {
    for (std::uint32_t b = 0; b < kKeyboardPushbuttonCount; ++b) put_f32(q + o + b * 4u, p.pushbuttonValue[b]);
  }
}

inline void read_keyboard_preset(const std::uint8_t* q, const StorageRecordField& rf,
                                 KeyboardPreset* p) {
  const std::uint32_t o = rf.offset;
  const std::string_view n = rf.name;
  if (n == "id") {
    p->id = get_u32(q + o);
  } else if (n == "pressure_behaviour") {
    p->pressureBehaviour = get_u8(q + o);
  } else if (n == "pressure_output") {
    p->pressureOutput = get_u8(q + o);
  } else if (n == "reserved") {
    p->reserved[0] = get_u8(q + o);
    p->reserved[1] = get_u8(q + o + 1u);
  } else if (n == "mode") {
    p->mode = get_u8(q + o);
  } else if (n == "arp_hold") {
    p->arpHold = get_u8(q + o);
  } else if (n == "arp_clock") {
    p->arpClock = get_u8(q + o);
  } else if (n == "arp_direction") {
    p->arpDirection = get_u8(q + o);
  } else if (n == "arp_variation") {
    p->arpVariation = get_u8(q + o);
  } else if (n == "arp_interval") {
    p->arpInterval = get_f32(q + o);
  } else if (n == "arp_rhythm") {
    p->arpRhythm = get_u8(q + o);
  } else if (n == "arp_length") {
    p->arpLength = get_f32(q + o);
  } else if (n == "seq_run") {
    p->seqRun = get_u8(q + o);
  } else if (n == "seq_length") {
    p->seqLength = get_f32(q + o);
  } else if (n == "seq_clock") {
    p->seqClock = get_u8(q + o);
  } else if (n == "seq_direction") {
    p->seqDirection = get_u8(q + o);
  } else if (n == "seq_cv_output") {
    p->seqCvOutput = get_u8(q + o);
  } else if (n == "seq_rhythm") {
    p->seqRhythm = get_u8(q + o);
  } else if (n == "seq_rhythm_length") {
    p->seqRhythmLength = get_f32(q + o);
  } else if (n == "seq_steps") {
    for (std::uint32_t s = 0; s < kKeyboardSeqStepCount; ++s) {
      const std::uint8_t* sp = q + o + s * kKeyboardSeqStepBytes;
      p->seqSteps.steps[s].note = get_u8(sp + 0u);
      p->seqSteps.steps[s].value = get_f32(sp + 1u);
      p->seqSteps.steps[s].gate = get_u8(sp + 5u);
    }
  } else if (n == "portamento_speed") {
    p->portamentoSpeed = get_f32(q + o);
  } else if (n == "portamento_legato") {
    p->portamentoLegato = get_u8(q + o);
  } else if (n == "vibrato_speed") {
    p->vibratoSpeed = get_f32(q + o);
  } else if (n == "vibrato_depth") {
    p->vibratoDepth = get_f32(q + o);
  } else if (n == "vibrato_delay") {
    p->vibratoDelay = get_f32(q + o);
  } else if (n == "vibrato_pressure") {
    p->vibratoPressure = get_f32(q + o);
  } else if (n == "pressure_rise") {
    p->pressureRise = get_f32(q + o);
  } else if (n == "pressure_fall") {
    p->pressureFall = get_f32(q + o);
  } else if (n == "quantise_scale_editor") {
    p->quantiseScaleEditor = get_u16(q + o);
  } else if (n == "quantise_load_scale") {
    p->quantiseLoadScale = get_u8(q + o);
  } else if (n == "root_note") {
    p->rootNote = get_f32(q + o);
  } else if (n == "plate_tune") {
    for (std::uint32_t t = 0; t < kKeyboardPlateTuneCount; ++t) p->plateTune[t] = get_f32(q + o + t * 4u);
  } else if (n == "pushbutton_value") {
    for (std::uint32_t b = 0; b < kKeyboardPushbuttonCount; ++b) p->pushbuttonValue[b] = get_f32(q + o + b * 4u);
  }
}

// Write/read one preset record following its declared layout (all 33 sub-fields).
inline void write_keyboard_presets(std::uint8_t* base, const KeyboardPreset& p,
                                   const StorageRecordLayout& rec) {
  for (std::uint32_t j = 0; j < rec.fieldCount; ++j) write_keyboard_preset(base, rec.fields[j], p);
}
inline void read_keyboard_presets(const std::uint8_t* base, KeyboardPreset* p,
                                  const StorageRecordLayout& rec) {
  for (std::uint32_t j = 0; j < rec.fieldCount; ++j) read_keyboard_preset(base, rec.fields[j], p);
}

// Write/read the live KeyboardSeq (16 steps of 6 bytes, packed).
inline void write_keyboard_seq(std::uint8_t* base, const KeyboardSeq& s) {
  for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
    std::uint8_t* sp = base + i * kKeyboardSeqStepBytes;
    put_u8(sp + 0u, s.steps[i].note);
    put_f32(sp + 1u, s.steps[i].value);
    put_u8(sp + 5u, s.steps[i].gate);
  }
}
inline void read_keyboard_seq(const std::uint8_t* base, KeyboardSeq* s) {
  for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
    const std::uint8_t* sp = base + i * kKeyboardSeqStepBytes;
    s->steps[i].note = get_u8(sp + 0u);
    s->steps[i].value = get_f32(sp + 1u);
    s->steps[i].gate = get_u8(sp + 5u);
  }
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
      for (std::uint32_t k = 0; k < f.count; ++k)
        write_keyboard_presets(out + off + k * stride, state.keyboardPresets[k], kKeyboardPresetLayout);
    } else if (n == "keyboard_settings") {
      put_u8(out + off + 0u, state.keyboardSettings.pressureBehaviour);
      put_u8(out + off + 1u, state.keyboardSettings.pressureOutput);
    } else if (n == "effector_left_program") {
      put_u32(out + off, static_cast<IdValue>(state.leftEffector.program));
    } else if (n == "effector_right_program") {
      put_u32(out + off, static_cast<IdValue>(state.rightEffector.program));
    } else if (n == "sequencer_physical") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_u8(out + off + k, state.sequencer.reserved[k]);
    } else if (n == "keyboard_seq_current") {
      write_keyboard_seq(out + off, state.keyboardSeqCurrent);
    } else if (n == "keyboard_scale_editor") {
      put_u16(out + off, state.keyboardScaleEditor);
    } else if (n == "keyboard_plate_tune") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_f32(out + off + k * 4u, state.keyboardPlateTune[k]);
    } else if (n == "keyboard_pushbutton") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_f32(out + off + k * 4u, state.keyboardPushbutton[k]);
    } else if (n == "keyboard_clock_selectors") {
      for (std::uint32_t k = 0; k < f.count; ++k) put_u8(out + off + k, state.keyboardClockSelectors[k]);
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
      for (std::uint32_t k = 0; k < f.count; ++k)
        read_keyboard_presets(in + off + k * stride, &state.keyboardPresets[k], kKeyboardPresetLayout);
    } else if (n == "keyboard_settings") {
      state.keyboardSettings.pressureBehaviour = get_u8(in + off + 0u);
      state.keyboardSettings.pressureOutput = get_u8(in + off + 1u);
    } else if (n == "effector_left_program") {
      state.leftEffector.program = static_cast<ProgramId>(get_u32(in + off));
    } else if (n == "effector_right_program") {
      state.rightEffector.program = static_cast<ProgramId>(get_u32(in + off));
    } else if (n == "sequencer_physical") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.sequencer.reserved[k] = get_u8(in + off + k);
    } else if (n == "keyboard_seq_current") {
      read_keyboard_seq(in + off, &state.keyboardSeqCurrent);
    } else if (n == "keyboard_scale_editor") {
      state.keyboardScaleEditor = get_u16(in + off);
    } else if (n == "keyboard_plate_tune") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.keyboardPlateTune[k] = get_f32(in + off + k * 4u);
    } else if (n == "keyboard_pushbutton") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.keyboardPushbutton[k] = get_f32(in + off + k * 4u);
    } else if (n == "keyboard_clock_selectors") {
      for (std::uint32_t k = 0; k < f.count; ++k) state.keyboardClockSelectors[k] = get_u8(in + off + k);
    }
    off += storage_field_bytes(f);
  }

  *out = state;
  return true;
}

}  // namespace lunar24::core
