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
import contextlib
import importlib.util
import io
import math
import os
import re
import struct
import sys
from _gh19_textio import open_text

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
#
# THE RING'S NODE ORDER IS DATA HERE, NOT A CHOICE RESTATED AT EACH USE SITE.
#
# It is declared once, below, and BOTH the band-limited reference family and the closed-form naive
# model read the nodes of a stretch out of it. That is deliberate, and it is the fix for a real
# defect this instrument shipped and then caught: `ref_mix_saw` and `naive_mix` each independently
# hardcoded the SAW as the LOWER node of every stretch, which is right on stretch 0 (saw -> invSaw)
# and wrong on stretch 1 (INVSAW -> sine, invSawShape = -(2p-1)). Two consequences, both measured:
#   * the two functions agreed with each other on stretch 1, so neither could expose the other;
#   * every reference-family self-check collapsed at u = 1, where the lower node's weight is 0 and
#     the wrong node is invisible -- a degenerate identity, not a check.
# The arm alignment gate -- which reconstructs the product's OWN law and so is not party to that
# agreement -- is what surfaced it, on all 32 stretch-1 interior cells at once. Restating the node
# order at a use site is the mechanism; keeping it in one place removes the mechanism.
NODE_SAW, NODE_INVSAW, NODE_SINE = 0, 1, 2
NODE_ORDER = (NODE_SAW, NODE_INVSAW, NODE_SINE)   # == Node::kSaw, kInvSaw, kSine (vco_wave_map.h:102)


def stretch_nodes(k):
    """The (lower, upper) NODES of stretch k, taken from the ring's declared order above."""
    if k + 1 >= len(NODE_ORDER):
        raise ValueError("stretch %d is past this instrument's two covered stretches" % k)
    return NODE_ORDER[k], NODE_ORDER[k + 1]


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


def bl_node(node, n0, count, f0, sr, kmax_override=None):
    """The band-limited form of ONE node, selected by the node itself rather than by the caller."""
    if node == NODE_SAW:
        return bl_saw(n0, count, f0, sr, kmax_override)
    if node == NODE_INVSAW:
        return bl_inv_saw(n0, count, f0, sr, kmax_override)
    if node == NODE_SINE:
        return bl_sine(n0, count, f0, sr, kmax_override)
    raise ValueError("node %r is not one of the ring's first three" % (node,))


def exact_node(node, p):
    """The same node, evaluated by the product's OWN closed form (vco_wave_map.h sawShape /
    invSawShape / sineShape). No series and no truncation: this is what the current product emits."""
    if node == NODE_SAW:
        return 2.0 * p - 1.0
    if node == NODE_INVSAW:
        return -(2.0 * p - 1.0)
    if node == NODE_SINE:
        return math.sin(TWO_PI * p)
    raise ValueError("node %r is not one of the ring's first three" % (node,))


def ref_mix_saw(n0, count, f0, sr, morph, kmax_override=None):
    """The band-limited reference for a saw/invSaw stretch cell, built from THAT cell's own mix law
    and from the nodes `stretch_nodes` names for that stretch. Stretch 2 and above are refused rather
    than approximated: this instrument covers stretches 0 and 1, and a reference from the wrong
    stretch is the cross-family error S3 named."""
    k, u = stretch_of(morph)
    if k > 1:
        raise ValueError("morph=%.6f is on stretch %d; gh19_s6_saw_analyze covers stretches 0-1"
                         % (morph, k))
    lo, hi = stretch_nodes(k)
    a = bl_node(lo, n0, count, f0, sr, kmax_override)
    b = bl_node(hi, n0, count, f0, sr, kmax_override)
    if a is None or b is None:
        return None
    return [(1.0 - u) * x + u * y for x, y in zip(a, b)]


