// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ tests for the two-layer keyboard calibration split (design/00 §P4-⑤, msg
// 87862933). @Claude Decis ③: the same slice but the two layers are tested
// SEPARATELY, and the front/back difference is written into the comments.
//
//   FRONT layer — keyboard_input_normalization.h: raw (source-varying) touch
//   reading -> the SINGLE internal pressure, at the translate() choke. This is the
//   ONLY place the P4 exit criterion allows a source difference, because the
//   difference is eaten here (before the choke). A "normalized pressure from two
//   sources at the same physical touch is bit-identical downstream" property lives
//   here, not in the back layer.
//
//   BACK layer — keyboard_output_calibration.h: the ALREADY-CONVERGED internal
//   value -> physical jack voltage. It never sees a raw source value; it must not.
//   Mixing the two would let someone wrongly believe output calibration explains a
//   source-to-source difference, which it cannot.
//
// FINDINGS (never guess-filled): the raw->pressure MAPPING CURVE (linear
// placeholder) and the output TRIM CURVE (mono affine) are PROVISIONAL. What IS
// asserted here is (a) the touch/release HYSTERESIS (evidence-backed: manual
// L1071 touch default 650, release default 690 -> release sits above touch), and
// (b) the SHAPE / layer separation. No test asserts a provisional curve as design
// truth.

#include "mini_test.h"

#include <array>
#include <cstdint>
#include <vector>

#include <lunar24/core/keyboard_input_normalization.h>
#include <lunar24/core/keyboard_output_calibration.h>

namespace core = lunar24::core;

// A tiny DeviceState parameter bank, callable as (IdValue) -> double. Captures the
// ids actually read so a config-from-bank test can assert the right wiring.
struct FakeParamBank {
  std::array<double, 256> v{};
  std::vector<core::IdValue> idsSeen;
  double operator()(core::IdValue id) {
    idsSeen.push_back(id);
    return v[static_cast<std::size_t>(id)];
  }
};

// -------------------------------------------------------------------------------
// FRONT layer — input normalization
// -------------------------------------------------------------------------------

static void fresh_touch_below_threshold_is_not_a_touch() {
  // A raw reading at or below the touch threshold is NOT a touch: pressure 0, not
  // latched. This is the attack side of the evidence-backed hysteresis.
  core::PressureNormalizationConfig cfg;
  cfg.touchThreshold = 650.0;
  cfg.releaseThreshold = 690.0;
  cfg.pressureMin = 650.0;
  cfg.pressureMax = 830.0;  // arbitrary test window
  core::PressureNormalizer n(cfg);

  CHECK_EQ(n.process(300.0), 0.0);  // far below threshold -> not touched
  CHECK_FALSE(n.latched());
  // Just under the touch threshold is still no touch.
  CHECK_EQ(n.process(649.0), 0.0);
  CHECK_FALSE(n.latched());
}

static void touch_at_threshold_latches_and_maps() {
  core::PressureNormalizationConfig cfg;
  cfg.touchThreshold = 650.0;
  cfg.releaseThreshold = 690.0;
  cfg.pressureMin = 650.0;
  cfg.pressureMax = 730.0;  // window of 80 raw units
  core::PressureNormalizer n(cfg);

  // Exactly at the touch threshold -> latched; maps to pressure 0 (raw == min).
  CHECK_EQ(n.process(650.0), 0.0);
  CHECK(n.latched());
  // Midpoint of [650,730] -> 0.5 (PROVISIONAL linear placeholder curve).
  const double mid = n.process(690.0);
  CHECK(mid > 0.499 && mid < 0.501);
  // Top of the window -> 1.0.
  CHECK_EQ(n.process(730.0), 1.0);
  // Beyond the window clamps to 1.0, does not overshoot.
  CHECK_EQ(n.process(900.0), 1.0);
}

