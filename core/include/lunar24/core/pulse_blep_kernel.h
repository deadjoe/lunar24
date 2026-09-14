// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH#19 S3: the polyBLEP kernel for the PULSE, i.e. the TWO-jump case.
//
// FORMULA (final; task #119 production authorization)
//
//     w                     = min(dt, kPolyblepMaxDt)          dt > 0, else no correction
//     correction(t, duty, dt) = R(t, w) - R(frac(t - duty), w)
//     output(t, duty, dt)     = naive(t, duty) + correction
//
// It is polyblep_kernel.h's residual (polyblepResidual, task #110 / S2) applied twice, once per
// value discontinuity. The pulse has TWO jumps a cycle and each needs its own kernel with its own
// SIGN and its own POSITION; re-deriving the residual here would fork the kernel.
//
// WHY THE SIGN IS NOT A FREE CHOICE. polyblepSaw corrects the saw, whose single jump is DOWNWARD
// by 2 at phase 0, and it does so by SUBTRACTING the residual. Read as a rule rather than as one
// function's algebra:
//
//     a -2 (downward) jump at phase x  ->  SUBTRACT  R( frac(t - x), dt )
//     a +2 (upward)   jump at phase x  ->  ADD       R( frac(t - x), dt )
//
// Both follow from R(0+) = -1 and R(1-) = +1 driving the two limits to a common value. The naive
// pulse (t < duty) ? +1 : -1 has exactly these two jumps:
//
//     phase 0     : -1 -> +1, an UPWARD   jump of +2   ->  +R(t, dt)
//     phase duty  : +1 -> -1, a DOWNWARD  jump of -2   ->  -R(frac(t - duty), dt)
//
// giving the correction implemented below. A pulse is NOT a saw with a shifted phase, and the two
// signs are what that difference costs.
//
// ---------------------------------------------------------------------------------
// APPLICABILITY BOUNDARY 1: NO DUTY-DEPENDENT SWITCH. ADDING ONE IS A DEFECT.
// ---------------------------------------------------------------------------------
//
// R is non-zero only on [0, dt) (after its jump) and (1 - dt, 1) (before it), so the two terms are
// live on four phase sets:
//
//     A-after  [0, dt)          A-before  (1 - dt, 1)
//     B-after  [duty, duty+dt)  B-before  (duty - dt, duty)          (mod 1)
//
// Those four sets are pairwise disjoint exactly when
//
//     min(duty, 1 - duty) >= 2 * dt                                    (*)
//
// (*) is a real, checkable property of a CELL. It is NOT a switch, and an earlier revision of this
// header that branched on it was WRONG -- at dt = 220/44100, t = dt/4, the output jumped by 0.5625
// between duty = 2*dt - 1e-12 and duty = 2*dt + 1e-12 although neither the phase nor the duty
// crossed a pulse edge: a discontinuity introduced by the guard itself, on duty values the
// product's own PWM sweep passes through continuously.
//
// WHY THE GUARD CANNOT BE MOVED EITHER. Vary `duty` at fixed t: the only discontinuous point of
// R(frac(t - duty), dt) is where that argument wraps, i.e. duty = t (mod 1) -- and at that exact
// duty the naive pulse crosses its own B edge and jumps by -2. The two jumps are equal and
// opposite, so `naive + correction` is CONTINUOUS in duty for every fixed t and dt. A guard that
// zeroes the correction anywhere else breaks that cancellation by the size of the correction
// there, which is exactly the 0.5625 above. There is consequently NO duty value at which the
// correction may be switched off, and (*) is not evidence that there is: it was never what kept
// the output bounded. (*) survives as a REGIME LABEL only -- see polyblepPulseWindowsDisjoint,
// which is reporting-only and gates nothing.
//
// The full refutation, its reproduction, and the withdrawn narrow-pulse claim are recorded in
// report/2026-09-14-task118-gh19-s3-pulse-aa.md (the revision record). This header keeps only the
// rule that follows from it.
//
// BOUNDEDNESS. For 0 < dt <= kPolyblepMaxDt and any duty in (0,1), the corrected pulse stays in
// [-1, +1] -- the same range as the naive pulse. Each residual term is in [-1, +1], so the
// CORRECTION alone can reach 2 in magnitude; it is the SUM that is bounded, by case analysis over
// the half-cycle the sample is in (recorded in the report above). VERIFIED, NOT ONLY DERIVED:
// tools/gh19_s3_pulse_kernel_sweep.cpp compiles THIS header directly and sweeps (dt, duty, t) on a
// dense grid -- max |output|, the largest one-sided gap in duty, and the boundary / narrow-pulse /
// moving-duty traversals. Re-run it rather than trusting this comment.
//
// ---------------------------------------------------------------------------------
// APPLICABILITY BOUNDARY 2: ABOVE kPolyblepMaxDt THE KERNEL WIDTH IS CAPPED, THE PHASE IS NOT
// ---------------------------------------------------------------------------------
//
// The three regimes of dt (see the sweep driver's out-of-domain section) end the proven domain at
// kPolyblepMaxDt = 0.5. Past it the residual ITSELF steps at t = dt -- R(dt-) = 0 against
// R(dt+) = (2 - 1/dt)^2, growing 0 -> 1 as dt goes 0.5 -> 1 -- and that step is not one of the
// pulse's own edges, so `naive + correction` carries a jump the construction was never entitled
// to add. Approaching dt = 0.5 from below gives the opposite reading: 0.5 is the LAST dt at which
// the two branches meet on the residual's own zero, i.e. the last dt at which the residual is
// continuous.
//
// The strategy adopted here (reviewed ruling, task #118) is to cap the WIDTH OF THE CORRECTION
// KERNEL and leave everything else alone: `w = min(dt, kPolyblepMaxDt)` in the formula above,
// while the phase accumulator keeps advancing by the TRUE step. Nothing about the oscillator's
// frequency, the V/OCT or CV law, the duty, the PWM transfer or the sync contract changes.
//
// WHY THAT MAKES THE OUTPUT CONTINUOUS. At w = 0.5 the two branches of polyblepResidual meet at
// t = 0.5 on the same value (the residual's own zero), so R(., 0.5) is continuous on the whole
// period. The correction is a difference of two such terms, and the only remaining discontinuity
// is the WINDOW WRAP at t = duty -- where R(frac(t - duty), w) steps down by 2 while the naive
// pulse steps down by 2 as well. Equal and opposite: the sum cancels, exactly as it does in
// domain. So the emitted waveform function is continuous at every dt > 0.
//
// WHAT THIS IS, AND WHAT IT IS NOT.
//   * IT IS a BOUNDED CONTINUOUS WAVEFORM EXTENSION. For dt > 0.5 the output is IDENTICAL, sample
//     for sample, to the output of this same formula at dt = 0.5: capping changes only the third
//     argument, so every out-of-domain cell is exactly an in-domain cell's value. Boundedness and
//     continuity therefore follow from the in-domain case analysis above rather than needing a
//     new one, and the sweep measures max|output| = 1 including at the largest step the declared
//     input box can produce.
//   * IT IS NOT anti-aliasing above Nyquist, and no spectral claim is made there. The phase still
//     advances by the TRUE step, so once dt > 0.5 the sample sequence is no longer a sampling of
//     any band-limited signal at that rate -- a two-point step kernel has no referent for it. The
//     extension keeps the waveform bounded and continuous; it does not make the result
//     band-limited, and the S3 acceptance numbers apply to 0 < dt <= kPolyblepMaxDt only.
//   * IT IS NOT the correct correction for a step of that width. Past dt = 0.5 the kernel's scale
//     no longer matches the step it is correcting: the amplitude is still the full +/-1 but the
//     support covers only half a period. It becomes meaningless as dt approaches the point where
//     a step spans whole cycles; the value it buys is that no extra jump enters the product.
//   * IT IS NOT a fallback to the naive pulse. An earlier candidate switched the correction OFF
//     out of domain, which reintroduces a hard discontinuity at the switch point; that candidate
//     was rejected and is retained only as a negative control (see the sweep driver), not as an
//     alternative implementation.
//
// IN DOMAIN, BIT-IDENTICAL. min(dt, kPolyblepMaxDt) IS dt for every dt <= 0.5 -- not merely close
// to it -- so every cell with 0 < dt <= kPolyblepMaxDt produces exactly the samples the uncapped
// formula produces, and the pre-cap static evidence for that range is re-checked as an exact
// comparison, not as a tolerance.

