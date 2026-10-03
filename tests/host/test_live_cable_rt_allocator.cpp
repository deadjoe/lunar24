// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Counting operator new / delete for test_live_cable_rt, in its own translation unit so GCC
// does not pair a new-expression with the free() below (-Wmismatched-new-delete).
#include <cstddef>
#include <cstdlib>
#include <new>

std::size_t g_newCount = 0;
std::size_t g_deleteCount = 0;

void* operator new(std::size_t n) {
  ++g_newCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  ++g_newCount;
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p) ++g_deleteCount;
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
