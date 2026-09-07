#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #86 / GH#19: RED->GREEN acceptance gate for the VCO triangle slope-correction
# (BLAMP). Compares the current analyzer output's full-band residual (blref_full_db)
# against the committed NAIVE baseline (tools/gh19_naive_baseline.tsv), cell by cell.
#
# The 24 in-scope cells and their 8/16 grouping are PINNED IN SOURCE (CANONICAL),
# NEVER derived from the input files. That makes a header-only, truncated, or
# duplicate-ridden baseline fail loudly (every required cell reports MISSING /
# duplicate / non-finite) instead of passing vacuously with "checked 0/24".
#
# Criterion:
#   * all 8 x 880 Hz A/B cells improve at least 6 dB  (more negative = better), and
#   * all 16 x 220/440 Hz A/B cells do not worsen beyond 0.5 dB.
#
# Structural rejection (exit 2): a required cell missing from baseline or current,
# a duplicate row, a non-finite value, or an sr / f0_refined value that does not match
# the cell's canonical (sr, f0_target). A triangle cell present in current but absent
# from the canonical set is also a failure (new/renamed/like-for-like-breaking).
#
# Usage:
#   python3 check_gh19_blamp_acceptance.py --baseline tools/gh19_naive_baseline.tsv \
#       --current <analyzer.tsv>
# Exit 0 = acceptance met; 1 = a criterion cell fails (RED); 2 = structural problem.
import argparse
import math
import sys

# The canonical in-scope set: 2 A/B lanes x 4 sample rates x 3 f0 targets = 24.
# Each entry maps a cell id to its (expected_sr, expected_f0_target). This is the
# SINGLE SOURCE OF TRUTH for coverage; the gate NEVER infers it from the baseline.
CANONICAL = {}
for _lane in ("vco_a_tri", "vco_b_tri"):
    for _sr in ("44100", "48000", "88200", "96000"):
        for _f0 in (220, 440, 880):
            CANONICAL[f"{_lane}_{_sr}_{_f0}"] = (int(_sr), _f0)
assert len(CANONICAL) == 24, len(CANONICAL)

# The 8/16 grouping is by the canonical f0_target, not by scanning input rows.
_880_IDS = sorted(k for k, v in CANONICAL.items() if v[1] == 880)
_OTHER_IDS = sorted(k for k, v in CANONICAL.items() if v[1] != 880)
assert len(_880_IDS) == 8, len(_880_IDS)
assert len(_OTHER_IDS) == 16, len(_OTHER_IDS)


def parse(path, analyzer):
    """Return (rows, dups, problems). rows[id] = {sr, f0, full}. The analyzer emits
    60 product cells but only the 24 `vco_{a,b}_tri` cells are in scope; EVERY other
    row (drone/sub/MACHINE/blank, whether `-`-placeholder or well-formed) is ignored
    here and is never a structural problem. Only a triangle row that is unparsable,
    duplicate, or non-finite is collected into `problems`."""
    rows, dups, problems = {}, [], []
    sr_i = 2 if analyzer else 1          # analyzer: 0=id,2=sr,4=f0,8=full; base: 1=sr
    f0_i = 4 if analyzer else 2
    full_i = 8 if analyzer else 3
    need = 9 if analyzer else 4
    for ln, line in enumerate(open(path, encoding="utf-8"), 1):
        s = line.strip()
        if not s or s.startswith("#") or s.startswith("MACHINE"):
            continue
        c = line.rstrip("\n").split("\t")
        if len(c) < need or c[0].strip() == "id":
            continue
        cid = c[0].strip()
        if not (cid.startswith("vco_a_tri") or cid.startswith("vco_b_tri")):
            continue                          # out-of-scope cell -> ignore silently.
        try:
            sr = int(c[sr_i].strip())
            f0 = float(c[f0_i])
            full = float(c[full_i])
        except ValueError:
            problems.append(f"{path}:{ln}: unparsable numeric row {cid!r}")
            continue
        if not (math.isfinite(f0) and math.isfinite(full)):
            problems.append(f"{path}:{ln}: non-finite value in {cid}")
            continue
        if cid in rows:
            dups.append(cid)
            continue
        rows[cid] = {"sr": sr, "f0": f0, "full": full}
    return rows, dups, problems


