# P2-⑤ Findings and Repayment Debts

Slice: P2-⑤ — snapshot publication path (Half 1) and state persistence
(Half 2). Author: @Pi (implementer). Adjudicator: @Claude (engineering
director). Team lead: @bearbone.

This file records two things deliberately kept separate:

- **The honest boundary of this slice** — what P2-⑤ could and could not prove.
- **Five pre-existing debts** — work owed to a later slice, each with an owner,
  a repayment point, and a status. An *owed debt* is a real task that must be
  repaid; a *declared boundary* is a known-unverified limit, not a defect.

---

## Honest boundary (declared, not a defect)

`deviation` — the P2-⑤ persistence claim is **order and interruption visibility
only**. The temp-file → flush → atomic-rename ORDER is proven by
`tests/core/test_state_persistence.cpp` (a mid-rename fault leaves the live file
untouched and loadable; an in-place write corrupts it). What is **NOT proven**
is true power-off durability: `fsync` durability cannot be established in this
environment (the file-system include ban keeps `unistd.h` out of core, so `fsync`
is not core-expressible), and a power cut between rename and metadata commit is
not modelled. The honest claim is therefore:

> **顺序已验证，掉电持久性未验证。**

This file and `state_persistence.h` never assert "crash-safe".

---

## Repayment debts

| # | Debt | Type | Owner | Repayment point | Status |
|---|------|------|-------|-----------------|--------|
| 1 | Host window height clamps so the 2× test render does not fit-to-window (P1 ④) | Owed debt | @Pi | P5 exit — real window sizing / full UI | Open |
| 2 | Real CoreAudio output-stream per-channel (interleaved) verification | Owed debt | @Pi | P3 exit — a real host callback feeding events into core merges into this | Open |
| 3 | `real_path` not routed through the executor in P2-③ (mixed-partition judge on the audio path) | Owed debt | @Pi | P3 exit — real modules exist; judge = ① (mixed partition) | **Landed** — executor consume-rule + judge/negatives in core & tests; final adjudication @Claude at P3 exit |
| 4 | Power-loss persistence not verified (only order + interruption visibility proven) | **Declared boundary** (NOT a task, not a defect) | Team | — | Knocked-down unverified, recorded in the honest boundary above |
| 5 | Windows real-machine audio/MIDI verification | Owed debt | @Pi | First Windows distributable build | Open |

Notes on classification:

- **#4 is a boundary, not a debt.** It is not expected to become a task with a
  "fixed" status; it is a limit that must be re-verified when the persistence
  layer gains real `fsync`. It is listed here so it is never silently promoted
  to "we proved crash-safe".
- **#1, #2, #3, #5 are owed debts.** They are real and must be repaid at the
  stated exit; they are not "declared out of scope and forgotten".
- **Repayment points are slice-gates, not timestamps** — the debt is closed when
  the gate at that point re-checks it and passes.

---

*This file is a documentation artifact of P2-⑤, not a runtime input to the core
library. It lives at the repo root alongside the source that it debts-forward to
the next phase.*

---

# P3-② Measurement Record (aliasing + noise band)

Slice: P3-② — new drone voice block (Schmitt relaxation oscillator, FM/AM voice,
noise source, sample-and-hold). Author: @Pi (implementer). Mandate + adjudication:
@Claude. These are **measurements only**: @Claude ("抗混叠本片不修但要测记 FINDINGS")
and ("可听带内噪声功率…本片只测量,不要求现在修"). The *fix / bandwidth-normalization
decision* is deferred to P3 exit to be made on this evidence, not patched silently now.

**Provenance rule (生效于本记录, @Claude P3-② 补充)**: 每个混叠数字都必须标注参数
出处。无手册/规格出处者一律显式标 "未取证合成极值"; 真实范围未取的记为
"待取证项", 须在用它做设计决策之前先解决。别让一个没出处的数字变成驱动设计的事实。

**Test**: `tests/core/test_drone_mod.cpp` (CTest #21; judges shared with P3-① in
`drone_test_common.h`). Measured by Goertzel single-frequency DFT (`goertzel_mag`)
and by band-fraction on the measured total variance (`noise_sample_var`).

## 1. Schmitt aliasing (folded 3rd harmonic)

| sr | fundamental f0 | first folded harmonic | folded alias freq | level rel. fundamental |
|----|----------------|-----------------------|-------------------|------------------------|
| 8000 Hz | 1858.78 Hz | 3rd (= 5576.3 Hz > Nyquist 4000) | 2423.65 Hz | **−16.24 dB** |

