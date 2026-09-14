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
**122 checks, exit 0** on the current head — built and run through the project's own CMake Release
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

**(c) The sync-frame reconciliation is ASSERTED, against a reference rebuilt from the published
source — and the earlier "report-only" form was WRONG.** The first revision of this test derived the
sync event by *scanning for the first output divergence* and compared it against "the frame the
published source crossed 5 V", printing a 47-vs-0 mismatch as an incidental observation. That
comparison had the wrong comparison object: the product's sync sink does not threshold at 5 V. It is
a **hysteresis latch with first-sample priming** (`core/include/lunar24/core/sink_interpret.h:68–96`):
the low state rises only at `threshold + hysteresis` and then holds until it falls below
`threshold − hysteresis`, and `st.primed` means the **very first sample can only prime** — it can
never emit an edge. This jack's own descriptor declares 5 V / 0.2 V, so the rise is at **5.2 V** and
the hold floor is **4.8 V**. Deriving the event by looking at where the *output* first disagrees is
also circular: it reads the divergence back out of the thing under test.

The criterion is therefore rebuilt from the source, in three independent steps:

1. `find_jack(vco_a_sync_in)` fetches the jack's **own declared** `gateThresholdVolts` /
   `hysteresisVolts` and the test first requires that they describe a real hysteresis gate
   (`thr > 0 && hyst > 0`) — if the descriptor is not one, the prediction is not attempted and the
   check fails rather than silently passing on a degenerate reference.
2. From the **published** sync volts alone, a fresh `high`/`prev`/`primed` state machine replays the
   same law and yields `predictedEdgeFrame`. No output is consulted.
3. At that frame, and only there, the reset is checked on the audio: `aSync[predicted] ==
   0.5 * aFree[predicted]` (the half-amplitude restart), the jump magnitude `> 0.1` (so the reset is
   sized on the **emitted** signal, not the raw shape), every frame **before** the predicted frame is
   bit-identical between the synced and free runs, the **first** divergence is exactly the predicted
   frame, and the post-edge divergence remains `> 0.1` (the slave genuinely restarted rather than
   glitched once).

Measured: `thr=5.0000 hyst=0.2000`, holds above `4.8000`, frame 0 primes, the hysteresis reference
raises its first rising edge at **frame 47** — and the first output divergence is **also frame 47**,
with `sync=0.250000000000` against `0.5*free=0.250000000000`. The 5 V reading was the error; the
frame-47 agreement is now a prediction confirmed at the frame it named, not a scan.

**Record correction:** the test wires `lfo_a_cv_out` into `vco_a_sync_in`. An earlier note of mine
wrote `lfo_b`; that was wrong (`lfo_b_cv_out` belongs to a different allocator test). The `lfo_a`
record under §7 is the accurate one.

The removed scan is replaced by an isolated mutation, `sync-consumer-one-frame-late` (§4), which
delays the *detected* edge by one frame while leaving detection otherwise intact; it is rejected by
name on `theResetLandsOnTheFrameTheHysteresisReferencePredicted` — i.e. the criterion really is
pinned to the frame the hysteresis law predicts, not merely to "some divergence happened".

---

## 4. THE ISOLATED-PRODUCTION MUTANT MATRIX — 10 mutations, 10 RED, 0 INVALID

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
| bypass-correction | `pulse_blep_kernel.h` | `theProductPulseEqualsTheTwoEdgeFormula` | RED 31/122 |
| wrong-sign | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWrongSignVariant` | RED 25/122 |
| wrong-position | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWrongEdgePositionVariant` | RED 10/122 |
| restore-duty-hard-switch | `pulse_blep_kernel.h` | `theProductPulseIsNotTheWithdrawnDutyGuardedVariant` | RED 15/122 |
| remove-width-cap | `pulse_blep_kernel.h` | `theProductPulseIsNotTheUncappedKernelVariant` | RED 11/122 |
| pwm-one-frame-late | `machine_runtime.h` | `thePwmSinkConsumesTheSameFramesPublishedCv` | RED 2/122 |
| block-boundary-graph-drive | `device_adapter.h` | `irregularBlocksReproduceTheUniformPartitionExactly` | RED 1/122 |
| phase-reset-per-sample | `machine_runtime.h` | `thePartitionStimulusIsChanging` | RED 22/121 |
| **output-one-frame-late** | `device_adapter.h` | **`theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance`** | **RED 12/122** |
| **sync-consumer-one-frame-late** | `machine_runtime.h` | **`theResetLandsOnTheFrameTheHysteresisReferencePredicted`** | **RED 3/122** |

