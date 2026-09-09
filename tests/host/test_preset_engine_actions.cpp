// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// task #103 (GH #12): the ENGINE-LAYER preset LOAD / SAVE / INITIALISE acceptance oracle.
//
// WHAT THIS PINS. The four native keyboard presets (A-D) and the three operations that manage
// them now have a product seam on the owner that actually renders:
//
//     encode -> decode -> StandaloneAudioEngine::applyDeviceState -> (engine preset action) ->
//     StandaloneAudioEngine::processBlock
//
// through the ONE shared host entry (tests/host/test_engine_harness.h). Nothing here calls
// buildMachineRuntimeCandidate / processFrame directly, and nothing here re-implements the
// state-layer transfer: the engine copies the canonical DeviceStateV1, applies the ONE existing
// transfer helper, and re-publishes through the ONE existing candidate/commit path.
//
// SECTION A is the D-1 defect (@Codex msg 7d734b47, task #102 §1.3): load_preset_to_live wrote
// the canonical keyboardSettings.pressureBehaviour but never its compatibility MIRROR
// parameters[keyboard_behaviour], which validate_device_state requires to be equal
// (check_keyboard_live, family keyboard_live_invalid, field 9002). Because check_keyboard_live
// runs BEFORE check_presets, ANY slot whose behaviour differed from the live mirror made the
// whole recalled state invalid -> applyDeviceState returned RejectedInvalidState and kept the
// prior runtime, i.e. the recall was a silent no-op. A1/A2 pin the fix; A3 pins the
// discriminating power of the assertion itself (a hand-built unsynchronised candidate MUST be
// rejected with field 9002), so A1 can never pass vacuously.
//
// SECTION B pins the engine API contract: not-ready / illegal slot / unknown action are reported,
// never a false success; LOAD changes the live config AND the existing two-sided behaviour
// consumes it; SAVE stores the LAST SUCCESSFULLY COMMITTED canonical live config; INITIALISE
// resets only the target slot and never implicitly loads; all three re-commit (so the event
// timebase resets) and are atomic on failure; unconsumed fields round-trip byte-exactly; and the
// real host render is partition-invariant after a preset action.
//
// EXIT CODE. 0 = every criterion green; non-zero = at least one criterion red. Each criterion
// prints a stable label so the isolated source-mutation negative controls
// (tools/run_preset_engine_negatives.py) can assert WHICH assertion fired, not merely that the
// binary failed.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <lunar24/core/device_layout.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/core/state_validation.h>

#include "test_engine_harness.h"

namespace core = lunar24::core;
namespace testengine = lunar24::testengine;
using StandaloneAudioEngine = lunar24::host::StandaloneAudioEngine;
using testengine::EngineHarness;

// ---- the tiny named checker (same shape as the GH#12 probe family) ---------------------------

static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static constexpr std::uint64_t kSeed = testengine::kSeed;
static constexpr double kGateHigh = 10.0;  // the registered 0..10 V gate rail

// ---- state helpers ---------------------------------------------------------------------------

// bank 0 (left/shared) = parameters[ParameterId]; bank 1 (right) = keyboardScalarRight[index].
static void set_left(core::DeviceStateV1& st, core::ParameterId id, double v) {
  st.parameters[static_cast<std::size_t>(id)] = v;
}
static void set_right(core::DeviceStateV1& st, core::ParameterId id, double v) {
  const std::int32_t i = core::keyboard_scalar_index(id);
  if (i >= 0) st.keyboardScalarRight[static_cast<std::size_t>(i)] = v;
}
// The canonical behaviour selector + its REQUIRED mirror (state_validation.h field 9002).
static void set_mode(core::DeviceStateV1& st, std::uint8_t mode) {
  st.keyboardSettings.pressureBehaviour = mode;
  set_left(st, core::ParameterId::keyboard_behaviour, static_cast<double>(mode));
}

// A legal value for `id` at fraction t in [0,1]: discrete selectors snap to an option index,
// continuous params stay inside [min,max] and honour `step`. Building every value through the
// registry descriptor is what keeps a hand-made fixture a LEGAL state — the criteria must never
// pass by smuggling an out-of-range value past the validator.
static double legal_value(core::ParameterId id, double t) {
  const core::ParameterDescriptor* d = core::find_parameter(id);
  if (d == nullptr) return 0.0;
  if (d->optionCount > 0u) {
    const double n = static_cast<double>(d->optionCount - 1u);
    double idx = std::floor(t * n + 0.5);
    if (idx < 0.0) idx = 0.0;
    if (idx > n) idx = n;
    return idx;
  }
  double v = d->min + t * (d->max - d->min);
  if (d->step > 0.0) v = d->min + std::floor((v - d->min) / d->step + 0.5) * d->step;
  if (v < d->min) v = d->min;
  if (v > d->max) v = d->max;
  // The preset record stores the scalar banks as float32 (the wire schema), so a fixture value
  // must BE a float32 value or the save->load round trip would quantise it and every exact
  // comparison below would fail for a reason that has nothing to do with the transfer. Step
  // INWARD when the nearest float32 rounds past a bound, so the result stays legal AND exact.
  float f = static_cast<float>(v);
  if (static_cast<double>(f) > d->max) f = std::nextafterf(f, -INFINITY);
  if (static_cast<double>(f) < d->min) f = std::nextafterf(f, INFINITY);
  return static_cast<double>(f);
}

// ---- a complete keyboard config, read/written through the PUBLIC state fields -----------------

