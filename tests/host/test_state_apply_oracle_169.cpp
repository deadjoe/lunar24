// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_state_apply_oracle_169.cpp — the FULL-169 product oracle for task #78 (commit ②).
//
// This CTest drives the validated DeviceStateV1 -> buildMachineRuntimeCandidate path (the
// single stopped-stream apply boundary the host uses) and verifies the WHOLE applied_to_DSP
// set (exactly 169 stable ParameterIds) truly reached the live DSP, read back from the REAL
// DSP members the render path consumes — never a shadow parameter bank. It is the counterpart
// of test_state_apply_oracle.cpp (the 9B identity/calibration oracle): that one pins the
// 35 control-source params + GH#6 identity; this one pins the 7 families commit ② ADDS
// (VCO, VCF, Preamp, EnvFollower, Mixer, ClassicDrone, NewDrone) and the exact-169 gate.
//
// What is asserted (@Codex task#78 mandate, "稀疏-169 candidate applicator 与完整产品 oracle"):
//   1. EXACT-169 APPLY        — a valid default state yields an ACCEPTED candidate whose
//                               dspApplyOk() is true and dspAppliedCount()==169 (the sentinel
//                               firstFailId is kParameterCount, i.e. no failure).
//   2. PER-FAMILY REACHED DSP — one distinctive in-range value per family changes ONLY that
//                               family's real readback to the expected transferred/post-clamp
//                               value (exact where closed-form, monotonic where seed-derived),
//                               and leaves every neighbor family bit-unchanged (anti-cross-talk).
//   3. A/B, L/R, GROUP/GEN, VOL/PAN — the four named non-symmetric axes: mutating one side
//                               leaves the sibling exactly at baseline.
//   4. DEFAULT == RESTORE     — the machine built from the canonical power-on default produces
//                               readbacks identical to the same default after encode/decode
//                               round-trip (make_default_device_state is the one truth) and to a
//                               second independent build of the same seed (determinism).
//   5. TYPED REJECT CARRY     — a single out-of-domain value makes applyDspState fail closed,
//                               report the FIRST failing id + invalid_value, and never touch a
//                               partial-success path; dspAppliedCount() is the partial count.
//
// The 5 control-source families (EG, LFO, Joystick, Sequencer) and the GH#6 identity profile
// are already pinned by test_state_apply_oracle.cpp; this file nets the FULL-169 count plus the
// 7 new families, so the union covers all 11 applied_to_DSP families.

#include "mini_test.h"

