#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Pin-drift gate for the GH#4 8B3 IPlug APP overrides (task#73).

The Lunar24Host must not silently carry two divergent host truths. The repo owns two FORKS of the
pinned iPlug2 submodule TUs (host/iPlug_app_override.cpp of IPlugAPP.cpp, host/
iPlug_app_host_override.cpp of IPlugAPP_host.cpp). Each fork may differ from upstream ONLY by the
curated, committed hunks (the Lunar banner + the AppProcess / InitAudio / AudioCallback bodies +
the failure-invalidation helper). Everything else must be byte-identical to the pinned submodule TU.

The 8B3 review (G5) required EXACT truth here, not a name anchor: the previous anchor-based gate
let you rewrite an allowlisted function's body arbitrarily (any text anchored to "AppProcess" /
"InitAudio" / "AudioCallback" / the preamble passed). The false-green is closed by pinning the
SHA-256 of the ENTIRE upstream->override unified diff. The differential is a fixed byte sequence;
if the override is edited ANYWHERE — inside an allowlisted function or in the untouched upstream
body — the diff changes, the hash changes, and the gate goes RED. This is "compare pinned upstream
-> byte-identical": the override must differ from the pinned TU by exactly this one curated diff.

The anchor-based region report is retained as a DIAGNOSTIC only: when the hash fails it prints WHICH
anchor drifted so a human sees the location. On a clean override the hash is the only gate.

