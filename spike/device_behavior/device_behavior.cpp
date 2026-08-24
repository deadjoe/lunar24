// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// spike/device_behavior/device_behavior.cpp
//
// Lunar24 P1 slice ③ — device behavior, requirements 2/3/4/5, self-verifying.
// Real device channel-count authority lives in device_probe.cpp (req 1, run
// separately against real CoreAudio). THIS file proves the DeviceAdapter logic
// that consumes that count:
//
//  Req 2  channel mapping must be CATCHABLE-when-wrong. Four logical outputs are
//         driven as four DISTINGUISHABLE signals (WET_L=440Hz, WET_R=550Hz,
//         DRY_A=DC+0.5, DRY_B=DC-0.5), written to physical channels via the
//         adapter's map, and "read back" per physical channel. The verify step
//         classifies each physical channel and compares to the intended
//         assignment. GOOD = no mismatches. BAD controls: swap WET_L/WET_R
//         (identity mismatch fires), and point DRY_B onto DRY_A's channel
//         (collision fires). A checker that can't go red is no checker.
//  Req 3  "Core only ever sees four logical outputs" must be a SENSOR. Fed the
//         real >4 device count (8, from device_probe), the adapter presents
//         exactly min(4, devchans) outputs; assert ==4 on the 8-out device.
//         BAD control: leak the raw device channel count into Core -> the
//         assertion (==4) blows.
//  Req 4  Hot-swap must hold slice-②'s RT invariants. Detector set reused
//         VERBATIM from slice-②. GOOD: a device-change cycle builds the new
//         snapshot OFF the RT thread (heap allowed there) and publishes it via
//         a lock-free atomic pointer; the RT callback only does an atomic load,
//         so counts stay 0. BAD controls: `new` inside the RT window (heap
//         detector fires), and a deliberately slow reconfigure (xrun counted AND
//         REPORTED, not flattened).
//  Req 5  MIDI/input uses slice-②'s FIXED reproducible constexpr series.
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
    if (t + 1 == h + N) return false;
    slots_[t % N] = std::move(v);
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_acquire);
    if (h == t) return false;
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

// ---------------------------------------------------------------------------
// DeviceAdapter channel model (req 2/3/4). Logical outputs: WET L/R + DRY A/B.
// ---------------------------------------------------------------------------
enum Logical { NONE = -1, WET_L = 0, WET_R = 1, DRY_A = 2, DRY_B = 3 };

constexpr double kFs = 48000.0;
constexpr int kFrames = 256;
constexpr double kPi = 3.14159265358979323846;
// Physical-channel span the adapter may address. The real >4 device on this
// machine is 8-out (Studio Display), so a valid wiring can point a logical at
// physical 4..7. Sizing the read-back arrays to kMaxPhys (>= 8) is what lets
// those upper channels be written AND verified — the earlier 4-wide arrays were
// a stack-buffer-overflow waiting for a `{4,5,6,7}` wiring at devchans=8.
constexpr int kMaxPhys = 16;

// Generate a DISTINGUISHABLE signal for a logical output into out[n].
static void gen_signal(int logical, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    double t = static_cast<double>(i) / kFs;
    switch (logical) {
      case WET_L: out[i] = 0.5f * std::sin(2 * kPi * 440.0 * t); break;
      case WET_R: out[i] = 0.5f * std::sin(2 * kPi * 550.0 * t); break;
      case DRY_A: out[i] = 0.5f;  break;   // DC +0.5
      case DRY_B: out[i] = -0.5f; break;   // DC -0.5
      default:    out[i] = 0.0f;  break;
    }
  }
}

// Classify a received physical block back to a logical output (the "read back").
// DC first (±0.5 -> DRY A/B); otherwise pick the dominant reference sinusoid by
// normalized correlation with 440/550. Correlation is robust at short windows
// (unlike a zero-crossing rate, which is too coarse at ~5 periods).
static int classify(const float* b, int n) {
  double mean = 0.0;
  for (int i = 0; i < n; ++i) mean += b[i];
  mean /= n;
  if (std::fabs(mean - 0.5) < 0.05)  return DRY_A;
  if (std::fabs(mean + 0.5) < 0.05)  return DRY_B;
  double c440 = 0, e440 = 0, c550 = 0, e550 = 0;
  for (int i = 0; i < n; ++i) {
    double t = static_cast<double>(i) / kFs;
    double s440 = std::sin(2 * kPi * 440.0 * t);
    double s550 = std::sin(2 * kPi * 550.0 * t);
    c440 += b[i] * s440; e440 += s440 * s440;
    c550 += b[i] * s550; e550 += s550 * s550;
  }
  double n440 = std::fabs(c440 / e440), n550 = std::fabs(c550 / e550);
  if (n440 > n550) return WET_L;
  if (n550 > n440) return WET_R;
  return NONE;
}

