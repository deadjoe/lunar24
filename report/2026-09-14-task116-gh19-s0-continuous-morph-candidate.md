# Raft task #116 (GH #19 S0) — continuous single-knob waveform mapping: runnable candidate + compatibility verification

Baseline `cc92b74cb2db5d75378c1886543e4169f2cf34d8` (parent of the #111/S5 merge tip).
Own isolated worktree, detached HEAD. **Not pushed, no PR.** Registry / target / wire /
disposition are **untouched**: nothing in this slice is a `ParameterId`, a persisted field, or a
state-disposition change.

Owner input: @bearbone Raft msg `b4ffcd85` (2026-09-13) — on the Elta Solar 42 / 42N the main V/Oct
VCOs' MORPHING WAVEFORM knob turns **continuously**. @Codex `d2b3f787` fixed the direction: ONE knob,
reuse the EXISTING per-side `morph`, no new wave selector, no second morph control quantity.
@Codex `ad68abb6` allows equal division as a provisional candidate; what is forbidden is presenting a
software segmentation as hardware structure.

---

## 1. The two complete candidates

Both are expressed by one pure, header-only function
(`core/include/lunar24/core/vco_wave_map.h`, `wave_map::sampleAt`). They differ **only** in the
boundary vector — node order, node shapes, blend form, phase convention and BLAMP rule are shared.

### 1.1 Shared structure

**Phase convention** — identical to production (`Vco::waveformSampleAt`, `core/include/lunar24/core/vco.h:303-326`):
`p = frac(cumPitch) ∈ [0,1)`, one cycle per unit.

| node | `Node` | shape `W(p)` |
|---|---|---|
| saw | `kSaw = 0` | `2p − 1` |
| inverted saw | `kInvSaw = 1` | `−(2p − 1)` |
| sine | `kSine = 2` | `sin(2πp)` |
| triangle | `kTriangle = 3` | `4·|p − 0.5| − 1` |
| pulse | `kPulse = 4` | `(p < duty) ? +1 : −1` |

**Interval map.** Five boundary values `c0 < c1 < c2 < c3 < c4` (with `c0 = 0`, `c4 = 1`) cut the
norm axis into four stretches `Sj = [cj, cj+1]`, each between adjacent nodes. For `norm ∈ Sj`:

```
u        = (norm − cj) / (cj+1 − cj)          // local coordinate, 0..1
value(p) = (1 − u)·W_j(p) + u·W_j+1(p)        // partition of unity
```

This is the **only** interpolation; there is no per-stretch curve parameter and no extra control
quantity. Endpoints: `norm = cj ⇒ value = W_j` exactly (all `p`).

**Two named stretches reuse the EXISTING product implementations verbatim**, at `morph = u`:

* `S0 = [c0, c1]`: saw → inverted saw. Bit-equal to the existing `kMorphSawInvSaw` closed form
  `(1−m)·a + m·(−a)`, `a = 2p−1` (measured: `worst_err = 0.000e+00`).
* `S2 = [c2, c3]`: sine → triangle. Bit-equal to the existing `kMorphSineTriangle` closed form
  `(1−m)·sin(2πp) + m·(4|p−0.5|−1)` (measured: `worst_err = 0.000e+00`).
* `S1 = [c1, c2]` and `S3 = [c3, c4]` are **unnamed software connectors** — they exist only because
  one continuous sweep must get from one named feature to the next. Reported as such; never claimed
  as hardware structure.

**BLAMP rule.** The existing triangle slope correction is applied **only** to the triangle component,
scaled by its weight: `value += triangleWeight(c, norm)·triangleBlampCorr(cp, step)`. `triangleWeight`
is `u` on `S2`, `1−u` on `S3`, `0` elsewhere. First-order and provisional: exact at a pure-triangle
node, approximate inside a mix. **No mix is ever called band-limited.**

**The six manual names are not six anchors.** "There are 6 waveforms in total" counts named features
on the sweep: 4 nodes (saw, sine, triangle, pulse) + 2 named stretches (S0, S2). This is deliberately
*not* an equal division into six detents, and neither candidate presents its segmentation as hardware
structure.

### 1.2 Candidate A — `kRingEqual`

```
c = {0.0, 0.25, 0.5, 0.75, 1.0}
```

| stretch | interval | from → to | at its centre |
|---|---|---|---|
| S0 | [0.00, 0.25] | saw → inverted saw | **0.1250 — exactly silent** |
| S1 | [0.25, 0.50] | inverted saw → sine | 0.3750 |
| S2 | [0.50, 0.75] | sine → triangle | 0.6250 |
| S3 | [0.75, 1.00] | triangle → pulse | 0.8750 |

Pure software equal division, explicitly permitted as provisional by @Codex `ad68abb6`.

### 1.3 Candidate B — `kRingPanel`

```
c = {0.0, 0.2933, 0.3667, 0.8200, 1.0}
```

Derived from the six glyph centres measured on `solar42N_panel_2400px.png`
(`pdftoppm -r 2000`, angles from the knob centre, 0° = straight up): −75, −53, −20, +16, +48, +75 deg,
mapped angle → norm linearly. Node boundaries are the **direct** glyph reads:

```
t1 = 0.2933 -> invSaw   = c1 = t1       = 0.2933
t2 = 0.3667 -> sine     = c2 = t2       = 0.3667
t4 = 0.8200 -> triangle = c3 = t4       = 0.8200
```

`t0 = 0` → saw = `c0`, `t5 = 1` → pulse = `c4`. The stretch centre `t3 = 0.5933` is **redundant**: it
independently predicts `c3 = 2·t3 − c2 = 0.8467` against the direct read `0.8200` — a residual of
`0.0267` norm (≈ 4.0° of arc). That residual is this model's own consistency check and is reported as
such; the direct node read is used because it needs no "the label sits at the stretch centre"
assumption.

**The angle → norm linearity is UNVERIFIED and this is weak evidence.** Candidate B is a provisional
candidate, not a measurement of hardware structure. It is the only one of the two that has any panel
provenance at all.

### 1.4 Explicitly NOT claimed

The node **order** `[saw, invSaw, sine, triangle, pulse]` is itself provisional (P1): the panel's
glyph order cannot be read with confidence (task #115 glyph confidences: triangle-vs-saw "low", the
morph glyphs "medium"). Only the two named stretches' internal directions are manual-fixed
("saw to inverted saw", "sine to triangle"). Symbols that could not be uniquely read were recorded as
unreadable rather than guessed as confirmed.

---

## 2. Measured evidence (`report/gh19s0-morph-map/`, 53 checks, 0 failures)

Probe: `tests/probes/gh19s0_morph_map_probe.cpp`, driven through the **real** chain
`make_default_device_state → encode_device_state → decode_device_state →
StandaloneAudioEngine::applyDeviceState → processBlock` via `tests/host/test_engine_harness.h`
(`EngineHarness` DRY_A / DRY_B are the real VCO taps). The candidate is switched on through the owner
(`SynthRuntime::setVcoWaveMap`).

### 2.1 Four sample rates, full sweep (`m1_sweep.tsv`)

41 norms × 4 rates (44100/48000/88200/96000) × 2 candidates, peak + RMS of the **rendered** A and B
outputs. Every sample finite at every rate (`M1f`).

| candidate | rmsA over the sweep | note |
|---|---|---|
| `equal` | `0.000000 … 0.500000` | hits the S0 centre exactly (0.125 is on the 0.025 grid) |
| `panel` | `0.006474 … 0.500000` @48k | S0 centre 0.1467 is off the grid; exact silence shown in §2.5 |

The rendered output MOVES with the norm at every rate for both candidates (`M1`), on both VCOs.
`M1c`: the two boundary vectors are **audibly different** — `max |Δ rmsA| = 0.088968` over all four
rates and 41 norms, so the boundary choice is a real decision, not cosmetics.

### 2.2 Interval boundaries, by two-sided limits (`m2_bounds.tsv`)

At **every** boundary, for both candidates:

* the boundary **value** equals the closed-form node shape — worst error `0.000e+00` (exact);
* **both one-sided limits exist**: `|f(b±d) − node| / d` is finite, worst `4.800` (equal) / `7.508`
  (panel) against a bound of `100`. These are the finite one-sided slopes, i.e. the sweep corners;
  a step would give `1/d → 1e6`.

A full-sweep continuity scan at a fixed phase over 4000 steps gives worst jump `9.600e-09` (equal) /
`1.502e-08` (panel) — continuous in `norm`, corners allowed, **C1 not claimed**.

### 2.3 Repeated restore of the same norm (`m3_restore.tsv`)

`norm = 0.625`, restored three times.

* **State lane** (three independent `codec → owner → render` loads, three fresh engines): repeat 1 and
  repeat 2 are **BIT-IDENTICAL** to repeat 0, peak and RMS bit-equal.
* **Live lane** (same target re-applied three times inside ONE session, through the existing smoother):
  **not** bit-identical, and that is a property of the lane, not the candidate — the live lane never
  resets phase and the oscillator period is not an integer number of samples (measured
  `218.182` frames at 220 Hz / 48 kHz). Aligning two captures by a zero crossing cannot make them
  bit-equal either, because the alignment itself quantises to the sample grid. The live lane is
  therefore judged against a floor **measured from the same capture** (sliding the analysis window
  inside one repeat rotates the phase across several periods):

| repeat | \|Δ rms\| | own measured phase-sampling floor | \|Δ peak\| |
|---|---|---|---|
| 1 | `1.213e-04` | `4.613e-04` | `5.929e-14` |
| 2 | `3.351e-04` | `4.830e-04` | `3.089e-11` |

Both differences are **inside** the floor the same capture produces by phase rotation alone. The
coordinate readback equals the applied target exactly on every repeat (`M3.readback`).

**Why the earlier criterion was withdrawn, not loosened.** A first version demanded bit-identity or a
hand-picked `1e-3` rms tolerance. Both were withdrawn *with the measurement above*, which shows the
statistic's own phase sensitivity is the right bound; no assertion was relaxed to make a run green.

### 2.4 Block partition (`m4_blocks.tsv`)

Through the real block entry `renderBlock()` (engine loaded at `blockFrames = 4096`), schedules
1 / 64 / 256 / irregular `(7,100,33,250,61)`:

```
schedule                        frames   rmsA          max|diff vs 1-frame|   same
1 (reference)                   4096     0.302567407   0.000e+00              1
64                              4096     0.302567407   0.000e+00              1
256                             4096     0.302567407   0.000e+00              1
irregular(7,100,33,250,61)      4096     0.302567407   0.000e+00              1
```

The mapping holds no cross-block state, so the partition cannot change the output.

### 2.5 A/B independence — split by what the graph actually allows (`m5_ab.tsv`, `m8_isolation.tsv`)

The canonical graph carries the documented normalised route `vco_a.dry_out → vco_b.cv_in`
(`machine_definition.h:349-361`), so **VCO B is audio-rate frequency modulated by VCO A's output**.
Demanding output-level independence in the A→B direction would be wrong on this product.

| claim | result |
|---|---|
| morph **coordinates** never leak, either direction (exact readback) | **PASS** |
| driving B leaves A **bit-identical** (no B→A route exists) | **PASS** (`\|Δ rmsA\| = 0.000e+00`) |
| driving A moves A | **PASS** |
| driving A leaves **B's own sup** untouched | **PASS** (see below) |
| the mapping has no cross-VCO channel, with no graph at all | **PASS** (bit-identical) |

Measured A→B numbers: driving A 0.50 → 0.90 moves B's rms `0.3291 → 0.3751` and its amplitude
histogram by `0.243`, while B's **period moves only `218.182 → 218.194` frames** (`7e-5`). That
combination is the signature of a **zero-mean audio-rate FM**, which is exactly the documented route —
not a pitch shift, and not a change of B's waveform function.

**A withdrawn claim, and why.** `shapeDistance` (amplitude-histogram TV distance) was first used as
the leak detector and it *fired* (`0.2456`). It was withdrawn because the measurement above refutes it
as a test here: deep audio-rate FM makes the phase non-monotone, a non-monotone reparameterisation
changes how long the signal spends at each value, so the histogram moves even when the waveform
function did not. Keeping it would have been a red for the wrong reason.

The replacement is the one quantity **invariant under every reparameterisation of time**: the sup.
`x(t) = W(φ(t))` visits exactly the values of `W` whatever `φ` does, so `sup` cannot be faked by FM.

```
|Δ peakB| when A is driven   3.989e-07   <= own measured estimator floor 4.770e-05   PASS
|Δ peakB| when B is driven   7.426e-04   >  the floor (16x)                          control fires
```

The floor is the spread of sub-block sups inside one capture (the sup estimator's own resolution);
sliding the window does not expose it, because the closest approach to the peak is unchanged by a
shift. The control keeps this from passing vacuously.

**M8 is where the isolation claim is unconfounded** — no graph, no route, no FM. Two independently
constructed `Vco` instances: one has its morph driven across the entire range, the other's emitted
samples must stay bit-identical to an untouched third.

```
normA    0.0000  0.1250  0.2500  0.5000  0.6250  0.7500  0.9000  1.0000
same        1       1       1       1       1       1       1       1
max|ΔA|  1.55    1.00    1.00    0.00    0.605   1.211   0.999   1.00
```

8 morph values × 4096 samples: the untouched Vco never moves by one bit, while the driven Vco's own
output does change at every non-centre norm. A shared/static/ambient cross-VCO channel would fire this.

### 2.6 Reuse and bit-identity through the real chain (`m6_reuse.tsv`)

| item | worst error |
|---|---|
| S0 vs the existing `kMorphSawInvSaw` closed form at `morph = u` | `0.000e+00` (bit-exact) |
| S2 vs the existing `kMorphSineTriangle` closed form at `morph = u` | `0.000e+00` (bit-exact) |
| the ring's **triangle node** (0.75) vs the legacy pure-triangle path, **BLAMP included** | `0.000e+00` (bit-for-bit) |
| off-node (0.375) vs the legacy path | `7.488e-01` (the wiring is live) |

### 2.7 The S0 silence — reported, not hidden (`m7_silence.tsv`)

S0's endpoints are antipodal (`invSaw(p) = −saw(p)`), so at the exact centre of S0 the convex blend is
**identically `0.0` for every phase**:

```
candidate   norm at S0 centre   peak over phase   rms over phase   endpoints peak
equal       0.1250              0.000000000e+00   0.000000000e+00  1.000000 / 1.000000
panel       0.1467              0.000000000e+00   0.000000000e+00  1.000000 / 1.000000
```

**No gain compensation is applied.** A `×1e6` compensation row is included and is still exactly zero:
zero multiplied by any gain is still zero, so the silence provably **cannot** be lifted by any
normalisation — only changing the waveform pair can remove it. The existing `kMorphSawInvSaw` has the
same property today (its comment already records "passes through 0 at mid"). Both candidates carry the
full swing at the sweep endpoints.

### 2.8 Negative controls (`neg_controls.tsv`)

Each control names the **one** check it must make fire, and runs the **same predicate** as that check,
so a green check is evidence rather than the absence of a check.

| id | defect | must fire | fired |
|---|---|---|---|
| N1 | fixed waveform — the REAL chain on the legacy path (`wave_` pinned to `kTriangle`), morph swept | `M1` movement | yes — `rmsA spread 0.000e+00` |
| N2 | wrong side — a source that reads the other side's coordinate | `M5.coord` | yes |
| N3 | interval discontinuity — a non-partition-of-unity source | `M2` continuity | yes — worst jump `1.200000` |
| N4 | silence-free saw↔invSaw source, both candidates | `M7` silence | yes — peak `0.500000` |

**N1 is the S0 defect measured on the product, not modelled**: on the real legacy chain the morph
coordinate moves the rendered output by exactly nothing (`rmsA spread 0.000e+00`), because `wave_` is
pinned to `kTriangle` and `setWaveform` has zero production callers.

---

## 3. Why the existing pure-triangle and S5 assertions change — and what is NOT deleted

**No existing assertion was deleted, weakened, or bypassed.** The candidate is opt-in behind
`Vco::setWaveMap`, whose default is `kLegacy`; `emittedAt_` takes the pre-existing path verbatim when
the map is legacy, so every existing test keeps its exact previous behaviour. The only new number in
the product is the enum member and one branch.

What *would* change if the mapping is switched on, stated separately:

1. **Pure-triangle assertions** (anything asserting that a given `wave_`/`morph` renders the legacy
   triangle): unchanged at `kLegacy`. Once the map is on, only `norm` at the triangle node reproduces
   the legacy triangle bit-for-bit — **including its BLAMP** (§2.6). Everywhere else the output is a
   convex mix of a band-limited triangle with **naive** saw / sine / pulse / invSaw, so any test that
   asserts a *band-limited* or *pure* waveform away from the triangle node must be re-stated as a mix
   assertion. It must not be "fixed" by loosening the old assertion.
2. **S5 hard-sync acceptance**: the sync path is untouched — `tick()` uses `emittedAt_` for both the
   emitted sample **and** the sync jump, so same-frame consumption and sub semantics are preserved.
   `emittedAt_` is the single place the map enters, and both call sites go through it. No S5
   assertion changes; the S5 acceptance probe is not re-run here (it is in the slow suite, deliberately
   not run for an undetermined mapping).

---

## 4. Compatibility table

| surface | today (`kLegacy`, the default) | after the mapping is switched on |
|---|---|---|
| **save bytes** | **unchanged** | **unchanged** — the map is not a `ParameterId`, not in the registry, not in `DeviceStateV1`, not persisted |
| **restore: coordinate readback** | unchanged | unchanged — exact in both directions (§2.5) |
| **restore: sound** | unchanged | **CHANGED** for any state whose `morph` is not the triangle node — those states previously rendered pure triangle regardless of `morph` (the S0 defect) and now render the sweep position |
| **the existing `norm = 0.5` default** | pure triangle (because `wave_` is pinned to `kTriangle`) | `equal`: **sine** (0.5 *is* the sine node) — `panel`: `0.706·sine + 0.294·triangle` |
| **old files' non-default `morph`** | previously inaudible | now audible |
| `pw` / SHAPE | inaudible | becomes audible through the pulse node (§5, D-2) |
| registry / target / wire / disposition | — | **untouched** |

**Save-byte-unchanged and restore-sound-unchanged are different facts.** Save bytes are unchanged
(every old file still loads, every field still means what it meant). Restore *sound* is unchanged only
while the mapping is off; the moment production enables it, an old file with a non-default `morph`
sounds different from what it sounded like when it was saved. That is the honest cost of the S0 fix,
and it is unavoidable: the whole point of S0 is that `morph` was inert.

**Cost of keeping `0.5` = pure triangle.** Under this node order, `0.5` is the sine node (`equal`) or
inside the sine→triangle stretch (`panel`). To make `0.5` emit pure triangle, the triangle node must
sit at `0.5`, i.e. the node order must become `[saw, invSaw, triangle, sine, pulse]` or the triangle
boundary must be moved to `0.5`. Cost: the two manual-named stretches stop being adjacent features in
the panel's order — `sine → triangle` is a named morph, so swapping triangle and sine inverts the
direction of a manual-evidenced morph, and `invSaw → triangle` becomes an unnamed connector where the
named `invSaw → sine` used to be. The panel's glyph order is provisional anyway (P1), so this is a
*possible* future candidate — but it is **not** a reason to distort the boundary vectors now, and no
curve was invented to keep the default bit-identical.

---

## 5. Decision needed from the supervisor

**D-2 — the pulse node reads `duty_` (= the `pw` panel parameter).** Consequence, reported not hidden:
once the sweep reaches the pulse region, the SHAPE/`pw` control becomes audible. This is **not** a
reclassification of `vco_a_pwm` / `vco_b_pwm`: both stay `transfer_unavailable`, both live lanes stay
fail-closed, and **this slice does not judge them consumed**. The registry, the state bytes and the
dispositions are untouched. Whether the product wants `pw` to become audible as a side effect of the
morph knob is a supervisor ruling.

**P1/P2/P3** (node order, boundary vectors, BLAMP scaling) are software-provisional and need the
supervisor's ruling as such (§1.4, §1.1).

---

## 6. Reproduction

The iPlug2 submodule is **not initialised in this worktree**, so CMake cannot configure here
(`CMakeLists.txt:781` `FATAL_ERROR`). `lunar_core` and `lunar_host` are both header-only
(`add_library(... INTERFACE)`), so the probe was compiled and run directly:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -Icore/include -Igenerated -Ihost/include -Itests/host \
    tests/probes/gh19s0_morph_map_probe.cpp -o build-116/gh19s0_morph_map_probe
./build-116/gh19s0_morph_map_probe --out report/gh19s0-morph-map --frames 1024
# -> checks=53 failures=0 ; writes the 10 matrices in report/gh19s0-morph-map/
```

**DISCLOSED:** the CMake target registration added to `CMakeLists.txt` (a `lunar_add_test` +
`set_tests_properties(... LABELS "slow")`, mirroring `gh19_hardsync_acceptance`) is therefore
**unverified locally** — it needs a worktree with the vendor submodule. The probe itself is verified.
The label `slow` keeps it out of every-push runs (`--label-exclude slow`) in line with the existing
probe gating.

---

## 7. Recommendation

**Take `kRingPanel` (candidate B) as the provisional production shape, with the node order still open
and D-2 ruled on separately.**

Reasoning, in the order the card asked for it:

* **Continuity** — both candidates are exactly equal here: boundary values are exact to `0.000e+00`,
  both one-sided limits exist at every boundary, worst jump over 4000 sweep steps is `~1e-8`, and both
  are bit-identical across block partitions and across repeated state restores. Continuity does not
  choose between them.
* **Panel evidence** — only candidate B has any: its boundaries come from six measured glyph centres,
  and its own redundancy check (`t3` predicting `c3 = 0.8467` vs the direct read `0.8200`, residual
  `0.0267 ≈ 4.0°`) is a *small* residual for a linearity that is admittedly unverified. Candidate A
  has no panel provenance at all and is an explicitly provisional software division. When two
  candidates are equally continuous, prefer the one with provenance.
* **Default impact** — this is the real cost of B: `0.5` lands at `0.706·sine + 0.294·triangle`
  instead of A's exact sine node, so the default knob position is a mix rather than a named waveform.
  Both candidates change restore-sound for non-default `morph` (unavoidable, §4) and neither changes
  a single saved byte.

If the supervisor prefers the default position to land exactly on a named waveform, `kRingEqual` is
the better pick and costs only the panel provenance — that is a legitimate call, and it is the one
trade this recommendation is actually making. **One thing I would not do is tune the boundaries to
make `0.5` bit-identical to today's triangle**, because that inverts a manual-evidenced morph
direction (§4).

*Not done and not claimed:* no merge, no push, no PR, no GH#19 closure, no release, no MET claim, no
registry/target/wire/disposition change, and PWM is **not** judged consumed. The unrelated
~90-minute slow suite was deliberately not run for an undetermined mapping.
