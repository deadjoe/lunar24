// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-③ live per-side SCALAR bank (design/00 §2d Decis B, @Claude msg c0d9e9be).
//
// The live keyboard scalars that carry a ParameterId are per-side: the LEFT /
// shared side reads `parameters[ParameterId]` (bank 0, untouched), the RIGHT side
// reads `keyboardScalarRight[index]` (bank 1). This header owns
//
//   * which ParameterIds are per-side scalars (kKeyboardScalarParameterIds), and
//   * the ParameterId -> keyboardScalarRight index map (keyboard_scalar_index),
//   * the preset <-> live transfer of those two banks (load/save_live_side_bank),
//   * the per-id twin accessor into a KeyboardPreset's left/right scalar fields.
//
// The invariant this must uphold (@Claude msg c0d9e9be): a parameter is per-side
// in the preset IFF it is per-side in live. The preset carries the same 30
// per-side keyboard params; the 22 scalars here are exactly that set minus the
// four no-domain clock/rhythm selectors (stored as keyboardClockSelectors[+_R],
// schema v4) and minus the four non-scalars (their `_r` mirror block, also v4).
// A preset per-side param that is NOT in this list gets dropped on the round-trip
// (the negative control that the test in test_keyboard_presets.cpp runs).
//
// bank[0] is never duplicated: the left value lives in `parameters[ParameterId]`
// and is the single source of truth for the shared / left side.

#pragma once

#include <cstdint>

#include <lunar24/core/device_state.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// The 22 keyboard scalars that carry a ParameterId and are per-side. The index in
// this array is the keyboardScalarRight index (bank 1). The frozen 30 keyboard
// params per side = this 22 (scalars) + 4 no-domain selectors (arp_clock /
// arp_rhythm / seq_clock / seq_rhythm) + 4 non-scalars (seq_steps /
// quantise_scale_editor / plate_tune / pushbutton_value). `keyboard_behaviour`
// (the global single/twin/split selector) is deliberately NOT here — it is the one
// non-per-side param.
inline constexpr ParameterId kKeyboardScalarParameterIds[kKeyboardScalarRightCount] = {
    ParameterId::keyboard_mode,                // 0  mode
    ParameterId::keyboard_arp_hold,            // 1  arp_hold
    ParameterId::keyboard_arp_direction,       // 2  arp_direction
    ParameterId::keyboard_arp_variation,       // 3  arp_variation
    ParameterId::keyboard_arp_interval,        // 4  arp_interval
    ParameterId::keyboard_arp_length,          // 5  arp_length
    ParameterId::keyboard_seq_run,             // 6  seq_run
    ParameterId::keyboard_seq_length,          // 7  seq_length
    ParameterId::keyboard_seq_direction,       // 8  seq_direction
    ParameterId::keyboard_seq_cv_output,       // 9  seq_cv_output
    ParameterId::keyboard_seq_rhythm_length,   // 10 seq_rhythm_length
    ParameterId::keyboard_portamento_speed,    // 11 portamento_speed
    ParameterId::keyboard_portamento_legato,   // 12 portamento_legato
    ParameterId::keyboard_vibrato_speed,       // 13 vibrato_speed
    ParameterId::keyboard_vibrato_depth,       // 14 vibrato_depth
    ParameterId::keyboard_vibrato_delay,       // 15 vibrato_delay
    ParameterId::keyboard_vibrato_pressure,    // 16 vibrato_pressure
    ParameterId::keyboard_pressure_output,     // 17 pressure_output
    ParameterId::keyboard_pressure_rise,       // 18 pressure_rise
    ParameterId::keyboard_pressure_fall,       // 19 pressure_fall
    ParameterId::keyboard_quantise_load_scale, // 20 quantise_load_scale
    ParameterId::keyboard_root_note,           // 21 root_note
};

