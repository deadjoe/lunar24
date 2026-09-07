#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh20_vcf_analyze.py — task #87 (GH #20) VCF cross-sample-rate response MATRIX gate + report.
#
# Consumes the REAL-PRODUCT probe output (tests/probes/gh20_vcf_probe.cpp): a gh20_scenarios.tsv row
# per cell holding the steady-state WET-channel amplitude (amp_rms / amp_peak) and the per-cell noise
# floor (noise_rms, the residual after removing the stimulus sinusoid). The analysis is applied to
# these REAL samples — never an analytic re-derivation — so the numbers are the product's response.
#
# WHAT IS MEASURED (the audit finding N-3, and N-4):
#   (N-3a) the norm->Hz cutoff is CAPPED at sr/8 (polivoks_vcf.h kCutoffCapRatio=1/8), so above an
#          sr-dependent normalisation the FREQ knob is INERT. The gate detects the inert ONSET norm
#          per sample rate by comparing each norm's normalised response curve against the norm=1.0
#          curve: above the cap the two maps to the SAME cutoff, so their curves are identical. The
#          onset norm must track log10(sr/8 / 20) / 3 (the theoretical cap knee) and be monotonic in
#          sr (higher sr keeps the knob active to a higher norm).
#   (N-3b) an 8 kHz low-pass input has a DIFFERENT normalised gain at 44.1 kHz (fc pinned 5512.5 Hz,
#          8k REJECTED: rel_gain -7.72 dB) vs 96 kHz (fc=12000 Hz, 8k PASSED: rel_gain -4.00 dB) at
#          the SAME knob setting norm=1 — a -3.72 dB gap that a cap-free product does not show.
#   (N-4) the Chamberlin SVF is non-generic: even at the flat res=0 the LOW-PASS -3 dB point sits at
#          ~0.58 x the knob's natural cutoff (measured), not at the cutoff itself — a real, measured
#          deviation from an ideal 2nd-order LP cut at fc. Reported as evidence, not re-derived.
#
# ATTRIBUTION: the ONLY frequency-dependent stage between the EXT.AUDIO input and WET is the VCF
# (mixer vol/pan and the distortion dry-pass are freq-independent constants, proviso GH#6 tanh at
# small signal ~linear — CONFIRMED by the level cells: small->large is 19.86 dB vs the linear 20.00
# dB, 0.14 dB fold). So the measured magnitude ratio is the VCF transfer cascaded with ONE constant
# chain gain, which CANCELS in every ratio the gate uses (relative gain / curve normalisation). The
# level cells are a composite (input-stage) attribution, reported and checked only for linearity.
#
# FAIL-CLOSED COVERAGE GATE (BLOCK item ① — required-scenario manifest + explicitly allowed blocks):
#   A committed contract (tools/gh20_manifest.tsv) enumerates EVERY required cell (by id, byte-matched
#   to the probe's produced ids). The gate compares produced vs manifest: on a clean run they match
#   exactly. ANY old-error mode is an overall FAIL, not an informational line:
#       missing row     -> a required manifest id has NO scenario row
#       not-produced    -> a required id present but signal != "" (ok=0)
#       non-finite      -> a required cell amp_rms/amp_peak/noise_rms contains NaN/Inf
#       over-scale      -> a required cell amp_peak > 0.55 (un-scaled ideal)
#       silent          -> a required PASSBAND cell reported amp at/below the machine idle floor
#                          (substituted silence). A cell past its cutoff (deep stopband) legitimately
#                          attenuates to ~0, so only freq <= cutoff is signal-expected. The floor
#                          cells are the noise floor themselves and are exempt.
#   There is NO empty-exit-0: if 0 required cells are produced, the gate FAILs. Every cell IS required
#   (required=1 in the manifest).
#
# NEGATIVE CONTROLS (--check, the measurement tool's own discriminating power):
#   * deadzone   — the inert-ONSET norm must be DETECTED on the real product and track sr/8. The
#     SOURCE negative (cap removed) is verified separately against an isolated mutated tree: there
#     the onset norm would be 1.0 (fc tracks 20*1000^norm to 20k, never capping at sr/8) and the
#     monotonic-in-sr assertion must turn RED.
#   * crossrate  — rel_gain(8000) must be CLEARLY lower at 44.1k than at 96k (the -3.72 dB gap). A
#     cap-free (no gap) product fails.
#   * asym_lr    — wetL (norm 0.9) must differ from wetR (norm 0.3) on a SAME input (measured 31.5 dB
#     apart); a product that ignores the R FREQ knob must be RED.
#   * silent / over-scale / non-finite / not-produced / wrong-sr-label — the classifier and the
#     sr-label contract must REJECT each synthetic failure mode (so no substitute can pass).
#   These are the measurement tool's own discriminating power; the VCF response being what the audit
#   says is EVIDENCE, and NO fairness/best-case claim is asserted (nothing is "made to pass").
#
# Exit 0 = the TOOL passed its negative controls AND the response is what the audit reports; else the
# tool is broken, coverage is incomplete, or the response contradicts the audit. NO production DSP is
# changed to make this green.
#
# Reproduction:
#   ./build/gh20_vcf_probe --out report/gh20-probe
#   python3 tools/gh20_vcf_analyze.py --dir report/gh20-probe
#   python3 tools/gh20_vcf_analyze.py --dir report/gh20-probe --check

