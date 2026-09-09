# GH#12 task#103 — engine-layer preset LOAD / SAVE / INITIALISE (+ the D-1 mirror fix)

**Lane:** GH#12 — task #103, the slice @Codex contracted in msg `0a602a4f` (thread `#Lunar24:9d73f060`),
following the task#102 contract report (`350d654d`, report `2026-09-09-task102-preset-recall-save-contract.md`).
**Baseline:** `7a45fbae6066c7f564a326487b7902e8e068ed3d`.
**Worktree:** `wt-103-preset-actions` — branch `feat/12-preset-engine-actions`. Shared checkout untouched.
**Candidate head:** `4762502` (revision content; this report is committed one commit later on the
same branch) — **UNPUSHED**, awaiting @Codex review of the revision pass.
**Status:** implementation + acceptance + negative controls complete and locally green.
NOT merged, NOT closing GH#12, NOT released, MET not claimed.

**Revision pass** per @Codex `b9d8ff9f` (Rev-1/Rev-2/Rev-3) — see §8. No product **behaviour**
changed: the revision added *directed criteria* and a *driver trust model*, plus the comment-only
D-1 wording edit @Codex requested.

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
| `tests/host/test_preset_engine_actions.cpp` | NEW — 1 268 lines, **98 checks** (the acceptance; 63 before the Rev-3 additions) |
| `tools/run_preset_engine_negatives.py` | NEW — 590 lines, isolated shadow-header negative controls (7 single-source + 4 constructed pairs) with the Rev-2 judge self-check |
| `CMakeLists.txt` | register `test_preset_engine_actions` (fast) + `preset_engine_actions_negative` (`if(UNIX)`, fast) |

Net: 163 inserted lines in existing files + 2 new files. No production DSP, registry, schema or
disposition change. The revision pass adds only test/tool/report lines.

---

## 3. Acceptance — `tests/host/test_preset_engine_actions.cpp`, 98 checks, 0 failures

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
| B10 | 7 | 64 / 256 / irregular host blocks equal the per-frame reference (DRY bit-exact, WET 1e-12) — partition invariance. **Rev-3:** the *actual block boundary* is reconciled too: for each real block the boundary sample is compared against the per-frame reference at that frame (vOct/pressure 1e-12, gate rails exact), and the compared render is proven live on both DRY channels. |
| B11 | 3 | A recalled **consumed** scalar (left portamento) reaches the published vOct **and** the real host audio. |
| C1 | 24 | **Rev-3, slot → live → consumer matrix.** All four slots × Single/Twin/Split: the recalled payload is consumed by the two-sided behaviour — `gateL/gateR`, `pressure_out`, `V/OCT` equal the expected triple, **and** the output differs from a never-recalled default live (so the criterion cannot pass by mode collapse or by ignoring the recall). The three mode triples are mutually distinct on purpose: Single `gateL=10, press=0.5`; Twin `gateR=10, press=0.0`; Split `gateR=10, press=5/12` (right root F under the single-note mask `0x0001`). |
| C2 | 4 | **Rev-3, recalled scale is consumed.** The slot's LEFT `keyboardScaleEditor = 0x0F0F` is installed in the live config; a pushed 0.30 V is quantised to 3/12 V (the mask came from the slot), the never-recalled default mask passes 0.30 V through, and the published pitch actually changed. |
| C3 | 4 | **Rev-3, recalled sequencer driven by an explicit clock.** The slot's free-run 2-step sequence is installed; each explicit clock edge publishes the recalled step (0, 7/12, 0, 7/12 V), the gate stays high while it runs, and the same clock edges on the default live publish no note. |
| D1 | 2 (+6) | An accepted LOAD really committed (no false success). In the **constructed downstream-failure fixture build** the whole rejection branch prints six more labels — reported `RejectedState`, atomic (canonical/plan/format/ready), **same runtime object**, pending-event progress vs a never-handed control over 1 200 frames, that continuation proven live, and **all four audio channels** preserved (Rev-1). |

Fixtures: 4 slots x Single/Twin/Split, float32-exact legal values (the preset record stores the
scalar banks as float32 — a fixture value must *be* a float32 value or the save->load round trip
quantises it). Unconsumed fields and other slots are compared as **wire bytes or encoded fields**,
never as raw objects. The two scalar banks are made asymmetric in a *consumed* scalar (left
`keyboard_root_note` = C / right = F, portamento instant on both), which is what makes a
right-bank mutation observable at all — a bank difference that no consumer reads would let
`no_right_transfer` and `bank_crosswire` pass vacuously.

