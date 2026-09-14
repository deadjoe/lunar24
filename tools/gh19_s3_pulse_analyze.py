#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# gh19_s3_pulse_analyze.py — task #118 (GH #19 S3): the pulse / PWM anti-aliasing analyzer.
#
# WHAT THIS MEASURES, AND WHAT IT REFUSES TO CLAIM.
#
# The metric is the 总诊断残差 (total diagnostic residual): the RMS of the difference between the
# product's own output and an independently constructed band-limited reference, expressed in dB
# relative to that reference's own RMS. It is deliberately NOT called "alias" anywhere: it contains
# the candidate's error, the reference construction's own error, and any gain/DC mismatch that was
# not removed. Where those have not been separated, the number is a diagnostic, not an attribution.
#
# THREE THINGS THAT WOULD EACH FAKE AN IMPROVEMENT, AND HOW THEY ARE EXCLUDED BY CONSTRUCTION:
#
#   1. A FITTED PHASE. A free phase offset absorbs exactly the effect under test: S2 measured a real
#      -9.92 dB improvement that a phase-fitted metric reported as +0.43 dB, sign reversed. So the
#      phase here is NEVER fitted. It comes from the probe's documented model `phi(n) = frac((n+1) *
#      f0 / sr)` (n = absolute frame index since load; the +1 is tick()'s advance-then-emit), and
#      the baseline arm is required to reproduce that model sample-for-sample before any residual is
#      trusted (`--align-check`, see below). ONLY the scale A is fitted.
#   2. A DELAY HIDDEN BY A SHIFT. Nothing is cross-correlated and nothing is shifted. A one-sample
#      shift must make the residual WORSE; `--self-check` asserts exactly that, so a metric that is
#      blind to timing cannot pass as sensitive.
#   3. GAIN OR DC DIFFERENCE REPORTED AS STRUCTURE. The fitted scale A, the measured fundamental
#      amplitude and the measured DC are PRINTED PER CELL rather than silently normalised away, so a
#      "improvement" that is really a level change is visible as such in the same row.
#
# THE TWO REFERENCE ROUTES, LISTED SEPARATELY BECAUSE THEY HAVE DIFFERENT ERROR STRUCTURES
# (the brief requires 参考构造 / 泄漏控制 / 延迟增益频率对齐 / 参考误差 per regime):
#
#   STATIC duty — ANALYTIC FOURIER, route "static". The reference is the exact band-limited pulse
#   series, x(p) = (2D-1) + 2*Re(sum_{k>=1} c_k e^{2*pi*i*k*p}) with c_k = (1 - e^{-2*pi*i*k*D})/(pi*i*k),
#   truncated at the LAST harmonic below sr/2. Construction: closed form, no samples read from the
#   product. Leakage: none — the window is never transformed, so there is no windowing and no
#   leakage floor at all; the "floor" of this route is float64 round-off (~1e-16 relative).
#   Delay/gain/frequency alignment: phase enters only as the model phase above; A is the one fitted
#   parameter; f0 is the probe's nominal target (verified against the measured ZCR frequency, printed
#   per cell). Reference error: the truncation is not an approximation — the removed harmonics are
#   exactly those above Nyquist, which is the DEFINITION of the band-limited ideal.
#
#   MOVING duty — HIGH-RATE MODEL + POLYPHASE DECIMATION, route "dynamic". Construction: the intended
#   continuous-time rectangle is rendered on a grid L times finer and low-passed at the output
#   Nyquist, then decimated; the FIR's group delay is removed EXACTLY and integrally (odd tap count,
#   output taken at high-rate index n*L + (Ntaps-1)//2), never as a fitted lag. Leakage control: the
#   decimation filter's transition band is placed ABOVE the output Nyquist so the passband stays flat
#   to sr/2, and the residual is additionally reported over a "resolved" band below the transition so
#   the top octave can be excluded where the reference is least certain. Reference error: quantified
#   per cell by the L-doubling convergence figure (`ref_conv_db`), which is the honest measure of how
#   much of the residual could be the reference rather than the product.
#
#   ⚠️ THE INTERPOLATION ASSUMPTION, STATED (@Codex 94b43e08 / 582c74ba). The moving-duty reference
#   holds the duty at its per-frame value (zero-order hold) and advances the phase continuously — that
#   is literally the DSP's own comparison (a continuous phase against a per-frame-read CV), so the
#   model is exact for the product's intent. Its convergence boundary is the DUTY'S OWN BANDWIDTH:
#   for the triangle-LFO cells the held duty is a fine staircase and the model converges as L grows;
#   for the SQUARE-LFO cells the duty JUMPS between adjacent frames, the intended waveform is then
#   genuinely discontinuous in duty, and NO finite-bandwidth reference is exact. Square cells are
#   therefore reported in their own block and are NOT counted as improved-coverage evidence anywhere
#   in this tool's output.
#
# Exit codes: 0 = all requested work completed; 3 = an instrument self-check failed (the metric is
# blind or inverted and NO result from this run may be quoted); 4 = a cell was missing/unreadable.

import argparse
import cmath
import math
import operator
import os
import struct
import sys

TWO_PI = 2.0 * math.pi


# ---------------------------------------------------------------------------- io

def read_raw(path):
    """The probe writes bare float64 little-endian, no header."""
    with open(path, "rb") as fh:
        buf = fh.read()
    n = len(buf) // 8
    return list(struct.unpack("<%dd" % n, buf[:n * 8]))


def read_trace(path):
    """gh19_s3_srcNNN.tsv: header + `frame<TAB>volts`, 17 significant digits."""
    out = []
    with open(path) as fh:
        for i, line in enumerate(fh):
            if i == 0:
                continue
            line = line.rstrip("\n")
            if not line:
                continue
            out.append(float(line.split("\t")[1]))
    return out


def max_abs_diff(a, b):
    """Exact per-index max |a-b|, or None when the two captures are not the same length. A length
    mismatch is NOT compared over the common prefix: two captures of one cell that differ in length
    are not the same signal, and comparing the shorter one would turn a truncated render into a clean
    identity."""
    if len(a) != len(b):
        return None
    m = 0.0
    for x, y in zip(a, b):
        d = abs(x - y)
        if d > m:
            m = d
    return m


def load_manifest(d):
    path = os.path.join(d, "gh19_s3_scenarios.tsv")
    with open(path) as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    head = lines[0].split("\t")
    rows = []
    for l in lines[1:]:
        f = l.split("\t")
        r = dict(zip(head, f))
        for k in ("sr_hz", "f_target_hz", "duty_param", "morph", "pw_depth", "f0_meas_hz",
                  "phi0_model", "peak", "warm", "win", "samples"):
            r[k] = float(r[k])
        r["warm"] = int(r["warm"])
        r["win"] = int(r["win"])
        r["samples"] = int(r["samples"])
        rows.append(r)
    return rows


# ---------------------------------------------------------------------------- fft

def fft(a):
    """Iterative radix-2 Cooley-Tukey, in place. len(a) must be a power of two. Pure Python on
    purpose: this repo's analyzer tooling has no numpy available, and a dependency that is missing
    at analysis time is a silent path to not running the check at all."""
    n = len(a)
    if n <= 1:
        return a
    if n & (n - 1):
        raise ValueError("fft length %d is not a power of two" % n)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        ang = -TWO_PI / length
        wl = complex(math.cos(ang), math.sin(ang))
        half = length >> 1
        for i in range(0, n, length):
            w = 1.0 + 0.0j
            for k in range(i, i + half):
                u = a[k]
                v = a[k + half] * w
                a[k] = u + v
                a[k + half] = u - v
                w *= wl
        length <<= 1
    return a


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


# ---------------------------------------------------------------------------- models

def duty_from_cv(cv, pw_base, pw_depth):
    """The product's PWM transfer, verbatim: effectiveDuty = clamp(basePW + depth*cv/10, 1e-3, 1-1e-3).
    Positive CV raises duty. This is a SOFTWARE PROVISIONAL, not a hardware fact (task #117), and it
    is reproduced here rather than guessed because the whole moving-duty reference depends on it."""
    d = pw_base + pw_depth * cv / 10.0
    return min(max(d, 1e-3), 1.0 - 1e-3)


def phi_at(n, f0, sr):
    """Model phase of absolute output frame n. tick() advances cumPitch_ and THEN emits, so the frame
    n sample carries the phase reached after n+1 increments."""
    return ((n + 1) * f0 / sr) % 1.0


# ---------------------------------------------------------------------------- settings

class Cell:
    def __init__(self, row, d):
        self.row = row
        self.id = row["id"]
        self.sr = row["sr_hz"]
        self.f0 = row["f_target_hz"]
        self.duty_param = row["duty_param"]
        self.side = row["side"]
        self.channel = row["channel"]
        self.path = row["path"]
        self.lfo_wave = row["lfo_wave"]
        self.pw_depth = row["pw_depth"]
        self.morph = row["morph"]
        self.phi0 = row["phi0_model"]
        self.warm = row["warm"]
        self.win = row["win"]
        self.x = read_raw(os.path.join(d, row["raw"]))
        self.trace = read_trace(os.path.join(d, row["src"])) if row["src"] != "-" else None

    def window(self):
        return self.x[self.warm:self.warm + self.win]


# ---------------------------------------------------------------------------- static reference

def ref_static(n0, count, f0, sr, duty):
    """Exact band-limited pulse, unit amplitude, mean (2D-1). No samples of the product are read and
    the window is never transformed, so this route has no leakage floor: its only error is float64
    round-off and the (definitional, not approximate) removal of harmonics above Nyquist."""
    kmax = int(math.floor((sr / 2.0) / f0))
    if kmax < 1:
        return None
    # c_k = (1 - e^{-2*pi*i*k*D}) / (pi*i*k);  x(p) = (2D-1) + 2*Re(sum_k c_k e^{2*pi*i*k*p}).
    # The phase is the model phase; harmonic k advances by 2*pi*k*f0/sr per frame exactly, so the
    # exponential is a pure rotation and the sum is accumulated directly (no trig per sample).
    step = [cmath.exp(complex(0.0, TWO_PI * k * f0 / sr)) for k in range(1, kmax + 1)]
    ck = [(1.0 - cmath.exp(complex(0.0, -TWO_PI * k * duty))) / complex(0.0, math.pi * k)
          for k in range(1, kmax + 1)]
    out = []
    for i in range(count):
        n = n0 + i
        p = ((n + 1) * f0 / sr) % 1.0
        s = 0.0 + 0.0j
        # z_k = e^{2*pi*i*k*p} evaluated from the closed-form phase (not accumulated), so the
        # reference cannot drift away from the phase the model check validated.
        for k in range(1, kmax + 1):
            s += ck[k - 1] * cmath.exp(complex(0.0, TWO_PI * k * p))
        out.append((2.0 * duty - 1.0) + 2.0 * s.real)
    return out


# ---------------------------------------------------------------------------- dynamic reference

