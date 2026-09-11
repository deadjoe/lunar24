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
# THREE BANDS. M = sr/(2*f0) is the REALIZED half-period in samples: the rail clamp
# discards the overshoot, so the steady state is exactly 2M-periodic and M is an integer.
# Band membership is therefore a PHYSICAL property of each cell, not a tuned threshold.
#
#   * fallback, M <= 8 -- the 8-tap kernel support has reached a half period, so the
#     product returns before touching the LUT and the cell is UNCORRECTED BY DESIGN. The
#     only honest criterion there is no-worsening; claiming improvement would be false.
#     This band additionally carries a BYTE-IDENTITY assertion (below).
#   * kernel-dense, 9 <= M <= 11 -- the correction is ON and does real work, but these are
#     the two cells whose half-period barely exceeds the kernel support (8*step is 0.444
#     of a half period at M=9 and 0.364 at M=11), so the corner overshoot that can be
#     recovered is nearly exhausted. Measured at the slice tip: +2.27 dB (M=9) and
#     +5.40 dB (M=11). Criterion: gain > 0 AND no worsening.
#   * full, M >= 12 -- criterion: improve by >= --min-imp dB (default 6). The WEAKEST
#     member of this band measures +8.41 dB, so the floor has 2.4 dB of margin over the
#     worst cell actually observed in it.
#
# THE UPPER BAND EDGE (M > 11 -> `full`) IS PROVISIONAL AND EMPIRICAL, NOT DERIVED, and it
# is recorded as such. What IS derived is the physical motivation above and the fact that
# the two cells falling short of 6 dB are exactly the two smallest-M cells above the
# fallback edge. What is NOT derived is why 11 rather than 10 or 12: the probe matrix has
# no M=10 and no M=12 cell, so the edge sits where the measured data changes regime with
# no cell available to test it against.
#
# NO-DOWNGRADE CLAUSE: a future slice that adds cells at M=10 or M=12 puts them in the
# band their own M selects; M=12 must NOT be moved into `kernel-dense` to keep it green.
# Anything at or above M=12 is held to --min-imp.
#
# M is DERIVED FROM THE COMMITTED BASELINE's own f0_refined, not from a list of magic
# numbers -- but it is also cross-checked against a PINNED expectation, so a silent
# baseline edit cannot move a cell into a more lenient band. Two structural checks enforce
# that: (1) the derived M must be an exact integer (see above), and (2) the derived band
# must equal the pinned one. Either failing is structural-RED (exit 2).
#
# FALLBACK BYTE IDENTITY. The fallback band's no-worsening rule is ONE-SIDED: removing the
# product's fallback gate only makes the M<=8 cell BETTER, so that rule alone cannot
# discriminate the failure it exists to catch. The gate therefore also asserts that the
# fallback cell's rendered output is byte-identical to the UNCORRECTED product's render of
# the same cell (NAIVE_FALLBACK_SHA256), which pins the "fallback = no correction"
# semantics exactly and makes gate removal RED.
#
# Structural rejection (exit 2): a required cell missing from baseline or current, a
# duplicate row, a non-finite value, an sr mismatch against the canonical set, a
# non-integer derived M, a pinned/derived BAND disagreement, a schmitt-looking cell
# present in current but absent from the canonical set (new/renamed -> not like-for-like),
# or the fallback cell's raw render being absent/unlocatable. A fallback render that is
# present but byte-DIFFERENT is criterion-RED (1), not structural: the inputs are exactly
# what they claim to be, and it is the product's output that changed.
#
# Usage:
#   python3 check_gh19_schmitt_blamp_acceptance.py \
#       --baseline tools/gh19_schmitt_naive_baseline.tsv --current <analyzer.tsv> \
#       --current-raws <probe-output-dir>
#   python3 check_gh19_schmitt_blamp_acceptance.py ... --self-check
# Exit 0 = acceptance met; 1 = a criterion cell fails (RED); 2 = structural problem.
#
# --current-raws is REQUIRED, not optional. The byte-identity assertion needs the probe's
# rendered doubles, which no TSV carries. A gate that could be silently downgraded to
# "did not check the bytes" by omitting a flag is the hole this assertion exists to close,
# so omitting it is an argparse error rather than a skip.
#
# --self-check proves this gate is not merely red-because-broken: it must go GREEN on a
# synthetic all-cells-improved input, and it must go RED (1 or 2) on nine specific
# corruptions -- including two that exercise the byte-identity assertion itself. A RED
# verdict from a gate that can never return GREEN is worthless.
import argparse
import hashlib
import math
import os
import struct
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

