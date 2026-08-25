#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""EvidenceRef gate — every EvidenceRef must be grounded on real, non-blank manual lines.

The generated registry (`generated/lunar24/registry.hpp`) exposes an `EvidenceRef` for each
module, parameter, jack, route and program. Each ref names a source document plus a
`lineStart..lineEnd` window. This gate walks every one of those refs mechanically and asserts
two structural facts:

  1. the ref stays in range — lineStart/lineEnd are >= 1 and <= the source document's line count;
  2. neither boundary line is blank — lineStart and lineEnd both land on a line with content,
     so a range never starts or ends on whitespace (a range that "points into" a blank boundary
     is a transcription drift, not a citation to content).

Scope is deliberately NARROW. This gate is MECHANICAL: it proves the citation is *grounded* and
its range does not straddle a blank boundary line. It does NOT verify that the cited lines are
actually RELEVANT to the referenced fact, that the range is complete, or that the fact is
correct. Those are human/adjudicator judgements and are deliberately out of scope here. A ref
can pass this gate and still be irrelevant — this gate never claims otherwise.

The generated header is the runtime product and is kept byte-identical to the implementation
source (`spec/machine/lunar24.json`) by `registry_regen_zero_diff`. Reading refs here means this
gate checks exactly the refs the machine will expose, not a parallel copy.

TWO-MODE GROUNDING. The manual (`design/reference/solar42N_manual_text.txt`) is gitignored —
it is third-party material, and carrying it in the repo is a copyright call that is not ours to
make (see design/reference/SOURCES.md). So the source text is present on a dev machine but NOT on
CI. This gate therefore resolves the source for each ref in one of two ways:

  LIVE mode (source text on disk): ground refs against the real text, and drift-guard the
    committed fingerprint (`generated/lunar24/evidence_layout.json`) against it. The fingerprint
    is derived from the text; if they disagree the fingerprint is stale and must be regenerated
    (tools/build_evidence_layout.py) and committed.

  ABSENT mode (CI, no text): ground refs against the committed fingerprint alone — its
    `lineCount` for the in-range check and its `blankLines` set for the boundary checks. This is
    the mode CI actually runs, so this is the path that must stay green.

HONEST LIMIT: ABSENT mode can only prove each ref is grounded *against the committed fingerprint*.
It cannot verify the fingerprint matches the live text — that comparison needs the text and so is
dev-only. This is inherent: CI has no manual, so CI cannot check freshness; the freshness guard
is the LIVE-mode drift check plus `build_evidence_layout.py --check` as the regen discipline.

Usage:
  python3 tools/check_evidence_refs.py [--registry <generated registry header>]
                                        [--layout <committed evidence layout json>]
                                        [--manual <text file>]
