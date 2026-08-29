// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-② preset STATE: the frozen keyboard_params_minus_clock payload lives in the
// four native presets, and load/save/initialise manage them. Test #4 of the P4-②
// mandate (sound behaviour flows through the P4-① InputStateMachine only) is the
// P4-③ behaviour half and is deliberately deferred — it is not exercised here.

#include "mini_test.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

// The frozen keyboard_params_minus_clock payload as named fields of the preset
// wire record (device_state.h kKeyboardPresetFields). "pressure_behaviour" is
// the layout name for the manual's 'Behaviour' param; "pressure_output" likewise.
static const char* const kExpectedParamFields[] = {
    "pressure_behaviour", "pressure_output", "mode", "arp_hold", "arp_clock",
    "arp_direction", "arp_variation", "arp_interval", "arp_rhythm", "arp_length",
    "seq_run", "seq_length", "seq_clock", "seq_direction", "seq_cv_output",
    "seq_rhythm", "seq_rhythm_length", "seq_steps", "portamento_speed",
    "portamento_legato", "vibrato_speed", "vibrato_depth", "vibrato_delay",
    "vibrato_pressure", "pressure_rise", "pressure_fall", "quantise_scale_editor",
    "quantise_load_scale", "root_note", "plate_tune", "pushbutton_value",
};
static constexpr std::uint32_t kExpectedParamCount = 31u;

// Test #1: exactly four native presets (A-D). Negative: a fifth slot is rejected.
static void only_four_presets() {
  CHECK_EQ(core::kDeviceKeyboardPresetCount, 4u);
  for (std::uint32_t slot = 0; slot < 4u; ++slot) {
    CHECK(core::preset_slot_is_valid(slot));
  }
  // Negative control: the "creation of a 5th preset" is structurally impossible —
  // the bank is sized 4, so an out-of-range slot is rejected rather than silently
  // indexing a fifth. A real bug (bank grown to 5, or an indexing slip past index 3)
  // fails these checks.
  CHECK_FALSE(core::preset_slot_is_valid(4u));
  core::DeviceStateV1 st;
  CHECK_FALSE(core::initialise_preset(st, 4u));
  CHECK_FALSE(core::preset_slot_is_valid(0xFFFFu));
}

// @Claude P4-2 pin: `preset.id == slot` must hold for the factory profile and after
// initialise_preset. The id field today carries no independent information — it always
// equals the slot index — so it is pinned as an invariant, not silently left as a
// "seems meaningful" field. Negative: if initial_keyboard_preset ever returns a
// divergent id (independent numeric identity), this fires red and forces an explicit
// decision instead of two silent identity concepts.
static void preset_id_pinned_to_slot() {
  for (std::uint32_t slot = 0; slot < core::kDeviceKeyboardPresetCount; ++slot) {
    CHECK_EQ(core::initial_keyboard_preset(slot).id, slot);
  }
  core::DeviceStateV1 st;
  for (std::uint32_t slot = 0; slot < core::kDeviceKeyboardPresetCount; ++slot) {
    CHECK(core::initialise_preset(st, slot));
    CHECK_EQ(st.keyboardPresets[slot].id, slot);
  }
  // The occupied-reset case must also keep id == slot (no drift after a real use).
  st.keyboardPresets[3].id = 7u;  // a divergent value written to a populated preset
  CHECK(core::initialise_preset(st, 3u));
  CHECK_EQ(st.keyboardPresets[3].id, 3u);
}

// Test #2: the preset record carries exactly the 31 keyboard params (minus tempo).
// Negative: storing tempo (clock_bpm) in a preset is forbidden and must be caught.
static void preset_payload_is_params_except_tempo() {
  std::uint32_t paramFieldCount = 0;
  bool seenTempo = false;
  for (std::uint32_t j = 0; j < core::kKeyboardPresetLayout.fieldCount; ++j) {
    const std::string_view n = core::kKeyboardPresetLayout.fields[j].name;
    if (n == "id" || n == "reserved") continue;  // shell bytes, not a parameter
    // Every preset parameter must be one of the frozen payload's 31, OR the
    // right-half "_r" variant of one of them (P4-③ per-side). "pressure_behaviour"
    // is the GLOBAL single/twin/split selector, so it is the one frozen field with
    // no per-side "_r" form.
    std::string_view base = n;
    bool isRight = false;
    if (n.size() > 2u && n.substr(n.size() - 2u) == "_r") {
      base = n.substr(0u, n.size() - 2u);
      isRight = true;
    }
    bool expected = false;
    for (std::uint32_t e = 0; e < kExpectedParamCount; ++e) {
      if (base == kExpectedParamFields[e]) { expected = true; break; }
    }
    CHECK(expected);  // a preset param field that is not in the frozen 31 -> red
    if (isRight) CHECK(base != std::string_view("pressure_behaviour"));
    if (base == std::string_view("clock_bpm")) seenTempo = true;
    ++paramFieldCount;
  }
  // 31 base params (incl. the global behaviour) + 30 right-half "_r" variants.
  CHECK_EQ(paramFieldCount, 61u);
  // Negative control: tempo/clock must NOT be storable in a preset, and the
  // frozen manifest excludes it (recordSchemas.keyboard_preset.excludes has
  // keyboard.clock_bpm). If a tempo field is ever added to the preset, this
  // fires red.
  CHECK_FALSE(seenTempo);

  // The shell (id + reserved) + 31 base params + 30 right params = 63 sub-fields.
  CHECK_EQ(core::kKeyboardPresetLayout.fieldCount, 63u);
  CHECK_EQ(core::kKeyboardPresetRecordBytes, 487u);
  // Structural invariant: the right half-bank is a contiguous kKeyboardSideBankBytes
  // region appended after the v2 247-byte record (0..246 untouched).
  CHECK_EQ(core::kKeyboardPresetRecordBytes, 247u + core::kKeyboardSideBankBytes);
}

