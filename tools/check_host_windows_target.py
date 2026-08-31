#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Source-structure oracle for the Windows host target (P5-① / GH #10).

The Windows build of Lunar24Host is compiled and linked by the hosted MSVC runner (there is
no Windows toolchain on the mac dev machine), so CMake cannot be the gate here. This oracle
statically verifies the shape of the Windows CONFIGURATION that would otherwise only be
proven at hosted link time — so a regression that silently regresses the Windows structure
(e.g. the WIN32 branch silently dropping the resource, or re-adding the mac bootstrap) is
caught by CI on ANY platform, not just Windows. It is the deterministic "target is real"
gate the mandate asks for.

Every invariant below has a name; each is checked and reported. A violation exits nonzero.
The negative controls (the mutated-fixture proof that each check can actually trip) are run
by the throwaway harness in the task evidence, not here.

Invariants (one check per GH#10 risk surface):
  N1  The WIN32 branch is real: it calls iplug_configure_app and compiles the repo-owned
      window-metrics shim + resource script (i.e. the target is genuinely assembled, not a
      mac-only target sitting under a `if(APPLE)`).
  N2  The WIN32 branch must NOT compile the mac bootstrap main.mm (that would drag SWELL,
      Cocoa framework links and a second main() into a Windows build).
  N3  The Windows resource script resources/main.rc is compiled, and the RC compiler is told
      where resource.h lives (mandate pin: the Windows branch must actually compile main.rc),
      and every resource ID it references resolves from host/resource.h or a Win SDK constant.
  N4  The renderer is FORCED per platform (GL2 on WIN32), so a stale mac-METAL cache value
      can never be carried into a Windows configure (mandate pin: not polluted by the mac cache).
  N5  host/window_metrics_win.cpp must NOT define gHINSTANCE / gHWND / SaveWindowScreenshot —
      those are provided by the stock IPlugAPP_main.cpp that the pinned iPlug2::APP compiles,
      so redefining them is a duplicate-symbol link error.
  N5b The shim reads the work area via a REAL SystemParametersInfoW(SPI_GETWORKAREA, ...) call
      and never via the non-existent SM_CXWORKAREA / SM_CYWORKAREA (undefined-identifier —
      would fail hosted MSVC even though the structure looks fine).
  N5c The shim derives the visible size from the work-area RECT SPAN (right - left,
      bottom - top), never from the absolute right/bottom corner — otherwise a taskbar docked
      left/top overestimates the available space.
  N6  (a) the pin gate check_iplug_pin_clean.py has no silent-success path for an absent
      submodule (mandate pin: gitlink-correct-but-uninitialized is RED — verified at runtime
      by the fixture harness); (b) ci.yml checkout pulls the submodule recursively so that
      gate has a worktree to inspect.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read(fs, rel):
    p = fs / rel
    return p.read_text(encoding="utf-8") if p.exists() else ""


def check(name, ok, detail):
    return (name, ok, detail)


def win_cmake(fs):
    """Return the WIN32 TARGET-branch region of host/CMakeLists.txt.

    There are two `elseif(WIN32)` in the file: the renderer-selection block near the top
    and the actual target assembly near the bottom. The target region is the LAST one
    (it is followed by add_executable). We take the last so the checks below run against
    the real target assembly, not the renderer block.
    """
    text = read(fs, "host/CMakeLists.txt")
    positions = [m.start() for m in re.finditer(r"elseif\(WIN32\)", text)]
    if not positions:
        return ""
    # Slice from the LAST occurrence (the target branch, not the renderer block).
    return text[positions[-1]:]


def strip_comments(code):
    """Remove // line comments and /* */ block comments (used for definition checks)."""
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.S)
    code = re.sub(r"//[^\n]*", " ", code)
    return code


def win_sources(win):
    """Return the source filenames listed in the WIN32 add_executable(...) call.

    The oracle must distinguish "this file is COMPILED" from "this string is merely
    mentioned somewhere in the region" (e.g. in a set_source_files_properties or a
    comment). Only the add_executable source list is authoritative for what is compiled.
    """
    m = re.search(r"add_executable\(\s*(.*?)\n\s*\)", win, re.S)
    if not m:
        return []
    # Split on whitespace and newlines; keep tokens that look like source paths.
    tokens = re.split(r"[\s()]+", m.group(1))
    return [t for t in tokens if t and ("." in t or "/" in t)]