struct Cfg {
  std::uint8_t mode = 0;
  double scalarL[core::kKeyboardScalarRightCount] = {};
  double scalarR[core::kKeyboardScalarRightCount] = {};
  std::uint16_t scaleL = 0;
  std::uint16_t scaleR = 0;
  float plateL[core::kKeyboardPlateTuneCount] = {};
  float plateR[core::kKeyboardPlateTuneCount] = {};
  float pushL[core::kKeyboardPushbuttonCount] = {};
  float pushR[core::kKeyboardPushbuttonCount] = {};
  std::uint8_t seqNoteL[core::kKeyboardSeqStepCount] = {};
  std::uint8_t seqNoteR[core::kKeyboardSeqStepCount] = {};
  float seqValueL[core::kKeyboardSeqStepCount] = {};
  float seqValueR[core::kKeyboardSeqStepCount] = {};
  std::uint8_t seqGateL[core::kKeyboardSeqStepCount] = {};
  std::uint8_t seqGateR[core::kKeyboardSeqStepCount] = {};
  std::uint8_t clockL[4] = {};
  std::uint8_t clockR[4] = {};
};

static Cfg cfg_from_state(const core::DeviceStateV1& st) {
  Cfg c;
  c.mode = st.keyboardSettings.pressureBehaviour;
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[j];
    c.scalarL[j] = st.parameters[static_cast<std::size_t>(id)];
    c.scalarR[j] = st.keyboardScalarRight[j];
  }
  c.scaleL = st.keyboardScaleEditor;
  c.scaleR = st.keyboardScaleEditorR;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    c.plateL[i] = st.keyboardPlateTune[i];
    c.plateR[i] = st.keyboardPlateTuneR[i];
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    c.pushL[i] = st.keyboardPushbutton[i];
    c.pushR[i] = st.keyboardPushbuttonR[i];
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    c.seqNoteL[i] = st.keyboardSeqCurrent.steps[i].note;
    c.seqValueL[i] = st.keyboardSeqCurrent.steps[i].value;
    c.seqGateL[i] = st.keyboardSeqCurrent.steps[i].gate;
    c.seqNoteR[i] = st.keyboardSeqCurrentR.steps[i].note;
    c.seqValueR[i] = st.keyboardSeqCurrentR.steps[i].value;
    c.seqGateR[i] = st.keyboardSeqCurrentR.steps[i].gate;
  }
  for (std::uint32_t i = 0; i < 4u; ++i) {
    c.clockL[i] = st.keyboardClockSelectors[i];
    c.clockR[i] = st.keyboardClockSelectorsR[i];
  }
  return c;
}

// A fully synthetic, LEGAL config that differs in every side of every field. Used by the
// validation-only criteria (no render), where exotic per-side selectors cannot disturb a trace.
static Cfg make_cfg(std::uint32_t slot, std::uint32_t variant) {
  Cfg c;
  const std::uint32_t base = slot * 7u + variant * 3u;
  c.mode = static_cast<std::uint8_t>(base % 3u);
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[j];
    const double tL = 0.08 + 0.030 * static_cast<double>((base + j) % 11u);
    const double tR = 0.55 + 0.030 * static_cast<double>((base + j * 3u) % 11u);
    c.scalarL[j] = legal_value(id, tL);
    c.scalarR[j] = legal_value(id, tR);
  }
  c.scaleL = static_cast<std::uint16_t>(0x0100u + 0x0011u * ((base % 7u) + 1u));
  c.scaleR = static_cast<std::uint16_t>(0x0200u + 0x0022u * ((base % 5u) + 1u));
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    c.plateL[i] = 0.10f + 0.01f * static_cast<float>(base + i);
    c.plateR[i] = 0.60f + 0.01f * static_cast<float>(base + i);
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    c.pushL[i] = 0.20f + 0.01f * static_cast<float>(base + i);
    c.pushR[i] = 0.70f + 0.01f * static_cast<float>(base + i);
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    c.seqNoteL[i] = static_cast<std::uint8_t>((base + i) % 12u);
    c.seqNoteR[i] = static_cast<std::uint8_t>((base + i * 5u) % 12u + 12u);
    c.seqValueL[i] = 0.05f * static_cast<float>(1u + ((base + i) % 7u));
    c.seqValueR[i] = 0.05f * static_cast<float>(1u + ((base + i * 2u) % 9u));
    c.seqGateL[i] = static_cast<std::uint8_t>((base + i) % 2u);
    c.seqGateR[i] = static_cast<std::uint8_t>((base + i + 1u) % 2u);
  }
  for (std::uint32_t i = 0; i < 4u; ++i) {
    c.clockL[i] = static_cast<std::uint8_t>((base + i) % 8u);
    c.clockR[i] = static_cast<std::uint8_t>((base + 4u + i) % 8u);
  }
  return c;
}

// A render-safe Split config: derived from the power-on default so every selector a rendered
// criterion does not care about stays at its default, then the two banks are made asymmetric in
// the one NON-scalar side path the GH#12 probe already established: the left bank is chromatic
// (quantises) and the right bank is a microtonal passthrough. A 0.04 V note then reads 0.0 V on
// the left pitch (v_oct) and 0.04 V on pressure_out (the right pitch) — a discriminator that
// cannot pass if a side read the other bank, or a slot was cross-wired.
static Cfg split_discriminator_cfg() {
  Cfg c = cfg_from_state(core::make_default_device_state(kSeed));
  c.mode = 2u;         // Split: both sides live, independent banks
  c.scaleL = 0x0FFFu;  // left bank: all 12 semitones -> quantises
  c.scaleR = 0x0000u;  // right bank: no semitones -> microtonal passthrough
  return c;
}

// A render-safe Split config whose LEFT bank carries a consumed scalar (portamento speed) that
// the two-sided behaviour actually uses. variant 0 = left slow, variant 1 = left instant; the
// right bank is instant in both, so the two configs differ in exactly one consumed scalar.
static Cfg audible_split_cfg(std::uint32_t variant) {
  Cfg c = split_discriminator_cfg();
  const core::ParameterId porta = core::ParameterId::keyboard_portamento_speed;
  const std::int32_t pi = core::keyboard_scalar_index(porta);
  c.scalarL[static_cast<std::size_t>(pi)] = legal_value(porta, variant == 0u ? 1.0 : 0.0);
  c.scalarR[static_cast<std::size_t>(pi)] = legal_value(porta, 0.0);
  return c;
}