def main():
    ap = argparse.ArgumentParser(description="GH#19 BLAMP acceptance gate")
    ap.add_argument("--baseline", required=True, help="committed naive baseline TSV")
    ap.add_argument("--current", required=True, help="current (BLAMP) analyzer TSV")
    ap.add_argument("--min-imp-880", type=float, default=6.0,
                    help="min dB improvement at 880 (default 6.0)")
    ap.add_argument("--max-worsen-other", type=float, default=0.5,
                    help="max dB worsening at 220/440 (default 0.5)")
    ap.add_argument("--f0-tol", type=float, default=1.0,
                    help="Hz tolerance for f0_refined vs canonical target (default 1.0)")
    args = ap.parse_args()

    base, base_dups, base_prob = parse(args.baseline, analyzer=False)
    cur, cur_dups, cur_prob = parse(args.current, analyzer=True)

    fails = [*base_prob, *cur_prob]
    # A triangle-looking cell that is NOT canonical = new/renamed/like-for-like-breaking.
    for k, v in sorted(cur.items()):
        if k not in CANONICAL and k.startswith(("vco_a_tri", "vco_b_tri")):
            fails.append(f"{k}: triangle cell in current is NOT in the canonical 24-set "
                         f"(new/renamed -> not like-for-like)")
    for k in sorted(set(base_dups) | set(cur_dups)):
        fails.append(f"{k}: duplicate row in input")

    print("GH#19 BLAMP acceptance  (baseline=naive triangle, current=BLAMP triangle)\n")
    print(f"{'cell':<24}{'sr':<7}{'f0':<8}{'naive':<9}{'blamp':<9}{'dB_imp':<8}"
          f"{'rule':<12}{'verdict'}")

    checked_880 = 0
    checked_other = 0
    # Iterate the CANONICAL set, not the baseline file. Data rows in baseline/current
    # that are NOT canonical are either already flagged above or ignored.
    for cid in CANONICAL:
        exp_sr, exp_f0 = CANONICAL[cid]
        if cid not in base:
            fails.append(f"{cid}: MISSING from naive baseline (a required cell has no "
                         f"reference -> baseline truncated/vacuous?)")
            continue
        if cid not in cur:
            fails.append(f"{cid}: MISSING from current analyzer output")
            continue
        b, c = base[cid], cur[cid]
        if b["sr"] != exp_sr or c["sr"] != exp_sr:
            fails.append(f"{cid}: sr mismatch (canonical {exp_sr}, baseline {b['sr']}, "
                         f"current {c['sr']})")
            continue
        for where, row in (("baseline", b), ("current", c)):
            if abs(row["f0"] - exp_f0) > args.f0_tol:
                fails.append(f"{cid}: f0 mismatch in {where} ({row['f0']:.2f} vs "
                             f"target {exp_f0})")
                break
        else:
            # f0 targets both aligned -> compare residuals.
            nb, cb = b["full"], c["full"]
            imp = nb - cb                        # positive = more band-limited (better)
            if exp_f0 == 880:
                rule = f"880:>={args.min_imp_880:g}dB"
                ok = imp >= args.min_imp_880
                if ok:
                    checked_880 += 1
            else:
                rule = f"220/440:!worse>{args.max_worsen_other:g}dB"
                ok = imp >= -args.max_worsen_other
                if ok:
                    checked_other += 1
            verdict = "GREEN" if ok else "RED"
            if not ok:
                fails.append(f"{cid}: {rule} -> imp={imp:.2f}dB")
            print(f"{cid:<24}{c['sr']:<7}{c['f0']:<8.2f}{nb:<9.2f}{cb:<9.2f}"
                  f"{imp:<8.2f}{rule:<12}{verdict}")
        # (the for/else runs exactly once per well-formed cell; missing/sr/f0 handled above)

    print(f"\nBins (pinned): 880Hz passed={checked_880}/8, 220/440Hz passed={checked_other}/16")
    if fails:
        print("\nFAIL:")
        for f in fails:
            print(f"  - {f}")
        # structural (2) vs criterion (1): if any structural problem -> 2
        structural = any("MISSING" in f or "duplicate" in f or "mismatch" in f
                         or "non-finite" in f or "NOT in the canonical" in f
                         or "too few columns" in f or "unparsable" in f
                         or "vacuous" in f for f in fails)
        return 2 if structural else 1
    print("\nPASS: all 8 x 880 Hz + 16 x 220/440 Hz A/B triangle cells meet the criterion.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
