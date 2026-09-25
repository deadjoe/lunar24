#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh19_s3_pulse_delta.py — task #118 (GH #19 S3): the baseline/candidate DELTA matrix.
#
# WHY THIS IS ITS OWN TOOL AND NOT PART OF THE ANALYZER. The analyzer answers "what is this arm's
# residual"; this answers "what changed between two arms". Keeping them apart is what lets the
# negative controls compare two MUTATED arms with the same reference construction the analyzer uses
# for the candidate, instead of re-deriving what a residual means in each script.
#
# THE COLUMNS ARE THE CLAIM'S OWN GUARD RAILS. dres_db is the change in 总诊断残差 (negative = better).
# dfloor_db is the change in the non-harmonic share specifically, which is the column an anti-aliasing
# change has to move; a change that only reshuffles energy between the harmonic and floor shares would
# show as dres_db ~ 0 with dfloor_db and dharm_db of opposite sign. dA_db / dfund_db / ddc and the
# duty column are there to make the three ways an "improvement" can be faked visible: a level change
# (A), a fundamental-amplitude change, a DC shift, or a duty/frequency change. They are printed for
# every cell, not only for the ones where they move.
#
# WHAT THIS PARSER REFUSES TO DO (@Codex e144b61 review, item 3). An earlier revision built each row
# with `v = [float(x) for x in f[1:]]` inside `except ValueError: continue`. The dynamic block prints
# "-" in refconv_rms_db / refconv_peak for the cells that did not get the 2L convergence reference, so
# five genuinely measured dynamic cells were discarded by a PARSE failure that then looked like an
# absence of data: the run reported 3 dynamic cells where the matrix holds 8, and exited 0. Five rules
# now hold, and every one of them is a hard error rather than a skip:
#
#   1. OPTIONAL COLUMNS ARE OPTIONAL, not a reason to drop a row. "-" parses to None.
#   2. IDS AND METADATA ARE PINNED against the cell id's own grammar. The id encodes sr, f0 and the
#      stimulus token, so sr, f0, duty/u and kmax are recomputed from the id string and compared with
#      the columns. A row filed at the wrong rate, or under a slightly different stimulus, fails.
#   3. MISSING ROWS, DUPLICATE IDS and NON-FINITE VALUES are failures.
#   4. NOTHING IS COMPARED AS AN INTERSECTION. The two arms must carry the SAME id set, kind by kind;
#      the same id with different stimulus metadata in the two arms is a failure, not a row.
#   5. THE COUNTS ARE RECONCILED against the analyzer's own census. tools/gh19_s3_pulse_analyze.py
#      fences each matrix with `-- BEGIN MATRIX <kind>` / `-- END MATRIX <kind> declared= emitted=
#      skipped=`, so "we compared the rows we could read and called it the total" is no longer a
#      representable outcome: parsed + declared-skips must equal `emitted`, `emitted + skipped` must
#      equal `declared`, and the moving-duty block must additionally account for every cell it did not
#      reference (`referenced + unreferenced` == the total it declares).
#   6. THE EXPECTED CELL SET COMES FROM A PINNED MANIFEST, NOT FROM THE ARMS. `--manifest` is required
#      and has no default. Rules 3-5 are all statements about an arm agreeing with ITSELF, and every
#      one of them is satisfied by deleting the same cell from both arms -- @Codex reproduced exactly
#      that (drop `vco_a_pulse_44100_220_pw10` from both arms, decrement each fence 80 -> 79, exit 0,
#      print "every declared cell is present in BOTH arms"). The manifest is the probe's own declared
#      stimulus plan, enumerated from the declared axes before any render and checked in as
#      report/gh19-s3-pulse-aa/expected_cells.tsv; it is regenerated and byte-compared by
#      tools/run_gh19_s3_coverage.py, so editing an axis without re-pinning it is itself a failure.
#      Both directions fail: a declared cell no arm consumed (not produced, or not parsed), and a
#      consumed cell no manifest declares.
#
# USAGE:
#   python3 tools/gh19_s3_pulse_delta.py --base <base_full.txt> --cand <cand_full.txt> \
#       --manifest report/gh19-s3-pulse-aa/expected_cells.tsv [--out <deltas.tsv>]
# The two inputs are the --out files of tools/gh19_s3_pulse_analyze.py for the two arms.

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gh19_s3_pulse_analyze as A  # noqa: E402  (single source of truth for pulse_node_weight)
from _gh19_textio import open_text

# (labels, optional v-indices, required v-indices). The two report blocks have different column counts
# and DIFFERENT COLUMN MEANINGS -- the mixed block replaces duty with u -- so they are parsed by their
# own arity and their own grammar rather than by position in one table. `v` is f[1:], so label i+1 of
# the printed header is v[i].
#
# sr_i / f0_i / a_i name the columns this block carries the rate, the target frequency and the fitted
# scale in, so those checks are per-block facts rather than a positional assumption that only holds
# for the blocks whose columns happen to start the same way. a_i is None for the two IDENTITY blocks:
# they compare two renders of one stimulus and fit no scale, so there is no A to range-check.
BLOCK_STATIC = {
    "arity": 13,
    "labels": ("id", "sr", "f0", "duty", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "floor_db", "harm_db", "kmax"),
    "str_v": frozenset(),
    "optional": frozenset(),
    "required": frozenset(range(0, 12)),
    "sr_i": 0, "f0_i": 1, "a_i": 3,
}
BLOCK_MIXED = {
    "arity": 13,
    "labels": ("id", "sr", "f0", "u", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "floor_db", "harm_db", "kmax"),
    "str_v": frozenset(),
    "optional": frozenset(),
    "required": frozenset(range(0, 12)),
    "sr_i": 0, "f0_i": 1, "a_i": 3,
}
BLOCK_DYNAMIC = {
    "arity": 14,
    "labels": ("id", "sr", "f0", "A", "fund", "dc", "res_db",
               "res_1k5k_db", "res_effbd_db", "refconv_rms_db", "refconv_peak",
               "res_1k5k2L_db", "res_effbd2L_db", "n"),
    "str_v": frozenset(),
    "optional": frozenset((8, 9, 10, 11)),
    "required": frozenset((0, 1, 2, 3, 4, 5, 6, 7, 12)),
    "sr_i": 0, "f0_i": 1, "a_i": 2,
}
# id twin sr f0 maxdiff_audio maxdiff_trace n -- the identity blocks do not print a duty/u or an A.
#
# ALL INDICES HERE ARE v-INDICES (v = f[1:], so v[0] is the printed column 2). This block's first
# column after the id is a NAME, not a number, so `str_v` exempts it from numeric parsing: without
# that exemption the twin is read as a float and the whole block fails to parse.
BLOCK_CONNECT = {
    "arity": 7,
    "labels": ("id", "twin", "sr", "f0", "maxdiff_audio", "maxdiff_trace", "n"),
    "str_v": frozenset((0,)),          # v[0] = twin, a cell id
    "optional": frozenset((4,)),       # v[4] = maxdiff_trace, '-' where no trace was recorded
    "required": frozenset((1, 2, 3, 5)),
    "sr_i": 1, "f0_i": 2, "a_i": None,
}
BLOCK_ABSYM = {
    "arity": 7,
    "labels": ("id", "twin", "sr", "f0", "maxdiff_audio", "maxdiff_trace", "n"),
    "str_v": frozenset((0,)),
    "optional": frozenset((4,)),
    "required": frozenset((1, 2, 3, 5)),
    "sr_i": 1, "f0_i": 2, "a_i": None,
}
BLOCKS = {"static": BLOCK_STATIC, "mixed": BLOCK_MIXED, "dynamic": BLOCK_DYNAMIC,
          "connect": BLOCK_CONNECT, "ab_asymmetry": BLOCK_ABSYM}
