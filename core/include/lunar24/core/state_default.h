// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1 factory default (design/07 §9, task #75 revision 2, ruling ① /
// correction 1).
//
// make_default_device_state(seed) builds a power-on default that is a VALID
// structured state, not aggregate-zero-legal. `seed` is the unit identity seed and
// is REQUIRED (no defaulted argument, no 0==uninitialized sentinel):
// state.identitySeed.seed equals the caller's argument exactly. The host supplies
// its current provisional startup seed when there is no persistent state; nothing
// in core assumes a fixed startup seed.
//
// The default:
//  * sets every landed parameter to its registry factory initial (holes/slack stay
//    0 — the canonical value), so it passes range + slot validation by construction;
//  * forms the four native preset slots as valid (id == slot, full factory payload);
//  * fills the LIVE bilateral keyboard banks and mirrors the left into the right
//    (single truth for the scalar bank stays in parameters[]; keyboardScalarRight
//    mirrors it), giving pressure-mirror convergence;
//  * selects a legal L/R ProgramId (cathedral.1 == ProgramId{0}, the first landed
//    program).

#pragma once

#include <cstdint>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_presets.h>      // initial_keyboard_preset
#include <lunar24/core/keyboard_side_bank.h>     // kKeyboardScalarParameterIds
#include <lunar24/core/state_disposition.h>      // find_parameter, find_program
#include <lunar24/core/unit_identity_profile.h>  // kIdentityModelVersionSupported
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

inline DeviceStateV1 make_default_device_state(std::uint64_t seed) noexcept {
  DeviceStateV1 st{};  // value-init: schema v5, calibration 1.0, everything zeroed
  st.identitySeed.seed = seed;
  st.identityModelVersion = kIdentityModelVersionSupported;  // explicit, not value-init

  // Landed parameters <- registry factory initial; holes/slack (the dispose table
  // classifies them invalid_unlanded) stay at their canonical 0.
  for (std::uint32_t i = 0; i < kDeviceParamCapacity; ++i) {
    const ParameterId pid = static_cast<ParameterId>(i);
    const ParameterDescriptor* d = find_parameter(pid);
    if (d != nullptr) st.parameters[i] = d->initial;
  }

  // Converge the #57 keyboard mirrors EXPLICITLY (they are otherwise value-init 0
  // and the registry mirror params are selector-initial 0, but we pin the invariant
  // here rather than rely on coincidence):
  //   parameters[keyboard_behaviour]      == KeyboardSettings.pressureBehaviour (canonical)
  //   parameters[keyboard_pressure_output]== KeyboardSettings.pressureOutput    (canonical LEFT)
  st.parameters[static_cast<std::uint32_t>(ParameterId::keyboard_behaviour)] =
      static_cast<double>(st.keyboardSettings.pressureBehaviour);
  st.parameters[static_cast<std::uint32_t>(ParameterId::keyboard_pressure_output)] =
      static_cast<double>(st.keyboardSettings.pressureOutput);

  // Four native preset slots: valid id (== slot) + full factory payload.
  for (std::uint32_t k = 0; k < kDeviceKeyboardPresetCount; ++k) {
    st.keyboardPresets[k] = initial_keyboard_preset(k);
  }

  // Live per-side scalar bank: the RIGHT mirrors the LEFT (which lives in
  // parameters[]); under single/twin the right bank is never read, under split it
  // reads this mirror. Convergence is explicit and reads the CONVERGED left above.
  for (std::uint32_t j = 0; j < kKeyboardScalarRightCount; ++j) {
    const ParameterId pid = kKeyboardScalarParameterIds[j];
    st.keyboardScalarRight[j] = st.parameters[static_cast<std::uint32_t>(pid)];
  }

  // Bilateral LIVE non-scalars: mirror the left bank into the right so the state is
  // complete under split and never diverges from the left under single/twin.
  st.keyboardSeqCurrentR = st.keyboardSeqCurrent;
  st.keyboardScaleEditorR = st.keyboardScaleEditor;
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    st.keyboardPlateTuneR[i] = st.keyboardPlateTune[i];
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    st.keyboardPushbuttonR[i] = st.keyboardPushbutton[i];
  for (std::uint32_t i = 0; i < 4u; ++i)
    st.keyboardClockSelectorsR[i] = st.keyboardClockSelectors[i];

  // Legal L/R dual-effector selection by stable ProgramId (cathedral.1), not a
  // dense 0 sentinel.
  st.leftEffector.program = ProgramId::program_cathedral_1;
  st.rightEffector.program = ProgramId::program_cathedral_1;

  return st;
}

}  // namespace lunar24::core
