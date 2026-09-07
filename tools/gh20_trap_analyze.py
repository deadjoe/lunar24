#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh20_trap_analyze.py — task #88 (GH #20) TWO-INTEGRATOR TRAPEZOIDAL (TPT/ZDF) acceptance gate.
#
# @Codex production contract (msg 8320798f) ruled direction A: replace the Chamberlin recursion with a
# two-integrator trapezoidal SVF, and adopt an INDEPENDENT numeric acceptance oracle (gh20_trap_reference):
#
#     k = damp = 2 - 1.9*res ;  r = tan(pi*f_test/sr) / tan(pi*fc/sr)
#     LP = 1/(1 - r^2 + jkr)  ;  BP = jr/(1 - r^2 + jkr)      (fc = 20*1000^n, capped min(20000, 0.49*sr))
#
# This gateway compares the REAL-PRODUCT probe output (tests/probes/gh20_vcf_probe.cpp, the same
# real codec->owner->render path as task #85/#87) against that reference:
#
#   (2) small-signal AMPLITUDE ERROR vs reference <= 0.1 dB, using the FUNDAMENTAL projection
#       (sqrt(amp_rms^2 - noise_rms^2), the single-frequency matched-sinusoid magnitude — NOT the
#       harmonic/whole-window RMS). The constant chain gain through mixer->pan->dist-dry is frequency
#       independent and is NORMALISED OUT by fitting the per-group chain gain C (median of
#       fundamental/reference_mag); the residual is the shape mismatch, which must be <= 0.1 dB.
#   * Resolvability: a cell is only reference-comparable when its fundamental is clearly above the
#       per-cell residual noise floor (SNR >= 20 dB, i.e. >= 10x in amplitude). A cell attenuated into
#       the machine floor (deep stopband, e.g. LP at a norm where fc is low) is physically unmeasurable
#       and is REPORTED per-item, NOT failed as an amplitude error — the contract's "report per-item".
#   * Endpoint: norm=1 / res=0 / LP, 100 Hz-normalised 8 kHz gain must be sample-rate-uniform across
#       44.1/48/88.2/96k to <= 1 dB (the cross-rate cap ghost the fix removes).
#
# The reference is a DIFFERENT object from the product recursion (it is the bilinear-mapped continuous
# prototype), so a product that reproduces the OLD sr/8-capped Chamberlin response does NOT match it
# and the gate turns RED (the contract: "the new criterion must be RED on the old sr/8 recursion").
#
# FAIL-CLOSED COVERAGE (imported from the task #87 gate, unchanged): every manifest-required cell must
# be produced ok=1, finite, in-range (over-scale guard), and non-silent where signal is expected.
#
# Exit 0 = coverage complete AND every reference-evaluable group matches the reference within 0.1 dB.
# No production DSP is changed to make this green; the tool only measures.
#
# Reproduction:
#   ./build/gh20_vcf_probe --out report/gh20-probe-trap
#   python3 tools/gh20_trap_analyze.py --dir report/gh20-probe-trap --manifest tools/gh20_manifest.tsv
#   python3 tools/gh20_trap_analyze.py --dir report/gh20-probe-trap --manifest tools/gh20_manifest.tsv --check

import argparse
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gh20_trap_reference as ref

# ---------------------------------------------------------------------------
# Tolerances — set FROM MEASUREMENT: the real trapezoidal product matches the reference to <= 0.03 dB
# at every resolvable cell (SNR >= 20 dB), so 0.1 dB is a healthy contract bound. The OLD sr/8-capped
# Chamberlin deviates far more (different topology + the sr/8 cap) and must exceed this -> RED.
# ---------------------------------------------------------------------------
REF_ERR_MAX_DB = 0.1       # contract: small-signal amplitude error vs reference <= 0.1 dB.
RESOLVABLE_SNR_DB = 20.0   # a cell is reference-comparable iff fundamental >= 10x residual noise.
MIN_RESOLVABLE_GROUP = 2   # a group needs >= this many resolvable cells to be reference-evaluable.
CROSS_MAX_DB = 1.0         # endpoint norm1/res0/LP 8k (100Hz-normalised) cross-rate max diff.
CROSS_REF_FREQ = 100.0     # pass-band reference (Hz).
CROSS_FOCUS_FREQ = 8000.0  # the sample-rate-sensitive frequency (Hz).
SILENT_RMS = 1e-12         # absolute amp_rms floor: at/below this is substituted silence.
OVERSCALE_PEAK = 1.0       # amp_peak upper bound (real res=1 resonance peak 0.709 at lvl 0.20).


