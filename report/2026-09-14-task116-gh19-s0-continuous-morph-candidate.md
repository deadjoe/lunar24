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

### 1.3 How five nodes come from six names — and why the panel supports no derivation

**The node count, derived (this is forced by the manual, not invented).** The manual's two morphs each
contribute a far endpoint, and that is the only source of the fifth node:

| manual name | endpoints | new node? |
|---|---|---|
| saw, sine, triangle, pulse ("traditional wave shapes") | — | 4 nodes |
| "saw to inverted saw" | saw (already traditional), **invSaw** | **+1** |
| "sine to triangle" | sine and triangle, **both already traditional** | +0 |

⇒ **4 + 1 = 5 structural nodes.** `invSaw` is the only node that has no separate glyph on the panel,
because it is reached *inside* a named morph stretch rather than at a labelled position.

**What the panel actually says.** The six glyphs are at measured angles −75, −53, −20, +16, +48, +75°
from the knob centre (`pdftoppm -r 2000` of `solar42N_panel_2400px.png`). Their identifications come
from task #115's layer-C reading of the same image, with its confidence column:

| # | angle | #115 reading | confidence |
|---|---|---|---|
| 1 | −75° | **sine** | **high** |
| 2 | −53° | triangle/saw family (which one) | low on which; medium overall |
| 3 | −20° | flat-top + falling slope — **reading not unique** | **low** |
| 4 | +16° | **square / pulse** | **high** |
| 5 | +48° | two crossed diagonals overlaid ⇒ **morph symbol** | medium |
| 6 | +75° | sine merging into a crossing ⇒ **morph symbol** | medium/low |

**This contradicts the node order in §1.1, and the previous revision of this report got it backwards.**
That revision put `saw` at −75° and `triangle` at +48°. −75° is my own **high-confidence sine**, and
+48° is a **morph symbol**. An earlier draft then went further and claimed the panel was *incompatible*
with any monotone mapping. **That step is withdrawn too**, and @Codex was right to stop it: whether a
glyph denotes a stretch's interior, a whole morph *type*, or one representative waveform is **itself
unknown**, so glyph positions alone cannot exclude anything. The defensible statement is the weaker
one: **this boundary derivation has no basis.** Not "the panel disproves it".

**Candidate B — `kRingSpaced` (the name is no longer `kRingPanel`).**

```
c = {0.0, 0.2933, 0.3667, 0.8200, 1.0}
```

Read the numbers off exactly one recomputable calculation, with its source stated — a plain rescaling
of the six measured glyph **angles** onto `[0,1]` (`arc = 150°`, so `t = (θ+75)/150`), then
`c = {t0, 2·t1, t2, t4, t5}`:

```
-75 -> 0.0000   -53 -> 0.1467   -20 -> 0.3667   +16 -> 0.6067   +48 -> 0.8200   +75 -> 1.0000
```

**The previous "independent redundancy evidence" is deleted, because it was circular.** The earlier
text cross-checked `2·t3 − c2` against `c3` and reported `0.8467` vs `0.8200`. But the value it
substituted for `t3` was `0.5933`, which is the *interval midpoint* `(t2+t4)/2 = 0.5933` computed
**from** `c2` and `c3` — so it reproduced `0.8199` against a directly-read `0.8200`, i.e. it checked the
model against itself. The header meanwhile used the *measured* `t3 = 0.6067`, giving `0.8467`. The
report and the header were running two different derivations; @Codex located exactly this. Only
source-consistent, recomputable arithmetic remains.

The rescaling also has **no measured basis**: the knob's mechanical travel was never measured, so
mapping the glyph arc onto `[0,1]` assumes the outermost labelled glyphs sit at the sweep's extremes.
The candidate is retained **solely** as a non-uniform-spacing specimen, so the selection can compare
"norm 0.5 **is** a node" against "norm 0.5 falls inside a stretch". Its values are not evidence about
the hardware and are not offered as such.

### 1.4 Explicitly NOT claimed

- **The node order** `[saw, invSaw, sine, triangle, pulse]` is provisional (P1). The manual fixes each
  morph's **endpoint pair** ("saw to inverted saw", "sine to triangle") — it does **not** fix which
  endpoint sits at the lower norm, i.e. it does not state the knob's rotation direction. So even S0's
  internal direction is a software choice, not a manual fact. The panel does not settle the order
  either (§1.3).
- **The stretch widths** are not evidenced. Equal division asserts all four stretches are equally
  wide; that is a claim with no more support than any other spacing, which is precisely why
  @Codex allowed it *as a labelled provisional candidate* while forbidding it being presented as
  hardware structure.
- Symbols that could not be uniquely read are recorded as unreadable, not upgraded to confirmed.

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

### 2.8 Negative controls — two kinds, and they are not the same evidence

**(i) In-probe controls (`neg_controls.tsv`).** Each names the **one** check it must make fire and
runs the **same predicate** as that check. They prove the *predicates* have discriminating power.
Because the substitute source is written inside the probe, they prove **nothing about the production
code** — a fixture authored alongside the code can only show the code agrees with itself.