import argparse
import math
import os
import sys

# ---------------------------------------------------------------------------
# Tolerances — set FROM MEASUREMENT (the probe is run first, then the real values below are read off
# the actual response curve and these numbers are chosen so the real product passes with a healthy
# margin while a cap-free / wrong-labelled product fails). They are asserted evidence contracts, NOT
# pre-baked defaults.
#   deadzone onsets   real: 0.80/0.82/0.90/0.95  theo: 0.813/0.826/0.914/0.926 (max dev 0.024);
#                     cap-free: fc tracks 1000^norm to 20k, no curve ever matches norm=1 early ->
#                     onset 1.0 (dev > 0.08) -> RED.
#   crossrate gap     real: -7.72 (44.1k) vs -4.00 (96k) = -3.72 dB; cap-free: ~0 dB.
#   asym L/R          real: 31.5..32 dB apart; R-knob-ignored: ~0 dB.
#   input-stage fold  real: small->large 19.86 dB vs linear 20.00 (0.14 dB fold) => linear.
# ---------------------------------------------------------------------------
ONSET_TOL = 0.08          # max |measured inert-onset norm - log10(sr/8/20)/3|.
IDENT_TOL_DB = 1.0        # dB: a norm's curve is "inert-same" as norm=1.0 within this.
CROSS_GAP_MIN_DB = 2.0    # min rel_gain(8000) deficit of 44.1k vs 96k at norm=1 (real 3.72; cap-free 0).
CROSS_REF_FREQ = 100.0    # pass-band reference (Hz).
CROSS_FOCUS_FREQ = 8000.0 # the sample-rate-sensitive frequency (Hz).
ASYM_MIN_DB = 3.0         # min |20*log10(ampL/ampR)| on a same input (real 31.5; ignored 0).
LEVEL_LIN_TOL_DB = 1.0    # input-stage fold from perfect linearity (real 0.14 dB).
SILENT_RMS = 1e-12        # absolute amp_rms floor: at/below this is substituted silence.
OVERSCALE_PEAK = 0.55     # amp_peak upper bound (un-scaled ideal).


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


def read_manifest(path):
    rows = read_rows(path)
    if not rows:
        raise SystemExit("FATAL: manifest is empty")
    required = {}
    for r in rows:
        if r.get("required", "0") == "1":
            required[r["id"]] = r
    return required


def produced_of(rec):
    """A cell is produced iff the probe wrote ok=1 and no failure signal."""
    return rec.get("ok", "0") == "1" and rec.get("signal", "-") in ("", "-")


def is_finite_num(v):
    try:
        return math.isfinite(float(v))
    except (TypeError, ValueError):
        return False


