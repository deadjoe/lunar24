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
// prior runtime. That is a STATE-LAYER latent defect, not a defect a user had already hit: at the
// time the APP had no preset-action caller (host/plugin.cpp builds MakeConfig(0, 0) and the
// keyboard menu is inert), so the refusal was reachable only from the state layer itself.
// A1/A2 pin the fix; A3 pins the discriminating power of the assertion itself (a hand-built
// unsynchronised candidate MUST be rejected with field 9002), so A1 can never pass vacuously.
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

// C1 (Rev-3): a render-safe payload for the slot x mode matrix. Derived from the power-on
// default so every selector a rendered criterion does not care about stays default, then the two
// banks are made asymmetric in a CONSUMED SCALAR plus the scale editor it acts on: the left bank
// is chromatic (0x0FFF, a 0.04 V plate quantises to 0.0 V whatever the root), the right bank is
// the single-note mask 0x0001 rooted at F (root_note norm 5/11 -> semitone 5, root_note_semitone
// = lround(norm*11)), so the same 0.04 V plate quantises to 5/12 V. The published pressure jack —
// left pressure in Single, right pitch in Twin/Split — therefore names WHICH bank and WHICH side
// the consumer read. Both banks get an instant portamento so the pitch settles inside the window.
static Cfg mode_matrix_cfg(std::uint8_t mode) {
  Cfg c = cfg_from_state(core::make_default_device_state(kSeed));
  c.mode = mode;
  c.scaleL = 0x0FFFu;
  c.scaleR = 0x0001u;
  const auto set = [](double* bank, core::ParameterId id, double v) {
    bank[static_cast<std::size_t>(core::keyboard_scalar_index(id))] = v;
  };
  const core::ParameterId porta = core::ParameterId::keyboard_portamento_speed;
  const core::ParameterId root = core::ParameterId::keyboard_root_note;
  set(c.scalarL, porta, legal_value(porta, 0.0));
  set(c.scalarR, porta, legal_value(porta, 0.0));
  set(c.scalarL, root, legal_value(root, 0.0));          // C
  set(c.scalarR, root, legal_value(root, 5.0 / 11.0));   // F
  return c;
}

// C2 (Rev-3): the same Single-mode payload with the LEFT scale mask parameterised — the recalled
// mask itself is the discriminator (a 0.30 V note quantises differently per mask).
static Cfg scale_probe_cfg(std::uint16_t scale_l) {
  Cfg c = mode_matrix_cfg(0u);
  c.scaleL = scale_l;
  return c;
}

// C3 (Rev-3): a Single-mode payload whose LEFT side is a 2-step sequencer running FREE
// (seq_run 0 = no plate needed) on an EXPLICIT external clock edge, with continuous CV output
// (seq_cv_output 0 = every step gated). steps[0] = 0 semitones, steps[1] = 7 semitones, so each
// clock edge alternates the published V/OCT between 0 and 7/12 V.
static Cfg seq_probe_cfg() {
  Cfg c = mode_matrix_cfg(0u);
  const auto set_l = [&c](core::ParameterId id, double v) {
    c.scalarL[static_cast<std::size_t>(core::keyboard_scalar_index(id))] = v;
  };
  set_l(core::ParameterId::keyboard_mode, 2.0);           // Sequencer (per-side arp/seq mux)
  set_l(core::ParameterId::keyboard_seq_run, 0.0);        // free-run: no held plate required
  set_l(core::ParameterId::keyboard_seq_length, 0.0);     // seq_length_steps(0) == 2 steps
  set_l(core::ParameterId::keyboard_seq_cv_output, 0.0);  // continuous: every step gated
  for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    c.seqNoteL[i] = 0u;
    c.seqGateL[i] = 1u;
    c.seqValueL[i] = 0.0f;
  }
  c.seqNoteL[0] = 0u;
  c.seqNoteL[1] = 7u;
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

