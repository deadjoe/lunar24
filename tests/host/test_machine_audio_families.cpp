// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_machine_audio_families.cpp — task#78 gap#3 (GH#12 9C): VCO init + the six audio families,
// including the NEW drone_3/6 applied_to_dsp parameters (which the earlier /tmp audio harness
// omitted). Driven through the AGREED entry — encode -> decode -> StandaloneAudioEngine
// applyDeviceState -> processBlock — via the shared test_engine_harness.h, asserted on the REAL
// output buffers the DeviceAdapter writes (WET_L=0, DRY_A=2, DRY_B=3) and on the published runtime
// getters (baseHz-live VCO-A, drone mixer-bus taps). No direct buildMachineRuntimeCandidate route.
//
// Metric notes (calibrated through this exact path, ground-truth probe):
//   * DRY A/B are scale-clamped at ±0.5 (peak is constant there) — their FREQUENCY (zero-crossing
//     rate) is the discriminator. WET is un-saturated, so PEAK is the discriminator for the
//     level families (mixer / preamp / vcf) and ZCR is a strong one for the sweep family.
//   * The NEW drone_3/6 channels are tapped before the mixer, so their published mixer-bus value
//     (drone3Channel()/drone6Channel()) is the real per-sample value feeding WET; that getter is
//     the reliable discriminator for a drone param (peak-abs is volume-scale-flat).

#include "mini_test.h"

#include <host/standalone_audio_engine.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_disposition.h>   // ParameterId
#include <lunar24/core/machine_runtime.h>     // SynthRuntime, VcoControlMode
#include <lunar24/core/device_layout.h>

#include "test_engine_harness.h"

#include <cmath>
#include <cstdint>
#include <vector>