// Populate every field of a preset with distinct, non-trivial values so a
// round-trip can prove each survives (a skipped byte in the serializer shows up).
static void fill_preset(core::KeyboardPreset& p) {
  p.id = 2u;
  p.pressureBehaviour = 1u;
  p.pressureOutput = 9u;
  p.reserved[0] = 0xEAu;
  p.reserved[1] = 0xF5u;  // a "version-N wrote here" byte, must be preserved
  p.mode = 3u;
  p.arpHold = 4u;
  p.arpClock = 5u;
  p.arpDirection = 6u;
  p.arpVariation = 7u;
  p.arpInterval = 0.5f;
  p.arpRhythm = 8u;
  p.arpLength = 0.75f;
  p.seqRun = 9u;
  p.seqLength = 1.0f;
  p.seqClock = 10u;
  p.seqDirection = 11u;
  p.seqCvOutput = 12u;
  p.seqRhythm = 13u;
  p.seqRhythmLength = 1.25f;
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    p.seqSteps.steps[i].note = static_cast<std::uint8_t>(i);
    p.seqSteps.steps[i].value = 0.25f * static_cast<float>(i);
    p.seqSteps.steps[i].gate = static_cast<std::uint8_t>(i % 2u);
  }
  p.portamentoSpeed = 0.125f;
  p.portamentoLegato = 1u;
  p.vibratoSpeed = 0.375f;
  p.vibratoDepth = 0.5f;
  p.vibratoDelay = 0.625f;
  p.vibratoPressure = 0.875f;
  p.pressureRise = 0.9f;
  p.pressureFall = 0.95f;
  p.quantiseScaleEditor = 0x0ABCu;
  p.quantiseLoadScale = 1u;
  p.rootNote = 4.5f;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i)
    p.plateTune[i] = 0.1f * static_cast<float>(i);
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i)
    p.pushbuttonValue[i] = 0.2f * static_cast<float>(i);

  // RIGHT half-bank (P4-③ per-side): distinct values so the round-trip proves the
  // two halves survive independently. A serializer that ignored "_r", or mapped a
  // right field onto the left offset, would fail the equality below.
  p.pressureOutputR = 21u;
  p.modeR = 3u + 100u;
  p.arpHoldR = 4u + 100u;
  p.arpClockR = 5u + 100u;
  p.arpDirectionR = 6u + 100u;
  p.arpVariationR = 7u + 100u;
  p.arpIntervalR = 0.5f + 100.0f;
  p.arpRhythmR = 8u + 100u;
  p.arpLengthR = 0.75f + 100.0f;
  p.seqRunR = 9u + 100u;
  p.seqLengthR = 1.0f + 100.0f;
  p.seqClockR = 10u + 100u;
  p.seqDirectionR = 11u + 100u;
  p.seqCvOutputR = 12u + 100u;
  p.seqRhythmR = 13u + 100u;
  p.seqRhythmLengthR = 1.25f + 100.0f;
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    p.seqStepsR.steps[i].note = static_cast<std::uint8_t>(i + 10u);
    p.seqStepsR.steps[i].value = 0.25f * static_cast<float>(i) + 0.5f;
    p.seqStepsR.steps[i].gate = static_cast<std::uint8_t>((i + 1u) % 2u);
  }
  p.portamentoSpeedR = 0.125f + 100.0f;
  p.portamentoLegatoR = 1u + 100u;
  p.vibratoSpeedR = 0.375f + 100.0f;
  p.vibratoDepthR = 0.5f + 100.0f;
  p.vibratoDelayR = 0.625f + 100.0f;
  p.vibratoPressureR = 0.875f + 100.0f;
  p.pressureRiseR = 0.9f + 100.0f;
  p.pressureFallR = 0.95f + 100.0f;
  p.quantiseScaleEditorR = 0x1Au;
  p.quantiseLoadScaleR = 1u + 100u;
  p.rootNoteR = 4.5f + 100.0f;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i)
    p.plateTuneR[i] = 0.1f * static_cast<float>(i) + 1.0f;
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i)
    p.pushbuttonValueR[i] = 0.2f * static_cast<float>(i) + 1.0f;
}