static void write_cfg(core::DeviceStateV1& st, const Cfg& c) {
  set_mode(st, c.mode);
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[j];
    set_left(st, id, c.scalarL[j]);
    set_right(st, id, c.scalarR[j]);
  }
  // pressure_output has its own canonical/mirror pair (field 9003): parameters[] is canonical,
  // keyboardSettings.pressureOutput is the mirror and must agree.
  st.keyboardSettings.pressureOutput = static_cast<std::uint8_t>(
      st.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_pressure_output)]);
  st.keyboardScaleEditor = c.scaleL;
  st.keyboardScaleEditorR = c.scaleR;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    st.keyboardPlateTune[i] = c.plateL[i];
    st.keyboardPlateTuneR[i] = c.plateR[i];
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    st.keyboardPushbutton[i] = c.pushL[i];
    st.keyboardPushbuttonR[i] = c.pushR[i];
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    st.keyboardSeqCurrent.steps[i].note = c.seqNoteL[i];
    st.keyboardSeqCurrent.steps[i].value = c.seqValueL[i];
    st.keyboardSeqCurrent.steps[i].gate = c.seqGateL[i];
    st.keyboardSeqCurrentR.steps[i].note = c.seqNoteR[i];
    st.keyboardSeqCurrentR.steps[i].value = c.seqValueR[i];
    st.keyboardSeqCurrentR.steps[i].gate = c.seqGateR[i];
  }
  for (std::uint32_t i = 0; i < 4u; ++i) {
    st.keyboardClockSelectors[i] = c.clockL[i];
    st.keyboardClockSelectorsR[i] = c.clockR[i];
  }
}

// Read the LIVE config back out of a state and compare it field-by-field with `c`. The two
// compatibility mirrors are part of "the live config is c": a recall that writes only the
// canonical side of a pair is a defect (fields 9002/9003).
static bool live_matches(const core::DeviceStateV1& st, const Cfg& c) {
  if (st.keyboardSettings.pressureBehaviour != c.mode) return false;
  if (st.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_behaviour)] !=
      static_cast<double>(c.mode))
    return false;
  const std::size_t po = static_cast<std::size_t>(
      core::keyboard_scalar_index(core::ParameterId::keyboard_pressure_output));
  if (st.keyboardSettings.pressureOutput != static_cast<std::uint8_t>(c.scalarL[po])) return false;
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[j];
    if (st.parameters[static_cast<std::size_t>(id)] != c.scalarL[j]) return false;
    if (st.keyboardScalarRight[j] != c.scalarR[j]) return false;
  }
  if (st.keyboardScaleEditor != c.scaleL || st.keyboardScaleEditorR != c.scaleR) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    if (st.keyboardPlateTune[i] != c.plateL[i]) return false;
    if (st.keyboardPlateTuneR[i] != c.plateR[i]) return false;
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    if (st.keyboardPushbutton[i] != c.pushL[i]) return false;
    if (st.keyboardPushbuttonR[i] != c.pushR[i]) return false;
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    if (st.keyboardSeqCurrent.steps[i].note != c.seqNoteL[i]) return false;
    if (st.keyboardSeqCurrent.steps[i].value != c.seqValueL[i]) return false;
    if (st.keyboardSeqCurrent.steps[i].gate != c.seqGateL[i]) return false;
    if (st.keyboardSeqCurrentR.steps[i].note != c.seqNoteR[i]) return false;
    if (st.keyboardSeqCurrentR.steps[i].value != c.seqValueR[i]) return false;
    if (st.keyboardSeqCurrentR.steps[i].gate != c.seqGateR[i]) return false;
  }
  for (std::uint32_t i = 0; i < 4u; ++i) {
    if (st.keyboardClockSelectors[i] != c.clockL[i]) return false;
    if (st.keyboardClockSelectorsR[i] != c.clockR[i]) return false;
  }
  return true;
}

// Read one preset SLOT back through the same public accessor the transfer uses.
static bool slot_matches(const core::DeviceStateV1& st, std::uint32_t slot, const Cfg& c) {
  const core::KeyboardPreset& p = st.keyboardPresets[slot];
  if (p.pressureBehaviour != c.mode) return false;
  for (std::uint32_t j = 0; j < core::kKeyboardScalarRightCount; ++j) {
    double l = 0.0, r = 0.0;
    core::read_preset_scalar_pair(p, core::kKeyboardScalarParameterIds[j], &l, &r);
    if (l != c.scalarL[j] || r != c.scalarR[j]) return false;
  }
  if (p.quantiseScaleEditor != c.scaleL || p.quantiseScaleEditorR != c.scaleR) return false;
  for (std::uint32_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    if (p.plateTune[i] != c.plateL[i] || p.plateTuneR[i] != c.plateR[i]) return false;
  }
  for (std::uint32_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    if (p.pushbuttonValue[i] != c.pushL[i] || p.pushbuttonValueR[i] != c.pushR[i]) return false;
  }
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    if (p.seqSteps.steps[i].note != c.seqNoteL[i]) return false;
    if (p.seqSteps.steps[i].value != c.seqValueL[i]) return false;
    if (p.seqSteps.steps[i].gate != c.seqGateL[i]) return false;
    if (p.seqStepsR.steps[i].note != c.seqNoteR[i]) return false;
    if (p.seqStepsR.steps[i].value != c.seqValueR[i]) return false;
    if (p.seqStepsR.steps[i].gate != c.seqGateR[i]) return false;
  }
  if (p.arpClock != c.clockL[0] || p.arpRhythm != c.clockL[1] || p.seqClock != c.clockL[2] ||
      p.seqRhythm != c.clockL[3])
    return false;
  if (p.arpClockR != c.clockR[0] || p.arpRhythmR != c.clockR[1] || p.seqClockR != c.clockR[2] ||
      p.seqRhythmR != c.clockR[3])
    return false;
  return true;
}

