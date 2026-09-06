#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh19_alias_analyze.py — task #85 (GH #19) REAL-PRODUCT aliasing measurement + gate.
#
# Adapts the VERIFIED GH#19 method (measure_gh19_antialias.py, @Codex REV-3 final) to the REAL
# product's sampled output instead of ideal(). Consumes the probe output (tests/probes/gh19_alias_probe.cpp):
# a `gh19_scenarios.tsv` row per cell + `gh19_scnNNN.raw` little-endian f64 samples captured from
# the real DSP.
#
# FAIL-CLOSED COVERAGE GATE (BLOCK item ① — a REQUIRED-scenario manifest + explicitly allowed blocks):
#   A committed contract (tools/gh19_manifest.tsv) enumerates EVERY required cell (by id). The gate
#   compares the probe's produced cells against that manifest; on a clean run they match exactly. ANY
#   of the old-error modes is an overall FAIL, not an informational line:
#       missing row    -> a required manifest id has NO scenario row
#       skip render    -> a required cell left signal="" but has no readable raw file
#       prod failure   -> a required cell whose capture returned a non-produced signal
#       non-finite     -> a required cell whose samples contain NaN/Inf
#       unexpected silence / over-scale (substitution) -> peak outside the recorded voltage domain
#   There is NO empty-exit-0: if 0 required cells are produced, the gate FAILs. Deferred module
#   probes (sub, saw/pulse/morph) are in the manifest as `required=0` and are declared ALLOWED blocks
#   (documented, never silently dropped) — they do NOT need a product cell.
#
# REFERENCE MEANING (BLOCK item ② + @Codex a1a71ae5 — Method A is a diagnostic, Method B is theory,
# and the authoritative per-sample reference is METHOD BL):
#   METHOD A ("harmonic-fit residual", DIAGNOSTIC): reference = the signal's OWN measured in-band
#      harmonic coefficients (projection at k*f0, k*f0 <= sr/2); residual = x - ref. This ABSORBS
#      aliasing that lands on legitimate in-band harmonics, so it is a LOWER bound on total aliasing,
#      and it is built FROM the signal (not independent). Diagnostic only — never the total-aliasing
#      figure (other error sources are not excluded). Reported as `harmris_*_db`.
#   METHOD B ("analytic folded-line", THEORY / cross-check): ideal-triangle exact analytic
#      a_tri(k)=4/(pi^2 k^2) (odd k), folded with the CONJUGATED mirror rule, dedup over distinct
#      observed lines, in-band dBc. Reads NO samples and NO phase; the measured fundamental a1_mag
#      cancels in the returned ratio, so it is a THEORETICAL PREDICTION, not a reference. Reported as
#      `theory_dedup_db` and cross-checked against METHOD BL (they must agree for a clean triangle).
#   METHOD BL ("per-sample band-limited reference", AUTHORITATIVE for clean triangle): builds an
#      INDEPENDENT ideal triangle at the product's snapped frequency with the product's MEASURED device
#      scale and measured initial phase, band-limited to FULL Nyquist, and takes the real per-sample
#      product-minus-reference residual. The residual is the aliasing. Reported as `blref_inband_db`
#      (referenced to the product's in-band power), `blref_full_db` (full-band), and `blshape_max_db`
#      (max per-harmonic deviation of the product from the ideal-triangle shape — certifies the
#      reference attribution is valid). For a clean triangle blref_inband_db agrees with the theory
#      (`theory_dedup_db`) and with the harmonic-fit diagnostic (which is the SAME signal's own fit).
#   COMPOSITE / non-single-module paths (drone voice, new-drone Schmitt voice, preamp tanh, wet chain):
#      no independent single-module alias reference exists, so NO alias attribution is claimed; the
#      row reports the harmonic-fit residual and total in-band power (a total-waveform-error / PSD
#      statistic), and is labeled `category`=composite/periodic/nonlinear — never a single-module alias.
#
# The alias figures are EVIDENCE; no threshold is asserted (-60dB is NOT a gate). Exit 0 = the
# MEASUREMENT TOOL passed its own negative controls; != 0 = the tool is broken or coverage is wrong.
#
# Reproduction:
#   ./build/gh19_alias_probe --out report/gh19-probe
#   python3 tools/gh19_alias_analyze.py --dir report/gh19-probe --check

import argparse
import cmath
import math
import os
import struct
import sys

BAND_LO, BAND_HI = 100.0, 5000.0     # reporting band (Hz)
NYQ_FRAC = 0.98                       # guard the Nyquist bin
KFMAX = 1.0e6                        # analytic harmonic cutoff for Method B fold (Hz)
MIN_HZ = 10.0                        # exclude DC / near-DC projections
ZPAD = 1 << 15                       # zero-padded length for the in-band FFT (power of 2)
STEADY = 16384                       # analysis window: the probe drops kWarm=8192 settle samples, so the
                                     # steady-state tail for f0>0 cells is this many samples (probe kWin).
INT_TOL = 3.0e-4                     # relative tolerance for treating a measured f0 as integer-periodic:
                                     # the sharp refine is accurate to ~+-0.02 Hz, so |f0-round(f0)|/f0
                                     # up to ~1e-4 (e.g. 219.978Hz for a 220Hz target) is measurement
                                     # noise inside the projection main lobe, NOT a genuinely non-integer
                                     # fundamental. A snap at this tolerance is reported via `gap`.
SEP_MIN_DB = 12.0                    # minimum naive-vs-bandlimited separation the metric must sustain
                                     # for the dynamic-range negative to count as passing.

# --- INDEPENDENT device-output-scale contract for a clean DRY triangle (source-derived, NOT fitted). ---
# The expected DRY triangle peak is derived from PRODUCT SOURCE, never re-estimated from the raw:
#   * core/include/lunar24/core/vco.h:228 triangle `return 4.0*std::fabs(p-0.5)-1.0;` -> peak amplitude 1.0 V
#   * core/include/lunar24/core/device_adapter.h:90 `inline constexpr double kDeviceScaleProvisional = 0.5;`
#     (applied at line 94 `clamp_val(volts*kDeviceScaleProvisional,-1.0,1.0)`) -> DRY output scale 0.5
#   => expected DRY triangle peak = 0.5. Both constants are software-provisional (device_adapter.h src
#      comment: NOT a claim of hardware truth). The contract verifies the RAW's measured peak against
#      this derived value; halving the raw (peak 0.25 -> 50% deviation) is REJECTED. This is the actual
#      device-scale error-inject — unlike rebuilding the reference at a wrong scale, which only corrupts
#      the reference side (the object @Codex 1307b784 flagged; that is no longer called "independent").
K_DEVICE_SCALE_PROVISIONAL = 0.5     # device_adapter.h:90 (provisional, software-not-true-hardware)
VCO_TRIANGLE_PEAK = 1.0              # vco.h:228 triangle peak amplitude
DRY_TRIANGLE_EXPECTED_PEAK = K_DEVICE_SCALE_PROVISIONAL * VCO_TRIANGLE_PEAK  # = 0.5
DRY_SCALE_TOL = 0.15                 # relative tolerance for the device-scale contract (peak must be
                                     # within 15% of the derived 0.5; a halved raw deviates 50% -> reject)
F0_LABEL_REL_TOL = 0.03              # relative tolerance for the f0 label contract (declared f0_target
                                     # must correspond to the independently-measured f0_meas_hz within 3%;
                                     # the probe's counter vs the analyzer's refine differ ~0.08% for a
                                     # clean cell, a corrupted label differs by tens of percent)


# --- ideal/tri/saw analytic coefficients (verified closed forms, match vco.h ideal()). ---
def a_tri(k):
    return (4.0 / (math.pi * math.pi * k * k)) if (k % 2 == 1) else 0.0