static bool presets_equal(const core::KeyboardPreset& a, const core::KeyboardPreset& b) {
  if (a.id != b.id || a.pressureBehaviour != b.pressureBehaviour ||
      a.pressureOutput != b.pressureOutput || a.reserved[0] != b.reserved[0] ||
      a.reserved[1] != b.reserved[1]) return false;
  if (a.mode != b.mode || a.arpHold != b.arpHold || a.arpClock != b.arpClock ||
      a.arpDirection != b.arpDirection || a.arpVariation != b.arpVariation ||
      a.arpInterval != b.arpInterval || a.arpRhythm != b.arpRhythm ||
      a.arpLength != b.arpLength || a.seqRun != b.seqRun || a.seqLength != b.seqLength ||
      a.seqClock != b.seqClock || a.seqDirection != b.seqDirection ||
      a.seqCvOutput != b.seqCvOutput || a.seqRhythm != b.seqRhythm ||
      a.seqRhythmLength != b.seqRhythmLength || a.portamentoSpeed != b.portamentoSpeed ||
      a.portamentoLegato != b.portamentoLegato || a.vibratoSpeed != b.vibratoSpeed ||
      a.vibratoDepth != b.vibratoDepth || a.vibratoDelay != b.vibratoDelay ||
      a.vibratoPressure != b.vibratoPressure || a.pressureRise != b.pressureRise ||
      a.pressureFall != b.pressureFall || a.quantiseScaleEditor != b.quantiseScaleEditor ||
      a.quantiseLoadScale != b.quantiseLoadScale || a.rootNote != b.rootNote)
    return false;
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i)
    if (a.seqSteps.steps[i].note != b.seqSteps.steps[i].note ||
        a.seqSteps.steps[i].value != b.seqSteps.steps[i].value ||
        a.seqSteps.steps[i].gate != b.seqSteps.steps[i].gate) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i)
    if (a.plateTune[i] != b.plateTune[i]) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i)
    if (a.pushbuttonValue[i] != b.pushbuttonValue[i]) return false;
  // RIGHT half-bank (P4-③ per-side).
  if (a.pressureOutputR != b.pressureOutputR || a.modeR != b.modeR ||
      a.arpHoldR != b.arpHoldR || a.arpClockR != b.arpClockR ||
      a.arpDirectionR != b.arpDirectionR || a.arpVariationR != b.arpVariationR ||
      a.arpIntervalR != b.arpIntervalR || a.arpRhythmR != b.arpRhythmR ||
      a.arpLengthR != b.arpLengthR || a.seqRunR != b.seqRunR ||
      a.seqLengthR != b.seqLengthR || a.seqClockR != b.seqClockR ||
      a.seqDirectionR != b.seqDirectionR || a.seqCvOutputR != b.seqCvOutputR ||
      a.seqRhythmR != b.seqRhythmR || a.seqRhythmLengthR != b.seqRhythmLengthR ||
      a.portamentoSpeedR != b.portamentoSpeedR || a.portamentoLegatoR != b.portamentoLegatoR ||
      a.vibratoSpeedR != b.vibratoSpeedR || a.vibratoDepthR != b.vibratoDepthR ||
      a.vibratoDelayR != b.vibratoDelayR || a.vibratoPressureR != b.vibratoPressureR ||
      a.pressureRiseR != b.pressureRiseR || a.pressureFallR != b.pressureFallR ||
      a.quantiseScaleEditorR != b.quantiseScaleEditorR ||
      a.quantiseLoadScaleR != b.quantiseLoadScaleR || a.rootNoteR != b.rootNoteR)
    return false;
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i)
    if (a.seqStepsR.steps[i].note != b.seqStepsR.steps[i].note ||
        a.seqStepsR.steps[i].value != b.seqStepsR.steps[i].value ||
        a.seqStepsR.steps[i].gate != b.seqStepsR.steps[i].gate) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i)
    if (a.plateTuneR[i] != b.plateTuneR[i]) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i)
    if (a.pushbuttonValueR[i] != b.pushbuttonValueR[i]) return false;
  return true;
}

