#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
# task #120 / GH#19 S6: the two BASELINE pins must refuse an edited baseline, and what each refuses
# must be something the verdict actually reads.
#
# WHAT THIS IS FOR. `run_gh19_s6_saw_pipeline.py` pins THREE committed artifacts: the baseline
# stimulus manifest (`gh19_s6_scenarios.tsv`), the baseline report (`base_report.txt`), and the
# eight `m50000` raw windows. A pin is only worth its line if two things are true of it, and
# neither is obvious:
#
#   1. IT FIRES, BY ITS OWN NAME, AND ISSUES NO VERDICT. A run handed an edited artifact must stop
#      BEFORE any measurement, must say `REFUSE BASELINE-REPORT-PIN` (or `-RAW-`), and must issue no
#      judgement -- a refusal that still printed `ACCEPT-GATE verdict=` would be a verdict against an
#      unstated baseline wearing a refusal's exit code. Because S6 has three pins, each check also
#      asserts that the EARLIER pins reported OK first: "the raw pin fired" is only a fact if the
#      manifest and report pins had already gone green, otherwise a first-pin abort would be
#      indistinguishable from a third-pin catch.
#
#   2. THE THING IT REFUSES IS NUMERICALLY LOAD-BEARING. If an edited artifact were inert -- if the
#      gate re-derived its numbers from somewhere else -- then pinning it would be decorative and
#      this runner would be proving a property the criterion does not depend on. So each tamper is
#      ALSO fed to the gate directly, with that pin bypassed, and it must move the answer. This
#      mirrors the product-side rule used everywhere else in this slice: a negative control that
#      cannot be shown to change the answer is not a control, it is decoration.
#
# WHY THE RAW PIN HAS ITS OWN PAIR OF CHECKS, AND WHAT CHANGED IN THEM. The equality criterion once
# read the committed baseline windows, so moving one of them fired `EQUALITY-SINE` and the raw pin
# was numerically load-bearing. It is not any more: the criterion is a SAME-BUILD A/B, because the
# committed snapshot was rendered on macOS and the default patch evaluates `std::sin`, whose two
# libms disagree by exactly 1 ULP on 3.76% of identical arguments -- so the snapshot can never be
# reproduced bit-for-bit by the CI toolchain, and comparing against it was measuring the toolchain.
# The raw pin is therefore PROVENANCE: it refuses a checkout whose committed evidence has been
# edited, and nothing judged reads those bytes. Asserting the old claim would assert something false,
# and dropping the check would leave the demotion untested, so the check is INVERTED -- the moved
# baseline window must be INERT to the verdict -- and the criterion's REAL input is exercised
# separately: one ulp moved in one window of arm B, the arm the pipeline renders fresh, which must
# fire `FAIL-EQUALITY-SINE` with the improvement columns untouched. Both halves run through the same
# gate invocation, so the difference between them is the input, not the harness.
#
# HOW THE REPORT TAMPER STAYS INSIDE THE PARSE CONTRACT. `gh19_s6_saw_acceptance.py` carries S6's
# residual columns through the same band-slack contract S3's delta tool enforced: for each row the
# three residual columns must stay mutually consistent. The tamper shifts all three by the SAME
# amount in every row, which preserves every such difference and every column's arity, so the edited
# report is one the tools accept as well-formed. That is the point: this is not a malformed-input
# test, it is a well-formed report carrying different numbers -- exactly the failure mode a SHA pin
# exists to catch. Column positions are read out of each block's OWN header line rather than
# hardcoded, so the fixture comes from the producer and cannot silently disagree with the format it
# edits.
#
# Usage (used by CMake; also runnable by hand):
#   python3 run_gh19_s6_saw_baseline_report_pin.py --probe <path-to-gh19_s6_saw_probe> \
#       --neutral-probe <path-to-gh19_s6_saw_probe_neutral> \
#       --pipeline tools/run_gh19_s6_saw_pipeline.py \
#       --repo-root . \
#       --baseline-arm report/gh19-s6-saw-aa/baseline_arm \
#       --base-report report/gh19-s6-saw-aa/base_report.txt \
#       --criteria report/gh19-s6-saw-aa/acceptance_criteria.tsv \
#       --analyzer tools/gh19_s6_saw_analyze.py \
#       --gate tools/gh19_s6_saw_acceptance.py \
#       --out <scratch-dir>
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from _gh19_textio import open_text

