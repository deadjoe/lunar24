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
LEVEL_IDS = ["small", "medium", "large"]
LEVEL_VALS = {"small": 0.05, "medium": 0.20, "large": 0.50}


def cells():
    """Yield (id, dict-of-schema-columns) for EVERY required cell, matching the probe."""
    # --- floor per sample rate (id floor_<sr>, matching the probe's emit format). ---
    for sr in SR_S:
        yield ("floor_%d" % sr,
               {"group": "floor", "sr": sr, "mode": "lp", "res": "0", "norm": "0.5",
                "norm_label": "-", "lvl": "0", "freq": "0", "channel": "wetL", "required": 1})
    # --- cutoff_norm: sr x norm x freq sweep, mode=LP, res=0. ---
    for sr in SR_S:
        for norm, label in NORM_GRID:
            for freq in FREQS[sr]:
                yield ("cutoff_norm_sr%d_n%s_f%d" % (sr, label, freq),
                       {"group": "cutoff_norm", "sr": sr, "mode": "lp", "res": "0",
                        "norm": ("%.2f" % norm) if abs(norm - round(norm, 2)) > 0 else "%.2f" % norm,
                        "norm_label": label, "lvl": "0.05", "freq": str(freq),
                        "channel": "wetL", "required": 1})
    # --- crossrate: sr x mode(LP|BP) x norm(>=0.85) x {100,8000}. ---
    for sr in SR_S:
        for mode in ("bp", "lp"):
            for norm, label in NORM_GRID:
                if norm < CROSS_MIN_NORM:
                    continue
                for freq in CROSS_FREQS:
                    yield ("crossrate_sr%d_%s_n%s_f%d" % (sr, mode, label, freq),
                           {"group": "crossrate", "sr": sr, "mode": mode, "res": "0",
                            "norm": "%.2f" % norm, "norm_label": label, "lvl": "0.05",
                            "freq": str(freq), "channel": "wetL", "required": 1})
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
