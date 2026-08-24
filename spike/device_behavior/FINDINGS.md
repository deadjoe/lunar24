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
  8-out device: canonical faults=0, wet-swap faults=2, dryB-collision faults=2, 2-out faults=0
  mapping checker catches a swap (WET L/R)       good=0   bad=2    PASS
  mapping checker catches DRY B on wrong phys    good=0   bad=2    PASS
  mapping checker silent on 2-out clamp (DRY not emitted) actual(expect 0)=0    PASS

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

- **Req 2, swap WET L/R** (`map={1,0,2,3}`): physical 0 now carries the 550 Hz
  WET_R (identity break vs. intended WET_L on 0), physical 1 carries the 440 Hz
  WET_L → **2 faults**. Good (canonical) = 0. Checker is real.
- **Req 2, DRY B onto wrong physical channel** (`map={0,1,2,2}`): physical 2 is
  claimed by both DRY_A and DRY_B → collision → **2 faults**. Good = 0.
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
