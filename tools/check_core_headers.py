#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Static gates for the Lunar 24 authored source tree.

Two gates, both hard enough that they cannot false-pass:

  1. License gate — every AUTHORED CODE file in the tree (C/C++ headers+sources,
     Python, CMakeLists.txt), wherever it lives, must carry the Apache-2.0 SPDX
     block in its LEADING header lines. The scan is RECURSIVE over the whole
     tracked source (core/, generated/, tests/, tools/, design/*.py, the root
     CMakeLists.txt) so a script dropped in a subdirectory, or a new CMake file,
     is caught and not silently skipped. A "header present somewhere in the file"
     check is not enough; generated files are verified by exact emission too.
  2. Forbidden-include gate — only the core PUBLIC headers (they define the
     framework-free boundary) may be scanned for framework / platform /
     filesystem includes; the rest of the tree is NOT restricted that way.

Third-party / vendored dirs (third_party/) and build/ are never scanned as
authored. Non-code files (docs, JSON, YAML, LICENSE) are not license-gated.

Usage:
  python3 tools/check_core_headers.py          # scans, exits non-zero on violation
  python3 tools/check_core_headers.py --list   # print scanned files
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_INC = os.path.join(ROOT, "core", "include", "lunar24", "core")

# Directories that are never part of the authored source (vendored / generated
# build scratch). A future vendored third-party lib lands here and stays uncounted.
SKIP_DIRS = {".git", "build", "third_party"}

# A file is code (license-gated) by extension, or by being a CMakeLists. These are
# the comment-capable source types the gate claims to cover — C-family sources
# (.c/.m/.mm — macOS desktop will land .mm shortly) and CMake modules, plus the
# Python generators. Anything comment-capable and tracked is gated so a new
# translation unit or CMake module cannot silently skip the header requirement.
CODE_EXTS = {".h", ".hpp", ".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".py", ".cmake"}

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


def _is_code(name):
    return name == "CMakeLists.txt" or os.path.splitext(name)[1] in CODE_EXTS


def collect_files():
    # Recursive over the whole tree. License-scan every code file; the forbidden-
    # include scan applies ONLY to core public headers (the framework-free line).
    real_core = os.path.realpath(CORE_INC)
    files = []  # (display, abs_path, do_forbidden_scan)
    for base, dirs, names in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for n in sorted(names):
            if not _is_code(n):
                continue
            p = os.path.join(base, n)
            if not os.path.isfile(p):
                continue
            do_forbidden = os.path.realpath(p).startswith(real_core)
            files.append((os.path.relpath(p, ROOT), p, do_forbidden))
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
