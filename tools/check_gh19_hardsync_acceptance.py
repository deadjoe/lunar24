#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #111 / GH#19 S5: RED->GREEN acceptance gate for the VCO-A HARD-SYNC path
# (reset-discontinuity anti-aliasing). Compares the current analyzer output's
# hard-sync residual (blsync_full_db) against the committed NAIVE baseline
# (tools/gh19_hardsync_naive_baseline.tsv), cell by cell.
#
# WHAT THE CELLS ARE. `vco_a.sync_in` is a registered, patchable jack that had no consumer
# before task #111. The probe patches LFO-A (square, x10 speed mult) to it through the public
# MachineRuntime::connect, producing exactly one rising edge every M samples (M=256 at
# 44.1/48 kHz, 512 at 88.2/96 kHz). The slave is the same dry_a-tapped VCO-A triangle arm as
# `vco_a_tri`. So each cell is a triangle whose phase is truncated and re-anchored once per
# sync period -- a VALUE discontinuity, which is why the correction is polyBLEP-family and
# not the BLAMP family of the free-running triangle.
#
# BASELINE = the NAIVE arm: the same wiring driven through Vco::syncPulse() (immediate
# mid-sample reset, no band-limited jump), i.e. the behaviour the S5 correction replaces. The
# baseline column is rendered BEFORE any correction is applied and committed, so the criterion
# is a measured gain, never a restatement of the corrected number.
#
# CRITERION (all per cell, more negative = more band-limited):
#   1. blsync_full_db improves on the naive arm by >= --min-gain-db, and by
#      >= --min-gain-high-db on the 880 Hz cells (largest triangle slope at the reset, so the
#      hardest case);
#   2. THE ANCHOR MUST BE SHARP: blsync_gap_db >= --min-gap-db. Unlike the free-running VCO
#      cells, the hard-sync cycle start is NOT known analytically (the reset sample holds the
#      band-limited MIDPOINT of the jump), so the alignment is MEASURED by scanning the
#      reference over all M offsets. If that scan had a flat minimum the measured anchor would
#      be arbitrary and the residual meaningless, so the gap is a REQUIREMENT, not a diagnostic.
#   3. THE PREMISE MUST HOLD: blsync_per <= --max-period-dev. The whole method is licensed on
#      the produced signal being exactly M-periodic; the analyzer fails closed (emits no
#      number) when it is not, and a number that arrives with a large deviation is not
#      admissible. A cell with no criterion at all is a STRUCTURAL failure (exit 2).
#
# Structural rejection (exit 2): a required cell missing from baseline or current, a duplicate
# row, an absent ("-") criterion -- which is how the analyzer reports "I refused to answer" --
# a non-finite value, or an sr / f0 that does not match the cell's canonical (sr, f0_target).
# A sync-looking cell present in current but absent from the canonical set is also a failure
# (new/renamed/like-for-like-breaking).
#
# Exit 0 = acceptance met; 1 = a criterion cell fails (RED); 2 = structural problem.
#
# Usage:
#   python3 check_gh19_hardsync_acceptance.py --baseline tools/gh19_hardsync_naive_baseline.tsv \
#       --current <analyzer.tsv> [--self-check]
import argparse
import math
import os
import re
import sys
import tempfile

# The canonical in-scope set: one A lane x 4 sample rates x 3 f0 targets = 12. Each entry maps a
# cell id to its (expected_sr, expected_f0_target). SINGLE SOURCE OF TRUTH for coverage; the gate
# NEVER infers it from the input files, so a truncated or empty baseline fails loudly instead of
# passing vacuously with "checked 0/12".
CANONICAL = {}
for _sr in (44100, 48000, 88200, 96000):
    for _f0 in (220, 440, 880):
        CANONICAL[f"vco_a_sync_tri_{_sr}_{_f0}"] = (_sr, _f0)
assert len(CANONICAL) == 12, len(CANONICAL)

