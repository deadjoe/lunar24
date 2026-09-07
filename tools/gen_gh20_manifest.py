#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gen_gh20_manifest.py — task #87 (GH #20) REQUIRED-cell coverage contract.
#
# Emits the committed tools/gh20_manifest.tsv that enumerates EVERY cell the probe MUST produce
# (tests/probes/gh20_vcf_probe.cpp). The analyzer (tools/gh20_vcf_analyze.py) compares the
# produced-id set against THIS manifest: on a clean run they match byte-for-byte; any required id
# with no produced row is a hard FAIL (fail-closed coverage, BLOCK item ①), and 0 produced ids
# is an empty-success FAIL.
#
# The cells here mirror the probe's matrix EXACTLY. The id schema — integer sr/freq and the
# norm-label token — is shared verbatim with the probe (normGrid()/probeFreqs() there, and the
# same literals here); gen_gh20_manifest.py is the single authoring source for the contract and is
# re-run whenever the probe's matrix changes. A divergence is caught immediately by the analyzer's
# coverage gate (a required id with no row, or a produced id with no manifest row).
#
# Reproduction:
#   python3 tools/gen_gh20_manifest.py --out tools/gh20_manifest.tsv

import argparse

# ---- SHARED matrix spec (must byte-match tests/probes/gh20_vcf_probe.cpp normGrid() AND the
#      analyzer normval table). 21 points, step 0.05 (per the GH#20 mandate "norm>=21 points"). ----
SR_S = [44100, 48000, 88200, 96000]
NORM_GRID = [
    (0.00, "0"), (0.05, "0p05"), (0.10, "0p1"), (0.15, "0p15"), (0.20, "0p2"),
    (0.25, "0p25"), (0.30, "0p3"), (0.35, "0p35"), (0.40, "0p4"), (0.45, "0p45"),
    (0.50, "0p5"), (0.55, "0p55"), (0.60, "0p6"), (0.65, "0p65"), (0.70, "0p7"),
    (0.75, "0p75"), (0.80, "0p8"), (0.85, "0p85"), (0.90, "0p9"), (0.95, "0p95"), (1.00, "1"),
]
FREQS = {
    44100: [60, 100, 200, 400, 800, 1200, 1600, 2500, 3500, 4500, 5500, 6500, 7000, 8000, 9000],
    48000: [60, 100, 200, 400, 800, 1200, 1600, 2500, 3500, 5000, 6000, 7000, 8000, 9000, 10000],
    88200: [60, 100, 200, 400, 800, 1500, 2500, 4000, 6000, 8000, 10000, 12000, 14000, 16000, 18000],
    96000: [60, 100, 200, 400, 800, 1500, 2500, 4000, 6000, 8000, 10000, 12000, 15000, 18000, 20000],
}
CROSS_MIN_NORM = 0.85       # only the cap-zone / high-res norms for the 8k cross-rate focus.
CROSS_FREQS = [100, 8000]
ASYM_L_FREQ, ASYM_R_FREQ = 0.90, 0.30

# @Codex correction 1 (e3d4211e): restore low/mid/high res + two legal input levels. The res enters
# ONLY via the damp coefficient (damp = 2.0 + (0.1-2.0)*res), so a res=0-only sweep was masking
# res-dependence of the 8k cross-rate gain, the stability margin, and the response shape. Legal res =
# {0.0 flat, 0.5 mid, 1.0 max}; two legal input levels {0.05, 0.20} bound the input-stage (drive=0 =>
# linear passthrough) so the VCF attribution holds at both.
#   (value, id-token, manifest-res-column) — the id token and the manifest `res` column must byte-match
#   the probe's emit AND the analyzer's grouping key.
RES_TABLE = [
    (0.0, "0", "0"),
    (0.5, "0p5", "0.5"),
    (1.0, "1", "1"),
]
#   (id-token, manifest-lvl-column) — the id token is the existing level-group token ("small"/"medium");
#   the manifest `lvl` column is the numeric level value.
LEVEL_TABLE = [
    ("small", "0.05"),
    ("medium", "0.2"),
]
LEVEL_IDS = ["small", "medium"]            # two legal input levels (0.05, 0.20).
LEVEL_VALS = {"small": 0.05, "medium": 0.2}


def _res_col(value):      # numeric res value -> manifest column string (byte-matched to the probe).
    for v, _tok, col in RES_TABLE:
        if abs(v - value) < 1e-12:
            return col
    raise ValueError("unknown res value %r" % value)


def _res_tok(value):      # numeric res value -> id token.
    for v, tok, _col in RES_TABLE:
        if abs(v - value) < 1e-12:
            return tok
    raise ValueError("unknown res value %r" % value)