// mini_test.h ships CHECK / CHECK_EQ / CHECK_TRUE / CHECK_FALSE only; a near-equality primitive is
// needed for the VCO init / rail assertions below.
#ifndef CHECK_CLOSE
#define CHECK_CLOSE(a, b, tol)                                                \
  do {                                                                         \
    const auto _aa = (a);                                                      \
    const auto _bb = (b);                                                      \
    const double _tol = (tol);                                                 \
    ::test::report(std::fabs(_aa - _bb) <= _tol, #a " ~= " #b, __FILE__, __LINE__); \
  } while (0)
#endif

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::VcoControlMode;
using lunar24::core::make_default_device_state;

namespace {

constexpr int kF = 4800;  // 0.1 s @48k.

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// Default-state WET peak, for the drone classic reachability assertion.
double hDefWet() {
  EngineHarness h;
  if (!h.load(make_default_device_state(kSeed))) return 0.0;
  if (!h.render(kF)) return 0.0;
  return peakOf(h.wetL());
}

// Mean absolute difference between two same-length traces — a trace-level "did the audio actually
// change" metric that avoids the peak (scale-clamped ±0.5 on DRY) and zcr (frequency-only) blind
// spots. Used to prove a classic drone tune change reaches the REAL WET buffer, not just the
// pre-mixer bus tap.
double traceDiff(const std::vector<double>& a, const std::vector<double>& b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) s += std::fabs(a[i] - b[i]);
  return n ? s / n : 0.0;
}

// --- 1. VCO INIT --------------------------------------------------------------------------
// The authoritative registry defaults land in the committed runtime; baseHz=440 is live (DRY_A
// actually oscillates at audio rate, not a DC 0).
void test_vco_init() {
  EngineHarness h;
  CHECK(h.load(make_default_device_state(kSeed)));
  CHECK(h.runtime() != nullptr);
  const auto& rt = *h.runtime();
  CHECK_CLOSE(rt.vcoATune(), 0.0, 1e-9);   CHECK_CLOSE(rt.vcoBTune(), 0.0, 1e-9);
  CHECK_CLOSE(rt.vcoAMorph(), 0.5, 1e-9);  CHECK_CLOSE(rt.vcoBMorph(), 0.5, 1e-9);
  CHECK_CLOSE(rt.vcoAPw(), 0.5, 1e-9);     CHECK_CLOSE(rt.vcoBPw(), 0.5, 1e-9);
  CHECK_EQ(rt.vcoAOctSelect(), 0);         CHECK_EQ(rt.vcoBOctSelect(), 0);
  CHECK_EQ(rt.vcoASubSelect(), 0);         CHECK_EQ(rt.vcoBSubSelect(), 0);
  CHECK_CLOSE(rt.vcoACvAmt(), 1.0, 1e-9);  CHECK_CLOSE(rt.vcoBCvAmt(), 1.0, 1e-9);
  // lin_exp registry default is 0 -> the codec apply sets the mode to kLinear.
  CHECK(rt.vcoAControlMode() == VcoControlMode::kLinear);
  CHECK(rt.vcoBControlMode() == VcoControlMode::kLinear);
  // baseHz=440 is a LIVE oscillator, not silent: DRY_A really oscillates.
  CHECK(h.render(kF));
  CHECK(zcrOf(h.dryA()) >= 10);
}

// --- 2. VCO-A / 3. VCO-B ------------------------------------------------------------------
void test_vco_voices() {
  EngineHarness hDef;
  CHECK(hDef.load(make_default_device_state(kSeed)));
  CHECK(hDef.render(kF));
  const int a0 = zcrOf(hDef.dryA());   // VCO-A audio-frequency, default.
  const int b0 = zcrOf(hDef.dryB());   // VCO-B default linear full-depth self-edge -> near-static.

  // VCO-A: vco_a_tune moves DRY_A's zero-crossing rate.
  EngineHarness hA;
  DeviceStateV1 a = make_default_device_state(kSeed);
  slot(a, ParameterId::vco_a_tune) = 0.5;
  CHECK(hA.load(a));
  CHECK(hA.render(kF));
  CHECK(a0 >= 10);                       // baseHz 440 live (not 0).
  CHECK(zcrOf(hA.dryA()) != a0);         // tune moved VCO-A frequency.

  // VCO-B: default self-edge stall (zcr ~1); cv_amt=0 OR lin_exp=1 restores oscillation; the
  // recovered VCO-B then responds to vco_b_tune.
  CHECK(b0 <= 4);                        // default near-static self-edge.
  {
    EngineHarness hB;
    DeviceStateV1 r = make_default_device_state(kSeed);
    slot(r, ParameterId::vco_b_cv_amt) = 0.0;
    CHECK(hB.load(r));
    CHECK(hB.render(kF));
    const int bR = zcrOf(hB.dryB());
    CHECK(bR >= 15);                     // cv_amt=0 restores oscillation.
    CHECK(bR != b0);
    EngineHarness hBt;
    DeviceStateV1 r2 = r;
    slot(r2, ParameterId::vco_b_tune) = 0.5;
    CHECK(hBt.load(r2));
    CHECK(hBt.render(kF));
    CHECK(zcrOf(hBt.dryB()) != bR);      // recovered VCO-B responds to tune.
  }
  {
    EngineHarness hLin;
    DeviceStateV1 r = make_default_device_state(kSeed);
    slot(r, ParameterId::vco_b_lin_exp) = 1.0;
    CHECK(hLin.load(r));
    CHECK(hLin.render(kF));
    CHECK(zcrOf(hLin.dryB()) >= 15);     // lin_exp=1 also restores oscillation.
  }
}

// --- 4. MIXER / 5. VCF / 6. PREAMP ---------------------------------------------------------
void test_level_families() {
  // MIXER: mixer_ch5_vol (channel 4 == VCO-A) moves the real WET level.
  {
    DeviceStateV1 m0 = make_default_device_state(kSeed);
    slot(m0, ParameterId::mixer_ch5_vol) = 0.0;
    DeviceStateV1 m1 = make_default_device_state(kSeed);
    slot(m1, ParameterId::mixer_ch5_vol) = 0.7;
    EngineHarness h0, h1;
    CHECK(h0.load(m0)); CHECK(h1.load(m1));
    CHECK(h0.render(kF)); CHECK(h1.render(kF));
    CHECK(std::fabs(peakOf(h1.wetL()) - peakOf(h0.wetL())) > 0.005);
  }
  // VCF: vcf_l_freq strongly changes WET (peak AND crossing rate both jump with a wide-open cutoff).
  {
    DeviceStateV1 vLo = make_default_device_state(kSeed);
    slot(vLo, ParameterId::vcf_l_freq) = 0.1;
    DeviceStateV1 vHi = make_default_device_state(kSeed);
    slot(vHi, ParameterId::vcf_l_freq) = 0.9;
    EngineHarness hLo, hHi;
    CHECK(hLo.load(vLo)); CHECK(hHi.load(vHi));
    CHECK(hLo.render(kF)); CHECK(hHi.render(kF));
    CHECK(peakOf(hHi.wetL()) > peakOf(hLo.wetL()) * 1.5);     // cutoff opens -> much bigger peak.
    CHECK(zcrOf(hHi.wetL()) > zcrOf(hLo.wetL()));             // and far more WET crossings.
  }
  // PREAMP: with a steady 1.0 V preamp feed, preamp_gain genuinely moves the real WET level.
  {
    DeviceStateV1 p0 = make_default_device_state(kSeed);
    slot(p0, ParameterId::preamp_gain) = 0.0;
    DeviceStateV1 p1 = make_default_device_state(kSeed);
    slot(p1, ParameterId::preamp_gain) = 0.9;
    EngineHarness h0, h1;
    CHECK(h0.load(p0)); CHECK(h1.load(p1));
    CHECK(h0.render(kF, 1.0)); CHECK(h1.render(kF, 1.0));   // hold a 1.0 V preamp feed on ch1.
    CHECK(std::fabs(peakOf(h1.wetL()) - peakOf(h0.wetL())) > 0.05);
  }
}

// --- 7. DRONE classic voice 1 --------------------------------------------------------------
void test_drone_classic() {
  // The drone's real mixer-bus audio (droneChannel) responds to tune (frequency) and mute (level);
  // raising its mixer channel makes it audible in the real WET buffer.
  DeviceStateV1 d0 = make_default_device_state(kSeed);
  slot(d0, ParameterId::mixer_ch1_vol) = 1.0;
  DeviceStateV1 d1 = d0;
  slot(d1, ParameterId::drone_1_tune_1) = 0.9;
  DeviceStateV1 dm = d1;
  slot(dm, ParameterId::drone_1_mute_1) = 1.0;

  EngineHarness h0, h1, hm;
  CHECK(h0.load(d0)); CHECK(h1.load(d1)); CHECK(hm.load(dm));
  CHECK(h0.render(kF)); CHECK(h1.render(kF)); CHECK(hm.render(kF));

  const double v0 = h0.runtime()->droneChannel(0);
  const double v1 = h1.runtime()->droneChannel(0);
  const double vM = hm.runtime()->droneChannel(0);
  CHECK(std::fabs(v1 - v0) > 1e-3);            // tune moved the real mixer-bus audio.
  CHECK(vM < v1);                              // mute lowered the drone's real audio.
  CHECK(peakOf(h0.wetL()) > hDefWet() + 0.02);  // voice reaches WET once its vol is up.

  // Same route (mixer_ch1_vol=1.0) and SAME volume, ONLY the classic drone tune differs: the REAL
  // WET trace must differ too. This is the param -> final-audio connection: the tune change is
  // audible in the actual output buffer, not just the pre-mixer bay tap.
  CHECK(traceDiff(h0.wetL(), h1.wetL()) > 1e-3);
}

// --- 8. NEW drone_3 / 9. NEW drone_6 --------------------------------------------------------
void test_drone_new() {
  // NEW applied_to_dsp params: drone_3_pitch / drone_6_pitch change the real mixer-bus tap
  // (drone3Channel/drone6Channel) and the real WET buffer after the voice is routed to the mix.
  // (These are the parameters the earlier /tmp audio harness omitted — added per @Codex 1e34b7bb.)
  {
    DeviceStateV1 base3 = make_default_device_state(kSeed);
    slot(base3, ParameterId::mixer_ch3_vol) = 1.0;          // drone_3 -> channel 2.
    DeviceStateV1 p3 = base3;
    slot(p3, ParameterId::drone_3_pitch) = 0.9;
    EngineHarness hB, hP;
    CHECK(hB.load(base3)); CHECK(hP.load(p3));
    CHECK(hB.render(kF)); CHECK(hP.render(kF));
    CHECK(std::fabs(hP.runtime()->drone3Channel() - hB.runtime()->drone3Channel()) > 1e-3);
    CHECK(std::fabs(peakOf(hP.wetL()) - peakOf(hB.wetL())) > 1e-3);   // real WET moved too.
  }
  {
    DeviceStateV1 base6 = make_default_device_state(kSeed);
    slot(base6, ParameterId::mixer_ch10_vol) = 1.0;         // drone_6 -> channel 9.
    DeviceStateV1 p6 = base6;
    slot(p6, ParameterId::drone_6_pitch) = 0.9;
    EngineHarness hB, hP;
    CHECK(hB.load(base6)); CHECK(hP.load(p6));
    CHECK(hB.render(kF)); CHECK(hP.render(kF));
    CHECK(std::fabs(hP.runtime()->drone6Channel() - hB.runtime()->drone6Channel()) > 1e-3);
    CHECK(std::fabs(peakOf(hP.wetL()) - peakOf(hB.wetL())) > 1e-3);   // real WET moved too.
  }
}

}  // namespace

int main() {
  test_vco_init();
  test_vco_voices();
  test_level_families();
  test_drone_classic();
  test_drone_new();
  return test::finish("test_machine_audio_families");
}