static void full_payload_round_trip() {
  // Test #5 (reuses the P2-⑤ pathway): a preset is part of DeviceStateV1, which is
  // exactly what encode_device_state / decode_device_state (the P2-⑤ serializer)
  // walk. Save a fully-populated preset, encode, decode, and require it survive
  // bit-for-bit — including the reserved bytes (never zeroed) and the composite
  // seq_steps / plate_tune / pushbutton regions.
  core::DeviceStateV1 src;
  src.schemaVersion = core::kDeviceStorageSchemaVersion;
  fill_preset(src.keyboardPresets[1]);
  // Also populate a distinct slot and the live state, so the round-trip proves
  // no field bleeds across the record stride.
  fill_preset(src.keyboardPresets[0]);
  src.keyboardPresets[0].id = 99u;
  src.keyboardSeqCurrent.steps[5].value = 0.6f;
  src.keyboardScaleEditor = 0x5555u;
  src.keyboardClockSelectors[0] = 3u;

  std::vector<std::uint8_t> buf(core::kDeviceStorageSchema.totalBytesHint);
  std::size_t written = 0;
  CHECK(core::encode_device_state(src, buf.data(), buf.size(), &written));
  CHECK_EQ(written, core::kDeviceStorageSchema.totalBytesHint);

  core::DeviceStateV1 dst;
  CHECK(core::decode_device_state(buf.data(), written, &dst));
  CHECK_EQ(dst.schemaVersion, core::kDeviceStorageSchemaVersion);
  CHECK(presets_equal(src.keyboardPresets[0], dst.keyboardPresets[0]));
  CHECK(presets_equal(src.keyboardPresets[1], dst.keyboardPresets[1]));
  // The live non-scalar / selector fields survive too.
  CHECK_EQ(dst.keyboardSeqCurrent.steps[5].value, 0.6f);
  CHECK_EQ(dst.keyboardScaleEditor, 0x5555u);
  CHECK_EQ(dst.keyboardClockSelectors[0], 3u);
  // The reserve bytes are preserved verbatim, not zeroed (P2-⑤ @Claude Q2).
  CHECK_EQ(dst.keyboardPresets[1].reserved[0], 0xEAu);
  CHECK_EQ(dst.keyboardPresets[1].reserved[1], 0xF5u);
}

// P4-③ absolute anchor: the round-trip is necessary-but-not-sufficient (a
// serializer that writes AND reads a field at the same WRONG offset passes it).
// Pin the right half-bank to concrete wire offsets so a "consistent but wrong"
// mapping goes red. The v2 region (0..246) stays byte-identical.
static void right_bank_wire_offset_anchor() {
  core::KeyboardPreset p;
  fill_preset(p);
  std::uint8_t buf[core::kKeyboardPresetRecordBytes] = {};
  core::write_keyboard_presets(buf, p, core::kKeyboardPresetLayout);

  // Right half-bank begins at 247. First field pressure_output_r@247, mode_r@248.
  CHECK_EQ(buf[247], p.pressureOutputR);
  CHECK_EQ(buf[248], p.modeR);
  // seq_steps_r: 96-byte region at offset 275 (first step's note).
  CHECK_EQ(buf[275], static_cast<std::uint8_t>(p.seqStepsR.steps[0].note));
  // pushbutton_value_r: 32 bytes at offset 455, ending at byte 486 = last record byte.
  CHECK_EQ(core::get_f32(buf + 455u + 7u * 4u), p.pushbuttonValueR[7]);
  CHECK_EQ(core::kKeyboardPresetRecordBytes, 487u);
  // The v2 left region must be untouched: mode@8 and the last left pushbutton byte@246.
  CHECK_EQ(buf[8], p.mode);
  // Left pushbutton_value is a 32-byte f32[8] region at offset 215; its last byte is
  // the MSB of pushbuttonValue[7]'s IEEE754 representation. Compare via get_f32, the
  // same way the serializer round-trips it.
  CHECK_EQ(core::get_f32(buf + 215u + 7u * 4u), p.pushbuttonValue[7]);
}