// ---- wire helpers ----------------------------------------------------------------------------
//
// "the other three slots are untouched" and "an unconsumed field survived" are asserted on the
// ENCODED WIRE BYTES, never on a raw struct compare: the struct carries host padding, the wire
// is the portable fixed-width record the schema declares.

static std::vector<std::uint8_t> encode_wire(const core::DeviceStateV1& st) {
  std::vector<std::uint8_t> wire(core::kDeviceStorageSchema.totalBytesHint, 0u);
  std::size_t written = 0;
  if (!core::encode_device_state(st, wire.data(), wire.size(), &written)) return {};
  wire.resize(written);
  return wire;
}

// Byte offset of a named top-level storage field, computed from the schema table (the same walk
// encode_device_state does), so no offset is hard-coded.
static std::uint32_t field_offset(const char* name) {
  std::uint32_t off = 0u;
  for (std::uint32_t i = 0; i < core::kDeviceStorageSchema.fieldCount; ++i) {
    const core::StorageField& f = core::kDeviceStorageSchema.fields[i];
    if (std::strcmp(f.name, name) == 0) return off;
    off += core::storage_field_bytes(f);
  }
  return 0xFFFFFFFFu;
}

// The wire bytes of one preset slot inside an encoded state.
static std::vector<std::uint8_t> slot_wire(const std::vector<std::uint8_t>& wire,
                                           std::uint32_t slot) {
  const std::uint32_t base = field_offset("keyboard_presets");
  if (base == 0xFFFFFFFFu || wire.size() < base) return {};
  const std::uint32_t off = base + slot * core::kKeyboardPresetRecordBytes;
  if (wire.size() < off + core::kKeyboardPresetRecordBytes) return {};
  return std::vector<std::uint8_t>(wire.begin() + off,
                                   wire.begin() + off + core::kKeyboardPresetRecordBytes);
}

// ---- event helpers (the canonical note triple InputStateMachine::translate emits) -------------

static void push(core::SynthRuntime& rt, core::ControlEventKind kind, double value,
                 core::KeyboardSide side, core::NoteId id, std::uint64_t sample) {
  core::ControlEvent e{};
  e.kind = kind;
  e.value = static_cast<core::SignalSample>(value);
  e.source = 1;
  e.channel = 0;
  e.noteId = id;
  e.producerSequence = sample;
  e.side = side;
  rt.enqueueControlEvent(core::TimedControlEvent{e, sample});
}
static void note(core::SynthRuntime& rt, core::KeyboardSide side, double pitch, double pressure,
                 core::NoteId id, std::uint64_t sample) {
  push(rt, core::ControlEventKind::pitch, pitch, side, id, sample);
  push(rt, core::ControlEventKind::pressure, pressure, side, id, sample);
  push(rt, core::ControlEventKind::gate_on, 1.0, side, id, sample);
}
static void release(core::SynthRuntime& rt, core::KeyboardSide side, core::NoteId id,
                    std::uint64_t sample) {
  push(rt, core::ControlEventKind::gate_off, 0.0, side, id, sample);
}

struct Snap {
  double vOct = 0.0, gateL = 0.0, gateR = 0.0, press = 0.0;
};
static Snap snap(const core::SynthRuntime& rt) {
  Snap s;
  s.vOct = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
  s.gateL = rt.controlVoltageAt(core::JackId::keyboard_gate_left_main_out);
  s.gateR = rt.controlVoltageAt(core::JackId::keyboard_gate_right_out);
  s.press = rt.controlVoltageAt(core::JackId::keyboard_pressure_out);
  return s;
}
static double range_of(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double lo = v[0], hi = v[0];
  for (double x : v) {
    if (x < lo) lo = x;
    if (x > hi) hi = x;
  }
  return hi - lo;
}

// The committed plan identity a rejected action must not change (device_adapter.h DevicePlan).
static std::vector<long long> plan_identity(const core::DevicePlan& p) {
  return {static_cast<long long>(p.outputCapability), static_cast<long long>(p.outputCount),
          static_cast<long long>(p.inputCapability),  static_cast<long long>(p.input),
          static_cast<long long>(p.inputCh[0]),      static_cast<long long>(p.inputCh[1]),
          static_cast<long long>(p.layout.totalChannels)};
}

// ============================================================ A. D-1: recall must be VALID

// A1/A2 use ONLY the pre-existing public surface: copy canonicalState(), call the state-layer
// transfer, re-publish through the owner's single-commit apply. On the unfixed baseline A1 is RED
// with RejectedInvalidState / field 9002 — exactly the silent no-op the D-1 report pinned.
static void a1_recall_with_differing_slot_behaviour_is_accepted() {
  std::printf("A  -- D-1: a slot whose behaviour differs from the live mirror must recall\n");
  const Cfg want = make_cfg(2u, 1u);  // mode 2 (Split) vs the default live mode 0
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, want);
  core::save_live_to_preset(scratch, 2u);
  st.keyboardPresets[2] = scratch.keyboardPresets[2];

  EngineHarness h;
  if (!h.load(st)) {
    check(false, "A1 baseline load (default live + a differing slot payload)");
    return;
  }
  core::DeviceStateV1 cand = *h.canonicalState();
  check(core::load_preset_to_live(cand, 2u), "A1 the state-layer transfer accepts slot 2");
  const auto s = h.applyCanonical(cand);
  check(s == StandaloneAudioEngine::StateApplyStatus::Accepted,
        "A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)");
  if (s != StandaloneAudioEngine::StateApplyStatus::Accepted) {
    std::printf("      apply status=%d validation.ok=%d family=%d field=%u\n",
                static_cast<int>(s), static_cast<int>(h.validation().ok),
                static_cast<int>(h.validation().family), h.validation().field);
    return;
  }
  check(live_matches(*h.canonicalState(), want),
        "A2 the recalled live config (both banks + non-scalars + selectors + BOTH sides of both "
        "compatibility mirrors) equals the slot payload");
}