"""

import argparse
import json
import os
import re
import sys

# One EvidenceRef in the generated header. All current refs are the single manual
# source; a future source joins here (name -> physical text file, relative to repo root).
REF_RE = re.compile(r'EvidenceRef\{"([^"]+)",\s*(\d+)u,\s*(\d+)u\}')

# map: reference source name -> path of the document its line numbers index into
# (repo-root relative). Any EvidenceRef whose source is absent here is a problem in
# its own right: the gate cannot ground an unmapped document.
SOURCE_FILES = {
    "solar42N_manual_v15": os.path.join("design", "reference", "solar42N_manual_text.txt"),
}
# The registry that carries the EvidenceRefs.
DEFAULT_REGISTRY = os.path.join("generated", "lunar24", "registry.hpp")
# The committed, derived fingerprint (one entry per SOURCE_FILES key).
DEFAULT_LAYOUT = os.path.join("generated", "lunar24", "evidence_layout.json")


def collect_refs(registry_text):
    """Return every EvidenceRef as a (source, lineStart, lineEnd) string triple."""
    return REF_RE.findall(registry_text)


def parse_manual(manual_text):
    """Return the manual's lines (1-based indexing: line n == lines[n-1]).

    IMPORTANT: split on '\n' only — never splitlines(). The manual is a
    pdftotext -layout extraction that carries form-feed (\\x0c) characters, and
    splitlines() treats those as line breaks, inflating the array (1412 real lines
    -> 1439) and shifting every ref's line index against the PDF-column line
    numbering the refs were authored to. Splitting on '\n' keeps the array aligned
    with the source's physical line numbers. A trailing newline (absent from the
    current manual, but guarded anyway) would leave a phantom '' element, so a
    single trailing '' from a terminal newline is dropped.
    """
    lines = manual_text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    return lines


def blank_lines(lines):
    """Return the 1-based line numbers that are blank (whitespace only).

    This is the fingerprint the committed `evidence_layout.json` carries per source:
    exactly the structural facts `check()` needs (total line count + how many of the
    boundaries are blank), so a ref can be grounded WITHOUT the gitignored text.
    """

    return [i + 1 for i, line in enumerate(lines) if line.strip() == ""]


class LinesSource:
    """A line source backed by an in-memory list of physical lines (the live text)."""

    def __init__(self, lines):
        self._lines = lines
        self.line_count = len(lines)

    def is_blank(self, n):
        if n < 1 or n > self.line_count:
            return True
        return self._lines[n - 1].strip() == ""


class LayoutSource:
    """A line source backed by the COMMITTED fingerprint (lineCount + blankLines).

    This is what grounds refs in ABSENT mode — e.g. CI, where the manual is
    gitignored and never on disk. It must reproduce the same is_blank/line_count the
    live text would, so a ref that points at a blank boundary is caught there exactly
    as on the dev machine.
    """

    def __init__(self, layout):
        self.line_count = layout["lineCount"]
        self._blank = {int(b) for b in layout.get("blankLines", [])}

    def is_blank(self, n):
        if n < 1 or n > self.line_count:
            return True
        return n in self._blank


def _as_source(value):
    """Normalize a `check()` source value to a line source.

    Accepts either a `list` of lines (the historical `manual_by_source` shape, and
    what the negative test drives) or a `LinesSource`/`LayoutSource`, so callers stay
    source-of-truth-agnostic.
    """
    if isinstance(value, (LinesSource, LayoutSource)):
        return value
    return LinesSource(value)


def check(registry_text, sources_by_name):
    """Validate every EvidenceRef in `registry_text`.

    `sources_by_name` maps a source name -> a line source: either a list of physical
    lines (the in-memory manual text) or a `LinesSource`/`LayoutSource` (the
    committed fingerprint, used on the CI absent-manual path). Returns a list of
    human-readable problems; an empty list means the gate passes.
    """
    problems = []
    refs = collect_refs(registry_text)
    if not refs:
        problems.append("no EvidenceRef found in generated registry")

    for idx, (source, a, b) in enumerate(refs):
        a = int(a)
        b = int(b)
        label = f"EvidenceRef #{idx} ({source} [{a},{b}])"

        if source not in sources_by_name:
            problems.append(f"{label}: unknown source document (cannot be grounded)")
            continue

        src = _as_source(sources_by_name[source])
        line_count = src.line_count
        if line_count < 1:
            problems.append(f"{label}: source document has no lines")
            continue

        if a < 1 or b < 1 or a > line_count or b > line_count:
            problems.append(
                f"{label}: out of range (source has lines 1..{line_count})"
            )
            continue

        if a > b:
            problems.append(f"{label}: lineStart {a} > lineEnd {b}")
            continue

        if src.is_blank(a):
            problems.append(f"{label}: lineStart {a} is a blank line")
        if src.is_blank(b):
            problems.append(f"{label}: lineEnd {b} is a blank line")

    return problems


def _load_layout(path):
    """Load the committed evidence layout JSON -> {source: {path, lineCount, blankLines}}."""
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return data.get("sources", {})


def _layout_mismatch(committed, live_lines):
    """Return a human-readable drift description, or None if committed == live.

    The committed layout is the fingerprint; the live manual is the source of truth on
    a machine that has it. If they disagree the fingerprint has drifted from the text it
    was derived from and must be regenerated/committed.
    """
    if committed is None:
        return "no committed layout entry"
    if committed["lineCount"] != len(live_lines):
        return f"lineCount {committed['lineCount']} != live {len(live_lines)}"
    if [int(b) for b in committed.get("blankLines", [])] != blank_lines(live_lines):
        return "blankLines do not match live text"
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--registry", type=str, default=DEFAULT_REGISTRY,
                    help="path to the generated registry header (default: %(default)s)")
    ap.add_argument("--layout", type=str, default=None,
                    help="override path of the committed evidence layout JSON (test convenience; "
                         "default: %(default)s)")
    ap.add_argument("--manual", type=str, default=None,
                    help="override path of the first source document (test convenience)")
    args = ap.parse_args(argv)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def resolve(path):
        return path if os.path.isabs(path) else os.path.join(root, path)

    registry_path = resolve(args.registry)
    with open(registry_path, encoding="utf-8") as fh:
        registry_text = fh.read()

    layout_path = resolve(args.layout or DEFAULT_LAYOUT)
    layout = _load_layout(layout_path)

    layout_problems = []
    sources_by_name = {}
    for source, rel in SOURCE_FILES.items():
        manual_path = rel
        if args.manual is not None and source.startswith("solar42N"):
            manual_path = args.manual
        manual_path = resolve(manual_path)

        if os.path.exists(manual_path):
            # LIVE mode: ground refs against the actual text, and drift-guard the
            # committed fingerprint against it. The manual is gitignored, so this is the
            # only place the two can be compared — it cannot run on CI.
            live_lines = parse_manual(open(manual_path, encoding="utf-8").read())
            mismatch = _layout_mismatch(layout.get(source), live_lines)
            if mismatch:
                layout_problems.append(
                    f"{source}: committed evidence layout drifted from live manual "
                    f"({mismatch}); run tools/build_evidence_layout.py and commit the result"
                )
            sources_by_name[source] = LinesSource(live_lines)
        else:
            # ABSENT mode (CI): the manual is gitignored and not on disk. Ground refs
            # against the committed fingerprint only. A source with no committed entry is
            # reported as an internal inconsistency below; `check()` still sees it as
            # ungrounded (0 lines), which is accurate.
            committed = layout.get(source)
            if committed is None:
                layout_problems.append(
                    f"{source}: no committed evidence layout entry in {os.path.basename(layout_path)}; "
                    f"cannot ground refs in absent-manual mode"
                )
                committed = {"lineCount": 0, "blankLines": []}
            sources_by_name[source] = LayoutSource(committed)

    problems = list(layout_problems)
    problems += check(registry_text, sources_by_name)

    if problems:
        print(f"FAIL: {len(problems)} EvidenceRef/layout problem(s) in {registry_path}")
        for p in problems:
            print(f"  {p}")
        return 1

    ref_count = len(collect_refs(registry_text))
    print(f"OK: all {ref_count} EvidenceRefs grounded on in-range, non-blank lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