# DELTA_KINDS are compared baseline-vs-candidate. IDENTITY_KINDS are per-arm checks that a stimulus
# rendered through two routes is ONE signal; they carry no arm-to-arm delta, but they are declared in
# the same manifest and must be consumed by a fence like every other cell.
DELTA_KINDS = ("static", "dynamic", "mixed")
IDENTITY_KINDS = ("connect", "ab_asymmetry")
# Every block whose columns must carry maxdiff_audio == 0 (and maxdiff_trace == 0 where recorded).
MUST_BE_ZERO = {"connect": (3, 4), "ab_asymmetry": (3,)}   # v-indices: maxdiff_audio, maxdiff_trace

# The id grammar IS the metadata pin: every cell id is built by the probe from the very numbers the
# analyzer prints beside it, so a mismatch means the row was not produced by the cell it names.
ID_STATIC = re.compile(r"^vco_(a|b)_pulse(hi)?_(\d+)_(\d+)_pw(\d+)$")
ID_MIXED = re.compile(r"^vco_a_ringmix_(\d+)_(\d+)_pw(\d+)_m(\d+)$")
ID_DYNAMIC = re.compile(r"^vco_(a|b)_dynpwm_(\d+)_(\d+)_(tri|sq)_r(\d+)_b(\d+)_d(\d+)$")
# The connect and ab_asymmetry cells are DYNAMIC ids with a route tag appended: the probe builds them
# with the same dynCellId() and only the cableTag differs, so the grammar here must be the dynamic one
# plus that literal suffix and nothing else. (A previous revision of this file assumed a `_connect`
# infix between `dynpwm` and the sample rate. No such id exists; the manifest caught it.)
ID_CONNECT = re.compile(r"^vco_a_dynpwm_(\d+)_(\d+)_(tri|sq)_r(\d+)_b(\d+)_d(\d+)_viaConnect$")
ID_ABSYM = re.compile(r"^vco_b_dynpwm_(\d+)_(\d+)_(tri|sq)_r(\d+)_b(\d+)_d(\d+)_unpatched_under_a_cable$")
# --- id grammar -> group numbers, so the pin cannot be written from memory of one block ---
# static  : vco_{a|b}_pulse{hi|}_SR_F0_pwPW          (1 side, 2 hi, 3 sr, 4 f0, 5 pw)
# mixed   : vco_a_ringmix_SR_F0_pwPW_mMORPH         (1 sr, 2 f0, 3 pw, 4 morph)
# dynamic : vco_{a|b}_dynpwm_SR_F0_WAV_rR_bB_dD     (1 side, 2 sr, 3 f0, 4 wave, 5 r, 6 b, 7 d)
# connect : vco_a_dynpwm_SR_F0_WAV_rR_bB_dD_viaConnect                  (1 sr, 2 f0, 3 wave, ...)
# absym   : vco_b_dynpwm_SR_F0_WAV_rR_bB_dD_unpatched_under_a_cable     (1 sr, 2 f0, 3 wave, ...)
ID_GROUPS = {
    "static": {"sr": 3, "f0": 4, "pw": 5},
    "mixed": {"sr": 1, "f0": 2, "morph": 4, "pw": 3},
    "dynamic": {"sr": 2, "f0": 3, "wave": 4, "rate": 5, "base": 6, "depth": 7},
    "connect": {"sr": 1, "f0": 2, "wave": 3, "rate": 4, "base": 5, "depth": 6},
    "ab_asymmetry": {"sr": 1, "f0": 2, "wave": 3, "rate": 4, "base": 5, "depth": 6},
}
ID_RE = {"static": ID_STATIC, "mixed": ID_MIXED, "dynamic": ID_DYNAMIC,
         "connect": ID_CONNECT, "ab_asymmetry": ID_ABSYM}

# "<id> no harmonic below Nyquist" / "<id> no source trace": the analyzer's two declared skips. They
# are accounted for in the fence rather than dropped, so they are allowed inside a fence -- but they
# must still name a grammar-valid id, and they count as neither parsed nor duplicated.
SKIP_SUFFIXES = ("no harmonic below Nyquist", "no source trace")

FENCE_BEGIN = re.compile(r"^-- BEGIN MATRIX (\w+)$")
FENCE_END = re.compile(r"^-- END MATRIX (\w+)((?:\s+\w+=\S+)*)$")
FENCE_FIELD = re.compile(r"(\w+)=(\S+)")
DYN_CENSUS = re.compile(r"declared reference subset:\s*(\d+)\s+of\s+(\d+)\s+moving-duty cells")
UNREF_HEAD = re.compile(r"un-referenced moving-duty cells \((\d+),")
# The analyzer's machine-readable band declaration, one per block, printed BEFORE that block's column
# header. ANCHORED AND ORDERED, not scraped by a key=value sweep: the line is followed by prose that
# also contains `<name> = <value>` text, and a permissive sweep would read the commentary as part of
# the declaration. A field dropped or reordered here is a parse failure, which is the intent.
BAND_DECL = re.compile(r"^BAND-DECL (\w+) b1_lo=(\S+) b1_hi=(\S+) eff_lo=(\S+) "
                       r"eff_rule=(\S+) eff_hi_cycles=(\S+)")

# The fitted scale A absorbs the unit-pulse -> device-domain conversion and measures ~0.500. A row
# whose A is far from that did not come from a rendered cell, so it is rejected rather than averaged
# into a delta. The bounds are deliberately loose: this is a "is this a rendered cell at all" guard,
# not a claim about the value.
A_MIN, A_MAX = 0.3, 0.7


class Fatal(Exception):
    pass


def _lbl(spec, i):
    """The printed header name of v-index i. `v = f[1:]` and `labels` is the printed header including
    its leading `id`, so the two index spaces are offset by one; naming that conversion once keeps
    every diagnostic from quietly reporting the neighbouring column."""
    return spec["labels"][i + 1]


def _num(tok, what, where):
    if tok == "-":
        raise Fatal("%s: %s is '-' but the column is required" % (where, what))
    try:
        v = float(tok)
    except ValueError:
        raise Fatal("%s: %s is not a number: %r" % (where, what, tok))
    if not math.isfinite(v):
        raise Fatal("%s: %s is not finite: %r" % (where, what, tok))
    return v