// A3 is the sensitivity proof: a candidate built the OLD way (canonical behaviour written, mirror
// left stale) MUST be rejected with the exact family/field. If this ever stops firing, A1's
// assertion has lost its discriminating power and is no longer evidence.
static void a3_unsynchronised_mirror_is_rejected() {
  const Cfg want = make_cfg(1u, 1u);  // mode 1 vs the default live mode 0
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, want);
  core::save_live_to_preset(scratch, 3u);
  st.keyboardPresets[3] = scratch.keyboardPresets[3];

  EngineHarness h;
  if (!h.load(st)) {
    check(false, "A3 baseline load");
    return;
  }
  const std::vector<std::uint8_t> before = encode_wire(*h.canonicalState());
  const std::vector<long long> planBefore = plan_identity(h.plan());
  core::DeviceStateV1 cand = *h.canonicalState();
  if (!core::load_preset_to_live(cand, 3u)) {
    check(false, "A3 transfer");
    return;
  }
  // Undo ONLY the mirror, reproducing the pre-fix state exactly.
  cand.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_behaviour)] =
      static_cast<double>(st.keyboardSettings.pressureBehaviour);
  const auto s = h.applyCanonical(cand);
  check(s == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState,
        "A3 an unsynchronised behaviour mirror is REJECTED (the assertion is load-bearing)");
  check(!h.validation().ok &&
            h.validation().family == core::ValidationFamily::keyboard_live_invalid &&
            h.validation().field == 9002u,
        "A3 the rejection is keyboard_live_invalid field 9002 (the mirror pair)");
  check(h.ready() && encode_wire(*h.canonicalState()) == before &&
            plan_identity(h.plan()) == planBefore && h.sampleRate() == testengine::kSr &&
            h.blockSize() == 4096,
        "A3 the rejected apply is atomic: canonical wire state, plan and format all unchanged");
}

// ============================================================ B. engine preset actions

// B1: a not-ready engine reports it — never a false success.
static void b1_not_ready_is_reported() {
  std::printf("B  -- engine preset API: explicit results, real recall, atomic failure\n");
  StandaloneAudioEngine engine;
  check(engine.applyPresetAction(0u, StandaloneAudioEngine::PresetAction::Load) ==
            StandaloneAudioEngine::PresetActionStatus::RejectedNotReady,
        "B1 LOAD on a not-ready engine reports RejectedNotReady");
  check(engine.applyPresetAction(0u, StandaloneAudioEngine::PresetAction::Save) ==
            StandaloneAudioEngine::PresetActionStatus::RejectedNotReady,
        "B1 SAVE on a not-ready engine reports RejectedNotReady");
  check(engine.applyPresetAction(0u, StandaloneAudioEngine::PresetAction::Initialise) ==
            StandaloneAudioEngine::PresetActionStatus::RejectedNotReady,
        "B1 INITIALISE on a not-ready engine reports RejectedNotReady");
  check(!engine.isReady() && engine.canonicalState() == nullptr,
        "B1 the rejected actions did not conjure an engine state");
}

// B2/B3: illegal slot / unknown action are reported AND leave the engine bit-identical.
static void b2_b3_illegal_slot_and_action_are_atomic() {
  EngineHarness h;
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  write_cfg(st, make_cfg(1u, 0u));
  if (!h.load(st)) {
    check(false, "B2 baseline load");
    return;
  }
  const std::vector<std::uint8_t> before = encode_wire(*h.canonicalState());
  const std::vector<long long> planBefore = plan_identity(h.plan());

  check(!h.presetAction(4u, StandaloneAudioEngine::PresetAction::Load) &&
            h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::RejectedInvalidSlot,
        "B2 slot 4 (there is no 5th preset) is rejected, not silently clamped");
  check(!h.presetAction(0xFFFFu, StandaloneAudioEngine::PresetAction::Save) &&
            h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::RejectedInvalidSlot,
        "B2 an out-of-range slot is rejected");
  check(!h.presetAction(1u, static_cast<StandaloneAudioEngine::PresetAction>(7)) &&
            h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::RejectedInvalidAction,
        "B3 an unknown action value is rejected");
  check(encode_wire(*h.canonicalState()) == before,
        "B2/B3 the rejected actions left the canonical wire state byte-identical");
  check(plan_identity(h.plan()) == planBefore,
        "B2/B3 the rejected actions left the committed plan unchanged");
  check(h.ready() && h.sampleRate() == testengine::kSr && h.blockSize() == 4096,
        "B2/B3 the rejected actions left the committed format unchanged");
}

// B4: LOAD really changes the live config, and the EXISTING two-sided behaviour consumes it.
static void b4_load_changes_live_and_the_two_sides_consume_it() {
  const Cfg want = split_discriminator_cfg();
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, want);
  core::save_live_to_preset(scratch, 1u);
  st.keyboardPresets[1] = scratch.keyboardPresets[1];  // live stays the power-on default

  EngineHarness h;
  if (!h.load(st)) {
    check(false, "B4 baseline load");
    return;
  }
  check(!live_matches(*h.canonicalState(), want), "B4 the baseline live config is NOT the slot");
  check(h.presetAction(1u, StandaloneAudioEngine::PresetAction::Load),
        "B4 LOAD slot 1 is accepted");
  check(h.applyStatus() == StandaloneAudioEngine::StateApplyStatus::Accepted,
        "B4 the accepted action really committed a candidate");
  check(live_matches(*h.canonicalState(), want),
        "B4 LOAD installs the slot payload into the live config (both banks + non-scalars)");

  // The REAL two-sided behaviour: Split, left chromatic (quantises), right microtonal.
  note(*h.producerRuntime(), core::KeyboardSide::Left, 0.04, 0.5, 1, 0);
  note(*h.producerRuntime(), core::KeyboardSide::Right, 0.04, 0.5, 2, 0);
  if (!h.renderSampled(4800, 0.0, [](const core::SynthRuntime&) {})) {
    check(false, "B4 render");
    return;
  }
  const Snap s = snap(*h.runtime());
  check(near(s.vOct, 0.0, 1e-9),
        "B4 the LEFT side plays the recalled chromatic bank (0.04 V quantises to 0.0 V)");
  check(near(s.press, 0.04, 1e-9),
        "B4 the RIGHT side plays the recalled microtonal bank (0.04 V passes through)");
  check(range_of(h.out(2)) > 0.01,
        "B4 the recalled config is audible on the real host DRY channel");
}