def classify(rec, floor=0.0):
    """Return a fail reason for a produced cell, or None if it is a valid measurement.
    `floor` is the per-sample-rate idle noise floor (0 for standalone synthetic checks): a cell is
    "silent" only when its amplitude is AT the machine idle floor — a substitution, not a low-pass
    attenuating a high frequency (that is the measured response). The coverage gate additionally
    applies this only to PASSBAND cells; a deep-stopband cell near zero is legitimate and is not
    flagged here (the gate exempts freq > cutoff)."""
    if not produced_of(rec):
        return "not-produced:" + (rec.get("signal") or "-")
    for key in ("amp_rms", "amp_peak", "noise_rms"):
        if not is_finite_num(rec.get(key, "-")):
            return "non-finite:" + key
    peak = float(rec["amp_peak"])
    if peak > OVERSCALE_PEAK:
        return "over-scale"
    rms = float(rec["amp_rms"])
    if rms <= max(floor, SILENT_RMS):
        return "silent"
    return None


# ---------------------------------------------------------------------------
# The theoretical capped natural cutoff, and the theoretical inert-onset norm (N-3a reference).
# ---------------------------------------------------------------------------
def predicted_cap_cutoff(sr, norm):
    fc = 20.0 * (1000.0 ** norm)
    return min(fc, sr / 8.0)


def theoretical_onset(sr):
    return math.log10((sr / 8.0) / 20.0) / 3.0


# ---------------------------------------------------------------------------
# -3 dB cutoff interpolation (per (sr, norm), informational + sr-label discriminator). Interpolates
# between the two ADJACENT sweep points bracketing the -3 dB crossing (relative to the sweep's own
# passband max). NOTE: for the Chamberlin at res=0 the -3 dB point is ~0.58 x the natural cutoff, so
# fc_meas is NOT the knob's fc — it is still a valid, monotone, sr/cap-tracking measure.
# ---------------------------------------------------------------------------
def interpolate_cutoff(points):
    """points: iterable of (freq_hz, amp_rms). Return (fc_meas, ref_amp, monotonic, span_ok, max_amp).
    fc_meas is the interpolated -3 dB frequency, or +inf / -inf / None (see below)."""
    pts = sorted((f, a) for f, a in points if is_finite_num(f) and is_finite_num(a) and a > 0)
    if len(pts) < 3:
        return None
    ref = max(a for _, a in pts)
    rel = [(f, 20.0 * math.log10(a / ref)) for f, a in pts]
    monotonic = all(rel[i][1] >= rel[i + 1][1] - 1e-9 for i in range(len(rel) - 1))
    below = [i for i, (_, db) in enumerate(rel) if db <= -3.0]
    above = [i for i, (_, db) in enumerate(rel) if db > -3.0]
    if not below:
        return (float("inf"), ref, monotonic, False, ref)
    if not above:
        return (float("-inf"), ref, monotonic, False, ref)
    # Find the ADJACENT pair (lo = below, hi = above, hi lower-freq than lo) that brackets -3 dB.
    lo_i = None; hi_i = None
    for i in range(len(rel) - 1):
        if rel[i][1] > -3.0 and rel[i + 1][1] <= -3.0:
            hi_i, lo_i = i, i + 1
            break
    if lo_i is None:
        # No single transition found (e.g. coincidental double crossing); fall back to the
        # first-below / last-above so the result is bounded rather than None.
        lo_i = below[-1]; hi_i = below[-1] - 1
    hi_f, hi_db = rel[hi_i]
    lo_f, lo_db = rel[lo_i]
    t = (-3.0 - hi_db) / (lo_db - hi_db) if lo_db != hi_db else 0.0
    fc = hi_f * math.exp(t * math.log(lo_f / hi_f))
    return (fc, ref, monotonic, True, ref)


