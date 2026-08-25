#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build/verify the committed evidence-layout fingerprint.

The Solar 42N manual is gitignored — it is third-party material, and carrying it in
the repo is a copyright call that is not ours to make (see design/reference/SOURCES.md).
But `evidence_refs_gate` needs, per cited source, its total line count plus which lines
are blank, to ground each EvidenceRef's lineStart/lineEnd without the text. This tool
derives that fingerprint from the REAL manual (which lives on the machine under
design/reference/) and writes `generated/lunar24/evidence_layout.json` — the committed,
derived form that CI grounding uses.

Usage:
  python3 tools/build_evidence_layout.py          # write the committed fingerprint
  python3 tools/build_evidence_layout.py --check  # fail if the committed file differs (regen==commit)

DEV-ONLY. This needs the manual text on disk, so it is intentionally NOT wired into
ctest (CI has no manual — a skip-everywhere drift guard would be the same disease the
gate is fixing). The freshness guard against a drifting manual lives in
evidence_refs_gate's LIVE mode; this tool is the regenerate-and-commit counterpart that
keeps the checked-in fingerprint reproducible.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_evidence_refs as gate

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "generated", "lunar24", "evidence_layout.json")


def build():
    """Derive the fingerprint from every source's live manual text."""
    sources = {}
    for source, rel in gate.SOURCE_FILES.items():
        manual_path = os.path.join(ROOT, rel)
        lines = gate.parse_manual(open(manual_path, encoding="utf-8").read())
        sources[source] = {
            "path": rel,
            "lineCount": len(lines),
            "blankLines": gate.blank_lines(lines),
        }
    return {"version": 1, "sources": sources}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="compare the committed file against a fresh build instead of writing")
    args = ap.parse_args()

    data = build()
    text = json.dumps(data, indent=2) + "\n"

    if args.check:
        if not os.path.exists(OUT):
            print(f"FAIL: committed {os.path.basename(OUT)} missing; run build_evidence_layout.py "
                  f"(no --check) to write it")
            return 1
        existing = open(OUT, encoding="utf-8").read()
        if existing != text:
            print(f"FAIL: committed evidence layout drifted; rerun build_evidence_layout.py "
                  f"and commit (regen==commit discipline)")
            return 1
        n_blank = sum(len(v.get("blankLines", [])) for v in data["sources"].values())
        print(f"OK: committed evidence layout matches live manual ({n_blank} blank lines)")
        return 0

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
