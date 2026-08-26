#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Pin-integrity gate for the iPlug2 host library (P5-①).

The product host (host/) depends on iPlug2 as an UNMODIFIED fixed-commit library. The
clone is a LOCAL, NON-VENDORED dependency: it lives at third_party/iPlug2/, is pinned
to one commit, and is never edited here (that is also why it is gitignored — see
.gitignore). A locally-edited pin is a SECRET FORK:

  * the edits are invisible to review (they live in a gitignored clone, not the tracked
    tree), so a reviewer patrolling the diff cannot see them;
  * they vanish silently on the next pin upgrade, with no gate reporting the loss.

This gate makes the dependency relationship honest. It verifies the clone is checked
out at exactly the pinned commit AND that its working tree is clean. A clone that is
dirty (a local edit) or on the wrong commit FAILS the gate — the negative must genuinely
fire, so a stray edit cannot ride along unseen.

ENVIRONMENT-DEPENDENT, same shape @Claude mandated for the real CoreAudio output test
and the evidence-layout drift-guard:

  * clone present (a dev machine) -> REALLY verify HEAD == pin and porcelain empty;
    any mismatch is a hard red.
  * clone absent (CI: iPlug2 is not vendored, no clone step) -> SKIP loudly, printing
    that this environment cannot protect the pin, and return success. Absent is NOT
    "clean"; it is "unverified". A silent absent guard would be indistinguishable from a
    guard that found nothing wrong.

Registered as a CTest (iplug_pin_clean_gate) so the SKIP is VISIBLE, never silent.
"""

import subprocess
import sys
from pathlib import Path

# iPlug2, pinned upstream commit (full SHA). This is the ONLY commit the host may build
# against; the product view is "iPlug2 at d54f6905", never "whatever is checked out".
EXPECTED_PIN = "d54f69050f517e43b941d88c2a170f0a840b9ee4"
PIN_SHORT = "d54f6905"

ROOT = Path(__file__).resolve().parent.parent
CLONE = ROOT / "third_party" / "iPlug2"


def run(argv, cwd):
    return subprocess.run(argv, cwd=cwd, capture_output=True, text=True)


def report_fail(lines):
    print("".join(lines), file=sys.stderr)
    return 1


def main():
    if not CLONE.exists() or not (CLONE / ".git").exists():
        # Register-and-loudly-skip, never a silent absent guard. CI has no clone (a local
        # dependency, not vendored); the gate cannot verify a pin that is not here, so it
        # says so on stdout and returns success to keep the whole test up.
        print(f"[SKIP] iPlug2 pin gate: no clone at {CLONE}. iPlug2 is a local, "
              "non-vendored dependency pinned to a fixed commit; this environment cannot "
              "verify it. Check the pin on the machine that holds the clone.")
        return 0

    head = run(["git", "rev-parse", "HEAD"], CLONE)
    if head.returncode != 0:
        return report_fail([f"iPlug2 pin gate: FAIL — cannot read HEAD in {CLONE}: "
                            f"{head.stderr.strip()}\n"])

    now = head.stdout.strip()
    porc = run(["git", "status", "--porcelain"], CLONE)
    dirty = [ln for ln in porc.stdout.splitlines() if ln.strip()]

    problems = []
    if now != EXPECTED_PIN:
        problems.append(f"  HEAD={now} != pinned {EXPECTED_PIN} ({PIN_SHORT}).\n"
                        "  The pin moved. Either re-pin to the intended commit and update "
                        "EXPECTED_PIN, or restore the pin. The host may only build against "
                        "the pinned commit.\n")
    if dirty:
        problems.append("  working tree is DIRTY with "
                        f"{len(dirty)} change(s) — {len(dirty)} local edit(s) to the pin "
                        "is a secret fork (invisible to review, silent on upgrade):\n"
                        + "".join(f"    {ln}\n" for ln in dirty))

    if problems:
        return report_fail(["iPlug2 pin gate: FAIL —\n"] + problems)

    print(f"iPlug2 pin gate: PASS — HEAD={PIN_SHORT} ({EXPECTED_PIN}) clean, "
          "no local edits. Dependency is an unmodified fixed commit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
