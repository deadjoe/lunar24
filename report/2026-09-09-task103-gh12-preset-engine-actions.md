# GH#12 task#103 — engine-layer preset LOAD / SAVE / INITIALISE (+ the D-1 mirror fix)

**Lane:** GH#12 — task #103, the slice @Codex contracted in msg `0a602a4f` (thread `#Lunar24:9d73f060`),
following the task#102 contract report (`350d654d`, report `2026-09-09-task102-preset-recall-save-contract.md`).
**Baseline:** `7a45fbae6066c7f564a326487b7902e8e068ed3d`.
**Worktree:** `wt-103-preset-actions` — branch `feat/12-preset-engine-actions`. Shared checkout untouched.
**Candidate head:** `2a8d74c` — **UNPUSHED**, awaiting @Codex review-revision.
**Status:** implementation + acceptance + negative controls complete and locally green.
NOT merged, NOT closing GH#12, NOT released, MET not claimed.

---

## 1. Scope delivered (contract §1-§4)

| # | Contract item | What landed |
|---|---------------|-------------|
| 1 | D-1: mirror the canonical behaviour | `load_preset_to_live` now also writes `parameters[keyboard_behaviour]` (`core/include/lunar24/core/keyboard_presets.h`, +12 lines incl. the defect comment). The opposite canonical direction of `pressure_output` is untouched; all slot ids, `reserved`, wire bytes and unconsumed fields unchanged; **no schema / registry / disposition change** (generator output byte-identical, §4). |
| 2 | Owner API with explicit slot+action | `StandaloneAudioEngine::applyPresetAction(slot, PresetAction{Load,Save,Initialise})` + `PresetActionStatus` (`host/include/host/standalone_audio_engine.h`, +97). The enum is internal: it is neither a `ParameterId` nor a `ControlEventKind`, so it never enters the wire or the ControlEvent space. |
| 3 | Stopped-stream boundary, current format, no false success | The call reuses the engine's **current committed** format (`sampleRate_/blockSize_/inputCapability_/outputCapability_`) and goes `copy canonical -> one existing helper -> the ONE applyDeviceState candidate/commit`. Not-ready / illegal slot / unknown action / rejected candidate are four distinct inspectable statuses. |
| 4 | LOAD / SAVE / INITIALISE semantics | LOAD writes the slot payload into the live keyboard state (the existing two-sided behaviour consumes it). SAVE snapshots the **last successfully committed** canonical live config into the slot. INITIALISE resets **only** the target slot and never implicitly loads. SAVE/INITIALISE leave the live config content and the other three slots as they are. |

**Software behaviour stated honestly (contract §3, in the API comment and here):** all three
operations re-commit a complete candidate, so a **success resets the performance/event time
baseline**. This is *not* a claim that a hardware SAVE re-triggers anything, and *not* a claim of
seamless operation during a running stream. Saving configuration is *not* capturing current
DSP/keys transients: a caller must have committed its configuration through `applyDeviceState`
first, because a direct runtime setter or ControlEvent does **not** write back to the canonical
state (verified: `machine_definition.h:752` exposes `deviceState()` const-only; the four live
setters have zero product callers).

**Out of scope, explicitly (contract §4):** no UI, no two-level menu navigation, no BEHAVIOUR
editor, no file path/persistence, no no-domain selector, no remaining-parameter consumer, no
reverse DSP capture, no third mirror. **There is still no APP caller** — `host/plugin.cpp:42` is
`MakeConfig(0, 0)` and the keyboard menu is inert; `applyPresetAction` is referenced only by the
owner itself and the test harness (§3).

---

## 2. Files changed

| File | Change |
|------|--------|
| `core/include/lunar24/core/keyboard_presets.h` | D-1 mirror convergence in `load_preset_to_live` |
| `host/include/host/standalone_audio_engine.h` | `PresetAction` / `PresetActionStatus` / `applyPresetAction` + the include of the preset helpers |
| `tests/host/test_engine_harness.h` | `presetAction()` / `presetAttempted()` / `presetStatus()` + `applyCanonical()` on the shared harness |
| `tests/host/test_preset_engine_actions.cpp` | NEW — 943 lines, 63 checks (the acceptance) |
| `tools/run_preset_engine_negatives.py` | NEW — 353 lines, isolated shadow-header negative controls (7 single-source + 3 constructed) |
| `CMakeLists.txt` | register `test_preset_engine_actions` (fast) + `preset_engine_actions_negative` (`if(UNIX)`, fast) |

Net: 163 inserted lines in existing files + 2 new files. No production DSP, registry, schema or
disposition change.

---

## 3. Acceptance — `tests/host/test_preset_engine_actions.cpp`, 63 checks, 0 failures

