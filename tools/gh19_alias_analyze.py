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
# REFERENCE MEANING (BLOCK item ② — Method A is NOT an independent band-limited reference):
#   METHOD A ("harmonic-fit residual", lower bound): reference = the signal's OWN measured in-band
#      harmonic coefficients (projection at k*f0, k*f0 <= sr/2); residual = x - ref. This ABSORBS
#      aliasing that lands on legitimate in-band harmonics, so it is a LOWER bound on total aliasing,
#      never the authoritative alias figure. Reported as `harmres_*_db`.
#   METHOD B ("analytic reference", authoritative for clean triangle): ideal-triaes exact analytic
#      a_tri(k)=4/(pi^2 k^2) (odd k), folded with the CONJUGATED mirror rule, dedup over distinct
#      observed lines, in-band dBc. For a triangle cell this is the INDEPENDENT alias reference.
#      Reported as `analytic_ref_inband_db`. A naive product triangle => analytic_ref ~= the naive fold;
#      an already-anti-aliased one => analytic_ref << naive fold. A-B consistency is evidence.
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
# METHOD A: harmonic-fit residual (real product, any periodic shape, leakage-free). This is a LOWER
# bound on total aliasing (it absorbs aliasing co-located with a legitimate in-band harmonic), so it
# is reported as a harmonic-fit residual, never as the authoritative alias figure.
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
# METHOD B: ideal-triangle analytic folded-line alias (CONJUGATE-corrected). Authoritative reference
# for a clean triangle cell — independent of the signal's own harmonics.
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
        out["analytic_ref"] = ("%.1f" % mb["dedup_db"]) if mb else "-"
    else:
        out["analytic_ref"] = "-"
    return out


def fmt_db(v):
    return ("%.2f" % v) if (v is not None and math.isfinite(v)) else "-"


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
           "analytic_ref_inband_db\tN\tperiods\tgap\tcategory")
    print(hdr)
    for cid in sorted(required) + [c["id"] for c in cells if c["id"] not in required]:
        rec = by_id.get(cid)
        if rec is None:
            # a manifest-required id with no row is already a coverage error; emit a stub row.
            m = required.get(cid)
            print("\t".join([cid, (m["path"] if m else "-"), (m["sr"] if m else "-"), "-", "-",
                             "-", "-", "-", "-", "-", "-", "MISSING"]))
            continue
        if not rec["produced"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-", "-",
                             "-", "-", "-", "-", "not-produced:" + rec.get("signal", "")]))
            continue
        a = analyze_cell(args.dir, rec)
        if a["err"]:
            print("\t".join([cid, rec["path"], rec["sr_hz"], rec["f0_target_hz"], "-", "-", "-",
                             "-", "-", "-", "-", a["err"]]))
            # a required cell that is produced but invalid is a hard FAIL (not an info line).
            if cid in required:
                gate_errors.append(f"invalid-cell {cid} [{a['err']}]")
            continue
        print("\t".join([
            cid, rec["path"], str(int(a["sr"])), str(int(a["f0_target"])),
            ("%.2f" % a["f0"]) if a["f0"] > 0 else "-",
            a["harmris"], a["harmris_inband"],
            a.get("analytic_ref", "-"), str(a["n"]), str(a["periods"]),
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

            # G2 wrong-label + wrong power-normalization reconciliation on a REAL triangle cell.
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
                ma = method_a(x, sr, f0)
                a1 = ma["a1_mag"]
                good = method_b_tri(sr, f0, a1)
                wrong_sr = method_b_tri((sr * 2) if sr != 96000 else 44100, f0, a1)
                wrong_f0 = method_b_tri(sr, f0 * 1.03, a1)
                if good is None or wrong_sr is None or wrong_f0 is None:
                    check_fails.append("G2 reconcile: no baseline figure")
                elif abs(good["dedup_db"] - wrong_sr["dedup_db"]) < 1e-3:
                    check_fails.append("G2 reconcile: wrong-sr-label not caught")
                elif abs(good["dedup_db"] - wrong_f0["dedup_db"]) < 1e-3:
                    check_fails.append("G2 reconcile: wrong-f0-label not caught")
                else:
                    print("  G2 reconcile: good=%.1fdB wrong_sr=%.1fdB wrong_f0=%.1fdB -> labels caught"
                          % (good["dedup_db"], wrong_sr["dedup_db"], wrong_f0["dedup_db"]))
                # wrong power normalization: the alias figure MUST be referenced to the carrier
                # (a1_mag^2 == carrier_p2). Recompute it against a genuinely WRONG denominator — the
                # whole signal's power (harmonic + alias, the classic dBc slip) — and assert the two
                # differ, so a power-normalisation error is caught REGARDLESS of whether this cell has
                # coincident-fold coincidence (naive vs dedup happens to agree on a clean triangle).
                total_p = sum(v * v for v in x)
                wrong_pw = (10.0 * math.log10(good["dedup_p"] / total_p)
                            if (total_p > 0 and good["dedup_p"] > 0) else float("-inf"))
                if wrong_pw == float("-inf") or abs(wrong_pw - good["dedup_db"]) < 1e-3:
                    check_fails.append("G2 reconcile: power-normalisation not distinguishable")
                else:
                    print("  G2 power-norm: correct(carrier)=%.1fdB wrong(signal-power)=%.1fdB -> caught"
                          % (good["dedup_db"], wrong_pw))
                # correct-scaled-ideal-stand-in negative anchored to this real cell.
                dr = dynamic_range_control(sr, f0, max(abs(v) for v in steady(x)))
                if dr is None:
                    check_fails.append("G4 dynamic-range: no figure at real cell sr/f0")
                else:
                    print("  G4 stand-in: naive_full=%.2f bandlimited_full=%.2f sep=%.2f"
                          % (dr["naive_full"], dr["bl_full"], dr["sep"]))
                    if dr["sep"] < SEP_MIN_DB:
                        check_fails.append("G4 dynamic-range: metric cannot distinguish aliased from clean (sep<%.0fdB)" % SEP_MIN_DB)

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
