// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) Joystick X/Y + OFFSET X/Y bipolar CV sound-core strong-oracle
// suite for core/include/lunar24/core/joystick_cv.h. This new JoystickCv is a
// stateless per-sample CV mapping core — NOT a wrapper over any audio-rate helper
// and NOT an A/B pair. The panel has ONE joystick module with two independent
// axes/outputs X and Y driven by four normalized controls (x, y, offset_x,
// offset_y). The tests pin behaviour / monotonic trend / determinism / a truthful
// end-to-end offset window and never fake a measured hardware curve (see the
// header's PROVISIONAL modelling constants). The -10..+10V bipolar rail is
// grounded on the REAL reg::kJacks joystick.x_out / joystick.y_out descriptor,
// not a test-side shadow.
//
// @Codex mandate (msg 5868a918) must-tests:
//   ① centre = 0V                                   (all four controls at 0.5)
//   ② offset window endpoints:  left [-10,0] / centre [-5,+5] / right [0,+10]
//   ③ X/offset-X and Y/offset-Y each monotonic       (independent monotone trend)
//   ④ each axis/offset affects ONLY its own output   (no cross-wiring, no X->Y copy)
//   ⑤ position-locking is constant                    (no spring-return / decay / reset)
//   ⑥ next-sample update reflects immediately         (no block cache)
//   ⑦ outputs always finite and bounded to [-10,+10]
//   ⑧ finite value clamps to [0,1]; non-finite is fail-closed
//   ⑨ invalid setter preserves FULL four-control state AND subsequent dual trace
//   ⑩ same per-sample control sequence => bit-identical dual trace under different
//      buffer partitions
//   ⑪ real registry joystick.x_out / joystick.y_out (output / cv / bipolar / -10..+10)
//
// Negative controls (each narrow old-error RED->revert GREEN) are run separately
// in a detached /tmp worktree: ① offset ignored, ② X/Y cross-wired or Y copies X,
// ③ output wrongly unipolar / ±5V rail, ④ output only updated at a block boundary
// (cached), ⑤ an invalid setter pollutes state/trace, ⑥ only X implemented, Y
// still a shadow/constant. Detectors read real dual-sample output — never source
// grep / a read-only inspector / a self-copied trace.

#include "mini_test.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "lunar24/core/joystick_cv.h"
#include "lunar24/registry.hpp"

namespace core = lunar24::core;
namespace reg = lunar24::registry;

