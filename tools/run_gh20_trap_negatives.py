#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# run_gh20_trap_negatives.py — task #88 (GH #20) NEGATIVE CONTROLS for the two-integrator
# trapezoidal (TPT/ZDF) acceptance gate.
#
# Verifies that the numeric acceptance criterion (tools/gh20_trap_analyze.py --check, which compares
# the PRODUCT response to the independent bilinear reference gh20_trap_reference) DISCRIMINATES: it
# must be GREEN on the real trapezoidal product and RED when the production PolivoksFilter is mutated
# away from the @Codex numeric contract (msg 8320798f). This is the runnable form of "the tool has
# real discriminating power" — an old-cap / old-recursion / wrong-damp / wrong-LP-BP / L-R-cross /
# skip-setter / wrong-update-order response is RED, not silently accepted.
#
# Under test (all mutations edit ONLY the header, in an isolated temp include dir that shadows the
# real one — nothing in the committed tree is ever modified):
#   1. old_cap       — restore kCutoffCapRatioSoft 0.49 -> 0.125 (the sr/8 dead-zone ghost).
#   2. old_recursion — restore the Chamberlin recursion (and the sr/8 cap).
#   3. wrong_damp    — kDampMin 0.1 -> 0.5 (res=1 is far less resonant; wrong Q).
#   4. wrong_lp_bp   — swap the mode-select return (LP gets BP, BP gets LP).
#   5. lr_cross      — both process() outputs read channel[0] (L/R state cross).
#   6. skip_setter   — setFreq/setRes ignore their target (always default 0.3/0.0).
#   7. wrong_update_order — update ic1eq BEFORE computing v2 (v2 reads the post-update ic1eq).
#
# The driver (gh20_trap_neg_driver.cpp) drives the PRODUCTION PolivoksFilter directly — the same
# header-only INTERFACE class the real codec->owner->render path instantiates — so each build/run is
# a few seconds (not the multi-minute full-chain probe). The chain gain the full path introduces is a
# frequency-independent constant that the reference comparison normalises out, so the response
# discrimination transfers to the full-chain gate.
#
# POXIS-only (the compiler is invoked to build the mutated driver); on Windows this is skipped, and
# the real acceptance gate (gh20_vcf_acceptance) still runs there.
#
# Reproduction (from the repo root):
#   python3 tools/run_gh20_trap_negatives.py
#   python3 tools/run_gh20_trap_negatives.py --verbose
#
import argparse
import csv
import io
import math
import os
import shutil
import subprocess
import sys
import tempfile

# The only file the negatives mutate; everything else is read from the real (committed) tree.
VCF_HEADER = "core/include/lunar24/core/polivoks_vcf.h"
DRIVER_SRC = "tools/gh20_trap_neg_driver.cpp"

REF_MAX_DB = 0.1        # contract: small-signal amplitude error vs reference <= 0.1 dB.
SNR_DB = 20.0           # resolvability floor (same as the acceptance gate).
MIN_RESV = 2            # a group needs >= this many resolvable cells to be reference-evaluable.
LR_CROSS_THRESH_DB = 1.0  # wetL(0.5) vs wetR(0.3) must separate by >= this (L/R isolation).
DC_TOL_DB = 0.05        # LP DC gain must be within this of 0 dB at both res (res-independent).
MUTATIONS = ["old_cap", "old_recursion", "wrong_damp", "wrong_lp_bp", "lr_cross",
             "skip_setter", "wrong_update_order"]


def read_tsv(text):
    return list(csv.DictReader(io.StringIO(text), delimiter="\t"))


def compile_driver(root, shadow_inc, compiler, binpath):
    subprocess.run([compiler, "-O2", "-std=c++17", "-I", shadow_inc,
                    "-I", os.path.join(root, "core/include/lunar24/core"),
                    os.path.join(root, DRIVER_SRC), "-o", binpath], check=True)


def run_driver(binpath):
    out = subprocess.run([binpath], capture_output=True, text=True, check=True)
    return read_tsv(out.stdout)


def reference_error(rows):
    groups = {}
    for r in rows:
        if r["cell"] != "sig":
            continue
        sr = float(r["sr_hz"]); mode = r["mode"]; res = float(r["res"])
        norm = float(r["norm"]); freq = float(r["freq_hz"])
        amp = abs(float(r["amp_rms"])); noise = abs(float(r["noise_rms"]))
        if amp <= 0 or mode not in ("lp", "bp"):
            continue
        mag = ref.reference_mag(freq, norm, sr, res, mode)
        if mag <= 1e-12:
            continue
        snr = float("inf") if noise <= 0 else (20.0*math.log10(amp/noise) if amp > 0 else -1e9)
        groups.setdefault((sr, mode, res, norm), []).append((freq, amp, mag, snr))
    worst = None
    for key, pts in groups.items():
        resv = [p for p in pts if p[3] >= SNR_DB]
        if len(resv) < MIN_RESV:
            continue
        ratios = [a/m for (_, a, m, _) in resv]
        C = statistics.median(ratios)
        if C <= 0:
            continue
        errs = [20.0*math.log10(a/(C*m)) for (_, a, m, _) in resv if a > 0 and C*m > 0]
        if not errs:
            continue
        mx = max(abs(e) for e in errs)
        if worst is None or mx > worst[0]:
            worst = (mx, key)
    return worst


