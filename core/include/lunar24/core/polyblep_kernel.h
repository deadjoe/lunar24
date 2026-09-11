// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// task #110 (GH#19 S2): the polyBLEP kernel -- the VALUE-jump counterpart of
// blamp_kernel.h.
//
// THE PAIRING IS THE POINT, and it is not a matter of taste:
//
//   * blamp_kernel.h  -> a SLOPE (first-derivative) discontinuity. Used by the VCO
//                        triangle and the Schmitt ramp, whose corners are slope
//                        reversals. That kernel is a WINDOWED analytic residual read
//                        from a generated LUT (C1 Hann window, L=8 support).
//   * this header     -> a VALUE (zeroth-order) discontinuity. Used by the classic
//                        drone sawtooth, which is a ramp with a jump.
//
// A saw is a value jump, so BLAMP is the wrong tool for it: band-limiting a value jump
// with a sloped kernel leaves the jump itself in place. This is the S1 correction
// recorded against the GH#19 baseline ("Schmitt audio output is a triangle => BLAMP
// family, not polyBLEP") read in the other direction -- each discontinuity class gets
// its own family, and neither kernel substitutes for the other.
//
// WHAT THIS IS
//
// The standard truncated polyBLEP residual (2nd-order / quadratic, the widely used
// form) for a unit sawtooth whose phase `t` is normalized to [0,1) and whose per-sample
// phase increment is `dt`:
//
//   R(t, dt) =  2u - u^2 - 1,   u = t/dt,       for t < dt        (below the jump)
//             =  u^2 + 2u + 1,  u = (t-1)/dt,   for t > 1 - dt    (above the jump)
//             =  0,                              otherwise
//
// with the corrected waveform `saw(t) = (2t - 1) - R(t, dt)`. The residual is a
// POLYNOMIAL, evaluated exactly -- there is no LUT, no interpolation and no window
// here, deliberately: the two kernels are not required to share a representation, only
// a division of labour. Support is +/- dt, i.e. FREQUENCY-DEPENDENT (two samples wide
// in the worst case), whereas the BLAMP table's support is a fixed 8 samples.
//
// WHY THE CORRECTION REMOVES THE JUMP: R(0+) = -1 and R(1-) = +1, while the naive saw
// takes +1 (t -> 1-) and -1 (t = 0). Subtracting R drives BOTH limits to 0, so the
// corrected waveform is continuous across the wrap and the -2 jump is spent as a ramp
// over the +/- dt neighbourhood instead of as a step. Note what is NOT claimed: this
// SUPPRESSES the aliasing that the step would radiate, it does not eliminate aliasing,
// and the truncated residual is an approximation - not the exact infinite-support
// kernel. No "exact" and no "no-droop" claim is made anywhere on this path.
//
// ---------------------------------------------------------------------------------
// CONTRACT (by-contract mathematical precondition, NOT a runtime guard)
// ---------------------------------------------------------------------------------
//
// The caller MUST satisfy `0 <= dt <= 0.5`. Option (B), @Kimi 612691ea: this header
// does NOT carry a runtime fallback to the naive waveform. A caller that violates the
// precondition is OUTSIDE THIS KERNEL'S DOMAIN OF DEFINITION; silently switching to a
// different waveform to "cope" would hide the violation inside the measurement it is
// supposed to be isolated from.
//
// This is stated as a precondition on the caller's increment because it CANNOT be
// stated as a theorem about the product's setters. In drone_bank.h the frequency sum
// that produces dt takes terms from setters that do not clamp:
//
//   setTune            -> voices_[gen].tune        is a plain field store  (:202)
//   setVolt            -> voices_[i].volt          is a plain field store  (:207)
//   setMod / setModCv  -> modAmount * (modCv + modCvG_[g])                 (:203-205)
//   setEnvironment     -> environmentHz_           is host-supplied, no clamp
//
// so `dt <= 0.5` is a statement about the INPUTS, never about the API. It is discharged in
// two layers, BOTH asserted in tests/core/test_polyblep_kernel.cpp (@Kimi e6e645d2 ruling
// 2); a report quoting only one of them is quoting half a discharge.
//
// LAYER 1 -- THE DECLARED DOMAIN: what the design can reach. The TUNE/VOLT half has an
// authoritative chain, so it is bounded by construction rather than by sampling:
//
//   generated/lunar24/registry.hpp:389-393  drone_1_tune_1..5  "norm", 0..1, confirmed
//   generated/lunar24/registry.hpp:404      drone_1_volt      "norm", 0..1, confirmed
//   machine_runtime.h:2735-2744             dspParamValid_ = min <= v <= max, at the entry
//   machine_runtime.h:2788                  tune -> (n-0.5)*24  = -12..+12 semitones
//   machine_runtime.h:2791                  volt -> 60*n        = 0..60 semitones down
//
// giving TUNE in [-12,+12] and VOLT in [0,60]. The bank's own constants close the rest:
//
//   freqBase       <= 1800 Hz        kHigh band 420 + u*1380                (:175)
//   (1 + tolerance) < 1.02           tolerance = nextUnit(0, 0.02)          (:177)
//   |drift|        <= 0.02*freqBase  driftA1, driftA2 < 0.01 each           (:186)
//   |jitter|       <= 0.02 Hz        kOscNoiseAmpHz                         (:109)
//
// The box maximum is at (tune = +12, volt = 0): voltScale is monotone decreasing, and for
// volt > 30 the base falls at ~37.5 Hz/semitone at the midpoint while mutual FM's
// 4.0*(volt-30)*|lastSample_| rises by no more than 4.0*(2/3) = 2.67, so the sum is
// strictly decreasing there too; at volt = 30 exactly the branch is off anyway, because
// `volt > kVvoltMid` is false. Hence
//
//   f0 <= 1800*2*1.02 + 0.02*1800 + 0.02 = 3708.02 Hz
//   dt <= 3708.02/44100                   = 0.084082   (5.95x inside the contract)
//
// At the S2 measurement cells (volt = 30, modAmount = 0, environmentHz_ = 0) nothing is
// transposed and mutual FM is off, so effFreq <= 1800*1.04 + 0.02 ~= 1872 Hz and
// dt ~= 0.0424 at 44.1 kHz -- the value the kernel's own test uses as its sample dt.
//
// LAYER 2 -- THE MEASURED BOUND: what is actually reached. Layer 1 cannot speak about the
// inputs the runtime supplies from OUTSIDE the registry, and one of those is product-live:
// machine_runtime.h:3065/3097 writes `modCvG_` from a patched CV route, and it enters the
// frequency sum at :285 with no clamp anywhere on the way. So the test renders the real
// DroneBank with tolerance, drift, jitter and mutual FM all live, sweeps TUNE over its
// declared range x VOLT over its declared range x MOD x the CV MOD input at both signs,
// and reads dt off the EXECUTED phase accumulator (phaseOf()), not off the model. Measured
// on the record: max dt = 0.077977 (f0 3438.81 Hz @ 44.1 kHz) = 6.41x inside the contract,
// with 273 Hz of headroom below the declared box.
//
// Layer 2 can only see the renders it made, so it is a lower bound on the reachable
// maximum; layer 1 is the authority on what the design allows. Neither replaces the other,
// and the CV MOD input is the reason the measured layer exists at all.
//
// The sample-rate hole (host/iPlug_app_host_override.cpp:190) is a KNOWN, UNCLOSED gap
// and is recorded here rather than papered over: this precondition is only as strong as
// the caller's sample-rate and increment provenance.