# The 4/8 grouping is by the canonical f0_target, not by scanning input rows.
_HIGH_IDS = sorted(k for k, v in CANONICAL.items() if v[1] == 880)
_LOW_IDS = sorted(k for k, v in CANONICAL.items() if v[1] != 880)
assert len(_HIGH_IDS) == 4, len(_HIGH_IDS)
assert len(_LOW_IDS) == 8, len(_LOW_IDS)

# Analyzer TSV column indices (name-based header, index pinned here so the gate reads the same
# columns the analyzer prints). Baseline TSV is the compact id/sr/f0/criterion form.
#
# A_F0 IS THE *DECLARED* f0_target (column index 3), NOT f0_refined (index 4). On the free-running
# VCO cells the two coincide (vco_a_tri_44100_440 prints target 440, refined 440.00), which is why
# reading index 4 looked right. They do NOT coincide on these cells, and the reason is structural:
# a hard-synced triangle is exactly M-periodic, so its only spectral lines sit at multiples of
# sr/M (172.27 Hz at 44.1 kHz), and the nearest lines to the seed's window fall OUTSIDE
# refine_f0's +-15% search span. With no line inside the window the refiner rails to the span
# edge -- measured: 504.80 Hz for a 440 Hz target (span edge 506.0) and exactly 187.00 for a
# 220 Hz target (span edge 187.0). f0_refined is therefore a search-boundary artifact on these
# cells, not a pitch, and cannot serve as an identity anchor. The analyzer agrees and never uses
# it here: method_bl_sync is seeded with the DECLARED f0 by design, so a frequency error shows up
# as residual instead of being absorbed into the reference (gh19_alias_analyze.py, sync_is_cell).
# The CROSS-CHECK is correspondingly weaker than on the free-running cells: it verifies the row's
# declared label against the id's target (catching a mislabelled scenario) and does NOT verify
# that a measurement locked onto that frequency.
A_ID, A_SR, A_F0, A_CRIT, A_GAP, A_PER = 0, 2, 3, 10, 11, 14
B_ID, B_SR, B_F0, B_CRIT = 0, 1, 2, 3
ZERO_EPS = 0.005          # dB band within which a delta counts as EXACTLY zero (removal control)

# The analyzer's OWN header literal is the layout authority for the fixtures below. A hand-written
# fixture header that merely AGREES WITH THIS FILE'S INDEX MAP cannot catch a wrong index map -- the
# two share the error, and the self-check passes while no real row can ever be accepted. So the
# names are read out of the producer, and `self_check` asserts the pinned indices against them.
ANALYZE_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gh19_alias_analyze.py")


def analyzer_header(path=ANALYZE_PY):
    """The analyzer's printed column names, verbatim and in order (parsed from its hdr literal).

    The literal is SOURCE TEXT, so its separators arrive as a backslash and a `t` rather than as
    tabs -- `split("\\t")` on the two characters, not on a real tab, or the whole header comes back
    as one name."""
    txt = open(path, encoding="utf-8").read()
    i = txt.index("hdr = (")
    block = txt[i:txt.index(")", i)]          # the literal holds no ')' of its own
    hdr = "".join(re.findall(r'"([^"]*)"', block)).split("\\t")
    if len(hdr) < 2:
        raise ValueError(f"parsed {len(hdr)} column name(s) from {path} -- wrong delimiter?")
    return hdr


# Resolved at import, but a failure here is NOT fatal: judging a run never needs the header, so
# only --self-check reports it (and then as a failure, never as a silent skip).
try:
    HDR = analyzer_header()
    A_F0_REFINED = HDR.index("f0_refined")
except Exception:                             # analyzer absent/renamed -> self-check fails loudly
    HDR, A_F0_REFINED = None, None


