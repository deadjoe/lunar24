// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// owner_atomicity_rejected_dsp.cpp — task#78 (GH#12 9C) gap#1 candidate evidence: the four
// owner-atomicity sequences at the REAL RejectedDspApply path.
//
// Decisive prior probe: NO real DeviceStateV1 reaches rejected_dsp_apply. validate()'s
// check_scalar_value is strictly-stronger-or-equal to the DSP admission dspParamValid_, and the 134
// direct-scalar applyDspParam cases are all "setter then return applied". The ONLY reliable route is
// a per-value failure, which needs a SOURCE mutation. So this harness is NOT a normal always-green
// test: it must be compiled against a PATCHED copy of core/include/lunar24/core/machine_runtime.h
// that conditionally rejects one legal value (vco_b_morph >= 1.0). Run the driver script
// tests/mutation/run_rejected_dsp_mutation.sh, which GENERATES that patched copy into a scratch dir,
// compiles against it (GREEN: 40/0), compiles against the real header (RED: 40/13, proving the
// rejection is genuinely load-bearing), then RESTORES — nothing under core/ or any tracked path is
// touched. Do NOT register this target in CTest: without the mutation it fails by design.
//
// The mutation is CONDITIONED ONLY on the legal-B value (vco_b_morph >= 1.0): A's default morph (0.5)
// still applies and passes. Real host calls, the validator, and the publish logic are unchanged;
// this exists so the four owner-atomicity sequences can be driven at a real RejectedDspApply path.

#include <cstdio>
#include <cmath>
#include <cstring>
#include <array>
#include <cstdint>
#include <lunar24/core/device_state.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/machine_candidate.h>
#include <host/standalone_audio_engine.h>

using namespace lunar24::core;
using lunar24::core::kParameterCount;
using lunar24::host::StandaloneAudioEngine;

