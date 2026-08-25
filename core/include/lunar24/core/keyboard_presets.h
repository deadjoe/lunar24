// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Keyboard preset operations (design/06 §L4/§P4, design/07 §6): the four native
// presets A-D and the load / save / initialise actions that manage them.
//
// Scope discipline (P4-② state half): this is the STATE layer. The four slots
// carry the frozen keyboard_params_minus_clock payload (31 params — schema in
// device_state.h) and these functions move a slot's OWNED state. The per-field ->
// ParameterId INTERPRETATION (which scalar lands in parameters[ParameterId],
// including the behaviour/output double-mapping) is the P4-③ behaviour layer and
// is deliberately deferred here; the slot record already carries those scalars,
// so nothing is lost and the state half round-trips the full payload exactly.
//
// Persistence reuses the P2-⑤ pathway: a slot is part of DeviceStateV1, which is
// serialized/deserialized by encode_device_state / decode_device_state (the
// temp->flush->rename snapshot writer). No separate preset store is invented.

#pragma once

#include <cstdint>

#include <lunar24/core/device_state.h>

namespace lunar24::core {

// A slot index is valid iff it addresses one of the four native presets.
inline bool preset_slot_is_valid(std::uint32_t slot) {
  return slot < kDeviceKeyboardPresetCount;
}

// The factory-default profile for a slot. The manual names the four presets A-D
// (manual L1040-1045) but assigns no numeric id, so the STABLE slot index stands
// in as the "never-renumbered" sys-selected id; every other field is value-
// initialised because the manual does not enumerate a per-field preset default.
// PROVISIONAL: when the manual / real unit supplies factory defaults, populate
// them here and record the source (FINDINGS). Non-zero defaults must not be
// invented at P0 — the keyboard preset payload has no evidenced initial value.
inline KeyboardPreset initial_keyboard_preset(std::uint32_t slot) {
  KeyboardPreset p{};  // value-initialised; no evidenced non-zero factory default
  p.id = slot;         // stable slot identity: A=0, B=1, C=2, D=3 (PROVISIONAL)
  return p;
}

// Reset one slot to the factory default WITHOUT touching the other three slots,
// the live keyboard state, or any other DeviceStateV1 member. This is the P4-②
// "initialise returns to initial value, not a whole-device zero" guarantee: the
// destructive scope is exactly the one slot. Returns false for an out-of-range
// slot (the "no 5th preset" rejection).
inline bool initialise_preset(DeviceStateV1& state, std::uint32_t slot) {
  if (!preset_slot_is_valid(slot)) return false;
  state.keyboardPresets[slot] = initial_keyboard_preset(slot);
  return true;
}

// Load one slot's OWNED state into the live keyboard state: the pressure-jack
// settings (shell) and the non-scalar / no-domain-selector state (design/07 §6).
// The landed scalar -> parameters[ParameterId] wiring is the P4-③ behaviour
// layer and is NOT performed here.
inline bool load_preset(const DeviceStateV1& state, std::uint32_t slot,
                        KeyboardSettings* keyboardSettings,
                        KeyboardSeq* keyboardSeqCurrent,
                        std::uint16_t* keyboardScaleEditor,
                        float* keyboardPlateTune, float* keyboardPushbutton,
                        std::uint8_t* keyboardClockSelectors) {
  if (!preset_slot_is_valid(slot)) return false;
  const KeyboardPreset& p = state.keyboardPresets[slot];
  keyboardSettings->pressureBehaviour = p.pressureBehaviour;
  keyboardSettings->pressureOutput = p.pressureOutput;
  *keyboardSeqCurrent = p.seqSteps;
  *keyboardScaleEditor = p.quantiseScaleEditor;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    keyboardPlateTune[i] = p.plateTune[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    keyboardPushbutton[i] = p.pushbuttonValue[i];
  keyboardClockSelectors[0] = p.arpClock;   // arp_clock
  keyboardClockSelectors[1] = p.arpRhythm;  // arp_rhythm
  keyboardClockSelectors[2] = p.seqClock;   // seq_clock
  keyboardClockSelectors[3] = p.seqRhythm;  // seq_rhythm
  return true;
}

// The inverse of load_preset: snapshot the live keyboard state into a slot.
// Also does NOT touch the slot's scalar fields (P4-③ interpretation).
inline bool save_preset(DeviceStateV1& state, std::uint32_t slot,
                        const KeyboardSettings& keyboardSettings,
                        const KeyboardSeq& keyboardSeqCurrent,
                        std::uint16_t keyboardScaleEditor,
                        const float* keyboardPlateTune, const float* keyboardPushbutton,
                        const std::uint8_t* keyboardClockSelectors) {
  if (!preset_slot_is_valid(slot)) return false;
  KeyboardPreset& p = state.keyboardPresets[slot];
  p.pressureBehaviour = keyboardSettings.pressureBehaviour;
  p.pressureOutput = keyboardSettings.pressureOutput;
  p.seqSteps = keyboardSeqCurrent;
  p.quantiseScaleEditor = keyboardScaleEditor;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    p.plateTune[i] = keyboardPlateTune[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    p.pushbuttonValue[i] = keyboardPushbutton[i];
  p.arpClock = keyboardClockSelectors[0];
  p.arpRhythm = keyboardClockSelectors[1];
  p.seqClock = keyboardClockSelectors[2];
  p.seqRhythm = keyboardClockSelectors[3];
  return true;
}

}  // namespace lunar24::core
