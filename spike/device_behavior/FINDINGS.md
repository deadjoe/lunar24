# P1 slice ③ — device behavior: findings

Disposable spike. Verdict: **PASS** — every requirement is proven GOOD(0) /
BAD(fires), and requirement 1 is measured on **real CoreAudio** devices. Scope is
macOS-only (Windows suspended per bearbone 8-24). Drives the P1 second line
(line 76) of the plan: device behavior.

Two artifacts in this directory:

- `device_probe.cpp` — **REAL** CoreAudio enumeration + hot-unplug listener
  (requirement 1 channel-count authority + requirement 4 detection).
- `device_behavior.cpp` — **self-verifying** DeviceAdapter logic (mapping,
  Core-sees-4 sensor, hot-swap invariant containment, MIDI), reusing slice-②'s
  detector set **verbatim** per the mandate.

## Requirement 1 — authoritative channel count (REAL, measured)

The DeviceAdapter must learn device channel count from the authoritative
CoreAudio property, **not** `system_profiler` + a homebrew filter (that path
leaks — Studio Display is actually 8 out).

Build + run:

```
cd spike/device_behavior
clang++ -std=c++17 -O2 -Wall -Wextra -framework CoreAudio -framework CoreFoundation device_probe.cpp -o device_probe
./device_probe
```

Raw output (this machine, 2026-08-24):

```
#   name                                          OUT       IN OUT(buf)  IN(buf)
85  Studio Display Microphone                       0        1        0        0
80  Studio Display Speakers                         8        0        1        0
74  Mac mini Speakers                               2        0        1        0
```

- **Studio Display Speakers: OUT=8** — the >4 device. `system_profiler` would
  under-report this; `kAudioDevicePropertyStreamConfiguration` gives the true 8.
- **Mac mini Speakers: OUT=2** — the 2-out device (clamp-to-2 case).
- **No native 4-out device exists on this machine.**

Consequence: requirement 1 says "test 4-channel on a real device, don't paper
over with an aggregate." Since there is no native 4-out device, the **4-logical
output path is exercised on the real 8-out device's physical channels 0–3**
(not an aggregate — a real physical output). This is the strictly more demanding
case (clamp 8→4) and is satisfied. **Flagged honestly** for @Claude.

## Requirement 4 detection — REAL hot-unplug listener

`device_probe --watch N` installs a `kAudioHardwarePropertyDevices` listener and
watches N s. This is the detection half of hot-swap; the *containment* is proven
in `device_behavior.cpp`.

Raw output (`--watch 3`): listener installs `OK (noErr=0)`; no device change in
the window (nothing unplugged in the 3 s) — reported honestly, not fabricated.
The callback is the same one the real host installs.

## Requirements 2/3/4(containment)/5 — self-verifying (modeled, reusable detectors)

Build + run:

```
cd spike/device_behavior
clang++ -std=c++17 -O2 -pthread -Wall -Wextra device_behavior.cpp -o device_behavior
./device_behavior
```

Raw output (2026-08-24):

```
Req 2 — channel mapping must be CATCHABLE-when-wrong (real SINK echoes back)
  8-out: lower canonical=0 lowerSwap=2 dryBCollision=2 | upperCorrect(4-7)=0 upperSwap=2 crossHalf=1 | 2out=0
  mapping checker catches a swap (WET L/R, lower) good=0   bad=2    PASS
  mapping checker catches DRY B on wrong phys (collision) good=0   bad=2    PASS
  mapping correct on UPPER half phys 4-7 (full 8 face) actual(expect 0)=0    PASS
  upper-half swap (phys 4-5) is caught           good=0   bad=2    PASS
  cross-half wrong mapping (WET_L below) is caught good=0   bad=1    PASS
  mapping silent on 2-out clamp (DRY not emitted) actual(expect 0)=0    PASS

Req 3 — 'Core only sees four logical outputs' is a SENSOR
  8-out device: core_sees=4 (expect 4); if the raw 8 were leaked, the same
  assert (==4) would see 8 and blow. GOOD:=core_sees==4, and the leak is a
  DISTINGUISHABLE from 4, so a real sensor cannot silently accept it.
  Core sees exactly 4; leak caught               core_sees=4 leak=8  PASS

Req 4 — hot-swap holds slice-② RT invariants (detector set reused verbatim)
  swap cycle: heap=0 mutex=0 file=0 log=0
  hot-swap GOOD: zero heap in RT window          actual(expect 0)=0    PASS
  hot-swap GOOD: zero mutex in RT window         actual(expect 0)=0    PASS
  hot-swap GOOD: zero file in RT window          actual(expect 0)=0    PASS
  hot-swap GOOD: zero log in RT window           actual(expect 0)=0    PASS
  (BAD control allocated snapshot at 0x...)
  hot-swap BAD: new inside RT window fires heap detector actual(expect >0)=1    PASS
  hot-swap xrun is reported, not flattened       actual(expect >0)=1    PASS

Req 5 — MIDI/input uses slice-②'s FIXED constexpr series (no randomness)
  MIDI fixed burst fully drained                 consumed=8/8  PASS

== slice ③ verdict: PASS ==
```