def _pin(where, kind, cid, v, sr, f0, bdecl):
    """Recompute the id's own metadata and compare it with the printed columns. `v` indices differ per
    block (see the BLOCK_* label tuples), so each kind names the columns it pins."""
    m = ID_RE[kind].match(cid)
    if not m:
        raise Fatal("%s: id %r does not match the %s grammar" % (where, cid, kind))
    g = ID_GROUPS[kind]
    id_sr, id_f0 = float(m.group(g["sr"])), float(m.group(g["f0"]))
    if sr != id_sr:
        raise Fatal("%s: %s sr column %g != sr in id %g" % (where, cid, sr, id_sr))
    if f0 != id_f0:
        raise Fatal("%s: %s f0 column %g != f0 in id %g" % (where, cid, f0, id_f0))
    if kind == "static":
        # v: 0 sr 1 f0 2 duty 3 A 4 fund 5 dc 6 res 7 b1 8 eb 9 floor 10 harm 11 kmax
        want = float(m.group(g["pw"])) / 100.0
        if abs(v[2] - want) > 6e-4:
            raise Fatal("%s: %s duty column %.4f != pw token in id -> %.4f" % (where, cid, v[2], want))
        _pin_kmax(where, cid, v[11], sr, f0)
        _pin_band_order(where, cid, sr, v[6], v[7], v[8], bdecl)
    elif kind == "mixed":
        # v: 0 sr 1 f0 2 u 3 A 4 fund 5 dc 6 res 7 b1 8 eb 9 floor 10 harm 11 kmax
        morph = float(m.group(g["morph"])) / 100000.0
        if not (0.0 <= morph <= 1.0):
            raise Fatal("%s: %s morph token %.5f outside [0,1]" % (where, cid, morph))
        want = A.pulse_node_weight(morph)
        if abs(v[2] - want) > 6e-4:
            raise Fatal("%s: %s u column %.4f != pulse_node_weight(%.5f) = %.4f"
                        % (where, cid, v[2], morph, want))
        _pin_kmax(where, cid, v[11], sr, f0)
        _pin_band_order(where, cid, sr, v[6], v[7], v[8], bdecl)
    elif kind == "dynamic":
        # v: 0 sr 1 f0 2 A 3 fund 4 dc 5 res 6 b1 7 eb 8 refconv_rms 9 refconv_peak 10 b1_2L 11 eb_2L 12 n
        # The dynamic block does not print duty, so the only id-derived pin left is that the window
        # length is a usable power of two; that it is the SAME for every cell is checked across the
        # block, not here.
        _pin_window(where, cid, v[12])
        _pin_band_order(where, cid, sr, v[5], v[6], v[7], bdecl)
        # The four optional columns TRAVEL TOGETHER: the convergence pair and the two 2L band columns
        # are written by one branch of the analyzer, so a row carrying some but not all of them was not
        # produced by that branch and the meaning of the ones it does carry is unknown.
        n2L = sum(1 for i in (8, 9, 10, 11) if v[i] is not None)
        if n2L not in (0, 4):
            raise Fatal("%s: %s carries %d of the 4 convergence/2L columns -- the analyzer writes "
                        "them together, so a partial row is not a measurement" % (where, cid, n2L))
    else:
        # connect / ab_asymmetry: v 0 twin 1 sr 2 f0 3 maxdiff_audio 4 maxdiff_trace 5 n
        # n here is the RAW CAPTURE length the two routes were compared over, NOT the dynamic block's
        # power-of-two analysis window: it is 24576 = 2^13 * 3 and would fail that block's pin. What it
        # must be is a positive integer, and the same one across the whole family -- a family whose
        # comparisons cover different amounts of audio is not one identity check.
        _pin_rawlen(where, cid, v[5])
        _pin_twin(where, kind, cid, v[0], sr, f0)


def _pin_window(where, cid, n):
    if n < 1024 or n != int(n) or (int(n) & (int(n) - 1)) != 0:
        raise Fatal("%s: %s n = %s is not a power of two >= 1024" % (where, cid, n))


def _pin_rawlen(where, cid, n):
    """The identity blocks' n is the raw capture length the two routes were compared over."""
    if n < 1 or n != int(n):
        raise Fatal("%s: %s n = %s is not a positive whole number of samples" % (where, cid, n))


def _pin_twin(where, kind, cid, twin, sr, f0):
    """The twin column is a CLAIM about which cell the identity was compared against. A row naming a
    twin it could not have compared against -- wrong family, wrong side, wrong rate, wrong duty -- is
    not evidence of identity even when its maxdiff column reads 0, so the name is pinned too."""
    if kind == "connect":
        if twin != cid[: -len("_viaConnect")]:
            raise Fatal("%s: %s twin %r is not this cell's kState counterpart" % (where, cid, twin))
        mt = ID_DYNAMIC.match(twin)
        if not mt:
            raise Fatal("%s: %s twin %r does not match the dynamic grammar" % (where, cid, twin))
        gt = ID_GROUPS["dynamic"]
        if mt.group(1) != "a":
            raise Fatal("%s: %s twin %r is not an A-side cell" % (where, cid, twin))
        if float(mt.group(gt["sr"])) != sr or float(mt.group(gt["f0"])) != f0:
            raise Fatal("%s: %s twin %r carries a different sr/f0" % (where, cid, twin))
    else:
        mt = ID_STATIC.match(twin)
        if not mt:
            raise Fatal("%s: %s twin %r does not match the static grammar" % (where, cid, twin))
        if mt.group(1) != "b" or mt.group(2) is not None or int(mt.group(5)) != 50:
            raise Fatal("%s: %s twin %r is not vco_b_pulse_<sr>_<f>_pw50" % (where, cid, twin))
        if float(mt.group(3)) != sr or float(mt.group(4)) != f0:
            raise Fatal("%s: %s twin %r carries a different sr/f0" % (where, cid, twin))


def _pin_kmax(where, cid, kmax_col, sr, f0):
    """kmax is an exact integer identity, not a fitted quantity: floor((sr/2)/f0)."""
    want_k = int(math.floor((sr / 2.0) / f0))
    if int(round(kmax_col)) != want_k or abs(kmax_col - want_k) > 1e-9:
        raise Fatal("%s: %s kmax column %s != floor((sr/2)/f0) = %d" % (where, cid, kmax_col, want_k))


# The full-band column and res_db are the same residual over the same band taken through two different
# weightings -- res_db is the unwindowed time-domain rms ratio, res_effbd_db is the same ratio after a
# Hann window, and the window's amplitude weighting is the only thing between them (the Parseval
# identity the analyzer's self-check (e) tests: with a RECTANGULAR window they are equal to machine
# precision, 7.25e-13, because the window power normalization and the N/2 one-sided factor cancel in
# the ratio). MEASURED on this grid: |res_effbd_db - res_db| <= 0.35 dB over all 94 delta cells in both
# arms and all three blocks. The slack is set to 1.0 dB -- loose enough not to be a threshold the
# values were fitted to, tight enough that a column wired to the wrong quantity leaves it: the narrow
# band column differs from res_db by up to 24.18 dB on the same cells, and a column filled from a
# different band or an unfitted reference moves by whole dB rather than tenths.
BAND_SLACK_DB = 1.0


