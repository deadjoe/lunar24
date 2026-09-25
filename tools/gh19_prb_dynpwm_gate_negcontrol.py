#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# GH#19 S6 PR-B (task #121) LEG 2: the acceptance gate's OWN negative controls.
#
# WHY THIS FILE EXISTS SEPARATELY FROM THE GATE. A gate that returns PASS on the delivered artifacts is
# evidence only if it can be made to refuse. "The gate passed" and "the gate cannot fail" are
# indistinguishable from a PASS alone, so every refusal the criteria name is given a control here: the
# input is mutated in exactly ONE way, and the control requires that the gate exits with the PINNED
# code -- not merely that it exits non-zero. A mutation that trips a DIFFERENT code is a control
# failure: the gate would then be refusing for a reason other than the one it reports, which is the
# failure mode this whole file exists to make visible.
#
# THE CONTROLS ARE ON THE GATE, NOT ON THE DSP. Nothing here touches core/, host/ or tests/probes/.
# The starting artifacts are real analyzer/arm output, and every control perturbs a copy of one text
# file or one rendered window in place and then RESTORES it. The source-level controls (the shadow
# include arm, the one-ulp tamper run as a pipeline) live in the mutant runner, because they have to be
# compiled or driven to be observed at all.
#
# *** THE TAMPERS ARE RESTORED, AND THE RESTORATION IS VERIFIED, NOT ASSUMED. *** Every in-place write
# first snapshots the original bytes, and after each control every touched path is rewritten from that
# snapshot and re-hashed against it. A restore that did not take would silently contaminate every
# later control with an earlier mutation -- the controls would then be measuring a pile of edits rather
# than one edit each, and they would still print RED-HIT. The verification is what makes that
# impossible rather than unlikely.
#
# THE POSITIVE CONTROL RUNS FIRST. The unmutated artifacts must PASS. Without it, "the mutation was
# caught" could just mean "this gate refuses everything", which is the one reading a negative-control
# suite must never allow.
#
# Usage (this is how the CTest entry calls it, via tools/run_gh19_prb_dynpwm_pipeline.py):
#   tools/gh19_prb_dynpwm_gate_negcontrol.py \
#       --criteria report/gh19-prb-dynpwm/acceptance_criteria.tsv \
#       --plan <arm>/gh19_s3_plan.tsv --cand-report <cand_report.txt> --cand-arm <arm> \
#       --neut-report <neut_report.txt> --neut-arm <arm> \
#       --degen-static-arm <arm> --degen-dyn-arm <arm>
import argparse
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "gh19_prb_dynpwm_acceptance.py")

EXIT_PASS = 0
EXIT_RED = 1
EXIT_REFUSE = 4

# The analyzer's own line shape, reused so a control can find the exit-code line it mutates rather
# than matching a substring that could occur elsewhere.
EXIT_LINE = "exit_code="


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def read_text(path):
    """Text in with the codec pinned. Not an optional detail: Python's text-mode default is the LOCALE
    codec (cp1252 on a Windows runner), and a control file read differently on one platform would make
    this suite's verdict a function of the runner's locale."""
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def first_difference(want, got):
    """Where two texts first differ, as `line N: want ... got ...`. For a self-check whose whole job is
    to say what went wrong: "the cache was not rolled back" is only actionable once it says which line
    the cache is stale at, and a first-80-characters excerpt of the two heads is not (they are equal
    for any mutation that is not on line 1, which is most of them)."""
    for i, (a, b) in enumerate(zip(want.splitlines(True), got.splitlines(True)), 1):
        if a != b:
            return "line %d: want %r got %r" % (i, a[:70], b[:70])
    if len(want) != len(got):
        return "one text is a prefix of the other (want %d bytes, got %d)" % (len(want), len(got))
    return "no difference found"


def find_line(text, pred):
    """Index of the first line satisfying `pred`, or None."""
    for i, ln in enumerate(text.splitlines(True)):
        if pred(ln):
            return i
    return None