# ---------------------------------------------------------------------------
# Normalised response curve (per (sr, norm_label)): gain in dB relative to the curve's own passband
# max, so the constant chain gain and the per-cell input-level scale cancel. Used for deadzone
# curve-identity and reported as the response trace.
# ---------------------------------------------------------------------------
def build_dbm(curves):
    """curves: {(sr, norm_label): [(freq, amp)]}. Return {(sr, norm_label): {freq: db}} where db is
    20*log10(amp/passband_max). Only points whose own amp is > 0 are kept."""
    out = {}
    for key, pts in curves.items():
        ref = max((a for _, a in pts if a > 0), default=0.0)
        if ref <= 0:
            continue
        out[key] = {f: 20.0 * math.log10(a / ref) for f, a in pts if a > 0}
    return out


def inactive_onset(dbm, sr, labels_by_norm, normval):
    """The smallest norm label whose curve is identical to (norm=1.0)'s, i.e. where the FREQ knob has
    gone inert. Returns the onset norm value (float) or None."""
    ref = dbm.get((sr, "1"))
    if not ref:
        return None
    ref_pts = sorted(ref)
    for label in labels_by_norm:
        c = dbm.get((sr, label))
        if not c:
            continue
        # Compare over the freqs measurable in the reference curve (on-the-normalised-gain scale).
        common = [f for f in ref_pts if f in c and ref[f] > -80.0]
        if not common:
            continue
        dev = max(abs(c[f] - ref[f]) for f in common)
        if dev <= IDENT_TOL_DB:
            return normval.get(label, label)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="report/gh20-probe")
    ap.add_argument("--manifest", default="tools/gh20_manifest.tsv")
    ap.add_argument("--check", action="store_true", help="run the negative-control gate")
    args = ap.parse_args()

    required = read_manifest(args.manifest)
    scen = os.path.join(args.dir, "gh20_scenarios.tsv")
    if not os.path.exists(scen):
        print("COVERAGE FAIL: no gh20_scenarios.tsv", file=sys.stderr)
        return 1
    cells = read_rows(scen)
    by_id = {c["id"]: c for c in cells}

    gate_errors = []   # coverage + per-cell validity (always).
    check_fails = []   # negative controls (--check only).

    # Join each produced cell's numerics with its manifest metadata (group/norm/mode/sr/...) by ID so
    # every metric reads ONE consistent record. A produced id with no manifest row is matrix drift.
    full = {}
    for mid, mrec in required.items():
        prod = by_id.get(mid)
        rec = dict(mrec)
        if prod is not None:
            rec.update(prod)
        full[mid] = rec

    # The per-sr idle floor (floor_sr<sr> cells). A cell at/below it is indistinguishable from the
    # machine idle; the floor cells THEMSELVES are exempt (they define the floor).
    floor_rms = {}
    for cid, prod in by_id.items():
        if cid.startswith("floor_") and produced_of(prod):
            floor_rms[int(float(prod.get("sr_hz", "0")))] = float(prod["amp_rms"])
    sr_floor = lambda rec: floor_rms.get(int(float(rec.get("sr_hz", "0"))), 0.0)

    # ---------------------------------------------------------------- coverage gate (always).
    # A cell is signal-expected (must be non-silent) only if its frequency is at/below its capped
    # natural cutoff; past the cutoff a near-zero (deep-stopband) measurement is the response, not a
    # failure. The floor cells are always exempt (they are the noise floor).
    cov_ok = 0
    for mid, mrec in required.items():
        prod = by_id.get(mid)
        if prod is None:
            gate_errors.append("required-cell-missing %s" % mid)
            continue
        r = classify(prod, sr_floor(prod))
        if r == "silent":
            try:
                sr = float(mrec.get("sr", 0)); norm = float(mrec.get("norm", 0))
                freq = float(mrec.get("freq", 0))
                expected = predicted_cap_cutoff(sr, norm) if sr > 0 else float("inf")
                in_passband = (freq <= expected)
            except (TypeError, ValueError):
                in_passband = False
            # Deep-stopband ~0 is legitimate; floor cells are exempt.
            if not in_passband or mid.startswith("floor_"):
                r = None
        if r is not None:
            gate_errors.append("invalid-cell %s [%s]" % (mid, r))
            continue
        cov_ok += 1
    if cov_ok == 0:
        gate_errors.append("0 required cells produced (empty-success)")
    n_req = len(required)
    for cid in by_id:
        if cid not in required:
            gate_errors.append("unexpected-produced-cell %s (matrix drift)" % cid)
    if len(by_id) != n_req:
        gate_errors.append("produced-id-count %d != required %d (matrix drift)" % (len(by_id), n_req))
    print("coverage: %d/%d required cells produced; produced-set=%d" % (cov_ok, n_req, len(by_id)),
          file=sys.stderr)
    if gate_errors:
        for g in gate_errors:
            print("  [cov-fail]", g, file=sys.stderr)

    # ---------------------------------------------------------------- build metric groups.
    cutoff_pts = {}   # {(sr, norm_label): [(freq, amp)]}
    cross = {}        # {(sr, mode, norm_label): {freq: amp}}
    asym = {}         # {sr: {channel: amp}}
    level = {}        # {level_id: amp}
    for cid, rec in full.items():
        if not produced_of(rec):
            continue
        g = rec["group"]
        a = float(rec["amp_rms"])
        if g == "cutoff_norm" and rec["mode"] == "lp":
            cutoff_pts.setdefault((int(float(rec["sr_hz"])), rec["norm_label"]), [])\
                .append((float(rec["freq_hz"]), a))
        elif g == "crossrate":
            cross.setdefault((int(float(rec["sr_hz"])), rec["mode"], rec["norm_label"]), {})\
                [float(rec["freq_hz"])] = a
        elif g == "asym_lr":
            asym.setdefault(int(float(rec["sr_hz"])), {})[rec["channel"]] = a
        elif g == "level":
            level[cid.split("level_", 1)[1]] = a

    srs = sorted({int(float(rec["sr_hz"])) for rec in full.values() if produced_of(rec) and is_finite_num(rec.get("sr_hz", ""))},
                 key=lambda s: s)
    normval = {"0": 0.0, "0p05": 0.05, "0p1": 0.10, "0p15": 0.15, "0p2": 0.20,
               "0p25": 0.25, "0p3": 0.30, "0p35": 0.35, "0p4": 0.40, "0p45": 0.45,
               "0p5": 0.50, "0p55": 0.55, "0p6": 0.60, "0p65": 0.65, "0p7": 0.70,
               "0p75": 0.75, "0p8": 0.80, "0p85": 0.85, "0p9": 0.90, "0p95": 0.95, "1": 1.0}

    # ---------------------------------------------------------------- deadzone (N-3a).
    dbm = build_dbm(cutoff_pts)
    print("\n--- N-3a inert-ONSET norm (normalised response curve == norm=1.0) ---")
    onsets = {}
    for sr in srs:
        labels = sorted({lbl for (s, lbl) in dbm if s == sr}, key=lambda L: normval[L])
        onset = inactive_onset(dbm, sr, labels, normval)
        onsets[sr] = onset
        theo = theoretical_onset(sr)
        # onset == the norm-1.0 reference means NO lower norm is inert: the knob stays active to the
        # top of the range, i.e. the sr/8 cap never binds -> the deadzone is absent. A source with the
        # cap removed (or the max cutoff driven below sr/8) is RED here by the deadzone-detect logic
        # itself, NOT by accidentally destabilising the filter.
        if onset is None or onset >= 0.999:
            gate_errors.append("deadzone sr%d: no inert onset (cap never activates; onset>=1.0)" % sr)
            continue
        dev = abs(onset - theo)
        if dev > ONSET_TOL:
            gate_errors.append("deadzone sr%d: onset norm=%.2f != theoretical %.3f (dev %.3f)"
                               % (sr, onset, theo, dev))
        print("  sr=%d  onset_norm=%.2f (theoretical %.3f)  cap=%.0f  inert-at-and-above-onset" % (sr, onset, theo, sr / 8.0))
    # The inert onset must be non-decreasing in sr (higher sr keeps the knob active longer).
    ons_seq = [onsets[s] for s in srs if onsets[s] is not None]
    if len(ons_seq) >= 2 and any(ons_seq[i] > ons_seq[i + 1] + 1e-9 for i in range(len(ons_seq) - 1)):
        gate_errors.append("deadzone: inert onset NOT monotonic in sr: %s (cap-free would cap flat)"
                           % ", ".join("%.2f" % v for v in ons_seq))

    # ---------------------------------------------------------------- crossrate (N-3b).
    print("\n--- N-3b crossrate rel_gain(%d) vs sr (norm=1, LP) ---" % int(CROSS_FOCUS_FREQ))
    cross_gain = {}
    for k, d in sorted(cross.items()):
        sr, mode, label = k
        if d.get(CROSS_REF_FREQ) and d.get(CROSS_FOCUS_FREQ):
            g = 20.0 * math.log10(d[CROSS_FOCUS_FREQ] / d[CROSS_REF_FREQ])
            cross_gain[k] = g
            if mode == "lp" and label in ("1", "0p95"):
                print("  sr=%d  norm=%s LP rel_gain(%d)=%.2f dB" % (sr, label, int(CROSS_FOCUS_FREQ), g))
    if (44100, "lp", "1") in cross_gain and (96000, "lp", "1") in cross_gain:
        g44 = cross_gain[(44100, "lp", "1")]
        g96 = cross_gain[(96000, "lp", "1")]
        gap = g44 - g96
        print("  N-3b: rel_gain(8k) 44.1k=%.2f dB  96k=%.2f dB  gap=%.2f dB" % (g44, g96, gap))
        if gap > -CROSS_GAP_MIN_DB:
            gate_errors.append("crossrate: 44.1k vs 96k at 8k gap=%.2f dB (need <= -%.1f dB); cap-free (no gap)"
                               % (gap, CROSS_GAP_MIN_DB))
    else:
        gate_errors.append("crossrate: missing 44.1k/96k norm=1 LP 8k reference or focus cell")

    # ---------------------------------------------------------------- asym same-input L/R.
    print("\n--- asym_lr same-input L/R (norm L=0.9, R=0.3, freq=1000) ---")
    for sr in sorted(asym):
        d = asym[sr]
        if d.get("wetL") is None or d.get("wetR") is None:
            gate_errors.append("asym sr%d: wetL or wetR missing" % sr)
            continue
        ampL, ampR = d["wetL"], d["wetR"]
        if ampR <= 0:
            gate_errors.append("asym sr%d: wetR non-positive (%.4g)" % (sr, ampR))
            continue
        asym_db = 20.0 * math.log10(ampL / ampR)
        print("  sr=%d  ampL=%.6g ampR=%.6g  |L/R|=%.2f dB" % (sr, ampL, ampR, abs(asym_db)))
        if abs(asym_db) < ASYM_MIN_DB:
            gate_errors.append("asym sr%d: |L/R|=%.2f dB < %.1f dB (R FREQ knob ignored?)"
                               % (sr, asym_db, ASYM_MIN_DB))

    # ---------------------------------------------------------------- level (composite fold).
    print("\n--- level (sr=44.1k, norm=0.9, freq=1000) — composite input-stage linearity ---")
    if len(level) == 3:
        ls, lm, ll = level["small"], level["medium"], level["large"]
        lin1 = 20.0 * math.log10(0.20 / 0.05)   # 12.04 dB if perfect linear small->medium.
        lin2 = 20.0 * math.log10(0.50 / 0.05)   # 20.00 dB if perfect linear small->large.
        d1 = 20.0 * math.log10(lm / ls)
        d2 = 20.0 * math.log10(ll / ls)
        dev_sm = d1 - lin1; dev_lg = d2 - lin2
        print("  small->medium %.2f dB (linear %.2f), small->large %.2f dB (linear %.2f)"
              % (d1, lin1, d2, lin2))
        print("  input-stage fold (composite, GH#6 tanh): dev_sm=+%.2f dB, dev_lg=+%.2f dB" % (dev_sm, dev_lg))
        if abs(dev_lg) > LEVEL_LIN_TOL_DB:
            gate_errors.append("level: input-stage not linear (large 19.86->%.2f, dev %.2f dB > %.1f)"
                               % (d2, dev_lg, LEVEL_LIN_TOL_DB))
        # NOTE: the level deviation is a COMPOSITE attribution (input stage, not the VCF response);
        # the small measured fold (~0.14 dB) confirms the small-signal assumption underpinning the
        # clean VCF attribution.

    # ---------------------------------------------------------------- N-4 note.
    print("\n  N-4 note: Chamberlin SVF is a non-generic low-pass — even at the flat res the -3 dB")
    print("  point sits at ~0.58 x the natural cutoff (measured), not at the cutoff. This is reported")
    print("  as measured evidence; no OTA / self-oscillation / genericity claim is made or inferred.")

    # ---------------------------------------------------------------- machine evidence.
    cpu_tsv = os.path.join(args.dir, "gh20_cpu.tsv")
    if os.path.exists(cpu_tsv):
        for r in read_rows(cpu_tsv):
            print("MACHINE\t" + "\t".join(str(r.get(k, "-")) for k in
                                          ("id", "value", "unit", "machine", "os", "compiler")))

    # ---------------------------------------------------------------- negative-control gate.
    if args.check:
        if not gate_errors:
            print("\n--- negative controls (tool discriminating power) ---")
            # deadzone: measured onset tracks sr/8 and is monotonic (already asserted above).
            if onsets:
                print("  deadzone: inert onset detected on real product & tracks sr/8 (see above)")
            # classifier self-checks: silence / over-scale / non-finite / not-produced.
            if classify({"ok": "1", "signal": "-", "amp_rms": "0", "amp_peak": "0", "noise_rms": "0"}) != "silent":
                check_fails.append("selfneg: silence not detected")
            else:
                print("  selfneg: silence substitution -> classified silent")
            if classify({"ok": "1", "signal": "-", "amp_rms": "0.1", "amp_peak": "2.0", "noise_rms": "1e-4"}) != "over-scale":
                check_fails.append("selfneg: over-scale not detected")
            else:
                print("  selfneg: over-scale output -> detected")
            if classify({"ok": "1", "signal": "-", "amp_rms": "nan", "amp_peak": "0.1", "noise_rms": "1e-4"}) != "non-finite:amp_rms":
                check_fails.append("selfneg: non-finite not detected")
            else:
                print("  selfneg: non-finite amp_rms -> detected")
            if classify({"ok": "0", "signal": "silent", "amp_rms": "-1", "amp_peak": "0", "noise_rms": "0"}) != "not-produced:silent":
                check_fails.append("selfneg: not-produced not detected")
            else:
                print("  selfneg: not-produced cell -> detected")
            # sr-label contract: a record declaring HALVED sr must be rejected. The measured inert onset
            # norm scales with sr, so a halved-sr response (drawn from sr but labelled sr/2) has its
            # onset at log10(sr/8/20)/3 (too high for the sr/2 prediction) -> mismatch > ONSET_TOL.
            sr_label_ok = True
            for sr in srs:
                if sr not in onsets or onsets[sr] is None:
                    continue
                ok = abs(onsets[sr] - theoretical_onset(sr)) <= ONSET_TOL      # real cell: accept.
                bad = abs(onsets[sr] - theoretical_onset(sr * 0.5)) <= ONSET_TOL  # halved-sr: reject.
                if not ok:
                    check_fails.append("sr-label sr%d: real onset %.2f not accepted by %s theory"
                                       % (sr, onsets[sr], "sr/8"))
                if bad:
                    check_fails.append("sr-label sr%d: halved-sr onset accepted (%.2f matches sr/16 theory)"
                                       % (sr, onsets[sr]))
            if sr_label_ok and not check_fails:
                print("  sr-label: real cells accepted; halved-sr records rejected")
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
        print("GATE PASS: coverage OK + N-3a deadzone / N-3b crossrate / L-R asym detected "
              "+ measurement negative controls OK. No production DSP changed.")
        return 0

    if gate_errors:
        print("COVERAGE FAIL (%d):" % len(gate_errors), file=sys.stderr)
        for g in gate_errors:
            print("  [FAIL]", g, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
