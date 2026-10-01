// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// state_edit.h — small helpers for editing a DeviceStateV1 by name: set a knob, plug or
// unplug a patch cable, pick an effector program. Used by the UI, the MIDI layer and the
// offline render tool. Not for the audio thread (string lookups).

#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/core/state_validation.h>
#include <lunar24/registry.hpp>

namespace lunar24::core {

inline const ParameterDescriptor* find_parameter_by_name(std::string_view stableId) noexcept {
  for (const auto& p : registry::kParameters)
    if (p.stable_id == stableId) return &p;
  return nullptr;
}

inline const JackDescriptor* find_jack_by_name(std::string_view stableId) noexcept {
  for (const auto& j : registry::kJacks)
    if (j.stable_id == stableId) return &j;
  return nullptr;
}

// Accepts "cathedral.1", "program.cathedral.1" or a program name such as "Shimmer"
// (case-sensitive).
inline const ProgramDescriptor* find_program_by_name(std::string_view name) noexcept {
  for (const auto& p : registry::kPrograms) {
    if (p.stable_id == name || p.name == name) return &p;
    if (p.stable_id.size() > 8 && p.stable_id.substr(8) == name) return &p;  // drop "program."
  }
  return nullptr;
}

// Set a parameter, clamped to its registry range and snapped to its step.
inline bool state_set_param(DeviceStateV1& st, ParameterId id, double value) noexcept {
  const ParameterDescriptor* d = find_parameter(id);
  if (d == nullptr || !std::isfinite(value)) return false;
  double v = value < d->min ? d->min : (value > d->max ? d->max : value);
  if (d->step > 0.0) v = d->min + std::round((v - d->min) / d->step) * d->step;
  st.parameters[static_cast<std::uint32_t>(id)] = v;
  // keyboard.behaviour mirrors the keyboard's global single/twin/split setting.
  if (id == ParameterId::keyboard_behaviour)
    st.keyboardSettings.pressureBehaviour = static_cast<std::uint8_t>(v);
  return true;
}

// Set the RIGHT side's copy of a per-side keyboard setting (the bank PLAY = SPLIT reads for the
// right half; the left half and SINGLE / TWIN read parameters[]). Clamped and stepped like
// state_set_param. False for a parameter that has no right-side copy.
inline bool state_set_keyboard_right(DeviceStateV1& st, ParameterId id, double value) noexcept {
  const ParameterDescriptor* d = find_parameter(id);
  const std::int32_t idx = keyboard_scalar_index(id);
  if (d == nullptr || idx < 0 || !std::isfinite(value)) return false;
  double v = value < d->min ? d->min : (value > d->max ? d->max : value);
  if (d->step > 0.0) v = d->min + std::round((v - d->min) / d->step) * d->step;
  st.keyboardScalarRight[static_cast<std::size_t>(idx)] = v;
  return true;
}

// Keep routeOverridden[] coherent with the cables (a normalled route is overridden exactly
// when its sink has a user cable plugged in).
inline void state_sync_routes(DeviceStateV1& st) noexcept {
  for (const auto& r : registry::kNormalizedRoutes) {
    const auto sink = static_cast<std::uint32_t>(r.sinkJack);
    st.routeOverridden[static_cast<std::uint32_t>(r.id)] =
        (sink < kDevicePatchCapacity && st.inputCable[sink] != 0u) ? 1u : 0u;
  }
}

// Plug a cable from an output jack into an input jack. One cable per input (as on the
// hardware); plugging into an occupied input replaces the old cable.
inline bool state_connect(DeviceStateV1& st, JackId source, JackId sink) noexcept {
  const JackDescriptor* s = validate_detail::find_jack(static_cast<std::uint32_t>(source));
  const JackDescriptor* k = validate_detail::find_jack(static_cast<std::uint32_t>(sink));
  if (s == nullptr || k == nullptr) return false;
  if (s->direction != PinDirection::output || k->direction != PinDirection::input) return false;
  const auto i = static_cast<std::uint32_t>(sink);
  if (i >= kDevicePatchCapacity) return false;
  st.inputCable[i] = 1u;
  st.cableSource[i] = source;
  state_sync_routes(st);
  return true;
}

inline bool state_disconnect(DeviceStateV1& st, JackId sink) noexcept {
  const auto i = static_cast<std::uint32_t>(sink);
  if (i >= kDevicePatchCapacity || st.inputCable[i] == 0u) return false;
  st.inputCable[i] = 0u;
  st.cableSource[i] = JackId{0};
  state_sync_routes(st);
  return true;
}

// Remove every cable that starts at `source` (used when dragging a cable off an output).
inline void state_disconnect_source(DeviceStateV1& st, JackId source) noexcept {
  for (std::uint32_t i = 0; i < kDevicePatchCapacity; ++i)
    if (st.inputCable[i] != 0u && st.cableSource[i] == source) {
      st.inputCable[i] = 0u;
      st.cableSource[i] = JackId{0};
    }
  state_sync_routes(st);
}

}  // namespace lunar24::core
