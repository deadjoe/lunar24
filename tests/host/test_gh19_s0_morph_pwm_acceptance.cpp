// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_gh19_s0_morph_pwm_acceptance.cpp — task #117 (GH #19 S0): the ACCEPTANCE surface for the
// continuous waveform-mapping single knob and the two-sided PWM product entry.
//
// Everything here runs through the ONE agreed entry (test_engine_harness.h):
//     encode_device_state -> decode_device_state -> StandaloneAudioEngine::applyDeviceState ->
//     StandaloneAudioEngine::processBlock,
// and asserts on the REAL captured output channels (WET_L/WET_R/DRY_A/DRY_B) and on the read-only
// published runtime. No test here calls buildMachineRuntimeCandidate/processFrame directly.
//
// TWO RULED TRANSFERS ARE UNDER TEST (design/00-status.md, the "GH#19 S0" ruling block):
//   (a) the single continuous morph knob maps to a linear partition-of-unity blend over the five
//       waveform nodes at coordinates {0, .25, .5, .75, 1} = {saw, inverted saw, sine, triangle,
//       pulse}; and
//   (b) the PWM SOFTWARE transfer  effectiveDuty = clamp(basePW + depth * cvVolts / 10, 0.001, 0.999)
//       with positive CV raising duty — explicitly NOT a hardware measurement.
//
// MEASURED BASIS. Every threshold below is a measured value quoted with its margin, taken through
// this same entry (never modelled):
//   * node audio signatures at 48 kHz, over whole measured periods (phase-robust aggregates):
//       norm 0.00 saw        meanAbs 0.249998  medSlope +0.004583
//       norm 0.25 inv-saw    meanAbs 0.249998  medSlope -0.004583   (exact mirror of the saw)
//       norm 0.50 sine       meanAbs 0.318312  (= 2/pi * 0.5)      <- the uniquely high-load node
//       norm 0.75 triangle   meanAbs 0.250002  pp 0.996285         <- the only node with pp < 1
//       norm 1.00 pulse      meanAbs 0.500000  rms 0.500000
//   * fine sweep in steps of 0.01: worst adjacent |d meanAbs| = 0.020000, |d rms| = 0.023094.
//   * DC duty law through joystick.x_out -> vco_a.pwm_in at the INTERMEDIATE norms (0.25/0.5/0.75)
//     at all four rates: worst |audio - law| 0.0022, measured with the zero-crossing estimator.
//   * THE TWO CLAMP ENDPOINTS (norms 0.00 / 1.00) are measured by the FIXED-PHASE TWO-EDGE
//     REFERENCE, not by zero crossings — at duty 0.001/0.999 the corrected pulse has no zero
//     crossing left to count (see the instrument header below). Measured, all four rates:
//     worst |DRY - reference| = 8.8e-12 device units, i.e. double round-off, against a tolerance of
//     0.02; the rejected neighbours sit at 1.000 (skipped PWM), 1.000 (a wrong clamp value), 1.000
//     (the mirror endpoint), and 0.158..0.388 (the nearest plausible mis-clamp) — every one of them
//     printed per cell, none tabulated. The consumer's duty equals the independently computed law
//     on its own input to 0.0 at all four rates, and the raw pre-clamp duty lands strictly outside
//     the window, so the clamp is genuinely exercised in both cells.
//   * THE SAME TWO-EDGE OBSERVATION ON VCO B. `endpointDutyAt` is parameterised over the side; B is
//     driven through its OWN PWM sink with the default A->B route isolated (vco_b.cv_amt = 0), and
//     the NON-driven side's sink is read in the same runs, so "this endpoint is B's" is measured
//     rather than assumed. Measured at 48 kHz: both endpoints land on the clamped duty exactly
//     (0.001000000 / 0.999000000), the DC means are the mirror pair (-0.499001 / +0.499001), worst
//     |DRY_B - reference| = 1.0e-12 device units against the same 0.02 tolerance, the rejected
//     neighbours sit at 1.000 (skipped PWM, a wrong clamp value, the mirror endpoint) and
//     0.190 / 0.205 (the nearest plausible mis-clamp), the declared and measured period agree
//     (0.004583333 vs 0.004583344), and the non-driven sink read exactly 0.0 throughout. B's two
//     endpoint criteria are separately shown to be load-bearing by two production mutants that leave
//     every A-side check green (tools/run_gh19_s0_mutants.py: acc-endpoint-b-pwm-not-consumed,
//     acc-endpoint-b-wrong-side-cable).
//   * CHANGING CV (lfo_a.cv_out -> vco_a.pwm_in, lfo_a.rate 5 Hz hence a measured 5.0000 Hz period,
//     depth 0.1): worst per-period |audio duty - implied transfer| = 0.00500.
//   * same-frame consumption: the sink readback equals the SAME frame's published source volts in
//     4799/4799 frames and the PREVIOUS frame's value in 0/4799 — so the lag-1 arm is a real
//     discriminator, not a formality (a one-sample-late consumer flips both counts).
//   * the default A->B normalised route LEGITIMATELY drives B: B's audio high-time fraction is not
//     its duty (B is frequency-modulated by A). B is isolated with vco_b.cv_amt = 0, and the
//     default-route modulation is asserted separately as legitimate, never as "no crosstalk".

#include "mini_test.h"

#include <lunar24/core/device_state.h>      // DeviceStateV1
#include <lunar24/core/machine_definition.h>  // kVcoBaseHzProvisional (the product's f0)
#include <lunar24/core/vco.h>                 // Vco::kLowOctave / kZeroOctave / kPlus3Octave,
                                              // kPwDutyMin / kPwDutyMax / kPwmCvFullScaleVolts
#include <lunar24/core/state_default.h>     // make_default_device_state
#include <lunar24/core/state_serializer.h>  // encode/decode_device_state
#include <lunar24/registry_ids.hpp>         // ParameterId / JackId full enums

#include "test_engine_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

// The allocator counters live in the SHARED allocator TU (test_state_apply_oracle_allocator.cpp,
// built into this target alongside this file — see CMakeLists.txt). It is reused rather than copied
// so the project keeps ONE replacement pair to audit. The definition must NOT be in this TU: a
// replaced operator new in the same TU as the ::operator new call sites trips GCC's
// -Wmismatched-new-delete, which this project builds as an error.
extern std::size_t g_allocCount;
extern std::size_t g_freeCount;

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::JackId;
using lunar24::core::ParameterId;
using lunar24::host::StandaloneAudioEngine;
using lunar24::core::make_default_device_state;

namespace {

// ---------------------------------------------------------------------------------------------
// Fixtures taken FROM THE PRODUCER, never hand-written.
// ---------------------------------------------------------------------------------------------

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// The descriptor's own declared initial — the "default values unchanged" anchor.
double declaredInitial(ParameterId id) {
  const auto* d = lunar24::core::find_parameter(id);
  return d != nullptr ? d->initial : -1.0;
}

void setCable(DeviceStateV1& st, JackId src, JackId sink) {
  const auto s = static_cast<std::uint32_t>(sink);
  st.inputCable[s] = 1;
  st.cableSource[s] = src;
}

// Rising zero crossings — an INDEPENDENT INDEX into the buffer. Absolute-timing facts must be read
// from an index like this one; a residual/gap metric is blind to a whole-sample time shift.
std::vector<std::size_t> risingEdges(const std::vector<double>& c) {
  std::vector<std::size_t> e;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if (c[i - 1] <= 0.0 && c[i] > 0.0) e.push_back(i);
  }
  return e;
}

// The five measured morph node signatures, as a phase-robust profile.
struct Profile {
  double meanAbs = 0.0;
  double rms = 0.0;
  double medSlope = 0.0;
  double pp = 0.0;
  bool ok = false;
};

// Aggregate over WHOLE measured periods (first to last measured rising edge), so the profile does
// not depend on where in the cycle the capture started.
Profile profileOf(const std::vector<double>& c) {
  Profile p;
  const auto e = risingEdges(c);
  if (e.size() < 3) return p;
  const std::size_t e0 = e.front(), eN = e.back();
  double s = 0.0, s2 = 0.0, mn = 1e9, mx = -1e9;
  std::vector<double> d;
  d.reserve(eN - e0);
  for (std::size_t i = e0; i < eN; ++i) {
    s += std::fabs(c[i]);
    s2 += c[i] * c[i];
    if (c[i] < mn) mn = c[i];
    if (c[i] > mx) mx = c[i];
    if (i + 1 < eN) d.push_back(c[i + 1] - c[i]);
  }
  const double n = double(eN - e0);
  p.meanAbs = s / n;
  p.rms = std::sqrt(s2 / n);
  p.pp = mx - mn;
  std::sort(d.begin(), d.end());
  p.medSlope = d.empty() ? 0.0 : d[d.size() / 2];
  p.ok = true;
  return p;
}

// Mean high-time fraction over whole measured periods — the AUDIO duty, independent of any readback.
double meanAudioDuty(const std::vector<double>& c, double* outMin = nullptr,
                     double* outMax = nullptr) {
  const auto e = risingEdges(c);
  if (e.size() < 2) return -1.0;
  double acc = 0.0, mn = 1e9, mx = -1e9;
  int cnt = 0;
  for (std::size_t k = 0; k + 1 < e.size(); ++k) {
    std::size_t hi = 0;
    for (std::size_t i = e[k]; i < e[k + 1]; ++i) {
      if (c[i] > 0.0) ++hi;
    }
    const double d = double(hi) / double(e[k + 1] - e[k]);
    acc += d;
    ++cnt;
    if (d < mn) mn = d;
    if (d > mx) mx = d;
  }
  if (outMin) *outMin = mn;
  if (outMax) *outMax = mx;
  return cnt ? acc / double(cnt) : -1.0;
}

// The ruled PWM software transfer. Named once, used everywhere.
double pwmTransfer(double basePw, double depth, double cvVolts) {
  const double d = basePw + depth * cvVolts / 10.0;
  return d < 0.001 ? 0.001 : (d > 0.999 ? 0.999 : d);
}

// WHICH VCO an endpoint arm drives. The endpoint instrument is parameterised over this so the SAME
// observation is made on both sides rather than only on A. Both builders below DEFAULT to A, so every
// pre-existing call site keeps the exact stimulus it had.
enum class EndpointSide { A, B };

// A pulse-node state with B isolated (vco_b.cv_amt = 0), so B's own duty claims are not confounded by
// the legitimate default A->B frequency modulation. `depth` is written to the CHOSEN side's PWM depth
// knob; the other side keeps its own default, so an A arm and a B arm differ only in their side.
DeviceStateV1 pulseState(double depth, EndpointSide side = EndpointSide::A) {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::vco_a_morph) = 1.0;  // pulse node
  slot(st, ParameterId::vco_b_morph) = 1.0;
  slot(st, ParameterId::vco_b_cv_amt) = 0.0;  // isolate B from the default A->B route
  if (side == EndpointSide::A) {
    slot(st, ParameterId::vco_a_pwm) = depth;
  } else {
    slot(st, ParameterId::vco_b_pwm) = depth;
  }
  return st;
}