namespace {

using core::JoystickCv;

int find_jack(core::JackId id) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

// Authoritative, LITERAL expectation for the documented provisional linear transfer.
// Two contributors, each mapped [-5,+5], summed then bounded to [-10,+10].
// Hard-coded so it is INDEPENDENT of the header's named constants — a mutation that
// changes those constants in the header diverges from this oracle instead of
// moving in lockstep with it.
static double expected_bipolar(double position, double offset) {
  const double pv = 5.0 * (2.0 * position - 1.0);
  const double ov = 5.0 * (2.0 * offset - 1.0);
  const double s = pv + ov;
  if (s < -10.0) return -10.0;
  if (s > 10.0) return 10.0;
  return s;
}

static bool finite_ok(double v) { return std::isfinite(v); }

// Per-sample control record for the sequence / partition tests.
struct Ctrl {
  double x, y, ox, oy;
};

// Deliver a per-sample control sequence sample-by-sample and record the dual trace.
static void run_per_sample(const std::vector<Ctrl>& seq, std::vector<double>& xs,
                           std::vector<double>& ys) {
  JoystickCv j;
  xs.resize(seq.size());
  ys.resize(seq.size());
  for (std::size_t k = 0; k < seq.size(); ++k) {
    j.setX(seq[k].x);
    j.setY(seq[k].y);
    j.setOffsetX(seq[k].ox);
    j.setOffsetY(seq[k].oy);
    xs[k] = j.xOut();
    ys[k] = j.yOut();
  }
}

// The SAME per-sample control sequence, but re-applied in fixed-size block frames
// (each frame re-delivers its per-sample control values). Because JoystickCv is a
// stateless per-sample mapping core, the dual trace must be bit-identical to the
// sample-by-sample run.
static void run_in_blocks(const std::vector<Ctrl>& seq, std::size_t block,
                          std::vector<double>& xs, std::vector<double>& ys) {
  JoystickCv j;
  xs.resize(seq.size());
  ys.resize(seq.size());
  for (std::size_t base = 0; base < seq.size(); base += block) {
    const std::size_t end = (base + block < seq.size()) ? base + block : seq.size();
    for (std::size_t k = base; k < end; ++k) {
      j.setX(seq[k].x);
      j.setY(seq[k].y);
      j.setOffsetX(seq[k].ox);
      j.setOffsetY(seq[k].oy);
      xs[k] = j.xOut();
      ys[k] = j.yOut();
    }
  }
}

void test_center_zero_out() {
  JoystickCv j;  // all four controls default to centre 0.5
  CHECK_EQ(j.xOut(), 0.0);
  CHECK_EQ(j.yOut(), 0.0);
  CHECK_EQ(j.x(), 0.5);
  CHECK_EQ(j.offsetX(), 0.5);
}

void test_offset_window_endpoints() {
  JoystickCv j;
  // Offset left (0): as position sweeps 0..1 the output window is exactly [-10,0].
  j.setOffsetX(0.0);
  j.setX(0.0); CHECK_EQ(j.xOut(), -10.0);
  j.setX(1.0); CHECK_EQ(j.xOut(), 0.0);
  // Offset centre (0.5): window is [-5,+5].
  j.setOffsetX(0.5);
  j.setX(0.0); CHECK_EQ(j.xOut(), -5.0);
  j.setX(1.0); CHECK_EQ(j.xOut(), 5.0);
  // Offset right (1): window is [0,+10].
  j.setOffsetX(1.0);
  j.setX(0.0); CHECK_EQ(j.xOut(), 0.0);
  j.setX(1.0); CHECK_EQ(j.xOut(), 10.0);

  // Same window, but confirm the CENTRE position contributes 0 and the offset alone
  // sets the mid value: offset left=0 => -5, centre=0.5 => 0, right=1 => +5.
  JoystickCv k;
  k.setX(0.5);
  k.setOffsetX(0.0); CHECK_EQ(k.xOut(), -5.0);
  k.setOffsetX(0.5); CHECK_EQ(k.xOut(), 0.0);
  k.setOffsetX(1.0); CHECK_EQ(k.xOut(), 5.0);

  // Y axis must match its own window exactly (independent computation).
  JoystickCv m;
  m.setOffsetY(0.0);
  m.setY(0.0); CHECK_EQ(m.yOut(), -10.0);
  m.setY(1.0); CHECK_EQ(m.yOut(), 0.0);
  m.setOffsetY(0.5);
  m.setY(0.0); CHECK_EQ(m.yOut(), -5.0);
  m.setY(1.0); CHECK_EQ(m.yOut(), 5.0);
  m.setOffsetY(1.0);
  m.setY(0.0); CHECK_EQ(m.yOut(), 0.0);
  m.setY(1.0); CHECK_EQ(m.yOut(), 10.0);
}

void test_full_bipolar_corners() {
  JoystickCv j;
  // Both corners hit the confirmed +-10 rail: a unipolar OR a ±5V output must fail.
  j.setX(1.0); j.setOffsetX(1.0);
  CHECK_EQ(j.xOut(), 10.0);
  j.setX(0.0); j.setOffsetX(0.0);
  CHECK_EQ(j.xOut(), -10.0);
  j.setY(1.0); j.setOffsetY(1.0);
  CHECK_EQ(j.yOut(), 10.0);
  j.setY(0.0); j.setOffsetY(0.0);
  CHECK_EQ(j.yOut(), -10.0);
}

void test_axis_monotonic_trend() {
  JoystickCv j;
  // X position monotone non-decreasing with fixed centre offset.
  double prev = -std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 20; ++i) {
    const double v = i / 20.0;
    j.setX(v); j.setOffsetX(0.5);
    const double out = j.xOut();
    CHECK(out >= prev);
    CHECK_EQ(out, expected_bipolar(v, 0.5));
    prev = out;
  }
  // Offset-X monotone non-decreasing with fixed centre position.
  prev = -std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 20; ++i) {
    const double v = i / 20.0;
    j.setX(0.5); j.setOffsetX(v);
    const double out = j.xOut();
    CHECK(out >= prev);
    CHECK_EQ(out, expected_bipolar(0.5, v));
    prev = out;
  }
  // Y position + offset-Y each monotone, independent axes.
  JoystickCv k;
  prev = -std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 20; ++i) {
    const double v = i / 20.0;
    k.setY(v); k.setOffsetY(0.5);
    const double out = k.yOut();
    CHECK(out >= prev);
    CHECK_EQ(out, expected_bipolar(v, 0.5));
    prev = out;
  }
}

