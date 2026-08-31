#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Pin-integrity gate for the iPlug2 host library (P5-① / GH #10).

The product host (host/) depends on iPlug2 as an UNMODIFIED fixed-commit library. Since
the GH#10 batch, iPlug2 is a REAL pinned git submodule (third_party/iPlug2, registered in
.gitmodules at an exact gitlink). A locally-edited pin is a SECRET FORK:

  * the edits drift the submodule working tree away from upstream, so the built host is
    no longer the library the review approved;
  * they vanish silently on the next pin upgrade, with no gate reporting the loss.

This gate makes the dependency relationship honest and FAIL-LOUD. It is no longer an
environment-dependent "skip if the clone is absent" guard — the submodule must be present
and correct everywhere the test suite runs (the CI checkout pulls it via a recursive
submodule init). It verifies, in order:

  1. .gitmodules registers exactly 'third_party/iPlug2' with the pinned upstream URL.
  2. the index gitlink for 'third_party/iPlug2' is mode 160000 at the EXACT pin commit
     ('gitlink correct' alone is not enough; a wrong gitlink is itself a defect).
  3. the submodule working tree is actually present and initialized (source files there),
     so a "gitlink correct but not initialized / empty worktree" is RED, not a pass.
  4. the submodule HEAD is exactly the pin.
  5. the submodule working tree is clean (a dirtied pin is a secret fork).

Any failure prints a diagnostic and exits nonzero. There is NO successful-skip path: an
absent or uninitialized submodule is a hard error, because the gate can no longer pretend
"this environment cannot verify it" once the submodule is a tracked dependency.