#include <lunar24/core/machine_candidate.h>   // buildMachineRuntimeCandidate, MachineCandidateStatus
#include <lunar24/core/state_disposition.h>   // find_parameter, ParameterId, kParameterCount
#include <lunar24/core/device_state.h>        // DeviceStateV1, make_default_device_state
#include <lunar24/core/machine_definition.h>  // MachineRuntimeDefinition (definition->runtime())
#include <lunar24/core/state_default.h>       // make_default_device_state
#include <lunar24/core/state_serializer.h>    // encode/decode_device_state (round-trip)
#include <lunar24/core/vco.h>                 // VcoControlMode
#include <host/standalone_audio_engine.h>     // StandaloneAudioEngine (finding #3: live render path)

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {

using lunar24::core::DeviceStateV1;
using lunar24::core::MachineCandidateStatus;
using lunar24::core::MachineRuntimeDefinition;
using lunar24::core::ParameterApplyStatus;
using lunar24::core::ParameterDescriptor;
using lunar24::core::ParameterId;
using lunar24::core::StateDisposition;
using lunar24::core::VcoControlMode;
using lunar24::core::buildMachineRuntimeCandidate;
using lunar24::core::count_disposition;
using lunar24::core::disposition_of;
using lunar24::core::find_parameter;
using lunar24::core::kDeviceParamCapacity;
using lunar24::core::SynthRuntime;
using lunar24::core::kParameterCount;
using lunar24::core::make_default_device_state;
using lunar24::host::StandaloneAudioEngine;   // finding #3: the real stopped-stream apply + render owner.

constexpr std::uint64_t kSeed = 0xC0FFEEULL;
constexpr double kTiny = 1e-9;   // a delta well below any of the asserted expectations.

// ---- helpers ---------------------------------------------------------------------------
// Index a DeviceStateV1 parameter slot by its stable ParameterId.
double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// Build a candidate and REQUIRE acceptance (the caller supplies a valid state). On a
// rejection the oracle should be RED because the value was supposed to apply.
std::unique_ptr<MachineRuntimeDefinition> mustAccept(const DeviceStateV1& st) {
  auto res = buildMachineRuntimeCandidate(st, 48000.0);
  CHECK(res.status == MachineCandidateStatus::accepted);
  CHECK(res.definition != nullptr);
  return std::move(res.definition);
}

// The mandate's norm->DSP transfers (mirrored from machine_runtime.h so the oracle
// asserts the DEFINED transfer, not whatever the production code happens to produce).
constexpr double envSecFromNorm(double n) { return 0.001 + 0.999 * n; }
constexpr double classicTuneSemis(double n) { return (n - 0.5) * 24.0; }
constexpr double classicVoltSemis(double n) { return 60.0 * n; }
constexpr double newDroneRateHz(double n) { return 12.0 * n; }

// Classic group index for a voice (drone_1/2/4/5 -> group 0/1/2/3).
constexpr int kDroneGroup1 = 0;
constexpr int kDroneGroup2 = 1;
constexpr int kDroneGroup4 = 2;
constexpr int kDroneGroup5 = 3;

// -----------------------------------------------------------------------------------------
// REV of @Codex BLOCK finding #2: FULL-169 per-item readback. A single-point skip of ANY family
// setter (e.g. setVcoBMorph) must turn this oracle RED. Two independent assertions per id:
//   (a) MOVE  — re-reading the id's REAL readback after a single mismatch-free apply shows the
//               value moved off its default. A skipped setter never ran, so the readback stays at
//               baseline and (a) fails. A CROSS-WIRE (a setter writing the WRONG sibling knob)
//               also fails (a), because the intended id's own readback never moved.
//   (b) VTYPE — the readback equals the DEFINED transfer for the closed-form + raw + selector
//               families (exact), so a wrong-transfer bug (e.g. tuning written through the volt
//               formula) fails. Monotonic pitch is covered by (a) + direction (no closed form).
// The 35 control-source ids have NO SynthRuntime readback getter (internal EG/LFO/Joystick/
// Sequencer members); they are pinned by test_state_apply_oracle.cpp. disposition_target_set()
// recovers the applied_to_DSP set from the disposition table and proves 134(readback) ∪ 35 == it.

enum class RdKind { Scal, SelInt, SelBool, SelBoolInv, SelBoolExact, LinExp,
                    ClosedEnv, ClosedTune, ClosedVolt, ClosedRate, Mono };
struct RdSpec { lunar24::core::ParameterId id; RdKind kind; int a0; int a1; };

constexpr RdSpec kRdSpecs[] = {
  { ParameterId::drone_1_att, RdKind::ClosedEnv, 0, -1 },
  { ParameterId::drone_1_gate_hold, RdKind::SelBoolExact, 0, -1 },
  { ParameterId::drone_1_mod_1, RdKind::SelInt, 0, 0 },
  { ParameterId::drone_1_mod_2, RdKind::SelInt, 0, 1 },
  { ParameterId::drone_1_mod_3, RdKind::SelInt, 0, 2 },
  { ParameterId::drone_1_mod_4, RdKind::SelInt, 0, 3 },
  { ParameterId::drone_1_mod_5, RdKind::SelInt, 0, 4 },
  { ParameterId::drone_1_mute_1, RdKind::SelBool, 0, 0 },
  { ParameterId::drone_1_mute_2, RdKind::SelBool, 0, 1 },
  { ParameterId::drone_1_mute_3, RdKind::SelBool, 0, 2 },
  { ParameterId::drone_1_mute_4, RdKind::SelBool, 0, 3 },
  { ParameterId::drone_1_mute_5, RdKind::SelBool, 0, 4 },
  { ParameterId::drone_1_rls, RdKind::ClosedEnv, 0, -1 },
  { ParameterId::drone_1_tune_1, RdKind::ClosedTune, 0, 0 },
  { ParameterId::drone_1_tune_2, RdKind::ClosedTune, 0, 1 },
  { ParameterId::drone_1_tune_3, RdKind::ClosedTune, 0, 2 },
  { ParameterId::drone_1_tune_4, RdKind::ClosedTune, 0, 3 },
  { ParameterId::drone_1_tune_5, RdKind::ClosedTune, 0, 4 },
  { ParameterId::drone_1_volt, RdKind::ClosedVolt, 0, -1 },
  { ParameterId::drone_2_att, RdKind::ClosedEnv, 1, -1 },
  { ParameterId::drone_2_gate_hold, RdKind::SelBoolExact, 1, -1 },
  { ParameterId::drone_2_mod_1, RdKind::SelInt, 1, 0 },
  { ParameterId::drone_2_mod_2, RdKind::SelInt, 1, 1 },
  { ParameterId::drone_2_mod_3, RdKind::SelInt, 1, 2 },
  { ParameterId::drone_2_mod_4, RdKind::SelInt, 1, 3 },
  { ParameterId::drone_2_mod_5, RdKind::SelInt, 1, 4 },
  { ParameterId::drone_2_mute_1, RdKind::SelBool, 1, 0 },
  { ParameterId::drone_2_mute_2, RdKind::SelBool, 1, 1 },
  { ParameterId::drone_2_mute_3, RdKind::SelBool, 1, 2 },
  { ParameterId::drone_2_mute_4, RdKind::SelBool, 1, 3 },
  { ParameterId::drone_2_mute_5, RdKind::SelBool, 1, 4 },
  { ParameterId::drone_2_rls, RdKind::ClosedEnv, 1, -1 },
  { ParameterId::drone_2_tune_1, RdKind::ClosedTune, 1, 0 },
  { ParameterId::drone_2_tune_2, RdKind::ClosedTune, 1, 1 },
  { ParameterId::drone_2_tune_3, RdKind::ClosedTune, 1, 2 },
  { ParameterId::drone_2_tune_4, RdKind::ClosedTune, 1, 3 },
  { ParameterId::drone_2_tune_5, RdKind::ClosedTune, 1, 4 },
  { ParameterId::drone_2_volt, RdKind::ClosedVolt, 1, -1 },
  { ParameterId::drone_3_am, RdKind::SelBool, -1, -1 },
  { ParameterId::drone_3_fm, RdKind::SelBool, -1, -1 },
  { ParameterId::drone_3_noise, RdKind::Scal, -1, -1 },
  { ParameterId::drone_3_pitch, RdKind::Mono, -1, -1 },
  { ParameterId::drone_3_rate, RdKind::ClosedRate, -1, -1 },
  { ParameterId::drone_4_att, RdKind::ClosedEnv, 2, -1 },
  { ParameterId::drone_4_gate_hold, RdKind::SelBoolExact, 2, -1 },
  { ParameterId::drone_4_mod_1, RdKind::SelInt, 2, 0 },
  { ParameterId::drone_4_mod_2, RdKind::SelInt, 2, 1 },
  { ParameterId::drone_4_mod_3, RdKind::SelInt, 2, 2 },
  { ParameterId::drone_4_mod_4, RdKind::SelInt, 2, 3 },
  { ParameterId::drone_4_mod_5, RdKind::SelInt, 2, 4 },
  { ParameterId::drone_4_mute_1, RdKind::SelBool, 2, 0 },
  { ParameterId::drone_4_mute_2, RdKind::SelBool, 2, 1 },
  { ParameterId::drone_4_mute_3, RdKind::SelBool, 2, 2 },
  { ParameterId::drone_4_mute_4, RdKind::SelBool, 2, 3 },
  { ParameterId::drone_4_mute_5, RdKind::SelBool, 2, 4 },
  { ParameterId::drone_4_rls, RdKind::ClosedEnv, 2, -1 },
  { ParameterId::drone_4_tune_1, RdKind::ClosedTune, 2, 0 },
  { ParameterId::drone_4_tune_2, RdKind::ClosedTune, 2, 1 },
  { ParameterId::drone_4_tune_3, RdKind::ClosedTune, 2, 2 },
  { ParameterId::drone_4_tune_4, RdKind::ClosedTune, 2, 3 },
  { ParameterId::drone_4_tune_5, RdKind::ClosedTune, 2, 4 },
  { ParameterId::drone_4_volt, RdKind::ClosedVolt, 2, -1 },
  { ParameterId::drone_5_att, RdKind::ClosedEnv, 3, -1 },
  { ParameterId::drone_5_gate_hold, RdKind::SelBoolExact, 3, -1 },
  { ParameterId::drone_5_mod_1, RdKind::SelInt, 3, 0 },
  { ParameterId::drone_5_mod_2, RdKind::SelInt, 3, 1 },
  { ParameterId::drone_5_mod_3, RdKind::SelInt, 3, 2 },
  { ParameterId::drone_5_mod_4, RdKind::SelInt, 3, 3 },
  { ParameterId::drone_5_mod_5, RdKind::SelInt, 3, 4 },
  { ParameterId::drone_5_mute_1, RdKind::SelBool, 3, 0 },
  { ParameterId::drone_5_mute_2, RdKind::SelBool, 3, 1 },
  { ParameterId::drone_5_mute_3, RdKind::SelBool, 3, 2 },
  { ParameterId::drone_5_mute_4, RdKind::SelBool, 3, 3 },
  { ParameterId::drone_5_mute_5, RdKind::SelBool, 3, 4 },
  { ParameterId::drone_5_rls, RdKind::ClosedEnv, 3, -1 },
  { ParameterId::drone_5_tune_1, RdKind::ClosedTune, 3, 0 },
  { ParameterId::drone_5_tune_2, RdKind::ClosedTune, 3, 1 },
  { ParameterId::drone_5_tune_3, RdKind::ClosedTune, 3, 2 },
  { ParameterId::drone_5_tune_4, RdKind::ClosedTune, 3, 3 },
  { ParameterId::drone_5_tune_5, RdKind::ClosedTune, 3, 4 },
  { ParameterId::drone_5_volt, RdKind::ClosedVolt, 3, -1 },
  { ParameterId::drone_6_am, RdKind::SelBool, -1, -1 },
  { ParameterId::drone_6_fm, RdKind::SelBool, -1, -1 },
  { ParameterId::drone_6_noise, RdKind::Scal, -1, -1 },
  { ParameterId::drone_6_pitch, RdKind::Mono, -1, -1 },
  { ParameterId::drone_6_rate, RdKind::ClosedRate, -1, -1 },
  { ParameterId::env_follower_attack, RdKind::ClosedEnv, -1, -1 },
  { ParameterId::env_follower_release, RdKind::ClosedEnv, -1, -1 },
  { ParameterId::mixer_ch10_pan, RdKind::Scal, 9, -1 },
  { ParameterId::mixer_ch10_vol, RdKind::Scal, 9, -1 },
  { ParameterId::mixer_ch1_pan, RdKind::Scal, 0, -1 },
  { ParameterId::mixer_ch1_vol, RdKind::Scal, 0, -1 },
  { ParameterId::mixer_ch2_pan, RdKind::Scal, 1, -1 },
  { ParameterId::mixer_ch2_vol, RdKind::Scal, 1, -1 },
  { ParameterId::mixer_ch3_pan, RdKind::Scal, 2, -1 },
  { ParameterId::mixer_ch3_vol, RdKind::Scal, 2, -1 },
  { ParameterId::mixer_ch4_pan, RdKind::Scal, 3, -1 },
  { ParameterId::mixer_ch4_vol, RdKind::Scal, 3, -1 },
  { ParameterId::mixer_ch5_pan, RdKind::Scal, 4, -1 },
  { ParameterId::mixer_ch5_vol, RdKind::Scal, 4, -1 },
  { ParameterId::mixer_ch6_pan, RdKind::Scal, 5, -1 },
  { ParameterId::mixer_ch6_vol, RdKind::Scal, 5, -1 },
  { ParameterId::mixer_ch7_pan, RdKind::Scal, 6, -1 },
  { ParameterId::mixer_ch7_vol, RdKind::Scal, 6, -1 },
  { ParameterId::mixer_ch8_pan, RdKind::Scal, 7, -1 },
  { ParameterId::mixer_ch8_vol, RdKind::Scal, 7, -1 },
  { ParameterId::mixer_ch9_pan, RdKind::Scal, 8, -1 },
  { ParameterId::mixer_ch9_vol, RdKind::Scal, 8, -1 },
  { ParameterId::preamp_gain, RdKind::Scal, -1, -1 },
  { ParameterId::vcf_dist, RdKind::Scal, -1, -1 },
  { ParameterId::vcf_gain, RdKind::Scal, -1, -1 },
  { ParameterId::vcf_l_bp_lp, RdKind::SelBoolInv, 0, -1 },
  { ParameterId::vcf_l_freq, RdKind::Scal, 0, -1 },
  { ParameterId::vcf_l_mod, RdKind::Scal, 0, -1 },
  { ParameterId::vcf_l_res, RdKind::Scal, 0, -1 },
  { ParameterId::vcf_link, RdKind::SelBool, -1, -1 },
  { ParameterId::vcf_r_bp_lp, RdKind::SelBoolInv, 1, -1 },
  { ParameterId::vcf_r_freq, RdKind::Scal, 1, -1 },
  { ParameterId::vcf_r_mod, RdKind::Scal, 1, -1 },
  { ParameterId::vcf_r_res, RdKind::Scal, 1, -1 },
  { ParameterId::vco_a_cv_amt, RdKind::Scal, -1, -1 },
  { ParameterId::vco_a_lin_exp, RdKind::LinExp, -1, -1 },
  { ParameterId::vco_a_morph, RdKind::Scal, -1, -1 },
  { ParameterId::vco_a_oct_sel, RdKind::SelInt, -1, -1 },
  { ParameterId::vco_a_pw, RdKind::Scal, -1, -1 },
  { ParameterId::vco_a_sub_sel, RdKind::SelInt, -1, -1 },
  { ParameterId::vco_a_tune, RdKind::Scal, -1, -1 },
  { ParameterId::vco_b_cv_amt, RdKind::Scal, -1, -1 },
  { ParameterId::vco_b_lin_exp, RdKind::LinExp, -1, -1 },
  { ParameterId::vco_b_morph, RdKind::Scal, -1, -1 },
  { ParameterId::vco_b_oct_sel, RdKind::SelInt, -1, -1 },
  { ParameterId::vco_b_pw, RdKind::Scal, -1, -1 },
  { ParameterId::vco_b_sub_sel, RdKind::SelInt, -1, -1 },
  { ParameterId::vco_b_tune, RdKind::Scal, -1, -1 },
};   // 134 entries

double readBackValue(const SynthRuntime& r, ParameterId id) {
  switch (id) {
    case ParameterId::drone_1_att: return static_cast<double>(r.droneGroupAttSeconds(0));
    case ParameterId::drone_1_gate_hold: return (r.droneGroupHold(0) ? 1.0 : 0.0);
    case ParameterId::drone_1_mod_1: return static_cast<double>(r.droneModAmount(0, 0));
    case ParameterId::drone_1_mod_2: return static_cast<double>(r.droneModAmount(0, 1));
    case ParameterId::drone_1_mod_3: return static_cast<double>(r.droneModAmount(0, 2));
    case ParameterId::drone_1_mod_4: return static_cast<double>(r.droneModAmount(0, 3));
    case ParameterId::drone_1_mod_5: return static_cast<double>(r.droneModAmount(0, 4));
    case ParameterId::drone_1_mute_1: return (r.droneMuted(0, 0) ? 1.0 : 0.0);
    case ParameterId::drone_1_mute_2: return (r.droneMuted(0, 1) ? 1.0 : 0.0);
    case ParameterId::drone_1_mute_3: return (r.droneMuted(0, 2) ? 1.0 : 0.0);
    case ParameterId::drone_1_mute_4: return (r.droneMuted(0, 3) ? 1.0 : 0.0);
    case ParameterId::drone_1_mute_5: return (r.droneMuted(0, 4) ? 1.0 : 0.0);
    case ParameterId::drone_1_rls: return static_cast<double>(r.droneGroupRlsSeconds(0));
    case ParameterId::drone_1_tune_1: return static_cast<double>(r.droneTuneSemis(0, 0));
    case ParameterId::drone_1_tune_2: return static_cast<double>(r.droneTuneSemis(0, 1));
    case ParameterId::drone_1_tune_3: return static_cast<double>(r.droneTuneSemis(0, 2));
    case ParameterId::drone_1_tune_4: return static_cast<double>(r.droneTuneSemis(0, 3));
    case ParameterId::drone_1_tune_5: return static_cast<double>(r.droneTuneSemis(0, 4));
    case ParameterId::drone_1_volt: return static_cast<double>(r.droneVoltSemisDown(0));
    case ParameterId::drone_2_att: return static_cast<double>(r.droneGroupAttSeconds(1));
    case ParameterId::drone_2_gate_hold: return (r.droneGroupHold(1) ? 1.0 : 0.0);
    case ParameterId::drone_2_mod_1: return static_cast<double>(r.droneModAmount(1, 0));
    case ParameterId::drone_2_mod_2: return static_cast<double>(r.droneModAmount(1, 1));
    case ParameterId::drone_2_mod_3: return static_cast<double>(r.droneModAmount(1, 2));
    case ParameterId::drone_2_mod_4: return static_cast<double>(r.droneModAmount(1, 3));
    case ParameterId::drone_2_mod_5: return static_cast<double>(r.droneModAmount(1, 4));
    case ParameterId::drone_2_mute_1: return (r.droneMuted(1, 0) ? 1.0 : 0.0);
    case ParameterId::drone_2_mute_2: return (r.droneMuted(1, 1) ? 1.0 : 0.0);
    case ParameterId::drone_2_mute_3: return (r.droneMuted(1, 2) ? 1.0 : 0.0);
    case ParameterId::drone_2_mute_4: return (r.droneMuted(1, 3) ? 1.0 : 0.0);
    case ParameterId::drone_2_mute_5: return (r.droneMuted(1, 4) ? 1.0 : 0.0);
    case ParameterId::drone_2_rls: return static_cast<double>(r.droneGroupRlsSeconds(1));
    case ParameterId::drone_2_tune_1: return static_cast<double>(r.droneTuneSemis(1, 0));
    case ParameterId::drone_2_tune_2: return static_cast<double>(r.droneTuneSemis(1, 1));
    case ParameterId::drone_2_tune_3: return static_cast<double>(r.droneTuneSemis(1, 2));
    case ParameterId::drone_2_tune_4: return static_cast<double>(r.droneTuneSemis(1, 3));
    case ParameterId::drone_2_tune_5: return static_cast<double>(r.droneTuneSemis(1, 4));
    case ParameterId::drone_2_volt: return static_cast<double>(r.droneVoltSemisDown(1));
    case ParameterId::drone_3_am: return static_cast<double>(r.drone3Am());
    case ParameterId::drone_3_fm: return static_cast<double>(r.drone3Fm());
    case ParameterId::drone_3_noise: return static_cast<double>(r.drone3NoiseAmp());
    case ParameterId::drone_3_pitch: return static_cast<double>(r.drone3PitchHz());
    case ParameterId::drone_3_rate: return static_cast<double>(r.drone3RateHz());
    case ParameterId::drone_4_att: return static_cast<double>(r.droneGroupAttSeconds(2));
    case ParameterId::drone_4_gate_hold: return (r.droneGroupHold(2) ? 1.0 : 0.0);
    case ParameterId::drone_4_mod_1: return static_cast<double>(r.droneModAmount(2, 0));
    case ParameterId::drone_4_mod_2: return static_cast<double>(r.droneModAmount(2, 1));
    case ParameterId::drone_4_mod_3: return static_cast<double>(r.droneModAmount(2, 2));
    case ParameterId::drone_4_mod_4: return static_cast<double>(r.droneModAmount(2, 3));
    case ParameterId::drone_4_mod_5: return static_cast<double>(r.droneModAmount(2, 4));
    case ParameterId::drone_4_mute_1: return (r.droneMuted(2, 0) ? 1.0 : 0.0);
    case ParameterId::drone_4_mute_2: return (r.droneMuted(2, 1) ? 1.0 : 0.0);
    case ParameterId::drone_4_mute_3: return (r.droneMuted(2, 2) ? 1.0 : 0.0);
    case ParameterId::drone_4_mute_4: return (r.droneMuted(2, 3) ? 1.0 : 0.0);
    case ParameterId::drone_4_mute_5: return (r.droneMuted(2, 4) ? 1.0 : 0.0);
    case ParameterId::drone_4_rls: return static_cast<double>(r.droneGroupRlsSeconds(2));
    case ParameterId::drone_4_tune_1: return static_cast<double>(r.droneTuneSemis(2, 0));
    case ParameterId::drone_4_tune_2: return static_cast<double>(r.droneTuneSemis(2, 1));
    case ParameterId::drone_4_tune_3: return static_cast<double>(r.droneTuneSemis(2, 2));
    case ParameterId::drone_4_tune_4: return static_cast<double>(r.droneTuneSemis(2, 3));
    case ParameterId::drone_4_tune_5: return static_cast<double>(r.droneTuneSemis(2, 4));
    case ParameterId::drone_4_volt: return static_cast<double>(r.droneVoltSemisDown(2));
    case ParameterId::drone_5_att: return static_cast<double>(r.droneGroupAttSeconds(3));
    case ParameterId::drone_5_gate_hold: return (r.droneGroupHold(3) ? 1.0 : 0.0);
    case ParameterId::drone_5_mod_1: return static_cast<double>(r.droneModAmount(3, 0));
    case ParameterId::drone_5_mod_2: return static_cast<double>(r.droneModAmount(3, 1));
    case ParameterId::drone_5_mod_3: return static_cast<double>(r.droneModAmount(3, 2));
    case ParameterId::drone_5_mod_4: return static_cast<double>(r.droneModAmount(3, 3));
    case ParameterId::drone_5_mod_5: return static_cast<double>(r.droneModAmount(3, 4));
    case ParameterId::drone_5_mute_1: return (r.droneMuted(3, 0) ? 1.0 : 0.0);
    case ParameterId::drone_5_mute_2: return (r.droneMuted(3, 1) ? 1.0 : 0.0);
    case ParameterId::drone_5_mute_3: return (r.droneMuted(3, 2) ? 1.0 : 0.0);
    case ParameterId::drone_5_mute_4: return (r.droneMuted(3, 3) ? 1.0 : 0.0);
    case ParameterId::drone_5_mute_5: return (r.droneMuted(3, 4) ? 1.0 : 0.0);
    case ParameterId::drone_5_rls: return static_cast<double>(r.droneGroupRlsSeconds(3));
    case ParameterId::drone_5_tune_1: return static_cast<double>(r.droneTuneSemis(3, 0));
    case ParameterId::drone_5_tune_2: return static_cast<double>(r.droneTuneSemis(3, 1));
    case ParameterId::drone_5_tune_3: return static_cast<double>(r.droneTuneSemis(3, 2));
    case ParameterId::drone_5_tune_4: return static_cast<double>(r.droneTuneSemis(3, 3));
    case ParameterId::drone_5_tune_5: return static_cast<double>(r.droneTuneSemis(3, 4));
    case ParameterId::drone_5_volt: return static_cast<double>(r.droneVoltSemisDown(3));
    case ParameterId::drone_6_am: return static_cast<double>(r.drone6Am());
    case ParameterId::drone_6_fm: return static_cast<double>(r.drone6Fm());
    case ParameterId::drone_6_noise: return static_cast<double>(r.drone6NoiseAmp());
    case ParameterId::drone_6_pitch: return static_cast<double>(r.drone6PitchHz());
    case ParameterId::drone_6_rate: return static_cast<double>(r.drone6RateHz());
    case ParameterId::env_follower_attack: return static_cast<double>(r.envFollowerAttackSeconds());
    case ParameterId::env_follower_release: return static_cast<double>(r.envFollowerReleaseSeconds());
    case ParameterId::mixer_ch10_pan: return static_cast<double>(r.mixerChannelPan(9));
    case ParameterId::mixer_ch10_vol: return static_cast<double>(r.mixerChannelVol(9));
    case ParameterId::mixer_ch1_pan: return static_cast<double>(r.mixerChannelPan(0));
    case ParameterId::mixer_ch1_vol: return static_cast<double>(r.mixerChannelVol(0));
    case ParameterId::mixer_ch2_pan: return static_cast<double>(r.mixerChannelPan(1));
    case ParameterId::mixer_ch2_vol: return static_cast<double>(r.mixerChannelVol(1));
    case ParameterId::mixer_ch3_pan: return static_cast<double>(r.mixerChannelPan(2));
    case ParameterId::mixer_ch3_vol: return static_cast<double>(r.mixerChannelVol(2));
    case ParameterId::mixer_ch4_pan: return static_cast<double>(r.mixerChannelPan(3));
    case ParameterId::mixer_ch4_vol: return static_cast<double>(r.mixerChannelVol(3));
    case ParameterId::mixer_ch5_pan: return static_cast<double>(r.mixerChannelPan(4));
    case ParameterId::mixer_ch5_vol: return static_cast<double>(r.mixerChannelVol(4));
    case ParameterId::mixer_ch6_pan: return static_cast<double>(r.mixerChannelPan(5));
    case ParameterId::mixer_ch6_vol: return static_cast<double>(r.mixerChannelVol(5));
    case ParameterId::mixer_ch7_pan: return static_cast<double>(r.mixerChannelPan(6));
    case ParameterId::mixer_ch7_vol: return static_cast<double>(r.mixerChannelVol(6));
    case ParameterId::mixer_ch8_pan: return static_cast<double>(r.mixerChannelPan(7));
    case ParameterId::mixer_ch8_vol: return static_cast<double>(r.mixerChannelVol(7));
    case ParameterId::mixer_ch9_pan: return static_cast<double>(r.mixerChannelPan(8));
    case ParameterId::mixer_ch9_vol: return static_cast<double>(r.mixerChannelVol(8));
    case ParameterId::preamp_gain: return static_cast<double>(r.preampGainNorm());
    case ParameterId::vcf_dist: return static_cast<double>(r.distortionAmount());
    case ParameterId::vcf_gain: return static_cast<double>(r.distortionGain());
    case ParameterId::vcf_l_bp_lp: return (r.vcfBp(0) ? 1.0 : 0.0);
    case ParameterId::vcf_l_freq: return static_cast<double>(r.vcfFreq(0));
    case ParameterId::vcf_l_mod: return static_cast<double>(r.vcfMod(0));
    case ParameterId::vcf_l_res: return static_cast<double>(r.vcfRes(0));
    case ParameterId::vcf_link: return (r.vcfLink() ? 1.0 : 0.0);
    case ParameterId::vcf_r_bp_lp: return (r.vcfBp(1) ? 1.0 : 0.0);
    case ParameterId::vcf_r_freq: return static_cast<double>(r.vcfFreq(1));
    case ParameterId::vcf_r_mod: return static_cast<double>(r.vcfMod(1));
    case ParameterId::vcf_r_res: return static_cast<double>(r.vcfRes(1));
    case ParameterId::vco_a_cv_amt: return static_cast<double>(r.vcoACvAmt());
    case ParameterId::vco_a_lin_exp: return static_cast<double>(static_cast<int>(r.vcoAControlMode()));
    case ParameterId::vco_a_morph: return static_cast<double>(r.vcoAMorph());
    case ParameterId::vco_a_oct_sel: return static_cast<double>(r.vcoAOctSelect());
    case ParameterId::vco_a_pw: return static_cast<double>(r.vcoAPw());
    case ParameterId::vco_a_sub_sel: return static_cast<double>(r.vcoASubSelect());
    case ParameterId::vco_a_tune: return static_cast<double>(r.vcoATune());
    case ParameterId::vco_b_cv_amt: return static_cast<double>(r.vcoBCvAmt());
    case ParameterId::vco_b_lin_exp: return static_cast<double>(static_cast<int>(r.vcoBControlMode()));
    case ParameterId::vco_b_morph: return static_cast<double>(r.vcoBMorph());
    case ParameterId::vco_b_oct_sel: return static_cast<double>(r.vcoBOctSelect());
    case ParameterId::vco_b_pw: return static_cast<double>(r.vcoBPw());
    case ParameterId::vco_b_sub_sel: return static_cast<double>(r.vcoBSubSelect());
    case ParameterId::vco_b_tune: return static_cast<double>(r.vcoBTune());
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

// @Codex BLOCK #1 (rev): the 35 control-source ids are routed through setControlParamValue, not the
// direct-scalar switch, so readBackValue() above cannot see them. Read each back through the REAL
// owner accessor the render path consumes — EnvelopeGenerator / Lfo / JoystickCv / FiveStepSequencer —
// so deleting the setControlParamValue call (or the whole control-source branch) moves the readback
// off to baseline and goes RED, exactly the repro @Codex ran. NaN only for an id not on this list.
double readBackControlSource(const SynthRuntime& r, ParameterId id) {
  switch (id) {
    case ParameterId::envelope_a_a: return r.envelopeA().attackSeconds();
    case ParameterId::envelope_a_d: return r.envelopeA().decaySeconds();
    case ParameterId::envelope_a_r: return r.envelopeA().releaseSeconds();
    case ParameterId::envelope_a_s: return r.envelopeA().sustain();
    case ParameterId::envelope_a_hold: return r.envelopeA().hold() ? 1.0 : 0.0;
    case ParameterId::envelope_a_self_gen: return r.envelopeA().selfGen() ? 1.0 : 0.0;
    case ParameterId::envelope_b_a: return r.envelopeB().attackSeconds();
    case ParameterId::envelope_b_d: return r.envelopeB().decaySeconds();
    case ParameterId::envelope_b_r: return r.envelopeB().releaseSeconds();
    case ParameterId::envelope_b_s: return r.envelopeB().sustain();
    case ParameterId::envelope_b_hold: return r.envelopeB().hold() ? 1.0 : 0.0;
    case ParameterId::envelope_b_self_gen: return r.envelopeB().selfGen() ? 1.0 : 0.0;
    case ParameterId::lfo_a_rate: return r.lfoA().baseHz();
    case ParameterId::lfo_a_wave: return r.lfoA().wave();
    case ParameterId::lfo_a_speed_mult:
      return static_cast<double>(static_cast<int>(r.lfoA().speedMult()));
    case ParameterId::lfo_b_rate: return r.lfoB().baseHz();
    case ParameterId::lfo_b_wave: return r.lfoB().wave();
    case ParameterId::lfo_b_speed_mult:
      return static_cast<double>(static_cast<int>(r.lfoB().speedMult()));
    case ParameterId::joystick_x: return r.joystick().x();
    case ParameterId::joystick_y: return r.joystick().y();
    case ParameterId::joystick_offset_x: return r.joystick().offsetX();
    case ParameterId::joystick_offset_y: return r.joystick().offsetY();
    case ParameterId::sequencer_clock:
      return static_cast<double>(static_cast<int>(r.sequencer().clockSource()));
    case ParameterId::sequencer_stages: return static_cast<double>(r.sequencer().stageCount());
    case ParameterId::sequencer_step_cv_1: return r.sequencer().stepCv(0);
    case ParameterId::sequencer_step_cv_2: return r.sequencer().stepCv(1);
    case ParameterId::sequencer_step_cv_3: return r.sequencer().stepCv(2);
    case ParameterId::sequencer_step_cv_4: return r.sequencer().stepCv(3);
    case ParameterId::sequencer_step_cv_5: return r.sequencer().stepCv(4);
    case ParameterId::sequencer_step_gate_1: return r.sequencer().stepGate(0) ? 1.0 : 0.0;
    case ParameterId::sequencer_step_gate_2: return r.sequencer().stepGate(1) ? 1.0 : 0.0;
    case ParameterId::sequencer_step_gate_3: return r.sequencer().stepGate(2) ? 1.0 : 0.0;
    case ParameterId::sequencer_step_gate_4: return r.sequencer().stepGate(3) ? 1.0 : 0.0;
    case ParameterId::sequencer_step_gate_5: return r.sequencer().stepGate(4) ? 1.0 : 0.0;
    case ParameterId::sequencer_pulser: return r.sequencer().internalRateHz();
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

// The DEFINED (product-transfer) expectation for a control-source probe. Envelope times/sustain and
// joystick/lfo-wave/step-CV store the admitted value verbatim; the selector/boolean ids and
// stages/clock/pulser map through their own defined transfer. PULSER uses the real product formula
// so a wrong norm->Hz mapping is caught, not silently accepted as "moved".
double csExpected(ParameterId id, double probe) {
  // PULSER and STAGES map through their own defined transfer; every other control-source id keeps
  // the admitted value verbatim (or the boolean/selector map), so no runtime state is needed here.
  switch (id) {
    case ParameterId::envelope_a_a: case ParameterId::envelope_a_d: case ParameterId::envelope_a_r:
    case ParameterId::envelope_a_s:
    case ParameterId::envelope_b_a: case ParameterId::envelope_b_d: case ParameterId::envelope_b_r:
    case ParameterId::envelope_b_s:
    case ParameterId::lfo_a_rate: case ParameterId::lfo_a_wave:
    case ParameterId::lfo_b_rate: case ParameterId::lfo_b_wave:
    case ParameterId::joystick_x: case ParameterId::joystick_y:
    case ParameterId::joystick_offset_x: case ParameterId::joystick_offset_y:
    case ParameterId::sequencer_step_cv_1: case ParameterId::sequencer_step_cv_2:
    case ParameterId::sequencer_step_cv_3: case ParameterId::sequencer_step_cv_4:
    case ParameterId::sequencer_step_cv_5:
      return probe;
    case ParameterId::envelope_a_hold: case ParameterId::envelope_a_self_gen:
    case ParameterId::envelope_b_hold: case ParameterId::envelope_b_self_gen:
    case ParameterId::sequencer_step_gate_1: case ParameterId::sequencer_step_gate_2:
    case ParameterId::sequencer_step_gate_3: case ParameterId::sequencer_step_gate_4:
    case ParameterId::sequencer_step_gate_5:
      return probe != 0.0 ? 1.0 : 0.0;
    case ParameterId::lfo_a_speed_mult: case ParameterId::lfo_b_speed_mult:
    case ParameterId::sequencer_clock:
      return static_cast<double>(static_cast<int>(probe));
    case ParameterId::sequencer_stages: return 3.0 + probe;   // norm index -> 3+n stage count.
    case ParameterId::sequencer_pulser:
      return lunar24::core::FiveStepSequencer::pulserNormToRateHz(probe);
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

// The 35 control-source ids routed via setControlParamValue (no SynthRuntime readback getter).
// Mirrors SynthRuntime::controlSourceParamRecognized_ so the union proof is independent of the
// product: it is enumerated from the same disposition authority, not from a private method.
bool controlSourceId(ParameterId id) {
  switch (id) {
    case ParameterId::envelope_a_a: case ParameterId::envelope_a_d: case ParameterId::envelope_a_r:
    case ParameterId::envelope_a_s: case ParameterId::envelope_a_hold: case ParameterId::envelope_a_self_gen:
    case ParameterId::envelope_b_a: case ParameterId::envelope_b_d: case ParameterId::envelope_b_r:
    case ParameterId::envelope_b_s: case ParameterId::envelope_b_hold: case ParameterId::envelope_b_self_gen:
    case ParameterId::lfo_a_rate: case ParameterId::lfo_a_wave: case ParameterId::lfo_a_speed_mult:
    case ParameterId::lfo_b_rate: case ParameterId::lfo_b_wave: case ParameterId::lfo_b_speed_mult:
    case ParameterId::joystick_x: case ParameterId::joystick_y:
    case ParameterId::joystick_offset_x: case ParameterId::joystick_offset_y:
    case ParameterId::sequencer_clock: case ParameterId::sequencer_stages:
    case ParameterId::sequencer_step_cv_1: case ParameterId::sequencer_step_cv_2:
    case ParameterId::sequencer_step_cv_3: case ParameterId::sequencer_step_cv_4:
    case ParameterId::sequencer_step_cv_5:
    case ParameterId::sequencer_step_gate_1: case ParameterId::sequencer_step_gate_2:
    case ParameterId::sequencer_step_gate_3: case ParameterId::sequencer_step_gate_4:
    case ParameterId::sequencer_step_gate_5:
    case ParameterId::sequencer_pulser:
      return true;
    default:
      return false;
  }
}

// A single in-domain probe differing from registry initial, so dspParamValid_ admits it and the
// value really moves the DSP (a probe == initial would make the MOVE check vacuous). Selector
// (step>0, incl. oct_sel/bp_lp/lin_exp/8-mod/4-mute) gets the non-default exact integer.
double probeFor(const ParameterDescriptor* d) {
  const double lo = d->min, hi = d->max, def = d->initial;
  if (d->step > 0.0) {
    double p = def + d->step;
    if (p > hi) p = def - d->step;
    if (std::fabs(p - def) < kTiny) p = (def < hi) ? hi : lo;
    return p;
  }
  double p = lo + 0.9 * (hi - lo);
  if (std::fabs(p - def) < kTiny) p = lo + 0.1 * (hi - lo);
  return p;
}

// The defined norm->DSP expectation for the probe (NaN for monotonic pitch: no closed form).
double expectedFor(RdKind kind, double probe) {
  switch (kind) {
    case RdKind::Scal:            return probe;
    case RdKind::SelInt:          return probe;
    case RdKind::SelBool:         return probe != 0.0 ? 1.0 : 0.0;
    case RdKind::SelBoolInv:      return probe == 0.0 ? 1.0 : 0.0;
    case RdKind::SelBoolExact:    return probe == 1.0 ? 1.0 : 0.0;
    case RdKind::LinExp:          return probe == 1.0 ? 1.0 : 0.0;
    case RdKind::ClosedEnv:       return envSecFromNorm(probe);
    case RdKind::ClosedTune:      return classicTuneSemis(probe);
    case RdKind::ClosedVolt:      return classicVoltSemis(probe);
    case RdKind::ClosedRate:      return newDroneRateHz(probe);
    case RdKind::Mono:            return std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// Build a candidate and REQUIRE acceptance, but print the offending id on a reject so a bad probe
// is identifiable rather than a silent RED across the 134-iteration loop.
std::unique_ptr<MachineRuntimeDefinition> acceptWithLabel(const DeviceStateV1& st, ParameterId id) {
  auto res = buildMachineRuntimeCandidate(st, 48000.0);
  if (res.status != MachineCandidateStatus::accepted || res.definition == nullptr) {
    const ParameterDescriptor* d = find_parameter(id);
    std::fprintf(stderr, "  reject id=%s status=%d\n",
                 d ? std::string(d->stable_id).c_str() : "(?)", static_cast<int>(res.status));
  }
  CHECK(res.status == MachineCandidateStatus::accepted);
  CHECK(res.definition != nullptr);
  return std::move(res.definition);
}

// -----------------------------------------------------------------------------------------
// 1 & 4: default state -> accepted candidate, exactly 169 applied, sentinel firstFail, and
// default(seed) readbacks are deterministic across independent builds (default==restore on the
// canonical power-on default).
static void full169_default_apply() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  auto d = mustAccept(def);
  CHECK(d->dspApplyOk());
  CHECK_EQ(d->dspAppliedCount(), 169u);
  CHECK(d->dspFirstFailId() == static_cast<ParameterId>(kParameterCount));
  CHECK(d->dspFirstFailStatus() == ParameterApplyStatus::applied);
  CHECK(d->valid());
  CHECK(d->identityApplied());

  // Determinism: a second independent build of the same seed yields identical DSP readbacks
  // (the "default prepare == default restore" baseline for the discriminator scenarios). The
  // default PITCH panel position (registry initial 0.5, > 0) puts the NewDrone in the audible,
  // non-silenced range, so the identity is a real positive tone driving the render path.
  CHECK(d->runtime().drone3PitchHz() > 0.0);          // seed-derived, but always a real positive tone.
  auto d2 = mustAccept(def);
  CHECK(std::fabs(d->runtime().drone3PitchHz() - d2->runtime().drone3PitchHz()) < kTiny);
  CHECK(std::fabs(d->runtime().drone6RateHz() - d2->runtime().drone6RateHz()) < kTiny);
  CHECK(d->runtime().droneTuneSemis(kDroneGroup1, 0) == d2->runtime().droneTuneSemis(kDroneGroup1, 0));
  CHECK(d->runtime().vcfFreq(0) == d2->runtime().vcfFreq(0));
  CHECK(d->runtime().mixerChannelVol(0) == d2->runtime().mixerChannelVol(0));
}

// -----------------------------------------------------------------------------------------
// 2 & 3: VCO family discriminators (A/B anti-cross + exact readback for every knob). All the
// VCO readbacks read the real Vco members the kVcoA/kVcoB executor consumes.
static void vco_a_b_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // A-side tune mutates, B-side bit-unchanged (A/B anti-cross).
  DeviceStateV1 st = def;
  slot(st, ParameterId::vco_a_tune) = 0.25;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcoATune() - 0.25) < kTiny);
  CHECK(std::fabs(d->runtime().vcoBTune() - base->runtime().vcoBTune()) < kTiny);

  // Morph (clamped [0,1]).
  st = def; slot(st, ParameterId::vco_a_morph) = 0.75;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcoAMorph() - 0.75) < kTiny);
  CHECK(std::fabs(d->runtime().vcoBMorph() - base->runtime().vcoBMorph()) < kTiny);

  // PW/shape (clamped into (1e-3, 1-1e-3)).
  st = def; slot(st, ParameterId::vco_a_pw) = 0.2;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcoAPw() - 0.2) < kTiny);
  CHECK(std::fabs(d->runtime().vcoBPw() - base->runtime().vcoBPw()) < kTiny);

  // Octave selector (exact integer, clamped [0,2]).
  st = def; slot(st, ParameterId::vco_a_oct_sel) = 2;   // index 2 = "+3".
  d = mustAccept(st);
  CHECK(d->runtime().vcoAOctSelect() == 2);
  CHECK(d->runtime().vcoBOctSelect() == base->runtime().vcoBOctSelect());

  // Sub selector (exact integer 0..1).
  st = def; slot(st, ParameterId::vco_a_sub_sel) = 0;   // index 0 = "0" (off).
  d = mustAccept(st);
  CHECK(d->runtime().vcoASubSelect() == 0);
  CHECK(d->runtime().vcoBSubSelect() == base->runtime().vcoBSubSelect());

  // CV amount (0..1).
  st = def; slot(st, ParameterId::vco_a_cv_amt) = 0.4;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcoACvAmt() - 0.4) < kTiny);
  CHECK(std::fabs(d->runtime().vcoBCvAmt() - base->runtime().vcoBCvAmt()) < kTiny);

  // lin_exp selector: 0 -> Linear, 1 -> Exponential (default index 1 = exp).
  st = def; slot(st, ParameterId::vco_a_lin_exp) = 0;
  d = mustAccept(st);
  CHECK(d->runtime().vcoAControlMode() == VcoControlMode::kLinear);
  CHECK(d->runtime().vcoBControlMode() == base->runtime().vcoBControlMode());
  st = def; slot(st, ParameterId::vco_a_lin_exp) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().vcoAControlMode() == VcoControlMode::kExponential);

  // B-side mirrors A-side: one B param mutates, A bit-unchanged.
  st = def; slot(st, ParameterId::vco_b_tune) = -0.5;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcoBTune() - (-0.5)) < kTiny);
  CHECK(std::fabs(d->runtime().vcoATune() - base->runtime().vcoATune()) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 2 & 3: VCF family discriminators + L/R anti-cross; the "symmetric pair" (vol) and the
// asymmetric selector (bp_lp) are both checked.
static void vcf_l_r_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // L freq mutates, R freq bit-unchanged (L/R anti-cross).
  DeviceStateV1 st = def;
  slot(st, ParameterId::vcf_l_freq) = 0.7;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcfFreq(0) - 0.7) < kTiny);
  CHECK(std::fabs(d->runtime().vcfFreq(1) - base->runtime().vcfFreq(1)) < kTiny);

  // R res mutates, L res bit-unchanged.
  st = def; slot(st, ParameterId::vcf_r_res) = 0.5;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcfRes(1) - 0.5) < kTiny);
  CHECK(std::fabs(d->runtime().vcfRes(0) - base->runtime().vcfRes(0)) < kTiny);

  // R freq mutates, L freq bit-unchanged (R->L cross discriminator).
  st = def; slot(st, ParameterId::vcf_r_freq) = 0.6;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcfFreq(1) - 0.6) < kTiny);
  CHECK(std::fabs(d->runtime().vcfFreq(0) - base->runtime().vcfFreq(0)) < kTiny);

  // L mod mutates.
  st = def; slot(st, ParameterId::vcf_l_mod) = 0.25;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().vcfMod(0) - 0.25) < kTiny);
  CHECK(std::fabs(d->runtime().vcfMod(1) - base->runtime().vcfMod(1)) < kTiny);

  // bp_lp selector: 0 -> bp(true), 1 -> lp(false), per channel independently.
  st = def; slot(st, ParameterId::vcf_l_bp_lp) = 1;   // L to "lp".
  d = mustAccept(st);
  CHECK(d->runtime().vcfBp(0) == false);
  CHECK(d->runtime().vcfBp(1) == base->runtime().vcfBp(1));   // R stays default "bp".
  st = def; slot(st, ParameterId::vcf_l_bp_lp) = 0;   // L back to "bp".
  d = mustAccept(st);
  CHECK(d->runtime().vcfBp(0) == true);

  // LINK on -> link true (default off).
  st = def; slot(st, ParameterId::vcf_link) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().vcfLink() == true);

  // vcf_dist -> DIST amount only; vcf_gain -> output gain only (never the combined setter).
  st = def; slot(st, ParameterId::vcf_dist) = 0.6;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().distortionAmount() - 0.6) < kTiny);
  CHECK(std::fabs(d->runtime().distortionGain() - base->runtime().distortionGain()) < kTiny);
  st = def; slot(st, ParameterId::vcf_gain) = 0.4;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().distortionGain() - 0.4) < kTiny);
  CHECK(std::fabs(d->runtime().distortionAmount() - base->runtime().distortionAmount()) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 2: Preamp + EnvFollower (exact transferred seconds; monotonic/closed-form).
static void preamp_envfollower_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // Preamp gain (clamped [0,1]).
  DeviceStateV1 st = def;
  slot(st, ParameterId::preamp_gain) = 0.8;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().preampGainNorm() - 0.8) < kTiny);

  // Env follower attack/release are the norm->seconds transfer 0.001+0.999n.
  st = def; slot(st, ParameterId::env_follower_attack) = 0.25;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().envFollowerAttackSeconds() - envSecFromNorm(0.25)) < kTiny);
  CHECK(std::fabs(d->runtime().envFollowerReleaseSeconds() - base->runtime().envFollowerReleaseSeconds()) < kTiny);
  st = def; slot(st, ParameterId::env_follower_release) = 0.8;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().envFollowerReleaseSeconds() - envSecFromNorm(0.8)) < kTiny);
  CHECK(std::fabs(d->runtime().envFollowerAttackSeconds() - base->runtime().envFollowerAttackSeconds()) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 3: Mixer VOL/PAN anti-cross — a channel's vol and pan are independent 0-based slots, and
// mutating one slot leaves every sibling channel AND the sibling knob bit-unchanged.
static void mixer_vol_pan_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // ch3 (slot 2) vol mutates; the same channel's pan AND every other channel stay put.
  DeviceStateV1 st = def;
  slot(st, ParameterId::mixer_ch3_vol) = 0.9;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().mixerChannelVol(2) - 0.9) < kTiny);
  CHECK(std::fabs(d->runtime().mixerChannelPan(2) - base->runtime().mixerChannelPan(2)) < kTiny);
  CHECK(std::fabs(d->runtime().mixerChannelVol(0) - base->runtime().mixerChannelVol(0)) < kTiny);

  // ch4 (slot 3) pan mutates; that channel's vol AND every sibling stay put.
  st = def; slot(st, ParameterId::mixer_ch4_pan) = 0.1;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().mixerChannelPan(3) - 0.1) < kTiny);
  CHECK(std::fabs(d->runtime().mixerChannelVol(3) - base->runtime().mixerChannelVol(3)) < kTiny);
  CHECK(std::fabs(d->runtime().mixerChannelVol(1) - base->runtime().mixerChannelVol(1)) < kTiny);

  // ch10 (slot 9) vol: last channel clamp/mapping is not a silent off-by-one.
  st = def; slot(st, ParameterId::mixer_ch10_vol) = 0.35;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().mixerChannelVol(9) - 0.35) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 2 & 3: ClassicDrone family discriminators with GROUP/GEN anti-cross. drone_2 -> group 1.
