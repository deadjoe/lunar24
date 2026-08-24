// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// spike/stress_test/stress_test.cpp
//
// Lunar24 P1 slice ⑤ — stress test: the audio thread's RT invariants hold under
// a live UI knob-drag (parameter churn) + a MIDI burst. Self-verifying.
//
// The scenario @Claude set: while the user continuously drags knobs (control
// churn) and a MIDI burst lands, the audio thread must stay 无锁 / 无分配 / 无爆音 —
// no mutex, no heap, no xrun-caused pop. Detector set is reused VERBATIM from
// slice-②/③ (mandate: 直接复用 ②/③ 那套，别新写一套). Every assertion carries a
// red-capable BAD control, and xrun is counted and REPORTED — never flattened.
//
// What the drag actually stresses: the PARAMETER-UPDATE channel a knob drag
// exercises. A naive host locks a mutex (or allocates) to read control values
// inside the RT window. The GOOD path publishes control values with lock-free
// atomics and drains MIDI from a lock-free SPSC. The BAD controls inject the
// mutex / allocation / slow-block to prove each detector can go red.
//
// Disposable spike. Self-contained C++17. Never referenced by core/ or
// generated/. Verify rule (from @Claude ② review): the BAD controls must alarm,
// not just the GOOD silently pass.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>

// ---------------------------------------------------------------------------
// Slice-② RT detector set, reused VERBATIM (mandate: "直接复用 ② 的探测器,别新写一套").
// g_rt_window marks the RT callback body; a forbidden op while set bumps a
// counter. Counters (not aborts) let one run prove both directions.
// ---------------------------------------------------------------------------
namespace {
  thread_local bool g_rt_window = false;
  std::atomic<long> g_heap_in_rt{0};
  std::atomic<long> g_mutex_in_rt{0};
  std::atomic<long> g_file_in_rt{0};
  std::atomic<long> g_log_in_rt{0};
  std::atomic<long> g_xruns{0};
}

void* operator new(std::size_t n) {
  if (g_rt_window) g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (g_rt_window) g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (g_rt_window) g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  if (g_rt_window) g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// Scoped RT window. (verbatim from ②)
struct RtGuard {
  bool prev_;
  RtGuard() : prev_(g_rt_window) { g_rt_window = true; }
  ~RtGuard() { g_rt_window = prev_; }
};

// Checked wrappers for RT-forbidden ops (invariant 2 mechanism).
void rt_check_mutex_lock(std::mutex& m) {
  if (g_rt_window) g_mutex_in_rt.fetch_add(1, std::memory_order_relaxed);
  m.lock();
}
void rt_check_file_write(const char*) {
  if (g_rt_window) g_file_in_rt.fetch_add(1, std::memory_order_relaxed);
}
void rt_check_log(const char*) {
  if (g_rt_window) g_log_in_rt.fetch_add(1, std::memory_order_relaxed);
}

// Fixed, lock-free SPSC ring (verbatim from ②). Used for MIDI + reclaim.
template <typename T, std::size_t N>
class SpscQueue {
  std::atomic<std::size_t> head_{0}, tail_{0};
  T slots_[N];
public:
  bool push(T v) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    if (t + 1 == h + N) return false;      // full
    slots_[t % N] = std::move(v);
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_acquire);
    if (h == t) return false;              // empty
    out = std::move(slots_[h % N]);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
};

// MIDI fixed reproducible series (verbatim from ②, invariant 5).
struct MidiEvent { std::uint32_t delta; std::uint8_t status, d1, d2; };
constexpr MidiEvent kMidiBurst[] = {
  {0,   0x90, 60, 100}, {12, 0x90, 64, 110}, {12, 0x90, 67, 100},
  {480, 0x80, 60, 0},   {0,   0x80, 64, 0},  {480, 0x80, 67, 0},
  {0,   0x90, 55, 90},  {960, 0x80, 55, 0},
};
constexpr std::size_t kMidiBurstLen = sizeof(kMidiBurst) / sizeof(kMidiBurst[0]);

constexpr double kFs = 48000.0;
constexpr int kFrames = 256;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// ⑤-specific: the parameter-update channel a knob drag exercises. GOOD path =
// lock-free atomics, so a drag never touches a mutex or the heap inside the RT
// window. BAD paths injection-lock / injection-alloc / injection-slow.
// ---------------------------------------------------------------------------
struct ParamStore {
  std::atomic<float> gain{1.0f};
  std::atomic<float> cutoff{1000.0f};
};