def _pin_band_order(where, cid, sr, res_db, b1_db, eb_db, bdecl):
    """The two band columns are OUTPUT-RESIDUAL ratios, and this pins the structure that makes them a
    pair of measurements instead of two numbers that happen to sit next to each other.

    WHY (@Codex 144b1339 item 2). The contract's 100-5000 Hz and effective-full-band requirement is a
    requirement on the RESIDUAL, not a statement about a filter's gain in those bands. Both columns are
    therefore band-limited residual-over-reference power ratios against the SAME fitted A, window and
    denominator, and the band they were taken over has to be a declared fact rather than something a
    reader recovers from whichever block they are looking at.

    WHAT IS CHECKED, and what is deliberately NOT. There is no ordering between the two values: power
    ratios over nested bands are not monotone, so `b1 <= eb` would be an invented law and a pin that
    asserted it would fail on a correct run. What must hold is the STRUCTURE -- the bands nest for this
    cell's rate, they are not the same band under two names, and the full-band column is the cell's own
    full-band residual. The producer's own B1_LO_HZ / B1_HI_HZ are read from the analyzer rather than
    restated here, so the two cannot drift apart, and the effective band edge comes from the block's
    own BAND-DECL line (sr/2 for an analytic reference, a MEASURED fraction of sr for a decimated one)
    instead of being assumed to be Nyquist.
    """
    lo, hi = A.B1_LO_HZ, A.B1_HI_HZ
    if abs(bdecl["b1_lo"] - lo) > 1e-9 or abs(bdecl["b1_hi"] - hi) > 1e-9:
        raise Fatal("%s: %s block declares band [%g, %g] Hz but the analyzer's constants are "
                    "[%g, %g]" % (where, cid, bdecl["b1_lo"], bdecl["b1_hi"], lo, hi))
    eff_lo, eff_hi = bdecl["eff_lo"], bdecl["eff_hi_cycles"] * sr
    if not (eff_lo <= lo <= hi <= eff_hi):
        raise Fatal("%s: %s declares the narrow band [%g, %g] Hz outside the effective band "
                    "(%.0f, %.0f] Hz at sr=%g -- the 'effective full band' column is the NARROWER of "
                    "the two for this cell, so the two labels describe each other's quantity"
                    % (where, cid, lo, hi, eff_lo, eff_hi, sr))
    if lo <= eff_lo and hi >= eff_hi:
        raise Fatal("%s: %s band [%g, %g] Hz IS the effective band (%.0f, %.0f] Hz: the two columns "
                    "are one measurement printed under two names" % (where, cid, lo, hi, eff_lo, eff_hi))
    for nm, val in (("res_1k5k_db", b1_db), ("res_effbd_db", eb_db)):
        if val is None or not math.isfinite(val):
            raise Fatal("%s: %s %s is not a finite number" % (where, cid, nm))
    if abs(eb_db - res_db) > BAND_SLACK_DB:
        raise Fatal("%s: %s res_effbd_db %.2f differs from res_db %.2f by %.2f dB, past the window's "
                    "amplitude weighting (%.2f dB) -- the full-band column is not this cell's "
                    "full-band residual" % (where, cid, eb_db, res_db, eb_db - res_db, BAND_SLACK_DB))


def _pin_band_decl(where, kind, bd):
    """The block's own BAND-DECL line, checked against the rule it names.

    Two of these are claims rather than formatting. An `output_nyquist` edge must be exactly 0.5 cycles
    per output sample -- an analytic reference's band ends at Nyquist by construction. Any OTHER rule
    names a MEASURED edge (the decimated block's is the filter's -0.1 dB endpoint), and a measured edge
    that came out at exactly Nyquist would be claiming a decimation filter that is flat to Nyquist, which
    is the one thing a decimator cannot be. Both directions are wrong for the same reason: the number
    and the rule have to describe the same thing.
    """
    if bd["eff_lo"] != 0.0:
        raise Fatal("%s: %s effective band starts at %g Hz, not 0" % (where, kind, bd["eff_lo"]))
    if bd["eff_rule"] == "output_nyquist":
        if abs(bd["eff_hi_cycles"] - 0.5) > 1e-12:
            raise Fatal("%s: %s declares rule %s but an edge at %.9f cycles, not Nyquist (0.5)"
                        % (where, kind, bd["eff_rule"], bd["eff_hi_cycles"]))
    elif not (0.0 < bd["eff_hi_cycles"] < 0.5):
        raise Fatal("%s: %s declares a MEASURED edge (rule %s) at %.9f cycles per output sample, "
                    "which is not inside (0, 0.5)" % (where, kind, bd["eff_rule"], bd["eff_hi_cycles"]))