class Sandbox(object):
    """Holds the mutable text inputs and performs RESTORABLE in-place writes on the arm trees.

    Every write snapshots the original bytes the first time a path is touched, and refuses a write
    that changes nothing: a control whose mutation is a no-op would report a RED-HIT while measuring
    the gate's behaviour on unmutated input, which is the single most convincing way for a control
    suite to be worthless.
    """

    def __init__(self, text_paths):
        self.text = {}
        self.paths = {}
        for key, p in text_paths.items():
            self.text[key] = read_text(p)
            self.paths[key] = p
        # The pre-mutation text, kept because `self.text` is not a pristine cache: `text_file()` sets it
        # to what it just wrote, so after a mutation the cache correctly mirrors the MUTATED file. The
        # snapshot is therefore the only record of what `restore()` must put back, and the rollback is
        # checked against it rather than against the file.
        self.text0 = dict(self.text)
        self.undo = {}

    def write(self, path, blob):
        if isinstance(blob, str):
            blob = blob.encode("utf-8")
        before = self.undo.get(path)
        if before is None:
            before = read_bytes(path)
            self.undo[path] = before
        if blob == before:
            raise SystemExit("control setup: the mutation left %s unchanged" % path)
        # Binary write, and no newline translation: these are rendered windows and reports whose bytes
        # are the measurement, so the write must be a function of the value alone.
        with open(path, "wb") as fh:
            fh.write(blob)

    def text_file(self, key, text):
        """A whole-file rewrite of one of the text inputs, with the newline pinned so the bytes depend
        on the content and not on the host that produced them."""
        p = self.paths[key]
        self.write(p, text.encode("utf-8"))
        self.text[key] = text

    def restore(self):
        """Put every input back, INCLUDING the in-memory text, and verify both rather than trust them.

        The file is not the only copy of these inputs. `text_file()` writes the file AND updates
        `self.text`, and the mutations read `self.text` -- so restoring only the file left the cache
        holding the mutated text, and every later case built its edit on the previous case's change.
        Each control then mutated several things while its docstring promised `changes EXACTLY ONE
        thing`, and the suite's results were silently about a different experiment than the one its
        names describe. Found 2026-09-25 by running it: a one-line `criteria_unknown_key` mutation
        printed THREE accumulated refusals, two of which belonged to earlier cases.
        """
        problems = []
        for p, before in self.undo.items():
            with open(p, "wb") as fh:
                fh.write(before)
            after = read_bytes(p)
            if after != before:
                problems.append(p)
        # The cache, and the self-check the director approved. Roll the text back FROM THE RESTORED
        # FILE, so the two copies cannot end up disagreeing about what "restored" means -- then check
        # the result against the pre-mutation snapshot, because that is the part a future edit could
        # break silently: delete the rollback line and the cache keeps the mutated text, the file looks
        # pristine, and the next control builds its edit on the previous control's change with nothing
        # in this file reporting it. The check is therefore about the state `restore()` leaves behind,
        # not about whether the file write succeeded (the loop above already checks that directly).
        #
        # Comparing the cache to the restored FILE instead -- the obvious reading of "assert the cache
        # equals the file bytes" -- is what this check was first written as, and it fired on every
        # control: the cache legitimately holds the mutated text until it is rolled back, so the two
        # copies disagree until this line runs. A check that fires on the correct state is worse than no
        # check, because it gets switched off.
        for key, p in self.paths.items():
            if p not in self.undo:
                continue
            self.text[key] = read_text(p)
            if self.text[key] != self.text0[key]:
                problems.append("%s (the text cache was not rolled back: %s)"
                                % (p, first_difference(self.text0[key], self.text[key])))
        self.undo = {}
        return problems

    def touched(self):
        return sorted(self.undo)


# ---------------------------------------------------------------------------------------------
# Reading the pinned contract. The victims are taken FROM the criteria rather than hard-coded, so a
# change in the cell set cannot leave a control quietly aiming at a cell that no longer exists --
# it would then mutate nothing meaningful and the "changed nothing" guard above would never fire.
# ---------------------------------------------------------------------------------------------
def criteria_victims(text):
    """(first cell id, first degen pair) from the criteria file. Raises if either is absent."""
    cell = None
    for ln in text.splitlines():
        f = ln.split("\t")
        if f[0] == "cell" and len(f) >= 2 and cell is None:
            cell = f[1]
        if f[0] == "degen_pair" and len(f) >= 4:
            return cell, (f[1], f[2], f[3])
    raise SystemExit("control setup: the criteria name no degen_pair row (the identity controls need one)")


