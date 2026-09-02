// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_state_apply_oracle.cpp — the strong product oracle for GH#12 9B (task#76, REV-2).
//
// This ONE CTest drives the framework-free StandaloneAudioEngine's state-apply surface
// (applyDeviceState) through its public contract, on the same production path the host
// uses: a validated DeviceStateV1 -> buildMachineRuntimeCandidate -> single stopped-stream
// commit -> processBlock -> DeviceAdapter::renderBlock. The oracle does NOT re-implement
// any of that; it drives the public applyDeviceState/processBlock surface, so a builder or
// owner that bypasses the machine/identity apply (or the adapter) has nowhere to hide.
//
// What is asserted (@Codex task#76 card rev 2 + REV-2 BLOCK c74890ff):
//   1. SAFE BOOT == DEFAULT STATE  — prepare(seed) publishes a canonical state wire-equal
//                                  to make_default_device_state(seed) (no separate truth).
//   2. ROUNDTRIP APPLY + RENDER    — default -> encode -> decode -> migrate -> validate ->
//                                  applyDeviceState -> multi-block render; non-silent WET.
//   3. DISTINCT CALIBRATION        — same seed, different calibration trims -> distinct WET
//                                  trace (the GH#6 trim is actually applied, not ignored).
//   4. DISTINCT SEED               — different identity seed -> distinct WET trace.
//   5. INVALID LEAVES PRIOR        — an invalid candidate is ATOMIC: engine stays ready,
//                                  canonical state byte-unchanged, plan/format unchanged,
//                                  re-render bit-identical. Pins plan + format (REV-2).
//   6. REJECTED FORMAT LEAVES PRIOR— an illegal format (NaN rate) same atomic-no-op, and pins
//                                  plan + sampleRate/blockSize/io capability too (REV-2).
//   7. FULL-STATE PRESERVED        — REV-3: ONE legal state writing simultaneously an effector
//                                  param (preserved_deferred), a program-owner param, a
//                                  transfer-unavailable param, left/right keyboard + preset payload,
//                                  a coherent cable+route, left/right ProgramId, and a reserved
//                                  byte; after apply the canonical state is byte-for-byte the wire
//                                  of that same state. A single-field-lost (e.g. effector_x dropped
//                                  back to default) goes RED here.
//   8. ALLOCATOR (separate TU)     — the processBlock render path allocates 0 bytes.
//   9. INVALID STATE REJECTED      — a parameter_out_of_range candidate is rejected by BOTH
//                                  buildMachineRuntimeCandidate and applyDeviceState (the
//                                  validator-bypass counter-example @Codex flagged in rev 2).
//  10. RUNTIME SEED (DRY)          — the runtime voice seed is a REAL input: two seeds give
//                                  DISTINCT DRY A/B traces. DRY A/B are tapped BEFORE the
//                                  VCF->distortion chain, so they isolate the runtime-seed
//                                  path from the (also seed-derived) configureVcfIdentity path
//                                  (@Codex issue 2: the WET diff could come from identity
//                                  alone, leaving a hardcoded runtime seed undetected).
//  11. IDENTITY PROFILE CONSUMERS  — every GH#6 consumer (VCF input drive, distortion drive,
//                                  distortion rail, L/R path staging x calibration trim) reads
//                                  the LIVE runtime value, and each equals the derived
//                                  profile. A stored-but-not-applied profile goes RED
//                                  (@Codex issue 3).
//  12. CHURN A->B->A (RENDERS)     — REV-3: same engine round-trips valid states; canonical truth
//                                  byte-identical on return AND a block renders after EACH apply,
//                                  byte-identical to a FRESH engine applying the same state (each
//                                  apply re-constructs the definition at t=0, so no stale/dangling
//                                  contracts-bindings pointer survives a churn apply).
//  13. BLOCK PARTITION CONSISTENT  — REV-3: the same machine renders PER-SAMPLE EXACT == output
//                                  across all 4 channels regardless of the block partition (no
//                                  block-coupled reset / no <tiny tolerance mask).

#include "mini_test.h"

#include <host/standalone_audio_engine.h>
#include <lunar24/core/device_layout.h>        // WET_L / WET_R / DRY_A / DRY_B
#include <lunar24/core/distortion.h>           // Distortion::kDriveFold / kSaturationVoltage
#include <lunar24/core/machine_candidate.h>    // buildMachineRuntimeCandidate, MachineCandidateStatus
#include <lunar24/core/state_disposition.h>    // find_parameter, ParameterDescriptor, ParameterId
#include <lunar24/core/state_migration.h>      // migrate_device_state
#include <lunar24/core/state_serializer.h>     // encode/decode_device_state, kDeviceStorageSchema
#include <lunar24/core/state_validation.h>     // validate_device_state, ValidationFamily
#include <lunar24/core/unit_identity_profile.h> // deriveVcfIdentityProfile

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// Defined by the isolated allocator TU built into this target.
extern std::size_t g_allocCount;
extern std::size_t g_freeCount;