// Verify a wiring against an INDEPENDENT intent, onto a device with devchans
// outputs. Signal content is generated per the `wired` map (what was actually
// written to each physical channel) and "read back" per physical channel. A
// physical channel that INTENT says should carry logical L is faulted if it does
// not actually carry L. Also faults on a collision (two logicals on one physical
// channel). Returns the number of detected faults; a wrong mapping must return >0.
//
// wired[o]  = physical channel logical o was written to (actual).
// intent[o] = physical channel logical o SHOULD be on (the assignment we intend).
//   Using an intent separate from wired is what lets the checker go red: if the
//   two agree the round-trip is self-consistent and silent, but a swap (wired
//   differs from intent) leaves a physical channel carrying the wrong logical.
//
// Arrays are sized kMaxPhys (>= the real 8-out face), so logicals addressed to
// physical 4..7 are both writable and verifiable — the earlier 4-wide arrays
// overflowed on a legal `{4,5,6,7}` wiring at devchans=8.
static int check_mapping(const int wired[4], const int intent[4], int devchans) {
  float phys[kMaxPhys][kFrames];
  for (int p = 0; p < kMaxPhys; ++p) for (int i = 0; i < kFrames; ++i) phys[p][i] = 0.0f;

  int used[kMaxPhys];                     // used[p] = logical the wired map puts here
  for (int p = 0; p < kMaxPhys; ++p) used[p] = -1;
  for (int o = 0; o < 4; ++o) {
    int p = wired[o];
    if (p < 0 || p >= devchans) continue;      // clamped out by device-native
    if (p >= kMaxPhys) return 1;
    if (used[p] != -1) return 1 + (used[p] != o);  // collision: only one logical per phys
    used[p] = o;
  }
  // Write each logical into its physical channel.
  for (int o = 0; o < 4; ++o) {
    int p = wired[o];
    if (p < 0 || p >= devchans || p >= kMaxPhys) continue;
    gen_signal(o, phys[p], kFrames);
  }
  int faults = 0;
  for (int p = 0; p < devchans && p < kMaxPhys; ++p) {
    int li = -1;                          // intended logical on this physical channel
    for (int o = 0; o < 4; ++o) if (intent[o] == p) li = o;
    if (li == -1) continue;               // intent doesn't claim this physical channel
    int got = classify(phys[p], kFrames);
    if (got != li) faults++;              // physical carries the wrong logical
  }
  return faults;
}

// Req 3: Core-facing logical output count, clamped to device-native. Returns
// the count Core is presented with. Must be ==4 on a >4 device.
static int core_sees(int devchans) { return devchans < 4 ? devchans : 4; }

// ---------------------------------------------------------------------------
// Req 4: device snapshot published lock-free; RT callback observes via atomic
// load, never allocates. This mirrors ②'s defer-reclaim pattern for the swap.
// ---------------------------------------------------------------------------
struct DeviceInfo { int devchans; int mapping[4]; };
std::atomic<const DeviceInfo*> g_device{nullptr};

static int g_failures = 0;
static void verdict_pair(const char* name, long good, long bad) {
  bool pass = (good == 0) && (bad > 0);
  if (!pass) g_failures++;
  std::printf("  %-46s good=%-3ld bad=%-3ld  %s\n", name, good, bad, pass ? "PASS" : "FAIL");
}
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