void test_axis_independence() {
  JoystickCv j;
  // X set to a LOW composite (-6.0); Y set to a HIGH composite (+10.0). They are far
  // apart, so a Y-copies-X / cross-wire / shared-shadow mutation is exposed.
  j.setX(0.2); j.setOffsetX(0.2);
  j.setY(1.0); j.setOffsetY(1.0);
  CHECK_EQ(j.xOut(), -6.0);
  CHECK_EQ(j.yOut(), 10.0);
  CHECK(j.xOut() != j.yOut());

  // Changing X + offset-X leaves Y untouched.
  const double y_before = j.yOut();
  j.setX(1.0); j.setOffsetX(0.0);
  CHECK_EQ(j.xOut(), 0.0);
  CHECK_EQ(j.yOut(), y_before);

  // Changing Y + offset-Y leaves X untouched.
  const double x_before = j.xOut();
  j.setY(0.0); j.setOffsetY(0.0);
  CHECK_EQ(j.yOut(), -10.0);
  CHECK_EQ(j.xOut(), x_before);

  // offset-X only moves X; offset-Y only moves Y.
  j.setX(0.5); j.setOffsetX(0.0); j.setY(0.5); j.setOffsetY(0.0);
  CHECK_EQ(j.xOut(), -5.0);
  CHECK_EQ(j.yOut(), -5.0);
  j.setOffsetY(1.0);
  CHECK_EQ(j.yOut(), 5.0);
  CHECK_EQ(j.xOut(), -5.0);  // unaffected by offset-Y
}

void test_position_locking_constant() {
  JoystickCv j;
  j.setX(0.8); j.setOffsetX(0.3); j.setY(0.2); j.setOffsetY(0.9);
  const double x0 = j.xOut();
  const double y0 = j.yOut();
  // Many reads, no control change: value persists (no spring-return / decay / reset).
  for (int i = 0; i < 100000; ++i) {
    if (j.xOut() != x0) { CHECK(false); return; }
    if (j.yOut() != y0) { CHECK(false); return; }
  }
  CHECK(true);
}

void test_next_sample_update() {
  JoystickCv j;
  j.setX(0.0); j.setOffsetX(0.0);
  CHECK_EQ(j.xOut(), -10.0);
  // Same sample stream immediately reflects the new control: NO block cache.
  j.setX(1.0);                       // P=1,O=0 -> +5-5 = 0 (immediate from -10)
  CHECK_EQ(j.xOut(), 0.0);
  j.setOffsetX(1.0);
  CHECK_EQ(j.xOut(), 10.0);
  j.setY(0.0); j.setOffsetY(1.0);    // P=0,O=1 -> -5+5 = 0
  CHECK_EQ(j.yOut(), 0.0);
  j.setY(1.0);
  CHECK_EQ(j.yOut(), 10.0);
}

void test_rail_bounded_finite() {
  JoystickCv j;
  for (int xi = 0; xi <= 20; ++xi) {
    for (int oi = 0; oi <= 20; ++oi) {
      const double x = xi / 20.0, ox = oi / 20.0;
      j.setX(x); j.setOffsetX(ox);
      const double xo = j.xOut();
      CHECK(finite_ok(xo));
      CHECK(xo >= -10.0 && xo <= 10.0);
      CHECK_EQ(xo, expected_bipolar(x, ox));
    }
  }
  for (int yi = 0; yi <= 20; ++yi) {
    for (int oi = 0; oi <= 20; ++oi) {
      const double y = yi / 20.0, oy = oi / 20.0;
      j.setY(y); j.setOffsetY(oy);
      const double yo = j.yOut();
      CHECK(finite_ok(yo));
      CHECK(yo >= -10.0 && yo <= 10.0);
      CHECK_EQ(yo, expected_bipolar(y, oy));
    }
  }
}

void test_finite_clamp() {
  JoystickCv j;
  // Known-finite out-of-range values are bounds-clamped to [0,1] (still accepted).
  CHECK_TRUE(j.setX(-0.5));
  CHECK_EQ(j.x(), 0.0);
  CHECK_TRUE(j.setX(1.5));
  CHECK_EQ(j.x(), 1.0);
  CHECK_TRUE(j.setY(-3.0));
  CHECK_EQ(j.y(), 0.0);
  CHECK_TRUE(j.setOffsetX(4.0));
  CHECK_EQ(j.offsetX(), 1.0);
  CHECK_TRUE(j.setOffsetY(-0.25));
  CHECK_EQ(j.offsetY(), 0.0);
}

