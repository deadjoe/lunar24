// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH#19 S3: the polyBLEP kernel for the PULSE, i.e. the TWO-jump case.
//
// FORMULA (final; task #119 production authorization)
//
//     w                       = min(dt, kPolyblepMaxDt)          dt > 0, else no correction
//     correction(t, duty, dt) = R(t, w) - R(frac(t - duty), w)
//     output(t, duty, dt)     = naive(t, duty) + correction
//
// It is polyblep_kernel.h's residual (polyblepResidual, task #110 / S2) applied twice, once per
// value discontinuity, each with its OWN SIGN and its OWN POSITION:
//
//     phase 0     : -1 -> +1, an UPWARD  jump of +2   ->  ADD      R(t, dt)
//     phase duty  : +1 -> -1, a DOWNWARD jump of -2   ->  SUBTRACT R(frac(t - duty), dt)
//
// Both follow from R(0+) = -1 and R(1-) = +1 driving the two limits to a common value -- the same
// rule polyblepSaw applies to the saw's single downward jump. The residual is CALLED, not
// re-derived: a second copy here would fork the kernel. A pulse is not a saw with a shifted phase,
// and the two signs are what that difference costs.
//
// ---------------------------------------------------------------------------------
// APPLICABILITY BOUNDARY 1: NO DUTY-DEPENDENT SWITCH. ADDING ONE IS A DEFECT.
// ---------------------------------------------------------------------------------
//
// The correction is UNCONDITIONAL in duty, and no code path here may branch on a duty-derived
// regime label. `naive + correction` is continuous in duty at every fixed t and dt: the naive
// pulse's own B-edge jump and the correction's window wrap at t = duty are equal and opposite, so a
// guard that zeroes the correction anywhere else breaks that cancellation by the size of the
// correction there. min(duty, 1 - duty) >= 2*dt is a real, checkable property of a CELL -- it is
// NOT a switch and not evidence that one is safe. It survives only as a reporting label: see
// polyblepPulseWindowsDisjoint below, which gates nothing.
//
// The withdrawn guard, its reproduction (a 0.5625 jump at duty = 2*dt with no edge crossed) and the
// full refutation are in report/2026-09-14-task118-gh19-s3-pulse-aa.md (the revision record); the
// production acceptance evidence is in report/2026-09-14-task119-gh19-s3-pulse-product.md.
//
// ---------------------------------------------------------------------------------
// APPLICABILITY BOUNDARY 2: ABOVE kPolyblepMaxDt THE KERNEL WIDTH IS CAPPED, THE PHASE IS NOT
// ---------------------------------------------------------------------------------
//
// Past dt = kPolyblepMaxDt = 0.5 the residual ITSELF steps at t = dt -- R(dt-) = 0 against
// R(dt+) = (2 - 1/dt)^2, growing 0 -> 1 as dt goes 0.5 -> 1 -- and that step is not one of the
// pulse's own edges, so the construction would add a jump it is not entitled to. Approaching 0.5
// from below reads the same way: 0.5 is the LAST dt at which the residual is continuous.
//
// The adopted strategy (reviewed ruling, task #118) caps the WIDTH OF THE CORRECTION KERNEL and
// changes nothing else: `w = min(dt, kPolyblepMaxDt)` in the formula above, while the phase
// accumulator keeps advancing by the TRUE step. Frequency, the V/OCT and CV law, the duty, the PWM
// transfer and the sync contract are all untouched.
//
//   * IT IS a BOUNDED CONTINUOUS WAVEFORM EXTENSION. At w = 0.5 the two branches of the residual
//     meet at t = 0.5 on the same value, so R(., 0.5) is continuous on the whole period and the only
//     remaining discontinuity is the window wrap at t = duty -- where the naive pulse steps down by
//     2 as well. Equal and opposite: the sum cancels, exactly as in domain, so the waveform function
//     is continuous at every dt > 0. Capping changes only the residual's third argument, so every
//     dt > 0.5 cell is IDENTICAL sample for sample to this formula at dt = 0.5, and boundedness
//     follows from the in-domain case analysis rather than needing a new one.
//   * IT IS NOT anti-aliasing above Nyquist, and no spectral claim is made there: the phase still
//     advances by the TRUE step, so the sample sequence is no longer a sampling of any band-limited
//     signal at that rate, and the S3 acceptance numbers apply to 0 < dt <= kPolyblepMaxDt only.
//   * IT IS NOT the correct correction for a step of that width: the amplitude is still the full
//     +/-1 while the support covers only half a period. It becomes meaningless as dt approaches the
//     point where a step spans whole cycles; the value it buys is that no extra jump enters the
//     product.
//   * IT IS NOT a fallback to the naive pulse. Switching the correction OFF out of domain
//     reintroduces a hard discontinuity at the switch point; that candidate was rejected and is
//     retained only as a negative control in the sweep driver.
//
// IN DOMAIN, BIT-IDENTICAL. min(dt, kPolyblepMaxDt) IS dt for every dt <= 0.5 -- not merely close
// to it -- so every cell with 0 < dt <= kPolyblepMaxDt produces exactly the samples the uncapped
// formula produces, and that is re-checked as an exact comparison, not as a tolerance.
//
// BOUNDEDNESS AND CONTINUITY ARE VERIFIED, NOT ONLY DERIVED: tools/gh19_s3_pulse_kernel_sweep.cpp
// compiles THIS header directly and sweeps (dt, duty, t) on a dense grid -- max |output|, the
// largest one-sided gap in duty, and the boundary / narrow-pulse / moving-duty traversals. Re-run it
// rather than trusting this comment.

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