The submodule SHA is pinned to d54f6905..., so a future upstream bump is reported rather than
silently re-forked. If the submodule is absent, uncheckoutable, or at a different SHA, the drift is
UNPROVABLE and this gate FAILS (it never silently passes). This is a permanent second line on top of
the two host configure-time source assertions in host/CMakeLists.txt.
"""

import difflib
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# GH#4 8B3 (task#73): the codec host check runs this gate under LC_ALL=C LANG=C PYTHONUTF8=0
# PYTHONCOERCECLOCALE=0, where stdout defaults to ASCII; the module docstring carries an em-dash.
# Pin the stream codec to UTF-8 so the gate's OUTPUT is as independent of the process locale as its
# file reads now are (read_text(encoding="utf-8")).
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")

# The exact pinned submodule SHA the overrides were forked from. If the submodule is not at this
# SHA, the override==upstream base is undefined, so the drift is unprovable -> FAIL.
PIN = "d54f69050f517e43b941d88c2a170f0a840b9ee4"

SUBMODULE = ROOT / "third_party" / "iPlug2"
UPSTREAM = {
    "app": ROOT / "third_party" / "iPlug2" / "IPlug" / "APP" / "IPlugAPP.cpp",
    "host": ROOT / "third_party" / "iPlug2" / "IPlug" / "APP" / "IPlugAPP_host.cpp",
}
OVERRIDE = {
    "app": ROOT / "host" / "iPlug_app_override.cpp",
    "host": ROOT / "host" / "iPlug_app_host_override.cpp",
}

# G5: the FIXED expected differential. SHA-256 of the complete unified diff from the pinned upstream
# TU to the repo override. The override is correct ONLY when its diff-to-upstream hashes to these
# values. Regenerated only when @Codex rules a legitimate change to a curated hunk; a mutation that
# silently rewrites an allowlisted body does NOT pass, because it changes the diff (and hence the
# hash) even though the function name is unchanged.
EXPECTED_DIFF_HASH = {
    "app": "6161896270765f649cfc964d85134178e8c15519997a3c01f272053610c3495e",
    "host": "8e46b85646917044c8e8f16bdfff8aee87becdaf04f403cec09defeefa81d36f",
}

# Function names (as "Class::func") that the curated diff is allowed to touch, per file. A region
# anchored to anything else is drift. The "" anchor = the file preamble (banner / added includes /
# the anonymous-namespace helper), which for both forks is part of the curated diff. This list is
# DIAGNOSTIC ONLY: the exact gate (EXPECTED_DIFF_HASH) is authoritative; this list just tells a human
# which anchors the curated diff legitimately touches, so a hash failure is attributed to the right place.
ALLOWED = {
    "app": {"", "IPlugAPP::AppProcess"},
    "host": {"", "IPlugAPPHost::InitAudio", "IPlugAPPHost::AudioCallback",
             "IPlugAPPHost::TryToChangeAudio"},
}

failures = []


def check(name: str, ok: bool, detail: str = ""):
    status = "PASS" if ok else "FAIL"
    print(f"  {status}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        failures.append(name)


def rev_parse(path: Path) -> str:
    """Return the checked-out commit of a git repo, or "" if it cannot be resolved."""
    try:
        out = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


# A class-member function definition opening line for the two plugin classes that own the APP
# override, e.g. "bool IPlugAPPHost::InitAudio(...)". The capture group is "Class::func". The anchor
# search is intentionally RESTRICTED to these two classes: the fork's body legitimately mentions
# std::string / static_cast / std::optional / iPlug2 helpers that also look like "x::y(z" but are not
# class-method definitions, so anchoring to them would produce a false drift report. Only the host
# classes can own a real method here.
SIG_RE = re.compile(r"\b((?:IPlugAPP|IPlugAPPHost)::[A-Za-z_][A-Za-z0-9_~]*)\s*\(")
# A line that is pure comment (a // line, a block-comment opener, a block-comment interior or
# close). The fork's banner / include notes legitimately write "Class::method(...)" in a comment, so
# an anchor search MUST skip comment lines or it would anchor a preamble diff to a name that is only
# mentioned in a comment (a false drift).
COMMENT_LINE_RE = re.compile(r"^\s*(//|/\*|\*)")


def anchor_for(override_lines: list, start: int) -> str:
    """Return the nearest preceding function name ("Class::func") for a diff region starting at
    override line `start`. The file preamble (before the first function) anchors to ""."""
    for i in range(start, -1, -1):
        if COMMENT_LINE_RE.match(override_lines[i]):
            continue
        m = SIG_RE.search(override_lines[i])
        if m:
            return m.group(1)
    return ""


def drift_regions(upstream_text: str, override_text: str, override_lines: list):
    """Yield (anchor_name, opcode, uptext, ovtext) for every differing region."""
    sm = difflib.SequenceMatcher(None, upstream_text.splitlines(keepends=True),
                                 override_lines)
    for op in sm.get_opcodes():
        tag, i1, i2, j1, j2 = op
        if tag == "equal":
            continue
        anchor = anchor_for(override_lines, j1)
        yield (anchor, tag, "".join(upstream_text.splitlines(keepends=True)[i1:i2]),
               "".join(override_lines[j1:j2]))


def unified_diff(up_lines: list, ov_lines: list, name: str) -> str:
    return "".join(difflib.unified_diff(
        up_lines, ov_lines, fromfile=f"upstream/{name}", tofile=f"override/{name}"))


def verify(kind: str):
    up = UPSTREAM[kind]
    ov = OVERRIDE[kind]
    check(f"[{kind}] upstream TU present", up.exists(), f"read {up.relative_to(ROOT)}")
    check(f"[{kind}] override TU present", ov.exists(), f"read {ov.relative_to(ROOT)}")
    if not (up.exists() and ov.exists()):
        return

    up_text = up.read_text(encoding="utf-8")
    ov_text = ov.read_text(encoding="utf-8")
    ov_lines = ov_text.splitlines(keepends=True)
    up_lines = up_text.splitlines(keepends=True)

    # The fork must still be a genuine iPlug2 file: it keeps the upstream copyright banner + the
    # iPlug2 include (a stock TU mix-in that dropped them is a red flag regardless of the diff).
    check(f"[{kind}] keeps iPlug2 copyright line",
          "the iPlug 2 developers" in ov_text, "the fork must keep the upstream iPlug2 license")
    check(f"[{kind}] keeps iPlug2 include",
          ('#include "IPlugAPP.h"' if kind == "app" else '#include "IPlugAPP_host.h"') in ov_text,
          "the fork must keep the base APP include")

    # G5 EXACT TRUTH: the override must differ from the pinned upstream by EXACTLY the curated diff.
    diff = unified_diff(up_lines, ov_lines, ov.name)
    got = hashlib.sha256(diff.encode("utf-8")).hexdigest()
    want = EXPECTED_DIFF_HASH[kind]
    check(f"[{kind}] override == pinned upstream + curated diff (byte-identical)",
          got == want,
          f"diff sha256={got}  (want {want})" if got != want else f"sha256={got}")

    # Diagnostic only (NOT a gate): when the exact hash fails, name which anchors drifted so a
    # human can attribute the change. A missing/non-curated anchor is reported as a NOTE, never as a
    # failing check — a wrong override already fails on the exact diff hash, and this attribution
    # must not independently flip the gate (nor false-positive on an iPlug2 helper name).
    regions = list(drift_regions(up_text, ov_text, ov_lines))
    drifted = [a or "preamble" for a in (a for a, _, _, _ in regions) if a not in ALLOWED[kind]]
    if drifted:
        print(f"  note  [{kind}] non-curated anchors in diff: {', '.join(sorted(set(drifted)))}")


def main() -> int:
    print("check_host_override_drift")
    head = rev_parse(SUBMODULE)
    check("pinned submodule at d54f6905", head == PIN,
          f"submodule HEAD={head or '<unresolvable>'}  (want {PIN})")

    if head != PIN:
        # Without the exact pin the override==upstream base is undefined; still attempt the
        # per-file diff so a drifting fork is caught, but fail overall because the base is
        # unverified.
        print("  (pinned SHA mismatch; the override base is UNPROVABLE)")
    for kind in ("app", "host"):
        verify(kind)

    if failures:
        print(f"  {len(failures)} drift invariant(s) FAILED")
        return 1
    print("  all override-drift invariants PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