def naive_mix(n0, count, f0, sr, morph):
    """The ring's OWN non-band-limited law, evaluated from the product's closed forms (sawShape =
    2p-1, invSawShape = -(2p-1), sineShape = sin(2*pi*p); vco_wave_map.h:125-128). This is what the
    current product emits on stretches 0-1, and it is the arm the S6 criterion has to be able to
    reject: a metric that cannot see this defect cannot certify its removal. It is INDEPENDENT of
    ref_mix_saw (no series, no truncation), which is what makes it usable as the sensitivity probe
    in self-check (d) rather than a restatement of the reference."""
    k, u = stretch_of(morph)
    if k > 1:
        raise ValueError("morph=%.6f is not on stretch 0-1" % morph)
    lo, hi = stretch_nodes(k)
    out = []
    for i in range(count):
        p = phi_at(n0 + i, f0, sr)
        out.append((1.0 - u) * exact_node(lo, p) + u * exact_node(hi, p))
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

    # (a2) THE NODE TABLE vs THE WEIGHT HELPERS, AT INTERIOR MORPHS. (a) above collapses at u = 1,
    # where the lower node's weight is 0 and its identity cannot matter -- so (a) passes for a
    # reference built on the WRONG lower node, which is exactly the defect this instrument shipped.
    # This check is the non-degenerate one: it rebuilds each reference as
    #     sum_k  w_k(morph) * bl_node(k)
    # from the weight helpers, which encode the node roles by NAME and independently of
    # `stretch_nodes`. Two separate encodings of "which node is the lower one" must agree at a
    # morph where BOTH weights are nonzero. The wrong-node alternative is printed alongside, so the
    # check's discriminating power is visible and not merely asserted.
    say("  (a2) reference == sum of named-node references, at INTERIOR morphs (where (a) is blind):")
    for morph in (0.0625, 0.1875, 0.3125, 0.375, 0.4375):
        k, u = stretch_of(morph)
        r = ref_mix_saw(n0, count, f0, sr, morph)
        w = {NODE_SAW: saw_node_weight(morph), NODE_INVSAW: invsaw_node_weight(morph),
             NODE_SINE: sine_node_weight(morph)}
        bl = {nd: bl_node(nd, n0, count, f0, sr) for nd in w}
        acc = [0.0] * count
        for nd, wt in w.items():
            if wt:
                acc = [a + wt * b for a, b in zip(acc, bl[nd])]
        d = max_abs([x - y for x, y in zip(r, acc)])
        # The same reconstruction with the LOWER node replaced by the one node that is neither the
        # stretch's lower nor its upper -- for stretch 0 that is the sine, for stretch 1 the saw.
        # (Using the saw unconditionally would make this a no-op on stretch 0, where the saw IS the
        # correct lower node, and "the check did not fire" would be misread as "the check is sound";
        # the shipped defect was itself a wrong-node choice, so the perturbation must be a real one.)
        lo, hi = stretch_nodes(k)
        alt = (set(NODE_ORDER) - {lo, hi}).pop()
        wrong = dict(w)
        wrong[alt] = w[lo]
        wrong[lo] = 0.0
        acc_w = [0.0] * count
        for nd, wt in wrong.items():
            if wt:
                acc_w = [a + wt * b for a, b in zip(acc_w, bl[nd])]
        dw = max_abs([x - y for x, y in zip(r, acc_w)])
        good = d < 1e-12 and dw > 0.01
        if not good:
            ok |= 3
        say("    morph=%.4f lower=%-7s max|ref-sum| =%.3g   same check with %-7s as lower node: "
            "%.3g  %s" % (morph, {NODE_SAW: "saw", NODE_INVSAW: "invSaw",
                                  NODE_SINE: "sine"}[lo], d,
                          {NODE_SAW: "saw", NODE_INVSAW: "invSaw", NODE_SINE: "sine"}[alt], dw,
                          "AGREES, and the wrong node is REJECTED (ok)" if good
                          else "*** NOT A DISCRIMINATING CHECK ***"))

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


# ------------------------------------------------------------------------- S6 arm measurement (--arm)

# Pinned here and NOT imported from S3: the gate keeps its own literal copy of this tuple, so a change
# to the column order in this file becomes a REFUSAL in the gate rather than being absorbed silently.
MATRIX_COLS = ("id", "sr", "f0", "morph", "stretch", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "kmax", "max_abs_out")