def parse(path, analyzer):
    """Return (rows, dups, problems). rows[id] = {sr, f0, crit, gap, per}. Only hard-sync cells are
    in scope; every other row (the free-running VCO cells, drone / sub / composite / MACHINE rows,
    whether `-`-placeholder or well-formed) is ignored here and is never a structural problem."""
    rows, dups, problems = {}, [], []
    if analyzer:
        sr_i, f0_i, crit_i, gap_i, per_i, need = A_SR, A_F0, A_CRIT, A_GAP, A_PER, A_PER + 1
    else:
        sr_i, f0_i, crit_i, gap_i, per_i, need = B_SR, B_F0, B_CRIT, None, None, B_CRIT + 1
    try:
        fh = open(path, encoding="utf-8")
    except OSError as e:
        return rows, dups, [f"{path}: cannot read ({e.strerror})"]
    with fh:
        for ln, line in enumerate(fh, 1):
            s = line.strip()
            if not s or s.startswith("#") or s.startswith("MACHINE"):
                continue
            c = line.rstrip("\n").split("\t")
            if len(c) < need or c[0].strip() == "id":
                continue
            cid = c[0].strip()
            if not cid.startswith("vco_a_sync_tri"):
                continue                      # out-of-scope cell -> ignore silently.
            if c[crit_i].strip() == "-":
                # The analyzer's fail-closed answer: the method's premise did not hold, so there is
                # NO criterion. That is a structural problem, not a RED cell -- a RED cell would mean
                # "measured and failed", which is a different and weaker statement.
                problems.append(f"{path}:{ln}: {cid} has NO criterion column value "
                                f"(analyzer failed closed: method premise not met)")
                continue
            try:
                sr = int(float(c[sr_i].strip()))
                f0 = float(c[f0_i])
                crit = float(c[crit_i])
            except ValueError:
                problems.append(f"{path}:{ln}: unparsable numeric row {cid!r}")
                continue
            if not (math.isfinite(f0) and math.isfinite(crit)):
                problems.append(f"{path}:{ln}: non-finite value in {cid}")
                continue
            gap = per = None
            if analyzer:
                try:
                    gap = float(c[gap_i])
                    per = float(c[per_i])
                except ValueError:
                    problems.append(f"{path}:{ln}: unparsable admissibility columns in {cid}")
                    continue
                if not (math.isfinite(gap) and math.isfinite(per)):
                    problems.append(f"{path}:{ln}: non-finite admissibility value in {cid}")
                    continue
            if cid in rows:
                dups.append(cid)
                continue
            rows[cid] = {"sr": sr, "f0": f0, "crit": crit, "gap": gap, "per": per}
    return rows, dups, problems