# The three pinned baseline SHAs, mirrored from run_gh19_s6_saw_pipeline.py. Duplicated on purpose:
# if this file imported them, an edit to the driver's constants would move both sides at once and
# this self-test could not notice that the pinned artifact had changed.
BASELINE_SCENARIOS_SHA256 = \
    "09b5a2c03d8f43096220391bbf86c20a670af89ab52a65db5237679e065ba55b"
BASELINE_REPORT_SHA256 = \
    "de30d2d3079473e80425f974a74b6da37066aa77f4afc8f3c71a540c44ee3696"
BASELINE_RAW_SHA256 = {
    "gh19_s6_scn125.raw": "be8224478487ef1dabeb074ad9e747474e7965ad0f7835dbdfe1d153317e9e99",
    "gh19_s6_scn126.raw": "32cd07fca93f9482088a5d7e2fd6a8e41bb58b9eb84a3ab71bac8e16047f2c37",
    "gh19_s6_scn127.raw": "ca032312b2bef91fae78d8c4b3da12837130749c5587e15f6a58b9061fd0acde",
    "gh19_s6_scn128.raw": "1f998e1a37ac2e1432999857e7cd47ddded26094fdf711d9873d5159c4f09e7d",
    "gh19_s6_scn133.raw": "be8224478487ef1dabeb074ad9e747474e7965ad0f7835dbdfe1d153317e9e99",
    "gh19_s6_scn134.raw": "32cd07fca93f9482088a5d7e2fd6a8e41bb58b9eb84a3ab71bac8e16047f2c37",
    "gh19_s6_scn135.raw": "ca032312b2bef91fae78d8c4b3da12837130749c5587e15f6a58b9061fd0acde",
    "gh19_s6_scn136.raw": "1f998e1a37ac2e1432999857e7cd47ddded26094fdf711d9873d5159c4f09e7d",
}

# The window whose samples get edited. scn125 is the first sine cell; its row in the manifest is
# read for `warm` so the edited frame is INSIDE the compared window, taken from the producer's own
# manifest rather than assumed.
TAMPER_RAW = "gh19_s6_scn125.raw"
TAMPER_CELL = "vco_a_sawmix_44100_440_m50000"

RESIDUAL_COLUMNS = ("res_db", "res_1k5k_db", "res_effbd_db")
# The judged improvement columns, as the gate names them in ACCEPT-REPORT. These are the figures a
# tampered baseline must move: if it moved only report-only rows the pin would be protecting a
# non-criterion.
JUDGED_IMPROVEMENT_REPORTS = ("worst_full_band_improvement_db", "worst_band_1k5k_improvement_db")

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

REPORT_REFUSAL_NAME = "REFUSE BASELINE-REPORT-PIN"
RAW_REFUSAL_NAME = "REFUSE BASELINE-RAW-PIN"
# The other pins' prose, quoted from the driver, so "the right pin fired" is checked on the message
# body and not merely on a name that a neighbouring refusal might share a prefix with.
MANIFEST_REFUSAL_TEXT = "the committed baseline manifest is not the pinned one"
RAW_REFUSAL_TEXT = "is not the pinned window"
VERDICT_MARK = "ACCEPT-GATE verdict="
# What the driver prints once it is past the pins and into measurement.
MEASURED_MARKS = ("ACCEPT-PIPELINE fresh_arm=", "ACCEPT-PIPELINE fresh_report=",
                  "ACCEPT-PIPELINE self_check=")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def shift_residuals(text, db):
    """Shift every matrix block's three residual columns by `db`, uniformly across rows.

    Returns (new_text, n_rows, n_blocks). A block is recognised structurally: a line whose fields
    carry all three residual names AND `id` opens one, the width it declares fixes the rows that
    belong to it, and any line of a different width closes it. Column positions are read out of that
    header rather than hardcoded, so the fixture cannot drift from the format the tools parse. A run
    whose header lacks the names is not a matrix and is left alone; if NO block matches, the caller
    reports INVALID rather than staging a pristine 'tampered' report, which would make the pin look
    like it works when nothing had moved.
    """
    out, n_rows, n_blocks = [], 0, 0
    idx, width = None, None
    for line in text.splitlines(keepends=True):
        fields = line.split()
        if fields and all(name in fields for name in RESIDUAL_COLUMNS) and "id" in fields:
            idx = [fields.index(name) for name in RESIDUAL_COLUMNS]
            width = len(fields)
            n_blocks += 1
            out.append(line)
            continue
        if idx is None or len(fields) != width:
            if idx is not None and fields and len(fields) != width:
                idx, width = None, None
            out.append(line)
            continue
        for i in idx:
            fields[i] = "%.6f" % (float(fields[i]) + db)
        n_rows += 1
        out.append("  " + "  ".join(fields) + "\n")
    return "".join(out), n_rows, n_blocks


