// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// task #110 (GH#19 S2): the polyBLEP kernel's own identities, and the TWO-LAYER
// discharge of its declared precondition `0 <= dt <= 0.5`.
//
// WHY TWO LAYERS, and why neither alone is enough (@Kimi e6e645d2 ruling 2):
//
//   * The DECLARED DOMAIN answers "what can the design reach". It has an authoritative
//     chain -- registry descriptor -> dspParamValid_ entry gate -> the runtime's norm-to-
//     unit mapping -- so the TUNE and VOLT half of dt is bounded by construction rather
//     than by sampling. But it says nothing about inputs the runtime supplies from
//     OUTSIDE the registry (the CV MOD jack), and it is a statement about the design, not
//     about any particular render.
//   * The MEASURED BOUND answers "what is actually reached". It renders the real
//     DroneBank with every disturbance live and reads dt off the executed phase
//     accumulator. It covers the inputs the registry does not bound, but it is only as
//     strong as its sweep.
//
// The product's reachable dt sits inside the intersection, so BOTH are asserted and both
// numbers are printed: a report quoting only one of them is quoting half a discharge.
//
// THE DECLARED CHAIN, with the file:line of each link this file depends on:
//   generated/lunar24/registry.hpp:389-393  drone_1_tune_1..5  "norm", min 0, max 1, confirmed
//   generated/lunar24/registry.hpp:404      drone_1_volt      "norm", min 0, max 1, confirmed
//   core/include/lunar24/core/machine_runtime.h:2735-2744  dspParamValid_ = min <= v <= max
//   core/include/lunar24/core/machine_runtime.h:2788       tune  -> (n-0.5)*24 = -12..+12 semis
//   core/include/lunar24/core/machine_runtime.h:2791       volt  -> 60*n       = 0..60 semis down
// so TUNE in [-12,+12] and VOLT in [0,60] are the unreachable-from-outside bounds, and the
// declared maximum of the frequency is attained at (tune = +12, volt = 0).
//
// WHAT IS *NOT* IN THAT CHAIN, stated so the gap is not read as covered by it:
//   * the group CV MOD input (drone_bank.h:285 `modCvG_`, written in production at
//     machine_runtime.h:3065/3097 from a patched CV route) -- registry-unbounded;
//   * drone_bank.h:202/207/203-205/228 are plain field stores with no clamp, so none of
//     the setters is a guard; the registry guards only the two ids it declares.
// The measured half drives the CV MOD input explicitly for exactly that reason.
//
// The sample-rate hole (host/iPlug_app_host_override.cpp:190) is a known UNCLOSED gap and
// is NOT asserted here: this test fixes sr from a literal list.

#include "mini_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include <lunar24/core/drone_bank.h>
#include <lunar24/core/polyblep_kernel.h>

namespace core = lunar24::core;

