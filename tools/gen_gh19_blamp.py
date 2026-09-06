#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gen_gh19_blamp.py — reproducible generator for the VCO triangle BLAMP LUT.
#
# task #86 / GH #19. @Codex (msg ab5315a6, b9a77738) mandate: from the paper
# (Esqueda / Välimäki / Bilbao, "Rounding Corners with BLAMP", DAFx-16, §2 /
# Eq. (6)-(7)) derive the dimensionless residual (Eq. 6 minus the trivial ramp,
# Eq. 2), truncate it to a finite interval with an EXPLICIT C1 smoothing window
# so the boundary value AND first derivative go to zero, and sum ALL periodic
# corner points inside the support.
#
# This tool emits a C++ LUT "blampG(u)" for the windowed analytic residual and
# VERIFIES (fail-exit, not print-only): corner value 8/pi^2, even symmetry,
# boundary continuity (value and first-difference -> 0 at |u| = L), and the
# linear-interpolation error. It also runs a GENERATION-CONSISTENCY check: the
# emitted numeric array is re-parsed and each value is compared against the
# analytic g at the same grid point, so a stale/hand-edited LUT cannot pass
# silently. It is the single source of truth for the numbers; do not hand-edit
# the emitted LUT.
#
# Derivation
# ----------
# The trivial ramp is r(t) = t*u(t) (Eq. 2; u = Heaviside). Its bandlimited form
# is the BLAMP (Eq. 6-7):
#     h(2)(t) = t*(1/2 + Si(pi*fs*t)/pi) + cos(pi*fs*t)/(pi^2*fs)
# where Si is the sine integral. The BLAMP RESIDUAL is h(2) - r. Writing it in
# the dimensionless distance u = fs*t (samples from the corner) and multiplying
# by fs so the 1/fs cancels:
#     R(u) = u*(1/2 + Si(pi*u)/pi) + cos(pi*u)/pi^2 - u*H(u)
# which is even about the corner, R(0) = 1/pi^2, and decays slowly (it does not
# vanish, hence the truncation window). The triangle's slope-jump magnitude is
# 8 (peak: -8, valley: +8), which is the 2*mu of the paper, so the emitted
# coefficient is g(u) = 8*R(u)*w(u), giving the corner 8/pi^2 = 0.81056947.
#
# The window w(u) = cos^2(pi*u/(2*L)) for |u| <= L, 0 elsewhere, is C1: it kills
# both the value and the first derivative at |u| = L, so the continuous truncated
# residual has no support-boundary discontinuity. The emitted discrete kernel is a
# LINEAR-INTERPOLATION LUT, so the final kernel is only C0 (piecewise-linear), NOT
# C1 — its last interpolation segment has a small residual slope before reaching 0
# at the support edge. This is labeled as a truncated approximation, never "exact"
# or "no-droop".
#
# Use:  python3 gen_gh19_blamp.py --L 8 --N 256   (prints LUT + verification)
#       python3 gen_gh19_blamp.py --L 8 --cpp    (emit C++ + fail on any check)
import argparse, math, re

PI = math.pi


def Si(x):
    # int_0^x sin(t)/t dt, odd, accurate to ~1e-8 for |x| <= ~200.
    if x == 0.0:
        return 0.0
    a = abs(float(x))
    n = max(8192, int(a * 8192))
    h = a / n
    f0 = 1.0  # lim sin(t)/t
    fa = math.sin(a) / a
    s = 0.0
    for k in range(1, n):
        t = k * h
        s += math.sin(t) / t
    val = h * (0.5 * f0 + s + 0.5 * fa)
    return math.copysign(val, x)


def residual(u):
    # Dimensionless exact BLAMP residual R(u) = Eq.(6) - trivial ramp, * fs.
    H = 1.0 if u >= 0 else 0.0
    return u * (0.5 + Si(PI * u) / PI) + math.cos(PI * u) / (PI * PI) - u * H


def window(u, L):
    r = abs(u) / L
    return math.cos(0.5 * PI * r) ** 2 if r <= 1.0 else 0.0


def g(u, L):
    # Slope-8 folded coefficient: g(u) = 8 * R(u) * w(u).
    return 8.0 * residual(u) * window(u, L)