# vco_a_sawmix_44100_220_m0 / vco_a_sawmixhi_44100_3520_m0. `sawmixhi` is FIRST in the alternation:
# `sawmix` is a prefix of it, so the other order would parse every high-frequency cell as a plain
# sawmix cell whose sr/f0/tag fields had swallowed a stray "hi".
CELL_RE = re.compile(r"^vco_(?P<side>[ab])_(?P<fam>sawmixhi|sawmix)_(?P<sr>\d+)_(?P<f0>\d+)_"
                     r"(?P<tag>m\d+)$")


def load_s6_manifest(d):
    """The S6 counterpart of S3's load_manifest. S3's is imported everywhere else in this file, but
    this one cannot be: it hardcodes the S3 manifest's FILENAME. The column set and the numeric
    coercions are identical, so the coercion list is copied verbatim rather than restated -- a
    manifest whose columns drifted then fails here loudly instead of being quietly re-typed."""
    path = os.path.join(d, "gh19_s6_scenarios.tsv")
    with open_text(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    head = lines[0].split("\t")
    rows = []
    for n, l in enumerate(lines[1:], 2):
        f = l.split("\t")
        if len(f) != len(head):
            raise ValueError("%s:%d: %d fields, header has %d" % (path, n, len(f), len(head)))
        r = dict(zip(head, f))
        for k in ("sr_hz", "f_target_hz", "duty_param", "morph", "pw_depth", "f0_meas_hz",
                  "phi0_model", "peak", "warm", "win", "samples"):
            r[k] = float(r[k])
        r["warm"] = int(r["warm"])
        r["win"] = int(r["win"])
        r["samples"] = int(r["samples"])
        rows.append(r)
    return rows


def measure_cell(c, kmax_cap=None):
    """The S6 analogue of S3's static-cell block, with the SAME call sequence and the same columns, so
    that a difference between an S3 number and an S6 number is attributable to the WAVEFORM FAMILY
    rather than to a different instrument.

    The reference is the cell's OWN mix law (`ref_mix_saw`), never a single-node series used on a
    mixed cell -- S3's named cross-family error. The two band columns and `res_db` share the same
    fitted scale A, one per cell, exactly as in S3; `res_effbd_db` is the Hann-windowed band residual
    over the EFFECTIVE full band, which for an analytic (non-decimated) reference is sr/2. Keeping
    the column even though the reference is never decimated is deliberate: the S3 gate's report rows
    name it, and a column that exists in one instrument and not the other would make the two
    instruments' reports un-diffable."""
    x = c.window()
    r = ref_mix_saw(c.warm, len(x), c.f0, c.sr, c.morph, kmax_cap)
    if r is None:
        return None
    fund, dc = S3.measure_fundamental_dc(x, c.warm, c.f0, c.sr)
    st = residual_stats(x, r)
    row = {"id": c.id, "sr": c.sr, "f0": c.f0, "morph": c.morph,
           "stretch": stretch_of(c.morph)[0], "fund": fund, "dc": dc,
           "kmax": kmax_for(c.f0, c.sr, kmax_cap), "max_abs_out": max_abs(x),
           "A": None, "res_db": None, "res_1k5k_db": None, "res_effbd_db": None}
    if st is not None:
        A = st["A"]
        b1 = S3.band_residual_db(x, r, A, c.sr, S3.B1_LO_HZ, S3.B1_HI_HZ)
        bf = S3.band_residual_db(x, r, A, c.sr, 0.0, c.sr / 2.0)
        row["A"] = A
        row["res_db"] = st["res_db"]
        row["res_1k5k_db"] = b1["db"] if b1 else None
        row["res_effbd_db"] = bf["db"] if bf else None
    return row


def align_check_s6(c, tol_rel=1e-6):
    """THE GATE THAT LICENSES EVERY NUMBER THE S6 ANALYZER PRINTS.

    Reconstruct the product's OWN pre-S6 law (`naive_mix` -- the ring's closed forms, no series, no
    truncation) and require the rendered arm to equal it, except inside the wrap window where a
    floor-limited correction is entitled to act. This pins the phase convention (S3's `n+1` rule), the
    stretch selection and the scale in one place: without it a residual could be small because the
    reference happened to line up with whatever the product did.

    THE SCALE IS A MEDIAN OF PER-SAMPLE RATIOS, NOT A LEAST-SQUARES FIT, and this is the same
    argument that made S3 take a median of |x| rather than a fit. A ramp spends every sample at a
    different magnitude, so no single |x| IS the amplitude; but x_i/m_i is a CONSTANT for a correctly
    scaled arm, so its median is that constant and is unmoved by the two anomalous wrap samples. A
    least-squares fit would let exactly those samples move the scale, after which every ordinary
    sample "disagrees" by that amount and the report would show a full window of mismatches -- the
    exact opposite of the truth. It applies to BOTH arms, which is what lets one check serve both.

    Mismatches are CLASSIFIED, not merely counted:
      - `wrap`: model phase within 1e-9 of 0 or 1. The C++ accumulator lands about one ulp below the
        integer at exact-wrap frames while the closed form returns exactly 0.0, so the two
        legitimately differ by one step there. Measured, not assumed.
      - `edge`: within dt = f0/sr of the wrap. This is the window a floor-limited candidate is
        ALLOWED to differ in, and therefore where the candidate arm's mismatches must live -- and
        where the baseline arm's must NOT, because the baseline has no correction to place there.
      - `unexplained`: everything else. Must be ZERO. This is the load-bearing number.

    morph = 0.125 IS A DEGENERATE CELL AND IS HANDLED AS ONE, not as a failure. There the ring's own
    law is identically zero (0.5*saw + 0.5*(-saw)), so there is no scale to recover and no ratio to
    take (`residual_stats` returns None for the same reason). The only alignment statement that still
    means anything is that the rendered output is zero too, and that is what is checked. Returning
    "no scale" instead would make the anti-phase-midpoint equality criterion unmeasurable by
    construction, which is the opposite of what that cell is in the matrix for.
    """
    x = c.window()
    n0 = c.warm
    m = naive_mix(n0, len(x), c.f0, c.sr, c.morph)
    dt = c.f0 / c.sr
    base = {"scale": 1.0, "n": len(x), "dt": dt, "wrap": 0, "edge": 0,
            "first_n": None, "first_i": None, "first_phase": 0.0, "first_err": 0.0,
            "degenerate": False}
    if max_abs(m) == 0.0:
        worst = max_abs(x)
        out = dict(base)
        out.update({"ok": worst == 0.0, "degenerate": True, "worst": worst,
                    "unexplained": 0 if worst == 0.0 else len(x)})
        return out
    ratios = sorted(x[i] / m[i] for i in range(len(x)) if m[i] != 0.0)
    scale = ratios[len(ratios) // 2]
    tol = tol_rel * abs(scale)
    wrap = edge = unexplained = 0
    worst = 0.0
    first_n = first_i = None
    first_phase = first_err = 0.0
    for i in range(len(x)):
        e = abs(x[i] - scale * m[i])
        if e > worst:
            worst = e
        if e <= tol:
            continue
        p = phi_at(n0 + i, c.f0, c.sr)
        near = min(abs(p), abs(p - 1.0))
        if near < 1e-9:
            wrap += 1
        elif near < dt:
            edge += 1
        else:
            unexplained += 1
            if first_n is None:
                first_n, first_i, first_phase, first_err = n0 + i, i, p, e
    out = dict(base)
    out.update({"ok": unexplained == 0, "scale": scale, "wrap": wrap, "edge": edge,
                "unexplained": unexplained, "worst": worst,
                "first_n": first_n, "first_i": first_i, "first_phase": first_phase,
                "first_err": first_err})
    return out


def parseval_check(sr=48000.0, f0=440.0, count=4096, n0=8192, verbose=True):
    """(e) THE POWER-UNIT TRIPWIRE, inherited from S3's self-check so the S6 gate can run the same
    check on the same terms. Two halves, because the first alone has a blind spot:

      * with a RECTANGULAR window, `band_residual_db` over [0, sr/2] must reproduce `res_db`
        (the unwindowed ratio, computed by `residual_stats` with no transform at all at any point);
      * for BOTH windows, each band power must equal a DIRECT time-domain mean square,
        sum_i (w_i y_i)^2 / sum_i w_i^2, computed with no transform. This is the half that pins the
        power UNITS: the first comparison divides two transform quantities by each other and is
        therefore completely blind to the 2/N coefficient that makes them physical.

    A metric whose units are wrong is not a metric, and a residual column printed in the wrong units
    would still look like a plausible dB number. Returns 0 if both halves held, else a bitmask, and
    prints the OK line the gate requires to see."""
    ok = 0
    x = naive_mix(n0, count, f0, sr, 0.1875)
    r = ref_mix_saw(n0, count, f0, sr, 0.1875)
    st = residual_stats(x, r)
    A = st["A"]
    rect = [1.0] * count
    for lbl, w in (("rect", rect), ("hann", S3.hann(count))):
        b = S3.band_residual_db(x, r, A, sr, 0.0, sr / 2.0, w)
        sw2 = sum(v * v for v in w)
        want_q = sum((A * r[i] * w[i]) ** 2 for i in range(count)) / sw2
        want_e = sum(((x[i] - A * r[i]) * w[i]) ** 2 for i in range(count)) / sw2
        good = (b is not None
                and abs(b["pq"] - want_q) <= 1e-12 * abs(want_q)
                and abs(b["pe"] - want_e) <= 1e-12 * max(abs(want_e), 1e-300))
        if not good:
            ok |= 4
        if verbose:
            print("    (e) Parseval absolute, %-4s window: band pq=%.12g vs direct %.12g  %s"
                  % (lbl, b["pq"] if b else float("nan"), want_q,
                     "MEAN SQUARES AGREE (ok)" if good else "*** UNITS WRONG ***"))
    b = S3.band_residual_db(x, r, A, sr, 0.0, sr / 2.0, rect)
    d = abs(b["db"] - st["res_db"])
    good = d <= 1e-12 * max(abs(st["res_db"]), 1.0)
    if not good:
        ok |= 4
    if verbose:
        print("    (e) Parseval ratio, rect window: band db=%.12f vs res_db=%.12f  max|diff|=%.3g  %s"
              % (b["db"], st["res_db"], d,
                 "BAND == RECT (ok)" if good else "*** BAND COLUMN IS NOT THE RECT RATIO ***"))
    if verbose:
        print("    (e) %s" % ("PASSED" if ok == 0 else "*** FAILED (rc=%d) ***" % ok))
    return ok


def _fmt(v):
    """One token per column, always. `nan` for a quantity that DOES NOT EXIST: the anti-phase
    midpoint's reference is identically zero, `residual_stats` returns None, and printing 0.0 or -inf
    there would both be claims the instrument is not entitled to make. A blank field would instead
    shift every later column left and the gate's fixed-width parse would read the wrong numbers as
    the right ones."""
    if v is None:
        return "nan"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, int):
        return str(v)
    return "%.17g" % v


def analyze_arm(armdir, label, kmax_cap=None, verbose=True):
    """Measure one rendered arm and return its matrix rows, or None if a cell has no reference. A
    cell without a reference is not skippable: the gate reconciles the matrix against the probe's
    declared plan, so a silently dropped cell would look like a smaller matrix rather than a
    broken run."""
    rows = []
    for row in load_s6_manifest(armdir):
        c = S3.Cell(row, armdir)
        meas = measure_cell(c, kmax_cap)
        if meas is None:
            sys.stderr.write("FATAL %s: no band-limited reference exists (kmax < 1)\n" % c.id)
            return None
        al = align_check_s6(c)
        meas["align"] = al
        rows.append(meas)
    nbad = sum(1 for r in rows if not r["align"]["ok"])
    if verbose:
        first = next((r["id"] for r in rows if not r["align"]["ok"]), "-")
        print("ALIGN-GATE ok=%d checked=%d bad=%d first_bad=%s"
              % (len(rows) - nbad, len(rows), nbad, first))
        for r in rows:
            if not r["align"]["ok"]:
                al = r["align"]
                print("  ALIGN-FAIL %s scale=%.12g wrap=%d edge=%d unexplained=%d worst=%.6g "
                      "first_n=%s first_phase=%s"
                      % (r["id"], al["scale"], al["wrap"], al["edge"], al["unexplained"],
                         al["worst"], al["first_n"], al["first_phase"]))
    if verbose:
        print("ARM-MEASURED arm=%s cells=%d" % (label, len(rows)))
    return rows


def render_report(rows, label, kmax_cap):
    """The matrix block, printed into the SAME artifact as the instrument lines on purpose: a matrix
    and a self-check taken from different runs would let a failed instrument be paired with a passing
    matrix. The gate reads one file per arm and requires both in it."""
    print("-- BEGIN MATRIX static")
    print("  Columns are inherited unchanged from S3's static block, with `stretch` and")
    print("  `max_abs_out` added: `stretch` says which segment of the morph ring the cell sits on")
    print("  (the reference must come from THAT stretch's own mix law), and `max_abs_out` is the")
    print("  quantity the anti-phase-midpoint equality criterion is stated on.")
    print("  Columns are printed to 17 significant digits: the two arms' matrices are compared cell")
    print("  by cell, and a rounded column would make 'the arms agree' a statement about the")
    print("  rounding rather than about the DSP.")
    print("")
    print(" ".join(MATRIX_COLS))
    for r in rows:
        print(" ".join([r["id"]] + [_fmt(r[k]) for k in MATRIX_COLS[1:]]))
    print("-- END MATRIX static declared=%d emitted=%d skipped=%d capped=%d"
          % (len(rows), len(rows), 0, 1 if kmax_cap is not None else 0))


def _run(args):
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

    print("-- GH#19 S6 (task #120) saw/invSaw reference instrument --")
    print("ARM %s" % args.label)
    print("REFERENCE-FAMILY every reference is built from the cell's OWN stretch mix law; "
          "kmax = floor((sr/2)/f0); no cross-stretch and no cross-family borrowing.")
    print("BAND-DECL b1_lo=%.3f b1_hi=%.3f eff_lo=%.3f eff_rule=analytic eff_hi_cycles=%.9f"
          % (S3.B1_LO_HZ, S3.B1_HI_HZ, 0.0, 0.5))
    if args.kmax_cap is not None:
        print("CAPPED kmax_cap=%d : the reference series was truncated below Nyquist, so this is "
              "NOT a full-band measurement." % args.kmax_cap)

    # The (e) tripwire and the alignment gate run UNCONDITIONALLY on the arm route rather than behind
    # switches: the gate refuses a report lacking either, and a report missing them because a caller
    # forgot a flag is indistinguishable from an instrument that never ran.
    rc |= parseval_check()
    rows = analyze_arm(args.arm, args.label, args.kmax_cap)
    if rows is None:
        return 3
    nbad = sum(1 for r in rows if not r["align"]["ok"])
    render_report(rows, args.label, args.kmax_cap)
    if rc != 0 or nbad:
        # An arm whose output is not the model it claims to be is not measurable: every residual below
        # it would be a statement about a different signal. The marker also makes the gate REFUSE
        # rather than judge, which is the point -- this is not a red verdict, it is no verdict.
        print("*** INSTRUMENT self_check_rc=%d align_bad=%d: the instrument or the rendered arm "
              "failed its own gate, so no number below it is a statement about the DSP." % (rc, nbad))
        return 1
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="GH#19 S6 saw/invSaw reference instrument")
    ap.add_argument("--arm", default=None, help="directory written by gh19_s6_saw_probe --out")
    ap.add_argument("--label", default="arm")
    ap.add_argument("--out", default=None, help="report file for --arm (the gate reads this)")
    ap.add_argument("--self-check", action="store_true")
    ap.add_argument("--baseline-ladder", action="store_true",
                    help="analytic naive-baseline residual for every (sr, f0) in the grid")
    ap.add_argument("--kmax-cap", type=int, default=None,
                    help="DEV/SMOKE ONLY: truncate the reference series. A capped run is NOT a "
                         "full-band measurement and the report says so; the gate refuses it.")
    args = ap.parse_args(argv)

    # Everything the run says is captured and then emitted twice -- once to the console, once to the
    # report file -- so the file the gate reads cannot be missing an instrument line that the console
    # showed, and vice versa.
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = _run(args)
    text = buf.getvalue()
    sys.stdout.write(text)
    if args.out and args.arm is not None:
        with open_text(args.out, "w") as fh:
            fh.write(text)
    return rc


if __name__ == "__main__":
    sys.exit(main())
