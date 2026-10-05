// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Counting operator new / delete for test_state_swap. Only threads that call
// countHeapOpsOnThisThread are counted (the audio thread): the UI thread allocates freely while
// it swaps. Kept in its own translation unit so GCC does not pair a new-expression with the
// free() below.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
thread_local bool t_countHeap = false;
}
std::atomic<std::size_t> g_countedHeapOps{0};
// The calling thread's heap operations count from now on (or no longer).
void countHeapOpsOnThisThread(bool on) { t_countHeap = on; }

void* operator new(std::size_t n) {
  if (t_countHeap) g_countedHeapOps.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (t_countHeap) g_countedHeapOps.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p && t_countHeap) g_countedHeapOps.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
