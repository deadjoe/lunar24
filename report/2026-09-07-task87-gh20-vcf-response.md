# task #87 (GH #20) — VCF cross-sample-rate response, knob dead-zone, narrowest correction contract

**Branch:** `measure/20-vcf-response` (stacked on `fix/19-triangle-blamp`; exact submitted SHA,
stacked-PR number and 4-platform CI result are recorded in a follow-up docs commit per the GH#19
pattern — §10).
**Baseline (GH#19 mainline):** production VCF/`polivoks_vcf.h` **unchanged** (read-only).
**Scope:** reproduce audit finding N-3 (sr/8 knob dead-zone + 8 k cross-rate gain gap) and characterise
N-4 (Chamberlin non-generic) with REAL-PRODUCT measurements. Deliver a reproducible baseline and ≤2
written correction-candidate contracts. **No production DSP changed.** `GH #20` is **NOT claimed fixed**
— this is a measurement + contract deliverable awaiting @Codex algorithm ruling + merge.

---

## 0. One-line finding

The Polivoks VCF's norm→Hz cutoff is `fc = min(20·1000^norm, sr/8)`
(`kCutoffCapRatio = 1/8` in `polivoks_vcf.h:134`). Because the cap is `sr/8`, at 44.1 kHz the FREQ knob
goes **inert** for `norm ≳ 0.813` (cutoff pinned at 5512.5 Hz) while at 96 kHz it stays live to
`norm ≳ 0.926` (cap 12000 Hz). This is the audit finding — reproduced and quantified below. The cap is a
**stability boundary**, not an arbitrary choice: raising it above `1/8` makes the Chamberlin SVF
non-finite at high norm (N-4 evidence, §7).

---

## 1. Matrix (mandate coverage)

| Dimension | Coverage | Notes |
|---|---|---|
| Sample rates | 44100 / 48000 / 88200 / 96000 | `kSrs` |
| Mode | LP and BP | crossrate cells |
| Resonance | **res = 0** (flat, `kDampMax=2.0`) | see §5 (scoping) |
| Norm points | **21** (0.00–1.00, step 0.05) | mandate "norm≥21 points" |
| Input levels | 3 (`0.05` small / `0.20` medium / `0.50` large) | mandate "≥2 legal levels" |
| Frequency sweep | per-sr log sweep to Nyquist (15–16 points) | `probeFreqs(sr)` |
| Label per cell | freq_hz, amp_rms, amp_peak, f0_meas_hz, noise_rms, window | actual measured values, not input copies |

Required cells: **1339 / 1339 produced**, 0 blocked, produced-set exactly equals the committed
manifest (`tools/gh20_manifest.tsv`). Each cell is rendered through the **one agreed real entry**:
`make_default_device_state → encode → decode → engine.applyDeviceState → DeviceAdapter::renderBlock`,
driven by `EngineHarness` (`tests/host/`).

---

## 2. Reproduce N-3a — knob dead-zone (inert onset norm, sr/8)

Normalised (to each curve's passband max) response curve is compared to the `norm=1.0` reference
curve; the **inert onset** is the lowest norm whose normalised curve matches `norm=1.0` across the
common band (i.e. the knob stops changing the response).

| sr | measured onset `norm` | theoretical `sr/8` onset | cap Hz | dev |
|---|---|---|---|---|
| 44100 | 0.80 | 0.813 | 5512 | 0.013 |
| 48000 | 0.85 | 0.826 | 6000 | 0.024 |
| 88200 | 0.90 | 0.914 | 11025 | 0.014 |
| 96000 | 0.95 | 0.926 | 12000 | 0.024 |

All within the `ONSET_TOL = 0.08` set from measurement. **This IS the N-3a reproduction**: the onset
tracks `sr/8`, so at lower sr the knob saturates at a **lower** norm — the knob is inert earlier at
44.1 k than at 96 k, for the same physical cutoff knob position.

---

## 3. Reproduce N-3b — 8 k cross-sample-rate gain gap

Low-band-normalised gain at 8 kHz, LP, norm = 1.0 (the crossrate focus):

| sr | rel_gain(8 k) |
|---|---|
| 44100 | −7.72 dB |
| 48000 | −7.38 dB |
| 88200 | −4.40 dB |
| 96000 | −4.00 dB |

**gap 44.1 k vs 96 k = −3.72 dB** (`CROSS_GAP_MIN_DB = 2.0`, from measurement — not pre-filled).
Same knob `norm=1`, same 8 k input: 44.1 k rejects (fc 5512.5 < 8 k), 96 k passes (fc 12000 > 8 k).
This is the N-3b reproduction.

---

## 4. ParameterId → consumer → cutoff calc → observation point; same-input L/R

Stable ParameterIds consumed by the VCF (from `generated/lunar24/registry_ids.hpp`):

| id | name | use |
|---|---|---|
| 13 | `vcf_l_freq` | L cutoff (norm) |
| 18 | `vcf_r_freq` | R cutoff (norm) |
| 14 | `vcf_l_res` | L resonance |
| 34 | `vcf_r_res` | R resonance |
| 15 | `vcf_l_mod` | L CV amount (0 = no shift) |
| 35 | `vcf_r_mod` | R CV amount |
| 19 | `vcf_l_bp_lp` | L mode selector 0=BP / 1=LP |
| 36 | `vcf_r_bp_lp` | R mode selector |
| 16 | `vcf_dist` | distortion (0 = exact dry passthrough) |
| 17 | `vcf_gain` | post-distortion gain (0 → distorted term dry) |
| 20 | `vcf_link` | link switch (off in probe) |

**Cutoff calc** (`polivoks_vcf.h:152`): `fc = 20·1000^freqNorm`, then `fc = min(fc, sr·kCutoffCapRatio)`.
**Observation point** (highest-fidelity, least-confounded): **`wetL`** — physical in0 = EXT.AUDIO →
mixer ch4 (`kChannelExtAudio`) → mixL → `PolivoksFilter.tick_` (L channel) → distortion (`dist=0` is an
EXACT dry passthrough) → WET L. The mixer is equal-power pan×vol (frequency-independent), the VCF input
stage (GH#6 tanh) is ~linear at small signal, so WET magnitude = VCF transfer `×` one constant gain —
the ONLY frequency-dependent stage in the chain is the VCF. `vcf_l_mod=0` zeroes any CV shift, so
`vcf_l_freq` alone sets the cutoff (no default-CV confound).

**Same-input asymmetric L/R test** (proves the product consumes the L/R FREQ knobs independently):
`vcf_l_freq=0.9`, `vcf_r_freq=0.3`, identical 1 kHz sine into both, `|amplitudes|`:

| sr | ampL (norm 0.9) | ampR (norm 0.3) | |L/R| dB |
|---|---|---|---|---|
| 44100 | 0.012029 | 0.000320557 | 31.49 |
| 48000 | 0.0121293 | 0.000319856 | 31.58 |
| 88200 | 0.012515 | 0.000316708 | 31.94 |
| 96000 | 0.0125327 | 0.000316537 | 31.95 |

`ASYM_MIN_DB = 3.0`. The two independent knobs produce a large same-input asymmetry → the FREQ knobs
are genuinely routed per-side (a same-input L/R difference fires).

---

## 5. Scoping — res and composite attribution (transparency)

- **res = 0 only** for the deadzone / crossrate / base-response sweeps. Justification, at the code
  level: the `sr/8` cap lives in `baseFreqHz_(freqNorm, sr)` (`polivoks_vcf.h:152`), which reads only
  `freqNorm` and `sr` — `res` is **not** an input there. `res` enters only via the separate
  `damp = kDampMax + (kDampMin - kDampMax)·res` coefficient (line 188). Therefore the dead-zone (N-3a)
  and the 8 k cross-rate gain gap (N-3b) are **res-independent**; `res=0` isolates the knob-map effect.
  A `res` sweep into `kDampMin = 0.1` (max resonance) sits at the **provisional** non-self-oscillation
  floor, and probing it is deferred per N-4's "don't invent OTA/self-oscillation" — see §7 and next-slice.
- **Composite attribution** for the level probe: the input-stage fold is `GH#6` tanh, NOT the VCF
  response. The level cells (small→large 19.86 dB vs linear 20.00, dev 0.14 dB; small→medium 12.02 dB
  vs 12.04, dev 0.02 dB) attribute the fold to the **input stage**, so the VCF response itself is NOT
  over-attributed. This is explicitly labeled composite in the analyzer output, not presented as a
  VCF transfer property.

---

## 6. N-4 — only verified existing nonlinearity positions + evidence (no invention)

- **Nonlinearity position verified:** the GH#6 input-stage fold, measured by the level probe (above).
  Small-signal slope stays ~1, high-input folds to lower normalised gain — the documented position.
- **Chamberlin non-generic (measured, not inferred):** even at `res=0` the −3 dB magnitude point sits
  at **~0.58 × the natural cutoff** (measured), not at the cutoff — a non-generic LP. This is reported
  as **measured evidence** only.
- **Stability-boundary evidence:** the `sr/8` cap is a genuine stability bound, not arbitrary. Raising
  `kCutoffCapRatio` above `1/8` makes the Chamberlin SVF **non-finite** at `norm ≳ 0.85` (the
  cap-raised negative in the analysis): at `norm=1.0`, `fc=20 kHz` needs `f=2·sin(π·fc/sr) > 2`
  → the recursion is unstable. The cap is where this stays finite.
- **NOT asserted:** no OTA model, no self-oscillation claim, no generic-SVF claim. The response is
  described only by what is measured.

---

## 7. Negative controls — the tool must not false-pass

- **Empty / silent:** silence substitution → classified silent (RED), not accepted.
- **Over-scale:** un-scaled-ideal substitution (output peak > 0.55) → detected (RED).
- **Non-finite:** `amp_rms` non-finite → detected (RED).
- **Not-produced cell:** a required id with no row → detected (RED), fail-closed coverage.
- **Wrong sr-param label:** a halved-sr record is rejected (sr-label contract).
- **Source negatives** (isolated copy of `polivoks_vcf.h`, never the committed tree):
  1. **cap-removed** (`kFreqMaxHz 20000 → 5000`, so the knob never saturates) — dead-zone absent →
     analyzer RED on "no inert onset". Real product GREEN.
  2. **lr-ignored** (R channel reads the L FREQ knob) — same-input L/R difference vanishes → analyzer
     RED on "|L/R| < ASYM_MIN_DB". Real product GREEN.
  - A build failure is reported and **not** counted as a red — the tool must *build* to meaningfully
    reject. Reproducible runner: `tools/run_gh20_vcf_negatives.py` (registered as CTest
    `gh20_vcf_negative`, UNIX-only). The real product is the positive control.
- **No DSP changed to make green.** All gates pass on the unchanged production VCF.

---

## 8. Correction-candidate contracts (≤2; wrote the contract only, no implementation)

### Candidate A — remove the dead-zone (re-derive the norm→Hz mapping stability-safe)
- **Reference model:** keep the Chamberlin SVF; change only the norm→Hz map so the knob does not
  saturate below the target cutoff. Target mapping `fc = min(20·1000^n, sr·capRatioₓ)` with
  `capRatioₓ` chosen so the map is monotone and the SVF stays finite.
- **Effective BW:** report as the measured −3 dB point per (sr, norm) on the *new* map; expected
  change vs baseline documented from the reproduction (baseline −3 dB at ~0.58×fc_knob, res=0).
- **Error metric:** max |Δrel_gain(8 k)| across 44.1/48/88.2/96 k at norm=1.0 LP, target < 1 dB
  (from the measured 3.72 dB gap, NOT a pre-filled value). Also the dead-zone must be removed: the
  inert onset must move to ≥ 0.999 at every sr.
- **Stability:** assert all cells finite at norm=1.0, res ∈ {0, 0.5} after the change (the sr/8 cap is
  the current stability boundary; the new map must keep `f ≤ 2`).
- **CPU cost:** the mapping is O(1) per sample (a `pow` + min); over the per-sample baseline the delta
  is ~one branch. The baseline per-sample CPU is guarded by the finite/block-partition invariants
  (`four_output failcount = 0`, `block_partition_maxdiff = 0`); a dedicated ns/sample figure for the
  corrected map is produced in the next-slice when the candidate is implemented.
- **Caveat:** a Chamberlin (SVF) with the cap lifted off `sr/8` needs a **TPT / ZDF** re-formulation
  (or a `prewarp`) to stay stable at high fc; a naive `kCutoffCapRatio` change is unstable (proven by
  the cap-raised negative). Document prewarping the bilinear coefficient, do not just remove the cap.

### Candidate B — reference model + effective-BW contract (no DSP change; correction at the boundary)
- Model the Chamberlin's known non-generic response and expose an **effective-BW** correction so the
  measured response matches a target reference, applied either as a knob→cutoff re-map or as a
  post-correction.
- **Effective BW:** the reference is the measured −3 dB point table; the correction maps `norm` →
  `norm_eff` so `BW_measured(norm_eff) ≈ BW_reference(norm)`.
- **Error metric:** same as A (≤1 dB on the 8 k gap + dead-zone removed), tolerance from measurement.
- **Stability / CPU:** identical framing to A (finite assert + O(1) per-sample).
- **Caveat:** this is a *reference* correction — it changes the audited response to match a target and
  must be reviewed against the product's identity ("you will not lose low frequencies when increasing
  resonance"); it does not change the underlying SVF.

Both candidates keep **production unchanged until @Codex rules** on the algorithm; none of the
correction candidates is implemented in this deliverable.

---

## 9. Reproduction commands (repo root = `measure/20-vcf-response`)

```bash
# 1) generator + manifest (single authoring source)
python3 tools/gen_gh20_manifest.py --out tools/gh20_manifest.tsv
# 2) probe (real product) → report/gh20-probe/gh20_{scenarios,cpu}.tsv
./build/gh20_vcf_probe --out report/gh20-probe
# 3) analyzer --check (the gate) → report/gh20-check.txt
python3 tools/gh20_vcf_analyze.py --dir report/gh20-probe --manifest tools/gh20_manifest.tsv --check
# 4) source negatives (isolated mutated header; POSIX compiler)
python3 tools/run_gh20_vcf_negatives.py --root . --manifest tools/gh20_manifest.tsv
```

**Artifacts (committed):** `report/gh20-probe/gh20_scenarios.tsv` (1339 cells), `report/gh20-probe/gh20_cpu.tsv`
(machine + finite/block-partition), `report/gh20-check.txt` (gate pass).

**CI-enforced tests:** `gh20_vcf_probe`, `gh20_vcf_acceptance`, `gh20_vcf_negative` (UNIX).

---

## 10. Verification & next step

- **Full regression:** build + `ctest` 74/74 passed (incl. `gh20_vcf_acceptance`, `gh20_vcf_negative`);
  the GH#19 `gh19_blamp_lut_gate` and host wiring/stream-plan/codec/CoreAudio tests all green.
- **Production:** 0 DSP / default / routing / registry / persistence changes. GH#20 **not** fixed.

**Submission record (docs follow-up commit):**
- Exact submitted SHA: **`33d6020`** (branch `measure/20-vcf-response`, stacked on `fix/19-triangle-blamp`).
- Stacked PR: **#25** (draft, base `fix/19-triangle-blamp`, head `measure/20-vcf-response`), MERGEABLE.
- 4-platform CI: manually dispatched (`workflow_dispatch`) because this PR's base is not `main` and the branch
  is not `feat/**`, so the `ci.yml` auto-triggers (`push main|feat/**`, `pull_request→main`) do **not** fire.
  Run **`34069185210`** @ exact head `33d6020` — **four matrix platforms all SUCCESS**
  (`ubuntu clang++` 6m55s, `ubuntu g++` 7m53s, `macos clang++` 6m55s, `windows cl` 13m26s);
  the `full coverage (--require-full)` job is **skipped** (`pull_request`-gated) — identical to the
  GH#19 `a27d807` / `942ad82` approved baseline state (0 new, gate unchanged).

**Next-slice recommendation:** (1) a **res sweep** {0.0, 0.5, 1.0} at the two endpoint srs and a
focused norm/freq set, labelled provisional for the `dampMin` non-self-oscillation floor; (2) a
per-sample `ns/sample` CPU figure for the baseline VCF; (3) after @Codex algorithms-ruling, implement
the chosen correction-candidate in an isolated branch with the above acceptance criteria.