def cells():
    """Yield (id, dict-of-schema-columns) for EVERY required cell, matching the probe."""
    # --- floor per sample rate (id floor_<sr>, matching the probe's emit format). ---
    for sr in SR_S:
        yield ("floor_%d" % sr,
               {"group": "floor", "sr": sr, "mode": "lp", "res": "0", "norm": "0.5",
                "norm_label": "-", "lvl": "0", "freq": "0", "channel": "wetL", "required": 1})
    # --- cutoff_norm: sr x mode(LP|BP) x norm x freq sweep, res x level. The -3 dB curve and the sr/8
    #     plateau onset are measured PER (mode, res, level) — the res-dependence of the response SHAPE —
    #     and the plateau onset must be res-independent (cap ≠ res-dependent) per the R&D. BOTH modes are
    #     swept at the full 21-point norm (mandate "LP/BP, norm>=21 points", @Codex 89f88d27 item ②). ---
    for sr in SR_S:
        for mode in ("bp", "lp"):
            for norm, label in NORM_GRID:
                for freq in FREQS[sr]:
                    for rv, rtok, _rc in RES_TABLE:
                        for ltok, lcol in LEVEL_TABLE:
                            yield ("cutoff_norm_sr%d_%s_r%s_lvl%s_n%s_f%d" % (sr, mode, rtok, ltok, label, freq),
                                   {"group": "cutoff_norm", "sr": sr, "mode": mode,
                                    "res": _res_col(rv), "norm": "%.2f" % norm,
                                    "norm_label": label, "lvl": lcol, "freq": str(freq),
                                    "channel": "wetL", "required": 1})
    # --- crossrate: sr x mode(LP|BP) x norm(>=0.85) x {100,8000} x res x level. The 8k cross-rate
    #     gain gap is the RES-DEPENDENT finding (model 2.21/1.78/2.56 dB); per-res here is the point. ---
    for sr in SR_S:
        for mode in ("bp", "lp"):
            for norm, label in NORM_GRID:
                if norm < CROSS_MIN_NORM:
                    continue
                for freq in CROSS_FREQS:
                    for rv, rtok, _rc in RES_TABLE:
                        for ltok, lcol in LEVEL_TABLE:
                            yield ("crossrate_sr%d_%s_r%s_lvl%s_n%s_f%d" % (sr, mode, rtok, ltok, label, freq),
                                   {"group": "crossrate", "sr": sr, "mode": mode,
                                    "res": _res_col(rv), "norm": "%.2f" % norm,
                                    "norm_label": label, "lvl": lcol, "freq": str(freq),
                                    "channel": "wetL", "required": 1})
    # --- asym_lr: wetL at norm 0.9, wetR at norm 0.3, SAME input, freq 1000. ---
    for sr in SR_S:
        yield ("asym_lr_sr%d_wetL" % sr,
               {"group": "asym_lr", "sr": sr, "mode": "lp", "res": "0", "norm": "0.90",
                "norm_label": "-", "lvl": "0.05", "freq": "1000", "channel": "wetL", "required": 1})
        yield ("asym_lr_sr%d_wetR" % sr,
               {"group": "asym_lr", "sr": sr, "mode": "lp", "res": "0", "norm": "0.30",
                "norm_label": "-", "lvl": "0.05", "freq": "1000", "channel": "wetR", "required": 1})
    # --- level: sr=44.1k, LP, res=0, norm=0.9, freq=1000, 3 input levels. ---
    for lid in LEVEL_IDS:
        yield ("level_" + lid,
               {"group": "level", "sr": 44100, "mode": "lp", "res": "0", "norm": "0.90",
                "norm_label": "-", "lvl": str(LEVEL_VALS[lid]), "freq": "1000",
                "channel": "wetL", "required": 1})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tools/gh20_manifest.tsv")
    args = ap.parse_args()
    hdr = ("id\tgroup\tsr\tmode\tres\tnorm\tnorm_label\tlvl\tfreq\tchannel\trequired")
    rows = [hdr]
    for cid, col in cells():
        rows.append("\t".join([
            cid, col["group"], str(col["sr"]), col["mode"], col["res"],
            col["norm"], col["norm_label"], col["lvl"], col["freq"], col["channel"],
            str(col["required"])]))
    with open(args.out, "w") as fh:
        fh.write("\n".join(rows) + "\n")
    print("gen_gh20_manifest: wrote %d required cells to %s" % (len(rows) - 1, args.out))


if __name__ == "__main__":
    main()
