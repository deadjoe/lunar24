#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #109 / GH#19 S1: RED->GREEN acceptance gate for the SCHMITT oscillator slope
# correction (BLAMP). Sibling of check_gh19_blamp_acceptance.py, which covers the VCO
# triangle. Compares the current analyzer output's full-band residual (blref_full_db)
# against the committed PRE-S1 NAIVE baseline (tools/gh19_schmitt_naive_baseline.tsv).
#
# The 12 in-scope cells are PINNED IN SOURCE (CANONICAL), NEVER derived from the input
# files. A header-only, truncated, or duplicate-ridden baseline therefore fails loudly
# (every required cell reports MISSING / duplicate / non-finite) instead of passing
# vacuously with "checked 0/12".
#
# TWO BINS -- and the binning is a PHYSICAL fact, not a tuned threshold:
#
#   * fallback bin -- realized half-period M = sr/(2*f0) <= 8. The product's high-step
#     fallback gate (M <= 8: the kernel support radius L=8 has reached a half period)
#     leaves these cells uncorrected BY DESIGN. The only honest criterion there is
#     no-worsening; claiming improvement would be false.
#   * in-gate bin  -- M > 8. These must measure a real improvement.
#
# M is DERIVED FROM THE COMMITTED BASELINE's own f0_refined, not from a list of magic
# numbers -- but it is also cross-checked against a PINNED expectation, so a silent
# baseline edit cannot move a cell into the lenient fallback bin. Two structural checks
# enforce that: (1) the derived M must be an exact integer (the rail clamp forces the
# steady state to be exactly 2M-periodic, so M = sr/(2*f0) holds to the precision of the
# recorded f0), and (2) the derived bin membership must equal the pinned one. Either
# failing is structural-RED (exit 2).
#
# Criterion:
#   * all in-gate cells improve the full-band residual by >= --min-imp dB
#     (more negative = better), and
#   * all fallback-bin cells do not worsen beyond --max-worsen dB.
#
# Structural rejection (exit 2): a required cell missing from baseline or current, a
# duplicate row, a non-finite value, an sr mismatch against the canonical set, a
# non-integer derived M, a pinned/bin disagreement, or a schmitt-looking cell present in
# current but absent from the canonical set (new/renamed -> not like-for-like).
#
# Usage:
#   python3 check_gh19_schmitt_blamp_acceptance.py \
#       --baseline tools/gh19_schmitt_naive_baseline.tsv --current <analyzer.tsv>
#   python3 check_gh19_schmitt_blamp_acceptance.py ... --self-check
# Exit 0 = acceptance met; 1 = a criterion cell fails (RED); 2 = structural problem.
#
# --self-check proves this gate is not merely red-because-broken: it must go GREEN on a
# synthetic all-cells-improved input, and it must go structural-RED (2) on five specific
# corruptions. A RED verdict from a gate that can never return GREEN is worthless.
import argparse
import math
import os
import sys
import tempfile

# The canonical in-scope set: 3 required Schmitt cells x 4 sample rates = 12.
# Each entry maps a cell id to its expected sample rate. This is the SINGLE SOURCE OF
# TRUTH for coverage; the gate NEVER infers it from the baseline.
_CASES = (("drone3_schmitt", "p20"), ("drone3_schmitt", "p60"), ("drone6_schmitt", "p30"))
CANONICAL = {}
for _lane, _suffix in _CASES:
    for _sr in ("44100", "48000", "88200", "96000"):
        CANONICAL["%s_%s_%s" % (_lane, _sr, _suffix)] = int(_sr)
assert len(CANONICAL) == 12, len(CANONICAL)

# The cells the product's M <= 8 fallback gate leaves UNCORRECTED, pinned as declared
# facts (not thresholds). drone6_p30 @ 44100 is the boundary case with M == 8 exactly.
# Criterion there is no-worsening only.
FALLBACK_IDS = frozenset({"drone6_schmitt_44100_p30"})
IN_GATE_IDS = frozenset(CANONICAL) - FALLBACK_IDS
assert len(FALLBACK_IDS) == 1, len(FALLBACK_IDS)
assert len(IN_GATE_IDS) == 11, len(IN_GATE_IDS)

# Derived-M sanity band. f0_refined is recorded at 2 dp, which bounds the induced M error
# well below 1e-3 on every committed row; 0.02 keeps a 20x margin while still catching a
# cell that moved close enough to a bin boundary to change bins.
M_INT_TOL = 0.02


