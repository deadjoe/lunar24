// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/chime.h>

#include <algorithm>
#include <cmath>
#include <vector>

using lunar24::host::Chime;

int main() {
  constexpr int kSr = 48000, kBlock = 256;
  for (int style = 0; style < 3; ++style) {
    Chime c(static_cast<Chime::Style>(style));
    std::vector<double> l(kBlock), r(kBlock);
    auto block = [&]() {
      std::fill(l.begin(), l.end(), 0.0);
      std::fill(r.begin(), r.end(), 0.0);
      c.render(l.data(), r.data(), kBlock, kSr);
      double p = 0.0;
      for (int i = 0; i < kBlock; ++i) p = std::max({p, std::fabs(l[i]), std::fabs(r[i])});
      return p;
    };
    CHECK(block() == 0.0);  // silent until triggered
    c.trigger();
    double peak = 0.0;
    const double first = block();
    const double onset = std::max(std::fabs(l[0]), std::fabs(r[0]));
    int blocks = 1;
    peak = first;
    while (c.active() && blocks < 8 * kSr / kBlock) { peak = std::max(peak, block()); ++blocks; }
    CHECK(first > 0.0);                     // starts in the next block
    CHECK(onset < 1e-3);                    // ... with a soft onset, not a click
    CHECK(peak > 0.05 && peak < 0.32);      // clearly heard, well below full scale (-10 dBFS)
    CHECK(!c.active());                     // over by itself ...
    CHECK(blocks * kBlock < 5 * kSr);       // ... within 5 s: a phrase, not a tail
    CHECK(block() == 0.0);
    // While MUTE is on the host renders it into nothing: consumed, never heard later.
    c.trigger();
    for (int i = 0; i < 8 * kSr / kBlock; ++i) c.render(nullptr, nullptr, kBlock, kSr);
    CHECK(!c.active());
    CHECK(block() == 0.0);
  }
  return test::finish("test_chime");
}
