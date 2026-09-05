// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ input-normalization layer (design/00 §P4-⑤, @Claude msg 87862933).
//
// This is the FRONT layer of the two-layer P4-⑤ calibration split (design/00
// §P4-⑤ Decis ③). Its single job is to converge a SOURCE-VARYING raw touch
// reading into ONE internal pressure value. Two sources (e.g. the panel's MPR121
// capacitive reading and a pointer/mouse substitute) disagree in raw units; they
// MUST disagree only BEFORE this layer, because the P4 exit criterion compares
// the internal pressure downstream. Therefore normalization is the choke at which
// source variance is removed, and it is the ONLY place the exit criterion allows
// a difference — the difference is eaten here, so a normalized pressure from two
// sources at the same physical touch is bit-identical to the passage below it.
//
// This is deliberately NOT the output-calibration layer (design/00 §P4-⑤ Decis
// ③): input normalization produces the single internal pressure; output
// calibration (keyboard_output_calibration.h) scales that SAME value to the
// physical jack voltage. One is before the choke, one after. They do not overlap
// and must never be conflated — mixing them would let someone wrongly believe the
// output calibration can explain a source-to-source difference, which it cannot.
//
// Grounding: the manual's pressure path (solar42N_manual_v15 L628-635, L601-607)
// says the pressure output is directly proportional to the capacitive sensor
// reading, and the calibration menu's touch/release/pressure-min/max knobs (L1071
// -1081) set the thresholds and the pressure window. The registry models these as
// scalar deviceState params (touch_threshold 133, release_threshold 134,
// pressure_min 135, pressure_max 136).
//
// FINDINGS (never guess-filled, @Claude msgs 87862933 + 90e14dfd):
//   * the raw->pressure MAPPING CURVE is PROVISIONAL. This layer uses a plain
//     linear interpolation between pressure_min and pressure_max as a labelled
//     placeholder ONLY; it is not an evidenced curve and no test asserts it as
//     design truth. It is a placeholder a later slice replaces with a measured
//     curve.
//   * the touch/release HYSTERESIS DIRECTION is PROVISIONAL. The manual names a
//     touch threshold (default 650) and a release / "post-touch" threshold
//     (default 690), but the text does not unambiguously state whether the de-latch
//     bar is the touch or the release figure, and with release (690) ARRANGED ABOVE
//     touch (650) the two readings imply an on-bar below an off-bar — atypical.
//     This layer therefore implements de-latch at the RELEASE threshold and
//     records the direction as FINDINGS, not as proven behaviour. Every test pins
//     behaviour to the exact configured bars so a later change with sensor data
//     flips a test, not the semantics silently.

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// Decode of the four calibration knobs driving pressure normalization, read from
// the live `parameters[]` bank. Widths are raw u8/f32 per the registry; a normal
// use reads them out of DeviceStateV1.parameters at these ParameterIds. The
// decode is kept here so the normalizer stays a pure (config, raw) function.
struct PressureNormalizationConfig {
  double touchThreshold = 0.0;   // keyboard_touch_threshold (133)
  double releaseThreshold = 0.0;  // keyboard_release_threshold (134)
  double pressureMin = 0.0;       // keyboard_pressure_min (135)
  double pressureMax = 0.0;       // keyboard_pressure_max (136)
};

// Read the four calibration knobs from a live DeviceState parameter bank. `bank`
// is a callable (IdValue) -> double, so this layer never depends on the
// DeviceState layout; a caller can pass `[&](id){ return state.parameters[id]; }`.
template <typename ParamBank>
PressureNormalizationConfig pressure_config_from_bank(ParamBank&& bank) noexcept {
  PressureNormalizationConfig c;
  c.touchThreshold =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_touch_threshold));
  c.releaseThreshold =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_release_threshold));
  c.pressureMin =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_pressure_min));
  c.pressureMax =
      std::forward<ParamBank>(bank)(static_cast<IdValue>(ParameterId::keyboard_pressure_max));
  return c;
}

// The pressure normalizer. Stateful only for the touch/release HOLD latch: once a
// touch latches, it stays latched until the raw reading falls below the RELEASE
// threshold (which is higher than the touch threshold), not immediately below the
// touch threshold. That hysteresis is the evidence-backed part. The value map is
// the PROVISIONAL linear placeholder — see the file FINDINGS.
class PressureNormalizer {
 public:
  explicit PressureNormalizer(PressureNormalizationConfig cfg) : cfg_(cfg) {}

  // Feed one raw capacitive/touch reading; returns the internal pressure in
  // [0, 1]. A fresh touch registers at the TOUCH threshold; once latched, a value
  // below the RELEASE threshold de-registers (a double-threshold hold). The
  // de-latch bar direction is PROVISIONAL (see FINDINGS). A latched touch maps
  // [pressureMin, pressureMax] linearly onto [0, 1].
  double process(double rawTouch) noexcept {
    const bool touched = lastTouched_ ? (rawTouch >= cfg_.releaseThreshold)
                                      : (rawTouch >= cfg_.touchThreshold);
    lastTouched_ = touched;
    if (!touched) return 0.0;
    // PROVISIONAL linear placeholder curve (FINDINGS) — the actual curve is un-
    // evidenced and must not be treated as design truth.
    const double span = cfg_.pressureMax - cfg_.pressureMin;
    if (span <= 0.0) return 0.0;  // degenerate window: no readable pressure
    const double p = (rawTouch - cfg_.pressureMin) / span;
    return std::clamp(p, 0.0, 1.0);
  }

  bool latched() const noexcept { return lastTouched_; }
  void reset() noexcept { lastTouched_ = false; }

 private:
  PressureNormalizationConfig cfg_;
  bool lastTouched_ = false;
};

}  // namespace lunar24::core