## Negative-control evidence (the mandate's "must catch a wrong mapping" rule)

Each detector is proven to **alarm**, not just silently pass:

- **Req 2, swap WET L/R — lower half** (`wired={1,0,2,3}` vs `intent {0,1,2,3}`):
  physical 0 now carries the 550 Hz WET_R (identity break vs. intended WET_L on
  0), physical 1 carries the 440 Hz WET_L → **2 faults**. Good (canonical) = 0.
  Checker is real.
- **Req 2, DRY B onto wrong physical channel** (`wired={0,1,2,2}`): physical 2 is
  claimed by both DRY_A and DRY_B → collision → **2 faults**. Good = 0.
- **Req 2, correct on the UPPER half** (`wired=intent={4,5,6,7}` on devchans=8):
  L/R/A/B land on physical 4–7 and each verifies correctly → **0 faults**. This
  is the real 8-out face — the pre-fix 4-wide arrays could neither reach nor
  check channels 4–7.
- **Req 2, upper-half swap** (`wired={5,4,6,7}` vs `intent {4,5,6,7}`): physical 4
  carries WET_R when intent says WET_L, physical 5 carries WET_L when intent says
  WET_R → **2 faults**. A mapping error confined to the upper half is caught.
- **Req 2, cross-half wrong mapping** (`wired={0,5,6,7}` vs `intent {4,5,6,7}`):
  logical0 (WET_L) lands on physical 0, but intent places it on physical 4 →
  physical 4 carries silence (none) when it must carry WET_L → **1 fault**. A
  wrong-half substitution is caught.
- **Req 2, 2-out clamp**: WET L/R emitted, DRY A/B clamped out (no physical 2/3);
  0 faults — verifies the device-native clamp leaves DRY off the wire on a 2-out.
- **Req 3, leak device count into Core**: raw 8 leaked in place of `min(4,8)` →
  sensor sees 8, assert `==4` blows. The 8 is distinguishable from 4, so the
  sensor cannot silently accept a leak.
- **Req 4, `new` inside the RT window**: heap detector fires (1). The pointer is
  `printf`'d so -O2 cannot dead-code-eliminate an otherwise-unused trivial
  construction (this is why ② used a matching `free(leak)` idiom — same trap).
- **Req 4, slow reconfigure**: xrun counter = 1, **reported** (not flattened).
- **Req 4 GOOD**: the swap builds the new device snapshot **off** the RT thread
  (worker, heap allowed there) and publishes via a lock-free atomic pointer; the
  RT callback only does an atomic load → heap/mutex/file/log = 0 across the whole
  device-change cycle. This is ②'s deferred-reclaim rule applied to hot-swap.

## ③ tail items closed (2026-08-24, @Claude 09eaf9f7 review)

Two items had to land before ③ could be marked done:

### (a) Stack-buffer-overflow in `check_mapping` — FIXED

@Claude proved with ASan (not by inspection) that the read-back arrays were
**4-wide** but the loop guard was `p < devchans` (=8), so a fully legal wiring
`{4,5,6,7}` at devchans=8 overflowed `want[p]` / `phys[p]`. The pre-fix test set
only ever used physical 0–3 (an in-place permutation + clamp), so the upper four
channels were neither written nor verified — a "detector that never reaches the
cell" variation of the ② `free(leak)` DCE trap.

Fix: arrays sized to `kMaxPhys = 16` (≥ the real 8-out face); the read-back loop
guard is now `p < devchans && p < kMaxPhys` (the old `&& p < 4` truncation is
gone). `check_mapping` now takes an **independent intent** array separate from
the wired map — that separation is what lets the checker go red (if wired==intent
the round-trip is self-consistent and correctly silent, but a swap leaves a
physical channel carrying the wrong logical).

