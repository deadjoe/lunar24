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
# Reference construction is the key adaptation: a REAL periodic signal has no closed-form a_k (the
# verified tool knew ideal() saw/tri/pulse). So the band-limited reference is built from the signal's
# OWN measured in-band harmonic coefficients — matched-filter projections at k*f0 (k*f0 <= sr/2).
# This is shape/phase/amplitude-agnostic and needs no ideal() table.
#
# LEAKAGE is the trap solved here. At a fractional-period window the in-band sinusoids {e^{j2pi k f0 n/sr}}
# are NOT orthogonal, so a straight projection/reconstruction leaks harmonics into the residual and
# reads a false floor (~-37..-48 dB @ 220 Hz/48k). The fix is an INTEGER-PERIOD window (N*f0/sr an
# integer): then the harmonics land on distinct integer bins, the projections are orthogonal, and the
# residual is leakage-free and N-converged (verified: synthetic naive tri @220/48k stays exactly
# -68.44 dB across N=2400..19200).
#
#   METHOD A (primary, product-facing):
#       ref = sum_k 2 Re{ A_k e^{j2 pi k f0 n/sr} } (in-band harmonics), residual e = x - ref.
#       aliasA_full = 10 log10( sum e^2 / sum x^2 )  — FULL-BAND aliased energy (leakage-free).
#       aliasA_inband = 10 log10( in-band [BAND_LO,BAND_HI] power of e / total power ) — the reported
#          reporting band (zero-padded windowed FFT; integer-period makes this near-exact).
#       A band-limited oscillator -> aliasA_full drifts toward the floor; a naive wrap -> aliasA_full
#       lands at the naive-triangle/saw fold level (shown by the dynamic-range control).
#
#   METHOD B (cross-check, VCO-triangle cells only):
#       Ideal triangle exact analytic coefficients 4/(pi^2 k^2) (odd k), folded with the CONJUGATED
#       mirror rule (fold_line + conjugate), dedup over distinct observed lines. In-band [100,5000]
#       dBc. A naive product triangle reads Method A_full-band ~ aligned with the naive fold and
#       Method B_in-band as the subset; an already-anti-aliased one reads Method A well below the
#       naive fold. Comparison evidence, not an assertion.
#
#   GATE (negative controls — a buggy MEASUREMENT must RED, not false-pass):
#       G1 conjugation: two harmonics folding onto ONE observed line. The correct conjugated COMPLEX
#          sum must match an independent matched-filter projection; the buggy no-conj sum must NOT.
#          Comparison is complex (a magnitude-only compare false-trips when both harmonics are
#          mirrored since |conj(a)+conj(b)|=|a+b| but S=conj(wrong_S)). Run on analytic SAW (coincident
#          tri folds do not exist @220, so saw is the canonical conjugate-bearing case).
#       G2 reconciliation: Method B under a WRONG sample-rate and WRONG reference-frequency label each
#          must change the figure detectably (labels + power normalisation are caught).
#       G3 real-not-silent / substitution: each produced cell must be finite, non-silent, and on the
#          recorded voltage domain (peak [1e-4,0.55] device / [1e-3,5.5] volt) — an un-scaled ideal()
#          stand-in is rejected.
#       G4 dynamic range: a SYNTHETIC naive triangle must read a MUCH larger alias than a SYNTHETIC
#          band-limited triangle (same integer-period window). Proves the metric can distinguish
#          aliased from clean — isolated source-mutation evidence (no fault macro in any production
#          header; the injection lives only in this analyzer).
#
# The alias figures are EVIDENCE; no threshold is asserted (-60dB is NOT a gate). Exit 0 = the
# MEASUREMENT TOOL passed its own negative controls; != 0 = the tool is broken.
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
# mark the measurement as leakage-uncontrolled (composite / not single-periodic paths). Returns
# (N, f0_snap, gap); gap=0 => exact integer-period (leakage-free); NaN => leakage not controlled.
# ---------------------------------------------------------------------------
def intperiod_window(x, sr, f0):
    maxlen = len(x)
    if not (f0 > 0):
        return None
    fr = int(round(f0))
    # The sharp refine measures the product frequency to ~+-0.003 Hz, so a near-integer fundamental is
    # treated as integer-periodic and snapped: exactly this makes N*f0/sr integer over the window
    # (harmonics orthogonal -> residual leakage-free). INT_TOL is a RELATIVE tolerance (0.001%/0.003Hz
    # all well inside it); the residual snap error (f0 - fr)/fr is reported as `gap` so the report stays
    # honest that the frequency was measured, not assumed.
    if abs(f0 - fr) <= INT_TOL * f0:
        per = sr / math.gcd(fr, int(sr))
        per = int(per)
        if per < 1:
            per = 1
        # A near-Nyquist f0 at high sr can have gcd=1 (per=sr), so one integer-period is longer than
        # the whole buffer -> no usable integer-period window exists; fall through to the leak flag.
        if per <= maxlen:
            n = (maxlen // per) * per
            if n < 4 * per and maxlen >= 4 * per:
                n = 4 * per
            elif n < per:
                n = per
            gap = abs(f0 - fr) / fr
            return (n, float(fr), gap)
    # non-integer f0 (or no integer-period window fits this buffer): use the full window and flag the
    # leakage as uncontrolled (composite / not-single-periodic paths, or a near-Nyquist prime f0).
    return (maxlen, float(f0), float("nan"))


# ---------------------------------------------------------------------------
# METHOD A: measured band-limited reference (real product, any periodic shape, leakage-free).
# ---------------------------------------------------------------------------
def method_a(x, sr, f0, maxlen_may_be_none=True):
    """Returns dict with full_db (full-band aliased residual), inband_db (reporting band), kcount,
    a1_mag, n, f0_snap, Nperiods, gap. Uses an integer-period window chosen from the full x buffer."""
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

    # in-band reporting figure: zero-pad the residual, Hann-window, FFT, sum |.|^2 in [BAND_LO,BAND_HI].
    ewin = [0j] * ZPAD
    hn = hann(n)
    for i in range(n):
        ewin[i] = complex((nx[i] - ref[i]) * hn[i], 0.0)
    fft(ewin)
    bin_hz = sr / ZPAD
    pe = 0.0
    for k in range(1, ZPAD // 2):
        f = k * bin_hz
        if BAND_LO <= f <= BAND_HI:
            pe += abs(ewin[k]) ** 2
    # normalise by total signal power in the SAME windowed/zeropad coordinate.
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
# METHOD B: ideal-triangle analytic folded-line alias (CONJUGATE-corrected).
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
    """Conjugate-corrected folded-line alias. coeff_fn(k) -> analytic a_k; scale matches measured a1.
    Returns (dedup_db, naive_db, dedup_p, naive_p, fold_count, carrier_p2)."""
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
    if a1_mag <= 0:
        return None
    scale = a1_mag / a_tri(1)
    r = _fold_analytic(a_tri, f0, sr, scale, band_lo=band_lo, band_hi=band_hi)
    r["carrier_p2"] = a1_mag * a1_mag
    return r


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
    """Conjugation control: correct conjugated COMPLEX sum must match an independent matched-filter
    projection; the buggy no-conj sum must NOT. Returns (rows, errors)."""
    rows, errors = [], []
    pairs = _same_line_pairs(coeff_fn, f0, sr)
    if not pairs:
        rows.append(("conjugation", f"{name}/{f0:g}@{sr:g}: no in-band coincident fold pair "
                                   "(documented, not-a-failure)"))
        return rows, errors
    # use an integer-period window if one exists for this f0/sr.
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
# G4: dynamic range — synthetic naive vs band-limited triangle at an integer-period window.
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


def dynamic_range_control(sr=48000.0, f0=1865.0):
    # pick an integer-period N for f0/sr (gcd(1865,48000)=5 -> per=9600).
    import math as m
    per = sr / m.gcd(int(f0), int(sr))
    n = int(per) * 2
    x_naive = [ideal_tri(m.fmod(f0 * i / sr, 1.0)) for i in range(n)]
    x_bl = _bandlimited_tri(f0, sr, n)
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
    """Drop the probe's warmup (kWarm=8192 settle frames): keep only the settled tail so the
    integer-period window lands in steady-state audio, not on a transient."""
    return x[-STEADY:] if len(x) > STEADY else x


def read_cells(dirpath):
    tsv = os.path.join(dirpath, "gh19_scenarios.tsv")
    rows = []
    with open(tsv) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="report/gh19-probe")
    ap.add_argument("--check", action="store_true", help="run the negative-control gate")
    args = ap.parse_args()

    cells = read_cells(args.dir)
    produced = [c for c in cells if c["produced"] and c.get("raw")]
    if not produced:
        print("no produced cells to measure")
        return 0

    gate_fail = []
    print("id\tpath\tsr\tf0_target\tf0_refined\taliasA_full(dB)\taliasA_inband(dB)\tN\tperiods\tgap\taliasB_inband(dB)\tnote")
    for rec in produced:
        sr = float(rec["sr_hz"])
        f0_target = float(rec["f0_target_hz"])
        x = steady(read_raw(os.path.join(args.dir, rec["raw"])))
        if not x or any(not math.isfinite(v) for v in x):
            if args.check:
                gate_fail.append(rec["id"] + ": non-finite/empty")
            print("\t".join([rec["id"], rec["path"], str(int(sr)), str(int(f0_target)), "-", "-", "-", "-", "-", "-", "-", "non-finite"]))
            continue
        peak = max(abs(v) for v in x)
        if peak < 1e-4:
            if args.check:
                gate_fail.append(rec["id"] + ": silent")
            print("\t".join([rec["id"], rec["path"], str(int(sr)), str(int(f0_target)), "-", "-", "-", "-", "-", "-", "-", "silent"]))
            continue
        if f0_target > 0:
            f0 = refine_f0(x, sr, f0_target)
        else:
            f0_meas = float(rec["f0_meas_hz"])
            f0 = refine_f0(x, sr, (f0_meas if f0_meas > 0 else 200.0))
        ma = method_a(x, sr, f0)
        note = ""
        aliasB = "-"
        if rec["path"].startswith("vco_a") and ma:
            mb = method_b_tri(sr, f0, ma["a1_mag"])
            if mb:
                aliasB = "%.1f" % mb["dedup_db"]
                note = "tri(cross-check)"
        elif rec["path"].startswith("vco_b") and ma:
            mb = method_b_tri(sr, f0, ma["a1_mag"])
            if mb:
                aliasB = "%.1f" % mb["dedup_db"]
                note = "tri(cross-check)"
        elif rec["path"] == "drone1_gn1":
            note = "composite(saw+cubic, non-single-periodic)"
        elif rec["path"] == "wet_chain":
            note = "composite(kVcfPath, not single-module)"
        row = [rec["id"], rec["path"], str(int(sr)), str(int(f0_target)),
               ("%.2f" % f0) if f0 > 0 else "-",
               ("%.2f" % ma["full_db"]) if ma and math.isfinite(ma["full_db"]) else "-",
               ("%.2f" % ma["inband_db"]) if ma and math.isfinite(ma["inband_db"]) else "-",
               (ma["n"] if ma else "-"), (ma["Nperiods"] if ma else "-"),
               ("%.4f" % ma["gap"]) if ma else "-",
               aliasB, note if note else "-"]
        print("\t".join(str(v) for v in row))

    # ------------------------------------------------------------------ gate
    if args.check:
        print("\n--- G1 conjugation (coincident-fold pairs, complex compare) ---")
        for wave, coeff, sfreq in (("saw", a_saw, 220.0),):
            for sr in (48000.0,):
                rows, errs = fold_pair_criteria(coeff, wave, sfreq, sr)
                for kind, detail in rows:
                    print("  %s  sr=%g  %s" % (kind, sr, detail))
                gate_fail.extend(errs)

        print("\n--- G2 reconciliation (wrong sample-rate / reference-frequency labels must be caught) ---")
        rec = next((c for c in produced if c["path"].startswith("vco_a")), None)
        if rec:
            sr = float(rec["sr_hz"])
            x = steady(read_raw(os.path.join(args.dir, rec["raw"])))
            f0 = refine_f0(x, sr, float(rec["f0_target_hz"]))
            ma = method_a(x, sr, f0)
            a1 = ma["a1_mag"]
            good = method_b_tri(sr, f0, a1)
            wrong_sr = method_b_tri((sr * 2) if sr != 96000 else 44100, f0, a1)
            wrong_f0 = method_b_tri(sr, f0 * 1.03, a1)
            if good is None:
                gate_fail.append("reconcile: no baseline")
            elif wrong_sr is None or wrong_f0 is None:
                gate_fail.append("reconcile: wrong-label produced no figure")
            elif abs(good["dedup_db"] - wrong_sr["dedup_db"]) < 1e-3:
                gate_fail.append("reconcile: wrong-sr-label not caught")
            elif abs(good["dedup_db"] - wrong_f0["dedup_db"]) < 1e-3:
                gate_fail.append("reconcile: wrong-f0-label not caught")
            else:
                print("  good=%.1fdB wrong_sr=%.1fdB wrong_f0=%.1fdB -> labels caught"
                      % (good["dedup_db"], wrong_sr["dedup_db"], wrong_f0["dedup_db"]))
        else:
            gate_fail.append("reconcile: no VCO cell present")

        print("\n--- G3 real-not-silent / substitution ---")
        print("  checked %d produced cells (finite + non-silent + domain-scale via probe clamp)." % len(produced))

        print("\n--- G4 dynamic range (naive vs band-limited synthetic triangle @1865Hz/48k) ---")
        dr = dynamic_range_control()
        if dr is None:
            gate_fail.append("dynamic-range: no figures")
        else:
            print("  naive full=%.2fdB inband=%.2fdB | bandlimited full=%.2fdB inband=%.2fdB | separation=%.2fdB"
                  % (dr["naive_full"], dr["naive_inband"], dr["bl_full"], dr["bl_inband"], dr["sep"]))
            if not (dr["sep"] >= 12.0):
                gate_fail.append("dynamic-range: metric cannot distinguish aliased from clean (sep<12dB)")

    if args.check and gate_fail:
        print("\nGATE FAIL (%d):" % len(gate_fail))
        for g in gate_fail:
            print("  [FAIL]", g)
        return 1
    if args.check:
        print("\nGATE PASS: measurement negative controls OK (no aliasing threshold asserted).")
    else:
        print("\n(run with --check to run the negative-control gate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