# Band edges, as declared facts (not thresholds). M_FALLBACK_MAX is the product's own
# fallback predicate (blampSupportReachesHalfPeriod) expressed on M. M_DENSE_MAX is the
# PROVISIONAL empirical edge explained in the header.
M_FALLBACK_MAX = 8
M_DENSE_MAX = 11

# The cells the product's M <= 8 fallback gate leaves UNCORRECTED. drone6_p30 @ 44100 is
# the boundary case with M == 8 exactly. Criterion there is no-worsening PLUS byte
# identity with the uncorrected render.
FALLBACK_IDS = frozenset({"drone6_schmitt_44100_p30"})                  # M == 8

# The kernel-dense band: correction is on, half-period barely exceeds kernel support.
DENSE_IDS = frozenset({"drone3_schmitt_44100_p60",                       # M == 11
                       "drone6_schmitt_48000_p30"})                      # M == 9

FULL_IDS = frozenset(CANONICAL) - FALLBACK_IDS - DENSE_IDS
assert len(FALLBACK_IDS) == 1, len(FALLBACK_IDS)
assert len(DENSE_IDS) == 2, len(DENSE_IDS)
assert len(FULL_IDS) == 9, len(FULL_IDS)


def band_of(m):
    """Band name for a realized half-period M. Single source of truth for membership."""
    if m <= M_FALLBACK_MAX:
        return "fallback"
    return "dense" if m <= M_DENSE_MAX else "full"


def pinned_band(cid):
    if cid in FALLBACK_IDS:
        return "fallback"
    return "dense" if cid in DENSE_IDS else "full"

# --- the fallback byte-identity fixture ----------------------------------------------
# SHA-256 of the 24576-double render of drone6_schmitt_44100_p30 (M == 8) by the
# UNCORRECTED product. Provenance: taken from the pre-S1 source tree, which contains no
# slope correction at all, and measured byte-identical to the same cell's render from the
# corrected tree at this slice's tip, from the pre-fix analytic-step tree, and from all
# three local build configurations (Release -O3 / Debug -O0 / ASan+UBSan Debug) -- seven
# independent renders, one digest.
#
# SCOPE OF THE CLAIM, stated exactly: this is a GOLDEN LOCK on the rendered bytes of the
# fallback cell. It asserts that the M<=8 cell passes through uncorrected and that its
# output has not drifted. It was verified on Apple clang only; the ubuntu (g++, clang++)
# and windows (cl) CI legs were NOT checked before committing it. The general risk is
# FMA contraction diverging between compilers. For THIS cell that risk is argued to be
# nil rather than merely unmeasured: the whole render path is exactly-rounded here
# (r = rate/sampleRate = 1/8 exactly, 1/r, /8 and *8 all exact; the fm term is multiplied
# by mod_ = 0 so every contraction candidate has a zero addend; and IEEE division and
# sqrt-free arithmetic are deterministic). If a CI leg ever goes RED on this line alone,
# the honest reading is "the fixture is not portable", not "the DSP changed" -- the
# accompanying shape diagnostics in _shaped() tell the two apart.
NAIVE_FALLBACK_SHA256 = \
    "d6a9f5b19462ed6e799da5db8f1ec74690c0dd494338262e1112d7d38fbc052b"