#pragma once

#include <cmath>

namespace lunar24::core {

// The contract boundary. Below/at half a period the two correction windows
// [0, dt) and (1-dt, 1] are disjoint (they touch at exactly dt = 0.5); beyond it they
// overlap and the phase-local model this residual is built on no longer holds.
inline constexpr double kPolyblepMaxDt = 0.5;

// True when the caller's increment has left the kernel's domain of definition.
// Mirrors blampSupportReachesHalfPeriod() in blamp_kernel.h so both discontinuity
// classes expose the same shape of predicate and a caller can test either without
// knowing which family it is holding. NOTE the difference in what the caller must do:
// the BLAMP header's callers return the bounded naive waveform on that branch, while
// here option (B) means there is no fallback at all -- a true result is a CONTRACT
// VIOLATION to be reported, not a branch to be handled.
inline bool polyblepSupportReachesHalfPeriod(double dt) {
  return dt >= kPolyblepMaxDt;
}

// The dimensionless polyBLEP residual R(t, dt) for a NORMALIZED phase t in [0,1) and a
// normalized per-sample increment dt in [0, 0.5]. Pure function of its arguments: no
// cross-block state, no allocation, no latency, causal. A dt of 0 (or any value for
// which neither window applies) yields exactly 0, leaving the caller's waveform
// untouched bit for bit.
//
// Deliberately NOT clamped here: a dt outside [0, 0.5] is a caller contract violation
// (see the header note), and folding a clamp into the kernel would convert a detectable
// violation into a silent waveform substitution.
inline double polyblepResidual(double t, double dt) {
  if (t < dt) {
    const double u = t / dt;
    return u + u - u * u - 1.0;             // 2u - u^2 - 1, from -1 at t=0 to 0 at t=dt
  }
  if (t > 1.0 - dt) {
    const double u = (t - 1.0) / dt;
    return u * u + u + u + 1.0;             // u^2 + 2u + 1, from 0 at t=1-dt to +1 at t=1
  }
  return 0.0;                               // exactly 0 outside the +/- dt neighbourhood
}

// The corrected unit sawtooth: the naive ramp minus the residual. Callers that need the
// jump spent as a ramp add this to their own oscillator, they do not re-derive it.
//
// PROPERTY THE S2 ACCEPTANCE COLUMN DEPENDS ON (@Kimi 69b64ff6 pin 2): this function
// changes the WAVEFORM VALUE at the rollover and nothing else. It does not touch the
// phase accumulator, so with the drift flag off the corrected and uncorrected arms share
// one exact phase trajectory -- which is what makes the pinned-phi comparison a
// comparison of waveforms at a common phase rather than of two drifting timelines. Any
// future edit that advances, rounds or wraps `t` inside this kernel would silently
// invalidate the pinned column, not merely change the timbre.
inline double polyblepSaw(double t, double dt) {
  return (2.0 * t - 1.0) - polyblepResidual(t, dt);
}

}  // namespace lunar24::core
