// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/computer_keys.h>

#include <vector>

using lunar24::host::ComputerKeys;

int main() {
  constexpr int kA = 0, kS = 2, kVkA = 65, kVkS = 83, kVkShift = 16;
  {
    ComputerKeys k;
    CHECK(k.press(kA, kVkA, 12));
    CHECK(!k.press(kA, kVkA, 12));  // auto-repeat: no second note
    ComputerKeys::Held h;
    CHECK_EQ(k.release(kA, kVkA, &h), kA);
    CHECK_EQ(h.sounding, 12);       // the pitch it was pressed at, whatever the octave is now
    CHECK(k.empty());
    CHECK_EQ(k.release(kA, kVkA), -1);  // a second key-up matches nothing
  }
  // The character changed while the key was down (Shift, an input method): matched by key code.
  {
    ComputerKeys k;
    CHECK(k.press(kS, kVkS, 2));
    CHECK_EQ(k.release(-1, kVkS), kS);
    CHECK(k.empty());
  }
  // An unrelated key-up releases nothing.
  {
    ComputerKeys k;
    CHECK(k.press(kA, kVkA, 0));
    CHECK_EQ(k.release(-1, kVkShift), -1);
    CHECK(!k.empty());
  }
  // Focus lost: every held key is released once, then nothing is held.
  {
    ComputerKeys k;
    CHECK(k.press(kA, kVkA, 0));
    CHECK(k.press(kS, kVkS, 14));
    std::vector<int> released, sounding;
    CHECK_EQ(k.releaseAll([&](int semi, const ComputerKeys::Held& h) {
      released.push_back(semi);
      sounding.push_back(h.sounding);
    }), 2);
    CHECK(released == (std::vector<int>{kA, kS}));
    CHECK(sounding == (std::vector<int>{0, 14}));
    CHECK(k.empty());
    CHECK_EQ(k.releaseAll([](int, const ComputerKeys::Held&) {}), 0);
    CHECK_EQ(k.release(kA, kVkA), -1);  // the real key-up arriving later is ignored
    CHECK(k.press(kA, kVkA, 0));        // and the key plays again normally
  }
  return test::finish("test_computer_keys");
}
