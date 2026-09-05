# P1 slice ② — audio-thread RT invariants + snapshot reclamation: findings

Disposable spike. Verdict: **PASS** — all six detector assertions are GOOD(0) /
BAD(fires). This measures the P1 audio path, it does not assert "runs fine".

## What it models

The DeviceAdapter audio callback: a real-time thread reads an **immutable
`RenderState`** (the module graph/state snapshot) and renders the **four P1
outputs** (WET L/R + DRY VCO A/B), draining MIDI from a lock-free SPSC ring.
The RT window is a thread-local flag (`g_rt_window`) set for the callback body;
every prohibited operation while it is set bumps a counter. Counters (not
aborts) let one invocation prove both directions: GOOD stays 0, BAD bumps.

## Build + run (reproduce)

```
cd spike/audio_thread_invariants
clang++ -std=c++17 -O2 -pthread -Wall -Wextra audio_thread_invariants.cpp -o audio_thread_invariants
./audio_thread_invariants
```

## Results (exact, from this run)

```
Invariant 1 — zero heap allocation in audio callback
  heap-alloc in RT window                good=0     bad=1      PASS

Invariant 2 — zero mutex / file / log in callback
  mutex in RT window                     good=0     bad=1      PASS
  file-write in RT window                good=0     bad=1      PASS
  log in RT window                       good=0     bad=1      PASS

Invariant 3 — snapshot last release not on RT thread
  release of snapshot (GOOD defer)       actual(expect 0)=0      PASS
      (deferred-reclaim worker drained the last ref OFF the RT thread)
  release of snapshot (BAD sneak-run)    actual(expect >0)=1      PASS
      (guarded detour that dropped the last ref in the RT window while
       g_rt_window was set -> destructor ran on the RT thread -> detector fired)

Invariant 4 — xrun/dropout counted (period 2.0 ms, no listening)
  xrun counter                           good=0     bad=1      PASS

Invariant 5 — MIDI burst fixed + fully drained (len 8)
  MIDI burst fully drained               consumed=8/8  PASS
```

## Per-invariant mechanism

1. **Zero heap in callback** — global `operator new/new[]` override: inside the
   RT window it bumps `g_heap_in_rt`. Intercepts *every* allocation (even one
   buried in a library), which is what makes it "actively catch". BAD control =
   `new int` in the callback.
2. **Zero mutex/file/log** — the render path routes all RT-forbidden operations
   through `rt_check_mutex_lock` / `rt_check_file_write` / `rt_check_log`, each
   of which bumps a counter if the RT window is set. Detector is a real
   mechanism, not a claim. BAD controls = call each once in the callback.
3. **Snapshot last release off the RT thread** — `RenderState::~RenderState`
   bumps `g_release_in_rt` if it runs inside the RT window (the shared_ptr
   "sneak-run"). GOOD = the deferred-reclamation pattern: the RT callback loads
   the current snapshot into a local, the UI publishes a new one (making the
   loaded ref the *last* to the old state), and the RT thread **parks** that ref
   in an SPSC reclaim queue that a worker drains — so the destructor runs off
   the RT thread. BAD = releasing the last reference inside an `RtGuard`.
4. **xrun/dropout counter** — the callback measures its own wall-time against the
   period; a deadline miss increments `g_xruns`. No listening. BAD = a callback
   that sleeps past the period.
5. **MIDI fixed reproducible burst** — a `constexpr` event array; a controller
   posts it and the callback drains it. Replays identically every run by
   construction. Checked that 8/8 are consumed.

## Notes / caveats

- This models the callback, it does not run real hardware I/O. The real audio
  device layer (RtAudio) was already proven separately in slice-①
  (`spike/probes/FINDINGS.md`): enumeration works, openStream fails clean on an
  over-device channel count, DeviceAdapter clamp-by-native is required. ② is the
  RT *behavioural* contract of the callback + snapshot publication, which is
  where the shared_ptr sneak-run would surface.
- The "assert" in the operator-new override is realized as a counter plus a
  final harness check, so a single run proves both the GOOD (0) and BAD (>0)
  directions instead of aborting mid-run.
- The deferred-reclaim GOOD path is the intended P1 pattern: the RT thread must
  never be the thread that drops the last reference to a retired snapshot. The
  detector is the sensor a regression would trip.