// B5: SAVE stores the LAST SUCCESSFULLY COMMITTED canonical config, and a legitimate historical
// sequence (SAVE A -> change live -> SAVE B -> LOAD A) reads the right one. The negative control
// for this criterion must use a LEGITIMATE historical state, never an illegal-mirror pollution.
static void b5_save_reads_the_latest_committed_config() {
  const Cfg cfgA = make_cfg(0u, 0u);
  const Cfg cfgB = make_cfg(0u, 5u);

  EngineHarness h;
  core::DeviceStateV1 stA = core::make_default_device_state(kSeed);
  write_cfg(stA, cfgA);
  if (!h.load(stA)) {
    check(false, "B5 baseline load");
    return;
  }
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Save), "B5 SAVE A -> slot 0");
  check(slot_matches(*h.canonicalState(), 0u, cfgA),
        "B5 the slot now holds config A (the last committed config)");

  // A second, legitimate live config, committed through the same public entry. It is derived
  // from the COMMITTED state (so slot 0 keeps its saved payload, exactly as the wire does across
  // a device-state change) and only the live keyboard config changes.
  core::DeviceStateV1 stB = *h.canonicalState();
  write_cfg(stB, cfgB);
  if (!h.load(stB)) {
    check(false, "B5 second load");
    return;
  }
  check(h.presetAction(1u, StandaloneAudioEngine::PresetAction::Save), "B5 SAVE B -> slot 1");
  check(slot_matches(*h.canonicalState(), 1u, cfgB), "B5 slot 1 holds config B");

  // LOAD A: the recall must produce A, not the more recent B and not the original default.
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Load), "B5 LOAD slot 0");
  check(live_matches(*h.canonicalState(), cfgA),
        "B5 LOAD slot 0 restores config A (SAVE captured the committed config, not a stale copy)");
  check(!live_matches(*h.canonicalState(), cfgB), "B5 the recall is not the later config B");

  check(h.presetAction(1u, StandaloneAudioEngine::PresetAction::Load), "B5 LOAD slot 1");
  check(live_matches(*h.canonicalState(), cfgB), "B5 LOAD slot 1 restores config B");
}

// B6/B7: INITIALISE resets ONLY the target slot and never implicitly loads; a later LOAD is what
// makes it audible. SAVE/INITIALISE leave the live config content and the other slots alone.
static void b6_b7_initialise_scope_and_save_scope() {
  const Cfg live = make_cfg(2u, 3u);
  const Cfg other = make_cfg(2u, 6u);

  EngineHarness h;
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  write_cfg(st, live);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, other);
  core::save_live_to_preset(scratch, 1u);
  st.keyboardPresets[1] = scratch.keyboardPresets[1];  // slot 1: a payload we must NOT disturb
  if (!h.load(st)) {
    check(false, "B6 baseline load");
    return;
  }
  const std::vector<std::uint8_t> slot1Before = slot_wire(encode_wire(*h.canonicalState()), 1u);

  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Initialise),
        "B6 INITIALISE slot 0 is accepted");
  check(slot_matches(*h.canonicalState(), 0u, Cfg{}),
        "B6 slot 0 is back to its factory payload");
  check(live_matches(*h.canonicalState(), live),
        "B6 INITIALISE did NOT touch the live config (no implicit LOAD)");
  check(slot_wire(encode_wire(*h.canonicalState()), 1u) == slot1Before,
        "B7 INITIALISE left the other slot's wire bytes untouched");

  // A later LOAD of the initialised slot is what changes the live config — to the factory payload.
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Load), "B6 LOAD slot 0");
  check(live_matches(*h.canonicalState(), Cfg{}),
        "B6 only the subsequent LOAD applies the initialised slot to the live config");
  check(slot_wire(encode_wire(*h.canonicalState()), 1u) == slot1Before,
        "B7 the subsequent LOAD left the other slot's wire bytes untouched");

  // SAVE writes the slot and leaves the live config exactly as committed.
  const Cfg live2 = make_cfg(3u, 2u);
  core::DeviceStateV1 st2 = core::make_default_device_state(kSeed);
  write_cfg(st2, live2);
  if (!h.load(st2)) {
    check(false, "B7 second load");
    return;
  }
  const std::vector<std::uint8_t> slot0Before = slot_wire(encode_wire(*h.canonicalState()), 0u);
  check(h.presetAction(3u, StandaloneAudioEngine::PresetAction::Save), "B7 SAVE -> slot 3");
  check(slot_matches(*h.canonicalState(), 3u, live2), "B7 SAVE stored the committed live config");
  check(live_matches(*h.canonicalState(), live2), "B7 SAVE left the live config content alone");
  check(slot_wire(encode_wire(*h.canonicalState()), 0u) == slot0Before,
        "B7 SAVE left the other slot's wire bytes untouched");
}

