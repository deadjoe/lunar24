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
| Resonance | **res ∈ {0.0, 0.5, 1.0}** (`kDampMax=2.0` / `damp=1.05` / `kDampMin=0.1`) | res-swept per @Codex `e3d4211e` item 1 |
| Norm points | **21** (0.00–1.00, step 0.05) | mandate "norm≥21 points" |
| Input levels | **2** (`0.05` small / `0.20` medium) | the two legal levels (drive=0 ⇒ level-invariant input stage; a `0.50` large was dropped — it exceeded the legal level pair and its fold is outside the attribution) |
| Frequency sweep | per-sr log sweep to Nyquist (15–16 points) | `probeFreqs(sr)` |
| Label per cell | freq_hz, amp_rms, amp_peak, f0_meas_hz, noise_rms, window | actual measured values, not input copies |

Required cells: **7958 / 7958 produced** (21 norm × 15-16 freq × 3 res × 2 lvl per sr for cutoff + the
384 crossrate + 8 asym + 4 floor + 2 level), 0 blocked, produced-set exactly equals the committed
manifest (`tools/gh20_manifest.tsv`). Each cell is rendered through the **one agreed real entry**:
`make_default_device_state → encode → decode → engine.applyDeviceState → DeviceAdapter::renderBlock`,
driven by `EngineHarness` (`tests/host/`).

---

## 2. Reproduce N-3a — knob dead-zone (inert onset norm, sr/8)