def parse(path, analyzer):
    """Return (rows, dups, problems). rows[id] = {sr, f0, full}.

    The analyzer emits 60 product cells but only the 12 required Schmitt cells are in
    scope; EVERY other row (vco/drone/sub/MACHINE/blank, whether `-`-placeholder or
    well-formed) is ignored here and is never a structural problem. Only a Schmitt row
    that is unparsable, duplicate, or non-finite is collected into `problems`."""
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
        if not (cid.startswith("drone3_schmitt") or cid.startswith("drone6_schmitt")):
            continue                          # out-of-scope cell -> ignore silently.
        try:
            sr = int(c[sr_i].strip())
            f0 = float(c[f0_i])
            full = float(c[full_i])
        except ValueError:
            problems.append("%s:%d: unparsable numeric row %r" % (path, ln, cid))
            continue
        if not (math.isfinite(f0) and math.isfinite(full)):
            problems.append("%s:%d: non-finite value in %s" % (path, ln, cid))
            continue
        if cid in rows:
            dups.append(cid)
            continue
        rows[cid] = {"sr": sr, "f0": f0, "full": full}
    return rows, dups, problems


def evaluate(baseline_path, current_path, min_imp, max_worsen):
    """Return (rc, lines). rc: 0 accept, 1 criterion RED, 2 structural RED."""
    base, base_dups, base_prob = parse(baseline_path, analyzer=False)
    cur, cur_dups, cur_prob = parse(current_path, analyzer=True)
    out = []

    def emit(s):
        out.append(s)

    fails = [*base_prob, *cur_prob]
    # A schmitt-looking cell that is NOT canonical = new/renamed/like-for-like-breaking.
    for k in sorted(cur):
        if k not in CANONICAL:
            fails.append("%s: schmitt cell in current is NOT in the canonical 12-set "
                         "(new/renamed -> not like-for-like)" % k)
    for k in sorted(set(base_dups) | set(cur_dups)):
        fails.append("%s: duplicate row in input" % k)

    emit("%-34s%-7s%-6s%-10s%-10s%-9s%-22s%s"
         % ("cell", "sr", "M", "naive", "blamp", "dB_imp", "rule", "verdict"))

    checked_in, checked_fb = 0, 0
    # Iterate the CANONICAL set, not the baseline file. Data rows in baseline/current
    # that are NOT canonical are either already flagged above or ignored.
    for cid in sorted(CANONICAL):
        exp_sr = CANONICAL[cid]
        if cid not in base:
            fails.append("%s: MISSING from naive baseline (a required cell has no "
                         "reference -> baseline truncated/vacuous?)" % cid)
            continue
        if cid not in cur:
            fails.append("%s: MISSING from current analyzer output" % cid)
            continue
        b, c = base[cid], cur[cid]
        if b["sr"] != exp_sr or c["sr"] != exp_sr:
            fails.append("%s: sr mismatch (canonical %d, baseline %d, current %d)"
                         % (cid, exp_sr, b["sr"], c["sr"]))
            continue
        if not (b["f0"] > 0.0):
            fails.append("%s: non-positive baseline f0_refined -> M underivable" % cid)
            continue

        # Bin membership is derived from the baseline's own recorded realized frequency,
        # then cross-checked against the pinned set. M = sr / (2*f0) is the realized
        # half-period in samples; the clamp makes it an exact integer in steady state.
        m_real = exp_sr / (2.0 * b["f0"])
        m = int(round(m_real))
        if abs(m_real - m) > M_INT_TOL:
            fails.append("%s: derived half-period M=%.5f is not an integer "
                         "(tolerance %.2f) -> baseline f0_refined no longer describes an "
                         "exactly 2M-periodic steady state" % (cid, m_real, M_INT_TOL))
            continue
        if (m <= 8) != (cid in FALLBACK_IDS):
            fails.append("%s: derived bin (M=%d) disagrees with the pinned bin (%s) -> "
                         "a cell moved across the fallback boundary"
                         % (cid, m, "fallback" if cid in FALLBACK_IDS else "in-gate"))
            continue

        nb, cb = b["full"], c["full"]
        imp = nb - cb                            # positive = more band-limited (better)
        if cid in FALLBACK_IDS:
            rule = "fallback:!worse>%gdB" % max_worsen
            ok = imp >= -max_worsen
            if ok:
                checked_fb += 1
        else:
            rule = "improve:>=%gdB" % min_imp
            ok = imp >= min_imp
            if ok:
                checked_in += 1
        verdict = "GREEN" if ok else "RED"
        if not ok:
            fails.append("%s: %s -> imp=%.2fdB" % (cid, rule, imp))
        emit("%-34s%-7d%-6d%-10.2f%-10.2f%-9.2f%-22s%s"
             % (cid, c["sr"], m, nb, cb, imp, rule, verdict))

    emit("")
    emit("Bins: in-gate(M>8) passed=%d/%d, fallback(M<=8) passed=%d/%d"
         % (checked_in, len(IN_GATE_IDS), checked_fb, len(FALLBACK_IDS)))
    if fails:
        emit("")
        emit("FAIL:")
        for f in fails:
            emit("  - %s" % f)
        structural = any("MISSING" in f or "duplicate" in f or "mismatch" in f
                         or "non-finite" in f or "NOT in the canonical" in f
                         or "unparsable" in f or "vacuous" in f or "disagrees" in f
                         or "not an integer" in f or "underivable" in f for f in fails)
        return (2 if structural else 1), out
    emit("")
    emit("PASS: all %d in-gate cells improve >= %g dB and the %d fallback cell(s) "
         "do not worsen beyond %g dB."
         % (len(IN_GATE_IDS), min_imp, len(FALLBACK_IDS), max_worsen))
    return 0, out


