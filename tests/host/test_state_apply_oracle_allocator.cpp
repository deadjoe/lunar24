// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Allocator-count probe for the GH#12 9B state-apply oracle (task#76). The replaceable
// operator new/delete pair is isolated into its own TU, built ONLY into the
// test_state_apply_oracle target, so GCC's -Wmismatched-new-delete does not misjudge the
// malloc/free implementation as a new/delete mismatch at the ::operator new / ::operator
// delete call sites in the test TU. An operator-new definition in the same TU as those call
// sites is what trips the warning; a TU boundary hides it.
//
// Semantics are unchanged from the engine-level probe (test_host_engine_oracle_allocator.cpp).
// Every operator new/new[] in THIS test binary (including the linked lunar_core interface and
// the MachineRuntimeDefinition / SynthRuntime it constructs) is counted in g_allocCount; the
// applyDeviceState -> processBlock() render path leaves the render window at zero, and a
// deliberate crossing allocation inside the window must be detected.
//
// ALIGNED: the @Codex false-green was that we only replaced the UNALIGNED operators, so an
// over-aligned (C++17 `alignas(64)` -> `operator new(size_t, align_val_t)`) allocation and its
// matching aligned delete bypassed the counters entirely. We therefore replace the aligned
// new/new[] and delete/delete[] pair too (unsized AND sized-aligned forms), and on the aligned
// path we OVER-ALLOCATE deliberately so the pointer stays freeable by the matching aligned
// delete. We do NOT use std::aligned_alloc — absent from MSVC/UCRT; plain malloc/free is the
// universally-present matched pair, so this one branch-free path is byte-identical on every
// platform and compiles on MSVC. `g_allocCount`/`g_freeCount` count BOTH unaligned and aligned.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

std::size_t g_allocCount = 0;
std::size_t g_freeCount = 0;

// ---- unaligned pair ---------------------------------------------------------
void* operator new(std::size_t n) {
  ++g_allocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { ++g_freeCount; std::free(p); }
void operator delete[](void* p) noexcept { ++g_freeCount; std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ++g_freeCount; std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { ++g_freeCount; std::free(p); }

// ---- aligned pair (C++17 over-aligned) --------------------------------------
void* operator new(std::size_t n, std::align_val_t al) {
  ++g_allocCount;
  const std::size_t a = static_cast<std::size_t>(al);
  const std::size_t base = n ? n : 1;
  const std::size_t total = base + a + sizeof(void*);
  void* raw = std::malloc(total);
  if (!raw) throw std::bad_alloc();
  const std::uintptr_t mask = static_cast<std::uintptr_t>(a) - 1u;
  void* aligned = reinterpret_cast<void*>(
      (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + mask) & ~mask);
  reinterpret_cast<void**>(aligned)[-1] = raw;
  return aligned;
}
void* operator new[](std::size_t n, std::align_val_t al) { return ::operator new(n, al); }
void operator delete(void* p, std::align_val_t) noexcept {
  if (p) { ++g_freeCount; std::free(reinterpret_cast<void**>(p)[-1]); }
}
void operator delete[](void* p, std::align_val_t) noexcept { ::operator delete(p, std::align_val_t{}); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p, std::align_val_t{}); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p, std::align_val_t{}); }