// B8: the whole slot payload round-trips BYTE-EXACTLY through live and back — including the four
// no-domain selectors and the parse-only params (arp_length / seq_rhythm_length /
// quantise_load_scale) that have no runtime consumer yet. A field silently dropped on the recall
// is caught here even though nothing reads it.
static void b8_slot_round_trip_is_byte_exact() {
  const Cfg c = make_cfg(1u, 3u);
  EngineHarness h;
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  write_cfg(st, c);
  if (!h.load(st)) {
    check(false, "B8 baseline load");
    return;
  }
  check(h.presetAction(2u, StandaloneAudioEngine::PresetAction::Save), "B8 SAVE -> slot 2");
  const std::vector<std::uint8_t> saved = slot_wire(encode_wire(*h.canonicalState()), 2u);
  check(!saved.empty(), "B8 the saved slot is addressable in the encoded wire");

  // Wipe the LIVE config with a different one (carrying the saved slot forward, as the wire does),
  // then recall the slot and save it again.
  core::DeviceStateV1 other = *h.canonicalState();
  write_cfg(other, make_cfg(0u, 9u));
  if (!h.load(other)) {
    check(false, "B8 second load");
    return;
  }
  check(h.presetAction(2u, StandaloneAudioEngine::PresetAction::Load), "B8 LOAD slot 2");
  check(h.presetAction(2u, StandaloneAudioEngine::PresetAction::Save), "B8 re-SAVE slot 2");
  const std::vector<std::uint8_t> resaved = slot_wire(encode_wire(*h.canonicalState()), 2u);
  check(!resaved.empty() && saved == resaved,
        "B8 slot -> live -> slot is BYTE-EXACT on the wire record (unconsumed fields included)");
}

// B9: repeated identical operations are stable, and a re-commit resets the event timebase.
static void b9_repeat_and_timebase() {
  const Cfg c = split_discriminator_cfg();
  EngineHarness h;
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, c);
  core::save_live_to_preset(scratch, 0u);
  st.keyboardPresets[0] = scratch.keyboardPresets[0];
  if (!h.load(st)) {
    check(false, "B9 baseline load");
    return;
  }
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Load), "B9 first LOAD");
  const std::vector<std::uint8_t> once = encode_wire(*h.canonicalState());
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Load), "B9 second LOAD");
  check(encode_wire(*h.canonicalState()) == once,
        "B9 the same LOAD applied twice is idempotent on the wire state");

  // The re-commit resets the performance/event timebase: replaying the SAME script from the same
  // starting state produces the SAME trace after each re-commit. (The trace is compared to itself
  // across commits, never before/after within one run — a reset is not a bit-equality claim.)
  const auto script = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 130);
    release(rt, core::KeyboardSide::Left, 1, 700);
  };
  std::vector<Snap> t1, t2;
  script(*h.producerRuntime());
  if (!h.renderSampled(1200, 0.0, [&t1](const core::SynthRuntime& rt) { t1.push_back(snap(rt)); })) {
    check(false, "B9 first render");
    return;
  }
  check(h.presetAction(0u, StandaloneAudioEngine::PresetAction::Load), "B9 re-commit");
  script(*h.producerRuntime());
  if (!h.renderSampled(1200, 0.0, [&t2](const core::SynthRuntime& rt) { t2.push_back(snap(rt)); })) {
    check(false, "B9 second render");
    return;
  }
  bool same = t1.size() == t2.size();
  for (std::size_t i = 0; same && i < t1.size(); ++i) {
    same = t1[i].vOct == t2[i].vOct && t1[i].gateL == t2[i].gateL && t1[i].gateR == t2[i].gateR &&
           t1[i].press == t2[i].press;
  }
  check(same, "B9 a re-commit resets the event timebase: the replayed script is bit-identical");
  check(t1.size() == 1200u && t1[0].gateL == kGateHigh && t1[699].gateL == kGateHigh &&
            t1[700].gateL == 0.0 && t1[129].gateR == 0.0 && t1[130].gateR == kGateHigh,
        "B9 the compared trace is live (the script's notes latch and release at their frames)");
}

// B10: after a preset action the real host render is partition-invariant, and the recalled config
// is what the four output channels carry.
static void b10_partition_invariance_after_action() {
  const Cfg c = split_discriminator_cfg();
  const auto build = [&c]() {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    core::DeviceStateV1 scratch = st;
    write_cfg(scratch, c);
    core::save_live_to_preset(scratch, 3u);
    st.keyboardPresets[3] = scratch.keyboardPresets[3];
    return st;
  };
  const auto script = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 0.5, 0.7, 1, 0);
    note(rt, core::KeyboardSide::Right, 1.5, 0.3, 2, 64);
  };
  const auto silent = [](std::size_t, double& in0, double& in1) {
    in0 = 0.0;
    in1 = 0.0;
  };

  EngineHarness ref;
  if (!ref.load(build()) || !ref.presetAction(3u, StandaloneAudioEngine::PresetAction::Load)) {
    check(false, "B10 reference");
    return;
  }
  script(*ref.producerRuntime());
  std::vector<Snap> refTrace;
  if (!ref.renderSampled(512, 0.0,
                         [&refTrace](const core::SynthRuntime& rt) {
                           refTrace.push_back(snap(rt));
                         })) {
    check(false, "B10 reference render");
    return;
  }

  // 64 / 256 / irregular HOST blocks must equal the per-frame reference.
  const int sizes[3][8] = {{64, 64, 64, 64, 64, 64, 64, 64},
                           {256, 256},
                           {37, 91, 1, 200, 3, 180}};
  const int counts[3] = {8, 2, 6};
  const char* labels[3] = {"B10 64-frame host blocks equal the per-frame render",
                           "B10 256-frame host blocks equal the per-frame render",
                           "B10 irregular host blocks equal the per-frame render"};
  for (int variant = 0; variant < 3; ++variant) {
    EngineHarness h;
    if (!h.load(build()) || !h.presetAction(3u, StandaloneAudioEngine::PresetAction::Load)) {
      check(false, "B10 partition load");
      return;
    }
    script(*h.producerRuntime());
    for (int k = 0; k < counts[variant]; ++k) {
      if (!h.renderBlock(sizes[variant][k], silent)) {
        check(false, "B10 block render");
        return;
      }
    }
    if (h.out(0).size() != refTrace.size()) {
      check(false, "B10 partition produced the expected frame count");
      return;
    }
    // DRY is bit-identical across partitions (the GH#12 N1 criterion); WET is compared to a
    // 1e-12 tolerance so the criterion still fails on a real partition-dependent render.
    bool drySame = h.out(2) == ref.out(2) && h.out(3) == ref.out(3);
    bool wetSame = h.out(0).size() == ref.out(0).size() && h.out(1).size() == ref.out(1).size();
    for (std::size_t i = 0; wetSame && i < h.out(0).size(); ++i)
      wetSame = near(h.out(0)[i], ref.out(0)[i], 1e-12) && near(h.out(1)[i], ref.out(1)[i], 1e-12);
    check(drySame && wetSame, labels[variant]);
  }
  check(range_of(ref.out(2)) > 0.01 && range_of(ref.out(3)) > 0.001,
        "B10 the compared render is live on both DRY channels (not silence)");
}