| id | defect | must fire | fired |
|---|---|---|---|
| N1 | fixed waveform — the REAL chain on the legacy path (`wave_` pinned to `kTriangle`), morph swept | `M1` movement | yes — `rmsA spread 0.000e+00` |
| N2 | wrong side — an in-probe source that reads the other side's coordinate | `M5.coord` | yes |
| N3 | interval discontinuity — an in-probe non-partition-of-unity source | `M2` continuity | yes — worst jump `1.200000` |
| N4 | silence-free saw↔invSaw source, both candidates | `M7` silence | yes — peak `0.500000` |

N1 is the exception among the four and the strongest of them: it runs the **real** chain, so it is the
S0 defect *measured on the product* rather than modelled — on the legacy path the morph coordinate
moves the rendered output by exactly nothing, because `wave_` is pinned to `kTriangle` and
`setWaveform` has zero production callers.

**(ii) Production mutants — the contract's actual requirement
(`report/gh19s0-morph-map/run_mutation_controls.sh`, log in `mutation_controls.txt`).** Three targeted
mutations of the **real production headers**. Each is built from a patched **copy** of `core/include`
placed first on the include path, so **no tracked file is modified**. Each must run to completion
(printing its `checks=` summary) *and* trip its specific named check — a mutant that merely exits
non-zero does not count, because the failure surface has to be the named one.

| mutant | production patch | result |
|---|---|---|
| fixed waveform | `vco.h`: `if (waveMap_ != VcoWaveMap::kLegacy) {` → `if (false) {` | runs to completion, **`checks=53 failures=14`**, `FAIL [M1.equal.44100]` … `FAIL [M1.spaced.96000]` |
| wrong side | `machine_runtime.h`: `setVcoBMorph` also calls `vcA_.setMorph(m)` (a real cross-VCO leak) | runs to completion, **`checks=53 failures=4`**, `FAIL [M5.coord]`, **`FAIL [M5.B]`**, `FAIL [M5.shape.B]` |
| interval discontinuity | `vco_wave_map.h`: `sampleAt` returns `a`/`b` instead of `(1−u)·a + u·b` | runs to completion, **`checks=53 failures=4`**, **`FAIL [M2cont.equal]`**, `FAIL [M2cont.spaced]`, `FAIL [M7.*]` |

Reference build in the same runner: `checks=53 failures=0`. The mutant builds deliberately omit
`-Werror` (a mutation can make a variable unused for a reason unrelated to the defect under test); the
reference build carries the full `-Wall -Wextra -Wpedantic -Werror` set.

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
| **the existing `norm = 0.5` default** | pure triangle (because `wave_` is pinned to `kTriangle`) | `equal`: **sine** (0.5 *is* the sine node) — `spaced`: `0.706·sine + 0.294·triangle` |
| **old files' non-default `morph`** | previously inaudible | now audible |
| `pw` / SHAPE | inaudible | becomes audible through the pulse node (§5, D-2) |
| registry / target / wire / disposition | — | **untouched** |

**Save-byte-unchanged and restore-sound-unchanged are different facts.** Save bytes are unchanged
(every old file still loads, every field still means what it meant). Restore *sound* is unchanged only
while the mapping is off; the moment production enables it, an old file with a non-default `morph`
sounds different from what it sounded like when it was saved. That is the honest cost of the S0 fix,
and it is unavoidable: the whole point of S0 is that `morph` was inert.

**Cost of keeping `0.5` = pure triangle — the real cost, corrected.** An earlier revision claimed this
required swapping the node order and therefore "inverting a manual-evidenced morph". **That was
wrong**, and @Codex was right to reject it: I had written the constraint of my chosen equal grid as if
it were a mathematical necessity. **The same node order can simply move `c3` to `0.5`** and adjust the
earlier boundaries — no order change, no inversion. Concretely, keeping
`[saw, invSaw, sine, triangle, pulse]` and setting `c3 = 0.5` means: S0 and S1 must share `[0, 0.5]`
instead of `[0, 0.75]`, and S3 (triangle→pulse) stretches over the whole `[0.5, 1]` — so the sweep is
**badly unbalanced** (the last stretch alone is half the travel) and every boundary is **decoupled from
the measured glyph angles** for the whole second half. The honest cost is therefore **spacing and
correspondence**, not order: you lose the ability to line any boundary up with any panel measurement,
and you get one stretch twice as wide as the others. Whether that is worth it is a legitimate thing for
the supervisor to decide — it is not forbidden, it is just not free.

What is *not* on the table is inventing an unreasonable curve to keep the default bit-identical, and I
have not done that.

---

## 5. Decisions — D-2 is ruled; the rest are still open

**D-2 (pulse node reads `duty_` = the `pw` panel parameter) — RULED IN SCOPE, no longer open.**
@Codex, 2026-09-13: this is the normal consumption of an *existing* parameter and needs no separate
owner confirmation. So the sweep making `pw` audible past the pulse node is **approved**. It remains
**not** a reclassification of `vco_a_pwm` / `vco_b_pwm`: both stay `transfer_unavailable`, both live
lanes stay fail-closed, and this slice does not judge them consumed. Registry, state dispositions and
state bytes are untouched.