def window_geometry(manifest, cell):
    """(warm, win) for `cell`, read from the arm's own manifest. Read rather than assumed so the
    edited frame is known to be inside the compared window."""
    with open(manifest, encoding="utf-8") as f:
        header = None
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if header is None:
                header = parts
                continue
            if len(parts) != len(header):
                continue
            row = dict(zip(header, parts))
            if row.get("id") == cell:
                return int(row["warm"]), int(row["win"])
    raise ValueError("cell %s not found in %s" % (cell, manifest))


def tamper_window(src, dst, warm):
    """Copy `src` to `dst` with ONE in-window frame moved by one ulp.

    One frame, and one ulp, on purpose: `bit_diff_count` counts frames whose bit pattern differs, so
    the smallest edit that a single sample admits is exactly the smallest edit that criterion can
    see. A larger tamper would prove the same thing while making it unclear whether the criterion is
    sensitive or merely loud.
    """
    with open(src, "rb") as f:
        buf = bytearray(f.read())
    n = len(buf) // 8
    if warm >= n:
        raise ValueError("warm=%d is outside %s (%d samples)" % (warm, src, n))
    (val,) = struct.unpack_from("<d", buf, warm * 8)
    nxt = struct.unpack("<d", struct.pack("<Q", struct.unpack("<Q", struct.pack("<d", val))[0] + 1))[0]
    struct.pack_into("<d", buf, warm * 8, nxt)
    with open(dst, "wb") as f:
        f.write(bytes(buf))
    return val, nxt


def read_report_rows(text, names):
    """Map name -> float for the gate's `ACCEPT-REPORT name=value` rows."""
    vals = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == "ACCEPT-REPORT":
            k, _, v = fields[1].partition("=")
            if k in names:
                vals[k] = float(v)
    return vals