// B11: a recalled CONSUMED scalar reaches the real two-sided behaviour and the host audio.
static void b11_recalled_consumed_scalar_reaches_the_audio() {
  const Cfg slow = audible_split_cfg(0u);  // left portamento slow
  const Cfg fast = audible_split_cfg(1u);  // left portamento instant

  const auto build = [](const Cfg& c, std::uint32_t slot) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    core::DeviceStateV1 scratch = st;
    write_cfg(scratch, c);
    core::save_live_to_preset(scratch, slot);
    st.keyboardPresets[slot] = scratch.keyboardPresets[slot];
    return st;
  };
  const auto play = [](EngineHarness& h, std::vector<double>* dry) {
    note(*h.producerRuntime(), core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(*h.producerRuntime(), core::KeyboardSide::Right, 1.0, 0.6, 2, 0);
    if (!h.render(4096)) return false;
    *dry = h.out(3);
    return true;
  };

  EngineHarness ha, hb;
  if (!ha.load(build(slow, 0u)) || !ha.presetAction(0u, StandaloneAudioEngine::PresetAction::Load) ||
      !hb.load(build(fast, 1u)) || !hb.presetAction(1u, StandaloneAudioEngine::PresetAction::Load)) {
    check(false, "B11 load");
    return;
  }
  std::vector<double> drySlow, dryFast;
  if (!play(ha, &drySlow) || !play(hb, &dryFast)) {
    check(false, "B11 render");
    return;
  }
  const Snap sSlow = snap(*ha.runtime());
  const Snap sFast = snap(*hb.runtime());
  check(sSlow.vOct < 0.9 && near(sFast.vOct, 1.0, 1e-6),
        "B11 the recalled LEFT portamento scalar is consumed (slow glides, instant does not)");
  check(range_of(drySlow) > 0.01 && range_of(dryFast) > 0.01,
        "B11 both recalled configs are audible (the comparison is not silence vs silence)");
  check(drySlow != dryFast,
        "B11 the recalled consumed scalar changes the real host audio, not just the control CV");
}

// D1: a downstream candidate failure AFTER a legal preset action must be REPORTED, never a false
// success, and must be ATOMIC. On the real tree the recalled candidate is legal, so the action is
// accepted — and an accepted action must really have committed the slot payload (a "success" that
// committed nothing is a defect). The failure branch is exercised by the isolated fixture mutation
// `load_produces_illegal_candidate` (tools/run_preset_engine_negatives.py --fixture), which makes
// the SAME legal call yield an illegal candidate; the paired isolated control
// `false_success_on_rejection` then reports success without a commit and this criterion goes RED.
static void d1_downstream_candidate_failure_is_reported_and_atomic() {
  const Cfg want = make_cfg(3u, 1u);
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, want);
  core::save_live_to_preset(scratch, 3u);
  st.keyboardPresets[3] = scratch.keyboardPresets[3];

  EngineHarness h;
  if (!h.load(st)) {
    check(false, "D1 baseline load");
    return;
  }
  const std::vector<std::uint8_t> wireBefore = encode_wire(*h.canonicalState());
  const std::vector<long long> planBefore = plan_identity(h.plan());
  const bool ok = h.presetAction(3u, StandaloneAudioEngine::PresetAction::Load);
  if (ok) {
    check(h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::Accepted,
          "D1 an accepted LOAD reports Accepted");
    check(live_matches(*h.canonicalState(), want),
          "D1 an accepted LOAD really committed the slot payload (no false success)");
  } else {
    check(h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::RejectedState,
          "D1 a downstream candidate failure after a legal preset action is reported as "
          "RejectedState");
    check(encode_wire(*h.canonicalState()) == wireBefore && plan_identity(h.plan()) == planBefore &&
              h.ready() && h.sampleRate() == testengine::kSr && h.blockSize() == 4096,
          "D1 the rejected preset action is atomic (canonical state/plan/format/ready unchanged)");
  }
}

int main() {
  a1_recall_with_differing_slot_behaviour_is_accepted();
  a3_unsynchronised_mirror_is_rejected();
  b1_not_ready_is_reported();
  b2_b3_illegal_slot_and_action_are_atomic();
  b4_load_changes_live_and_the_two_sides_consume_it();
  b5_save_reads_the_latest_committed_config();
  b6_b7_initialise_scope_and_save_scope();
  b8_slot_round_trip_is_byte_exact();
  b9_repeat_and_timebase();
  b10_partition_invariance_after_action();
  b11_recalled_consumed_scalar_reaches_the_audio();
  d1_downstream_candidate_failure_is_reported_and_atomic();
  if (g_fail != 0) {
    std::fprintf(stderr, "[preset engine actions] %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("[preset engine actions] %d checks OK\n", g_checks);
  return 0;
}