Entry (contract §"先写会红的产品判据"): `encode -> decode -> validate -> EngineHarness ->
applyPresetAction -> DeviceAdapter/processBlock`. Every criterion below renders real audio through
the real host block path; ~25k frames total, so it stays in the fast every-push set.

| Criterion | Checks | What it pins |
|-----------|-------:|--------------|
| A1 | 2 | Recall of a slot whose behaviour differs from the live mirror is **ACCEPTED** on the pre-existing public surface (D-1 fixed), and the live config really changed. |
| A2 | 1 | The recalled live config equals the slot payload on **both** scalar banks + non-scalars + selectors + **both** compatibility mirrors. |
| A3 | 3 | An unsynchronised behaviour mirror is still REJECTED (`keyboard_live_invalid`, field 9002) and atomically — so A1 cannot pass vacuously. |
| B1 | 4 | Not-ready engine: all three actions return `RejectedNotReady`, never success. |
| B2/B3 | 6 | Illegal slot (4) and unknown action (0x7F) rejected; wire bytes / plan / format unchanged by the rejected call. |
| B4 | 7 | LOAD accepted, real commit, **two-sided** discrimination via the split probe (left `keyboardScaleEditor=0x0FFF` chromatic: 0.04 V quantises to 0.0 V; right `0x0000` microtonal: 0.04 V survives) and an audible DRY render. |
| B5 | 9 | Legal history sequence SAVE A -> change live -> SAVE B -> LOAD A -> LOAD B: the slot holds the **last committed** config, not an earlier copy. |
| B6 | 5 | INITIALISE resets only the slot, does **not** touch live; the subsequent LOAD is what applies it. |
| B7 | 6 | SAVE scope: live content preserved, other slots' **wire bytes** untouched (unconsumed fields included). |
| B8 | 5 | Slot -> live -> slot is **byte-exact on the encoded wire record** (never a raw struct compare — padding is meaningless). |
| B9 | 6 | Idempotence (same action twice) + a re-commit replays an identical event script **bit-identically** from the same new input; the compared trace is proven live (gate latches at frames 0/130, releases at 700). |
| B10 | 4 | 64 / 256 / irregular host blocks equal the per-frame reference (DRY bit-exact, WET 1e-12) — partition invariance. |
| B11 | 3 | A recalled **consumed** scalar (left portamento) reaches the published vOct **and** the real host audio. |
| D1 | 2 | An accepted LOAD really committed; a rejected candidate is reported `RejectedState` and is atomic. |

Fixtures: 4 slots x Single/Twin/Split, asymmetric banks, float32-exact legal values (the preset
record stores the scalar banks as float32 — a fixture value must *be* a float32 value or the
save->load round trip quantises it). Unconsumed fields and other slots are compared as **wire
bytes or encoded fields**, never as raw objects.

---

## 4. Negative controls — `tools/run_preset_engine_negatives.py`, 12/12 assertions PASS

Each control mutates **one production source** inside an isolated shadow include tree (`-I shadow`
first, nothing in the committed tree touched), rebuilds the acceptance, and asserts it terminates
normally (rc=1) and hits its **named** assertion. Retention positive control + mutations are
reported separately, as required.

**Retention positive control:** unmutated build GREEN, rc=0, "63 checks OK".

| Control | Mutated source | Hit assertion |
|---------|----------------|---------------|
| `no_behaviour_mirror` | `keyboard_presets.h` — drop the D-1 mirror write (pre-fix source) | `A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)` (+15 collateral) |
| `no_right_transfer` | `keyboard_side_bank.h` — drop the right scalar-bank write | `A2 the recalled live config` |
| `slot_crosswire` | `keyboard_presets.h` — `live.keyboardPresets[slot ^ 1u]` | `B4 LOAD installs the slot payload into the live config` |
| `bank_crosswire` | `keyboard_side_bank.h` — write bank0/bank1 swapped | `A2 the recalled live config` |
| `save_reads_earlier_state` | `standalone_audio_engine.h` — SAVE snapshots a stale copy | `B5 slot 1 holds config B` |
| `initialise_steals_load` | `standalone_audio_engine.h` — INITIALISE also LOADs | `B6 INITIALISE did NOT touch the live config (no implicit LOAD)` |
| `skip_real_apply` | `standalone_audio_engine.h` — return `Accepted` without the apply | `B4 LOAD installs the slot payload into the live config` |

