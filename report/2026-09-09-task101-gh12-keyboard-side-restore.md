# GH#12 task#101 — per-side keyboard performance state + side-tagged events

**Lane:** GH#12 "keyboard state restore" — task #101, the slice @Codex contracted in msg `57a5ab2a`
(thread `#Lunar24:a0b57998`), following the task #100 ruling `9943ae28`.
**Baseline:** `920f51dbcee4321536ca1c8695b65e67fe2453a0`.
**Worktree:** `wt-101-kbd-side` — branch `feat/12-keyboard-side-restore`. Shared checkout untouched.
**Status:** implementation + acceptance + negative controls complete and locally green. Candidate is
**UNPUSHED**, awaiting @Codex review-revision. NOT merged, NOT closing GH#12, NOT released, NOT MET.

---

## 1. What this slice does (and what it deliberately does not)

The keyboard already had a decode/validate half (task #100: 35 stored items, 31 readers, 19 runtime
effects) but only **one** `KeyboardBehaviour` + **one** `ArpSeq` instance, and the producer seam had
no way to say *which physical side* a performance came from. This slice adds:

1. **Two independent performance instances per type** (`keyboardBeh_[2]`, `keyboardArpSeq_[2]`).
   Shared *configuration* is not shared *performance state*: Single/Twin install bank 0 into both,
   Split installs bank 0 (left) / bank 1 (right); each instance keeps its own held notes, chord,
   glide and envelope.
2. **An explicit internal `KeyboardSide`** (`Left=0, Right=1`) on `PerformanceInput` and
   `ControlEvent`, propagated through `InputStateMachine::translate()`, through `ArpSeq::emit_()`,
   and into the runtime's per-side dispatch. It is **not** a persisted format, **not** a
   `ParameterId`, **not** a physical jack, and is **never** inferred from MIDI channel / pitch /
   `noteId`. Old callers default to `Left` = pre-#101 behaviour. `control_event_before` is
   deliberately unchanged (the side is not a comparator key — adding one would silently re-order
   existing same-sample sequences with no evidence).
3. **The four registered keyboard outputs published**, no new jack, no new route:
   `v_oct_out` ← left pitch (all modes); `gate_left_main_out` ← left gate; `gate_right_out` ← right
   gate with an **explicit low rail** under Single; `pressure_out` ← Single: the real pressure
   behaviour output / Twin+Split: the **right pitch** (manual BEHAVIOUR). Right pitch and pressure
   are never summed. VCO-B's default normal source is untouched.
4. **`applyKeyboardState(state)`** — the one place the owned state's keyboard half reaches the
   instances, called from the state ctor **after** the DSP apply. It reuses the existing
   `side_bank()` / `read_side_scalar` / `read_behaviour_params` / `read_arp_seq_params` choke
   points; no new per-side id. No failure branch: every input is already validated upstream, so a
   `keyboardApplyOk_` flag or a new status enumerator with no reachable false would be a vacuous
   pass. The non-vacuous evidence is the per-side readback.

**Out of scope, explicitly (contract §3/§4):** calibration, input normalisation, MPR/encoder
configuration, plate/button execution, preset recall actions, APP startup/save, any new
time/scale/hardware transfer function, any internal BPM clock or clock division. `clock_bpm` (129)
stays **parsed only**.

**Disposition honesty.** The 35-item target contract is unchanged. This slice's *actual* coverage:
**19 runtime-effect parameters** (readback + behavioural discrimination), **3 parsed-but-unconsumed**
(108/115/127 — proven to change no sample), **1 parsed-no-consumer** (`clock_bpm`, registry norm
0.0, documented). The remaining items (8 non-scalar Root-A structures, 4 no-value-domain
selectors, plus calibration/preset/APP payloads) are **not** claimed as applied. **No 35/35 claim.**

---

## 2. Files changed

| File | Change |
|------|--------|
| `core/include/lunar24/core/control_event.h` | `ControlEvent::side` (appended last; not a comparator key) |
| `core/include/lunar24/core/input_state_machine.h` | `PerformanceInput::side`; `translate()` stamps it on every emitted event |
| `core/include/lunar24/core/arp_sequencer.h` | `ArpSeq::params()` readback; `emit_()` propagates `src.side` |
| `core/include/lunar24/core/keyboard_behaviour.h` | `KeyboardBehaviour::configure()` stores `params_`; `params()` readback |
| `core/include/lunar24/core/machine_definition.h` | binds **all four** keyboard jacks; calls `applyKeyboardState(state_)` after the DSP apply |
| `core/include/lunar24/core/machine_runtime.h` | per-side instances, `setKeyboardBindings(4)`, `applyKeyboardState`, per-side event dispatch, per-side tick + 4-jack publish, readbacks |
| `tests/probes/gh12_keyboard_side_restore_probe.cpp` | NEW acceptance (95 checks) |
| `tests/mutation/run_gh12_side_restore_mutation.sh` | NEW 8-point isolated negative-control driver |
| `CMakeLists.txt` | registers the probe (label `slow`) |