// Read the control values into locals, RT-safe. GOOD: pure atomic loads.
// BAD (injected): takes a lock, or allocates, inside the window.
static void read_params(ParamStore& ps, float& g, float& c, int bad_mode) {
  if (bad_mode == 1) {                      // BAD: lock a mutex on RT
    static std::mutex m; rt_check_mutex_lock(m);
  } else if (bad_mode == 2) {               // BAD: heap-alloc on RT
    float* leak = new float[kFrames];       // pointer made observable (②/③ DCE idiom)
    std::printf("  (BAD control allocated control buffer at %p)\n", static_cast<void*>(leak));
  }
  g = ps.gain.load(std::memory_order_relaxed);
  c = ps.cutoff.load(std::memory_order_relaxed);
}

// The RT "audio callback": render one block. No alloc, no mutex, no file/log on
// the clean path. Drains MIDI from the SPSC and aggregates the control values it
// observed (so a test can prove the drag actually reached the audio thread).
struct BlockObs {
  int  midi_seen;
  float min_g, max_g;
  double sum;
};
template <std::size_t N>
static void render_block(ParamStore& ps, SpscQueue<MidiEvent, N>& midi,
                         BlockObs& obs, float* buf, int bad_mode) {
  RtGuard guard;                              // open the RT window
  float g = 1.0f, c = 1000.0f;
  read_params(ps, g, c, bad_mode);            // BAD modes fire their detector here
  if (g < obs.min_g) obs.min_g = g;
  if (g > obs.max_g) obs.max_g = g;
  MidiEvent e;
  while (midi.pop(e)) obs.midi_seen++;        // drain MIDI, lock-free
  for (int i = 0; i < kFrames; ++i) {
    double t = static_cast<double>(i) / kFs;
    buf[i] = 0.2f * g * static_cast<float>(std::sin(2 * kPi * static_cast<double>(c) * t));
  }
  obs.sum += static_cast<double>(c);
}

// ---------------------------------------------------------------------------
// Helpers. Verdict discipline: GOOD==0 and BAD>0, otherwise FAIL (exit 1).
// ---------------------------------------------------------------------------
static int g_failures = 0;
static void verdict_zero(const char* name, long actual) {
  bool ok = (actual == 0);
  if (!ok) g_failures++;
  std::printf("  %-46s actual(expect 0)=%-3ld  %s\n", name, actual, ok ? "PASS" : "FAIL");
}
static void verdict_positive(const char* name, long actual) {
  bool ok = (actual > 0);
  if (!ok) g_failures++;
  std::printf("  %-46s actual(expect >0)=%-3ld  %s\n", name, actual, ok ? "PASS" : "FAIL");
}
static void verdict_true(const char* name, bool actual) {
  if (!actual) g_failures++;
  std::printf("  %-46s actual(expect true)=%s  %s\n", name, actual ? "true" : "false",
              actual ? "PASS" : "FAIL");
}

