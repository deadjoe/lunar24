// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_format.h — parameter values as the panel shows them (knob readout, keyboard menu).
// Framework-free: shared by the IGraphics editor and the SVG preview.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <lunar24/core/arp_sequencer.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/registry.hpp>

namespace lunar24::host {

inline std::string formatValue(const core::ParameterDescriptor* d, double v) {
  char b[48];
  if (d == nullptr) return "";
  const std::string unit(d->unit);
  if (unit == "norm") std::snprintf(b, sizeof b, "%.0f%%", 100.0 * (v - d->min) / (d->max - d->min));
  else if (unit == "seconds") std::snprintf(b, sizeof b, v < 1.0 ? "%.0f ms" : "%.2f s", v < 1.0 ? v * 1000.0 : v);
  else if (unit == "hz") std::snprintf(b, sizeof b, "%.2f Hz", v);
  else if (unit == "oct") std::snprintf(b, sizeof b, "%+.2f oct", v);
  else if (unit == "volts") std::snprintf(b, sizeof b, "%.2f V", v);
  else std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

// Keyboard menu values in the units the manual uses (pp.16-19); other parameters fall
// back to formatValue.
inline std::string formatParam(std::uint32_t id, double v) {
  using P = core::ParameterId;
  char b[32];
  const auto n = [v](double top) { return int(std::lround(std::clamp(v, 0.0, 1.0) * top)); };
  const auto count = [&b](int k, const char* one, const char* many) {
    std::snprintf(b, sizeof b, "%d %s", k, k == 1 ? one : many);
    return std::string(b);
  };
  switch (static_cast<P>(id)) {
    case P::keyboard_clock_bpm: std::snprintf(b, sizeof b, "%.0f BPM", 10.0 + 290.0 * std::clamp(v, 0.0, 1.0)); return b;
    case P::keyboard_root_note: {
      static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      return kNames[n(11)];
    }
    case P::keyboard_arp_interval: return count(int(core::arp_interval_semitones(v)), "semitone", "semitones");
    case P::keyboard_arp_length: return count(int(core::arp_length_steps(v)), "step", "steps");
    case P::keyboard_seq_length: return count(int(core::seq_length_steps(v)), "step", "steps");
    case P::keyboard_seq_rhythm_length: return count(int(core::seq_rhythm_length_steps(v)), "step", "steps");
    case P::keyboard_portamento_speed:
    case P::keyboard_pressure_rise:
    case P::keyboard_pressure_fall: std::snprintf(b, sizeof b, "%d", n(255)); return b;
    case P::keyboard_vibrato_speed:
    case P::keyboard_vibrato_depth:
    case P::keyboard_vibrato_delay:
    case P::keyboard_vibrato_pressure: std::snprintf(b, sizeof b, "%d", n(127)); return b;
    default: return formatValue(core::find_parameter(static_cast<core::ParameterId>(id)), v);
  }
}

}  // namespace lunar24::host
