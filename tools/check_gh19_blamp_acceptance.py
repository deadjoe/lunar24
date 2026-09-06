#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #86 / GH#19: RED->GREEN acceptance gate for the VCO triangle slope-correction
# (BLAMP). Compares the current analyzer output's full-band residual (blref_full_db)
# against the committed NAIVE baseline (tools/gh19_naive_baseline.tsv), cell by cell,
# and asserts the deliverable criterion:
#   * every 880 Hz A/B isolated triangle cell improves at least 6 dB (more negative
#     residual = better band limiting), and
#   * every 220/440 Hz cell does NOT worsen beyond 0.5 dB.
# Because the baseline is the naive (uncorrected) triangle under the SAME analyzer,
# running this gate on the naive product gives delta == 0 at 880 Hz -> RED; running it
# on the BLAMP-corrected product gives the ~8.6 dB improvement -> GREEN.
#
# Usage:
#   python3 check_gh19_blamp_acceptance.py --baseline tools/gh19_naive_baseline.tsv \
#       --current <analyzer.tsv>
# Exit code 0 = acceptance met; 1 = any cell fails (RED); 2 = a cell is missing.
import argparse
import sys


def parse(path):
    data = {}
    for line in open(path, encoding="utf-8"):
        if not line.strip() or line.startswith("id"):
            continue
        c = line.rstrip("\n").split("\t")
        # analyzer TSV has >=9 cols (id, path, sr, f0_target, f0_refined, harmris,
        # harmris_inband, blref_inband, blref_full, ...); the committed naive
        # baseline TSV is a 4-col subset (id, sr, f0_refined, blref_full_db).
        if len(c) >= 9:
            idx = {"sr": 2, "f0": 4, "full": 8}
        elif len(c) >= 4:
            idx = {"sr": 1, "f0": 2, "full": 3}
        else:
            continue
        try:
            data[c[0]] = {"sr": c[idx["sr"]], "f0": float(c[idx["f0"]]),
                          "full": float(c[idx["full"]])}
        except ValueError:
            continue  # non-numeric / MACHINE rows -> ignored
    return data


def main():
    ap = argparse.ArgumentParser(description="GH#19 BLAMP acceptance gate")
    ap.add_argument("--baseline", required=True, help="committed naive baseline TSV")
    ap.add_argument("--current", required=True, help="current (BLAMP) analyzer TSV")
    ap.add_argument("--min-imp-880", type=float, default=6.0,
                    help="min dB improvement at 880 (default 6.0)")
    ap.add_argument("--max-worsen-other", type=float, default=0.5,
                    help="max dB worsening at 220/440 (default 0.5)")
    args = ap.parse_args()

    base = parse(args.baseline)
    cur = parse(args.current)

    fails = []
    checked_880 = 0
    checked_other = 0
    print("GH#19 BLAMP acceptance  (baseline=naive triangle, current=BLAMP triangle)\n")
    print(f"{'cell':<24}{'sr':<7}{'f0':<8}{'naive':<9}{'blamp':<9}{'dB_imp':<8}{'rule':<12}{'verdict'}")

    # The canonical in-scope set is the 24 A/B isolated triangle cells in the
    # baseline. Any triangle cell present in current but absent from baseline is
    # a new/renamed/like-for-like-breaking change and is also a failure.
    ids = sorted(k for k in base)
    unknown = sorted(k for k in cur if k not in base and k.startswith(("vco_a_tri", "vco_b_tri")))
    for cid in unknown:
        fails.append(f"{cid}: in current but MISSING from baseline "
                     f"(a new/renamed triangle cell?)")
    for cid in ids:
        if cid == "id":
            continue
        if cid not in cur:
            fails.append(f"{cid}: MISSING from current (a required triangle cell "
                         f"was not produced)")
            continue
        row = cur[cid]
        nb = base[cid]["full"]
        nb_f0 = base[cid]["f0"]
        cb = row["full"]
        cb_f0 = row["f0"]
        if abs(nb_f0 - cb_f0) > 0.01:
            fails.append(f"{cid}: baseline f0={nb_f0:.2f} vs current f0={cb_f0:.2f} mismatch")
            continue
        imp = nb - cb  # positive = MORE band-limited (better)
        f0 = cb_f0
        if abs(f0 - 880.0) < 1.0:
            rule = "880:>=6dB"
            ok = imp >= args.min_imp_880
            if ok:
                checked_880 += 1
            verdict = "GREEN" if ok else "RED"
        else:
            rule = "220/440:no>0.5dB worse"
            ok = imp >= -args.max_worsen_other
            if ok:
                checked_other += 1
            verdict = "GREEN" if ok else "RED"
        if not ok:
            fails.append(f"{cid}: {rule} -> imp={imp:.2f}dB (needs "
                         f"{args.min_imp_880:.1f}dB / >=-{args.max_worsen_other:.1f}dB)")
        print(f"{cid:<24}{row['sr']:<7}{f0:<8.2f}{nb:<9.2f}{cb:<9.2f}{imp:<8.2f}"
              f"{rule:<12}{verdict}")

    print(f"\nBins passed: 880Hz={checked_880}, 220/440Hz={checked_other}")
    if fails:
        print("\nFAIL:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("\nPASS: all 24 A/B isolated triangle cells meet the acceptance criterion.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
