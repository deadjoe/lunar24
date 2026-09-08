# GH#21 Surface-2 acceptance (task #94 g) — bearbone report

**Date:** 2026-09-08 · **Branch:** `feat/21-continuous-smoothing` · **Worktree:** `wt-21-gh21-smoothing`
**Head:** this commit is the `(g)` acceptance slice, stacked on `c1652bc` ((a)–(f) design/impl),
baseline main `e61d0ed`. The pushed `(g)` commit SHA is reported separately to `@Kimi`.

## Scope

Surface-2 wires the **16 vco/vcf panel-knob `Smoothing::seconds` params** into the shared
continuous-smoothing family (20 → 36). Live use goes through the `ControlEvent` lane (one-pole
ramp), whole-state restore through `applyDspParam` (snap). Surface-1's discipline is reused
verbatim — the `@Kimi` hard constraint "严禁第二套平滑实现" is satisfied by construction: the
16 use the very same `advanceControlSmoothing_` / `applySmoothedControl_` / `currentControlParam_`
members; only the member set grew.

This is the acceptance slice (g). (a)–(f) are already landed (`81f2cf9` impl, `717d08f` flip,
`6960938` target re-freeze, `c1652bc` design report).

## New test — `test_18_gh21_surface2_acceptance` (file `tests/core/test_machine_control_sources.cpp`)

Pins the six acceptance items from the REV-2 contract (`report/2026-09-08-task93-gh21-surface2-design.md`).

| Id | Contract item | What it asserts |
|----|---------------|-----------------|
| T0 | exactly-16 admission | the 16 vco/vcf panel-knob seconds params are in the smoothing family — **not 15, not 17** |
| T1 | `applyDspParam` SNAPS | whole-state apply lands exactly on the target immediately, then is **inert** (no drift), and a live ramp **reaches the exact target within the tau-derived settle window** (`ceil(sr·tau·ln(1/relTol))+2`, not a magic constant) |
| T2 | anti-old-frame | the *first* published frame of a live set is **strictly between** v0 and v1 (one-pole, no single-sample jump); the ramp is **monotone**, never overshoots `[v0,v1]`, and stays finite |
| T3 | partition consistency | the same ramp rendered under `256 / 128 / 64 / mixed(128,16,96,8,8)` partitions is **bit-identical** per-sample wetL, and the final knob state is partition-independent (the ramp began identically, not a silent no-op) |
| T4 | zero-alloc | with all 36 smoothers live (sources + panel knobs), the render path allocates **zero** (`g_allocCount` unchanged over 4000 blocks of 1 frame) |
| T5 | bypass fail-closed | an out-of-domain vco/vcf value in **both** lanes lands `invalid_value` / is rejected and **keeps the prior valid value** (the smoother is never reset to a garbage level) |
| T6 | out-of-scope lock | a discrete `Smoothing::none` selector still whole-state **snaps** via the dispatch and still has **no live lane** — the 16-move did not sweep its sibling selectors into the family |

Each of the 16 is exercised in a `Surf2Case` table `{pid, v0, v1, getter, name}`:
- `vco_a_tune`/`vco_b_tune` (oct `[-1,1]`) ramp `-0.5 → 0.5` → `vcoATune()`/`vcoBTune()`.
- `vco_a_morph/pw/cv_amt`, `vco_b_morph/pw/cv_amt`, `vcf_l_freq/res/mod`, `vcf_r_freq/res/mod`,
  `vcf_dist`, `vcf_gain` (norm `[0,1]`) ramp `0.1 → 0.7` → the matching getter.

So T1/T2 run 16×, giving broad per-param coverage; T3–T6 are global discriminator checks.

### Negative-control validation (proof it is not a tautology)

Temporarily bypassed the domain validator (`dspParamValid_` step-0 branch `return v>=min&&v<=max`
→ `return true;`), rebuilt, and re-ran `test_18`:

```
FAIL: T5 oct below -1 -> invalid_value (fail-closed, not setter-coerced)
FAIL: T5 invalid whole-state keeps the prior 0.3 (never reset)
FAIL: T5 whole-state NaN keeps the prior 0.3
FAIL: T5 live invalid value does not retarget the smoother (keeps 0.3)
[FAIL] 560 checks, 4 failed
```

T5 went RED (4 checks) — the bypass-fail-closed path is a real discriminator, not a check that
reads the implementation's own validation. Mutation reverted before commit; `git status` confirms
only the test file changed.

### One correctness note surfaced (already resolved in the test)

The live lane carries the target as a **float32** `ControlEvent` sample value, so the smoother
converges **exactly** to the float32-carried target, not the un-quantized double literal. The
initial convergence check compared against the double literal (e.g. `0.7`) and red 14 norm-knob
cases; the fix compares against `static_cast<double>(static_cast<core::SignalSample>(v1))`. This
is the true "reaches the exact instructed target" semantics (mirrors how Surface-1 reads the
float32-carried control voltage, e.g. `JoystickCv` landing one float32 ULP off).

## Count pin (acceptance ①) — unchanged

```
Smoothing::seconds=177    Smoothing::none=168   (total 345, unchanged)
```

`(f)` was evidence-status-only, so the count is preserved bit-for-bit (`/Users/…/generated/lunar24/registry.hpp`).
The 2 excluded params `vco_a_pwm`/`vco_b_pwm` stay `Smoothing::seconds` but their fieldEvidence
index-4 is `unverified` (transfer_unavailable → deferred to GH#15), and they are NOT part of the 36.
`vcf.r_res`/`vcf.r_mod` were re-frozen on the target side (`6960938`).

## Local verification (mac only — the only repo = this worktree)

Standard runnable three-leg, `ctest --label-exclude slow` (the every-push fast suite):

| Config  | Result |
|---------|--------|
| Release  | 70/70 passed, 43.2 s |
| Debug    | 70/70 passed, 53.1 s |
| ASan+UBSan | 70/70 passed, 85.3 s |

`test_machine_control_sources` reports **560 checks, 0 failed** in Release (new `test_18` included).
The only CI red by design is `--require-full` coverage (the existing 12-declared keyboard gap), which
is byte-identical to before — gate unchanged.

## Deferrals / not claimed

- No self-merge, no close of GH#21, no release, no MET.
- The `vco_a/b_pwm` seconds-unwired gap is a pre-existing declaration-only gap, deferred to GH#15.
- `x-had` future Surface-2 work would reuse this shared mechanism; no second smoothing implementation.

Report head: this file is versioned alongside the `test_18` change in the same `(g)` commit; the
pushed `(g)` HEAD is the deliverable to `@Kimi`.