def lr_isolation(rows):
    m = {}
    for r in rows:
        if r["cell"] != "lr":
            continue
        key = (float(r["sr_hz"]), float(r["freq_hz"]), float(r["norm"]))
        m.setdefault(key, {})[r["channel"]] = abs(float(r["amp_rms"]))
    paired = {}
    for (sr, freq, norm), chs in m.items():
        if norm == 0.5 and "wetL" in chs:
            paired.setdefault((sr, freq), {})["wetL"] = chs["wetL"]
        if norm == 0.3 and "wetR" in chs:
            paired.setdefault((sr, freq), {})["wetR"] = chs["wetR"]
    worst = float("inf")
    for (_, _), chs in paired.items():
        if len(chs) < 2 or chs["wetL"] <= 0 or chs["wetR"] <= 0:
            continue
        d = abs(20.0*math.log10(chs["wetL"]/chs["wetR"]))
        if d < worst:
            worst = d
    return worst


def dc_checks(rows):
    m = {}
    for r in rows:
        if r["cell"] != "dc":
            continue
        m[(r["mode"], float(r["res"]))] = float(r["amp_rms"])
    out = {}
    for (mode, res), dc in m.items():
        db = 20.0*math.log10(abs(dc)/0.05) if (abs(dc) > 0) else -80.0
        out[(mode, res)] = db
    return out


def apply_patch(text, name):
    if name == "old_cap":
        needle = "static constexpr double kCutoffCapRatioSoft = 0.49;"
        assert needle in text, "old_cap needle"
        return text.replace(needle, "static constexpr double kCutoffCapRatioSoft = 0.125;")
    if name == "old_recursion":
        assert "static constexpr double kCutoffCapRatioSoft = 0.49;" in text
        text = text.replace("static constexpr double kCutoffCapRatioSoft = 0.49;",
                            "static constexpr double kCutoffCapRatioSoft = 0.125;")
        text = text.replace("    double ic1eq = 0.0, ic2eq = 0.0;  // two-integrator trapezoidal (TPT/ZDF) states.",
                            "    double ic1eq = 0.0, ic2eq = 0.0;  // two-integrator trapezoidal (TPT/ZDF) states.\n    double lp = 0.0, bp = 0.0;  // Chamberlin (mutation) states.")
        start = "    const double g = std::tan(3.14159265358979323846 * fc / sr_);"
        end = "    return (c.mode < 0.5) ? v1 : v2;  // bp=v1, lp=v2."
        i = text.index(start); j = text.index(end) + len(end)
        chmb = ("    const double f = 2.0 * std::sin(3.14159265358979323846 * fc / sr_);\n"
                "    const double qq = 1.0 / (kDampMax + (kDampMin - kDampMax) * c.res);\n"
                "    const double hp = x - c.lp - qq * c.bp;\n"
                "    c.bp += f * hp;\n"
                "    c.lp += f * c.bp;\n"
                "    return (c.mode < 0.5) ? c.bp : c.lp;\n")
        return text[:i] + chmb + text[j:]
    if name == "wrong_damp":
        needle = "static constexpr double kDampMin = 0.1;"
        assert needle in text, "wrong_damp needle"
        return text.replace(needle, "static constexpr double kDampMin = 0.5;")
    if name == "wrong_lp_bp":
        needle = "return (c.mode < 0.5) ? v1 : v2;  // bp=v1, lp=v2."
        assert needle in text, "wrong_lp_bp needle"
        return text.replace(needle, "return (c.mode < 0.5) ? v2 : v1;  // SWAPPED: bp=v2, lp=v1.")
    if name == "lr_cross":
        needle = ("    outL = tick_(channel_[0], inL);\n"
                  "    outR = tick_(channel_[1], inR);\n")
        assert needle in text, "lr_cross needle"
        return text.replace(needle,
                            "    outL = tick_(channel_[0], inL);\n"
                            "    outR = tick_(channel_[0], inR);  // L/R state cross: both use ch0.\n")
    if name == "skip_setter":
        f_needle = "void setFreq(int ch, double freq) { if (idx_(ch)) channel_[ch].freq = clamp01_(freq); }"
        assert f_needle in text, "skip_setter freq needle"
        text = text.replace(f_needle,
                            "void setFreq(int ch, double freq) { if (idx_(ch)) { (void)freq; channel_[ch].freq = 0.3; } }")
        r_needle = "void setRes(int ch, double res)   { if (idx_(ch)) channel_[ch].res   = clamp01_(res); }"
        assert r_needle in text, "skip_setter res needle"
        text = text.replace(r_needle,
                            "void setRes(int ch, double res)   { if (idx_(ch)) { (void)res; channel_[ch].res   = 0.0; } }")
        return text
    if name == "wrong_update_order":
        old = ("    const double v3 = x - c.ic2eq;\n"
               "    const double v1 = a1 * c.ic1eq + a2 * v3;\n"
               "    const double v2 = c.ic2eq + a2 * c.ic1eq + a3 * v3;\n"
               "    c.ic1eq = 2.0 * v1 - c.ic1eq;\n"
               "    c.ic2eq = 2.0 * v2 - c.ic2eq;\n")
        new = ("    const double v3 = x - c.ic2eq;\n"
               "    const double v1 = a1 * c.ic1eq + a2 * v3;\n"
               "    c.ic1eq = 2.0 * v1 - c.ic1eq;\n"
               "    const double v2 = c.ic2eq + a2 * c.ic1eq + a3 * v3;\n"
               "    c.ic2eq = 2.0 * v2 - c.ic2eq;\n")
        assert old in text, "wrong_update_order needle"
        return text.replace(old, new)
    raise ValueError("unknown mutation %r" % name)