**Still open, and the supervisor's to rule:**

- **P1 — the node order.** Which end of each named morph sits at the lower norm, i.e. the knob's
  rotation direction. The manual fixes the endpoint *pairs* only; the panel does not settle it
  (§1.3, §1.4).
- **P2 — the boundary vector.** Neither candidate has evidence. This is the choice §7 is about.
- **P3 — the BLAMP scaling.** First-order, exact only at a pure-triangle node.

**NOT a production entry point (standing disclosure).** The candidate is switched on only from tests
(`SynthRuntime::setVcoWaveMap`). It is not a `ParameterId`, not in the registry, and not persisted, so
**no SAVE can select a candidate**. This test-injection step is acceptable for this experimental card
only: the production slice that adopts a mapping **must remove it** and give the mapping a real,
persisted, ruled-on entry point.

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

The §2.8 **production** negative controls have their own runner, because they must mutate the real
headers rather than the probe:

```sh
sh report/gh19s0-morph-map/run_mutation_controls.sh
# -> reference 53/0 ; three mutants each run to completion and are caught BY NAME
# -> "MUTATION CONTROLS: all 3 production mutants ran to completion and were caught by name." ; exit 0
# saved transcript: report/gh19s0-morph-map/mutation_controls.txt
```

Note the `.txt` extension: the repo's `.gitignore:31` ignores `*.log`, so the transcript is saved with
an extension that keeps it reviewable in the commit rather than force-adding past the ignore rule.

It never touches a tracked file: each mutant is built from a `cp -R core/include` copy placed
**first** on the include path, with the patch verified by `grep -q 'MUTANT n'` before the build. Note
the differing warning policy — the **reference** build carries `-Wall -Wextra -Wpedantic -Werror`
(matching the real builds), while the **mutant** builds deliberately omit `-Werror`: a mutation such
as `if (false) {` can leave a variable unused, which `-Werror` would reject for a reason that has
nothing to do with the mutation under test.

**DISCLOSED:** the CMake target registration added to `CMakeLists.txt` (a `lunar_add_test` +
`set_tests_properties(... LABELS "slow")`, mirroring `gh19_hardsync_acceptance`) is therefore
**unverified locally** — it needs a worktree with the vendor submodule. The probe itself is verified.
The label `slow` keeps it out of every-push runs (`--label-exclude slow`) in line with the existing
probe gating.

---

## 7. Recommendation

**Take `kRingEqual` (candidate A) as the provisional production shape.**

The previous revision recommended `kRingSpaced` on the strength of its panel provenance. **That
recommendation is withdrawn** — the provenance is gone (§1.3), so the recommendation has to stand on
what is actually left. Against the card's three criteria:

* **Continuity** — a tie, and not a close call: boundary values exact to `0.000e+00`, both one-sided
  limits finite at every boundary, worst jump over 4000 sweep steps `~1e-8`, bit-identical across
  block partitions and across repeated state restores, for both candidates. Continuity does not choose
  between them.
* **Panel evidence** — **neither has any.** The glyph identifications contradict the node order for
  both, and no boundary derivation survives. So this criterion no longer separates them; what it does
  do is *penalise* `kRingSpaced`, whose numbers came from the withdrawn derivation, relative to
  `kRingEqual`, which claims nothing beyond "an even provisional division" — a claim @Codex explicitly
  permitted and which does not depend on any reading of the hardware.
* **Default impact** — `kRingEqual` puts `norm 0.5` **exactly on a named waveform** (the sine node).
  `kRingSpaced` puts it inside a stretch (`0.706·sine + 0.294·triangle`), which is harder to explain,
  harder to rule on, and rests on numbers with no basis. Both change restore-sound for non-default
  `morph` (unavoidable, §4); neither changes a single saved byte.

**The trade, stated plainly:** `kRingEqual` is simpler, its default position lands on a name, and it is
the only one of the two that does not inherit a withdrawn derivation — but it does assert that all four
stretches are equally wide, which is unevidenced. `kRingSpaced` avoids the equal-width assertion, but
its non-uniform spacing has no evidence either, so it trades one unevidenced claim for another while
losing the clean default. **On a tie in evidence, prefer the candidate that claims less and lands on a
name.** If the supervisor wants non-uniform spacing for a reason of their own, that is a legitimate
override — but it should be *their* reason, not mine, because mine was the panel and the panel does not
support it.

Two things I would not do, both of which this slice avoided: **tune the boundaries so `0.5` stays
bit-identical to today's triangle** (§4 shows the real cost of that), and **present any of these
numbers as hardware structure**.

*Not done and not claimed:* no merge, no push, no PR, no GH#19 closure, no release, no MET claim, no
registry/target/wire/disposition change, and PWM is **not** judged consumed. The unrelated
~90-minute slow suite was deliberately not run for an undetermined mapping.