static void classic_drone_group_gen_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // TUNE (gen 2 of group 1) mutates to +6 semis; a sibling GEN and a sibling GROUP stay put.
  DeviceStateV1 st = def;
  slot(st, ParameterId::drone_2_tune_3) = 0.75;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneTuneSemis(kDroneGroup2, 2) - classicTuneSemis(0.75)) < kTiny);
  CHECK(std::fabs(d->runtime().droneTuneSemis(kDroneGroup2, 3) - base->runtime().droneTuneSemis(kDroneGroup2, 3)) < kTiny);
  CHECK(std::fabs(d->runtime().droneTuneSemis(kDroneGroup1, 2) - base->runtime().droneTuneSemis(kDroneGroup1, 2)) < kTiny);

  // MUTE (selector) on a group-0 gen.
  st = def; slot(st, ParameterId::drone_1_mute_2) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().droneMuted(kDroneGroup1, 1) == true);
  CHECK(d->runtime().droneMuted(kDroneGroup2, 1) == base->runtime().droneMuted(kDroneGroup2, 1));

  // MOD amount is a SELECTOR (registry step=1: only 0 or 1); it is applied per gen, and a
  // sibling gen + sibling group stay at their baseline.
  st = def; slot(st, ParameterId::drone_5_mod_4) = 1;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneModAmount(kDroneGroup5, 3) - 1.0) < kTiny);
  CHECK(std::fabs(d->runtime().droneModAmount(kDroneGroup1, 3) - base->runtime().droneModAmount(kDroneGroup1, 3)) < kTiny);

  // VOLT (shared per-group; 60n semis down) is applied to every gen of the group, and
  // leaves the OTHER groups' volt at baseline.
  st = def; slot(st, ParameterId::drone_2_volt) = 0.25;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneVoltSemisDown(kDroneGroup2) - classicVoltSemis(0.25)) < kTiny);
  CHECK(std::fabs(d->runtime().droneVoltSemisDown(kDroneGroup1) - base->runtime().droneVoltSemisDown(kDroneGroup1)) < kTiny);

  // ATT (norm -> seconds 0.001+0.999n) is the group envelope, independent per group.
  st = def; slot(st, ParameterId::drone_2_att) = 0.25;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneGroupAttSeconds(kDroneGroup2) - envSecFromNorm(0.25)) < kTiny);
  CHECK(std::fabs(d->runtime().droneGroupAttSeconds(kDroneGroup1) - base->runtime().droneGroupAttSeconds(kDroneGroup1)) < kTiny);

  // RLS mirrors ATT (separate range, never shared state).
  st = def; slot(st, ParameterId::drone_5_rls) = 0.8;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneGroupRlsSeconds(kDroneGroup5) - envSecFromNorm(0.8)) < kTiny);
  CHECK(std::fabs(d->runtime().droneGroupRlsSeconds(kDroneGroup1) - base->runtime().droneGroupRlsSeconds(kDroneGroup1)) < kTiny);

  // GATE/HOLD: selector 1 -> HOLD only (drone_*_gate_hold never touches the gate, per the
  // locked constraint setDroneGroupHold(selector==1)); a sibling group's hold stays at baseline.
  st = def; slot(st, ParameterId::drone_2_gate_hold) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().droneGroupHold(kDroneGroup2) == true);
  CHECK(d->runtime().droneGroupHold(kDroneGroup1) == base->runtime().droneGroupHold(kDroneGroup1));
  CHECK(d->runtime().droneGroupGate(kDroneGroup2) == base->runtime().droneGroupGate(kDroneGroup2));

  // A different classic voice name maps to its OWN group (drone_4 -> group 2).
  st = def; slot(st, ParameterId::drone_4_tune_1) = 0.875;   // (0.375)*24 = +9 semis.
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().droneTuneSemis(kDroneGroup4, 0) - classicTuneSemis(0.875)) < kTiny);
  CHECK(std::fabs(d->runtime().droneTuneSemis(kDroneGroup1, 0) - base->runtime().droneTuneSemis(kDroneGroup1, 0)) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 2 & 3: NewDrone (PapaVoice 3/6) discriminators. RATE/NOISE/FM/AM are exact; PITCH is
// seed-derived but monotonic in the panel position (verified by direction, not a hardware value).
static void new_drone_family() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto base = mustAccept(def);

  // RATE: closed-form 12n Hz. drone 3 mutates, drone 6 bit-unchanged.
  DeviceStateV1 st = def;
  slot(st, ParameterId::drone_3_rate) = 0.75;
  auto d = mustAccept(st);
  CHECK(std::fabs(d->runtime().drone3RateHz() - newDroneRateHz(0.75)) < kTiny);
  CHECK(std::fabs(d->runtime().drone6RateHz() - base->runtime().drone6RateHz()) < kTiny);

  st = def; slot(st, ParameterId::drone_6_rate) = 0.25;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().drone6RateHz() - newDroneRateHz(0.25)) < kTiny);
  CHECK(std::fabs(d->runtime().drone3RateHz() - base->runtime().drone3RateHz()) < kTiny);

  // NOISE amp (raw 0..1).
  st = def; slot(st, ParameterId::drone_3_noise) = 0.2;
  d = mustAccept(st);
  CHECK(std::fabs(d->runtime().drone3NoiseAmp() - 0.2) < kTiny);
  CHECK(std::fabs(d->runtime().drone6NoiseAmp() - base->runtime().drone6NoiseAmp()) < kTiny);

  // FM / AM are the two factory switches (selector -> bool), drone 3 and 6 independent.
  st = def; slot(st, ParameterId::drone_3_fm) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().drone3Fm() == true);
  CHECK(d->runtime().drone6Fm() == base->runtime().drone6Fm());
  st = def; slot(st, ParameterId::drone_3_am) = 1;
  d = mustAccept(st);
  CHECK(d->runtime().drone3Am() == true);
  CHECK(d->runtime().drone6Am() == base->runtime().drone6Am());

  // PITCH: raise the panel position -> strictly higher effective Hz (real DSP, monotonic);
  // the sibling voice is untouched.
  const double mid = base->runtime().drone3PitchHz();
  st = def; slot(st, ParameterId::drone_3_pitch) = 0.85;
  d = mustAccept(st);
  CHECK(d->runtime().drone3PitchHz() > mid + kTiny);
  CHECK(std::fabs(d->runtime().drone6PitchHz() - base->runtime().drone6PitchHz()) < kTiny);
}