// Test #3: the new COMPLETE transfer (task #57 / GH #12 partial) replaces the old
// fragment load_preset / save_preset. It moves the slot's whole keyboard-owned
// payload, so a snapshot into a slot round-trips every per-side scalar bank, the
// global behaviour selector, the left/right non-scalars, and the left/right
// no-domain selectors. initialise still returns the SLOT to factory (not a
// whole-device zero). Negative: an initialise that clobbers a sibling slot or the
// live state catches a real bug.
static void load_save_initialise() {
  core::DeviceStateV1 st;

  // Distinctive LIVE keyboard state to snapshot into slot 2.
  st.keyboardSettings.pressureBehaviour = 5u;
  st.keyboardSettings.pressureOutput = 44u;  // stale mirror — must NOT leak into slot 2
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const auto id = core::kKeyboardScalarParameterIds[i];
    st.parameters[static_cast<core::IdValue>(id)] = 5.0 + static_cast<double>(i);
    st.keyboardScalarRight[i] = 6.0 + static_cast<double>(i);
  }
  st.keyboardSeqCurrent.steps[3].note = 42u;
  st.keyboardSeqCurrent.steps[3].value = 0.5f;
  st.keyboardSeqCurrent.steps[3].gate = 1u;
  st.keyboardScaleEditor = 0x1234u;
  st.keyboardScaleEditorR = 0x4321u;
  st.keyboardClockSelectors[0] = 1u; st.keyboardClockSelectors[1] = 2u;
  st.keyboardClockSelectors[2] = 3u; st.keyboardClockSelectors[3] = 4u;
  st.keyboardClockSelectorsR[0] = 11u; st.keyboardClockSelectorsR[1] = 12u;
  st.keyboardClockSelectorsR[2] = 13u; st.keyboardClockSelectorsR[3] = 14u;

  CHECK(core::save_live_to_preset(st, 2u));

  // The saved slot carries the live state — every per-side scalar bank, both halves.
  const auto po = static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_output);
  CHECK_EQ(st.keyboardPresets[2].pressureBehaviour, 5u);
  // pressure_output in the slot comes from the CANONICAL left scalar bank, never the
  // stale mirror 44u (the negative that a wrong "read the shell" save would trip).
  CHECK_EQ(st.keyboardPresets[2].pressureOutput,
           static_cast<std::uint8_t>(st.parameters[po]));
  CHECK_EQ(st.keyboardPresets[2].pressureOutputR,
           static_cast<std::uint8_t>(st.keyboardScalarRight[core::keyboard_scalar_index(core::ParameterId::keyboard_pressure_output)]));
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const auto id = core::kKeyboardScalarParameterIds[i];
    double l = 0.0, r = 0.0;
    core::read_preset_scalar_pair(st.keyboardPresets[2], id, &l, &r);
    CHECK_EQ(l, st.parameters[static_cast<core::IdValue>(id)]);
    CHECK_EQ(r, st.keyboardScalarRight[i]);
  }
  // Non-scalars + no-domain selectors, both halves, are moved verbatim.
  CHECK_EQ(st.keyboardPresets[2].seqSteps.steps[3].note, 42u);
  CHECK_EQ(st.keyboardPresets[2].seqSteps.steps[3].value, 0.5f);
  CHECK_EQ(st.keyboardPresets[2].quantiseScaleEditor, 0x1234u);
  CHECK_EQ(st.keyboardPresets[2].quantiseScaleEditorR, 0x4321u);
  CHECK_EQ(st.keyboardPresets[2].arpClock, 1u);
  CHECK_EQ(st.keyboardPresets[2].arpRhythm, 2u);
  CHECK_EQ(st.keyboardPresets[2].seqClock, 3u);
  CHECK_EQ(st.keyboardPresets[2].seqRhythm, 4u);
  CHECK_EQ(st.keyboardPresets[2].arpClockR, 11u);
  CHECK_EQ(st.keyboardPresets[2].arpRhythmR, 12u);
  CHECK_EQ(st.keyboardPresets[2].seqClockR, 13u);
  CHECK_EQ(st.keyboardPresets[2].seqRhythmR, 14u);

  // initialise resets the SLOT to factory and must not touch the other three slots,
  // the live keyboard state, or any non-keyboard DeviceStateV1 member.
  const auto before = st.keyboardPresets;
  CHECK(core::initialise_preset(st, 2u));
  CHECK_EQ(st.keyboardPresets[2].id, 2u);  // the slot's stable identity is restored
  CHECK(presets_equal(st.keyboardPresets[0], before[0]));  // sibling A untouched
  CHECK(presets_equal(st.keyboardPresets[1], before[1]));  // sibling B untouched
  CHECK(presets_equal(st.keyboardPresets[3], before[3]));  // sibling D untouched
  // Negative control: an initialise that wiped the machine (params, live seq,
  // identity) would fail the "non-zeroing" requirement — a real clobber bug.
  CHECK_EQ(st.keyboardSeqCurrent.steps[3].note, 42u);  // live seq is NOT reset by slot initialise
  CHECK(st.schemaVersion == core::kDeviceStorageSchemaVersion);

  // Out-of-range load/save are rejected (no 5th slot).
  CHECK_FALSE(core::save_live_to_preset(st, 4u));
  CHECK_FALSE(core::load_preset_to_live(st, 4u));
}

// A distinct per-field LEFT/RIGHT preset, loaded through the complete public API,
// must land in the live state on BOTH halves: bank[0] = parameters[id], bank[1] =
// keyboardScalarRight; the left/right non-scalars and the left/right no-domain
// selectors too. This is the "prove live both sides match" oracle.
static void full_preset_live_transfer_both_halves() {
  core::KeyboardPreset pt;
  fill_preset(pt);  // each left field differs from its right `_R` counterpart

  core::DeviceStateV1 live;
  live.keyboardPresets[0] = pt;
  // Pre-existing live values that must be overwritten by the load.
  live.keyboardSettings.pressureBehaviour = 7u;
  live.keyboardSettings.pressureOutput = 250u;
  const auto mode_id = static_cast<core::IdValue>(core::ParameterId::keyboard_mode);
  const auto mode_idx = core::keyboard_scalar_index(core::ParameterId::keyboard_mode);
  live.parameters[mode_id] = 999.0;
  live.keyboardScalarRight[mode_idx] = 888.0;

  CHECK(core::load_preset_to_live(live, 0u));

  // Global behaviour selector overwritten.
  CHECK_EQ(live.keyboardSettings.pressureBehaviour, pt.pressureBehaviour);
  // Both scalar banks, every per-side scalar: left = parameters[id], right =
  // keyboardScalarRight[idx], and the two are distinct (fill_preset makes each right
  // a distinct value from its left).
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const auto id = core::kKeyboardScalarParameterIds[i];
    double l = 0.0, r = 0.0;
    core::read_preset_scalar_pair(pt, id, &l, &r);
    CHECK_EQ(live.parameters[static_cast<core::IdValue>(id)], l);
    CHECK_EQ(live.keyboardScalarRight[i], r);
    CHECK(live.parameters[static_cast<core::IdValue>(id)] != live.keyboardScalarRight[i]);
  }
  // LEFT non-scalars + no-domain selectors.
  CHECK_EQ(live.keyboardSeqCurrent.steps[3].note, pt.seqSteps.steps[3].note);
  CHECK_EQ(live.keyboardScaleEditor, pt.quantiseScaleEditor);
  CHECK_EQ(live.keyboardClockSelectors[0], pt.arpClock);
  CHECK_EQ(live.keyboardClockSelectors[1], pt.arpRhythm);
  CHECK_EQ(live.keyboardClockSelectors[2], pt.seqClock);
  CHECK_EQ(live.keyboardClockSelectors[3], pt.seqRhythm);
  // RIGHT non-scalars + no-domain selectors.
  CHECK_EQ(live.keyboardSeqCurrentR.steps[3].note, pt.seqStepsR.steps[3].note);
  CHECK_EQ(live.keyboardScaleEditorR, pt.quantiseScaleEditorR);
  CHECK_EQ(live.keyboardClockSelectorsR[0], pt.arpClockR);
  CHECK_EQ(live.keyboardClockSelectorsR[1], pt.arpRhythmR);
  CHECK_EQ(live.keyboardClockSelectorsR[2], pt.seqClockR);
  CHECK_EQ(live.keyboardClockSelectorsR[3], pt.seqRhythmR);
  // Pressure-output compatibility mirror converges to the canonical LEFT scalar.
  const auto po = static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_output);
  CHECK_EQ(live.keyboardSettings.pressureOutput,
           static_cast<std::uint8_t>(live.parameters[po]));
}

