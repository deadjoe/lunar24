// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The classic drones' photo sensor: a hand darkens it and lowers MOD-on generators only;
// the CdS cell brightens faster than it darkens and is slower back after a long cover; room
// light and the hand's tremor keep it moving; the same seed repeats exactly.
#include "mini_test.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <lunar24/core/drone_bank.h>
#include <lunar24/core/photo_sensor.h>

namespace {
using lunar24::core::DroneBank;
using lunar24::core::PhotoSensor;
constexpr double kSr = 48000.0;

double run(PhotoSensor& p, double seconds) {
  double o = 0.0;
  for (int i = 0; i < int(seconds * kSr); ++i) o = p.tick();
  return o;
}
PhotoSensor still() {  // no room drift: only the hand moves the light
  PhotoSensor p;
  p.prepare(7, kSr);
  p.setAmbientEnabled(false);
  return p;
}
// Cycles generator 0 runs in `seconds` (phase wraps).
int cycles(DroneBank& b, double seconds) {
  double out[DroneBank::kMaxVoices];
  int n = 0;
  double last = b.phaseOf(0);
  for (int i = 0; i < int(seconds * kSr); ++i) {
    b.tick(out);
    if (b.phaseOf(0) < last) ++n;
    last = b.phaseOf(0);
  }
  return n;
}
}  // namespace

int main() {
  // No hand, still air: no detune at all.
  {
    PhotoSensor p = still();
    CHECK_EQ(run(p, 1.0), 0.0);
    CHECK(std::fabs(p.light01() - 1.0) < 1e-12);
  }
  // A hand on the eye darkens it and lowers the pitch, bounded by the fixed resistor.
  {
    PhotoSensor p = still();
    p.setShade(1.0);
    const double early = run(p, 0.2);
    const double settled = run(p, 8.0);
    CHECK(settled < -0.1 && settled > -0.25);
    CHECK(p.light01() < 0.1);
    // Two-part response: most of it at once, the rest sinks in over seconds.
    CHECK(early < 0.4 * settled);
    CHECK(early > 0.9 * settled);
  }
  // Brightens faster than it darkens: half a second after the change, the light left to go
  // (in stops) is smaller after uncovering than after covering.
  {
    const double covered = std::log2(PhotoSensor::kFullShadeTransmit);
    PhotoSensor p = still();
    p.setShade(1.0);
    run(p, 0.5);
    const double leftCovering = p.lightStops() - covered;
    run(p, 1.5);  // 2 s covered
    p.setShade(0.0);
    run(p, 0.5);
    const double leftUncovering = -p.lightStops();
    CHECK(leftCovering > 0.0 && leftUncovering > 0.0);
    CHECK(leftUncovering < 0.5 * leftCovering);
    // ... and comes back to the room light in the end.
    CHECK(std::fabs(run(p, 10.0)) < 1e-3);
  }
  // Light-history memory: after a long cover the way back is slower than after a short one.
  {
    PhotoSensor shortCover = still(), longCover = still();
    shortCover.setShade(1.0);
    longCover.setShade(1.0);
    run(shortCover, 0.5);
    run(longCover, 8.0);
    shortCover.setShade(0.0);
    longCover.setShade(0.0);
    const double shortLeft = std::fabs(run(shortCover, 0.4));
    const double longLeft = std::fabs(run(longCover, 0.4));
    CHECK(longLeft > shortLeft);
  }
  // A hovering hand is never still (tremor and sway); the untouched room drifts slowly.
  {
    PhotoSensor p = still();
    p.setShade(0.5);
    run(p, 2.0);
    double lo = 1.0, hi = -1.0;
    for (int i = 0; i < int(1.0 * kSr); ++i) {
      const double o = p.tick();
      lo = std::min(lo, o);
      hi = std::max(hi, o);
    }
    CHECK(hi - lo > 1e-4);

    PhotoSensor room;
    room.prepare(7, kSr);
    double rlo = 1.0, rhi = -1.0;
    for (int i = 0; i < int(30.0 * kSr); i += 64) {
      for (int k = 0; k < 64; ++k) room.tick();
      rlo = std::min(rlo, room.octaves());
      rhi = std::max(rhi, room.octaves());
    }
    CHECK(rhi - rlo > 1e-3);                            // it moves ...
    CHECK(std::fabs(rlo) < 0.03 && std::fabs(rhi) < 0.03);  // ... but only a little
  }
  // Same seed, same light, sample for sample.
  {
    PhotoSensor a, b;
    a.prepare(42, kSr);
    b.prepare(42, kSr);
    a.setShade(0.7);
    b.setShade(0.7);
    bool same = true;
    for (int i = 0; i < 48000; ++i) same = same && a.tick() == b.tick();
    CHECK(same);
  }
  // In the drone bank: a shaded eye lowers a MOD-on generator; a MOD-off one ignores it.
  {
    auto bank = [](bool mod, double shade) {
      auto b = std::make_unique<DroneBank>(1, kSr, DroneBank::kMaxVoices, false);
      b->setMod(0, mod ? 1.0 : 0.0);
      b->setGroupShade(0, shade);
      cycles(*b, 3.0);  // let the cell settle
      return cycles(*b, 2.0);
    };
    const int open = bank(true, 0.0);
    CHECK(bank(true, 1.0) < open - 3);  // ~55 Hz generator, over a semitone down
    CHECK_EQ(bank(false, 1.0), bank(false, 0.0));
  }
  return test::finish("test_photo_sensor");
}
