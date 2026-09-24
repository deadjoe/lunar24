#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh19_s6_saw_analyze.py - task #120 (GH #19 S6) reference instrument for the saw / invSaw
# stretches of the continuous wave-morph ring.
#
# WHAT S6 MEASURES. The morph ring renders, on stretch k,  y = (1-u)*node_k(p) + u*node_{k+1}(p).
# Stretch 0 is saw -> invSaw and stretch 1 is invSaw -> sine. Both carry a waveform whose only
# non-smoothness is a JUMP at the phase wrap: saw jumps -2, invSaw jumps +2 (invSawShape is the
# exact negation of sawShape - vco_wave_map.h:125-126), sine does not jump at all. S3 corrected the
# pulse node; S6 corrects the saw and invSaw nodes, where each node's correction is scaled by that
# node's weight in the mix.
#
# WHY A MIXED CELL NEEDS ITS OWN REFERENCE (S3's lesson, carried forward verbatim). The S3 report
# states it for the pulse case: a single-waveform model is not the model of a MIX, so applying it to
# a mixed cell measures nothing. The same applies here, so every reference in this file is built
# from the cell's own mix law. Nothing is borrowed across stretches or across waveform families.
#
# THE REFERENCES ARE DERIVED, NOT ASSUMED. Each is the closed-form truncated Fourier series of the
# node waveform, truncated at kmax = floor((sr/2)/f0), i.e. the only difference from the ideal node
# is the definitional removal of harmonics above Nyquist - no window, no fit, no tuning:
#
#     bl_saw(p)    = sum_{k=1..kmax} -(2/(pi*k)) * sin(2*pi*k*p)
#     bl_invSaw(p) = -bl_saw(p)                        (exact: the node IS the negation)
#     bl_sine(p)   = sin(2*pi*p)                       (exact: sine has one harmonic)
#
# and the mixed reference is
#
#     stretch 0:  (1-u)*bl_saw    + u*bl_invSaw      u = 4*morph          (morph in [0, 0.25])
#     stretch 1:  (1-u)*bl_invSaw + u*bl_sine        u = 4*morph - 1      (morph in [0.25, 0.5])
#
# The claim that these are the ring's own law rather than a second guess is NOT asserted in prose:
# --self-check evaluates the endpoints and requires the mixed reference to collapse onto each node
# reference exactly (morph = 0 -> bl_saw, 0.25 -> bl_invSaw, 0.5 -> bl_sine), and the weights to
# sum to 1 on each stretch. Degenerate identities pass trivially, so note that the endpoint checks
# are made against INDEPENDENTLY EVALUATED node series, not against the mix formula evaluated at a
# limit - see the (c) block.
#
# ---------------------------------------------------------------------------------------------
# THE TWO DEGENERATE POSITIONS, AND WHY THEY ARE NOT IMPROVEMENT CELLS. Writing stretch 0 as
# y = (1-u)*saw + u*(-saw) makes both the jump J(u) = (1-u)(-2) + u(+2) = -2 + 4u and the peak
# amplitude |1-2u| vanish at u = 0.5, i.e. morph = 0.125, where the mixture is identically zero for
# every phase. Two consequences the instrument must respect rather than paper over:
#
#   * morph = 0.125: the reference is identically 0, so residual_stats() returns None (its den == 0
#     guard). A dB improvement is not merely hard there, it does not exist. This is measured as an
#     EQUALITY (max |output| == 0), never as a ratio of two residuals that are both zero.
#   * morph = 0.5 (the sine node): the waveform is already smooth, so there is no jump to remove and
#     no improvement to demand. The invariant that holds exactly is that both new correction
#     coefficients are 0 there, so the output must be BIT-IDENTICAL to the pre-S6 product. That is
#     checked by comparison of the two arms' raw windows, not by any residual.
#
# EXIT CODES: 0 = requested work completed; 3 = an instrument self-check FAILED (the reference or
# the metric is not trustworthy, so no verdict may be drawn from this run); 2 = a required input was
# missing or malformed.

import argparse
import importlib.util
import math
import os
import sys

TWO_PI = 2.0 * math.pi