def scenario_raw_path(arm, table_name, cell):
    """The render a cell's scenario row names, resolved exactly as the gate resolves it -- through the
    table's own `raw` column. Resolving it any other way (a guessed filename) would make this control
    tamper a file the gate does not read, and it would then report a MISS that is really a control bug.
    """
    path = os.path.join(arm, table_name)
    text = read_text(path)
    lines = [l for l in text.splitlines() if l.strip() and not l.lstrip().startswith("#")]
    header = lines[0].split("\t")
    try:
        id_col, raw_col = header.index("id"), header.index("raw")
    except ValueError:
        raise SystemExit("control setup: %s has no id/raw column" % path)
    for ln in lines[1:]:
        f = ln.split("\t")
        if len(f) > max(id_col, raw_col) and f[id_col] == cell:
            if not f[raw_col] or f[raw_col] == "-":
                raise SystemExit("control setup: %s names no render for %s" % (path, cell))
            return os.path.join(arm, f[raw_col])
    raise SystemExit("control setup: %s has no row for %s" % (path, cell))


# ---------------------------------------------------------------------------------------------
# Mutations. Each takes the sandbox and the resolved arm paths and changes EXACTLY ONE thing.
# ---------------------------------------------------------------------------------------------
def m_pristine(sb, arms):
    """The positive control changes nothing, and does so by construction rather than by omission."""
    return


def m_criteria_drop_criterion(sb, arms):
    t = sb.text["criteria"]
    out = [l for l in t.splitlines(True)
           if not l.startswith("criterion\tfloor_margin_min_db\t")]
    if len(out) == len(t.splitlines(True)):
        raise SystemExit("control setup: no floor_margin_min_db row to drop")
    sb.text_file("criteria", "".join(out))


def m_criteria_drop_negcontrol_row(sb, arms):
    """The falsifiability control row removed. The gate must REFUSE rather than run with no control
    named -- a criteria file that can lose its negative control silently is a criteria file whose
    controls are decorative."""
    t = sb.text["criteria"]
    out = [l for l in t.splitlines(True)
           if not l.startswith("criterion\tnegcontrol_degen_identity_tamper\t")]
    if len(out) == len(t.splitlines(True)):
        raise SystemExit("control setup: no negcontrol_degen_identity_tamper row to drop")
    sb.text_file("criteria", "".join(out))


def m_criteria_unknown_key(sb, arms):
    """A criterion the authorization never set. An unknown key must be refused, not ignored: an
    ignored threshold is a threshold that silently does not apply."""
    sb.text_file("criteria", sb.text["criteria"] +
                 "criterion\timprovement_min_db_invented\t99.0\n")


def m_criteria_drop_degen_pair(sb, arms):
    """One pair row gone while `axis degen_pair_count` still claims six."""
    t = sb.text["criteria"]
    out = [l for l in t.splitlines(True) if not l.startswith("degen_pair\t")]
    if len(out) >= len(t.splitlines(True)) - 5:
        raise SystemExit("control setup: the criteria do not carry six degen_pair rows")
    sb.text_file("criteria", "".join(out))


# The value a renamed code must print INSTEAD of its default. Not a word this slice uses anywhere: no
# criteria key, cell id, metric name or verdict in the transcript contains it, so if it appears in the
# gate's output it can only have got there from the criteria being read.
SENTINEL_CODE = "LIVENESS-SENTINEL-7Q4Z"


def m_criteria_code_key_is_read(sb, arms):
    """THE LIVENESS HALF of the code wiring's acceptance. The identity half (`output unchanged`) is
    satisfied by a DEAD read just as well as by a live one -- a gate that wires the criteria into a map
    and never consults it prints exactly the same bytes -- so identity alone proves the change is safe
    and proves nothing about whether the vocabulary is actually the criteria's.

    This control renames `code_cellset` to a sentinel and, in the same edit, drops the degen_pair rows,
    which is what forces the gate to EMIT a CELLSET refusal. BOTH halves are needed: without the forcing
    edit the pristine run passes and prints no code at all, so a rename would be a control that cannot
    fail. The default literal is listed as forbidden, so a gate that ignores the criteria and prints
    `PRB-REFUSE-CELLSET` MISSes here rather than passing on a coincidence of strings.
    """
    t = sb.text["criteria"]
    lines = t.splitlines(True)
    out, renamed = [], 0
    for l in lines:
        if l.startswith("criterion\tcode_cellset\t"):
            out.append("criterion\tcode_cellset\t%s\n" % SENTINEL_CODE)
            renamed += 1
        else:
            out.append(l)
    if renamed != 1:
        raise SystemExit("control setup: expected exactly one code_cellset row, found %d" % renamed)
    kept = [l for l in out if not l.startswith("degen_pair\t")]
    if len(kept) >= len(out) - 5:
        raise SystemExit("control setup: the criteria do not carry six degen_pair rows")
    sb.text_file("criteria", "".join(kept))


