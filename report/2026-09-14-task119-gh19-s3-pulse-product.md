# task #119 — GH #19 S3: the pulse two-edge correction, carried to the PRODUCT and put behind ONE acceptance gate

Baseline: `d9991872d1375318e81ca96a44995915c7a70ffd` (`origin/main` tip at claim time).
Worktree: `wt-119-gh19-s3-pulse-product`, branch `feat/19-s3-pulse-product`, **unpushed**.
Reuses the reviewed, frozen candidate `ac226f12125d2c0bf29b93824208ead341f16807` (task #118 rev 3) as
its own commits; task #118 is CLOSED and its head `ac226f1` is not rewritten.
Nothing was pushed, no PR was opened, no registry / wire / default mapping / gate threshold was
touched, the shared checkout was not written to, and GH #19 / #15 remain open with **no MET claim**.
`build-119-rel/` is a regenerable artifact and is left untracked.

This report is the production-side record. The **historical argument** that used to live in
`core/include/lunar24/core/pulse_blep_kernel.h` — the 0.5625-at-`duty = 2*dt` refutation of the
withdrawn duty guard, the boundedness case analysis, the out-of-domain discussion — was moved here
and into `report/2026-09-14-task118-gh19-s3-pulse-aa.md` (the revision record), on the director's
instruction: *「生产头保留最终公式/适用边界即可，历史争论放报告」*. §7 records that the header
change is comment-only, verified.

---

## 0. The production delta, in full

ONE file, ONE function, plus the comment block that documents it — `core/include/lunar24/core/
pulse_blep_kernel.h`:

```cpp
inline double polyblepPulseCorrection(double t, double duty, double dt) {
  if (!(dt > 0.0)) return 0.0;
  const double w = (dt < kPolyblepMaxDt) ? dt : kPolyblepMaxDt;
  double b = t - duty;
  b -= std::floor(b);
  return polyblepResidual(t, w) - polyblepResidual(b, w);
}
```

* the two edges get their own kernel, each with its own SIGN and its own POSITION (edge A at phase 0
  is an upward jump, edge B at `duty` a downward one);
* the correction is added into the **existing** continuous mix at the **existing** pulse weight;
* **only the kernel WIDTH is capped**; the phase accumulator still advances by the TRUE step;
* the sync jump amount keeps using the same actual output function;
* phase advance, frequency / V·OCT / CV law, PWM transfer, sub, default mapping, registry and wire
  are unchanged.

Applicability boundaries, as authorized: **no duty-dependent switch** (adding one is a defect, and
the guard that was withdrawn is retained only as a negative control), and above
`dt = kPolyblepMaxDt = 0.5` the width is capped while the phase is not — a **bounded continuous
extension**, with **no anti-aliasing claim** there.

---

## 1. THE ONE ACCEPTANCE GATE

### 1.1 What it is

`tools/gh19_s3_pulse_acceptance.py`, driven by the **pinned expectation**
`report/gh19-s3-pulse-product/acceptance_criteria.tsv` (72 `cell` rows + 5 `criterion` rows + 4
`axis` rows + 8 `report_only_cell` rows + 6 `report` rows). The expected cell set is pinned in that
file and is **never inferred from the tool's own output**; two independently produced artifacts (the
two arms' analyzer reports and the delta matrix) are cross-checked against it.

The gate is per cell, over the original **72 cells**: A/B × four sample rates × 220/440/880 Hz ×
PW .1/.5/.9, `morph=1`, `depth0`, on the **preserved PR44-uncorrected baseline**:

| criterion | threshold | measured |
| --- | --- | --- |
| effective full band, total diagnostic residual | ≥ 6 dB better per cell | **all 72 pass** |
| 100–5000 Hz, total diagnostic residual | ≥ 6 dB better per cell | **all 72 pass** |
| real fundamental amplitude, relative change | ≤ 1 % | max 0.090 % |
| DC, absolute difference | ≤ 0.0015 | max 0.00010 |
| actual frequency | unchanged | unchanged (fitted, reported) |

```
ACCEPT-GATE criteria=acceptance_criteria.tsv cells=72 report_only=8 plan_static=80
ACCEPT-GATE verdict=PASS refusals=0 failures=0
ACCEPT-REPORT worst_full_band_improvement_db=-7.66
ACCEPT-REPORT worst_band_1k5k_improvement_db=-24.15
```

Recomputing the *minimum* improvement per cell straight out of the published `deltas.tsv` gives
≈ −7.00 dB (effective full band) and ≈ −13.63 dB (100–5000 Hz) — i.e. the binding cell still clears
the 6 dB floor by more than a decibel, and the two columns are **not the same measurement** (their
per-cell values differ by up to 24 dB). `worst_*` in the tool's own summary lines is the
**largest** improvement, not the smallest; both readings are printed by the tool.

**Fixed reference, fixed phase, fixed denominator.** The two columns are total output residuals
measured against one fixed reference with one fixed phase and one declared denominator; **no phase
is re-fitted**, no threshold is changed, and the fitted amplitude `A` is reported **separately**
(`max_abs_fitted_scale_delta`, `max_pinned_scale_cost_db`) so that no fit can stand in for gain.
`rel/abs` band gains are not claimed: the columns are residual *drops*, not kernel gains.

### 1.2 Its negative controls — 22 of 22 hit

`report/gh19-s3-pulse-product/gate_negcontrol.txt`: 22 controls, each exiting with its own declared
failure code, `NEGCTL-GATE ok=22 bad=0 controls=22`, `verdict=ALL-HIT`. They cover the rejection
list the order fixed: empty data, header-only, one arm missing a cell, **both arms missing the same
cell** (a missing row cannot be washed out by both arms agreeing), duplicated delta row, ID swap,
non-finite delta / non-finite matrix, wrong kind, wrong metadata, arms not from these matrices,
power-unit tripwire removed, alignment-gate failure, instrument-failure marker, criteria row dropped
/ invented threshold / report dropped / duplicate cell / shrunk enumeration, plan dropping a gate
cell, plan dropping a report-only cell.

### 1.3 What was NOT done to the gate

The 12 pre-existing `--require-full` coverage gaps are **listed separately and unchanged**; the full
coverage gate was not touched. Task #118's sensitivity experiments are kept as **research artifacts**
and are **not** presented as a shared acceptance gate. The high-frequency 8 cells and the mixed 6
cells continue to be reported in full and are **not** counted into the 72-cell threshold. The
`step > 0.5` domain is **not** claimed as an anti-aliasing improvement.

---

## 2. THE PRODUCT ACCEPTANCE SURFACE

`tests/host/test_gh19_s3_pulse_product_acceptance.cpp` (registered in `CMakeLists.txt`).
**119 checks, exit 0** on the current head — built and run through the project's own CMake Release
target, not only through the mutant runner's compiler invocation.

Everything runs through the ONE agreed entry — `encode_device_state → decode_device_state →
StandaloneAudioEngine::applyDeviceState → StandaloneAudioEngine::processBlock` — and asserts on the
**real captured** `WET_L/WET_R/DRY_A/DRY_B` and on the read-only published runtime. No test here
calls `Vco`, `buildMachineRuntimeCandidate` or `processFrame`, and **no test slices a Vco render loop
into blocks**: task #118's `VCO-BLOCK` evidence is Vco-level and cannot stand in for a product-block
claim (order, and the `VCO-BLOCK` note).

Claims carried, and the measured numbers behind them:

| claim | how | measured |
| --- | --- | --- |
| the DRY sequence **is** the two-edge formula, sample by sample | longhand reference model, at the pinned origin phase (no free parameter) | residual 1.4e-11 … 7.0e-11 vs `kFitTol = 1e-5` |
| it is **not** the naive shape / wrong sign / wrong edge / withdrawn guard / uncapped width | same model, each variant at its own best phase (best case for the variant) | naive 0.41–0.62, sign 0.83–1.00, pos 0.48–0.50, guard 0.62 — all past `kFitMargin = 1e-2` |
| the driven step is the step the output is a function of | ±1 ppm must be excluded by `kFitTol`; ±0.1 % must clear `kFitMargin` | 1.198e-3 (2 orders), 0.943 / 0.930 |
| the phase advance is unchanged | independent rising-edge index | measured period 109.083 frames vs driven 109.091 |
| the cap bites and the phase does not | four V·OCT cells drive `step` across 0.5 **inside the legal reachable box** | driven 0.547 … 1.806; continuous V·OCT sweep 0.0001…9.9999 V ⇒ step 0.147…150.18, four outputs finite throughout |
| PWM is consumed in the frame it is published | published-CV trace vs duty trace | **4800 / 4800** same-frame, 0 wrong-frame; duty 0.650 vs idle 0.500 |
| depth 0 leaves the base duty | | unchanged |
| a saved PWM cable restores onto the DSP | repeated restore | 0.700 → 0.500 |
| hard sync is sized on the EMITTED signal | first divergence frame vs the free arm | reset sample `0.250000000000 == 0.5 * free` **exactly** |
| a rejected apply preserves state / format / plan / subsequent trace | four rejected-apply arms | all preserved (rejection is the `route_incoherent` / range path, not a silent retry) |
| irregular `processBlock` partitions are invariant — with a **changing** CV | uniform vs irregular partition | identical, **and** the capture is asserted to be moving (non-vacuity) |
| the render callback allocates / frees nothing | allocator TU, with a probe-activity positive control | 0 / 0 over 24 blocks |

**Unchanged non-pulse paths and the original triangle / S5 numeric baselines are preserved; no old
assertion was deleted to make this green.**

---

## 3. TWO CLASSES OF ALIGNMENT EVIDENCE — AND WHY THEY ARE NOT INTERCHANGEABLE

Director note (msg `6b58ef7b`): *「变体各自拟合相位可以用于诊断，但不能替代接受式的绝对时序验证；72格数值门仍按原固定相位参考，不得拟合相位。交付时请分清这两类，并保留独立源帧/相位推进或sync帧对账，使整体晚一帧不能靠拟合通过。」*

The two classes are now separated in the code and in this report:

**(a) Fitted alignment — DIAGNOSTICS.** `fitPhi` recovers one constant phase per cell, and each
*negative control* variant is then re-aligned on **its own** best phase (`bestVariantWorst`). That is
the strongest form of "this variant cannot explain the capture", and it is what stopped a negative
control from failing open when a mutated product moved the accepted fit. It is **a free parameter**:
`frac(phi + i*step)` with `phi' = phi - step` is the same sequence, so **no fit can ever reject a
uniformly delayed capture**. It proves nothing about absolute timing and is not asked to.

**(b) The absolute ORIGIN anchor — A GATE.** `theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance`,
carried on **both** model domains (the 5 in-domain core cells and the 4 capped `step > 0.5` cells).
The capture starts at the engine's frame 0; the product's per-frame loop advances the phase
accumulator and then publishes what it advanced to (`Vco::tick`: `cumPitch_ += step` … then
`emittedAt_(cumPitch_, step)`), so frame 0 is emitted at phase = one step. The alignment is therefore
**not free**: it is `frac(step)`, fixed by the product's own frame convention. Judging the accepted
model at that phase — no search, no free parameter — is the timing criterion. Measured:
`fitted − pinned = 1e-10` (the fit lands on the pinned phase within its own resolution), and the
residual at the pinned phase is **1.4e-11 … 7.0e-11**, i.e. *better* than the fit-limited value the
fitted check reports (up to 3.9e-8), because the grid search is the limiting factor there.

Both numbers and their difference are printed per cell (`origin[...]`), so the reconciliation is
readable in the run rather than asserted in prose.

**(c) The sync-frame reconciliation is REPORTED, not asserted — and that is deliberate.**
The test prints the frame the emitted reset landed on against the frame the **published** sync source
crossed its 5 V gate (measured: divergence at frame 47, source crossing at frame 0). It is not turned
into a criterion because that offset is derived from a **floating-point threshold crossing** in
pre-existing product scheduling; pinning it as an integer constant would make a cross-platform
constant load-bearing on a claim this change does not own (this project has been burned by exactly
that class of constant before — local macOS/clang green is not MSVC green). The origin anchor in (b)
already provides the source-frame / phase-advance reconciliation the note asks for, and §4 shows it
**discriminates**. If the director wants the sync offset asserted anyway, say so and it becomes one
line — with the portability caveat recorded next to it.

---

## 4. THE ISOLATED-PRODUCTION MUTANT MATRIX — 9 mutations, 9 RED, 0 INVALID

`tools/run_gh19_s3_pulse_mutants.py`. Every load-bearing claim is re-run against a production
**header** mutated away from it, in an **isolated shadow include tree**: the committed tree is never
written to. What counts as RED is defined so that the two failure surfaces stay separate:

* the REFERENCE arm must exit 0 **and** print its COMPLETE summary **and** report 0 failures;
* each MUTANT arm must exit with a normal failure status (1), print its complete summary, **and name
  the specific check that mutation is supposed to trip**;
* a build failure, a signal/abort exit, a zero exit, a truncated summary, or a failure on some OTHER
  check is **INVALID, not a red**.

| mutation | file | named check it must trip | result |
| --- | --- | --- | --- |
| bypass-correction | `pulse_blep_kernel.h` | `theProductPulseEqualsTheTwoEdgeFormula` | RED 29/119 |
| wrong-sign | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWrongSignVariant` | RED 25/119 |
| wrong-position | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWrongEdgePositionVariant` | RED 10/119 |
| restore-duty-hard-switch | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWithdrawnDutyGuardedVariant` | RED 15/119 |
| remove-width-cap | `pulse_blep_kernel.h` | `theProductPulseIsNotTheUncappedKernelVariant` | RED 11/119 |
| pwm-one-frame-late | `machine_runtime.h` | `thePwmSinkConsumesTheSameFramesPublishedCv` | RED 2/119 |
| block-boundary-graph-drive | `device_adapter.h` | `irregularBlocksReproduceTheUniformPartitionExactly` | RED 1/119 |
| phase-reset-per-sample | `machine_runtime.h` | `thePartitionStimulusIsChanging` | RED 22/118 |
| **output-one-frame-late** | `device_adapter.h` | **`theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance`** | **RED 9/119** |

(`phase-reset-per-sample` reports 118 rather than 119 because the frozen capture it produces never
reaches the edge-index check, which is skipped when fewer than 8 edges are measured — an expected
consequence of that mutation, not a truncated run: the summary line is still complete.)

A mutant tripping *more* checks than its target is normal and does not weaken the classification —
what matters is that each mutation trips **its own** named check. The converse is the interesting
one, and it is exactly the ninth mutation's result below.

### 4.1 The ninth mutation is the demonstration the note asked for

`output-one-frame-late` holds the product's output back one frame at its **real output boundary**
(`DeviceAdapter::renderBlock`, a per-adapter latch, so the delay persists across the engine's block
calls). It is the one mutation here that **a phase fit cannot see**, and the run shows exactly that:

| | fitted `phi` | fitted − pinned | residual at the **pinned** phase | fitted-phase check |
| --- | --- | --- | --- | --- |
| reference | `frac(step)` ± 1e-10 | −1.8e-10 … +2.4e-10 | 1.4e-11 … 7.0e-11 | GREEN |
| one frame late | **0.000000000** (the fit slid one whole step left) | **−step**, exactly | **0.197 … 0.750** | **still GREEN** |

So a uniformly late emission passes the fitted check — by construction, not by accident — and is
rejected **only** by the origin anchor: in that arm all **9** failures are
`theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance` (5 in-domain + 4 capped cells) and
`theProductPulseEqualsTheTwoEdgeFormula` does not appear among the failures at all. That is the
concrete evidence that the absolute-timing criterion is not decorative.

---

## 5. TWO DEFECTS THIS WORK FOUND IN MY OWN TEST (both fixed by strengthening)

1. **The variant negative controls inherited the accepted model's `phi` and could fail OPEN.** On a
   product that really is the wrong-sign variant, the objective at the accepted alignment goes flat
   and the fit drifts somewhere meaningless (measured: at `duty = 0.5` it drifted to `phi ≈ step`,
   where the wrong-sign variant *matched*), so the check whose job is to reject that variant passed.
   Fix: each variant is aligned on **its own** best phase (`bestVariantWorst`). This is a
   strengthening — it makes every negative control a best-case-for-the-variant claim.
2. **The partition-invariance comparison passed vacuously on a frozen capture.** When the DRY_A
   signal does not move at all (measured `span 0.000000000, differing 0`), "the two partitions agree"
   is trivially true. Fix: the non-vacuity guard is folded into the same named assertion
   (`same && thePartitionStimulusIsChanging`), so a flat capture can no longer buy the invariance
   claim — and it is now the surface that catches the per-sample phase reset.

Neither fix moved a threshold; both were forced by mutations that had been graded INVALID.

---

## 6. ONE MEASURED ARCHITECTURE FINDING (handed over for review)

**The product drives the runtime ONE FRAME AT A TIME.** `DeviceAdapter::renderBlock` loops
`rt.processBlock(&in, 1, &out, true)` (`device_adapter.h`, "one drain, one frame, one timebase"), and
`StandaloneAudioEngine::processBlock` hands it the whole block. So `MachineRuntime::processBlock`
**never sees a multi-frame block on the product path**, and:

* the product's block boundary lives in the **adapter**, not in the runtime — the block-boundary
  mutation therefore had to be written against `device_adapter.h`;
* a mutation inserted at the runtime's block entry degenerates to a **per-sample** mutation: a
  `syncPulse()` there resets the phase every sample and *freezes* the pulse (measured `|DRY_A| span
  0.000000000, differing 0`), which is why it is kept as its own documented mutant and caught by the
  non-vacuity guard rather than by the partition comparison.

---

## 7. THE HEADER SLIMMING IS COMMENT-ONLY (verified)

`git diff -U0 core/include/lunar24/core/pulse_blep_kernel.h`, with every comment line filtered out,
contains **no code line**. The kernel's code is byte-for-byte what task #118 rev 3 delivered; only
the 123-line historical preamble was condensed to the final formula + the two applicability
boundaries, with pointers to this report and to `report/2026-09-14-task118-gh19-s3-pulse-aa.md` for
the history. The full mutant matrix was **re-run on the slimmed header** and reproduced the run on
the un-slimmed one exactly (same arms, same named checks, same failure counts, 0 INVALID, reference
GREEN); the ninth mutation and the origin anchor were added afterwards and the matrix re-run again,
which is the 9/9 result in §4.

---

## 8. WHAT IS **NOT** CLAIMED

* No anti-aliasing improvement above `dt = 0.5` (bounded continuous extension only).
* No complete dynamic band-limiting: dynamic PWM remains a frozen-duty approximation.
* No band gain: the two gate columns are **output residual drops**, and the fitted amplitude is
  reported separately, never used to hide gain.
* The `step > 0.5` reachable box is stated as measured (`0.5 … 1307.7` on the declared input box),
  not as a hardware fact.
* Not merged, not pushed, no PR, GH #19 / #15 open, no MET.

---

## 9. EVIDENCE FILES

| file | what it is |
| --- | --- |
| `report/gh19-s3-pulse-product/acceptance_criteria.tsv` | **the pinned expectation** (72 gate cells + criteria + axes + 8 report-only cells + reports) |
| `report/gh19-s3-pulse-product/gate.txt` | the gate's verdict block, with every input's sha256 |
| `report/gh19-s3-pulse-product/gate_negcontrol.txt` | 22 / 22 rejection controls |
| `report/gh19-s3-pulse-product/base_report.txt` / `cand_report.txt` | the two arms' analyzer output |
| `report/gh19-s3-pulse-product/deltas.tsv` | the per-cell delta matrix (94 referenced cells) |
| `report/gh19-s3-pulse-product/gh19_s3_plan.tsv` | the static plan (80 cells) |
| `tools/gh19_s3_pulse_acceptance.py` | the gate |
| `tools/run_gh19_s3_pulse_mutants.py` | the 9-mutation matrix (isolated shadow include tree) |
| `report/gh19-s3-pulse-product/mutants.txt` | that matrix's full output (9 RED, 0 INVALID, reference 119 checks OK) |
| `report/gh19-s3-pulse-product/product_acceptance_run.txt` | the CMake-built acceptance test's own output (119 checks OK, rc 0) |
| `tests/host/test_gh19_s3_pulse_product_acceptance.cpp` | the product acceptance surface (119 checks) |