def runner_main():
    ap = argparse.ArgumentParser(
        description="GH#19 S6: the baseline pins must refuse an edited baseline, and the edit each "
                    "refuses must move the verdict evidence")
    ap.add_argument("--probe", required=True, help="path to the gh19_s6_saw_probe binary; this "
                    "runner compiles nothing itself, it only feeds the pipeline tampered baselines")
    ap.add_argument("--pipeline", required=True, help="tools/run_gh19_s6_saw_pipeline.py")
    ap.add_argument("--neutral-probe", required=True,
                    help="path to the gh19_s6_saw_probe_neutral binary (arm B of the equality A/B); "
                         "the pipeline requires it, and the raw-pin check below needs the pipeline's "
                         "freshly rendered neutral arm to tamper with")
    ap.add_argument("--repo-root", default=".", help="checkout the gate's wiring check reads")
    ap.add_argument("--baseline-arm", required=True)
    ap.add_argument("--base-report", required=True)
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--gate", required=True)
    ap.add_argument("--shift-worse-db", type=float, default=3.0,
                    help="uniform shift applied to the baseline report's residual columns to make the "
                         "baseline look worse (default 3.0 dB). This is the tamper checked END TO "
                         "END: refused by the pin, and -- with the pin bypassed -- still ACCEPTED by "
                         "the gate while the published improvements grow by that amount")
    ap.add_argument("--shift-better-db", type=float, default=-30.0,
                    help="the same uniform shift the other way (default -30.0 dB): the baseline "
                         "looks better, so every improvement shrinks by that much and the gate's own "
                         "6 dB criterion must reject it (asserted). The point is that one artifact "
                         "carries the verdict both ways while the other two pins stay green")
    ap.add_argument("--out", default=None,
                    help="scratch dir (default: a tempdir, removed on exit)")
    args = ap.parse_args()

    if not os.path.exists(args.base_report):
        sys.stderr.write("INVALID: %s does not exist\n" % args.base_report)
        return 2
    pristine_sha = sha256_of(args.base_report)
    if pristine_sha != BASELINE_REPORT_SHA256:
        sys.stderr.write("INVALID: %s is not the pinned baseline report (pinned %s, actual %s).\n"
                         "The whole point of this runner is to edit the PINNED report; editing a "
                         "report that is already unpinned proves nothing.\n"
                         % (args.base_report, BASELINE_REPORT_SHA256, pristine_sha))
        return 2

    manifest = os.path.join(args.baseline_arm, "gh19_s6_scenarios.tsv")
    if not os.path.exists(manifest) or sha256_of(manifest) != BASELINE_SCENARIOS_SHA256:
        sys.stderr.write("INVALID: the baseline manifest is missing or is not the pinned one.\n"
                         "With the manifest also moved, a refusal could be the manifest pin firing "
                         "and neither the report pin nor the raw pin would be tested.\n")
        return 2
    for name, want in sorted(BASELINE_RAW_SHA256.items()):
        p = os.path.join(args.baseline_arm, name)
        if not os.path.exists(p) or sha256_of(p) != want:
            sys.stderr.write("INVALID: baseline window %s is missing or is not the pinned one\n" % p)
            return 2

    tmp = tempfile.mkdtemp(prefix="gh19s6brpin-") if args.out is None else args.out
    try:
        if args.out is not None:
            os.makedirs(tmp, exist_ok=True)
        problems = []

        # ---- build the tampers ---------------------------------------------------------------
        text = open(args.base_report, encoding="utf-8").read()
        tampers = {}
        for tag, db in (("worse", args.shift_worse_db), ("better", args.shift_better_db)):
            new_text, n_rows, n_blocks = shift_residuals(text, db)
            if n_rows == 0 or n_blocks == 0:
                sys.stderr.write("INVALID: the %s tamper matched no block/row in %s -- the column "
                                 "layout has changed and this fixture no longer edits anything\n"
                                 % (tag, args.base_report))
                return 2
            path = os.path.join(tmp, "base_report_%s.txt" % tag)
            with open_text(path, "w") as f:
                f.write(new_text)
            tampers[tag] = {"path": path, "sha": sha256_of(path), "rows": n_rows, "blocks": n_blocks}
            print("[tamper report-%-6s] %d block(s), %d rows, residual columns shifted %+.1f dB  "
                  "sha256=%s" % (tag, n_blocks, n_rows, db, tampers[tag]["sha"][:16]))
            if tampers[tag]["sha"] == pristine_sha:
                problems.append("the report-%s tamper produced a file identical to the pinned "
                                "report; nothing was edited" % tag)

        # The arm with ONE window moved. Copied wholesale so that "only this window differs" can be
        # asserted over the other seven rather than assumed.
        tampered_arm = os.path.join(tmp, "arm_raw_tampered")
        shutil.copytree(args.baseline_arm, tampered_arm)
        warm, win = window_geometry(manifest, TAMPER_CELL)
        raw_path = os.path.join(tampered_arm, TAMPER_RAW)
        before, after = tamper_window(os.path.join(args.baseline_arm, TAMPER_RAW), raw_path, warm)
        raw_sha = sha256_of(raw_path)
        print("[tamper raw      ] %s frame %d (first in-window; warm=%d win=%d) moved one ulp: "
              "%.17g -> %.17g  sha256=%s"
              % (TAMPER_RAW, warm, warm, win, before, after, raw_sha[:16]))
        if raw_sha == BASELINE_RAW_SHA256[TAMPER_RAW]:
            problems.append("the raw tamper produced a window identical to the pinned one")
        moved_others = [n for n in sorted(BASELINE_RAW_SHA256)
                        if n != TAMPER_RAW
                        and sha256_of(os.path.join(tampered_arm, n)) != BASELINE_RAW_SHA256[n]]
        if moved_others:
            problems.append("the raw tamper moved windows it must not have touched: %s"
                            % ", ".join(moved_others))
        print("[tamper raw      ] the other 7 windows are byte-identical to their pins")

        # The stimulus manifest is NOT touched -- asserted, not assumed.
        if sha256_of(manifest) != BASELINE_SCENARIOS_SHA256:
            problems.append("the baseline manifest changed during the run; this runner must only "
                            "ever edit the report and one raw window")

        # ---- 1. the REPORT pin must refuse, by name, with no verdict --------------------------
        p = subprocess.run(
            [sys.executable, args.pipeline, "--probe", args.probe,
             "--neutral-probe", args.neutral_probe,
             "--repo-root", args.repo_root,
             "--baseline-arm", args.baseline_arm,
             "--base-report", tampers["worse"]["path"],
             "--criteria", args.criteria, "--analyzer", args.analyzer, "--gate", args.gate,
             "--out", os.path.join(tmp, "refused_report")],
            capture_output=True, text=True)
        print("[pin report ] pipeline with the tampered baseline report: rc=%d" % p.returncode)
        if p.returncode != EXIT_REFUSE:
            problems.append("the driver exited rc=%d on the tampered report, not the refusal (%d). "
                            "An edited baseline was judged." % (p.returncode, EXIT_REFUSE))
        if REPORT_REFUSAL_NAME not in p.stderr:
            problems.append("the driver did not name `%s` on stderr; the refusal text was:\n%s"
                            % (REPORT_REFUSAL_NAME, p.stderr[-1500:]))
        if MANIFEST_REFUSAL_TEXT in p.stderr:
            problems.append("the refusal that fired is the baseline MANIFEST pin, not the report "
                            "pin: the report pin would then be untested by this run")
        if VERDICT_MARK in p.stdout:
            problems.append("the driver printed `%s` while refusing: a refusal must issue NO "
                            "judgement, or an unstated baseline gets a verdict with a refusal's "
                            "exit code attached" % VERDICT_MARK)
        for mark in MEASURED_MARKS:
            if mark in p.stdout:
                problems.append("the driver reached `%s` before refusing: the pin is not being "
                                "checked before measurement" % mark)
        if "ACCEPT-PIPELINE baseline_pin=OK" not in p.stdout:
            problems.append("the driver's output does not show the MANIFEST pin passing first, so a "
                            "report-pin refusal cannot be distinguished from an early abort")

        # ---- 2. the RAW pin must refuse, by name, with no verdict, and be the THIRD pin --------
        q = subprocess.run(
            [sys.executable, args.pipeline, "--probe", args.probe,
             "--neutral-probe", args.neutral_probe,
             "--repo-root", args.repo_root,
             "--baseline-arm", tampered_arm,
             "--base-report", args.base_report,
             "--criteria", args.criteria, "--analyzer", args.analyzer, "--gate", args.gate,
             "--out", os.path.join(tmp, "refused_raw")],
            capture_output=True, text=True)
        print("[pin raw    ] pipeline with one baseline window moved: rc=%d" % q.returncode)
        if q.returncode != EXIT_REFUSE:
            problems.append("the driver exited rc=%d on the tampered window, not the refusal (%d). "
                            "An edited baseline was judged." % (q.returncode, EXIT_REFUSE))
        if RAW_REFUSAL_NAME not in q.stderr:
            problems.append("the driver did not name `%s` on stderr; the refusal text was:\n%s"
                            % (RAW_REFUSAL_NAME, q.stderr[-1500:]))
        if RAW_REFUSAL_TEXT not in q.stderr:
            problems.append("`%s` appeared without the raw-pin message body (`%s`); the name alone "
                            "does not show which pin spoke" % (RAW_REFUSAL_NAME, RAW_REFUSAL_TEXT))
        # The two earlier pins must have gone green in THIS run, or a first-pin abort would look
        # exactly like a raw-pin catch.
        for mark in ("ACCEPT-PIPELINE baseline_pin=OK", "ACCEPT-PIPELINE base_report_pin=OK"):
            if mark not in q.stdout:
                problems.append("the raw-pin run does not show `%s` first: with the earlier pins "
                                "unverified, this refusal cannot be attributed to the raw pin" % mark)
        if VERDICT_MARK in q.stdout:
            problems.append("the driver printed `%s` while refusing on the raw pin" % VERDICT_MARK)
        for mark in MEASURED_MARKS:
            if mark in q.stdout:
                problems.append("the raw-pin run reached `%s` before refusing" % mark)

        # ---- 3. the pristine baseline must PASS, and gives the fresh candidate arm -------------
        ref = os.path.join(tmp, "pipeline_pristine")
        r = subprocess.run(
            [sys.executable, args.pipeline, "--probe", args.probe,
             "--neutral-probe", args.neutral_probe,
             "--repo-root", args.repo_root,
             "--baseline-arm", args.baseline_arm,
             "--base-report", args.base_report,
             "--criteria", args.criteria, "--analyzer", args.analyzer, "--gate", args.gate,
             "--out", ref],
            capture_output=True, text=True)
        if r.returncode != EXIT_PASS:
            # A pristine baseline that does not pass makes every "the tamper changed the answer"
            # claim unreadable, so this is a hard stop rather than a problem row.
            sys.stderr.write("INVALID: the pristine baseline did not PASS through the pipeline "
                             "(rc=%d). Without a passing reference there is no answer for the "
                             "tamper to change.\n--- stdout ---\n%s\n--- stderr ---\n%s\n"
                             % (r.returncode, r.stdout[-4000:], r.stderr[-4000:]))
            return 2
        print("[reference  ] pristine baseline through the pipeline: rc=0 (PASS)")
        cand_arm = os.path.join(ref, "cand")
        cand_report = os.path.join(ref, "cand_report.txt")
        cand_plan = os.path.join(cand_arm, "gh19_s6_plan.tsv")
        # The pipeline rendered arm B fresh (it is handed --neutral-probe and does so on every run),
        # and the equality criterion reads those windows. Section 5 below tampers ONE of them.
        ref_neutral_arm = os.path.join(ref, "neutral")
        if not os.path.exists(os.path.join(ref_neutral_arm, TAMPER_RAW)):
            sys.stderr.write("INVALID: the pipeline's reference run left no neutral arm to tamper "
                             "with at %s; the equality criterion's own input cannot be exercised\n"
                             % ref_neutral_arm)
            return 2
        ref_rows = read_report_rows(r.stdout, JUDGED_IMPROVEMENT_REPORTS)
        for name in JUDGED_IMPROVEMENT_REPORTS:
            if name not in ref_rows:
                problems.append("the gate's own PASS output does not carry `ACCEPT-REPORT %s=`, so "
                                "the tamper movement below cannot be measured" % name)

        def gate(base_report, base_arm, outdir, neutral_arm=None):
            os.makedirs(outdir, exist_ok=True)
            return subprocess.run(
                [sys.executable, args.gate, "--criteria", args.criteria, "--plan", cand_plan,
                 "--base-report", base_report, "--cand-report", cand_report,
                 "--base-arm", base_arm, "--cand-arm", cand_arm,
                 "--neutral-arm", neutral_arm or ref_neutral_arm,
                 "--repo-root", args.repo_root],
                capture_output=True, text=True)

        # ---- 4. the refused REPORT edit must be numerically load-bearing -----------------------
        for tag in ("worse", "better"):
            g = gate(tampers[tag]["path"], args.baseline_arm, os.path.join(tmp, "bypass_report_" + tag))
            verdicts = [v for v in ("PASS", "RED", "REFUSE") if VERDICT_MARK + v in g.stdout]
            rows = read_report_rows(g.stdout, JUDGED_IMPROVEMENT_REPORTS)
            if tag == "worse":
                moved = min(abs(rows[n] - ref_rows[n]) for n in ref_rows) if rows else 0.0
                print("[bypass report-worse ] gate on the tampered baseline: rc=%d verdict=%s; the "
                      "same edit, pin bypassed, moved the judged improvement columns by %.2f dB"
                      % (g.returncode, ",".join(verdicts) or "?", moved))
                if g.returncode != EXIT_PASS or verdicts != ["PASS"]:
                    problems.append("with the pin bypassed, the `worse` tamper did not still pass "
                                    "(rc=%d, verdict=%s). The claim this runner makes is that such "
                                    "an edit is INVISIBLE to the gate, so it must not be caught by "
                                    "anything else -- if another check does catch it, the report "
                                    "pin's value must be restated in those terms."
                                    % (g.returncode, ",".join(verdicts) or "none"))
                elif moved <= 0.0:
                    problems.append("the `worse` tamper left every judged improvement column "
                                    "bit-identical. The gate does not read the report's residual "
                                    "columns, so pinning them is decorative -- which is a finding "
                                    "about the PIN, not about the tamper")
                elif abs(moved - args.shift_worse_db) > 0.05:
                    problems.append("the `worse` tamper moved the judged improvement columns by "
                                    "%.3f dB where a uniform %+.1f dB shift of the baseline's "
                                    "residuals must move them by exactly that much; a partial "
                                    "movement means the columns are not read the way this fixture "
                                    "assumes" % (moved, args.shift_worse_db))
            else:
                print("[bypass report-better] gate on the tampered baseline: rc=%d verdict=%s"
                      % (g.returncode, ",".join(verdicts) or "?"))
                if g.returncode != EXIT_RED or verdicts != ["RED"]:
                    problems.append("with the pin bypassed, the `better` tamper (%+.1f dB on the "
                                    "baseline's residual columns) was not rejected (rc=%d, "
                                    "verdict=%s). Every gate cell improves by %.1f dB less than "
                                    "published, which is far below the 6 dB the criterion asks "
                                    "for; a PASS here would mean the improvement columns are not "
                                    "read from this report at all."
                                    % (args.shift_better_db, g.returncode,
                                       ",".join(verdicts) or "none", -args.shift_better_db))

        # ---- 5. the raw pin is PROVENANCE, and the equality criterion's live input is arm B -----
        # This section used to assert the opposite of what it asserts now, and the change is a
        # finding rather than a convenience. The equality criterion once read the committed baseline
        # windows, so a moved window fired it and the raw pin was numerically load-bearing. The
        # criterion is now a same-build A/B (the committed snapshot was rendered by a different libm
        # and cannot be reproduced bit-for-bit; see the task report), so NO judged criterion reads
        # these bytes any more. Asserting the old claim would be asserting something false, and
        # deleting the check would leave the demotion untested -- so the check is INVERTED: the moved
        # baseline window must be INERT to the verdict, and the pin that now guards the criterion's
        # real input is exercised separately, on the freshly rendered arm B below.
        g = gate(args.base_report, tampered_arm, os.path.join(tmp, "bypass_raw"))
        verdicts = [v for v in ("PASS", "RED", "REFUSE") if VERDICT_MARK + v in g.stdout]
        rows = read_report_rows(g.stdout, JUDGED_IMPROVEMENT_REPORTS)
        print("[bypass raw        ] gate with the pinned report but one BASELINE window moved: "
              "rc=%d verdict=%s" % (g.returncode, ",".join(verdicts) or "?"))
        if g.returncode != EXIT_PASS or verdicts != ["PASS"]:
            problems.append("with the report pin green and one baseline window moved, the gate did "
                            "not PASS (rc=%d, verdict=%s). Since the equality criterion became a "
                            "same-build A/B, no judged criterion reads the committed windows, so a "
                            "verdict change here would mean some criterion still does -- and the "
                            "raw pin's classification in the pipeline header (PROVENANCE, not a "
                            "judged basis) would be wrong." % (g.returncode,
                                                               ",".join(verdicts) or "none"))
        if "FAIL-EQUALITY-SINE" in g.stdout or "FAIL-EQUALITY-ANTIPHASE" in g.stdout:
            problems.append("moving a committed baseline window fired an equality criterion; those "
                            "criteria are supposed to read the candidate arm and arm B, not the "
                            "committed artifact. Stderr was:\n%s" % g.stderr[-1500:])
        else:
            print("[bypass raw        ] no equality criterion fired, which is the claim: no judged "
                  "criterion reads the committed windows any more")
        # The improvement columns must NOT have moved either: the improvement criteria read the
        # REPORT, which is untouched here.
        if rows and ref_rows and any(abs(rows[n] - ref_rows[n]) > 1e-9 for n in ref_rows):
            problems.append("moving a baseline WINDOW also moved the judged improvement columns; "
                            "the improvement criteria read the report, so a movement here means "
                            "the two pins are not separating the surfaces this runner claims")

        # ---- 5b. the equality criterion's REAL input: arm B, rendered by this run ---------------
        # One frame of one window in the neutral arm, moved by one ulp. The criterion must fire,
        # naming EQUALITY-SINE, with the improvement columns still untouched -- the same shape of
        # assertion section 5 used to make about the committed artifact, aimed at the input the
        # criterion actually reads now. Its manifest comes with the copied directory, so the window
        # geometry the criterion reads it through is arm B's own.
        neutral_tampered = os.path.join(tmp, "neutral_tampered")
        shutil.copytree(ref_neutral_arm, neutral_tampered)
        # The frame moved must be inside the window the criterion reads. `warm` comes from the
        # baseline manifest; arm B's own manifest is consulted and must agree, rather than assumed to
        # (the plan is byte-identical by the gate's NEUTRAL-ARM refusal, but a geometry that differs
        # per arm would silently move the edit outside the compared window).
        b_warm, b_win = window_geometry(os.path.join(ref_neutral_arm, "gh19_s6_scenarios.tsv"),
                                        TAMPER_CELL)
        if (b_warm, b_win) != (warm, win):
            problems.append("arm B's window geometry for %s is (warm=%d, win=%d), the baseline "
                            "manifest says (warm=%d, win=%d); the ulp edit below would be made at a "
                            "different offset than the one section 5 uses"
                            % (TAMPER_CELL, b_warm, b_win, warm, win))
        nt_path = os.path.join(neutral_tampered, TAMPER_RAW)
        b_before, b_after = tamper_window(os.path.join(ref_neutral_arm, TAMPER_RAW), nt_path, warm)
        print("[bypass neutral    ] arm B's %s frame %d moved one ulp: %.17g -> %.17g"
              % (TAMPER_RAW, warm, b_before, b_after))
        if b_before == b_after:
            problems.append("the neutral-arm tamper moved nothing (%.17g -> %.17g), so the check "
                            "below would be measuring an unedited arm" % (b_before, b_after))
        else:
            gn = gate(args.base_report, args.baseline_arm, os.path.join(tmp, "bypass_neutral"),
                      neutral_arm=neutral_tampered)
            nv = [v for v in ("PASS", "RED", "REFUSE") if VERDICT_MARK + v in gn.stdout]
            nrows = read_report_rows(gn.stdout, JUDGED_IMPROVEMENT_REPORTS)
            print("[bypass neutral    ] gate with one arm-B window moved: rc=%d verdict=%s"
                  % (gn.returncode, ",".join(nv) or "?"))
            if gn.returncode != EXIT_RED or nv != ["RED"]:
                problems.append("with one ulp moved in ONE window of arm B, the gate did not go RED "
                                "(rc=%d, verdict=%s). `sine_node_bit_diff_count` compares the "
                                "candidate's window against arm B's frame by frame; if an edit to "
                                "arm B does not fire it, the criterion's input is not what the "
                                "criteria file says it is."
                                % (gn.returncode, ",".join(nv) or "none"))
            elif "FAIL-EQUALITY-SINE" not in gn.stdout:
                problems.append("the arm-B tamper went RED without naming `FAIL-EQUALITY-SINE`; "
                                "the failure surface must be the equality criterion. Stderr was:"
                                "\n%s" % gn.stderr[-1500:])
            elif nrows and ref_rows and any(abs(nrows[n] - ref_rows[n]) > 1e-9 for n in ref_rows):
                problems.append("moving an arm-B window also moved the judged improvement columns; "
                                "the improvement criteria read the report, and arm B is not part "
                                "of them, so the two layers are not separating as claimed")
            else:
                print("[bypass neutral    ] the failure that fired is FAIL-EQUALITY-SINE, with the "
                      "improvement columns untouched")

        if problems:
            print("\nBASELINE-PIN FAILED:")
            for prob in problems:
                print("  - %s" % prob)
            return 1
        print("\nBASELINE-PIN PASS: an edited baseline report is refused by name before any "
              "measurement and issues no verdict, while the manifest pin reports OK first so the "
              "two surfaces are distinguishable; and the edit that was refused is not inert. With "
              "the pin bypassed, the same well-formed report -- the other pins still green -- is "
              "ACCEPTED when the baseline is made to look worse (and publishes %+.1f dB more "
              "improvement than it earned) and is REJECTED by the gate's own 6 dB criterion when "
              "the baseline is made to look better. The raw pin is refused by name as the THIRD pin, "
              "after both earlier pins report OK -- and, since the equality criterion became a "
              "same-build A/B, moving a committed baseline window is INERT to the verdict, which is "
              "asserted rather than glossed: that criterion is now fed by the freshly rendered arm "
              "B, and one ulp moved in one of ITS windows fires FAIL-EQUALITY-SINE with the "
              "improvement columns untouched. The raw pin therefore guards committed evidence, and "
              "the arm-B half guards the judged input; each pin removes a state the gate "
              "demonstrably accepts, and each protects a different one." % args.shift_worse_db)
        return 0
    finally:
        if args.out is None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(runner_main())
