#!/usr/bin/env python3
"""Static gates for the framework-free core public headers.

  * every public header in core/include/lunar24/core/ must carry an SPDX
    Apache-2.0 header (license gate),
  * no header may include a framework / platform / filesystem type
    (forbidden-include scan — keeps lunar-core framework-free).

Usage:
  python3 tools/check_core_headers.py          # scans, exits non-zero on violation
  python3 tools/check_core_headers.py --list   # print scanned files
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_INC = os.path.join(ROOT, "core", "include", "lunar24", "core")

SPDX = "SPDX-License-Identifier: Apache-2.0"

# Tokens that must never appear in a core public header. i2 = iPlug2.
FORBIDDEN = {
    "iplug2", "igraphics", "iplug", "coreaudio", "audiotoolbox", "audiounit",
    "windows.h", "windef.h", "winbase.h", "mmsystem.h", "wasapi",
    "fstream", "ofstream", "ifstream", "<filesystem>", "filesystem",
    "unistd.h", "pthread.h", "mach/mach", "objc/", "nsstring",
    "wx/", "gtk/", "juce_audio", "juce::",
}

# Headers a core header is allowed to include (our own core/ and standard <...>).
_SKIP_DIRECTIVE = False


def scan(path):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    problems = []
    if SPDX not in text:
        problems.append("missing SPDX header")

    lines = text.splitlines()
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            continue
        low = stripped.lower()
        for token in FORBIDDEN:
            if token in low:
                problems.append(f"forbidden include token {token!r}: {stripped}")
                break
    return problems


def main():
    files = sorted(f for f in os.listdir(CORE_INC) if f.endswith((".h", ".hpp")))
    nok = 0
    all_problems = {}
    for f in files:
        p = os.path.join(CORE_INC, f)
        problems = scan(p)
        if problems:
            nok += 1
            for prob in problems:
                print(f"{f}: {prob}")
    if "--list" in sys.argv[1:]:
        print(f"scanned {len(files)} headers in {CORE_INC}")
    if nok:
        print(f"{nok} header(s) failed core-header gates")
        return 1
    print(f"OK: {len(files)} core headers pass SPDX + forbidden-include gates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