// A distinct live keyboard set, saved through the complete public API, must land in
// the SELECTED slot exactly and leave the three siblings, the slot's own id/reserved,
// and every non-keyboard DeviceStateV1 member untouched.
static void full_live_save_matches_slot_only() {
  core::DeviceStateV1 st;
  for (std::uint32_t s = 0; s < core::kDeviceKeyboardPresetCount; ++s) {
    core::KeyboardPreset pp;
    fill_preset(pp);
    pp.id = s;
    pp.reserved[0] = static_cast<std::uint8_t>(0x10u + s);
    pp.reserved[1] = static_cast<std::uint8_t>(0x20u + s);
    st.keyboardPresets[s] = pp;
  }
  const auto before0 = st.keyboardPresets[0];
  const auto before2 = st.keyboardPresets[2];
  const auto before3 = st.keyboardPresets[3];
  // A couple of non-keyboard markers that a stray write would clobber.
  st.identityModelVersion = 7u;
  st.routeOverridden[3] = 1u;

  st.keyboardSettings.pressureBehaviour = 6u;
  st.keyboardSettings.pressureOutput = 77u;  // stale mirror, ignored on save
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const auto id = core::kKeyboardScalarParameterIds[i];
    st.parameters[static_cast<core::IdValue>(id)] = 5.0 + static_cast<double>(i);
    st.keyboardScalarRight[i] = 6.0 + static_cast<double>(i);
  }
  st.keyboardSeqCurrent.steps[7].note = 30u;
  st.keyboardSeqCurrentR.steps[7].note = 31u;
  st.keyboardScaleEditor = 0x2222u;
  st.keyboardScaleEditorR = 0x3333u;
  st.keyboardClockSelectors[0] = 8u; st.keyboardClockSelectors[1] = 7u;
  st.keyboardClockSelectors[2] = 6u; st.keyboardClockSelectors[3] = 5u;
  st.keyboardClockSelectorsR[0] = 18u; st.keyboardClockSelectorsR[1] = 17u;
  st.keyboardClockSelectorsR[2] = 16u; st.keyboardClockSelectorsR[3] = 15u;

  CHECK(core::save_live_to_preset(st, 1u));

  const auto& s1 = st.keyboardPresets[1];
  // Slot 1 identity + reserved preserved (they are NOT live controls).
  CHECK_EQ(s1.id, 1u);
  CHECK_EQ(s1.reserved[0], 0x11u);
  CHECK_EQ(s1.reserved[1], 0x21u);
  CHECK_EQ(s1.pressureBehaviour, 6u);
  // Live scalar banks land verbatim (canonical, not the 77u mirror).
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const auto id = core::kKeyboardScalarParameterIds[i];
    double l = 0.0, r = 0.0;
    core::read_preset_scalar_pair(s1, id, &l, &r);
    CHECK_EQ(l, st.parameters[static_cast<core::IdValue>(id)]);
    CHECK_EQ(r, st.keyboardScalarRight[i]);
  }
  // Non-scalars + no-domain selectors, both halves.
  CHECK_EQ(s1.seqSteps.steps[7].note, 30u);
  CHECK_EQ(s1.seqStepsR.steps[7].note, 31u);
  CHECK_EQ(s1.quantiseScaleEditor, 0x2222u);
  CHECK_EQ(s1.quantiseScaleEditorR, 0x3333u);
  CHECK_EQ(s1.arpClock, 8u); CHECK_EQ(s1.arpRhythm, 7u);
  CHECK_EQ(s1.seqClock, 6u); CHECK_EQ(s1.seqRhythm, 5u);
  CHECK_EQ(s1.arpClockR, 18u); CHECK_EQ(s1.arpRhythmR, 17u);
  CHECK_EQ(s1.seqClockR, 16u); CHECK_EQ(s1.seqRhythmR, 15u);

  // Siblings untouched, and the non-keyboard markers untouched.
  CHECK(presets_equal(st.keyboardPresets[0], before0));
  CHECK(presets_equal(st.keyboardPresets[2], before2));
  CHECK(presets_equal(st.keyboardPresets[3], before3));
  CHECK_EQ(st.identityModelVersion, 7u);
  CHECK_EQ(st.routeOverridden[3], 1u);
}