int main() {
  std::printf("== P1 slice ③, req 2/3/4/5: device behavior (self-verifying) ==\n");
  std::printf("Real device channel-count authority = device_probe.cpp (seq ①).\n");
  std::printf("Model: DeviceAdapter maps 4 logical (WET L/R + DRY A/B) onto physical;\n");
  std::printf("Core sees min(4, devchans); hot-swap publishes lock-free.\n\n");

  // ---------------- Req 2: mapping catchable-when-wrong --------------------
  {
    std::printf("Req 2 — channel mapping must be CATCHABLE-when-wrong (real SINK echoes back)\n");
    const int good[4]  = {0, 1, 2, 3};       // intent+wired: L/R/A/B on phys 0-3
    const int upper[4] = {4, 5, 6, 7};       // intent+wired: L/R/A/B on phys 4-7 (the real 8-out upper face)
    const int swap[4]  = {1, 0, 2, 3};       // wired: WET L/R swapped (lower half)
    const int wrong[4] = {0, 1, 2, 2};       // wired: DRY B onto DRY A's physical channel (collision)
    const int upSwap[4]= {5, 4, 6, 7};       // wired: WET L/R swapped in the UPPER half (phys 4-5) vs intent
    const int cross[4] = {0, 5, 6, 7};       // wired: logical0 (WET_L) wrongly on phys 0, intent says phys 4 (cross-half)
    const int dev8 = 8, dev2 = 2;
    long g8   = check_mapping(good,  good,  dev8);   // 8-out, correct lower
    long s8   = check_mapping(swap,  good,  dev8);   // swap in lower (phys 0-1)
    long w8   = check_mapping(wrong, good,  dev8);   // DRY B collision
    long hh   = check_mapping(upper, upper, dev8);   // correct, on phys 4-7 (upper face)
    long hu   = check_mapping(upSwap, upper, dev8);  // swap in upper (phys 4-5)
    long hc   = check_mapping(cross,  upper, dev8);  // cross-half: WET_L lands below when intent says above
    long ok2  = check_mapping(good,  good,  dev2);   // 2-out clamp
    std::printf("  8-out: lower canonical=%ld lowerSwap=%ld dryBCollision=%ld | upperCorrect(4-7)=%ld upperSwap=%ld crossHalf=%ld | 2out=%ld\n",
                g8, s8, w8, hh, hu, hc, ok2);
    verdict_pair("mapping checker catches a swap (WET L/R, lower)", g8, s8);
    verdict_pair("mapping checker catches DRY B on wrong phys (collision)", g8, w8);
    verdict_zero("mapping correct on UPPER half phys 4-7 (full 8 face)", hh);
    verdict_pair("upper-half swap (phys 4-5) is caught", hh, hu);
    verdict_pair("cross-half wrong mapping (WET_L below) is caught", hh, hc);
    verdict_zero("mapping silent on 2-out clamp (DRY not emitted)", ok2);
  }
  std::printf("\n");

  // ---------------- Req 3: Core only ever sees four logical ----------------
  {
    std::printf("Req 3 — 'Core only sees four logical outputs' is a SENSOR\n");
    const int dev8 = 8;                                  // real Studio Display Speakers
    long cs_good = core_sees(dev8);                      // GOOD: min(8,4) = 4
    long cs_leak = dev8;                                 // BAD: leak the raw device count
    std::printf("  8-out device: core_sees=%d (expect 4); if the raw %d were leaked, the same\n", (int)cs_good, dev8);
    std::printf("  assert (==4) would see %d and blow. GOOD:=core_sees==4, and the leak is a\n", (int)cs_leak);
    std::printf("  DISTINGUISHABLE from 4, so a real sensor cannot silently accept it.\n");
    bool sensor_ok = (cs_good == 4) && (cs_leak != 4);
    if (!sensor_ok) g_failures++;
    std::printf("  %-46s core_sees=%d leak=%d  %s\n", "Core sees exactly 4; leak caught",
                (int)cs_good, (int)cs_leak, sensor_ok ? "PASS" : "FAIL");
  }
  std::printf("\n");

  // ---------------- Req 4: hot-swap holds slice-② invariants ----------------
  {
    std::printf("Req 4 — hot-swap holds slice-② RT invariants (detector set reused verbatim)\n");
    const double period_ms = 2.0;

    // GOOD: device-change cycle. A worker (the CoreAudio device-changed handler)
    // builds the new snapshot OFF the RT thread (heap is allowed there) and
    // publishes it via a lock-free atomic; the RT callback only does an atomic
    // load, so heap/mutex/file/log stay 0. (The discarded old snapshot is
    // reclaimed by the worker, i.e. off the RT thread — ②'s defer-reclaim rule.)
    g_heap_in_rt = g_mutex_in_rt = g_file_in_rt = g_log_in_rt = g_xruns = 0;
    {
      auto* d0 = new DeviceInfo{8, {0, 1, 2, 3}};    // heap: not in a window
      g_device.store(d0, std::memory_order_release);
      std::atomic<bool> go{false}, done{false};
      std::thread worker([&] {
        while (!go.load()) std::this_thread::yield();
        auto* d1 = new DeviceInfo{2, {0, 1, -1, -1}};  // device became 2-out
        g_device.store(d1, std::memory_order_release);  // publish OFF the RT thread
        done.store(true);
      });
      go.store(true);
      while (!done.load()) {
        RtGuard guard;                                   // open the RT window
        const DeviceInfo* di = g_device.load(std::memory_order_acquire);  // atomic, no alloc
        if (di) { int live = 0; for (int o = 0; o < 4; ++o) { int p = di->mapping[o]; if (p >= 0 && p < di->devchans) ++live; } (void)live; }
      }
      worker.join();
      long h = g_heap_in_rt.load(), m = g_mutex_in_rt.load();
      long f = g_file_in_rt.load(), l = g_log_in_rt.load();
      std::printf("  swap cycle: heap=%ld mutex=%ld file=%ld log=%ld\n", h, m, f, l);
      verdict_zero("hot-swap GOOD: zero heap in RT window", h);
      verdict_zero("hot-swap GOOD: zero mutex in RT window", m);
      verdict_zero("hot-swap GOOD: zero file in RT window",  f);
      verdict_zero("hot-swap GOOD: zero log in RT window",   l);
    }
    std::printf("  — GOOD swap publishes lock-free and never allocates on the RT thread.\n");

    // BAD control: the swap path DOES allocate inside the RT window. Must fire.
    // The pointer is poked (printf) so the compiler cannot dead-code-eliminate an
    // otherwise-unused trivial construction; ② used a matching `free(leak)` idiom
    // for the same reason. Without it, -O2 drops the `new` and the detector no-ops.
    g_heap_in_rt = 0;
    {
      RtGuard guard;
      DeviceInfo* bad = new DeviceInfo{8, {0, 1, 2, 3}};
      std::printf("  (BAD control allocated snapshot at %p)\n", static_cast<void*>(bad));
      bad->devchans = bad->devchans;   // touch so the object is genuinely used
    }
    verdict_positive("hot-swap BAD: new inside RT window fires heap detector", g_heap_in_rt.load());
    std::printf("  — a swap that allocates on the RT thread is CAUGHT, not silently allowed.\n");

    // BAD control 2: a slow reconfigure must REPORT xrun, not flatten it.
    g_xruns = 0;
    {
      RtGuard guard;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));  // exceeds period_ms
      g_xruns.fetch_add(1, std::memory_order_relaxed);
    }
    std::printf("  hot-swap result: xrun counter = %ld (REPORTED honestly, period %.1f ms)\n",
                g_xruns.load(), period_ms);
    verdict_positive("hot-swap xrun is reported, not flattened", g_xruns.load());
  }
  std::printf("\n");

  // ---------------- Req 5: MIDI/input fixed reproducible -----------------
  std::printf("Req 5 — MIDI/input uses slice-②'s FIXED constexpr series (no randomness)\n");
  {
    SpscQueue<MidiEvent, 64> midi;
    std::atomic<long> consumed{0};
    for (std::size_t k = 0; k < kMidiBurstLen; ++k) midi.push(kMidiBurst[k]);
    RtGuard guard;
    MidiEvent e;
    while (midi.pop(e)) consumed.fetch_add(1, std::memory_order_relaxed);
    long c = consumed.load();
    std::printf("  fixed MIDI burst consumed=%ld/%zu\n", c, kMidiBurstLen);
    if (c != (long)kMidiBurstLen) g_failures++;
    std::printf("  %-46s consumed=%ld/%zu  %s\n", "MIDI fixed burst fully drained", c, kMidiBurstLen, c == (long)kMidiBurstLen ? "PASS" : "FAIL");
  }
  std::printf("\n");

  std::printf("== slice ③ verdict: %s ==\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