// =============================================================================================
// THE ENDPOINT INSTRUMENT (GH#19 S3 consequence; ruled by @Codex msg ce80e516, option (a)).
//
// WHY THE OLD OBSERVATION DIED. `meanAudioDuty` above recovers the duty from ZERO CROSSINGS: it
// counts the samples above 0 between consecutive rising edges. That estimator is only valid while
// the pulse actually crosses zero. The ruled transfer CLAMPS at 0.001 / 0.999, and at those two
// endpoints the naive high window is 0.001 of a cycle — about 0.2 of one sample at 220 Hz, 0.9 of
// one at 440 Hz / 48 kHz — i.e. NARROWER THAN THE BAND-LIMITING KERNEL ITSELF (w = 1/218 cycle at
// 220 Hz). The two-edge correction therefore lifts the whole window across zero and the corrected
// waveform never becomes positive at all (measured range [-1.0000, -0.5881] at duty 0.001). The old
// instrument read "no zero crossing" as "PWM not consumed"; that was the instrument failing, not the
// product. The old duty guard is NOT thereby rehabilitated: it introduced a parameter-boundary jump
// and stays withdrawn.
//
// WHAT REPLACES IT. A FIXED-PHASE two-edge output reference. This file writes out the product's own
// declared pulse law
//
//     v(p) = naive(p, duty) + R(p, w) - R(frac(p - duty), w),   naive = (p < duty) ? +1 : -1
//
// and predicts the real DRY sequence sample by sample at the phase the PRODUCT's own frame
// convention fixes: `Vco::tick()` advances the accumulator and then publishes (`cumPitch_ += step`
// BEFORE `emittedAt_(cumPitch_, step)`), so with an accumulator starting at 0 the sample at frame i
// is read at phase frac((i+1)*w). That phase is not searched for and not fitted.
//
// The duty is built the way the ruling asks — INDEPENDENTLY, from the known base width, the depth
// the knob actually reports, and the CV each frame publishes through the graph — and then checked
// against what the consumer actually used. The reference is compared against the real DRY capture at
// several discrimination arms, so passing means the emitted waveform IS the endpoint's waveform and
// is NOT one of its neighbours:
//   * skipped PWM  — the sink never consumed the CV, so the duty stayed at the unmodulated 0.5
//   * wrong value  — a plausible but wrong clamp (0.05 / 0.95)
//   * wrong side   — the mirror endpoint (1 - duty)
//   * nearby clamp — the nearest plausible mis-clamp (0.002 / 0.998)
// The last two are what make "the clamp is in force AND it sits at 0.001/0.999" an AUDIO statement
// rather than a restatement of the readback. Readback alone is not accepted as sound evidence.
//
// Separations are MEASURED, and every one of them is printed, not tabulated.
// =============================================================================================
inline constexpr double kEndpointResidualTol = 0.02;  // device units
inline constexpr double kEndpointMargin = 0.10;       // minimum separation from a rejected arm

// `polyblepResidual` as the kernel defines it (core/include/lunar24/core/polyblep_kernel.h),
// written out here so the reference does not call the code path it is checking.
double refResidual(double t, double dt) {
  const double w = dt < 0.5 ? dt : 0.5;
  if (!(w > 0.0)) return 0.0;
  if (t < w) {
    const double x = t / w;
    return x + x - x * x - 1.0;
  }
  if (t > 1.0 - w) {
    const double x = (t - 1.0) / w;
    return x * x + x + x + 1.0;
  }
  return 0.0;
}

// The naive two-rail shape, optionally carrying the two-edge correction. Returns the VCO's own
// +-1 swing (the DRY capture is this times the device scale).
double twoEdgeSwing(double phase, double duty, double w, bool corrected = true, double wOverride = -1.0) {
  const double p = phase - std::floor(phase);
  const double naive = (p < duty) ? 1.0 : -1.0;
  if (!corrected) return naive;
  const double ww = wOverride > 0.0 ? wOverride : w;
  double b = p - duty;
  b -= std::floor(b);
  return naive + (refResidual(p, ww) - refResidual(b, ww));
}

struct EndpointEvidence {
  EndpointSide side = EndpointSide::A;  // which VCO this evidence was measured on
  bool rendered = false;
  bool consumerSawThePublishedCv = false;   // every frame: the sink read this frame's published V
  bool consumerDutyIsTheRuledLaw = false;   // every frame: used duty == the ruled law on its OWN input
  bool knobDepthReachedItsSetting = false;  // the smoothed PWM depth settled to 1.0
  bool rawDutyIsOutsideTheClampWindow = false;  // so the clamp is actually exercised
  bool theReferenceSquareWaveWasFound = false;  // the duty-0.5 arm really produced edges
  bool phaseAnchorAgrees = false;   // declared pitch law vs the period measured from clean edges
  double wDeclared = 0.0, wMeasured = 0.0;
  double dSource = 0.0;    // independent: base + depth * publishedCv / 10, clamped
  double dConsumer = 0.0;  // the duty the VCO actually used
  double otherSinkMaxAbs = 0.0;  // the NON-driven side's PWM sink readback, max |.| over the run
  double dcActual = 0.0, dcReference = 0.0;
  // Fixed-phase residuals of the real DRY capture against the reference and against each reject arm.
  double rExact = 0.0, rOneStepOff = 0.0, rSkippedPwm = 0.0, rWrongValue = 0.0, rWrongSide = 0.0,
         rNearbyClamp = 0.0;
  double wrongValueDuty = 0.0, nearbyClampDuty = 0.0, mirrorDuty = 0.0;
  int windowFrames = 0, windowStart = 0;
};

// The step the PRODUCT advances by, from its own declared pitch law: `Vco::tick` uses
// step = frequencyHz() / sr, and frequencyHz() = baseHz * 2^(octave + tune) * 2^(vOct) * cv-scaling.
// These arms patch the PWM jack ONLY, so the CHOSEN side's pitch inputs are unpatched (vOct = cv = 0
// on A; on B the default A->B route is switched off by `vco_b.cv_amt = 0`, which is what makes B's
// pitch inputs equally unpatched) and the linear-FM depth is at its default. That reasoning is not
// trusted on its own: `endpointDutyAt` re-measures the period from a clean duty-0.5 arm and requires
// the two to agree, on whichever side it was asked to drive.
double declaredStep(const EngineHarness& h, double sr, EndpointSide side = EndpointSide::A) {
  const lunar24::core::SynthRuntime* rt = h.runtime();
  const double octs[3] = {lunar24::core::Vco::kLowOctave, lunar24::core::Vco::kZeroOctave,
                          lunar24::core::Vco::kPlus3Octave};
  const int sel = (side == EndpointSide::A) ? rt->vcoAOctSelect() : rt->vcoBOctSelect();
  const double tune = (side == EndpointSide::A) ? rt->vcoATune() : rt->vcoBTune();
  const double oct = octs[(sel < 0 || sel > 2) ? 1 : sel];
  return lunar24::core::kVcoBaseHzProvisional * std::pow(2.0, oct + tune) / sr;
}

EndpointEvidence endpointDutyAt(double sr, double norm, EndpointSide side = EndpointSide::A) {
  EndpointEvidence ev;
  ev.side = side;
  const bool onB = (side == EndpointSide::B);
  const double base = declaredInitial(onB ? ParameterId::vco_b_pw : ParameterId::vco_a_pw);
  const double depth = 1.0;
  const int frames = 40000;

  // ---- (1) the REFERENCE arm: same state and rate, PWM cable REMOVED, so the duty is the
  // unmodulated base width (0.5) and the zero crossings are intact. Its only job is to measure the
  // period independently of the declared pitch law above.
  {
    DeviceStateV1 st = pulseState(depth, side);
    EngineHarness h;
    h.reserve(static_cast<std::size_t>(frames));
    if (!h.load(st, sr) || !h.render(frames)) return ev;
    const auto e = risingEdges(onB ? h.dryB() : h.dryA());
    if (e.size() >= 3 && e.back() > e.front()) {
      ev.theReferenceSquareWaveWasFound = true;
      ev.wMeasured = double(e.size() - 1) / double(e.back() - e.front());
    }
    ev.wDeclared = declaredStep(h, sr, side);
    ev.phaseAnchorAgrees = ev.theReferenceSquareWaveWasFound && ev.wDeclared > 0.0 &&
                           std::fabs(ev.wMeasured - ev.wDeclared) / ev.wDeclared < 1e-4;
  }

  // ---- (2) the ENDPOINT arm.
  DeviceStateV1 st = pulseState(depth, side);
  slot(st, ParameterId::joystick_x) = norm;
  setCable(st, JackId::joystick_x_out,
           onB ? JackId::vco_b_pwm_in : JackId::vco_a_pwm_in);
  EngineHarness h;
  h.reserve(static_cast<std::size_t>(frames));
  if (!h.load(st, sr)) return ev;
  std::vector<double> cv, consumed, used, knob;
  cv.reserve(static_cast<std::size_t>(frames));
  consumed.reserve(static_cast<std::size_t>(frames));
  used.reserve(static_cast<std::size_t>(frames));
  knob.reserve(static_cast<std::size_t>(frames));
  double otherMax = 0.0;
  if (!h.renderSampled(frames, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
        cv.push_back(rt.controlVoltageAt(JackId::joystick_x_out));
        consumed.push_back(onB ? rt.vcoBPwmCv() : rt.vcoAPwmCv());
        used.push_back(onB ? rt.vcoBEffectiveDuty() : rt.vcoAEffectiveDuty());
        knob.push_back(onB ? rt.vcoBPwm() : rt.vcoAPwm());
        // The side that was NOT driven: its PWM sink must read exactly nothing, so "this endpoint is
        // B's" is established inside the same run rather than inferred from the other side's test.
        const double other = onB ? rt.vcoAPwmCv() : rt.vcoBPwmCv();
        if (std::fabs(other) > otherMax) otherMax = std::fabs(other);
      })) {
    return ev;
  }
  ev.rendered = true;
  ev.otherSinkMaxAbs = otherMax;

  // Per-frame: the sink saw the value the graph published THIS frame, and the duty it applied is the
  // ruled law evaluated on the sink's OWN input. The independent duty uses the knob's own reported
  // depth, so it never calls Vco::effectiveDuty() to check Vco::effectiveDuty().
  bool saw = true, law = true;
  std::size_t settled = 0;
  for (std::size_t i = 0; i < cv.size(); ++i) {
    if (consumed[i] != cv[i]) saw = false;
    const double dIndep = pwmTransfer(base, knob[i], consumed[i]);
    if (std::fabs(used[i] - dIndep) > 1e-12) law = false;
    if (settled == 0 && i > 0 && std::fabs(knob[i] - depth) < 1e-9) settled = i;
  }
  ev.consumerSawThePublishedCv = saw;
  ev.consumerDutyIsTheRuledLaw = law;
  ev.knobDepthReachedItsSetting = settled > 0;
  if (settled == 0) return ev;

  ev.windowStart = static_cast<int>(settled);
  ev.windowFrames = static_cast<int>(std::ceil(3.0 / ev.wDeclared));
  if (ev.windowStart + ev.windowFrames >= frames) return ev;

  const double d = used[static_cast<std::size_t>(ev.windowStart)];
  const double indep = pwmTransfer(base, knob[static_cast<std::size_t>(ev.windowStart)],
                                  consumed[static_cast<std::size_t>(ev.windowStart)]);
  ev.dConsumer = d;
  ev.dSource = indep;
  ev.mirrorDuty = 1.0 - d;
  ev.wrongValueDuty = (d < 0.5) ? 0.05 : 0.95;
  ev.nearbyClampDuty = (d < 0.5) ? 0.002 : 0.998;
  // The clamp is only exercised if the RAW (pre-clamp) duty is strictly outside the window.
  const double rawDuty =
      base + knob[static_cast<std::size_t>(ev.windowStart)] *
                 consumed[static_cast<std::size_t>(ev.windowStart)] /
                 lunar24::core::Vco::kPwmCvFullScaleVolts;
  ev.rawDutyIsOutsideTheClampWindow =
      rawDuty < lunar24::core::Vco::kPwDutyMin || rawDuty > lunar24::core::Vco::kPwDutyMax;

  const std::vector<double>& a = onB ? h.dryB() : h.dryA();
  double dcA = 0.0;
  int dcN = 0;
  for (int k = 0; k < ev.windowFrames; ++k) {
    const std::size_t i = static_cast<std::size_t>(ev.windowStart + k);
    const double p = double(i + 1) * ev.wDeclared;  // the product's own frame-0 phase convention
    const double actual = a[i];
    // The reference is driven by the INDEPENDENT duty for THIS frame — base width, the depth the
    // knob reports, and the CV the graph published this frame — NOT by the consumer's readback. If
    // it were driven by the readback, a consumer that lagged or ignored the source would drag the
    // reference along with it and the comparison would agree with its own defect.
    const double dIndepFrame = pwmTransfer(base, knob[i], cv[i]);
    auto res = [&](double dd, double phase, bool corrected) {
      return std::fabs(actual - 0.5 * twoEdgeSwing(phase, dd, ev.wDeclared, corrected));
    };
    ev.rExact = std::max(ev.rExact, res(dIndepFrame, p, true));
    ev.rOneStepOff = std::max(ev.rOneStepOff, res(d, p + ev.wDeclared, true));
    ev.rSkippedPwm = std::max(ev.rSkippedPwm, res(base, p, true));
    ev.rWrongValue = std::max(ev.rWrongValue, res(ev.wrongValueDuty, p, true));
    ev.rWrongSide = std::max(ev.rWrongSide, res(ev.mirrorDuty, p, true));
    ev.rNearbyClamp = std::max(ev.rNearbyClamp, res(ev.nearbyClampDuty, p, true));
    dcA += actual;
    ++dcN;
  }
  ev.dcActual = dcN ? dcA / double(dcN) : 0.0;
  // REPORT ONLY, never a criterion: the two-rail mean is 2*duty-1 (the BLEP residuals integrate to
  // zero over a cycle), in device units. It is printed beside the measured DC so the endpoint's
  // level is visible, but no exact duty is ever back-derived from a finite-window mean here.
  ev.dcReference = 0.5 * (2.0 * d - 1.0);
  return ev;
}