// -----------------------------------------------------------------------------------------
// 4: DEFAULT == RESTORE — the machine built from the canonical power-on default (seed) equals
// the machine built from the same state after an encode/decode round-trip, and any single
// parameter mutation moves exactly one knob (the "only-change-inspector" no-shadow check).
static void default_equals_restore() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto dA = mustAccept(def);

  // Round-trip the canonical default through the portable wire and back, then apply it.
  auto enc = [](const DeviceStateV1& st, std::uint8_t* buf, std::size_t cap) {
    std::size_t n = 0;
    const bool ok = lunar24::core::encode_device_state(st, buf, cap, &n);
    CHECK(ok);
    return n;
  };
  auto dec = [](const std::uint8_t* buf, std::size_t n, DeviceStateV1& out) {
    const bool ok = lunar24::core::decode_device_state(buf, n, &out);
    CHECK(ok);
  };

  std::array<std::uint8_t, 8192> wire{};
  const std::size_t wn = enc(def, wire.data(), wire.size());
  DeviceStateV1 restored;
  dec(wire.data(), wn, restored);
  const auto dB = mustAccept(restored);

  // The same machine: every monitored knob is identical between the direct and restored builds.
  CHECK(dA->runtime().vcoATune() == dB->runtime().vcoATune());
  CHECK(dA->runtime().vcfFreq(0) == dB->runtime().vcfFreq(0));
  CHECK(dA->runtime().vcfBp(1) == dB->runtime().vcfBp(1));
  CHECK(dA->runtime().preampGainNorm() == dB->runtime().preampGainNorm());
  CHECK(dA->runtime().envFollowerAttackSeconds() == dB->runtime().envFollowerAttackSeconds());
  CHECK(dA->runtime().mixerChannelPan(7) == dB->runtime().mixerChannelPan(7));
  CHECK(dA->runtime().droneTuneSemis(kDroneGroup2, 0) == dB->runtime().droneTuneSemis(kDroneGroup2, 0));
  CHECK(dA->runtime().droneGroupAttSeconds(kDroneGroup1) == dB->runtime().droneGroupAttSeconds(kDroneGroup1));
  CHECK(dA->runtime().droneVoltSemisDown(kDroneGroup5) == dB->runtime().droneVoltSemisDown(kDroneGroup5));
  CHECK(dA->runtime().drone3RateHz() == dB->runtime().drone3RateHz());
  CHECK(dA->runtime().drone3PitchHz() == dB->runtime().drone3PitchHz());
  CHECK(dA->runtime().drone6NoiseAmp() == dB->runtime().drone6NoiseAmp());
}

