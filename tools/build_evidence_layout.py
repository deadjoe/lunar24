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
  python3 tools/build_evidence_layout.py --check  # drift-guard: fail if committed differs from live

REGISTERED IN CTEST (evidence_layout_guard). This needs the manual text on disk, and the
manual is gitignored — so the check's meaning is environment-dependent, exactly the shape
@Claude mandated for the real CoreAudio output test:

  * manual present (a dev machine) -> really compare the freshly-derived fingerprint
    against the committed one (regen==commit discipline; a drift is a hard red);
  * manual absent (CI)            -> SKIP loudly, printing that it cannot protect this
    environment, and return success.

Registering it rather than leaving it a dev-only script is precisely the point @Claude
drew: an unregistered check that "someone runs by hand" is actually never run, and a CI
skip that is never registered is invisible. Registering makes the CI skip VISIBLE — a
reader sees the guard is present but inert here. A silent absent guard is the same
disease the gate is fixing.
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


def sources_present():
    """True when every source's manual text is on disk.

    The manual is gitignored, so this is false only in a no-manual environment (CI).
    It gates whether the drift-guard can actually compute a fingerprint to compare:
    on CI there is no live text to regenerate from, so the guard must loudly skip
    rather than crash or fake a pass.
    """
    return all(os.path.exists(os.path.join(ROOT, rel)) for rel in gate.SOURCE_FILES.values())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="compare the committed file against a fresh build instead of writing")
    args = ap.parse_args()

    if args.check:
        if not sources_present():
            # Registered in ctest so the SKIP is visible, never silent. On CI there is
            # no manual to regenerate from — this guard cannot compare fingerprint-vs-live
            # there, and a FileNotFoundError crash would take the whole test down. Same
            # register-and-loudly-skip shape as the real CoreAudio output test.
            print("[SKIP] evidence-layout drift-guard: source manual absent in this "
                  "environment (CI): the manual is gitignored, so the drift-guard cannot "
                  "compare the committed fingerprint against the live text here. Registered "
                  "so it is visible it is NOT protecting this environment.")
            return 0

        data = build()
        text = json.dumps(data, indent=2) + "\n"

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

    data = build()
    text = json.dumps(data, indent=2) + "\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
