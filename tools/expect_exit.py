# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# expect_exit.py — run a command and require an exact exit code.
#
# Used by the TSan selftest (task #37 close-out). The selftest rebuilds the
# state_publish probe with -DLUNAR24_PROBE_SELFTEST, which short-circuits the pool's
# pin guard in state_snapshot.h, so the probe MUST report a ThreadSanitizer data race.
# TSan is configured with `TSAN_OPTIONS=abort_on_error=0:exitcode=66`, so a race is a
# clean exit 66, not an abort. CTest treats a non-zero process exit as a failure, so
# this wrapper INVERTS the expectation: the selftest CTest passes only when the probe
# exits 66 — precisely "the detector fired, proving the committed probe can actually
# fail". If the probe exits 0 (it did NOT detect the race a guard-less pool must
# produce), or crashes with any other code, the wrapper returns non-zero and CTest
# fails — so a dead probe is caught, never silently green. A hang is caught by the
# test's TIMEOUT instead.
#
# Usage: expect_exit.py <expected-exit-code> <command...>

import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: expect_exit.py <expected> <cmd...>", file=sys.stderr)
        return 2
    expected = int(sys.argv[1])
    result = subprocess.run(sys.argv[2:])
    if result.returncode == expected:
        return 0
    print(
        f"expect_exit: wanted exit {expected}, got {result.returncode}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
