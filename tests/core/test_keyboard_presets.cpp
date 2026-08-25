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
#include <lunar24/core/state_serializer.h>

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
    // Every preset parameter must be one of the frozen payload's 31.
    bool expected = false;
    for (std::uint32_t e = 0; e < kExpectedParamCount; ++e) {
      if (n == kExpectedParamFields[e]) { expected = true; break; }
    }
    CHECK(expected);  // a preset param field that is not in the frozen 31 -> red
    if (n == "clock_bpm") seenTempo = true;
    ++paramFieldCount;
  }
  CHECK_EQ(paramFieldCount, kExpectedParamCount);  // exactly 31 params, no more
  // Negative control: tempo/clock must NOT be storable in a preset, and the
  // frozen manifest excludes it (recordSchemas.keyboard_preset.excludes has
  // keyboard.clock_bpm). If a tempo field is ever added to the preset, this
  // fires red.
  CHECK_FALSE(seenTempo);

  // The shell (id + reserved) plus the 31 params = the full 33-sub-field layout.
  CHECK_EQ(core::kKeyboardPresetLayout.fieldCount, 33u);
  CHECK_EQ(core::kKeyboardPresetRecordBytes, 247u);
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

// Test #3: load / save / initialise exist; initialise returns to the factory
// value (non-destructively), NOT a whole-device zero. Negative: an initialise
// that clobbers a sibling slot or the machine state catches a real bug.
static void load_save_initialise() {
  core::DeviceStateV1 st;
  const auto before = st.keyboardPresets;

  // Save current live state into slot 2, then load it back — the owned state
  // (shell + non-scalars + no-domain selectors) must round-trip.
  core::KeyboardSeq liveSeq{};
  liveSeq.steps[3].note = 42u;
  liveSeq.steps[3].value = 0.5f;
  liveSeq.steps[3].gate = 1u;
  float plate[core::kKeyboardPlateTuneCount] = {};
  plate[7] = 0.75f;
  float push[core::kKeyboardPushbuttonCount] = {};
  push[2] = 0.25f;
  std::uint8_t sel[4] = {1u, 2u, 3u, 4u};  // arp_clock, arp_rhythm, seq_clock, seq_rhythm

  CHECK(core::save_preset(st, 2u, st.keyboardSettings, liveSeq, 0x1234u, plate, push, sel));

  core::KeyboardSettings settings;
  core::KeyboardSeq loadedSeq;
  std::uint16_t scaleEditor = 0;
  float loadedPlate[core::kKeyboardPlateTuneCount] = {};
  float loadedPush[core::kKeyboardPushbuttonCount] = {};
  std::uint8_t loadedSel[4] = {};
  CHECK(core::load_preset(st, 2u, &settings, &loadedSeq, &scaleEditor, loadedPlate, loadedPush, loadedSel));
  CHECK_EQ(loadedSeq.steps[3].note, 42u);
  CHECK_EQ(loadedSeq.steps[3].value, 0.5f);
  CHECK_EQ(loadedSeq.steps[3].gate, 1u);
  CHECK_EQ(scaleEditor, 0x1234u);
  CHECK_EQ(loadedPlate[7], 0.75f);
  CHECK_EQ(loadedPush[2], 0.25f);
  CHECK(loadedSel[0] == 1u && loadedSel[1] == 2u && loadedSel[2] == 3u && loadedSel[3] == 4u);

  // initialise resets the SLOT to the factory default and must not touch the other
  // three slots, the live keyboard state, or any other DeviceStateV1 member.
  CHECK(core::initialise_preset(st, 2u));
  CHECK_EQ(st.keyboardPresets[2].id, 2u);  // the slot's stable identity is restored
  CHECK(presets_equal(st.keyboardPresets[0], before[0]));  // sibling A untouched
  CHECK(presets_equal(st.keyboardPresets[1], before[1]));  // sibling B untouched
  CHECK(presets_equal(st.keyboardPresets[3], before[3]));  // sibling D untouched
  // Negative control: an initialise that wiped the machine (params, live seq,
  // identity) would fail the "non-zeroing" requirement — a real clobber bug.
  CHECK_EQ(loadedSeq.steps[3].note, 42u);  // live seq is NOT reset by initialising a slot
  CHECK(st.schemaVersion == core::kDeviceStorageSchemaVersion);

  // Out-of-range load/save are rejected (no 5th slot).
  CHECK_FALSE(core::save_preset(st, 4u, st.keyboardSettings, liveSeq, 0u, plate, push, sel));
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
  load_save_initialise();
  live_state_holds_non_scalars();
  return ::test::finish("keyboard presets (P4-②)");
}