// ParameterId -> keyboardScalarRight index (its position in the list above), or -1
// for a ParameterId that is NOT a per-side keyboard scalar. The id space itself
// never grows for the right side — this is a value-bank index, not a new id.
inline constexpr std::int32_t keyboard_scalar_index(ParameterId id) noexcept {
  for (std::uint32_t i = 0; i < kKeyboardScalarRightCount; ++i) {
    if (kKeyboardScalarParameterIds[i] == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}

// Read a keyboard scalar's LEFT and RIGHT live values as a (double, double) pair
// from a KeyboardPreset. The preset stores them in their native typed fields
// (u8 / f32); this widens to double so both live banks can be one uniform type.
// Unknown ids fall to the (0.0, 0.0) default so a misinterpreted id degrades
// safely rather than reading a wrong member.
inline void read_preset_scalar_pair(const KeyboardPreset& p, ParameterId id,
                                    double* left, double* right) noexcept {
  switch (id) {
    case ParameterId::keyboard_mode:                *left = p.mode;              *right = p.modeR;              return;
    case ParameterId::keyboard_arp_hold:            *left = p.arpHold;           *right = p.arpHoldR;           return;
    case ParameterId::keyboard_arp_direction:       *left = p.arpDirection;      *right = p.arpDirectionR;      return;
    case ParameterId::keyboard_arp_variation:       *left = p.arpVariation;      *right = p.arpVariationR;      return;
    case ParameterId::keyboard_arp_interval:        *left = p.arpInterval;       *right = p.arpIntervalR;       return;
    case ParameterId::keyboard_arp_length:          *left = p.arpLength;         *right = p.arpLengthR;         return;
    case ParameterId::keyboard_seq_run:             *left = p.seqRun;            *right = p.seqRunR;            return;
    case ParameterId::keyboard_seq_length:          *left = p.seqLength;         *right = p.seqLengthR;         return;
    case ParameterId::keyboard_seq_direction:       *left = p.seqDirection;      *right = p.seqDirectionR;      return;
    case ParameterId::keyboard_seq_cv_output:       *left = p.seqCvOutput;       *right = p.seqCvOutputR;       return;
    case ParameterId::keyboard_seq_rhythm_length:   *left = p.seqRhythmLength;   *right = p.seqRhythmLengthR;   return;
    case ParameterId::keyboard_portamento_speed:    *left = p.portamentoSpeed;   *right = p.portamentoSpeedR;   return;
    case ParameterId::keyboard_portamento_legato:   *left = p.portamentoLegato;  *right = p.portamentoLegatoR;  return;
    case ParameterId::keyboard_vibrato_speed:       *left = p.vibratoSpeed;      *right = p.vibratoSpeedR;      return;
    case ParameterId::keyboard_vibrato_depth:       *left = p.vibratoDepth;      *right = p.vibratoDepthR;      return;
    case ParameterId::keyboard_vibrato_delay:       *left = p.vibratoDelay;      *right = p.vibratoDelayR;      return;
    case ParameterId::keyboard_vibrato_pressure:    *left = p.vibratoPressure;   *right = p.vibratoPressureR;   return;
    case ParameterId::keyboard_pressure_output:     *left = p.pressureOutput;    *right = p.pressureOutputR;    return;
    case ParameterId::keyboard_pressure_rise:       *left = p.pressureRise;      *right = p.pressureRiseR;      return;
    case ParameterId::keyboard_pressure_fall:       *left = p.pressureFall;      *right = p.pressureFallR;      return;
    case ParameterId::keyboard_quantise_load_scale: *left = p.quantiseLoadScale; *right = p.quantiseLoadScaleR; return;
    case ParameterId::keyboard_root_note:           *left = p.rootNote;          *right = p.rootNoteR;          return;
    default: *left = 0.0; *right = 0.0; return;
  }
}

// The inverse of read_preset_scalar_pair: narrow a (double, double) live pair back
// into the preset's typed fields. Unknown ids are ignored (no field to write).
inline void write_preset_scalar_pair(KeyboardPreset& p, ParameterId id,
                                     double left, double right) noexcept {
  switch (id) {
    case ParameterId::keyboard_mode:                p.mode = static_cast<std::uint8_t>(left);              p.modeR = static_cast<std::uint8_t>(right);              return;
    case ParameterId::keyboard_arp_hold:            p.arpHold = static_cast<std::uint8_t>(left);           p.arpHoldR = static_cast<std::uint8_t>(right);           return;
    case ParameterId::keyboard_arp_direction:       p.arpDirection = static_cast<std::uint8_t>(left);      p.arpDirectionR = static_cast<std::uint8_t>(right);      return;
    case ParameterId::keyboard_arp_variation:       p.arpVariation = static_cast<std::uint8_t>(left);      p.arpVariationR = static_cast<std::uint8_t>(right);      return;
    case ParameterId::keyboard_arp_interval:        p.arpInterval = static_cast<float>(left);              p.arpIntervalR = static_cast<float>(right);              return;
    case ParameterId::keyboard_arp_length:          p.arpLength = static_cast<float>(left);                p.arpLengthR = static_cast<float>(right);                return;
    case ParameterId::keyboard_seq_run:             p.seqRun = static_cast<std::uint8_t>(left);           p.seqRunR = static_cast<std::uint8_t>(right);           return;
    case ParameterId::keyboard_seq_length:          p.seqLength = static_cast<float>(left);               p.seqLengthR = static_cast<float>(right);               return;
    case ParameterId::keyboard_seq_direction:       p.seqDirection = static_cast<std::uint8_t>(left);     p.seqDirectionR = static_cast<std::uint8_t>(right);     return;
    case ParameterId::keyboard_seq_cv_output:       p.seqCvOutput = static_cast<std::uint8_t>(left);      p.seqCvOutputR = static_cast<std::uint8_t>(right);      return;
    case ParameterId::keyboard_seq_rhythm_length:   p.seqRhythmLength = static_cast<float>(left);         p.seqRhythmLengthR = static_cast<float>(right);         return;
    case ParameterId::keyboard_portamento_speed:    p.portamentoSpeed = static_cast<float>(left);         p.portamentoSpeedR = static_cast<float>(right);         return;
    case ParameterId::keyboard_portamento_legato:   p.portamentoLegato = static_cast<std::uint8_t>(left); p.portamentoLegatoR = static_cast<std::uint8_t>(right); return;
    case ParameterId::keyboard_vibrato_speed:       p.vibratoSpeed = static_cast<float>(left);            p.vibratoSpeedR = static_cast<float>(right);            return;
    case ParameterId::keyboard_vibrato_depth:       p.vibratoDepth = static_cast<float>(left);            p.vibratoDepthR = static_cast<float>(right);            return;
    case ParameterId::keyboard_vibrato_delay:       p.vibratoDelay = static_cast<float>(left);            p.vibratoDelayR = static_cast<float>(right);            return;
    case ParameterId::keyboard_vibrato_pressure:    p.vibratoPressure = static_cast<float>(left);         p.vibratoPressureR = static_cast<float>(right);         return;
    case ParameterId::keyboard_pressure_output:     p.pressureOutput = static_cast<std::uint8_t>(left);   p.pressureOutputR = static_cast<std::uint8_t>(right);   return;
    case ParameterId::keyboard_pressure_rise:       p.pressureRise = static_cast<float>(left);            p.pressureRiseR = static_cast<float>(right);            return;
    case ParameterId::keyboard_pressure_fall:       p.pressureFall = static_cast<float>(left);            p.pressureFallR = static_cast<float>(right);            return;
    case ParameterId::keyboard_quantise_load_scale: p.quantiseLoadScale = static_cast<std::uint8_t>(left); p.quantiseLoadScaleR = static_cast<std::uint8_t>(right); return;
    case ParameterId::keyboard_root_note:           p.rootNote = static_cast<float>(left);                p.rootNoteR = static_cast<float>(right);                return;
    default: return;
  }
}

// Transfer a preset slot's per-side scalar banks into the LIVE machine state:
//   bank 0 (left/shared): live.parameters[ParameterId]      = preset.left
//   bank 1 (right):        live.keyboardScalarRight[index]   = preset.right
// Only the per-side scalar ParameterIds are copied. A per-side preset param that
// is NOT in kKeyboardScalarParameterIds is silently skipped — which is exactly
// what the invariant's negative control exploits (a param made global in live is
// dropped here and the round-trip loses its right value -> red).
inline void load_live_side_bank(const KeyboardPreset& p, DeviceStateV1& live) noexcept {
  for (std::uint32_t i = 0; i < kKeyboardScalarRightCount; ++i) {
    const ParameterId id = kKeyboardScalarParameterIds[i];
    double left = 0.0, right = 0.0;
    read_preset_scalar_pair(p, id, &left, &right);
    live.parameters[static_cast<IdValue>(id)] = left;
    live.keyboardScalarRight[i] = right;
  }
}

// The inverse of load_live_side_bank: snapshot the LIVE per-side scalar banks into
// a preset slot. The same 22 scalar ParameterIds drive both directions, so the
// round-trip is exactly the invariant: preset -> live -> preset is lossless iff
// every preset per-side scalar is also a live per-side scalar.
inline void save_live_side_bank(const DeviceStateV1& live, KeyboardPreset& p) noexcept {
  for (std::uint32_t i = 0; i < kKeyboardScalarRightCount; ++i) {
    const ParameterId id = kKeyboardScalarParameterIds[i];
    write_preset_scalar_pair(p, id, live.parameters[static_cast<IdValue>(id)],
                             live.keyboardScalarRight[i]);
  }
}

}  // namespace lunar24::core