def parse(path, arm):
    """({kind: {id: row}}, {kind: census}, {kind: {skip ids}}, {kind: band declaration}). Every
    irregularity is a Fatal, never a skip. The skip ids are returned as well as counted because the
    manifest reconciliation has to know WHICH declared cells were skipped, not just how many: a run that
    skips one id and parses another twice has the right total and the wrong cells."""
    rows = {k: {} for k in BLOCKS}
    census = {}
    cur = None
    saw_header = False
    declared_skips = {}
    skip_ids = {k: {} for k in BLOCKS}     # id -> the reason the line claims
    unref = None          # None, or the declared count while inside the un-referenced id list
    unref_ids = []
    dyn_of = None
    decls = {}

    with open_text(path) as fh:
        for lineno, raw in enumerate(fh, 1):
            ln = raw.rstrip("\n")
            f = ln.split()
            if not f:
                continue
            where = "%s:%d" % (os.path.basename(path), lineno)

            def fence_end(kind, tail):
                nonlocal cur, saw_header, unref, declared_skips, dyn_of
                if kind not in BLOCKS:
                    raise Fatal("%s: END fence names unknown matrix %r" % (where, kind))
                if cur != kind:
                    raise Fatal("%s: END fence for %r but %r is open" % (where, kind, cur))
                fields = dict(FENCE_FIELD.findall(tail))
                for need in ("declared", "emitted", "skipped"):
                    if need not in fields:
                        raise Fatal("%s: END fence for %r lacks %s=" % (where, kind, need))
                declared, emitted, skipped = (int(fields[n]) for n in ("declared", "emitted", "skipped"))
                n_parsed = len(rows[kind])
                n_skip_decl = declared_skips.get(kind, 0)
                # `emitted` is the number of DATA ROWS the producer printed; a cell it skipped is
                # accounted for by `skipped`, not by `emitted`. The two invariants are therefore
                # n_parsed == emitted and n_skip_decl == skipped, with declared == emitted + skipped
                # (checked just below). An earlier revision wrote the first one as
                # `n_parsed + n_skip_decl == emitted`, which is only ever satisfied while no skip
                # exists -- it rejected every legitimate skip and would have made the block's own
                # skip accounting unusable the first time a cell needed one.
                if n_parsed != emitted:
                    raise Fatal("%s: %s parsed %d rows != emitted=%d"
                                % (where, kind, n_parsed, emitted))
                if emitted + skipped != declared:
                    raise Fatal("%s: %s emitted=%d + skipped=%d != declared=%d"
                                % (where, kind, emitted, skipped, declared))
                if n_skip_decl != skipped:
                    raise Fatal("%s: %s parsed %d declared skip(s) != the fence's skipped=%d"
                                % (where, kind, n_skip_decl, skipped))
                census[kind] = {"declared": declared, "emitted": emitted, "skipped": skipped}
                if kind == "dynamic":
                    n_unref = int(fields.get("unreferenced", "-1"))
                    if n_unref < 0:
                        raise Fatal("%s: dynamic END fence lacks unreferenced=" % where)
                    if n_unref != len(unref_ids):
                        raise Fatal("%s: declared unreferenced=%d but %d ids are listed"
                                    % (where, n_unref, len(unref_ids)))
                    census["dynamic"]["unreferenced"] = n_unref
                    census["dynamic"]["unreferenced_ids"] = list(unref_ids)
                    if dyn_of is None:
                        raise Fatal("%s: dynamic block never declared its moving-duty census" % where)
                    if emitted + skipped != dyn_of[0]:
                        raise Fatal("%s: dynamic emitted=%d + skipped=%d != referenced subset %d"
                                    % (where, emitted, skipped, dyn_of[0]))
                    if dyn_of[0] + n_unref != dyn_of[1]:
                        raise Fatal("%s: referenced %d + unreferenced %d != total moving-duty %d"
                                    % (where, dyn_of[0], n_unref, dyn_of[1]))
                # A block whose two band columns are printed without a declared band has columns whose
                # meaning the reader has to reconstruct. The declaration is required, not optional.
                if kind in DELTA_KINDS:
                    bd = decls.get(kind)
                    if bd is None:
                        raise Fatal("%s: the %s block declares no BAND-DECL line, so its two band "
                                    "columns have no stated band" % (where, kind))
                    _pin_band_decl(where, kind, bd)
                cur, saw_header, unref = None, False, None
                return

            mb = FENCE_BEGIN.match(ln.strip())
            if mb:
                if cur is not None:
                    raise Fatal("%s: BEGIN fence for %r while %r is open" % (where, mb.group(1), cur))
                cur, saw_header = mb.group(1), False
                if cur not in BLOCKS:
                    raise Fatal("%s: BEGIN fence names unknown matrix %r" % (where, cur))
                # Opened inside its own block, so the count cannot be satisfied by a list that
                # belonged to an earlier block.
                unref_ids.clear()
                continue
            me = FENCE_END.match(ln.strip())
            if me:
                fence_end(me.group(1), me.group(2))
                continue

            if unref is not None:
                # inside the un-referenced id list: one id per line, already fenced above
                if len(f) != 1 or not ID_DYNAMIC.match(f[0]):
                    raise Fatal("%s: expected an un-referenced dynamic id, got %r" % (where, ln.strip()))
                unref_ids.append(f[0])
                if len(unref_ids) > unref:
                    raise Fatal("%s: more than the declared %d un-referenced ids" % (where, unref))
                continue

            m = UNREF_HEAD.search(ln)
            if m:
                unref = int(m.group(1))
                continue

            if cur is None:
                continue

            md = DYN_CENSUS.search(ln)
            if md:
                dyn_of = (int(md.group(1)), int(md.group(2)))
                continue

            mbd = BAND_DECL.match(ln.strip())
            if mbd:
                if cur is None:
                    raise Fatal("%s: BAND-DECL outside any matrix fence" % where)
                if mbd.group(1) != cur:
                    raise Fatal("%s: BAND-DECL names %r but the %r block is open"
                                % (where, mbd.group(1), cur))
                if saw_header:
                    raise Fatal("%s: the %s BAND-DECL appears after its column header, so the rows "
                                "above it were read without it" % (where, cur))
                if cur in decls:
                    raise Fatal("%s: the %s block declares its band twice" % (where, cur))
                # The RAW line is kept beside the parsed fields: the arm-to-arm pin is a byte
                # comparison of the declarations, which is a stronger statement than comparing the
                # numbers they parse to.
                decls[cur] = {"raw": ln.strip(),
                              "b1_lo": _num(mbd.group(2), "BAND-DECL b1_lo", where),
                              "b1_hi": _num(mbd.group(3), "BAND-DECL b1_hi", where),
                              "eff_lo": _num(mbd.group(4), "BAND-DECL eff_lo", where),
                              "eff_rule": mbd.group(5),
                              "eff_hi_cycles": _num(mbd.group(6), "BAND-DECL eff_hi_cycles", where)}
                continue

            if not saw_header:
                if f[0] == "id":
                    saw_header = True
                    continue
                continue  # prose above the column header

            # ---- a line after the column header must be a data row or a declared skip ----
            spec = BLOCKS[cur]
            skip_hit = None
            for suf in SKIP_SUFFIXES:
                if ln.strip().endswith(suf):
                    head_tok = ln.strip()[: -len(suf)].split()
                    if len(head_tok) != 1:
                        raise Fatal("%s: malformed skip line %r" % (where, ln.strip()))
                    cid = head_tok[0]
                    if not ID_RE[cur].match(cid):
                        raise Fatal("%s: skip line names %r, not a %s id" % (where, cid, cur))
                    if cid in rows[cur]:
                        raise Fatal("%s: %s both parsed and declared skipped" % (where, cid))
                    if cid in skip_ids[cur]:
                        raise Fatal("%s: %s declared skipped twice" % (where, cid))
                    declared_skips[cur] = declared_skips.get(cur, 0) + 1
                    # The reason is KEPT, not just counted, so the coverage lock can check the
                    # condition the skip claims instead of taking the producer's word for it. See
                    # skip_legitimate().
                    skip_ids[cur][cid] = suf
                    skip_hit = cid
                    break
            if skip_hit is not None:
                continue

            if len(f) != spec["arity"]:
                raise Fatal("%s: %d fields in the %s block, expected %d: %r"
                            % (where, len(f), cur, spec["arity"], ln.strip()))
            cid = f[0]
            if cid in rows[cur]:
                raise Fatal("%s: duplicate id %s in the %s block" % (where, cid, cur))
            if not ID_RE[cur].match(cid):
                raise Fatal("%s: id %r does not match the %s grammar" % (where, cid, cur))
            v = []
            for i, tok in enumerate(f[1:]):
                if i in spec["str_v"]:
                    v.append(tok)          # a NAME column (the identity blocks' twin): never a float
                elif i in spec["optional"] and tok == "-":
                    v.append(None)
                else:
                    v.append(_num(tok, _lbl(spec, i), "%s (%s)" % (where, cid)))
            for i in spec["required"]:
                if v[i] is None:
                    raise Fatal("%s: %s %s is '-' but the column is required"
                                % (where, cid, _lbl(spec, i)))
            if spec["a_i"] is not None:
                a_i = spec["a_i"]
                if not (A_MIN < v[a_i] < A_MAX):
                    raise Fatal("%s: %s A = %s outside [%g, %g] -- not a rendered cell"
                                % (where, cid, v[a_i], A_MIN, A_MAX))
            # Only the delta blocks carry band columns, so only they have a band to declare; the two
            # identity blocks compare two routes of one stimulus and print no residual. The
            # requirement is per-block-fact, not applied to a block that never had such a column.
            bd = decls.get(cur)
            if bd is None and cur in DELTA_KINDS:
                # Fail closed rather than passing None into the band pin: a row that reached here
                # without its block's declaration is a row whose band columns were not checked.
                raise Fatal("%s: %s was read before its block's BAND-DECL line" % (where, cid))
            _pin(where, cur, cid, v, v[spec["sr_i"]], v[spec["f0_i"]], bd)
            rows[cur][cid] = {"v": v}

    if cur is not None:
        raise Fatal("%s: file ended inside the %r fence" % (os.path.basename(path), cur))
    # EVERY family the manifest can declare must have a fence in each arm. A missing fence is not an
    # empty block: it is a block whose cells were neither parsed nor declared skipped, and treating it
    # as absent would let a whole family disappear without changing any count the tool looks at.
    for kind in BLOCKS:
        if kind not in census:
            raise Fatal("%s: no %s matrix in the report" % (os.path.basename(path), kind))
    # Every dynamic cell shares one analysis window length; a block that mixes two is not one matrix.
    ns = {r["v"][12] for r in rows["dynamic"].values()}
    if len(ns) > 1:
        raise Fatal("%s: dynamic block mixes window lengths %s" % (os.path.basename(path), sorted(ns)))
    # Same rule for the identity blocks' raw-capture length: two cells of one family compared over
    # different amounts of audio are not one identity check, even when both read 0.
    for kind in ("connect", "ab_asymmetry"):
        ns = {r["v"][5] for r in rows[kind].values()}
        if len(ns) > 1:
            raise Fatal("%s: %s block mixes comparison lengths %s"
                        % (os.path.basename(path), kind, sorted(ns)))
    return rows, census, skip_ids, decls