def design_decimator(taps, L, beta):
    """Blackman-windowed sinc low-pass for decimation by L, cutoff fc = 0.5/L cycles per high-rate
    sample, i.e. AT the output Nyquist. Odd tap count so the group delay is an integer number of
    high-rate samples and can be removed exactly rather than fitted.

    WHAT THE CUTOFF BEING AT NYQUIST DOES AND DOES NOT BUY, because an earlier revision of this
    docstring claimed the second half and the claim was FALSE. It said the transition band sat ABOVE
    the output Nyquist so the passband stayed flat up to sr/2. A windowed sinc with fc at Nyquist
    necessarily has |H| = 0.5 there -- -6.0206 dB by construction -- which `--check-decimator`
    MEASURES (it prints -6.0206 dB for taps=2001, L=8, and the number is stable in taps because 0.5 at
    fc is a property of the ideal prototype, not of the window). So the response is already 6 dB down
    AT the output Nyquist and the transition band STRADDLES it, not sits above it: the -0.1 dB point is
    a hair BELOW Nyquist and the -60 dB point a hair ABOVE, with the whole rolloff squeezed into a very
    narrow interval around it. The accurate statement is that the passband is flat to well under
    0.1 dB out to roughly 0.94 of the output Nyquist (0.469 cycles/out, 20672 Hz at 44.1 kHz), after
    which the rolloff begins. `--check-decimator` prints the worst deviation over 100-5000 Hz (the
    band the product claim actually lives in) and over the effective full band rather than leaving a
    reader to infer flatness from the cutoff placement.

    Why fc at Nyquist is still the right choice here: the residual is what is being measured, and both
    the product output and the reference go through the SAME decimation in the comparison, so a gentle
    near-Nyquist rolloff is common-mode. A filter that rolled off WELL BELOW sr/2 would remove the
    near-Nyquist content this measurement exists to look at, so that would be worse, not better.

    `beta` IS DEAD AND IS NOT PART OF THE DESIGN. It is accepted for call-site compatibility only; the
    window is a fixed Blackman with the standard coefficients above and no beta-derived constant
    appears in this function. Three call sites pass 0.06. The docstring previously implied beta shaped
    the window. `--check-decimator` asserts the honest consequence: two different betas must produce
    IDENTICAL coefficients, so the day someone wires beta up, that check goes red instead of the
    parameter quietly starting to mean something."""
    assert taps % 2 == 1
    m = taps - 1
    fc = 0.5 / L
    h = []
    for i in range(taps):
        x = i - m / 2.0
        if x == 0.0:
            s = 2.0 * fc
        else:
            s = math.sin(TWO_PI * fc * x) / (math.pi * x)
        # Blackman
        w = 0.42 - 0.5 * math.cos(TWO_PI * i / m) + 0.08 * math.cos(2 * TWO_PI * i / m)
        h.append(s * w)
    tot = sum(h)
    return [v / tot for v in h]


def resp_db(h, L, f_out):
    """|H| in dB of a decimation filter at an OUTPUT-band frequency, in cycles per output sample.
    Evaluated with an incremental phasor rather than a per-tap trig call: thousands of taps times
    thousands of probe frequencies is otherwise minutes of pure-Python trigonometry.

    Module level rather than nested inside the self-check, because the EFFECTIVE BAND EDGE of every
    band-limited residual against a decimated reference is defined by this response. A quantity that
    sets a reported band must not live inside the block that happens to report it."""
    fh = f_out / float(L)
    wr = math.cos(TWO_PI * fh)
    wi = -math.sin(TWO_PI * fh)
    zr, zi = 1.0, 0.0
    ar = ai = 0.0
    for v in h:
        ar += v * zr
        ai += v * zi
        zr, zi = zr * wr - zi * wi, zr * wi + zi * wr
    m = math.hypot(ar, ai)
    return 20.0 * math.log10(m) if m > 0 else -math.inf


def decimator_edge01(h, L, netaps=801):
    """The MEASURED -0.1 dB endpoint of a decimation filter's response, in cycles per OUTPUT sample.

    MEASURED, not derived from the cutoff placement. design_decimator puts fc exactly at the output
    Nyquist, so |H| = -6.0206 dB there by construction, and "flat up to the cutoff" is not a claim
    this filter can support. The endpoint has to be found by search over the actual coefficients, and
    it is what sets the upper edge of the effective-full-band residual columns.

    USING THIS AS A BAND EDGE IS NOT THE SAME AS CLAIMING REFERENCE CONVERGENCE, and the report keeps
    them apart (@Codex 144b1339 #2): this number is a property of the reconstruction FILTER's design.
    The reference's convergence is the movement between the L and 2L references, reported separately
    as refconv_rms_db / refconv_peak and as the 2L band-metric columns."""
    e = 0.0
    for i in range(netaps):
        f = 0.5 * i / (netaps - 1)
        if abs(resp_db(h, L, f)) <= 0.1:
            e = f
    return e


def ref_dynamic(n0, count, f0, sr, duty_of_frame, L, h):
    """The band-limited ideal of the SAME intended waveform the DSP produces, on the duty model stated
    in the header: phase advances continuously, duty is held per output frame. Rendered at Lx, low-
    passed at sr/2, decimated by L, with the FIR's (taps-1)/2 group delay removed exactly.

    TWO THINGS ABOUT THIS ROUTE ARE LOAD-BEARING, and both are reported rather than assumed:

    (1) THE DECIMATOR MUST BE DESIGNED FOR THE L ACTUALLY USED, AND THE CONVERGENCE COLUMN IS WHAT
    FINDS OUT WHEN IT IS NOT. The first version of this function was called from the matrix with a
    filter built for L and then reused for the 2L run, i.e. it decimated by 2L with a cutoff at twice
    its own output Nyquist. That folds the 2L images straight back into the output band, so the
    "refined reference" was wrong in a way that has nothing to do with the signal, and the column
    reported the misconfiguration as non-convergence (ref_conv_db pinned at -0.20 dB no matter the
    signal). An earlier diagnosis of mine blamed the tap count instead; MEASURING IT FALSIFIED THAT:
    raising taps 65 -> 2001 moved the residual only -24.08 -> -23.72 dB and left ref_conv_db at
    -0.20 dB, which a genuinely under-length filter cannot do. The tap count was never the cause.
    With the decimator redesigned per L (and a still-long 2001 taps, since this is offline) the two
    references agree to 0.1 dB. Report the convergence figure as the RMS of (L-reference minus
    2L-reference) relative to ref_rms, with the peak alongside: max|d| is a large number on a
    two-level signal even when the RMS movement is far below the residual, so quoting the peak alone
    reads as "the reference error exceeds the residual" and is not the comparable quantity.

    (2) THE REFERENCE IS BUILT FROM THE FULL HIGH-RATE RECORD ONCE, then read at the decimated
    positions. Building it per output sample inside the tap loop is the same arithmetic at ~250x the
    cost, which is what made a properly long filter look unaffordable in pure Python. The values
    summed are identical.

    Convergence boundary of the model itself, stated precisely because an earlier revision of this
    docstring got the mechanism wrong in two ways.

    (a) What the per-frame duty hold IS: `duty_of_frame(floor(t))` makes the duty a piecewise-constant
    function of time with jumps at output-frame boundaries -- a ZOH of the control parameter. It does
    NOT turn a step into a ramp, and it does NOT lock the oscillator's threshold crossing to the output
    grid. The crossing is still where the continuously-advancing phase `phi(m/L)` meets the held duty,
    so its TIME is resolved on the high-rate grid at 1/L of an output frame and refining L does refine
    edge placement. What L cannot refine is the duty trajectory's own quantization to control-rate
    frames. So the only thing wrong with a square-LFO cell here is that the reference's duty jumps at
    frame boundaries while the device's jumps wherever its own control path puts it -- a timing
    alignment question between two step functions, not a smoothing question.

    (b) What the reference therefore IS: the band-limited ideal of the product's own per-frame duty
    sampling, i.e. a FROZEN-DUTY-PER-FRAME approximation of a continuously moving edge. It is the
    right reference for what is under test (the correction's behaviour under this duty model) and is
    NOT the band-limited ideal of a continuously-moving edge. Whether a smoothly-moving PWM edge is
    representable at all is a separate question this instrument does not answer, and the earlier
    "smooth PWM is landable" reading is WITHDRAWN to this frozen-duty approximation pending a
    measurement that actually varies the edge continuously. Square-LFO cells are excluded from
    improvement claims for the timing reason in (a); they are still rendered and reported."""
    taps = len(h)
    delay = (taps - 1) // 2
    # High-rate indices needed: for output i, the correlation reads h[j] * v[base - j], j = 0..taps-1,
    # with base = (n0 + i)*L + delay. So the covered range is [first_base - taps + 1, last_base].
    first_base = n0 * L + delay
    last_base = (n0 + count - 1) * L + delay
    lo = first_base - taps + 1
    hi = last_base
    # v is the ZOH-duty ideal pulse on the high-rate grid. Phase model is the product's own:
    # phi(n) = frac((n + 1) * f0 / sr) for OUTPUT frame index n from load; here t is in output frames.
    v = []
    for m in range(lo, hi + 1):
        t = m / float(L)
        fr = int(math.floor(t))
        d = duty_of_frame(fr)
        p = ((1.0 + t) * f0 / sr) % 1.0
        v.append(1.0 if p < d else -1.0)
    out = []
    mul = operator.mul
    for i in range(count):
        base = first_base + i * L
        s = base - lo
        seg = v[s - taps + 1:s + 1]
        seg.reverse()
        out.append(sum(map(mul, h, seg)))
    return out


# ---------------------------------------------------------------------------- metric

def residual_stats(x, r):
    """Fit ONLY the scale, never the phase. Returns the fitted scale, the residual RMS, the reference
    RMS and the residual in dB relative to the reference."""
    num = 0.0
    den = 0.0
    for a, b in zip(x, r):
        num += a * b
        den += b * b
    if den == 0.0:
        return None
    A = num / den
    se = 0.0
    sr_ = 0.0
    for a, b in zip(x, r):
        e = a - A * b
        se += e * e
        sr_ += (A * b) * (A * b)
    n = len(x)
    res_rms = math.sqrt(se / n)
    ref_rms = math.sqrt(sr_ / n)
    if res_rms == 0.0:
        return {"A": A, "res_rms": 0.0, "ref_rms": ref_rms, "res_db": -math.inf}
    return {"A": A, "res_rms": res_rms, "ref_rms": ref_rms,
            "res_db": 20.0 * math.log10(res_rms / ref_rms)}


def hann(n):
    return [0.5 - 0.5 * math.cos(TWO_PI * i / n) for i in range(n)]


# ------------------------------------------------------------------- mixed reference (S6 baseline)

def pulse_node_weight(morph):
    """wave_map::pulseWeight(kRingEqual, morph) in closed form: on stretch 3 ([0.75, 1.0]) the local
    coordinate u = (morph - 0.75)/0.25, and the pulse node's weight in the convex blend is u. Off
    stretch 3 the weight is 0. Reproduced here exactly as the header defines it, because the mixed
    reference's whole construction depends on which node's weight is which."""
    if morph < 0.75:
        return 0.0
    u = (morph - 0.75) / 0.25
    return 1.0 if u > 1.0 else u