namespace {

constexpr double kTwoPi = 6.283185307179586;

// =====================================================================================
// PART 1 -- the kernel's identities. Every assertion here is a claim made in the kernel
// header's own comments; if one is false the header is lying to its callers.
// =====================================================================================
void testKernelIdentities() {
  const double dt = 0.0424;  // the largest S2 measurement-cell value at 44.1 kHz.

  // R(0+) == -1 and R(1-) == +1: this is why the corrected saw's jump is removed.
  CHECK(std::fabs(core::polyblepResidual(0.0, dt) - (-1.0)) < 1e-15);
  CHECK(std::fabs(core::polyblepResidual(1.0 - 1e-15, dt) - 1.0) < 1e-12);
  // Both inner window edges reach 0, so the two branches agree with "0 otherwise".
  CHECK(std::fabs(core::polyblepResidual(dt - 1e-15, dt)) < 1e-12);
  CHECK(std::fabs(core::polyblepResidual(1.0 - dt, dt)) < 1e-15);

  // OUTSIDE the window the residual is EXACTLY zero, not merely small. That is the whole
  // reason the correction is a no-op away from the rollover: `(2t-1) - 0.0` is bit-identical
  // to the naive `2*(phase/2pi) - 1`, so on the classic saw the fix can only change the
  // samples in the +/-dt neighbourhood. The exactness is load-bearing, not cosmetic.
  double worstOutside = 0.0;
  for (double t = dt + 1e-9; t < 1.0 - dt - 1e-9; t += 1e-5)
    worstOutside = std::fmax(worstOutside, std::fabs(core::polyblepResidual(t, dt)));
  CHECK(worstOutside == 0.0);

  // The corrected waveform is continuous across the wrap: both one-sided limits are 0.
  CHECK(std::fabs(core::polyblepSaw(0.0, dt)) < 1e-15);
  CHECK(std::fabs(core::polyblepSaw(1.0 - 1e-15, dt)) < 1e-9);

  // |R| <= 1 -- the residual can never exceed the +-1 the jump itself supplies.
  double rmin = 1e9, rmax = -1e9;
  for (double t = 0.0; t < 1.0; t += 1e-6) {
    rmin = std::fmin(rmin, core::polyblepResidual(t, dt));
    rmax = std::fmax(rmax, core::polyblepResidual(t, dt));
  }
  CHECK(rmin >= -1.0 - 1e-12);
  CHECK(rmax <= 1.0 + 1e-12);

  // Degenerate dt leaves the waveform untouched bit for bit (the zero-disturbance edge).
  double worstDt0 = 0.0;
  for (double t = 0.0; t < 1.0; t += 1e-4)
    worstDt0 = std::fmax(worstDt0, std::fabs(core::polyblepResidual(t, 0.0)));
  CHECK(worstDt0 == 0.0);

  // The contract predicate and its boundary.
  CHECK(!core::polyblepSupportReachesHalfPeriod(0.5 - 1e-12));
  CHECK(core::polyblepSupportReachesHalfPeriod(0.5));
  CHECK(core::polyblepSupportReachesHalfPeriod(0.6));
  CHECK_EQ(core::kPolyblepMaxDt, 0.5);
}

// =====================================================================================
// PART 2 -- the DECLARED bound. Seed-independent by construction: it uses the SUPREMUM of
// the kHigh band, not any drawn frequency, so it is a statement about the design rather
// than about one render. `extraHz` exists so PART 3 can ask "and how much does the
// registry-unbounded CV MOD term widen it", without the declared number itself absorbing
// a term the registry does not bound.
// =====================================================================================
double declaredMaxFreqHz(double extraHz = 0.0) {
  const double kHighBandSupHz = 1800.0;  // drone_bank.h:175  420 + u*1380, u < 1
  const double kTuneScaleMax = 2.0;      // (n-0.5)*24 at n = 1  -> +12 semis
  const double kVoltScaleMin = 1.0;      // 60*n        at n = 0  -> 0 semis down
  const double kToleranceSup = 1.02;     // nextUnit(0, 0.02) is exclusive of 0.02
  // driftNow is NOT scaled by tune/volt (drone_bank.h:283 adds it after `base`), so its
  // supremum is 0.02 * freqBase -- not 0.02 * base.
  const double kDriftSupHz = 0.02 * kHighBandSupHz;
  const double kJitterSupHz = 0.02;  // kOscNoiseAmpHz
  // Mutual FM is OFF at volt <= 30, and for volt > 30 the base's volt term falls at ~37.5
  // Hz per semitone at the midpoint against FM's +2.67 Hz per semitone, so the sum stays
  // strictly decreasing across that whole region. The box maximum really is at volt = 0.
  return kHighBandSupHz * kTuneScaleMax * kVoltScaleMin * kToleranceSup + kDriftSupHz +
         kJitterSupHz + extraHz;
}

void testDeclaredBound() {
  const double fmax = declaredMaxFreqHz();
  // 1800 * 2 * 1.02 + 36 + 0.02 = 3708.02 Hz.
  CHECK(std::fabs(fmax - 3708.02) < 0.01);

  const double srWorst = 44100.0;  // the LOWEST supported rate is the worst case for dt
  const double dt = fmax / srWorst;
  CHECK(dt > 0.0 && dt < core::kPolyblepMaxDt);
  CHECK(dt * 2.0 < 0.2);  // margin > 5x, asserted rather than merely printed
  std::printf("  [declared] TUNE in [-12,+12] x VOLT in [0,60] => f0 <= %.2f Hz, "
              "dt <= %.6f @ %.0f Hz (margin %.2fx)\n",
              fmax, dt, srWorst, core::kPolyblepMaxDt / dt);
}

// =====================================================================================
// PART 3 -- the MEASURED bound, read off the executed phase accumulator.
//
// dt is recovered from the phase rather than recomputed from the frequency model: a
// re-derivation would only restate the constants, while reading phaseOf() after a tick
// measures the value the oscillator actually advanced by, including any term the model in
// the kernel header does not name.
//
// HONEST SCOPE: the number this prints is the max over the renders BELOW, so it is a lower
// bound on the true reachable maximum (the box in PART 2 is the authority on what is
// design-reachable; this is the authority on what was reached). At the swept settings the
// dominance argument puts the worst case at volt = 0 / tune = +12, where mutual FM is off
// and the only extra term is CV MOD -- which is why CV MOD is swept explicitly.
//
// The unwrap is unambiguous for dt < 1. Since the assertion is dt <= 0.5, a violation
// large enough to alias past 1 instead of showing up would have to be a >2x overshoot on
// top of an already-failing result; the measured values are printed so that gap stays
// visible rather than assumed away.
// =====================================================================================
struct MaxDt {
  double dt = 0.0;
  double tune = 0.0;
  double volt = 0.0;
  double sr = 0.0;
  double modAmount = 0.0;
  double groupModCv = 0.0;
  std::size_t voice = 0;
};

MaxDt sweepOne(double sr, double tune, double volt, double modAmount, double groupModCv,
               std::uint64_t seed, int samples) {
  const std::size_t n = core::DroneBank::kMaxVoices;
  core::DroneBank bank(seed, sr, n, /*driftEnabled=*/true);
  const std::size_t groups = n / core::DroneBank::kGensPerVoice;
  for (std::size_t g = 0; g < groups; ++g) {
    bank.setVolt(g, volt);
    bank.setGroupModCv(static_cast<int>(g), groupModCv);
  }
  for (std::size_t i = 0; i < n; ++i) {
    bank.setTune(i, tune);
    bank.setMod(i, modAmount);
  }

  double phaseBefore[core::DroneBank::kMaxVoices];
  double out[core::DroneBank::kMaxVoices];
  MaxDt m;
  m.tune = tune;
  m.volt = volt;
  m.sr = sr;
  m.modAmount = modAmount;
  m.groupModCv = groupModCv;

  for (std::size_t i = 0; i < n; ++i) phaseBefore[i] = bank.phaseOf(i);

  for (int k = 0; k < samples; ++k) {
    bank.tick(out);
    for (std::size_t i = 0; i < n; ++i) {
      const double now = bank.phaseOf(i);
      double d = now - phaseBefore[i];
      if (d < 0.0) d += kTwoPi;
      const double dt = d / kTwoPi;
      if (dt > m.dt) {
        m.dt = dt;
        m.voice = i;
      }
      phaseBefore[i] = now;
    }
  }
  return m;
}

void testMeasuredBound() {
  const double kSrs[] = {44100.0, 48000.0, 88200.0, 96000.0};
  // TUNE endpoints and interior; VOLT every 5 semitones across its declared range; MOD off
  // and at full; the registry-unbounded CV MOD at zero and both signs.
  const double kTunes[] = {-12.0, -6.0, 0.0, 6.0, 12.0};
  const double kMods[] = {0.0, 1.0};
  const double kCvs[] = {0.0, 4.0, -4.0};
  double cvSupHz = 0.0;
  for (double mod : kMods)
    for (double cv : kCvs) cvSupHz = std::fmax(cvSupHz, std::fabs(mod * cv));

  MaxDt worst{};
  long cases = 0;
  for (double sr : kSrs) {
    for (double tune : kTunes) {
      for (double volt = 0.0; volt <= 60.0; volt += 5.0) {
        for (double mod : kMods) {
          for (double cv : kCvs) {
            const MaxDt m = sweepOne(sr, tune, volt, mod, cv, 0x5A17C0DEu, 2048);
            ++cases;
            if (m.dt > worst.dt) worst = m;
          }
        }
      }
    }
  }

  const double f0Reached = worst.dt * worst.sr;
  std::printf("  [measured] %ld render(s) x 2048 samples, all disturbances live, "
              "max dt = %.6f (f0 %.2f Hz @ %.0f Hz, TUNE %+.0f, VOLT %.0f down, "
              "MOD %.1f, CV %+.1f, voice %zu)\n",
              cases, worst.dt, f0Reached, worst.sr, worst.tune, worst.volt,
              worst.modAmount, worst.groupModCv, worst.voice);
  std::printf("  [measured] margin to the 0.5 contract: %.2fx\n",
              core::kPolyblepMaxDt / worst.dt);

  CHECK(worst.dt > 0.0);
  CHECK(!core::polyblepSupportReachesHalfPeriod(worst.dt));
  CHECK(worst.dt <= core::kPolyblepMaxDt);
  // Consistency between the layers: what a real render reached must sit inside the box the
  // declared chain allows, widened by the largest CV MOD term this sweep applied. If this
  // fires, either the box is too small (a term was missed in PART 2) or the sweep found a
  // frequency the design claims is unreachable -- both are findings, not noise.
  const double boxHz = declaredMaxFreqHz(cvSupHz);
  std::printf("  [cross-check] reached %.2f Hz vs box %.2f Hz (+%.0f Hz CV MOD), "
              "headroom %.2f Hz\n",
              f0Reached, boxHz, cvSupHz, boxHz - f0Reached);
  CHECK(f0Reached <= boxHz + 1e-6);

  // How far past the box the contract survives: at 44.1 kHz the kernel's domain ends at
  // 22050 Hz. Recorded so a future reader can see the headroom the unclamped setters have
  // before the precondition breaks, rather than having to recompute it.
  const double breakHz = core::kPolyblepMaxDt * 44100.0;
  std::printf("  [headroom] the contract breaks at %.0f Hz @ 44.1 kHz = %.2fx the "
              "declared maximum\n",
              breakHz, breakHz / declaredMaxFreqHz());
}

}  // namespace

int main() {
  std::printf("test_polyblep_kernel: identities, declared bound, measured bound\n");
  testKernelIdentities();
  testDeclaredBound();
  testMeasuredBound();
  return test::finish("test_polyblep_kernel");
}