def assert_ok(cond, label, detail):
    if not cond:
        raise SystemExit("ASSERT FAILED: %s  (%s)" % (label, detail))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--L", type=float, default=8.0)
    ap.add_argument("--N", type=int, default=256)  # uniform samples over [0,L]
    ap.add_argument("--interp-tol", type=float, default=2e-3)
    ap.add_argument("--cpp", action="store_true")
    a = ap.parse_args()
    L, N = a.L, a.N
    if N < 2:
        raise SystemExit("N must be >= 2")

    # Uniform grid over [0, L]; index 0 is the corner (u=0).
    grid = [i * L / (N - 1) for i in range(N)]
    vals = [g(x, L) for x in grid]

    corner = g(0.0, L)
    target = 8.0 / PI**2

    # --- verification: fail-exit assertions (was print-only) ---
    assert_ok(abs(corner - target) <= 1e-9, "corner=8/pi^2",
              "g(0)=%.12f vs 8/pi^2=%.12f delta=%.3e" % (corner, target, abs(corner - target)))
    maxsym = max(abs(g(x, L) - g(-x, L)) for x in grid)
    assert_ok(maxsym <= 1e-12, "even symmetry",
              "max|g(x)-g(-x)|=%.3e" % maxsym)
    bv = g(L, L)
    assert_ok(abs(bv) <= 1e-12, "boundary value->0", "g(L)=%.6e" % bv)
    step = L / (N - 1)
    bd = abs(g(L, L) - g(L - 2 * step, L))
    assert_ok(bd <= 1e-4, "boundary slope->0",
              "g(L)-g(L-2*step)=%.3e" % bd)

    def lut_lin(uv):
        if uv <= 0:
            return vals[0]
        if uv >= L:
            return 0.0
        t = uv / L * (N - 1)
        i = int(t)
        fr = t - i
        return vals[i] * (1 - fr) + vals[min(i + 1, N - 1)] * fr
    err = 0.0
    for k in range(0, 4001):
        uv = L * k / 4000.0
        err = max(err, abs(lut_lin(uv) - g(uv, L)))
    assert_ok(err <= a.interp_tol, "linear-interp error <= tol",
              "max=%.4e tol=%.4e" % (err, a.interp_tol))

    print("VERIFY PASS for L=%g N=%d" % (L, N))
    print("  corner=%.9f (8/pi^2=%.9f) sym=%.3e boundary g(L)=%.3e slope-eg=%.3e interp-err=%.4e"
          % (corner, target, maxsym, bv, bd, err))

    if not a.cpp:
        return

    # --- emit C++ ---
    lines = []
    lines.append("//--------------------------------------------------------------------------")
    lines.append("// Generated by tools/gen_gh19_blamp.py --L %g --N %d --cpp" % (L, N))
    lines.append("// g(u) = 8*R(u)*w(u): R = paper Eq.(6)-ramp dimensionless residual (even,")
    lines.append("// corner 1/pi^2), w = C1 Hanning window cos^2(pi*u/(2L)), |u|<=%g, L=%g." % (L, L))
    lines.append("// Copyright (c) 2026 Lunar 24 contributors / SPDX-License-Identifier: Apache-2.0")
    lines.append("// FINITE-SUPPORT linear-interp LUT = a TRUNCATED APPROXIMATION of the analytic")
    lines.append("// residual, NOT the exact infinite-support kernel. The continuous (windowed)")
    lines.append("// kernel is C1 at |u|=L (value and slope -> 0); the linear-interp LUT is only")
    lines.append("// piecewise-linear (C0) — last segment has a small residual slope, then 0.")
    lines.append("static constexpr double kBlampUmax = %g;      // support radius (samples)." % L)
    lines.append("static constexpr int kBlampN = %d;          // [0,L] -> index [0,N-1]." % (N - 1))
    lines.append("static constexpr double kBlampLut[] = {")
    for i in range(0, N, 8):
        lines.append("    " + ", ".join("%.9f" % v for v in vals[i:i + 8]) + ",")
    lines.append("};")
    lines.append("// blampG(u) = linear-interp of the even LUT over |u| in [0,L], 0 outside (windowed).")

    # --- generation-consistency: re-parse the emitted LUT and compare to analytic ---
    s = "\n".join(lines)
    # Extract ONLY the LUT array body (between the declarator and its closing "};"),
    # so license/comment decimals (e.g. "Apache-2.0" -> "2.0") are not counted.
    array_line = next(ln for ln in lines if ln.startswith("static constexpr double kBlampLut"))
    aloc = s.index(array_line)
    body = s[aloc + len(array_line):s.index("};", aloc)]
    nums = [float(x) for x in re.findall(r'-?\d+\.\d+', body)]
    assert_ok(len(nums) == N, "emitted LUT size", "got %d want %d" % (len(nums), N))
    maxdiff = max(abs(nums[i] - vals[i]) for i in range(N))
    assert_ok(maxdiff <= 5e-10, "LUT consistent with analytic formula",
              "max|emitted-analytic|=%.3e" % maxdiff)
    print("  GENERATION CONSISTENCY PASS: emitted LUT (%d pts) matches analytic to %.3e" % (N, maxdiff))

    print(s)


if __name__ == "__main__":
    main()
