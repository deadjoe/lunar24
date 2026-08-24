// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// spike/audio_thread_invariants/audio_thread_invariants.cpp
//
// Lunar24 P1 slice ② — prove (measure, not "runs fine") that the P1 audio path
// honors real-time invariants. Models the DeviceAdapter audio callback: an RT
// thread reads an immutable RenderState (the module graph/state snapshot) and
// renders the four P1 outputs (WET L/R + DRY VCO A/B), draining MIDI from a
// lock-free SPSC ring.
//
// Disposable spike. Never referenced by core/ or generated/. Self-contained
// C++17, no external deps. Report exact head + hold.
//
// Every invariant has a GOOD run (counter must stay 0) and a BAD negative
// control (deliberately trip it; counter must bump). A detector that does not
// fire is worse than none — it hands you a green light.
//
//  1. zero heap allocation in the audio callback   (operator new/delete + RT flag)
//  2. zero mutex / file / log in the callback      (checked wrappers + RT flag)
//  3. snapshot last release must NOT land on audio cb (RenderState dtor + RT flag,
//                                                     GOOD = deferred reclamation)
//  4. xrun/dropout counted, not judged by ear      (deadline-miss counter)
//  5. MIDI burst is a fixed reproducible sequence  (constexpr event array)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Detector state. g_rt_window marks the RT callback body; any forbidden op while
// it is set bumps a counter. Counters (not aborts) let ONE run prove both
// directions: GOOD => 0, BAD => >0.
// ---------------------------------------------------------------------------
namespace {
  thread_local bool g_rt_window = false;
  std::atomic<long> g_heap_in_rt{0};     // new/new[] while in RT window
  std::atomic<long> g_mutex_in_rt{0};    // mutex lock while in RT window
  std::atomic<long> g_file_in_rt{0};     // file write while in RT window
  std::atomic<long> g_log_in_rt{0};      // log call while in RT window
  std::atomic<long> g_release_in_rt{0};  // RenderState dtor in RT window (sneak-run)
  std::atomic<long> g_xruns{0};          // deadline-miss count
  std::atomic<long> g_midi_consumed{0};  // events actually drained by the callback
  std::atomic<long> g_cb_count{0};       // callbacks completed
}

// ---------------------------------------------------------------------------
// Invariant 1: global operator new/delete override. Intercepts every
// allocation, so even one buried in a library is caught ("actively catch").
// ---------------------------------------------------------------------------
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

// Scoped RT window.
struct RtGuard {
  bool prev_;
  RtGuard() : prev_(g_rt_window) { g_rt_window = true; }
  ~RtGuard() { g_rt_window = prev_; }
};

// ---------------------------------------------------------------------------
// Checked wrappers for the forbidden operations (invariant 2). The render path
// never calls them; if one is called inside the RT window it is flagged. Routing
// all RT-forbidden ops through these makes the detector a real mechanism, not a
// claim.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// The immutable graph/state snapshot. Its destructor is the sensor for the
// shared_ptr "sneak-run": if the LAST release lands on the RT thread it has to
// destruct the whole graph there — the bug invariant 3 forbids.
// ---------------------------------------------------------------------------
struct RenderState {
  int sample_rate;
  int noutputs;    // 4 -> WET L/R + DRY VCO A/B (the P1 4-out shape)
  double phase;
  RenderState(int sr, int outs) : sample_rate(sr), noutputs(outs), phase(0.0) {}
  ~RenderState() {
    if (g_rt_window) g_release_in_rt.fetch_add(1, std::memory_order_relaxed);
  }
};

// The "current" snapshot the RT callback reads. Swapped by the UI (harness)
// thread when parameters change. Plain shared_ptr; the RT callback takes a copy.
std::shared_ptr<RenderState> g_current_snapshot;