**Constructed downstream-candidate failure (contract: "若最后一种无法用合法状态自然触发，用隔离
源码突变构造既有失败路径，不加生产故障宏").** An isolated *fixture* (not a defect) makes the recall
write an out-of-range scale editor, so the candidate the owner hands to `applyDeviceState` is
illegal. Paired experiment:

- fixture only -> the run is RED overall, but the two **D1** labels hold (`RejectedState` reported,
  and atomic: canonical wire / plan / format / ready unchanged).
- fixture + `false_success_on_rejection` (owner reports `Accepted` on a rejected candidate) ->
  `D1 an accepted LOAD really committed the slot payload (no false success)` goes **RED**.
- fixture + `failure_still_commits` (the failed action re-runs the ONE commit path on a repaired
  candidate, so the caller still reads `RejectedState` while the canonical state changed) ->
  `D1 a downstream candidate failure ... is reported as RejectedState` **holds** and
  `D1 the rejected preset action is atomic` goes **RED**.

**On the sixth control ("失败仍 commit") — corrected.** My first pass said it was not expressible
because the owner has exactly one commit path and `deviceState()` is const-only. That reasoning was
too strong: the *observable symptom* is expressible on the single existing path, and it is what the
paired `failure_still_commits` control now turns RED. What is true and unchanged: the defect cannot
arise inside the owner without a source change (one commit path, const-only readback), and the
control adds no second production path — it re-invokes the existing one in the shadow tree only.

---

## 5. Gates run locally at `2a8d74c` (product head; unchanged by the later commits)

| Gate | Result |
|------|--------|
| Release build | rc=0; no first-party warnings (only pre-existing third-party iPlug2/RtAudio/yoga warnings) |
| Release fast CTest | **72/72 passed** (84.5 s) — was 70/70 on the baseline; +2 = `test_preset_engine_actions`, `preset_engine_actions_negative` |
| Debug + ASan + UBSan build | rc=0, no errors |
| Debug + ASan + UBSan fast CTest | **72/72 passed** (154.0 s) |
| Real host target | `Lunar24Host.app/Contents/MacOS/Lunar24Host` linked in both legs; host-family gates (`host_engine_wiring_gate`, `host_script_codec`, `test_host_stream_plan`, `test_coreaudio_output`) all pass |
| Generator reproducibility | `tools/generate_registry.py` re-run -> `generated/` **byte-identical** (no diff) |
| Registry gate (always-on) | rc=0 — manifest self-coherent, no new rogue, `mustComplete == landed` |
| Registry gate `--require-full` | rc=1 — **exactly the pre-existing 12 by-design gaps**, single-listed and unchanged (8 non-scalar Root-A + 4 no-domain selectors); no gap added or removed by this slice |

The two later commits (`1fba3f5` report, `834e1d8` negative-control hardening) change **only**
`report/` and `tools/run_preset_engine_negatives.py` — `git diff --name-only 2a8d74c 834e1d8 --
core host tests CMakeLists.txt` is empty, so the table above still describes the product head.
Re-verified at `834e1d8`: `ctest -R 'preset_engine_actions_negative|test_preset_engine_actions'`
-> 2/2 pass.

| Local slow/probe set (`ctest -L slow`) | **7/7 passed** (854.3 s): `test_d3_divider_restore`, `gh19_alias_probe`, `gh19_blamp_acceptance`, `gh20_vcf_probe`, `gh20_vcf_acceptance`, `gh12_keyboard_owner_probe`, `gh12_keyboard_side_restore_probe` |

Four-platform CI is deliberately **not** run yet: per the contract it runs after @Codex's revision
pass on the final fixed head. The local slow set was run now because @Codex `ae65355c` confirmed it
is part of the already-authorised complete check, not an exempt item.

---

## 6. Facts a reviewer should check (no hidden premises)

1. `applyPresetAction` has **no APP caller** (`grep` shows only the owner, the harness and the
   test). The product entry still does not exist; this is the engine seam.
2. The new enum never reaches the wire: `PresetAction`/`PresetActionStatus` are engine-internal and
   appear in no `ParameterId`, `ControlEventKind`, schema table or serialized field.
3. `applyPresetAction` reuses `applyDeviceState` verbatim — it does not construct a definition,
   does not touch `runtime_`, and does not rebuild from the audio callback.
4. A rejection is atomic because it is the existing `applyDeviceState` atomic rejection: the D1
   fixture proves owner/canonical/format/plan/ready are unchanged.
5. The D-1 fix changes no default: it only converges a mirror the validator already demanded
   (`make_default_device_state` already writes both).

## 7. Not claimed

- No UI / menu / file persistence / BEHAVIOUR editor / remaining consumer / no-domain selector.
- No hardware-transfer or hardware-SAVE behaviour claim; no seamless-running-stream claim.
- GH#12 stays **OPEN** (the 12 coverage gaps are unchanged); no release; MET not claimed; nothing
  pushed, no PR, no merge.
- Next (after this slice): the APP persistence contact point and the remaining consumers — this
  card ending is not the end of the round.