# gh19_scenarios.tsv column indices: 0=id ... 9=raw filename. Read from the probe's OWN
# manifest so a renumbering of gh19_scnNNN.raw cannot silently retarget this check.
SCEN_RAW_COL = 9

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


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _shaped(path):
    """(bytes, n_samples, n_nonfinite, peak) -- shape diagnostics for a byte mismatch.

    No reference bytes are committed (a 192 KB f64 fixture is not worth the review
    surface), so a mismatch cannot be localised to a sample index here. What these four
    numbers CAN separate is the two ways this assertion fails in practice: a uniform
    numeric shift (same length, same finiteness, peak moved) versus a truncated or
    malformed file (length or finiteness changed)."""
    with open(path, "rb") as fh:
        b = fh.read()
    n = len(b) // 8
    nf, peak = 0, 0.0
    for i in range(n):
        v = struct.unpack_from("<d", b, i * 8)[0]
        if not math.isfinite(v):
            nf += 1
        else:
            peak = max(peak, abs(v))
    return len(b), n, nf, peak


def fallback_raw_path(raws_dir):
    """Absolute path of the fallback cell's raw render, or (None, problem line).

    The filename is read from the probe's OWN gh19_scenarios.tsv rather than pinned as an
    index, so a renumbering of gh19_scnNNN.raw cannot silently retarget this assertion at
    a different cell."""
    fb_id = sorted(FALLBACK_IDS)[0]
    scen = os.path.join(raws_dir, "gh19_scenarios.tsv")
    if not os.path.isfile(scen):
        return None, ("fallback byte-identity: MISSING %s -> the fallback cell's raw "
                      "render cannot be located" % scen)
    name = None
    for line in open(scen, encoding="utf-8"):
        c = line.rstrip("\n").split("\t")
        if len(c) > SCEN_RAW_COL and c[0].strip() == fb_id:
            name = c[SCEN_RAW_COL].strip()
            break
    if not name or name == "-":
        return None, ("fallback byte-identity: MISSING raw column for %s in %s"
                      % (fb_id, scen))
    raw = os.path.join(raws_dir, name)
    if not os.path.isfile(raw):
        return None, ("fallback byte-identity: MISSING %s -> the probe did not render "
                      "the fallback cell" % raw)
    return raw, None


def check_fallback_bytes(raws_dir):
    """Byte identity of the fallback cell's render vs the UNCORRECTED product.

    Return (verdict, lines): verdict None = OK, 1 = criterion RED, 2 = structural RED."""
    raw, problem = fallback_raw_path(raws_dir)
    if problem is not None:
        return 2, [problem]
    got = _sha256(raw)
    name = os.path.basename(raw)
    if got == NAIVE_FALLBACK_SHA256:
        return None, ["fallback byte-identity: %s == the uncorrected render (sha256 %s...)"
                      % (name, got[:16])]
    size, n, nf, peak = _shaped(raw)
    return 1, [
        "fallback byte-identity: %s DIFFERS from the uncorrected render" % name,
        "    got    sha256 %s" % got,
        "    pinned sha256 %s" % NAIVE_FALLBACK_SHA256,
        "    shape  %d bytes / %d samples / %d non-finite / peak %.6f"
        % (size, n, nf, peak),
    ]


