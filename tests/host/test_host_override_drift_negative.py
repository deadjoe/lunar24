#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Negative control for check_host_override_drift.py (GH#4 8B3, task#73, G5).

@Codex's G5: the OLD anchor-based drift gate let you rewrite an allowlisted function's body
arbitrarily and still stay green (any diff anchored to "IPlugAPP::AppProcess" / "IPlugAPPHost::
InitAudio" / "AudioCallback" / the preamble passed). The gate was therefore rewritten to pin the
SHA-256 of the ENTIRE upstream->override diff, so a mutation ANYWHERE — including inside an
allowlisted function — changes the diff and goes RED. This negative drives that exact comparator on
a MUTATED override (the old anti-pattern: re-connect all MaxNChannels() in AppProcess, a 2-out
device would be read out of bounds) and asserts the hash CHANGES. If the gate ever stops flagging a
same-name allowlisted mutation, this breaks first.

It ALSO asserts the un-mutated override still hashes to the pinned green value, so a future curated
change that legitimately re-pins EXPECTED_DIFF_HASH cannot go silently stale (this negative would
fail on the stale green assertion, forcing a review of the re-pin).

Run: python3 tests/host/test_host_override_drift_negative.py
"""

import difflib
import hashlib
import os
import sys

# Allow importing the gate module from tools/ regardless of CWD.
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(_ROOT, "tools"))

import check_host_override_drift as gate  # noqa: E402


def diff_hash(up_text: str, ov_text: str, ov_name: str) -> str:
    up = up_text.splitlines(keepends=True)
    ov = ov_text.splitlines(keepends=True)
    d = gate.unified_diff(up, ov, ov_name)
    return hashlib.sha256(d.encode("utf-8")).hexdigest()


def mutate_appprocess(override_text: str) -> str:
    """Re-connect all MaxNChannels() in AppProcess (the exact anti-pattern that would read a 2-out
    device's smaller buffers out of bounds). Injected immediately before the Lunar comment that now
    replaces the two re-connect lines, so it lands INSIDE the allowlisted AppProcess body."""
    needle = "  // Lunar 24 (task#73): do NOT re-connect all MaxNChannels() every block."
    if needle not in override_text:
        raise SystemExit("FAIL: could not find the AppProcess Lunar comment anchor to mutate")
    reconnects = (
        "  SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), !IsInstrument()); //TODO: go elsewhere - enable inputs\n"
        "  SetChannelConnections(ERoute::kOutput, 0, MaxNChannels(ERoute::kOutput), true); //TODO: go elsewhere\n"
    )
    return override_text.replace(needle, reconnects + needle, 1)


def main() -> int:
    # The un-modified override must STILL hash to the pinned green value (the exact gate baseline).
    for kind in ("app", "host"):
        up = gate.UPSTREAM[kind]
        ov = gate.OVERRIDE[kind]
        up_text = up.read_text(encoding="utf-8")
        ov_text = ov.read_text(encoding="utf-8")
        green = diff_hash(up_text, ov_text, ov.name)
        if green != gate.EXPECTED_DIFF_HASH[kind]:
            raise SystemExit(
                f"FAIL: [{kind}] un-mutated override no longer matches pinned green hash — "
                f"got {green}, want {gate.EXPECTED_DIFF_HASH[kind]}. "
                "If this is a legitimate curated change, re-pin EXPECTED_DIFF_HASH AND review it.")

    # The anti-pattern mutation (inside an ALLOWLISTED function, same name) MUST change the hash.
    up_app = gate.UPSTREAM["app"].read_text(encoding="utf-8")
    ov_app = gate.OVERRIDE["app"].read_text(encoding="utf-8")
    mutated = mutate_appprocess(ov_app)
    m_hash = diff_hash(up_app, mutated, gate.OVERRIDE["app"].name)
    if m_hash == gate.EXPECTED_DIFF_HASH["app"]:
        raise SystemExit(
            "FAIL: reconnecting MaxNChannels() inside AppProcess did NOT change the drift hash — "
            "the exact gate has become insensitive to an allowlisted-function mutation (G5 regressed).")

    print("OK: allowlisted AppProcess mutation changes the drift hash "
          f"(green={gate.EXPECTED_DIFF_HASH['app'][:12]}… mutated={m_hash[:12]}…)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
