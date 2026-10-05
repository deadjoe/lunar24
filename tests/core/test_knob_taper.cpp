// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Knob tapers: envelope A/D/R cubic (the drones' ATT / RLS curve), LFO RATE exponential,
// everything else linear; knob_to_value and value_to_knob are inverses.

#include "mini_test.h"

#include <cmath>

#include <lunar24/core/drone_bank.h>
#include <lunar24/core/knob_taper.h>
#include <lunar24/core/state_disposition.h>

namespace core = lunar24::core;
using core::ParameterId;

namespace {
const core::ParameterDescriptor& desc(ParameterId id) { return *core::find_parameter(id); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}  // namespace

static void envelope_times_are_cubic() {
  for (ParameterId id : {ParameterId::envelope_a_a, ParameterId::envelope_a_d, ParameterId::envelope_a_r,
                         ParameterId::envelope_b_a, ParameterId::envelope_b_d, ParameterId::envelope_b_r}) {
    const auto& d = desc(id);
    CHECK_TRUE(core::knob_taper(id) == core::KnobTaper::Cubic);
    CHECK_TRUE(near(core::knob_to_value(d, 0.0), 0.0, 1e-12));
    CHECK_TRUE(near(core::knob_to_value(d, 1.0), 10.0, 1e-12));
    CHECK_TRUE(near(core::knob_to_value(d, 0.5), 1.25, 1e-9));  // was 5 s on the linear knob
    // Same curve as the drones' ATT knob (which floors at 1 ms).
    CHECK_TRUE(near(core::knob_to_value(d, 0.3), core::DroneBank::mapAttSeconds(0.3), 2e-3));
    // 0-1 s now takes almost half the travel instead of the first 10 %.
    CHECK_TRUE(core::value_to_knob(d, 1.0) > 0.45);
  }
}

static void lfo_rate_is_exponential() {
  for (ParameterId id : {ParameterId::lfo_a_rate, ParameterId::lfo_b_rate}) {
    const auto& d = desc(id);
    CHECK_TRUE(core::knob_taper(id) == core::KnobTaper::Exponential);
    CHECK_TRUE(near(core::knob_to_value(d, 0.0), 0.1, 1e-12));
    CHECK_TRUE(near(core::knob_to_value(d, 1.0), 20.0, 1e-9));
    // Equal turns multiply the rate by equal amounts.
    const double r1 = core::knob_to_value(d, 0.25) / core::knob_to_value(d, 0.0);
    const double r2 = core::knob_to_value(d, 0.75) / core::knob_to_value(d, 0.5);
    CHECK_TRUE(near(r1, r2, 1e-9));
    CHECK_TRUE(core::value_to_knob(d, 1.0) > 0.4);  // 0.1-1 Hz: ~43 % of the travel, was 5 %
  }
}

static void round_trip_and_linear_default() {
  for (ParameterId id : {ParameterId::envelope_a_d, ParameterId::lfo_b_rate, ParameterId::lfo_a_wave}) {
    const auto& d = desc(id);
    for (double p = 0.0; p <= 1.0; p += 0.125)
      CHECK_TRUE(near(core::value_to_knob(d, core::knob_to_value(d, p)), p, 1e-9));
  }
  const auto& wave = desc(ParameterId::lfo_a_wave);
  CHECK_TRUE(core::knob_taper(ParameterId::lfo_a_wave) == core::KnobTaper::Linear);
  CHECK_TRUE(near(core::knob_to_value(wave, 0.5), wave.min + 0.5 * (wave.max - wave.min), 1e-12));
  // Out-of-range input clamps.
  const auto& att = desc(ParameterId::envelope_a_a);
  CHECK_TRUE(near(core::knob_to_value(att, 1.5), 10.0, 1e-12));
  CHECK_TRUE(near(core::value_to_knob(att, -3.0), 0.0, 1e-12));
}

int main() {
  envelope_times_are_cubic();
  lfo_rate_is_exponential();
  round_trip_and_linear_default();
  return ::test::finish("test_knob_taper");
}
