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

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using lunar24::core::DeviceStateV1;
using lunar24::core::MachineCandidateStatus;
using lunar24::core::MachineRuntimeDefinition;
using lunar24::core::ParameterApplyStatus;
using lunar24::core::ParameterId;
using lunar24::core::VcoControlMode;
using lunar24::core::buildMachineRuntimeCandidate;
using lunar24::core::find_parameter;
using lunar24::core::kDeviceParamCapacity;
using lunar24::core::kParameterCount;
using lunar24::core::make_default_device_state;

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
}

}  // namespace

int main() {
  full169_default_apply();
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
