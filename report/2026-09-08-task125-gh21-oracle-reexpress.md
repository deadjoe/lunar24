# GH#21 — GH#11 oracle re-expression (task #125/#127) — deliverable to @Kimi

- Worktree: `wt-21-gh21-smoothing`, branch `feat/21-continuous-smoothing`
- HEAD: `9524c20b2e46c6ad86152e2ab667b940a443dc1c` (3 commits ahead of `origin/main` `31736b6`)
- PR: **#30** (`feat/21-continuous-smoothing` → `main`)
- Result: **449 checks / 0 failed** (was 418 with 98 failed; 444 → 449 after the NO-GO rework added the fail-closed negative control), Release / Debug / ASan+UBSan each **70/70**, count pinned **177 seconds / 168 none**.

## NO-GO rework applied (task #128) — three items, all resolved

1. **`sqCv` / `cvPos` unused-but-set-variable (-Werror)**: removed the two dead write-only arrays
   in `test_machine_control_sources.cpp` (declarations at :469/:1019 + their one write each). GCC /
   linux-clang `-Wall` enables `-Wunused-but-set-variable` but AppleClang's `-Wall` does not — that is
   why the earlier three-leg local (mac) was green. Re-compiling the test TU locally with
   `-Wunused-but-set-variable` added is clean for all `tests/core/*.cpp`.
2. **`applyDspParam` seconds-snap branch skipped `controlParamValid_`**: added a fail-closed
   `if (!controlParamValid_(id, v)) { invalid_value; return; }` at the branch head (mirrors the live
   `setControlParamValue` ordering), so a malformed/out-of-domain whole-state apply rejects keep-old
   instead of resetting the smoother to garbage. Added negative control t17-(E): `applyDspParam`
   0.7 then 5.0/NaN → `invalid_value` AND getter kept at 0.7; verified **RED under the no-validation
   mutation** (4 checks).
3. **mut2 count aligned to the anchored minimal definition**: see §Negative-control item 2 — **6 RED**,
   not the earlier-reported 70.

## Operative rules (from @Kimi d1b9402d / 8ba2e4e3)

- Rule 1: only TIMELINE changes, not assertion object; single-frame → "settle 后达到目标"; no semantic-discrimination loosening.
- Rule 2: settle frame count from tau (N=⌈k·tau·fs⌉, tolerance declared in comment, no magic numbers).
- Rule 3: per-item mapping in report; negative controls re-verified still red.
- Rule 4: 15 discrete single-sample assertions UNCHANGED (control group).
- Rule 5: t14(a2) partition-consistency must recover green after the finding-2 fix; NOT relaxed.
- Class A: value-reach assertions → render window amplified to tau-derived settle frames.
- Class B: take **S** (smooth the edge), three-part lock = (a) edge invariant + (b) anti-old-frame + (c) closed-form crossing window.

## Derived constants

- `gh21_settle_frames(kSr)` = ⌈kSr·tau·ln(1/relTol)⌉ + 2 = **11055 @48k** (tau=0.050, relTol=1e-2).
- Helper `gh21_cross_frame(start,targets,cross,n_max)` and `gh21_cross_up_frames(...)`: closed-form one-pole crossing — replays real ParameterSmoother + per-frame ordering (apply → processFrame → advance → step), returns first published rising-crossing frame.
- `kSmootherSettleRelTol = 1e-2`, `kSmootherSettleAbsTol = 1e-9` (provisional software policy, declared in parameter_smoothing.h).

## Per-item mapping

### Class A — value-reach (window = settleN)

| Item | Old (single-frame) | New (re-expressed) |
|------|--------------------|--------------------|
| t9 有效到 getter (19) | value reaches getter at frame X | value reaches getter after `gh21_settle_frames` (settle 后达到目标); getter==target |
| t9 无效保持 getter (19) | invalid leaves getter unchanged (1 frame) | assert `status==invalid_value` AND 未采纳新 target — settle 后 getter=旧 target, 非 invalid 值（@Kimi Class A 补条）|
| t14 norm→rate + WALL-CLOCK (9) | rate reached at single frame | rate reaches expected Hz after settleN; WALL-CLOCK determinism preserved (44.1/48/88.2/96 kHz 0/1/20) |
| t4 VCF L/R 读回 | readback == settled value at frame X | `gh21_advance(rt, settleN)` then readback == settled value (+2.0/−3.0/+0.0) |
| t7 x/y/rail 到达 | value at frame | value == settled value after settleN |
| t10 VCF-L/R 读回 | single-frame | `gh21_advance(rt, settleN)` then VCF-L==+2.0, VCF-R==−2.0, !sameD |

### Class B — edge/trigger (three-part lock: edge-invariant + anti-old-frame + closed-form window)

