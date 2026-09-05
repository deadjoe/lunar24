# P1 slice ⑤ — stress test: RT invariants under UI drag + MIDI burst

Disposable spike. Verdict: **PASS** — the audio thread stays 无锁 / 无分配 / 无爆音
under a live knob-drag (parameter churn) plus a MIDI burst, and each detector
can be driven red by a BAD control. Runs self-verifying, exits 0. Scope is
macOS-only (Windows suspended per bearbone 8-24). Drives the P1 audio-thread +
UI threading line. Follows [[lunar24-p1-slices]] ②/③.

One artifact here:

- `stress_test.cpp` — **self-verifying harness**. Detector set reused **VERBATIM**
  from slice-②/③ (`operator new` override, `RtGuard`, `rt_check_*`, `SpscQueue`,
  `kMidiBurst`). It never touches `core/` or `generated/`.
- `FINDINGS.md` — this file.

## What the drag actually stresses

The **parameter-update channel** a knob drag exercises. The host that drives the
Solar 42N UI is the one we author (per ①b architecture), so the question is
whether reading control values on the audio thread is RT-safe while the user is
continuously dragging. A naive design locks a mutex (or allocates) to read the
control store inside the callback; the clean design publishes each control value
lock-free and reads it with an atomic load.

- **GOOD path**: control values live in lock-free `std::atomic<float>` slots
  (`ParamStore`). A "UI" thread writes them in a ramp (the drag); the audio
  thread reads them with relaxed atomic loads and drains MIDI from a lock-free
  SPSC. No mutex, no heap on the RT thread.
- **BAD roots** (each injected individually): read takes a mutex on RT; read
  allocates on RT; a block runs slower than one period. Each must go red.

## Build + run

```
cd spike/stress_test
clang++ -std=c++17 -O2 -Wall -Wextra stress_test.cpp -o stress_test
./stress_test
```

Should print `== slice ⑤ verdict: PASS ==` and exit 0.

## Coverage (this machine, 2026-08-25)

```
GOOD — UI knob-drag (param churn) + MIDI burst; audio thread must stay
      no-lock / no-alloc / no-pop. (stress = 20000 blocks, 5333.3 us/block)
  param updates written by UI = 5791; MIDI events consumed by RT = 8/8
  RT window counters: heap=0 mutex=0 file=0 log=0 xrun=0
  GOOD: zero heap in RT window under drag        PASS
  GOOD: zero mutex in RT window under drag       PASS
  GOOD: zero file in RT window under drag        PASS
  GOOD: zero log in RT window under drag         PASS
  GOOD: zero xrun (audio thread kept up, 无爆音) PASS
  GOOD: the drag was real (>=1 param update)     PASS
  GOOD: the drag reached the audio thread (gain varied) PASS
  GOOD: full MIDI burst delivered into the window PASS

BAD 1 — the param path LOCKS a mutex inside the RT window (the naive design)
  mutex inside RT window fires mutex detector    PASS
BAD 2 — the param path ALLOCATES inside the RT window
  new inside RT window fires heap detector       PASS
BAD 3 — a slow RT block must be counted as an xrun, NOT flattened
  slow RT block is REPORTED as xrun (>0)         PASS
```

### GOOD — a drag that reaches the audio thread without violating RT

- **heap/mutex/file/log in RT window = 0**. The drag writes atomic slots; the
  audio thread reads them and drains the MIDI SPSC with no lock, no allocation,
  no file/log. (The `g_rt_window` flag is `thread_local`, so only the audio
  thread's ops are counted — the UI thread's own allocations are irrelevant.)
- **xrun = 0 (无爆音)**. The audio thread kept up: a tight but light block never
  exceeds its period, so the counter stays honest at 0, not flattened.
- **The drag was real & reached the audio thread**: 5791 parameter updates, and
  the `gain` value the audio thread observed actually varied (min != max) — the
  knob movement genuinely got into the render, proving the lock-free channel
  works under churn, not that it was idle.
- **MIDI 8/8 consumed** in the window.

### BAD controls (each can go red — the mandate's "every BAD control must ALARM")

- **BAD 1** — read takes `rt_check_mutex_lock` on RT: **mutex detector fires (1)**, PASS.
- **BAD 2** — read `new float[kFrames]` on RT: **heap detector fires (1)**, PASS
  (the pointer is made observable with a `printf`, the ②/③ DCE idiom — a trivial
  unused `new` would otherwise be elided at `-O2` and the detector would no-op).
- **BAD 3** — a block over one period: **xrun counter REPORTS >0**, PASS. This is
  the "no flattening" rule — the pop-producing overrun is visible in the count,
  not silently absorbed.

## Real-vs-modeled boundary (honest)

- **Real**: the detector set is the same one that ran against real audio threads
  in ②/③; the atomic-param + SPSC channel is code we author. The counters and
  verdicts are measured by an actual concurrent run (one audio thread + one UI
  thread), not a formula.
- **Modeled**: this is a thread/fan-in correctness spike. The actual Studio
  Display / Mac mini device scheduling, and the real CoreAudio or RtAudio
  callback invocation under load, is the P1 device layer (slice ③ + the P3
  real-output-stream debt) — out of scope. In particular, "0 xrun" here means
  our light RT block kept up in this harness, not that a real device can never
  overrun; the xrun *detector and reporting path* are what this proves red/green.

## Notes

- Detector set is copied verbatim from ③ so the mandate's "直接复用 ②/③ 那套，别新写
  一套" is literally honored. Not referenced by `core/` or `generated/`; not wired
  into any build. Cleanup: file is `spike/`-scoped, binary gitignored.