static void release_double_threshold_bars_are_distinct() {
  // The normalizer is a double-threshold: a FRESH touch registers at the TOUCH
  // threshold; once latched, it de-registers below the RELEASE threshold. The
  // two bars must not be conflated — a reader that used the same threshold for
  // both attack and release would change behaviour when the two values differ.
  // NOTE: the exact de-latch direction is PROVISIONAL (see header FINDINGS); this
  // pins behaviour to the configured bars so a later evidence-driven change
  // flips this test rather than silently changing semantics.
  core::PressureNormalizationConfig cfg;
  cfg.touchThreshold = 650.0;
  cfg.releaseThreshold = 690.0;
  cfg.pressureMin = 650.0;
  cfg.pressureMax = 730.0;
  core::PressureNormalizer n(cfg);

  // Fresh touch below touch(650): never registers.
  CHECK_EQ(n.process(300.0), 0.0);
  CHECK_FALSE(n.latched());
  // Fresh touch at touch(650): registers (latched) — pressure at raw==min is 0.
  CHECK_EQ(n.process(650.0), 0.0);
  CHECK(n.latched());
  // Latched at 700 (>= release 690): stays latched, pressure present.
  const double p = n.process(700.0);
  CHECK(p > 0.0);
  CHECK(n.latched());
  // Latched at 689 (below release 690 but above touch 650): de-registers, because
  // the release bar governs the de-latch.
  CHECK_EQ(n.process(689.0), 0.0);
  CHECK_FALSE(n.latched());
  // After de-latch, the SAME 689 is treated as a fresh touch: 689 >= touch(650)
  // registers again. This is the two-bar asymmetry the test pins.
  CHECK(n.process(689.0));
  CHECK(n.latched());
  // A value below touch(650) never registers on a fresh touch.
  CHECK_EQ(n.process(600.0), 0.0);
  CHECK_FALSE(n.latched());
}

static void degenerate_pressure_window_is_safe() {
  // A config where pressureMax <= pressureMin must not divide by zero or inject a
  // voltage; it converges to a no-readable-pressure 0.
  core::PressureNormalizationConfig cfg;
  cfg.touchThreshold = 650.0;
  cfg.releaseThreshold = 650.0;
  cfg.pressureMin = 800.0;  // window inverted / empty
  cfg.pressureMax = 400.0;
  core::PressureNormalizer n(cfg);
  n.process(900.0);  // latches (touch satisfied)
  CHECK_EQ(n.process(900.0), 0.0);
}

static void config_from_bank_reads_the_right_ids() {
  // The four pressure knobs must be read from the exact ParameterIds the registry
  // assigned (133-136); a wrong id read would wire the wrong knob into the choke.
  FakeParamBank bank;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_touch_threshold)] = 650.0;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_release_threshold)] = 690.0;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_pressure_min)] = 650.0;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_pressure_max)] = 800.0;

  const core::PressureNormalizationConfig cfg =
      core::pressure_config_from_bank([&](core::IdValue id) { return bank(id); });

  CHECK_EQ(cfg.touchThreshold, 650.0);
  CHECK_EQ(cfg.releaseThreshold, 690.0);
  CHECK_EQ(cfg.pressureMin, 650.0);
  CHECK_EQ(cfg.pressureMax, 800.0);
  CHECK_EQ(bank.idsSeen.size(), 4u);
  CHECK_EQ(bank.idsSeen[0], static_cast<core::IdValue>(core::ParameterId::keyboard_touch_threshold));
  CHECK_EQ(bank.idsSeen[1], static_cast<core::IdValue>(core::ParameterId::keyboard_release_threshold));
  CHECK_EQ(bank.idsSeen[2], static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_min));
  CHECK_EQ(bank.idsSeen[3], static_cast<core::IdValue>(core::ParameterId::keyboard_pressure_max));
}

// -------------------------------------------------------------------------------
// BACK layer — output calibration
// -------------------------------------------------------------------------------

