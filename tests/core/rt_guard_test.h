// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Real-time-guard detector, replicated from the proven P1-② spike
// (spike/audio_thread_invariants). It lives in tests/ (NOT core/) for the same
// reason the spike lives in spike/: the global operator-new override is a
// test-only probe, so core/ + generated/ stay framework- and probe-free and
// `check_spike_clean.py` never scans tests/. This is a single-TU include: include
// it from exactly one test translation unit.
//
// Purpose (P2-③ @Claude addition #8): prove that a graph RECOMPILE never runs on
// the audio thread. The audio thread is modelled by an RtGuard; the global
// operator-new override flags any allocation that happens while it is set. A
// recompile builds vectors/CompiledGraph on the control thread and is therefore
// clean; triggering one inside the RT window must bump g_heap_in_rt.
//
// Counters (not aborts) so one binary proves both directions: a GOOD run leaves
// the counter at 0, a BAD (negative-control) run bumps it.

#ifndef LUNAR24_TESTS_CORE_RT_GUARD_TEST_H
#define LUNAR24_TESTS_CORE_RT_GUARD_TEST_H

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>

namespace rt {
namespace {

thread_local bool g_rt_window = false;
std::atomic<long> g_heap_in_rt{0};     // new/new[] while in RT window
std::atomic<long> g_mutex_in_rt{0};    // mutex lock while in RT window
std::atomic<long> g_file_in_rt{0};     // file write while in RT window
std::atomic<long> g_log_in_rt{0};      // log call while in RT window

inline void reset() {
  g_heap_in_rt.store(0);
  g_mutex_in_rt.store(0);
  g_file_in_rt.store(0);
  g_log_in_rt.store(0);
}

// Scoped RT window: the audio-callback body is bracketed by this RAII type.
struct RtGuard {
  bool prev_;
  RtGuard() : prev_(g_rt_window) { g_rt_window = true; }
  ~RtGuard() { g_rt_window = prev_; }
};

// Checked wrappers for the RT-forbidden operations (invariant 2 of the spike).
// Routed here so the render path is intercepted as a real mechanism, not a claim.
inline void rt_check_mutex_lock(std::mutex& m) {
  if (g_rt_window) g_mutex_in_rt.fetch_add(1, std::memory_order_relaxed);
  m.lock();
}
inline void rt_check_file_write(const char*) {
  if (g_rt_window) g_file_in_rt.fetch_add(1, std::memory_order_relaxed);
}
inline void rt_check_log(const char*) {
  if (g_rt_window) g_log_in_rt.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace
}  // namespace rt

// ---------------------------------------------------------------------------
// Global operator new/delete overrides. These are the probe for invariant 1:
// they intercept EVERY allocation in this TU, so even a buried one is caught.
// They only mark; they never count as an error on their own.
// ---------------------------------------------------------------------------
void* operator new(std::size_t n) {
  if (rt::g_rt_window) rt::g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (rt::g_rt_window) rt::g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (rt::g_rt_window) rt::g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  if (rt::g_rt_window) rt::g_heap_in_rt.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#endif  // LUNAR24_TESTS_CORE_RT_GUARD_TEST_H