def evaluate(baseline_path, current_path, min_imp, max_worsen, raws_dir):
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

    emit("%-34s%-7s%-6s%-10s%-10s%-9s%-27s%s"
         % ("cell", "sr", "M", "naive", "blamp", "dB_imp", "rule", "verdict"))

    checked_full, checked_dense, checked_fb = 0, 0, 0
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
        band = band_of(m)
        if band != pinned_band(cid):
            fails.append("%s: derived band (M=%d -> %s) disagrees with the pinned band "
                         "(%s) -> a cell moved across a band boundary"
                         % (cid, m, band, pinned_band(cid)))
            continue

        nb, cb = b["full"], c["full"]
        imp = nb - cb                            # positive = more band-limited (better)
        if band == "fallback":
            rule = "fallback:!worse>%gdB" % max_worsen
            ok = imp >= -max_worsen
            if ok:
                checked_fb += 1
        elif band == "dense":
            # Gain must be strictly positive AND must not worsen. The second clause is
            # subsumed by the first at any sane max_worsen; it is written out because the
            # band's contract is "bounded improvement", not "any positive number".
            rule = "dense:gain>0&!worse>%gdB" % max_worsen
            ok = (imp > 0.0) and (imp >= -max_worsen)
            if ok:
                checked_dense += 1
        else:
            rule = "improve:>=%gdB" % min_imp
            ok = imp >= min_imp
            if ok:
                checked_full += 1
        verdict = "GREEN" if ok else "RED"
        if not ok:
            fails.append("%s: %s -> imp=%.2fdB" % (cid, rule, imp))
        emit("%-34s%-7d%-6d%-10.2f%-10.2f%-9.2f%-27s%s"
             % (cid, c["sr"], m, nb, cb, imp, rule, verdict))

    # The fallback byte-identity assertion. Independent of the dB table above: it is what
    # makes the one-sided fallback rule discriminating.
    b_verdict, b_lines = check_fallback_bytes(raws_dir)
    emit("")
    for s in b_lines:
        emit(s)
    if b_verdict is not None:
        fb_id = sorted(FALLBACK_IDS)[0]
        if b_verdict == 2:
            fails.append("%s: fallback byte-identity cannot be checked -> %s"
                         % (fb_id, b_lines[0]))  # carries MISSING -> structural
        else:
            fails.append("%s: fallback byte-identity vs the uncorrected render FAILED "
                         "(the M<=%d cell is no longer passing through uncorrected)"
                         % (fb_id, M_FALLBACK_MAX))

    emit("")
    emit("Bins: full(M>=%d) passed=%d/%d, dense(%d<=M<=%d) passed=%d/%d, "
         "fallback(M<=%d) passed=%d/%d"
         % (M_DENSE_MAX + 1, checked_full, len(FULL_IDS),
            M_FALLBACK_MAX + 1, M_DENSE_MAX, checked_dense, len(DENSE_IDS),
            M_FALLBACK_MAX, checked_fb, len(FALLBACK_IDS)))
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
    emit("PASS: %d full-band cell(s) improve >= %g dB, %d kernel-dense cell(s) gain "
         "without worsening, and the %d fallback cell(s) do not worsen beyond %g dB and "
         "are byte-identical to the uncorrected render."
         % (len(FULL_IDS), min_imp, len(DENSE_IDS), len(FALLBACK_IDS), max_worsen))
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


def _raws_symlink_copy(raws_dir, drop=None):
    """A temp dir of symlinks to the probe output, minus `drop` (a basename)."""
    d = tempfile.mkdtemp(prefix="gh19raws-")
    for fn in os.listdir(raws_dir):
        if fn != drop:
            os.symlink(os.path.join(raws_dir, fn), os.path.join(d, fn))
    return d


def _raws_broken_fallback(raws_dir):
    """The fallback cell's render with ONE sample perturbed by 1 ULP."""
    raw, problem = fallback_raw_path(raws_dir)
    if problem is not None:
        raise SystemExit("self-check needs a usable --current-raws: %s" % problem)
    d = _raws_symlink_copy(raws_dir, drop=os.path.basename(raw))
    with open(raw, "rb") as fh:
        blob = bytearray(fh.read())
    blob[0] ^= 0x01
    with open(os.path.join(d, os.path.basename(raw)), "wb") as fh:
        fh.write(bytes(blob))
    return d


def _raws_no_fallback(raws_dir):
    """The fallback cell's render deleted from the probe output."""
    raw, problem = fallback_raw_path(raws_dir)
    if problem is not None:
        raise SystemExit("self-check needs a usable --current-raws: %s" % problem)
    return _raws_symlink_copy(raws_dir, drop=os.path.basename(raw))