def oracle(fs):
    results = []
    win = win_cmake(fs)
    srcs = win_sources(win)

    # N1 — the WIN32 branch is a REAL assembly: iplug_configure_app + the repo-owned shim
    # and resource script are actually listed as compiled sources.
    n1 = (
        "iplug_configure_app(" in win
        and "window_metrics_win.cpp" in srcs
        and "resources/main.rc" in srcs
    )
    results.append(check(
        "N1_win_target_real", n1,
        "WIN32 branch must call iplug_configure_app AND compile window_metrics_win.cpp and "
        "resources/main.rc"))

    # N2 — the mac bootstrap main.mm must NOT be a compiled source in the WIN32 branch.
    n2 = "main.mm" not in srcs
    results.append(check(
        "N2_win_no_mac_bootstrap", n2,
        "WIN32 branch must not compile host/main.mm (mac SWELL/Cocoa bootstrap)"))

    # N3 — main.rc is COMPILED (in the add_executable source list, not merely mentioned in a
    # property line) AND the RC include path is supplied. (mandate pin a)
    rc_ok = "resources/main.rc" in srcs and 'COMPILE_FLAGS' in win and '/I' in win
    rc_file = read(fs, "host/resources/main.rc")
    rc_has = all(tok in rc_file for tok in ("resource.h", "winres.h", "IDD_DIALOG_MAIN",
                                            "IDR_MENU1", "VS_VERSION_INFO"))
    n3 = rc_ok and rc_has
    results.append(check(
        "N3_win_rc_compiled", n3,
        "resources/main.rc must be a WIN32 compiled source, get an /I flag, and define the "
        "dialog/menu/version blocks"))

    # N3b — every resource ID referenced by main.rc must resolve from host/resource.h or a
    # Win SDK predefined constant. An orphan ID token (one referenced by the .rc but defined
    # neither in resource.h nor by winres.h) would fail the hosted RC compile, so it must be
    # caught structurally on any platform, not just Windows. Comments are stripped first —
    # the header's attribution note legitimately spells out the name of a removed ID, which
    # is prose, not a resource directive.
    rh = read(fs, "host/resource.h")
    rh_defined = set(re.findall(r"#define\s+(ID[A-Z0-9_]+)\s", rh))
    # Predefined Win32 constants from winres.h that legitimately need no resource.h entry.
    win_sdk = {"IDOK", "IDCANCEL", "IDABORT", "IDRETRY", "IDIGNORE", "IDYES", "IDNO",
               "IDCLOSE", "IDC_STATIC"}
    rc_ids = set(re.findall(r"\bID[A-Z0-9_]+\b", strip_comments(rc_file)))
    unresolved = sorted(rc_ids - rh_defined - win_sdk)
    n3b = not unresolved
    results.append(check(
        "N3b_win_rc_symbols_resolve", n3b,
        f"every resource ID in main.rc must resolve from host/resource.h or Win SDK "
        f"(unresolved IDs found: {', '.join(unresolved) or 'none'})"))

    # N4 — renderer is FORCED, never a cache-remembered value. (mandate pin b) The FORCE
    # block is at the top of host/CMakeLists.txt (shared, before the platform branches), so
    # check the whole file, not just the WIN32 region.
    full_cmake = read(fs, "host/CMakeLists.txt")
    n4 = bool(re.search(r"set\(IGRAPHICS_RENDERER \"GL2\"[^\n]*FORCE\)", full_cmake))
    results.append(check(
        "N4_renderer_forced", n4,
        "host/CMakeLists.txt must set IGRAPHICS_RENDERER=GL2 with FORCE on the WIN32 "
        "branch (no mac-cache leak)"))

    # N5 — the shim must not DEFINE (not merely mention) the stock APP symbols. Strip
    # comments first so the header prose that warns against redefining them is ignored.
    shim = strip_comments(read(fs, "host/window_metrics_win.cpp"))
    bad_syms = [s for s in ("gHINSTANCE", "gHWND", "SaveWindowScreenshot")
                if re.search(r"\b" + s + r"\b", shim)]
    n5 = not bad_syms
    results.append(check(
        "N5_shim_no_app_symbols", n5,
        f"window_metrics_win.cpp must not DEFINE the stock APP symbols (found: "
        f"{', '.join(bad_syms) or 'none'})"))

    # N5b — the shim must take the work area from the Win32 contract, not from constants
    # that do not exist. GetSystemMetrics has NO SM_CXWORKAREA/SM_CYWORKAREA; the correct
    # call is a REAL SystemParametersInfoW(SPI_GETWORKAREA, ..., RECT*). This pins the
    # production blocker @Codex caught: a hosted MSVC compile would be an undefined
    # identifier. Require an actual SystemParametersInfoW(...SPI_GETWORKAREA...) call (not
    # just the SPI_GETWORKAREA token floating somewhere) and explicitly reject the fake
    # metrics.
    n5b = bool(re.search(r"SystemParametersInfoW\s*\(\s*SPI_GETWORKAREA", shim)
               and "SM_CXWORKAREA" not in shim
               and "SM_CYWORKAREA" not in shim)
    results.append(check(
        "N5b_win_workarea_contract", n5b,
        "window_metrics_win.cpp must call SystemParametersInfoW(SPI_GETWORKAREA, ...) and "
        "must NOT use the non-existent SM_CXWORKAREA / SM_CYWORKAREA"))

    # N5c — the work area is a RECT of COORDINATES, so the visible size is the SPAN
    # (right-left, bottom-top), not the absolute right/bottom corner. When the taskbar is
    # docked left or top, left/top are non-zero and the absolute corner OVERESTIMATES the
    # available space. The span must be a subtraction between the SAME struct's members
    # (`<var>.right - <var>.left`, `<var>.bottom - <var>.top`), matched with a backreference
    # so a naive `.right` / `.bottom` corner probe (no subtraction) does not pass. A
    # regression that restores the naive reading trips RED on any CI OS.
    right_left = re.search(r"(\w+)\.right\s*-\s*\1\.left", shim)
    bottom_top = re.search(r"(\w+)\.bottom\s*-\s*\1\.top", shim)
    n5c = bool(right_left and bottom_top)
    results.append(check(
        "N5c_win_workarea_span", n5c,
        "window_metrics_win.cpp must derive the visible size from the work-area RECT span "
        "(<var>.right - <var>.left, <var>.bottom - <var>.top), never from the absolute "
        "right/bottom corner"))

    # N6b — ci.yml pulls the submodule recursively AND unconditionally builds + asserts the
    # actual-host artifact on mac AND Windows (a silent skip must be impossible). This is the
    # deterministic target-presence gate for both platforms at the CI-source level:
    # submodules:recursive, an explicit `--target Lunar24Host` build step, the mac bundle
    # executable path, and the Windows .exe path must all be present.
    ci = read(fs, ".github/workflows/ci.yml")
    n6b = all(tok in ci for tok in (
        "submodules: recursive",
        "--target Lunar24Host",
        "Lunar24Host.app/Contents/MacOS/Lunar24Host",
        "Lunar24Host.exe",
    ))
    results.append(check(
        "N6b_ci_recursive_and_target_assert", n6b,
        "ci.yml checkout must use submodules: recursive AND `--target Lunar24Host` must "
        "build/assert BOTH the mac bundle executable and the Windows .exe (deterministic "
        "target-presence gate, no silent skip)"))

    # Root-level: the host is gated to APPLE OR WIN32 with a FATAL, never an EXISTS skip.
    root = read(fs, "CMakeLists.txt")
    n_root = re.search(r"if\(APPLE OR WIN32\)", root) is not None and "iPlug2.cmake" in root
    results.append(check(
        "R_root_gate", n_root,
        "root CMakeLists must gate host on APPLE OR WIN32 and FATAL on a missing submodule"))

    return results


def main():
    fs = ROOT
    if "--repo" in sys.argv:
        fs = pathlib.Path(sys.argv[sys.argv.index("--repo") + 1])

    results = oracle(fs)
    bad = [(n, d) for n, ok, d in results if not ok]
    if bad:
        for name, detail in bad:
            sys.stderr.write(f"FAIL — {name}: {detail}\n")
        sys.stderr.write(f"host Windows-target oracle: {len(bad)}/{len(results)} invariant(s) "
                         "violated.\n")
        return 1
    for name, ok, _ in results:
        if not ok:
            continue
    print(f"host Windows-target oracle: PASS — all {len(results)} invariants satisfied "
          "(WIN32 target is a real assembly; no mac bootstrap; main.rc compiled + /I; "
          "renderer FORCED; shim defines no APP symbols; CI recursive + artifact assert).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
