#!/usr/bin/env python3
"""ZERO-PERTURBATION AUDIT for a change to tools/gh19_alias_analyze.py.

Runs two analyzer reports over the SAME rendered probe directory (before = the unmodified analyzer,
after = the modified one) and classifies EVERY cell into exactly one of three classes:

  (a) IDENTICAL       -- the row is byte-for-byte the same string.
  (b) NEWLY-POPULATED -- the row changed ONLY by acquiring a value in a previously `-` metric column,
                         every other column unchanged. This is the intended deliverable.
  (c) CHANGED         -- the row differs in any other way (a previously-non-`-` value moved, a label
                         moved, a cell appeared/disappeared, the row order changed).

(c) MUST be zero: an analyzer change that moves an existing measurement invalidates every figure
already recorded against it. (b) is additionally constrained to the ids the change is scoped to.
Exit 0 only when (c)==0 and every (b) id is inside the declared scope.

Non-cell lines (the analyzer interleaves MACHINE/cpu lines whose timing values differ on every run)
are compared by KEY SET, not by value -- so a cell that silently stopped being reported still fails.

--self-check proves the audit can actually fail, by mutating the `after` report five ways and
requiring a red verdict from each. A zero-perturbation claim from a gate that cannot go red is
worthless.

Usage:
  check_gh19_analyzer_perturbation.py --before B.tsv --after A.tsv [--scope-prefix P]... [--out F]
  check_gh19_analyzer_perturbation.py --before B.tsv --after A.tsv --self-check
"""
import argparse
import os
import sys
import tempfile

DEFAULT_SCOPE = ("drone3_schmitt", "drone6_schmitt")