def self_check(baseline, current, min_imp, max_worsen, raws_dir):
    """Prove the gate can go GREEN, and that the corruptions below go RED.

    Cases 1-7 exercise the dB criterion and the structural checks; cases 8-9 exercise the
    fallback BYTE-IDENTITY assertion specifically -- without them nothing would prove that
    assertion can reject, which is the entire reason it exists (the fallback dB rule is
    one-sided and cannot)."""
    cases = [
        # (label, expected rc, mutated baseline, mutated current, mutated raws)
        ("NC-1 full-band cells improved by %g dB, everything else left as-is -> must PASS"
         % min_imp,
         0, None, _shift_current(min_imp, FULL_IDS), None),
        ("NC-2 a required baseline row dropped -> structural",
         2, _drop_row("drone3_schmitt_88200_p60"), None, None),
        ("NC-3 a duplicate baseline row -> structural",
         2, _dup_row("drone3_schmitt_44100_p20"), None, None),
        ("NC-4 fallback cell's f0 edited so it derives into the dense band -> structural",
         2, _set_col("drone6_schmitt_44100_p30", 2, "1800.0"), None, None),
        ("NC-5 f0 edited so the derived M is no longer an integer -> structural",
         2, _set_col("drone3_schmitt_44100_p20", 2, "1234.5"), None, None),
        ("NC-6 a current cell renamed out of the canonical set -> structural",
         2, None, _rename("drone6_schmitt_88200_p30", "drone6_schmitt_88200_p31"), None),
        # Proves the fallback no-worsening rule is not vacuous: it can reject.
        ("NC-7 fallback cell worsened beyond %g dB -> criterion RED (not structural)"
         % max_worsen,
         1, None, _shift_current(-(max_worsen + 1.0), FALLBACK_IDS), None),
        # Proves the BYTE-IDENTITY assertion is not vacuous: it can reject.
        ("NC-8 fallback cell's rendered bytes perturbed by 1 ULP -> criterion RED",
         1, None, None, _raws_broken_fallback),
        ("NC-9 fallback cell's raw render removed from the probe output -> structural",
         2, None, None, _raws_no_fallback),
    ]
    print("self-check: (1) must PASS; (2)-(6),(9) structural-RED; (7),(8) criterion-RED")
    bad, tmps, rdirs = 0, [], []
    try:
        for label, want, mutb, mutc, mutr in cases:
            bp = baseline if mutb is None else _rewrite(baseline, mutb)
            cp = current if mutc is None else _rewrite(current, mutc)
            rd = raws_dir if mutr is None else mutr(raws_dir)
            if mutb is not None:
                tmps.append(bp)
            if mutc is not None:
                tmps.append(cp)
            if mutr is not None:
                rdirs.append(rd)
            rc, _ = evaluate(bp, cp, min_imp, max_worsen, rd)
            ok = (rc == want)
            bad += 0 if ok else 1
            print("    %-5s rc=%d (want %d)  %s"
                  % ("OK  " if ok else "WRONG", rc, want, label))
    finally:
        for p in tmps:
            os.unlink(p)
        for d in rdirs:
            import shutil
            shutil.rmtree(d, ignore_errors=True)
    print("self-check: %s" % ("PASS" if not bad else "FAIL (%d case(s) wrong)" % bad))
    return (1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description="GH#19 S1 Schmitt BLAMP acceptance gate")
    ap.add_argument("--baseline", required=True, help="committed pre-S1 naive baseline TSV")
    ap.add_argument("--current", required=True, help="current (BLAMP) analyzer TSV")
    ap.add_argument("--current-raws", required=True,
                    help="the probe's output dir (carries the raw renders and "
                         "gh19_scenarios.tsv) -- REQUIRED; the fallback byte-identity "
                         "assertion reads it and a gate that can be silently downgraded "
                         "to 'did not check the bytes' is the hole it exists to close")
    ap.add_argument("--min-imp", type=float, default=6.0,
                    help="min dB improvement for full-band (M>=12) cells (default 6.0)")
    ap.add_argument("--max-worsen", type=float, default=0.5,
                    help="max dB worsening allowed outside the full band (default 0.5)")
    ap.add_argument("--out", default=None, help="also write the report to this path")
    ap.add_argument("--self-check", action="store_true",
                    help="prove green-ability + nine corruptions go RED")
    args = ap.parse_args()

    print("GH#19 S1 Schmitt BLAMP acceptance  "
          "(baseline=pre-S1 naive Schmitt, current=BLAMP Schmitt)\n")
    rc, lines = evaluate(args.baseline, args.current, args.min_imp, args.max_worsen,
                         args.current_raws)
    for s in lines:
        print(s)

    if args.self_check:
        print("")
        rc = self_check(args.baseline, args.current, args.min_imp, args.max_worsen,
                        args.current_raws) or rc

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