constexpr double kRate44k1 = 44100.0;
constexpr double kRate48k = 48000.0;
constexpr double kRate88k2 = 88200.0;
constexpr double kRate96k = 96000.0;
const char* const kRateName[4] = {"44k1", "48k", "88k2", "96k"};

// One measured VCO period in frames (~220 Hz), used to express a ONE-SAMPLE tolerance.
double measuredPeriodFrames(double sr) { return sr / 220.0; }
double oneSampleDutyTol(double sr) { return 1.5 / measuredPeriodFrames(sr) + 1e-3; }

// =============================================================================================
// (1) CHANGING (time-varying) CV: the duty must track the ruled transfer PER MEASURED PERIOD, at
//     all four sample rates, and the modulation must be real (not a static DC read).
// =============================================================================================
struct ChangingCv {
  bool tracks = false;
  bool modulates = false;
  bool inBand = false;
  double worst = 0.0;
  double dutyMin = 0.0;
  double dutyMax = 0.0;
  std::size_t periods = 0;
};

ChangingCv changingCvAt(double sr) {
  ChangingCv res;
  const double depth = 0.1;  // the LFO publishes 0..10 V, so the whole swing stays unsaturated
  DeviceStateV1 st = pulseState(depth);
  slot(st, ParameterId::lfo_a_rate) = 5.0;  // Hz (descriptor unit "hz"); measured 5.0000 Hz
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);

  EngineHarness h;
  const int frames = static_cast<int>(sr / 2.0);  // 0.5 s = 2.5 LFO cycles
  h.reserve(static_cast<std::size_t>(frames));
  if (!h.load(st, sr)) return res;
  std::vector<double> cv;
  cv.reserve(static_cast<std::size_t>(frames));
  if (!h.renderSampled(frames, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
        cv.push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
      })) {
    return res;
  }

  const std::vector<double>& a = h.dryA();
  const auto e = risingEdges(a);
  const double base = declaredInitial(ParameterId::vco_a_pw);
  double worst = 0.0;
  bool any = false;
  for (std::size_t k = 0; k + 1 < e.size(); ++k) {
    std::size_t hi = 0;
    double imp = 0.0;
    for (std::size_t i = e[k]; i < e[k + 1]; ++i) {
      if (a[i] > 0.0) ++hi;
      imp += pwmTransfer(base, depth, cv[i]);
    }
    const double n = double(e[k + 1] - e[k]);
    worst = std::max(worst, std::fabs(double(hi) / n - imp / n));
    any = true;
  }
  res.periods = e.size() ? e.size() - 1 : 0;
  res.worst = worst;
  res.tracks = any && worst <= 0.02;  // measured worst 0.00500 -> 4x margin
  double mn = 0.0, mx = 0.0;
  static_cast<void>(meanAudioDuty(a, &mn, &mx));
  res.dutyMin = mn;
  res.dutyMax = mx;
  res.modulates = (mx - mn) > 0.05;  // the LFO swing is depth*10/10 = 0.1 in duty
  res.inBand = (mn >= 0.001 - 1e-9) && (mx <= 0.999 + 1e-9);
  return res;
}

void changing_cv_tracks_the_ruled_transfer() {
  const ChangingCv r44k1 = changingCvAt(kRate44k1);
  const ChangingCv r48k = changingCvAt(kRate48k);
  const ChangingCv r88k2 = changingCvAt(kRate88k2);
  const ChangingCv r96k = changingCvAt(kRate96k);
  const ChangingCv all[4] = {r44k1, r48k, r88k2, r96k};
  for (int r = 0; r < 4; ++r) {
    std::printf("  changing-cv %-4s periods=%zu worstGap=%.5f duty[%.4f..%.4f]\n", kRateName[r],
                all[r].periods, all[r].worst, all[r].dutyMin, all[r].dutyMax);
  }
  const bool changingCvHasEnoughPeriods =
      r44k1.periods > 20 && r48k.periods > 20 && r88k2.periods > 20 && r96k.periods > 20;
  CHECK(changingCvHasEnoughPeriods);

  const bool changingCvTracksTransfer44k1 = r44k1.tracks;
  const bool changingCvTracksTransfer48k = r48k.tracks;
  const bool changingCvTracksTransfer88k2 = r88k2.tracks;
  const bool changingCvTracksTransfer96k = r96k.tracks;
  CHECK(changingCvTracksTransfer44k1);
  CHECK(changingCvTracksTransfer48k);
  CHECK(changingCvTracksTransfer88k2);
  CHECK(changingCvTracksTransfer96k);

  const bool changingCvModulatesDuty44k1 = r44k1.modulates;
  const bool changingCvModulatesDuty48k = r48k.modulates;
  const bool changingCvModulatesDuty88k2 = r88k2.modulates;
  const bool changingCvModulatesDuty96k = r96k.modulates;
  CHECK(changingCvModulatesDuty44k1);
  CHECK(changingCvModulatesDuty48k);
  CHECK(changingCvModulatesDuty88k2);
  CHECK(changingCvModulatesDuty96k);

  const bool changingCvWithinClampBand44k1 = r44k1.inBand;
  const bool changingCvWithinClampBand48k = r48k.inBand;
  const bool changingCvWithinClampBand88k2 = r88k2.inBand;
  const bool changingCvWithinClampBand96k = r96k.inBand;
  CHECK(changingCvWithinClampBand44k1);
  CHECK(changingCvWithinClampBand48k);
  CHECK(changingCvWithinClampBand88k2);
  CHECK(changingCvWithinClampBand96k);
}

// =============================================================================================
// (2) DC CV law at four rates: joystick.x_out -> vco_a.pwm_in, all five norm points, plus input
//     INVERSION (negative CV lowers duty) and SATURATION (both clamp endpoints).
// =============================================================================================
// Does the emitted waveform really carry this endpoint, judged on the AUDIO? Everything the ruling
// asked for in one place: the independent duty, the actual consumer value, and the fixed-phase
// two-edge reconciliation against a set of rejected neighbours.
bool endpointIsCarriedByTheAudio(const EndpointEvidence& ev) {
  return ev.rendered && ev.consumerSawThePublishedCv && ev.consumerDutyIsTheRuledLaw &&
         ev.knobDepthReachedItsSetting && ev.rawDutyIsOutsideTheClampWindow &&
         ev.theReferenceSquareWaveWasFound && ev.phaseAnchorAgrees &&
         std::fabs(ev.dConsumer - ev.dSource) <= 1e-12 &&
         ev.rExact <= kEndpointResidualTol &&       // the emitted waveform IS the endpoint's
         ev.rSkippedPwm > kEndpointMargin &&        // ... and NOT the unmodulated base
         ev.rWrongValue > kEndpointMargin &&        // ... and NOT a wrong clamp value
         ev.rWrongSide > kEndpointMargin &&         // ... and NOT the mirror endpoint
         ev.rNearbyClamp > kEndpointResidualTol &&  // ... and NOT the nearest mis-clamp
         ev.rOneStepOff > kEndpointResidualTol;     // the fixed phase anchor is load-bearing
}