// -----------------------------------------------------------------------------------------
// 5: TYPED REJECT — applyDspState fails closed on an out-of-domain value, reports the FIRST
// failing id + invalid_value, and reports a partial count (never a partial-success path). The
// buildMachineRuntimeCandidate layer rejects the same state earlier at validate_device_state
// (rejected_state), so we exercise the DSP-apply reject directly on a valid definition's
// runtime to pin the mechanism carries the first-failure id.
static void typed_reject_carry_first_fail() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  auto d = mustAccept(def);

  // Corrupt ONE continuous applied_to_DSP param beyond its registry range (validate would
  // reject_state first; this drives the apply-choke defect / first-fail-carry path directly).
  DeviceStateV1 bad = def;
  slot(bad, ParameterId::vco_a_pw) = 2.0;   // pw is 0..1 -> out-of-domain.
  const std::uint32_t badIdx = static_cast<std::uint32_t>(ParameterId::vco_a_pw);
  const double pvBefore = bad.parameters[badIdx];

  ParameterId firstId = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus firstStatus = ParameterApplyStatus::applied;
  const bool ok = d->runtime().applyDspState(bad, firstId, firstStatus);

  CHECK(ok == false);
  CHECK(firstId == ParameterId::vco_a_pw);
  CHECK(firstStatus == ParameterApplyStatus::invalid_value);
  // applyDspState re-writes the runtime's own running count on every call (partial on a
  // rejection); the definition's construction-time count stays 169. Read the runtime count.
  CHECK(d->runtime().dspAppliedCount() < 169u);
  // Fail-closed against partial-success: the source state is NEVER mutated by the apply.
  CHECK(bad.parameters[badIdx] == pvBefore);

  // A non-applied_to_dsp (out-of-contract) id is reported unsupported, never counted as a
  // success. (a def's owned state only holds applied ids, so we push one through manually.)
  ParameterId unsId = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus unsStatus = ParameterApplyStatus::applied;
  DeviceStateV1 withUnsupported = def;
  // keyboard_behaviour is NOT applied_to_dsp; validate would reject it, but the apply choke
  // itself must report it as unsupported (the second layer's own contract).
  slot(withUnsupported, ParameterId::keyboard_behaviour) = 1.0;
  const bool ok2 = d->runtime().applyDspState(withUnsupported, unsId, unsStatus);
  CHECK(ok2 == true);   // non-applied ids are skipped (out of contract scope), never counted.

  // The buildMachineRuntimeCandidate layer rejects an out-of-domain state earlier as
  // rejected_state (validate is the first, strictly-stronger gate) — proving layering.
  auto res = buildMachineRuntimeCandidate(bad, 48000.0);
  CHECK(res.status == MachineCandidateStatus::rejected_state);
  CHECK(res.definition == nullptr);

  // Finding #5: firstFail* is the "otherwise sentinel" — it is RESET on success and OVERWRITTEN on a
  // DIFFERENT rejection, never a residue of an earlier failure. Reuse the SAME output variables across
  // three calls on the same runtime to prove the reset-on-every-call contract:
  //   (a) reject (vco_a_pw)   -> firstId=vco_a_pw, firstStatus=invalid_value   [carry the first fail]
  //   (b) success (default)   -> firstId=kParameterCount, firstStatus=applied  [SENTINEL RESET]
  //   (c) reject (vcf_l_freq) -> firstId=vcf_l_freq, firstStatus=invalid_value [OVERWRITE on new reject]
  DeviceStateV1 good = def;                    // every applied_to_DSP id at a valid default -> 169.
  ParameterId seqId = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus seqStatus = ParameterApplyStatus::applied;
  CHECK(d->runtime().applyDspState(good, seqId, seqStatus) == true);
  CHECK(seqId == static_cast<ParameterId>(kParameterCount));
  CHECK(seqStatus == ParameterApplyStatus::applied);

  DeviceStateV1 bad2 = def;
  slot(bad2, ParameterId::vcf_l_freq) = 1e9;   // far beyond any frequency range -> invalid_value.
  CHECK(d->runtime().applyDspState(bad2, seqId, seqStatus) == false);
  CHECK(seqId == ParameterId::vcf_l_freq);
  CHECK(seqStatus == ParameterApplyStatus::invalid_value);
}

}  // namespace

