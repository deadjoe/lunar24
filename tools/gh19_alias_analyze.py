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
# S2 PROBE-CONTRACT GATE (fail-closed, runs under --check): the classic cells' acceptance column is
#   measured off a probe-built drift-OFF DroneBank, so the probe must prove that its own construction
#   is the shipped one. `gh19_fidelity.tsv` carries that proof per classic cell, and this gate rejects
#   the run unless: (a) the file exists and has a row for EVERY produced classic cell with the right
#   sample count; (b) `bit_identical == YES` (the probe's drift-ON arm == the shipped runtime raw, bit
#   for bit -- the fork detector); (c) `drift_off_differs == YES` (the drift flag is not vacuous); and
#   (d) every classic cell carries a positive finite model f0. The row predicate is itself exercised on
#   hand-built good/forked/vacuous rows, so it cannot pass by being unable to go red.
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
# S2 CLASSIC SAW CELLS (drone_1/2/4/5, saw->cubic composite) — the acceptance column is the PINNED-phi
# residual `blpin_full_db`, NOT `blref_full_db`. Reason (measured, scratch/s2_underread_split.py): the
# low-read of `blref_full_db` is 100% the phase degree of freedom -- a fitted `phi` absorbs the very
# effect under test (d_amp == 0.00 dB at all eight frequencies, d_ph is the whole gap), so it reports a
# real -9.92 dB improvement as +0.43 dB (sign reversed) on 441 Hz. `blpin_full_db` fits ONLY the scale
# A at an explicitly supplied window-start phase, so it has no phase freedom to absorb anything.
#   * `blref_full_db` / `blref_inband_db` / `blshape_max_db` are retained as DIAGNOSTICS on these cells.
#   * `blpin_full_db` = 10log10(sum((x - A*ref)^2)/sum(x^2)) with ref built at (f0_snap, phi_pin) and
#     A fitted; `blpin_amp` and `blpin_phi_rad` are the fitted scale and the pinned phase it used.
#   * f0 SOURCE -- the ACCEPTANCE column is pinned at `blf0_model_hz`, MEASURED (not fitted) on the
#     product side [ruling B, @Kimi f260fd91]. The probe builds its own DroneBank for each classic cell
#     with `driftEnabled=false`, renders it, and reports the accumulator rate it actually ran at:
#     freqBase*2^(tune/12)*2^(-volt/12)*(1+tolerance). On that arm the rate is an exact constant, so the
#     phase enters as the analytic `sawcubic_pin_phi(f0_snap, sr, warm)` = 2*pi*frac(warm*f0/sr) and the
#     ONLY free parameter left on the column is the scale A. Nothing on this column is fitted.
#     `blf0_aligned_hz` is retained as the ALIGNER's own diagnostic, not as the pin: it is derived from
#     the signal under test, so pinning on it would reintroduce the degree of freedom this column exists
#     to remove. `blf0_model_hz` is "-" (and the pinned columns with it) on any cache predating ruling B:
#     the pinned instrument is defined on the constant-frequency arm only, and reporting it off a
#     frequency-modulated arm would be a reading, not a measurement.
#     PROVENANCE: the product's in-production path is untouched byte-for-byte -- the runtime still
#     constructs its DroneBank with drift ON. Only the probe constructs a drift-off bank, for these 12
#     cells, and the probe-contract gate below asserts that the probe's own drift-ON arm reproduces the
#     shipped runtime raw BIT FOR BIT, so the two paths cannot silently diverge.
#     Residual phase uncertainty on the pin is the jitter phase-walk over the warm segment, 1.5e-4 rad
#     (written caveat, negligible against the effects measured here).
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
PERIODIC_TOL = 1.0e-9                # relative-to-peak tolerance for realized_period_window's
                                     # exact-repetition test. The Schmitt steady state repeats
                                     # BIT-EXACTLY at the realized period (measured max|x[i]-x[i+per]|
                                     # == 0 over the full window on all 12 required Schmitt cells), so
                                     # this tolerance is pure FP-contraction head-room across build
                                     # platforms, not a model allowance: a residual of 1e-9 of full
                                     # scale leaks at the ~-180 dB level, far below the metric floor.
SEP_MIN_DB = 12.0                    # minimum naive-vs-bandlimited separation the metric must sustain
                                     # for the dynamic-range negative to count as passing.

# --- S2: the classic drone saw->cubic composite (GH#19 S2). ---
# core/include/lunar24/core/drone_bank.h:302 `nonlinearity(sawtooth(v.phase))`: a SAW is a VALUE jump, so
# the anti-aliasing family is polyBLEP, not the BLAMP family used for the Schmitt triangle (S1). The
# composite saw->cubic has the pure-sin Fourier series y(theta) = sum_k c_k sin(k*theta) with
#     c_k = -4/(3*pi*k) - 4/(pi^3*k^3)      (c_1 = -0.553419 = 0.869*(-2/pi))
# (two independent derivations agree to rel 2.6e-9; scratch/s2_sawcubic_precheck.py). The reference and
# its leakage compensation are built in the S2 section below `bandlimited_tri`.
SAWCUBIC_IDENT_TOL = 1.0e-9          # structural leakage-free detector: |n*f0_snap/sr - round(...)|
SAWCUBIC_SOLVE_TOL = 1.0e-15         # Newton step that ends the phi root-find (converges in <5)
SAWCUBIC_MAX_ITERS = 60              # Newton iteration cap
SAWCUBIC_SELFTEST_TOL = 1.0e-9       # self-check tolerance: closed-form H vs a numerically projected
                                     # reference, and the inversion's recovery of a known (A, phi)

# --- PINNED-phi acceptance column (ruling 2026-09-11 #1; instrument = sawcubic_pinned) -----------------
# The classic-cell acceptance column is the residual of the bandlimited reference at an EXPLICIT,
# externally-supplied (f0, phi) with ONLY the scale A fitted. Both inputs come from the probe's phase
# sidecar (DroneBank::phaseOf), so the column has no free phase parameter to absorb the effect under
# test. See self_check_pinned() for the three regressions that keep it honest.
SAWCUBIC_PIN_FLOOR_DB = -180.0       # an exactly-matching composite must land below this (machine floor
                                     # is ~-300; -180 leaves room for the FFT-free direct sum)
SAWCUBIC_PIN_SCALE_TOL = 1.0e-9      # the recovered A must equal the built A to this relative tolerance
SAWCUBIC_PIN_DISCRIM_DB = 20.0       # a 0.02 rad phase error must move the pinned column by at least
                                     # this much (measured: see the S2 report; a no-op pin fails here)
