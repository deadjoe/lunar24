// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ output-calibration layer (design/00 §P4-⑤, @Claude msg 87862933).
//
// This is the BACK layer of the P4-⑤ two-layer calibration split (design/00
// §P4-⑤ Decis ③). It takes an ALREADY-CONVERGED internal value (the single
// internal pressure, or the pitch CV) and scales it to the PHYSICAL jack voltage
// the connector exposes. It does NOT touch source variance at all — that was
// removed by keyboard_input_normalization.h at the translate() choke before this
// ever runs. Mixing the two layers would let someone wrongly believe output
// calibration explains a source-to-source difference; the front layer alone owns
// that, and this layer only ever sees one value.
//
// Grounding: the manual's calibration menu (solar42N_manual_v15 L1048-1093)
// calibrates the V/OCT and PRESSURE outputs against a bench multimeter at a set
// of target voltages, and sets the DAC voltage-reference source (INTERNAL vs
// EX). The registry models these as scalar deviceState params
// (calibration_v_oct 130, calibration_pressure 131, dac_vref 132).
//
// FINDINGS (never guess-filled, @Claude msgs 87862933 + 90e14dfd):
//   * the manual describes calibration at FOUR per-point voltages (0V, 2V, 5V,
//     8V), but the FROZEN registry models each output as ONE scalar
//     (calibration_v_oct, calibration_pressure). A per-point trim is NOT
//     representable in the frozen id-space without growing parameters, and P5
//     forbids new controls. This layer therefore models each output as a single
//     AFFINE trim derived from that one scalar; the per-point 0/2/5/8V
//     segmentation is recorded as FINDINGS, not guessed into a per-point store
//     the registry does not have.
//   * the exact trim curve and the DAC-reference transfer are PROVISIONAL. The
//     dac_vref selector is carried as context only (no evidenced voltage-domain
//     effect is observable from one scalar), so it does NOT change the voltage
//     here — like MPR121/debounce it is a real, settable, storable setting with
//     no modelled runtime effect (see design/00 §P4-⑤ FINDINGS).
//   * only the SHAPE (one affine scale per output, over the nominal jack range)
//     and the layer separation are asserted; no test asserts the trim curve as
//     design truth.

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// Nominal jack voltage ranges (registry jack.v_oct_out and jack.pressure_out,
// both unipolar cv). Used so a unit internal value lands on a physical volts; the
// exact range is from the registry jack nominal fields.
inline constexpr double kVoctNominalMaxV = 8.0;
inline constexpr double kPressureNominalMaxV = 8.0;

// PROVISIONAL maximum trim, ±: a calibration knob at its neutral gives gain 1.0,
// at the knob's extremes gives 1.0 ± kMaxTrim. Not a measured band (FINDINGS).
inline constexpr double kOutputCalTrimRange = 0.20;

// Decode of the three calibration knobs driving the output layer, read from the
// live `parameters[]` bank. `vOctCalibration` / `pressureCalibration` are the
// single-scalar trim per output (FINDINGS: per-point trim not representable);
// `dacVref` is the 0=INTERNAL / 1=EX selector (carried as context only — see the
// FINDINGS note above).
struct OutputCalibrationConfig {
  double vOctCalibration = 0.5;      // keyboard_calibration_v_oct (130), neutral 0.5
  double pressureCalibration = 0.5;  // keyboard_calibration_pressure (131), neutral 0.5
  std::uint8_t dacVref = 0;          // keyboard_dac_vref (132): 0=internal, 1=external
};

// Read the three calibration knobs from a live DeviceState parameter bank.
// `bank` is a callable (IdValue) -> double, so this layer never depends on the
// DeviceState layout.
template <typename ParamBank>
OutputCalibrationConfig output_calibration_from_bank(ParamBank&& bank) noexcept {
  OutputCalibrationConfig c;
  c.vOctCalibration =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_calibration_v_oct));
  c.pressureCalibration =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_calibration_pressure));
  c.dacVref = static_cast<std::uint8_t>(
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_dac_vref)));
  return c;
}

// Map a norm [0,1] calibration knob to a trim gain, neutral (0.5) -> 1.0.
// PROVISIONAL mono mapping (FINDINGS): the knob's exact trim law is un-evidenced.
inline double calibration_gain(double norm) noexcept {
  const double scaled = (norm - 0.5) * 2.0 * kOutputCalTrimRange;
  return 1.0 + scaled;
}

// Scale an already-normalised pitch CV (internal Volts/octave) to the physical
// V/OCT jack voltage. PROVISIONAL affine trim (FINDINGS): one scalar gain over the
// nominal range; per-point trim / exact curve are un-evidenced.
inline double apply_v_oct_calibration(double internalVolts,
                                      const OutputCalibrationConfig& c) noexcept {
  const double gain = calibration_gain(c.vOctCalibration);
  return internalVolts * gain;
}

// Scale an already-normalised internal pressure in [0,1] to the physical PRESSURE
// jack voltage (nominal 0..kPressureNominalMaxV, unipolar). PROVISIONAL affine
// trim (FINDINGS) — same single-scalar caveat as above.
inline double apply_pressure_calibration(double internalPressure,
                                         const OutputCalibrationConfig& c) noexcept {
  const double gain = calibration_gain(c.pressureCalibration);
  const double base = std::clamp(internalPressure, 0.0, 1.0);
  return base * kPressureNominalMaxV * gain;
}

}  // namespace lunar24::core