// preset -> live -> preset through the complete public API is lossless on the whole
// keyboard-owned payload, INCLUDING the shell's id/reserved bytes (which the load
// never reads and the save never writes, so they survive verbatim).
static void full_preset_live_preset_round_trip_lossless() {
  core::KeyboardPreset orig;
  fill_preset(orig);  // id=2, reserved[0]=0xEA, reserved[1]=0xF5

  core::DeviceStateV1 st;
  st.keyboardPresets[0] = orig;
  CHECK(core::load_preset_to_live(st, 0u));
  CHECK(core::save_live_to_preset(st, 0u));

  CHECK(presets_equal(st.keyboardPresets[0], orig));
}

// An out-of-range slot must return false AND leave both the live keyboard state and
// the preset bank untouched (no partial write into some other memory).
static void invalid_slot_returns_false_zero_mutation() {
  core::DeviceStateV1 st;
  core::KeyboardPreset known;
  fill_preset(known);
  st.keyboardPresets[0] = known;
  const auto before0 = st.keyboardPresets[0];
  const auto liveBefore = st.keyboardSettings;
  const auto seqBefore = st.keyboardSeqCurrent;

  CHECK_FALSE(core::load_preset_to_live(st, 4u));
  CHECK_FALSE(core::save_live_to_preset(st, 4u));
  CHECK_FALSE(core::load_preset_to_live(st, 0xFFFFu));
  CHECK_FALSE(core::save_live_to_preset(st, 0xFFFFu));

  CHECK(presets_equal(st.keyboardPresets[0], before0));
  CHECK_EQ(st.keyboardSettings.pressureBehaviour, liveBefore.pressureBehaviour);
  CHECK_EQ(st.keyboardSettings.pressureOutput, liveBefore.pressureOutput);
  CHECK(st.keyboardSeqCurrent.steps[0].note == seqBefore.steps[0].note);
  // A genuinely invalid slot index rejected at the gate never writes slot 1 either.
  CHECK(presets_equal(st.keyboardPresets[1], core::KeyboardPreset{}));
}

// pressure_output single-truth (design/00 §181, task #57): the canonical live values
// are the left scalar bank and the right scalar bank; keyboardSettings.pressureOutput
// is a COMPAT MIRROR that converges to the canonical LEFT on load and must NOT be
// read on save (a stale mirror must never pollute the preset).
static void pressure_output_canonicality() {
  core::KeyboardPreset pt;
  fill_preset(pt);  // pressureOutput = 9 (left), pressureOutputR = 21 (right)

  // --- load: mirror converges to canonical left ---
  core::DeviceStateV1 live;
  live.keyboardPresets[0] = pt;
  live.keyboardSettings.pressureOutput = 200u;  // stale mirror BEFORE load
  CHECK(core::load_preset_to_live(live, 0u));
  const auto po = static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_output);
  const auto po_idx = core::keyboard_scalar_index(core::ParameterId::keyboard_pressure_output);
  CHECK_EQ(live.parameters[po], static_cast<double>(pt.pressureOutput));   // canonical left
  CHECK_EQ(live.keyboardScalarRight[po_idx], static_cast<double>(pt.pressureOutputR));  // canonical right
  CHECK_EQ(live.keyboardSettings.pressureOutput,
           static_cast<std::uint8_t>(pt.pressureOutput));  // mirror converged to left

  // --- save: canonical left wins, a conflicting mirror does NOT pollute ---
  core::KeyboardPreset target;
  target.pressureOutput = 99u;   // an old divergent preset value, must be overwritten
  target.pressureOutputR = 88u;
  target.id = 3u;
  target.reserved[0] = 0xABu;
  live.keyboardPresets[1] = target;
  live.parameters[po] = 30.0;                // canonical LEFT
  live.keyboardScalarRight[po_idx] = 40.0;   // canonical RIGHT
  live.keyboardSettings.pressureOutput = 240;  // STALE mirror, conflict with canonical
  CHECK(core::save_live_to_preset(live, 1u));
  CHECK_EQ(live.keyboardPresets[1].pressureOutput, 30u);   // from canonical left, NOT 240/99
  CHECK_EQ(live.keyboardPresets[1].pressureOutputR, 40u);  // from canonical right
  CHECK_EQ(live.keyboardPresets[1].id, 3u);                // id preserved
  CHECK_EQ(live.keyboardPresets[1].reserved[0], 0xABu);    // reserved preserved
}