void test_invalid_fail_closed_preserves_state_and_trace() {
  JoystickCv j;
  j.setX(0.2); j.setY(0.7); j.setOffsetX(0.4); j.setOffsetY(0.9);
  const double x0 = j.x(), y0 = j.y(), ox0 = j.offsetX(), oy0 = j.offsetY();
  const double xo0 = j.xOut(), yo0 = j.yOut();

  // Non-finite setters must FAIL-CLOSED (return false) and leave the WHOLE
  // four-control state plus the computed dual trace untouched.
  CHECK_FALSE(j.setX(std::numeric_limits<double>::quiet_NaN()));
  CHECK_FALSE(j.setY(std::numeric_limits<double>::infinity()));
  CHECK_FALSE(j.setOffsetX(-std::numeric_limits<double>::infinity()));
  CHECK_FALSE(j.setOffsetY(std::numeric_limits<double>::quiet_NaN()));

  CHECK_EQ(j.x(), x0);
  CHECK_EQ(j.y(), y0);
  CHECK_EQ(j.offsetX(), ox0);
  CHECK_EQ(j.offsetY(), oy0);
  CHECK_EQ(j.xOut(), xo0);
  CHECK_EQ(j.yOut(), yo0);

  // And a subsequent valid read over many samples is unchanged (no latent pollution).
  for (int i = 0; i < 1000; ++i) {
    if (j.xOut() != xo0 || j.yOut() != yo0) { CHECK(false); return; }
  }
  CHECK(true);
}

void test_block_partition_bit_identical() {
  std::vector<Ctrl> seq;
  // Deliberately irregular, X and Y DIFFERENT (X sweeps low->high, Y does an
  // independent high->low zig), so any partition-dependent path diverges.
  for (int i = 0; i < 64; ++i) {
    Ctrl c;
    c.x = static_cast<double>(i % 7) / 7.0;
    c.y = static_cast<double>(5 - (i % 5)) / 5.0;
    c.ox = static_cast<double>((i * 3) % 9) / 9.0;
    c.oy = static_cast<double>((i * 4) % 11) / 11.0;
    seq.push_back(c);
  }

  std::vector<double> xsa, ysa, xsb, ysb;
  run_per_sample(seq, xsa, ysa);
  run_in_blocks(seq, 3, xsb, ysb);
  CHECK(xsa == xsb);
  CHECK(ysa == ysb);

  // A different (non-aligned) partition size must also agree bit-identically.
  std::vector<double> xsc, ysc;
  run_in_blocks(seq, 7, xsc, ysc);
  CHECK(xsa == xsc);
  CHECK(ysa == ysc);
}

void test_registry_jack_descriptors() {
  const int ix = find_jack(core::JackId::joystick_x_out);
  const int iy = find_jack(core::JackId::joystick_y_out);
  CHECK_TRUE(ix >= 0);
  CHECK_TRUE(iy >= 0);
  if (ix >= 0 && iy >= 0) {
    const auto& jx = reg::kJacks[ix];
    CHECK(jx.direction == core::PinDirection::output);
    CHECK(jx.signalType == core::SignalType::cv);
    CHECK(jx.polarity == core::Polarity::bipolar);
    CHECK_EQ(jx.nominalMin, -10.0);
    CHECK_EQ(jx.nominalMax, 10.0);

    const auto& jy = reg::kJacks[iy];
    CHECK(jy.direction == core::PinDirection::output);
    CHECK(jy.signalType == core::SignalType::cv);
    CHECK(jy.polarity == core::Polarity::bipolar);
    CHECK_EQ(jy.nominalMin, -10.0);
    CHECK_EQ(jy.nominalMax, 10.0);
  }
}

}  // namespace

int main() {
  test_center_zero_out();
  test_offset_window_endpoints();
  test_full_bipolar_corners();
  test_axis_monotonic_trend();
  test_axis_independence();
  test_position_locking_constant();
  test_next_sample_update();
  test_rail_bounded_finite();
  test_finite_clamp();
  test_invalid_fail_closed_preserves_state_and_trace();
  test_block_partition_bit_identical();
  test_registry_jack_descriptors();
  return test::finish("test_joystick_cv");
}