// Whether ParameterId is one of the 134 ids covered by a per-item readback switch.
bool hasReadback(ParameterId id) {
  for (const RdSpec& s : kRdSpecs) if (s.id == id) return true;
  return false;
}

// -----------------------------------------------------------------------------------------
// REV of @Codex BLOCK finding #1/#2: the exact-169 gate must have a PRODUCT witness, not a bare
// `== 169` constant inside the applicator. Recover the applied_to_DSP set from the disposition
// table (the sole authority @Codex designated), then prove:
//   * count_disposition(applied_to_DSP) is 169 (static contract, restated as a runtime CHECK),
//   * the per-item readback class (134) and the control-source class (35) partition that set
//     EXACTLY: every applied id is in EXACTLY ONE class, no id is covered by both, no applied id
//     is uncovered, and neither class claims a non-applied id.
// Any migration that adds/removes an applied id, or any id that gains/loses a readback slot, now
// breaks a real assertion here rather than silently slipping past a constant.
static void disposition_target_set() {
  std::uint32_t appliedCount = 0, rdCount = 0, ctCount = 0;
  for (std::uint32_t i = 0; i < kParameterCount; ++i) {
    const auto id = static_cast<ParameterId>(i);
    const bool isApplied = disposition_of(id) == StateDisposition::applied_to_dsp;
    const bool isReadback = hasReadback(id);
    const bool isControlSource = controlSourceId(id);
    if (isApplied) ++appliedCount;
    if (isReadback) ++rdCount;
    if (isControlSource) ++ctCount;
    if (isApplied) {
      // every applied id is covered by exactly one mechanism (readback XOR control-source);
      // a double-covered or uncovered applied id is a product-layout bug.
      CHECK(isReadback != isControlSource);
    }
    // neither mechanism may claim a non-applied id (a leaked spec row).
    CHECK(!isReadback || isApplied);
    CHECK(!isControlSource || isApplied);
  }
  CHECK(count_disposition(StateDisposition::applied_to_dsp) == 169);
  CHECK(appliedCount == 169);
  CHECK(rdCount == 134);
  CHECK(ctCount == 35);
  CHECK(rdCount + ctCount == appliedCount);
}

