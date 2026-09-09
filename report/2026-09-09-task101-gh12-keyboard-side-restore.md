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

## 4. Negative controls — 8/8 RED on a pinned assertion (first round @ `a5062df`; superseded by §8)

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
| Mutation harness | `./tests/mutation/run_gh12_side_restore_mutation.sh` | baseline 95/95 GREEN, **8/8 mutations RED on their pin** (first round @ `a5062df`; 10/10 at the current head — §8) |

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

---

## 8. Review rework — @Codex msg `b7d63c1f` groups 1-3 (head `45a2be2`, UNPUSHED)

Baseline of this round: `a5062df`. The bank / mode / event-propagation tests were **not** rewritten
(they were already correct). Only the three contracted groups changed.

### 8.1 Group 1 — the real host single-commit entry (probe section `Q`)

New `accept_host_entry()` re-enters the SAME mode / restore / failure facts through
`testengine::EngineHarness` — encode → decode → `StandaloneAudioEngine::applyDeviceState` →
`processBlock`, capturing the four real output channels — instead of only
`buildMachineRuntimeCandidate`.

- `Q1` every mode (Single/Twin/Split) is accepted; a re-commit of the same state on the same owner
  is output-identical on all four channels. `Q1b` proves the compared trace is a **live** render.
- `Q2` a **live, already-rendering** owner: load A → render → attempt an invalid B
  (`keyboard.mode = 99`) → `RejectedInvalidState` (typed) + validation family; **state / format /
  plan unchanged**, the canonical state is the **same object with the same bytes** (A, never B),
  and the **subsequent** four-channel trace still equals the untouched A control.
- `Q3` illegal formats (`sr=0`, `block=0`, `outCh=1`) are `RejectedFormat` on the live owner, which
  keeps format + plan + canonical bytes + trace.
- `Q4` anti-vacuity: a **legal** re-commit does replace the canonical bytes, the block size and the
  free-run audio (VCO-B +3 octaves) — so “unchanged after rejection” is a real claim, not a path
  that never changes anything.