bool dcCvLawHoldsAt(double sr, const char* tag) {
  const double base = declaredInitial(ParameterId::vco_a_pw);
  const double tol = oneSampleDutyTol(sr);
  const double kNorm[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
  bool all = true;
  for (int i = 0; i < 5; ++i) {
    if (kNorm[i] <= 0.0 || kNorm[i] >= 1.0) {
      // The two CLAMP ENDPOINTS: the zero-crossing estimator above has no edges to count here, so
      // they are judged by the fixed-phase two-edge reference instead. The intermediate norms below
      // keep their existing, still-discriminating edge-count observation.
      const EndpointEvidence ev = endpointDutyAt(sr, kNorm[i]);
      const bool ok = endpointIsCarriedByTheAudio(ev);
      std::printf(
          "  dc-cv %-4s norm=%.2f ENDPOINT used=%.9f indep=%.9f (mirror %.3f, wrong %.2f, near "
          "%.3f) w=%.9f/%.9f | exact=%.3e stepOff=%.3e skipped=%.3e wrongVal=%.3e side=%.3e "
          "near=%.3e | dc=%.6f ref=%.6f | win=%d@%d rawsOfWindow=%d\n",
          tag, kNorm[i], ev.dConsumer, ev.dSource, ev.mirrorDuty, ev.wrongValueDuty,
          ev.nearbyClampDuty, ev.wDeclared, ev.wMeasured, ev.rExact, ev.rOneStepOff, ev.rSkippedPwm,
          ev.rWrongValue, ev.rWrongSide, ev.rNearbyClamp, ev.dcActual, ev.dcReference,
          ev.windowFrames, ev.windowStart, ev.rawDutyIsOutsideTheClampWindow ? 1 : 0);
      if (!ok) {
        all = false;
        std::printf("  dc-cv %-4s norm=%.2f ENDPOINT REJECTED (rendered=%d saw=%d law=%d knob=%d "
                    "rawOutside=%d refFound=%d anchor=%d)\n",
                    tag, kNorm[i], ev.rendered ? 1 : 0, ev.consumerSawThePublishedCv ? 1 : 0,
                    ev.consumerDutyIsTheRuledLaw ? 1 : 0, ev.knobDepthReachedItsSetting ? 1 : 0,
                    ev.rawDutyIsOutsideTheClampWindow ? 1 : 0,
                    ev.theReferenceSquareWaveWasFound ? 1 : 0, ev.phaseAnchorAgrees ? 1 : 0);
      }
      continue;
    }
    DeviceStateV1 st = pulseState(1.0);
    slot(st, ParameterId::joystick_x) = kNorm[i];
    setCable(st, JackId::joystick_x_out, JackId::vco_a_pwm_in);
    EngineHarness h;
    const int frames = static_cast<int>(sr / 3.0);
    h.reserve(static_cast<std::size_t>(frames));
    double got = -1.0;
    if (h.load(st, sr) && h.render(frames)) got = meanAudioDuty(h.dryA());
    const double want = pwmTransfer(base, 1.0, (kNorm[i] - 0.5) * 10.0);
    if (got < 0.0 || std::fabs(got - want) > tol) {
      all = false;
      std::printf("  dc-cv %-4s norm=%.2f got=%.4f want=%.4f tol=%.4f\n", tag, kNorm[i], got, want,
                  tol);
    }
  }
  return all;
}

void dc_cv_law_and_inversion_and_saturation() {
  const bool dcCvLawHolds44k1 = dcCvLawHoldsAt(kRate44k1, kRateName[0]);
  const bool dcCvLawHolds48k = dcCvLawHoldsAt(kRate48k, kRateName[1]);
  const bool dcCvLawHolds88k2 = dcCvLawHoldsAt(kRate88k2, kRateName[2]);
  const bool dcCvLawHolds96k = dcCvLawHoldsAt(kRate96k, kRateName[3]);
  CHECK(dcCvLawHolds44k1);
  CHECK(dcCvLawHolds48k);
  CHECK(dcCvLawHolds88k2);
  CHECK(dcCvLawHolds96k);

  // ---------------------------------------------------------------------------------------------
  // INVERSION and SATURATION, judged on the AUDIO at the two clamp endpoints. These used to read
  // the duty off zero crossings; at duty 0.001 / 0.999 there are none left to read (see the
  // instrument header above), so each statement is now made against the fixed-phase two-edge
  // reference instead — and made STRONGER, because "the emitted waveform is the 0.001 waveform and
  // is not the 0.5 / 0.05 / 0.999 / 0.002 waveform" says more than "some duty number is smaller
  // than another".
  //
  // BOTH VCOs are measured this way. The A arms come first; the B arms follow them and repeat the
  // same statements on the other side rather than substituting an intermediate-duty observation.
  // ---------------------------------------------------------------------------------------------
  const double sr = kRate48k;
  const double base = declaredInitial(ParameterId::vco_a_pw);
  const EndpointEvidence low = endpointDutyAt(sr, 0.0);   // -5 V -> raw 0.0001, clamped to 0.001
  const EndpointEvidence high = endpointDutyAt(sr, 1.0);  // +5 V -> raw 0.9999, clamped to 0.999
  std::printf(
      "  dc-cv inversion/saturation (audio): -5V duty=%.9f dc=%.6f | base=%.6f | +5V duty=%.9f "
      "dc=%.6f (clamp window %.3f/%.3f)\n",
      low.dConsumer, low.dcActual, base, high.dConsumer, high.dcActual,
      lunar24::core::Vco::kPwDutyMin, lunar24::core::Vco::kPwDutyMax);

  // Negative CV LOWERS the duty: the emitted waveform is the low endpoint's, and it is neither the
  // unmodulated base width nor the mirror endpoint.
  const bool negativeCvLowersDuty = endpointIsCarriedByTheAudio(low) &&
                                    low.dConsumer < base - 0.01 && low.rSkippedPwm > kEndpointMargin;
  CHECK(negativeCvLowersDuty);
  // Positive CV SATURATES at the top of the clamp window.
  const bool positiveCvSaturatesAtDutyMax = endpointIsCarriedByTheAudio(high) &&
                                            std::fabs(high.dConsumer - 0.999) <= 1e-9 &&
                                            high.rawDutyIsOutsideTheClampWindow;
  CHECK(positiveCvSaturatesAtDutyMax);
  // Negative CV SATURATES at the bottom of the clamp window.
  const bool negativeCvSaturatesAtDutyMin = endpointIsCarriedByTheAudio(low) &&
                                            std::fabs(low.dConsumer - 0.001) <= 1e-9 &&
                                            low.rawDutyIsOutsideTheClampWindow;
  CHECK(negativeCvSaturatesAtDutyMin);
  // The endpoints are the CLAMPED values, not whatever the raw transfer happened to land on: both
  // raws sit strictly outside the window, which is what makes the two checks above about the clamp.
  const bool bothRawsFallOutsideTheClampWindow =
      low.rawDutyIsOutsideTheClampWindow && high.rawDutyIsOutsideTheClampWindow;
  CHECK(bothRawsFallOutsideTheClampWindow);
  // The two endpoints are genuinely different waveforms, so neither check can pass by accident.
  const bool theTwoEndpointsAreNotTheSameWaveform = low.rExact <= kEndpointResidualTol &&
                                                    high.rExact <= kEndpointResidualTol &&
                                                    std::fabs(low.dcActual - high.dcActual) > 0.5;
  CHECK(theTwoEndpointsAreNotTheSameWaveform);

  // ---------------------------------------------------------------------------------------------
  // THE SAME OBSERVATION ON VCO B. The ruled transfer is a statement about the PWM SOFTWARE path and
  // `Vco` is ONE class instantiated twice (machine_runtime.h: `Vco vcA_; Vco vcB_;`), so B's endpoints
  // are the same claim made on the other side, not a separate feature. Until now B was only ever
  // observed at INTERMEDIATE duty (the A/B asymmetry and default-route arms), and that cannot stand in
  // for the 0.001 / 0.999 endpoints: at an endpoint the naive high window is NARROWER than the
  // band-limiting kernel, which is precisely the regime the two-edge correction exists for.
  //
  // B is driven by its OWN depth knob and its OWN sink, with the default A->B route switched off
  // (`vco_b.cv_amt = 0`, inside `pulseState`) — otherwise A frequency-modulates B and B's phase stops
  // following the declared pitch law the fixed-phase reference is anchored to. That isolation is not
  // assumed: `otherSinkMaxAbs` reads the NON-driven side's sink in this same run and must be exactly 0.
  // The names carry a `vcoB` prefix rather than an A-style suffix on purpose: the A names above are
  // targeted by string from tools/run_gh19_s0_mutants.py, and a suffix would make "…DutyMin" a
  // substring of both sides, so a mutant aimed at A could be reported as having tripped B's check.
  // ---------------------------------------------------------------------------------------------
  const double baseB = declaredInitial(ParameterId::vco_b_pw);
  const EndpointEvidence lowB = endpointDutyAt(sr, 0.0, EndpointSide::B);   // -5 V -> raw 0.0001
  const EndpointEvidence highB = endpointDutyAt(sr, 1.0, EndpointSide::B);  // +5 V -> raw 0.9999
  std::printf(
      "  dc-cv inversion/saturation B (audio): -5V dutyB=%.9f dcB=%.6f | baseB=%.6f | +5V "
      "dutyB=%.9f dcB=%.6f (other-sink max %.3e)\n",
      lowB.dConsumer, lowB.dcActual, baseB, highB.dConsumer, highB.dcActual,
      std::max(lowB.otherSinkMaxAbs, highB.otherSinkMaxAbs));
  std::printf(
      "  dc-cv B endpoint separations: -5V exact=%.3e skipped=%.3e wrongVal=%.3e side=%.3e "
      "near=%.3e | +5V exact=%.3e skipped=%.3e wrongVal=%.3e side=%.3e near=%.3e | w=%.9f/%.9f\n",
      lowB.rExact, lowB.rSkippedPwm, lowB.rWrongValue, lowB.rWrongSide, lowB.rNearbyClamp,
      highB.rExact, highB.rSkippedPwm, highB.rWrongValue, highB.rWrongSide, highB.rNearbyClamp,
      lowB.wDeclared, lowB.wMeasured);

  // Negative CV LOWERS B's duty: the emitted waveform is the low endpoint's, and it is neither the
  // unmodulated base width nor the mirror endpoint.
  const bool vcoBNegativeCvLowersDuty = endpointIsCarriedByTheAudio(lowB) &&
                                        lowB.dConsumer < baseB - 0.01 &&
                                        lowB.rSkippedPwm > kEndpointMargin;
  CHECK(vcoBNegativeCvLowersDuty);
  // Positive CV SATURATES at the top of the clamp window — on B.
  const bool vcoBPositiveCvSaturatesAtDutyMax = endpointIsCarriedByTheAudio(highB) &&
                                                std::fabs(highB.dConsumer - 0.999) <= 1e-9 &&
                                                highB.rawDutyIsOutsideTheClampWindow;
  CHECK(vcoBPositiveCvSaturatesAtDutyMax);
  // Negative CV SATURATES at the bottom of the clamp window — on B.
  const bool vcoBNegativeCvSaturatesAtDutyMin = endpointIsCarriedByTheAudio(lowB) &&
                                                std::fabs(lowB.dConsumer - 0.001) <= 1e-9 &&
                                                lowB.rawDutyIsOutsideTheClampWindow;
  CHECK(vcoBNegativeCvSaturatesAtDutyMin);
  // Both of B's raws sit strictly outside the window, which is what makes the two checks above about
  // the clamp rather than about wherever the raw transfer happened to land.
  const bool vcoBBothRawsFallOutsideTheClampWindow = lowB.rawDutyIsOutsideTheClampWindow &&
                                                     highB.rawDutyIsOutsideTheClampWindow;
  CHECK(vcoBBothRawsFallOutsideTheClampWindow);
  // B's two endpoints are genuinely different waveforms too.
  const bool vcoBTheTwoEndpointsAreNotTheSameWaveform =
      lowB.rExact <= kEndpointResidualTol && highB.rExact <= kEndpointResidualTol &&
      std::fabs(lowB.dcActual - highB.dcActual) > 0.5;
  CHECK(vcoBTheTwoEndpointsAreNotTheSameWaveform);
  // The fixture drove B through B's OWN sink and nothing else: the non-driven side's PWM sink read
  // exactly nothing for the whole of BOTH runs. An A/B mis-wire — in the fixture or in the product's
  // sink bindings — fails here, so "this endpoint is B's" is measured rather than assumed.
  const bool vcoBEndpointsAreDrivenThroughBsOwnSink =
      lowB.otherSinkMaxAbs == 0.0 && highB.otherSinkMaxAbs == 0.0;
  CHECK(vcoBEndpointsAreDrivenThroughBsOwnSink);
}

// =============================================================================================
// (3) Depth 0 with a LIVE changing cable is STRICTLY inert, and depth 0 / mid / 1 are ordered.
// =============================================================================================
void depth_arms_and_depth_zero_inertness() {
  const double sr = kRate48k;
  const double base = declaredInitial(ParameterId::vco_a_pw);
  const double tol = oneSampleDutyTol(sr);

  // Depth 0 with the LFO actually patched in and moving: the cable exists, the duty must not move.
  double d0 = -1.0, d0min = 0.0, d0max = 0.0, cvSpan = -1.0;
  {
    DeviceStateV1 st = pulseState(0.0);
    slot(st, ParameterId::lfo_a_rate) = 5.0;
    setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
    EngineHarness h;
    h.reserve(24000);
    if (h.load(st, sr)) {
      double lo = 1e9, hi = -1e9;
      if (h.renderSampled(24000, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
            const double v = rt.controlVoltageAt(JackId::lfo_a_cv_out);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
          })) {
        d0 = meanAudioDuty(h.dryA(), &d0min, &d0max);
        cvSpan = hi - lo;
      }
    }
  }
  std::printf("  depth 0 with a live cable: duty=%.6f spread=%.6f sourceSpan=%.4f V\n", d0,
              d0max - d0min, cvSpan);
  const bool depthZeroSourceWasReallyMoving = cvSpan > 5.0;
  CHECK(depthZeroSourceWasReallyMoving);
  const bool depthZeroIsStrictlyInert =
      d0 >= 0.0 && std::fabs(d0 - base) <= tol && (d0max - d0min) <= tol;
  CHECK(depthZeroIsStrictlyInert);

  auto dutyAtDepth = [&](double depth) {
    DeviceStateV1 st = pulseState(depth);
    slot(st, ParameterId::joystick_x) = 0.75;  // +2.5 V
    setCable(st, JackId::joystick_x_out, JackId::vco_a_pwm_in);
    EngineHarness h;
    h.reserve(20000);
    if (!h.load(st, sr) || !h.render(20000)) return -1.0;
    return meanAudioDuty(h.dryA());
  };
  const double dHalf = dutyAtDepth(0.5);
  const double dFull = dutyAtDepth(1.0);
  const double wantHalf = pwmTransfer(base, 0.5, 2.5);
  const double wantFull = pwmTransfer(base, 1.0, 2.5);
  std::printf("  depth arms: d0=%.6f d0.5=%.6f d1=%.6f (want %.6f / %.6f)\n", d0, dHalf, dFull,
              wantHalf, wantFull);
  const bool pwmDepthMidMatchesHalfTheModulation =
      dHalf > 0.0 && std::fabs(dHalf - wantHalf) <= 0.005;
  CHECK(pwmDepthMidMatchesHalfTheModulation);
  const bool pwmDepthOneMatchesTheFullModulation = dFull > 0.0 && std::fabs(dFull - wantFull) <= 0.005;
  CHECK(pwmDepthOneMatchesTheFullModulation);
  const bool pwmDepthArmsAreOrdered = d0 < dHalf && dHalf < dFull;
  CHECK(pwmDepthArmsAreOrdered);
}