def evaluate(baseline_path, current_path, min_low, min_high, min_gap, max_per, f0_tol):
    """The whole gate as a pure function of two files. Returns (rc, report_lines)."""
    base, base_dups, base_prob = parse(baseline_path, analyzer=False)
    cur, cur_dups, cur_prob = parse(current_path, analyzer=True)

    fails = [*base_prob, *cur_prob]
    # A sync-looking cell that is NOT canonical = new/renamed/like-for-like-breaking.
    for k in sorted(cur):
        if k not in CANONICAL:
            fails.append(f"{k}: hard-sync cell in current is NOT in the canonical 12-set "
                         f"(new/renamed -> not like-for-like)")
    for k in sorted(base):
        if k not in CANONICAL:
            fails.append(f"{k}: hard-sync cell in baseline is NOT in the canonical 12-set")
    for k in sorted(set(base_dups) | set(cur_dups)):
        fails.append(f"{k}: duplicate row in input")

    out = ["GH#19 S5 hard-sync acceptance  (baseline=naive immediate reset, "
           "current=deferred reset + band-limited jump)", ""]
    out.append(f"{'cell':<26}{'sr':<7}{'f0':<7}{'naive':<10}{'current':<10}{'dB_gain':<9}"
               f"{'gap':<8}{'per':<10}{'rule':<18}{'verdict'}")

    ok_high = ok_low = 0
    zero_deltas = 0
    for cid in sorted(CANONICAL):
        exp_sr, exp_f0 = CANONICAL[cid]
        if cid not in base:
            fails.append(f"{cid}: MISSING from naive baseline (a required cell has no reference "
                         f"-> baseline truncated/vacuous?)")
            continue
        if cid not in cur:
            fails.append(f"{cid}: MISSING from current analyzer output")
            continue
        b, c = base[cid], cur[cid]
        if b["sr"] != exp_sr or c["sr"] != exp_sr:
            fails.append(f"{cid}: sr mismatch (canonical {exp_sr}, baseline {b['sr']}, "
                         f"current {c['sr']})")
            continue
        bad = None
        for where, row in (("baseline", b), ("current", c)):
            if abs(row["f0"] - exp_f0) > f0_tol:
                bad = f"{cid}: f0 mismatch in {where} ({row['f0']:.2f} vs target {exp_f0})"
                break
        if bad:
            fails.append(bad)
            continue

        nb, cb = b["crit"], c["crit"]
        imp = nb - cb                          # positive = more band-limited (better)
        if abs(imp) <= ZERO_EPS:
            zero_deltas += 1
        if exp_f0 == 880:
            rule = f"880:>={min_high:g}dB"
            ok = imp >= min_high
            counts_as = "high"
        else:
            rule = f"220/440:>={min_low:g}dB"
            ok = imp >= min_low
            counts_as = "low"
        # (2) the measured anchor must be sharp, or the residual is not admissible evidence.
        if ok and not (c["gap"] >= min_gap):
            rule = f"gap>={min_gap:g}dB"
            ok = False
        # (3) the M-periodicity premise the method is licensed on must actually hold.
        if ok and not (c["per"] <= max_per):
            rule = f"per<={max_per:g}"
            ok = False
        if ok:
            if counts_as == "high":
                ok_high += 1
            else:
                ok_low += 1
        else:
            fails.append(f"{cid}: {rule} -> dB_gain={imp:.2f} gap={c['gap']:.2f} "
                         f"per={c['per']:.2e}")
        out.append(f"{cid:<26}{c['sr']:<7}{c['f0']:<7.2f}{nb:<10.2f}{cb:<10.2f}{imp:<9.2f}"
                   f"{c['gap']:<8.2f}{c['per']:<10.2e}{rule:<18}"
                   f"{'GREEN' if ok else 'RED'}")

    out.append("")
    out.append(f"Bins (pinned): 880Hz passed={ok_high}/4, 220/440Hz passed={ok_low}/8")
    # This line is the removal control's NAMED evidence: with the correction absent the two arms
    # are the same signal, so every delta is exactly zero and the criterion cannot be satisfied.
    out.append(f"exact-zero deltas (correction absent): "
               f"{'YES' if zero_deltas == len(CANONICAL) else 'no'} ({zero_deltas}/12)")
    if fails:
        out.append("")
        out.append("FAIL:")
        for f in fails:
            out.append(f"  - {f}")
        structural = any(("MISSING" in f or "duplicate" in f or "mismatch" in f
                          or "non-finite" in f or "NOT in the canonical" in f
                          or "unparsable" in f or "vacuous" in f or "NO criterion" in f
                          or "cannot read" in f) for f in fails)
        return (2 if structural else 1), out
    out.append("")
    out.append("PASS: all 12 hard-sync cells improve on the naive arm with a sharp measured "
               "anchor and a holding M-periodicity premise.")
    return 0, out


# ---------------------------------------------------------------------------------------------
# --self-check: the gate's own negative controls. Each MUTATES a synthetic input pair and asserts
# BOTH the exit code and the named rule, so a control cannot pass by failing for the wrong reason.
# The synthetic values are self-consistent (baseline worse than current by 12 dB everywhere), and
# a "make it pass" arm proves the controls are not merely asserting failure-on-anything.
# ---------------------------------------------------------------------------------------------
_SYN_BASE = 18.0
_SYN_CUR = 6.0
_SYN_GAP = 14.0
_SYN_PER = 0.0


def _syn_rows():
    rows = {}
    for cid, (sr, f0) in CANONICAL.items():
        rows[cid] = (sr, f0)
    return rows