#ifndef LUNAR24_CORE_PULSE_BLEP_KERNEL_H
#define LUNAR24_CORE_PULSE_BLEP_KERNEL_H

#include <cmath>

#include "lunar24/core/polyblep_kernel.h"

namespace lunar24::core {

// (*): true when the two jumps' correction windows are pairwise disjoint, i.e. when the
// phase-local derivation of each individual term is stated for that cell.
//
// REPORTING ONLY. No code path in this header branches on this, and none may: the header note
// above records the defect that a switch here caused (a 0.5625 jump at duty = 2*dt with no edge
// crossed). It is exported so a caller can LABEL a cell's regime -- the same reason
// polyblepSupportReachesHalfPeriod is public -- and the value it returns is a fact about the
// stimulus, not a statement about whether the correction ran.
//
// duty is the EFFECTIVE duty actually used to render the pulse (Vco::effectiveDuty(), i.e.
// including PWM), not the raw `pw` parameter: the windows move with the duty the waveform uses.
//
// NOTE on the width argument under the capping above. The live window half-width is now
// w = min(dt, kPolyblepMaxDt), so the honest predicate would be `m >= 2*w`. It is NOT, and the
// label is nevertheless EXACTLY the same function of its arguments, because the two candidates
// cannot disagree: m = min(duty, 1-duty) <= 0.5 always, so `m >= 2*w` is false whenever w = 0.5,
// and `m >= 2*dt` is false whenever dt >= 0.5. Both forms are therefore false on the whole of
// dt >= 0.5 and identical on the whole of dt <= 0.5. The sweep driver checks that equality as a
// directed cell rather than leaving it as a comment.
inline bool polyblepPulseWindowsDisjoint(double duty, double dt) {
  if (!(dt > 0.0)) return true;  // no correction to overlap; mirrors polyblepResidual's dt=0 exit
  const double m = (duty < 1.0 - duty) ? duty : (1.0 - duty);
  return m >= 2.0 * dt;
}

// The pulse's correction term: the two signed residuals. Unconditional in duty -- see boundary 1:
// there is no duty-dependent branch here, and adding one is a defect, not a safety net. Kept
// separate from polyblepPulse because the mix path (Vco::emittedAt_) needs the correction alone to
// scale by the pulse node's weight; the pure-pulse path just adds it to the naive shape. Exposed
// for that reason, not as an invitation to compose it differently.
//
// t is the NORMALIZED phase in [0,1) and dt the normalized per-sample increment, as in
// polyblepResidual. Pure function of its arguments: no state, no allocation, no latency.
inline double polyblepPulseCorrection(double t, double duty, double dt) {
  if (!(dt > 0.0)) return 0.0;
  // THE CAPPED KERNEL WIDTH (see boundary 2). `dt` is the step the phase accumulator really
  // advanced by and is what the caller advances with; only the width handed to the residual is
  // bounded. The ternary is written out rather than spelled std::min so the in-domain case is
  // visibly the identity: for dt <= kPolyblepMaxDt the comparison is false and `w` IS `dt`, bit
  // for bit, which is what makes the in-domain samples unchanged rather than merely close.
  // NaN cannot reach the ternary -- the guard above already rejected it -- and +inf caps to 0.5.
  const double w = (dt < kPolyblepMaxDt) ? dt : kPolyblepMaxDt;
  // Edge A at phase 0 is the UPWARD jump (+2) -> add R. Edge B at `duty` is the DOWNWARD
  // jump (-2) -> subtract R, read at the phase the B edge is seen from: frac(t - duty).
  double b = t - duty;
  b -= std::floor(b);  // frac, so B's window is measured from B in both directions
  return polyblepResidual(t, w) - polyblepResidual(b, w);
}

// The corrected unit pulse at normalized phase t with effective duty `duty`. Equivalent to the
// naive pulse plus polyblepPulseCorrection, bit-identical to the naive pulse wherever the
// correction is exactly 0 (everywhere outside the four windows listed above), and -- the property
// the removal of the guard was for -- CONTINUOUS in `duty` at every fixed t. With the kernel-width
// cap it is also CONTINUOUS in `t` at every fixed duty and EVERY dt > 0, including
// dt > kPolyblepMaxDt, where it is the bounded continuous extension documented above.
//
// PROPERTY the S3 acceptance column depends on (same shape as polyblepSaw's): this function
// changes the WAVEFORM VALUE and nothing else. It does not touch the phase accumulator, so a
// corrected and an uncorrected arm share one exact phase trajectory -- which is what makes the
// pinned-phase comparison a comparison of waveforms at a common phase rather than of two drifting
// timelines. Any future edit that advances, rounds or wraps `t` here would silently invalidate
// that column, not merely change the timbre.
inline double polyblepPulse(double t, double duty, double dt) {
  const double naive = (t < duty) ? 1.0 : -1.0;
  return naive + polyblepPulseCorrection(t, duty, dt);
}

}  // namespace lunar24::core

#endif  // LUNAR24_CORE_PULSE_BLEP_KERNEL_H
