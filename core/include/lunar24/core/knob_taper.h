// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Knob taper: how far a knob is turned (0..1) <-> the parameter value it sets. Most knobs are
// linear over the descriptor's min..max. Time and rate knobs are not: on a linear 0-10 s knob,
// 0-1 s is only the first 10 % of the travel, and on a linear 0.1-20 Hz LFO the slow
// 0.1-1 Hz range is only the first 5 %.
//   * Envelope A/B A, D, R: cubic, the same curve as the drones' ATT / RLS
//     (DroneBank::mapAttSeconds): the knob's middle is 1.25 s.
//   * LFO A/B RATE: exponential, equal turns multiply the rate by equal steps; the middle
//     is 1.4 Hz.
// Values stay in their units (seconds, Hz), so saved states are unchanged; only the panel knob,
// MIDI controllers and host automation go through this map.

#pragma once

#include <algorithm>
#include <cmath>

#include <lunar24/core/descriptors.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

enum class KnobTaper { Linear, Cubic, Exponential };

constexpr KnobTaper knob_taper(ParameterId id) noexcept {
  switch (id) {
    case ParameterId::envelope_a_a: case ParameterId::envelope_a_d: case ParameterId::envelope_a_r:
    case ParameterId::envelope_b_a: case ParameterId::envelope_b_d: case ParameterId::envelope_b_r:
      return KnobTaper::Cubic;
    case ParameterId::lfo_a_rate: case ParameterId::lfo_b_rate:
      return KnobTaper::Exponential;
    default:
      return KnobTaper::Linear;
  }
}

// Knob position (0..1, clamped) -> parameter value.
inline double knob_to_value(const ParameterDescriptor& d, double pos) noexcept {
  const double p = std::clamp(pos, 0.0, 1.0);
  switch (knob_taper(d.id)) {
    case KnobTaper::Cubic: return d.min + p * p * p * (d.max - d.min);
    case KnobTaper::Exponential:
      if (d.min > 0.0 && d.max > d.min) return d.min * std::pow(d.max / d.min, p);
      break;
    case KnobTaper::Linear: break;
  }
  return d.min + p * (d.max - d.min);
}

// Parameter value -> knob position (0..1), the inverse of knob_to_value.
inline double value_to_knob(const ParameterDescriptor& d, double v) noexcept {
  if (!(d.max > d.min)) return 0.0;
  const double x = std::clamp(v, d.min, d.max);
  switch (knob_taper(d.id)) {
    case KnobTaper::Cubic: return std::cbrt((x - d.min) / (d.max - d.min));
    case KnobTaper::Exponential:
      if (d.min > 0.0) return std::log(x / d.min) / std::log(d.max / d.min);
      break;
    case KnobTaper::Linear: break;
  }
  return (x - d.min) / (d.max - d.min);
}

}  // namespace lunar24::core