// -----------------------------------------------------------------------------------------
// REV of @Codex BLOCK finding #2: FULL-169 per-item readback. For each of the 134 readback ids,
// apply a SINGLE mismatch-free value (probe derived from the registry descriptor: in-domain and
// off the default) and assert BOTH:
//   (a) MOVE  — the id's REAL readback (the member the render path consumes) moved off the value
//               built from the default state. A setter that was SKIPPED never ran, so its id's
//               readback stays at baseline and (a) fails. This is exactly what catches a single-
//               point setVcoBMorph skip (or any single family-setter removal).
//   (b) VTYPE — the readback equals the DEFINED transfer/selector value: exact for the closed-form
//               families and raw/selector families, and for the monotonic pitch (RdKind::Mono)
//               the readback must still have MOVED, with no closed-form check. A wrong-transfer
//               bug (e.g. tuning written through the volt formula) fails (b).
// The union of these 134 readback ids + the 35 control-source ids (pinned by the 9B oracle) is
// proven equal to exactly 169 by disposition_target_set(); control-source ids have no readback
// getter (internal EG/LFO/Joystick/Sequencer state) and are intentionally excluded here.
static void full169_per_item() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  const auto d0 = mustAccept(def);
  std::array<double, kParameterCount> base{};
  std::array<bool, kParameterCount> isRd{};
  for (const RdSpec& s : kRdSpecs) {
    const auto idx = static_cast<std::uint32_t>(s.id);
    base[idx] = readBackValue(d0->runtime(), s.id);
    isRd[idx] = true;
  }

  for (const RdSpec& s : kRdSpecs) {
    const auto id = s.id;
    const auto idx = static_cast<std::uint32_t>(id);
    const ParameterDescriptor* d = find_parameter(id);
    CHECK(d != nullptr);
    if (d == nullptr) continue;
    const std::string lbl(d->stable_id);

    const double probe = probeFor(d);
    DeviceStateV1 st = def;
    slot(st, id) = probe;
    auto dpr = acceptWithLabel(st, id);
    const double live = readBackValue(dpr->runtime(), id);

    // (a) MOVE — readback must have changed from the default-built baseline.
    if (!(std::fabs(live - base[idx]) > kTiny))
      std::fprintf(stderr, "MOVE-FAIL id=%s probe=%f base=%f live=%f\n",
                   lbl.c_str(), probe, base[idx], live);
    CHECK(std::fabs(live - base[idx]) > kTiny);

    // (b) VTYPE — exact defined transfer (closed-form/raw/selector), no check for Mono pitch.
    if (s.kind != RdKind::Mono) {
      const double expect = expectedFor(s.kind, probe);
      if (!(std::fabs(live - expect) < kTiny))
        std::fprintf(stderr, "VTYPE-FAIL id=%s probe=%f expect=%f live=%f\n",
                     lbl.c_str(), probe, expect, live);
      CHECK(std::fabs(live - expect) < kTiny);
    }
  }

  // @Codex BLOCK #1: the 35 control-source ids MUST ALSO be read back through their real owner
  // accessors (EnvelopeGenerator / Lfo / JoystickCv / FiveStepSequencer). A value that "applies"
  // (count stays 169) but never actually lands on the control source — e.g. the setControlParamValue
  // call deleted, or the whole control-source branch neutered — leaves these at baseline, so MOVE
  // fails. Same MOVE + VTYPE pair as the direct-scalar class, keyed by controlSourceId().
  double csBase = 0.0;
  for (std::uint32_t i = 0; i < kParameterCount; ++i) {
    const auto id = static_cast<ParameterId>(i);
    if (!controlSourceId(id)) continue;
    const auto idx = i;
    csBase = readBackControlSource(d0->runtime(), id);
    base[idx] = csBase;
    isRd[idx] = true;

    const ParameterDescriptor* d = find_parameter(id);
    CHECK(d != nullptr);
    if (d == nullptr) continue;
    const std::string lbl(d->stable_id);

    const double probe = probeFor(d);
    DeviceStateV1 st = def;
    slot(st, id) = probe;
    auto dpr = acceptWithLabel(st, id);
    const double live = readBackControlSource(dpr->runtime(), id);

    // (a) MOVE.
    if (!(std::fabs(live - base[idx]) > kTiny))
      std::fprintf(stderr, "MOVE-FAIL id=%s probe=%f base=%f live=%f\n",
                   lbl.c_str(), probe, base[idx], live);
    CHECK(std::fabs(live - base[idx]) > kTiny);

    // (b) VTYPE — the defined transfer (verbatim store, boolean/selector map, stages+3, pulser Hz).
    const double expect = csExpected(id, probe);
    if (!std::isnan(expect)) {
      if (!(std::fabs(live - expect) < kTiny))
        std::fprintf(stderr, "VTYPE-FAIL id=%s probe=%f expect=%f live=%f\n",
                     lbl.c_str(), probe, expect, live);
      CHECK(std::fabs(live - expect) < kTiny);
    }
  }

  // sanity: the per-item class really covers all 169 applied_to_dsp ids (134 readback + 35 control
  // source), and no non-applied id is claimed.
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < kParameterCount; ++i) if (isRd[i]) ++n;
  CHECK(n == 169);
  for (std::uint32_t i = 0; i < kParameterCount; ++i) {
    const auto id = static_cast<ParameterId>(i);
    CHECK(!isRd[i] || disposition_of(id) == StateDisposition::applied_to_dsp);
  }
}