**参数出处** — f0=1858.78 Hz 是**合成测试频率**, 非硬性规格频率: 由
`pick_schmitt_alias`(sr=8000, 扫 seed 1..32)选出"干净中带折叠"里最大的 f0, 专供量
折叠波形功率。Schmitt 真实设计域=20..2000 Hz(seed 推定), 此 f0 落在该域上端, 其
3 次谐波才因此折叠——这是**极端处的行为探针**, 不是"这台琴会发出的声音"。三角波
奇次谐波按 1/n² 衰减, 3rd 应为基波 −19.05 dB, 实测 −16.24 dB(离散振荡器不完全对称)。
**Reading for P3 exit**: the folded-harmonic level is small but not negligible when
the pitch is a large fraction of Nyquist; for typical audio sr (44.1..96 kHz) the
drone band (20..2000 Hz) is far below Nyquist, so harmonic folding there is a
non-issue — it only becomes audible in the contrived low-sr case shown.

## 2. FM aliasing (high deviation)

| sr | carrier fc | fDev | peak inst. freq | probe mirror | energy at mirror, hi vs lo fDev |
|----|-----------|------|-----------------|--------------|---------------------------------|
| 48000 Hz | 821.21 Hz | 30000 Hz | 30821.21 Hz (> Nyquist 24000) | 17178.79 Hz | **+37.87 dB** |

The mirror is where the peak overshoot reflects (`fold_to_baseband(fc+fDev)`).
Rendering the same seed with a tiny fDev (no overshoot) gives almost no energy at
that probe; the +37.87 dB delta at high fDev is the aliasing-added energy. (AM
depth set to 0 to isolate FM.)

