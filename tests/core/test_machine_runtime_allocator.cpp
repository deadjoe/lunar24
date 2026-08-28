// #38 criterion ⑤ (rule 5) allocator-count probe. The replaceable operator new/delete
// pair is isolated into its own translation unit, built ONLY into the
// test_machine_runtime target, so GCC's -Wmismatched-new-delete does not misjudge the
// malloc/free implementation: within a single TU that also contains ::operator new /
// ::operator delete call sites, GCC pairs a new-allocated pointer with the free() in
// operator delete and reports a mismatch; a TU boundary hides that trace.
//
// Semantics are unchanged from when this lived inside test_machine_runtime.cpp. Every
// operator new/new[] in THIS test binary (including the linked lunar_core library) is
// counted in g_allocCount; the render loop must leave it at zero, and a deliberate
// crossing allocation inside the measurement window must be detected (see
// test_machine_runtime.cpp criterion ⑤). We replace only the unaligned operators (a
// deliberate-alloc vector goes through unaligned new; the render path allocates nothing
// at all, aligned or not). Forwarding to malloc/free keeps the default delete (which
// frees) compatible.
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
