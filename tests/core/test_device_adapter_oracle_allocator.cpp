// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Allocator-count probe for the GH#4 product oracle (task#71). The replaceable operator
// new/delete pair is isolated into its own TU, built ONLY into the
// test_device_adapter_oracle target, so GCC's -Wmismatched-new-delete does not misjudge
// the malloc/free implementation as a new/delete mismatch at the ::operator new /
// ::operator delete call sites in the test TU. An operator-new definition in the same TU
// as those call sites is what trips the warning; a TU boundary hides it.
//
// Semantics are unchanged from the engine-level probe (test_machine_definition_allocator.cpp).
// Every operator new/new[] in THIS test binary (including the linked lunar_core interface and
// the MachineRuntimeDefinition / SynthRuntime it constructs) is counted in g_allocCount; the
// runtime processFrame/processBlock + DeviceAdapter render loop must leave it at zero, and a
// deliberate crossing allocation inside the measurement window must be detected. We replace
// only the unaligned operators (a deliberate-alloc goes through unaligned new; the render path
// allocates nothing at all, aligned or not). Forwarding to malloc/free keeps the default
// delete (which frees) compatible.
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
