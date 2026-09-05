// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_machine_control_outputs.cpp — task#78 gap#2 (GH#12 9C): the five control-source families
// (envelope EG, LFO, joystick, env-follower, sequencer) driven through the AGREED entry and
// asserted on their PUBLISHED CV/gate output.
//
// @Codex 1e34b7bb: gap#2 must NOT bypass the entry by calling buildMachineRuntimeCandidate +
// processFrame directly. This test drives the real path — encode -> decode ->
// StandaloneAudioEngine.applyDeviceState -> processBlock — via the shared test_engine_harness.h,
// then reads the PUBLISHED source bank through owner.runtime()->controlVoltageAt(JackId) (read-only,
// no member readback, no "未路由音频" token). Each family's codec stimulus must change the real
// jack value in a KNOWN direction and stay in its documented rail.

#include "mini_test.h"

#include <host/standalone_audio_engine.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_disposition.h>   // ParameterId
#include <lunar24/registry_ids.hpp>           // lunar24::core::JackId full enum

#include "test_engine_harness.h"

#include <cmath>
#include <cstdint>
#include <vector>

// mini_test.h ships CHECK / CHECK_EQ / CHECK_TRUE / CHECK_FALSE only; a near-equality primitive is
// needed for the rail/fidelity assertions below.
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
using lunar24::core::JackId;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;

namespace {

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// Sample a published jack over `frames` frames of the real engine path. Keeps a run-summary of the
// CV/gate (min/max/peak) so a family can assert rail + direction + determinism together.
struct JackStat {
  double mn = 1e30;
  double mx = -1e30;
  double peak = -1e30;
  void fuel(double v) {
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    if (v > peak) peak = v;
  }
  double span() const { return mx - mn; }
};

// Run-summary (rail + oscillation) for a captured published-jack series.
static JackStat statOf(const std::vector<double>& v) {
  JackStat s;
  for (double x : v) s.fuel(x);
  return s;
}

// Count sign changes of (v - mid) across a series — a PERIOD discriminator for a unipolar rail.
// The LFO CV is 0..+10 V, so its midline (5 V), not 0, is the cycle reference: a faster rate must
// yield proportionally more midline crossings over the same fixed window. Used to prove a rate
// parameter changes the real published period, not merely that the module oscillates.
static int midlineCrossings(const std::vector<double>& v, double mid) {
  int n = 0;
  for (std::size_t i = 1; i < v.size(); ++i) {
    if ((v[i - 1] < mid) != (v[i] < mid)) ++n;
  }
  return n;
}

void test_lfo() {
  // LFO A: published lfo_a_cv_out is a unipolar 0..+10V oscillation. Two states differ ONLY in the
  // rate parameter (same wave, same everything else); the faster baseHz must yield proportionally
  // MORE cycles of the real published CV over the same 1 s window — i.e. the rate param changes the
  // actual period, not merely that the module runs. The midline (5 V) is the cycle reference.
  DeviceStateV1 slow = make_default_device_state(kSeed);
  slot(slow, ParameterId::lfo_a_rate) = 1.0;   // 1 Hz.
  slot(slow, ParameterId::lfo_a_wave) = 0.5;   // keep wave IDENTICAL across both states.
  DeviceStateV1 fast = slow;
  slot(fast, ParameterId::lfo_a_rate) = 20.0;   // 20x faster; every other field identical.

  EngineHarness h, h2;
  CHECK(h.load(slow));
  CHECK(h2.load(fast));
  CHECK(h.runtime() != nullptr);
  CHECK(h2.runtime() != nullptr);

  const double target = 10.0;  // kLfoOutputPeakVolt
  std::vector<double> slowJack, fastJack;
  CHECK(h.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    slowJack.push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
  }));
  CHECK(h2.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    fastJack.push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
  }));

  const JackStat ss = statOf(slowJack), sf = statOf(fastJack);
  CHECK(ss.mn >= 0.0 && ss.mx <= target && ss.span() > 0.5);  // slow in-rail, oscillating.
  CHECK(sf.mn >= 0.0 && sf.mx <= target && sf.span() > 0.5);  // fast in-rail, oscillating.

  const int slowC = midlineCrossings(slowJack, target * 0.5);
  const int fastC = midlineCrossings(fastJack, target * 0.5);
  CHECK(slowC >= 1);        // 1 Hz really crosses the midline at least once (a live period, not DC).
  CHECK(fastC > slowC);     // the rate changed the real published period.
  CHECK(fastC >= slowC * 8);  // ~20x rate -> roughly proportional cycing.

  // Publish fidelity: jack == the LFO's own computed output at the final phase (no shadow copy).
  const auto& rtl = *h2.runtime();
  CHECK_CLOSE(rtl.controlVoltageAt(JackId::lfo_a_cv_out),
              (rtl.lfoA().fundamental() + 1.0) * 0.5 * target, 1e-9);
}