SAWCUBIC_FIT_ABSORB_DB = 3.0         # ...while the phi-FITTED comparator must move by LESS than this,
                                     # i.e. the contrast that proves the fit absorbs phase is exercised

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
# REALIZED-period window: for a path whose steady state is exactly periodic in an INTEGER number of
# samples but whose realized frequency sr/(2M) is a FRACTIONAL Hz, so the integer-Hz snap above cannot
# reach it. This is the Schmitt audio path: schmitt_osc.h clamps ramp_ to the rail and DISCARDS the
# overshoot, so every corner lands exactly on a sample, the half-period M = ceil(1/r) is an integer and
# the steady state is exactly 2M-periodic (the product header documents this).
#
# The window is validated by the property that actually makes it leakage-free -- EXACT repetition at
# `per` -- rather than by a frequency tolerance, so it does not depend on refine_f0's +-0.02 Hz
# precision. If the signal does not repeat, this returns None and the caller reports the cell as
# unmeasurable (fail-closed) instead of emitting a leaky figure. Measured on the 12 required Schmitt
# cells: max|x[i]-x[i+per]| == 0 over the whole window, i.e. zero, against a 1e-9*peak tolerance.
# Returns (N, f0_snap, gap) with gap == 0 (leakage-free by construction), or None when inapplicable.
# ---------------------------------------------------------------------------
def realized_period_window(x, sr, f0):
    maxlen = len(x)
    if not (f0 > 0):
        return None
    M = int(round(sr / (2.0 * f0)))
    if M < 1:
        return None
    per = 2 * M
    if per > maxlen:
        return None
    peak = 0.0
    for v in x:
        a = abs(v)
        if a > peak:
            peak = a
    if peak <= 0.0:
        return None
    tol = PERIODIC_TOL * peak
    for i in range(maxlen - per):
        if abs(x[i] - x[i + per]) > tol:
            return None
    n = (maxlen // per) * per
    if n < 4 * per and maxlen >= 4 * per:
        n = 4 * per
    elif n < per:
        n = per
    return (n, sr / per, 0.0)


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


# ---------------------------------------------------------------------------
# S2: the band-limited reference for the classic drone saw->cubic composite (a VALUE-jump path).
#
# `bandlimited_tri` (above) builds its reference from the measured fundamental ALONE. That is sound only
# when proj(x, sr, f0) returns the fundamental's own complex amplitude a1 -- i.e. on a leakage-free
# window. For a NON-integer-bin window it does not: the projection carries Dirichlet-kernel leakage from
# every other harmonic. The classic cells sit at f0 = 14.1..31.9 Hz, so f0*n/sr is 1.3..5.9 bins and the
# leak is not small: measured on a PERFECT synthetic composite at a known (A, phi), the naive
# construction recovers A with a 1.4 %..2.7 % error and lands on a -11..-19 dB residual floor -- ABOVE
# the -31..-34 dB the classic cells' own aliasing is measured at. An S1-style naive reference would
# therefore be measuring its own leakage, not the product.
#
# The fix uses the composite's closed form, whose projection is exactly computable from the Dirichlet
# kernel D(F) = (1/N) sum_i e^{i*2*pi*F*i/sr}:
#     proj(x_ref(A, phi)) = A * H(phi)
#     H(phi) = (1/(2i)) sum_k c_k [ e^{i*k*phi} D((k-1)*f0) - e^{-i*k*phi} conj(D((k+1)*f0)) ]
# so proj alone determines (A, phi): arg H(phi) = arg(proj) is ONE real equation in phi (Newton), then
# A = |proj| / |H(phi)|. a1 does NOT factor out linearly -- harmonic k carries phase offset k*phi while
# a1 carries only the k=1 phase -- so the inversion is a 1-D root-find, NOT the 2x2 real solve an earlier
# draft assumed (that draft gave every harmonic the SAME offset, which is a different waveform: -1.7..
# -2.3 dB against the true composite, i.e. it explained ~60 % of the energy).
#
# ZERO-DISTURBANCE: on a leakage-free window D(m*f0) = delta_m0, so H(phi) = i*(|c_1|/2)*e^{i*phi} and the
# construction reduces EXACTLY to the plain (A, phi) = (2|a1|/|c_1|, arg(a1) - pi/2). The leakage-free
# case is detected STRUCTURALLY (n*f0_snap/sr integer) and short-circuited to the plain convention, so
# those cells are bit-identical to an uncompensated implementation -- asserted by self_check_sawcubic().
# That is what keeps the S1 Schmitt cells (realized_period_window: exactly periodic => integer bins) and
# the integer-Hz VCO cells provably untouched.
#
# SCOPE OF THE CLAIM: H(phi) is built from the in-band harmonic LINES k*f0 only. Aliasing energy folding
# BETWEEN those lines has no representation in H, so this compensation cannot absorb it -- there is no
# circularity in which the fix hides the defect it is measuring.
# ---------------------------------------------------------------------------
def sawcubic_c(k):
    """Fourier coefficient of the saw->cubic composite in y(theta) = sum_k c_k sin(k*theta)."""
    return -4.0 / (3.0 * math.pi * k) - 4.0 / (math.pi ** 3 * k ** 3)


def sawcubic_ks(f0_snap, sr):
    """Harmonic indices of the composite reference: ALL k (not odd-only, unlike the triangle)."""
    ks = []
    kmax = int(sr * 0.5 / f0_snap) + 1
    for k in range(1, kmax + 1):
        if MIN_HZ <= k * f0_snap <= sr * 0.5:
            ks.append(k)
    return ks


def _dirichlet(F, n, sr):
    """D(F) = (1/n) sum_{i<n} e^{i*2*pi*F*i/sr}, in sinc form so it survives F -> 0."""
    x = 2.0 * math.pi * F / sr
    if abs(x) < 1e-12:
        return 1.0 + 0j
    return cmath.exp(1j * x * (n - 1) / 2.0) * (math.sin(x * n / 2.0) / (n * math.sin(x / 2.0)))


def sawcubic_H(n, sr, f0_snap, phi):
    """The composite's projection gain: proj(bandlimited_sawcubic(n, sr, f0, A, phi)) == A * H(phi)."""
    acc = 0j
    for k in sawcubic_ks(f0_snap, sr):
        dp = _dirichlet((k - 1) * f0_snap, n, sr)
        dm = _dirichlet(-(k + 1) * f0_snap, n, sr)
        acc += sawcubic_c(k) * (cmath.exp(1j * k * phi) * dp - cmath.exp(-1j * k * phi) * dm)
    return acc / 2j


def sawcubic_leakage_free(n, sr, f0_snap):
    """Structural test: is this window exactly periodic in f0_snap (=> D(m*f0) = delta, no leakage)?
    n*f0_snap/sr is an exact integer for both windows the two window_fns return -- the integer-Hz snap
    (f0_snap = round(f0), n a multiple of sr/gcd) and the realized period (f0_snap = sr/per, n a multiple
    of per) -- so this recovers the leakage-free case without a fitted threshold."""
    m = n * f0_snap / sr
    return abs(m - round(m)) <= SAWCUBIC_IDENT_TOL


def sawcubic_plain(a1_meas):
    """The uncompensated convention: a1 = (A*c_1/2) e^{i(phi-pi/2)} with c_1 < 0."""
    return (2.0 * abs(a1_meas) / abs(sawcubic_c(1)), cmath.phase(a1_meas) - math.pi * 0.5)


def sawcubic_fit(n, sr, f0_snap, a1_meas):
    """Invert proj = A*H(phi) for (A, phi). A leakage-free window short-circuits to the plain convention
    (bit-identical to an uncompensated implementation); otherwise Newton on arg H(phi) = arg(proj)."""
    if sawcubic_leakage_free(n, sr, f0_snap):
        return sawcubic_plain(a1_meas)
    want = cmath.phase(a1_meas)
    phi = want - math.pi * 0.5
    h = 1.0e-6
    for _ in range(SAWCUBIC_MAX_ITERS):
        hc = sawcubic_H(n, sr, f0_snap, phi)
        r = math.atan2(hc.imag, hc.real) - want
        r = math.atan2(math.sin(r), math.cos(r))
        hp = sawcubic_H(n, sr, f0_snap, phi + h)
        hm = sawcubic_H(n, sr, f0_snap, phi - h)
        d = (math.atan2(hp.imag, hp.real) - math.atan2(hm.imag, hm.real)) / (2.0 * h)
        if d == 0.0:
            break
        step = -r / d
        if abs(step) > 0.5:
            step = 0.5 * (1.0 if step > 0.0 else -1.0)
        phi += step
        if abs(step) < SAWCUBIC_SOLVE_TOL:
            break
    hh = sawcubic_H(n, sr, f0_snap, phi)
    if abs(hh) == 0.0:
        return None
    return (abs(a1_meas) / abs(hh), phi)


def bandlimited_sawcubic(n, sr, f0_snap, A_amp, phi):
    """x_ref[i] = A * sum_{k: MIN_HZ<=k*f0_snap<=sr/2} c_k sin(k*(2*pi*f0_snap*i/sr) + k*phi).
    Full-Nyquist band-limit; harmonic k's phase advances as k*phi (the product's measured progression).
    Returns (ref, ks)."""
    tt = 2.0 * math.pi * f0_snap / sr
    ks = sawcubic_ks(f0_snap, sr)
    ref = [0.0] * n
    for k in ks:
        w = k * tt
        C, S = math.cos(w), math.sin(w)
        c, s = 1.0, 0.0
        kp = k * phi
        cp, sp = math.cos(kp), math.sin(kp)
        amp = A_amp * sawcubic_c(k)
        for i in range(n):
            ref[i] += amp * (s * cp + c * sp)
            nc, ns = c * C - s * S, s * C + c * S
            c, s = nc, ns
    return ref, ks


def sawcubic_pin_phi(f0_snap, sr, warm):
    """The composite reference's phase argument at the window's FIRST sample, ANALYTIC -- not fitted.

    Derivation (product, read at HEAD): the classic saw renders from v.phase == 0
    (drone_bank.h:185) and advances 2*pi*effFreq/sr per sample (drone_bank.h:305), wrapping mod 2*pi
    (:306-307). Its waveform is sawtooth(phase) = 2*(phase/2pi) - 1 (:351), so phase/2pi IS the
    fractional position q, and the cubic composite's series is sum_k c_k*sin(k*2*pi*q) (:352).
    The reference family is written in the same convention -- bandlimited_sawcubic synthesises
    sin(k*(2*pi*f0*i/sr) + k*phi) -- so matching harmonic k's argument gives phi == 2*pi*q. At the
    window's first sample the accumulator has advanced `warm` samples (the probe's kWarm settle
    samples == len(raw) - STEADY), hence

        phi_pin = 2*pi*frac(warm*f0_snap/sr).

    This deliberately omits the oscillator's per-sample jitter (kOscNoiseAmpHz = 0.02 Hz,
    drone_bank.h:109), whose accumulated contribution over `warm` samples is a zero-mean random walk.
    Its magnitude is MEASURED against the probe's phaseOf() and reported -- not assumed (the
    estimator-versus-truth control required by the 2026-09-10 methodology memo). The pin is a pure
    function of (f0_snap, sr, warm) and carries NO information from the signal under test, which is
    exactly why it cannot absorb the effect this slice measures.
    """
    return 2.0 * math.pi * math.fmod(warm * f0_snap / sr, 1.0)


def sawcubic_pinned(nx, sr, f0_snap, warm):
    """The acceptance reading (ruling 2026-09-11 item 1): pin phi analytically, fit ONLY the scale A.

    Returns (full_db, A_pin, phi_pin), or None if the window is degenerate. `full_db` is the SAME
    time-domain residual ratio as blref_full_db so the two columns are directly comparable; the only
    difference is that phi is not a free parameter here. Fitting A alone is harmless: the measured
    amplitude degree of freedom contributes 0.00 dB at every frequency (d_amp == 0 in all 8 rows of
    the split), while the fitted phi is what turned a measured -9.92 dB correction into a reported
    +0.43 dB regression at 441 Hz.
    """
    n = len(nx)
    if n <= 0 or f0_snap <= 0.0:
        return None
    phi = sawcubic_pin_phi(f0_snap, sr, warm)
    unit, _ = bandlimited_sawcubic(n, sr, f0_snap, 1.0, phi)
    num = 0.0
    den = 0.0
    tot = 0.0
    for i in range(n):
        v = nx[i]
        num += v * unit[i]
        den += unit[i] * unit[i]
        tot += v * v
    if den <= 0.0 or tot <= 0.0:
        return None
    A_pin = num / den
    resid = 0.0
    for i in range(n):
        e = nx[i] - A_pin * unit[i]
        resid += e * e
    if resid <= 0.0:
        return None
    return (10.0 * math.log10(resid / tot), A_pin, phi)


def self_check_sawcubic():
    """Self-check for the S2 reference. Returns a list of failure strings (empty == pass).

    Three things must hold, and only the first is a zero-disturbance claim:
      (1) STRUCTURAL IDENTITY: on a leakage-free window the fit must return the plain convention BIT-
          EXACTLY (same tuple), i.e. the compensation is provably not in the path for those cells. This
          is the regression assertion covering S1: the Schmitt cells are exactly periodic, so their
          window is leakage-free and their reference cannot move.
      (2) CLOSED FORM: A*H(phi) must equal the numerically projected reference.
      (3) INVERSION: on a non-bin (leaky) synthetic at a known (A, phi), the fit must recover both, and
          the rebuilt reference must reconcile to a numerically-zero residual. This is what proves the
          compensation actually works where it IS in the path.
    """
    fails = []
    sr = 44100.0
    n = 4096
    A_amp, phi = 0.0658, 1.2345
    a1_true = (A_amp * abs(sawcubic_c(1)) / 2.0) * cmath.exp(1j * (phi + math.pi * 0.5))

    # (1) leakage-free: bins exact (n*f0/sr = 3), so the fit must be the plain convention, bit-exact.
    for lab, f0, nw in (("bin-exact", 44100.0 * 3.0 / 4096.0, 4096),
                        ("realized-period", 44100.0 / 128.0, 4096)):
        m = nw * f0 / sr
        if abs(m - round(m)) > SAWCUBIC_IDENT_TOL:
            fails.append("sawcubic-selftest: %s window is not leakage-free (%.6f)" % (lab, m))
            continue
        ref_a, ks_a = bandlimited_sawcubic(nw, sr, f0, A_amp, phi)
        got = sawcubic_fit(nw, sr, f0, a1_true)
        want = sawcubic_plain(a1_true)
        if got != want:
            fails.append("sawcubic-selftest: %s fit != plain convention (bit-exact identity broken)" % lab)
        # (1b) ZERO-DISTURBANCE REGRESSION, end to end: the RENDERED reference the analyzer actually
        # subtracts must be element-for-element identical before and after compensation -- not merely
        # close. `!=` on the two lists is exact float equality, so this catches a compensation that
        # reaches the rendering path by any route other than the (A, phi) it returns. This is the
        # assertion that covers S1: those cells are exactly periodic => leakage-free => never
        # compensated, so their reference cannot move by one bit.
        ref_before, _ = bandlimited_sawcubic(nw, sr, f0, want[0], want[1])
        ref_after, _ = bandlimited_sawcubic(nw, sr, f0, got[0], got[1])
        if ref_before != ref_after:
            nd = sum(1 for a, b in zip(ref_before, ref_after) if a != b)
            fails.append("sawcubic-selftest: %s compensated reference is not byte-identical to the "
                         "uncompensated one (%d/%d samples differ) -- S1 zero-disturbance broken"
                         % (lab, nd, len(ref_before)))
        hh = sawcubic_H(nw, sr, f0, phi)
        want_h = 1j * (abs(sawcubic_c(1)) / 2.0) * cmath.exp(1j * phi)
        if abs(hh - want_h) > SAWCUBIC_SELFTEST_TOL * abs(want_h):
            fails.append("sawcubic-selftest: %s H(phi) != i(|c_1|/2)e^{i phi} (rel %.2e)"
                         % (lab, abs(hh - want_h) / abs(want_h)))
        del ref_a, ks_a

    # (2)+(3) leaky (non-bin) synthetic at a known (A, phi).
    for f0 in (21.146, 14.353, 31.9477):
        ks = sawcubic_ks(f0, sr)
        if not ks:
            fails.append("sawcubic-selftest: no harmonics at f0=%.4f" % f0)
            continue
        ref_true, _ = bandlimited_sawcubic(n, sr, f0, A_amp, phi)
        lhs = proj(ref_true, sr, f0)
        rhs = A_amp * sawcubic_H(n, sr, f0, phi)
        if abs(lhs - rhs) > SAWCUBIC_SELFTEST_TOL * abs(rhs):
            fails.append("sawcubic-selftest: closed form H(phi) off at f0=%.4f (rel %.2e)"
                         % (f0, abs(lhs - rhs) / abs(rhs)))
        fit = sawcubic_fit(n, sr, f0, lhs)
        if fit is None:
            fails.append("sawcubic-selftest: fit returned None at f0=%.4f" % f0)
            continue
        A_hat, phi_hat = fit
        if abs(A_hat - A_amp) > 1e-6 * A_amp or abs(phi_hat - phi) > 1e-6:
            fails.append("sawcubic-selftest: inversion off at f0=%.4f (A %.9f vs %.9f, phi %.9f vs %.9f)"
                         % (f0, A_hat, A_amp, phi_hat, phi))
        ref_hat, _ = bandlimited_sawcubic(n, sr, f0, A_hat, phi_hat)
        e2 = sum((ref_true[i] - ref_hat[i]) ** 2 for i in range(n))
        t2 = sum(v * v for v in ref_true)
        if t2 > 0.0 and (e2 <= 0.0 or 10.0 * math.log10(e2 / t2) > -120.0):
            fails.append("sawcubic-selftest: rebuilt reference does not reconcile at f0=%.4f (%.2f dB)"
                         % (f0, 10.0 * math.log10(e2 / t2) if e2 > 0 else float("-inf")))
    return fails


F0_ALIGN_REL_STEP = 1.0e-4            # first-bracket half-width when the detector seeded us (~5e-5 error)
F0_ALIGN_REL_STEP_COARSE = 3.0e-3     # ... and when it did not (coarse estimator error ~2e-3): a bracket
                                      #    that already straddles, so the budget is not burnt widening
F0_ALIGN_STAGES = 3                   # fixed shrink stages: h0, h0/10, h0/100 -- after stage 1 the
                                      #    centre is already at the minimum, so each later bracket
                                      #    nests inside it and the parabola is unbiased
F0_ALIGN_SHRINK = 0.1                 # bracket shrink factor between stages
F0_ALIGN_WIDEN = 2                    # x3 widenings allowed per stage (bracket must bracket)
F0_ALIGN_MAX_EVAL = 24                # hard evaluation budget; exhausted => fail closed
F0_ALIGN_SPAN_MAX = 0.02              # never accept a candidate more than 2% from the passed seed
F0_ALIGN_STEP_THRESH = 0.03           # level crossing: a sample-to-sample drop below -thresh*peak
F0_ALIGN_MIN_STEPS = 3                # resets needed before the detector's average is trusted
F0_ALIGN_DETECT_TOL = 0.03            # reject a detector reading this far (relative) from the seed


def step_f0_detect(x, sr, thresh_frac=F0_ALIGN_STEP_THRESH):
    """Fundamental from the composite's reset steps, by level crossing. O(n), no band-limited
    reconstruction, so it is essentially free next to one method_bl call.

    y = x - x^3/3 is monotone in x on [-1,1] (dy/dx = 1-x^2 >= 0), so a saw period contains exactly ONE
    large downward jump -- at the wrap, where x: +1 -> -1 and y: +2/3 -> -2/3, a drop of 4/3 against a
    peak of 2/3. A 3% threshold therefore selects the resets and rejects the ramp and any modulation
    wiggle. The crossing is placed by linear interpolation between the two straddling samples, and the
    fundamental is the mean over the span between the first and last reset, which averages out the
    within-window drift ramp. Returns (f0, nsteps); (None, 0) when the detector does not apply."""
    if len(x) < 4:
        return (None, 0)
    peak = max(abs(v) for v in x)
    if not (peak > 0.0):
        return (None, 0)
    thr = -thresh_frac * peak
    pos = []
    for i in range(1, len(x)):
        if x[i] - x[i - 1] < thr:
            hi, lo = x[i - 1], x[i]
            d = hi - lo
            pos.append(i - 1 + ((hi - 0.5 * (hi + lo)) / d if d > 0.0 else 0.5))
    if len(pos) < F0_ALIGN_MIN_STEPS or pos[-1] <= pos[0]:
        return (None, len(pos))
    return (sr * (len(pos) - 1) / (pos[-1] - pos[0]), len(pos))


def align_f0(x, sr, seed, ref_kind="sawcubic"):
    """Align the reference's f0 for a PHASE-SENSITIVE time-domain residual, by a bounded 1-D
    minimization of blref_full itself. [@Kimi ruling (A).]

    Why this is needed: blref_full_db = 10log10(sum (x-ref)^2 / sum x^2) takes f0 as an INPUT, so a
    relative error in f0 lays down a linear phase ramp across the window and the residual is dominated by
    that ramp, not by aliasing. Measured: the same ideal composite reads -241 dB at its true f0 and
    -14 dB when the f0 input is 2.7e-3 relative away -- i.e. 100% of the observed floor was input error.

    Why it is cheap: the reset detector above measures f0 directly from the waveform and lands within
    0.01-1.20 mHz of the residual's true minimum (measured on the 3 cells of s2_f0refine_probe_out.txt),
    so the ladder merely VERIFIES a bracket that is already right; it does not have to find it. Total
    cost is bounded at 3 + 2*F0_ALIGN_WIDEN evaluations, and the ladder step (1e-4 relative) is orders of
    magnitude finer than the projection main lobe's sr/(n*f0) in relative terms (6.8e-2 at 44100/n=16384/
    f0=13.17 Hz), satisfying the "step <= lobe/3" requirement with a wide margin.

    Deterministic by construction: fixed relative offsets, fixed evaluation order, fixed interpolation.
    Two calls on identical input MUST return bit-identical results -- see self_check_align; a failure
    there means the pipeline has non-determinism (search order / interpolation boundary / float
    association order), which is itself a defect, not a tolerance question. [@Kimi pin 1.]

    Fail-closed: the bracket is widened while the minimum sits on an edge, and if no interior minimum
    with positive curvature is found within F0_ALIGN_SPAN_MAX of the seed, the seed itself is returned
    with converged=False. Returns a dict; `f0` is the value to use."""
    seed_f0, nsteps = step_f0_detect(x, sr)
    detected = seed_f0 is not None
    # Detector sanity gate: the reset detector is exact on the product's naive composite (one 4/3*A jump
    # per period, no ringing) but its level-crossing rule misfires on a BAND-LIMITED signal, whose Gibbs
    # ringing places several sub-threshold crossings around each reset and makes the mean-period estimate
    # a large multiple of the truth. Those readings are incompatible with the coarse seed by orders of
    # magnitude, so gate on that and fall back rather than searching around a garbage seed. (Measured:
    # 1022.7 Hz reported for a band-limited composite built at 123.4567 Hz.)
    seed_src = "reset-detector"
    if not detected:
        # Fail closed to the caller's seed. step_f0_detect returns (None, n) when the window holds too
        # few resets (high sr / low f0: 96000 Hz at 14 Hz is only ~2.4 periods in 16384 samples), and
        # the fallback MUST assign seed_f0 -- leaving the None in place makes the positivity guard
        # below raise TypeError instead of taking the no-steps path it was written for.
        seed_f0 = float(seed)
        seed_src = "seed(no-steps)"
    elif not (seed > 0.0) or abs(seed_f0 - seed) / seed > F0_ALIGN_DETECT_TOL:
        seed_f0 = float(seed)
        seed_src = "seed(detector-rejected)"
    if not (seed_f0 > 0.0):
        return {"f0": float(seed), "seed_f0": float(seed), "detected": False, "nsteps": nsteps,
                "seed_src": "none", "evals": 0, "widened": 0, "converged": False, "half": 0.0,
                "vertex_rel": 0.0, "min_db": None, "reason": "no-seed"}

    half = F0_ALIGN_REL_STEP if seed_src == "reset-detector" else F0_ALIGN_REL_STEP_COARSE
    center = seed_f0
    evals = 0
    widened = 0
    trace = []
    min_db = None
    last_vertex = 0.0
    converged = False
    reason = "stage-failed"
    for stage in range(F0_ALIGN_STAGES):
        vertex = None
        for w in range(F0_ALIGN_WIDEN + 1):
            if half > F0_ALIGN_SPAN_MAX or evals + 3 > F0_ALIGN_MAX_EVAL:
                break
            offs = (-half, 0.0, half)
            ys = []
            for o in offs:
                m = method_bl(x, sr, center * (1.0 + o), ref_kind=ref_kind)
                evals += 1
                ys.append(m["blref_full_db"] if m is not None else float("inf"))
            j = min(range(3), key=lambda i: ys[i])
            if 0 < j < 2 and all(math.isfinite(v) for v in ys):
                (x0, y0), (x1, y1), (x2, y2) = (offs[0], ys[0]), (offs[1], ys[1]), (offs[2], ys[2])
                den = (x0 - x1) * (x0 - x2) * (x1 - x2)
                if den != 0.0:
                    a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / den
                    b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / den
                    if a > 0.0:
                        vertex, min_db, widened = -b / (2.0 * a), float(ys[j]), w
                        break
            half *= 3.0
        if vertex is None:
            reason = ("eval-budget" if evals >= F0_ALIGN_MAX_EVAL else "no-interior-minimum")
            break
        newc = center * (1.0 + vertex)
        if abs(newc - seed_f0) / seed_f0 > F0_ALIGN_SPAN_MAX:
            reason = "left-convergence-domain"
            break
        center = newc
        last_vertex = vertex
        trace.append("%.1e->%+.2e" % (half, vertex))
        converged = (stage == F0_ALIGN_STAGES - 1)
        half *= F0_ALIGN_SHRINK
    # Fail closed: on any stage that could not bracket an interior minimum the best centre found so far
    # is returned flagged converged=False. The caller still measures, but reports the cell as unaligned
    # rather than silently presenting a number whose f0 was never aligned.
    return {"f0": float(center), "seed_f0": float(seed_f0), "detected": detected, "nsteps": nsteps,
            "seed_src": seed_src, "evals": evals, "widened": widened, "converged": converged,
            "half": half, "vertex_rel": float(last_vertex), "min_db": min_db,
            "trace": ";".join(trace), "reason": None if converged else reason}


def naive_composite(n, sr, f0, A_amp, phi=0.0):
    """The PRODUCT's shape: a naive sawtooth through the cubic, i.e. nonlinearity(sawtooth(phase)).
    y = x - x^3/3 is monotone in x, so this has exactly one 4/3*A downward jump per period and NO
    ringing -- which is what step_f0_detect is built for. (The band-limited reference is the wrong
    signal to test the detector on: its Gibbs ringing puts several sub-threshold crossings around each
    reset, and the detector then reports a multiple of the true frequency.)"""
    x = [2.0 * (((i * f0 / sr) + phi) % 1.0) - 1.0 for i in range(n)]
    return [A_amp * (v - (1.0 / 3.0) * v * v * v) for v in x]


def self_check_align():
    """Self-check for the f0 aligner's determinism identity [@Kimi pin 1]. Returns a list of failure
    strings (empty == pass).

    The aligner is claimed to be a pure, deterministic function of (x, sr, seed). If two calls on the
    same input differ at all, the pipeline contains non-determinism -- and then the "去修正负控" (where
    the fix is removed, so both arms are byte-identical and the measured gain must be EXACTLY zero)
    could not be evaluated as an identity at all. This check is what makes that negative control a
    determinate test rather than a tolerance."""
    fails = []
    sr = 44100.0
    n = 4096
    # a frequency with no exact-integer relationship to sr, so the aligner's non-leakage-free branch
    # (the one the classic cells actually take) is exercised.
    f_hz = 123.4567

    # (a) the detector, on the shape it will actually see: the naive product composite.
    naive = naive_composite(n, sr, f_hz, 0.7)
    d0, nsteps = step_f0_detect(naive, sr)
    if d0 is None:
        fails.append("align-selftest: reset detector found no steps on a naive composite "
                     "(%d periods in the window)" % nsteps)
    elif abs(d0 - f_hz) / f_hz > 1.0e-4:
        fails.append("align-selftest: reset detector returned %.6f for a composite built at %.6f "
                     "(%.2e relative)" % (d0, f_hz, abs(d0 - f_hz) / f_hz))

    # (b) the aligner, on the signal it will actually see as a reference: the band-limited ideal, which
    # IS its own reference, so the residual minimum is known to sit at the built frequency. The seed is
    # the coarse estimator the pipeline actually passes, not a hand-picked value.
    ref, _ = bandlimited_sawcubic(n, sr, f_hz, 0.7, 0.31)
    seed = refine_f0(ref, sr, 123.0)
    f0a = align_f0(ref, sr, seed, ref_kind="sawcubic")
    f0b = align_f0(ref, sr, seed, ref_kind="sawcubic")
    if f0a["f0"] != f0b["f0"] or f0a["evals"] != f0b["evals"]:
        fails.append("align-selftest: align_f0 is not deterministic on identical input "
                     "(%r vs %r) -- the 去修正 negative control could not be an identity" % (f0a, f0b))
    # the Gibbs-ringing misfire must be caught by the sanity gate, not searched around.
    if f0a["seed_src"] != "seed(detector-rejected)":
        fails.append("align-selftest: detector sanity gate did not reject the band-limited reading "
                     "(seed_src=%s, detector=%r, seed=%.6f)"
                     % (f0a["seed_src"], f0a["detected"], f0a["seed_f0"]))
    if not f0a["converged"]:
        fails.append("align-selftest: aligner did not converge on an ideal composite at %.6f "
                     "(seed=%.6f reason=%s)" % (f_hz, seed, f0a["reason"]))
    elif abs(f0a["f0"] - f_hz) / f_hz > 1.0e-3:
        fails.append("align-selftest: aligner moved seed %.6f -> %.6f for a composite built at %.6f "
                     "(%.2e relative)" % (seed, f0a["f0"], f_hz, abs(f0a["f0"] - f_hz) / f_hz))

    # (c) the aligner must also converge on the NAIVE arm -- not to the right PLACE (the naive signal is
    # not the reference's shape, so where its minimum sits is exactly what the report must measure), but
    # to an interior minimum. A silent fallback to the raw seed on the arm under test would make the
    # naive-vs-corrected difference an artefact of one arm being unaligned.
    f0n = align_f0(naive, sr, seed, ref_kind="sawcubic")
    if not f0n["converged"]:
        fails.append("align-selftest: aligner did not converge on the naive product composite "
                     "(reason=%s) -- the arm under test would be left unaligned" % f0n["reason"])

    # (d) the no-steps fallback path. step_f0_detect needs at least F0_ALIGN_MIN_STEPS resets in the
    # window, which high-sr/low-f0 classic cells cannot supply (96000 Hz at ~14 Hz is about 2.4 periods
    # in 16384 samples). That path must fall back to the caller's seed and stay inside the convergence
    # domain -- it must not raise, and it must not silently return an unset f0. Regression: the first
    # version of this fallback left seed_f0 as the detector's None and died on the positivity guard.
    z = naive_composite(512, sr, 14.0, 0.7)
    d_none, zsteps = step_f0_detect(z, sr)
    if d_none is not None:
        fails.append("align-selftest: no-steps case is not being exercised -- the detector returned "
                     "%.4f on a %.2f-period window" % (d_none, 512 * 14.0 / sr))
    f0z = align_f0(z, sr, 14.0, ref_kind="sawcubic")
    if not (isinstance(f0z["f0"], float) and f0z["f0"] > 0.0):
        fails.append("align-selftest: aligner did not fall back to a usable seed when the detector "
                     "found no steps (f0=%r seed_src=%s)" % (f0z["f0"], f0z["seed_src"]))
    elif abs(f0z["f0"] - 14.0) / 14.0 > F0_ALIGN_SPAN_MAX + 1e-12:
        fails.append("align-selftest: no-steps fallback left the convergence domain "
                     "(seed 14.0 -> %.6f)" % f0z["f0"])
    return fails


def self_check_pinned():
    """Self-check for the PINNED-phi acceptance instrument. Returns a list of failure strings.

    The load-bearing regression required by ruling 2026-09-11 item 1: a synthetic composite rendered
    from absolute sample 0 at phase 0 and WINDOWED at absolute sample `warm` must be recovered by the
    pinned instrument at the numerical floor. That validates the pin formula and the reference's own
    phase convention TOGETHER, end to end -- it is not a restatement of the formula.

    Three assertions:
      (1) END-TO-END RECOVERY: pinned residual on an exactly-matching composite is at the floor, and
          the A-fitted scale equals the built scale.
      (2) DISCRIMINATION (negative control): put a genuine, unknown extra phase error into the signal.
          The pinned column must DEGRADE by roughly that error's energy while the phi-fitted column
          must NOT (the fit absorbs it). A pin that were a no-op, or a fit that did not absorb phase,
          fails here -- this is what makes (1) meaningful rather than vacuous.
      (3) SIGNAL-INDEPENDENCE (structural): the pin is a function of (f0_snap, sr, warm) only. Two
          DIFFERENT windows at the same (f0_snap, sr, warm) must receive the identical pinned phi.
          This is the property that stops the instrument from absorbing the effect under test.
    """
    fails = []
    sr = 44100.0
    f0 = 219.5                 # deliberately not integer-periodic in the window
    warm = 1024
    n = 2048
    A_amp = 0.7
    if abs(n * f0 / sr - round(n * f0 / sr)) <= SAWCUBIC_IDENT_TOL:
        fails.append("pin-selftest: the synthetic window is leakage-free; it must be a leaky one")

    clean, _ = bandlimited_sawcubic(warm + n, sr, f0, A_amp, 0.0)   # phase 0 at absolute sample 0
    win_clean = clean[warm:]
    got = sawcubic_pinned(win_clean, sr, f0, warm)
    if got is None:
        fails.append("pin-selftest: pinned instrument returned None on a clean composite")
        return fails
    db_pin_clean, A_pin, phi_pin = got
    if phi_pin != sawcubic_pin_phi(f0, sr, warm):
        fails.append("pin-selftest: pinned phi is not the analytic pin")
    if not (db_pin_clean < SAWCUBIC_PIN_FLOOR_DB):
        fails.append("pin-selftest: pinned residual %.1f dB on an exactly-matching composite "
                     "(floor %.0f dB) -- the pin does not reproduce the reference convention"
                     % (db_pin_clean, SAWCUBIC_PIN_FLOOR_DB))
    if abs(A_pin - A_amp) / A_amp > SAWCUBIC_PIN_SCALE_TOL:
        fails.append("pin-selftest: pinned scale %.9f != built %.9f" % (A_pin, A_amp))

    # (2) the same window with an extra, unmodelled phase error of dphi radians
    dphi = 0.02
    bad, _ = bandlimited_sawcubic(warm + n, sr, f0, A_amp, dphi)
    win_bad = bad[warm:]
    gotb = sawcubic_pinned(win_bad, sr, f0, warm)
    if gotb is None:
        fails.append("pin-selftest: pinned instrument returned None on the phase-erroneous composite")
    else:
        db_pin_bad = gotb[0]
        if abs(gotb[2] - phi_pin) > 0.0:
            fails.append("pin-selftest: the pin moved with the signal (phi %.9f -> %.9f)"
                         % (phi_pin, gotb[2]))
        if db_pin_bad - db_pin_clean < SAWCUBIC_PIN_DISCRIM_DB:
            fails.append("pin-selftest: pinned column barely moved on a %.3f rad phase error "
                         "(%.1f -> %.1f dB, need >= %.0f dB) -- the pin is absorbing phase"
                         % (dphi, db_pin_clean, db_pin_bad, SAWCUBIC_PIN_DISCRIM_DB))
    fit_clean = sawcubic_fit(n, sr, f0, proj(win_clean, sr, f0))
    fit_bad = sawcubic_fit(n, sr, f0, proj(win_bad, sr, f0))
    if fit_clean is None or fit_bad is None:
        fails.append("pin-selftest: the phi-fitted comparator returned None")
    else:
        rc = sawcubic_resid_db(win_clean, sr, f0, fit_clean[0], fit_clean[1])
        rb = sawcubic_resid_db(win_bad, sr, f0, fit_bad[0], fit_bad[1])
        if rb - rc > SAWCUBIC_FIT_ABSORB_DB:
            fails.append("pin-selftest: the phi-FITTED column also degraded (%.1f -> %.1f dB) on a "
                         "pure phase error; the non-absorption contrast is not being exercised"
                         % (rc, rb))

    # (3) structural: the pin cannot see the signal
    other, _ = bandlimited_sawcubic(warm + n, sr, f0, 0.25, 1.1)
    got_o = sawcubic_pinned(other[warm:], sr, f0, warm)
    if got_o is None or got_o[2] != phi_pin:
        fails.append("pin-selftest: pinned phi depends on the signal under test")
    return fails


FIDELITY_COLS = ("id", "samples", "rt_vs_selfON_max_abs_diff", "bit_identical",
                 "driftON_vs_OFF_max_abs_diff", "drift_off_differs")


def _fidelity_fails(cid, row, idx, samples_expected):
    """Fail reasons for ONE `gh19_fidelity.tsv` row. Pure, so the check below can also feed it
    hand-built rows and prove it can go red. `row` is the raw column list."""
    out = []
    if row[idx["samples"]] != samples_expected:
        out.append("probe-contract: %s fidelity row says samples=%s but the cell rendered %s"
                   % (cid, row[idx["samples"]], samples_expected))
    if row[idx["bit_identical"]] != "YES":
        out.append("probe-contract: %s runtime-vs-probe-built drift-ON raw is NOT bit-identical "
                   "(bit_identical=%s, max_abs_diff=%s) -- the probe-built bank has forked from the "
                   "shipped runtime path" % (cid, row[idx["bit_identical"]],
                                             row[idx["rt_vs_selfON_max_abs_diff"]]))
    if row[idx["drift_off_differs"]] != "YES":
        out.append("probe-contract: %s flipping driftEnabled did NOT change the render "
                   "(driftON_vs_OFF_max_abs_diff=%s) -- the drift-off acceptance arm is vacuous"
                   % (cid, row[idx["driftON_vs_OFF_max_abs_diff"]]))
    return out


def self_check_probe_contract(dirpath, cells):
    """Self-check for the PROBE-side provenance contract (ruling B, @Kimi `f260fd91`).

    Ruling B takes the classic cells' render off the runtime path and onto a probe-built DroneBank,
    because only a self-built bank can be constructed with `driftEnabled=false` -- the arm on which
    the accumulator advances at a constant frequency and the analytic f0/phi pin is exact. That move
    creates exactly ONE real risk: the probe-built bank could silently diverge from the shipped
    runtime path (a different norm mapping, a different default, a control the probe forgot to set),
    and every classic measurement would then be taken on a signal the product does not produce.

    The probe renders each classic cell BOTH ways and emits `gh19_fidelity.tsv`. This check is
    fail-closed on it:
      (1) the file exists and every produced classic cell has a row with the right sample count;
      (2) bit_identical == YES -- the runtime raw equals the probe-built drift-ON raw, bit for bit.
          This is the fork detector, and it is the reason the provenance swap is admissible at all;
      (3) drift_off_differs == YES -- flipping ONLY the drift flag actually changed the render. A
          NO here would mean the "drift-off acceptance arm" is not the drift-off arm and the whole
          measurement is vacuous (the non-vacuity control, fail-closed like the rest);
      (4) every classic cell carries a POSITIVE finite f0_model_hz -- without it the pinned column
          has no frequency to pin, and the acceptance reading is undefined rather than merely absent.
    (5) the row predicate itself is exercised on hand-built good/bad rows, so (2)/(3) cannot be
        unreachable-by-construction.
    """
    fails = []
    classic = [c for c in cells if c["produced"] and is_classic_saw(c["path"])]
    if not classic:
        fails.append("probe-contract: no produced classic saw cell in the cache; the provenance "
                     "assertion has no subject")
        return fails

    fpath = os.path.join(dirpath, "gh19_fidelity.tsv")
    if not os.path.exists(fpath):
        fails.append("probe-contract: gh19_fidelity.tsv is missing -- the classic cells' provenance "
                     "(probe-built DroneBank, driftEnabled=false, ruling B) is unverified, so their "
                     "acceptance column is not admissible evidence")
        return fails

    with open(fpath) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        fails.append("probe-contract: gh19_fidelity.tsv is empty")
        return fails
    header = lines[0].split("\t")
    missing = [n for n in FIDELITY_COLS if n not in header]
    if missing:
        fails.append("probe-contract: gh19_fidelity.tsv lacks column(s) %s" % ", ".join(missing))
        return fails
    idx = {n: i for i, n in enumerate(header)}
    rows = {}
    for line in lines[1:]:
        cols = line.split("\t")
        if len(cols) < len(header):
            continue
        rows[cols[idx["id"]]] = cols

    for c in classic:
        cid = c["id"]
        row = rows.get(cid)
        if row is None:
            fails.append("probe-contract: %s has no fidelity row" % cid)
            continue
        fails.extend(_fidelity_fails(cid, row, idx, c.get("samples", "")))

        raw_model = c.get("f0_model_hz", "")
        try:
            model = float(raw_model)
            model_ok = math.isfinite(model) and model > 0.0
        except (TypeError, ValueError):
            model_ok = False
        if not model_ok:
            fails.append("probe-contract: %s has no positive finite f0_model_hz (got %r) -- the "
                         "pinned acceptance column has no frequency to pin" % (cid, raw_model))

    # (5) the predicate is not unreachable: a good row passes, a forked row and a vacuous row fail.
    pidx = {n: i for i, n in enumerate(FIDELITY_COLS)}
    if _fidelity_fails("x", ["x", "24576", "0", "YES", "3.2", "YES"], pidx, "24576"):
        fails.append("probe-contract: the fidelity predicate rejects a good hand-built row; the "
                     "assertion is over-broad and would fail on correct data")
    if not _fidelity_fails("x", ["x", "24576", "1.0e-06", "NO", "3.2", "YES"], pidx, "24576"):
        fails.append("probe-contract: the fidelity predicate accepts a hand-built FORKED row")
    if not _fidelity_fails("x", ["x", "24576", "0", "YES", "0", "NO"], pidx, "24576"):
        fails.append("probe-contract: the fidelity predicate accepts a hand-built VACUOUS row")
    return fails


def sawcubic_resid_db(nx, sr, f0_snap, A_amp, phi):
    """The time-domain residual ratio for a reference built at an EXPLICIT (A, phi). Same quantity as
    blref_full_db / blpin_full_db, factored out so the self-check can read the fitted comparator."""
    ref, _ = bandlimited_sawcubic(len(nx), sr, f0_snap, A_amp, phi)
    resid = 0.0
    tot = 0.0
    for i in range(len(nx)):
        e = nx[i] - ref[i]
        resid += e * e
        tot += nx[i] * nx[i]
    if tot <= 0.0 or resid <= 0.0:
        return float("-inf")
    return 10.0 * math.log10(resid / tot)


def method_bl(x, sr, f0, band_lo=BAND_LO, band_hi=BAND_HI, window_fn=intperiod_window,
              ref_kind="tri", pin_warm=None, pin_f0=None):
    """Real per-sample product-minus-band-limited-reference reconciliation for a clean triangle cell.
    Returns the in-band aliasing figure referenced to the PRODUCT's in-band power (the authoritative
    ratio), a full-band residual, and a per-harmonic shape-verification metric. The reference scale is
    the MEASURED fundamental (a1_mag): the aliasing ratio is scale-invariant (a halved output gives the
    same dBc), which is CORRECT for an alias ratio; the independent device-scale CONTRACT is a separate
    raw-side check (dry_triangle_scale_contract), not a reference-side scale override.

    `window_fn` selects the leakage-free window: the integer-Hz snap by default (correct for the VCO
    cells, whose nominal f0 IS an integer Hz), or realized_period_window for an integer-sample-periodic
    path whose realized frequency is fractional (the Schmitt audio path).

    `ref_kind` selects the reference family and its shape authority:
      "tri"      -- the ideal triangle (BLAMP/S1 path and the VCO cells); scale = the measured a1_mag.
      "sawcubic" -- the classic drone saw->cubic composite (S2); the fundamental is fitted through the
                    composite's closed-form projection gain H(phi) so that Dirichlet leakage from the
                    other harmonics is compensated rather than mistaken for aliasing. On a leakage-free
                    window the fit IS the plain convention, bit-exactly (see self_check_sawcubic).

    `pin_f0` separates the PIN's frequency from the reference's. Both default to the window's
    `f0_snap`, but the acceptance column needs them apart: `f0_snap` (from the aligner's 1-D
    minimisation) is right for the DIAGNOSTIC blref_* columns, whose job is the best free
    constant-frequency fit, while the PIN must be fed the frequency the oscillator actually ran at --
    on the S2 acceptance arm that is a probe-measured model value (ruling B, @Kimi f260fd91), not a
    minimisation result. Feeding the pin the minimised f0 would move the absorption from phi to f0
    (the same defect in new clothes), which is exactly what the pin exists to exclude."""
    win = window_fn(x, sr, f0)
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
    A_amp = None
    phi = None
    leak_free = None
    if ref_kind == "sawcubic":
        leak_free = sawcubic_leakage_free(n, sr, f0_snap)
        fit = sawcubic_fit(n, sr, f0_snap, a1)
        if fit is None:
            return None
        A_amp, phi = fit
        if not (A_amp > 0.0):
            return None
        ref, ks = bandlimited_sawcubic(n, sr, f0_snap, A_amp, phi)
    else:
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

    # --- PINNED-phi acceptance column (ruling 2026-09-11 #1) -------------------------------------------
    # When the caller supplies the window's true start sample (`pin_warm`) AND we are in the classic
    # saw->cubic family, read the residual at the EXPLICIT phase the product's own accumulator started
    # that window with, fitting ONLY the scale A. The `phi` used above is a FITTED free parameter and
    # absorbs the effect under test (scratch/s2_underread_split.py: d_ph is the whole low-read while
    # d_amp is identically 0.00 dB), which is why blref_full_db reports a real -9.92 dB improvement as
    # +0.43 dB on 441 Hz. This column has no free phase parameter, so it cannot do that.
    blpin_full_db = None
    blpin_amp = None
    blpin_phi_rad = None
    if ref_kind == "sawcubic" and pin_warm is not None:
        f0_pin = f0_snap if pin_f0 is None else float(pin_f0)
        pin = sawcubic_pinned(nx, sr, f0_pin, pin_warm)
        if pin is not None:
            blpin_full_db, blpin_amp, blpin_phi_rad = pin

    # shape verification: max relative deviation of the product's in-band harmonic MAGNITUDES from the
    # reference's ideal ratio -- a_tri(k)/a_tri(1) for the triangle, |c_k|/|c_1| for the composite. Small
    # => the product IS the ideal shape at the measured scale+phase and the reference attribution
    # (aliasing == residual) is valid; large => the reference does not describe the product (wrong shape /
    # wrong scale), so blref is NOT a valid alias figure for this cell.
    shape_max = -float("inf")
    for k in ks:
        ideal_mag = (A_amp * abs(sawcubic_c(k)) / 2.0) if ref_kind == "sawcubic" \
            else (scale * a_tri(k) / a_tri(1))
        if ideal_mag > 0:
            meas_mag = abs(proj(nx, sr, k * f0_snap))
            dev = abs(meas_mag - ideal_mag) / ideal_mag
            shape_max = max(shape_max, 20.0 * math.log10(dev if dev > 0 else 1e-300))
    return {"f0_snap": f0_snap, "kfull": len(ks), "a1_mag": a1_mag, "arg_a1": arg_a1,
            "blref_full_db": full_db, "blref_inband_db": inband_db, "blshape_max_db": shape_max,
            "resid_inband_p2": pe, "prod_inband_p2": px, "prod_total_p2": total,
            "n": n, "Nperiods": int(round(n * f0_snap / sr)), "gap": gap,
            "ref_kind": ref_kind, "A_amp": A_amp, "phi": phi, "leakage_free": leak_free,
            "blpin_full_db": blpin_full_db, "blpin_amp": blpin_amp, "blpin_phi_rad": blpin_phi_rad}


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
# Classic drone saw->cubic cells (GH#19 S2). The role list grows with the probe matrix -- a HIGH-role
# cell (drone_4) is approved for addition -- so match on the role + suffix rather than one literal id.
CLASSIC_SAW_ROLES = ("drone1", "drone4")


def is_classic_saw(path):
    return any(path.startswith(role + "_classic") for role in CLASSIC_SAW_ROLES)


def cat_of(path):
    if path.startswith("vco_a_tri") or path.startswith("vco_b_tri"):
        return "triangle(analytic_target)"
    if path == "preamp_ac":
        return "nonlinear(preamp_tanh)"
    if path == "wet_chain":
        return "composite(kVcfPath)"
    if is_classic_saw(path):
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
    xs = read_raw(raw)
    # warm = how many samples the probe dropped before the analysis window begins. The pinned-phi
    # instrument needs it to reconstruct the window's TRUE start phase (see sawcubic_pin_phi); the
    # probe raw is kWarm+kWin and steady() keeps the LAST STEADY of it, so warm = len - STEADY.
    warm = max(0, len(xs) - STEADY)
    x = steady(xs)
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
    elif path.startswith("drone3_schmitt") or path.startswith("drone6_schmitt"):
        # Schmitt audio path: integer-sample-periodic steady state at a FRACTIONAL realized frequency
        # sr/(2M), so the BL reference must be anchored on the realized period rather than the
        # integer-Hz snap (which reports gap!=0 / NaN for all 12 of these cells). No theory_dedup_db:
        # the fold-table metric is defined for the VCO cells and is not claimed here.
        out["theory_dedup_db"] = "-"
        mbl = method_bl(x, sr, f0, window_fn=realized_period_window)
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
    elif is_classic_saw(path):
        # Classic drone saw->cubic (S2): a VALUE-jump saw under the cubic nonlinearity -- the polyBLEP
        # family, not the BLAMP family of the Schmitt triangle. No leakage-free window exists for these
        # cells (intperiod_window reports gap=NaN for all 12 -- f0 is not an integer Hz -- and the drift /
        # jitter modulation makes realized_period_window fail closed), so the reference cannot rely on an
        # exactly periodic window and instead compensates the projection's Dirichlet leakage in closed
        # form (sawcubic_fit). No theory_dedup_db: the fold-table metric is defined for the VCO cells and
        # is not claimed here.
        out["theory_dedup_db"] = "-"
        # f0 for THIS branch is aligned by 1-D minimization of blref_full itself (ruling (A)), seeded
        # from the reset detector. Only the sawcubic column set consumes it: method_a above keeps the
        # refine_f0 value, so every pre-existing column of every cell is untouched (S1 zero-disturbance).
        al = align_f0(x, sr, f0, ref_kind="sawcubic")
        out["blf0_aligned_hz"] = al["f0"]
        out["blf0_seed_hz"] = al["seed_f0"]
        out["blf0_steps"] = al["nsteps"]
        out["blf0_evals"] = al["evals"]
        out["blf0_converged"] = al["converged"]
        out["blf0_src"] = al["seed_src"]
        # The ACCEPTANCE column's frequency is NOT the aligner's. It is the model value the probe
        # measured off the very DroneBank instance it rendered with, on a bank whose drift model was
        # disabled (ruling B, @Kimi f260fd91): the accumulator then advances at exactly
        # freqBase*2^(tune/12)*2^(-volt/12)*(1+tolerance) -- a constant -- so phi enters as the exact
        # analytic 2*pi*frac(warm*f0/sr) and the ONLY free parameter left is the scale A. On a cache
        # with no model f0 (any probe predating ruling B) the pinned columns are reported as "-": the
        # pinned instrument is defined on the constant-frequency arm only, and reporting it off a
        # frequency-modulated arm would be a reading, not a measurement.
        f0_pin = None
        raw_model = rec.get("f0_model_hz", "")
        try:
            cand = float(raw_model)
            if cand > 0:
                f0_pin = cand
        except (TypeError, ValueError):
            f0_pin = None
        out["blf0_model_hz"] = f0_pin
        mbl = method_bl(x, sr, al["f0"], ref_kind="sawcubic",
                        pin_warm=(warm if f0_pin is not None else None), pin_f0=f0_pin)
        if mbl:
            out["blref_inband_db"] = (fmt_db(mbl["blref_inband_db"]) if mbl["blref_inband_db"] ==
                                      mbl["blref_inband_db"] else "-")
            out["blref_full_db"] = (fmt_db(mbl["blref_full_db"]) if mbl["blref_full_db"] ==
                                    mbl["blref_full_db"] else "-")
            out["blshape_max_db"] = (fmt_db(mbl["blshape_max_db"]) if mbl["blshape_max_db"] ==
                                     mbl["blshape_max_db"] else "-")
            out["blpin_full_db"] = fmt_db(mbl["blpin_full_db"])
            out["blpin_amp"] = ("%.9g" % mbl["blpin_amp"]) if mbl["blpin_amp"] is not None else "-"
            out["blpin_phi_rad"] = ("%.9f" % mbl["blpin_phi_rad"]) \
                if mbl["blpin_phi_rad"] is not None else "-"
        else:
            out["blref_inband_db"] = "-"
            out["blref_full_db"] = "-"
            out["blshape_max_db"] = "-"
            out["blpin_full_db"] = "-"
            out["blpin_amp"] = "-"
            out["blpin_phi_rad"] = "-"
    else:
        out["theory_dedup_db"] = "-"
        out["blref_inband_db"] = "-"
        out["blref_full_db"] = "-"
        out["blshape_max_db"] = "-"
        out["blpin_full_db"] = "-"
        out["blpin_amp"] = "-"
        out["blpin_phi_rad"] = "-"
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
def label_contract(rec, x, mrec=None):
    """Return (ok, reason, detail). `rec` is a scenario record carrying `sr_hz`, `f0_target_hz` and the
    probe-independent `f0_meas_hz`; `x` is the steady-state raw; `mrec` (optional) is the INDEPENDENT
    manifest row carrying `sr` and `stim` (the product-reachable target frequency). Compares:
      (0) if mrec given, the declared sr and f0_target against the manifest expectation (EXTERNAL ground
          truth). This breaks the self-consistency hole @Codex 1175c321 flagged: a record whose sr AND
          f0_target (and f0_meas) are all written wrong-but-consistent would otherwise pass checks (1)/(2).
      (1) declared f0_target vs the independently-measured f0_meas_hz (within F0_LABEL_REL_TOL), and
      (2) declared sr self-consistency: the per-sample band-limited reference must reconcile under the
          declared sr (a wrong sr breaks the integer-period window / reference alignment), AND the
          fundamental re-derived in Hz under the declared sr must agree with f0_meas_hz.
    A correct label set passes all; a corrupted sr or f0 label fails => REJECT."""
    sr_lab = float(rec["sr_hz"])
    f0_tgt = float(rec["f0_target_hz"])
    f0_meas = float(rec["f0_meas_hz"])
    # (0) independent manifest expectation (external ground truth; do this FIRST so a record whose
    #     sr AND f0_meas are both rewritten consistently cannot self-pass):
    if mrec is not None:
        man_sr = float(mrec["sr"])
        man_f0 = float(mrec["stim"])
        if abs(sr_lab / man_sr - 1.0) > F0_LABEL_REL_TOL:
            return (False, "sr-label-manifest",
                    "declared sr %.0f != independent manifest sr %.0f (rel dev %.1f%%)"
                    % (sr_lab, man_sr, abs(sr_lab / man_sr - 1.0) * 100.0))
        if abs(f0_tgt / man_f0 - 1.0) > F0_LABEL_REL_TOL:
            return (False, "f0-label-manifest",
                    "declared f0_target %.2f Hz != independent manifest target %.2f Hz (rel dev %.2f%%)"
                    % (f0_tgt, man_f0, abs(f0_tgt / man_f0 - 1.0) * 100.0))
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
           "blref_inband_db\tblref_full_db\tblshape_max_db\t"
           "blpin_full_db\tblpin_amp\tblpin_phi_rad\tblf0_model_hz\t"
           "blf0_aligned_hz\tblf0_seed_hz\tblf0_steps\tblf0_evals\tblf0_converged\tblf0_src\t"
           "theory_dedup_db\t"
           "N\tperiods\tgap\tcategory")
    print(hdr)
    _stub9 = ("-", "-", "-", "-", "-", "-", "-", "-", "-", "-")   # 4 blpin_* + 6 blf0_*
    for cid in sorted(required) + [c["id"] for c in cells if c["id"] not in required]:
        rec = by_id.get(cid)
        if rec is None:
            # a manifest-required id with no row is already a coverage error; emit a stub row.
            m = required.get(cid)
            print("\t".join([cid, (m["path"] if m else "-"), (m["sr"] if m else "-"),
                             "-", "-", "-", "-", "-", "-", "-"] + list(_stub9) +
                            ["-", "-", "-", "-", "MISSING"]))
            continue
        if not rec["produced"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-",
                             "-", "-", "-", "-"] + list(_stub9) +
                            ["-", "-", "-", "-",
                             "not-produced:" + rec.get("signal", "")]))
            continue
        a = analyze_cell(args.dir, rec)
        if a["err"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-",
                             "-", "-", "-", "-"] + list(_stub9) +
                            ["-", "-", "-", "-", a["err"]]))
            # a required cell that is produced but invalid is a hard FAIL (not an info line).
            if cid in required:
                gate_errors.append(f"invalid-cell {cid} [{a['err']}]")
            continue
        print("\t".join([
            cid, rec["path"], str(int(a["sr"])), str(int(a["f0_target"])),
            ("%.2f" % a["f0"]) if a["f0"] > 0 else "-",
            a["harmris"], a["harmris_inband"],
            a.get("blref_inband_db", "-"), a.get("blref_full_db", "-"),
            a.get("blshape_max_db", "-"),
            a.get("blpin_full_db", "-"), a.get("blpin_amp", "-"),
            a.get("blpin_phi_rad", "-"),
            ("%.6f" % a["blf0_model_hz"]) if a.get("blf0_model_hz") else "-",
            ("%.5f" % a["blf0_aligned_hz"]) if "blf0_aligned_hz" in a else "-",
            ("%.5f" % a["blf0_seed_hz"]) if "blf0_seed_hz" in a else "-",
            str(a["blf0_steps"]) if "blf0_steps" in a else "-",
            str(a["blf0_evals"]) if "blf0_evals" in a else "-",
            ("yes" if a["blf0_converged"] else "no") if "blf0_converged" in a else "-",
            a.get("blf0_src", "-"),
            a.get("theory_dedup_db", "-"),
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

            # ------------------------------------------------------------------
            # S2 ENTRY — the classic saw->cubic reference (bandlimited_sawcubic) and its leakage
            # compensation. This is a reference-legitimacy check, not a product judgment: it asserts the
            # closed form, the inversion, AND the zero-disturbance property the S1 cells depend on.
            # Failure here means the classic cells' blref columns are not admissible evidence.
            # ------------------------------------------------------------------
            sc_fail = self_check_sawcubic()
            check_fails.extend(sc_fail)
            for f in sc_fail:
                print("  S2  %s" % f)
            if not sc_fail:
                print("  S2  sawcubic reference: PASS (closed form, inversion, integer-bin bit-exact identity)")

            # The f0 aligner (ruling (A)) must be a pure determinism identity: the naive-vs-corrected
            # comparison is only admissible if, with the correction removed, the two arms are
            # byte-identical AND the shared alignment pipeline maps that identical input to an identical
            # f0. A failure here means the pipeline is non-deterministic, which would make the
            # "去修正负控" untestable as an identity. [@Kimi pin 1]
            al_fail = self_check_align()
            check_fails.extend(al_fail)
            for f in al_fail:
                print("  S2  %s" % f)
            if not al_fail:
                print("  S2  f0 aligner: PASS (deterministic; detector recovers a built f0 to <1e-4 rel)")

            # The PINNED-phi acceptance column (ruling 2026-09-11 #1). The hard requirement is the
            # in-gate self-check that a synthetic composite with a KNOWN phase must be recovered exactly
            # (at the floor), that a genuine unmodelled phase error must DEGRADE the pinned column while
            # the phi-fitted comparator ABSORBS it, and that the pin cannot see the signal under test.
            # Failure here means the classic cells' acceptance column is not admissible evidence.
            pin_fail = self_check_pinned()
            check_fails.extend(pin_fail)
            for f in pin_fail:
                print("  S2  %s" % f)
            if not pin_fail:
                print("  S2  pinned-phi column: PASS (end-to-end known-phase recovery at the floor; "
                      "phase error discriminates; signal-independent)")

            # The PROBE-side provenance contract (ruling B, @Kimi `f260fd91` pin 1): the classic cells
            # are rendered by a probe-built DroneBank so that driftEnabled=false is reachable, and the
            # ONLY risk that move carries is a silent fork from the shipped runtime path. The probe
            # renders both ways; this asserts the two raws are bit-identical and that the drift flag
            # actually did something. Failure here means the classic cells' provenance is unverified
            # and their acceptance column is not admissible evidence.
            pc_fail = self_check_probe_contract(args.dir, cells)
            check_fails.extend(pc_fail)
            for f in pc_fail:
                print("  S2  %s" % f)
            if not pc_fail:
                print("  S2  probe fidelity: PASS (runtime raw == probe-built drift-ON raw bit for "
                      "bit; drift flag non-vacuous; analytic f0 present)")

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
                    # @Codex 1175c321: the device-scale contract must apply to EVERY required clean DRY
                    # triangle cell, not just the representative first one — so a mal-scaled raw anywhere
                    # in the triangular matrix is caught, not only at the one cell we inspect here.
                    scale_errs, scale_cnt = [], 0
                    for c in cells:
                        if not (c["produced"] and (c["path"].startswith("vco_a_tri") or
                                                   c["path"].startswith("vco_b_tri"))):
                            continue
                        xr = steady(read_raw(os.path.join(args.dir, c["raw"])))
                        if classify_samples(xr) is not None:
                            continue
                        scale_cnt += 1
                        sc = dry_triangle_scale_contract(xr)
                        if not sc["ok"]:
                            scale_errs.append("%s: peak=%.4f dev=%.1f%%"
                                              % (c["id"], sc["peak"], sc["dev"] * 100.0))
                    if scale_errs:
                        check_fails.append("G2 scale-matrix: %d of %d triangle cells fail the device-scale "
                                           "contract" % (len(scale_errs), scale_cnt))
                        for e in scale_errs:
                            print("    [scale]", e)
                    else:
                        print("  G2 scale-matrix: %d triangle cells satisfy the device-output-scale contract"
                              % scale_cnt)
                    # ---------------------------------------------------------------------
                    # CHECK ENTRY 2 — LABEL CONTRACT (accept/reject entry). The SAME label_contract()
                    # validates the real cell's sr/f0 declaration against the independent actual-state
                    # expectation (probe-measured f0_meas_hz), and a DUPLICATED record whose sr or f0 label
                    # is changed (raw + actual-state expectation unchanged) MUST be rejected here. This
                    # replaces the estimator-robustness test ("wrong target corrected by frequency search"),
                    # which was estimator behavior, not label validation (@Codex 1307b784).
                    # ---------------------------------------------------------------------
                    mrec_tri = required.get(tri["id"])
                    lk, lr, ld = label_contract(tri, x, mrec_tri)
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
                    # @Codex 1175c321: the INDEPENDENT manifest cross-check must be load-bearing, not a
                    # tautology. Build a record whose sr AND f0_target AND f0_meas are ALL rewritten to a
                    # self-consistent wrong value (a "both fields corrupt together" attack): the Hz the raw
                    # re-derives under the halved sr is written back into f0_target/f0_meas, so internal
                    # checks (1)/(2) agree. Only the external manifest cross-check (0) can catch it; it MUST
                    # be rejected. sr is hr-sensitive: halving the declared sr halves the re-derived Hz.
                    rec_plant = dict(tri)
                    bad_sr = sr * 0.5
                    plant_f0 = refine_f0(x, bad_sr, float(tri["f0_meas_hz"]))
                    rec_plant["sr_hz"] = "%g" % bad_sr
                    rec_plant["f0_target_hz"] = "%g" % plant_f0
                    rec_plant["f0_meas_hz"] = "%g" % plant_f0
                    lk, lr, ld = label_contract(rec_plant, x, mrec_tri)
                    if lk:
                        check_fails.append("G2 label-manifest: self-consistent corrupt sr+f0 NOT caught by "
                                           "the manifest cross-check")
                    else:
                        print("  G2 label-manifest: self-consistent wrong sr+f0 record -> rejected [%s] %s"
                              % (lr, ld))
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
                        lk, lr, ld = label_contract(c, xr, required.get(c["id"]))
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
                    # @Codex 1175c321: treat the fixed-ideal stand-in EXPLICITLY as a state-association
                    # NEGATIVE control. The state-association POSITIVE is the real triangular matrix: cells
                    # at a normal frequency AND other legal tuning states (220/440/880, vco_a_tri + the
                    # FM-tuned-away vco_b_tri) ALL track their declared state via label_contract (done above).
                    # Acceptance should NOT claim to distinguish a fully-equivalent ideal implementation
                    # (black-box output cannot); it only must catch a stand-in that IGNORES state.
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
                                    print("  G4 state-assoc (negative): fixed correct-scale ideal at 2*f0 "
                                          "(state says f0) -> rejected [%s] %s" % (lr, ld))
                                    print("  G4 state-assoc (positive): real cells at normal + other legal "
                                          "tuning states all track their declared state via label_contract; "
                                          "black-box output cannot distinguish a fully-equivalent ideal, so "
                                          "acceptance only requires a state-IGNORING stand-in to be RED.")
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
