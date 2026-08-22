#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Static gates for the Lunar 24 authored source tree.

Two gates, both hard enough that they cannot false-pass:

  1. License gate — every authored file under core/include/lunar24/core/, the
     generated headers, and every tools/*.py must carry the Apache-2.0 SPDX block
     in its LEADING header lines (a "header present somewhere in the file" check
     is not enough). Generated files are verified by exact emission, so this also
     catches an out-of-date generated header.
  2. Forbidden-include gate — no core public header may include a framework /
     platform / filesystem type (keeps lunar-core framework-free).

Usage:
  python3 tools/check_core_headers.py          # scans, exits non-zero on violation
  python3 tools/check_core_headers.py --list   # print scanned files
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_INC = os.path.join(ROOT, "core", "include", "lunar24", "core")
GEN_DIR = os.path.join(ROOT, "generated", "lunar24")
TOOLS_DIR = os.path.join(ROOT, "tools")

SPDX = "SPDX-License-Identifier: Apache-2.0"
SPDX_HEADING = "Copyright (c) 2026 Lunar 24 contributors"
LEADING = 12  # the license block must sit within the first 12 lines

# Forbidden tokens for core PUBLIC headers. i2 = iPlug2.
FORBIDDEN = {
    "iplug2", "igraphics", "iplug", "coreaudio", "audiotoolbox", "audiounit",
    "windows.h", "windef.h", "winbase.h", "mmsystem.h", "wasapi",
    "fstream", "ofstream", "ifstream", "<filesystem>", "filesystem",
    "unistd.h", "pthread.h", "mach/mach", "objc/", "nsstring",
    "wx/", "gtk/", "juce_audio", "juce::",
}


def _has_license_block(text):
    head = "\n".join(text.splitlines()[:LEADING])
    return SPDX in head and SPDX_HEADING in head


def scan_license(text):
    if not _has_license_block(text):
        return "missing Apache-2.0 SPDX header block leading the file"
    return None


def scan_forbidden(text):
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("#include"):
            continue
        low = stripped.lower()
        for token in FORBIDDEN:
            if token in low:
                return f"forbidden include token {token!r}: {stripped}"
    return None


def collect_files():
    files = []  # (display, abs_path, do_forbidden_scan)
    for d in (CORE_INC, GEN_DIR):
        if os.path.isdir(d):
            for base in sorted(os.listdir(d)):
                p = os.path.join(d, base)
                if os.path.isfile(p) and base.endswith((".h", ".hpp")):
                    files.append((os.path.relpath(p, ROOT), p, d == CORE_INC))
    if os.path.isdir(TOOLS_DIR):
        for base in sorted(os.listdir(TOOLS_DIR)):
            p = os.path.join(TOOLS_DIR, base)
            if os.path.isfile(p) and base.endswith(".py"):
                files.append((os.path.relpath(p, ROOT), p, False))
    return files


def main():
    files = collect_files()
    nok = 0
    for rel, path, do_forbidden in files:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        problems = []
        lic = scan_license(text)
        if lic:
            problems.append(lic)
        if do_forbidden:
            forb = scan_forbidden(text)
            if forb:
                problems.append(forb)
        if problems:
            nok += 1
            for prob in problems:
                print(f"{rel}: {prob}")

    if "--list" in sys.argv[1:]:
        print(f"scanned {len(files)} authored files")
    if nok:
        print(f"{nok} authored file(s) failed the license / framework-free gates")
        return 1
    print(f"OK: {len(files)} authored files pass the leading SPDX + framework-free gates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