def _write_cur(path, over=None, crit_raw=None):
    """Analyzer-format TSV with the pinned column indices. `over` maps cid ->
    (crit, gap, per) overrides; `crit_raw` maps cid -> a VERBATIM string for the criterion
    column (used to write the analyzer's fail-closed "-").

    The header is the REAL analyzer header (see analyzer_header), and the refined-f0 column is
    populated with the value the real cells carry -- railed to the +15% span edge, i.e. NOT the
    target. That is deliberate: it makes the positive arm fail if A_F0 is ever pointed back at
    f0_refined, so this exact off-by-one cannot return unnoticed."""
    hdr = HDR
    lines = ["\t".join(hdr)]
    for cid, (sr, f0) in sorted(CANONICAL.items()):
        crit, gap, per = over.get(cid, (_SYN_CUR, _SYN_GAP, _SYN_PER)) if over else (
            _SYN_CUR, _SYN_GAP, _SYN_PER)
        rm = ["-"] * len(hdr)
        rm[A_ID], rm[A_SR], rm[A_F0] = cid, str(sr), str(f0)
        rm[A_F0_REFINED] = "%.2f" % (f0 * 1.15)
        rm[A_CRIT] = crit_raw[cid] if (crit_raw and cid in crit_raw) else f"{crit:.2f}"
        rm[A_GAP] = f"{gap:.2f}"
        rm[A_PER] = f"{per:.2e}"
        lines.append("\t".join(rm))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def _write_base(path, over=None):
    lines = ["id\tsr\tf0_target\tblsync_full_db"]
    for cid, (sr, f0) in sorted(CANONICAL.items()):
        crit = over.get(cid, _SYN_BASE) if over else _SYN_BASE
        lines.append(f"{cid}\t{sr}\t{f0}\t{crit:.2f}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def self_check(min_low, min_high, min_gap, max_per, f0_tol):
    tmp = tempfile.mkdtemp(prefix="gh19-s5-gate-selfcheck-")
    base = os.path.join(tmp, "base.tsv")
    cur = os.path.join(tmp, "cur.tsv")
    fails = []
    if HDR is None:
        return [f"self-check: the analyzer header could not be read from {ANALYZE_PY} -- the "
                f"fixtures below would be written to a guessed layout, which is exactly the "
                f"failure this check exists to prevent"]
    # The fixture layout is only as good as its agreement with the PRODUCER, so the pinned indices
    # are asserted against the analyzer's real column names before any arm runs.
    for idx, want in ((A_ID, "id"), (A_SR, "sr"), (A_F0, "f0_target"),
                      (A_F0_REFINED, "f0_refined"), (A_CRIT, "blsync_full_db"),
                      (A_GAP, "blsync_gap_db"), (A_PER, "blsync_per")):
        got = HDR[idx] if 0 <= idx < len(HDR) else "<missing>"
        if got != want:
            fails.append(f"self-check [header]: analyzer column {idx} is {got!r}, expected "
                         f"{want!r} -- the producer's column order changed; re-pin the indices")
    if fails:
        return fails

    def run(label, want_rc, want_sub):
        rc, lines = evaluate(base, cur, min_low, min_high, min_gap, max_per, f0_tol)
        txt = "\n".join(lines)
        if rc != want_rc:
            fails.append(f"self-check [{label}]: rc={rc}, expected {want_rc}")
        elif want_sub and want_sub not in txt:
            fails.append(f"self-check [{label}]: report does not contain {want_sub!r}")
        return txt

    # (1) the good pair must PASS. Without this the remaining arms could all be satisfied by a
    #     gate that simply always fails.
    _write_base(base)
    _write_cur(cur)
    run("positive", 0, "PASS: all 12")

    # (2) REMOVAL CONTROL: current == baseline (the correction is absent). Every delta is exactly
    #     zero, so every cell is RED and the report says so by name.
    _write_cur(cur, over={cid: (_SYN_BASE, _SYN_GAP, _SYN_PER) for cid in CANONICAL})
    txt = run("removal", 1, "exact-zero deltas (correction absent): YES")
    if "880:>=" not in txt:
        fails.append("self-check [removal]: the named 880 rule is not in the report")

    # (3) a single cell one notch below the 880 threshold -> RED, and ONLY that bin drops.
    _write_cur(cur, over={"vco_a_sync_tri_96000_880":
                          (_SYN_BASE - min_high + 0.5, _SYN_GAP, _SYN_PER)})
    run("one-below-880", 1, "880:>=")

    # (4) VACUOUS ANCHOR: a cell that improves BUT whose measured anchor is a plateau (gap below
    #     the requirement) must be RED. If this arm passed, requirement (2) would be decorative.
    _write_cur(cur, over={"vco_a_sync_tri_44100_220":
                          (_SYN_CUR, min_gap - 0.5, _SYN_PER)})
    run("vacuous-anchor", 1, "gap>=")

    # (5) PREMISE BROKEN: a large M-periodicity deviation must be RED even though the residual
    #     improves. If this arm passed, the method would be answering outside its premises.
    _write_cur(cur, over={"vco_a_sync_tri_44100_440":
                          (_SYN_CUR, _SYN_GAP, max_per * 10.0)})
    run("premise-broken", 1, "per<=")

    # (6) ABSENT CRITERION: the analyzer's fail-closed answer ("-") must be STRUCTURAL (2), not a
    #     criterion RED -- "could not measure" and "measured and failed" are different claims.
    _write_cur(cur, crit_raw={"vco_a_sync_tri_88200_440": "-"})
    run("absent-criterion", 2, "NO criterion")

    # (7) MISSING row -> structural; (8) DUPLICATE row -> structural.
    _write_cur(cur)
    with open(cur, encoding="utf-8") as fh:
        keep = [l for l in fh.read().split("\n")
                if not l.startswith("vco_a_sync_tri_48000_220\t")]
    with open(cur, "w", encoding="utf-8") as fh:
        fh.write("\n".join(keep))
    run("missing-row", 2, "MISSING from current")
    _write_cur(cur)
    with open(cur, encoding="utf-8") as fh:
        allc = fh.read().split("\n")
    dup = [l for l in allc if l.startswith("vco_a_sync_tri_48000_220\t")][0]
    allc.insert(allc.index(dup) + 1, dup)
    with open(cur, "w", encoding="utf-8") as fh:
        fh.write("\n".join(allc))
    run("duplicate-row", 2, "duplicate row")

    # (9) NOT-LIKE-FOR-LIKE: a renamed sync cell must be rejected, so a new id cannot be smuggled
    #     in as a replacement for a canonical one.
    _write_cur(cur)
    with open(cur, encoding="utf-8") as fh:
        allc = fh.read().replace("vco_a_sync_tri_96000_880", "vco_a_sync_tri_96000_881").split("\n")
    with open(cur, "w", encoding="utf-8") as fh:
        fh.write("\n".join(allc))
    run("renamed-cell", 2, "NOT in the canonical")

    # (10) EMPTY current (header only) -> structural, never "checked 0/12 -> PASS".
    with open(cur, "w", encoding="utf-8") as fh:
        fh.write("id\tpath\tsr\n")
    run("empty-current", 2, "MISSING from current")

    # (11) A threshold of 0 dB with a zero delta must still be a PASS on the gain rule but the
    #      sharpness requirement must still be enforced -- i.e. the arms above are not passing
    #      merely because the thresholds are large.
    _write_cur(cur, over={cid: (_SYN_BASE, _SYN_GAP, _SYN_PER) for cid in CANONICAL})
    rc0, _ = evaluate(base, cur, 0.0, 0.0, min_gap, max_per, f0_tol)
    if rc0 != 0:
        fails.append("self-check [zero-threshold]: a zero dB gain requirement did not pass on "
                     "identical values -- the gain rule is not the plain difference")
    return fails


def main():
    ap = argparse.ArgumentParser(description="GH#19 S5 hard-sync acceptance gate")
    ap.add_argument("--baseline", help="committed naive baseline TSV")
    ap.add_argument("--current", help="current (S5) analyzer TSV")
    # THRESHOLDS FOLLOW THE MEASUREMENT, they do not precede it (the S2 ruling, @Kimi 612691ea).
    # Measured on the fixed arm 2026-09-12 (12/12 cells): gain +9.90..+16.43 dB, of which the
    # 220/440 Hz cells span +9.90..+10.37 and the 880 Hz cells +12.32..+16.43; anchor gap 13.52
    # (min) .. 24.75 dB; steady-state M-periodicity deviation exactly 0.0e+00. The defaults below
    # therefore sit 0.90 / 0.82 dB under the weakest cell of each bin, matching S2's ~0.85 dB
    # margin discipline, and 3.5 dB under the weakest anchor gap. Changing a threshold requires
    # new evidence, not an edit here.
    ap.add_argument("--min-gain-db", type=float, default=9.0,
                    help="min dB gain vs the naive arm on the 220/440 Hz cells (default 9.0; "
                         "measured weakest +9.90)")
    ap.add_argument("--min-gain-high-db", type=float, default=11.5,
                    help="min dB gain on the 880 Hz cells (default 11.5; measured weakest +12.32)")
    ap.add_argument("--min-gap-db", type=float, default=10.0,
                    help="min dB cost of a +-1 sample anchor error, i.e. how sharp the measured "
                         "anchor must be (default 10.0; measured weakest 13.52)")
    ap.add_argument("--max-period-dev", type=float, default=1e-9,
                    help="max |x[i]-x[i+M]|/peak allowed for the M-periodicity premise "
                         "(default 1e-9)")
    ap.add_argument("--f0-tol", type=float, default=1.0,
                    help="Hz tolerance for the DECLARED f0_target vs the canonical target "
                         "(default 1.0; f0_refined is not comparable on these cells -- see A_F0)")
    ap.add_argument("--out", default=None, help="also write the report to this path")
    ap.add_argument("--self-check", action="store_true",
                    help="ALSO run the gate's own negative controls (with --baseline/--current "
                         "this is additive -- the run is still judged; alone it is the only "
                         "thing that runs)")
    args = ap.parse_args()

    min_high = args.min_gain_db if args.min_gain_high_db is None else args.min_gain_high_db

    if not args.baseline or not args.current:
        if not args.self_check:
            ap.error("--baseline and --current are required unless --self-check is given")
        rc, text = 0, None
    else:
        rc, lines = evaluate(args.baseline, args.current, args.min_gain_db, min_high,
                             args.min_gap_db, args.max_period_dev, args.f0_tol)
        text = "\n".join(lines)
        print(text)
        if args.out:
            with open(args.out, "w", encoding="utf-8") as fh:
                fh.write(text + "\n")

    # --self-check ADDS the gate's own controls to this invocation; it never REPLACES the
    # judgement. The pipeline driver (run_gh19_blamp_pipeline.py) passes --baseline/--current AND
    # --self-check in ONE call, so a mode that short-circuited on --self-check would print
    # "self-check PASS", exit 0, and never look at the rendered run -- a FALSE GREEN in exactly
    # the configuration CI uses. Control failures are folded into the exit code, never masked by
    # a green run.
    if args.self_check:
        fails = self_check(args.min_gain_db, min_high, args.min_gap_db,
                           args.max_period_dev, args.f0_tol)
        if text is not None:
            print("")
        for f in fails:
            print("  %s" % f)
        if fails:
            print("self-check FAILED (%d)" % len(fails))
        else:
            print("self-check PASS (positive, removal/exact-zero, one-below-threshold, vacuous "
                  "anchor, broken premise, absent criterion, missing row, duplicate row, "
                  "renamed cell, empty current, zero threshold)")
        rc = rc or (1 if fails else 0)
    return rc


if __name__ == "__main__":
    sys.exit(main())