def _load_s3():
    """The S3 analyzer is IMPORTED, never copied and never edited: its evidence is SHA-pinned by
    REFUSE BASELINE-REPORT-PIN, so a local copy would be a second, drifting authority for the same
    metric. Loading it by path keeps the shared primitives (read_raw, phi_at, residual_stats, fft,
    band power, the decimator design) single-sourced."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "gh19_s3_pulse_analyze.py")
    if not os.path.exists(path):
        sys.stderr.write("FATAL: %s not found; the S6 instrument imports it rather than forking it\n"
                         % path)
        sys.exit(2)
    spec = importlib.util.spec_from_file_location("gh19_s3_pulse_analyze", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


S3 = _load_s3()
phi_at = S3.phi_at              # the ONE phase convention (frame n carries the phase after n+1 ticks)
residual_stats = S3.residual_stats


# ------------------------------------------------------------------- the ring's own node weights
# Closed forms of wave_map::sawWeight / invSawWeight, which S6 introduces, and of the sine node's
# weight, which already exists implicitly in the blend. Each is the weight of a NODE in a convex
# blend, NOT a normalisation: on every stretch the weights of the two active nodes sum to 1, and a
# node's weight is exactly 1.0 at its own boundary and 0.0 outside its stretches. They are written
# closed-form here so the reference cannot silently disagree with the product's locate()/u; the
# boundaries (morph exactly 0.25 / 0.5) are continuous from either side, which --self-check asserts.

def saw_node_weight(morph):
    """node 0 (saw). Right end of no stretch, left end of stretch 0 -> weight 1-u on stretch 0."""
    if morph < 0.0 or morph > 0.25:
        return 0.0
    return 1.0 - morph / 0.25


def invsaw_node_weight(morph):
    """node 1 (invSaw). Right end of stretch 0 (weight u) and left end of stretch 1 (weight 1-u)."""
    if morph < 0.25:
        return morph / 0.25
    if morph <= 0.5:
        return 1.0 - (morph - 0.25) / 0.25
    return 0.0


def sine_node_weight(morph):
    """node 2 (sine). Right end of stretch 1 (weight u), left end of stretch 2 (weight 1-u)."""
    if morph <= 0.25:
        return 0.0
    if morph <= 0.5:
        return (morph - 0.25) / 0.25
    if morph <= 0.75:
        return 1.0 - (morph - 0.5) / 0.25
    return 0.0


def stretch_of(morph):
    """The stretch index and its local u, for morph in [0, 1]. Boundary morphs resolve to the LEFT
    stretch with u = 1, matching the value continuity the ring's own comment relies on."""
    for k in range(4):
        lo = 0.25 * k
        hi = 0.25 * (k + 1)
        if morph <= hi or k == 3:
            return k, (morph - lo) / 0.25
    return 3, 1.0


# ------------------------------------------------------------------------------- node references

def kmax_for(f0, sr, cap=None):
    k = int(math.floor((sr / 2.0) / f0))
    if cap is not None and k > cap:
        k = int(cap)
    return k


def bl_saw(n0, count, f0, sr, kmax_override=None):
    """Band-limited saw, unit peak, jump -2. Truncated Fourier series of 2p-1, evaluated on the
    model phase grid with the SHARED phase convention, so alignment cannot drift away from the phase
    the product's own model check validates. Closed-form phase per sample (no accumulated rotation):
    the only error is float64 round-off and the definitional Nyquist truncation."""
    kmax = kmax_for(f0, sr, kmax_override)
    if kmax < 1:
        return None
    amp = [-2.0 / (math.pi * k) for k in range(1, kmax + 1)]
    out = []
    for i in range(count):
        p = phi_at(n0 + i, f0, sr)
        s = 0.0
        for k in range(1, kmax + 1):
            s += amp[k - 1] * math.sin(TWO_PI * k * p)
        out.append(s)
    return out


def bl_inv_saw(n0, count, f0, sr, kmax_override=None):
    """invSawShape is -(2p-1), so its band-limited form is the exact negation. Negating the samples
    rather than re-deriving the series is what makes 'exact' checkable."""
    r = bl_saw(n0, count, f0, sr, kmax_override)
    return None if r is None else [-v for v in r]


def bl_sine(n0, count, f0, sr, kmax_override=None):
    """sineShape(p) = sin(2*pi*p): one harmonic, so this is exact at every sr with kmax >= 1."""
    if kmax_for(f0, sr, kmax_override) < 1:
        return None
    return [math.sin(TWO_PI * phi_at(n0 + i, f0, sr)) for i in range(count)]