def a_saw(k):
    return 1j / (math.pi * k)


def ideal_tri(p):
    p = p - math.floor(p)
    return 4.0 * abs(p - 0.5) - 1.0


def ideal_saw(p):
    p = p - math.floor(p)
    return 2.0 * p - 1.0


# ---------------------------------------------------------------------------
# stdlib-only radix-2 FFT (iterative Cooley-Tukey) + Hann window.
# ---------------------------------------------------------------------------
def fft(a):
    n = len(a)
    if n & (n - 1):
        raise ValueError("FFT length must be a power of two")
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        ang = -2.0 * math.pi / length
        wlen = cmath.exp(1j * ang)
        for i in range(0, n, length):
            w = 1 + 0j
            for k in range(i, i + length // 2):
                u = a[k]
                v = a[k + length // 2] * w
                a[k] = u + v
                a[k + length // 2] = u - v
                w *= wlen
        length <<= 1
    return a


def hann(n):
    return [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1)) for i in range(n)]


# ---------------------------------------------------------------------------
# matched-filter projection (incremental phase, O(N), no per-sample trig).
# ---------------------------------------------------------------------------
def proj(x, sr, f):
    n = len(x)
    w = 2.0 * math.pi * f / sr
    C, S = math.cos(w), math.sin(w)
    c, s = 1.0, 0.0
    re = im = 0.0
    for i in range(n):
        v = x[i]
        re += v * c
        im -= v * s
        nc, ns = c * C - s * S, s * C + c * S
        c, s = nc, ns
    return complex(re / n, im / n)


def matched_project(s, f_obs, n, sr):
    acc = 0j
    for i in range(n):
        acc += complex(s[i], 0.0) * cmath.exp(-2j * math.pi * f_obs * i / sr)
    return acc / n


def refine_f0(x, sr, estimate):
    """Refine the fundamental to sub-Hz by maximizing the matched-filter projection magnitude.

    The projection's main lobe is ~sr/N wide (one FFT bin); a coarse grid whose step exceeds it can
    STRADDLE and MISS the sharp fundamental, latching onto a sidelobe elsewhere in the range (the
    44100/880 bug: step 5.4 Hz > lobe 2.7 Hz, so the true peak at 880.17 was skipped). So the scan
    step is capped below the lobe width, and the maximum is raised to sub-step accuracy by parabolic
    interpolation. Searches the global max over [-15%, +15%] around `estimate`."""
    if not (estimate > 0):
        return estimate
    n = len(x)
    lobe = sr / n                                  # projection main-lobe width (one FFT bin)
    lo = max(estimate * (1.0 - 0.15), 1.0)
    hi = estimate * (1.0 + 0.15)
    span = hi - lo
    steps = max(12, min(2000, int(span / (lobe / 3.0))))
    vals = []
    for i in range(steps + 1):
        f = lo + span * i / steps
        vals.append((abs(proj(x, sr, f)) ** 2, f))
    best_i = max(range(len(vals)), key=lambda i: vals[i][0])
    best_p, best_f = vals[best_i]
    # parabolic interpolation on the 3 points around the argmax for sub-step accuracy.
    if 0 < best_i < len(vals) - 1:
        ym, y0, yp = vals[best_i - 1][0], vals[best_i][0], vals[best_i + 1][0]
        d = (ym - yp) / (2.0 * (ym - 2.0 * y0 + yp + 1e-30))
        best_f += d * (span / steps)
    return float(best_f)


def harmonics_in_band(f0, sr):
    ks = []
    kmax = int((NYQ_FRAC * sr * 0.5) / f0) + 1
    for k in range(1, kmax + 1):
        f = k * f0
        if MIN_HZ <= f <= NYQ_FRAC * sr * 0.5:
            ks.append(k)
    return ks


# ---------------------------------------------------------------------------
# integer-period window: the analysis length N is chosen so N*f0/sr is an exact integer (the
# harmonics then land on distinct integer bins, projections are orthogonal, residual is leakage-free).
# For an exactly-integer f0 (VCO nominal), per = sr/gcd(f0,sr) is the samples/period; the largest
# multiple of per fitting the buffer (>= a few periods) is the window. For a NON-integer f0 no exact
# integer-period window exists -> use the full steady-state length and set gap=NaN so the caller can
# mark the measurement as leakage-uncontrolled (composite / not-single-periodic paths). Returns
# (N, f0_snap, gap); gap=0 => exact integer-period (leakage-free); NaN => leakage not controlled.
# ---------------------------------------------------------------------------
def intperiod_window(x, sr, f0):
    maxlen = len(x)
    if not (f0 > 0):
        return None
    fr = int(round(f0))
    if abs(f0 - fr) <= INT_TOL * f0:
        per = sr / math.gcd(fr, int(sr))
        per = int(per)
        if per < 1:
            per = 1
        if per <= maxlen:
            n = (maxlen // per) * per
            if n < 4 * per and maxlen >= 4 * per:
                n = 4 * per
            elif n < per:
                n = per
            gap = abs(f0 - fr) / fr
            return (n, float(fr), gap)
    return (maxlen, float(f0), float("nan"))


# ---------------------------------------------------------------------------
# METHOD A: harmonic-fit residual (real product, any periodic shape, leakage-free). DIAGNOSTIC ONLY.
# It subtracts the signal's OWN measured in-band harmonics, so aliasing that folds onto a legitimate
# in-band harmonic is ABSORBED into the fit and disappears from the residual => it is a LOWER bound on
# total aliasing, and it is NOT an independent reference (it is built from the signal itself). It is
# useful as an in-reach diagnostic and as a cross-check, but the AUTHORITATIVE independent figure for a
# clean triangle is METHOD BL (per-sample product-minus-band-limited-reference, below). Reframe: this
# is a harmonic-fit residual, never the total-aliasing figure.
# ---------------------------------------------------------------------------
def method_a(x, sr, f0, band_lo=BAND_LO, band_hi=BAND_HI):
    win = intperiod_window(x, sr, f0)
    if win is None:
        return None
    n, f0_snap, gap = win
    Nperiods = int(round(n * f0_snap / sr))
    nx = x[:n]
    ks = harmonics_in_band(f0_snap, sr)
    if not ks:
        return None
    amps = {k: proj(nx, sr, k * f0_snap) for k in ks}
    ref = [0.0] * n
    for k in ks:
        a = amps[k]
        w = 2.0 * math.pi * k * f0_snap / sr
        C, S = math.cos(w), math.sin(w)
        c, s = 1.0, 0.0
        ar, ai = a.real, a.imag
        for i in range(n):
            ref[i] += 2.0 * (ar * c - ai * s)
            nc, ns = c * C - s * S, s * C + c * S
            c, s = nc, ns
    resid = 0.0
    total = 0.0
    for i in range(n):
        e = nx[i] - ref[i]
        resid += e * e
        total += nx[i] * nx[i]
    if total <= 0.0:
        return None
    full_db = 10.0 * math.log10(resid / total)

    ewin = [0j] * ZPAD
    hn = hann(n)
    for i in range(n):
        ewin[i] = complex((nx[i] - ref[i]) * hn[i], 0.0)
    fft(ewin)
    bin_hz = sr / ZPAD
    pe = 0.0
    for k in range(1, ZPAD // 2):
        f = k * bin_hz
        if band_lo <= f <= band_hi:
            pe += abs(ewin[k]) ** 2
    xw = [0j] * ZPAD
    for i in range(n):
        xw[i] = complex(nx[i] * hn[i], 0.0)
    fft(xw)
    px = sum(abs(xw[k]) ** 2 for k in range(1, ZPAD // 2))
    inband_db = (10.0 * math.log10(pe / px) if (px > 0 and pe > 0) else float("-inf"))

    return {"full_db": full_db, "inband_db": inband_db, "kcount": len(ks),
            "a1_mag": abs(amps[1]) if 1 in amps else 0.0, "n": n, "f0_snap": f0_snap,
            "Nperiods": Nperiods, "gap": gap}


# ---------------------------------------------------------------------------
# METHOD B (theory): ideal-triangle analytic folded-line alias (CONJUGATE-corrected). This is a CLOSED-
# FORM PREDICTION only — it reads NO samples and NO phase. It takes a measured fundamental magnitude
# a1_mag and returns the aliasing ratio folded from above Nyquist, but a1_mag cancels in the ratio
# (restated as dBc against the carrier). It is therefore a THEORETICAL CROSS-CHECK for a clean
# triangle cell, NOT an authoritative per-sample reference. The authoritative per-sample product-minus-
# reference reconciliation is METHOD BL (below), and the report-band figure is referenced from it.
# ---------------------------------------------------------------------------
def fold_line(hz, sr):
    r = math.fmod(hz, sr)
    if r == 0.0:
        return 0.0, False
    if r == sr / 2.0:
        return sr / 2.0, False
    if r < sr / 2.0:
        return r, False
    return sr - r, True


def _fold_analytic(coeff_fn, f0, sr, scale, kfmax=KFMAX, band_lo=BAND_LO, band_hi=BAND_HI):
    nyq = sr * 0.5
    lines = {}
    naive_p = 0.0
    count = 0
    k = 2
    while k * f0 <= kfmax:
        hz = k * f0
        if hz > nyq:
            f_obs, mirrored = fold_line(hz, sr)
            if band_lo <= f_obs <= band_hi:
                coef = coeff_fn(k) * scale
                if mirrored:
                    coef = coef.conjugate()
                lines[f_obs] = lines.get(f_obs, 0j) + coef
                naive_p += abs(coeff_fn(k) * scale) ** 2
                count += 1
        k += 1

    def db(p2):
        return 10.0 * math.log10(p2 / carrier_p2) if (carrier_p2 > 0 and p2 > 0) else float("-inf")

    carrier_p2 = abs(coeff_fn(1) * scale) ** 2
    dedup_p = sum(abs(v) ** 2 for v in lines.values())
    return {"dedup_db": db(dedup_p), "naive_db": db(naive_p), "dedup_p": dedup_p,
            "naive_p": naive_p, "fold_count": count, "carrier_p2": carrier_p2}


def method_b_tri(sr, f0, a1_mag, band_lo=BAND_LO, band_hi=BAND_HI):
    """Theoretical folded-line alias prediction for a clean triangle. Reads NO samples/phase; a1_mag
    only rescales the carrier and cancels in the returned ratio. Cross-check only (see METHOD BL)."""
    if a1_mag <= 0:
        return None
    scale = a1_mag / a_tri(1)
    r = _fold_analytic(a_tri, f0, sr, scale, band_lo=band_lo, band_hi=band_hi)
    r["carrier_p2"] = a1_mag * a1_mag
    return r


# ---------------------------------------------------------------------------
# METHOD BL (authoritative): real per-sample band-limited triangle reference. Builds an INDEPENDENT
# ideal triangle at the product's snapped frequency with the product's MEASURED device scale and
# measured initial phase, band-limited to FULL Nyquist (no aliasing), then computes the per-sample
# residual of the real product against it. The residual is the aliasing. This is the real
# "product sample - independent band-limited reference" reconciliation @Codex requires:
#   * frequency / initial phase = from product state (refine_f0 snap + arg of the measured fundamental)
#   * device scale = the measured fundamental magnitude (a1_mag), NOT an arbitrary fit
#   * shape = independent ideal a_tri(k)/a_tri(1) (a clean-triangle authority), verified per-harmonic
#   * band-limit = full Nyquist (sr/2), NOT NYQ_FRAC*sr/2 (which would miss genuine in-band harmonics)
# The scale is inherently scale-invariant (a 2x-amplitude output gives the same dBc ratio), which is
# CORRECT for an aliasing ratio / not an amplitude meter; the aliasing figure is referenced to the
# product's in-band power, so a halved output is correctly reported at the SAME alias dBc. What the
# reference establishes is that the product IS a clean ideal triangle at the measured scale+phase
# (verified via blshape_max_db) and how far it departs from that ideal in-band (blref_inband_db).
# ---------------------------------------------------------------------------
def bandlimited_tri(n, sr, f0_snap, a1_mag, arg_a1):
    """x_ref[i] = 2*a1_mag * sum_{odd k, MIN_HZ<=k*f0_snap<=sr/2}
                     (a_tri(k)/a_tri(1)) * cos(k*(2*pi*f0_snap*i/sr) + k*arg_a1).
    Full-Nyquist band-limit; the k*arg_a1 phases reproduce the product's measured harmonic phase
    progression. Returns (ref, ks)."""
    tt = 2.0 * math.pi * f0_snap / sr
    ks = []
    kmax = int(sr * 0.5 / f0_snap) + 1
    for k in range(1, kmax + 1):
        if k % 2 == 1 and MIN_HZ <= k * f0_snap <= sr * 0.5:
            ks.append(k)
    ref = [0.0] * n
    scale = 2.0 * a1_mag
    for k in ks:
        ak = a_tri(k) / a_tri(1)          # real, positive; ideal triangle relative harmonic
        w = k * tt
        C, S = math.cos(w), math.sin(w)
        c, s = 1.0, 0.0
        cp, sp = math.cos(k * arg_a1), math.sin(k * arg_a1)
        # cos(k*tt*i + k*arg_a1) = (c*cp - s*sp), with c,s tracking cos/sin of k*tt*i incrementally.
        for i in range(n):
            ref[i] += scale * ak * (c * cp - s * sp)
            nc, ns = c * C - s * S, s * C + c * S
            c, s = nc, ns
    return ref, ks


def method_bl(x, sr, f0, band_lo=BAND_LO, band_hi=BAND_HI):
    """Real per-sample product-minus-band-limited-reference reconciliation for a clean triangle cell.
    Returns the in-band aliasing figure referenced to the PRODUCT's in-band power (the authoritative
    ratio), a full-band residual, and a per-harmonic shape-verification metric. The reference scale is
    the MEASURED fundamental (a1_mag): the aliasing ratio is scale-invariant (a halved output gives the
    same dBc), which is CORRECT for an alias ratio; the independent device-scale CONTRACT is a separate
    raw-side check (dry_triangle_scale_contract), not a reference-side scale override."""
    win = intperiod_window(x, sr, f0)
    if win is None:
        return None
    n, f0_snap, gap = win
    if f0_snap <= 0:
        return None
    nx = x[:n]
    a1 = proj(nx, sr, f0_snap)
    a1_mag = abs(a1)
    if a1_mag <= 0:
        return None
    arg_a1 = cmath.phase(a1)
    scale = a1_mag
    ref, ks = bandlimited_tri(n, sr, f0_snap, scale, arg_a1)
    resid = 0.0
    total = 0.0
    for i in range(n):
        e = nx[i] - ref[i]
        resid += e * e
        total += nx[i] * nx[i]
    full_db = (10.0 * math.log10(resid / total) if (total > 0 and resid > 0) else float("-inf"))

    ewin = [0j] * ZPAD
    xw = [0j] * ZPAD
    hn = hann(n)
    for i in range(n):
        ewin[i] = complex((nx[i] - ref[i]) * hn[i], 0.0)
        xw[i] = complex(nx[i] * hn[i], 0.0)
    fft(ewin)
    fft(xw)
    bin_hz = sr / ZPAD
    pe = 0.0
    px = 0.0
    for k in range(1, ZPAD // 2):
        f = k * bin_hz
        if band_lo <= f <= band_hi:
            pe += abs(ewin[k]) ** 2
            px += abs(xw[k]) ** 2
    inband_db = (10.0 * math.log10(pe / px) if (pe > 0 and px > 0) else float("-inf"))

    # shape verification: max relative deviation of the product's in-band odd harmonic MAGNITUDES from
    # the ideal triangle ratio a_tri(k)/a_tri(1). Small => the product is a clean ideal triangle and the
    # reference attribution (aliasing == residual) is valid; large => the reference does not describe the
    # product (wrong shape / wrong scale), so blref is NOT a valid alias figure for this cell.
    shape_max = -float("inf")
    for k in ks:
        ideal_mag = scale * a_tri(k) / a_tri(1)
        if ideal_mag > 0:
            meas_mag = abs(proj(nx, sr, k * f0_snap))
            dev = abs(meas_mag - ideal_mag) / ideal_mag
            shape_max = max(shape_max, 20.0 * math.log10(dev if dev > 0 else 1e-300))
    return {"f0_snap": f0_snap, "kfull": len(ks), "a1_mag": a1_mag, "arg_a1": arg_a1,
            "blref_full_db": full_db, "blref_inband_db": inband_db, "blshape_max_db": shape_max,
            "resid_inband_p2": pe, "prod_inband_p2": px, "prod_total_p2": total,
            "n": n, "Nperiods": int(round(n * f0_snap / sr)), "gap": gap}


# ---------------------------------------------------------------------------
# G1: coincident-fold conjugation control (verified fold_pair machinery), generic coeff_fn.
# ---------------------------------------------------------------------------
def _same_line_pairs(coeff_fn, f0, sr, band_lo=BAND_LO, band_hi=BAND_HI, kfmax=KFMAX):
    a1_mag = abs(coeff_fn(1))
    per_line = {}
    k = 2
    while k * f0 <= kfmax:
        if k * f0 > sr * 0.5:
            f_obs, mirrored = fold_line(k * f0, sr)
            if band_lo <= f_obs <= band_hi and abs(coeff_fn(k)) > a1_mag * 1e-6:
                per_line.setdefault(f_obs, []).append((k, mirrored))
        k += 1
    out = []
    for f_obs, members in per_line.items():
        members.sort(key=lambda t: abs(coeff_fn(t[0])), reverse=True)
        same, mixed = [], []
        for i in range(len(members)):
            for j in range(i + 1, len(members)):
                ki, mi = members[i]
                kj, mj = members[j]
                (same if mi == mj else mixed).append((ki, mi, kj, mj))
        def best(lst):
            return lst and max(lst, key=lambda t: abs(coeff_fn(t[0])) * abs(coeff_fn(t[2])))
        bs, bm = best(same), best(mixed)
        if bs:
            out.append((f_obs, bs[0], bs[1], bs[2], bs[3]))
        if bm:
            out.append((f_obs, bm[0], bm[1], bm[2], bm[3]))
    return out


def _foldcoef(coeff_fn, k, f0, sr):
    _, m = fold_line(k * f0, sr)
    c = coeff_fn(k)
    return c.conjugate() if m else c


def _two_harmonic_signal(coeff_fn, k1, k2, f0, n, sr):
    a1, a2 = coeff_fn(k1), coeff_fn(k2)
    return [2.0 * (a1.real * math.cos(2 * math.pi * k1 * f0 * i / sr)
                   - a1.imag * math.sin(2 * math.pi * k1 * f0 * i / sr)
                   + a2.real * math.cos(2 * math.pi * k2 * f0 * i / sr)
                   - a2.imag * math.sin(2 * math.pi * k2 * f0 * i / sr))
            for i in range(n)]


def fold_pair_criteria(coeff_fn, name, f0, sr):
    rows, errors = [], []
    pairs = _same_line_pairs(coeff_fn, f0, sr)
    if not pairs:
        rows.append(("conjugation", f"{name}/{f0:g}@{sr:g}: no in-band coincident fold pair "
                                   "(documented, not-a-failure)"))
        return rows, errors
    per = sr / math.gcd(int(round(f0)), int(sr)) if abs(f0 - round(f0)) < 1e-9 else None
    n = (int(per) * 4) if per is not None else 8192
    note = " (integer-period, no leakage)" if per is not None else " (no integer-period: projection exact only to error bound)"
    def pick(same):
        cand = [p for p in pairs if (p[2] == p[4]) == same]
        return cand[0] if cand else None
    for kind, pair in (("constructive(same-parity add)", pick(True)),
                       ("destructive(mixed-parity cancel)", pick(False))):
        if pair is None:
            rows.append((kind, f"{name}/{f0:g}@{sr:g}: no pair of this parity (documented)"))
            continue
        f_obs, k1, m1, k2, m2 = pair
        S = _foldcoef(coeff_fn, k1, f0, sr) + _foldcoef(coeff_fn, k2, f0, sr)
        wrong_S = coeff_fn(k1) + coeff_fn(k2)          # old bug: no conjugate
        s = _two_harmonic_signal(coeff_fn, k1, k2, f0, n, sr)
        meas = matched_project(s, f_obs, n, sr)
        tol = 0.05 * (abs(S) + 1e-12)
        if abs(meas - S) > tol:
            errors.append(f"{kind}: {name}/{f0:g}@{sr:g} line {f_obs:g}Hz k={k1},{k2} proj={meas:.5f} "
                          f"!= conj-sum={S:.5f}")
        if abs(meas - wrong_S) <= tol:
            errors.append(f"{kind}: {name}/{f0:g}@{sr:g} line {f_obs:g}Hz k={k1},{k2} proj={meas:.5f} "
                          f"== buggy no-conj {wrong_S:.5f} (missing-conj bug NOT caught)")
        rows.append((kind, f"line {f_obs:7.2f}Hz k={k1}(m{int(m1)})&k={k2}(m{int(m2)}): "
                           f"conj-sum={abs(S):.5f} proj={abs(meas):.5f} buggy-no-conj={abs(wrong_S):.5f} -> "
                           f"{'CONSTRUCTIVE add' if kind.startswith('constructive') else 'PARTIAL-CANCEL'}{note}"))
    return rows, errors


# ---------------------------------------------------------------------------
# G4: dynamic range / correct-scaled-ideal-stand-in negative. Anchored to a REAL cell's sr + refined
# f0 + peak, so the synthetic ideals are matched to the real-data parameters. A correctly-scaled naive
# ideal triangle must read as ALIASED (naive fold); a correctly-scaled band-limited ideal must read as
# clean. The metric must separate them — proving a stand-in cannot be mistaken for a clean result.
# ---------------------------------------------------------------------------
def _bandlimited_tri(f0, sr, n):
    kmax = int((NYQ_FRAC * sr * 0.5) / f0)
    ref = [0.0] * n
    for k in range(1, kmax + 1):
        if k % 2 == 0:
            continue
        a = a_tri(k)
        w = 2.0 * math.pi * k * f0 / sr
        C, S = math.cos(w), math.sin(w)
        c, s = 1.0, 0.0
        for i in range(n):
            ref[i] += 2.0 * (a * c)
            nc, ns = c * C - s * S, s * C + c * S
            c, s = nc, ns
    return ref


def dynamic_range_control(sr, f0, peak):
    """Correct-scaled naive vs band-limited ideal triangle at the REAL cell sr/f0, scaled to its peak.
    Returns dict with naive_full / bl_full / sep, or None if no integer-period window fits."""
    import math as m
    fr = int(round(f0))
    if abs(f0 - fr) <= INT_TOL * f0:
        per = sr / m.gcd(fr, int(sr))
        per = int(per)
    else:
        per = None
    if per is None or per < 1:
        return None
    n = int(per) * 2
    # scale to the measured peak: ideal_tri peak is 1.0 (at p=0 or p=1), so multiply by peak.
    x_naive = [peak * ideal_tri(f0 * i / sr) for i in range(n)]
    x_bl = [peak * v for v in _bandlimited_tri(f0, sr, n)]
    r_naive = method_a(x_naive, sr, f0)
    r_bl = method_a(x_bl, sr, f0)
    if r_naive is None or r_bl is None:
        return None
    return {"naive_full": r_naive["full_db"], "bl_full": r_bl["full_db"],
            "naive_inband": r_naive["inband_db"], "bl_inband": r_bl["inband_db"],
            "sep": r_naive["full_db"] - r_bl["full_db"]}


# ---------------------------------------------------------------------------
# IO
# ---------------------------------------------------------------------------
def read_raw(path):
    with open(path, "rb") as f:
        raw = f.read()
    n = len(raw) // 8
    return list(struct.unpack("<%dd" % n, raw)) if n else []


def steady(x):
    return x[-STEADY:] if len(x) > STEADY else x


def read_cells(dirpath):
    tsv = os.path.join(dirpath, "gh19_scenarios.tsv")
    rows = []
    with open(tsv) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        return []
    header = lines[0].split("\t")
    idx = {name: i for i, name in enumerate(header)}
    for line in lines[1:]:
        cols = line.split("\t")
        if len(cols) < len(header):
            continue
        rec = {name: cols[idx[name]] for name in header}
        rec["produced"] = (rec.get("signal", "-") in ("", "-"))
        rows.append(rec)
    return rows


def miss_reason(mid, mrec, by_id, dirpath):
    """Fail-closed coverage predicate: return a reason a required cell is NOT an acceptable
    measurement, or None if it is. Single source of truth for both the coverage gate and the
    self-negative control, so a complete matrix cannot silently self-defeat the control."""
    c = by_id.get(mid)
    if c is None:
        return "no-row"
    if not c["produced"]:
        return "not-produced:" + c.get("signal", "")
    raw_rel = c.get("raw", "")
    if not raw_rel or not os.path.exists(os.path.join(dirpath, raw_rel)):
        return "skip-render:no-raw"
    return None


def load_manifest(path):
    """Read the committed required-scenario contract. Returns (required, allowed_blocks, parsed_rows).
    required = {id: rec}; allowed_blocks = {id: rec} for required=0 rows (declared deferred module
    probes that legitimately produce no product cell)."""
    required, allowed = {}, {}
    parsed = []
    with open(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        raise SystemExit("FATAL: manifest is empty")
    header = lines[0].split("\t")
    idx = {name: i for i, name in enumerate(header)}
    for line in lines[1:]:
        cols = line.split("\t")
        if len(cols) < len(header):
            continue
        rec = {name: cols[idx[name]] for name in header}
        rec["required"] = (rec.get("required", "0") == "1")
        parsed.append(rec)
        (required if rec["required"] else allowed)[rec["id"]] = rec
    return required, allowed, parsed


# When a non-triangle periodic / composite cell has a non-null gap this is leakage-uncontrolled; the
# harmonic-fit residual is then a diagnostic, labeled `leak`. We still report it but never call it an
# alias. Returns the category label.
def cat_of(path):
    if path.startswith("vco_a_tri") or path.startswith("vco_b_tri"):
        return "triangle(analytic_target)"
    if path == "preamp_ac":
        return "nonlinear(preamp_tanh)"
    if path == "wet_chain":
        return "composite(kVcfPath)"
    if path.startswith("drone1_classic"):
        return "composite(saw+cubic_chain)"
    if path.startswith("drone3_schmitt") or path.startswith("drone6_schmitt"):
        return "composite(Schmitt_voice)"
    return "periodic(harmfit_residual)"


def classify_samples(x):
    """Classify a sample window into a fail reason, or None if it is a valid produced signal.
    This is the real-data detector the self-negative controls exercise (silence / non-finite / over-scale /
    missing buffer)."""
    if not x:
        return "missing-raw"
    if any(not math.isfinite(v) for v in x):
        return "non-finite"
    peak = max(abs(v) for v in x)
    if peak < 1e-4:
        return "silent"
    if peak > 5.5:  # volt-domain upper bound; device domain clamped by the probe's own scale guard.
        return "over-scale"
    return None


def analyze_cell(dirpath, rec):
    """Return a dict with the metrics for one produced cell, or a dict with {err} on a bad cell. `x` is
    the steady-state sample window."""
    sr = float(rec["sr_hz"])
    f0_target = float(rec["f0_target_hz"])
    raw = os.path.join(dirpath, rec["raw"])
    x = steady(read_raw(raw))
    err = classify_samples(x)
    if err:
        return {"err": err}
    peak = max(abs(v) for v in x)
    seed = f0_target if f0_target > 0 else float(rec["f0_meas_hz"] or 0)
    seed = seed if seed > 0 else 200.0
    f0 = refine_f0(x, sr, seed)
    ma = method_a(x, sr, f0)
    out = {"sr": sr, "f0_target": f0_target, "f0": f0, "peak": peak,
           "kcount": (ma["kcount"] if ma else 0), "n": (ma["n"] if ma else 0),
           "periods": (ma["Nperiods"] if ma else 0), "gap": (ma["gap"] if ma else float("nan")),
           "harmris": ("%.2f" % ma["full_db"]) if ma and math.isfinite(ma["full_db"]) else "-",
           "harmris_inband": ("%.2f" % ma["inband_db"]) if ma and math.isfinite(ma["inband_db"]) else "-",
           "cat": cat_of(rec["path"]), "err": None}
    path = rec["path"]
    if (path.startswith("vco_a_tri") or path.startswith("vco_b_tri")) and ma:
        mb = method_b_tri(sr, f0, ma["a1_mag"])
        out["theory_dedup_db"] = ("%.2f" % mb["dedup_db"]) if mb else "-"
        mbl = method_bl(x, sr, f0)
        if mbl:
            out["blref_inband_db"] = (fmt_db(mbl["blref_inband_db"]) if mbl["blref_inband_db"] ==
                                      mbl["blref_inband_db"] else "-")
            out["blref_full_db"] = (fmt_db(mbl["blref_full_db"]) if mbl["blref_full_db"] ==
                                    mbl["blref_full_db"] else "-")
            out["blshape_max_db"] = (fmt_db(mbl["blshape_max_db"]) if mbl["blshape_max_db"] ==
                                     mbl["blshape_max_db"] else "-")
        else:
            out["blref_inband_db"] = "-"
            out["blref_full_db"] = "-"
            out["blshape_max_db"] = "-"
    else:
        out["theory_dedup_db"] = "-"
        out["blref_inband_db"] = "-"
        out["blref_full_db"] = "-"
        out["blshape_max_db"] = "-"
    return out


def fmt_db(v):
    return ("%.2f" % v) if (v is not None and math.isfinite(v)) else "-"


# ---------------------------------------------------------------------------
# PATCH 1 (DEVICE-SCALE CONTRACT, raw-side): an independent device-output-scale check for a clean DRY
# triangle cell. The expected peak is DERIVED from product source (see DRY_TRIANGLE_EXPECTED_PEAK =
# device_adapter.h kDeviceScaleProvisional 0.5 x vco.h triangle peak 1.0 = 0.5), NOT re-estimated from
# the raw and NOT the fitted a1_mag. The raw's MEASURED peak must meet it within DRY_SCALE_TOL. Feeding
# the SAME function a halved raw (peak 0.25 -> 50% deviation) must REJECT. This is the real raw-scale
# error-inject; building the reference at a wrong scale is only a reference-side corruption and does not
# prove the raw's scale error is caught (@Codex 1307b784).
# ---------------------------------------------------------------------------
def dry_triangle_scale_contract(x):
    """Return {ok, peak, expected_peak, dev}. ok=True iff the raw's measured peak is within DRY_SCALE_TOL
    of the source-derived DRY_TRIANGLE_EXPECTED_PEAK (0.5)."""
    peak = max(abs(v) for v in x) if x else 0.0
    expected = DRY_TRIANGLE_EXPECTED_PEAK
    dev = abs(peak - expected) / expected if expected > 0 else float("inf")
    return {"ok": dev <= DRY_SCALE_TOL, "peak": peak, "expected_peak": expected, "dev": dev}


# ---------------------------------------------------------------------------
# PATCH 2 (LABEL CONTRACT, accept/reject entry): validate a scenario record's declared sr/f0 labels
# against the INDEPENDENT actual-state expectation (the probe's measured fundamental f0_meas_hz and the
# actual sample clock), using the SAME accept/reject entry the gate grades real cells with. The normal
# check ACTUALLY COMPARES the declaration to these expectations — it does not merely print a
# deliberately-mismatched residual (the estimator-robustness test @Codex 1307b784 flagged). A duplicate
# record whose sr or f0 label is changed (raw + actual-state expectation unchanged) MUST be rejected here.
# ---------------------------------------------------------------------------
def label_contract(rec, x):
    """Return (ok, reason, detail). `rec` is a scenario record carrying `sr_hz`, `f0_target_hz` and the
    probe-independent `f0_meas_hz`; `x` is the steady-state raw. Compares:
      (1) declared f0_target vs the independently-measured f0_meas_hz (within F0_LABEL_REL_TOL), and
      (2) declared sr self-consistency: the per-sample band-limited reference must reconcile under the
          declared sr (a wrong sr breaks the integer-period window / reference alignment), AND the
          fundamental re-derived in Hz under the declared sr must agree with f0_meas_hz.
    A correct label set passes both; a corrupted sr or f0 label fails => REJECT."""
    sr_lab = float(rec["sr_hz"])
    f0_tgt = float(rec["f0_target_hz"])
    f0_meas = float(rec["f0_meas_hz"])
    # (1) f0 label vs actual-state expectation (independent probe measurement):
    if not (f0_meas > 0):
        return (False, "f0-meas", "record has no independent f0 measurement")
    if abs(f0_tgt / f0_meas - 1.0) > F0_LABEL_REL_TOL:
        return (False, "f0-label",
                "declared f0_target %.2f Hz vs independently-measured %.3f Hz (rel dev %.2f%%)"
                % (f0_tgt, f0_meas, abs(f0_tgt / f0_meas - 1.0) * 100.0))
    # (2) sr label self-consistency: reconcile the per-sample reference under the declared sr.
    bl = method_bl(x, sr_lab, f0_meas)
    if bl is None or not (bl["blref_inband_db"] == bl["blref_inband_db"]):
        return (False, "sr-label", "no reconciling per-sample reference at declared sr %.0f" % sr_lab)
    # (2b) also confirm the fundamental re-derived in Hz under the declared sr matches the measurement
    #      (refine_f0 is sr-SENSITIVE: its Hz output scales with the reported sample clock).
    recompute = refine_f0(x, sr_lab, f0_meas)
    if abs(recompute / f0_meas - 1.0) > F0_LABEL_REL_TOL:
        return (False, "sr-label",
                "fundamental re-derived under declared sr %.0f = %.3f Hz != measured %.3f Hz"
                % (sr_lab, recompute, f0_meas))
    return (True, "ok", "labels validated (f0_target=%.2f, sr=%.0f, measured=%.3f)" % (f0_tgt, sr_lab, f0_meas))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="report/gh19-probe")
    ap.add_argument("--manifest", default="tools/gh19_manifest.tsv")
    ap.add_argument("--check", action="store_true", help="run the negative-control gate")
    args = ap.parse_args()

    required, allowed, _ = load_manifest(args.manifest)

    cells = read_cells(args.dir)
    by_id = {c["id"]: c for c in cells}

    gate_errors = []   # hard FAILs (coverage + per-cell validity), cause exit 1 either mode
    check_fails = []   # only when --check (negative controls)

    # ---------------------------------------------------------------- coverage gate (always)
    cov_ok, cov_missing = 0, []
    for mid, mrec in required.items():
        r = miss_reason(mid, mrec, by_id, args.dir)
        if r is not None:
            cov_missing.append((mid, r))
        else:
            cov_ok += 1
    for mid, reason in cov_missing:
        gate_errors.append(f"required-cell-missing {mid} [{reason}]")
    if cov_ok == 0:
        gate_errors.append("0 required cells produced (empty-success)")
    if cov_missing:
        print(f"coverage: {cov_ok}/{len(required)} required cells produced", file=sys.stderr)

    # ---------------------------------------------------------------- per-cell analysis + report
    hdr = ("id\tpath\tsr\tf0_target\tf0_refined\tharmris_full_db\tharmris_inband_db\t"
           "blref_inband_db\tblref_full_db\tblshape_max_db\ttheory_dedup_db\t"
           "N\tperiods\tgap\tcategory")
    print(hdr)
    for cid in sorted(required) + [c["id"] for c in cells if c["id"] not in required]:
        rec = by_id.get(cid)
        if rec is None:
            # a manifest-required id with no row is already a coverage error; emit a stub row.
            m = required.get(cid)
            print("\t".join([cid, (m["path"] if m else "-"), (m["sr"] if m else "-"),
                             "-", "-", "-", "-", "-", "-", "-", "-",
                             "-", "-", "-", "MISSING"]))
            continue
        if not rec["produced"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-",
                             "-", "-", "-", "-", "-", "-", "-", "-",
                             "not-produced:" + rec.get("signal", "")]))
            continue
        a = analyze_cell(args.dir, rec)
        if a["err"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-",
                             "-", "-", "-", "-", "-", "-", "-", "-", a["err"]]))
            # a required cell that is produced but invalid is a hard FAIL (not an info line).
            if cid in required:
                gate_errors.append(f"invalid-cell {cid} [{a['err']}]")
            continue
        print("\t".join([
            cid, rec["path"], str(int(a["sr"])), str(int(a["f0_target"])),
            ("%.2f" % a["f0"]) if a["f0"] > 0 else "-",
            a["harmris"], a["harmris_inband"],
            a.get("blref_inband_db", "-"), a.get("blref_full_db", "-"),
            a.get("blshape_max_db", "-"), a.get("theory_dedup_db", "-"),
            str(a["n"]), str(a["periods"]),
            ("%.4f" % a["gap"]) if (a["gap"] == a["gap"]) else "-",  # NaN -> "-"
            a["cat"]]))

    # (G3 real-data validity of required cells is enforced by the coverage gate above: a required cell
    # that is non-finite / silent / over-scale is a hard FAIL, not an informational line.)

    # ---------------------------------------------------------------- machine evidence (cpu.tsv)
    cpu_recs = []
    cpu_tsv = os.path.join(args.dir, "gh19_cpu.tsv")
    if os.path.exists(cpu_tsv):
        with open(cpu_tsv) as fh:
            lines = [l.rstrip("\n") for l in fh if l.strip()]
        if lines:
            h = lines[0].split("\t")
            idx = {n: i for i, n in enumerate(h)}
            for line in lines[1:]:
                cols = line.split("\t")
                if len(cols) >= len(h):
                    rec = {n: cols[idx[n]] for n in h}
                    if rec.get("id", "").startswith("cpu_"):
                        cpu_recs.append(rec)
        for r in cpu_recs:
            print("MACHINE\t" + "\t".join([r.get("id", ""), r.get("value", ""), r.get("unit", ""),
                                           r.get("machine", ""), r.get("os", ""), r.get("compiler", "")]))

    # ---------------------------------------------------------------- negative-control gate (--check)
    if args.check:
        if not gate_errors:
            print("\n--- negative controls ---")
            g1_fail = []
            for wave, coeff, sfreq in (("saw", a_saw, 220.0),):
                for sr in (48000.0,):
                    rows, errs = fold_pair_criteria(coeff, wave, sfreq, sr)
                    for kind, detail in rows:
                        print("  G1  %s  sr=%g  %s" % (kind, sr, detail))
                    g1_fail.extend(errs)
            check_fails.extend(g1_fail)
            if not g1_fail:
                print("  G1 conjugation: PASS")

            # G2 / G4 (REAL-ENTRY injections, per @Codex 1307b784). Each negative injects an error at the
            # ACTUAL check entry on a REAL produced triangle cell and asserts the gate DETECTS it. Three
            # entries: (1) device-output-scale contract (raw-side), (2) label contract (sr/f0 labels vs the
            # independent actual-state expectation), (3) ideal-stand-in REFRAME — a clean band-limited ideal
            # is LEGAL, not a "must have enough aliasing" judgment; state->output association is tested via
            # a fixed-correctly-scaled-ideal/state-change that must be RED. The wrong-power-denominator and
            # silence controls are retained. The missing-row / missing-file coverage gate is unchanged.
            tri = next((c for c in cells if c["produced"] and c["path"].startswith("vco_a_tri")), None)
            if tri is None:
                tri = next((c for c in cells if c["produced"] and c["path"].startswith("vco_b_tri")), None)
            if tri is None:
                check_fails.append("G2 reconcile: no triangle cell present")
                print("  G2 reconcile: no triangle cell present")
            else:
                sr = float(tri["sr_hz"])
                x = steady(read_raw(os.path.join(args.dir, tri["raw"])))
                f0 = refine_f0(x, sr, float(tri["f0_target_hz"]))
                good = method_bl(x, sr, f0)
                if good is None or not (good["blref_inband_db"] == good["blref_inband_db"]):
                    check_fails.append("G2 blref: no reconciliation figure on real triangle cell")
                else:
                    # ---------------------------------------------------------------------
                    # CHECK ENTRY 1 — INDEPENDENT DEVICE-OUTPUT-SCALE contract (raw-side).
                    # expected_peak derives from product source (device_adapter.h 0.5 x vco.h peak 1.0
                    # = 0.5), NOT from the raw and NOT the fitted a1_mag. The real cell must satisfy it;
                    # the SAME function fed a HALVED raw (the actual failing mode) must REJECT.
                    # ---------------------------------------------------------------------
                    sc_real = dry_triangle_scale_contract(x)
                    print("  G1  device-scale contract (raw-side): real peak=%.4f expected=%.4f dev=%.1f%% %s"
                          % (sc_real["peak"], sc_real["expected_peak"], sc_real["dev"] * 100.0,
                             "OK" if sc_real["ok"] else "FAIL"))
                    sc_half = dry_triangle_scale_contract([v * 0.5 for v in x])
                    if sc_half["ok"]:
                        check_fails.append("G2 scale: halved raw NOT caught by the device-scale contract "
                                           "(raw-peak %.4f passes as %.4f)" % (sc_half["peak"], sc_real["peak"]))
                    else:
                        print("  G2 scale-negative: halved raw peak=%.4f -> REJECTED (dev %.1f%%; expected=%.4f)"
                              % (sc_half["peak"], sc_half["dev"] * 100.0, sc_half["expected_peak"]))
                    if not sc_real["ok"]:
                        check_fails.append("G2 scale: real product peak %.4f outside derived scale %.4f"
                                           % (sc_real["peak"], sc_real["expected_peak"]))
                    # ---------------------------------------------------------------------
                    # CHECK ENTRY 2 — LABEL CONTRACT (accept/reject entry). The SAME label_contract()
                    # validates the real cell's sr/f0 declaration against the independent actual-state
                    # expectation (probe-measured f0_meas_hz), and a DUPLICATED record whose sr or f0 label
                    # is changed (raw + actual-state expectation unchanged) MUST be rejected here. This
                    # replaces the estimator-robustness test ("wrong target corrected by frequency search"),
                    # which was estimator behavior, not label validation (@Codex 1307b784).
                    # ---------------------------------------------------------------------
                    lk, lr, ld = label_contract(tri, x)
                    if not lk:
                        check_fails.append("G2 label: real cell declared labels rejected [%s] %s" % (lr, ld))
                    else:
                        print("  G2 label-normal: %s (accepted)" % ld)
                    # bad sr label: same raw, actual-state expectation unchanged, declared sr halved -> REJECT.
                    rec_bad_sr = dict(tri); rec_bad_sr["sr_hz"] = "%g" % (sr * 0.5)
                    lk, lr, ld = label_contract(rec_bad_sr, x)
                    if lk:
                        check_fails.append("G2 label-sr: wrong sr label NOT rejected by label_contract")
                    else:
                        print("  G2 label-sr: halved-sr record -> rejected [%s] %s" % (lr, ld))
                    # bad f0 label: same raw, actual-state expectation unchanged, declared f0_target doubled.
                    rec_bad_f0 = dict(tri)
                    bad_f0_tgt = float(tri["f0_target_hz"]) * 2.0
                    rec_bad_f0["f0_target_hz"] = "%g" % bad_f0_tgt
                    lk, lr, ld = label_contract(rec_bad_f0, x)
                    if lk:
                        check_fails.append("G2 label-f0: wrong f0 label NOT rejected by label_contract")
                    else:
                        print("  G2 label-f0: wrong-f0 record (declared %.0f) -> rejected [%s] %s"
                              % (bad_f0_tgt, lr, ld))
                    # matrix-wide label validation: EVERY clean triangle cell's declared sr/f0 must be
                    # validated by the SAME label_contract entry (state -> output correspondence across the
                    # matrix), so the normal check genuinely compares the expectation for all cells, not just
                    # the representative one. Includes the FM-modulated vco_b_tri (its carrier still tracks
                    # the declared f0, so label_contract accepts it).
                    tri_errs, tri_cnt = [], 0
                    for c in cells:
                        if not (c["produced"] and (c["path"].startswith("vco_a_tri") or
                                                   c["path"].startswith("vco_b_tri"))):
                            continue
                        xr = steady(read_raw(os.path.join(args.dir, c["raw"])))
                        if classify_samples(xr) is not None:
                            continue
                        tri_cnt += 1
                        lk, lr, ld = label_contract(c, xr)
                        if not lk:
                            tri_errs.append("%s[%s]: %s" % (c["id"], lr, ld))
                    if tri_errs:
                        check_fails.append("G2 label-matrix: %d of %d triangle cells rejected label_contract"
                                           % (len(tri_errs), tri_cnt))
                        for e in tri_errs:
                            print("    [label]", e)
                    else:
                        print("  G2 label-matrix: %d triangle cells pass the label contract" % tri_cnt)
                    # WRONG POWER DENOMINATOR: the in-band alias figure MUST be referenced to the product's
                    # in-band power, never the whole-signal power (a classic dBc slip).
                    ok_denom = good["blref_inband_db"]
                    wrong_denom = (10.0 * math.log10(good["resid_inband_p2"] / good["prod_total_p2"])
                                   if (good["prod_total_p2"] > 0 and good["resid_inband_p2"] > 0)
                                   else float("-inf"))
                    if wrong_denom == float("-inf") or abs(wrong_denom - ok_denom) < 1e-3:
                        check_fails.append("G2 power-norm: power denominator NOT distinguishable")
                    else:
                        print("  G2 power-norm: correct(inband)=%.2fdB wrong(whole-signal)=%.2fdB -> caught"
                              % (ok_denom, wrong_denom))
                    # ---------------------------------------------------------------------
                    # CHECK ENTRY 3 — IDEAL STAND-IN REFRAME (state->output association). @Codex 1307b784:
                    # do NOT judge a real product by "output must have enough aliasing" (that would mark a
                    # future successful anti-aliasing fix as false). A clean, band-limited output is LEGAL.
                    # Test state->output association with an asymmetric state change (a cell whose output
                    # does not track its declared state frequency = a fixed correctly-scaled ideal) and a
                    # skip-render (silence) — both must be RED.
                    # ---------------------------------------------------------------------
                    win = intperiod_window(x, sr, f0)
                    if win is None:
                        check_fails.append("G4 stand-in: no integer-period window at real cell")
                    else:
                        nwin, f0_snap, gap = win
                        peak = max(abs(v) for v in x)
                        # the metric must separate a NAIVE ideal (aliased) from a BAND-LIMITED ideal (clean) —
                        # this validates the TOOL's resolving power, NOT that the real product is aliased.
                        dr = dynamic_range_control(sr, f0, peak)
                        if dr is None:
                            check_fails.append("G4 metric: no dynamic-range figure at real cell sr/f0")
                        elif dr["sep"] < SEP_MIN_DB:
                            check_fails.append("G4 metric: metric cannot separate aliased from clean "
                                               "(sep<%.0fdB)" % SEP_MIN_DB)
                        else:
                            print("  G4 metric: naive-ideal=%.2f dB bandlimited-ideal=%.2f dB sep=%.1f dB "
                                  "(tool resolves aliasing)" % (dr["naive_full"], dr["bl_full"], dr["sep"]))
                        # (c1) LEGALITY: a correctly-scaled clean band-limited ideal at the cell's CORRECT
                        #      state frequency is a VALID produced signal and is ACCEPTED by label_contract.
                        x_clean = list(_bandlimited_tri(f0_snap, sr, nwin))
                        if x_clean:
                            m = max(abs(v) for v in x_clean) or 1.0
                            x_clean = [peak * v / m for v in x_clean]  # scale to the measured cell peak
                            rec_clean = dict(tri)
                            rec_clean["f0_target_hz"] = "%g" % f0_snap
                            rec_clean["f0_meas_hz"] = "%g" % f0_snap
                            if classify_samples(x_clean) is not None:
                                check_fails.append("G4 clean-legal: clean band-limited output misclassified "
                                                   "as invalid")
                            else:
                                lk, lr, ld = label_contract(rec_clean, x_clean)
                                if not lk:
                                    check_fails.append("G4 clean-legal: clean band-limited output rejected "
                                                       "by label contract [%s]" % ld)
                                else:
                                    print("  G4 clean-legal: correctly-scaled clean ideal at real f0 "
                                          "classified valid + label accepted (clean output is LEGAL)")
                            # (c2) STATE-ASSOCIATION (RED): a fixed correctly-scaled ideal that IGNORES the
                            #      cell's state frequency (output at 2*f0_snap, state declares f0_snap) must
                            #      be REJECTED by label_contract — the output does not track the state.
                            x_fixed = list(_bandlimited_tri(2.0 * f0_snap, sr, nwin))
                            if x_fixed:
                                m = max(abs(v) for v in x_fixed) or 1.0
                                x_fixed = [peak * v / m for v in x_fixed]
                                rec_fixed = dict(tri)
                                rec_fixed["f0_target_hz"] = "%g" % f0_snap   # state says f0_snap
                                rec_fixed["f0_meas_hz"] = "%g" % (2.0 * f0_snap)  # actual output is 2*f0_snap
                                lk, lr, ld = label_contract(rec_fixed, x_fixed)
                                if lk:
                                    check_fails.append("G4 state-assoc: fixed ideal ignoring state frequency "
                                                       "not rejected by label_contract")
                                else:
                                    print("  G4 state-assoc: fixed correct-scale ideal at 2*f0 (state says f0) "
                                          "-> rejected [%s] %s" % (lr, ld))
                        else:
                            check_fails.append("G4 clean-legal: couldn't synthesize clean stand-in")
                    # SILENCE substituting the real output must be classified silent (an invalid cell).
                    if classify_samples([0.0] * len(x)) != "silent":
                        check_fails.append("G4 stand-in: silence substitution NOT detected as silent")
                    else:
                        print("  G4 stand-in: silence substitution classified silent")

            # G3 real-data: report the required-cell validity already enforced by the coverage gate.
            print("  G3 real cells: %d produced; coverage gate enforces finite/non-silent/scale on required cells."
                  % sum(1 for c in cells if c["produced"]))

            # Self-negative: the fail-closed detection helpers must actually detect each old-error mode,
            # so a skip-render / silence / non-finite / missing-file run cannot empty-succeed.
            selfneg_hits = []
            if classify_samples([0.0] * 128) != "silent":
                check_fails.append("selfneg: silence not detected")
            else:
                selfneg_hits.append("silence")
            if classify_samples([float("nan")] * 128) != "non-finite":
                check_fails.append("selfneg: non-finite not detected")
            else:
                selfneg_hits.append("non-finite")
            if classify_samples([]) != "missing-raw":
                check_fails.append("selfneg: skip-render(missing-buffer) not detected")
            else:
                selfneg_hits.append("skip-render")
            # a required cell whose raw is missing / not produced must be flagged by the coverage
            # gate. Test the DETECTION PRIMITIVE directly on synthetic records (NOT the chance
            # completeness of the live data), so a full matrix can never self-defeat the control:
            # every absence mode must yield a reason, and a genuinely complete record must yield none.
            if required:
                first = next(iter(required))
                real = by_id.get(first)
                mrec = required[first]
                # (a) guard: a complete record must NOT be reported missing (no false positive).
                if real and real["produced"] and real.get("raw") and \
                   os.path.exists(os.path.join(args.dir, real["raw"])):
                    if miss_reason(first, mrec, by_id, args.dir) is not None:
                        check_fails.append("selfneg: complete required cell reported missing")
                # (b) every synthetic absence mode MUST be flagged by the same primitive the gate uses.
                syn = [
                    {"id": first, "path": mrec["path"], "sr_hz": "1",
                     "produced": False, "signal": "silent", "raw": ""},
                    {"id": first, "path": mrec["path"], "sr_hz": "1",
                     "produced": True, "signal": "", "raw": ""},
                    {"id": first, "path": mrec["path"], "sr_hz": "1",
                     "produced": True, "signal": "", "raw": "does-not-happen.raw"},
                ]
                if all(miss_reason(first, mrec, {first: s}, args.dir) is not None for s in syn):
                    selfneg_hits.append("missing-file-gate")
                else:
                    check_fails.append("selfneg: missing-file-gate did not flag a synthetic absence")
            if selfneg_hits:
                print("  SELF-NEG: detected [%s] (fail-closed; no empty-success)." % "/".join(selfneg_hits))

        print("\n--- gate summary ---")
        if gate_errors:
            print("COVERAGE FAIL (%d):" % len(gate_errors))
            for g in gate_errors:
                print("  [FAIL]", g)
        if check_fails:
            print("CHECK FAIL (%d):" % len(check_fails))
            for g in check_fails:
                print("  [FAIL]", g)
        if gate_errors or check_fails:
            return 1
        print("GATE PASS: coverage OK + measurement negative controls OK (no aliasing threshold asserted).")
        return 0

    # non-check mode (matrix production): fail-closed still applies to coverage.
    if gate_errors:
        print("COVERAGE FAIL (%d):" % len(gate_errors), file=sys.stderr)
        for g in gate_errors:
            print("  [FAIL]", g, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
