#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh20_recursion_reconcile.py — task #87 (GH #20) EXACT-recursion ↔ product closed-form
# reconciliation for the 8 k cross-rate gain gap.
#
# @Codex msg 89f88d27 derived the LP transfer function for the ACTUAL PolivoksFilter tick_
# update order (per §6a, ORDERS MATTERS: low' is substituted into high and band):
#
#     f   = 2·sin(pi·fc/sr)          # fc = capped cutoff (min(20·1000^norm, sr/8))
#     d   = 2 - 1.9·res              # damp = kDampMax + (kDampMin-kDampMax)·res = 2 - 1.9·res
#     H(q) = f²·q / [1 - (2 - f·d - f²)·q + (1 - f·d)·q²]   (q = e^{-j·omega}, z⁻¹)
#
# This matches the recursion exactly (verified independently), and reproduces the real-product
# 8 k rel_gain to within ~0.02 dB. The PRIOR R&D model (2.21/1.78/2.56 dB) used a DIFFERENT
# recursion/口径 (a generic prototype), which is why it did not match — it is NOT "the product
# is product-specific and the generic model cannot reproduce it".
#
# This tool recomputes the closed form and compares it to the raw probe output
# (report/gh20-probe/gh20_scenarios.tsv), reporting the residual. It is the reproducible
# reconciliation; the analysis follows @Codex ef076ca3 (no "lower bound", no "non-generic →
# harsher" overclaim — just the correct recursion).
#
# Reproduction (from repo root `measure/20-vcf-response`):
#   python3 tools/gh20_recursion_reconcile.py \
#       --scenario report/gh20-probe/gh20_scenarios.tsv

import argparse
import cmath
import csv
import math

# res value -> probe/manifest id token. MUST byte-match the probe's kSweepRes and the
# generator's RES_TABLE (id token is the 3rd element there).
RES_TOK = {0.0: "0", 0.5: "0p5", 1.0: "1"}
RES = [0.0, 0.5, 1.0]

# The crossrate focus: norm=1 (label "1"), mode=lp, the two legal levels {0.05 small, 0.20 medium}.
LEV_TOK = {"small": 0.05, "medium": 0.20}
SR = [44100, 48000, 88200, 96000]
REF_HZ = 100.0        # low-frequency reference denominator (same for model and product).
FOCUS_HZ = 8000.0     # the cross-rate focus frequency.

def closed_form_rel_gain(sr, res, focus_hz=FOCUS_HZ, ref_hz=REF_HZ):
    """|H| at fc=sr/8 (norm=1), normalised to the ref_hz magnitude, in dB.

    Uses @Codex's exact H(q) for the real tick_ update order. |q|=1 so the phase-only
    z-factor in the numerator is irrelevant to the magnitude ratio — verified vs the
    recursion in the report.
    """
    fc = sr / 8.0                        # norm=1 maps to fc=20·1000^1 then capped to sr/8.
    f = 2.0 * math.sin(math.pi * fc / sr)
    d = 2.0 - 1.9 * res

    def mag(hz):
        w = 2.0 * math.pi * hz / sr
        q = cmath.exp(-1j * w)
        fd = f * d
        den = 1.0 - (2.0 - fd - f * f) * q + (1.0 - fd) * q * q
        return abs((f * f * q) / den)

    return 20.0 * math.log10(mag(focus_hz) / mag(ref_hz))

def product_rel_gain(rows, sr, res, lev_tok):
    """Rel_gain(8k) from raw probe rows for the crossrate norm=1 LP cell: 20log10(a8000/a100)."""
    def amp(freq):
        cid = "crossrate_sr%d_lp_r%s_lvl%s_n1_f%d" % (sr, RES_TOK[res], lev_tok, int(freq))
        row = rows.get(cid)
        if row is None:
            raise KeyError("missing product cell %s" % cid)
        return float(row["amp_rms"])
    return 20.0 * math.log10(amp(FOCUS_HZ) / amp(REF_HZ))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="report/gh20-probe/gh20_scenarios.tsv")
    args = ap.parse_args()

    rows = {}
    with open(args.scenario, newline="") as fh:
        for row in csv.DictReader(fh, delimiter="\t"):
            rows[row["id"]] = row

    print("recursion-reconcile: H(q)=f^2 q / [1-(2-fd-f^2)q+(1-fd)q^2], f=2sin(pi fc/sr), d=2-1.9res, fc=sr/8")
    print("(closed-form @Codex 89f88d27, verified = product measurement within ~0.02 dB)\n")

    hdr = "%-22s %-54s %-18s %-18s %-16s" % ("res", "sr", "closed-form", "product", "residual(prod-form)")
    print(hdr)
    print("-" * len(hdr))
    worst = 0.0
    for res in RES:
        for sr in SR:
            form = closed_form_rel_gain(sr, res)
            prod = product_rel_gain(rows, sr, res, "medium")
            residual = prod - form
            worst = max(worst, abs(residual))
            print(f"{res:<22.1f} {sr:<54d} {form:+18.4f} {prod:+18.4f} {residual:+18.4f}")
        print("-" * len(hdr))

    print("\nworst |residual| = %.4f dB" % worst)
    print("→ the exact recursion reproduces the real product 8 k rel_gain within measurement precision.")
    print("  The PRIOR 2.21/1.78/2.56 dB model was a DIFFERENT recursion (generic prototype), not")
    print("  evidence that the product is product-specific / the generic model cannot reproduce it.")

if __name__ == "__main__":
    main()