Normalised (to each curve's passband max) response curve is compared to the `norm=1.0` reference
curve; the **inert onset** is the lowest norm whose normalised curve matches `norm=1.0` across the
common band (i.e. the knob stops changing the response).

Now measured **per (res, level)** (`res ∈ {0, 0.5, 1}` × `level ∈ {0.05, 0.20}`); the onset is
**res-independent** (within one grid step), so the table shows the per-res onset values and the spread:

| sr | onset res=0 | onset res=0.5 | onset res=1 | theoretical `sr/8` onset | cap Hz | res spread |
|---|---|---|---|---|---|---|
| 44100 | 0.80 | 0.85 | 0.85 | 0.813 | 5512 | 0.05 |
| 48000 | 0.85 | 0.85 | 0.85 | 0.826 | 6000 | 0.00 |
| 88200 | 0.90 | 0.95 | 0.95 | 0.914 | 11025 | 0.05 |
| 96000 | 0.95 | 0.95 | 0.95 | 0.926 | 12000 | 0.00 |

All within `ONSET_TOL = 0.08`; the per-(res,level) spread is `≤ RES_ONSET_TOL = 0.05`. **This IS the
N-3a reproduction**: the onset tracks `sr/8`, so at lower sr the knob saturates at a **lower** norm — the
knob is inert earlier at 44.1 k than at 96 k, for the same physical cutoff knob position.

> **Res-independence (the `e3d4211e` item-1 point, now measured).** `baseFreqHz_(freqNorm, sr)` reads only
> `freqNorm` and `sr` — `res` is not an input — so the *position* of the `sr/8` cap is a pure freq-map
> equation `20·1000^n = sr/8` and is genuinely **res-independent in the code**. The measured onset carries
> a ≤one-grid-step (0.05) spread between res=0 and res=0.5/1 — this is **grid-resolution + identity
> tolerance** detection noise (the response-curve identity flips one norm step later at res=0.5/1 as the
> resonance alters the residual difference), **not** code res-dependence. Reported as interval, not a single
> exact value (per @Codex item 4 — a finite-grid/judgement-tolerance value).

> **Onset is reported as an interval, not an exact start (per @Codex item 4).** The norm grid is coarse
> (step 0.05) and the onset is the *lowest grid point whose normalised response already matches the
> `norm=1.0` curve within the judgement tolerance* — so the measured `0.80` is **not** claimed as the exact
> first-inert norm; it is the **grid-resolution-bounded** estimate, with a **±0.05 (one grid step)**
> resolution and a **0.08 judgement tolerance** baked in. The theoretical `sr/8` onset (0.813) falls inside
> the measured **interval `[0.800, 0.850]`** for 44.1 k (0.90–0.95 for 88.2 k, theo 0.914) — the measured-grid
> onset matches theory to within one grid step.

---

## 3. Reproduce N-3b — 8 k cross-sample-rate gain gap (RES-DEPENDENT)

Low-band-normalised gain at 8 kHz, LP, norm = 1.0 (the crossrate focus), now measured **per res**
(`res ∈ {0, 0.5, 1}`, level 0.20):

| res | rel_gain(8 k) @44.1 k | rel_gain(8 k) @96 k | gap 44.1→96 k |
|---|---|---|---|
| 0.0 | −7.72 dB | −4.00 dB | **−3.72 dB** |
| 0.5 | −2.02 dB | −0.01 dB | **−2.01 dB** |
| 1.0 | **+0.72 dB** | **+4.96 dB** | **−4.24 dB** |

**The gap is res-dependent** (0.5: −2.01, 0: −3.72, 1: −4.24 dB — **not** one value, not res-independent).
All three are ≤ `CROSS_GAP_MIN_DB = 1.5` (negative → the cross-rate rejection is real at every res).
Same knob `norm=1`, same 8 k input: 44.1 k rejects (fc 5512.5 < 8 k, so a cut), 96 k passes (fc 12000 > 8 k).
The magnitude ordering is not monotonic in res — **res=0.5 has the *smallest* gap and res=1 the largest.**

> **res=1 rel_gain flips sign (both positive) — a resonance-peak signature, not a "cut".** At `res=1`
> (`damp=0.1`, near the provisional high-resonance floor) the resonance peak sits close to 8 kHz, so the
> 8 k *gain* is a **boost** (44.1 k **+0.72 dB**, 96 k **+4.96 dB**), not an attenuation. The *gap* is still
> negative (the 96 k boost is larger), but the **interpretation differs** — this is the **R&D sign-flip
> prediction confirmed** (the model predicted the res=1 relative gain would turn positive as the resonance
> peak crosses 8 k). Reporting a single "−3.72 dB" for all res would be **wrong**; the res=1 row is a
> resonance up-top, not a cut. This is also why the res=0-only baseline (§5) under-reported the res structure.

This is the N-3b reproduction: at low sr the knob's cap (`sr/8`) pulls the cutoff below 8 k, so the same
8 k input is rejected there and passed at high sr — quantified per-res above.

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

- **res sweep — the gap is now CLOSED (measured, not deferred).** The earlier res=0-only matrix was a
  coverage gap (per @Codex `e3d4211e` item 1); it was expanded and re-run to the legal `res ∈ {0, 0.5, 1}`
  × two legal levels. The res-independence of the *dead-zone onset* is confirmed (§2) and the res-dependence
  of the *8 k cross-rate gap* is measured (§3) — so the earlier caution "res=0 does not prove the 8 k gap
  is res-independent" is now superseded by measured per-res data. **Lawful scope at res=1:** the high-resonance
  floor is the *provisional* `kDampMin=0.1`; the small-signal, linear-reporting probe does not assert an OTA
  model or self-oscillation — same no-invention caveat as §6. The res sweep is a measurement of an existing
  legal parameter range, not a new self-oscillation hypothesis.
- **Composite attribution** for the level probe: the input-stage fold is `GH#6` tanh, NOT the VCF
  response. The level cells (small→medium 12.02 dB vs linear 12.04, dev 0.02 dB) attribute the fold to
  the **input stage**, so the VCF response itself is NOT over-attributed. (The previous `small→large`
  0.50 level was **dropped** — it exceeds the legal level pair `{0.05, 0.20}` and its fold is outside the
  input-stage attribution.) This is explicitly labeled composite in the analyzer output, not presented as
  a VCF transfer property.

---

## 6. N-4 — only verified existing nonlinearity positions + evidence (no invention)

- **Nonlinearity position verified:** the GH#6 input-stage fold, measured by the level probe (above).
  Small-signal slope stays ~1, high-input folds to lower normalised gain — the documented position.
- **Chamberlin non-generic (measured, not inferred):** even at `res=0` the −3 dB magnitude point sits
  at **~0.58 × the natural cutoff** (measured), not at the cutoff — a non-generic LP. This is reported
  as **measured evidence** only.
- **Stability-boundary evidence (corrected, see §6a):** the `sr/8` cap is a genuine stability bound, not
  an arbitrary choice — but for the correct reason. The Chamberlin SVF's linear state matrix is
  `A = [[1, f], [-f, 1 - f·damp - f²]]` (the `-f²` term comes from substituting the **updated** `low'`
  into `high`, so the band coupling is `1 - f·damp - f²`, not `1 - f·damp`). Its eigenvalues cross
  `|λ| = 1` at a res-dependent `fc/sr`; the `sr/8 = 0.125` cap sits below all of them. The naive
  `f = 2·sin(π·fc/sr) > 2` boundary claim in earlier drafts was mathematically impossible (f ∈ [0,2])
  and has been **withdrawn**; the real sufficient condition is `|λ| ≤ 1` from `A`. See §6a.
- **NOT asserted:** no OTA model, no self-oscillation claim, no generic-SVF claim. The response is
  described only by what is measured.

## 6a. Stability — corrected state-matrix bound (per-res boundary + margin + finite-length verify)

The exact recursion (mode-agnostic, `tick_`):
```
f = 2·sin(π·fc/sr)                        # fc = capped cutoff
damp = kDampMax + (kDampMin - kDampMax)·res
low += f·band
high = x - low - damp·band                 # ORDERS MATTER: `low` here is the UPDATED low'
band += f·high
```
Substituting `low' = low + f·band` into `high` gives the linear state update (input x=0):
```
low'  = 1·low + f·band
band' = -f·low + (1 - f·damp - f²)·band
```
so `A = [[1, f], [-f, 1 - f·damp - f²]]`, with `trace = 2 - f·damp - f²`, `det = 1 - f·damp`.

The eigenvalues cross `max|λ| = 1` (the instability mode is the **negative real pole crossing λ = -1**,
i.e. the overdamped-to-oscillatory flip, NOT `f > 2` which is impossible) at these theoretical key
points, measured independently from the pole formula and verified with a finite-length sustained-sine
run of the exact recursion:

| res | damp | theoretical boundary `fc/sr` | `max|λ|` at cap `=0.125` | fc-parameter margin `log2(boundary/cap)` | pole-distance margin `log2(1/max|λ|@cap)` | finite-length sustained (exact recursion) |
|---|---|---|---|---|---|---|
| 0.0 | 2.00 | **0.1359** | 0.7891 | **0.127 oct (thinnest)** | 0.342 oct | runaway at `fc/sr ≥ 0.20` (0.1359 < 0.20) |
| 0.5 | 1.05 | **0.2066** | 0.4431 | 0.725 oct | 1.174 oct | runaway at `fc/sr ≥ 0.25` (0.2066 < 0.25) |
| 1.0 | 0.10 | **0.4001** | 0.9610 | 1.678 oct | **0.057 oct (thinnest)** | still bounded at `fc/sr = 0.40` (≈ boundary) |

> **Two "margins" are different quantities — keep them separate.** Both columns answer "how much room
> is there before λ reaches the unit circle," but they are measured on different axes and rank the three
> res values **in opposite order**:
> - **fc-parameter margin** `log2(boundary / 0.125)` = how much more you can raise the **cutoff cap**
>   (the fc/sr parameter) before that res hits its boundary. Because the *boundary* is lowest at res=0
>   (0.1359), **res=0 has the thinnest fc-parameter margin (0.127 oct)** — raising the cap above sr/8
>   destabilises **res=0 first**, before res=0.5 or res=1.
> - **pole-distance (radial-attenuation) margin** `log2(1 / max|λ|@cap)` = how close the pole sits to
>   the unit circle **at the cap itself (the fixed operating point at high norm)**. Because `max|λ|@cap`
>   is *highest* at res=1 (0.961), **res=1 has the thinnest pole-distance margin (0.057 oct)** — but this
>   is the pole's radial distance to λ=−1 at a *fixed* fc, NOT the distance of the fc boundary.
>
> Concretely: the cap is an fc/sr **parameter**; the quantity that matters for "raise the cap → which res
> destabilises first" is the **fc-parameter** margin, which is **thinnest at res=0**, not res=1. My earlier
> draft reported only the pole-distance column and then used it to claim "the cap margin is thinnest at
> res=1." That mixes the two axes and is **wrong** for the *raise-the-cap* question. Both margins are
> real, they just describe different limits; the report reports both and uses the fc-parameter one for the
> "is the sr/8 cap a boundary" claim.

**Findings (three distinct claims, separated per @Codex item 2):**
1. **Theoretical boundary** — a genuine, res-dependent `fc/sr` at which `max|λ|` crosses 1:
   res=0 → 0.136, res=0.5 → 0.207, res=1 → 0.400. Verified by finite-length sustained-sine.
2. **Empirical safety margin (the "raise the cap" limit)** — the product's `sr/8 = 0.125` cap sits below
   every boundary, so the product is stable at the cap at every res. **But the margin is THINNEST at
   res=0 (fc-parameter 0.127 oct)**, i.e. **raising the cap above sr/8 destabilises the flat res=0 case
   FIRST** — its boundary (0.1359) is closest to the cap. res=1 has the *largest* fc-parameter margin
   (1.678 oct), so it is the *last* to blow up as the cap is raised. This resolves @Codex's point
   (better-fc-boundary-of-res=1 ≠ it destabilises first; on fc-up, res=0 hits the boundary first).
3. **Pole-distance at the fixed cap** — a *separate* quantity: at the cap itself res=1 is 0.057 oct from
   λ=−1 (the closest). This describes the resonance gain peaking at the operating point, NOT the
   raise-the-cap limit. The finite-length sustained run confirms all three boundaries: res=0 runaways at
   0.20 (>0.136), res=0.5 at 0.25 (>0.207), res=1 still bounded at 0.40 (≈ its boundary, neutral).
4. **Nonlinear-chain measurement** — the above is the *linear* SVF alone. The product adds the GH#6
   `tanh` input stage. At small signal that is ~linear, so the linear boundary governs the small-signal
   probe here; at large input the fold bounds the excursion and changes the onset empirically. This
   report measures the small-signal/linear chain and states the linear boundary; the saturated-branch
   onset is a separate measurement not claimed here.

**This supersedes the earlier `f > 2` text.** It also corrects the prior "cap protects the tightest res=0
case" statement (which used the boundary ordering — res=0 has the lowest boundary): the cap being
**below** the res=0 boundary (0.1359) is why **raising it above 1/8 destabilises res=0 first** (the
thinnest fc-parameter margin), and it is the reason the cap is a genuine stability boundary for that
res.

---

## 7. Negative controls — the tool must not false-pass

- **Empty / silent:** silence substitution → classified silent (RED), not accepted.
- **Over-scale:** un-scaled-ideal substitution (output peak > 1.0) → detected (RED). The threshold is
  **res-aware**: at `res=1` the genuine resonance peak measures **0.709** (level 0.20, §4 probe max), while
  `res=0` stays ≤ 0.072 and `res=0.5` ≤ 0.081 — so a flat 0.55 guard (calibrated for the res=0-only
  matrix) would FALSELY flag the real res=1 resonance. 1.0 is set once: it is full-scale / a genuinely
  un-scaled ideal at these test levels (the synthetic negative control reports `amp_peak=2.0 > 1.0` and
  still fires).
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

## 8. Correction-candidate contracts (≤2; re-derived per @Codex item 3 — contradiction resolved)

The original pair (A="lift the cap on the Chamberlin", B="reference/BW correction") was internally
contradictory, and the R&D simulation now resolves it — but **not** as the first draft claimed. The
first draft overstated Candidate B as "the dead-zone is *irreducible* by any map-only change". That is
**wrong**, and the split must be corrected along the *two separable goals* @Codex named:

> - **Full-travel usable** — the knob responds across its entire norm travel (no inert plateau). This
>   **IS reachable on the Chamberlin** by **remapping** the reachable cutoff range.
> - **Preserve the absolute 20 kHz endpoint** — `norm=1` still maps to 20 kHz. This is **NOT
>   reachable on the Chamberlin**, because 20 kHz lands at `fc/sr = 0.454` at 44.1 k, far past its
>   res=0 linear boundary `0.136`.

The Chamberlin cannot run above its `fc/sr` linear boundary (0.136 at res=0, the *default*), so it
cannot hold the 20 kHz endpoint while staying stable. **The two goals are separable — you can trade one
for the other, but you cannot hold both at once.** That is the real constraint; it is *not* "Candidate B
is mathematically impossible". The two corresponding narrow candidates are the two ends of that trade.

### Candidate A — replace the SVF topology (TPT/ZDF), remove the src/8 cap → *identity change*
- **Layer / formula path:** swap the `tick_` SVF for a **bilinear-prewarped TPT (ZDF) state-variable
  filter** (Vadim Zavalishin), stable to Nyquist for *any* `fc/sr`. Change only the filter core; keep
  the norm→Hz map (`20·1000^n`), the input stage, and the mode (bp|lp) selection.
- **Common Hz range:** `fc` spans **20 Hz → 20 kHz at every sr** (the cap is removed) — dead-zone gone.
- **Response reference:** the generic (`0.707`-≈ −3 dB at fc) SVF; this is the **reference**
  the current `0.58×fc` Chamberlin is compared *against*, and it is what the 8 k cross-rate gap is
  judged to.
- **Actual measured error (R&D, exact recursion; NOT a <1 dB claim):** after lifting the cap to
  `sr/2` and running TPT/ZDF, the 8 kHz gain at `norm=1.0` is **+0.07 dB at 44.1 k vs +1.64 dB at 96 k →
  residual cross-rate gap ≈ 1.57 dB** at res=0, versus the measured Chamberlin gap **−3.72 dB at res=0**.
  *The exact recursion confirms the TPT/ZDF is stable at `fc/sr = 0.454` for res ∈ {0, 0.5, 1} (no
  non-finite); the Chamberlin is not.* **These two numbers are NOT directly comparable** — the 1.57 dB is a
  *generic 2nd-order TPT prototype* while the 3.72 dB is the *real Chamberlin*; see §8a for why they differ.
  Both are reported honestly, neither is claimed as < 1 dB.
- **Feasibility counterexample (isolated R&D):** verified — TPT/ZDF at `fc/sr=0.4535`, res=0/0.5/1,
  stays finite and measured 0.07/1.64 dB at 8 k. This is the `new candidate evidence` @Codex asked for.
- **Stability:** TPT/ZDF is unconditionally stable (bilinear-prewarped `g=tan(π·fc/sr)`, `a1=1/(1+g(g+k))`);
  no `sr/8` cap needed. Finite-length confirmed.
- **CPU cost:** ~3 multiplies + 2 adds to the existing recursion (O(1)); measured in the next-slice.
- **Trade-off (must be ruled by @Codex/owner):** this is a **filter-identity change** — the Polivoks
  character (non-generic `0.58×fc`, resonance behavior) is replaced by a generic SVF. It *is* a change
  to the audited sound, not a transparent measurement-only correction. **This is the honest reason the
  two goals conflict.**

### Candidate B — keep the Chamberlin, remap the reachable cutoff range → *full-travel usable* (loses the 20 kHz endpoint)
- **Layer / formula path:** keep `tick_` and the stability cap; change **only the norm→Hz re-map** so
  that `norm ∈ [0,1]` lands inside the **common safe Hz range** `[kFreqMinHz, min(sr/8, res−boundary·sr)]`
  instead of `[20 Hz, 20 kHz]`. Concretely, `baseFreqHz_` becomes a monotone curve whose top equals the
  safe cap at every sr, so **no norm saturates the cap — the knob plateau is removed**.
- **Reachable range / bandwidth cost:** the knob's top cutoff is now **`sr/8` (≈ 5.5 kHz @44.1 k)** instead
  of 20 kHz. **The absolute bandwidth is lost** — the 20 kHz endpoint is traded for full-travel usability.
  This is the honest cost, and the *reason* the first draft's "irreducible" was wrong: a map-only change
  can remove the plateau (reach **full-travel usable**), it just cannot simultaneously keep the 20 kHz
  endpoint. The top can be pushed as high as the res=0 boundary (`0.136`, i.e. ~0.127 oct above `sr/8`),
  but that headroom is marginal and does not change the picture.
- **Response reference:** the current Chamberlin response re-anchored so the top norm maps to the safe
  cap; the `0.58×fc` −3 dB point still holds.
- **Actual error:** **no 8 k-gap reduction** — the cross-rate gap (3.72 dB) is caused by the cap being
  rate-dependent (`sr/8`), and re-mapping the norm range does **not** change the cutoffs the filter runs
  at, only which norm lands on which cutoff. So Candidate B **removes the plateau** (fixes the dead-zone
  symptom = N-3a) but **does not close N-3b** (the 8 k gap). It is offered to make explicit that
  **full-travel usability and the 20 kHz endpoint are separable**, and to show the plateau can be bought
  at a bandwidth price without a topology change.
- **Feasibility:** trivially implemented (a `pow` + min over a re-based curve); the plateau goes away,
  the top cutoff drops to `sr/8`.

**Net:** the two goals are **separable**, not mutually exclusive. **Candidate A** (topology → TPT/ZDF)
achieves **both** full-travel usable *and* the 20 kHz endpoint, at a filter-identity cost that must be
ruled by @Codex/owner. **Candidate B** (remap on the Chamberlin) achieves **full-travel usable** (removes
the dead-zone plateau) but trades away the 20 kHz endpoint and does **not** close the 8 k gap. Neither is
"impossible": the honest trade is *identity + absolute bandwidth* (A) vs. *bandwidth, keeping identity*
(B). Production is unchanged until @Codex rules on the algorithm.

### 8a. Model ↔ product reconciliation (per @Codex item 2(c))

**The R&D (recursive) numbers and the product numbers differ, and they are *not* the same quantity.**
This subsection unifies the observation point, the reference denominator, the res/identity split, and
explains the residual (rather than asserting an unexplained 2.21→3.72 discrepancy).

| res | R&D recursive model gap | product probe gap (measured, §3) | diff (product − model) |
|---|---|---|---|
| 0.0 | 2.21 dB | −3.72 dB | −5.93 dB |
| 0.5 | 1.78 dB | −2.01 dB | −3.79 dB |
| 1.0 | 2.56 dB | −4.24 dB | −6.80 dB |

**Unification — what is ALREADY identical (so the comparison is meaningful):**
- **Input observation point:** both drive a sine at the cutoff-map input; the probe drives the real
  product through `encode → decode → applyDeviceState → DeviceAdapter → wetL` (§1). The R&D drives the
  same recursion at the same frequencies.
- **Reference denominator:** both normalise to the **same 100 Hz reference** at `norm=1`/8 k in-band —
  i.e. `rel_gain(8k) = 20·log10(|H(sr,8k)| / |H(sr,100Hz)|)`. The denominator and its sign are preserved.
- **Same `baseFreqHz_` map** `20·1000^norm` and **same `sr/8` cap** in both (the attribute being measured).

**Why they differ — the model is a GENERIC prototype, the product is the NON-GENERIC Chamberlin (N-4):**
- The R&D gap (2.21/1.78/2.56 dB) is computed from a **generic** 2nd-order state-variable prototype whose
  −3 dB point sits *at* fc. The product is the **actual Chamberlin**, whose −3 dB point sits at **~0.58 ×
  fc** (§4) — it rejects **earlier and harder** than the generic prototype. At the tight 44.1 k cap
  (fc=5512.5 Hz) the Chamberlin's non-generic LP reaches 8 k well into its upper stopband, so **it rejects
  the 8 k input more at 44.1 k than the generic model does** — hence a **larger** product gap at the three
  measured res points. **This is a measured observation at those points, NOT a proven universal inequality
  and NOT a "lower bound".** The `reject-before-cutoff` mechanism explains the res=0/0.5 direction but does
  **not** hold at res=1, where the resonance **boosts** 8 k (below); the magnitude ordering across res is
  product-specific (see next bullet).
- **Res-order sensitivity:** the product gap is **non-monotonic in res** (−2.01 at 0.5, −3.72 at 0, −4.24 at 1),
  while the generic model is **monotonic** (1.78 at 0.5, 2.21 at 0, 2.56 at 1). The **res=0.5 minimum** and the
  **res=1 maximum** are product-specific (the non-generic response + the resonance-peak-vs-8 k interaction
  at res=1, §3 sign flip), and cannot be reproduced by the generic prototype. Only the res=0 row is a
  meaningful model-vs-product sanity check; the res=0.5/1 rows are product-only.
- **|H| magnitude vs windowed steady-state:** the model reports a pure transfer |H|; the product measures a
  **finite-windowed steady state** (Hann window + noise floor, §4 columns) so its resonance peak at res=1
  is slightly damped and the deep-stopband cells sit *at/below the probe noise floor* (those are labeled
  **"not a response", not a 0 dB value** — per @Codex item 1, cells at/below the noise floor past their
  cutoff are reported as **indistinguishable**, not as a measured floor).

**Conclusion:** the R&D and product numbers are **not two measurements of the same thing**; the R&D is a
generic-topology directional check and the product is the authoritative non-generic Chamberlin measurement.
The direction and the larger-product-gap-at-the-measured-points are explained by N-4 (non-generic −3 dB at
0.58×fc rejects harder at the tight 44.1 k cap). The **product values are authoritative**; the R&D is
retained only as a reproducibility sanity check for the generic-topology direction/sign. **It is NOT a
lower bound, and is NOT used to select the production algorithm (per @Codex `ef076ca3`)** — the larger
product gap is an observation at three points, not a proven inequality, and at res=1 the product *boosts*
8 k (resonance), so the res=1 "gap" is a boost difference rather than a rejection difference. Neither the
TPT 1.57 dB nor the generic 2.21 dB is claimed as the product's expected post-fix gap, and no <1 dB claim
is made.

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

**Artifacts (committed):** `report/gh20-probe/gh20_scenarios.tsv` (**7958 cells**: 7560 cutoff_norm +
384 crossrate + 8 asym_lr + 4 floor + 2 level), `report/gh20-probe/gh20_cpu.tsv` (machine +
finite/block-partition), `report/gh20-check.txt` (gate pass).

**CI-enforced tests:** `gh20_vcf_probe`, `gh20_vcf_acceptance`, `gh20_vcf_negative` (UNIX).

---

## 10. Verification & next step

- **Full regression:** build + `ctest` 74/74 passed (incl. `gh20_vcf_acceptance`, `gh20_vcf_negative`);
  the GH#19 `gh19_blamp_lut_gate` and host wiring/stream-plan/codec/CoreAudio tests all green.
- **Production:** 0 DSP / default / routing / registry / persistence changes. GH#20 **not** fixed.

**Submission record (prior docs commit — `265f4de`):**
- Prior documented submitted SHA: **`33d6020`** (branch `measure/20-vcf-response`, stacked on
  `fix/19-triangle-blamp`); docs follow-up head **`265f4de`**.
- Stacked PR: **#25** (draft, base `fix/19-triangle-blamp`, head `measure/20-vcf-response`), MERGEABLE.
- 4-platform CI: manually dispatched (`workflow_dispatch`) because this PR's base is not `main` and the branch
  is not `feat/**`, so the `ci.yml` auto-triggers (`push main|feat/**`, `pull_request→main`) do **not** fire.
  Run **`34069185210`** @ exact head `33d6020` — **four matrix platforms all SUCCESS**
  (`ubuntu clang++` 6m55s, `ubuntu g++` 7m53s, `macos clang++` 6m55s, `windows cl` 13m26s);
  the `full coverage (--require-full)` job is **skipped** (`pull_request`-gated) — identical to the
  GH#19 `a27d807` / `942ad82` approved baseline state (0 new, gate unchanged).

**This document is a FURTHER unpushed REVISION** (per @Codex `f7c895bd`: run the full matrix + directional
negatives now, do **not** push unreviewed revisions nor repeat full CI). It expands the matrix to
res ∈ {0, 0.5, 1} × two legal levels (**7958 cells**), re-measures §2/§3/§5/§7/§8a, and is awaiting
@Codex algorithm ruling. **Exact local commit: `800ed40`** (on top of `265f4de`; origin branch
`measure/20-vcf-response` is unchanged at `33d6020` — **not pushed**; no full CI run per `f7c895bd`).

**Next step after this revision:** the res sweep requested in the prior next-slice is **done** (§2/§3/§5).
What remains: (1) @Codex algorithm ruling on the candidate direction (A = topology → TPT/ZDF, or B =
remap the Chamberlin); (2) optionally, a per-sample `ns/sample` CPU figure for the baseline VCF (the
`gh20_cpu.tsv` emits the machine + finite/block-partition columns, not yet a cost-per-sample); (3) after
the ruling, implement the chosen correction-candidate in an isolated branch with the above acceptance
criteria. No production DSP change and no GH#20-fixed claim until @Codex rules.