---

## 4. Negative controls — `tools/run_preset_engine_negatives.py`, 29/29 assertions PASS

Each control mutates **one production source** inside an isolated shadow include tree (`-I shadow`
first, nothing in the committed tree touched), rebuilds the acceptance, and asserts it terminates
normally and hits its **named** assertion. Retention positive control + mutations are reported
separately, as required.

**Rev-2 — the judge is now self-checked.** `rc != 0` is no longer treated as "the control fired",
and "the label is absent from the FAIL list" is no longer treated as "the label holds":

- red requires **rc exactly 1** (a negative-signal exit, an abort >1 and a compile failure are all
  rejected as evidence), a **complete termination summary** (`[preset engine actions] N/M checks
  FAILED`) whose counts agree with the printed `[PASS]`/`[FAIL]` lines, the named label present as
  an actual `[FAIL]` line, and every must-hold label present as an actual `[PASS]` line.
- green requires rc 0, the `N checks OK` summary, **no** `[FAIL]` line, and every required
  criterion family actually printed a `[PASS]` line (6 exact labels: A1, B4-right, C1-slot0-Single,
  C2 `0x0F0F`, C3 clock edge, D1 accepted) — a criterion that silently stopped running fails the
  control.
- 12 synthetic observations are fed to the same judge before any real build, and the judge must
  reject each untrustworthy one (negative-signal exit, abort, missing summary, summary/line
  disagreement, summary/FAIL-count disagreement, residual-only FAIL, missing expected RED, compile
  failure counted as red) and accept the clean red/green cases. If the self-check fails, the driver
  refuses to run any control.

**Retention positive control:** unmutated build GREEN, rc=0, complete summary, "98 checks OK",
every required criterion family observed.

| Control | Mutated source | Hit assertion |
|---------|----------------|---------------|
| `no_behaviour_mirror` | `keyboard_presets.h` — drop the D-1 mirror write (pre-fix source) | `A1 recall of a slot with a different behaviour is ACCEPTED (D-1 fixed)` (17 FAIL lines total, incl. `C1 `) |
| `no_right_transfer` | `keyboard_side_bank.h` — drop the right scalar-bank write | `A2 the recalled live config` (7, incl. `C1 `) |
| `slot_crosswire` | `keyboard_presets.h` — `live.keyboardPresets[slot ^ 1u]` | `B4 LOAD installs the slot payload into the live config` (22, incl. `C1 `) |
| `bank_crosswire` | `keyboard_side_bank.h` — write bank0/bank1 swapped | `A2 the recalled live config` (13, incl. `C1 `) |
| `save_reads_earlier_state` | `standalone_audio_engine.h` — SAVE snapshots a stale copy | `B5 slot 1 holds config B` (5) |
| `initialise_steals_load` | `standalone_audio_engine.h` — INITIALISE also LOADs | `B6 INITIALISE did NOT touch the live config (no implicit LOAD)` (1 — the mutation is exactly scoped) |
| `skip_real_apply` | `standalone_audio_engine.h` — return `Accepted` without the apply | `B4 LOAD installs the slot payload into the live config` (21, incl. `C1 `) |

The `C1 ` column is the Rev-3 connectivity family observing each transfer mutation independently:
the driver asserts it for the five mutations that must corrupt a bank or the slot selection, so the
new criteria are proven discriminating, not merely present.

