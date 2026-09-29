// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Dual effector: every program stays finite and bounded, BLEND=0 is dry, program switching works.

#include <cmath>

#include "mini_test.h"
#include <lunar24/core/effector.h>

using lunar24::core::DualEffector;
using lunar24::core::EffectorSlot;

namespace {

double testInput(int i, double sr) {
  const double t = i / sr;
  return 0.6 * std::sin(6.283185307 * 110.0 * t) + 0.3 * std::sin(6.283185307 * 331.0 * t);
}

}  // namespace

int main() {
  const double sr = 48000.0;

  // Every program, knobs at the extremes: finite, bounded (feedback paths are limited).
  {
    static EffectorSlot slot;
    slot.init(sr, 0);
    bool allFinite = true;
    double worst = 0.0;
    for (int p = 0; p < 39; ++p) {
      for (double k : {0.0, 1.0}) {
        slot.setProgram(p);
        for (int i = 0; i < int(sr); ++i) {
          const double o = slot.process(testInput(i, sr), k, k, k);
          allFinite = allFinite && std::isfinite(o);
          worst = std::fmax(worst, std::fabs(o));
        }
      }
    }
    CHECK(allFinite);
    CHECK(worst < 8.0);
  }

  // BLEND = 0 passes the dry signal (at MASTER 0.5 = unity) once the knobs have settled.
  {
    static DualEffector fx;
    fx.init(sr);
    fx.setBlend(0.0);
    fx.setMaster(0.5);
    double maxErr = 0.0;
    for (int i = 0; i < int(sr); ++i) {
      double l = 0.3 * testInput(i, sr), r = l;
      const double dry = l;
      fx.process(l, r);
      if (i > int(sr) / 2) maxErr = std::fmax(maxErr, std::fabs(std::tanh(dry / 1.9) * 1.9 - l));
    }
    CHECK(maxErr < 1e-6);
  }

  // Cartridge x 1-2-3 switch selects program cartridge*3 + switch, per side.
  {
    static DualEffector fx;
    fx.init(sr);
    fx.setCartridge(0, 7);  // INFINITY
    fx.setSelect(0, 2);
    fx.setCartridge(1, 1);  // MAGIC
    CHECK_EQ(fx.program(0), 23);
    CHECK_EQ(fx.program(1), 3);
  }

  // A program switch takes effect after the short crossfade and makes a different sound.
  {
    static EffectorSlot a, b;
    a.init(sr, 0);
    b.init(sr, 0);
    b.setProgram(13);  // FILTER HP/LP
    double diff = 0.0;
    for (int i = 0; i < int(sr); ++i) {
      const double in = testInput(i, sr);
      diff += std::fabs(a.process(in, 0.9, 0.1, 0.5) - b.process(in, 0.9, 0.1, 0.5));
    }
    CHECK(b.program() == 13);
    CHECK(diff > 100.0);
  }

  return test::finish("test_effector");
}
