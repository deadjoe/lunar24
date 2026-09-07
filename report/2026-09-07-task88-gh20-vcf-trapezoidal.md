# task #88 — GH #20: two-integrator trapezoidal (TPT/ZDF) VCF corner — acceptance report

**Date:** 2026-09-07
**Operator:** @Pi
**Direction:** @Codex `8320798f` (direction A: replace the Chamberlin numeric recursion with a two-integrator trapezoidal/TPT-ZDF SVF).
**Branch:** `fix/20-vcf-trapezoidal` (worktree `wt-20-vcf-trapezoidal`), stacked on task #87 (`fc27c48`).
**Base for PR:** `measure/20-vcf-response` (the task #87 measurement baseline).
**Production changed:** `core/include/lunar24/core/polivoks_vcf.h` only (plus the reset/real-state read seam already in place). No default/routing/registry/persistence change. No Q→Butterworth, no added feedback non-linearity, no self-oscillation, no output compensation, no modulation smoothing.

---

## 1. What was changed (production)

`PolivoksFilter::tick_` now runs the @Codex-specified two-integrator trapezoidal recursion (Simper's SvfLinearTrapOptimised2):

```
g  = tan(pi·fc/sr)          ; damp k = kDampMax + (kDampMin − kDampMax)·res   (= 2 − 1.9·res)
a1 = 1/(1 + g(g+k))         ; a2 = g·a1 ; a3 = g·a2
v3 = x − ic2eq
v1 = a1·ic1eq + a2·v3
v2 = ic2eq + a2·ic1eq + a3·v3
ic1eq = 2·v1 − ic1eq        ; ic2eq = 2·v2 − ic2eq
output = (mode < 0.5) ? v1 (BP) : v2 (LP)
```

- Cutoff map unchanged: `fc = 20·1000^norm`, capped `min(20000, 0.49·sr)`, floor 20 Hz. `kCutoffCapRatioSoft = 0.49` is a software safety policy, **not** a stability theorem (0.49 ≠ 0.45351 boundary — both are policy, over-tightened relative to the true TPT/ZDF stable range).
- Preserved: input `tanh` stage (GH#6, per-channel drive), L/R calibration, `res → damp = 2 − 1.9·res`, LP/BP selector, LINK/CV owner, registry/persistence format.
- The lowpass now has **unity DC gain independent of `damp`** (the Polivoks-defining property the Chamberlin lost): raising resonance adds a peak near cutoff but does not drop the low band.

## 2. Independent reference oracle (`tools/gh20_trap_reference.py`)

The bilinear-mapped continuous prototype, a different object from the product recursion:

```
k = damp = 2 − 1.9·res        ; r = tan(pi·f_test/sr) / tan(pi·fc/sr)
LP = 1/(1 − r² + jkr)         ; BP = j·r/(1 − r² + jkr)          (fc = 20·1000^norm, capped min(20000, 0.49·sr))
```

LP DC gain = 1 (0 dB) independent of res. BP has no peak normalisation. The OLD sr/8-capped Chamberlin does **not** match this, so the criterion is not self-fulfilling.

## 3. Positive acceptance — real codec→owner→render, full matrix

`tests/probes/gh20_vcf_probe.cpp` renders the actual product path (encode→decode→applyDeviceState→processBlock via the host harness) at **44.1/48/88.2/96k × {LP,BP} × res{0,0.5,1} × {0.05,0.20} level × 21-point norm grid** (norm grid ≥0.85 also emits a 100 Hz + 8 kHz cross-rate pair), 15518 cells. `tools/gh20_trap_analyze.py --check`:

```
coverage required=15518 produced=15518 failures=0
reference-evaluable groups=996 (out of 1008), sub-resolvable (deep stopband, reported not failed)=12
endpoint norm1/res0/lp 8k-normalized gains (dB): 44100:-0.089, 48000:-0.220, 88200:-0.941, 96000:-1.001
ACCEPTANCE PASS (exit 0)
```

- **Coverage:** 15518/15518 produced, finite, in-range, no missing/silent/over-scale cell.
- **Reference match:** all **996** reference-evaluable groups match the bilinear reference within **≤ 0.1 dB** (resolvability floor 20 dB SNR; each group's chain gain is normalised out via per-group median fit). 12 groups are deep-stopband cells physically below the measurement floor — reported per-item (contract's "report per-item"), not failed.
- **Cross-rate endpoint:** 100 Hz-normalised 8 kHz gain at norm=1/res=0/LP is **0.912 dB spread** across 44.1/48/88.2/96k (**≤ 1.0 dB**), and **all four rates are present**. This is the sample-rate cap ghost gone: no more sr/8 dead-zone, no 8 kHz gain collapse at higher sr.
- **Warmup artifact resolved:** the earlier > 0.1 dB outliers at bp/res=1/norm=0.15/60 Hz were a probe measurement artifact (the fixed 4096-sample warmup under-settled the Q≈10 resonator; the LS-fit noise projection was also biasing the floor). Bumped `kWarm` to 24576 (settles ~4.5τ at the binding cell) and fixed `residualNoiseRms` to solve the true 2×2 Gram system. The product itself was shown exact (0.0000 dB under long warmup) — this report is the measurement, corrected not the filter.

## 4. Negative controls — the gate discriminates

`tools/run_gh20_trap_negatives.py` (fast: header-only direct drive of `PolivoksFilter` through the same reference discriminator, shadowed header build per mutation). Chain gain is a frequency-independent constant the reference comparison normalises out, so the response discrimination transfers to the full-chain gate.

| mutation | result | identity |
|---|---|---|
| **real** (benign shadow copy) | **GREEN** 0.0160 dB | reference match ≤0.1 |
| old_cap (0.49→0.125 sr/8) | **RED** 10.374 dB @ lp/res0/norm1 | reference |
| old_recursion (Chamberlin + sr/8) | **RED** 34.416 dB | reference |
| wrong_damp (kDampMin 0.1→0.5) | **RED** 10.152 dB | reference |
| wrong_lp_bp (mode-select swapped) | **RED** 22.516 dB; LP_DC −297 dB (LP now returns BP, DC blocked) | reference + DC |
| lr_cross (both outputs read ch0) | **RED** — L/R isolation collapsed to 0.019 dB | L/R isolation |
| skip_setter (setFreq/setRes no-op) | **RED** 48.409 dB | reference |
| wrong_update_order (ic1eq updated before v2) | **RED** 8.186 dB | reference |

Every mutation turns the gate RED with its specific identity; the real product stays GREEN; the mutation mechanism itself is not the cause (a benign shadow copy of the real header is GREEN).

## 5. Contract item (3) — functional properties (from the same probe)

- **LP DC / low-freq gain:** 0.0000 dB at res=0 and res=1 — **unity DC gain, independent of `damp`** (the Polivoks-defining property).
- **BP direction:** BP is `j·r/(1−r²+jkr)` — peaks near cutoff, bandpass; the wrong_lp_bp mutation flips this and is RED.
- **L/R isolation:** channel[0] at norm, channel[1] at a different norm, same sine → outputs separate (min 1.080 dB); the lr_cross mutation collapses it to 0.019 dB (RED).
- **Extreme states finite / partition zero-alloc:** `cpu_four_output` failcount 0, `cpu_blockpartition` maxdiff 0 (clean).
- **No added buffer latency:** same-frame integrator states, no block delay; reset clears retained transient (real-state read seam `ic1eq/ic2eq` present).

## 6. Reproduction

```
# build the probe (real chain)
cmake --build build --target gh20_vcf_probe
# positive acceptance (full 15518-cell probe + reference gate)
./build/gh20_vcf_probe --out report/gh20-probe-trap
python3 tools/gh20_trap_analyze.py --dir report/gh20-probe-trap --manifest tools/gh20_manifest.tsv --check
# negatives (fast, header-only)
python3 tools/run_gh20_trap_negatives.py --root .
# via ctest
ctest -R gh20_vcf_acceptance
ctest -R gh20_vcf_negative
```

## 7. Deliberately NOT done

- Not merged; GH #20 and #6 not closed; no release; **not claimed fixed** (the chord correction is delivered for the corner; @Codex rules the criterion/algorithm + independent review, then merge decision remains @Codex's).
- No production DSP beyond `polivoks_vcf.h` and the reset/real-state seam; no default/routing/registry/persistence change; the 12 keyboard-coverage gaps and the `--require-full` full-coverage gate are unchanged.
- The `gh20_vcf_analyze.py` / `run_gh20_vcf_negatives.py` task #87 criterion is superseded by `gh20_trap_analyze.py` (the CMake now registers the trapezoidal criterion); the old files remain as archived task #87 work.