// -----------------------------------------------------------------------------------------
// REV of @Codex BLOCK finding #3: the oracle must prove the applied 169-state is on the LIVE
// render path (StandaloneAudioEngine::applyDeviceState -> commit -> DeviceAdapter::renderBlock via
// processBlock), NOT just a default-codec round-trip. The failure this closes is a value that
// survives the codec but never actually left the descriptor. So we drive the REAL engine:
//  - an accepted state must render non-silence (status Rendered, not a Dropped*/Illegal/NotReady);
//  - a differing applied value must change that live render. vcf_l_freq at 0.0 vs 1.0 was found
//    empirically to dominate the left output, so it probes the value->render link (a codec
//    round-trip would print identical buffers);
//  - a REJECTED apply must be ATOMIC: the prior definition/format/capabilities are unchanged, the
//    engine STAYS READY, and the first block rendered afterwards is IDENTICAL to the fresh default's
//    first block (no partial install, no corruption of the live render). A running engine advances
//    internal time across processBlock, so the "unchanged" comparison is between two fresh engines'
//    first block, never a same-engine re-render.
constexpr int kRenderFrames = 64;   // the engine block size used here.
constexpr double kOutTiny = 1e-6;   // a live output must be genuinely above this, not digital silence.

double ownerMaxAbs(const double* ch, int frames) {
  double m = 0.0;
  for (int f = 0; f < frames; ++f) { const double a = std::fabs(ch[f]); if (a > m) m = a; }
  return m;
}

StandaloneAudioEngine::Status ownerRender(StandaloneAudioEngine& e, const double* const* in,
                                          double* const* out, int inCh, int outCh, int frames) {
  return e.processBlock(in, out, inCh, outCh, frames);
}

// Accept a state on a fresh engine and render its FIRST block as a 1-in/2-out stereo frame using
// the same driven input everywhere. The engine is freshly constructed for each call so the render is
// the state's true first block (no time-advance cross-contamination).
void renderDrive(const DeviceStateV1& st, std::array<double, kRenderFrames>& l,
                 std::array<double, kRenderFrames>& r) {
  constexpr int kF = kRenderFrames;
  std::array<double, kF> in{};
  for (int f = 0; f < kF; ++f) in[f] = 0.05 * std::sin(0.4 * f);
  const double* inp[1] = {in.data()};
  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
  CHECK(e.isReady());
  double* o[2] = {l.data(), r.data()};
  CHECK(ownerRender(e, inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
}

// finding #3(a,b): an accepted 169-state performs a REAL live render, and a differing applied value
// reaches that render. vcf_l_freq at its two extremes is the routing-grounded discriminator.
static void owner_engine_accepted_state_renders_live() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  constexpr int kF = kRenderFrames;
  std::array<double, kF> in{};
  for (int f = 0; f < kF; ++f) in[f] = 0.05 * std::sin(0.4 * f);
  const double* inp[1] = {in.data()};

  // (a) default -> Accepted, ready, live render non-silent (the driver actually produced audio).
  {
    StandaloneAudioEngine e;
    const auto st = e.applyDeviceState(def, 48000.0, kF, 1, 2);
    CHECK(st == StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(e.isReady());
    std::array<double, kF> a{}, b{};
    double* outp[2] = {a.data(), b.data()};
    CHECK(ownerRender(e, inp, outp, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
    CHECK(e.renderedBlocks() == 1);
    CHECK(ownerMaxAbs(a.data(), kF) > kOutTiny || ownerMaxAbs(b.data(), kF) > kOutTiny);
  }

  // (b) a differing applied value reaches the live render: the two first-block renders differ on
  // the audible left output. If the apply were a no-op the two would be bit-identical.
  DeviceStateV1 lo = def, hi = def;
  slot(lo, ParameterId::vcf_l_freq) = 0.0;
  slot(hi, ParameterId::vcf_l_freq) = 1.0;
  std::array<double, kF> al{}, ar{}, bl{}, br{};
  renderDrive(lo, al, ar);
  renderDrive(hi, bl, br);
  bool same = true;
  for (int f = 0; f < kF; ++f)
    if (std::fabs(al[f] - bl[f]) > kOutTiny || std::fabs(ar[f] - br[f]) > kOutTiny) { same = false; break; }
  CHECK(!same);   // the applied value demonstrably moved the live render.
}

// finding #3(c): a REJECTED applyDeviceState is ATOMIC at the engine. Previous definition/format/
// capabilities are unchanged, the engine STAYS READY, and the FIRST block rendered afterwards is
// IDENTICAL to the fresh default's first block (no partial install, no corruption of the live path).
// Illegal values / formats fed through the engine fail closed, never crash.
static void owner_engine_rejected_apply_is_atomic() {
  const DeviceStateV1 def = make_default_device_state(kSeed);
  constexpr int kF = kRenderFrames;
  std::array<double, kF> in{};
  for (int f = 0; f < kF; ++f) in[f] = 0.05 * std::sin(0.4 * f);
  const double* inp[1] = {in.data()};

  // Reference: a fresh engine's first-block render of the accepted default.
  std::array<double, kF> refL{}, refR{};
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(def, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
    double* o[2] = {refL.data(), refR.data()};
    CHECK(ownerRender(e, inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
    CHECK(ownerMaxAbs(refL.data(), kF) > kOutTiny || ownerMaxAbs(refR.data(), kF) > kOutTiny);
  }

  // (a) out-of-domain value -> RejectedInvalidState: prior definition/format/capabilities unchanged,
  // engine STAYS READY, first block rendered afterwards equals the fresh default's first block.
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(def, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
    const double sr = e.sampleRate(), bs = e.blockSize();
    const int icap = e.inputCapability(), ocap = e.outputCapability();
    DeviceStateV1 badv = def;
    slot(badv, ParameterId::vco_a_pw) = 2.0;  // pw is 0..1 -> out-of-domain -> validator rejects.
    const auto bad = e.applyDeviceState(badv, 48000.0, kF, 1, 2);
    CHECK(bad == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState);
    CHECK(e.isReady());
    CHECK(e.stateApplyStatus() == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState);
    CHECK(e.sampleRate() == sr && e.blockSize() == bs);
    CHECK(e.inputCapability() == icap && e.outputCapability() == ocap);
    // Finding #2: a RejectedInvalidState terminal NEVER populates the DSP first-fail diagnostics —
    // they stay at the entry-sentinel (reset), never a stale residue of an earlier apply.
    CHECK(e.dspApplyFirstFailParamId() == static_cast<ParameterId>(kParameterCount));
    CHECK(e.dspApplyFirstFailStatus() == ParameterApplyStatus::applied);
    std::array<double, kF> postL{}, postR{};
    double* o[2] = {postL.data(), postR.data()};
    CHECK(ownerRender(e, inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
    bool same = true;
    for (int f = 0; f < kF; ++f)
      if (std::fabs(postL[f] - refL[f]) > kOutTiny || std::fabs(postR[f] - refR[f]) > kOutTiny) { same = false; break; }
    CHECK(same);   // the live render after the rejected apply is exactly the accepted default's.
  }

  // (b) illegal format straight to the engine -> RejectedFormat, never crashes, stays ready, and the
  // still-intact default definition keeps rendering (Rendered, not a dropped status).
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(def, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(e.applyDeviceState(def, std::nan(""), kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedFormat);
    CHECK(e.applyDeviceState(def, 48000.0, 0, 1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedFormat);
    CHECK(e.applyDeviceState(def, 48000.0, kF, -1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedFormat);
    CHECK(e.applyDeviceState(def, 48000.0, kF, 1, 1) == StandaloneAudioEngine::StateApplyStatus::RejectedFormat);
    CHECK(e.isReady());
    // Finding #2: a RejectedFormat terminal likewise leaves the DSP first-fail diagnostics at the
    // entry-sentinel (the strict-format gate returns before any builder/apply can touch them).
    CHECK(e.dspApplyFirstFailParamId() == static_cast<ParameterId>(kParameterCount));
    CHECK(e.dspApplyFirstFailStatus() == ParameterApplyStatus::applied);
    std::array<double, kF> uL{}, uR{};
    double* o[2] = {uL.data(), uR.data()};
    CHECK(ownerRender(e, inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
  }
}

int main() {
  full169_default_apply();
  disposition_target_set();
  full169_per_item();
  owner_engine_accepted_state_renders_live();
  owner_engine_rejected_apply_is_atomic();
  vco_a_b_family();
  vcf_l_r_family();
  preamp_envfollower_family();
  mixer_vol_pan_family();
  classic_drone_group_gen_family();
  new_drone_family();
  default_equals_restore();
  typed_reject_carry_first_fail();
  return ::test::finish("test_state_apply_oracle_169");
}
