// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Keyboard preset operations (design/06 §L4/§P4, design/07 §6): the four native
// presets A-D and the load / save / initialise actions that manage them.
//
// Scope discipline (P4-② state half + task #57 / GH #12 partial): this is the STATE
// layer. The four slots carry the frozen keyboard_params_minus_clock payload (31
// params — schema in device_state.h) and these functions move a slot's WHOLE
// keyboard-owned payload between the slot and the LIVE keyboard state. The per-side
// scalar banks are carried through keyboard_side_bank.h's load_live_side_bank /
// save_live_side_bank (the one ParameterId -> bank[0]/bank[1] interpretation), so
// the transfer is complete: the former fragment load_preset / save_preset (which
// moved only the shell + non-scalars and silently dropped the scalar banks) is
// replaced by the full load_preset_to_live / save_live_to_preset. The per-field ->
// live-runtime-setter INTERPRETATION remains the P4-③ behaviour layer (GH #12).
//
// Persistence reuses the P2-⑤ pathway: a slot is part of DeviceStateV1, which is
// serialized/deserialized by encode_device_state / decode_device_state (the
// temp->flush->rename snapshot writer). No separate preset store is invented.

#pragma once

#include <cstdint>

#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_side_bank.h>

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

// Load one preset slot's COMPLETE keyboard-owned state into the LIVE keyboard
// state. This is the single product recall entry (task #57 / GH #12 partial): unlike
// the former fragment load_preset (which moved only the shell + non-scalars and
// silently dropped the scalar banks), it moves the slot's whole payload across the
// full state layer — the global single/twin/split selector, both per-side scalar
// banks (via load_live_side_bank), the left/right non-scalars, and the left/right
// no-domain selectors.
//
// It consumes the SAME DeviceStateV1 that save_live_to_preset reads, so the two are
// exact inverses on the keyboard-owned region. It never touches the slot's own
// id/reserved, the other three slots, or any non-keyboard DeviceStateV1 member.
inline bool load_preset_to_live(DeviceStateV1& live, std::uint32_t slot) noexcept {
  if (!preset_slot_is_valid(slot)) return false;
  const KeyboardPreset& p = live.keyboardPresets[slot];

  // The one non-per-side keyboard field: single/twin/split behaviour selector.
  live.keyboardSettings.pressureBehaviour = p.pressureBehaviour;

  // Both per-side scalar banks: left = parameters[id], right = keyboardScalarRight.
  load_live_side_bank(p, live);

  // Pressure-output compatibility mirror: keyboardSettings.pressureOutput is NOT a
  // third source of truth (design/00 §181 rules pressure_output per-side; the
  // canonical live value is the left scalar bank). After a load the mirror simply
  // converges to the canonical left value — it is inert even if it was stale before.
  live.keyboardSettings.pressureOutput = static_cast<std::uint8_t>(
      live.parameters[static_cast<IdValue>(ParameterId::keyboard_pressure_output)]);

  // LEFT live non-scalars (design/07 §6: kept as structured DeviceState fields).
  live.keyboardSeqCurrent = p.seqSteps;
  live.keyboardScaleEditor = p.quantiseScaleEditor;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    live.keyboardPlateTune[i] = p.plateTune[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    live.keyboardPushbutton[i] = p.pushbuttonValue[i];
  // LEFT no-domain selectors (no ParameterId in the frozen id-space).
  live.keyboardClockSelectors[0] = p.arpClock;   // arp_clock
  live.keyboardClockSelectors[1] = p.arpRhythm;  // arp_rhythm
  live.keyboardClockSelectors[2] = p.seqClock;   // seq_clock
  live.keyboardClockSelectors[3] = p.seqRhythm;  // seq_rhythm

  // RIGHT half-bank mirror (P4-③ per-side): the same four non-scalars and four
  // no-domain selectors, taken from the `_R` preset fields.
  live.keyboardSeqCurrentR = p.seqStepsR;
  live.keyboardScaleEditorR = p.quantiseScaleEditorR;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    live.keyboardPlateTuneR[i] = p.plateTuneR[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    live.keyboardPushbuttonR[i] = p.pushbuttonValueR[i];
  live.keyboardClockSelectorsR[0] = p.arpClockR;   // arp_clock_r
  live.keyboardClockSelectorsR[1] = p.arpRhythmR;  // arp_rhythm_r
  live.keyboardClockSelectorsR[2] = p.seqClockR;   // seq_clock_r
  live.keyboardClockSelectorsR[3] = p.seqRhythmR;  // seq_rhythm_r

  return true;
}

// The inverse of load_preset_to_live: snapshot the live keyboard state into one
// preset slot. Reuses save_live_side_bank so both scalar banks are written from the
// CANONICAL live banks (left = parameters[id], right = keyboardScalarRight) — never
// from the keyboardSettings.pressureOutput compatibility mirror, which may be stale
// and must not pollute the preset.
//
// It NEVER writes the slot's own id or reserved bytes (slot identity and the shell
// region are not live controls) and never touches the other three slots, so a save
// re-writes only the keyboard-owned payload that load_preset_to_live restores.
inline bool save_live_to_preset(DeviceStateV1& live, std::uint32_t slot) noexcept {
  if (!preset_slot_is_valid(slot)) return false;
  KeyboardPreset& p = live.keyboardPresets[slot];

  // Global single/twin/split selector.
  p.pressureBehaviour = live.keyboardSettings.pressureBehaviour;

  // Both per-side scalar banks from the canonical live values (incl. pressure_output
  // from the left scalar bank, never from the shell mirror).
  save_live_side_bank(live, p);

  // LEFT live non-scalars.
  p.seqSteps = live.keyboardSeqCurrent;
  p.quantiseScaleEditor = live.keyboardScaleEditor;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    p.plateTune[i] = live.keyboardPlateTune[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    p.pushbuttonValue[i] = live.keyboardPushbutton[i];
  // LEFT no-domain selectors.
  p.arpClock = live.keyboardClockSelectors[0];
  p.arpRhythm = live.keyboardClockSelectors[1];
  p.seqClock = live.keyboardClockSelectors[2];
  p.seqRhythm = live.keyboardClockSelectors[3];

  // RIGHT half-bank mirror.
  p.seqStepsR = live.keyboardSeqCurrentR;
  p.quantiseScaleEditorR = live.keyboardScaleEditorR;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    p.plateTuneR[i] = live.keyboardPlateTuneR[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    p.pushbuttonValueR[i] = live.keyboardPushbuttonR[i];
  p.arpClockR = live.keyboardClockSelectorsR[0];
  p.arpRhythmR = live.keyboardClockSelectorsR[1];
  p.seqClockR = live.keyboardClockSelectorsR[2];
  p.seqRhythmR = live.keyboardClockSelectorsR[3];

  return true;
}

}  // namespace lunar24::core