// Store `c` as slot `slot` of a fresh default state whose LIVE config stays the default. The
// recall is therefore always a real change, and the only way the payload can reach the consumer
// is through the engine preset action (Rev-3: slot -> live -> consumer connectivity).
static core::DeviceStateV1 state_with_slot(const Cfg& c, std::uint32_t slot) {
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, c);
  core::save_live_to_preset(scratch, slot);
  st.keyboardPresets[slot] = scratch.keyboardPresets[slot];
  return st;
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
  const char* boundaryLabels[3] = {
      "B10 64-frame block boundaries: the two-sided control equals the per-frame reference",
      "B10 256-frame block boundaries: the two-sided control equals the per-frame reference",
      "B10 irregular block boundaries: the two-sided control equals the per-frame reference"};
  for (int variant = 0; variant < 3; ++variant) {
    EngineHarness h;
    if (!h.load(build()) || !h.presetAction(3u, StandaloneAudioEngine::PresetAction::Load)) {
      check(false, "B10 partition load");
      return;
    }
    script(*h.producerRuntime());
    // Each block is ONE real processBlock call; the published control state is sampled ONCE at
    // the END of that block. Reconcile every boundary sample with the per-frame reference at the
    // same frame — the two-sided control (v_oct / gate L / gate R / pressure) must agree, not just
    // the audio. `frameAtBoundary` is the 1-based frame count; the reference index is one less.
    std::vector<Snap> blockTrace;
    int frameAtBoundary = 0;
    bool boundarySame = true;
    for (int k = 0; k < counts[variant]; ++k) {
      if (!h.renderBlockSampled(sizes[variant][k], 0.0,
                                [&blockTrace](const core::SynthRuntime& rt) {
                                  blockTrace.push_back(snap(rt));
                                })) {
        check(false, "B10 block render");
        return;
      }
      frameAtBoundary += sizes[variant][k];
      const Snap& b = blockTrace.back();
      const Snap& r = refTrace[static_cast<std::size_t>(frameAtBoundary - 1)];
      // Gates are rails (exact 0/10 V); the continuous CVs are compared at the same 1e-12
      // tolerance the WET audio uses, which still fails on a wrong side or a wrong bank
      // (those differ by whole semitones, i.e. >= 1/12 V).
      boundarySame = boundarySame && near(b.vOct, r.vOct, 1e-12) && b.gateL == r.gateL &&
                     b.gateR == r.gateR && near(b.press, r.press, 1e-12);
    }
    check(boundarySame, boundaryLabels[variant]);
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

// ============================================================ C. the slot -> live -> consumer matrix
//
// Rev-3 (@Codex b9d8ff9f): the four slots and the three behaviour modes must be exercised through
// the ENGINE API with a real OUTPUT criterion, and at least one recalled scale and one recalled
// sequence must reach the published control — the slot -> live -> consumer chain, not just the
// state layer. Each criterion below renders through the real host block path and reads the four
// published jacks (keyboard.v_oct_out / gate_left_main_out / gate_right_out / pressure_out).

// C1: every one of the FOUR slots, recalled in each of the THREE modes, reaches the consumer with
// the mode-correct output. A right-only plate is the discriminator (pitch 0.04 V, pressure 0.5):
//   Single : the right plate is the SAME performer as left -> gate LEFT high, pressure_out = the
//            pressure stage (the plate's pressure), pitch read from BANK 0 (chromatic 0x0FFF ->
//            0.04 V quantises to 0.0 V).
//   Twin   : the sides are independent -> gate RIGHT high, pressure_out = right PITCH read from
//            BANK 0 (twin shares one bank -> chromatic, so 0.04 V quantises to 0.0 V).
//   Split  : same gating as Twin, but the right pitch reads BANK 1 (microtonal 0x0000 -> 0.04 V
//            survives).
// The three expected triples are mutually distinct (Single differs on gateL, Twin vs Split on
// pressure), and each is compared against a control harness that was never handed the action.
static void c1_four_slots_times_three_modes_reach_the_consumer() {
  std::printf("C  -- the engine API matrix: 4 slots x Single/Twin/Split reach the consumer\n");
  struct Expect {
    double gateL, gateR, press, vOct;
  };
  // Single: the merged left side publishes vOct = pitchL and pressure_out = pressL.
  // Twin: the right side reads bank 0 (left root C), so pressure_out = pitchR = 0.0.
  // Split: the right side reads bank 1 (right root F), so pressure_out = pitchR = 5/12 V —
  // the three modes are mutually distinct on purpose (no criterion can pass by mode collapse).
  const Expect expect[3] = {{kGateHigh, 0.0, 0.5, 0.0},            // Single
                            {0.0, kGateHigh, 0.0, 0.0},            // Twin
                            {0.0, kGateHigh, 5.0 / 12.0, 0.0}};    // Split
  const char* modeName[3] = {"Single", "Twin", "Split"};
  const auto play = [](EngineHarness& e) {
    note(*e.producerRuntime(), core::KeyboardSide::Right, 0.04, 0.5, 2, 0);
    return e.renderSampled(64, 0.0, [](const core::SynthRuntime&) {});
  };

  // The control is the power-on default LIVE (Single, microtonal bank 0) with the SAME plate. It
  // is never handed a preset action, so every recalled combo must differ from it somewhere — that
  // is the slot -> live -> consumer connectivity proof (the recall really changed the output).
  EngineHarness hCtrl;
  if (!hCtrl.load(core::make_default_device_state(kSeed)) || !play(hCtrl)) {
    check(false, "C1 control load/render");
    return;
  }
  const Snap ctrl = snap(*hCtrl.runtime());

  for (std::uint32_t slot = 0; slot < core::kDeviceKeyboardPresetCount; ++slot) {
    for (std::uint32_t mode = 0; mode < 3u; ++mode) {
      const Cfg payload = mode_matrix_cfg(static_cast<std::uint8_t>(mode));
      EngineHarness h;
      if (!h.load(state_with_slot(payload, slot)) ||
          !h.presetAction(slot, StandaloneAudioEngine::PresetAction::Load) ||
          !live_matches(*h.canonicalState(), payload) || !play(h)) {
        check(false, "C1 load / recall / live-config / render");
        return;
      }
      const Snap s = snap(*h.runtime());
      char label[192];
      std::snprintf(label, sizeof label,
                    "C1 slot %u x %s: the recalled payload reaches the consumer (gate L/R, "
                    "pressure, V/OCT)",
                    slot, modeName[mode]);
      check(s.gateL == expect[mode].gateL && s.gateR == expect[mode].gateR &&
                near(s.press, expect[mode].press, 1e-9) &&
                near(s.vOct, expect[mode].vOct, 1e-9),
            label);
      char diffLabel[192];
      std::snprintf(diffLabel, sizeof diffLabel,
                    "C1 slot %u x %s: the output differs from the never-recalled default live", slot,
                    modeName[mode]);
      check(s.gateL != ctrl.gateL || s.gateR != ctrl.gateR ||
                !near(s.press, ctrl.press, 1e-12) || !near(s.vOct, ctrl.vOct, 1e-12),
            diffLabel);
    }
  }
}

// C2: a recalled QUANTISER SCALE is what the published pitch runs through. A 0.30 V plate is 3.6
// semitones: the chromatic mask (0x0FFF) rounds it to 4 semitones (4/12 V), the 0x0F0F mask to 3
// (3/12 V), and the power-on microtonal mask (0x0000) passes 0.30 V through untouched. The three
// values are distinct, so the criterion cannot pass by reading the wrong bank or ignoring the
// recall.
static void c2_recalled_scale_drives_the_published_cv() {
  std::printf("C  -- a recalled quantiser scale drives the published V/OCT\n");
  const Cfg payload = scale_probe_cfg(0x0F0Fu);
  EngineHarness h, hCtrl;
  if (!h.load(state_with_slot(payload, 2u)) ||
      !hCtrl.load(core::make_default_device_state(kSeed))) {  // default live: mask 0x0000
    check(false, "C2 load");
    return;
  }
  check(h.presetAction(2u, StandaloneAudioEngine::PresetAction::Load) &&
            live_matches(*h.canonicalState(), payload),
        "C2 the recalled payload installed the 0x0F0F scale mask into the live config");
  const auto play = [](EngineHarness& e) {
    note(*e.producerRuntime(), core::KeyboardSide::Left, 0.30, 0.6, 1, 0);
    return e.renderSampled(64, 0.0, [](const core::SynthRuntime&) {});
  };
  if (!play(h) || !play(hCtrl)) {
    check(false, "C2 render");
    return;
  }
  const double recalled = snap(*h.runtime()).vOct;
  const double untouched = snap(*hCtrl.runtime()).vOct;
  // The event value travels as SignalSample (float32), so the "untouched" reference is the float32
  // of 0.30 — not a tolerance: an unquantised passthrough returns the sample EXACTLY.
  const double plate03 = static_cast<double>(static_cast<float>(0.30));
  check(near(recalled, 3.0 / 12.0, 1e-9),
        "C2 the recalled 0x0F0F mask quantises 0.30 V to 3/12 V (the slot's scale is consumed)");
  check(untouched == plate03,
        "C2 the never-recalled default mask (microtonal) passes 0.30 V through untouched");
  check(!near(recalled, untouched, 1e-6),
        "C2 the recall CHANGED the published pitch (the scale came from the slot, not the default)");
}

// C3: a recalled SEQUENCER payload is driven by an EXPLICIT external clock edge and its steps
// reach the published V/OCT. Free-run 2-step sequence (seq_run 0, seq_length 0 -> 2 steps),
// continuous CV output, steps[0] = 0 semitones and steps[1] = 7: four clock edges must publish
// 0, 7/12, 0, 7/12 V at the stable frames after each edge. The control (the same edges on the
// never-recalled default live) publishes 0 V and no gate — the sequence exists only because the
// slot was recalled through the engine API.
static void c3_recalled_seq_steps_are_driven_by_the_clock() {
  std::printf("C  -- a recalled sequencer runs on an explicit clock edge\n");
  const Cfg payload = seq_probe_cfg();
  EngineHarness h, hCtrl;
  if (!h.load(state_with_slot(payload, 1u)) ||
      !hCtrl.load(core::make_default_device_state(kSeed))) {
    check(false, "C3 load");
    return;
  }
  check(h.presetAction(1u, StandaloneAudioEngine::PresetAction::Load) &&
            live_matches(*h.canonicalState(), payload),
        "C3 the recalled payload installed the free-run 2-step sequence into the live config");

  const auto script = [](core::SynthRuntime& rt) {
    for (std::uint64_t k = 0; k < 4u; ++k)
      push(rt, core::ControlEventKind::clock, 1.0, core::KeyboardSide::Left, 0u, k * 50u);
  };
  const std::size_t probeFrames[4] = {10u, 60u, 110u, 160u};
  const double expectCv[4] = {0.0, 7.0 / 12.0, 0.0, 7.0 / 12.0};
  script(*h.producerRuntime());
  std::vector<Snap> trace;
  if (!h.renderSampled(200, 0.0,
                       [&trace](const core::SynthRuntime& rt) { trace.push_back(snap(rt)); })) {
    check(false, "C3 render");
    return;
  }
  bool pattern = true, gated = true;
  for (std::size_t k = 0; k < 4u; ++k) {
    const Snap& s = trace[probeFrames[k]];
    pattern = pattern && near(s.vOct, expectCv[k], 1e-9);
    gated = gated && s.gateL == kGateHigh;
  }
  check(pattern, "C3 each clock edge publishes the recalled step (0, 7/12, 0, 7/12 V)");
  check(gated, "C3 the recalled sequence holds the gate high while it runs");

  script(*hCtrl.producerRuntime());
  std::vector<Snap> ctrlTrace;
  if (!hCtrl.renderSampled(200, 0.0,
                           [&ctrlTrace](const core::SynthRuntime& rt) {
                             ctrlTrace.push_back(snap(rt));
                           })) {
    check(false, "C3 control render");
    return;
  }
  bool quiet = true;
  for (const Snap& s : ctrlTrace) quiet = quiet && near(s.vOct, 0.0, 1e-12) && s.gateL == 0.0;
  check(quiet,
        "C3 the same clock edges on the default live publish no note (the sequence is the slot's)");
}

// D1: a downstream candidate failure AFTER a legal preset action must be REPORTED, never a false
// success, and must be ATOMIC — where "atomic" means the OWNER THAT WAS ALREADY RUNNING is left
// exactly as it was, not merely that the canonical state/plan/format are unchanged (@Codex
// b9d8ff9f Rev-1). On the real tree the recalled candidate is legal, so the action is accepted —
// and an accepted action must really have committed the slot payload (a "success" that committed
// nothing is a defect).
//
// The failure branch is exercised by the isolated fixture mutation `load_produces_illegal_candidate`
// (tools/run_preset_engine_negatives.py), which makes the SAME legal call yield an illegal
// candidate. The owner is ALREADY RUNNING WITH A PENDING EVENT when the action is attempted, and a
// control harness with the IDENTICAL history is never handed the action: the rejected action must
// keep the same runtime OBJECT, and the full continuation (both-side control trace + all four audio
// channels) must equal that control. Paired isolated controls:
//   * `false_success_on_rejection`  -> reports success without a commit (accepted branch, RED).
//   * `failure_still_commits`       -> canonical state changes (the atomicity label, RED).
//   * `runtime_mutated_on_rejection` -> the ONE rejection path mutates the live runtime
//     (definition_->runtime().setVcoBaseHz) and the audio-preservation label goes RED, while the
//     identity / trace / liveness labels stay PASS — the control is specific, not a shotgun.
static void d1_downstream_candidate_failure_is_reported_and_atomic() {
  std::printf("D  -- a downstream candidate failure is reported AND leaves the running owner alone\n");
  const Cfg live = split_discriminator_cfg();  // Split: both sides live, so the trace is two-sided
  const Cfg want = make_cfg(3u, 1u);
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  write_cfg(st, live);
  core::DeviceStateV1 scratch = st;
  write_cfg(scratch, want);
  core::save_live_to_preset(scratch, 3u);
  st.keyboardPresets[3] = scratch.keyboardPresets[3];

  EngineHarness h, hCtrl;  // hCtrl: the SAME history, never handed the action
  if (!h.load(st) || !hCtrl.load(st)) {
    check(false, "D1 baseline load");
    return;
  }
  const std::vector<std::uint8_t> wireBefore = encode_wire(*h.canonicalState());
  const std::vector<long long> planBefore = plan_identity(h.plan());

  // Already running, with a PENDING event: the script is enqueued up front, 400 frames render, and
  // the release at frame 700 has not fired yet when the action is attempted.
  const auto script = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 130);
    release(rt, core::KeyboardSide::Left, 1, 700);
  };
  std::vector<Snap> traceH, traceC;
  script(*h.producerRuntime());
  script(*hCtrl.producerRuntime());
  if (!h.renderSampled(400, 0.0,
                       [&traceH](const core::SynthRuntime& rt) { traceH.push_back(snap(rt)); }) ||
      !hCtrl.renderSampled(400, 0.0,
                           [&traceC](const core::SynthRuntime& rt) { traceC.push_back(snap(rt)); })) {
    check(false, "D1 pre-action render");
    return;
  }
  const core::SynthRuntime* rtBefore = h.runtime();

  const bool ok = h.presetAction(3u, StandaloneAudioEngine::PresetAction::Load);
  if (ok) {
    check(h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::Accepted,
          "D1 an accepted LOAD reports Accepted");
    check(live_matches(*h.canonicalState(), want),
          "D1 an accepted LOAD really committed the slot payload (no false success)");
    return;
  }

  check(h.presetStatus() == StandaloneAudioEngine::PresetActionStatus::RejectedState,
        "D1 a downstream candidate failure after a legal preset action is reported as "
        "RejectedState");
  check(encode_wire(*h.canonicalState()) == wireBefore && plan_identity(h.plan()) == planBefore &&
            h.ready() && h.sampleRate() == testengine::kSr && h.blockSize() == 4096,
        "D1 the rejected preset action is atomic (canonical state/plan/format/ready unchanged)");
  check(h.runtime() == rtBefore,
        "D1 the rejected action keeps the SAME runtime object (the running owner is not swapped)");

  // The continuation: the pending release must still fire, and the FULL trace (pre + post) must be
  // exactly what the control produced — so a divergence in either half is caught.
  if (!h.renderSampled(800, 0.0,
                       [&traceH](const core::SynthRuntime& rt) { traceH.push_back(snap(rt)); }) ||
      !hCtrl.renderSampled(800, 0.0,
                           [&traceC](const core::SynthRuntime& rt) { traceC.push_back(snap(rt)); })) {
    check(false, "D1 post-action render");
    return;
  }
  bool sameTrace = traceH.size() == traceC.size() && traceH.size() == 1200u;
  for (std::size_t i = 0; sameTrace && i < traceH.size(); ++i) {
    sameTrace = traceH[i].vOct == traceC[i].vOct && traceH[i].gateL == traceC[i].gateL &&
                traceH[i].gateR == traceC[i].gateR && traceH[i].press == traceC[i].press;
  }
  check(sameTrace,
        "D1 the rejected action preserves the pending-event progress: the full two-sided trace "
        "equals the control that was never handed the action");
  check(traceH[0].gateL == kGateHigh && traceH[699].gateL == kGateHigh &&
            traceH[700].gateL == 0.0 && traceH[129].gateR == 0.0 && traceH[130].gateR == kGateHigh,
        "D1 the compared continuation is live on both sides (the pending release fires at its frame)");
  const bool audioSame = h.out(0) == hCtrl.out(0) && h.out(1) == hCtrl.out(1) &&
                         h.out(2) == hCtrl.out(2) && h.out(3) == hCtrl.out(3);
  check(audioSame && range_of(h.out(2)) > 0.01 && range_of(h.out(3)) > 0.001,
        "D1 the rejected action preserves all four audio channels against the control");
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
  c1_four_slots_times_three_modes_reach_the_consumer();
  c2_recalled_scale_drives_the_published_cv();
  c3_recalled_seq_steps_are_driven_by_the_clock();
  d1_downstream_candidate_failure_is_reported_and_atomic();
  if (g_fail != 0) {
    std::fprintf(stderr, "[preset engine actions] %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("[preset engine actions] %d checks OK\n", g_checks);
  return 0;
}