def report(name, res):
    ra = res.get("ref_at")
    if res.get("ref_max_db") is not None and ra:
        print("  ref_max_err=%.4f dB @ (sr=%s mode=%s res=%s norm=%s)" % (res["ref_max_db"], ra[0], ra[1], ra[2], ra[3]))
    else:
        print("  ref_max_err=none")
    print("  L/R min delta=%s dB   LP_DC(res0)=%s dB  LP_DC(res1)=%s dB" % (
        "%.4f" % res["lr_min_delta_db"] if res["lr_min_delta_db"] != -999.0 else "n/a",
        "%.4f" % res["lp_dc_res0_db"] if res["lp_dc_res0_db"] is not None else "n/a",
        "%.4f" % res["lp_dc_res1_db"] if res["lp_dc_res1_db"] is not None else "n/a"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--compiler", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    compiler = args.compiler or os.environ.get("CXX", "c++")
    real_text = open(os.path.join(root, VCF_HEADER)).read()

    results = {}
    for name in ["real"] + MUTATIONS:
        with tempfile.TemporaryDirectory() as td:
            shadow = os.path.join(td, "shadow")
            os.makedirs(shadow, exist_ok=True)
            patched = real_text if name == "real" else apply_patch(real_text, name)
            with open(os.path.join(shadow, "polivoks_vcf.h"), "w") as fh:
                fh.write(patched)
            binpath = os.path.join(td, "driver")
            try:
                compile_driver(root, shadow, compiler, binpath)
                rows = run_driver(binpath)
            except Exception as e:
                results[name] = {"error": str(e)}
                continue
            refw = reference_error(rows)
            lr = lr_isolation(rows)
            dc = dc_checks(rows)
            results[name] = {
                "ref_max_db": refw[0] if refw else None,
                "ref_at": refw[1] if refw else None,
                "lr_min_delta_db": lr if lr != float("inf") else -999.0,
                "lp_dc_res0_db": dc.get(("lp", 0.0)),
                "lp_dc_res1_db": dc.get(("lp", 1.0)),
            }
        print("=== %s ===" % name)
        if "error" in results[name]:
            print("  ERROR: %s" % results[name]["error"])
        else:
            report(name, results[name])

    print("\n== ASSERTIONS ==")
    ok = True
    def check(cond, msg):
        nonlocal ok
        print(("  PASS " if cond else "  FAIL ") + msg)
        ok = ok and cond

    real = results["real"]
    check(real.get("ref_max_db") is not None and real["ref_max_db"] <= REF_MAX_DB,
          "real product matches reference (%.4f dB <= %.1f)" % (real["ref_max_db"], REF_MAX_DB))
    check(real["lr_min_delta_db"] >= LR_CROSS_THRESH_DB,
          "real L/R isolation separates (min %.3f dB)" % real["lr_min_delta_db"])
    check(real["lp_dc_res0_db"] is not None and abs(real["lp_dc_res0_db"]) <= DC_TOL_DB,
          "real LP DC gain(res0)=%.4f dB (~0)" % (real["lp_dc_res0_db"] or 0))
    check(real["lp_dc_res1_db"] is not None and abs(real["lp_dc_res1_db"]) <= DC_TOL_DB,
          "real LP DC gain(res1)=%.4f dB (~0, res-independent)" % (real["lp_dc_res1_db"] or 0))

    for name in MUTATIONS:
        r = results.get(name)
        if r is None or "error" in r:
            check(False, "%s: RED — NO data/error (%s)" % (name, r.get("error") if r else "missing"))
            continue
        if name == "lr_cross":
            check(r["lr_min_delta_db"] < LR_CROSS_THRESH_DB,
                  "%s: RED L/R isolation collapsed to %.3f dB (<%.1f)" % (name, r["lr_min_delta_db"], LR_CROSS_THRESH_DB))
        else:
            check(r.get("ref_max_db") is not None and r["ref_max_db"] > REF_MAX_DB,
                  "%s: RED ref_max_err=%.4f dB > %.1f" % (name, r.get("ref_max_db", 0.0), REF_MAX_DB))

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gh20_trap_reference as ref
    import statistics
    sys.exit(main())