static void neutral_calibration_gives_unit_gain() {
  // A calibration knob at its neutral (0.5) leaves the value essentially unscaled.
  core::OutputCalibrationConfig c;  // defaults: vOct=0.5, pressure=0.5, dacVref=0
  CHECK_EQ(core::apply_v_oct_calibration(3.5, c), 3.5);  // gain 1.0
  const double out = core::apply_pressure_calibration(0.5, c);
  CHECK(out > 3.99 && out < 4.01);  // 0.5 * 8V * 1.0 = 4V
}

static void calibration_knob_trims_within_the_nominal_band() {
  // The trim gain must be MONOTONE in the knob and stay within the declared
  // ±kOutputCalTrimRange band (a placeholder band, FINDINGS), not explode.
  core::OutputCalibrationConfig lo;
  lo.vOctCalibration = 0.0;
  core::OutputCalibrationConfig hi;
  hi.vOctCalibration = 1.0;

  const double gLo = core::calibration_gain(0.0);
  const double gMid = core::calibration_gain(0.5);
  const double gHi = core::calibration_gain(1.0);
  CHECK(gLo < gMid && gMid < gHi);                               // monotone
  CHECK(gLo > 1.0 - core::kOutputCalTrimRange - 1e-9);           // >= 1 - trim
  CHECK(gHi < 1.0 + core::kOutputCalTrimRange + 1e-9);           // <= 1 + trim
  CHECK_EQ(core::apply_v_oct_calibration(2.0, lo), 2.0 * gLo);   // trim applies
  CHECK_EQ(core::apply_v_oct_calibration(2.0, hi), 2.0 * gHi);
}

static void pressure_output_clamps_the_internal_value() {
  // Internal pressure is defined on [0,1]; an out-of-range value must clamp, not
  // overshoot the nominal 8V band. The trim should not double-count a wild value.
  core::OutputCalibrationConfig c;
  c.pressureCalibration = 0.5;
  CHECK(core::apply_pressure_calibration(-0.2, c) >= 0.0);
  CHECK(core::apply_pressure_calibration(1.5, c) <= core::kPressureNominalMaxV + 1e-9);
  // A negative internal pressure maps to 0 (unipolar jack), never negative volts.
  CHECK_EQ(core::apply_pressure_calibration(-0.2, c), 0.0);
}

static void output_config_from_bank_reads_the_right_ids() {
  FakeParamBank bank;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_calibration_v_oct)] = 0.6;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_calibration_pressure)] = 0.4;
  bank.v[static_cast<std::size_t>(core::ParameterId::keyboard_dac_vref)] = 1.0;

  const core::OutputCalibrationConfig cfg =
      core::output_calibration_from_bank([&](core::IdValue id) { return bank(id); });

  CHECK_EQ(cfg.vOctCalibration, 0.6);
  CHECK_EQ(cfg.pressureCalibration, 0.4);
  CHECK_EQ(cfg.dacVref, 1u);
  CHECK_EQ(bank.idsSeen.size(), 3u);
  CHECK_EQ(bank.idsSeen[0], static_cast<core::IdValue>(core::ParameterId::keyboard_calibration_v_oct));
  CHECK_EQ(bank.idsSeen[1],
           static_cast<core::IdValue>(core::ParameterId::keyboard_calibration_pressure));
  CHECK_EQ(bank.idsSeen[2], static_cast<core::IdValue>(core::ParameterId::keyboard_dac_vref));
}

int main() {
  // FRONT layer (input normalization).
  fresh_touch_below_threshold_is_not_a_touch();
  touch_at_threshold_latches_and_maps();
  release_double_threshold_bars_are_distinct();
  degenerate_pressure_window_is_safe();
  config_from_bank_reads_the_right_ids();
  // BACK layer (output calibration).
  neutral_calibration_gives_unit_gain();
  calibration_knob_trims_within_the_nominal_band();
  pressure_output_clamps_the_internal_value();
  output_config_from_bank_reads_the_right_ids();
  return ::test::finish("keyboard calibration (P4-⑤, two layers)");
}