// =============================================================================================
// (4) SAME-FRAME consumption. The sink readback must equal the SAME frame's published source volts,
//     and must NOT equal the previous frame's — so a one-sample-late consumer goes red.
// =============================================================================================
void same_frame_consumption() {
  DeviceStateV1 st = pulseState(0.1);
  slot(st, ParameterId::lfo_a_rate) = 5.0;
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
  EngineHarness h;
  h.reserve(9600);
  if (!h.load(st, kRate48k)) {
    const bool sameFrameRenderCommitted = false;
    CHECK(sameFrameRenderCommitted);
    return;
  }
  std::vector<double> cv, sink;
  cv.reserve(9600);
  sink.reserve(9600);
  if (!h.renderSampled(9600, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
        cv.push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
        sink.push_back(rt.vcoAPwmCv());
      })) {
    const bool sameFrameRenderCompleted = false;
    CHECK(sameFrameRenderCompleted);
    return;
  }
  int same = 0, sameTotal = 0, late = 0;
  for (std::size_t i = 1; i < cv.size(); ++i) {
    ++sameTotal;
    if (sink[i] == cv[i]) ++same;
    if (sink[i] == cv[i - 1]) ++late;
  }
  std::printf("  same-frame: same=%d/%d oneLate=%d/%d\n", same, sameTotal, late, sameTotal);
  const bool sameFrameSampleCountIsNonVacuous = sameTotal > 9000;
  CHECK(sameFrameSampleCountIsNonVacuous);
  const bool pwmCvIsConsumedInTheSameFrameItIsPublished = same == sameTotal;
  CHECK(pwmCvIsConsumedInTheSameFrameItIsPublished);
  const bool pwmCvIsNotThePreviousFramesValue = late == 0;
  CHECK(pwmCvIsNotThePreviousFramesValue);
}

// =============================================================================================
// (5) Full morph sweep + boundary continuity at all four rates, asserted on the AUDIO.
// =============================================================================================
struct MorphNodes {
  bool sawOk = false, invMirror = false, sineNode = false, triNode = false, pulseNode = false;
  double sawMean = 0.0, sawSlope = 0.0, invMean = 0.0, invSlope = 0.0, sineMean = 0.0, sineRms = 0.0,
         triMean = 0.0, triPp = 0.0, pulseMean = 0.0, pulseRms = 0.0;
};

MorphNodes morphNodesAt(double sr) {
  MorphNodes m;
  auto arm = [&](double norm) {
    DeviceStateV1 st = make_default_device_state(kSeed);
    slot(st, ParameterId::vco_a_morph) = norm;
    slot(st, ParameterId::vco_b_cv_amt) = 0.0;  // A is the observed side
    EngineHarness h;
    h.reserve(24000);
    if (!h.load(st, sr) || !h.render(24000)) return Profile{};
    return profileOf(h.dryA());
  };
  const Profile saw = arm(0.0);
  const Profile invsaw = arm(0.25);
  const Profile sine = arm(0.5);
  const Profile tri = arm(0.75);
  const Profile pulse = arm(1.0);
  m.sawMean = saw.meanAbs;   m.sawSlope = saw.medSlope;
  m.invMean = invsaw.meanAbs; m.invSlope = invsaw.medSlope;
  m.sineMean = sine.meanAbs; m.sineRms = sine.rms;
  m.triMean = tri.meanAbs;   m.triPp = tri.pp;
  m.pulseMean = pulse.meanAbs; m.pulseRms = pulse.rms;
  m.sawOk = saw.ok && std::fabs(saw.meanAbs - 0.25) < 0.01 && saw.medSlope > 0.0;
  m.invMirror = invsaw.ok && std::fabs(invsaw.meanAbs - saw.meanAbs) < 0.01 &&
                std::fabs(invsaw.medSlope + saw.medSlope) < 0.01 && invsaw.medSlope < 0.0;
  m.sineNode = sine.ok && std::fabs(sine.meanAbs - 0.318312) < 0.01 &&
               std::fabs(sine.rms - 0.353553) < 0.01;
  m.triNode = tri.ok && std::fabs(tri.meanAbs - 0.25) < 0.01 && tri.pp < 0.999 && tri.pp > 0.99;
  m.pulseNode = pulse.ok && std::fabs(pulse.meanAbs - 0.5) < 0.01 &&
                std::fabs(pulse.rms - 0.5) < 0.01;
  return m;
}

double morphSweepWorstStepAt(double sr) {
  auto arm = [&](double norm) {
    DeviceStateV1 st = make_default_device_state(kSeed);
    slot(st, ParameterId::vco_a_morph) = norm;
    slot(st, ParameterId::vco_b_cv_amt) = 0.0;
    EngineHarness h;
    h.reserve(24000);
    if (!h.load(st, sr) || !h.render(24000)) return Profile{};
    return profileOf(h.dryA());
  };
  double worst = 0.0;
  Profile prev = arm(0.0);
  for (int i = 1; i <= 100; ++i) {
    const Profile cur = arm(double(i) / 100.0);
    if (!prev.ok || !cur.ok) return 1e9;
    worst = std::max(worst, std::fabs(cur.meanAbs - prev.meanAbs));
    prev = cur;
  }
  return worst;
}

void morph_sweep_continuity_and_nodes() {
  const MorphNodes n44k1 = morphNodesAt(kRate44k1);
  const MorphNodes n48k = morphNodesAt(kRate48k);
  const MorphNodes n88k2 = morphNodesAt(kRate88k2);
  const MorphNodes n96k = morphNodesAt(kRate96k);
  const MorphNodes all[4] = {n44k1, n48k, n88k2, n96k};
  const double kSweepTol = 0.05;  // measured worst adjacent |d meanAbs| = 0.020000 -> 2.5x margin

  const double w44k1 = morphSweepWorstStepAt(kRate44k1);
  const double w48k = morphSweepWorstStepAt(kRate48k);
  const double w88k2 = morphSweepWorstStepAt(kRate88k2);
  const double w96k = morphSweepWorstStepAt(kRate96k);
  const double wAll[4] = {w44k1, w48k, w88k2, w96k};
  for (int r = 0; r < 4; ++r) {
    std::printf("  morph %-4s saw(%.5f,%+.5f) inv(%.5f,%+.5f) sine(%.5f,%.5f) tri(%.5f,pp %.5f) "
                "pulse(%.5f,%.5f) worstStep=%.6f\n",
                kRateName[r], all[r].sawMean, all[r].sawSlope, all[r].invMean, all[r].invSlope,
                all[r].sineMean, all[r].sineRms, all[r].triMean, all[r].triPp, all[r].pulseMean,
                all[r].pulseRms, wAll[r]);
  }

  const bool morphFiveNodeSignaturesAt44k1 = n44k1.sawOk && n44k1.invMirror && n44k1.sineNode &&
                                             n44k1.triNode && n44k1.pulseNode;
  const bool morphFiveNodeSignaturesAt48k = n48k.sawOk && n48k.invMirror && n48k.sineNode &&
                                            n48k.triNode && n48k.pulseNode;
  const bool morphFiveNodeSignaturesAt88k2 = n88k2.sawOk && n88k2.invMirror && n88k2.sineNode &&
                                             n88k2.triNode && n88k2.pulseNode;
  const bool morphFiveNodeSignaturesAt96k = n96k.sawOk && n96k.invMirror && n96k.sineNode &&
                                            n96k.triNode && n96k.pulseNode;
  CHECK(morphFiveNodeSignaturesAt44k1);
  CHECK(morphFiveNodeSignaturesAt48k);
  CHECK(morphFiveNodeSignaturesAt88k2);
  CHECK(morphFiveNodeSignaturesAt96k);

  const bool morphSweepIsContinuous44k1 = w44k1 <= kSweepTol;
  const bool morphSweepIsContinuous48k = w48k <= kSweepTol;
  const bool morphSweepIsContinuous88k2 = w88k2 <= kSweepTol;
  const bool morphSweepIsContinuous96k = w96k <= kSweepTol;
  CHECK(morphSweepIsContinuous44k1);
  CHECK(morphSweepIsContinuous48k);
  CHECK(morphSweepIsContinuous88k2);
  CHECK(morphSweepIsContinuous96k);
}