def _triple(base, cand, nd=2):
    """`base`, `cand`, `cand - base` for one of the residual columns, each `nd` decimals.

    The three columns travel as a unit because a delta without its two endpoints is not checkable: the
    band deltas are the whole point of the band columns (@Codex 144b1339 item 2 asked for the change in
    these metrics, not only their values), and a reader who wants to know whether a -0.3 dB band
    improvement is real has to see what it was taken between.
    """
    if base is None or cand is None:
        return ["-", "-", "-"]
    return ["%.*f" % (nd, base), "%.*f" % (nd, cand), "%+.*f" % (nd, cand - base)]


def _dyn_row(kid, B, C):
    """Index map for both arms' `v`: 0 sr, 1 f0, 2 A, 3 fund, 4 dc, 5 res, 6 b1, 7 eb,
    8 refconv_rms, 9 refconv_peak, 10 b1_2L, 11 eb_2L, 12 n.

    The convolution columns are the ones an arm can be missing (only the first --conv-cells cells get
    the 2L reference), so they print `-` and are routed through _triple/_pair rather than formatted
    directly: the missing case must produce `-` in ALL of the columns that depend on it, not a number
    in some of them.
    """
    rc_b, rc_c = B[8], C[8]
    row = [kid, "dynamic", "%.0f" % B[0], "%.0f" % B[1], "-"]
    row += _triple(B[5], C[5]) + _triple(B[6], C[6]) + _triple(B[7], C[7])
    row += ["-", "-", "%+.5f" % (C[2] - B[2]), "%+.5f" % (C[3] - B[3]), "%+.5f" % (C[4] - B[4])]
    row += [("%.2f" % rc_b) if rc_b is not None else "-",
            ("%.2f" % rc_c) if rc_c is not None else "-",
            ("%+.2f" % (rc_c - rc_b)) if (rc_b is not None and rc_c is not None) else "-"]
    row += _triple(B[10], C[10]) + _triple(B[11], C[11])
    return row


def _stat_row(kid, kind, B, C):
    """Index map for both arms' `v`: 0 sr, 1 f0, 2 duty(u), 3 A, 4 fund, 5 dc, 6 res, 7 b1, 8 eb,
    9 floor, 10 harm, 11 kmax. The static and mixed blocks have no decimated reference, so the four 2L
    band columns and the convergence pair are `-`: this row and _dyn_row have the same arity on purpose,
    so one header describes the whole matrix and no reader has to know which block they are in to count
    columns."""
    row = [kid, kind, "%.0f" % B[0], "%.0f" % B[1], "%.3f" % B[2]]
    row += _triple(B[6], C[6]) + _triple(B[7], C[7]) + _triple(B[8], C[8])
    row += ["%+.2f" % (C[9] - B[9]), "%+.2f" % (C[10] - B[10]),
            "%+.5f" % (C[3] - B[3]), "%+.5f" % (C[4] - B[4]), "%+.5f" % (C[5] - B[5])]
    row += ["-", "-", "-"] + _triple(None, None) + _triple(None, None)
    return row


# The residual triples come first and together, in the order the contract names them: the total
# diagnostic residual, then the two band-limited output residuals (100-5000 Hz and the effective full
# band), each as base / candidate / delta. The duty timing, level, fundamental and DC columns follow,
# because they are the ways a residual improvement can be faked and a reader checking for that should
# find them next to it rather than at the far end of the row.
HEADER = ("id\tkind\tsr\tf0\tduty_or_u"
          "\tres_base_db\tres_cand_db\tdres_db"
          "\tres1k5k_base_db\tres1k5k_cand_db\tdres1k5k_db"
          "\treseffbd_base_db\treseffbd_cand_db\tdreseffbd_db"
          "\tdfloor_db\tdharm_db\tdA\tdfund\tddc"
          "\trefconv_base_db\trefconv_cand_db\tdrefconv_db"
          "\tres1k5k2L_base_db\tres1k5k2L_cand_db\tdres1k5k2L_db"
          "\treseffbd2L_base_db\treseffbd2L_cand_db\tdreseffbd2L_db")

# Columns that describe the STIMULUS, not the result. The two arms of a baseline/candidate pair render
# the same cells, so these must be identical; a difference means the pair is not a controlled
# comparison and every delta in that row is meaningless. (dynamic's is `n`, the shared analysis window
# length -- v[12] since the band columns were inserted, and a stale index here would silently compare
# the wrong column between arms.)
STIMULUS_COL = {"static": 2, "mixed": 2, "dynamic": 12}
# The residual columns the summary counts improvements in, as (static/mixed v-index, dynamic v-index,
# name). The two blocks index the same three quantities differently because the dynamic block prints no
# duty column and puts the fitted scale first; naming both indices per column is what keeps a summary
# from silently reading refconv_rms as a residual. The total diagnostic residual is what revision 2
# reported; the two band columns are what the contract asks a candidate to move, and a candidate that
# improves the total while leaving both bands alone has not done that.
RES_COLS = ((6, 5, "res_db"), (7, 6, "res1k5k"), (8, 7, "reseffbd"))

PLAN_REV_RE = re.compile(r"^plan_rev\t(\d+)$")
PLAN_HEADER = ("block", "id")
# The analyzer's own verdict on the two identity families, as a machine-readable line. The GATE line
# is required in every arm: a block whose rows all read 0 while the check that produced them never ran
# is the shape of a green that means nothing.
IDENTITY_GATE = re.compile(r"^(CONNECT|ABSYM)-GATE ok=(\d+) checked=(\d+) bad=(\d+)")
GATE_BLOCK = {"CONNECT": "connect", "ABSYM": "ab_asymmetry"}


def load_plan(path):
    """({block: {ids}}, plan_rev) from the PINNED declared stimulus plan.

    THE EXPECTATION IS READ, NEVER DERIVED. This file is produced by the probe's buildPlan() from the
    declared stimulus axes and checked in; the tool that judges a run must not be able to compute what
    the run was supposed to contain from the run. An earlier revision locked coverage by comparing the
    two arms with each other, which a cell deleted from BOTH arms satisfies.
    """
    blocks = {}
    seen = set()
    plan_rev = None
    name = os.path.basename(path)
    with open_text(path) as fh:
        for lineno, raw in enumerate(fh, 1):
            ln = raw.rstrip("\n")
            if not ln:
                continue
            if plan_rev is None:
                m = PLAN_REV_RE.match(ln)
                if not m:
                    raise Fatal("%s:%d: first non-empty line must be plan_rev<TAB>N, got %r"
                                % (name, lineno, ln))
                plan_rev = int(m.group(1))
                continue
            f = ln.split("\t")
            if tuple(f) == PLAN_HEADER:
                continue
            if len(f) != 2:
                raise Fatal("%s:%d: expected <block><TAB><id>, got %r" % (name, lineno, ln))
            blk, cid = f
            if blk not in BLOCKS:
                raise Fatal("%s:%d: manifest names unknown block %r" % (name, lineno, blk))
            if not ID_RE[blk].match(cid):
                raise Fatal("%s:%d: manifest id %r does not match the %s grammar" % (name, lineno, cid, blk))
            if cid in seen:
                raise Fatal("%s:%d: manifest declares %s twice" % (name, lineno, cid))
            seen.add(cid)
            blocks.setdefault(blk, set()).add(cid)
    if plan_rev is None:
        raise Fatal("%s: empty manifest (no plan_rev line)" % name)
    if not blocks:
        raise Fatal("%s: manifest declares no cells" % name)
    return blocks, plan_rev