# ---------------------------------------------------------------------------
# IO
# ---------------------------------------------------------------------------
def read_rows(path):
    with open(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        return []
    header = lines[0].split("\t")
    idx = {name: i for i, name in enumerate(header)}
    rows = []
    for line in lines[1:]:
        cols = line.split("\t")
        if len(cols) < len(header):
            continue
        rows.append({name: cols[idx[name]] for name in header})
    return rows


def produced_of(rec):
    return rec.get("ok", "0") == "1" and rec.get("signal", "-") in ("", "-")


def is_finite_num(v):
    try:
        return math.isfinite(float(v))
    except (TypeError, ValueError):
        return False


def fundamental_rms(rec):
    """Single-frequency matched-sinusoid magnitude in RMS. The probe's residualNoiseRms is the
    orthogonal fit residual at the stimulus frequency, so the fundamental is sqrt(amp^2 - noise^2)."""
    amp = float(rec.get("amp_rms", "0") or 0)
    noise = float(rec.get("noise_rms", "0") or 0)
    if amp <= 0:
        return 0.0
    return math.sqrt(max(amp * amp - noise * noise, 0.0))


def read_manifest(path):
    rows = read_rows(path)
    if not rows:
        raise SystemExit("FATAL: manifest is empty")
    full = {}
    required = {}
    for r in rows:
        full[r["id"]] = r
        if r.get("required", "0") == "1":
            required[r["id"]] = r
    return required, full


# ---------------------------------------------------------------------------
# Per-group reference comparison. Group key = (sr, mode, res, norm, lvl); norm is the VALUE (0..1).
# ---------------------------------------------------------------------------
def group_cells(cells):
    """Group probe cells (not floor, freq>0, wetL) by (sr, mode, res, norm, lvl)."""
    groups = {}
    for c in cells:
        try:
            freq = float(c.get("freq_hz", "0"))
            sr = float(c.get("sr_hz", "0"))
            norm = float(c.get("norm", "0"))
            res = float(c.get("res", "0"))
            lvl = float(c.get("lvl", "0"))
        except (TypeError, ValueError):
            continue
        if freq <= 0 or not produced_of(c) or c.get("channel", "") != "wetL":
            continue
        key = (sr, c.get("mode", ""), res, norm, lvl)
        groups.setdefault(key, []).append(c)
    return groups


def group_reference_error(key, cells, snr_db):
    """Return (group_max_err_db, n_resolvable, n_total) for one group, or None if not reference-evaluable.

    resolvable cells only (fundamental >= snr_db above residual noise AND reference magnitude finite).
    Fits C = median(fundamental/reference_mag) over resolvable cells, then group_max_err_db =
    max|20*log10(fundamental/(C*reference_mag))|."""
    sr, mode, res, norm, lvl = key
    rmode = "lp" if mode == "lp" else "bp"
    pts = []
    for c in cells:
        fn = fundamental_rms(c)
        noise = float(c.get("noise_rms", "0") or 0)
        freq = float(c["freq_hz"])
        mag = ref.reference_mag(freq, norm, sr, res, rmode)
        if mag <= 1e-12 or fn <= 0:
            continue
        snr = float("inf") if noise <= 0 else (20.0 * math.log10(fn / noise))
        pts.append((freq, fn, mag, snr))
    resolvable = [p for p in pts if p[3] >= snr_db]
    if len(resolvable) < MIN_RESOLVABLE_GROUP:
        return None
    ratios = [fn / m for (_, fn, m, _) in resolvable]
    C = statistics.median(ratios)
    if C <= 0:
        return None
    errs = [20.0 * math.log10(fn / (C * m)) for (_, fn, m, _) in resolvable if fn > 0 and C * m > 0]
    if not errs:
        return None
    return (max(abs(e) for e in errs), len(resolvable), len(pts))


def crossrate_endpoint(cells_by_sr, norm=1.0, res=0.0, mode="lp"):
    """norm=1/res=0/LP 100 Hz-normalised 8 kHz gain per sr. Returns {sr: gain_db} (>=0 value)."""
    out = {}
    for c in cells_by_sr:
        try:
            sr = float(c.get("sr_hz", "0"))
            freq = float(c.get("freq_hz", "0"))
            if float(c.get("norm", "0")) != norm or float(c.get("res", "0")) != res:
                continue
            if c.get("mode", "") != mode or c.get("channel", "") != "wetL" or freq <= 0:
                continue
            if not produced_of(c):
                continue
        except (TypeError, ValueError):
            continue
        if not math.isclose(freq, CROSS_REF_FREQ, rel_tol=1e-9) and not math.isclose(freq, CROSS_FOCUS_FREQ, rel_tol=1e-9):
            continue
        fn = fundamental_rms(c)
        out.setdefault(sr, {})[freq] = fn
    gains = {}
    for sr, m in out.items():
        if CROSS_REF_FREQ in m and CROSS_FOCUS_FREQ in m and m[CROSS_REF_FREQ] > 0 and m[CROSS_FOCUS_FREQ] > 0:
            gains[sr] = 20.0 * math.log10(m[CROSS_FOCUS_FREQ] / m[CROSS_REF_FREQ])
    return gains


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="report/gh20-probe-trap")
    ap.add_argument("--manifest", default="tools/gh20_manifest.tsv")
    ap.add_argument("--snr", type=float, default=RESOLVABLE_SNR_DB,
                    help="resolvability floor in dB (default %s)" % RESOLVABLE_SNR_DB)
    ap.add_argument("--check", action="store_true", help="run the acceptance gate (exit 0/1)")
    args = ap.parse_args()

    scen = os.path.join(args.dir, "gh20_scenarios.tsv")
    if not os.path.exists(scen):
        print("COVERAGE FAIL: no gh20_scenarios.tsv", file=sys.stderr)
        return 1
    cells = read_rows(scen)
    by_id = {c["id"]: c for c in cells}

    required, _full = read_manifest(args.manifest)

    failures = []

    # ------------------------------------------------------- coverage gate (always, failure-closed).
    # A cell is signal-expected only at/below its effective cutoff; deep-stopband is the response.
    floor_rms = {}
    for c in cells:
        if c["id"].startswith("floor_") and produced_of(c):
            floor_rms[int(float(c.get("sr_hz", "0")))] = float(c["amp_rms"])
    sr_floor = lambda rec: floor_rms.get(int(float(rec.get("sr_hz", "0"))), 0.0)

    for mid, mrec in required.items():
        prod = by_id.get(mid)
        if prod is None:
            failures.append("[COVERAGE] missing:" + mid)
            continue
        rec = dict(mrec); rec.update(prod)
        if not produced_of(rec):
            failures.append("[COVERAGE] not-produced:%s (%s)" % (mid, rec.get("signal", "-")))
            continue
        for k in ("amp_rms", "amp_peak", "noise_rms"):
            if not is_finite_num(rec.get(k, "-")):
                failures.append("[COVERAGE] non-finite:%s:%s" % (mid, k))
                continue
        amp_peak = float(rec["amp_peak"])
        if amp_peak > OVERSCALE_PEAK:
            failures.append("[COVERAGE] over-scale:%s" % mid)
        # silent only when signal is expected (freq <= effective cutoff and NOT a floor cell).
        if not mid.startswith("floor_"):
            eff = ref.cutoff_hz(float(rec["norm"]), float(rec["sr"]))
            if float(rec["freq"]) <= eff and float(rec["amp_rms"]) <= max(sr_floor(rec), SILENT_RMS):
                failures.append("[COVERAGE] silent:%s" % mid)

    # ------------------------------------------------ reference comparison (contract item (2)).
    groups = group_cells(cells)
    sub_resolvable = []
    ref_evaluated = 0
    ref_bad = []
    for key in sorted(groups, key=lambda k: (k[0], k[1], k[2], k[3], k[4])):
        res = group_reference_error(key, groups[key], args.snr)
        if res is None:
            sub_resolvable.append(key)
            continue
        group_max, n_resv, n_total = res
        ref_evaluated += 1
        if group_max > REF_ERR_MAX_DB:
            ref_bad.append((key, group_max, n_resv, n_total))
            failures.append("[REF-ERR] sr=%g mode=%s res=%g norm=%g lvl=%g max=%.3fdB > %.1f (n=%d/%d resolvable)" % (
                key[0], key[1], key[2], key[3], key[4], group_max, REF_ERR_MAX_DB, n_resv, n_total))

    # ------------------------------------------- endpoint cross-rate uniformity (norm1/res0/LP, 8k).
    xgains = crossrate_endpoint(cells)
    if len(xgains) >= 2:
        lo, hi = min(xgains.values()), max(xgains.values())
        if (hi - lo) > CROSS_MAX_DB:
            failures.append("[CROSS] norm1/res0/LP 8k normalized diff %.3fdB > %.1f (%s)" % (
                hi - lo, CROSS_MAX_DB, ", ".join("%g:%.2f" % (int(s), g) for s, g in sorted(xgains.items()))))

    # ------------------------------------------------------------- report.
    print("coverage required=%d produced=%d failures=%d" % (
        len(required), sum(1 for c in cells if produced_of(c)), len(failures)))
    print("reference-evaluable groups=%d (out of %d groups), sub-resolvable (deep stopband, reported not failed)=%d" % (
        ref_evaluated, len(groups), len(sub_resolvable)))
    if xgains:
        print("endpoint norm1/res0/lp 8k-normalized gains (dB): %s" %
              ", ".join("%g:%.3f" % (int(s), g) for s, g in sorted(xgains.items())))

    if failures:
        print()
        for f in failures:
            print(f)
        print("ACCEPTANCE FAIL: %d gate check(s)" % len(failures))
        return 1

    print("\nACCEPTANCE PASS: coverage complete, %d reference-evaluable group(s) match the "
          "independent bilinear reference within %.1f dB (resolvability floor %g dB); "
          "%d deep-stopband group(s) below the resolvability floor are reported separately." % (
              ref_evaluated, REF_ERR_MAX_DB, args.snr, len(sub_resolvable)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