(`phase-reset-per-sample` reports 121 rather than 122 because the frozen capture it produces never
reaches the edge-index check, which is skipped when fewer than 8 edges are measured — an expected
consequence of that mutation, not a truncated run: the summary line is still complete.)

A mutant tripping *more* checks than its target is normal and does not weaken the classification —
what matters is that each mutation trips **its own** named check. The converse is the interesting
one, and it is exactly the `output-one-frame-late` result below.

### 4.1 `output-one-frame-late` is the demonstration the note asked for

`output-one-frame-late` holds the product's output back one frame at its **real output boundary**
(`DeviceAdapter::renderBlock`, a per-adapter latch, so the delay persists across the engine's block
calls). It is the one mutation here that **a phase fit cannot see**, and the run shows exactly that:

| | fitted `phi` | fitted − pinned | residual at the **pinned** phase | fitted-phase check |
| --- | --- | --- | --- | --- |
| reference | `frac(step)` ± 1e-10 | −1.8e-10 … +2.4e-10 | 1.4e-11 … 7.0e-11 | GREEN |
| one frame late | **0.000000000** (the fit slid one whole step left) | **−step**, exactly | **0.197 … 0.750** | **still GREEN** |

So a uniformly late emission passes the fitted check — by construction, not by accident — and is
rejected **only** by the origin anchor: in that arm all **12** failures are
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
GREEN); the one-frame-late mutants and the origin anchor were added afterwards and the matrix
re-run again, which is the 10/10 result in §4.

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

## 9. THE VERDICT IS NOW PRODUCED FROM THE CURRENT HEAD, NOT READ FROM A CHECKED-IN FILE

The gap this section closes was real and stated plainly: `CMakeLists.txt` registered only the
**measurement probe** (`gh19_s3_pulse_probe`, label `slow`) and the product acceptance test. Nothing
in the repository's build or CI ever invoked `tools/gh19_s3_pulse_acceptance.py`. The 72-cell
verdict lived in a **checked-in** `cand_report.txt` / `deltas.tsv` pair, so "the gate passes" was a
statement about a file, and a later edit to the kernel would not have moved it. The 9-arm mutant
runner did not cover this either: it runs the C++ product assertions, not the gate.

### 9.1 `tools/run_gh19_s3_pulse_pipeline.py` — the slow pipeline

One driver, five steps, every artifact regenerated from the tree under test:

0. **Baseline pin.** `--baseline-arm/gh19_s3_scenarios.tsv` is sha256-checked against
   `BASELINE_SCENARIOS_SHA256 = f3075e7e…`; any drift prints `REFUSE BASELINE-PIN` and exits **4**.
   The historical baseline is a fixed reference the candidate is *compared against*, so it may not
   silently move with the tree.
1. **Fresh probe** → the candidate arm's scenarios + plan.
2. **Fresh analyzer** (`--align-check --self-check --taps 2001`) → the candidate report.
3. **Fresh delta** against the pinned base report and the pinned expected-cell manifest.
4. **Gate**, handed the **fresh** plan and the **fresh** report, its stdout echoed.

`--cand-report` is deliberately **not** an argument of the driver: there is no way to ask it to judge
a checked-in file, so "the gate is judging committed output" is not a reachable state.

Evidence, from the current head (`report/gh19-s3-pulse-product/pipeline_run.txt`):

```
ACCEPT-PIPELINE baseline_pin=OK sha256=f3075e7ea021953a487ada97b9c8249ec4466c9ad47d0cb0ea3078f2a79d61bc
ACCEPT-PIPELINE fresh_arm=/tmp/gh19s3-pipe-ref/cand plan_sha256=621b0c758959b20623e0b4596a789f266aad3d6aef9dc1a271144f975eb57811
ACCEPT-PIPELINE fresh_report=… sha256=27589bd045549b366a9c2b29424cacc24bfef1629b51b2c9cbfee9ddf12d35fc
ACCEPT-PIPELINE deltas=… sha256=ea3755033164d37fbbcb72b2b1ca096b9805db0291eef332a322698202f6a12b
ACCEPT-GATE verdict=PASS refusals=0 failures=0
NEGCTL-GATE ok=22 bad=0 controls=22
NEGCTL-GATE verdict=ALL-HIT
```

The freshly rendered report's sha256 is **identical** to the committed `cand_report.txt` — the
committed artifact is corroborated by a re-render, not merely trusted. The 22 refusal controls are
run inside the same pipeline invocation on the same artifacts.

### 9.2 `tools/run_gh19_s3_pulse_pipeline_mutant.py` — the pipeline must be able to say no

