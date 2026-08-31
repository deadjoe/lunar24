// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Allocator-count probe for the GH#4 8B2 host-owner oracle (task#72). The replaceable
// operator new/delete pair is isolated into its own TU, built ONLY into the
// test_host_engine_oracle target, so GCC's -Wmismatched-new-delete does not misjudge the
// malloc/free implementation as a new/delete mismatch at the ::operator new / ::operator
// delete call sites in the test TU. An operator-new definition in the same TU as those call
// sites is what trips the warning; a TU boundary hides it.
//
// Semantics are unchanged from the engine-level probe (test_machine_definition_allocator.cpp
// and test_device_adapter_oracle_allocator.cpp). Every operator new/new[] in THIS test binary
// (including the linked lunar_core interface and the MachineRuntimeDefinition / SynthRuntime it
// constructs) is counted in g_allocCount; the owner prepare() -> processBlock() render path
// leaves the render window at zero, and a deliberate crossing allocation inside the window must
// be detected.
//
// ALIGNED: the @Codex false-green was that we only replaced the UNALIGNED operators, so an
// over-aligned (C++17 `alignas(64)` -> `operator new(size_t, align_val_t)`) allocation and its
// matching aligned delete bypassed the counters entirely: a pre-allocated over-aligned object
// reset() inside the callback stayed a silent green. We therefore replace the aligned new/new[]
// and delete/delete[] pair too (unsized AND sized-aligned forms), and on the aligned path we
// OVER-ALLOCATE deliberately so the pointer stays freeable by the matching aligned delete: the
// aligned new calls plain malloc (base + align + sizeof(void*)), rounds the returned pointer up
// to the required alignment, and stashes the raw pointer one slot before the aligned pointer
// (aligned[-1]); the aligned delete recovers that raw pointer and frees it with plain free. We do
// NOT use std::aligned_alloc — it is absent from MSVC/UCRT entirely (not declared in <cstdlib>
// even under /std:c++17), and MSVC's aligned runtime uses _aligned_malloc/_aligned_free, which
// must never be freed with std::free. Plain malloc/free is the universally-present matched pair,
// so this one branch-free path is byte-identical on every platform and compiles on MSVC.
// `g_allocCount`/`g_freeCount` now count BOTH unaligned and aligned allocations, so the
// whole-delegate "0 alloc / 0 free" window and the aligned live probe both see the real total.
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
// PORTABLE over-allocation — deliberately NOT std::aligned_alloc: that function is entirely absent
// from MSVC (not declared in <cstdlib> even under /std:c++17), and MSVC's aligned runtime uses
// `_aligned_malloc`/`_aligned_free`, which must never be freed with std::free. So instead we
// over-allocate with the plain, universally-present malloc/free pair (same heap, so the matched
// aligned delete can always free it): allocate `base + align + sizeof(void*)`, round the returned
// pointer UP to the required alignment, and stash the raw pointer one slot before the aligned
// pointer. The aligned delete recovers that raw pointer and frees it. Byte-for-byte identical on
// every platform — no platform availability gate, no "size must be a multiple of alignment" rule,
// no _aligned_malloc/free mismatch. `align_val_t` is always a power of two >= alignof(max_align_t),
// so the mask round-up is exact and the aligned pointer is always >= sizeof(void*) past raw
// (leaving room to store the raw pointer at aligned[-1]).
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