// P4-③ §2e invariant (design/00, @Claude msg c0d9e9be): a keyboard scalar is
// per-side in the preset IFF it is per-side in live. Bank[0] (left/shared) stays in
// `parameters[ParameterId]`; bank[1] (right) lives in `keyboardScalarRight[index]`.
// Preset -> live -> preset must be lossless on every per-side scalar, and the two
// banks must hold DISTINCT values — split is two banks under ONE id (the id space
// never grows for the right; the index map, not a new id, addresses the right bank).
//
//   * Absolute anchor — live bank[0] and bank[1] hold the correct, separate left /
//     right values (this separates "two banks under one id" from "one global value
//     on both sides"). Anchored on keyboard_mode and on pressure_output, the one
//     @Claude specifically ruled per-side in §2e.
//   * Round-trip — preset -> live -> preset on the whole preset is lossless.
//   * Index map — exact both ways: every per-side scalar maps to its own slot, and
//     the global selector keyboard_behaviour maps to -1 (no right bank).
//
// (The negative control is run out-of-band: drop a right-bank transfer in
// load_live_side_bank and this test reds — the round-trip loses the right value.)
static void live_scalar_bank_preset_live_round_trip() {
  core::KeyboardPreset orig;
  fill_preset(orig);

  core::DeviceStateV1 live;
  core::load_live_side_bank(orig, live);

  // Absolute anchor: bank[0] = parameters[id] holds LEFT, bank[1] = keyboardScalarRight
  // holds RIGHT, and they are distinct (fill_preset sets mode=3 vs modeR=103).
  const auto mode_id = static_cast<core::IdValue>(core::ParameterId::keyboard_mode);
  const std::int32_t mode_idx = core::keyboard_scalar_index(core::ParameterId::keyboard_mode);
  CHECK_EQ(live.parameters[mode_id], static_cast<double>(orig.mode));
  CHECK_EQ(live.keyboardScalarRight[mode_idx], static_cast<double>(orig.modeR));
  CHECK(live.parameters[mode_id] != live.keyboardScalarRight[mode_idx]);
  // pressure_output is per-side too (a live scalar, not a global shell value).
  const auto po_id = static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_output);
  const std::int32_t po_idx = core::keyboard_scalar_index(core::ParameterId::keyboard_pressure_output);
  CHECK_EQ(live.parameters[po_id], static_cast<double>(orig.pressureOutput));
  CHECK_EQ(live.keyboardScalarRight[po_idx], static_cast<double>(orig.pressureOutputR));
  CHECK(live.parameters[po_id] != live.keyboardScalarRight[po_idx]);

  // Index map is exact both ways.
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i)
    CHECK_EQ(core::keyboard_scalar_index(core::kKeyboardScalarParameterIds[i]),
             static_cast<std::int32_t>(i));
  CHECK_EQ(core::keyboard_scalar_index(core::ParameterId::keyboard_behaviour), -1);

  // Round-trip: preset -> live -> preset is lossless on the whole preset (the
  // non-scalar `_r` fields are an untouched copy, so they are preserved too).
  core::KeyboardPreset back = orig;
  core::save_live_side_bank(live, back);
  CHECK(presets_equal(back, orig));
}

static void live_state_holds_non_scalars() {
  // design/07 §6: the non-scalars live as structured DeviceState fields, never
  // flattened into a scalar ParameterDescriptor. Assert the four kinds are all
  // present and correctly sized.
  core::DeviceStateV1 st;
  CHECK_EQ(core::kKeyboardSeqStepCount, 16u);
  CHECK_EQ(core::kKeyboardSeqStepBytes, 6u);
  CHECK_EQ(core::kKeyboardSeqBytes, 96u);
  st.keyboardSeqCurrent.steps[0].note = 1u;
  st.keyboardScaleEditor = 0xABCDu;
  st.keyboardPlateTune[11] = 0.9f;
  st.keyboardPushbutton[7] = 0.4f;
  st.keyboardClockSelectors[3] = 7u;
  CHECK_EQ(st.keyboardSeqCurrent.steps[0].note, 1u);
  CHECK_EQ(st.keyboardScaleEditor, 0xABCDu);
  CHECK_EQ(st.keyboardPlateTune[11], 0.9f);
  CHECK_EQ(st.keyboardPushbutton[7], 0.4f);
  CHECK_EQ(st.keyboardClockSelectors[3], 7u);
}

int main() {
  only_four_presets();
  preset_id_pinned_to_slot();
  preset_payload_is_params_except_tempo();
  full_payload_round_trip();
  right_bank_wire_offset_anchor();
  load_save_initialise();
  live_state_holds_non_scalars();
  live_scalar_bank_preset_live_round_trip();
  full_preset_live_transfer_both_halves();
  full_live_save_matches_slot_only();
  full_preset_live_preset_round_trip_lossless();
  invalid_slot_returns_false_zero_mutation();
  pressure_output_canonicality();
  return ::test::finish("keyboard presets (P4-②, full transfer)");
}