def load(path):
    """Return (header, cell_rows, other_keys)."""
    with open(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        raise SystemExit("FATAL: %s is empty" % path)
    header = lines[0].split("\t")
    rows = []
    other = []
    for line in lines[1:]:
        cols = line.split("\t")
        if len(cols) != len(header):
            other.append("\t".join(cols[:2]))
            continue
        rows.append((cols[0], cols, line))
    return header, rows, other


def classify(before, after, scope):
    """Return (violations:list[str], lines:list[str]) for one before/after pair of report files."""
    lines, viol = [], []

    def emit(s):
        lines.append(s)

    try:
        hb, rb, ob_other = load(before)
        ha, ra, oa_other = load(after)
    except SystemExit as e:
        return [str(e)], [str(e)]

    if hb != ha:
        viol.append("metric header differs")
        emit("FATAL: metric header differs\n  before: %s\n  after:  %s" % ("\t".join(hb), "\t".join(ha)))
        return viol, lines
    if sorted(ob_other) != sorted(oa_other):
        viol.append("non-cell line key set differs")
        emit("FATAL: non-cell line key set differs\n  before: %s\n  after:  %s"
             % (sorted(ob_other), sorted(oa_other)))
        return viol, lines

    emit("metric columns: %s" % "\t".join(hb))
    emit("non-cell lines compared by key set only (timing values vary per run): %d keys"
         % len(ob_other))
    emit("allowed new-population scope: %s" % ", ".join(scope))

    ids_b = [i for i, _, _ in rb]
    ids_a = [i for i, _, _ in ra]
    if ids_b != ids_a:
        viol.append("cell id set/order differs")
        emit("FATAL: cell id set/order differs\n  before-only=%s\n  after-only=%s"
             % ([i for i in ids_b if i not in ids_a], [i for i in ids_a if i not in ids_b]))
        return viol, lines

    oa = {i: c for i, c, _ in ra}
    identical, newly, changed = [], [], []
    for cid, cb, lb in rb:
        ca = oa[cid]
        if lb == "\t".join(ca):
            identical.append(cid)
            continue
        newly_cols, illegal = [], []
        for name, vb, va in zip(hb, cb, ca):
            if vb == va:
                continue
            if vb == "-" and va != "-":
                newly_cols.append((name, va))
            else:
                illegal.append((name, vb, va))
        # a row is NEWLY-POPULATED only if the SOLE difference is `-` -> value in some column and every
        # column that was non-`-` before still carries the SAME string.
        if illegal or not newly_cols:
            changed.append((cid, illegal or [("(no column newly populated)", "", "")]))
        else:
            newly.append((cid, newly_cols))

    emit("")
    emit("(a) identical        : %d" % len(identical))
    emit("(b) newly-populated  : %d" % len(newly))
    emit("(c) changed          : %d   <-- must be 0" % len(changed))
    for cid, cols in newly:
        emit("    + %-34s %s" % (cid, "  ".join("%s=%s" % (n, v) for n, v in cols)))
    for cid, cols in changed:
        viol.append("value-changed %s" % cid)
        emit("    ! %-34s %s" % (cid, "  ".join("%s: %r->%r" % (n, b, a) for n, b, a in cols)))
    out_of_scope = [cid for cid, _ in newly if not cid.startswith(scope)]
    if out_of_scope:
        viol.append("newly-populated outside scope: %s" % ", ".join(out_of_scope))
        emit("    ! newly-populated OUTSIDE declared scope: %s" % ", ".join(out_of_scope))

    emit("")
    emit("OVERALL: %s  (%d violation(s); %d cell(s) newly populated, all in scope)"
         % ("FAIL" if viol else "PASS", len(viol), len(newly) if not out_of_scope else
            len(newly) - len(out_of_scope)))
    return viol, lines


def self_check(before, after, scope):
    """Mutate the `after` report five ways; each mutation MUST produce a red verdict."""
    hb, rb, ob_other = load(before)
    hdr = hb

    def with_row(idx, col, value, drop=False, rename=None):
        """Rewrite the `after` report with one cell row mutated."""
        with open(after) as fh:
            raw = [l.rstrip("\n") for l in fh if l.strip()]
        outr = []
        seen = 0
        for line in raw:
            cols = line.split("\t")
            if len(cols) == len(hdr):
                cid = cols[0]
                if seen == idx:
                    seen += 1
                    if drop:
                        continue
                    if rename:
                        cols[0] = rename
                    if col is not None:
                        cols[col] = value
                    outr.append("\t".join(cols))
                    continue
                seen += 1
            outr.append(line)
        fh = tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False)
        fh.write("\n".join(outr) + "\n")
        fh.close()
        return fh.name

    def with_header(col, value):
        with open(after) as fh:
            raw = [l.rstrip("\n") for l in fh if l.strip()]
        cols = raw[0].split("\t")
        cols[col] = value
        raw[0] = "\t".join(cols)
        fh = tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False)
        fh.write("\n".join(raw) + "\n")
        fh.close()
        return fh.name

    # index of a cell that is NOT in scope and already has all-`-` blref columns, and one that is
    idx_other, idx_in, idx_vco = None, None, None
    for i, (cid, cols, _) in enumerate(rb):
        if cid.startswith("vco_a_tri") and idx_vco is None:
            idx_vco = i
        if cid.startswith(scope) and idx_in is None:
            idx_in = i
        if (not cid.startswith(scope)) and cols[8] == "-" and idx_other is None:
            idx_other = i
    if None in (idx_other, idx_in, idx_vco):
        return 1, ["SELF-CHECK FATAL: could not locate mutation anchors"]

    cases = [
        ("NC-1 value changed on an existing VCO cell",
         with_row(idx_vco, 8, "-99.99")),
        ("NC-2 value changed in a non-newly-populated column of an in-scope cell",
         with_row(idx_in, 5, "-99.99")),
        ("NC-3 newly populated OUTSIDE the declared scope",
         with_row(idx_other, 8, "-99.99")),
        ("NC-4 a cell row dropped",
         with_row(idx_in, None, None, drop=True)),
        ("NC-5 metric header renamed",
         with_header(8, "blref_full_dB")),
    ]
    results, bad = [], 0
    for label, path in cases:
        v, _ = classify(before, path, scope)
        os.unlink(path)
        ok = len(v) > 0
        bad += 0 if ok else 1
        results.append("    %s %s" % ("RED  " if ok else "GREEN", label))
    print("self-check: each mutation must produce a RED verdict")
    for r in results:
        print(r)
    print("self-check: %s" % ("PASS" if not bad else "FAIL (%d mutation(s) not caught)" % bad))
    return (1 if bad else 0), results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--scope-prefix", action="append", default=None,
                    help="id prefix the change is ALLOWED to newly populate (repeatable)")
    ap.add_argument("--out", default=None, help="also write the report to this path")
    ap.add_argument("--self-check", action="store_true",
                    help="mutate the after-report five ways; each MUST be caught")
    args = ap.parse_args()

    scope = tuple(args.scope_prefix) if args.scope_prefix else DEFAULT_SCOPE
    viol, lines = classify(args.before, args.after, scope)
    for s in lines:
        print(s)
    rc = 1 if viol else 0

    if args.self_check:
        print("")
        src, _ = self_check(args.before, args.after, scope)
        rc = rc or src

    if args.out:
        with open(args.out, "w") as fh:
            fh.write("\n".join(lines) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