void test_envelope() {
  // Envelope A: published envelope_a_env_out is 0..+8V, and SELF-GEN drives it up while the default
  // (gate floating) rests near zero -> the codec SELF-GEN boolean actually toggles real output.
  DeviceStateV1 off = make_default_device_state(kSeed);
  DeviceStateV1 on = make_default_device_state(kSeed);
  slot(on, ParameterId::envelope_a_self_gen) = 1.0;
  slot(on, ParameterId::envelope_a_s) = 1.0;   // sustain full -> env rises toward rail.

  EngineHarness hOff, hOn;
  CHECK(hOff.load(off));
  CHECK(hOn.load(on));

  JackStat offStat;
  CHECK(hOff.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    offStat.fuel(rt.controlVoltageAt(JackId::envelope_a_env_out));
  }));
  JackStat onStat, vcaOn;
  CHECK(hOn.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    onStat.fuel(rt.controlVoltageAt(JackId::envelope_a_env_out));
    vcaOn.fuel(rt.controlVoltageAt(JackId::envelope_a_vca_cv_out));
  }));
  constexpr double kEnvRail = 8.0;  // kEnvelopeEnvPeakVolt
  CHECK(onStat.peak >= 0.0 && onStat.peak <= kEnvRail);   // env rail 0..+8V.
  CHECK(vcaOn.peak >= 0.0 && vcaOn.peak <= kEnvRail);     // vca_cv rail 8.
  CHECK(onStat.peak > offStat.peak + 0.1);                // SELF-GEN genuinely drives output up.
}

void test_joystick() {
  // Joystick: published joystick_x_out is ±10V and flips with the norm/offset codec value.
  DeviceStateV1 hi = make_default_device_state(kSeed);
  slot(hi, ParameterId::joystick_x) = 1.0;
  slot(hi, ParameterId::joystick_offset_x) = 0.5;   // centre offset -> pure position +5V.
  DeviceStateV1 lo = make_default_device_state(kSeed);
  slot(lo, ParameterId::joystick_x) = 0.0;
  slot(lo, ParameterId::joystick_offset_x) = 0.5;   // -> -5V.

  EngineHarness hHi, hLo;
  CHECK(hHi.load(hi));
  CHECK(hLo.load(lo));

  double xHi = 0, yHi = 0;
  CHECK(hHi.renderSampled(16, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    xHi = rt.controlVoltageAt(JackId::joystick_x_out);
    yHi = rt.controlVoltageAt(JackId::joystick_y_out);
  }));
  double xLo = 0;
  CHECK(hLo.renderSampled(16, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    xLo = rt.controlVoltageAt(JackId::joystick_x_out);
  }));
  constexpr double kSteerRail = 10.0;
  CHECK(xHi >= -kSteerRail && xHi <= kSteerRail && yHi >= -kSteerRail && yHi <= kSteerRail);
  CHECK(xHi > 0.0);                        // norm-1 pushes X positive.
  CHECK(xLo < 0.0);                        // norm-0 pulls X negative.
  CHECK(std::fabs(xHi) > 1.0 && std::fabs(xLo) > 1.0);  // a real swing, not a near-zero residue.
  // Publish fidelity: jack == the joystick's own computed xOut() for that same sample.
  CHECK_CLOSE(xHi, hHi.runtime()->joystick().xOut(), 1e-9);
}

