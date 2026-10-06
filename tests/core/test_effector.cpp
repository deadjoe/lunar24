// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Dual effector: every program stays finite and bounded, BLEND=0 is dry, program switching works,
// and the two reverbs stay balanced on a steady chord.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "mini_test.h"
#include <lunar24/core/effector.h>

using lunar24::core::DualEffector;
using lunar24::core::EffectorSlot;

namespace {

double testInput(int i, double sr) {
  const double t = i / sr;
  return 0.6 * std::sin(6.283185307 * 110.0 * t) + 0.3 * std::sin(6.283185307 * 331.0 * t);
}

// Reference history stores newest first and really zeroes on clear. It has no ring cursor
// or written-count bookkeeping, so stale samples / off-by-one errors cannot hide in both.
void delayLineClear() {
  lunar24::core::fx::DelayLine line;
  for (std::size_t minLen : {0u, 13u, 29u, 125u, 0u}) {  // also re-init used storage
    line.init(minLen);
    const std::size_t n = line.size();
    std::vector<float> history(n, 0.0f);
    auto write = [&](double x) {
      line.write(x);
      std::move_backward(history.begin(), history.end() - 1, history.end());
      history[0] = static_cast<float>(x);
    };
    auto clear = [&] {
      line.clear();
      std::fill(history.begin(), history.end(), 0.0f);
    };
    auto verify = [&] {
      bool integersMatch = true, fractionsMatch = true;
      for (std::size_t d = 0; d < 3 * n; ++d)
        integersMatch = integersMatch && line.readInt(d) == history[d % n];
      CHECK(integersMatch);  // includes wrapped reads after only a partial refill
      CHECK_EQ(line.readInt(std::numeric_limits<std::size_t>::max()), history[n - 1]);
      for (int quarter = -4; quarter <= static_cast<int>(4 * n); ++quarter) {
        const double delay = quarter * 0.25;
        const double d = std::max(0.0, std::min(delay, static_cast<double>(n - 4)));
        const auto i = static_cast<std::size_t>(std::floor(d));
        const double f = d - std::floor(d);
        const double y0 = history[i], y1 = history[i + 1];
        const double ym1 = history[i == 0 ? 0 : i - 1], y2 = history[i + 2];
        const double c1 = 0.5 * (y1 - ym1);
        const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
        const double expected = ((c3 * f + c2) * f + c1) * f + y0;
        const double actual = line.read(delay);
        fractionsMatch = fractionsMatch && std::memcmp(&actual, &expected, sizeof(double)) == 0;
      }
      CHECK(fractionsMatch);  // four taps straddling the written / unwritten boundary
    };
    verify();
    clear();
    clear();
    verify();
    for (std::size_t k = 0; k < 5 * n; ++k) {
      // Clear partially filled and full lines at different cursor positions, including wrap.
      if (k == 3 || k == n - 1 || k == 2 * n + 3) { clear(); verify(); }
      write(std::sin(static_cast<double>(k) * 0.73) + 0.123456789);
      verify();
    }
    // Even a non-finite old value must be hidden, not multiplied by zero into another NaN.
    write(std::numeric_limits<double>::quiet_NaN());
    write(std::numeric_limits<double>::infinity());
    clear();
    verify();
    write(0.375);
    verify();
  }
}

}  // namespace

int main() {
  delayLineClear();
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

  // BLEND = 0 passes the dry signal (at MASTER 0.5 = the output trim; below the limiter knee)
  // once the knobs have settled.
  {
    static DualEffector fx;
    fx.init(sr);
    fx.setBlend(0.0);
    fx.setMaster(0.5);
    double maxErr = 0.0;
    for (int i = 0; i < int(sr); ++i) {
      double l = 0.3 * testInput(i, sr), r = l;
      const double dry = DualEffector::kOutputTrim * l;
      fx.process(l, r);
      if (i > int(sr) / 2) maxErr = std::fmax(maxErr, std::fabs(dry - l));
    }
    CHECK(maxErr < 0.05);  // only the 5 Hz DC blocker differs from the dry signal
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

  // A steady drone chord into both sides of Space reverb: the left and right reverbs (different
  // line lengths) come out about equally loud, whatever the root, instead of one side sitting
  // on a resonance and the other between two.
  {
    static EffectorSlot left, right;
    double sumDiff = 0.0;
    int n = 0;
    for (double root : {49.0, 55.0, 65.4, 82.4, 98.0, 110.0}) {
      left.init(sr, 0);
      right.init(sr, 1);
      left.setProgram(2);
      right.setProgram(2);
      double el = 0.0, er = 0.0;
      for (int i = 0; i < int(16 * sr); ++i) {
        const double t = i / sr;
        const double x = 0.3 * (std::sin(6.283185307 * root * t) +
                                std::sin(6.283185307 * root * 1.502 * t) +
                                std::sin(6.283185307 * root * 2.003 * t));
        const double a = left.process(x, 0.5, 0.5, 0.5), b = right.process(x, 0.5, 0.5, 0.5);
        if (t > 6.0) { el += a * a; er += b * b; }
      }
      sumDiff += std::fabs(10.0 * std::log10(el / er));
      ++n;
    }
    CHECK(sumDiff / n < 3.5);  // the eight-line reverb averaged ~5 dB
  }

  return test::finish("test_effector");
}
