// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Minimal dependency-free test harness. No third-party framework: the core
// library is framework-free and testing it must not drag one in. Compile with
// LUNAR_TEST_MAIN defined in exactly one translation unit to get the runner.

#pragma once

#include <cstdio>
#include <cstdlib>

namespace test {
inline int failures = 0;
inline int checks = 0;

inline void report(bool ok, const char* expr, const char* file, int line) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}

inline int finish(const char* suite) {
  if (failures) {
    std::fprintf(stderr, "[%s] %d/%d checks FAILED\n", suite, failures, checks);
    return EXIT_FAILURE;
  }
  std::printf("[%s] %d checks OK\n", suite, checks);
  return EXIT_SUCCESS;
}
}  // namespace test

#define CHECK(cond) ::test::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    auto _va = (a);                                                           \
    auto _vb = (b);                                                           \
    ::test::report(_va == _vb, #a " == " #b, __FILE__, __LINE__);             \
  } while (0)
#define CHECK_TRUE(cond) CHECK(cond)
#define CHECK_FALSE(cond) CHECK(!(cond))