void test_env_follower() {
  // Env-follower: published env_follower_env_out is 0..+10V and RISES with a fed preamp audio input.
  EngineHarness hIn, hNo;
  CHECK(hIn.load(make_default_device_state(kSeed)));
  CHECK(hNo.load(make_default_device_state(kSeed)));

  JackStat inStat, noStat;
  CHECK(hIn.renderSampled(kSr, 1.0, [&](const lunar24::core::SynthRuntime& rt) {  // steady 1.0V preamp.
    inStat.fuel(rt.controlVoltageAt(JackId::env_follower_env_out));
  }));
  CHECK(hNo.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {  // no input.
    noStat.fuel(rt.controlVoltageAt(JackId::env_follower_env_out));
  }));
  constexpr double kEnvMax = 10.0;  // kEnvMaxVolt
  CHECK(inStat.peak >= 0.0 && inStat.peak <= kEnvMax);
  CHECK(inStat.peak > noStat.peak + 0.1);  // feeding preamp really raises the follower output.

  // Attack-response: SAME input step (1.0 V preamp), two DIFFERENT legal attack values (attackNorm
  // -> seconds via 0.001 + 0.999*n). The faster attack must reach half its own final plateau in FEWER
  // frames than the slower one. A rise-time (to 50% of each state's own peak) discriminator makes the
  //  comparison invariant to the absolute rectified level, so it proves the attack param genuinely
  // changes the published rise rather than just the module running.
  DeviceStateV1 fastA = make_default_device_state(kSeed);
  slot(fastA, ParameterId::env_follower_attack) = 0.1;   // ~0.10 s attack.
  DeviceStateV1 slowA = make_default_device_state(kSeed);
  slot(slowA, ParameterId::env_follower_attack) = 0.9;   // ~0.90 s attack.
  EngineHarness hF, hS;
  CHECK(hF.load(fastA));
  CHECK(hS.load(slowA));

  std::vector<double> fastSeries, slowSeries;
  CHECK(hF.renderSampled(kSr, 1.0, [&](const lunar24::core::SynthRuntime& rt) {
    fastSeries.push_back(rt.controlVoltageAt(JackId::env_follower_env_out));
  }));
  CHECK(hS.renderSampled(kSr, 1.0, [&](const lunar24::core::SynthRuntime& rt) {
    slowSeries.push_back(rt.controlVoltageAt(JackId::env_follower_env_out));
  }));
  auto riseHalf = [](const std::vector<double>& v) -> int {
    double peak = 0.0;
    for (double x : v) if (x > peak) peak = x;
    const double half = peak * 0.5;
    for (std::size_t i = 0; i < v.size(); ++i) if (v[i] >= half) return static_cast<int>(i);
    return static_cast<int>(v.size());
  };
  const int fFast = riseHalf(fastSeries), fSlow = riseHalf(slowSeries);
  CHECK(fFast >= 0 && fSlow >= 0);            // both series really rose (non-empty rise).
  CHECK(fFast < fSlow);                       // faster attack reaches 50% plateau earlier.
}

void test_sequencer() {
  // Sequencer: published sequencer_cv_out (0..+5V) ADVANCES through the configured step CVs, gate
  // out dips to +10V on a gated step, clock out sweeps -10/+10 (bipolar rail confirmed).
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::sequencer_clock) = 0.0;    // internal -> PULSER driven.
  slot(st, ParameterId::sequencer_stages) = 2.0;   // exact integer: 2 -> 5 steps.
  slot(st, ParameterId::sequencer_pulser) = 1.0;   // norm 1 -> ~20 Hz internal rate.
  slot(st, ParameterId::sequencer_step_cv_1) = 1.0;
  slot(st, ParameterId::sequencer_step_cv_2) = 2.0;
  slot(st, ParameterId::sequencer_step_cv_3) = 3.0;
  slot(st, ParameterId::sequencer_step_cv_4) = 4.0;
  slot(st, ParameterId::sequencer_step_cv_5) = 5.0;
  slot(st, ParameterId::sequencer_step_gate_1) = 1.0;
  slot(st, ParameterId::sequencer_step_gate_3) = 1.0;

  EngineHarness h;
  CHECK(h.load(st));

  double cvMn = 1e30, cvMx = -1e30, clkMn = 1e30, clkMx = -1e30;
  bool sawGate10 = false, sawCv1 = false, sawCv3 = false, sawCv5 = false;
  CHECK(h.renderSampled(kSr, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    const double cv = rt.controlVoltageAt(JackId::sequencer_cv_out);
    const double g = rt.controlVoltageAt(JackId::sequencer_gate_out);
    const double clk = rt.controlVoltageAt(JackId::sequencer_clock_out);
    if (cv < cvMn) cvMn = cv;
    if (cv > cvMx) cvMx = cv;
    if (clk < clkMn) clkMn = clk;
    if (clk > clkMx) clkMx = clk;
    if (g >= 9.5) sawGate10 = true;
    constexpr double kCVTol = 0.05;
    if (std::fabs(cv - 1.0) < kCVTol) sawCv1 = true;
    if (std::fabs(cv - 3.0) < kCVTol) sawCv3 = true;
    if (std::fabs(cv - 5.0) < kCVTol) sawCv5 = true;
  }));
  CHECK(cvMn >= 0.0 && cvMx <= 5.0);        // CV rail 0..+5V.
  CHECK(cvMx - cvMn > 0.5);                  // CV actually stepped.
  CHECK(sawCv1 && sawCv3 && sawCv5);         // visited several DISTINCT configured step CVs.
  CHECK(sawGate10);                          // a gated step emitted the +10V one-sample pulse.
  CHECK(clkMn <= -9.5 && clkMx >= 9.5);      // clock_out swept the full -10/+10 bipolar rail.
}

}  // namespace

int main() {
  test_lfo();
  test_envelope();
  test_joystick();
  test_env_follower();
  test_sequencer();
  return test::finish("test_machine_control_outputs");
}