**FINDING F-1 (reported to the owner, deliberately NOT fixed in this slice).** The host render path
`StandaloneAudioEngine::processBlock` → `DeviceAdapter::renderBlock`
(`core/include/lunar24/core/device_adapter.h:286-294`, one `rt.processFrame(in, true)` per frame) →
`SynthRuntime::processFrame` (`core/include/lunar24/core/machine_runtime.h:1821`) **never drains
`EventTimebase`**. Only `SynthRuntime::processBlock` does
(`core/include/lunar24/core/machine_runtime.h:1849-1860`: `eventTimebase_.processBlock(...)` then a
per-frame `applyControlEvent_`), and **no product code calls it** — `host/plugin.cpp` only calls
`engine_.processBlock`. Consequence: queued `ControlEvent`s (keyboard / MIDI) are invisible through
the host entry today; the compared traces there are the **free-running** machine. Design intent
(`design/00-status.md:975-1000`, #46) placed the drain in `processBlock`, so this is a host-path
defect, not a probe defect. Minimal repro: a `EngineHarness` loaded with a Split state, one
`note(Left, 1.0)` enqueued, then `render(4800)` publishes `v_oct = 0` (no note), while the same
sequence through the canonical runtime block entry publishes `v_oct = 1.0`. Per the task contract
this pauses **only** the affected (host-entry event-driven) part: `Q` keeps the enqueues so its
checks strengthen automatically once the host path drains, and the event-driven families are pinned
in `R`/`S` on the canonical runtime block entry (the only entry that drains events today).

### 8.2 Group 2 — the executed configuration (not the `params_` mirror)

`KeyboardBehaviour::executed()` (`core/include/lunar24/core/keyboard_behaviour.h`),
`SynthRuntime::keyboardBehaviourExecuted(side)` (`machine_runtime.h`) and
`ParameterSmoother::timeConstantSeconds()` (`parameter_smoothing.h`) read the state `tick()`
**actually runs off** (mode / rise / fall / tau / legato / Hz / depth / delay / pressure-control /
mask / root / rate). `H10`-`H19` now pin those. The `ArpSeq` half still reads `params_` (it really
uses it) and is kept.

### 8.3 Group 3 — real consumption + real block boundaries (probe sections `R`, `S`)

- `R` (canonical runtime block entry): legal **user cables** `pressure_out → vco_b.v_oct_in` and
  `gate_right_out → envelope_b.gate_in` drive the **real consumers**. `R1` reads VCO-B’s own audio
  (zero-crossing ratio ≈ 2 at +1 octave, and it follows the **right** side, not the left), `R2`
  reads EG-B’s own `envelope_b_env_out` (opens from the normalised left gate; the right gate does
  **not** reach it without a cable; the cable **overrides** rather than sums; removal is
  **bit-identical**).
- `S` replaces the old end-of-run `N` comparison: the same state + **mid-block** event script
  (samples 0/130/700/1030/1500) rendered (a) one frame per block (reference) and (b) in real
  256-frame blocks. All **four audio channels** are bit-identical; **every boundary** equals
  reference frame `(i+1)·256−1` on all four published values; the traces all move (non-vacuity);
  the right key event lands at its exact frame 130.
- `T` (kept from the previous round): preserved fields + the 4-preset payload round-trip.

### 8.4 Negative controls — now 10/10, rc **exactly** 1, summary must be the LAST line

`tests/mutation/run_gh12_side_restore_mutation.sh` (host include path added; `-I$WORK` first so the
host shadow wins):

| # | Mutation | Anchor | Pinned assertion | Result |
|---|----------|--------|------------------|--------|
| NC-1..NC-5, NC-7, NC-8 | unchanged | unchanged | unchanged | RED (see §4) |
| NC-6 | **validator_bypassed** (reframed to its true meaning: the validator no longer rejects; it does **not** claim “a failure is still published”) | `machine_candidate.h` | `O2 an out-of-range keyboard.mode is rejected_state with no definition` | RED, 5 |
| NC-9 | **config_not_executed** — `configure()` keeps `params_ = p` but skips `setMode`/`setTimes`/`setNorm` | `keyboard_behaviour.h` | `H10 keyboard.portamento_speed (117) EXECUTED as the installed glide time constant` | RED, 19 |
| NC-10 | **host_error_commit** — the host format gate records the rejection but falls through and commits the bad format | `host/standalone_audio_engine.h` | `Q3 blockSize=0 is RejectedFormat; the live owner keeps format+plan+trace` | RED, 2 |

Baseline at this head: **130 checks, 0 failures**. Every mutation exits with **exactly rc=1**, prints
the summary as its **last** line, and trips its pinned `[FAIL]` line.

### 8.5 Gates at this head

| Gate | Command | Result |
|------|---------|--------|
| Probe (CTest) | `ctest --test-dir build-release -R '^gh12_keyboard_side_restore_probe$'` | **Passed, 130/0** |
| Incremental Release build (all targets) | `cmake --build build-release -j8` | **rc=0, no errors/warnings** |
| Mutation harness | `bash tests/mutation/run_gh12_side_restore_mutation.sh` | **rc=0**: baseline 130/0 + 10/10 RED |
| Full fast suite / ASan+UBSan / CI | **not re-run** — per @Codex “暂不重复全套或push” | owed on the pushed head |

The earlier slow/probe-gate run (`/tmp/gh12_101_slow_release.log`, 129 checks / 7 failures) is the
**pre-rework** tree and is superseded by §8.5; it must be re-run on the pushed head.

## 9. F-1 fix — @Codex msg `0fd75e9f` (code head `8f827d0`, UNPUSHED)

@Codex: “F-1 成立，授权在本片修复。优先最窄实现：DeviceAdapter 每帧完成输入映射后调用既有
`SynthRuntime::processBlock(&in,1,&out,true)`，再按原样写输出；不把 drain 偷加进 processFrame…
沿用唯一 EventTimebase 和 dispatch，不建第二套事件循环，不加动态缓冲或新延迟。”

### 9.1 The change (product, 1 file)

`core/include/lunar24/core/device_adapter.h` — `DeviceAdapter::renderBlock` now drives each frame
through the EXISTING `SynthRuntime::processBlock(&in, 1, &out, true)` instead of
`SynthRuntime::processFrame(in, true)`. One drain, one frame of time advanced, one `EventTimebase`,
no second event loop, no dynamic buffer, no added latency; the drain is **not** inside `processFrame`
(that would advance the time base twice per frame through `processBlock`). The header doc block
records why. No new id / jack / route / schema / enum; no product header gained a fault macro.

### 9.2 RED first, then GREEN

Section `U` was written and run **against the unfixed adapter** before the fix: `151 checks, 7
failures`, every failure F-1-driven (`U1`×4 exact-frame/gate/pitch/audio, `U2`×2 host-vs-canonical,
`U3` live). After the one-line fix the same probe is `151 checks, 0 failures`, and the 130 pre-existing
checks are untouched.

| `U` criterion | Pins |
|---|---|
| U1 | A Split script queued on the **host** entry acts at its EXACT absolute frames: left note@0 / release@700 (`gateL` high at 0 and 699, 0 at 700), right note@130 (`gateR` 0 at 129, rail at 130), `v_oct` = left pitch, `pressure_out` = right pitch from 130; and the four-channel host audio differs from a no-event control. |
| U2 | The host entry's published control trace **and** all four audio channels equal the canonical per-frame entry (audio through the one frozen 0.5 device scale), bit-for-bit. |
| U3 | 64 (×40) / 256 (×10) / irregular `{300,100,1024,7,501,400,100,128}` host blocks are bit-identical to a per-frame reference; every real block boundary equals the reference frame `cum-1`; the compared trace is live. |
| U4 | Re-committing the same state on the same owner **resets** the event time base (replayed script is bit-identical control + audio), then a default-Left `reset` clears BOTH sides. |
| U5 | Standalone `EventTimebase` semantics under the per-frame drain: same-frame global order (`stable_sort` reference), per-frame drain == whole-block drain, future event retained, late event delivered at offset 0 + counted, continuous 65th non-coalescible event refused + counted, parameter coalescing by `ParameterId`, critical overflow → ONE reconcile reset first (`criticalFlushed()` = 64, pending 0), dispatch capacity leaves the remainder pending + counted. |

### 9.3 Q/R/S re-pointed at the fixed host path

Per “Q/R/S 的事件驱动验证接到修好的真实 host 路径；自由跑 trace 不能充作键盘验收”:

- **Q** already entered through `EngineHarness`; its queued scripts are now live rather than
  free-running, and the stale “F-1 deliberately NOT fixed here” comment block is replaced.
- **R** (`accept_cable_consumption`) now builds a `EngineHarness`, enqueues on `producerRuntime()`
  and renders one real 9600-frame host block; VCO-B is read from the host's `DRY_B`. Same five
  assertions (cable ⇒ VCO-B tracks the RIGHT pitch ≈2×; right side not the normalised left; removal
  bit-identical; EG-B driven by the right gate only when cabled; override, not sum).
- **S** (`accept_block_boundary_traces`) now renders the eight real 256-frame blocks through the
  host entry and compares all four **host** channels to the per-frame reference through the 0.5
  scale (exact), plus every block-boundary control snapshot; the mid-block right key event is still
  pinned at frame 130.

### 9.4 Zero-alloc / zero-free with events in flight

`tests/host/test_host_engine_oracle.cpp::allocator_probe()` — the measured window now also covers
the drain: (a) a held note + a **late** release (sample 5, queued after `blockStart_` = 128) + a
second note, with enqueues **inside** the window (the producer seam is fixed-capacity/no-heap, and a
real producer enqueues while audio renders); (b) both capacity boundaries under load — 70 parameter
events (coalesce), 70 non-coalescible pitch events (continuous overflow), 70 clock edges (critical
overflow → reconcile reset). `g_allocCount`/`g_freeCount` are unchanged across the whole window, and
a non-vacuity check proves the events were really DELIVERED (`keyboard_gate_left_main_out` > 1 V
after the window). Reverting the adapter to `processFrame` (the NC-11 mutation, applied to a
detached shadow build) makes **11/3866** checks fail, including exactly that delivery check — so the
new zero-alloc claim is not vacuous. Oracle: **3866 checks OK**.

### 9.5 Negative controls — now 11/11

`tests/mutation/run_gh12_side_restore_mutation.sh`: baseline `151 checks, 0 failures`, then all
eleven defense points RED on their pinned `[FAIL]` line with **rc exactly 1** and the summary as the
**last** line. New:

| NC | Mutation (file) | Pinned assertion | Result |
|---|---|---|---|
| NC-11 | **adapter_reverts_processframe** (`device_adapter.h`): `renderBlock` back to `rt.processFrame(in, true)` — the host path never drains | `U1 a left note at frame 0 and its release at frame 700 act at their EXACT frames` | RED, 17 |

### 9.6 Same-machine CPU cost (@Codex: report a comparison if the cost is unsuitable)

Same process, same machine, same default state, empty event queues (the common case), interleaved
repeats, `-O2`: per-frame `processBlock` (fixed, drains) vs per-frame `processFrame` (pre-F-1).

| Run | fixed | pre-F-1 | ratio | delta |
|---|---|---|---|---|
| 1 | 1.196 s | 1.158 s | 1.033 | +19.0 ns/frame |
| 2 | 1.162 s | 1.164 s | 0.999 | −0.9 ns/frame |

400 000 frames × 5 reps per column, ≈2.9 µs/frame. **Reported at raw values only** (@Codex msg
`7fbf04f6`): the two runs disagree in sign (1.033 then 0.999), so no single "noise floor" figure is
asserted and the earlier "≤0.7 %" characterisation is withdrawn. The defensible reading is limited to
"the fixed per-frame drive costs the same order as the pre-F-1 drive at ~2.9 µs/frame". Measurement
tool: `/tmp/gh12_101_cpu_compare.cpp` (not committed).

### 9.7 Gates re-run at this head (affected directed tests + negatives only, per instruction)

| Gate | Result |
|---|---|
| `gh12_keyboard_side_restore_probe` (CTest) | **Passed, 151/0** |
| Affected directed tests (16): `test_device_adapter_oracle`, `test_host_engine_oracle`, `test_state_apply_oracle`, `test_state_apply_oracle_169`, `test_machine_control_outputs`, `test_machine_audio_families`, `test_machine_cable_restore`, `test_vco_normal_source`, `test_d3_divider_restore`, `test_host_stream_plan`, `host_engine_wiring_gate`, `host_script_codec`, `gh12_keyboard_owner_probe`, `gh19_alias_probe`, `gh20_vcf_probe` | **16/16 passed** |
| Full Release build, all targets | **rc=0, zero warnings/errors** |
| Mutation harness | **rc=0: baseline 151/0 + 11/11 RED** |
| Full fast suite / ASan+UBSan / CI / slow probe gate | **NOT re-run** — per @Codex “只重跑受影响定向与负控…再推进全套” |

### 9.8 Housekeeping (honest note) — history rewritten

The review-rework commit swept `build-release/` (913 regenerable artifacts) into git via `git add -A`.
Deleting them in a later commit is **not** enough: the blobs stay in the unpushed history and would be
uploaded on push (@Codex msg `7fbf04f6`). The branch (unpushed, no remote ref) was therefore rewritten
with `git filter-repo --path build-release/ --invert-paths --refs 920f51d..HEAD --partial --force`;
`/build-release/` stays in `.gitignore`. **No source or test line changed** — the net diff
`920f51d..HEAD` is byte-identical to the pre-rewrite diff (15 files, +3180/−21), and no commit tree in
the range contains a `build-release/` path any more. The pre-rewrite tip is preserved locally at
`refs/backup/task101-prewipe` (`fd9befe`) until the review is closed.

| pre-rewrite | rewritten |
|---|---|
| `c435c2e` (review rework) | **`45a2be2`** |
| `cd850eb` (§8 docs) | **`fc726e9`** |
| `aaad381` (F-1 fix, code head) | **`8f827d0`** |
| `fd9befe` (§9 docs) | **`09fd7ee`** |

`ef89a07`/`6c97eae`/`5551b46`/`a5062df`/`cf4ad48` are unchanged (never contained the artifacts).

### 9.9 Not claimed

No push, no merge, no GH#12 closure, no release, no MET. The 35-item ledger unchanged (§6).

---

## 10. Full gate suite at the rewritten head — @Codex msg `8dc08f0d` (pushed head, UNPUSHED at writing)

@Codex authorized the full local suite after the targeted re-run: “授权继续完整 Release/Debug/ASan+UBSan
及真实 host/generator 检查，**按实际 CTest 数报告**”. Every figure below is the actual CTest count — no
extrapolation from a subset, and the slow gate was run to completion rather than assumed. The gates
were run on the source/test tree of `f854478`; the only commit after it is this §10 report text, so the
numbers hold verbatim at the pushed head.

### 10.1 Results

| Gate | Command | Result |
|---|---|---|
| Release build, all targets | `cmake --build build-release -j` | **rc=0, 0 warnings / 0 errors** |
| Release fast suite | `ctest --test-dir build-release --output-on-failure --label-exclude slow` | **100% tests passed out of 70** (48.58 s) |
| Debug + ASan + UBSan fast suite | `ctest --test-dir build-debug --output-on-failure --label-exclude slow` | **100% tests passed out of 70** (113.37 s) |
| Release slow / probe gate | `ctest --test-dir build-release --output-on-failure -L slow` | **100% tests passed out of 7** (851.24 s, rc=0) |
| ↳ of which the slice's own probe | `#72 gh12_keyboard_side_restore_probe` | **Passed 0.32 s** (151 checks / 0 failures) |
| ↳ other slow tests | `#65 d3_divider_restore` 6.55 / `#66 gh19_alias_probe` 14.04 / `#67 gh19_blamp_acceptance` 82.39 / `#69 gh20_vcf_probe` 373.80 / `#70 gh20_vcf_acceptance` 373.96 / `#71 gh12_keyboard_owner_probe` 0.17 | all **Passed** |
| Mutation harness | `tests/mutation/run_gh12_side_restore_mutation.sh` | **rc=0**: baseline 151/0 green + **11/11** defenses RED (rc exactly 1, pinned `[FAIL]` line, summary last) |
| Zero-alloc oracle | `test_host_engine_oracle` (in the fast suite) | **3866 checks OK**; the shadow adapter-revert fails 11/3866 |
| Full-coverage gate — **NOT a pass** | `tools/check_registry_complete.py --require-full` | **rc=1, the same 12 pre-existing gaps** (§10.2); RED by design, gate/target/classification untouched |

ASan/UBSan runs use `ASAN_OPTIONS=detect_leaks=0` (AppleClang ships no LSan) and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The 77-test inventory is unchanged: **70 fast +
7 `slow`**. The Debug build also reports 0 warnings / 0 errors. The Debug row above is the fast
subset; the **complete** Debug suite (all 77, no label filter) was run afterwards on @Codex
`d7daabf9` — see **§11.1**.

### 10.2 The 12 pre-existing coverage gaps (listed separately, unchanged)

`--require-full` stays RED by design; this slice does not touch the gate, the frozen target, or the
classification. The gate's own two groups, verbatim ids:

- **8 Root-A non-scalar — structural, NOT a to-do; must NOT be flattened into scalar parameters:**
  `keyboard.plate_tune`, `keyboard.preset_a`, `keyboard.preset_b`, `keyboard.preset_c`,
  `keyboard.preset_d`, `keyboard.pushbutton_value`, `keyboard.quantise_scale_editor`,
  `keyboard.seq_steps`
- **4 selector-toggle with no evidenced value domain — must stay a gap until the domain is evidenced:**
  `keyboard.arp_clock`, `keyboard.arp_rhythm`, `keyboard.seq_clock`, `keyboard.seq_rhythm`

### 10.3 History / tree reconciliation (@Codex `8dc08f0d`: “交整理后的SHA与树差异对账”)

Content head **`f854478`**; the pushed head is `f854478` + this §10 report-only commit (its exact SHA is
in the hand-off message and the PR head, not here — naming it would require rewriting this section).

- Rewritten commits: `45a2be2` / `fc726e9` / `8f827d0` / `09fd7ee` (§9.8);
  `cf4ad48`/`a5062df`/`5551b46`/`6c97eae`/`ef89a07` unchanged; then the report-only `f854478`
  (housekeeping note + §9.6 CPU raw-value correction) and this §10 commit.
- **No commit in `920f51d..f854478` contains a `build-release/` path** — checked tree-by-tree across
  all **10** commits in the range: 0 hits each. **277** tracked files; `git status` clean;
  `git ls-files | grep -c '^build-release/'` = **0**.
- Net diff `920f51d..f854478` = **15 files, +3197 / −21**: the 10 modified headers/tests, `.gitignore`,
  `CMakeLists.txt`, the 2 new test files, and the report. With this §10 section the same 15-file diff
  reads +3264/−21 — the report is the only file that moves.
- At the rewritten docs head `09fd7ee` the net diff was byte-identical to the pre-rewrite diff
  (15 files, +3180/−21, §9.8). Everything added since is **report text only**: `f854478` (+17 lines:
  the §9.8 note and the §9.6 CPU raw-value correction @Codex `7fbf04f6`) and this §10.
- **Source/test tree unchanged since the reviewed code head:**
  `git diff --name-only 8f827d0..f854478 -- core/ tests/ CMakeLists.txt` → **empty**.
- Pre-rewrite tip preserved locally at `refs/backup/task101-prewipe` (`fd9befe`) until the review closes.

### 10.4 Not claimed

No merge, no GH#12 closure, no release, no MET. Push + draft PR are per @Codex `8dc08f0d`; the
four-platform CI and the PR-only slow `probe-gate` must pass on the **exact pushed head** before any
merge decision, which @Codex holds.

## 11. Complete Debug/ASan+UBSan suite + the Windows build fix — @Codex `d7daabf9` / `66ccc9cb`

@Codex `d7daabf9`: the §10 hand-off reported only the Debug **fast** suite; the full Debug/ASan
authorization was still owed. @Codex `66ccc9cb` then found the remote Windows job red and authorized a
narrow in-scope build fix. Both are closed here. **Pushed head for this section: `6cf79ef`.**

### 11.1 Debug + ASan + UBSan, complete suite — actual counts

`ctest --test-dir build-debug --output-on-failure` with **no label filter**, env
`ASAN_OPTIONS=detect_leaks=0` (AppleClang ships no LSan) and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`:

| Item | Result |
|---|---|
| Whole Debug suite | **100% tests passed out of 77** (rc=0) |
| Total wall time | **10509.74 s** (≈2 h 55 m) |
| `slow` label | 10399.69 s across **7** tests, all Passed |
| `#65 test_d3_divider_restore` | Passed 86.02 s |
| `#66 gh19_alias_probe` | Passed 180.02 s |
| `#67 gh19_blamp_acceptance` | Passed 248.19 s |
| `#69 gh20_vcf_probe` | Passed 4941.57 s |
| `#70 gh20_vcf_acceptance` | Passed 4937.40 s |
| `#71 gh12_keyboard_owner_probe` | Passed 2.25 s |
| `#72 gh12_keyboard_side_restore_probe` | Passed 4.25 s |
| Remaining 70 fast tests | all Passed |
| Real host / generator checks (inside the 77) | `host_engine_wiring_gate`, `host_script_codec`, `test_host_engine_oracle`, `registry_regen_zero_diff`, `manifest_regen_zero_diff` — all Passed |

Per-configuration counts in one place: **Release = 70 fast + 7 slow**; **Debug+ASan+UBSan = 77/77**
(70 fast + 7 slow). No figure is extrapolated from a subset; the slow segment was run to completion.

### 11.2 The Windows build fix (`6cf79ef`, one test line)

Remote run `34307653815` failed the `windows` job with
`gh12_keyboard_side_restore_probe.cpp(1133,18): error C2220` /
`warning C4244: 'argument': conversion from 'int' to 'uint8_t'` — MSVC escalates C4244 to an error
under `/WX`. Fix, exactly as authorized (narrow, value-preserving, `/WX` untouched):

```cpp
-  set_mode(st, mode);
+  set_mode(st, static_cast<std::uint8_t>(mode));
```

`mode` is the loop variable of `for (int mode = 0; mode <= 2; ++mode)` in `accept_repeat_and_reject()`;
the value domain is 0..2, so the cast changes no behaviour. No other file, no warning-level change, no
product code. Re-verification at the fixed source: Release rebuild **rc=0, 0 warnings**; affected
directed set **14/14 passed**; mutation harness **rc=0** (baseline **151/151** + **11/11** RED, each rc
exactly 1 with the pinned `[FAIL]` line and the summary last); `build-debug` rebuild rc=0 and the two
keyboard probes re-run **2/2 Passed** (`#71` 2.24 s, `#72` **4.69 s**, 151 checks / 0 failures).

### 11.3 Which results correspond to a source-equivalent version

The 77/77 Debug run was executed on the **pre-cast** binary (source of `7ed9d95`). The only difference
in `6cf79ef` is the single `int→uint8_t` cast above, inside the probe; the product tree `core/` and the
rest of the probe are byte-identical, so the 77/77 result is **source-equivalent for product
behaviour**. The affected probes were re-run at the exact pushed head (`#71`, `#72` → 2/2 Passed) per
@Codex's "不重做无关慢测". Release fast/slow were not re-run (already passed, and the cast cannot reach
them beyond the same single probe line).

### 11.4 Not claimed (unchanged)

No merge, no GH#12 closure, no release, no MET, and no 35/35 coverage claim (§10.2's 12 gaps stay
listed separately; the gate is untouched).

**Exact-head CI, terminal (head `597b335`, the pushed head of this section):**

| Run / job | Result |
|---|---|
| PR run `34319567762` — `ubuntu-latest (g++)` / `ubuntu-latest (clang++)` / `macos-latest (clang++)` / **`windows-latest (cl)`** | all **success** (the C2220 above is gone) |
| PR run `34319567762` — `probe gate (slow, PR-only, ubuntu)` | **7/7 passed**, 3304.65 s (`#65` 24.93 / `#66` 53.35 / `#67` 194.97 / `#69` 1516.13 / `#70` 1513.15 / `#71` 0.70 / `#72` 1.21) |
| PR run `34319567762` — `full coverage (--require-full)` | **failure, by design** — the only failing step is `Run registry completeness gate with --require-full`, i.e. §10.2's 12 gaps |
| Push run `34319564136` | four platforms **success**; `full-coverage` and `probe-gate` skipped on push, by design |

The merge gate stays with @Codex.