// =============================================================================================
// (6) A/B asymmetry: a morph change on one side must not move the other, at BOTH the readback and
//     the AUDIO level, and the two PWM sides must be independently addressable.
// =============================================================================================
void morph_ab_asymmetry() {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::vco_b_cv_amt) = 0.0;  // B isolated, so each side's node is unambiguous
  slot(st, ParameterId::vco_a_morph) = 0.75;  // A = triangle
  slot(st, ParameterId::vco_b_morph) = 1.0;   // B = pulse
  EngineHarness h;
  h.reserve(24000);
  if (!h.load(st, kRate48k) || !h.render(24000)) {
    const bool abAsymmetryRenderCompleted = false;
    CHECK(abAsymmetryRenderCompleted);
    return;
  }
  const Profile a = profileOf(h.dryA());
  const Profile b = profileOf(h.dryB());
  std::printf("  ab-asymmetry: A(%.5f,pp %.5f) B(%.5f) readback A=%.3f B=%.3f\n", a.meanAbs, a.pp,
              b.meanAbs, h.runtime()->vcoAMorph(), h.runtime()->vcoBMorph());
  const bool morphReadbackIsPerSide = std::fabs(h.runtime()->vcoAMorph() - 0.75) < 1e-12 &&
                                      std::fabs(h.runtime()->vcoBMorph() - 1.0) < 1e-12;
  CHECK(morphReadbackIsPerSide);
  const bool vcoAMorphDrivesOnlyTheAAudioNode =
      a.ok && std::fabs(a.meanAbs - 0.25) < 0.01 && a.pp < 0.999;
  CHECK(vcoAMorphDrivesOnlyTheAAudioNode);
  const bool vcoBMorphDrivesOnlyTheBAudioNode = b.ok && std::fabs(b.meanAbs - 0.5) < 0.01;
  CHECK(vcoBMorphDrivesOnlyTheBAudioNode);

  // PWM side isolation through the same owner: driving A's PWM jack must leave B's own depth alone.
  DeviceStateV1 st2 = pulseState(1.0);
  slot(st2, ParameterId::vco_b_pwm) = 0.25;
  slot(st2, ParameterId::joystick_x) = 1.0;
  setCable(st2, JackId::joystick_x_out, JackId::vco_a_pwm_in);
  EngineHarness h2;
  h2.reserve(8000);
  if (!h2.load(st2, kRate48k) || !h2.render(8000)) {
    const bool pwmSideIsolationRenderCompleted = false;
    CHECK(pwmSideIsolationRenderCompleted);
    return;
  }
  std::printf("  pwm side isolation: cvA=%.4f cvB=%.4f depthB=%.4f\n", h2.runtime()->vcoAPwmCv(),
              h2.runtime()->vcoBPwmCv(), h2.runtime()->vcoBPwm());
  const bool vcoAPwmInReceivesTheASourceVolts =
      std::fabs(h2.runtime()->vcoAPwmCv() - 5.0) < 1e-9;
  CHECK(vcoAPwmInReceivesTheASourceVolts);
  const bool vcoBPwmInStaysUnpatchedWhenOnlyAIsDriven = h2.runtime()->vcoBPwmCv() == 0.0;
  CHECK(vcoBPwmInStaysUnpatchedWhenOnlyAIsDriven);
  const bool drivingAPwmDoesNotMoveBOwnDepth = std::fabs(h2.runtime()->vcoBPwm() - 0.25) < 1e-12;
  CHECK(drivingAPwmDoesNotMoveBOwnDepth);
}

// =============================================================================================
// (7) Irregular block sizes through processBlock: the render must be partition-independent.
// =============================================================================================
void irregular_blocks_are_partition_invariant() {
  DeviceStateV1 st = pulseState(0.6);
  slot(st, ParameterId::joystick_x) = 0.75;
  setCable(st, JackId::joystick_x_out, JackId::vco_a_pwm_in);
  const int kBlock = 512;
  const int kTotal = 4096;
  // Every chunk <= kBlock (processBlock drops to silence above the prepared block size) and the
  // chunks sum EXACTLY to the reference window, so the two arms are directly comparable.
  const int kIrregular[16] = {7, 13, 32, 64, 100, 441, 512, 37, 128, 91, 512, 512, 512, 512, 512, 111};
  int sum = 0;
  for (int v : kIrregular) sum += v;
  const bool irregularPartitionSumsToTheReferenceWindow = sum == kTotal;
  CHECK(irregularPartitionSumsToTheReferenceWindow);
  if (!irregularPartitionSumsToTheReferenceWindow) return;

  EngineHarness hRef, hIrr;
  hRef.reserve(static_cast<std::size_t>(kTotal));
  hIrr.reserve(static_cast<std::size_t>(kTotal));
  if (!hRef.load(st, kRate48k, kBlock) || !hIrr.load(st, kRate48k, kBlock)) {
    const bool irregularPartitionEnginesCommitted = false;
    CHECK(irregularPartitionEnginesCommitted);
    return;
  }
  const auto silent = [](std::size_t, double& i0, double& i1) {
    i0 = 0.0;
    i1 = 0.0;
  };
  bool rendered = true;
  for (int i = 0; i < kTotal / kBlock; ++i) {
    if (!hRef.renderBlock(kBlock, silent)) rendered = false;
  }
  for (int v : kIrregular) {
    if (!hIrr.renderBlock(v, silent)) rendered = false;
  }
  const bool irregularPartitionRendersCompleted = rendered && !hIrr.dryA().empty();
  CHECK(irregularPartitionRendersCompleted);
  if (!irregularPartitionRendersCompleted) return;

  bool same = hRef.dryA().size() == hIrr.dryA().size();
  if (same) {
    for (std::size_t i = 0; i < hIrr.dryA().size(); ++i) {
      if (hRef.dryA()[i] != hIrr.dryA()[i]) {
        same = false;
        break;
      }
    }
  }
  const bool irregularBlocksReproduceTheUniformPartitionExactly = same;
  CHECK(irregularBlocksReproduceTheUniformPartitionExactly);
}

// =============================================================================================
// (8) Repeated restore: applying the same state twice must land on the same value, and a restore
//     back to default must track the state rather than latch.
// =============================================================================================
void repeated_restore_tracks_the_state() {
  DeviceStateV1 def = make_default_device_state(kSeed);
  DeviceStateV1 nonDef = def;
  slot(nonDef, ParameterId::vco_a_pwm) = 0.9;
  slot(nonDef, ParameterId::vco_b_pwm) = 0.9;
  const double defA = declaredInitial(ParameterId::vco_a_pwm);
  const bool pwmDefaultDepthIsZero = std::fabs(defA) < 1e-12;
  CHECK(pwmDefaultDepthIsZero);
  if (!pwmDefaultDepthIsZero) return;

  EngineHarness h;
  if (!h.load(def, kRate48k)) {
    const bool repeatedRestoreFirstCommit = false;
    CHECK(repeatedRestoreFirstCommit);
    return;
  }
  const double wantA = 0.9;
  bool okA = true, okB = true;
  for (int rep = 0; rep < 2; ++rep) {
    h.applyCanonical(nonDef);
    okA = okA && std::fabs(h.runtime()->vcoAPwm() - wantA) < 1e-12;
    okB = okB && std::fabs(h.runtime()->vcoBPwm() - wantA) < 1e-12;
  }
  const bool repeatedRestoreReachesThePwmDepthSetter = okA && okB;
  CHECK(repeatedRestoreReachesThePwmDepthSetter);
  h.applyCanonical(def);
  const bool restoreBackToDefaultTracksTheStateWithoutLatching =
      std::fabs(h.runtime()->vcoAPwm() - defA) < 1e-12 &&
      std::fabs(h.runtime()->vcoBPwm() - defA) < 1e-12;
  CHECK(restoreBackToDefaultTracksTheStateWithoutLatching);
}

// =============================================================================================
// (9) A rejected apply must leave state/format/plan and the SUBSEQUENT TRACE untouched.
// =============================================================================================
void rejected_apply_is_atomic_for_morph_and_pwm() {
  const double sr = kRate48k;
  const int bs = 512;
  DeviceStateV1 good = pulseState(0.5);
  DeviceStateV1 badMorph = good;
  slot(badMorph, ParameterId::vco_a_morph) = 2.0;  // outside the declared [0,1]
  DeviceStateV1 badPwm = good;
  slot(badPwm, ParameterId::vco_a_pwm) = 2.0;  // outside the declared [0,1]

  const DeviceStateV1* const bad[2] = {&badMorph, &badPwm};
  bool rejected[2] = {false, false};
  bool formatKept[2] = {false, false};
  bool traceKept[2] = {false, false};
  bool prepared[2] = {false, false};

  for (int i = 0; i < 2; ++i) {
    EngineHarness hClean, hBad;
    hClean.reserve(6144);
    hBad.reserve(6144);
    if (!hClean.load(good, sr, bs) || !hBad.load(good, sr, bs)) continue;
    // Both render the SAME first window, so their DSP state is aligned before the reject.
    if (!hClean.render(2048) || !hBad.render(2048)) continue;
    prepared[i] = true;
    const double srBefore = hBad.sampleRate();
    const int bsBefore = hBad.blockSize();
    const int inBefore = hBad.inputCapability();
    const int outBefore = hBad.outputCapability();
    const int planOutCapBefore = hBad.plan().outputCapability;
    const int planOutCountBefore = hBad.plan().outputCount;
    const int planInCh0Before = hBad.plan().inputCh[0];
    const int planInCh1Before = hBad.plan().inputCh[1];

    const auto status = hBad.applyCanonical(*bad[i]);
    rejected[i] = status == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState;
    formatKept[i] = hBad.sampleRate() == srBefore && hBad.blockSize() == bsBefore &&
                    hBad.inputCapability() == inBefore && hBad.outputCapability() == outBefore &&
                    hBad.plan().valid() && hBad.plan().outputCapability == planOutCapBefore &&
                    hBad.plan().outputCount == planOutCountBefore &&
                    hBad.plan().inputCh[0] == planInCh0Before &&
                    hBad.plan().inputCh[1] == planInCh1Before;

    // The subsequent trace must be bit-identical to an engine that never saw the bad state.
    if (!hClean.render(2048) || !hBad.render(2048)) continue;
    bool same = hClean.dryA().size() == hBad.dryA().size();
    if (same) {
      for (std::size_t k = 0; k < hBad.dryA().size(); ++k) {
        if (hClean.dryA()[k] != hBad.dryA()[k]) {
          same = false;
          break;
        }
      }
    }
    traceKept[i] = same;
  }
  const bool rejectedApplyArmsWerePrepared = prepared[0] && prepared[1];
  CHECK(rejectedApplyArmsWerePrepared);
  const bool anOutOfRangeMorphIsRejectedAsInvalidState = rejected[0];
  CHECK(anOutOfRangeMorphIsRejectedAsInvalidState);
  const bool anOutOfRangePwmDepthIsRejectedAsInvalidState = rejected[1];
  CHECK(anOutOfRangePwmDepthIsRejectedAsInvalidState);
  const bool aRejectedApplyPreservesTheCommittedFormat = formatKept[0] && formatKept[1];
  CHECK(aRejectedApplyPreservesTheCommittedFormat);
  const bool aRejectedMorphApplyPreservesTheSubsequentTrace = traceKept[0];
  CHECK(aRejectedMorphApplyPreservesTheSubsequentTrace);
  const bool aRejectedPwmApplyPreservesTheSubsequentTrace = traceKept[1];
  CHECK(aRejectedPwmApplyPreservesTheSubsequentTrace);
}

