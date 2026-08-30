// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Allocator-count probe for the GH#11 partial acceptance (task#66,
// @Codex D4 — "real audio/PatchGraph oracle"; A′ "render-path zero allocation").
// The replaceable operator new/delete pair is isolated into its own TU, built ONLY
// into the test_machine_control_sources target, so GCC's -Wmismatched-new-delete
// does not misjudge the malloc/free implementation as a new/delete mismatch at the
// ::operator new / ::operator delete call sites in the test TU (identical to
// test_machine_definition_allocator.cpp — a TU boundary hides the warning).
//
// Semantics are unchanged: every operator new/new[] in this test binary (incl. the
// linked lunar_core library and the MachineRuntimeDefinition it constructs) is
// counted in g_allocCount; the processFrame/processBlock render loop must leave it
// at zero. We replace only the unaligned operators and forward to malloc/free.
#include <cstddef>
#include <cstdlib>
#include <new>

std::size_t g_allocCount = 0;

void* operator new(std::size_t n) {
  ++g_allocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