New cases prove the upper face and the cross-half alarms:
`wired=intent={4,5,6,7}` → 0 faults (correct on physical 4–7);
`wired={5,4,6,7}` vs intent → 2 faults (upper-half swap);
`wired={0,5,6,7}` vs intent → 1 fault (cross-half, WET_L lands below when
intent places it on physical 4).

Verification: `clang++ -fsanitize=address,undefined` build of the same source
runs the full suite to exit 0 — **no sanitizer hit** — where the old code aborted
on the `{4,5,6,7}` wiring. All 17 checks still PASS under the non-sanitized build.

### (b) P3-exit debt: real CoreAudio output-stream per-channel verification

- **What is deferred:** REAL (open-stream + write) per-channel verification of
  the CoreAudio **output stream** — i.e. driving an actual `AudioBufferList` on
  the real 8-out device and reading back the per-channel result.
- **Why (the reason this is a debt, not a rounding error):** the current modeled
  read-back uses a **per-channel planar** model (`phys[p][i]`, one array per
  channel). The real device (Studio Display, from `device_probe`) reports
  **8 channels in 1 buffer = interleaved** `AudioBufferList`. A bug in stride /
  interleave math — the most common real-world wiring error — is therefore
  **structurally invisible** to the per-channel model: it can produce a correct
  per-channel classifier yet a wrong interleaved stream. The model touches the
  wrong layer for interleave.
- **When it is repaid:** **before P3 exit — no further.** The P3 exit condition
  reads "the four logical outputs land correctly on physical outputs." Against a
  modeled (planar) sink that sentence cannot be honestly claimed; it is only
  honest once real `AudioBufferList` output-stream per-channel behavior is
  verified on the real 8-out (and 2-out) device. Repaying it is a **P3-blocking**
  task, and this entry is its standing marker.
- Windows equivalent: suspended (bearbone 8-24), same as the rest of P1 — not
  chargeable to a Windows debt specifically.

## How each requirement is met

1. **2/4-channel CoreAudio** — real device counts from
   `kAudioDevicePropertyStreamConfiguration` (authoritative). 4-out path
   exercised on the real 8-out device's physical 0–3; clamp-by-native is
   mandatory (① already proved over-device openStream fails clean). Windows
   suspended — not touched.
2. **Mapping catchable** — four distinguishable signals (440 Hz / 550 Hz /
   DC +0.5 / DC −0.5) driven to physical channels; per-channel read-back
   classifies and compares to the intended assignment. Swap + wrong-channel
   negatives both go red.
3. **Core always sees four logical** — the Core-facing adapter presents
   `min(4, devchans)`. On the real 8-out, that is exactly 4 (sensor), and a
   leaked 8 is caught. The clamp is a live assertion, not a "reads-like" comment.
4. **Hot-swap** — detection via the real `kAudioHardwarePropertyDevices`
   listener; containment via slice-②'s detector set (heap/mutex/file/log must
   stay 0 through the swap window, xrun reported honestly). GOOD path
   lock-free, negative `new` in window fires.
5. **MIDI/input** — slice-②'s fixed `constexpr` series reused (no randomness);
   8/8 drained.

## Real-vs-modeled boundary (honest)

- **Real**: device enumeration + channel counts (req 1); hot-unplug listener
  install + arm (req 4 detection).
- **Modeled**: the per-channel audio "read-back" (req 2) and the CoreAudio
  streaming swap (req 4 containment). Real analog read-back of the Studio
  Display speakers would need a 4+ channel capture interface, which this machine
  does not have; the mapping round-trip is therefore verified at the
  DeviceAdapter→sink boundary (the layer where a wrong mapping is detectable),
  driven by the **real** 8-out / 2-out counts. Opening a real CoreAudio output
  stream + writing audio is the P1 device-layer integration; ① already proved
  `openStream` works on the real drivers. The mandate's two scope notes — no
  native 4-out device, and no capture device for physical read-back — are both
  flagged here rather than silently substituted.

## Notes

- Reuses slice-②'s detector set **verbatim** (operator-new override, `RtGuard`,
  checked wrappers, `SpscQueue`, constexpr MIDI burst). The `operator new`
  override intercepts *every* allocation (even buried in a library) — the
  property @Claude's ② mutation test (`std::vector` in the GOOD path → 0→1)
  confirmed carries over to ③ unchanged.
- Not referenced by `core/` or `generated/`; not wired into any build. Cleanup:
  files are `spike/-scoped`, binaries gitignored.