def ref_mix_saw(n0, count, f0, sr, morph, kmax_override=None):
    """The band-limited reference for a saw/invSaw stretch cell, built from THAT cell's own mix law.
    Stretch 2 and above are refused rather than approximated: this instrument covers stretches 0
    and 1, and a reference from the wrong stretch is the cross-family error S3 named."""
    k, u = stretch_of(morph)
    if k > 1:
        raise ValueError("morph=%.6f is on stretch %d; gh19_s6_saw_analyze covers stretches 0-1"
                         % (morph, k))
    a = bl_saw(n0, count, f0, sr, kmax_override)
    if a is None:
        return None
    if k == 0:
        b = bl_inv_saw(n0, count, f0, sr, kmax_override)
    else:
        b = bl_sine(n0, count, f0, sr, kmax_override)
    return [(1.0 - u) * x + u * y for x, y in zip(a, b)]


def naive_mix(n0, count, f0, sr, morph):
    """The ring's OWN non-band-limited law, evaluated from the product's closed forms (sawShape =
    2p-1, invSawShape = -(2p-1), sineShape = sin(2*pi*p); vco_wave_map.h:125-128). This is what the
    current product emits on stretches 0-1, and it is the arm the S6 criterion has to be able to
    reject: a metric that cannot see this defect cannot certify its removal. It is INDEPENDENT of
    ref_mix_saw (no series, no truncation), which is what makes it usable as the sensitivity probe
    in self-check (d) rather than a restatement of the reference."""
    k, u = stretch_of(morph)
    out = []
    for i in range(count):
        p = phi_at(n0 + i, f0, sr)
        s = 2.0 * p - 1.0
        if k == 0:
            b = -s
        elif k == 1:
            b = math.sin(TWO_PI * p)
        else:
            raise ValueError("morph=%.6f is not on stretch 0-1" % morph)
        out.append((1.0 - u) * s + u * b)
    return out


# --------------------------------------------------------------------- the EQUALITY measurements

def max_abs(x):
    m = 0.0
    for v in x:
        a = math.fabs(v)
        if a > m:
            m = a
    return m


def bit_diff_count(a, b):
    """Count of frames whose float64 bit pattern differs. Used for the default-invariance criterion,
    where 'the correction's coefficients are exactly 0' has to be demonstrated as BIT equality and
    not as 'the difference is small': a tiny difference is exactly what a scaled-but-nearly-zero
    correction would produce, and it is the defect this criterion exists to catch."""
    if len(a) != len(b):
        return -1
    import struct as _s
    n = 0
    for x, y in zip(a, b):
        if _s.pack("<d", x) != _s.pack("<d", y):
            n += 1
    return n


# ----------------------------------------------------------------------------------- self-check