// ---------------------------------------------------------------------------
// Deferred-reclaim SPSC queue (invariant 3 GOOD). Fixed-size and preallocated:
// moving a shared_ptr in/out never allocates, so an RT thread can park a held
// snapshot here instead of letting it destruct in place. A non-RT worker drains
// it, so the final release (and destructor) always runs off the RT thread.
// ---------------------------------------------------------------------------
template <typename T, std::size_t N>
class SpscQueue {
  std::atomic<std::size_t> head_{0}, tail_{0};
  T slots_[N];
public:
  bool push(T v) {                              // by value: const copy or rvalue move
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

// ---------------------------------------------------------------------------
// MIDI burst (invariant 5): a FIXED, reproducible sequence — constexpr array,
// not improvised. A controller posts it; the RT callback drains it. Replays
// identically every run by construction.
// ---------------------------------------------------------------------------
struct MidiEvent { std::uint32_t delta; std::uint8_t status, d1, d2; };
constexpr MidiEvent kMidiBurst[] = {
  {0,   0x90, 60, 100},
  {12,  0x90, 64, 110},
  {12,  0x90, 67, 100},
  {480, 0x80, 60, 0},
  {0,   0x80, 64, 0},
  {480, 0x80, 67, 0},
  {0,   0x90, 55, 90},
  {960, 0x80, 55, 0},
};
constexpr std::size_t kMidiBurstLen = sizeof(kMidiBurst) / sizeof(kMidiBurst[0]);

// ---------------------------------------------------------------------------
// The audio callback. Modes: KGood (honor all invariants) plus one deliberate
// violation each for the negatives. out_held optionally receives the snapshot
// the callback loaded, so a GOOD reclaim can park it off the RT thread.
// ---------------------------------------------------------------------------
enum class Mode { KGood, KBadHeapAlloc, KBadMutex, KBadFileLog, KBadSlow };

double call_audio_cb(Mode m, SpscQueue<MidiEvent, 64>& midi, std::mutex& mu,
                     std::size_t frames, double period_ms,
                     std::shared_ptr<RenderState>* out_held = nullptr) {
  RtGuard guard;                                   // open the RT window
  auto start = std::chrono::steady_clock::now();
  std::shared_ptr<RenderState> held = g_current_snapshot;   // atomic-ish load (copy)
  if (held) {
    (void)held->sample_rate;
    (void)held->noutputs;                          // 4 = WET L/R + DRY A/B
    for (std::size_t i = 0; i < frames; ++i) held->phase += 0.01;  // no heap, no lock
  }
  MidiEvent e;
  while (midi.pop(e)) g_midi_consumed.fetch_add(1, std::memory_order_relaxed);

  switch (m) {
    case Mode::KBadHeapAlloc: { int* leak = new int(7); std::free(leak); break; }
    case Mode::KBadMutex:     { rt_check_mutex_lock(mu); mu.unlock(); break; }
    case Mode::KBadFileLog:   { rt_check_file_write("/tmp/x"); rt_check_log("rt log"); break; }
    case Mode::KBadSlow:      { std::this_thread::sleep_for(std::chrono::milliseconds(5)); break; }
    case Mode::KGood:         break;
  }
  auto end = std::chrono::steady_clock::now();
  double taken = std::chrono::duration<double, std::milli>(end - start).count();
  if (taken > period_ms) g_xruns.fetch_add(1, std::memory_order_relaxed);
  g_cb_count.fetch_add(1, std::memory_order_relaxed);

  if (out_held) *out_held = std::move(held);       // caller decides release (off-RT)
  else held.reset();                               // g_current still holds it -> ref 2->1, no dtor
  return taken;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
static int g_failures = 0;
static void verdict(const char* name, bool good_ok, bool bad_ok, long good, long bad) {
  bool pass = good_ok && bad_ok;
  if (!pass) g_failures++;
  std::printf("  %-38s good=%-5ld bad=%-5ld  %s\n", name, good, bad,
              pass ? "PASS" : "FAIL");
  std::fflush(stdout);
}
static void verdict_zero(const char* name, long actual) {
  bool ok = (actual == 0);
  if (!ok) g_failures++;
  std::printf("  %-38s actual(expect 0)=%-5ld  %s\n", name, actual, ok ? "PASS" : "FAIL");
  std::fflush(stdout);
}
static void verdict_positive(const char* name, long actual) {
  bool ok = (actual > 0);
  if (!ok) g_failures++;
  std::printf("  %-38s actual(expect >0)=%-5ld  %s\n", name, actual, ok ? "PASS" : "FAIL");
  std::fflush(stdout);
}

int main() {
  std::printf("== P1 slice ②: RT invariants + snapshot reclamation (spike) ==\n");
  std::printf("Model: RT callback reads immutable RenderState, renders 4 outputs\n");
  std::printf("(WET L/R + DRY A/B), drains MIDI from an SPSC ring.\n");
  std::printf("Each detector proven GOOD(0) and BAD(fires).\n\n");

  const std::size_t frames = 128;
  const double period_ms = 2.0;   // callback must beat this
  std::mutex mu;

  // ---- Invariant 1: zero heap allocation in callback -------------------
  {
    SpscQueue<MidiEvent, 64> midi;
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);
    call_audio_cb(Mode::KGood, midi, mu, frames, period_ms);
    const long h_good = g_heap_in_rt.load();
    g_heap_in_rt = 0;
    call_audio_cb(Mode::KBadHeapAlloc, midi, mu, frames, period_ms);
    const long h_bad = g_heap_in_rt.load();
    std::printf("Invariant 1 — zero heap allocation in audio callback\n");
    verdict("heap-alloc in RT window", h_good == 0, h_bad > 0, h_good, h_bad);
    g_current_snapshot.reset();
  }
  std::printf("\n");

  // ---- Invariant 2: zero mutex / file / log in callback ---------------
  {
    SpscQueue<MidiEvent, 64> midi;
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);
    call_audio_cb(Mode::KGood, midi, mu, frames, period_ms);
    const long m_good = g_mutex_in_rt.load(), f_good = g_file_in_rt.load(), l_good = g_log_in_rt.load();
    call_audio_cb(Mode::KBadMutex, midi, mu, frames, period_ms);
    const long m_bad = g_mutex_in_rt.load();
    call_audio_cb(Mode::KBadFileLog, midi, mu, frames, period_ms);
    const long f_bad = g_file_in_rt.load(), l_bad = g_log_in_rt.load();
    std::printf("Invariant 2 — zero mutex / file / log in callback\n");
    verdict("mutex in RT window",  m_good == 0, m_bad > m_good, m_good, m_bad - m_good);
    verdict("file-write in RT window", f_good == 0, f_bad > f_good, f_good, f_bad - f_good);
    verdict("log in RT window",    l_good == 0, l_bad > l_good, l_good, l_bad - l_good);
    g_current_snapshot.reset();
  }
  std::printf("\n");

  // ---- Invariant 3: snapshot last release not on RT thread ------------
  {
    SpscQueue<MidiEvent, 64> midi;
    std::printf("Invariant 3 — snapshot last release not on RT thread\n");

    // GOOD: full publish/reclaim cycle. The RT callback loads S0 into `held`;
    // the UI then publishes S1 (so `held` is the ONLY ref to S0); instead of
    // dropping `held` on the RT thread, park it in a reclaim queue that a worker
    // drains (deterministically, after the push) — so the destructor runs off
    // the RT thread.
    g_release_in_rt = 0;
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);            // S0
    std::shared_ptr<RenderState> held;
    call_audio_cb(Mode::KGood, midi, mu, frames, period_ms, &held);          // held=S0
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);            // S1; S0 ref=1 (held)
    SpscQueue<std::shared_ptr<RenderState>, 16> reclaim;
    std::atomic<bool> go{false}, done{false};
    std::thread worker([&] {
      while (!go.load()) std::this_thread::yield();
      std::shared_ptr<RenderState> r;
      while (reclaim.pop(r)) r.reset();      // destruct off the RT thread
      done.store(true);
    });
    reclaim.push(std::move(held));
    go.store(true);
    while (!done.load()) std::this_thread::yield();
    worker.join();
    const long rel_good = g_release_in_rt.load();
    verdict_zero("release of snapshot (GOOD defer)", rel_good);
    std::printf("      (deferred-reclaim worker drained the last ref OFF the RT thread)\n");