def ref_mix_static(n0, count, f0, sr, duty, u, kmax_override=None):
    """The band-limited ideal of the MIXED coordinate (wave_map's stretch 3), i.e. what the ringmix
    cells are a rendering of: the convex blend of the band-limited TRIANGLE and the band-limited
    PULSE, both truncated at the last harmonic below Nyquist.

    The triangle's series is not assumed, it is derived from Vco::waveformSampleAt's own closed form
    triangleShape(p) = 4|p-0.5|-1. That function is even about p = 0.5 with zero mean, so

        triangleShape(p) = sum_{k odd} -8/(pi^2 k^2) * cos(2*pi*k*(p - 0.5))

    (verified numerically against the naive shape to 3e-8 away from the corners, with the residual
    at the corners being the usual Gibbs ringing of a truncated series). The pulse part is the exact
    series ref_static uses, so at u = 1 this function reproduces ref_static sample for sample and at
    u = 0 it is the band-limited triangle alone. Those two limits are asserted in --self-check, which
    is what keeps this a second reference rather than a second guess.

    WHY A MIXED CELL NEEDS ITS OWN REFERENCE: the triangle node in this product is corrected by the
    existing BLAMP and the pulse node by the S3 BLEP, each scaled by its own weight, so the mixed
    output is a blend of two separately-corrected shapes rather than a correction of a blend. Feeding
    a mixed cell to the pure-pulse reference would compare the product against a waveform it is not
    trying to be, and any dB from that comparison is meaningless."""
    kmax = int(math.floor((sr / 2.0) / f0)) if kmax_override is None else int(kmax_override)
    if kmax < 1:
        return None
    step = [cmath.exp(complex(0.0, TWO_PI * k * f0 / sr)) for k in range(1, kmax + 1)]
    ck = [(1.0 - cmath.exp(complex(0.0, -TWO_PI * k * duty))) / complex(0.0, math.pi * k)
          for k in range(1, kmax + 1)]
    out = []
    for i in range(count):
        n = n0 + i
        p = ((n + 1) * f0 / sr) % 1.0
        sp = 0.0 + 0.0j
        for k in range(1, kmax + 1):
            sp += ck[k - 1] * cmath.exp(complex(0.0, TWO_PI * k * p))
        bl_pulse = (2.0 * duty - 1.0) + 2.0 * sp.real
        bl_tri = 0.0
        for k in range(1, kmax + 1, 2):
            bl_tri += -8.0 / (math.pi * math.pi * k * k) * math.cos(TWO_PI * k * (p - 0.5))
        out.append((1.0 - u) * bl_tri + u * bl_pulse)
    return out