def self_check(verbose=True):
    """Asserts the reference family against independently evaluated node series. Categories, because
    they answer different questions:
      (a) the mix must COLLAPSE onto each node reference at that node's own morph, and the collapse
          must be exact to round-off, not approximate;
      (b) the weights must sum to 1 on every stretch (convex blend) and be 0 outside their stretches;
      (c) MUST BE DEGENERATE: at morph = 0.125 the reference is identically zero. This is asserted as
          an exact zero, because a reference that is merely small there would let a residual ratio
          produce a number that looks like evidence and is not;
      (d) the instrument must be ABLE to see the defect S6 exists to remove, and must lose that
          ability exactly where the defect disappears: the naive product law's residual against the
          band-limited reference must exceed 20 dB wherever a full-scale jump exists, must track
          |J(u)|, and must collapse to UNDEFINED at the anti-phase midpoint (where the reference is
          identically zero). If it were flat across the sweep the metric would be blind on its own
          axis and no verdict could be drawn from the run.
    Returns 0 if every check held, else a bitmask."""
    ok = 0
    sr, f0, count, n0 = 48000.0, 440.0, 4096, 8192

    def say(*p):
        if verbose:
            print(*p)

    say("-- reference family self-check (sr=%.0f f0=%.0f count=%d) --" % (sr, f0, count))

    # (a) collapse onto the nodes. Each endpoint reference is evaluated INDEPENDENTLY (its own
    # series), never as ref_mix_saw at a limit, or the check would compare the mix law to itself.
    say("  (a) mixed reference must collapse onto each node reference:")
    for morph, name, node in ((0.0, "bl_saw", bl_saw), (0.25, "bl_invSaw", bl_inv_saw),
                              (0.5, "bl_sine", bl_sine)):
        r = ref_mix_saw(n0, count, f0, sr, morph)
        e = node(n0, count, f0, sr)
        d = max_abs([x - y for x, y in zip(r, e)])
        good = d < 1e-12
        if not good:
            ok |= 3
        say("    morph=%.4f vs %-9s  max|diff|=%.3g  %s"
            % (morph, name, d, "EXACT (ok)" if good else "*** MIX LAW DISAGREES ***"))

    # (b) convexity: the two active node weights sum to 1, and each is 0 outside its stretches.
    say("  (b) weights must be a convex blend on each stretch:")
    for morph in (0.0, 0.0625, 0.125, 0.1875, 0.25, 0.3125, 0.375, 0.4375, 0.5):
        k, u = stretch_of(morph)
        w = (saw_node_weight(morph), invsaw_node_weight(morph), sine_node_weight(morph))
        good = abs(sum(w) - 1.0) < 1e-15
        if not good:
            ok |= 3
        say("    morph=%.4f stretch=%d w(saw,invSaw,sine)=(%.6f,%.6f,%.6f) sum=%.16f %s"
            % (morph, k, w[0], w[1], w[2], sum(w), "ok" if good else "*** NOT CONVEX ***"))

    # (c) the anti-phase midpoint is identically zero - exactly.
    say("  (c) morph=0.125 must be identically zero (the anti-phase midpoint):")
    r = ref_mix_saw(n0, count, f0, sr, 0.125)
    m = max_abs(r)
    good = m == 0.0
    if not good:
        ok |= 3
    say("    max|reference|=%.3g (exact zero required)  %s"
        % (m, "IDENTICALLY ZERO (ok)" if good else "*** NOT ZERO - ALGEBRA IS WRONG ***"))
    say("    NOTE: because it is zero, residual_stats() returns None there (its den==0 guard).")
    say("    That is measured as an equality, never as a ratio of two zero residuals.")

    # (d) STRUCTURE OF THE BASELINE RESIDUAL. A first draft of this block asserted "the naive
    # residual must exceed 20 dB wherever a full jump exists", which read -19 dB and failed. The
    # assertion was simply wrong, and the value it rejected is the right value: on stretch 0
    #
    #     naive = (1-u)*saw + u*(-saw) = (1-2u)*saw   and   ref = (1-2u)*bl_saw ,
    #
    # so the residual is (1-2u)*(saw - bl_saw) and res_db = 20*log10(||saw - bl_saw|| / ||bl_saw||)
    # CANNOT DEPEND ON u. The measured residual is the definitional Nyquist truncation of the saw
    # series (predicted 20*log10(||tail|| / ||kept||), which matches to within the fit's absorption),
    # not a defect and not a floor the correction has to beat. What is asserted here is that
    # structure, because it is exact, falsifiable and it is the property that makes the improvement
    # column - not this residual - the discriminating measurement.
    say("  (d) the baseline residual is the Nyquist truncation, and is u-independent on stretch 0:")
    vals = []
    for morph in (0.0, 0.0625, 0.1875, 0.25):
        k, u = stretch_of(morph)
        r = ref_mix_saw(n0, count, f0, sr, morph)
        st = residual_stats(naive_mix(n0, count, f0, sr, morph), r)
        vals.append((morph, st["res_db"]))
        say("    morph=%.4f (stretch %d) naive vs reference res_db=%7.3f  jump=%+.4f"
            % (morph, k, st["res_db"], 2.0 - 4.0 * u))
    spread = max(v for _, v in vals) - min(v for _, v in vals)
    good = spread < 1e-9
    if not good:
        ok |= 3
    say("    spread across stretch 0 = %.3g dB  %s"
        % (spread, "U-INDEPENDENT (ok)" if good else "*** REFERENCE DOES NOT SCALE WITH THE MIX ***"))
    say("    kmax=%d at sr=%.0f f0=%.0f; the reference is truncated there by definition, so this"
        % (kmax_for(f0, sr), sr, f0))
    say("    number is the reference's own Nyquist tail, shared by BOTH arms and not a floor.")

    say("  (d2) the two degenerate morphs, and the improvement cells' defect:")
    for morph in (0.125, 0.5):
        k, u = stretch_of(morph)
        r = ref_mix_saw(n0, count, f0, sr, morph)
        st = residual_stats(naive_mix(n0, count, f0, sr, morph), r)
        if morph == 0.125:
            good = (st is None) and (max_abs(r) == 0.0)
            if not good:
                ok |= 3
            say("    morph=0.125  reference identically zero -> residual UNDEFINED  %s"
                % ("DEGENERATE AS PREDICTED (ok)" if good else "*** UNEXPECTED ***"))
        else:
            good = (st is not None) and st["res_db"] < -80.0
            if not good:
                ok |= 3
            say("    morph=0.500  naive IS the sine node, res_db=%s  %s"
                % (("%7.2f" % st["res_db"]) if st and math.isfinite(st["res_db"]) else "   -inf",
                   "ALREADY SMOOTH - no improvement to demand (ok)" if good
                   else "*** NOT SMOOTH ***"))
    for morph in (0.0, 0.0625, 0.1875, 0.25, 0.3125, 0.375, 0.4375):
        k, u = stretch_of(morph)
        jump = (2.0 - 4.0 * u) if k == 0 else (2.0 * (1.0 - u))
        good = abs(jump) > 0.4
        if not good:
            ok |= 3
        say("    improvement cell morph=%.4f has |jump|=%.3f  %s"
            % (morph, abs(jump), "REAL DEFECT TO REMOVE (ok)" if good else "*** NO DEFECT ***"))

    say("  self-check %s" % ("PASSED" if ok == 0 else "*** FAILED (rc=%d) ***" % ok))
    return ok