**Constructed downstream-candidate failure (contract: "若最后一种无法用合法状态自然触发，用隔离
源码突变构造既有失败路径，不加生产故障宏").** An isolated *fixture* (not a defect) makes the recall
write an out-of-range scale editor, so the candidate the owner hands to `applyDeviceState` is
illegal. Paired experiment:

- fixture only -> the run is RED overall, but the whole **D1** rejection branch holds: reported
  `RejectedState`, atomic (canonical / plan / format / ready), **same runtime object**, pending-event
  progress equal to a never-handed control over 1 200 frames, that continuation proven live, and all
  four audio channels preserved.
- fixture + `false_success_on_rejection` (owner reports `Accepted` on a rejected candidate) ->
  `D1 an accepted LOAD really committed the slot payload (no false success)` goes **RED**.
  (This mutation makes the rejection branch unreachable, so no must-hold label is required of it —
  requiring one would demand the mutation be a no-op.)
- fixture + `failure_still_commits` (the failed action re-runs the ONE commit path on a repaired
  candidate, so the caller still reads `RejectedState` while the canonical state changed) ->
  `D1 a downstream candidate failure ... is reported as RejectedState` **holds** and
  `D1 the rejected preset action is atomic` goes **RED**.
- fixture + `runtime_mutated_on_rejection` (**Rev-1**) — the rejection is reported but the *running
  owner* is mutated anyway (`definition_->runtime().setVcoBaseHz(123.0)` on the non-accepted path) ->
  `D1 the rejected action preserves all four audio channels against the control` goes **RED** while
  runtime identity, pending-event progress, liveness and the atomicity labels all still **hold**.
  This is the control that closes Rev-1: the earlier D1 compared only state/plan/format, so this
  exact mutation passed undetected.

**On the sixth control ("失败仍 commit") — corrected.** My first pass said it was not expressible
because the owner has exactly one commit path and `deviceState()` is const-only. That reasoning was
too strong: the *observable symptom* is expressible on the single existing path, and it is what the
paired `failure_still_commits` control now turns RED. What is true and unchanged: the defect cannot
arise inside the owner without a source change (one commit path, const-only readback), and the
control adds no second production path — it re-invokes the existing one in the shadow tree only.

---

## 5. Gates run locally

| Gate | Result |
|------|--------|
| Release build | rc=0; no first-party warnings (only pre-existing third-party iPlug2/RtAudio/yoga warnings) |
| Release fast CTest (revision head) | **72/72 passed** (79.9 s) — was 70/70 on the baseline; +2 = `test_preset_engine_actions`, `preset_engine_actions_negative` |
| `test_preset_engine_actions` directly (revision head) | **98 checks / 0 FAIL / rc=0**, `[preset engine actions] 98 checks OK` |
| `preset_engine_actions_negative` (revision head) | **PASS**, 29/29 assertions, rc=0 |
| Debug + ASan + UBSan build | rc=0, no errors |
| Debug + ASan + UBSan fast CTest (revision head) | **72/72 passed** (142.3 s) |
| Real host target | `Lunar24Host.app/Contents/MacOS/Lunar24Host` linked in both legs; host-family gates (`host_engine_wiring_gate`, `host_script_codec`, `test_host_stream_plan`, `test_coreaudio_output`) all pass |
| Generator reproducibility | `tools/generate_registry.py` re-run -> `generated/` **byte-identical** (no diff) |
| Registry gate (always-on) | rc=0 — manifest self-coherent, no new rogue, `mustComplete == landed` |
| Registry gate `--require-full` | rc=1 — **exactly the pre-existing 12 by-design gaps**, single-listed and unchanged (8 non-scalar Root-A + 4 no-domain selectors); no gap added or removed by this slice |
| Local slow/probe set (`ctest -L slow`) | **7/7 passed** (854.3 s, run at `2a8d74c`): `test_d3_divider_restore`, `gh19_alias_probe`, `gh19_blamp_acceptance`, `gh20_vcf_probe`, `gh20_vcf_acceptance`, `gh12_keyboard_owner_probe`, `gh12_keyboard_side_restore_probe` |

The revision commits touch **only** `report/`, `tests/host/test_preset_engine_actions.cpp`,
`tools/run_preset_engine_negatives.py` and a **comment-only** edit in
`core/include/lunar24/core/keyboard_presets.h` (the D-1 wording @Codex requested; 3 comment lines
added, 2 reworded, no executable line changed). `git diff 2a8d74c HEAD -- core/include/lunar24/core/keyboard_presets.h`
shows only that hunk, so every gate above still describes the same product behaviour.

Per @Codex `b9d8ff9f` ("先不重跑无关慢全套") the unrelated slow set was **not** re-run in the revision
pass; it was green at `2a8d74c` and no product source changed since. Four-platform CI also stays
deferred to the final fixed head after @Codex's revision review.

---

## 6. Facts a reviewer should check (no hidden premises)

1. `applyPresetAction` has **no APP caller** (`grep` shows only the owner, the harness and the
   test). The product entry still does not exist; this is the engine seam.
2. The new enum never reaches the wire: `PresetAction`/`PresetActionStatus` are engine-internal and
   appear in no `ParameterId`, `ControlEventKind`, schema table or serialized field.
3. `applyPresetAction` reuses `applyDeviceState` verbatim — it does not construct a definition,
   does not touch `runtime_`, and does not rebuild from the audio callback.
4. A rejection is atomic because it is the existing `applyDeviceState` atomic rejection: the D1
   fixture proves canonical/format/plan/ready are unchanged, the **runtime object identity** is
   unchanged, and the **pending-event continuation plus all four audio channels** equal a control
   that was never handed the action.
5. The D-1 fix changes no default: it only converges a mirror the validator already demanded
   (`make_default_device_state` already writes both).

## 7. Not claimed

- No UI / menu / file persistence / BEHAVIOUR editor / remaining consumer / no-domain selector.
- No hardware-transfer or hardware-SAVE behaviour claim; no seamless-running-stream claim.
- GH#12 stays **OPEN** (the 12 coverage gaps are unchanged); no release; MET not claimed; nothing
  pushed, no PR, no merge.
- Next (after this slice): the APP persistence contact point and the remaining consumers — this
  card ending is not the end of the round.

---

## 8. Revision pass per @Codex `b9d8ff9f` (Rev-1 / Rev-2 / Rev-3)

Product sources untouched; every change below is a directed criterion, a control, or tooling.

**Rev-1 — the rejection must preserve the running owner, not just the state.** @Codex added
`if (st != Accepted) definition_->runtime().setVcoBaseHz(123.0);` to the isolated head and both D1
labels still passed, i.e. the check compared only state/plan/format and missed a corrupted sound.
D1 now runs a **same-owner, already-running, pending-event** fixture: it asserts the reported status,
atomicity, the **runtime pointer** is the same object, the **full two-sided control/event progress**
over 1 200 frames equals a control that was never handed the action, that continuation is proven
live (the pending release fires at its frame), and **all four audio channels** equal the control
(DRY bit-exact, WET 1e-12). The mutation above is added as the paired control
`runtime_mutated_on_rejected_candidate`, which now turns the audio-preservation label RED.
`failure_still_commits` is kept, as @Codex directed, but no longer substitutes for runtime
preservation.

**Rev-2 — the driver's trust model.** `rc != 0` no longer counts as "the control fired", and
"the label is not in the FAIL list" no longer counts as "the label holds". See §4: red requires
rc exactly 1, a complete termination summary consistent with the printed check lines, the named
label printed as `[FAIL]`, and every must-hold label printed as `[PASS]`; green requires rc 0, the
`N checks OK` summary, no `[FAIL]`, and six exact required labels actually printed. Twelve synthetic
observations self-check the judge (negative-signal exit, abort, missing summary, summary/line
disagreement, summary/FAIL-count disagreement, residual-only FAIL, missing expected RED, compile
failure counted as red, and the clean red/green cases) before any real build is run.

**Rev-3 — the product matrix, recalled scale, and clock-driven sequence.** The report claimed
"four slots × three modes" but the acceptance only exercised one slot at the state layer and mostly
Split for behaviour. Added:
- **C1 (24 checks):** all four slots × Single/Twin/Split through the real render path, asserting
  `gateL/gateR`, `pressure_out` and `V/OCT` equal the expected triple *and* differ from a
  never-recalled default live. The two scalar banks are now asymmetric in a **consumed** scalar
  (left root C / right root F) — without that, a right-bank mutation was invisible (this is what
  fixed the first `no_right_transfer` / `bank_crosswire` failures). The three mode expectations are
  mutually distinct so no criterion can pass by mode collapse: Single `gateL=10, press=0.5`;
  Twin `gateR=10, press=0.0`; Split `gateR=10, press=5/12`.
  *(Design note: the Split expectation needs the single-note mask `0x0001` — under a full chromatic
  mask `0x0FFF` the root is inert, so a root-only difference would not move the published pitch.)*
- **C2 (4 checks):** a recalled `0x0F0F` scale mask is installed and consumed — 0.30 V quantises to
  3/12 V, the default microtonal mask passes 0.30 V, and the published pitch really changed.
- **C3 (4 checks):** a recalled free-run 2-step sequence driven by an **explicit clock** publishes
  the recalled step on every edge (0, 7/12, 0, 7/12 V), holds the gate high, and the same edges on
  the default live publish nothing.
- **B10:** the actual block boundary is reconciled against the per-frame reference (not only the
  four-channel aggregate), keeping the existing audio comparison.
- The source comment describing the pre-fix behaviour was reworded to "**state-layer latent
  defect**; the APP had no caller at the time" (was "silent no-op for existing users").

The directed criteria and the driver are complete; this head is submitted for @Codex's revision
review. Nothing is pushed.