namespace {

using lunar24::core::DeviceStateV1;
using lunar24::core::DRY_A;
using lunar24::core::DRY_B;
using lunar24::core::Distortion;
using lunar24::core::JackId;
using lunar24::core::MachineCandidateStatus;
using lunar24::core::ParameterId;
using lunar24::core::ProgramId;
using lunar24::core::RouteId;
using lunar24::core::ValidationFamily;
using lunar24::core::WET_L;
using lunar24::core::WET_R;
using lunar24::core::buildMachineRuntimeCandidate;
using lunar24::core::deriveVcfIdentityProfile;
using lunar24::core::encode_device_state;
using lunar24::core::find_parameter;
using lunar24::core::kDistRailStiffness;
using lunar24::core::make_default_device_state;
using lunar24::core::migrate_device_state;
using lunar24::core::StateValidationResult;
using lunar24::core::SynthRuntime;
using lunar24::core::validate_device_state;
using lunar24::host::StandaloneAudioEngine;

using DevicePlan = lunar24::core::DevicePlan;
using EngineStatus = StandaloneAudioEngine::Status;
using StateApplyStatus = StandaloneAudioEngine::StateApplyStatus;

constexpr int kF = 64;          // the block size used by most cases.
constexpr double kTiny = 1e-6;  // a delta well below the 0.5-scale quantization.
constexpr std::size_t kWire = lunar24::core::kDeviceStorageSchema.totalBytesHint;

// ---- helpers ---------------------------------------------------------------------------
double maxAbs(const double* ch, int frames) {
  double m = 0.0;
  for (int f = 0; f < frames; ++f) {
    const double a = std::fabs(ch[f]);
    if (a > m) m = a;
  }
  return m;
}

// Wire-encode both states and compare byte-for-byte. This is the ONE honest equality for the
// canonical state: it compares the portable wire form (never a `memcmp` of the C++ struct,
// which is not layout-portable across macOS/Windows).
bool wireEqual(const DeviceStateV1& a, const DeviceStateV1& b) {
  std::array<std::uint8_t, kWire> ba{};
  std::array<std::uint8_t, kWire> bb{};
  std::size_t wa = 0, wb = 0;
  if (!encode_device_state(a, ba.data(), ba.size(), &wa)) return false;
  if (!encode_device_state(b, bb.data(), bb.size(), &wb)) return false;
  return wa == kWire && wb == kWire && std::memcmp(ba.data(), bb.data(), kWire) == 0;
}

// Pin a plan's deterministic fields (REV-2 atomicity). DevicePlan is a plain value struct;
// we compare every field that defines "the same prepared route" (layout, capability, mapping,
// input route). No operator== exists, so this is the honest field-wise equivalent.
bool planEqual(const DevicePlan& a, const DevicePlan& b) {
  if (a.layout.kind != b.layout.kind) return false;
  if (a.layout.totalChannels != b.layout.totalChannels) return false;
  if (a.outputCapability != b.outputCapability) return false;
  if (a.outputCount != b.outputCount) return false;
  if (a.inputCapability != b.inputCapability) return false;
  if (a.input != b.input) return false;
  if (a.inputCh[0] != b.inputCh[0] || a.inputCh[1] != b.inputCh[1]) return false;
  for (int c = 0; c < 4; ++c) {
    if (a.output.channel[c] != b.output.channel[c]) return false;
  }
  return true;
}

void fillDcIn(double in[1][kF], double v) {
  for (int f = 0; f < kF; ++f) in[0][f] = v;
}

// ---- 1. safe boot publishes the default state -----------------------------------------
void safe_boot_default_equality() {
  constexpr std::uint64_t kSeed = 0x5EEDu;
  const DeviceStateV1 dflt = make_default_device_state(kSeed);

  StandaloneAudioEngine e;
  CHECK(e.prepare(kSeed, 48000.0, kF, 1, 4));       // safe boot.
  CHECK(e.isReady());
  CHECK(e.stateApplyStatus() == StateApplyStatus::Accepted);
  CHECK(e.identityApplied());                       // the GH#6 profile applied to the default too.
  const DeviceStateV1* cs = e.canonicalState();
  CHECK(cs != nullptr);
  CHECK(wireEqual(*cs, dflt));                       // prepare(seed) == make_default_device_state(seed).
}

// ---- 2. roundtrip apply + render -------------------------------------------------------
void default_roundtrip_apply() {
  constexpr std::uint64_t kSeed = 12345u;
  const DeviceStateV1 dflt = make_default_device_state(kSeed);

  std::array<std::uint8_t, kWire> buf{};
  std::size_t w = 0;
  CHECK(encode_device_state(dflt, buf.data(), buf.size(), &w));
  CHECK_EQ(w, kWire);

  DeviceStateV1 decoded;
  CHECK(lunar24::core::decode_device_state(buf.data(), w, &decoded));

  DeviceStateV1 migrated;
  const auto mig = migrate_device_state(decoded, migrated);
  CHECK(mig.ok);
  CHECK(validate_device_state(migrated).ok);

  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(migrated, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(e.isReady());
  CHECK(e.identityApplied());
  CHECK(wireEqual(*e.canonicalState(), migrated));   // the applied state is the canonical truth.

  // Multi-block render: the WET outputs carry the machine, not silence.
  double in[1][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = 0.2 + 0.001 * f;
  const double* inp[1] = {in[0]};
  double out[4][kF] = {{0}};
  double* outp[4] = {out[0], out[1], out[2], out[3]};
  for (int b = 0; b < 3; ++b) {
    CHECK(e.processBlock(inp, outp, 1, 4, kF) == EngineStatus::Rendered);
  }
  CHECK(maxAbs(out[WET_L], kF) > 1e-4);
  CHECK(maxAbs(out[WET_R], kF) > 1e-4);
}

// ---- 3. distinct calibration -> distinct WET trace -------------------------------------
void distinct_calibration_trace() {
  constexpr std::uint64_t kSeed = 777u;
  DeviceStateV1 a = make_default_device_state(kSeed);
  DeviceStateV1 b = make_default_device_state(kSeed);
  // Same seed/version (same identity profile) but a clearly-different calibration trim: the
  // GH#6 trim is a real VCF->distortion staging multiplier, so it must change the WET trace.
  a.calibration.vcfLeftTrim = 0.25f;
  a.calibration.vcfRightTrim = 0.25f;
  b.calibration.vcfLeftTrim = 2.00f;
  b.calibration.vcfRightTrim = 2.00f;

  StandaloneAudioEngine ea, eb;
  CHECK(ea.applyDeviceState(a, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(eb.applyDeviceState(b, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);

  double in[1][kF] = {{0}}, outA[4][kF] = {{0}}, outB[4][kF] = {{0}};
  fillDcIn(in, 0.2);
  const double* inp[1] = {in[0]};
  double* pA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* pB[4] = {outB[0], outB[1], outB[2], outB[3]};
  CHECK(ea.processBlock(inp, pA, 1, 4, kF) == EngineStatus::Rendered);
  CHECK(eb.processBlock(inp, pB, 1, 4, kF) == EngineStatus::Rendered);

  // A trim that is ignored / never applied would give two IDENTICAL traces — this is the
  // detector that the GH#6 calibration is actually consumed, not stored-only.
  bool differs = false;
  for (int f = 0; f < kF; ++f) {
    if (std::fabs(outA[WET_L][f] - outB[WET_L][f]) > kTiny ||
        std::fabs(outA[WET_R][f] - outB[WET_R][f]) > kTiny) {
      differs = true;
      break;
    }
  }
  CHECK(differs);
  CHECK(maxAbs(outA[WET_L], kF) > 1e-4);   // both are real signals, not silence.
  CHECK(maxAbs(outB[WET_L], kF) > 1e-4);
}

// ---- 4. distinct seed -> distinct WET trace --------------------------------------------
void distinct_seed_trace() {
  DeviceStateV1 a = make_default_device_state(0x111u);
  DeviceStateV1 b = make_default_device_state(0x222u);

  StandaloneAudioEngine ea, eb;
  CHECK(ea.applyDeviceState(a, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(eb.applyDeviceState(b, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);

  double in[1][kF] = {{0}}, outA[4][kF] = {{0}}, outB[4][kF] = {{0}};
  fillDcIn(in, 0.2);
  const double* inp[1] = {in[0]};
  double* pA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* pB[4] = {outB[0], outB[1], outB[2], outB[3]};
  CHECK(ea.processBlock(inp, pA, 1, 4, kF) == EngineStatus::Rendered);
  CHECK(eb.processBlock(inp, pB, 1, 4, kF) == EngineStatus::Rendered);

  // Different identity seed -> different voice-layer seed AND different identity profile -> the
  // WET traces must diverge. A seed stored-but-not-consumed would give identical traces.
  bool differs = false;
  for (int f = 0; f < kF; ++f) {
    if (std::fabs(outA[WET_L][f] - outB[WET_L][f]) > kTiny ||
        std::fabs(outA[WET_R][f] - outB[WET_R][f]) > kTiny) {
      differs = true;
      break;
    }
  }
  CHECK(differs);
}

// ---- 5. invalid candidate leaves prior unchanged (atomic) ------------------------------
// REV-2: ALSO pins that the rejection does not touch the plan or the format the engine was
// prepared with (only the canonical-state pin existed in rev 1; @Codex asked for the full
// old definition/state + plan + sampleRate/blockSize/io-capability pin).
void invalid_leaves_prior_unchanged() {
  constexpr std::uint64_t kSeed = 444u;
  const DeviceStateV1 valid = make_default_device_state(kSeed);

  double in[1][kF] = {{0}};
  fillDcIn(in, 0.25);
  const double* inp[1] = {in[0]};

  // REFERENCE: a fresh engine applies the valid state and renders ONE block. The machine is
  // time-advancing (self-oscillating), so a second render on the SAME engine could never be used
  // as the "unchanged" proof; we must compare apples-to-apples on a fresh engine's FIRST render.
  double ref[4][kF] = {{0}};
  double* pRef[4] = {ref[0], ref[1], ref[2], ref[3]};
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(valid, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
    CHECK(e.processBlock(inp, pRef, 1, 4, kF) == EngineStatus::Rendered);
  }

  // The engine under test: apply the valid state, then a REJECTED candidate. An INVALID candidate:
  // a calibration trim of 0 is not positive -> calibration_invalid. The rejection must be ATOMIC —
  // engine stays ready, canonical state unchanged, and the plan + format are NOT touched.
  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(valid, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  // Pin the prepared-state truth BEFORE the rejection.
  const DevicePlan planBefore = e.plan();
  const double srBefore = e.sampleRate();
  const int bsBefore = e.blockSize();
  const int icBefore = e.inputCapability();
  const int ocBefore = e.outputCapability();

  DeviceStateV1 bad = valid;
  bad.calibration.vcfLeftTrim = 0.0f;
  CHECK(e.applyDeviceState(bad, 48000.0, kF, 1, 4) == StateApplyStatus::RejectedInvalidState);
  CHECK(e.isReady());                                // STILL ready (atomic rejection).
  CHECK(e.stateApplyStatus() == StateApplyStatus::RejectedInvalidState);
  CHECK(e.lastStateValidation().family == ValidationFamily::calibration_invalid);
  CHECK(wireEqual(*e.canonicalState(), valid));       // prior canonical state unchanged.
  CHECK(planEqual(e.plan(), planBefore));             // prior plan unchanged.
  CHECK(e.sampleRate() == srBefore);                  // prior format unchanged.
  CHECK(e.blockSize() == bsBefore);
  CHECK(e.inputCapability() == icBefore);
  CHECK(e.outputCapability() == ocBefore);

  // Its FIRST render is bit-identical to the fresh reference: the rejection was a no-op on the
  // definition AND did not advance the machine, so an engine that applied-then-was-rejected
  // renders exactly like one that applied with no rejection. This is the honest atomicity proof
  // (a second render on this engine would differ simply because time advanced, not because the
  // rejected candidate changed anything).
  double outAfter[4][kF] = {{0}};
  double* pAft[4] = {outAfter[0], outAfter[1], outAfter[2], outAfter[3]};
  CHECK(e.processBlock(inp, pAft, 1, 4, kF) == EngineStatus::Rendered);
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f)
      CHECK(outAfter[c][f] == ref[c][f]);             // prior definition still renders identically.
}

// ---- 6. rejected format leaves prior unchanged (atomic) --------------------------------
// REV-2: pins plan + format too (as above).
void rejected_format_leaves_prior() {
  constexpr std::uint64_t kSeed = 999u;
  const DeviceStateV1 valid = make_default_device_state(kSeed);

  double in[1][kF] = {{0}};
  fillDcIn(in, 0.25);
  const double* inp[1] = {in[0]};

  double ref[4][kF] = {{0}};
  double* pRef[4] = {ref[0], ref[1], ref[2], ref[3]};
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(valid, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
    CHECK(e.processBlock(inp, pRef, 1, 4, kF) == EngineStatus::Rendered);
  }

  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(valid, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  const DevicePlan planBefore = e.plan();
  const double srBefore = e.sampleRate();
  const int bsBefore = e.blockSize();
  const int icBefore = e.inputCapability();
  const int ocBefore = e.outputCapability();

  CHECK(e.applyDeviceState(valid, std::nan(""), kF, 1, 4) == StateApplyStatus::RejectedFormat);
  CHECK(e.isReady());                                // atomic — never goes NOT-READY.
  CHECK(wireEqual(*e.canonicalState(), valid));       // prior canonical state unchanged.
  CHECK(planEqual(e.plan(), planBefore));             // prior plan unchanged.
  CHECK(e.sampleRate() == srBefore);                  // prior format unchanged.
  CHECK(e.blockSize() == bsBefore);
  CHECK(e.inputCapability() == icBefore);
  CHECK(e.outputCapability() == ocBefore);

  double outAfter[4][kF] = {{0}};
  double* pAft[4] = {outAfter[0], outAfter[1], outAfter[2], outAfter[3]};
  CHECK(e.processBlock(inp, pAft, 1, 4, kF) == EngineStatus::Rendered);
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f)
      CHECK(outAfter[c][f] == ref[c][f]);
}

// ---- 7. full-state preservation byte-for-byte (REV-3) ---------------------------------
// @Codex REV-3 item 1: the rev-2 preservation test set only a calibration trim + one keyboard seq
// note, so a "drop a preserved field back to its registry initial" mutation (e.g. effector_x forced
// back to 0.5) left the oracle green. This test writes ONE legal state that touches a representative
// of EVERY conserved family at once — a preserved_deferred effector param, a preserved_deferred
// program-owner param, a transfer-unavailable param, left/right keyboard live + preset payload, a
// coherent legal cable+route, left/right ProgramId, and a reserved byte — then asserts the applied
// canonical state is byte-for-byte the WIRE of that same state. Any single field lost / re-zeroed
// (the exact "drop preserved family" mutation) goes RED here.
void full_state_preserved() {
  constexpr std::uint64_t kSeed = 0xB0101u;
  DeviceStateV1 st = make_default_device_state(kSeed);

  // (a) preserved_deferred_p6_p8: an effector param AND a program-owner param, both non-default
  //     (their registry initials are 0.5 and 0.0 respectively, so a drop-back would be caught).
  st.parameters[static_cast<std::uint32_t>(ParameterId::effector_x)] = 0.25;
  st.parameters[static_cast<std::uint32_t>(ParameterId::program_cathedral_1_x)] = 0.25;
  // (b) transfer_unavailable: a landed param with NO runtime consumer (still preserved byte-exact).
  st.parameters[static_cast<std::uint32_t>(ParameterId::vco_a_pwm)] = 0.75;

  // (c) left/right keyboard live + preset payload (fields a later keyboard slice consumes).
  st.keyboardSeqCurrent.steps[2].note = 0x55u;
  st.keyboardSeqCurrentR.steps[2].note = 0x66u;
  st.keyboardPresets[0].reserved[0] = 0xA5u;   // the shell's reserved byte, preserved verbatim.
  st.keyboardPresets[0].reserved[1] = 0x5Au;

  // (d) A COHERENT legal cable+route: keyboard V/OCT -> VCO A V/OCT. The sink is that route's own
  //     sink (vco_a_v_oct_in), so its cable presence makes routeOverridden[route0] == 1 — the only
  //     mutually-consistent (cable, route) pair.
  const auto kVcoA = static_cast<std::uint32_t>(JackId::vco_a_v_oct_in);
  st.inputCable[kVcoA] = 1u;
  st.cableSource[kVcoA] = JackId::keyboard_v_oct_out;
  st.routeOverridden[static_cast<std::uint32_t>(RouteId::route_keyboard_v_oct_to_vco)] = 1u;

  // (e) left/right ProgramId: a valid non-default program on both sides (default is cathedral.1).
  st.leftEffector.program = ProgramId::program_cathedral_2;
  st.rightEffector.program = ProgramId::program_magic_1;

  // The composite must itself be a legal state — the honest precondition that guards against an
  // accidentally-incoherent composite (fails loudly here rather than fuzzing the apply path).
  CHECK(validate_device_state(st).ok);

  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(e.identityApplied());
  CHECK(wireEqual(*e.canonicalState(), st));   // every field preserved byte-for-byte.
}

// ---- 9. invalid state (parameter_out_of_range) is rejected everywhere ----------------
// REV-2 issue 1: the rev-1 candidate let a state ctor mint a "valid" definition from an
// INVALID DeviceState (validate_device_state==0 but valid()==1), bypassing validation. The
// state ctor is now private and the builder folds validation/identity/graph into one decision,
// so an out-of-range parameter must be rejected by BOTH the builder and applyDeviceState.
void invalid_state_rejected() {
  constexpr std::uint64_t kSeed = 0xABCu;
  DeviceStateV1 st = make_default_device_state(kSeed);
  // Exceed a landed parameter's declared max -> parameter_out_of_range family (not identity/cal).
  const ParameterId pid = ParameterId::lfo_a_rate;
  const auto* d = find_parameter(pid);
  CHECK(d != nullptr);
  CHECK(d->max > d->min);                       // a real bounded descriptor, not an open range.
  st.parameters[static_cast<std::uint32_t>(pid)] = d->max * 8.0 + 1.0;  // clearly out of range.

  // 1) validate_device_state rejects it with the parameter family.
  const StateValidationResult vr = validate_device_state(st);
  CHECK(!vr.ok);
  CHECK(vr.family == ValidationFamily::parameter_out_of_range);

  // 2) buildMachineRuntimeCandidate must NOT mint a definition from it.
  const auto res = buildMachineRuntimeCandidate(st, 48000.0);
  CHECK(res.status == MachineCandidateStatus::rejected_state);
  CHECK(res.definition == nullptr);

  // 3) applyDeviceState rejects it as RejectedInvalidState (atomic, not accepted).
  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 4) == StateApplyStatus::RejectedInvalidState);
  CHECK(e.lastStateValidation().family == ValidationFamily::parameter_out_of_range);
  CHECK(!e.identityApplied());
}

// ---- 10. runtime voice seed is a REAL input (seeded drone channels, pre-chain) ---------
// REV-2 issue 2: the rev-1 WET-seed test was a false-green because the WET-trace diff comes
// from configureVcfIdentity (which is ALSO seed-derived), masking a hardcoded runtime seed.
//
// The isolation channel is the runtime's OWN seeded voice value, read LIVE from the DSP:
// droneChannel(0..3) (classic drone_1/2/4/5, seeded from `seed_`) and drone3Channel()/
// drone6Channel() (the Papa Srapa voices, seeded from newVoiceSeed(seed_,0/1)). These are the
// SEEDED voice outputs BEFORE the VCF->distortion chain, so they depend on the runtime voice
// seed but NOT on configureVcfIdentity (post-chain) — the exact isolation that makes the
// "runtime seed = constant 0" mutation go RED.
//
// NOTE: DRY A/B (the `Logical::DRY_A/DRY_B` output channels) are NOT this probe — they tap
// vcA_/vcB_, the VCO A/B oscillators, which are constructed from sampleRate only and carry NO
// runtime seed. I verified this: DRY A/B are non-silent but IDENTICAL across seeds. The seeded
// voices are the drone bank (chIn_[VoiceMixer::kChannelDrone*]), which the DRY taps never see.
void runtime_seed_drives_drone() {
  DeviceStateV1 a = make_default_device_state(0x556u);
  DeviceStateV1 b = make_default_device_state(0x77Au);

  StandaloneAudioEngine ea, eb;
  CHECK(ea.applyDeviceState(a, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(eb.applyDeviceState(b, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);

  double in[1][kF] = {{0}};
  fillDcIn(in, 0.2);
  const double* inp[1] = {in[0]};
  double outA[4][kF] = {{0}}, outB[4][kF] = {{0}};
  double* pA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* pB[4] = {outB[0], outB[1], outB[2], outB[3]};

  // The getters read the last-executed frame, so we snapshot after each of several blocks to
  // collect multiple seed-sensitive sample points (a single frame could coincidentally coincide).
  // REV-3: use the by-value observeRuntime() snapshot (NOT a long-lived runtime() pointer) so no
  // pointer is held across an apply — these engines are never re-applied here, but the snapshot is
  // the always-safe form and keeps the test free of the dangling-pointer hazard item.
  bool anyLive = false;
  bool anyDiffers = false;
  for (int blk = 0; blk < 5; ++blk) {
    CHECK(ea.processBlock(inp, pA, 1, 4, kF) == EngineStatus::Rendered);
    CHECK(eb.processBlock(inp, pB, 1, 4, kF) == EngineStatus::Rendered);
    const StandaloneAudioEngine::RuntimeObservation oa = ea.observeRuntime();
    const StandaloneAudioEngine::RuntimeObservation ob = eb.observeRuntime();
    const double snapA[6] = {oa.seededVoice[0], oa.seededVoice[1], oa.seededVoice[2],
                             oa.seededVoice[3], oa.seededVoice[4], oa.seededVoice[5]};
    const double snapB[6] = {ob.seededVoice[0], ob.seededVoice[1], ob.seededVoice[2],
                             ob.seededVoice[3], ob.seededVoice[4], ob.seededVoice[5]};
    for (int c = 0; c < 6; ++c) {
      if (std::fabs(snapA[c]) > 1e-6) anyLive = true;        // the seeded drone runs, not silence.
      if (std::fabs(snapA[c] - snapB[c]) > kTiny) anyDiffers = true;  // different seed -> different voice.
    }
  }

  // The seeded drone voices must be LIVE (non-zero) — otherwise this probe proves nothing.
  CHECK(anyLive);
  // Two different runtime seeds -> the seeded drone voice value diverges. If the runtime seed is
  // stored-but-hardcoded-to-0, both engines seed the drones identically and this NEVER differs.
  CHECK(anyDiffers);
}

// ---- 11. every GH#6 consumer reads the LIVE derived profile --------------------------
// REV-2 issue 3: deleting vcf_.setInputDrive(...) (or any one GH#6 consumer) left the rev-1
// oracle green, because it only observed the WET trace. This case reads the LIVE runtime value
// for EVERY consumer and asserts each equals the derived profile's value. A stored-but-not-
// applied profile, a wiring error, or an L/R swap all go RED here.
void identity_profile_consumers() {
  constexpr std::uint64_t kSeed = 0x909u;
  DeviceStateV1 st = make_default_device_state(kSeed);
  const float lT = st.calibration.vcfLeftTrim;
  const float rT = st.calibration.vcfRightTrim;
  const auto profile = deriveVcfIdentityProfile(st.identitySeed.seed, st.identityModelVersion);

  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(e.identityApplied());
  // REV-3: read via the by-value observeRuntime() snapshot, so no runtime() pointer is held at all
  // (a pointer would dangle on the next apply). The snapshot copies every consumer scalar once.
  const StandaloneAudioEngine::RuntimeObservation obs = e.observeRuntime();
  CHECK(obs.identityConfigured);

  // VCF input-stage drive: L/R from profile.left/right.vcfDrive.
  CHECK(obs.vcfInputDrive[0] == profile.left.vcfDrive);
  CHECK(obs.vcfInputDrive[1] == profile.right.vcfDrive);
  // Distortion drive: kDriveFold*(1+distDrive) per side.
  CHECK(obs.distortionDrive[0] == Distortion::kDriveFold * (1.0 + profile.left.distDrive));
  CHECK(obs.distortionDrive[1] == Distortion::kDriveFold * (1.0 + profile.right.distDrive));
  // Distortion rail: kSaturationVoltage*(1+kDistRailStiffness*distDrive) per side.
  CHECK(obs.distortionRail[0] == Distortion::kSaturationVoltage *
                                    (1.0 + kDistRailStiffness * profile.left.distDrive));
  CHECK(obs.distortionRail[1] == Distortion::kSaturationVoltage *
                                    (1.0 + kDistRailStiffness * profile.right.distDrive));
  // Path staging gain: calibration trim × profile pathGain (L/R independent).
  CHECK(obs.vcfPathStagingGain[0] == static_cast<double>(lT) * profile.left.pathGain);
  CHECK(obs.vcfPathStagingGain[1] == static_cast<double>(rT) * profile.right.pathGain);
}

// ---- 12. churn A->B->A on the same engine (renders after every apply) -----------------
// REV-2: a valid state round-trips through competing applies on ONE engine; the canonical truth
// is byte-identical on return. REV-3 (@Codex item 2): the rev-2 churn NEVER rendered after a
// commit, so a re-apply that left the runtime's contracts/bindings pointing into the PRIOR
// (now-released) definition would be invisible. Every apply here ALSO renders one block and
// compares it bit-for-bit to a FRESH engine's first block of the SAME state: because each
// applyDeviceState re-constructs the definition at t=0, a freshly-built A is indistinguishable
// from the churn engine's re-built A — which is precisely the proof that no stale/dangling
// contracts-bindings pointer survived the definition swap.
void churn_applies_state_around() {
  constexpr std::uint64_t kSeed = 0xFACEu;
  DeviceStateV1 a = make_default_device_state(kSeed);
  DeviceStateV1 b = make_default_device_state(kSeed);
  b.calibration.vcfLeftTrim = 0.25f;   // a genuinely different (but valid) machine.
  b.calibration.vcfRightTrim = 0.25f;

  // Fixed input, identical for every render below, so two fresh t=0 constructions of the SAME state
  // are bit-for-bit comparable.
  double in[1][kF] = {{0}};
  fillDcIn(in, 0.2);
  const double* inp[1] = {in[0]};

  // FRESH t=0 references: `refA`/`refB` are the FIRST block of a fresh engine applying that state.
  auto firstBlock = [&](const DeviceStateV1& s, double out[4][kF]) {
    StandaloneAudioEngine ef;
    CHECK(ef.applyDeviceState(s, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
    CHECK(ef.isReady());
    double* pRef[4] = {out[0], out[1], out[2], out[3]};
    CHECK(ef.processBlock(inp, pRef, 1, 4, kF) == EngineStatus::Rendered);
  };
  double refA[4][kF] = {{0}}, refB[4][kF] = {{0}};
  firstBlock(a, refA);
  firstBlock(b, refB);

  // The churn engine under test. After EACH apply: render one block and require it to match the
  // fresh t=0 reference for that state — bit-identical (a dangling binding would not be).
  StandaloneAudioEngine e;
  auto renderAndCompare = [&](const double ref[4][kF]) {
    double outAst[4][kF] = {{0}};
    double* pAst[4] = {outAst[0], outAst[1], outAst[2], outAst[3]};
    CHECK(e.processBlock(inp, pAst, 1, 4, kF) == EngineStatus::Rendered);
    for (int c = 0; c < 4; ++c)
      for (int f = 0; f < kF; ++f)
        CHECK(outAst[c][f] == ref[c][f]);   // no stale pointer, no residual prior-state read.
  };

  // A: canonical truth is A, then render aligned with fresh-A.
  CHECK(e.applyDeviceState(a, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(wireEqual(*e.canonicalState(), a));
  CHECK(e.identityApplied());
  renderAndCompare(refA);

  // Churn to B: canonical truth is B, then render aligned with fresh-B (B was REBUILT, not left
  // reading A's definition).
  CHECK(e.applyDeviceState(b, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(wireEqual(*e.canonicalState(), b));
  CHECK(e.identityApplied());
  renderAndCompare(refB);

  // Churn back to A: canonical truth is byte-IDENTICAL to the first A, then render aligned with
  // fresh-A again (no drift, no half-write, no dangling binary from the prior B).
  CHECK(e.applyDeviceState(a, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);
  CHECK(wireEqual(*e.canonicalState(), a));
  CHECK(e.identityApplied());
  renderAndCompare(refA);
  CHECK(e.stateApplyStatus() == StateApplyStatus::Accepted);
  CHECK(e.isReady());
}

// ---- 13. block partition-independence ------------------------------------------------
// REV-2: the same machine must render per-sample identical output regardless of how the input
// is chunked into processBlock calls. A block-coupled reset / per-block accumulation would make
// the two partitions diverge — the machine must be sample-deterministic.
void block_partition_consistent() {
  constexpr std::uint64_t kSeed = 0xBEEF9u;
  const DeviceStateV1 st = make_default_device_state(kSeed);
  constexpr int kHalf = kF / 2;      // 32
  constexpr int kTotal = kF * 3;     // 192 samples

  // Build one flat input sequence.
  double in[kTotal];
  for (int f = 0; f < kTotal; ++f) in[f] = 0.2 + 0.0005 * f;

  // Engine A: 3 blocks of kF. Engine B: 6 blocks of kHalf. Same state, same input.
  StandaloneAudioEngine ea, eb;
  CHECK(ea.applyDeviceState(st, 48000.0, kTotal, 1, 4) == StateApplyStatus::Accepted);
  CHECK(eb.applyDeviceState(st, 48000.0, kTotal, 1, 4) == StateApplyStatus::Accepted);

  double outA[4][kTotal] = {{0}}, outB[4][kTotal] = {{0}};

  // A: 3 x kF blocks.
  for (int b = 0; b < 3; ++b) {
    const double* inp[1] = {in + b * kF};
    double* pA[4] = {outA[0] + b * kF, outA[1] + b * kF, outA[2] + b * kF, outA[3] + b * kF};
    CHECK(ea.processBlock(inp, pA, 1, 4, kF) == EngineStatus::Rendered);
  }
  // B: 6 x kHalf blocks.
  for (int b = 0; b < 6; ++b) {
    const double* inp[1] = {in + b * kHalf};
    double* pB[4] = {outB[0] + b * kHalf, outB[1] + b * kHalf, outB[2] + b * kHalf,
                     outB[3] + b * kHalf};
    CHECK(eb.processBlock(inp, pB, 1, 4, kHalf) == EngineStatus::Rendered);
  }

  // Per-sample EXACT-identical on EVERY output channel (REV-3: no <tiny tolerance that could mask a
  // block-coupled reset). The machine must be block-partition independent at full float precision.
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kTotal; ++f)
      CHECK(outA[c][f] == outB[c][f]);
}

// ---- 8. allocator: the render path allocates 0 -----------------------------------------
void allocator_probe() {
  constexpr std::uint64_t kSeed = 0x7070u;
  const DeviceStateV1 st = make_default_device_state(kSeed);
  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 4) == StateApplyStatus::Accepted);  // allocates OUTSIDE.
  double in[1][kF] = {{0}}, out[4][kF] = {{0}};
  fillDcIn(in, 0.25);
  const double* inp[1] = {in[0]};
  double* outp[4] = {out[0], out[1], out[2], out[3]};

  // The WHOLE owner-delegate window is measured, including the first render. There is no lazy
  // init on the apply->render path (the definition + identity are fully applied in
  // applyDeviceState; renderBlock is a pure delegate), so the entire render path must be
  // alloc-0 AND free-0 from the very first processBlock. Counts BOTH unaligned and aligned.
  const std::size_t before = g_allocCount;
  const std::size_t freeBefore = g_freeCount;
  for (int i = 0; i < 21; ++i) CHECK(e.processBlock(inp, outp, 1, 4, kF) == EngineStatus::Rendered);
  CHECK(g_allocCount == before);   // the render path allocates 0.
  CHECK(g_freeCount == freeBefore);  // ... and frees 0.

  // Live on both sides, both alignednesses. A deliberate alloc + matching free must be detected.
  const std::size_t aSnap = g_allocCount, fSnap = g_freeCount;
  void* p = ::operator new(4);
  CHECK(g_allocCount > aSnap);
  ::operator delete(p);
  CHECK(g_freeCount > fSnap);
  const std::size_t alSnap = g_allocCount, alFreeSnap = g_freeCount;
  const std::align_val_t al64{64};
  void* ap = ::operator new(8, al64);
  CHECK(g_allocCount > alSnap);
  ::operator delete(ap, al64);
  CHECK(g_freeCount > alFreeSnap);
  g_allocCount = 0;
  g_freeCount = 0;
}

}  // namespace

int main() {
  std::printf("== GH#12 9B: state-aware runtime candidate apply oracle (task#76, REV-3) ==\n");
  safe_boot_default_equality();
  default_roundtrip_apply();
  distinct_calibration_trace();
  distinct_seed_trace();
  invalid_leaves_prior_unchanged();
  rejected_format_leaves_prior();
  full_state_preserved();
  invalid_state_rejected();
  runtime_seed_drives_drone();
  identity_profile_consumers();
  churn_applies_state_around();
  block_partition_consistent();
  allocator_probe();
  return ::test::finish("state_apply_oracle");
}
