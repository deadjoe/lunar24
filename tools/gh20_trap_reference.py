#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh20_trap_reference.py — task #88 (GH #20) independent bilinear SVF acceptance reference.
#
# @Codex production contract (msg 8320798f): the product's numeric acceptance oracle is an
# INDEPENDENT reference — the same-damp continuous 2nd-order prototype transformed by the bilinear
# (matched) mapping — NOT a re-derivation of the product's own recursion. The reference is:
#
#   k   = damp = 2 - 1.9 * res                 (the SAME res->damp map, preserved)
#   r   = tan(pi * f_test / sr) / tan(pi * fc / sr)
#   LP  = 1 / (1 - r^2 + j k r)                (mag in dB = -10*log10((1-r^2)^2 + (k r)^2))
#   BP  = j r / (1 - r^2 + j k r)              (mag     = r / |1 - r^2 + j k r|)
#
# where fc is the product's EFFECTIVE cutoff: fc = 20 * 1000^norm, capped at min(20000, 0.49*sr),
# floored at 20 Hz (the identical effective-cap the new product applies — read back from
# polivoks_vcf.h cutoffCapHz_). This is the same-damp prototype: LP DC gain = 1 (0 dB) INDEPENDENT of
# res (the Polivoks no-low-loss property the fix must preserve), and BP is NOT peak-normalised.
#
# This module is written to be the SINGLE numeric oracle source of truth for the acceptance gate
# (run_gh20_vcf_gate.py / gh20_vcf_analyze.py --reference) and for the negative-control runner: it
# must produce the exact reference magnitude the product should match. It is deliberately free of any
# product state/coefficient — the only shared inputs are (sr, norm, res, mode, f_test), which are the
# measured-cell parameters, plus the two PROVISIONAL software constants (kFreqMinHz, kFreqMaxHz, and
# the 0.49 safety policy) that define the norm->Hz map. If those constants change, the reference here
# MUST be updated in lockstep (they are the map, not the filter).
#
# It also provides the analytic difference between two pole variants when a directional "RED on old"
# check is needed (the Chamberlin sr/8-capped response is itself a function of the SAME map, not a
# separate oracle) — but the PRIMARY acceptance is the reference above.
#
# Reproduction: python3 -c "import gh20_trap_reference as r; ..."  (it is a library, plus a demo
#   self-test under __main__ that prints a few reference magnitudes.)
#
# NOTE: the magnitude conventions here are the REAL magnitude (not dB) for a unit-amplitude steady
#   sinusoid, so a product probe reporting amp_rms of a unit sine must be compared as
#   20*log10(measured_amp / reference_mag) — see the analyzer.

import math

# The norm->Hz cutoff map constants (PROVISIONAL software policy, NOT filter theorems). These MUST be
# kept in lockstep with polivoks_vcf.h; they define the knob range and the safety cap, nothing else.
K_FREQ_MIN_HZ = 20.0
K_FREQ_MAX_HZ = 20000.0
K_CUTOFF_CAP_RATIO_SOFT = 0.49      # min(20000, 0.49*sr) software safety policy (not a stability theorem).

# res -> damp map (SAME map preserved from the Chamberlin; k = damp, not 1/Q).
K_DAMP_MAX = 2.0     # res = 0  -> flat / no resonance.
K_DAMP_MIN = 0.1     # res = 1  -> max resonance (kept > 0 -> no self-oscillation).


def damp_for_res(res):
    """damp = 2 - 1.9 * res. Clamp res to [0, 1] first (the product clamps knob-set res)."""
    r = 0.0 if res < 0.0 else (1.0 if res > 1.0 else res)
    return K_DAMP_MAX + (K_DAMP_MIN - K_DAMP_MAX) * r


def cutoff_hz(norm, sr):
    """The effective cutoff applied by the product: 20 * 1000^norm capped at min(20000, 0.49*sr),
    floored at 20 Hz. Mirrors polivoks_vcf.h cutoffCapHz_ / baseFreqHz_ (ZERO-CV, mod=0)."""
    fc = K_FREQ_MIN_HZ * (K_FREQ_MAX_HZ / K_FREQ_MIN_HZ) ** norm
    cap = sr * K_CUTOFF_CAP_RATIO_SOFT
    if cap < K_FREQ_MIN_HZ:
        cap = K_FREQ_MIN_HZ
    if cap > K_FREQ_MAX_HZ:
        cap = K_FREQ_MAX_HZ
    if fc > cap:
        fc = cap
    if fc < K_FREQ_MIN_HZ:
        fc = K_FREQ_MIN_HZ
    return fc


def r_param(f_test, fc, sr):
    """r = tan(pi * f_test / sr) / tan(pi * fc / sr)."""
    if fc <= 0.0 or sr <= 0.0:
        raise ValueError("fc and sr must be positive")
    num = math.tan(math.pi * f_test / sr)
    den = math.tan(math.pi * fc / sr)
    return num / den


def reference_mag(f_test, norm, sr, res, mode):
    """Reference magnitude for a unit steady sinusoid at f_test.
    mode: 'lp' -> LP = 1/(1 - r^2 + j k r);  'bp' -> BP = j r / (1 - r^2 + j k r).
    Returns the real magnitude |H| (not dB). The LP magnitude at f_test -> 0 is 1 (DC gain 1), and is
    INDEPENDENT of res (the Polivoks no-low-loss property). BP is NOT peak-normalised."""
    fc = cutoff_hz(norm, sr)
    r = r_param(f_test, fc, sr)
    k = damp_for_res(res)
    denom2 = (1.0 - r * r) ** 2 + (k * r) ** 2
    denom = math.sqrt(denom2) if denom2 > 0 else 1e-300
    if mode == "lp":
        return 1.0 / denom
    if mode == "bp":
        return abs(r) / denom
    raise ValueError("mode must be 'lp' or 'bp', got %r" % (mode,))


def reference_mag_db(f_test, norm, sr, res, mode):
    m = reference_mag(f_test, norm, sr, res, mode)
    return 20.0 * math.log10(m) if m > 0 else -float("inf")


def prewarp_sanity(norm, sr):
    """The product must keep tan(pi*fc/sr) finite -> fc/sr strictly < 0.5. The safety cap 0.49
    guarantees fc/sr <= 0.49 < 0.5 for the direct norm; CV shifts are independently capped at the same
    cutoffCapHz_. Returns (fc, fc_over_sr, finite)."""
    fc = cutoff_hz(norm, sr)
    over = fc / sr if sr > 0 else 0.0
    return fc, over, over < 0.5


if __name__ == "__main__":
    # Demo: print the LP/BP reference magnitude in dB across a couple of representative cells so a
    # human can eyeball the oracle. Not part of any gate.
    print("demo: independent bilinear reference (dB), res sweep at norm=1, sr=44100")
    for res in (0.0, 0.5, 1.0):
        for freq in (100.0, 1000.0, 8000.0, 20000.0):
            lp = reference_mag_db(freq, 1.0, 44100.0, res, "lp")
            bp = reference_mag_db(freq, 1.0, 44100.0, res, "bp")
            print("  f=%6.0f  res=%.1f  LP=%.3f dB  BP=%.3f dB" % (freq, res, lp, bp))
    fc, over, finite = prewarp_sanity(1.0, 44100.0)
    print("  norm=1 sr=44100 -> fc=%.2f Hz  fc/sr=%.4f  finite=%s" % (fc, over, finite))
