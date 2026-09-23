// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_s3_pulse_kernel_sweep.cpp — task #118 (GH #19 S3) KERNEL-DOMAIN sweep for the pulse
// correction, compiled against the production header with no product link at all.
//
// WHY THIS EXISTS SEPARATELY FROM THE PROBE. tests/probes/gh19_s3_pulse_probe.cpp measures the
// PRODUCT: it renders real audio through encode -> owner.applyDeviceState -> processBlock and
// asks what the spectrum did. That instrument cannot see the defect this driver exists for,
// because a residual/spectral metric is time-shift blind and, more to the point, the cells it
// renders are at duty values where the old guard happened to be constant. @Codex e144b61 review
// found the defect by compiling the header DIRECTLY at one coordinate; this driver generalises
// that into a sweep so the property is checked everywhere, not at one point.
//
// WHAT IT MEASURES (all on polyblepPulse, unit amplitude, normalized phase):
//
//   repro    the review's coordinate, evaluating BOTH the current header and the guarded formula
//            of the refuted revision (reproduced inline as `guardedPulse`, transcribed from the
//            committed 58f41be text). The pair is the tool's own positive control: a driver that
//            cannot reproduce 0.5625 on the guarded formula is not evidence about the current one.
//   grid     max |output| over a dense (dt, duty, t) grid — the boundedness claim — plus the
//            largest one-sided gap in duty and in t at the finest resolution used.
//   refine   the REFINEMENT TEST, which is the falsifier that needs no assumption about how big
//            a step "should" be: sample a duty trajectory that crosses a candidate switch point
//            at N = 1e3, 1e4, 1e5, 1e6 points and report the largest consecutive-output gap at
//            each. A continuous function's gap falls with the step; a jump's does not move at all.
//            The guarded formula's gap must stay pinned at the jump size across all four
//            resolutions while the current header's falls; if both fall, the driver proved nothing.
//   narrow   duty -> 0 and the narrow-pulse band, reported as the value at the smallest duty and
//            the max |output| over duty in [1e-3, 4*dt].
//   moving   a duty TRAJECTORY with a per-frame increment (the PWM case, where duty moves between
//            samples), swept across the old switch point, reported as the worst single-frame
//            output step at several frame rates.
//
// OUT OF DOMAIN (dt > kPolyblepMaxDt), where the CURRENT header caps the kernel width at
// kPolyblepMaxDt and leaves the phase advancing by the true step. Every out-of-domain cell is
// reported TWICE, under distinct prefixes:
//
//   *-CAP    the current header, i.e. `polyblepPulse`.
//   *-UNC    the same two-residual formula with the width UNCAPPED (`w = dt`), which is what the
//            header did before the cap. This is the section's POSITIVE CONTROL: the uncapped arm
//            must still show the residual's own step at t = dt (closed form (2 - 1/dt)^2) and a
//            FLAT t-gap, or the driver is not measuring the thing it claims to measure.
//
// `uncappedPulse` below is built from the PRODUCTION polyblepResidual rather than transcribed, so
// the control differs from the header in exactly one bit of source (the capped argument) and in
// nothing else. The refuted-revision control (`guardedPulse`, above) stays because it is a
// different control for a different claim: that one has no cap AND a duty-dependent switch.
//
// STRUCTURE (OVER-STRUCT) and the REAL-VCO section (VCO-*) are directed checks rather than sweeps:
// they assert the specific identities the header note claims (cap is the identity in domain; the
// out-of-domain output IS the dt = 0.5 output sample for sample; the window-disjointness LABEL is
// unchanged) and the ones the product's wiring claims (the emitted sample equals the standalone
// kernel at the same phase; the accumulator is block-invariant; a hard sync lands on phase 0 and
// the emitted post-reset sample obeys the documented formula at every reset phase; the pitch law
// stops rather than reversing).
//
// COMPILE (header-only, no host library):
//   c++ -std=c++17 -O2 -I core/include tools/gh19_s3_pulse_kernel_sweep.cpp -o /tmp/gh19_s3_sweep
// tools/run_gh19_s3_kernel_sweep.py does exactly that and captures the output as evidence.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lunar24/core/pulse_blep_kernel.h"
// The reachable-box section (runReachableBox) drives the REAL Vco object, so the third regime is
// measured through the production pitch law and its clamps rather than recomputed by hand here.
#include "lunar24/core/vco.h"

using lunar24::core::polyblepPulse;
using lunar24::core::polyblepPulseWindowsDisjoint;