Registered as a CTest (iplug_pin_clean_gate) so a failure is never silent.
"""

import subprocess
import sys
from pathlib import Path

# iPlug2, pinned upstream commit (full SHA). This is the ONLY commit the host may build
# against; the product view is "iPlug2 at d54f6905", never "whatever is checked out".
EXPECTED_PIN = "d54f69050f517e43b941d88c2a170f0a840b9ee4"
PIN_SHORT = "d54f6905"

EXPECTED_URL = "https://github.com/iPlug2/iPlug2.git"
SUBMODULE_PATH = "third_party/iPlug2"

ROOT = Path(__file__).resolve().parent.parent
CLONE = ROOT / SUBMODULE_PATH


def run(argv, cwd):
    return subprocess.run(argv, cwd=cwd, capture_output=True, text=True)


def report_fail(lines):
    sys.stderr.write("".join(lines))
    return 1


def submodule_entries(gm_path):
    """Parse .gitmodules into a list of {path, url} dicts, one per submodule entry.

    Each entry is a NEW submodule section and must carry its OWN path+url. The fields are
    bound per entry so a cross-entry mutation — e.g. the real entry's URL changed to wrong
    AND a decoy entry added that carries the correct URL — cannot make the gate green by
    having the correct URL SOMEWHERE in the file. If a submodule section is missing a url
    (git allows url in the 'submodule.<name>.url' config instead), it is still an entry, just
    with url=None, so the caller can fail it explicitly.
    """
    import configparser

    if gm_path is None:
        return [], "no .gitmodules"
    cp = configparser.ConfigParser()
    cp.optionxform = str  # preserve case so 'url'/'path' match exactly
    try:
        cp.read(gm_path, encoding="utf-8")
    except Exception as exc:  # noqa: BLE001 — surface any parse failure as a defect
        return [], f".gitmodules unreadable: {exc}"

    entries = []
    for section in cp.sections():
        if not section.startswith("submodule"):
            continue
        path = cp.get(section, "path", fallback=None)
        url = cp.get(section, "url", fallback=None)
        entries.append({"path": path, "url": url})
    return entries, None


def main():
    problems = []

    # 1. .gitmodules registers the EXACT path + upstream URL as the SAME entry — not merely
    #    the path somewhere and the URL somewhere. A wrong URL on the real entry plus a decoy
    #    entry carrying the right URL must still be RED (this is a dedicated negative control).
    gm = ROOT / ".gitmodules"
    entries, gm_err = submodule_entries(gm if gm.exists() else None)
    if gm_err:
        problems.append(f"  {gm_err}\n")
    elif not entries:
        problems.append("  no [submodule ...] entries parsed from .gitmodules.\n")
    else:
        # The iPlug2 entry is the one whose path is exactly the submodule path.
        target = [e for e in entries if e["path"] == SUBMODULE_PATH]
        if not target:
            problems.append(f"  .gitmodules has NO entry with path={SUBMODULE_PATH} "
                            f"(entries: {', '.join(repr(e.get('path')) for e in entries)}).\n")
        elif len(target) > 1:
            problems.append(f"  .gitmodules has {len(target)} entries with path="
                            f"{SUBMODULE_PATH} — must be exactly ONE.\n")
        else:
            entry = target[0]
            if entry["url"] != EXPECTED_URL:
                problems.append(f"  .gitmodules entry path={SUBMODULE_PATH} has "
                                f"url={entry['url']!r} != {EXPECTED_URL!r}. "
                                "A changed/pinned-away upstream is NOT the approved "
                                "library.\n")

    # 2. The index gitlink is a SINGLE '160000 <EXACT_PIN> <stage 0> <EXACT_PATH>' line. A
    #    substring check would pass a wrong entry that merely CONTAINS the pin; it is parsed
    #    precisely instead.
    ls = run(["git", "ls-files", "-s", "--", SUBMODULE_PATH], ROOT)
    lines = [ln for ln in ls.stdout.splitlines() if ln.strip()]
    if len(lines) != 1:
        problems.append(f"  gitlink for {SUBMODULE_PATH}: expected exactly ONE staged entry, "
                        f"got {len(lines)}: {ls.stdout.strip()!r}\n")
    else:
        parts = lines[0].split(None, 3)
        mode, sha, stage, path = (parts + [None] * 4)[:4]
        if mode != "160000":
            problems.append(f"  gitlink mode={mode!r} != 160000 (expected a submodule "
                            "gitlink).\n")
        if sha != EXPECTED_PIN:
            problems.append(f"  gitlink sha={sha!r} != pinned {EXPECTED_PIN} ({PIN_SHORT}). "
                            "The index gitlink is not the approved commit.\n")
        if stage != "0":
            problems.append(f"  gitlink stage={stage!r} != 0 (expected unmerged-index "
                            "stage 0).\n")
        if path != SUBMODULE_PATH:
            problems.append(f"  gitlink path={path!r} != {SUBMODULE_PATH!r}.\n")

    # 3. The submodule working tree is present and initialized (source is actually here).
    if not (CLONE / "iPlug2.cmake").exists():
        problems.append(f"  submodule worktree at {CLONE} is absent/not initialized — "
                        "source files missing. Run `git submodule update --init --recursive`.\n")

    # 4./5. HEAD is the pin and the worktree is clean — but only if the tree is there.
    head = run(["git", "rev-parse", "HEAD"], CLONE)
    if head.returncode == 0:
        now = head.stdout.strip()
        if now != EXPECTED_PIN:
            problems.append(f"  submodule HEAD={now} != pinned {EXPECTED_PIN} ({PIN_SHORT}).\n")
        porc = run(["git", "status", "--porcelain"], CLONE)
        dirty = [ln for ln in porc.stdout.splitlines() if ln.strip()]
        if dirty:
            problems.append("  submodule working tree is DIRTY with "
                            f"{len(dirty)} change(s) — a local edit to the pin is a "
                            "secret fork:\n" + "".join(f"    {ln}\n" for ln in dirty))
    else:
        problems.append(f"  cannot read {CLONE} HEAD: {head.stderr.strip()}\n")

    if problems:
        return report_fail(["iPlug2 pin gate: FAIL —\n"] + problems)

    print(f"iPlug2 pin gate: PASS — gitlink {PIN_SHORT} ({EXPECTED_PIN}), "
          f".gitmodules path+url OK, submodule initialized and clean. "
          "Dependency is an unmodified pinned commit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