    // BAD: the sneak-run. Release the LAST reference inside the RT window.
    g_release_in_rt = 0;
    std::shared_ptr<RenderState> s(new RenderState(48000, 4));   // refcount 1, s sole holder
    {
      RtGuard guard;
      s.reset();                               // last release -> ~RenderState IN the RT window
    }
    const long rel_bad = g_release_in_rt.load();
    verdict_positive("release of snapshot (BAD sneak-run)", rel_bad);
    std::printf("      (guarded detour that dropped the last ref in the RT window while\n");
    std::printf("       g_rt_window was set -> destructor ran on the RT thread -> detector fired)\n");
    g_current_snapshot.reset();
  }
  std::printf("\n");

  // ---- Invariant 4: xrun/dropout counted ---------------------------------
  {
    SpscQueue<MidiEvent, 64> midi;
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);
    g_xruns = 0;
    call_audio_cb(Mode::KGood, midi, mu, frames, period_ms);
    const long x_good = g_xruns.load();
    call_audio_cb(Mode::KBadSlow, midi, mu, frames, period_ms);
    const long x_bad = g_xruns.load();
    std::printf("Invariant 4 — xrun/dropout counted (period %.1f ms, no listening)\n", period_ms);
    verdict("xrun counter", x_good == 0, x_bad > x_good, x_good, x_bad - x_good);
    g_current_snapshot.reset();
  }
  std::printf("\n");

  // ---- Invariant 5: MIDI burst fixed + fully drained ---------------------
  {
    SpscQueue<MidiEvent, 64> midi;
    g_current_snapshot = std::make_shared<RenderState>(48000, 4);
    g_midi_consumed = 0;
    for (std::size_t k = 0; k < kMidiBurstLen; ++k) midi.push(kMidiBurst[k]);
    call_audio_cb(Mode::KGood, midi, mu, frames, period_ms);
    const long mc = g_midi_consumed.load();
    std::printf("Invariant 5 — MIDI burst fixed + fully drained (len %zu)\n", kMidiBurstLen);
    const bool midi_ok = (mc == (long)kMidiBurstLen);
    if (!midi_ok) g_failures++;
    std::printf("  %-38s consumed=%ld/%zu  %s\n", "MIDI burst fully drained",
                mc, kMidiBurstLen, midi_ok ? "PASS" : "FAIL");
    g_current_snapshot.reset();
  }
  std::printf("\n");

  std::printf("== slice ② verdict: %s ==\n",
              g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