def m_plan_truncated_row(sb, arms):
    """A plan row with one field: not a (block, id) pair, so it is not a declaration."""
    ls = sb.text["plan"].splitlines(True)
    i = find_line(sb.text["plan"], lambda l: l.startswith("dynamic\t"))
    if i is None:
        raise SystemExit("control setup: no dynamic row in the plan")
    ls[i] = ls[i].split("\t")[0] + "\n"
    sb.text_file("plan", "".join(ls))


def m_plan_drops_dynamic_cell(sb, arms):
    """A cell the criteria enumerate, absent from the probe's own declared plan. The criterion set must
    not be satisfiable by a probe that quietly produced fewer cells."""
    t = sb.text["plan"]
    i = find_line(t, lambda l: l.startswith("dynamic\t"))
    if i is None:
        raise SystemExit("control setup: no dynamic row in the plan")
    ls = t.splitlines(True)
    del ls[i]
    sb.text_file("plan", "".join(ls))


def m_report_exit_code_nonzero(sb, arms):
    """The analyzer's own exit line, made non-zero: its checks did not all pass, so nothing it printed
    is entitled to a verdict."""
    t = sb.text["cand_report"]
    i = find_line(t, lambda l: l.startswith("== ") and EXIT_LINE in l)
    if i is None:
        raise SystemExit("control setup: no analyzer exit line in the candidate report")
    ls = t.splitlines(True)
    ls[i] = ls[i].replace("exit_code=0", "exit_code=1")
    sb.text_file("cand_report", "".join(ls))


def m_report_column_dropped(sb, arms):
    """One column removed from the DYNAMIC header only, so every dynamic data row is now wider than
    its own header: the matrix is not the pinned shape and cannot be read column-wise at all.

    AIMED AT THE FENCE, and that is the whole of an earlier revision's defect. It used to take the
    first line matching `id ... res_db ... fund`, which the report also satisfies in its STATIC and
    MIXED matrix blocks -- so it rewrote the static header while claiming the dynamic one, the gate
    (which parses only the dynamic block) never looked at the edited line, and the control reported
    `MISS` with the gate exiting 0. A control that edits a surface the gate does not read is a control
    whose expectation is not about the gate at all. Locating the fence first is the same shape as
    `m_report_cell_row_dropped` uses, and for the same reason.
    """
    t = sb.text["cand_report"]
    ls = t.splitlines(True)
    begin = None
    for i, ln in enumerate(ls):
        if ln.startswith("-- BEGIN MATRIX dynamic"):
            begin = i
            break
    if begin is None:
        raise SystemExit("control setup: no dynamic matrix fence in the candidate report")
    end = next((i for i in range(begin + 1, len(ls))
                if ls[i].startswith("-- END MATRIX dynamic")), len(ls))
    for i in range(begin + 1, end):
        f = ls[i].split()
        if f[:1] == ["id"]:
            if len(f) < 4:
                raise SystemExit("control setup: the dynamic header is too short to drop a column from")
            del f[3]
            ls[i] = "  " + " ".join(f) + "\n"
            sb.text_file("cand_report", "".join(ls))
            return
    raise SystemExit("control setup: no dynamic column header in the candidate report")


