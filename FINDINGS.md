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
| 3 | `real_path` not routed through the executor in P2-③ (mixed-partition judge on the audio path) | Owed debt | @Pi | P3 exit — real modules exist; judge = ① (mixed partition) | Open |
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