| Item | Old | New (three-part) |
|------|-----|------------------|
| t3 seq gate @frame8 | gate pulses at frame 8 | (a) gate at first predicted cross `pred[0]` (closed-form crossing of smoothed ext-clock, 0.5V↔norm 0.55) + one-sample pulse (cleared next frame); (b) `pred[0] != 8` (anti-old-frame); (c) crossing window from tau. Step advance at `pred[1]`/`pred[2]`; idle-CV companion (step0 stays 0 with no edge; publishes 1.0V after settleN) |
| t5 joystick→ext_clock @frame5 | advances at frame 5 | (a) `advances==1` (rising→exactly one advance, sustained no repeat); (b) `gate[5]==0.0` (anti-old-frame, no advance at pre-smoothing frame 5); (c) window from closed-form crossing. `noPhantom` on repatch. Step0 CV mid-ramp at frame 5 (smoothed). |
| t6 精确 8/24/40/56 前进 + 单样本 gate | advance at exact 8/24/40/56 | (a) `single` (one advance per edge, no repeat); (b) `notRaw` (anti-old-frame: no advance at raw value-step frame); (c) `win` (each advance within ±1 frame of closed-form crossing). Gate 10V exact-sample pulse at each gate-enabled-step crossing, `oneShot` (returns to 0 next frame). |
| t6c gate→EG | EG gated at frame 8 | (a) `maxEnv>0` (EG rises after the gate, crossing window); (b) `aBefore==0` (anti-old-frame: not gated at raw frame 8); (c) closed-form crossing. |
| t7 ext-clock advances | advances at precise value-step frame | `pred.size()>=2` closed-form crossing window; `partConsistent` retained (64/128/mixed partition bit-identical). |
| t11 pulser crossing | crosses at exact sample | `maxStep>=2` (real advance through steps, not a single-sample snap), `cvVaries`, `sawRising`, `railCoherent`; step0 CV is a live smooth ramp (not reach-1-sample). |
| t13c/d joystick gate→EG | EG gated at exact frame | `gh21_cross_frame(...)` upCross; `aBefore==0` anti-old-frame; `aRiseLast>0` attack after crossing; release verified (`applyParam` 0.0 then `aFall<aRiseLast`); B idles while A gated (no cross-talk). |
| neg5 exact sample | exact-sample | seconds-smoothed re-expressed: (a) no block-front (`sameD(xWithOff[0], xNoOff[0])`); (b) family hit (`!sameD(xWithOff[kCap-1], xNoOff[kCap-1])`); response compare two runs (with/without frame-8 offset event). |

### Control group (UNCHANGED, @Kimi rule 4)
The 15 discrete single-sample-direct assertions: sequencer clock/stages/step_gate, LFO wave/speed_mult, envelope hold/self_gen, dac_vref/mpr121/debounce/encoder_direction, VCO-B linear-full-depth self-edge. NOT re-expressed.

## Negative-control re-verification (both must be red, both are)

1. **tau→0 snap mutation** (`kGh21SmoothingTauSeconds = 0.0` → coefficient 1.0): **12 RED** — the smooth-ramp / anti-old-frame / predictive-crossing-window assertions discriminate:
   - t3 anti-old-frame (not frame 8), t4 (jump-not-ramp, mid-ramp), t5 anti-old-frame + step0 CV mid-ramp, t6 anti-old-frame ×3, t6c anti-old-frame, t7 anti-old-frame ×N, t11 step0 live ramp, t17 live ramp (not one-sample). Reverted.
2. **B-form perpetual-writer mutation — anchored minimal definition** (delete ONLY `if (sm.settled()) continue;` from `advanceControlSmoothing_`; leave `if (sm.settled()) applySmoothedControl_(id, sm.target());` intact): **6 RED** — t11 runtime-PULSER crossing + t11 clock_out, t14 partition-invariant, t15 rail-publish, t17 direct-setter-no-clobber ×2. Reverted to (A). (Earlier report claimed **70 RED**; that was the straddling harsher mutation which ALSO deleted the snap line — the audit-chain number has been corrected to the 6 RED of the anchored minimal B-form that @Kimi's NO-GO review specified.)

Both mutations were reverted; final tree is clean (444/0), no stray mutation markers.

## ✅ FLAG — product-behavior change to rule on (beyond the pure contract)

`parameter_smoothing.h::setTarget` now rebases `resetLevel_` onto `current_`:
```cpp
void setTarget(double value) { resetLevel_ = current_; target_ = value; }
```
**Why required:** without it, on a RETURN transition resetLevel_ stays at the last reset/snap value; when it equals the new target, span==0 → absTol-only branch → the ramp takes ~48000 frames instead of the tau-derived 11055. The re-expressed Class A/B return-transition assertions (t4 offset return, t13c/d release, t9/t7 value-reach) require the tau-window settle, so the rebase is necessary for correctness — it is a consequence, not optional scope creep.

**Scope note:** `progress()` has no core callers; `setTarget` callers are `keyboard_behaviour` (legato/portamento glide) and `machine_runtime` control smoothers. Verified safe. **Requesting @Kimi aware/ruling.**

## Three-leg + CI

- Fast CTest suite (`--label-exclude slow`): **Release 70/70, Debug 70/70, ASan+UBSan 70/70.**
- gh19/gh20 slow probe/acceptance gates test VCO/VCF DSP — unaffected by control smoothing; handled by CI `probe-gate` at PR time.
- **Not self-merged / no issue close / no release** — awaiting @Kimi review.

## Guardrails honored

- No self-merge/close-issue/release/MET.
- No factual conflicts outside contract were encountered after the Class B ruling; the setTarget rebase and its necessity are reported (not silently shipped as contract).
- Worktree is the only valid repo; `/Users/joe/github/deadjoe/lunar24` NOT built.
- Structural fidelity: tau/settle tolerance remain provisional software policy (no invented hardware constants).