def m_report_cell_row_dropped(sb, arms):
    """One dynamic data row gone while the fence still declares it emitted."""
    t = sb.text["cand_report"]
    lines = t.splitlines(True)
    begin = end = None
    for i, ln in enumerate(lines):
        if ln.startswith("-- BEGIN MATRIX dynamic"):
            begin = i
        elif ln.startswith("-- END MATRIX dynamic"):
            end = i
    if begin is None or end is None:
        raise SystemExit("control setup: no dynamic matrix fence in the candidate report")
    # The first data row is the one whose cells have DYNAMIC_COLS fields.
    for i in range(begin + 1, end):
        if lines[i].split() and lines[i].split()[0].startswith("vco_"):
            del lines[i]
            sb.text_file("cand_report", "".join(lines))
            return
    raise SystemExit("control setup: no dynamic data row to drop")


def m_report_cell_id_renamed(sb, arms):
    """An id that no longer decodes to the axes that claim it. The id IS the claim, so an
    undecodable one cannot be accepted as the cell the criteria name."""
    t = sb.text["cand_report"]
    lines = t.splitlines(True)
    begin = end = None
    for i, ln in enumerate(lines):
        if ln.startswith("-- BEGIN MATRIX dynamic"):
            begin = i
        elif ln.startswith("-- END MATRIX dynamic"):
            end = i
    if begin is None or end is None:
        raise SystemExit("control setup: no dynamic matrix fence in the candidate report")
    for i in range(begin + 1, end):
        f = lines[i].split()
        if f and f[0].startswith("vco_"):
            f[0] = f[0] + "_x"
            lines[i] = " ".join(f) + "\n"
            sb.text_file("cand_report", "".join(lines))
            return
    raise SystemExit("control setup: no dynamic data row to rename")