**参数出处** — ⚠️ **fDev = 30 kHz 是未取证的合成极值 (UNEVIDENCED SYNTHETIC
EXTREME)**, 不是手册支持的数值范围。手册只说 drone 侧 FM 是开关("FM switched on
in the down position", 深度由电路定死、数值未知); VCO 侧写 "linear FM input with
attenuator", 同样**没有数值范围**。fc=821.21 Hz 由 seed=0xF4 推导; depth=0 用于隔离
AM。故 +37.87 dB 是一个**人为构造的最坏情况**(HARD-CASE bound), 不是"这台琴会
发出的声音"。⚠️ **FM 深度真实范围 = 待取证项**: P3 出口做抗混叠决策**之前必须先
解决**——要么取到真实范围(避免为不存在的 case 过度设计), 要么按硬界极值继续并保持
此出处标注(避免低估)。这是 polarity 那条规矩的推广: **别让一个没出处的数字变成
驱动设计的事实。**

**Reading for P3 exit**: FM aliasing is a real, large
effect when `fDev` is a substantial fraction of the sample rate — the instantaneous
frequency genuinely crosses Nyquist and folds back. This is the case the P3-exit
antialiasing decision must handle.

## 3. Audible-band noise power vs sample rate

White noise (amplitude 0.5), per-sample uniform. Total variance = **0.083273**
(sr-invariant — the generator is amplitude-scaled, not bandwidth-scaled). The
one-sided spectrum is flat up to Nyquist, so the fraction inside 20 Hz..20 kHz is
`19980 / (sr/2)`:

| sr | Nyquist | audible fraction | audible-band power |
|----|---------|------------------|--------------------|
| 44100 Hz | 22050 | 0.906 | 0.075456 |
| 48000 Hz | 24000 | 0.833 | 0.069331 |
| 88200 Hz | 44100 | 0.453 | 0.037738 |
| 96000 Hz | 48000 | 0.416 | 0.034662 |

**参数出处** — amplitude=0.5(标准化增益, seed=0x21C), 用于本测量; 带内功率随幅度
平方缩放, 比值(3.38 dB)与幅度无关。

**Deviation = 3.38 dB** across 44.1 → 96 kHz (falls as sr rises). The deviation is
> 1 dB, so it is genuinely measurable, and it is the expected physical effect: a
fixed per-sample amplitude spreads the same total power over a wider band at higher
sr, so a fixed audible band captures less of it. **Reading for P3 exit**: the honest
choice is between band-limiting/oversampling the noise (constant audible power) or
normalizing by bandwidth (constant PSD); the numbers here are the evidence for that
decision.

---

*Honest side-note: the Schmitt relaxation oscillator's discrete frequency is
`sr / (2·ceil(sr/(2·vT·freqBase·(1+tolerance))))` — it quantizes to integer ramp
samples and is never more than one sub-sample per half-swing below the continuous
`chargeRate/(4·vT)`. This is inherent to a relaxation oscillator, not the
fixed-increment trap; it stays sr-invariant to within that bound, whereas a fixed
per-sample step scales with sr. Documented in `schmitt_osc.h` and bounded at 5% by
the cross-sample-rate must-test.*

---

# P3-③ VCO Measurement Record (V/OCT, sub, sync, morph, aliasing)

Slice: P3-③ — the triangle-core AS3340 VCO (VCO A / VCO B: one shared DSP core,
VCO A has a SYNC input, VCO B does not; A's output is normalised into B's CV input
via P2-②'s NormalizedRoute, *re-presented not re-coded* here). Author: @Pi
(implementer). Mandate + adjudication: @Claude. These are **measurements only**:
hard-sync splatter and narrow-pulse folding are recorded here and deferred to P3
exit by evidence, not patched silently. The V/OCT / sub / sync / morph must-tests
are behaviour assertions with red-negatives; their PASS/FAIL runs in
`test_vco.cpp`.

**Provenance rule (already declared above)** applies to every number here, and
additionally: the VCO's waveform set and control ranges carry their own evidence
labelling (below), because the registry is the frozen implementation basis and
several of its entries are *not* manual-confirmed.

## 1. Hard-sync splatter (VCO A's sync input, A-mode)

| sr | f_slave | f_master (sync clock) | probe | energy added by sync |
|----|---------|-----------------------|-------|----------------------|
| 48000 Hz | 500 Hz | 600 Hz | 600 Hz | **+69.09 dB** (synced vs clean) |

Sync = the slave saw restarts every `M = round(48000/600) = 80` samples (a rising
master edge). The probe (600 Hz) lies on the master's harmonic comb but is NOT a
harmonic of the clean 500 Hz saw, so the clean render has negligible energy there
and the synced render carries a strong line: **69.09 dB** above the clean floor.

**参数出处** — f_slave=500, f_master=600 are **synthetic test frequencies**, not
hard specs (the manual gives no sync-ratio figure). The probe=600 Hz is a probe on
the master comb, deliberately chosen to sit where the clean saw has no harmonic, so
the delta is a clean measure of the splatter the hard sync ADDS. Reading for P3
exit: hard sync genuinely injects broadband energy (it restarts the phase, so the
slave's output is no longer a pure saw); this is the aliasing-adjacent effect the
antialiasing decision must consider, and 69 dB is a **HARD-CASE bound**, not
audience-level.   | label: 合成测试点, 非硬规格.

## 2. Narrow-pulse folding (SHAPE = pulse width, extreme duty)

| sr | f0 (pulse) | duty | fold order | folded alias f | level rel. fundamental |
|----|-----------|------|------------|----------------|-----------------------|
| 48000 Hz | 1800 Hz | 0.10 | 15th | 21000 Hz | **−14.32 dB** |

A pulse's harmonics extend to ~1/duty; at duty 0.10 the 15th harmonic (27000 Hz)
exceeds Nyquist (24000 Hz) and folds to 21000 Hz.

**参数出处** — f0=1800 Hz, duty=0.10 are **synthetic test points** (the manual gives
no pulse-width / duty value; SHAPE is "put pulse width with a CV input and a
restorer"). The folded level −14.32 dB is measured by Goertzel at the alias vs the
fundamental. Reading for P3 exit: narrow pulses (extreme duty) carry high
harmonics that fold when the pitch is a large fraction of Nyquist; at typical audio
sr (44.1..96 kHz) and a VCO band ≪ Nyquist this is a non-issue, and it only becomes
audible in the contrived low-sr case.   | label: 合成测试点, 非硬规格.

## 3. Evidence labelling carried into `vco.h` (implementer's open items)

These are **not silently resolved** — they are recorded here and remain open
(@Claude: "冲突要留着可见, 不要被实现悄悄消化掉").

| Item | Evidence strength | Record |
|------|-------------------|--------|
| V/OCT (v_oct_in 0..8 V) | **Confirmed** (direction/type/transfer all confirmed) | `f = fBase·2^vOct`; 1 V = 1 octave, full 8-octave range. |
| oct_sel 3 positions "low"/"0"/"+3" | **Provisional** (names evidenced; counts not) | "low" count = −1 octave is a **standing guess**, not fact. |
| tune range −1..+1 | **Unverified + IN CONFLICT** | Registry reads −1..+1 (two octaves); manual literal "tune knob … over ONE octave". Implemented per registry (oct, −1..+1). **Must-resolve** before it drives design. |
| sub_sel 2 positions "0"/"-1" | Provisional | Default index 1 = "-1" (one octave down). |
| CV input lin/exp | Provisional | Generic CV input transfer UNKNOWN (−5..+5); `2^cv` (exp) vs `(1+cv)` (lin) are **provisional modeling choices**. |
| SHAPE (pw) 0..1 | Unverified | Duty for the pulse waveform. |
| Traditional 4 waveforms (saw/tri/sine/pulse) | **Inference** (AS3340 convention) | Manual says "6 waveforms, two morphing" and never enumerates the four. Recording as inference, not manual evidence. |
| Two morphing (saw↔inv-saw, sine↔tri) | Manual-evidenced | Per the manual's own parenthetical. |
| Sub is phase-locked | Design | Sub derived from the same unwrapped pitch accumulator (`frac(cumPitch/2)`) — exactly −1 octave, **phase-locked**. The OPPOSITE of DroneBank's independent voices. |

*This file is a documentation artifact of P3-③, not a runtime input to the core
library.*

---

# P3-④ Measurement Record (preamp aliasing + evidence labels)

Slice: P3-④ — the PREAMP (`core/include/lunar24/core/preamp.h`) and the ENVELOPE
FOLLOWER (`core/include/lunar24/core/envelope_follower.h`). Author: @Pi
(implementer). Mandate + adjudication: @Claude. These are **measurements only**:
preamp soft-saturation aliasing is recorded here and deferred to P3 exit by
evidence, not patched silently. The other four must-tests (A/R in seconds, EXT
unconnected → no noise/DC/NaN, silence-converges, cross-sr/cross-buffer) are
behaviour assertions with red-negatives; their PASS/FAIL runs in
`tests/core/test_preamp_envelope.cpp` (CTest #25).

**Provenance rule (declared above)** applies to every number here. The manual
(L518-558) evidences the preamp stage and the envelope follower, sets the CONFIRMED
output ranges (env CV 0…10 V, GATE 0…8 V), and shows a clipping indicator, but gives
NO saturation curve, NO gain taper, NO A/R time values, and NO gate-detector trigger
level. Every non-manual value below is marked provisional.

**Test**: `tests/core/test_preamp_envelope.cpp` (judges shared with P3-①/②/③ in
`drone_test_common.h`). Aliasing measured by Goertzel (`goertzel_mag`).

## 1. Preamp soft-saturation aliasing

| sr | f0 (sine) | gain norm | fold order | folded alias f | level rel. fundamental |
|----|-----------|-----------|------------|----------------|-----------------------|
| 48000 Hz | 10000 Hz | 1.0 (x100, +40 dB) | 3rd | 18000 Hz | **−10.01 dB** |

**参数出处** — f0=10 kHz, amp=1.0, gain norm=1.0 (x100) are **synthetic test
points**: the manual gives no measurement frequency or gain-taper (gain is a
normalized 0..1 knob; 40 dB is a confirmed ceiling, the in-between curve and the
amp rail `kSaturationVoltage=10 V` are provisional modeling choices). The tanh
soft-knee is the clipping-indicator nonlinearity; at x100 a 1 V sine drives the rail
hard, so the 3rd harmonic (30000 Hz) exceeds Nyquist (24000 Hz) and folds to
18000 Hz. **−10.01 dB** = folded-3rd amplitude relative to the fundamental, picked
up exactly on a bin where the un-distorted fundamental has no energy. Reading for
P3 exit: the preamp's OWN nonlinearity folds measurable harmonics near Nyquist at
high gain — a within-module antialiasing case the P3-exit antialiasing decision must
weigh alongside the drone-mod and VCO ones.   | label: 合成测试点, 非硬规格.

## 2. Evidence labelled into `preamp.h` / `envelope_follower.h` (open items)

These are **provisional modeling choices**, recorded so the implementation is not
mistaken for manual evidence (@Claude: "冲突要留着可见, 不要被实现悄悄消化掉").

| Item | Evidence strength | Record |
|------|-------------------|--------|
| preamp.gain taper (norm 0..1) | Provisional | Linear-in-amplitude knob: 0=mute (manual L527 "set GAIN to minimum"), 1=+40 dB (manual L544 ceiling). In-between curve is a modeling choice, not manual. |
| preamp soft-saturation rail | Provisional | tanh soft-knee to ±10 V (`kSaturationVoltage`). A clipping indicator exists (L538-539) but no curve/rail is in the manual. |
| env_follower attack/release norm → seconds | Provisional (control/UI) | Core works in SECONDS (design/07 §5); the registry's normalized knob is a UI mapping resolved outside this header. |
| gate-detector threshold + hysteresis | Provisional | Manual gives "gate detector activity" (L557-558) and the CONFIRMED 0..+8 V range, but no trigger level. `kGateThreshold=0.5`, `kGateHysteresis=0.05` are modeling choices. |

*This file is a documentation artifact of P3-④, not a runtime input to the core
library.*

---

# P3-⑤ Measurement Record (resonance curve, distortion aliasing, evidence labels)

Slice: P3-⑤ — the WET/DRY chain: the ten-channel panoramic mixer
(`core/include/lunar24/core/voice_mixer.h`), the dual 12 dB Polivoks VCF
(`core/include/lunar24/core/polivoks_vcf.h`), the post-filter DISTORTION
(`core/include/lunar24/core/distortion.h`), and their composition
(`core/include/lunar24/core/wet_dry.h`). Author: @Pi (implementer). Mandate +
adjudication: @Claude (msg edf0c9e0 / GO 187d8313). The five behaviour must-tests
(① res-doesn't-lose-lows, ② DIST≠GAIN, ③ L/R state independent, ⑤ LP/BP two-state,
⑥ four logic outputs) and the two measurement must-tests (④ route re-presented,
⑦ cross-sr/cross-buffer + aliasing) run in `tests/core/test_mix_filter_distortion.cpp`
(CTest #26, 36 checks — 35 original + the ① peak-normalised negative added on
@Claude's review). This record captures the **measurements** the mandate asked me
to make and the provisional choices the mandate asked me to label — none are patched
silently.

**Provenance rule (declared above)** applies to every number here. Manual lines
L1099-1153 evidence the mixer, the Polivoks VCF and the distortion stage. The
CONFIRMED editorial core is: 10-channel PAN/VOL mixer (L1099-1113); the Polivoks
definitional "you will not lose low frequencies when increasing resonance" (L1120-1122);
CV L normalled to CV R (L1142-1143); DIST/GAIN on the same panel as independent
control (L1151-1153). The manual gives NO pan law, NO gain taper, NO saturation
curve/rail, NO CV V/oct, and NO cutoff Hz range. Every non-manual value below is
marked provisional.

## 1. Resonance does not lose lows — the mandate's premise, measured

@Claude's ① framing ("标准 SVF/梯波高共振必然丢低频") is a **premise to verify, not a
fact** — and my refutation (it is FALSE for the stable linear topologies) was later
**confirmed by @Claude's own re-derivation** (2-pole LP DC gain = 1 independent of Q,
so his originally-specified negative could never fire). I built three candidate LP
models and measured |H(100 Hz)| — a deep-bass probe, 1 decade below the 1 kHz
cutoff — as resonance rose from 0 to max:

| Topology | |H(100 Hz)| res=0 | res=1 | ratio | verdict |
|----------|--------|------------|-------|-------|---------|
| RBJ biquad LP (bilinear) | — | — | ~1.0 | (**flat**) | passes bass |
| 2-pole ladder (Moog-ish) | — | — | ~1.89 | (**boosts**) | passes bass |
| **Chamberlin SVF (chosen)** | 0.9888 | 1.0100 | **1.0214** | (**held**) | passes bass |
| Buggy "LP→BP tilt" (negative) | — | — | **0.1022** | (**loses**) | **RED** |
| **Peak-normalised 2-pole** (`1/Q`, @Claude review) | 1.4141 | 0.2020 | **0.1428** | (**loses**) | **RED** |

The measured curve: **all three stable linear topologies preserve or BOOST the low
band** — none loses it. The stable linear topologies cannot lose low end at high
resonance; the reliable losses are real, representable errors, not the imposed
premise. Two negatives now stand, each a genuine degradation an implementer could
actually produce (per @Claude's review rule: a negative must represent a real error
someone might write, not a construct built to trip the flag):

- **mode tilt** (resonance routing the LP output toward bandpass, which rejects the
  bass) — ratio **0.1022**.
- **peak-normalised 2-pole** (dividing the whole 2-pole output by Q to keep the
  resonant peak from clipping — the common real implementation) — ratio **0.1428**.
  Sampled at high Q: base **Q=0.707 (=1.4141)**, max **Q=5 (=0.2020)**. Not near
  Q=2, where the bass |H(100 Hz)| ≈ 0.5044 just grazes an absolute threshold
  (@Claude's explicit warning).

Threshold **0.5** separates all healthy cases from both red cases (1.02 vs 0.10 /
0.14 in ratio terms), and was set from the measured curve, not before it (per the
mandate "阈值定多少你先量真实曲线再定，别先定阈值再凑").

**Reading for the gate**: the definitional "not lose lows" is satisfied by the
Chamberlin SVF's unity-DC-lowpass (res feedback `damp` does not enter the DC term).
The ① threshold is **0.5** (green ≥0.5, red <0.5). Note the resonance boost appears
at the cutoff (|H(1 kHz)| grows 0.5→~10 as res→max), which is the manual's "boost
the frequency near cutoff point" (L1140) — the low band is held while the cutoff
peak rises, exactly the Polivoks editorial.

## 2. Post-filter distortion aliasing (⑦, measure → record, do NOT fix)

| sr | f0 (sine) | dist | gain | fold order | folded alias f | level rel. fundamental |
|----|-----------|------|------|------------|----------------|-----------------------|
| 48000 Hz | 10000 Hz | 1.0 | 1.0 | 3rd | 18000 Hz | **−12.52 dB** |

**参数出处** — f0=10 kHz, amp=1.0, dist=1 (fully wet), gain=1 are **synthetic test
points**: the manual gives no measurement frequency and no saturation curve/rail
(the tanh rail `kSaturationVoltage=2 V` and the signal-driven `kDriveFold=8` are
provisional modeling choices). The strongly-driven tanh produces odd harmonics; the
3rd (30 kHz) exceeds Nyquist (24 kHz) and folds to 18 kHz. **−12.52 dB** = folded-3rd
amplitude relative to the fundamental (a rational probe, integer bin). Reading for
P3 exit: the post-filter distortion's OWN nonlinearity folds measurable harmonics
near Nyquist at dist/gain=1 — a within-module antialiasing case the P3-exit
antialiasing decision must weigh. **Not fixed here**, labeled provenance per the
mandate ("distortion aliasing measured + recorded in FINDINGS (not fixed, label
provenance)"). (Direct comparison: the P3-④ preamp nonlinearity measured −10.01 dB
at the same f0; the post-filter distortion −12.52 dB.)

## 3. Declared coverage boundary — the 6 cathedral/magic `.1` leaves

Six program leaves — cathedral.1 x/y/z, magic.1 x/z (y is fixed, not movable) — are
recorded in `lunar24.json`/`registry.hpp`/`mustComplete`/`base program_params`
(L480-510) but lack an **independent transcribe cross-check** in `landedDescriptorFacts`.
This is a **declared coverage boundary** (a documented gap in the cross-check
evidence for exactly six source-system leaves), NOT a debt and NOT an un-annotated
omission: `landedDescriptorFacts` is a declaration set, not a full enumeration, and
the `mustComplete == landed` gate (L1691) closes it. Recorded here so the boundary is
explicit rather than silently absorbed (parallel to @Claude's ".1-leaf" instruction,
msg 9baf5612 as noted during P3-③).

## 4. Evidence labelled into the P3-⑤ headers (open items)

| Item | Evidence strength | Record |
|------|-------------------|--------|
| BP-LP default | **Resolved — registry literal confirmed** | Frozen registry literal `default: 0.0` → `positions[0]="bp"` (filter defaults to BP). @Claude initially read the `ParameterDescriptor` field order as `min,max,initial,step` and believed "default 1"; on re-reading the actual order (`min,max,step,initial`) **they retracted** and confirmed `initial=0` is correct. The manual gives no default pose; the frozen-registry literal is authoritative and is followed (`setMode` maps bp→0/lp→1). Difference surfaced, value corrected at source — not silently absorbed. |
| PAN taper (mixer.ch1_..ch10_pan) | Provisional | Equal-power two-way pan (`theta=0.5πp; cos/sin`), an industry convention keeping power constant across the throw. No pan law / dB in the manual (L1105-1110) — all six mixer fieldEvidence are unverified. |
| VOL taper (mixer.ch1_..ch10_vol) | Provisional | Plain linear amplitude gain (vol in [0,1]). No manual dB taper; the host UI maps the taper. |
| WET/DRY headroom | Declared-boundary | design/07 voltage table: DRY V4/V5 max 1 V, WET max 2 V — a 2:1 ratio. The peak/RMS/pk-pk basis is unknown, so **no −6.02 dB constant is invented**; `kWetToDryRatio=2.0` is recorded as a boundary, not a dB figure. |
| VCF cutoff norm→Hz | Provisional | log map 20 Hz..20 kHz, capped at sr/8 for SVF stability. No Hz range in the manual. |
| VCF CV V/oct | Provisional | `kCvVoltsPerOctave=1.0`; cutoff shifted by 2^(mod·cv/oct). No CV range / V-oct figure in the manual. |
| VCF resonance damp curve (res→damp) | Provisional | `damp = 2.0 → 0.1` across res 0→1 (max res kept >0, no self-oscillation). No manual figure. |
| DIST nonlinear rail | Provisional | tanh soft-clip, `kSaturationVoltage=2 V` (aligned to WET nominal max). No manual curve/rail. |
| GAIN drive shaping | Provisional | Signal-driven per-channel drive (`drive → gain·\|x\|` smoothed, `kDriveFold=8`, `kSmoothSeconds=0.005`) so a hot channel saturates only its own non-linearity. No manual values. |
| LINK overrides plugged CV R | **Provisional, UN-EVIDENCED, 待取证** | The interaction "LINK on + CV R plugged" has NO manual ruling. Provisional default: LINK (active switch) overrides the resolved CV R. Added to the P3-exit 待取证 list per @Claude GO (a). |

## 5. Reviewer corrections (@Claude review of the 262d0cd head)

@Claude adjudicated P3-⑤ (CTest 26/26). Two of their own points were re-derived and
**corrected** on their side (both in the implementer's favour); one required a
**test change**:

- **bp_lp default**: @Claude misread the `ParameterDescriptor` field order
  (`min,max,step,initial`) as `min,max,initial,step` and held "default 1". On
  re-reading they **retracted** and confirmed the frozen-registry literal
  `initial=0` → `positions[0]="bp"` is correct. The implementer's choice to surface
  the difference rather than silently follow @Claude's phrase is the correct
  behaviour both ways: a wrong word from the director must not be laundered into the
  code as "director-approved".
- **① premise was false**: @Claude re-derived the 2-pole LP — DC gain = 1 independent
  of Q — so their originally-specified "standard SVF" negative could never fire. The
  implementer's measurement + report shut off an empty judgement.
- **New negative required**: @Claude held that the ① tilt negative is "人造" (built to
  trip the flag) and required a **peak-normalised 2-pole** (a) as a real-error
  negative, sampled at **high Q** (not near Q=2, where bass ≈ 0.5044 grazes an
  absolute threshold). Added to test #1: base Q=0.707 → 1.4141, max Q=5 → 0.2020,
  ratio **0.1428 < 0.5** → RED. Faithful to @Claude's rule: a negative must represent
  an error a real implementer could actually write.
- **`detect_leaks=1` on macOS arm64 SIGABRTs** the whole ASan run (leak detection is
  unsupported there) — @Claude asked this be recorded. Use
  `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.

# P3-⑥ Exit-Debt Record (Debt 1 real CoreAudio + drift-guard registration)

P3-⑤ head `262d0cd`; @Claude P3 出口裁决 = **NOT MET** (msg 954a0763): the five DSP slices are
complete, but two exit debts plus four 取证项 remain. This section records each debt drawn
down as it lands.

## 1. Debt 1 (platform half) — real CoreAudio output layout, four logicals per-channel

`tests/core/test_coreaudio_output.cpp`, registered in ctest as `test_coreaudio_output`
(Apple-only `if(APPLE)` block; CoreAudio/CoreFoundation linked only there; the `.cpp` is
`#ifdef __APPLE__`-guarded so the non-macOS matrix never compiles it). It reads the **real**
default output device's `kAudioDevicePropertyStreamConfiguration` (`AudioBufferList` — the
same authority `spike/device_behavior/device_probe.cpp` uses), projects the four logicals
(WET L/R + DRY A/B) onto that real layout via `OutputMapping::canonical()`, renders, reads
back, and asserts exact-match purity.

- **ORACLE is a default, not a constant**: the wiring under test is
  `OutputMapping::canonical()` = `{WET_L=0, WET_R=1, DRY_A=2, DRY_B=3}` (the P1-③ default,
  preserved from the previous Debt-1 adjudication). `device_layout.h` documents it as data a
  caller may override; there is no hardcoded branch.
- **Read-back semantics (declared boundary)**: the assertion validates the ADAPTER-FILLED
  buffer memory under the real layout — it is an in-memory stride/interleave check. It does
  NOT prove the driver delivered those slots to the intended physical jack; that is out of
  scope here. We never open an audible live stream nor push a hardware echo.
- **Loud skip, never silent**: three distinct skip reasons, each printing count/reason —
  (a) no default output device; (b) default device has <4 out channels; (c) not building on
  macOS (`#ifdef __APPLE__` + `if(APPLE)`). On CI (headless, no device) it SKIPs — and **this
  test never covers CI**. It is registered so the skip is VISIBLE, per @Claude's
  register-and-LOUDLY-skip principle.
- **Verified on the author machine**: default output = Studio Display Speakers, **8 channels,
  1 buffer = interleaved**. GOOD layout-aware read-back = 0 faults; NEGATIVE (re-read the same
  buffer as planar / channel-major) = **4 faults → RED** — i.e. the "treat interleaved as
  planar" bug Debt 1 exists to catch is detectable, not merely decorrelated. (`logical_signal`
  recomputes the same value; a transposed block is bit-mismatched.)

## 2. Evidence-layout drift-guard registered (per @Claude correction, msg db43f6a4)

`build_evidence_layout.py --check` was originally a dev-only script, NOT registered. @Claude
corrected that the "CI has no manual → skip-everywhere drift guard" rationale mis-applied
their own principle: the real CoreAudio test is also skipped on CI, yet is **registered +
loudly skipped**, because "don't register = invisible". The same shape is now applied to the
drift-guard: `evidence_layout_guard` is registered in ctest; manual present (dev) → real
regen==commit drift check (hard red on mismatch); manual absent (CI) → **loud skip** printing
"manual absent, drift-guard not applicable in this environment" and returning success. A CI
reader now SEES the guard is present but inert there.

## 3. Debt 2 — `real_path` actually routed through the executor

`tests/core/test_executor_realpath.cpp` (registered in ctest as `test_executor_realpath`), plus the
executor **consume-rule** codified into `core/include/lunar24/core/graph_compiler.h` (the
`CompiledRegion` doc block). This is the P3-⑥ exit debt from @Claude's mandate (msg 954a0763),
landed on his full GO (msg de1a60e8).

The P2-③ `GraphCompiler` *decides* `real_path` vs `z_inverse`; the gap was that no executor routing
a `real_path` edge actually realized the declared delay. This slice closes that, and lands the
"read it from a delay line" rule as core contract.

- **The consume-rule (core contract, graph_compiler.h)**:
  > For a `real_path` feedback edge with `delaySamples==D`, the executor MUST read the value it
  > delivers to the consuming module from a **per-edge delay line of depth D** (the line holding
  > the loop-forward values traversing that edge), handing the sending module the value from D
  > samples ago. It MUST NOT read the source module's last-written output. Reading the last output
  > is off-by-one for any D>1 (source ran after the consumer, so the consumer reads the previous
  > pass's value → loop delay D+1, not D).

- **Judge = invariance is necessary, not sufficient** (the methodological point @Claude raised).
  Two halves, both asserted in `test_executor_realpath.cpp`:
  - *Partition invariance* across the mixed non-uniform partition `(64,100,37,128,7,256,91)`
    (design/07 §4 line 107). Fires only on a block-lazy / one-buffer break.
  - *Absolute reference*: the realized loop delay must equal `delaySamples` exactly — NOT merely
    "the same under every partition". A wrong-but-consistent executor passes invariance and is
    caught only here.

- **Negatives (both kept, per @Claude "两个都留")**:
  - negative-1 (block-lazy / one-buffer) is **partition-VARIANT** → the partition judge fires.
  - negative-2 (read `moduleOut[source]` for a `real_path` edge) is **partition-INVARIANT but
    off-by-one** (loop 4 vs 3; first divergence at n=3) → only the absolute reference fires. This
    is the exact bug the P2-③ `MiniExec` scaffold commits.

- **MiniExec boundary (explicit @Claude requirement)**: the existing P2-③ `MiniExec` reads a
  `real_path` feedback via `moduleOut[source]` (last-written output), which is **off-by-one**.
  That read method does **NOT** enter the core contract — `MiniExec` is P2-③ test scaffolding, and
  its `real_path` consumption is what the negative-2 test now proves wrong. The `test_graph_compiler.cpp`
  `MiniExec` is left as-is (it is a plan-compiler test, not an executor) and the authoritative
  executor rule lives in the `graph_compiler.h` contract + `test_executor_realpath.cpp`.

To be settled at P3 exit by @Claude (after CI green): whether this counts as the debt closed; the
row above is marked **Landed** pending that adjudication.

*This file is a documentation artifact of the P3 slices, not a runtime input to the core
library.*
