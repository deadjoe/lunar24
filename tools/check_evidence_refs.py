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

Usage:
  python3 tools/check_evidence_refs.py [--registry <generated registry header>] [--manual <text file>]
"""

import argparse
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


def check(registry_text, manual_by_source):
    """Validate every EvidenceRef in `registry_text`.

    `manual_by_source` maps a source name -> its parsed manual lines (list of str).
    Returns a list of human-readable problems; an empty list means the gate passes.
    """
    problems = []
    refs = collect_refs(registry_text)
    if not refs:
        problems.append("no EvidenceRef found in generated registry")

    for idx, (source, a, b) in enumerate(refs):
        a = int(a)
        b = int(b)
        label = f"EvidenceRef #{idx} ({source} [{a},{b}])"

        if source not in manual_by_source:
            problems.append(f"{label}: unknown source document (cannot be grounded)")
            continue

        lines = manual_by_source[source]
        line_count = len(lines)
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

        if lines[a - 1].strip() == "":
            problems.append(f"{label}: lineStart {a} is a blank line")
        if lines[b - 1].strip() == "":
            problems.append(f"{label}: lineEnd {b} is a blank line")

    return problems


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--registry", type=str, default=DEFAULT_REGISTRY,
                    help="path to the generated registry header (default: %(default)s)")
    ap.add_argument("--manual", type=str, default=None,
                    help="override path of the first source document (test convenience)")
    args = ap.parse_args(argv)

    registry_path = args.registry
    if not os.path.isabs(registry_path):
        # resolve repo-root-relative paths from the script's own location
        registry_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), args.registry)

    with open(registry_path, encoding="utf-8") as fh:
        registry_text = fh.read()

    manual_by_source = {}
    for source, rel in SOURCE_FILES.items():
        manual_path = rel
        if args.manual is not None and source.startswith("solar42N"):
            manual_path = args.manual
        if not os.path.isabs(manual_path):
            manual_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), manual_path)
        with open(manual_path, encoding="utf-8") as fh:
            manual_by_source[source] = parse_manual(fh.read())

    problems = check(registry_text, manual_by_source)
    if problems:
        print(f"FAIL: {len(problems)} EvidenceRef problem(s) in {registry_path}")
        for p in problems:
            print(f"  {p}")
        return 1

    ref_count = len(collect_refs(registry_text))
    print(f"OK: all {ref_count} EvidenceRefs grounded on in-range, non-blank lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
