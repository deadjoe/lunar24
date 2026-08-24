#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Gate: the produced tree must never depend on the disposable P1 `spike/` dir.

P1's spike artifacts are use-once-and-discard: they exist only to prove the
iPlug2 framework works (or reveal that it does not) before any of it becomes a
product extension. If any of them grow `../` includes into a core/ or
generated/ file, or if a generated header starts referencing a spike path, then
a later "delete spike/ and ship" step would break the build.

So this gate scans the two dirs that must stay framework- and spike-free —
`core/` and `generated/` — for ANY reference that could reach into `spike/`.
A reference is a `spike/` path token, or a spike include/header spelling.
A hit FAILS the gate.

This scans ONLY `core/` and `generated/`, never `spike/` itself (spike/ is
allowed to reference anything; the boundary is one-directional).

Usage:
  python3 tools/check_spike_clean.py          # scans, exits non-zero on violation
  python3 tools/check_spike_clean.py --list   # print scanned files
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The two trees that must never reach into the disposable spike/ dir.
SCAN_DIRS = ("core", "generated")

# A "reference to spike/" is a path token (`spike/...`, `../spike/...`, a CMake
# `.../spike/...`) or an include/header spelling (`#include "spike.h"`,
# `#include <spike.h>`). We do NOT match the bare audio word "spike" — it is a
# real DSP term (an impulse/spike) and would false-positive.
REFERENCE = re.compile(r"""

    spike\s*/            # `spike/` path token (optional whitespace before /)
  | "spike               # `"spike...` include/string header spelling
  | <spike               # `<spike...` include header spelling

""", re.VERBOSE)


def iter_text_files(dirpath):
    """Yield (abs_path) for every file under dirpath, skipping VCS/scratch."""
    for base, dirs, files in os.walk(dirpath):
        # Do not wander into build scratch / .git if a future build lands inside.
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__")]
        for name in files:
            yield os.path.join(base, name)


def main(argv):
    list_only = "--list" in argv
    scanned = []
    hits = []

    for rel in SCAN_DIRS:
        absd = os.path.join(ROOT, rel)
        if not os.path.isdir(absd):
            continue
        for fpath in iter_text_files(absd):
            scanned.append(fpath)
            if list_only:
                continue
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as fh:
                    lines = fh.readlines()
            except OSError:
                pass
            for lineno, line in enumerate(lines, 1):
                m = REFERENCE.search(line)
                if m:
                    hits.append((fpath, lineno, line.rstrip("\n")))

    if list_only:
        for f in scanned:
            print(f)
        return 0

    if hits:
        print("spike-cleanliness gate FAILED: core/ + generated/ must not reference spike/")
        for fpath, lineno, line in hits:
            rel = os.path.relpath(fpath, ROOT)
            print(f"  {rel}:{lineno}: {line.strip()}")
        return 1

    print(f"spike-cleanliness gate OK: {len(scanned)} files scanned, no spike/ reference")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