def m_trace_truncated(sb, arms):
    """A duty trace cut short of the analysis window. The window is pinned by the criteria, so a trace
    that cannot fill it is a missing measurement, not a short one."""
    cell = arms["victim_cell"]
    # The trace sits beside the render, and its name comes from the same scenario row's `src` column.
    table = read_text(os.path.join(arms["cand_arm"], "gh19_s3_scenarios.tsv"))
    lines = [l for l in table.splitlines() if l.strip() and not l.lstrip().startswith("#")]
    header = lines[0].split("\t")
    src_col = header.index("src")
    for ln in lines[1:]:
        f = ln.split("\t")
        if f[0] == cell:
            src = os.path.join(arms["cand_arm"], f[src_col])
            break
    else:
        raise SystemExit("control setup: no scenario row for %s" % cell)
    body = read_text(src)
    ls = body.splitlines(True)
    if len(ls) < 10:
        raise SystemExit("control setup: %s is too short to truncate" % src)
    sb.write(src, "".join(ls[: len(ls) // 2]))


def m_scenario_row_dropped(sb, arms):
    """A cell absent from the renderer's table. Its render can then never be resolved, so the identity
    has nothing to compare and must not be reported as agreement."""
    p = os.path.join(arms["cand_arm"], "gh19_s3_scenarios.tsv")
    body = read_text(p)
    ls = body.splitlines(True)
    for i, ln in enumerate(ls):
        if ln.startswith(arms["victim_cell"] + "\t"):
            del ls[i]
            sb.write(p, "".join(ls))
            return
    raise SystemExit("control setup: no scenario row for %s" % arms["victim_cell"])


def m_degen_identity_one_ulp(sb, arms):
    """THE IDENTITY'S FALSIFIABILITY CONTROL, and the reason it is a TAMPER rather than a product
    mutation. At pwDepth == 0 the product's duty is `duty_ + 0.0 * cv` == `duty_` exactly, so the CV
    value is irrelevant BY ARITHMETIC: no CV-side or depth-side mutation can break this identity while
    leaving its antecedent (a constant duty) standing. The narrowest possible difference is therefore
    injected into the INSTRUMENT's input instead -- the low bit of the constructed cell's last sample,
    i.e. one ulp -- which is the S6 `gh19_s6_saw_baseline_report_pin` precedent. If a byte-level
    identity cannot be broken by one ulp, it is not being checked at all.
    """
    p = scenario_raw_path(arms["degen_dyn_arm"], "gh19_prb_degen_scenarios.tsv", arms["const_cell"])
    blob = bytearray(read_bytes(p))
    if len(blob) < 8:
        raise SystemExit("control setup: %s is too short to hold a sample" % p)
    blob[-8] ^= 0x01          # the low byte of the final little-endian f64
    sb.write(p, bytes(blob))


def m_degen_nonvacuous_collapse(sb, arms):
    """The non-vacuity control's own falsifiability: the moving-duty render is replaced by the
    constant-duty one, so the SAME comparison that must differ now agrees. The identity itself must
    still pass (the constant render is untouched) and the non-vacuity leg must fire alone -- which is
    also what proves the two surfaces are independent rather than one check reported twice.
    """
    src = scenario_raw_path(arms["degen_dyn_arm"], "gh19_prb_degen_scenarios.tsv", arms["const_cell"])
    dst = scenario_raw_path(arms["degen_dyn_arm"], "gh19_prb_degen_scenarios.tsv", arms["moving_cell"])
    sb.write(dst, read_bytes(src))


# (name, mutation, expected exit, expected code in the output, codes that must NOT appear, what it is for)
CONTROLS = (
    ("criteria_drop_criterion", m_criteria_drop_criterion, EXIT_REFUSE, "PRB-REFUSE-CRITERIA", (),
     "a missing criterion must not become a criterion that passed"),
    ("criteria_drop_negcontrol_row", m_criteria_drop_negcontrol_row, EXIT_REFUSE, "PRB-REFUSE-CRITERIA", (),
     "the falsifiability control cannot be deleted from the criteria without the gate noticing"),
    ("criteria_unknown_key", m_criteria_unknown_key, EXIT_REFUSE, "PRB-REFUSE-CRITERIA", (),
     "a threshold the authorization never set must be refused, not ignored"),
    ("criteria_drop_degen_pair", m_criteria_drop_degen_pair, EXIT_REFUSE, "PRB-REFUSE-CELLSET", (),
     "pairs deleted while the axis still claims six must not silently shrink the identity's scope",
     False),
    # The liveness half of the code-wiring acceptance. It forces a CELLSET refusal (by the same edit
    # that drops the pairs) while a sandboxed criteria renames that code, and requires the SENTINEL on
    # both printed surfaces. `need_surfaces` is True because renaming is evidence that the criteria was
    # read only if the detail line and the counts line move together -- see `finish` and `_report`.
    ("criteria_code_key_is_read", m_criteria_code_key_is_read, EXIT_REFUSE,
     "PRB-REFUSE-" + SENTINEL_CODE, ("PRB-REFUSE-CELLSET",),
     "the code vocabulary is the criteria's: a renamed code must print the criteria's string, on both "
     "the detail line and the counts line", True),
    ("plan_truncated_row", m_plan_truncated_row, EXIT_REFUSE, "PRB-REFUSE-PLAN", (),
     "a plan row that is not a (block, id) pair is not a declaration"),
    ("plan_drops_dynamic_cell", m_plan_drops_dynamic_cell, EXIT_REFUSE, "PRB-REFUSE-CELLSET", (),
     "the criteria set must not be satisfiable by a probe that produced fewer cells"),
    ("report_exit_code_nonzero", m_report_exit_code_nonzero, EXIT_REFUSE, "PRB-REFUSE-MATRIX-EXIT", (),
     "an analyzer whose own checks failed is not entitled to a verdict"),
    ("report_column_dropped", m_report_column_dropped, EXIT_REFUSE, "PRB-REFUSE-MATRIX-SHAPE", (),
     "a matrix that is not the pinned shape cannot be read column-wise at all"),
    ("report_cell_row_dropped", m_report_cell_row_dropped, EXIT_REFUSE, "PRB-REFUSE-MATRIX-SHAPE", (),
     "a cell the fence declares emitted but which is not in the matrix was never measured"),
    ("report_cell_id_renamed", m_report_cell_id_renamed, EXIT_REFUSE, "PRB-REFUSE-CELLSET", (),
     "an id that no longer decodes is not the cell the criteria name"),
    ("trace_truncated", m_trace_truncated, EXIT_REFUSE, "PRB-REFUSE-TRACE", (),
     "a trace that cannot fill the pinned window is a missing measurement"),
    ("scenario_row_dropped", m_scenario_row_dropped, EXIT_REFUSE, "PRB-REFUSE-CELLSET", (),
     "a cell absent from the renderer's table has no render to compare"),
    # The two judgement-bucket controls. These are the ones that show the gate can issue a NAMED RED,
    # which a refusal-only suite never demonstrates.
    ("degen_identity_one_ulp", m_degen_identity_one_ulp, EXIT_RED, "PRB-FAIL-DEGEN-IDENTITY",
     ("PRB-FAIL-DEGEN-NONVACUOUS",),
     "one ulp in the constructed render must break a BYTE-IDENTICAL identity, and only that leg"),
    ("degen_nonvacuous_collapse", m_degen_nonvacuous_collapse, EXIT_RED, "PRB-FAIL-DEGEN-NONVACUOUS",
     ("PRB-FAIL-DEGEN-IDENTITY",),
     "a moving-duty render made identical to the static one is the vacuity the identity must not survive"),
)

# The quantities the identity falsification must leave ALONE. The criterion's own wording is that the
# tamper must not move the improvement columns; asserting it mechanically is the only way that clause
# is a check rather than a promise.
FROZEN_REPORTS = ("improvement_corrected_db", "improvement_neutralized_db",
                  "max_abs_fitted_scale_delta")


def report_values(out):
    vals = {}
    for ln in out.splitlines():
        f = ln.split()
        for i, tok in enumerate(f):
            if tok == "REPORT" and i + 1 < len(f) and "=" in f[i + 1]:
                k, v = f[i + 1].split("=", 1)
                vals[k] = v
    return vals


def run_gate(args, sb):
    cmd = [sys.executable, args.gate,
           "--criteria", sb.paths["criteria"],
           "--plan", sb.paths["plan"],
           "--cand-report", sb.paths["cand_report"],
           "--cand-arm", args.cand_arm,
           "--neut-report", sb.paths["neut_report"],
           "--neut-arm", args.neut_arm,
           "--degen-static-arm", args.degen_static_arm,
           "--degen-dyn-arm", args.degen_dyn_arm]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, encoding="utf-8")
    return p.returncode, p.stdout


def main(argv):
    ap = argparse.ArgumentParser(description="negative controls for the GH#19 S6 PR-B acceptance gate")
    ap.add_argument("--criteria", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--cand-report", required=True)
    ap.add_argument("--cand-arm", required=True)
    ap.add_argument("--neut-report", required=True)
    ap.add_argument("--neut-arm", required=True)
    ap.add_argument("--degen-static-arm", required=True)
    ap.add_argument("--degen-dyn-arm", required=True)
    ap.add_argument("--gate", default=GATE, help="the gate under control (default: the sibling file)")
    ap.add_argument("--only", default=None, help="run one control by name")
    ap.add_argument("--skip", default=None, action="append",
                    help="run every control EXCEPT this one (repeatable). Needed to state the "
                         "code-wiring identity claim over the paths two builds share: a control whose "
                         "purpose is to require that the criteria is read is expected to differ between "
                         "a wired and an unwired build, so a suite containing it cannot be the evidence "
                         "that the wiring changed nothing else.")
    args = ap.parse_args(argv)

    sb = Sandbox({"criteria": args.criteria, "plan": args.plan,
                  "cand_report": args.cand_report, "neut_report": args.neut_report})
    cell, pair = criteria_victims(sb.text["criteria"])
    if cell is None:
        raise SystemExit("control setup: the criteria enumerate no cell rows")
    arms = {"cand_arm": args.cand_arm, "degen_dyn_arm": args.degen_dyn_arm,
            "victim_cell": cell, "static_cell": pair[0], "const_cell": pair[1], "moving_cell": pair[2]}
    print("NEGCTL-PRB victims static=%s const=%s moving=%s victim_cell=%s"
          % (arms["static_cell"], arms["const_cell"], arms["moving_cell"], cell))

    # ---- the positive control, FIRST and unprompted ------------------------------------------
    code, out = run_gate(args, sb)
    pristine_reports = report_values(out)
    print("CONTROL %-30s exit=%d expected=%d  %s"
          % ("pristine", code, EXIT_PASS, "PASS" if code == EXIT_PASS else "MISMATCH"))
    if code != EXIT_PASS:
        print(out)
        print("NEGCTL-PRB verdict=INVALID reason=the unmutated artifacts did not pass, so no control "
              "below could distinguish a caught mutation from a gate that refuses everything")
        return 2

    ok = bad = 0
    # The 7th field is optional so the tuple stays a readable declaration. It asks for the code to be
    # present on BOTH surfaces the gate prints it on: a line that STARTS with the token (the per-code
    # detail line) and a `COUNT` line carrying it (the tally). `code in out` alone would be satisfied by
    # either one, and a gate that renamed one surface but not the other would then pass a control whose
    # whole point is that the two surfaces describe the same string.
    entries = [c if len(c) == 7 else c + (False,) for c in CONTROLS]
    skip = set(args.skip or ())
    unknown = sorted(skip - set(c[0] for c in entries))
    if unknown:
        raise SystemExit("control setup: --skip names no such control: %s" % ",".join(unknown))
    print("NEGCTL-PRB skipped=%s" % (",".join(sorted(skip)) if skip else "(none)"))
    for name, mut, want_exit, want_code, forbidden, why, need_surfaces in entries:
        if args.only and name != args.only:
            continue
        if name in skip:
            continue
        try:
            mut(sb, arms)
            touched = sb.touched()
            if not touched and name != "pristine":
                print("CONTROL %-30s SETUP FAILED: the mutation touched no input" % name)
                bad += 1
                continue
            code, out = run_gate(args, sb)
        finally:
            # Restore BEFORE judging, so a control that raised still leaves the inputs pristine for the
            # next one, and so the restoration itself is verified rather than trusted.
            problems = sb.restore()
        if problems:
            print("CONTROL %-30s SETUP FAILED: restoration did not take for %s"
                  % (name, ",".join(problems)))
            bad += 1
            continue
        trip = want_code in out
        crashed = "Traceback (most recent call last)" in out
        leaked = [c for c in forbidden if c in out]
        # The identity tamper must not move the improvement columns; the gate cannot be asked to
        # enforce that about itself, so it is asserted here, on the two runs' own printed quantities.
        moved = []
        if name.startswith("degen_"):
            now = report_values(out)
            moved = [k for k in FROZEN_REPORTS
                     if pristine_reports.get(k) != now.get(k)]
        missing_surface = []
        if need_surfaces:
            ls = out.splitlines()
            if not any(l.startswith(want_code) for l in ls):
                missing_surface.append("detail-line")
            if not any(" COUNT " in l and want_code in l for l in ls):
                missing_surface.append("counts-line")
        good = (code == want_exit) and trip and not crashed and not leaked and not moved \
            and not missing_surface
        print("CONTROL %-30s exit=%d expected=%d code=%-26s %s%s%s%s%s"
              % (name, code, want_exit, want_code,
                 "HIT" if good else "MISS",
                 "  (CRASHED)" if crashed else "",
                 "  (ALSO FIRED %s)" % ",".join(leaked) if leaked else "",
                 "  (MOVED %s)" % ",".join(moved) if moved else "",
                 "  (ABSENT FROM %s)" % ",".join(missing_surface) if missing_surface else ""))
        if good:
            ok += 1
        else:
            bad += 1
            if not trip:
                print("         the gate did not print %s. Its own verdict lines:" % want_code)
            elif code != want_exit:
                print("         the gate exited %d where %d was required. Its verdict lines:" % (code, want_exit))
            elif missing_surface:
                print("         the gate printed %s but not on %s, so the two surfaces that are supposed "
                      "to name one string disagree. Its own verdict lines:"
                      % (want_code, ",".join(missing_surface)))
            for ln in out.splitlines():
                if " verdict=" in ln or " COUNT " in ln or ln.startswith("PRB-REFUSE-") \
                        or ln.startswith("PRB-FAIL-"):
                    print("         | " + ln)

    print("NEGCTL-PRB ok=%d bad=%d controls=%d" % (ok, bad, ok + bad))
    if ok + bad == 0:
        # Reachable via `--only X --skip X`, and worth refusing rather than reporting: a suite that ran
        # nothing would otherwise print ALL-HIT, which is the same word a suite that ran everything and
        # caught everything prints. "No control ran" is not "every control passed".
        print("NEGCTL-PRB verdict=INVALID reason=no control ran, so ALL-HIT would mean nothing")
        return 2
    print("NEGCTL-PRB verdict=%s" % ("ALL-HIT" if bad == 0 else "INCOMPLETE"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