def baseline_ladder(verbose=True):
    """The NAIVE baseline residual for every (sr, f0) in the approved grid, computed ANALYTICALLY
    (the ring's own closed forms vs the truncated series) rather than by rendering the product.

    Why this is worth having before the product is touched. The gate's improvement column is
    baseline_res_db - candidate_res_db, so this number is the baseline it will subtract. Computing
    it here by a route that shares no code with the render gives the gate an independent
    cross-check on its own baseline arm: if the rendered baseline arm's residual disagrees with
    this ladder for a pure-node cell, one of the two is wrong and neither verdict is trustworthy.

    It is also the answer to "is 6 dB available here at all". A perfect correction drives the
    residual to zero, so the demand is not bounded above by this number - but a cell whose baseline
    is already at the reference's own truncation tail has less of its residual attributable to the
    jump, and that is exactly the risk worth seeing before implementation."""
    grid = [(sr, f) for sr in (44100.0, 48000.0, 88200.0, 96000.0)
            for f in (220.0, 440.0, 880.0)]
    count, n0 = 4096, 8192
    if verbose:
        print("-- analytic naive-baseline ladder (pure saw node, morph=0) --")
        print("   %-8s %-7s %-6s %12s %14s" % ("sr", "f0", "kmax", "naive res_db", "6dB needs <="))
    rows = []
    for sr, f0 in grid:
        r = ref_mix_saw(n0, count, f0, sr, 0.0)
        st = residual_stats(naive_mix(n0, count, f0, sr, 0.0), r)
        db = st["res_db"]
        rows.append((sr, f0, kmax_for(f0, sr), db))
        if verbose:
            print("   %-8.0f %-7.0f %-6d %12.3f %14.3f"
                  % (sr, f0, kmax_for(f0, sr), db, db - 6.0))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description="GH#19 S6 saw/invSaw reference instrument")
    ap.add_argument("--arm", default=None, help="directory written by gh19_s6_saw_probe --out")
    ap.add_argument("--label", default="arm")
    ap.add_argument("--out", default=None)
    ap.add_argument("--self-check", action="store_true")
    ap.add_argument("--baseline-ladder", action="store_true",
                    help="analytic naive-baseline residual for every (sr, f0) in the grid")
    ap.add_argument("--kmax-cap", type=int, default=None,
                    help="DEV/SMOKE ONLY: truncate the reference series. A capped run is NOT a "
                         "full-band measurement and the report says so; the gate refuses it.")
    args = ap.parse_args(argv)

    rc = 0
    if args.self_check:
        rc |= self_check()
    if args.baseline_ladder:
        baseline_ladder()
    if args.arm is None:
        if not (args.self_check or args.baseline_ladder):
            sys.stderr.write("FATAL: nothing to do (pass --arm, --self-check and/or "
                             "--baseline-ladder)\n")
            return 2
        return rc
    sys.stderr.write("FATAL: the full-matrix route is not wired yet in this revision\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