Product diff: 6 headers, +256/−18 lines. No new `ParameterId`, no new jack, no new route, no
schema change, no fault macro in a production header.

---

## 3. Acceptance — 95 checks, 0 failures

Entry is the real chain, never a private setter:
`DeviceStateV1 → encode_device_state → decode_device_state → buildMachineRuntimeCandidate →
enqueueControlEvent / InputStateMachine::translate → processBlock → controlVoltageAt`.

| Group | What it pins | Checks |
|-------|--------------|--------|
| A | Single default path: silent when unplayed; +1.0 V v_oct, 0/10 V gate, real pressure, audible; unused right gate explicit low; right-plate note collapses to the Left identity | 10 |
| **P** | **the REAL producer seam**: `translate()` carries the side — left+right notes are two independent performances; a left note-off releases only the left | 2 |
| B | Twin: both sides install **bank 0** config; two plates are two independently gated notes; both glide with the shared speed; releasing left leaves right held; `v_oct`=left, `pressure_out`=right | 7 |
| C | Split: each side installs **its own** bank (scale editor + portamento asymmetry) | 6 |
| D | the **same** `(source, channel, noteId)` on both sides = two notes; independent release | 3 |
| E | `pressure_out` is mode-correct; right pitch + pressure are never summed | 3 |
| G | a reset clears **both** sides (no stuck right note) | 2 |
| F | the **existing** arp/seq paths reachable from an explicit clock: no internal BPM clock; arp direction/variation/hold; keyboard passthrough; seq run/length/gated CV | 14 |
| H | the 19 runtime-effect parameters read back per item | 19 |
| H′ | 108/115/127 are parsed but change **no sample** (honest "parsed, not applied") | 2 |
| I | portamento family (117 speed / 118 legato) discriminates | 3 |
| J | vibrato family (119/120/121/122) discriminates | 4 |
| K | pressure family (123 mode / 124 rise / 125 fall) discriminates | 3 |
| L | quantiser root (128) discriminates a sparse (Ionian) scale | 1 |
| M | default prepare == default restore (one documented parse difference: `clock_bpm`) | 7 |
| N | 64/256/irregular partitions bit-identical (4 outputs + dryA) | 2 |
| O | **every mode** (Single/Twin/Split) restored twice is bit-identical; an invalid state is `rejected_state`; caller state untouched; a rejected attempt leaves an **already-active** runtime's subsequent trace bit-identical | 7 |

Selected load-bearing results:

- **`M4`** — the parsed `clock_bpm` is the **registry norm 0.0**, one shared global value, and no
  consumer derives a tempo from it. (Contract §3 forbids inventing a norm→BPM law.)
- **`H′2`** — two states differing **only** in 108/115/127 render bit-identical audio: the parse is
  preserved and the behaviour is honestly absent.
- **`N1/N2`** — 8192 frames in 64/256/irregular partitions produce bit-identical `dryA` and
  bit-identical four-output snapshots.
- **`O1`** — the same state restored twice is bit-identical in **every** mode (Single/Twin/Split,
  both sides driven), i.e. restore is idempotent and does not leak performance state across sides.
- **`O2`** — `keyboard.mode = 99` is rejected by `validate_device_state` inside the candidate chain
  and yields **no** definition; `O3` proves the caller's state is byte-identical afterwards.
- **`O4`** — a rejected attempt made **while another runtime is already active** leaves that
  runtime's subsequent trace bit-identical (the failure keeps the active state/format/plan).
- **`M1/M5`** — default prepare and default restore compile the same graph and render bit-identical
  audio and identical keyboard outputs.

---

## 4. Negative controls — 8/8 RED on a pinned assertion

`tests/mutation/run_gh12_side_restore_mutation.sh`. Baseline is built and run first and must be
**GREEN (95/95)**; then each point is a *detached production-source mutation* in a shadow header
(no tracked path is written, no fault macro added). A point passes only if the mutated build
(a) compiles, (b) terminates normally (prints the `N checks, M failures` summary — a crash is
rejected), and (c) trips the **exact pinned `[FAIL]` line**, not merely a non-zero count.