// =============================================================================================
// (10) All four outputs finite across the morph x depth surface, at all four rates.
// =============================================================================================
struct SurfaceResult {
  bool finite = true;
  int cells = 0;
};

SurfaceResult surfaceAt(double sr) {
  SurfaceResult res;
  const double kMorph[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
  const double kDepth[3] = {0.0, 0.5, 1.0};
  for (int mi = 0; mi < 5; ++mi) {
    for (int di = 0; di < 3; ++di) {
      DeviceStateV1 st = make_default_device_state(kSeed);
      slot(st, ParameterId::vco_a_morph) = kMorph[mi];
      slot(st, ParameterId::vco_b_morph) = kMorph[4 - mi];
      slot(st, ParameterId::vco_a_pwm) = kDepth[di];
      slot(st, ParameterId::vco_b_pwm) = kDepth[(di + 1) % 3];
      slot(st, ParameterId::joystick_x) = kMorph[mi];
      setCable(st, JackId::joystick_x_out, JackId::vco_a_pwm_in);
      EngineHarness h;
      h.reserve(8192);
      if (!h.load(st, sr) || !h.render(4096)) {
        res.finite = false;
        continue;
      }
      ++res.cells;
      for (int c = 0; c < 4; ++c) {
        for (double x : h.out(c)) {
          if (!std::isfinite(x)) res.finite = false;
        }
      }
    }
  }
  return res;
}

void four_outputs_finite_across_the_surface() {
  const SurfaceResult s44k1 = surfaceAt(kRate44k1);
  const SurfaceResult s48k = surfaceAt(kRate48k);
  const SurfaceResult s88k2 = surfaceAt(kRate88k2);
  const SurfaceResult s96k = surfaceAt(kRate96k);
  std::printf("  surface cells: %d/%d/%d/%d\n", s44k1.cells, s48k.cells, s88k2.cells, s96k.cells);
  const bool surfaceWasFullyCovered =
      s44k1.cells == 15 && s48k.cells == 15 && s88k2.cells == 15 && s96k.cells == 15;
  CHECK(surfaceWasFullyCovered);
  const bool fourOutputsFiniteAcrossSurface44k1 = s44k1.finite && s44k1.cells == 15;
  const bool fourOutputsFiniteAcrossSurface48k = s48k.finite && s48k.cells == 15;
  const bool fourOutputsFiniteAcrossSurface88k2 = s88k2.finite && s88k2.cells == 15;
  const bool fourOutputsFiniteAcrossSurface96k = s96k.finite && s96k.cells == 15;
  CHECK(fourOutputsFiniteAcrossSurface44k1);
  CHECK(fourOutputsFiniteAcrossSurface48k);
  CHECK(fourOutputsFiniteAcrossSurface88k2);
  CHECK(fourOutputsFiniteAcrossSurface96k);
}

// =============================================================================================
// (11) SAVE BYTES and SOUND are asserted SEPARATELY: the wire change is confined to the parameter's
//      own f64 slot (a structural claim) and, independently, the audio changes (a sound claim).
// =============================================================================================
void save_bytes_and_sound_asserted_separately() {
  const std::size_t wire = kWire;
  DeviceStateV1 st = make_default_device_state(kSeed);
  DeviceStateV1 sMorph = st;
  slot(sMorph, ParameterId::vco_a_morph) = 0.25;
  DeviceStateV1 sPwm = st;
  slot(sPwm, ParameterId::vco_a_pwm) = 0.5;
  DeviceStateV1 sOther = st;
  slot(sOther, ParameterId::vco_b_morph) = 0.25;  // 9 ids above vco_a_morph

  std::vector<std::uint8_t> def(wire, 0), morphChanged(wire, 0), pwmChanged(wire, 0),
      otherChanged(wire, 0);
  std::size_t w = 0;
  const bool encoded =
      lunar24::core::encode_device_state(st, def.data(), def.size(), &w) &&
      lunar24::core::encode_device_state(sMorph, morphChanged.data(), morphChanged.size(), &w) &&
      lunar24::core::encode_device_state(sPwm, pwmChanged.data(), pwmChanged.size(), &w) &&
      lunar24::core::encode_device_state(sOther, otherChanged.data(), otherChanged.size(), &w);
  const bool saveBytesEncodeAllArms = encoded;
  CHECK(saveBytesEncodeAllArms);
  if (!saveBytesEncodeAllArms) return;

  // The wire diff of a single-parameter change. NOTE the encoding is NOT the raw f64 image: 0.5 and
  // 0.25 are 0x3FE0000000000000 and 0x3FD0000000000000, which differ in exactly ONE byte (LE index
  // 6), and 0.0 differs from 0.5 in exactly two. So the claim is not "8 bytes moved" — it is that
  // every moved byte lies inside ONE f64 parameter slot, and that the slot index moves by the
  // ParameterId delta. Both are read off the measurement, with no hardcoded base offset.
  struct Diff {
    std::size_t count = 0;
    std::size_t first = 0;
    std::size_t last = 0;
  };
  const auto diffOf = [&](const std::vector<std::uint8_t>& a,
                          const std::vector<std::uint8_t>& b) {
    Diff d{0, wire, 0};
    for (std::size_t i = 0; i < wire; ++i) {
      if (a[i] != b[i]) {
        if (d.count == 0) d.first = i;
        d.last = i;
        ++d.count;
      }
    }
    return d;
  };
  const Diff dMorph = diffOf(def, morphChanged);
  const Diff dPwm = diffOf(def, pwmChanged);
  const Diff dOther = diffOf(def, otherChanged);
  std::printf("  save bytes: morph(id %u) %zu bytes @ [%zu..%zu]  pwm(id %u) %zu @ [%zu..%zu]  "
              "b_morph(id %u) %zu @ [%zu..%zu]\n",
              static_cast<unsigned>(ParameterId::vco_a_morph), dMorph.count, dMorph.first, dMorph.last,
              static_cast<unsigned>(ParameterId::vco_a_pwm), dPwm.count, dPwm.first, dPwm.last,
              static_cast<unsigned>(ParameterId::vco_b_morph), dOther.count, dOther.first,
              dOther.last);

  // Every moved byte of a single-parameter change is inside ONE 8-byte (f64) parameter slot.
  const bool aMorphChangeStaysInsideOneParameterSlot =
      dMorph.count > 0 && dMorph.first / 8u == dMorph.last / 8u;
  CHECK(aMorphChangeStaysInsideOneParameterSlot);
  const bool aPwmDepthChangeStaysInsideOneParameterSlot =
      dPwm.count > 0 && dPwm.first / 8u == dPwm.last / 8u;
  CHECK(aPwmDepthChangeStaysInsideOneParameterSlot);
  // ... and the slot index moves EXACTLY by the ParameterId delta: vco_a_morph(1) -> vco_b_morph(10).
  const std::size_t idDeltaMorphToBMorph =
      static_cast<std::size_t>(ParameterId::vco_b_morph) -
      static_cast<std::size_t>(ParameterId::vco_a_morph);
  const bool theParameterSlotIndexMovesByTheParameterIdDelta =
      dOther.first / 8u == dMorph.first / 8u + idDeltaMorphToBMorph;
  CHECK(theParameterSlotIndexMovesByTheParameterIdDelta);
  // ... and the PWM depth slot sits at its own id offset above the morph slot.
  const std::size_t idDeltaMorphToPwm = static_cast<std::size_t>(ParameterId::vco_a_pwm) -
                                        static_cast<std::size_t>(ParameterId::vco_a_morph);
  const bool thePwmDepthSlotSitsAtItsOwnIdOffsetAboveMorph =
      dPwm.first / 8u == dMorph.first / 8u + idDeltaMorphToPwm;
  CHECK(thePwmDepthSlotSitsAtItsOwnIdOffsetAboveMorph);

  // The VALUE claims go through the producer's own decoder, so they do not depend on my byte
  // arithmetic: the default wire image still decodes to the descriptor initial, and the changed
  // image decodes bit-exactly to the changed value.
  const auto decodeOf = [&](const std::vector<std::uint8_t>& buf) {
    DeviceStateV1 out;
    const bool ok = lunar24::core::decode_device_state(buf.data(), buf.size(), &out);
    return std::pair<bool, DeviceStateV1>{ok, out};
  };
  const auto rDef = decodeOf(def);
  const auto rMorph = decodeOf(morphChanged);
  const auto rPwm = decodeOf(pwmChanged);
  const bool theDefaultWireImageDecodesToTheDescriptorInitials =
      rDef.first &&
      rDef.second.parameters[static_cast<std::uint32_t>(ParameterId::vco_a_morph)] ==
          declaredInitial(ParameterId::vco_a_morph) &&
      rDef.second.parameters[static_cast<std::uint32_t>(ParameterId::vco_a_pwm)] ==
          declaredInitial(ParameterId::vco_a_pwm);
  CHECK(theDefaultWireImageDecodesToTheDescriptorInitials);
  const bool theChangedMorphValueIsWireExact =
      rMorph.first && rMorph.second.parameters[static_cast<std::uint32_t>(
                                     ParameterId::vco_a_morph)] == 0.25;
  CHECK(theChangedMorphValueIsWireExact);
  const bool theChangedPwmValueIsWireExact =
      rPwm.first &&
      rPwm.second.parameters[static_cast<std::uint32_t>(ParameterId::vco_a_pwm)] == 0.5;
  CHECK(theChangedPwmValueIsWireExact);

  // SEPARATELY: the same two states must differ in SOUND. A byte-faithful save with no audible
  // consequence would be its own defect, so this is asserted independently of the byte claims.
  const auto traceOf = [&](const DeviceStateV1& s) {
    EngineHarness h;
    h.reserve(8192);
    if (!h.load(s, kRate48k) || !h.render(8192)) return std::vector<double>{};
    return h.dryA();
  };
  const auto tDef = traceOf(st);
  const auto tMorph = traceOf(sMorph);
  double worst = 0.0;
  bool comparable = tDef.size() == tMorph.size() && !tDef.empty();
  if (comparable) {
    for (std::size_t i = 0; i < tDef.size(); ++i) {
      worst = std::max(worst, std::fabs(tDef[i] - tMorph[i]));
    }
  }
  std::printf("  save-bytes vs sound: worst audio delta for morph 0.50->0.25 = %.6f\n", worst);
  const bool aMorphByteChangeIsAlsoASoundChange = comparable && worst > 1e-6;
  CHECK(aMorphByteChangeIsAlsoASoundChange);
}

// =============================================================================================
// (12) Cable override and unplug: patching the PWM jack changes the duty; removing the patch
//      restores the unpatched render bit-exactly; and an unpatched sink reads exactly 0.
// =============================================================================================
void cable_override_and_unplug_restore() {
  DeviceStateV1 base = pulseState(0.75);
  DeviceStateV1 patched = base;
  slot(patched, ParameterId::lfo_a_rate) = 5.0;
  setCable(patched, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);

  const auto renderTo = [&](const DeviceStateV1& s) {
    EngineHarness h;
    h.reserve(24000);
    if (!h.load(s, kRate48k) || !h.render(24000)) return std::vector<double>{};
    return h.dryA();
  };
  const auto tBase = renderTo(base);
  const auto tPatched = renderTo(patched);
  const auto tRestored = renderTo(base);  // the unplug: byte-identical state to the base
  const bool cableRestoreArmsRendered =
      !tBase.empty() && tBase.size() == tPatched.size() && tBase.size() == tRestored.size();
  CHECK(cableRestoreArmsRendered);
  if (!cableRestoreArmsRendered) return;

  double dPatched = 0.0, dRestored = 0.0;
  for (std::size_t i = 0; i < tBase.size(); ++i) {
    dPatched += std::fabs(tBase[i] - tPatched[i]);
    dRestored += std::fabs(tBase[i] - tRestored[i]);
  }
  std::printf("  cable: |base-patched|=%.6f |base-restored|=%.3e\n", dPatched, dRestored);
  const bool patchingThePwmJackChangesTheRender = dPatched > 1e-3;
  CHECK(patchingThePwmJackChangesTheRender);
  const bool unpluggingThePwmJackRestoresTheRenderBitExactly = dRestored == 0.0;
  CHECK(unpluggingThePwmJackRestoresTheRenderBitExactly);

  // No cable at all: the sink reads exactly 0 and the duty is the declared base pw.
  DeviceStateV1 unpatched = pulseState(1.0);
  EngineHarness h;
  h.reserve(20000);
  if (!h.load(unpatched, kRate48k) || !h.render(20000)) {
    const bool unpatchedPwmRenderCompleted = false;
    CHECK(unpatchedPwmRenderCompleted);
    return;
  }
  const bool anUnpatchedPwmSinkReadsExactlyZero = h.runtime()->vcoAPwmCv() == 0.0;
  CHECK(anUnpatchedPwmSinkReadsExactlyZero);
  const double duty = meanAudioDuty(h.dryA());
  const bool anUnpatchedPwmSinkLeavesTheBasePwDuty =
      duty >= 0.0 && std::fabs(duty - declaredInitial(ParameterId::vco_a_pw)) <=
                         oneSampleDutyTol(kRate48k);
  CHECK(anUnpatchedPwmSinkLeavesTheBasePwDuty);
}

// =============================================================================================
// (13) Isolated B: the default A->B normalised route LEGITIMATELY modulates B (so B's audio
//      high-time fraction is NOT its duty), while a B-only change never reaches A.
// =============================================================================================
void isolated_b_and_the_legitimate_default_route() {
  const double defaultCvAmt = declaredInitial(ParameterId::vco_b_cv_amt);
  const bool theDefaultBCvAmountIsALiveRoute = defaultCvAmt > 0.0;
  CHECK(theDefaultBCvAmountIsALiveRoute);
  if (!theDefaultBCvAmountIsALiveRoute) return;

  // Default route live: B is frequency-modulated by A, so its audio high-time is not its duty.
  DeviceStateV1 live = pulseState(0.0);
  slot(live, ParameterId::vco_b_cv_amt) = defaultCvAmt;
  EngineHarness hLive;
  hLive.reserve(24000);
  if (!hLive.load(live, kRate48k) || !hLive.render(24000)) {
    const bool defaultRouteRenderCompleted = false;
    CHECK(defaultRouteRenderCompleted);
    return;
  }
  const double audioDutyB = meanAudioDuty(hLive.dryB());
  const double readbackB = hLive.runtime()->vcoBEffectiveDuty();
  std::printf("  default route: B audio high-time=%.6f readback duty=%.6f\n", audioDutyB, readbackB);
  const bool theDefaultRouteDoesNotChangeBOwnDutyReadback =
      std::fabs(readbackB - declaredInitial(ParameterId::vco_a_pw)) < 1e-9;
  CHECK(theDefaultRouteDoesNotChangeBOwnDutyReadback);
  const bool theDefaultAToBRouteLegitimatelyMovesBAudioOffItsDuty =
      audioDutyB >= 0.0 && std::fabs(audioDutyB - readbackB) > 0.05;
  CHECK(theDefaultAToBRouteLegitimatelyMovesBAudioOffItsDuty);

  // The control for the line above: with B isolated, B's audio high-time IS its duty.
  EngineHarness hIso;
  hIso.reserve(24000);
  if (!hIso.load(pulseState(0.0), kRate48k) || !hIso.render(24000)) {
    const bool isolatedBRenderCompleted = false;
    CHECK(isolatedBRenderCompleted);
    return;
  }
  const double isoAudioB = meanAudioDuty(hIso.dryB());
  const double isoReadbackB = hIso.runtime()->vcoBEffectiveDuty();
  std::printf("  isolated B: audio=%.6f readback=%.6f\n", isoAudioB, isoReadbackB);
  const bool withBIsolatedBAudioHighTimeEqualsItsDuty =
      std::fabs(isoAudioB - isoReadbackB) <= oneSampleDutyTol(kRate48k);
  CHECK(withBIsolatedBAudioHighTimeEqualsItsDuty);

  // A B-only change must never reach A.
  DeviceStateV1 b1 = make_default_device_state(kSeed);
  DeviceStateV1 b2 = b1;
  slot(b1, ParameterId::vco_b_morph) = 0.0;
  slot(b2, ParameterId::vco_b_morph) = 1.0;
  const auto traceA = [&](const DeviceStateV1& s) {
    EngineHarness h;
    h.reserve(24000);
    if (!h.load(s, kRate48k) || !h.render(24000)) return std::vector<double>{};
    return h.dryA();
  };
  const auto a1 = traceA(b1);
  const auto a2 = traceA(b2);
  bool identical = !a1.empty() && a1.size() == a2.size();
  if (identical) {
    for (std::size_t i = 0; i < a1.size(); ++i) {
      if (a1[i] != a2[i]) {
        identical = false;
        break;
      }
    }
  }
  const bool aBOnlyMorphChangeNeverReachesVcoA = identical;
  CHECK(aBOnlyMorphChangeNeverReachesVcoA);
}

// =============================================================================================
// (14) The audio CALLBACK allocates and frees nothing on the morph/PWM surface. The callback is
//      StandaloneAudioEngine::processBlock on caller-owned planar buffers, so the buffers are sized
//      OUTSIDE the measured window and the counter must not move across it.
// =============================================================================================
void render_callback_allocates_nothing() {
  const int frames = 512;
  DeviceStateV1 st = pulseState(0.8);
  slot(st, ParameterId::joystick_x) = 0.75;
  setCable(st, JackId::joystick_x_out, JackId::vco_a_pwm_in);
  EngineHarness h;
  if (!h.load(st, kRate48k, frames)) {
    const bool allocatorRenderWindowCommitted = false;
    CHECK(allocatorRenderWindowCommitted);
    return;
  }
  std::vector<double> i0(frames, 0.0), i1(frames, 0.0), o0(frames), o1(frames), o2(frames),
      o3(frames);
  const double* in[kInCh] = {i0.data(), i1.data()};
  double* out[kOutCh] = {o0.data(), o1.data(), o2.data(), o3.data()};

  // Settle any first-call initialisation BEFORE the measured window, then measure.
  bool rendered = h.processPure(in, out, frames) == StandaloneAudioEngine::Status::Rendered;
  const std::size_t allocBefore = g_allocCount;
  const std::size_t freeBefore = g_freeCount;
  for (int i = 0; i < 24; ++i) {
    if (h.processPure(in, out, frames) != StandaloneAudioEngine::Status::Rendered) rendered = false;
  }
  const bool theRenderCallbackAllocatesNothing = rendered && g_allocCount == allocBefore;
  CHECK(theRenderCallbackAllocatesNothing);
  const bool theRenderCallbackFreesNothing = rendered && g_freeCount == freeBefore;
  CHECK(theRenderCallbackFreesNothing);

  // Positive control: the counters are live on this path (a deliberate allocation IS detected), so a
  // green above cannot be a silently dead probe.
  const std::size_t aSnap = g_allocCount;
  void* p = ::operator new(4);
  const bool theAllocatorProbeDetectsAnAllocation = g_allocCount > aSnap;
  CHECK(theAllocatorProbeDetectsAnAllocation);
  ::operator delete(p);
  const std::size_t fSnap = g_freeCount;
  void* q = ::operator new(4);
  ::operator delete(q);
  const bool theAllocatorProbeDetectsAFree = g_freeCount > fSnap;
  CHECK(theAllocatorProbeDetectsAFree);
}

}  // namespace

int main() {
  std::printf("GH#19 S0 acceptance: morph map + two-sided PWM through the agreed entry\n");
  changing_cv_tracks_the_ruled_transfer();
  dc_cv_law_and_inversion_and_saturation();
  depth_arms_and_depth_zero_inertness();
  same_frame_consumption();
  morph_sweep_continuity_and_nodes();
  morph_ab_asymmetry();
  irregular_blocks_are_partition_invariant();
  repeated_restore_tracks_the_state();
  rejected_apply_is_atomic_for_morph_and_pwm();
  four_outputs_finite_across_the_surface();
  save_bytes_and_sound_asserted_separately();
  cable_override_and_unplug_restore();
  isolated_b_and_the_legitimate_default_route();
  render_callback_allocates_nothing();
  return ::test::finish("test_gh19_s0_morph_pwm_acceptance");
}