def _rewrite(path, fn):
    """Apply fn(lines) -> lines to a copy of `path`; return the temp path."""
    with open(path, encoding="utf-8") as fh:
        raw = [l.rstrip("\n") for l in fh if l.strip()]
    fh = tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False, encoding="utf-8")
    fh.write("\n".join(fn(raw)) + "\n")
    fh.close()
    return fh.name


def _drop_row(cid):
    return lambda raw: [l for l in raw if not l.startswith(cid + "\t")]


def _dup_row(cid):
    def fn(raw):
        out = []
        for l in raw:
            out.append(l)
            if l.startswith(cid + "\t"):
                out.append(l)
        return out
    return fn


def _set_col(cid, col, value):
    def fn(raw):
        out = []
        for l in raw:
            c = l.split("\t")
            if c[0] == cid and len(c) > col:
                c[col] = value
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def _rename(cid, new):
    return _set_col(cid, 0, new)


def _shift_current(delta, ids):
    """Reduce the listed cells' blref_full_db by `delta` (more negative = better)."""
    def fn(raw):
        out = []
        for l in raw:
            c = l.split("\t")
            if len(c) >= 9 and c[0] in ids:
                c[8] = "%.2f" % (float(c[8]) - delta)
                out.append("\t".join(c))
            else:
                out.append(l)
        return out
    return fn


def self_check(baseline, current, min_imp, max_worsen):
    """Prove the gate can go GREEN, and that five corruptions go structural-RED."""
    cases = [
        # (label, expected rc, mutated baseline, mutated current)
        ("NC-1 in-gate cells improved by %g dB, fallback cell left as-is -> must PASS"
         % min_imp,
         0, None, _shift_current(min_imp, IN_GATE_IDS)),
        ("NC-2 a required baseline row dropped -> structural",
         2, _drop_row("drone3_schmitt_88200_p60"), None),
        ("NC-3 a duplicate baseline row -> structural",
         2, _dup_row("drone3_schmitt_44100_p20"), None),
        ("NC-4 fallback cell's f0 edited so it derives into the in-gate bin -> structural",
         2, _set_col("drone6_schmitt_44100_p30", 2, "1800.0"), None),
        ("NC-5 f0 edited so the derived M is no longer an integer -> structural",
         2, _set_col("drone3_schmitt_44100_p20", 2, "1234.5"), None),
        ("NC-6 a current cell renamed out of the canonical set -> structural",
         2, None, _rename("drone6_schmitt_88200_p30", "drone6_schmitt_88200_p31")),
        # Proves the fallback no-worsening rule is not vacuous: it can reject.
        ("NC-7 fallback cell worsened beyond %g dB -> criterion RED (not structural)"
         % max_worsen,
         1, None, _shift_current(-(max_worsen + 1.0), FALLBACK_IDS)),
    ]
    print("self-check: (1) must PASS, (2)-(6) structural-RED, (7) criterion-RED")
    bad, tmps = 0, []
    try:
        for label, want, mutb, mutc in cases:
            bp = baseline if mutb is None else _rewrite(baseline, mutb)
            cp = current if mutc is None else _rewrite(current, mutc)
            if mutb is not None:
                tmps.append(bp)
            if mutc is not None:
                tmps.append(cp)
            rc, _ = evaluate(bp, cp, min_imp, max_worsen)
            ok = (rc == want)
            bad += 0 if ok else 1
            print("    %-5s rc=%d (want %d)  %s"
                  % ("OK  " if ok else "WRONG", rc, want, label))
    finally:
        for p in tmps:
            os.unlink(p)
    print("self-check: %s" % ("PASS" if not bad else "FAIL (%d case(s) wrong)" % bad))
    return (1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description="GH#19 S1 Schmitt BLAMP acceptance gate")
    ap.add_argument("--baseline", required=True, help="committed pre-S1 naive baseline TSV")
    ap.add_argument("--current", required=True, help="current (BLAMP) analyzer TSV")
    ap.add_argument("--min-imp", type=float, default=6.0,
                    help="min dB improvement for in-gate cells (default 6.0)")
    ap.add_argument("--max-worsen", type=float, default=0.5,
                    help="max dB worsening allowed in the fallback bin (default 0.5)")
    ap.add_argument("--out", default=None, help="also write the report to this path")
    ap.add_argument("--self-check", action="store_true",
                    help="prove green-ability + five corruptions go structural-RED")
    args = ap.parse_args()

    print("GH#19 S1 Schmitt BLAMP acceptance  "
          "(baseline=pre-S1 naive Schmitt, current=BLAMP Schmitt)\n")
    rc, lines = evaluate(args.baseline, args.current, args.min_imp, args.max_worsen)
    for s in lines:
        print(s)

    if args.self_check:
        print("")
        rc = self_check(args.baseline, args.current, args.min_imp, args.max_worsen) or rc

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