| # | Mutation | Production anchor | Pinned assertion | Result |
|---|----------|-------------------|------------------|--------|
| NC-1 | drop side routing | `keyboardEventSideIndex_` → `return 0u` | `B1 Twin: two plates are two independently gated notes` | RED, 12 failures |
| NC-2 | shared performance state | right gate published from `keyboardBeh_[0]` | `B1 Twin: releasing the left note leaves the right note held` | RED, 3 |
| NC-3 | right reads left bank | `read_behaviour_params(..., Left)` for both | `C1 Split: each side installs ITS OWN bank's scale editor` | RED, 3 |
| NC-4 | skip the configure | `configure(...)` → `(void)read_behaviour_params(...)` | `H10 keyboard.portamento_speed (117) read back` | RED, 25 |
| NC-5 | right publishes left | `single ? pressL : pitchR` → `pitchL` | `E2 Split: pressure_out is the right pitch (2.0 V), not pitch + pressure` | RED, 5 |
| NC-6 | invalid candidate still published | `if (!v.ok) return rejected_state` removed | `O2 an out-of-range keyboard.mode is rejected_state with no definition` | RED, 1 |
| NC-7 | clear at block boundary | reset dispatched at the end of every `processBlock` | `N1 dryA is bit-identical across 64/256/irregular` | RED, 13 |
| NC-8 | translate drops the side | `out[n].side = KeyboardSide::Left` | `P1 translate(): a left note and a right note are two independent performances` | RED, 2 |

---

## 5. Gates run locally

| Gate | Command | Result |
|------|---------|--------|
| Release fast suite | `ctest --test-dir build-release --label-exclude slow` | **70/70 passed** (incl. regeneration zero-diff, license/header gate, negative-fixture gate, id-stability, host engine oracle, state apply oracles, host wiring + script codec) |
| Release slow/probe gate | `ctest --test-dir build-release --label-regex slow` | 7 tests: `test_d3_divider_restore`, `gh19_alias_probe`, `gh19_blamp_acceptance`, `gh20_vcf_probe`, `gh20_vcf_acceptance`, `gh12_keyboard_owner_probe`, **`gh12_keyboard_side_restore_probe`** — see §5.1 |
| ASan + UBSan Debug | `cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"` then full build + fast suite | **build rc=0**, **70/70 passed**, probe **95/95, 0 failures, 0 sanitizer reports** |
| Real host artifact | `cmake --build build-release --target Lunar24Host` | built; `build-release/out/Lunar24Host.app/Contents/MacOS/Lunar24Host` present and executable |
| Full-coverage gate | `python3 tools/check_registry_complete.py --require-full` | **rc=1, by design** — the *pre-existing* 12-item keyboard gap is unchanged (`newRogue=[]`, no regression, same 8 Root-A non-scalar + 4 no-value-domain selectors). Per contract §5 this gap is listed separately and does **not** change the gate. |
| Mutation harness | `./tests/mutation/run_gh12_side_restore_mutation.sh` | baseline 95/95 GREEN, **8/8 mutations RED on their pin** |

### 5.1 Slow/probe gate detail

The slow suite is PR-time only (CI `probe-gate` job, ubuntu, `-L slow`). Local Release run:

```
1/7 test_d3_divider_restore ...... Passed    6.39 sec
2/7 gh19_alias_probe ............. Passed   18.66 sec
3/7 gh19_blamp_acceptance ........ Passed   82.36 sec
4/7 gh20_vcf_probe ............... <running>
...
```

`gh20_vcf_probe` / `gh20_vcf_acceptance` / `gh12_keyboard_owner_probe` /
`gh12_keyboard_side_restore_probe` follow; the final result is appended to the delivery message
when the run completes.

---

## 6. Facts a reviewer should check (no hidden premises)

1. **The side is metadata, not identity.** `source`/`channel`/`noteId` remain the event identity;
   `side` is a separate field. Criterion **D** proves the *same* identity on both sides is two notes
   and that neither release touches the other.
2. **Single collapses at the event entry**, not in the bank reader: `keyboardEventSideIndex_`
   returns `0` under Single, so a right-plate event is one performer. Twin/Split keep the event's
   own side. A reset clears **both** instances regardless of the event's side (**G2**).
3. **No new enumerator and no vacuous pass.** `applyKeyboardState` has no failure branch; the
   candidate status set is unchanged; `keyboardApplyOk_` does not exist. `machine_candidate.h` was
   **not** modified by this slice.
4. **No invented law.** No norm→BPM mapping; `clock_bpm` is readback-only (`M4`). The arp/seq modes
   are reachable **only** from an explicit external clock edge (`F1`).
5. **Default behaviour.** Default prepare and default restore are bit-identical (`M5`); an unplayed
   keyboard publishes zero on all four outputs (`A0`).
6. **`pressure_out` under Twin/Split is the right V/oct**, per the manual BEHAVIOUR layout; the
   pressure behaviour still runs internally but is not mixed into that jack (`E1`/`E2`).

---

## 7. Open items / not claimed

- 35/35 disposition is **not** claimed. This slice's coverage is 19 consumed + 3 parsed-unconsumed +
  1 parsed-no-consumer; the rest is unchanged and still owed.
- The 12 full-coverage gaps are pre-existing and untouched (`--require-full` stays RED by design).
- The slow/probe gate must run on the exact pushed head in CI before any merge decision.
- Merge / GH#12 closure / release / MET are **not** in this slice; @Codex holds the merge gate.