def res_split(sig, sr, f0, guard_bins=3.0):
    """Split the residual's energy into its HARMONIC part and its FLOOR part.

    This is the decomposition that answers the question a single dB number cannot: did the candidate
    reduce the aliasing, or did it just trade alias energy for harmonic error? For a naive pulse the
    aliased images land on NON-harmonic frequencies, so the floor carries most of the residual; a
    correction that only smeared the edges would move energy INTO the harmonic bands instead.

    Leakage control: the residual is Hann-windowed before the transform (an unwindowed transform of a
    non-integer-period window leaks as 1/df and would smear the strong harmonic mismatch across the
    whole spectrum).

    THE GUARD BAND WIDTH IS SET BY THE WINDOW, NOT BY THE HARMONIC SPACING, and getting that wrong is
    not a detail — it silently destroys the measurement. A guard of +-f0/2 is the tempting choice
    ("half a harmonic either side") but the harmonics are exactly f0 apart, so +-f0/2 bands TILE the
    whole spectrum and every bin is classified as harmonic: the split then reports harm = everything
    and floor = -30 dB of nothing, regardless of what the signal does. The guard only needs to cover
    the analysis window's own main lobe, so it is 3 bins (a Hann main lobe is 4 bins wide); everything
    outside is floor, which is where a folded alias actually lives.

    WHAT THE TWO NUMBERS ARE, AND WHAT THEY ARE NOT (@Codex e144b61 review, item 5).
    Both are 10*log10(share / total) with share + other = total in LINEAR power, so the two LINEAR
    shares sum to 1 -- the two dB values do NOT sum to 0, and an earlier revision of both this
    docstring and the printed matrix header said they did. (A share of one half is -3.01 dB, not
    -0.00 dB; the cells below show -0.04 / -20.30, whose linear shares are 0.9908 + 0.0093 = 1.0001.)
    Second, and more load-bearing: the partition is by frequency NEIGHBOURHOOD, not by alias
    IDENTITY. k is round(f/f0), so a folded image that lands within the guard of a legitimate
    harmonic is classified as harmonic -- at 48 kHz / 220 Hz the image that folds to 220 Hz is
    counted as the fundamental, not as an alias. The split therefore cannot separate "alias" from
    "harmonic error"; it bounds how much of the residual sits AWAY from the harmonic comb. That is
    why the report carries it as a SHAPE column beside res_db and never as an alias measurement,
    and why floor_db down is necessary but not sufficient evidence of reduced aliasing."""
    n = len(sig)
    N = next_pow2(n)
    w = hann(n)
    a = [complex(sig[i] * w[i], 0.0) for i in range(n)] + [0j] * (N - n)
    fft(a)
    harm = 0.0
    floor = 0.0
    guard = guard_bins * sr / float(N)
    for m in range(0, N // 2 + 1):
        f = m * sr / float(N)
        p = (a[m] * a[m].conjugate()).real
        mult = 0.5 if (m == 0 or m == N // 2) else 1.0
        if m == 0:
            harm += p * mult        # DC belongs to the harmonic group (k = 0)
            continue
        k = round(f / f0)
        if k >= 1 and abs(f - k * f0) <= guard:
            harm += p * mult
        else:
            floor += p * mult
    tot = harm + floor
    if tot == 0.0:
        return -math.inf, -math.inf
    return (10.0 * math.log10(harm / tot), 10.0 * math.log10(floor / tot))


# ------------------------------------------------- the band-limited OUTPUT residual (contract item 3)

# The contract's band, in Hz. Named once, so the two columns, the prose and the delta tool's pin
# cannot drift apart.
B1_LO_HZ, B1_HI_HZ = 100.0, 5000.0


def band_decl(kind, rule, cycles, extra=""):
    """The MACHINE-READABLE band declaration for a block, printed once inside its fence.

    WHY THIS LINE EXISTS (@Codex 144b1339 #2, "写出有效带宽"). The two band columns are only meaningful
    against a stated band, and "100-5000 Hz" says nothing about the effective-full-band edge -- which
    is sr/2 for an analytic reference and a MEASURED fraction of sr for a decimated one. Printing the
    rule and the derived constant makes the band a declared fact instead of something a reader has to
    recover from the code, and it gives the delta tool something to PIN ACROSS ARMS: if the two arms
    declared different bands, every band delta between them would be a comparison of two different
    measurements, and that must be a failure rather than a number.

    `cycles` is in cycles per OUTPUT sample, which is the one unit that is the same at every sample
    rate in the grid; the Hz value is sr*cycles and is printed in the prose beside it."""
    return ("BAND-DECL %s b1_lo=%.3f b1_hi=%.3f eff_lo=%.3f eff_rule=%s eff_hi_cycles=%.9f%s"
            % (kind, B1_LO_HZ, B1_HI_HZ, 0.0, rule, cycles, extra))


def band_residual_db(x, r, A, sr, f_lo, f_hi, w=None):
    """The band-limited OUTPUT RESIDUAL: band power of the residual `x - A*r` over band power of the
    fitted reference `A*r`, over the SAME band, the same window and the same normalization.

    WHY THE OUTPUT AND NOT THE FILTER. The contract asks for 100-5000 Hz and the effective full band
    as properties OF THE RESIDUAL (@Codex 144b1339 #2). Measuring the decimation filter's passband
    flatness answers a different question -- it says the reference is trustworthy in that band, which
    is a PRECONDITION for a residual measurement, not the result of one. Both are reported, in
    separate places, and neither is allowed to stand in for the other. The filter's own response is
    asserted in --self-check (d); these columns are what the candidate is actually judged on.

    ONE DENOMINATOR, DECLARED. The denominator is the band power of the SAME fitted reference the
    numerator's residual is taken against -- same fitted A, same window, same band, same bin weights.
    Nothing else is comparable cell to cell: dividing by the reference's FULL-band power, or by an
    unfitted reference, would make the number move with the cell's spectral shape instead of with its
    error. A is fitted scale-only, exactly as in residual_stats, so this metric inherits res_db's
    blindness to a level change and does not double-count one.

    BIN WEIGHTING IS res_split's, DELIBERATELY. A one-sided spectrum doubles every interior bin to
    account for its negative-frequency twin; DC and Nyquist have no twin and take 0.5 where the
    interior takes 1.0. Using the same constant keeps the band columns and the floor/harm split
    agreeing on what a bin at the edge is worth, instead of the report carrying two conventions that
    differ exactly where aliasing folds to.

    THE BAND IS SELECTED IN Hz, NOT IN BINS. A band named 100-5000 Hz must be the same physical band
    at every sample rate in the grid -- that is the property the contract's 100-5000 Hz requirement is
    about. Selecting bins [0.0023*N, 0.113*N] would have made the band move with N, which is the one
    thing this column exists to hold fixed. Bin centres are m*sr/N, so the edges are resolved to one
    bin; the resolved edge is printed by the caller rather than left to be inferred.

    THE ONE-SIDED 2/N COEFFICIENT, AND WHY `pe`/`pq` ARE MEAN SQUARES (@Codex 4940d6ea item 1). The
    FFT here is UNNORMALIZED, so a one-sided band sum is N/2 times the signal's own sum of squares
    (Parseval: sum_{m=0}^{N/2} mult(m)|X[m]|^2 = (N/2) * sum_i |x_i|^2, because every interior bin
    carries twice its own power and the two half-spectrum bins carry it once). Both sums are therefore
    multiplied by 2/N before being returned. WITHOUT that coefficient `pe` and `pq` still have the
    right RATIO -- both sides would carry the same missing factor, which is exactly why the dB column
    never moved and why the omission survived -- but they are DFT sums, not powers: their value scales
    with the transform length, so a reader could not compare them against any other energy number in
    this report. The coefficient is not decoration; it is what makes the two intermediates readable.

    THE HANN POWER NORMALIZATION CANCELS IN THE RATIO, AND IS APPLIED ANYWAY. Dividing each band sum
    by sum(w^2) makes each side a mean-square in device units rather than a raw DFT sum, so the two
    intermediates are physical and can be read on their own. Saying so matters: a reader who believed
    the normalization was doing work in the ratio would attribute the numbers to the wrong thing. What
    does NOT cancel is the window's AMPLITUDE WEIGHTING, which is real -- which is why this function's
    full-band value under a Hann window differs slightly from the unwindowed res_db.

    THE TWO NORMALIZATIONS TOGETHER ARE AN IDENTITY, AND THAT IS HOW THEY ARE PINNED. For the full
    band, 2/N and sum(w^2) combine into the windowed mean square itself,

        (2/N) * [sum_{m=0}^{N/2} mult(m)|X[m]|^2] / sum_i w_i^2  ==  sum_i (w_i y_i)^2 / sum_i w_i^2

    exactly, with y the signal the transform was taken of. --self-check (e) asserts BOTH halves of
    that: the ratio against res_db (rectangular window) AND each of `pe`/`pq` against a direct
    time-domain mean square computed without any transform. The first alone is blind to the 2/N
    coefficient -- that is its whole blind spot, and the reason the second exists.

    Returns None when the reference has no power in the band. A ratio against zero is not a number,
    and printing it as -inf dB would read as an infinitely good correction."""
    n = len(x)
    N = next_pow2(n)
    if w is None:
        w = hann(n)
    we = [complex((x[i] - A * r[i]) * w[i], 0.0) for i in range(n)] + [0j] * (N - n)
    wq = [complex((A * r[i]) * w[i], 0.0) for i in range(n)] + [0j] * (N - n)
    fft(we)
    fft(wq)
    g = 0.0
    for wi in w:
        g += wi * wi
    pe = 0.0
    pq = 0.0
    nbins = 0
    for m in range(0, N // 2 + 1):
        f = m * sr / float(N)
        if f < f_lo or f > f_hi:
            continue
        mult = 0.5 if (m == 0 or m == N // 2) else 1.0
        nbins += 1
        pe += mult * (we[m] * we[m].conjugate()).real
        pq += mult * (wq[m] * wq[m].conjugate()).real
    if g <= 0.0 or pq <= 0.0 or nbins == 0:
        return None
    # The one-sided 2/N coefficient -- see the note above. Applied AFTER the band selection so the
    # band comparison is unaffected, and applied to BOTH sides so the dB column is unaffected too.
    c = 2.0 / float(N)
    return {"db": (10.0 * math.log10(pe / pq) if pe > 0.0 else -math.inf),
            "pe": c * pe / g, "pq": c * pq / g, "nbins": nbins, "df": sr / float(N)}


def measure_fundamental_dc(sig, n0, f0, sr):
    """Direct projection at f0 (and the mean), reported PER CELL so a level or DC change cannot be
    mistaken for a structural improvement."""
    re = 0.0
    im = 0.0
    dc = 0.0
    for i, v in enumerate(sig):
        p = ((n0 + i + 1) * f0 / sr) % 1.0
        re += v * math.cos(TWO_PI * p)
        im += v * math.sin(TWO_PI * p)
        dc += v
    n = len(sig)
    return 2.0 * math.hypot(re, im) / n, dc / n


# ---------------------------------------------------------------------------- alignment proof

def align_check(cell, f0, sr, tol_rel=1e-6):
    """THE GATE THAT LICENSES EVERY NUMBER THIS TOOL PRINTS.

    Reconstruct the model's own sample sequence and require the product's output to equal it, except
    where the model itself says something is happening. This simultaneously pins the phase convention,
    the duty, the scale and (for dynamic cells) the per-frame consumption of the CV trace — without
    it, a residual could be small because the model happened to line up with whatever the product did.

    THE SCALE IS DERIVED ROBUSTLY, NOT BY LEAST SQUARES, and that distinction is the whole point. A
    least-squares fit on a pulse lets the handful of anomalous edge samples drag the fitted scale off
    the true one, after which EVERY ordinary sample "disagrees" by that small amount and the report
    says 16376/16384 mismatched — the exact opposite of the truth. The scale is therefore taken as the
    median of |x| (a pulse takes only one magnitude away from its edges), and the mismatches are then
    the anomaly rather than the fit's residual.

    Mismatches are CLASSIFIED, not just counted:
      - `wrap`: model phase within 1e-9 of 0 or 1. Measured, not assumed: the C++ accumulator
        `cumPitch_ += step` lands about 1 ulp below the integer at exact-wrap frames while the closed
        form returns exactly 0.0, so the two legitimately disagree by one full step there.
      - `edge`: within dt of an edge (phase 0 or phase D). This is where a floor-limited candidate is
        ALLOWED to differ from the naive sequence, so it is where the candidate arm's mismatches must
        live — and the baseline arm's must not, which is what makes this one check serve both arms.
      - `unexplained`: everything else. Must be ZERO. This is the load-bearing number.

    A FAILURE IS NAMED, NOT COUNTED (@Codex e144b61 review, item 4). `unexplained > 0` is the gate,
    but a gate that reports only a count cannot be reconciled with a specific product claim: "some
    samples are wrong" is consistent with a dozen different causes, including a bug in this very
    function. So the FIRST unexplained sample is returned whole -- its absolute frame index `first_n`
    (the probe's own sample counter, so it can be found in the rendered arm), its in-window offset
    `first_i`, the model phase and duty read AT THAT FRAME, and the residual there. The timing claim
    under test ("the correction consumes the CV of frame n, not of frame n-1") is a claim about a
    specific frame index, so the failure has to be reported at a frame index to be about it at all.

    `first_edge_start` is returned for the same reason on the other side: a candidate whose
    mismatches are all inside an edge window is PASSING, and the report has to be able to show that
    the first mismatch it does have sits in the `edge` class rather than merely assert that the class
    was non-empty somewhere.
    """
    x = cell.window()
    n0 = cell.warm
    if cell.trace is not None:
        dwt = [duty_from_cv(v, cell.duty_param, cell.pw_depth) for v in cell.trace]
        d_of = lambda n: dwt[n] if 0 <= n < len(dwt) else dwt[-1]
    else:
        d_of = lambda n: cell.duty_param
    mags = sorted(abs(v) for v in x)
    scale = mags[len(mags) // 2]           # median |x| = the pulse's own magnitude
    if scale == 0.0:
        return {"ok": False, "scale": 0.0, "wrap": 0, "edge": 0, "unexplained": len(x),
                "frac_ok": 0.0, "worst": 0.0, "n": len(x), "dt": 0.0,
                "first_n": n0, "first_i": 0, "first_phase": 0.0, "first_duty": 0.0,
                "first_err": 0.0, "first_edge_n": None, "first_edge_err": 0.0}
    dt = f0 / sr
    tol = tol_rel * scale
    wrap = edge = unexplained = 0
    worst = 0.0
    first_n = first_i = None
    first_phase = first_duty = first_err = 0.0
    first_edge_n = None
    first_edge_err = 0.0
    for i in range(len(x)):
        n = n0 + i
        p = phi_at(n, f0, sr)
        d = d_of(n)
        m = 1.0 if p < d else -1.0
        e = abs(x[i] - scale * m)
        if e > worst:
            worst = e
        if e <= tol:
            continue
        near = min(abs(p), abs(p - 1.0), abs(p - d), abs(p - 1.0 + d))
        if near < 1e-9:
            wrap += 1
        elif near < dt:
            edge += 1
            if first_edge_n is None:
                first_edge_n, first_edge_err = n, e
        else:
            unexplained += 1
            if first_n is None:
                first_n, first_i = n, i
                first_phase, first_duty, first_err = p, d, e
    frac_ok = 1.0 - (wrap + edge + unexplained) / float(len(x))
    return {"ok": unexplained == 0, "scale": scale, "wrap": wrap, "edge": edge,
            "unexplained": unexplained, "frac_ok": frac_ok, "worst": worst, "n": len(x), "dt": dt,
            "first_n": first_n, "first_i": first_i, "first_phase": first_phase,
            "first_duty": first_duty, "first_err": first_err,
            "first_edge_n": first_edge_n, "first_edge_err": first_edge_err}


# ---------------------------------------------------------------------------- main

# The report is written to stdout AND, when --out is given, to that file. --out used to be accepted
# and then ignored, which is worse than not having it: a caller who passed it would read the old
# file (or nothing) and believe the run had produced those numbers. One buffer, so the file and the
# console cannot disagree.
_REPORT = []


def say(*parts):
    line = " ".join(str(p) for p in parts)
    print(line)
    _REPORT.append(line)


def write_report(path):
    with open(path, "w") as fh:
        fh.write("\n".join(_REPORT) + "\n")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", required=True, help="directory written by gh19_s3_pulse_probe --out")
    ap.add_argument("--label", default="arm")
    ap.add_argument("--out", default=None)
    ap.add_argument("--static-only", action="store_true")
    ap.add_argument("--dynamic-cells", type=int, default=8,
                    help="how many moving-duty cells get the full high-rate reference")
    ap.add_argument("--oversample", type=int, default=8)
    ap.add_argument("--taps", type=int, default=65)
    ap.add_argument("--conv-cells", type=int, default=3,
                    help="how many of the referenced dynamic cells also get the 2L reference, to "
                         "report reference convergence rather than assume it")
    ap.add_argument("--align-check", action="store_true")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args(argv)

    rows = load_manifest(args.arm)
    cells = []
    missing = []
    for r in rows:
        if r["signal"] != "-":
            missing.append((r["id"], r["signal"]))
            continue
        cells.append(Cell(r, args.arm))

    say("== gh19_s3_pulse_analyze: arm=%s cells=%d missing=%d ==" %
          (args.label, len(cells), len(missing)))
    for cid, why in missing:
        say("  MISSING %s [%s]" % (cid, why))

    rc = 0
    if missing:
        rc |= 4

    by_id = {c.id: c for c in cells}

    # ---------------- alignment proof (baseline arm) ----------------
    if args.align_check:
        say("\n-- alignment proof: product output vs the model sequence at the robust scale --")
        say("   (pure-pulse and moving-duty cells only: the S6 `vco_a_ringmix` cells are a morph MIX,")
        say("    so a single-pulse model is not their model and applying it there measures nothing.)")
        say("   A failure is NAMED at a frame index: frame= is the probe's own absolute sample counter")
        say("   (the same counter the rendered arm is indexed by), i= is the offset inside the window.")
        nbad = 0
        nchecked = 0
        bad_ids = []
        for c in cells:
            if c.path == "vco_a_ringmix":
                continue
            nchecked += 1
            a = align_check(c, c.f0, c.sr)
            say("  %-52s scale=%.6f wrap=%-4d edge=%-5d unexplained=%-4d frac_ok=%.6f" %
                  (c.id, a["scale"], a["wrap"], a["edge"], a["unexplained"], a["frac_ok"]))
            if a["first_edge_n"] is not None:
                say("      ALIGN-FIRST-EDGE %s frame=%d err=%.9f (permitted class)" %
                      (c.id, a["first_edge_n"], a["first_edge_err"]))
            if not a["ok"]:
                nbad += 1
                bad_ids.append(c.id)
                say("      ALIGN-FIRST-BAD %s frame=%d i=%d phase=%.12f duty=%.12f err=%.9f" %
                      (c.id, a["first_n"], a["first_i"], a["first_phase"], a["first_duty"],
                       a["first_err"]))
        say("  -> %d/%d cells align (unexplained == 0)" % (nchecked - nbad, nchecked))
        # The machine-readable form of the same verdict, so a caller does not have to parse prose and
        # so an EMPTY bad list is distinguishable from a check that never ran.
        say("ALIGN-GATE ok=%d checked=%d bad=%d first_bad=%s" %
              (1 if nbad == 0 else 0, nchecked, nbad, ",".join(bad_ids) if bad_ids else "-"))
        if nbad:
            rc |= 3

    # ---------------- static residual matrix ----------------
    static_cells = [c for c in cells
                    if c.path in ("vco_a_pulse", "vco_b_pulse", "vco_a_pulsehi")]
    say("\n-- BEGIN MATRIX static")
    say("-- STATIC residual matrix (route: analytic Fourier) --")
    say("   res_db = 总诊断残差, dB vs the reference's own RMS. floor_db / harm_db are the two shares")
    say("   of the residual in LINEAR power: floor_db = 10*log10(floor/total), harm_db likewise, with")
    say("   floor + harm = total. The two LINEAR shares therefore sum to 1; the two dB NUMBERS DO NOT")
    say("   sum to 0 (an earlier revision of this header claimed they did -- corrected). The partition")
    say("   is by FREQUENCY NEIGHBOURHOOD (k = round(f/f0)), not by alias identity: a folded image")
    say("   that lands on a legitimate harmonic is counted as harmonic, so this split cannot identify")
    say("   aliases, only bound how much of the residual sits away from the harmonic comb. floor_db")
    say("   down is necessary but NOT sufficient evidence that aliasing fell.")
    say("   res_1k5k_db / res_effbd_db are the BAND-LIMITED OUTPUT RESIDUALS the contract asks for:")
    say("   10*log10(band power of (x - A*r) / band power of (A*r)), one declared denominator, Hann")
    say("   window, power-normalized by sum(w^2), and res_split's DC/Nyquist half-weight. They are NOT")
    say("   the decimator's passband flatness -- that is a precondition, asserted in --self-check (d).")
    say("   The effective band here is the analytic reference's own limit: the series is truncated at")
    say("   the last harmonic under Nyquist, so the reference is trustworthy over (0, sr/2] exactly.")
    say("  " + band_decl("static", "output_nyquist", 0.5) +
        "  (eff_hi = sr/2 = %.0f Hz @44100, %.0f Hz @96000)" % (44100 / 2.0, 96000 / 2.0))
    say("  " + "  ".join("%-13s" % h for h in
                           ("id", "sr", "f0", "duty", "A", "fund", "dc", "res_db",
                            "res_1k5k_db", "res_effbd_db", "floor_db", "harm_db", "kmax")))
    static_rows = []
    static_skipped = 0
    for c in static_cells:
        x = c.window()
        r = ref_static(c.warm, len(x), c.f0, c.sr, c.duty_param)
        if r is None:
            say("  %-13s no harmonic below Nyquist" % c.id)
            static_skipped += 1
            continue
        st = residual_stats(x, r)
        fund, dc = measure_fundamental_dc(x, c.warm, c.f0, c.sr)
        e = [a - st["A"] * b for a, b in zip(x, r)]
        hdb, fdb = res_split(e, c.sr, c.f0)
        b1 = band_residual_db(x, r, st["A"], c.sr, B1_LO_HZ, B1_HI_HZ)
        eb = band_residual_db(x, r, st["A"], c.sr, 0.0, c.sr / 2.0)
        kmax = int(math.floor((c.sr / 2.0) / c.f0))
        static_rows.append((c, st, fund, dc, hdb, fdb, b1, eb))
        say("  %-13s %6.0f %6.0f %5.3f %7.4f %7.4f %8.5f %8.2f %10.2f %10.2f %8.2f %8.2f %5d" %
              (c.id, c.sr, c.f0, c.duty_param, st["A"], fund, dc, st["res_db"],
               b1["db"], eb["db"], fdb, hdb, kmax))
    say("-- END MATRIX static declared=%d emitted=%d skipped=%d" %
          (len(static_cells), len(static_rows), static_skipped))

    # ---------------- mixed-coordinate matrix (S6 baseline) ----------------
    # These cells are pulse only PARTIALLY weighted (wave_map stretch 3, triangle -> pulse), and they
    # are reported in their OWN block against their OWN reference. They are never counted together
    # with the pure-pulse numbers: a mixed coordinate is not a pure pulse, and its residual contains
    # the still-naive saw/sine/triangle content of the blend as well as the pulse's.
    mix_cells = [c for c in cells if c.path == "vco_a_ringmix"]
    if mix_cells:
        say("\n-- BEGIN MATRIX mixed")
        say("-- MIXED-coordinate matrix (sweep fraction 4*morph-3 on stretch 3; S6 baseline) --")
        say("   reference = (1-u)*BL_triangle + u*BL_pulse, both truncated below Nyquist. u is the")
        say("   PULSE node's weight, so u = 1 would be the pure-pulse column above; these cells are")
        say("   NOT that, and their residual is not comparable to it.")
        say("   The two band columns carry the SAME declared denominator as the static block's, taken")
        say("   against THIS block's own mixed reference -- so they are comparable within this block")
        say("   and not across the fence. Reference and effective band are the analytic ones.")
        say("  " + band_decl("mixed", "output_nyquist", 0.5) +
            "  (eff_hi = sr/2 = %.0f Hz @44100, %.0f Hz @96000)" % (44100 / 2.0, 96000 / 2.0))
        say("  " + "  ".join("%-13s" % h for h in
                               ("id", "sr", "f0", "u", "A", "fund", "dc", "res_db",
                                "res_1k5k_db", "res_effbd_db", "floor_db", "harm_db", "kmax")))
        mix_cells.sort(key=lambda c: (c.sr, c.morph))
        mix_rows = 0
        mix_skipped = 0
        for c in mix_cells:
            u = pulse_node_weight(c.morph)
            x = c.window()
            r = ref_mix_static(c.warm, len(x), c.f0, c.sr, c.duty_param, u)
            if r is None:
                say("  %-13s no harmonic below Nyquist" % c.id)
                mix_skipped += 1
                continue
            st = residual_stats(x, r)
            fund, dc = measure_fundamental_dc(x, c.warm, c.f0, c.sr)
            e = [a - st["A"] * b for a, b in zip(x, r)]
            hdb, fdb = res_split(e, c.sr, c.f0)
            b1 = band_residual_db(x, r, st["A"], c.sr, B1_LO_HZ, B1_HI_HZ)
            eb = band_residual_db(x, r, st["A"], c.sr, 0.0, c.sr / 2.0)
            kmax = int(math.floor((c.sr / 2.0) / c.f0))
            mix_rows += 1
            say("  %-13s %6.0f %6.0f %5.3f %7.4f %7.4f %8.5f %8.2f %10.2f %10.2f %8.2f %8.2f %5d" %
                  (c.id, c.sr, c.f0, u, st["A"], fund, dc, st["res_db"],
                   b1["db"], eb["db"], fdb, hdb, kmax))
        say("-- END MATRIX mixed declared=%d emitted=%d skipped=%d" %
              (len(mix_cells), mix_rows, mix_skipped))

    # ---------------- cable-route identity matrix (connect) ----------------
    # WHY THIS BLOCK EXISTS. The probe renders each moving-duty spec twice: once with the PWM cable
    # carried inside the saved DeviceState (CableMode::kState) and once with the same plain state and
    # the cable made AFTER the load (CableMode::kConnect). Until this revision those 8 cells were
    # produced and consumed by NOTHING -- the probe's own comment claimed a byte-identity check that
    # no code in the tree performed. Two cells with no consumer are cells whose value is not evidence.
    #
    # THE CLAIM IS NOT DECORATIVE. It is what makes "the saved patch and the patched one are one
    # signal" measurable rather than asserted, and it is the check that the restore path and the
    # connect path carry the SAME stimulus to the SAME sink. BOTH the audio and the per-frame LFO
    # trace are compared: a cable that reached the sink one frame late, or on the wrong side, can
    # still produce audio whose residual looks plausible.
    #
    # The fences are emitted UNCONDITIONALLY, even with zero cells, so a run that stops producing the
    # family shows declared=0 against the manifest's 8 rather than a block that quietly disappeared.
    conn_cells = [c for c in cells if c.path == "vco_a_dynpwm_connect"]
    conn_cells.sort(key=lambda c: c.id)
    say("\n-- BEGIN MATRIX connect")
    say("-- CABLE-ROUTE IDENTITY matrix (state-carried cable vs cable made after the load) --")
    say("   twin = the SAME stimulus rendered through the other route. maxdiff_audio is over the whole")
    say("   raw capture, maxdiff_trace over the per-frame LFO-A CV record. Both must be exactly 0.")
    say("  " + "  ".join("%-13s" % h for h in
                           ("id", "twin", "sr", "f0", "maxdiff_audio", "maxdiff_trace", "n")))
    conn_rows = 0
    conn_skipped = 0
    conn_bad = []
    for c in conn_cells:
        # The twin is derived by stripping the route suffix, so a cell that reached this fence WITHOUT
        # that suffix would silently be compared against a truncated id -- typically nothing, which
        # would read as a declared skip rather than as the malformed id it is. Fail it as bad instead:
        # the identity claim is unavailable, and "unavailable" must never look like "nothing to see".
        if not c.id.endswith("_viaConnect"):
            conn_bad.append(c.id)
            conn_skipped += 1
            say("      CONNECT-ID-MALFORMED %s: this block's cells carry a _viaConnect suffix, so a "
                "twin cannot be derived from this id" % c.id)
            continue
        twin_id = c.id[: -len("_viaConnect")]
        t = by_id.get(twin_id)
        if t is None or t.path != "vco_a_dynpwm" or not t.x:
            say("  %-13s no source trace" % c.id)
            conn_skipped += 1
            continue
        d_audio = max_abs_diff(c.x, t.x)
        d_trace = max_abs_diff(c.trace, t.trace) if (c.trace and t.trace) else None
        conn_rows += 1
        say("  %-13s %-13s %6.0f %6.0f %12s %12s %5d" %
            (c.id, twin_id, c.sr, c.f0,
             "LENGTH" if d_audio is None else "%.6g" % d_audio,
             "-" if d_trace is None else "%.6g" % d_trace, len(c.x)))
        if d_audio != 0.0 or (t.trace and c.trace and d_trace != 0.0):
            conn_bad.append(c.id)
            say("      CONNECT-DIFFERS %s twin=%s audio=%s trace=%s" %
                (c.id, twin_id,
                 "LENGTH" if d_audio is None else "%.6g" % d_audio,
                 "-" if d_trace is None else "%.6g" % d_trace))
        if (c.trace is None) != (t.trace is None):
            conn_bad.append(c.id)
            say("      CONNECT-TRACE-PRESENCE %s twin=%s: one route recorded a trace and the other "
                "did not" % (c.id, twin_id))
    say("-- END MATRIX connect declared=%d emitted=%d skipped=%d" %
          (len(conn_cells), conn_rows, conn_skipped))
    say("CONNECT-GATE ok=%d checked=%d bad=%d %s" %
          (1 if not conn_bad else 0, conn_rows, len(conn_bad),
           ",".join(conn_bad) if conn_bad else "-"))
    if conn_bad:
        rc |= 8

    # ---------------- A/B isolation contrast matrix (ab_asymmetry) ----------------
    # The probe patches A's PWM sink only, and requires B to be untouched. B is left a pure pulse at
    # the SAME base duty as the shipped cell vco_b_pulse_<sr>_<f>_pw50 (morph 1.0, cv_amt 0), so the
    # two renders must be byte-identical. This is the cheapest check that "A -> B modulation" is
    # really ISOLATED: A's waveform IS changing while these are captured, and B must not see it
    # through the cv_amt = 0 route. Like `connect`, these 2 cells had no consumer before this
    # revision. Reported, not arm-delta'd: an asymmetry here is a wiring defect that no candidate
    # accumulates away, so it is a per-arm identity check.
    asym_cells = [c for c in cells if c.path == "vco_b_ab_asymmetry"]
    asym_cells.sort(key=lambda c: c.id)
    say("\n-- BEGIN MATRIX ab_asymmetry")
    say("-- A/B ISOLATION CONTRAST matrix (A patched, B required untouched) --")
    say("   twin = vco_b_pulse_<sr>_<f>_pw50, the same carrier recipe rendered with A unpatched.")
    say("   maxdiff_audio must be exactly 0; a non-zero value is cable or correction LEAKAGE to B.")
    say("  " + "  ".join("%-13s" % h for h in
                           ("id", "twin", "sr", "f0", "maxdiff_audio", "maxdiff_trace", "n")))
    asym_rows = 0
    asym_skipped = 0
    asym_bad = []
    for c in asym_cells:
        twin_id = "vco_b_pulse_%d_%d_pw50" % (c.sr, c.f0)
        t = by_id.get(twin_id)
        if t is None or not t.x:
            say("  %-13s no source trace" % c.id)
            asym_skipped += 1
            continue
        d_audio = max_abs_diff(c.x, t.x)
        asym_rows += 1
        say("  %-13s %-13s %6.0f %6.0f %12s %12s %5d" %
            (c.id, twin_id, c.sr, c.f0,
             "LENGTH" if d_audio is None else "%.6g" % d_audio, "-", len(c.x)))
        if d_audio != 0.0:
            asym_bad.append(c.id)
            say("      ABSYM-DIFFERS %s twin=%s audio=%s" %
                (c.id, twin_id, "LENGTH" if d_audio is None else "%.6g" % d_audio))
    say("-- END MATRIX ab_asymmetry declared=%d emitted=%d skipped=%d" %
          (len(asym_cells), asym_rows, asym_skipped))
    say("ABSYM-GATE ok=%d checked=%d bad=%d %s" %
          (1 if not asym_bad else 0, asym_rows, len(asym_bad),
           ",".join(asym_bad) if asym_bad else "-"))
    if asym_bad:
        rc |= 8

    # ---------------- instrument self-check ----------------
    if args.self_check:
        say("\n-- instrument self-check --")
        say("   TWO categories, because they answer different questions and one of them is not a")
        say("   pass/fail on the metric at all:")
        say("   (a) MUST WORSEN   - the metric has to be sensitive to a real misalignment. A metric")
        say("                       that cannot see a one-sample shift cannot see a late edge either.")
        say("   (b) MUST BE INVARIANT - a known symmetry of the metric. Reference SCALE is one: the")
        say("                       residual fits A by design, so scaling the reference changes nothing.")
        say("                       That is not a blind spot, it is the separation of structure from")
        say("                       level - and it is exactly why A, fund and dc are printed per cell.")
        say("                       (Verified rather than assumed: if this invariance FAILED, the fit")
        say("                       would be broken and every residual would be suspect.)")
        probe = None
        for c in cells:
            if c.path == "vco_a_pulse" and c.sr == 44100.0 and c.f0 == 220.0 and c.duty_param == 0.5:
                probe = c
                break
        if probe is None:
            say("  NO PROBE CELL FOUND - self-check did not run")
            rc |= 3
        else:
            x = probe.window()
            r = ref_static(probe.warm, len(x), probe.f0, probe.sr, probe.duty_param)
            base = residual_stats(x, r)["res_db"]
            say("  probe cell %s  baseline res_db=%.3f" % (probe.id, base))

            say("  (a) must worsen:")
            sens = []
            sens.append(("shift +1 sample",
                         ref_static(probe.warm + 1, len(x), probe.f0, probe.sr, probe.duty_param)))
            sens.append(("shift +8 samples",
                         ref_static(probe.warm + 8, len(x), probe.f0, probe.sr, probe.duty_param)))
            sens.append(("time reversal", list(reversed(r))))
            sens.append(("duty off by 1e-3",
                         ref_static(probe.warm, len(x), probe.f0, probe.sr,
                                    probe.duty_param + 1e-3)))
            sens.append(("f0 off by 0.1%%",
                         ref_static(probe.warm, len(x), probe.f0 * 1.001, probe.sr,
                                    probe.duty_param)))
            for name, rr in sens:
                v = residual_stats(x, rr)["res_db"]
                ok = v > base + 0.05
                if not ok:
                    rc |= 3
                say("    %-22s res_db=%8.3f  delta=%+8.3f  %s" %
                      (name, v, v - base, "WORSE (ok)" if ok else "*** BLIND/INVERTED ***"))

            say("  (b) must be invariant:")
            for name, f in (("reference scale x1.01", 1.01), ("reference scale x0.5", 0.5)):
                rr = [v * f for v in r]
                st2 = residual_stats(x, rr)
                d = abs(st2["res_db"] - base)
                ok = d < 1e-6
                if not ok:
                    rc |= 3
                say("    %-22s res_db=%8.3f  delta=%.3g  A=%.6f  %s" %
                      (name, st2["res_db"], d, st2["A"],
                       "INVARIANT (ok)" if ok else "*** FIT BROKEN ***"))

            say("  (c) mixed reference must reduce to its two endpoints:")
            rp = ref_static(probe.warm, len(x), probe.f0, probe.sr, probe.duty_param)
            r1 = ref_mix_static(probe.warm, len(x), probe.f0, probe.sr, probe.duty_param, 1.0)
            e1 = max(abs(a - b) for a, b in zip(rp, r1)) if r1 else float("inf")
            ok1 = e1 < 1e-9
            if not ok1:
                rc |= 3
            say("    %-22s max|ref_mix(u=1) - ref_static| = %.3g  %s" %
                  ("u = 1 == pure pulse", e1, "EXACT (ok)" if ok1 else "*** MISMATCH ***"))
            # u = 0 must be the band-limited TRIANGLE, and here the honest check is CONVERGENCE, not
            # an absolute tolerance: the series is truncated at the last harmonic under Nyquist, so
            # its deviation from 4|p-0.5|-1 is the truncation tail, which scales like 4/(pi^2*kmax)
            # and therefore SHRINKS when the limit is raised. A fixed tolerance would either be so
            # loose it proves nothing or so tight it fails on a correct reference; requiring the
            # deviation to be inside the analytic tail bound AND to fall when kmax doubles tests the
            # property that actually distinguishes a truncated series from a wrong one.
            def tri_dev(km):
                rr = ref_mix_static(probe.warm, len(x), probe.f0, probe.sr, probe.duty_param, 0.0, km)
                w = 0.0
                for i, v in enumerate(rr):
                    p = ((probe.warm + i + 1) * probe.f0 / probe.sr) % 1.0
                    if 0.05 < p < 0.45 or 0.55 < p < 0.95:   # exclude the corner Gibbs band
                        w = max(w, abs(v - (4.0 * abs(p - 0.5) - 1.0)))
                return w, sum(rr) / len(rr)
            km = int(math.floor((probe.sr / 2.0) / probe.f0))
            d1, dc1 = tri_dev(km)
            d2, _ = tri_dev(2 * km)
            tail = 4.0 / (math.pi * math.pi * km)
            ok0 = d1 < tail and d2 < d1 and abs(dc1) < tail
            if not ok0:
                rc |= 3
            say("    %-22s kmax=%d dev=%.3g (tail bound %.3g) -> kmax=%d dev=%.3g  mean=%.3g  %s" %
                  ("u = 0 == triangle", km, d1, tail, 2 * km, d2, dc1,
                   "CONVERGING (ok)" if ok0 else "*** NOT THE BAND-LIMITED TRIANGLE ***"))

            # ------------------------------------------------------------------ (d) DECIMATOR RESPONSE
            # THE MEASUREMENT THAT KEEPS design_decimator's DOCSTRING HONEST. That docstring used to
            # claim the transition band sat entirely above the output Nyquist so the passband was flat
            # up to sr/2, and a reviewer measured -6.0206 dB AT the output Nyquist instead. This block
            # measures the response and asserts the three claims that are now made about it, plus the
            # one consequence of `beta` being dead. Each assertion is falsifiable by a specific edit:
            # moving fc, lengthening/shortening the rolloff, or wiring beta into the window.
            say("  (d) decimator response (taps=%d, L=%d; documented by design_decimator):"
                % (args.taps, args.oversample))
            hdec = design_decimator(args.taps, args.oversample, 0.06)

            netaps = 801
            grid = [0.5 * i / (netaps - 1) for i in range(netaps)]
            db = [resp_db(hdec, args.oversample, f) for f in grid]

            nyq = resp_db(hdec, args.oversample, 0.5)
            ok_nyq = abs(nyq - (-6.0206)) < 0.05
            if not ok_nyq:
                rc |= 3
            say("    %-34s %+9.4f dB (ideal windowed sinc at its own cutoff = -6.0206) %s" %
                  ("|H| at the output Nyquist", nyq,
                   "AS DOCUMENTED (ok)" if ok_nyq else "*** DOCSTRING CLAIM REFUTED ***"))

            # The band the product claim lives in, in Hz, at each rate the grid uses.
            for sr_hz in (44100.0, 96000.0):
                worst = 0.0
                worst_f = 0.0
                for f, d in zip(grid, db):
                    hz = f * sr_hz
                    if 100.0 <= hz <= 5000.0 and abs(d) > worst:
                        worst, worst_f = abs(d), hz
                ok_pb = worst < 0.01
                if not ok_pb:
                    rc |= 3
                say("    %-34s worst |dev| = %.6f dB at %.0f Hz  %s" %
                      ("passband 100-5000 Hz @ %.0f Hz" % sr_hz, worst, worst_f,
                       "FLAT (ok)" if ok_pb else "*** NOT FLAT ***"))

            # Effective full band: how far a 0.1 dB budget actually reaches, and where the stopband
            # is. Reported rather than asserted from the cutoff placement, which is the mistake the
            # docstring made. The endpoint comes from decimator_edge01 -- the SAME function the
            # dynamic block's effective band edge uses -- so these assertions are about the band
            # actually being reported, not about a second copy of the same search.
            edge01 = decimator_edge01(hdec, args.oversample)
            stop60 = None
            for f, d in zip(grid, db):
                if d <= -60.0:
                    stop60 = f
                    break
            band_frac = edge01 / 0.5
            ok_band = band_frac > 0.90
            if not ok_band:
                rc |= 3
            say("    %-34s %.4f cycles/out = %.2f%% of the output Nyquist (%.0f Hz @44.1k)" %
                  ("effective full band (|dev| <= 0.1 dB)", edge01, 100.0 * band_frac, edge01 * 44100.0))
            say("    %-34s %s" %
                  ("-60 dB reached at",
                   "NOT inside the output band at this grid resolution (%.5f cycles/out steps) -- the "
                   "rolloff is narrower than the grid, which is itself the point"
                   % (0.5 / (netaps - 1)) if stop60 is None
                   else "%.4f cycles/out (%s Nyquist)"
                        % (stop60, "below" if stop60 < 0.5 else "at/above")))
            say("    %-34s the rolloff straddles Nyquist, so the docstring's 'transition band above "
                "Nyquist' is FALSE" % "")

            # beta is dead: two different betas must give byte-identical coefficients. If someone ever
            # wires beta in, THIS goes red instead of the parameter silently acquiring meaning.
            same = (design_decimator(args.taps, args.oversample, 0.06)
                    == design_decimator(args.taps, args.oversample, 1.7))
            if not same:
                rc |= 3
            say("    %-34s %s" %
                  ("beta is unused (0.06 == 1.7)", "IDENTICAL (ok)" if same
                   else "*** DIFFERENT: beta now affects the design, update the docstring ***"))

            # (d2) THE TRANSITION BAND, SAMPLED rather than described. The docstring's old claim was
            # about where this band sits, so name the frequencies and print what is actually there: the
            # measurement is the refutation, and a reader should not have to take the crossing search's
            # word for the shape.
            say("    transition band, sampled (cycles per output sample; 0.5 = output Nyquist):")
            for f in (0.49, 0.4999, 0.5, 0.5001, 0.502, 0.51):
                say("      f_out=%-8.4f %+10.4f dB   (%.0f Hz @44.1k)" %
                      (f, resp_db(hdec, args.oversample, f), f * 44100.0))

            # (d3) GAIN. The coefficients are normalized to sum 1, so |H(0)| must be exactly 0 dB --
            # this is what makes the decimated reference the same amplitude scale as the signal it is
            # compared against, and it is the one place a broken normalization would hide.
            g0 = resp_db(hdec, args.oversample, 0.0)
            ok_g0 = abs(g0) < 1e-9
            if not ok_g0:
                rc |= 3
            say("    %-34s %+12.9f dB  %s" %
                  ("DC gain |H(0)| (normalized to 1)", g0,
                   "UNITY (ok)" if ok_g0 else "*** NOT UNITY: reference scale is wrong ***"))

            # (d4) CONVERGENCE IN TAPS: a longer filter narrows the transition, so the 0.1 dB endpoint
            # must move toward Nyquist as taps grow. That is what distinguishes a converged design from
            # one whose rolloff is limited by truncation -- the earlier tap-count diagnosis got this
            # backwards, so it is asserted rather than assumed.
            def edge01_for(nt):
                return decimator_edge01(design_decimator(nt, args.oversample, 0.06), args.oversample)
            e_short, e_long = edge01_for(501), edge01_for(4001)
            ok_conv = e_short <= edge01 < e_long
            if not ok_conv:
                rc |= 3
            say("    %-34s %.4f (501 taps) <= %.4f (%d taps) < %.4f (4001 taps)  %s" %
                  ("0.1 dB endpoint, convergence", e_short, edge01, args.taps, e_long,
                   "CONVERGING (ok)" if ok_conv else "*** NOT CONVERGING IN TAPS ***"))

            # ------------------------------------------------------------------ (e) BAND RESIDUAL COLUMNS
            # The two contract columns are the newest thing in this report and the easiest to get
            # subtly wrong, so the instrument proves them against an identity rather than against a
            # second implementation of the same idea.
            #
            # THE IDENTITY IS EXACT AND IT PINS FOUR THINGS AT ONCE. With a RECTANGULAR window and the
            # full output band, `band_residual_db` must equal `res_db` to machine precision, by
            # Parseval: sum over the one-sided bands of mult(m)|X[m]|^2 is (N/2)*sum_i |x_i|^2, and the
            # sum(w^2) normalization and the N/2 both cancel in the ratio, leaving sum(e_i^2)/sum(q_i^2)
            # -- which is exactly (res_rms/ref_rms)^2. If the band selection, the DC/Nyquist half
            # weight, the power normalization or the zero-padding were wrong in ANY way that changed
            # the total, this fails. (It cannot catch a wrong BAND, only a wrong total; the band is
            # pinned by BAND-DECL and by the containment check below.)
            nprobe = len(x)
            ident = band_residual_db(x, r, residual_stats(x, r)["A"], probe.sr, 0.0,
                                     probe.sr / 2.0, [1.0] * nprobe)
            d_id = abs(ident["db"] - base) if ident else float("inf")
            ok_id = d_id < 1e-6
            if not ok_id:
                rc |= 3
            say("    %-34s res_db=%.6f  band(full, rect)=%s  delta=%.3g  %s" %
                  ("(e) full band, rect window == res_db",
                   base, ("%.6f" % ident["db"]) if ident else "None", d_id,
                   "EXACT (ok)" if ok_id else "*** Parseval identity BROKEN ***"))

            # PARSEVAL IN ABSOLUTE UNITS, NOT ONLY AS A RATIO (@Codex 4940d6ea item 1). The check just
            # above pins the TOTAL, and a missing one-sided 2/N coefficient CANCELS out of it -- both
            # sides of that ratio would be short by the same factor, so the omission is invisible
            # there. It therefore needs its own assertion, taken in a different direction: compare
            # `pe`/`pq` THEMSELVES against a windowed mean square computed DIRECTLY in the time domain,
            # with no transform anywhere in the path. Two windows are used so the 2/N and the
            # sum(w^2) normalizations cannot cover for each other -- under a rectangular window the
            # second of those is just n, the one value at which a wrong coefficient could hide.
            A_fit = residual_stats(x, r)["A"]
            for lbl, ww in (("hann", hann(nprobe)), ("rect", [1.0] * nprobe)):
                gg = 0.0
                for wi in ww:
                    gg += wi * wi
                du = dq = 0.0
                for i in range(nprobe):
                    eu = ww[i] * (x[i] - A_fit * r[i])
                    eq = ww[i] * (A_fit * r[i])
                    du += eu * eu
                    dq += eq * eq
                du /= gg
                dq /= gg
                got = band_residual_db(x, r, A_fit, probe.sr, 0.0, probe.sr / 2.0, ww)
                if got is None or not (dq > 0.0):
                    rc |= 3
                    say("    %-34s no band value to reconcile (*** instrument failure ***)" % lbl)
                    continue
                eu_a = abs(got["pe"] - du)
                eq_a = abs(got["pq"] - dq)
                # Relative to the band's own scale, with an absolute floor scaled by the reference
                # power so a near-zero residual cannot turn a physical comparison into 0/0.
                ok_p = (eq_a < 1e-9 * dq) and (eu_a < 1e-9 * max(du, 1e-6 * dq))
                if not ok_p:
                    rc |= 3
                say("    %-34s pe=%.9g vs %.9g (d=%.2g)  pq=%.9g vs %.9g (d=%.2g)  %s" %
                      ("(e) Parseval absolute, %s window" % lbl,
                       got["pe"], du, eu_a, got["pq"], dq, eq_a,
                       "MEAN SQUARES AGREE (ok)" if ok_p
                       else "*** pe/pq ARE NOT MEAN SQUARES: check the 2/N coefficient ***"))

            # The Hann-windowed full-band value differs from res_db by the window's AMPLITUDE
            # weighting alone. Reported rather than asserted away, because that difference is the
            # honest price of the leakage control and a reader is entitled to its size. The bound is
            # deliberately loose (this is a stationary periodic signal, not a general one).
            hannfb = band_residual_db(x, r, residual_stats(x, r)["A"], probe.sr, 0.0,
                                      probe.sr / 2.0, hann(nprobe))
            d_h = abs(hannfb["db"] - base) if hannfb else float("inf")
            ok_h = d_h < 1.0
            if not ok_h:
                rc |= 3
            say("    %-34s res_db=%.6f  band(full, hann)=%s  delta=%.3g  %s" %
                  ("(e) full band, hann window", base,
                   ("%.6f" % hannfb["db"]) if hannfb else "None", d_h,
                   "within the window's amplitude weighting (ok)" if ok_h
                   else "*** HANN FULL BAND FAR FROM res_db: check the normalization ***"))

            # The two columns must be NESTED: 100-5000 Hz has to sit inside the effective band, or the
            # narrow column would be reporting a band the wide one has already declared out of scope.
            # At 44100 Hz / Nyquist 22050 the containment is not close, but stating it as a check is
            # what stops the two band definitions from drifting apart in a later edit.
            cont = band_residual_db(x, r, residual_stats(x, r)["A"], probe.sr, B1_LO_HZ, B1_HI_HZ)
            ok_c = (B1_LO_HZ >= 0.0 and B1_HI_HZ <= probe.sr / 2.0 and cont is not None)
            if not ok_c:
                rc |= 3
            say("    %-34s %s  bins=%s  df=%.6f Hz  %s" %
                  ("(e) 100-5000 inside (0, sr/2]",
                   "100 >= 0 and 5000 <= %.0f" % (probe.sr / 2.0),
                   cont["nbins"] if cont else "-", cont["df"] if cont else -1.0,
                   "NESTED (ok)" if ok_c else "*** BAND OUTSIDE THE EFFECTIVE BAND ***"))

            # Sensitivity OF THE NEW COLUMNS, not just of res_db. A band column that stayed flat while
            # the waveform moved would be reporting the band and not the error, and the two columns
            # are the ones the contract asks to be judged on -- so they get the same must-worsen test
            # res_db gets, against their OWN Hann-windowed baselines (cont for 100-5000, hannfb for
            # the effective band), not against res_db's.
            #
            # WHAT "MUST WORSEN" MEANS HERE AND WHY IT IS NOT NEEDED TO WORSEN BY THE SAME AMOUNT. The
            # 100-5000 Hz column deliberately excludes everything above 5 kHz, which is where a naive
            # pulse's folded images mostly land, so a misalignment that wrecks the full-window residual
            # can move this column by MORE than res_db moves. That is the column working, not a
            # defect -- the assertion is on the SIGN and it is made separately for each column rather
            # than by comparing the two.
            say("    (e) sensitivity of the band columns (each must worsen, like res_db):")
            sens_ok = (cont is not None and hannfb is not None)
            if not sens_ok:
                # No baseline means no sensitivity statement is possible. Say that, and fail: a
                # silently skipped sensitivity block reads exactly like a passing one.
                say("      *** a band baseline is unavailable (cont ok=%s, hannfb ok=%s): the columns' "
                    "sensitivity was NOT evaluated ***" % (cont is not None, hannfb is not None))
                rc |= 3
            sens_cases = (("shift +1 sample",
                           ref_static(probe.warm + 1, len(x), probe.f0, probe.sr, probe.duty_param)),
                          ("shift +8 samples",
                           ref_static(probe.warm + 8, len(x), probe.f0, probe.sr, probe.duty_param)),
                          ("time reversal", list(reversed(r))),
                          ("duty off by 1e-3",
                           ref_static(probe.warm, len(x), probe.f0, probe.sr,
                                      probe.duty_param + 1e-3)))
            for name, rr in (sens_cases if sens_ok else ()):
                stp = residual_stats(x, rr)
                if stp is None:
                    say("      %-22s *** degenerate reference: sensitivity NOT evaluated ***" % name)
                    rc |= 3
                    continue
                bp = band_residual_db(x, rr, stp["A"], probe.sr, B1_LO_HZ, B1_HI_HZ)
                ep = band_residual_db(x, rr, stp["A"], probe.sr, 0.0, probe.sr / 2.0)
                worse = (stp["res_db"] > base + 0.05
                         and bp is not None and bp["db"] > cont["db"] + 0.05
                         and ep is not None and ep["db"] > hannfb["db"] + 0.05)
                if not worse:
                    rc |= 3
                say("      %-22s res_db=%8.3f (%+7.3f)  b1=%8.3f (%+7.3f)  eb=%8.3f (%+7.3f)  %s" %
                      (name, stp["res_db"], stp["res_db"] - base,
                       bp["db"] if bp else float("nan"),
                       (bp["db"] - cont["db"]) if bp else float("nan"),
                       ep["db"] if ep else float("nan"),
                       (ep["db"] - hannfb["db"]) if ep else float("nan"),
                       "ALL THREE WORSEN (ok)" if worse else "*** A COLUMN IS BLIND ***"))

    # ---------------- dynamic residual matrix ----------------
    if not args.static_only:
        say("\n-- BEGIN MATRIX dynamic")
        say("-- DYNAMIC residual matrix (route: high-rate ZOH-duty model + polyphase decimation) --")
        dyn = [c for c in cells if c.path in ("vco_a_dynpwm", "vco_b_dynpwm")]
        # The full reference is expensive; it is applied to a declared subset and the rest are
        # reported as un-referenced rather than silently omitted. Triangle-LFO cells come first: the
        # per-frame duty hold models them faithfully, whereas a square LFO's duty step is smeared by
        # the hold, so square-LFO cells are reported but excluded from improvement claims.
        dyn.sort(key=lambda c: (c.lfo_wave != "tri", c.sr, c.f0))
        subset = dyn[:args.dynamic_cells]
        h = design_decimator(args.taps, args.oversample, 0.06)
        h2 = design_decimator(args.taps, args.oversample * 2, 0.06)
        say("  L=%d, taps=%d; declared reference subset: %d of %d moving-duty cells; "
              "convergence-checked: %d" %
              (args.oversample, args.taps, len(subset), len(dyn), min(args.conv_cells, len(subset))))
        say("  reference movement is scaled by the fitted A and divided by ref_rms, so"
              " refconv_rms_db / refconv_peak are in the SAME units as res_db (device units)")
        # ---- the effective band of a DECIMATED reference, measured rather than assumed ----
        # The reference is rendered at Lx and low-passed by design_decimator, whose cutoff sits AT the
        # output Nyquist and is therefore 6 dB down there. Content between the filter's -0.1 dB
        # endpoint and sr/2 is attenuated in the REFERENCE and not in the product, so including it
        # would charge the candidate for the reference's own rolloff. The effective band stops at the
        # measured endpoint.
        #
        # THE COARSER OF THE TWO FILTERS SETS THE BAND FOR BOTH COLUMNS. The 2L convergence reference
        # has a slightly higher endpoint; using the L endpoint for both keeps every number inside the
        # 0.1 dB band of the WORSE filter, so the L-vs-2L band columns differ by the reference's own
        # movement and not by one of them being measured through its rolloff.
        #
        # WHAT THIS IS NOT (@Codex 144b1339 #2). The -0.1 dB endpoint is a property of the reconstruction
        # FILTER's design. That it sits near Nyquist, or that it moves toward Nyquist when taps or L
        # increase, is NOT evidence that the reference converged -- a longer filter and a better
        # reference are different claims. Reference convergence is refconv_rms_db / refconv_peak and the
        # L-vs-2L band columns, reported separately and never replaced by this endpoint.
        edge_L = decimator_edge01(h, args.oversample)
        edge_2L = decimator_edge01(h2, args.oversample * 2)
        edge_eff = min(edge_L, edge_2L)
        srs = sorted({c.sr for c in cells})
        worst_b1_hz = edge_eff * min(srs)
        ok_nest = worst_b1_hz > B1_HI_HZ
        say("  the 100-5000 Hz column needs no truncation here only if the NARROWEST effective band in")
        say("  this grid contains 5000 Hz: %.0f Hz (@%.0f Hz) %s 5000 Hz -> %s" %
            (worst_b1_hz, min(srs), ">" if ok_nest else "<=",
             "NESTED, the two band columns are consistent" if ok_nest else
             "*** 5000 Hz IS OUTSIDE THE EFFECTIVE BAND: the b1 column would report a band the "
             "reference is not valid over, so it is refused rather than printed ***"))
        if not ok_nest:
            rc |= 3
        say("  " + band_decl("dynamic", "decimator_0.1db", edge_eff,
                             " edge_L=%.9f edge_2L=%.9f" % (edge_L, edge_2L)) +
            "  (eff_hi = %.0f Hz @44100, %.0f Hz @96000)" % (edge_eff * 44100.0, edge_eff * 96000.0))
        say("  " + "  ".join("%-14s" % hh for hh in
                               ("id", "sr", "f0", "A", "fund", "dc", "res_db",
                                "res_1k5k_db", "res_effbd_db", "refconv_rms_db", "refconv_peak",
                                "res_1k5k2L_db", "res_effbd2L_db", "n")))
        dyn_rows = 0
        dyn_skipped = 0
        for ci, c in enumerate(subset):
            if c.trace is None:
                say("  %-14s no source trace" % c.id)
                dyn_skipped += 1
                continue
            dwt = [duty_from_cv(v, c.duty_param, c.pw_depth) for v in c.trace]
            d_of = lambda n, t=dwt: t[n] if 0 <= n < len(t) else t[-1]
            x = c.window()
            r = ref_dynamic(c.warm, len(x), c.f0, c.sr, d_of, args.oversample, h)
            st = residual_stats(x, r)
            fund, dc = measure_fundamental_dc(x, c.warm, c.f0, c.sr)
            b1 = band_residual_db(x, r, st["A"], c.sr, B1_LO_HZ, B1_HI_HZ)
            eb = band_residual_db(x, r, st["A"], c.sr, 0.0, edge_eff * c.sr)
            # Convergence: refine the REFERENCE (2L, with the decimator redesigned for 2L) and report
            # how far it MOVED. Two columns, because they answer different questions and only one of
            # them is comparable to res_db.
            #
            # UNITS: both columns are in the RESIDUAL's units, i.e. device units. The residual is
            # `x - A*r`, and ref_rms is rms(A*r), so both are device-scale. The raw reference movement
            # `r - r2` is in REFERENCE units, and comparing it against a device-scale ref_rms
            # understates it by exactly 20*log10|A| -- about 6 dB at the fitted A ~ 0.5. An earlier
            # revision did that, so its movement column was not comparable to res_db, which is the one
            # thing the column exists for. Scale the movement by the SAME fitted A before comparing.
            if ci < args.conv_cells:
                r2 = ref_dynamic(c.warm, len(x), c.f0, c.sr, d_of, args.oversample * 2, h2)
                d = [st["A"] * (a - b) for a, b in zip(r, r2)]
                crms = math.sqrt(sum(v * v for v in d) / len(d))
                cpeak = max(abs(v) for v in d)
                crms_db = (20.0 * math.log10(crms / st["ref_rms"]) if crms > 0 else -math.inf)
                # The two band columns re-measured against the 2L reference. Reported so the reader
                # can see whether the band numbers are a property of the candidate or of the reference
                # they were taken against -- a metric that moves when only the REFERENCE improves is
                # measuring the reference.
                b1_2 = band_residual_db(x, r2, st["A"], c.sr, B1_LO_HZ, B1_HI_HZ)
                eb_2 = band_residual_db(x, r2, st["A"], c.sr, 0.0, edge_eff * c.sr)
                dyn_rows += 1
                say("  %-14s %6.0f %6.0f %7.4f %7.4f %8.5f %8.2f %10.2f %10.2f %14.2f %12.6f "
                    "%11.2f %12.2f %5d" %
                      (c.id, c.sr, c.f0, st["A"], fund, dc, st["res_db"], b1["db"], eb["db"],
                       crms_db, cpeak,
                       b1_2["db"] if b1_2 else -math.inf,
                       eb_2["db"] if eb_2 else -math.inf, len(x)))
            else:
                dyn_rows += 1
                say("  %-14s %6.0f %6.0f %7.4f %7.4f %8.5f %8.2f %10.2f %10.2f %14s %12s "
                    "%11s %12s %5d" %
                      (c.id, c.sr, c.f0, st["A"], fund, dc, st["res_db"], b1["db"], eb["db"],
                       "-", "-", "-", "-", len(x)))
        unreferenced = [c.id for c in dyn if c not in subset]
        # The list comes BEFORE the END fence, not after it. The parser reconciles the fence against
        # everything it read between the fences, so a list printed past the fence is a list no
        # reconciliation covers -- and an un-reconciled list that is silently empty is exactly the
        # failure mode this fence exists to prevent. Inside the block, the fence's `unreferenced=`
        # can be checked against the ids actually printed above it.
        if unreferenced:
            say("  un-referenced moving-duty cells (%d, no independent reference this run):" %
                  len(unreferenced))
            for cid in unreferenced:
                say("    %s" % cid)
        # The fence carries BOTH counts, so a consumer can tell "the analyzer deliberately excluded
        # this cell (no independent reference)" from "the cell should be here and is not". A parser
        # that only compared the rows it could read would silently drop the referenced subset itself,
        # which is exactly how the "-" placeholder in refconv_rms/refconv_peak used to cost 5 rows.
        say("-- END MATRIX dynamic declared=%d emitted=%d skipped=%d unreferenced=%d" %
              (len(subset), dyn_rows, dyn_skipped, len(unreferenced)))

    say("\n== %s: exit_code=%d ==" % (args.label, rc))
    if args.out:
        try:
            write_report(args.out)
        except OSError as exc:
            print("FATAL: could not write %s (%s)" % (args.out, exc), file=sys.stderr)
            return 4
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