def consumed(rows_k, cen_k, sk_k, kind):
    """The ids this arm ACCOUNTED FOR in `kind`: parsed rows, declared skips, and -- for the dynamic
    block only -- the moving-duty cells the analyzer reports as un-referenced. Those last ones carry no
    reference this run, so they produce no row; they are still declared stimuli the manifest names,
    and leaving them out is what made `declared=8` look like a complete account of a 56-cell family."""
    got = set(rows_k[kind]) | set(sk_k[kind])
    if kind == "dynamic":
        got |= set(cen_k[kind].get("unreferenced_ids", []))
    return got


def skip_legitimate(kind, cid, reason):
    """Is this skip line's claimed condition actually true of this cell? (true, why) / (false, why).

    WHY THIS EXISTS. A declared skip is the producer's own statement that a cell needs no row, and set
    equality alone accepts any of them: a cell relabelled as skipped is still "consumed", so a report
    could launder a missing measurement into a green matrix without ever looking inconsistent. The
    claim is therefore replaced by the condition it stands for, evaluated from the cell's own declared
    metadata -- which is exactly the metadata the analyzer's own skip tests use:

      no harmonic below Nyquist   the reference is built from harmonics 1..floor((sr/2)/f0), so it
                                  exists iff floor((sr/2)/f0) >= 1, i.e. f0 <= sr/2. True for any
                                  block that builds a band-limited reference.
      no source trace             only a family that RECORDS a per-frame trace can be missing one.
                                  Static and mixed cells record no trace at all, so this reason can
                                  never be true for them; for the three trace-bearing families it is
                                  a fact about the render, which the empty trace column already shows.
    """
    m = ID_RE[kind].match(cid)
    if not m:
        return False, "id does not match the %s grammar" % kind
    g = ID_GROUPS[kind]
    sr, f0 = float(m.group(g["sr"])), float(m.group(g["f0"]))
    if reason == "no harmonic below Nyquist":
        # The reference is built from harmonics 1..floor((sr/2)/f0) and the analyzer skips exactly when
        # that set is EMPTY, i.e. floor((sr/2)/f0) < 1. The skip is legitimate when the condition is
        # true of this cell and a fabrication when it is not -- so the predicate is `kmax < 1`, and on
        # this grid (f0 <= 7040 at sr >= 44100, so kmax >= 3 everywhere) NO cell can legitimately use
        # this reason. Writing it as `kmax >= 1` would have accepted the fabrication and rejected the
        # one real case, which is the wrong way round in both directions.
        kmax = int(math.floor((sr / 2.0) / f0))
        return kmax < 1, "floor((sr/2)/f0) = floor(%g/%g) = %d %s 1" % (
            sr / 2.0, f0, kmax, "<" if kmax < 1 else ">=")
    if reason == "no source trace":
        ok = kind in ("dynamic", "connect", "ab_asymmetry")
        return ok, ("a %s cell records a per-frame trace" % kind if ok else
                    "a %s cell records no trace at all, so it cannot be missing one" % kind)
    return False, "unknown skip reason"


def reconcile(plan, arm, rows_k, cen_k, sk_k):
    """Set equality between the pinned manifest and what this arm consumed, per block, BOTH ways.

    A manifest cell nothing consumed was not produced or not parsed -- those look identical from here,
    so it is reported by name rather than guessed at. A consumed cell the manifest never declared is a
    stimulus that was rendered without being declared. Neither is a smaller green matrix.

    Declared skips are admitted only after skip_legitimate() substitutes the condition for the claim,
    so a skip cannot be used to make a missing measurement look accounted for.
    """
    problems = []
    for kind in sorted(set(plan) | set(cen_k)):
        want, got = plan.get(kind, set()), consumed(rows_k, cen_k, sk_k, kind)
        for cid in sorted(want - got):
            problems.append("%s: %s is declared in the manifest but consumed by nothing "
                            "(not produced, or not parsed)" % (arm, cid))
        for cid in sorted(got - want):
            problems.append("%s: %s was consumed but is not declared in the manifest" % (arm, cid))
        for cid in sorted(sk_k.get(kind, {})):
            if cid not in plan.get(kind, set()):
                problems.append("%s: %s was declared skipped but the manifest does not declare it"
                                % (arm, cid))
                continue
            ok, why = skip_legitimate(kind, cid, sk_k[kind][cid])
            if not ok:
                problems.append("%s: %s was declared skipped as %r, but that is not true of this "
                                "cell (%s)" % (arm, cid, sk_k[kind][cid], why))
    return problems