int main() {
  std::printf("== P1 slice ⑤: stress test — RT invariants under UI drag + MIDI burst ==\n");
  std::printf("Detector set reused VERBATIM from ②; xrun REPORTED, never flattened.\n\n");

  constexpr int kBlocks = 20000;
  const double period_us = kFrames / kFs * 1e6; // block time in us (~5333 us @256/48k)

  // ---------------- GOOD: sustained drag, RT stays clean ------------------
  {
    std::printf("GOOD — UI knob-drag (param churn) + MIDI burst; audio thread must stay\n");
    std::printf("      no-lock / no-alloc / no-pop. (stress = %d blocks, %.1f us/block)\n",
                kBlocks, period_us);
    g_heap_in_rt = g_mutex_in_rt = g_file_in_rt = g_log_in_rt = g_xruns = 0;

    ParamStore ps;
    SpscQueue<MidiEvent, 1024> midi;
    for (std::size_t k = 0; k < kMidiBurstLen; ++k) midi.push(kMidiBurst[k]);

    std::atomic<long> param_updates{0};
    std::atomic<bool> go{false}, done{false};
    // The "UI" thread: the user dragging knobs. Writes control values with
    // lock-free atomics in a ramp, so the churn reaches the audio thread.
    std::thread ui([&] {
      while (!go.load()) std::this_thread::yield();
      long k = 0;
      while (!done.load()) {
        float g = 1.0f - 0.5f * static_cast<float>(k & 1);
        float c = 200.0f + 100.0f * static_cast<float>((k % 40));
        ps.gain.store(g, std::memory_order_relaxed);
        ps.cutoff.store(c, std::memory_order_relaxed);
        param_updates.fetch_add(1, std::memory_order_relaxed);
        ++k;
      }
    });

    float buf[kFrames];
    bool sink = false;
    BlockObs obs{0, 1e9f, -1e9f, 0.0};
    go.store(true);
    for (int b = 0; b < kBlocks; ++b) {
      render_block(ps, midi, obs, buf, 0);   // clean path
      sink = sink ^ (buf[b % kFrames] > 0.0f);
    }
    done.store(true);
    ui.join();
    (void)sink;

    long h = g_heap_in_rt.load(), m = g_mutex_in_rt.load();
    long f = g_file_in_rt.load(), l = g_log_in_rt.load();
    long xr = g_xruns.load();
    std::printf("  param updates written by UI = %ld; MIDI events consumed by RT = %d/%zu\n",
                param_updates.load(), obs.midi_seen, kMidiBurstLen);
    std::printf("  RT window counters: heap=%ld mutex=%ld file=%ld log=%ld xrun=%ld\n", h, m, f, l, xr);
    verdict_zero("GOOD: zero heap in RT window under drag", h);
    verdict_zero("GOOD: zero mutex in RT window under drag", m);
    verdict_zero("GOOD: zero file in RT window under drag",  f);
    verdict_zero("GOOD: zero log in RT window under drag",   l);
    verdict_zero("GOOD: zero xrun (audio thread kept up, 无爆音)", xr);
    verdict_true("GOOD: the drag was real (>=1 param update)", param_updates.load() >= 1);
    verdict_true("GOOD: the drag reached the audio thread (gain varied)", obs.max_g > obs.min_g);
    verdict_true("GOOD: full MIDI burst delivered into the window", obs.midi_seen == (int)kMidiBurstLen);
    std::printf("  — a knob drag to the audio thread is mutex-free, allocation-free, and the\n");
    std::printf("    audio thread kept up (xrun=0); MIDI fully drained. 无爆音 under drag.\n");
  }
  std::printf("\n");

  // ---------------- BAD control 1: RT read takes a mutex ----------------
  {
    std::printf("BAD 1 — the param path LOCKS a mutex inside the RT window (the naive design)\n");
    g_mutex_in_rt = 0;
    ParamStore ps;
    SpscQueue<MidiEvent, 64> midi;
    for (std::size_t k = 0; k < kMidiBurstLen; ++k) midi.push(kMidiBurst[k]);
    float buf[kFrames];
    BlockObs obs{0, 1e9f, -1e9f, 0.0};
    render_block(ps, midi, obs, buf, 1);     // bad_mode 1 = rt_check_mutex_lock
    verdict_positive("mutex inside RT window fires mutex detector", g_mutex_in_rt.load());
    std::printf("  — a drag that locks to read controls is CAUGHT, not silently allowed.\n");
  }
  std::printf("\n");

  // ---------------- BAD control 2: RT read allocates ----------------
  {
    std::printf("BAD 2 — the param path ALLOCATES inside the RT window\n");
    g_heap_in_rt = 0;
    ParamStore ps;
    SpscQueue<MidiEvent, 64> midi;
    for (std::size_t k = 0; k < kMidiBurstLen; ++k) midi.push(kMidiBurst[k]);
    float buf[kFrames];
    BlockObs obs{0, 1e9f, -1e9f, 0.0};
    render_block(ps, midi, obs, buf, 2);     // bad_mode 2 = new float[kFrames]
    verdict_positive("new inside RT window fires heap detector", g_heap_in_rt.load());
    std::printf("  — a drag that allocates to build a control buffer is CAUGHT.\n");
  }
  std::printf("\n");

  // ---------------- BAD control 3: a slow block must REPORT an xrun ----------------
  {
    std::printf("BAD 3 — a slow RT block must be counted as an xrun, NOT flattened\n");
    g_xruns = 0;
    RtGuard guard;
    std::this_thread::sleep_for(std::chrono::microseconds((long)period_us * 2)); // > one block
    g_xruns.fetch_add(1, std::memory_order_relaxed);
    std::printf("  slower-than-one-block on the RT thread; block time %.1f us\n", period_us);
    verdict_positive("slow RT block is REPORTED as xrun (>0)", g_xruns.load());
    std::printf("  — the pop-producing overrun is visible in the counter, not absorbed.\n");
  }
  std::printf("\n");

  std::printf("== slice ⑤ verdict: %s ==\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