A gate that only ever says PASS is indistinguishable from one that cannot fail. This runner compiles
the probe **twice** — same source, same compiler, same flags — differing only in a shadow include
root holding one mutated header, and puts **each** through the same driver CMake invokes. The
mutation is `bypass-correction`: `polyblepPulseCorrection()` returns `0.0` immediately, so the
product emits the naive pulse. That is exactly the thing this slice exists to prevent.

Grading separates the three outcomes that are always in danger of being conflated:

* the **reference** arm must exit **0** — otherwise a red mutant would only prove the pipeline broke;
* the **mutant** arm must exit **exactly 1** (the gate's RED);
* a refusal (exit **4**) is **explicitly not** a catch: it means the artifacts failed an input check
  and *no judgement was issued*. It is rejected as evidence by the runner's own grading;
* the mutant must print the **named** `ACCEPT-FAIL-IMPROVEMENT` and `ACCEPT-GATE verdict=RED` —
  "non-zero exit" alone would pass on any other failure.

The mutation anchor is verified to occur **exactly once** before anything is compiled: a stale anchor
would stage a pristine "mutant" that passes, which would read as "the pipeline cannot see this
defect" when in fact the defect was never applied.

Result (`report/gh19-s3-pulse-product/pipeline_mutant_run.txt`):

```
[reference         ] pipeline rc=0  named_improvement_failure=False  verdict_red=False
[bypass-correction ] pipeline rc=1  named_improvement_failure=True   verdict_red=True
ACCEPT-FAIL-IMPROVEMENT vco_a_pulse_44100_220_pw10: dreseffbd_db=+0.00 (need <= -6.00) dres1k5k_db=+0.00 (need <= -6.00)
…
ACCEPT-GATE verdict=RED refusals=0 failures=72
PIPELINE-MUTANT PASS
```

Both runners are registered in `CMakeLists.txt` with label `slow`, alongside the measurement probe,
so the PR-only `probe-gate` job (`ctest --label-regex slow`) runs probe → analyze → fix-baseline
reconciliation → 72-cell acceptance, and the rejection self-test, on the current head.

### 9.3 What this does not claim

The gate still measures a **frozen-duty** per-cell approximation; the pipeline makes the verdict
reproducible from the head, it does not widen what the verdict covers. The 12 pre-existing
`--require-full` coverage gaps are **unchanged and still listed separately**.

## 11. EVIDENCE FILES

| file | what it is |
| --- | --- |
| `report/gh19-s3-pulse-product/acceptance_criteria.tsv` | **the pinned expectation** (72 gate cells + criteria + axes + 8 report-only cells + reports) |
| `report/gh19-s3-pulse-product/gate.txt` | the gate's verdict block, with every input's sha256 |
| `report/gh19-s3-pulse-product/gate_negcontrol.txt` | 22 / 22 rejection controls |
| `report/gh19-s3-pulse-product/base_report.txt` / `cand_report.txt` | the two arms' analyzer output |
| `report/gh19-s3-pulse-product/deltas.tsv` | the per-cell delta matrix (94 referenced cells) |
| `report/gh19-s3-pulse-product/gh19_s3_plan.tsv` | the static plan (80 cells) |
| `tools/gh19_s3_pulse_acceptance.py` | the gate |
| `tools/run_gh19_s3_pulse_mutants.py` | the 10-mutation matrix (isolated shadow include tree) |
| `report/gh19-s3-pulse-product/mutants.txt` | that matrix's full output (10 RED, 0 INVALID, reference 122 checks OK) |
| `report/gh19-s3-pulse-product/baseline_arm/gh19_s3_scenarios.tsv` | the **pinned historical baseline** (`f3075e7e…`), byte-identical across two independent renders |
| `report/gh19-s3-pulse-product/pipeline_run.txt` | the slow pipeline's full output from the current head (pin OK, fresh report sha, PASS, 22/22 controls) |
| `report/gh19-s3-pulse-product/pipeline_mutant_run.txt` | the same pipeline on the bypass-correction mutant (rc 1, `ACCEPT-FAIL-IMPROVEMENT`, `verdict=RED`) |
| `tools/run_gh19_s3_pulse_pipeline.py` | the slow pipeline (probe → analyze → reconcile → 72-cell gate) |
| `tools/run_gh19_s3_pulse_pipeline_mutant.py` | the pipeline's rejection self-test |
| `report/gh19-s3-pulse-product/product_acceptance_run.txt` | the CMake-built acceptance test's own output (122 checks OK, rc 0) |
| `tests/host/test_gh19_s3_pulse_product_acceptance.cpp` | the product acceptance surface (122 checks) |
