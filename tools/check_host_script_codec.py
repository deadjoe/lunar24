#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Codec portability gate for the two GH#4 host gate scripts (task#73).

The hosted CI run for d31bd3e exposed a third-class hosted failure: on the Windows runner the gate
scripts called Path.read_text() with no encoding=, so Python 3.14 there defaulted to cp1252 and either
crashed on a UTF-8 byte undefined in cp1252 (the pre-existing "⚠️" emoji in host/config.h) or silently
mojibaked the UTF-8 non-ASCII bytes and shifted the pinned drift hash. Both failed tests were the same
root cause: the file reads depended on the process default encoding.

This gate makes that class of bug impossible to re-introduce, and it is itself encoding-robust:
  * AST: every .read_text(...) call in both gate scripts must carry a CONSTANT encoding="utf-8".
  * disposable-copy negative: removing any ONE encoding from a mutated copy must go RED; the
    unmutated source must stay GREEN, so a future edit cannot drop the encoding unnoticed.
  * env independence: run both gates under LC_ALL=C LANG=C PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 and
    require exit 0, proving the file reads no longer depend on the process default encoding.
The pinned UPSTREAM->override diff hash and all product/override semantics are untouched: this only
guards the encoding the gate scripts read their inputs with.
"""

import ast
import copy
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GATES = {
    "check_host_override_drift.py": ROOT / "tools" / "check_host_override_drift.py",
    "check_host_engine_wiring.py": ROOT / "tools" / "check_host_engine_wiring.py",
}

failures = []


def check(name: str, ok: bool, detail: str = ""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        failures.append(name)


def read_text_calls(tree) -> list:
    """Return every .read_text(...) Call node, ordered by source position."""
    nodes = [n for n in ast.walk(tree)
             if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
             and n.func.attr == "read_text"]
    nodes.sort(key=lambda n: (n.lineno, n.col_offset))
    return nodes


def has_utf8_encoding(call) -> bool:
    """True iff the call carries a keyword encoding="utf-8" (constant string)."""
    for kw in call.keywords:
        if kw.arg == "encoding" and isinstance(kw.value, ast.Constant) \
                and kw.value.value == "utf-8":
            return True
    return False


def scan(src: str):
    """Parse a source string and return (total_calls, bad_calls, calls_in_order)."""
    tree = ast.parse(src)
    calls = read_text_calls(tree)
    bad = [c for c in calls if not has_utf8_encoding(c)]
    return len(calls), bad, calls


def run_gate(path: Path):
    """Run one gate with the hostile-C-locale env; return its returncode."""
    env = dict(os.environ)
    env.update({"LC_ALL": "C", "LANG": "C", "PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0"})
    r = subprocess.run([sys.executable, str(path)], cwd=str(ROOT), env=env,
                       capture_output=True)
    return r.returncode, r.stdout.decode("utf-8", errors="replace"), \
        r.stderr.decode("utf-8", errors="replace")


def main() -> int:
    print("check_host_script_codec")

    # 1. AST invariant: every .read_text(...) in both gates carries a constant encoding="utf-8".
    parsed = {}
    for name, path in GATES.items():
        src = path.read_text(encoding="utf-8")
        parsed[name] = src
        try:
            total, bad, _ = scan(src)
        except SyntaxError as exc:
            check(f"[{name}] parseable", False, f"SyntaxError: {exc}")
            continue
        check(f"[{name}] parseable", True)
        check(f"[{name}] every .read_text carries encoding='utf-8'", total > 0 and not bad,
              f"{total} call(s)" if not bad else f"{len(bad)} of {total} missing")

    # 2. Disposable-copy negative: removing any ONE encoding must go RED; unmutated stays GREEN.
    for name, path in GATES.items():
        src = parsed.get(name)
        if src is None:
            continue
        tree = ast.parse(src)
        calls = read_text_calls(tree)
        for i, _ in enumerate(calls):
            mutated_tree = copy.deepcopy(tree)
            mcalls = read_text_calls(mutated_tree)
            mcalls[i].keywords = [k for k in mcalls[i].keywords if k.arg != "encoding"]
            mutated = ast.unparse(mutated_tree)
            mtotal, mbad, _ = scan(mutated)
            check(f"[{name}] strip encoding from call #{i + 1} -> RED", len(mbad) >= 1,
                  f"{len(mbad)} missing of {mtotal}")
        _, bad, _ = scan(src)
        check(f"[{name}] unmutated source stays GREEN", not bad)

    # 3. Env independence: both gates must PASS under a hostile (C) locale + UTF-8 mode off.
    for name, path in GATES.items():
        rc, out, err = run_gate(path)
        tail = (out.strip().splitlines() or [""])[-1]
        check(f"[{name}] passes under LC_ALL=C LANG=C PYTHONUTF8=0 PYTHONCOERCECLOCALE=0",
              rc == 0, f"exit={rc} {tail}" if rc else tail or "exit=0")

    if failures:
        print(f"  {len(failures)} codec invariant(s) FAILED")
        return 1
    print("  all codec invariants PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