def identity_problems(rows_k, kind, arm):
    """The identity blocks' own numbers: maxdiff_audio (and maxdiff_trace where recorded) must be
    exactly 0. A row that prints '-' for one of them has not been checked, which is a failure and not
    a pass by omission."""
    bad = []
    spec = BLOCKS[kind]
    for cid in sorted(rows_k[kind]):
        v = rows_k[kind][cid]["v"]
        for i in MUST_BE_ZERO[kind]:
            if v[i] is None:
                bad.append("%s: %s %s is '-' -- the identity was not checked for this cell"
                           % (arm, cid, _lbl(spec, i)))
            elif v[i] != 0.0:
                bad.append("%s: %s %s = %s, must be exactly 0"
                           % (arm, cid, _lbl(spec, i), v[i]))
    return bad


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="analyzer --out of the BASELINE arm")
    ap.add_argument("--cand", required=True, help="analyzer --out of the CANDIDATE arm")
    ap.add_argument("--manifest", required=True,
                    help="the PINNED declared stimulus plan, e.g. "
                         "report/gh19-s3-pulse-aa/expected_cells.tsv. Required and with no default: "
                         "the expected cell set must come from a checked-in artifact, not from a count "
                         "flag the caller can restate and not from the arms being compared.")
    ap.add_argument("--out", default=None)
    args = ap.parse_args(argv)

    try:
        plan, plan_rev = load_plan(args.manifest)
        b, bc, bs, bd_b = parse(args.base, "base")
        c, cc, cs, bd_c = parse(args.cand, "cand")
    except Fatal as e:
        print("FATAL: %s" % e, file=sys.stderr)
        return 4
    except OSError as e:
        print("FATAL: %s" % e, file=sys.stderr)
        return 4

    mname = os.path.basename(args.manifest)
    n_manifest = sum(len(v) for v in plan.values())
    print("manifest %s plan_rev=%d cells=%d (%s)"
          % (mname, plan_rev, n_manifest,
             ", ".join("%s=%d" % (k, len(plan[k])) for k in sorted(plan))))

    # ---- BAND: the two arms must have declared the SAME bands, byte for byte ----
    # Every band delta in the matrix is a subtraction of two band-limited ratios. If the two arms took
    # them over different bands -- a different effective edge because a filter changed, a different
    # narrow band because a constant moved -- then the difference is between two measurements and not an
    # improvement, and it would look exactly like a number. Comparing the RAW declaration line is
    # stronger than comparing the values it parses to: it also pins the rule that produced the edge.
    for kind in DELTA_KINDS:
        if kind not in bd_b or kind not in bd_c:
            print("FATAL: %s BAND-DECL missing in %s"
                  % (kind, "base" if kind not in bd_b else "cand"), file=sys.stderr)
            return 4
        if bd_b[kind]["raw"] != bd_c[kind]["raw"]:
            print("FATAL: %s band declaration differs between arms:\n  base: %s\n  cand: %s"
                  % (kind, bd_b[kind]["raw"], bd_c[kind]["raw"]), file=sys.stderr)
            return 4

    # ---- COVERAGE: the manifest is the expectation, and both arms are checked against it ----
    problems = []
    for arm, rows_k, cen_k, sk_k in (("base", b, bc, bs), ("cand", c, cc, cs)):
        problems += reconcile(plan, arm, rows_k, cen_k, sk_k)
        for kind in IDENTITY_KINDS:
            problems += identity_problems(rows_k, kind, arm)
    if problems:
        for p in problems:
            print("FATAL: %s" % p, file=sys.stderr)
        return 4

    # ---- the analyzer's own identity verdicts must have RUN, in every arm ----
    for path, arm in ((args.base, "base"), (args.cand, "cand")):
        gates = {}
        with open_text(path) as fh:
            for ln in fh:
                mg = IDENTITY_GATE.match(ln.strip())
                if mg:
                    gates[GATE_BLOCK[mg.group(1)]] = tuple(int(mg.group(i)) for i in (2, 3, 4))
        for kind in IDENTITY_KINDS:
            if kind not in gates:
                print("FATAL: %s: no %s-GATE line -- the identity check did not run"
                      % (arm, "CONNECT" if kind == "connect" else "ABSYM"), file=sys.stderr)
                return 4
            ok, checked, nbad = gates[kind]
            if ok != 1 or nbad != 0 or checked != len(b[kind] if arm == "base" else c[kind]):
                print("FATAL: %s: %s gate ok=%d checked=%d bad=%d over %d parsed rows"
                      % (arm, kind, ok, checked, nbad,
                         len(b[kind] if arm == "base" else c[kind])), file=sys.stderr)
                return 4

    lines = [HEADER]
    summary = []
    rc = 0

    for kind in DELTA_KINDS:
        spec = BLOCKS[kind]
        ids_b, ids_c = set(b[kind]), set(c[kind])
        if ids_b != ids_c:
            print("FATAL: %s id sets differ -- base only: %s ; cand only: %s"
                  % (kind, sorted(ids_b - ids_c)[:8], sorted(ids_c - ids_b)[:8]), file=sys.stderr)
            return 4
        if bc[kind] != cc[kind]:
            print("FATAL: %s census differs between arms: base=%s cand=%s"
                  % (kind, bc[kind], cc[kind]), file=sys.stderr)
            return 4
        n_imp = {nm: 0 for _, _, nm in RES_COLS}
        n_imp2L = {nm: 0 for nm in ("res1k5k", "reseffbd")}
        n_conv = 0
        for kid in sorted(ids_b):
            B, C = b[kind][kid]["v"], c[kind][kid]["v"]
            sc = STIMULUS_COL[kind]
            if (B[sc] != C[sc] or B[spec["sr_i"]] != C[spec["sr_i"]]
                    or B[spec["f0_i"]] != C[spec["f0_i"]]):
                print("FATAL: %s %s stimulus metadata differs between arms (sr %s/%s f0 %s/%s "
                      "col%d %s/%s)" % (kind, kid, B[spec["sr_i"]], C[spec["sr_i"]],
                                        B[spec["f0_i"]], C[spec["f0_i"]], sc, B[sc], C[sc]),
                      file=sys.stderr)
                return 4
            row = _dyn_row(kid, B, C) if kind == "dynamic" else _stat_row(kid, kind, B, C)
            lines.append("\t".join(row))
            for i_sm, i_dy, nm in RES_COLS:
                i = i_dy if kind == "dynamic" else i_sm
                if C[i] - B[i] < -0.5:
                    n_imp[nm] += 1
            if kind == "dynamic" and B[10] is not None and C[10] is not None:
                # The 2L arm only exists for the cells the analyzer referenced twice; its count is
                # reported over those cells, not over the block, so it is not read as a rate on a
                # denominator it never had.
                n_conv += 1
                for i, nm in ((10, "res1k5k"), (11, "reseffbd")):
                    if C[i] - B[i] < -0.5:
                        n_imp2L[nm] += 1
        summary.append((kind, n_imp, n_imp2L, n_conv, len(ids_b), bc[kind]))

    n_all = sum(s[4] for s in summary)
    print("cells compared: %d -- PER BLOCK, reconciled against the PINNED manifest (%s plan_rev=%d, "
          "%d declared cells):" % (n_all, mname, plan_rev, n_manifest))
    print("  improvement is counted per residual column, because the two band columns can move "
          "independently of the total:")
    print("  %-8s %-8s %5s %6s %6s %6s %8s %9s %9s   (cells improving > 0.50 dB)"
          % ("kind", "cells", "mnfst", "emit", "skip", "decl", "res_db", "res1k5k", "reseffbd"))
    for kind, n_imp, _, _, n, cen in summary:
        print("  %-8s %-8d %5d %6d %6d %6d %5d/%-3d %5d/%-3d %5d/%-3d"
              % (kind, n, len(plan[kind]), cen["emitted"], cen["skipped"], cen["declared"],
                 n_imp["res_db"], n, n_imp["res1k5k"], n, n_imp["reseffbd"], n))
    for kind, _, n2, n_conv, n, cen in summary:
        if kind == "dynamic":
            print("  dynamic vs the 2L reference (%d of %d cells carry it): res1k5k %d/%d, "
                  "reseffbd %d/%d improve > 0.50 dB -- the same cells, the same bands, a better "
                  "reference, so these are the improvement with the REFERENCE's own error removed"
                  % (n_conv, n, n2["res1k5k"], n_conv, n2["reseffbd"], n_conv))
    dyn = [s[5] for s in summary if s[0] == "dynamic"][0]
    print("  dynamic moving-duty census: referenced=%d unreferenced=%d total=%d  (ids: %s)"
          % (dyn["emitted"] + dyn["skipped"], dyn.get("unreferenced", 0),
             dyn["emitted"] + dyn["skipped"] + dyn.get("unreferenced", 0),
             "listed" if dyn.get("unreferenced") == len(dyn.get("unreferenced_ids", [])) else "MISMATCH"))
    for kind in DELTA_KINDS:
        print("  %-8s bands declared identically in both arms: %s" % (kind, bd_b[kind]["raw"]))
    for kind in IDENTITY_KINDS:
        print("  %-8s compared=%d manifest=%d   identity (maxdiff == 0) confirmed in BOTH arms: %d/%d"
              % (kind, len(b[kind]), len(plan[kind]), len(b[kind]), len(plan[kind])))
    # This sentence used to read "every declared cell is present in BOTH arms (0 skips)", where
    # "declared" meant the analyzer's own count -- a sentence the coverage mutation produced itself.
    # It is now gated on a manifest reconciliation that ran before any delta was computed, and it
    # names the artifact it was checked against.
    if all(s[5]["skipped"] == 0 for s in summary):
        print("  manifest coverage: every one of the %d declared cells was consumed by BOTH arms "
              "(0 skips), checked against %s plan_rev=%d" % (n_manifest, mname, plan_rev))

    if args.out:
        try:
            with open_text(args.out, "w") as fh:
                fh.write("\n".join(lines) + "\n")
        except OSError as e:
            print("FATAL: could not write %s: %s" % (args.out, e), file=sys.stderr)
            return 4
        print("wrote %s" % os.path.basename(args.out))
    else:
        for l in lines:
            print(l)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