static const std::uint64_t kSeed = 0x4C554E4152ULL;
static constexpr int kF = 128;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
  std::printf("  FAIL %-28s %s:%d\n", #cond, __FILE__, __LINE__); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; if (!((a) == (b))) { ++g_fail; \
  std::printf("  FAIL %-28s (%s=%lld vs %s=%lld) %s:%d\n", "expr", #a, (long long)(a), #b, (long long)(b), __FILE__, __LINE__); } } while (0)

static double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// Full-byte semantic equality of the canonical state (DeviceStateV1 is a plain aggregate).
// Both sides are produced by the same default/apply path, so identical value => identical bytes.
static bool stateEqual(const DeviceStateV1& a, const DeviceStateV1& b) {
  return std::memcmp(&a, &b, sizeof(DeviceStateV1)) == 0;
}

// A reference first-block render of the accepted default on a FRESH engine (the "A output").
static void renderRef(const DeviceStateV1& st, std::array<double, kF>& l, std::array<double, kF>& r) {
  std::array<double, kF> in{};
  for (int f = 0; f < kF; ++f) in[f] = 0.05 * std::sin(0.4 * f);
  const double* inp[1] = {in.data()};
  StandaloneAudioEngine e;
  CHECK(e.applyDeviceState(st, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
  double* o[2] = {l.data(), r.data()};
  CHECK(e.processBlock(inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
}

static double maxAbs(const double* c, int n) { double m = 0.0; for (int i = 0; i < n; ++i) { double a = std::fabs(c[i]); if (a > m) m = a; } return m; }

int main() {
  const DeviceStateV1 A = make_default_device_state(kSeed);
  DeviceStateV1 B = A;
  slot(B, ParameterId::vco_b_morph) = 1.0;   // legal (in [0,1], continuous) -> passes validate -> only the MUTATION rejects.

  // ---- sanity: B really reaches rejected_dsp_apply via the mutation, and carries the typed detail.
  {
    StandaloneAudioEngine e;
    const auto st = e.applyDeviceState(B, 48000.0, kF, 1, 2);
    CHECK_EQ(static_cast<int>(st), (int)StandaloneAudioEngine::StateApplyStatus::RejectedDspApply);
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)ParameterId::vco_b_morph);
    CHECK_EQ((int)e.dspApplyFirstFailStatus(), (int)ParameterApplyStatus::invalid_value);
    // A's default morph (0.5) still passes through the mutation -> Accepted on a fresh engine.
    StandaloneAudioEngine a2;
    CHECK(a2.applyDeviceState(A, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
  }

  // ---- SEQ 1: A(accepted) -> legal-B triggers DSP reject -> A state / format / plan / output unchanged.
  {
    std::array<double, kF> refL{}, refR{};
    renderRef(A, refL, refR);

    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(A, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(e.isReady());
    // Capture A's committed view before the failing apply.
    const double sr = e.sampleRate(), bs = e.blockSize();
    const int icap = e.inputCapability(), ocap = e.outputCapability();
    const int pOutCount = e.plan().outputCount, pOutputCap = e.plan().outputCapability;
    const int pInputCap = e.plan().inputCapability, pInCh0 = e.plan().inputCh[0], pInCh1 = e.plan().inputCh[1];
    const DeviceStateV1 aCanon = *e.canonicalState();

    const auto bad = e.applyDeviceState(B, 48000.0, kF, 1, 2);
    CHECK_EQ((int)bad, (int)StandaloneAudioEngine::StateApplyStatus::RejectedDspApply);
    CHECK(e.isReady());
    // State unchanged.
    CHECK(e.canonicalState() != nullptr && stateEqual(*e.canonicalState(), aCanon));
    // Format unchanged.
    CHECK(e.sampleRate() == sr && e.blockSize() == bs);
    CHECK(e.inputCapability() == icap && e.outputCapability() == ocap);
    // Plan unchanged (plan-defining scalars).
    CHECK(e.plan().outputCount == pOutCount && e.plan().outputCapability == pOutputCap);
    CHECK(e.plan().inputCapability == pInputCap && e.plan().inputCh[0] == pInCh0 && e.plan().inputCh[1] == pInCh1);
    // Diagnostics carry the real typed rejection.
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)ParameterId::vco_b_morph);
    CHECK_EQ((int)e.dspApplyFirstFailStatus(), (int)ParameterApplyStatus::invalid_value);
    // Subsequent output unchanged: rendering after the failed apply equals fresh-A's first block.
    std::array<double, kF> in{};
    for (int f = 0; f < kF; ++f) in[f] = 0.05 * std::sin(0.4 * f);
    const double* inp[1] = {in.data()};
    std::array<double, kF> postL{}, postR{};
    double* o[2] = {postL.data(), postR.data()};
    CHECK(e.processBlock(inp, o, 1, 2, kF) == StandaloneAudioEngine::Status::Rendered);
    bool same = true;
    for (int f = 0; f < kF; ++f)
      if (std::fabs(postL[f] - refL[f]) > 1e-12 || std::fabs(postR[f] - refR[f]) > 1e-12) { same = false; break; }
    CHECK(same);
  }

  // ---- SEQ 2: DSP reject -> subsequent valid apply -> Accepted (+ diagnostics reset to sentinel).
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(B, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedDspApply);
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)ParameterId::vco_b_morph);
    const auto ok = e.applyDeviceState(A, 48000.0, kF, 1, 2);
    CHECK_EQ((int)ok, (int)StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(e.isReady());
    // "otherwise sentinel" contract (finding #5): a later success rewrites first-fail to sentinel.
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)kParameterCount);
    CHECK_EQ((int)e.dspApplyFirstFailStatus(), (int)ParameterApplyStatus::applied);
    CHECK(e.canonicalState() != nullptr && stateEqual(*e.canonicalState(), A));
  }

  // ---- SEQ 3: DSP reject -> a DIFFERENT (invalid) state -> RejectedInvalidState, atomic.
  {
    DeviceStateV1 badInv = A;
    slot(badInv, ParameterId::vco_a_pw) = 2.0;  // pw is 0..1 -> out-of-domain -> validator rejects.
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(A, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(e.applyDeviceState(B, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedDspApply);
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)ParameterId::vco_b_morph);
    const DeviceStateV1 aCanon = *e.canonicalState();
    const auto other = e.applyDeviceState(badInv, 48000.0, kF, 1, 2);
    CHECK_EQ((int)other, (int)StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState);
    CHECK(e.isReady());
    // A still intact after the other-reject.
    CHECK(e.canonicalState() != nullptr && stateEqual(*e.canonicalState(), aCanon));
    // A RejectedInvalidState terminal never populates the DSP first-fail diagnostics (finding #2):
    // they return to the entry sentinel, NOT the earlier vco_b_morph residue.
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)kParameterCount);
    CHECK_EQ((int)e.dspApplyFirstFailStatus(), (int)ParameterApplyStatus::applied);
  }

  // ---- SEQ 4: DSP reject -> prepare() success -> diagnostics reset to sentinel, engine ready.
  {
    StandaloneAudioEngine e;
    CHECK(e.applyDeviceState(B, 48000.0, kF, 1, 2) == StandaloneAudioEngine::StateApplyStatus::RejectedDspApply);
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)ParameterId::vco_b_morph);
    CHECK(e.prepare(kSeed, 48000.0, kF, 1, 2));
    CHECK(e.isReady());
    // prepare() resets the DSP first-fail diagnostic to the sentinel at entry (lines 280-281).
    CHECK_EQ((int)e.dspApplyFirstFailParamId(), (int)kParameterCount);
    CHECK_EQ((int)e.dspApplyFirstFailStatus(), (int)ParameterApplyStatus::applied);
  }

  std::printf("owner-atomicity RejectedDspApply harness: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