namespace {

double frac(double x) { return x - std::floor(x); }

// The REFUTED revision's predicate (58f41be), FROZEN here rather than taken from the header.
//
// WHY IT IS FROZEN. The refuted revision's defect was a branch on this predicate, so reconstructing
// it means reconstructing the predicate too. Reading the live `polyblepPulseWindowsDisjoint` instead
// would make the tool's POSITIVE CONTROL depend on a function the current header declares to be
// REPORTING ONLY -- and the negative controls proved that matters: an edit that drifts the shipped
// predicate by 0.01 was enough to drive this reconstruction to 0 and kill the positive control, which
// the negative-control runner correctly reads as "the driver can no longer see the refutation" and
// propagates as INVALID. A control that a change to an unrelated reporting function can switch off is
// not an independent control. Frozen, the reconstruction is a fixed fact about revision 58f41be, and
// a drift in the shipped predicate is measured on its own terms by OVER-STRUCT-LABEL.
bool refutedRevisionWindowsDisjoint(double duty, double dt) {
  if (!(dt > 0.0)) return true;
  const double m = (duty < 1.0 - duty) ? duty : (1.0 - duty);
  return m >= 2.0 * dt;
}

// The REFUTED revision's formula (58f41be), transcribed so the driver can show what it detects.
// `guardedPulse` is NOT the production header and is never what the grid/refine claims are about;
// it exists only as the positive control for the tool and as the thing the refinement test must
// fail to shrink on. Nothing else in this file calls it.
double guardedPulse(double t, double duty, double dt) {
  const double naive = (t < duty) ? 1.0 : -1.0;
  if (!(dt > 0.0)) return naive;
  if (!refutedRevisionWindowsDisjoint(duty, dt)) return naive;  // the switch that was the defect
  return naive + (lunar24::core::polyblepResidual(t, dt) -
                  lunar24::core::polyblepResidual(frac(t - duty), dt));
}

// The UNCAPPED two-residual formula -- the header's own construction with `w = dt`, i.e. what the
// current header would emit if the cap were removed. NOT a transcription: it calls the production
// polyblepResidual exactly as the header does, so the only difference from `polyblepPulse` is the
// third argument. Used as the positive control for every out-of-domain claim (it must still show
// the residual's own step at t = dt, growing 0 -> 1 as dt goes 0.5 -> 1) and as the in-domain
// reference for the "the cap IS the identity" check, where it must agree BIT FOR BIT.
double uncappedPulse(double t, double duty, double dt) {
  const double naive = (t < duty) ? 1.0 : -1.0;
  return naive + (lunar24::core::polyblepResidual(t, dt) -
                  lunar24::core::polyblepResidual(frac(t - duty), dt));
}

struct Worst {
  double v = -1.0;
  double dt = 0.0, duty = 0.0, t = 0.0;
  void see(double x, double d, double du, double tt) {
    if (x > v) { v = x; dt = d; duty = du; t = tt; }
  }
};

// ---- the largest consecutive gap along a duty sweep at resolution nD ------------------------
// `guarded` selects the refuted revision's formula, so the same grid reports both and the
// contrast is a measurement rather than a claim. A jump shows up as a gap that is invariant to
// nD; a continuous function's gap is O(1/nD).
Worst dutyGap(const std::vector<double>& dts, int nD, int nT, bool guarded) {
  Worst w;
  for (double dt : dts) {
    for (int c = 0; c < nT; ++c) {
      const double t = (static_cast<double>(c) + 0.5) / static_cast<double>(nT);
      double pv = guarded ? guardedPulse(t, 0.0, dt) : polyblepPulse(t, 0.0, dt);
      for (int b = 1; b <= nD; ++b) {
        const double duty = static_cast<double>(b) / static_cast<double>(nD);
        const double v = guarded ? guardedPulse(t, duty, dt) : polyblepPulse(t, duty, dt);
        w.see(std::fabs(v - pv), dt, duty, t);
        pv = v;
      }
    }
  }
  return w;
}

void runGrid(double dMax, int nDt, int nDuty, int nT) {
  Worst wAbs, wAbsG, wAbsP;
  for (int a = 1; a <= nDt; ++a) {
    const double dt = dMax * static_cast<double>(a) / static_cast<double>(nDt);
    for (int b = 0; b < nDuty; ++b) {
      const double duty = (static_cast<double>(b) + 0.5) / static_cast<double>(nDuty);
      for (int c = 0; c < nT; ++c) {
        const double t = (static_cast<double>(c) + 0.5) / static_cast<double>(nT);
        wAbs.see(std::fabs(polyblepPulse(t, duty, dt)), dt, duty, t);
        wAbsG.see(std::fabs(guardedPulse(t, duty, dt)), dt, duty, t);
        // vco.h clamps effectiveDuty to [1e-3, 1-1e-3]; report the bound over the REACHABLE duty
        // set as well, so a bound that only holds below the clamp cannot be read as the product's.
        if (duty >= 1e-3 && duty <= 1.0 - 1e-3) {
          wAbsP.see(std::fabs(polyblepPulse(t, duty, dt)), dt, duty, t);
        }
      }
    }
  }
  // The claim is |output| <= 1; report the EXCESS over 1 rather than only the max, because
  // "max = 1.000000000000000" is also what a max of 1.0000000000000004 prints as at 15 digits.
  std::printf("GRID maxabs current  nDt=%d nDuty=%d nT=%d  max=%.15f excess_over_1=%.3e"
              " at dt=%.6f duty=%.9f t=%.9f\n",
              nDt, nDuty, nT, wAbs.v, wAbs.v - 1.0, wAbs.dt, wAbs.duty, wAbs.t);
  std::printf("GRID maxabs guarded  (same grid, refuted formula)  max=%.15f excess_over_1=%.3e\n",
              wAbsG.v, wAbsG.v - 1.0);
  std::printf("GRID maxabs product-duty (duty in [1e-3, 1-1e-3], the vco.h clamp)  max=%.15f"
              " excess_over_1=%.3e at dt=%.6f duty=%.9f t=%.9f\n",
              wAbsP.v, wAbsP.v - 1.0, wAbsP.dt, wAbsP.duty, wAbsP.t);

  const std::vector<double> coarseDts = {dMax / 8, dMax * 2 / 8, dMax * 4 / 8, dMax * 6 / 8, dMax};
  const std::vector<double> fineDts = {dMax / 8, dMax};
  for (int pass = 0; pass < 2; ++pass) {
    const int nD = (pass == 0) ? 20001 : 160001;
    const int nT = (pass == 0) ? 512 : 64;
    const std::vector<double>& dts = (pass == 0) ? coarseDts : fineDts;
    const Worst gc = dutyGap(dts, nD, nT, false);
    const Worst gg = dutyGap(dts, nD, nT, true);
    std::printf("GRID dutygap_%s nDuty=%-7d nT=%-4d  current=%.15f (dt=%.6f duty=%.9f t=%.9f)"
                "   guarded=%.15f\n",
                (pass == 0) ? "coarse" : "fine", nD, nT, gc.v, gc.dt, gc.duty, gc.t, gg.v);
  }
}

// ---- refinement test: the assumption-free falsifier ------------------------------------------
// A duty trajectory crossing `crossDuty` at fixed t, sampled at N points. Report the largest gap
// between consecutive outputs. Continuous => gap ~ 1/N. Jump => gap stays at the jump size.
void runRefine(double dt, double t, double d0, double d1, bool guarded) {
  const int Ns[] = {1000, 10000, 100000, 1000000};
  std::printf("REFINE %s dt=%.9f t=%.9f duty=%.9f..%.9f\n",
              guarded ? "guarded(58f41be)" : "current-header", dt, t, d0, d1);
  for (int k = 0; k < 4; ++k) {
    const int N = Ns[k];
    double worst = 0.0, at = 0.0;
    double prev = guarded ? guardedPulse(t, d0, dt) : polyblepPulse(t, d0, dt);
    for (int i = 1; i <= N; ++i) {
      const double duty = d0 + (d1 - d0) * static_cast<double>(i) / static_cast<double>(N);
      const double v = guarded ? guardedPulse(t, duty, dt) : polyblepPulse(t, duty, dt);
      const double g = std::fabs(v - prev);
      if (g > worst) { worst = g; at = duty; }
      prev = v;
    }
    std::printf("REFINE   N=%-8d maxgap=%.15f at duty=%.12f\n", N, worst, at);
  }
}

void runRepro(double dt) {
  const double t = dt / 4.0;
  std::printf("REPRO dt=220/44100=%.15f  t=dt/4=%.15f\n", dt, t);
  for (int s = -1; s <= 1; s += 2) {
    const double duty = 2.0 * dt + static_cast<double>(s) * 1e-12;
    std::printf("REPRO duty=2dt%+de-12  guarded=%.15f  current=%.15f  disjoint=%d\n", s,
                guardedPulse(t, duty, dt), polyblepPulse(t, duty, dt),
                polyblepPulseWindowsDisjoint(duty, dt) ? 1 : 0);
  }
  const double lo = guardedPulse(t, 2.0 * dt - 1e-12, dt);
  const double hi = guardedPulse(t, 2.0 * dt + 1e-12, dt);
  const double nlo = polyblepPulse(t, 2.0 * dt - 1e-12, dt);
  const double nhi = polyblepPulse(t, 2.0 * dt + 1e-12, dt);
  std::printf("REPRO jump guarded=%.15f   current=%.15f\n", std::fabs(hi - lo),
              std::fabs(nhi - nlo));
}

void runNarrow(double dt) {
  Worst w;
  const double d0 = 1e-3, d1 = 4.0 * dt;
  for (int b = 0; b <= 40000; ++b) {
    const double duty = d0 + (d1 - d0) * static_cast<double>(b) / 40000.0;
    for (int c = 0; c < 2000; ++c) {
      const double t = (static_cast<double>(c) + 0.5) / 2000.0;
      w.see(std::fabs(polyblepPulse(t, duty, dt)), dt, duty, t);
    }
  }
  std::printf("NARROW dt=%.9f duty in [%.1e, 4dt]  max|out|=%.15f at duty=%.9f t=%.9f\n",
              dt, d0, w.v, w.duty, w.t);
  for (double d : {0.0, 1e-9, 1e-6, 1e-4, dt / 10.0, dt, 2.0 * dt, 3.0 * dt}) {
    double lo = 1e9, hi = -1e9;
    for (int c = 0; c < 20000; ++c) {
      const double t = (static_cast<double>(c) + 0.5) / 20000.0;
      const double v = polyblepPulse(t, d, dt);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    std::printf("NARROW duty=%-12.3e range=[%.9f, %.9f]\n", d, lo, hi);
  }
}

// ---- moving duty: a trajectory with a per-frame increment ------------------------------------
// The PWM case: duty is not held but read once per frame, so between two emitted samples it moves
// by `winPerFrame` correction windows. The trajectory starts at the product's own duty clamp
// (1e-3, vco.h effectiveDuty) so it CROSSES the refuted revision's switch at duty = 2*dt; a
// trajectory starting above 2*dt would never exercise the switch and would report a vacuous zero.
//
// WHAT THE DISCRIMINATOR IS. The output has to change fast wherever duty sweeps past a pulse edge
// (it travels the full 2-unit jump over a duty width of 2*dt), so a large step at a coarse frame
// rate is not a defect for either formula. The signature of a JUMP is instead a floor: as
// winPerFrame -> 0 the trajectory resolves the edge crossing and every step must shrink
// proportionally -- unless the formula is switched, in which case the switch contributes a step
// of its own that no refinement removes.
void runMoving(double dt) {
  const double dStart = 1e-3, dEnd = 0.95;   // 1e-3 is vco.h's effectiveDuty clamp
  // The phase must sweep too. A switch at duty = 2*dt is invisible at a phase that is nowhere near
  // a correction window -- at t = 0.4 both formulas give the bare naive pulse there and agree
  // exactly, which is a true statement about t = 0.4 and no statement at all about the switch. The
  // worst case over the phase is what the claim is about, so t is swept as well.
  const int nT = 512;
  std::printf("MOVING dt=%.9f  worst |out(n+1)-out(n)| over duty %.4g -> %.2f and %d phases\n",
              dt, dStart, dEnd, nT);
  for (double winPerFrame : {400.0, 100.0, 25.0, 6.25, 1.5625, 0.390625, 0.09765625,
                             0.0244140625}) {
    const double step = winPerFrame * dt;
    double worstG = 0.0, worstC = 0.0, atG = 0.0, atC = 0.0, tG = 0.0, tC = 0.0;
    long n = 0;
    for (int c = 0; c < nT; ++c) {
      const double t = (static_cast<double>(c) + 0.5) / static_cast<double>(nT);
      double pg = guardedPulse(t, dStart, dt), pc = polyblepPulse(t, dStart, dt);
      for (double d = dStart; d <= dEnd; d += step) {
        const double vg = guardedPulse(t, d, dt);
        const double vc = polyblepPulse(t, d, dt);
        if (std::fabs(vg - pg) > worstG) { worstG = std::fabs(vg - pg); atG = d; tG = t; }
        if (std::fabs(vc - pc) > worstC) { worstC = std::fabs(vc - pc); atC = d; tC = t; }
        pg = vg;
        pc = vc;
      }
      if (c == 0) {
        for (double d = dStart; d <= dEnd; d += step) ++n;
      }
    }
    std::printf("MOVING %-14.8f win/frame (n=%6ld)  guarded maxstep=%.9f at duty=%.9f t=%.9f"
                "   current maxstep=%.9f at duty=%.9f t=%.9f\n",
                winPerFrame, n, worstG, atG, tG, worstC, atC, tC);
  }
}

// ================================================================================================
// OUT OF DOMAIN: dt > kPolyblepMaxDt
//
// Everything above this line lives inside 0 < dt <= 0.5, which is the domain the kernel's own
// boundedness/continuity case analysis is scoped to (polyblep_kernel.h) and the domain runGrid()
// sweeps. @Codex 144b1339 item 3: `pulseBlepCorr_` hands `step` straight to that kernel with no
// upper bound at all, while `triangleBlampCorr` bounds its own kernel with
// `blampSupportReachesHalfPeriod(dt)`. So the section below measures what the pulse formula does
// OUTSIDE the proven domain instead of inheriting the in-domain conclusion.
//
// The whole section is a MEASUREMENT. Nothing here is used by the in-domain claims, and the
// in-domain claims are not weakened by it: the two domains are reported separately because they
// are different statements about different inputs.
//
// THREE REGIMES, and the middle one is the one that matters:
//
//   0 < dt <= 0.5   branch 1 on [0,dt), branch 2 on [dt,1). Both residual windows fit inside one
//                   period, the corner at t=dt is cancelled by the residual's own zero there, and
//                   R is continuous. This is the proven domain.
//   0.5 < dt < 1    same two branches, but now 1-dt < dt, so branch 2 starts at t=dt and
//                   R(dt+) = (2 - 1/dt)^2 while R(dt-) = 0: THE RESIDUAL ITSELF STEPS. The step
//                   grows from 0 at dt=0.5 to 1 at dt=1. naive + correction therefore carries an
//                   extra jump of that size at t = dt, and (when duty+dt < 1) a second one of the
//                   opposite sign at t = duty+dt. Those are jumps the polyBLEP construction was
//                   never entitled to add: they are not the pulse's own edges.
//   dt >= 1         t < dt holds for every t in [0,1), so branch 1 is taken everywhere and the
//                   phase advances a full cycle or more per output sample. The emitted sample
//                   sequence is no longer a sampling of any waveform, so there is nothing for a
//                   two-point step correction to band-limit.
// ================================================================================================

double naivePulse(double t, double duty) { return (t < duty) ? 1.0 : -1.0; }

// The largest |out(t_{i+1}) - out(t_i)| around one period of phase, at fixed dt and duty, at t
// resolution nT. A CONTINUOUS output has a gap that falls as 1/nT; a JUMP's gap is invariant in
// nT. Same discriminator as dutyGap(), applied along t instead of along duty.
// `capped` selects the current header (true) or the uncapped control (false), so the same grid
// reports both and the contrast is a measurement rather than a claim.
Worst tGap(bool capped, double dt, double duty, long nT) {
  Worst w;
  double prev = capped ? polyblepPulse(0.0, duty, dt) : uncappedPulse(0.0, duty, dt);
  for (long i = 1; i < nT; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(nT);
    const double v = capped ? polyblepPulse(t, duty, dt) : uncappedPulse(t, duty, dt);
    w.see(std::fabs(v - prev), dt, duty, t);
    prev = v;
  }
  return w;
}

// The residual's own jump at t=dt, measured as a one-sided difference, for dt in (0.5, 1). duty is
// taken ABOVE dt so the naive part is constant across t=dt (the pulse's own edge is at t=duty), so
// what the difference sees is the correction and only the correction.
//
// WHAT THE THREE eps COLUMNS ARE FOR. A one-sided difference is jump + O(eps)*slope, so it is NOT
// eps-invariant even when the function jumps -- the leftover shrinks linearly in eps. What the
// columns therefore show is the LIMIT: as eps falls they must converge to the closed form, and the
// leftover must fall with eps. "Genuine jump" versus "merely steep edge" is decided by the t-gap
// discriminator in runOverGrid() (a jump's t-gap is flat in nT, a continuous function's is
// O(1/nT)), not by these columns.
void runOverResidualJump() {
  std::printf("OVER-RESIDUAL signed output jump across t=dt, duty>dt (naive constant there)\n");
  std::printf("OVER-RESIDUAL-CAP dt        jump(e=1e-6)  jump(e=1e-9)  jump(e=1e-10) "
              "jump(e=1e-12)  duty\n");
  std::printf("OVER-RESIDUAL-UNC dt        jump(e=1e-10)  closed (2-1/dt)^2  duty\n");
  // dt is kept strictly inside (0.5, 1) for the closed-form column: outside that range the
  // coordinate t=dt is not the corner (for dt >= 1 branch 1 covers the whole period) and the closed
  // form does not describe it. The dt values outside are measured in runOverGrid() instead, on the
  // t-gap discriminator.
  //
  // WHAT THE TWO PREFIXES ARE. -*CAP is the current header; its one-sided difference must fall
  // LINEARLY in eps (3.2 * eps here) because the output is continuous, and must NOT hold at the
  // closed form. -*UNC is the uncapped control; its difference must sit at the closed form and its
  // eps-dependence must be the residual leftover only. A driver that reports the same shape for
  // both is not measuring the cap.
  for (double dt : {0.5, 0.5000001, 0.51, 0.55, 0.6, 2.0 / 3.0, 0.7, 0.75, 0.8, 0.9, 0.99}) {
    const double duty = 0.5 * (dt + 1.0);  // strictly between dt and 1, so frac(dt-duty) misses its corner
    const double closed = (2.0 - 1.0 / dt) * (2.0 - 1.0 / dt);
    std::printf("OVER-RESIDUAL-CAP %-14.7g", dt);
    for (double eps : {1e-6, 1e-9, 1e-10, 1e-12}) {
      const double hi = polyblepPulse(dt + eps, duty, dt);
      const double lo = polyblepPulse(dt - eps, duty, dt);
      // Scientific, for the same reason as the UNC pair below: what this table has to SHOW is that
      // the difference falls in proportion to eps (one decade per column here). At 9 fixed decimals
      // the last two columns -- the ones that carry the claim -- would print as a row of zeros, and
      // a table whose claim is invisible is not evidence for it.
      std::printf(" %+13.9e", hi - lo);
    }
    std::printf("  %.6f\n", duty);
    const double unc = uncappedPulse(dt + 1e-10, duty, dt) - uncappedPulse(dt - 1e-10, duty, dt);
    // Scientific rather than fixed notation for the uncapped pair: the driver's own runner asserts
    // that `unc` sits ON `closed` to within 1e-9, and a 9-decimal fixed print cannot resolve a
    // difference that small at every magnitude this column spans (1e-3 .. 1). Printing 10
    // significant digits makes the assertion checkable at the resolution it is stated at.
    std::printf("OVER-RESIDUAL-UNC %-14.7g %+15.9e %+15.9e  %.6f\n", dt, unc, closed, duty);
  }
}

// The review's exact coordinate, printed as the two values it printed, so the reproduction is a
// number in the evidence file and not a restatement of the closed form.
void runOverRepro() {
  const double dt = 0.75, duty = 0.2;
  const double closed = (2.0 - 1.0 / dt) * (2.0 - 1.0 / dt);
  std::printf("OVER-REPRO dt=%.2f duty=%.2f  eps=1e-10  closed(2-1/dt)^2=%.15f\n", dt, duty, closed);
  // The coordinate @Codex's ruling names. Printed as the two headline values (capped / uncapped),
  // the residual terms, and the eps = 1e-10 two-sided difference he quotes so the number in his
  // ruling and the number in this file can be compared directly instead of paraphrased.
  std::printf("OVER-REPRO-CAP   t=0.75-1e-10  out=%.15f   t=0.75+1e-10  out=%.15f   "
              "diff=%.15g  (linear in eps => continuous)\n",
              polyblepPulse(0.75 - 1e-10, duty, dt), polyblepPulse(0.75 + 1e-10, duty, dt),
              polyblepPulse(0.75 + 1e-10, duty, dt) - polyblepPulse(0.75 - 1e-10, duty, dt));
  std::printf("OVER-REPRO-UNC   t=0.75-1e-10  out=%.15f   t=0.75+1e-10  out=%.15f   "
              "diff=%.15f  (sits at the closed form => the control is live)\n",
              uncappedPulse(0.75 - 1e-10, duty, dt), uncappedPulse(0.75 + 1e-10, duty, dt),
              uncappedPulse(0.75 + 1e-10, duty, dt) - uncappedPulse(0.75 - 1e-10, duty, dt));
  {
    const double t = 0.75;
    std::printf("OVER-REPRO-TERMS t=0.75 naive=%.1f  R(t)=%.15f  R(frac(t-duty))=%.15f  "
                "cap_corr=%+.15f  unc_corr=%+.15f\n",
                naivePulse(t, duty), lunar24::core::polyblepResidual(t, dt),
                lunar24::core::polyblepResidual(frac(t - duty), dt),
                polyblepPulse(t, duty, dt) - naivePulse(t, duty),
                uncappedPulse(t, duty, dt) - naivePulse(t, duty));
    // The header note's second explicit non-claim, checked rather than restated: `guardedPulse`
    // switches the correction OFF at this cell, so it is exactly the naive pulse there -- which is
    // what a "fall back to naive out of domain" strategy would do to a REAL cell and not merely to
    // a synthetic one. Reported so the rejected candidate's behaviour is on the record as a number.
    std::printf("OVER-REPRO-GUARDED t=0.75 guarded=%.15f naive=%.1f  (switch-off arm = naive)\n",
                guardedPulse(t, duty, dt), naivePulse(t, duty));
  }
  // THE BOUNDARY CORNER AT dt = 0.5, MEASURED TWO WAYS, BECAUSE THE TWO WAYS ARE DIFFERENT QUESTIONS.
  //
  //   * phase varied at fixed step (`OVER-REPRO-X0` below): t = 0.5 +/- eps, duty = 0.75, dt = 0.5
  //     held. This asks whether the OUTPUT is continuous in PHASE at the boundary. It reads
  //     -4.00000033096148e-10 at eps = 1e-10, exactly -4*eps.
  //   * step varied at fixed phase (`OVER-REPRO-X0STEP` below): dt = 0.5 +/- eps, t = 0.49,
  //     duty = 0.2 held. This asks whether the output is continuous in the STEP at the boundary --
  //     which is where the cap actually acts, since w = min(dt, cap) changes form there. It reads
  //     8.95999941e-11 at eps = 1e-10.
  //
  // @Codex's `step = .5` line quotes ~8.96e-11 with exactly this construction (fixed phase, right
  // minus left, program /tmp/review118-widthcap.cpp), and that is the number reproduced here. My own
  // earlier reading of his line as the phase-varied coordinate produced a "discrepancy" that was
  // never one: the two are different questions at different (t, duty, eps) triples, and both are
  // reproduced. Recorded because the wrong reading was mine and is worth not repeating.
  //
  // WHAT MAKES THE STEP-VARIED VALUE WHAT IT IS. Only the LEFT sample changes w (0.5 - eps -> w =
  // 0.5 - eps, still below the cap, so w tracks dt) while the RIGHT sample is pinned at w = 0.5. So
  // the capped difference is exactly HALF the uncapped one, where both sides vary -- the two columns
  // below are in that 1:2 ratio at every eps. Nothing here is asserted as a product claim; the check
  // that the boundary behaviour is pinned by the cap is `OVER-REPRO-X0` and `OVER-STRUCT-CAPISIDENTITY`.
  {
    const double d5 = 0.5, duty5 = 0.5 * (d5 + 1.0);
    std::printf("OVER-REPRO-X0    dt=0.5 duty=%.6f eps=1e-10  cap_diff=%.15g  unc_diff=%.15g"
                "  (phase varied, step held)\n",
                duty5, polyblepPulse(d5 + 1e-10, duty5, d5) - polyblepPulse(d5 - 1e-10, duty5, d5),
                uncappedPulse(d5 + 1e-10, duty5, d5) - uncappedPulse(d5 - 1e-10, duty5, d5));
    const double tS = 0.49, dutyS = 0.2;
    std::printf("OVER-REPRO-X0STEP t=%.2f duty=%.2f  (step varied, phase held; right minus left)\n",
                tS, dutyS);
    for (double eps : {1e-9, 1e-10, 1e-11, 1e-12}) {
      const double cap = polyblepPulse(tS, dutyS, d5 + eps) - polyblepPulse(tS, dutyS, d5 - eps);
      const double unc = uncappedPulse(tS, dutyS, d5 + eps) - uncappedPulse(tS, dutyS, d5 - eps);
      std::printf("OVER-REPRO-X0STEP eps=%-7.0e cap_diff=%-16.9g unc_diff=%-16.9g"
                  "  cap/eps=%-14.9g unc/cap=%.9f\n",
                  eps, cap, unc, cap / eps, unc / cap);
    }
  }
}

// Per-dt out-of-domain summary: the boundedness question (max |out| over the reachable duty set),
// the t-gap at four resolutions (the jump/no-jump discriminator), and the value at @Codex's duty.
void runOverGrid() {
  std::printf("OVER-GRID out-of-domain cells: max|out| over duty in [1e-3,1-1e-3] and the t-gap "
              "floor\n");
  std::printf("OVER-GRID-CAP   dt         maxabs     excess_over_1  worst_duty  t_gap(1e3)   "
              "(1e4)        (1e5)        ratio(1e3/1e5)\n");
  std::printf("OVER-GRID-UNC   dt         maxabs     t_gap(1e3)   (1e4)        (1e5)        "
              "ratio(1e3/1e5)  closed_re\n");
  // The discriminator, stated once and then measured twice. A CONTINUOUS output's t-gap falls as
  // 1/nT, so the ratio between the 1e3 and 1e5 columns is ~100. A JUMP's gap is invariant in nT, so
  // the ratio is ~1. The capped arm must show ~100 at EVERY dt; the uncapped control must show ~1
  // for 0.5 < dt < 1 (its own residual step) and ~100 for dt <= 0.5 (where the two arms coincide).
  for (double dt : {0.5, 0.6, 2.0 / 3.0, 0.75, 0.9, 0.999, 1.0, 1.01, 1.5, 2.0, 4.0, 13.0,
                    1307.747846, 1e4, 1e300}) {
    Worst wAbs, wAbsU;
    const int nDuty = 400, nT = 400;
    for (int b = 0; b < nDuty; ++b) {
      const double duty = 1e-3 + (1.0 - 2e-3) * static_cast<double>(b) / static_cast<double>(nDuty - 1);
      for (int c = 0; c < nT; ++c) {
        const double t = (static_cast<double>(c) + 0.5) / static_cast<double>(nT);
        wAbs.see(std::fabs(polyblepPulse(t, duty, dt)), dt, duty, t);
        wAbsU.see(std::fabs(uncappedPulse(t, duty, dt)), dt, duty, t);
      }
    }
    // The closed form (2-1/dt)^2 describes the jump at t=dt only for 0.5 < dt < 1. Outside that
    // range the column says so rather than printing a 0 that would read as "no jump here".
    char closed[32];
    if (dt > 0.5 && dt < 1.0) {
      std::snprintf(closed, sizeof(closed), "%.9f", (2.0 - 1.0 / dt) * (2.0 - 1.0 / dt));
    } else {
      std::snprintf(closed, sizeof(closed), "n/a");
    }
    const double g[3] = {tGap(true, dt, 0.2, 1000L).v, tGap(true, dt, 0.2, 10000L).v,
                         tGap(true, dt, 0.2, 100000L).v};
    const double gu3 = tGap(false, dt, 0.2, 1000L).v;
    const double gu4 = tGap(false, dt, 0.2, 10000L).v;
    const double gu5 = tGap(false, dt, 0.2, 100000L).v;
    std::printf("OVER-GRID-CAP %-14.7g %11.6f %14.3e  %-11.6f %12.7f %12.7f %12.7f %13.2f\n",
                dt, wAbs.v, wAbs.v - 1.0, wAbs.duty, g[0], g[1], g[2], g[0] / g[2]);
    std::printf("OVER-GRID-UNC %-14.7g %11.6f %12.7f %12.7f %12.7f %13.2f  %11s\n",
                dt, wAbsU.v, gu3, gu4, gu5, gu3 / gu5, closed);
  }
}

// ================================================================================================
// STRUCTURE: the three identities the header note above kPolyblepMaxDt actually claims
//
// These are DIRECTED checks, not sweeps. Each one is the machine-checkable form of a sentence in
// pulse_blep_kernel.h, so a future edit that breaks the sentence breaks this line:
//
//   OVER-STRUCT-CAPISIDENTITY   min(dt, kPolyblepMaxDt) IS dt in domain -> BIT-IDENTICAL, not
//                               "close". Compared against `uncappedPulse` (the same formula with
//                               `w = dt`), which is by construction what the previous revision did.
//   OVER-STRUCT-EXTENSION       for every dt >= 0.5 the output is the SAME FUNCTION of (t, duty) as
//                               dt = 0.5, sample for sample. This is the whole content of "every
//                               out-of-domain cell is exactly an in-domain cell's value".
//   OVER-STRUCT-LABEL           polyblepPulseWindowsDisjoint returns the same value under the
//                               capped width as under the raw one, on the whole (duty, dt) grid.
//   OVER-STRUCT-SUP             sup |output| over a closed in-domain grid, reported as the EXCESS
//                               over 1 rather than only the max, because "max = 1.000000000000000"
//                               is also what a max of 1.0000000000000004 prints as at 15 digits.
// ================================================================================================
void runOverStructure() {
  {
    long n = 0, bad = 0;
    for (double dt : {1e-12, 1e-9, 1e-6, 1e-3, 0.01, 0.1, 220.0 / 44100.0, 0.25, 0.4999, 0.5}) {
      for (int b = 0; b <= 600; ++b) {
        const double duty = static_cast<double>(b) / 600.0;
        for (int c = 0; c < 600; ++c) {
          const double t = (static_cast<double>(c) + 0.5) / 600.0;
          ++n;
          if (polyblepPulse(t, duty, dt) != uncappedPulse(t, duty, dt)) ++bad;
        }
      }
    }
    std::printf("OVER-STRUCT-CAPISIDENTITY in-domain samples=%ld bit-different=%ld"
                "  (cap == uncapped for every dt <= kPolyblepMaxDt)\n", n, bad);
  }
  {
    long n = 0, bad = 0, mmMax = 0;
    for (double dt : {0.5, 0.5000001, 0.51, 0.75, 1.0, 2.0, 13.0, 1307.747846, 1e4, 1e300}) {
      long mm = 0;
      for (int b = 0; b <= 1200; ++b) {
        const double duty = static_cast<double>(b) / 1200.0;
        for (int c = 0; c < 1200; ++c) {
          const double t = (static_cast<double>(c) + 0.5) / 1200.0;
          ++n;
          if (polyblepPulse(t, duty, dt) != polyblepPulse(t, duty, 0.5)) { ++bad; ++mm; }
        }
      }
      mmMax = std::max(mmMax, mm);
    }
    std::printf("OVER-STRUCT-EXTENSION dt in {0.5,5e-7 over,0.51,0.75,1,2,13,1307.747846,1e4,1e300}"
                " samples=%ld bit-different=%ld worst_per_dt=%ld"
                "  (out(dt) IS out(0.5), sample for sample)\n", n, bad, mmMax);
  }
  {
    long n = 0, bad = 0;
    for (int a = 0; a <= 400; ++a) {
      const double dt = 0.6 * static_cast<double>(a) / 400.0;
      for (int b = 0; b <= 400; ++b) {
        const double duty = static_cast<double>(b) / 400.0;
        const double m = std::min(duty, 1.0 - duty);
        const bool cappedForm = !(dt > 0.0) ? true : (m >= 2.0 * std::min(dt, lunar24::core::kPolyblepMaxDt));
        ++n;
        if (cappedForm != polyblepPulseWindowsDisjoint(duty, dt)) ++bad;
      }
    }
    std::printf("OVER-STRUCT-LABEL cells=%ld disagreements=%ld"
                "  (the shipped predicate equals the honest m >= 2*min(dt,cap) form)\n", n, bad);
  }
  {
    Worst w;
    long n = 0;
    for (int a = 1; a <= 2000; ++a) {
      const double dt = 0.5 * static_cast<double>(a) / 2000.0;
      for (int b = 0; b <= 400; ++b) {
        const double duty = static_cast<double>(b) / 400.0;
        for (int c = 0; c < 400; ++c) {
          const double t = static_cast<double>(c) / 400.0;
          ++n;
          w.see(std::fabs(polyblepPulse(t, duty, dt)), dt, duty, t);
        }
      }
    }
    std::printf("OVER-STRUCT-SUP in-domain closed grid cells=%ld sup=%.15f excess_over_1=%.3e"
                " at dt=%.9f duty=%.9f t=%.9f\n", n, w.v, w.v - 1.0, w.dt, w.duty, w.t);
  }
}

// STOP / REVERSE. `frequencyHz()` returns `p > 0.0 ? p : 0.0`, so the pitch that reaches tick() is
// never negative, and `instHz = pitch + fmDevHz_*fmCv_` with fmDevHz_/fmCv_ having no caller
// anywhere in the tree, so step >= 0 through every path the product has. The kernel's own behaviour
// at a non-positive step is nevertheless a stated part of the strategy and is measured here rather
// than asserted: `if (!(dt > 0.0)) return 0.0;` is the FIRST statement, so the output must be
// BIT-IDENTICAL to the naive pulse for dt <= 0, including the signed-zero and NaN-adjacent cases.
void runStopReverse() {
  std::printf("OVER-STOP  dt <= 0 must return the naive pulse BIT-IDENTICALLY (not merely to 1e-15)\n");
  const long bad[] = {0, 0};
  long mismatches = 0, total = 0;
  const double dts[] = {0.0, -0.0, -1e-300, -1e-9, -0.3, -0.75, -4.0};
  for (double dt : dts) {
    long mm = 0;
    for (int b = 0; b <= 2000; ++b) {
      const double duty = static_cast<double>(b) / 2000.0;
      for (int c = 0; c < 2000; ++c) {
        const double t = (static_cast<double>(c) + 0.5) / 2000.0;
        const double got = polyblepPulse(t, duty, dt);
        const double want = naivePulse(t, duty);
        ++total;
        if (got != want) ++mm;
      }
    }
    mismatches += mm;
    std::printf("OVER-STOP  dt=%-10.4g non-identical samples=%ld  (of 4002000)\n", dt, mm);
  }
  std::printf("OVER-STOP  total non-identical=%ld of %ld over %zu dt values\n",
              mismatches, total, sizeof(dts) / sizeof(dts[0]));
  (void)bad;
}

// ================================================================================================
// REACHABLE BOX: what dt the PRODUCT can actually hand to this kernel
//
// This section drives the REAL `lunar24::core::Vco` (the class the runtime instantiates, via its
// public setters) and reads back `frequencyHz()`, so every number below is the production pitch law
// and its clamps EVALUATED, not re-derived in the tool. `tick()` computes `step = instHz / sr_` with
// `instHz = frequencyHz() + fmDevHz_ * fmCv_`; `setFmDepth`/`setFmCv` have no caller anywhere in the
// tree (checked in run_gh19_s3_kernel_sweep.py, which fails closed if that stops being true), so on
// every path the product has, instHz IS frequencyHz() and dt = frequencyHz()/sr exactly.
//
// The input box these configurations come from is the FROZEN registry/spec, not this file:
//   base          440.0 Hz            machine_definition.h kVcoBaseHzProvisional
//   oct_sel       3 positions         spec/machine/lunar24.json vco_a.oct_sel, values {-1,0,+3} (vco.h)
//   tune          -1 .. +1 oct        spec/machine/lunar24.json vco_a.tune
//   v_oct_in      0 .. 8 V, exp       spec/machine/lunar24.json vco_a.v_oct_in
//   cv_in        -5 .. +5 V           spec/machine/lunar24.json vco_a.cv_in
//   cv_amt        0 .. 1              spec/machine/lunar24.json vco_a.cv_amt
//   lin_exp       lin | exp           spec/machine/lunar24.json vco_a.lin_exp
// The runner re-reads those files and refuses to write the evidence if a constant has moved.

void boxCell(const char* label, double sr, double octSel, double tune, double vOct,
             lunar24::core::VcoControlMode mode, double cv, double cvAmt, bool report) {
  lunar24::core::Vco v(sr);
  v.setBaseHz(440.0);
  v.setOctaveSelect(static_cast<int>(octSel));
  v.setTune(tune);
  v.setVoct(vOct);
  v.setCvAmt(cvAmt);
  v.setCvInput(cv, mode);
  const double f = v.frequencyHz();
  if (report) {
    std::printf("BOX %-34s sr=%-6.0f f=%18.6f Hz  dt=f/sr=%14.9f  %s\n", label, sr, f, f / sr,
                (f / sr > lunar24::core::kPolyblepMaxDt) ? "OUTSIDE kernel contract" : "inside");
  }
}

void runReachableBox() {
  const double srs[] = {44100.0, 48000.0, 88200.0, 96000.0};
  std::printf("BOX kPolyblepMaxDt=%.6f  (dt is the argument handed to polyblepPulseCorrection)\n",
              lunar24::core::kPolyblepMaxDt);
  for (double sr : srs) {
    // Panel controls only, NO cable patched. This is the largest dt the instrument reaches with
    // the front panel alone (oct_sel "+3" with tune all the way up).
    boxCell("panel max (oct+3, tune+1)", sr, 2, 1.0, 0.0,
            lunar24::core::VcoControlMode::kExponential, 0.0, 1.0, true);
    // The smallest V/OCT that leaves the contract domain at that panel setting, found by bisection
    // ON THE REAL OBJECT rather than by solving the law: the problem is "which voltage does the
    // instrument actually reach dt=0.5 at", and the object is the authority on that.
    {
      auto dtAt = [&](double volts) {
        lunar24::core::Vco v(sr);
        v.setBaseHz(440.0);
        v.setOctaveSelect(2);
        v.setTune(1.0);
        v.setVoct(volts);
        return v.frequencyHz() / sr;
      };
      double lo = 0.0, hi = 8.0;   // v_oct_in is declared unipolar 0..8 V
      if (dtAt(hi) > lunar24::core::kPolyblepMaxDt) {
        for (int i = 0; i < 200; ++i) {
          const double mid = 0.5 * (lo + hi);
          if (dtAt(mid) > lunar24::core::kPolyblepMaxDt) hi = mid; else lo = mid;
        }
        std::printf("BOX leaves the contract at V/OCT = %.6f V  (panel oct+3 tune+1, sr=%.0f);"
                    " dt(%.6f V)=%.9f  dt(%.6f V)=%.9f\n",
                    hi, sr, lo, dtAt(lo), hi, dtAt(hi));
      } else {
        std::printf("BOX V/OCT 0..8 V stays inside the contract at panel max, sr=%.0f\n", sr);
      }
    }
    // The declared box in full: panel max AND both cable inputs at their declared extremes, in
    // each of the two selectable CV laws (lin_exp is a panel selector, so both are reachable).
    boxCell("declared max, CV exp (+5V)", sr, 2, 1.0, 8.0,
            lunar24::core::VcoControlMode::kExponential, 5.0, 1.0, true);
    boxCell("declared max, CV lin (+5V)", sr, 2, 1.0, 8.0,
            lunar24::core::VcoControlMode::kLinear, 5.0, 1.0, true);
    // STOP, through the real object: the linear law turns a large enough negative CV into a
    // non-positive pitch, and frequencyHz() clamps it to exactly 0. So the "reverse" the kernel
    // would need to handle is not reachable -- the instrument stops instead.
    boxCell("CV lin -2V (clamp -> stop)", sr, 2, 1.0, 0.0,
            lunar24::core::VcoControlMode::kLinear, -2.0, 1.0, true);
    boxCell("CV exp -5V (never negative)", sr, 2, 1.0, 0.0,
            lunar24::core::VcoControlMode::kExponential, -5.0, 1.0, true);
    boxCell("V/OCT -8V (jack below range)", sr, 2, 1.0, -8.0,
            lunar24::core::VcoControlMode::kExponential, 0.0, 0.0, true);
  }

  // STOP measured at the EMITTED signal: with the pitch clamped to 0 the phase never advances, so
  // the VCO must emit a constant sample whose value is the naive pulse at the held phase, with the
  // correction exactly 0 (the kernel's own `!(dt > 0)` branch). This is the stop case end to end.
  {
    const double sr = 44100.0;
    lunar24::core::Vco v(sr);
    v.setBaseHz(0.0);           // the clamped-to-zero pitch, expressed at its source
    v.setOctaveSelect(1);
    v.setShape(0.3);
    v.setWaveform(lunar24::core::VcoWaveform::kPulse);
    double first = 0.0, worst = 0.0;
    for (int i = 0; i < 64; ++i) {
      double out = 0.0;
      v.tick(&out);
      if (i == 0) first = out;
      worst = std::max(worst, std::fabs(out - first));
    }
    std::printf("BOX stop: baseHz=0 over 64 ticks -> first=%.15f  max deviation=%.15f"
                "  (naive pulse at the held phase, correction exactly 0)\n", first, worst);
    std::printf("BOX stop: frequencyHz()=%.15f  step=%.15f\n", v.frequencyHz(),
                v.frequencyHz() / sr);
  }
}

// ================================================================================================
// DIRECTED EVIDENCE THROUGH THE REAL Vco (@Codex, task #118 ruling: "真实 Vco 和产品 V/OCT 跨界、
// 停止、分块/sync 定向证据")
//
// Everything above measures polyblepPulse as a pure function. This section measures the PRODUCT
// PATH: the Vco object the runtime actually instantiates, driven through its public setters and
// tick(), with no re-derivation of the pitch law anywhere in the tool.
//
// The one identity that makes all of it checkable is the WIRING IDENTITY. At kMorphRing with
// morph = 1.0 the mix collapses to the pulse node (wave_map::pulseWeight = 1, triangleWeight = 0
// EXACTLY -- printed below rather than assumed), so `emittedAt_` is exactly
// `naive + polyblepPulseCorrection`, i.e.
//
//     tick_out(k)  ==  polyblepPulse(phase_after_tick, effectiveDuty(), step)
//
// BIT FOR BIT, with `phase_after_tick` read back from the object's own `phase()` accessor rather
// than recomputed from k*step. That distinction matters: recomputing the phase reintroduces the
// accumulator's rounding as a false mismatch (my own first attempt at this measured 38011/40960
// mismatches at 2.2e-08 that way -- an artefact of the tool, not of the product). Reading the
// object's phase makes the check exact, and it checks what actually matters: that the correction
// the product applies is the standalone kernel's correction, at the phase the sampler used, with
// the effective duty the sampler used -- in domain AND out of it.
// ================================================================================================

void runVcoDirected() {
  using lunar24::core::VcoControlMode;
  using lunar24::core::VcoWaveform;
  const double sr = 44100.0;

  // ---- (1) the wiring identity, across the whole reachable V/OCT range ----------------------
  {
    long n = 0, mm = 0;
    double worst = 0.0, sup = 0.0;
    for (double voct : {0.0, 0.5, 1.0, 1.647131, 1.7, 2.0, 3.0, 4.0, 6.0, 8.0}) {
      lunar24::core::Vco v(sr);
      v.setBaseHz(440.0);
      v.setOctaveSelect(2);
      v.setTune(1.0);
      v.setVoct(voct);
      v.setWaveform(VcoWaveform::kMorphRing);
      v.setMorph(1.0);
      v.setShape(0.3);
      const double step = v.frequencyHz() / sr, duty = v.effectiveDuty();
      for (int k = 1; k <= 4096; ++k) {
        double out = 0.0;
        v.tick(&out);
        const double want = polyblepPulse(v.phase(), duty, step);
        ++n;
        sup = std::max(sup, std::fabs(out));
        if (out != want) { ++mm; worst = std::max(worst, std::fabs(out - want)); }
      }
    }
    std::printf("VCO-WIRING samples=%ld bit-mismatches=%ld worst_abs=%.6g sup|out|=%.15f"
                "  (tick_out == polyblepPulse(phase(), effectiveDuty(), step))\n",
                n, mm, worst, sup);
    std::printf("VCO-WIRING weights at morph=1.0: pulseWeight=%.17g triangleWeight=%.17g"
                "  (exact collapse to the pulse node is what makes the identity bit-exact)\n",
                lunar24::core::wave_map::pulseWeight(lunar24::core::wave_map::kRingEqual, 1.0),
                lunar24::core::wave_map::triangleWeight(lunar24::core::wave_map::kRingEqual, 1.0));
  }

  // ---- (2) block invariance: the accumulator does not depend on the chunking ----------------
  // The kernel is called once per sample from tick() and holds no state of its own, so the emitted
  // sequence must not depend on how the caller groups the calls. Measured, not argued: one 4096
  // run against 64 runs of 64 on identical objects.
  {
    auto run = [](int chunks, int per, std::vector<double>& o) {
      lunar24::core::Vco v(44100.0);
      v.setBaseHz(440.0);
      v.setOctaveSelect(2);
      v.setTune(1.0);
      v.setVoct(1.7);
      v.setWaveform(VcoWaveform::kMorphRing);
      v.setMorph(1.0);
      v.setShape(0.3);
      for (int c = 0; c < chunks; ++c)
        for (int i = 0; i < per; ++i) { double x = 0.0; v.tick(&x); o.push_back(x); }
    };
    std::vector<double> a, b;
    run(1, 4096, a);
    run(64, 64, b);
    long d = 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++d;
    std::printf("VCO-BLOCK samples=%zu bit-different=%ld  (1 x 4096 vs 64 x 64, voct=1.7"
                " => dt beyond the cap)\n", a.size(), d);
  }

  // ---- (3) HARD SYNC: land on phase 0, and the emitted sample obeys its own formula ----------
  // tick() computes `jmp = emittedAt_(0) - emittedAt_(cumPitch_)` and then `out -= 0.5*jmp`, so
  // with e0 = emittedAt_(0.0, step) and E the twin's un-reset output at the same tick,
  //
  //     out  ==  0.5*e0 + 0.5*E
  //
  // The twin (an identical Vco that is never asked to sync) supplies E BIT EXACTLY, because both
  // objects advance identically up to and including the reset tick and the reset is applied only
  // after `jmp` has been measured. e0 is then SOLVED from the offset-0 arm (out0 = 0.5*e0 + 0.5*E1)
  // instead of being recomputed, so the identity is closed over measured quantities with no
  // standalone reconstruction in it. Then it must hold at every reset phase, not just one.
  {
    const double voct = 2.0;
    auto make = [&]() {
      lunar24::core::Vco v(sr);
      v.setBaseHz(440.0);
      v.setOctaveSelect(1);
      v.setTune(1.0);
      v.setVoct(voct);
      v.setWaveform(VcoWaveform::kMorphRing);
      v.setMorph(1.0);
      v.setShape(0.35);
      return v;
    };
    lunar24::core::Vco probe = make();
    const double step = probe.frequencyHz() / sr, duty = probe.effectiveDuty();
    const int NOFF = 200;
    std::vector<double> twin(NOFF + 2, 0.0);
    {
      lunar24::core::Vco t = make();
      for (int i = 1; i <= NOFF + 1; ++i) { double x = 0.0; t.tick(&x); twin[i] = x; }
    }
    std::vector<double> outv(NOFF + 1, 0.0);
    std::vector<double> phaseAfter(NOFF + 1, 0.0);
    long bad = 0, badPhase = 0;
    double sup = 0.0;
    for (int off = 0; off <= NOFF; ++off) {
      lunar24::core::Vco v = make();
      for (int i = 0; i < off; ++i) { double x = 0.0; v.tick(&x); }
      v.requestSync();
      double y = 0.0;
      v.tick(&y);
      outv[off] = y;
      phaseAfter[off] = v.phase();
      sup = std::max(sup, std::fabs(y));
      if (v.phase() != 0.0) ++badPhase;
    }
    // e0 solved from the arm whose pre-reset phase is exactly 0: out0 = 0.5*e0 + 0.5*twin[1].
    const double e0 = 2.0 * outv[0] - twin[1];
    for (int off = 0; off <= NOFF; ++off) {
      // The twin at tick off+1 is the emitted signal at the phase this arm RESET FROM, because the
      // reset is applied after `jmp` is measured and the two accumulators agree up to that tick.
      const double want = 0.5 * e0 + 0.5 * twin[off + 1];
      if (outv[off] != want) ++bad;
    }
    std::printf("VCO-SYNC resets=%d  phase_after_reset_nonzero=%ld  formula_mismatches=%ld"
                "  sup|out|=%.15f\n", NOFF + 1, badPhase, bad, sup);
    std::printf("VCO-SYNC solved e0=%.17g  polyblepPulse(0,duty,step)=%.17g  bitmatch=%d"
                "  step=%.17g duty=%.17g\n", e0, polyblepPulse(0.0, duty, step),
                (int)(e0 == polyblepPulse(0.0, duty, step)), step, duty);
  }

  // ---- (4) STOP: the pitch law clamps to zero and the phase freezes --------------------------
  // Two independent stop routes are driven, because `p > 0.0 ? p : 0.0` is reached from two
  // directions: a linear CV that drives the bracket to zero or below (the only way to get exactly
  // 0 out of a positive base), and an exponential exponent so negative that the power underflows.
  {
    struct StopCase { const char* label; double cv; VcoControlMode mode; double voct; };
    const StopCase cases[] = {
      {"lin CV -1.0V (bracket exactly 0)", -1.0, VcoControlMode::kLinear, 0.0},
      {"lin CV -5.0V (bracket negative)",  -5.0, VcoControlMode::kLinear, 0.0},
      {"exp V/OCT -1200V (underflow)",      0.0, VcoControlMode::kExponential, -1200.0},
    };
    for (const StopCase& c : cases) {
      lunar24::core::Vco v(sr);
      v.setBaseHz(440.0);
      v.setOctaveSelect(2);
      v.setTune(1.0);
      v.setVoct(c.voct);
      v.setCvAmt(1.0);
      v.setCvInput(c.cv, c.mode);
      v.setWaveform(VcoWaveform::kMorphRing);
      v.setMorph(1.0);
      v.setShape(0.3);
      const double f = v.frequencyHz();
      double first = 0.0, mx = 0.0, mn = 0.0;
      long diff = 0;
      for (int i = 0; i < 1000; ++i) {
        double x = 0.0;
        v.tick(&x);
        if (i == 0) { first = x; mn = mx = x; }
        else { if (x != first) ++diff; mx = std::max(mx, x); mn = std::min(mn, x); }
      }
      std::printf("VCO-STOP %-34s freq=%.17g  step=%.17g  samples!=first=%ld  range=[%.15f, %.15f]"
                  "  phase=%.17g\n", c.label, f, f / sr, diff, mn, mx, v.phase());
    }
    // Negative pitch must STOP and never reverse: the phase is monotone non-decreasing under every
    // non-negative step, and `frequencyHz()` has no route to a negative return.
    {
      long neg = 0;
      for (double voct : {-1.0, -5.0, -50.0, -1000.0}) {
        lunar24::core::Vco v(sr);
        v.setVoct(voct);
        if (v.frequencyHz() < 0.0) ++neg;
      }
      for (double cv : {-1.0, -2.0, -5.0}) {
        lunar24::core::Vco v(sr);
        v.setCvAmt(1.0);
        v.setCvInput(cv, VcoControlMode::kLinear);
        if (v.frequencyHz() < 0.0) ++neg;
      }
      std::printf("VCO-STOP negative_pitch_returns=%ld  (frequencyHz() clamps, so the kernel's"
                  " dt<0 branch is unreachable from the product)\n", neg);
    }
  }

  // ---- (5) V/OCT CROSSOVER: the cap turns on at dt = kPolyblepMaxDt, continuously -------------
  // The product crosses the cap boundary somewhere between the two contract V/OCT points
  // (1.647131 V -> dt just under 0.5, 1.769387 V -> dt just over). Two things are measured:
  // (a) that the crossing is a CONTINUOUS turn-on -- the emitted waveform is Lipschitz in dt with a
  //     bounded constant near it, so no jump is introduced by the cap switching in; and
  // (b) that the flattened dt >= 0.5 function is what the object emits on the far side, via the
  //     wiring identity already established above.
  {
    double worstStep = 0.0, atDt = 0.0;
    for (double t : {0.0, 0.13, 0.4, 0.4999, 0.5, 0.77, 0.99}) {
      for (double duty : {0.05, 0.2, 0.5, 0.8, 0.95}) {
        double prev = 0.0;
        bool first = true;
        for (int i = -4000; i <= 4000; ++i) {
          const double dt = 0.5 + static_cast<double>(i) * 2.5e-7;
          const double val = polyblepPulse(t, duty, dt);
          if (!first) {
            const double j = std::fabs(val - prev);
            if (j > worstStep) { worstStep = j; atDt = dt; }
          }
          prev = val;
          first = false;
        }
      }
    }
    std::printf("VCO-CROSSOVER max |out(dt+d)-out(dt)| over dt in [0.499,0.501] step 2.5e-7"
                " = %.6g at dt=%.9f  (bounded by ~2*d => continuous turn-on, no jump)\n",
                worstStep, atDt);
    // The two contract endpoints, on the real object, showing the cap is OFF just below and ON just
    // above, and that the object's own dt agrees with the contract's stated values.
    for (double voct : {1.647131, 1.769387}) {
      lunar24::core::Vco v(sr);
      v.setBaseHz(440.0);
      v.setOctaveSelect(2);
      v.setTune(1.0);
      v.setVoct(voct);
      const double dt = v.frequencyHz() / sr;
      std::printf("VCO-CROSSOVER contract V/OCT %.6f V -> f=%.6f Hz  dt=%.12f  capped=%d\n",
                  voct, v.frequencyHz(), dt, (dt > lunar24::core::kPolyblepMaxDt) ? 1 : 0);
    }
    // And the identity that ties (b) to the object: on the far side the object's emitted sample
    // equals the dt = 0.5 function at the same phase, bit for bit.
    {
      lunar24::core::Vco v(sr);
      v.setBaseHz(440.0);
      v.setOctaveSelect(2);
      v.setTune(1.0);
      v.setVoct(1.769387);
      v.setWaveform(VcoWaveform::kMorphRing);
      v.setMorph(1.0);
      v.setShape(0.3);
      const double step = v.frequencyHz() / sr, duty = v.effectiveDuty();
      long n = 0, mm = 0;
      double sup = 0.0;
      for (int k = 1; k <= 4096; ++k) {
        double out = 0.0;
        v.tick(&out);
        ++n;
        sup = std::max(sup, std::fabs(out));
        if (out != polyblepPulse(v.phase(), duty, lunar24::core::kPolyblepMaxDt)) ++mm;
      }
      std::printf("VCO-CROSSOVER far side: samples=%ld mismatches_vs_out(dt=cap)=%ld sup|out|=%.15f"
                  " (dt=%.9f)\n", n, mm, sup, step);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool all = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--all") == 0) all = true;
  }
  (void)all;

  std::printf("gh19_s3_pulse_kernel_sweep: header compiled directly, no product link\n");
  std::printf("kPolyblepMaxDt=%.6f\n", lunar24::core::kPolyblepMaxDt);

  runRepro(220.0 / 44100.0);

  // The refinement point: the review's own coordinate, and the same coordinate's mirror.
  runRefine(220.0 / 44100.0, (220.0 / 44100.0) / 4.0, 0.9 * 2.0 * (220.0 / 44100.0),
            1.1 * 2.0 * (220.0 / 44100.0), true);
  runRefine(220.0 / 44100.0, (220.0 / 44100.0) / 4.0, 0.9 * 2.0 * (220.0 / 44100.0),
            1.1 * 2.0 * (220.0 / 44100.0), false);
  // and a wider traverse so the B edge is crossed too (the wrap point at duty = t)
  runRefine(220.0 / 44100.0, 0.4, 0.30, 0.50, true);
  runRefine(220.0 / 44100.0, 0.4, 0.30, 0.50, false);

  runGrid(0.5, 64, 1500, 1500);
  runNarrow(220.0 / 44100.0);
  runMoving(220.0 / 44100.0);
  runMoving(5000.0 / 44100.0);

  // The above all lives inside the proven domain (runGrid's dMax is exactly kPolyblepMaxDt, which
  // is the gap @Codex 144b1339 item 3 names). The section below is the outside of it.
  runStopReverse();
  runOverRepro();
  runOverResidualJump();
  runOverGrid();
  runOverStructure();
  runReachableBox();
  runVcoDirected();

  std::fflush(stdout);
  return 0;
}
