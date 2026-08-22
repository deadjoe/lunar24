// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1: the single whole-device current state (design/07 §6).
//
// Schema + version are frozen here at P0. It is the ONLY "last-exit state" for
// the whole machine and must stay strictly separate from the keyboard's own four
// presets. Contains: all physical knob/switch parameters, patch cables and
// normalized-route overrides, keyboard settings + its 4 KeyboardPreset,
// dual-effector cartridge/program, sequencer physical settings (NOT transient
// gate/playhead/envelope/delay state), plus persistent UnitIdentitySeed,
// calibration state and schema version.

#pragma once

#include <cstddef>
#include <cstdint>

#include <lunar24/core/id_types.h>

namespace lunar24::core {

// Capacity of the fixed parameter value bank. Runtime has no heap allocation;
// the bank is a fixed-size array indexed by ParameterId. This constant must be
// >= registry kParameterCount (validated by a P0 test).
inline constexpr std::size_t kDeviceParamCapacity = 256;

// Maximum number of patch connections and normalized-route overrides stored.
inline constexpr std::size_t kDevicePatchCapacity = 64;
inline constexpr std::size_t kDeviceRouteCapacity = 32;

// Fixed per-unit constitution, not re-randomized per launch (design/07 §7).
struct UnitIdentitySeed {
  std::uint64_t seed = 0;
};

// Persistent per-unit calibration. Left/right channel normalization and
// level-dependent gain staging are independent.
struct CalibrationState {
  float vcfLeftTrim = 1.0f;
  float vcfRightTrim = 1.0f;
};

// A KeyboardPreset is owned by the keyboard subsystem only; it is never
// serialized as a whole-device preset. (Declared; populated in P4.)
struct KeyboardPreset {
  std::uint32_t id = 0;
};

struct DeviceStateV1 {
  std::uint32_t schemaVersion = 1;
  std::uint32_t identityModelVersion = 1;
  UnitIdentitySeed identitySeed;
  CalibrationState calibration;

  // Fixed parameter value bank, indexed by ParameterId. No allocation.
  double parameters[kDeviceParamCapacity] = {};

  // Patch facts. `inputCable[i]` is nonzero if a user cable is plugged into the
  // i-th input jack; `cableSource[i]` is that cable's source jack id. Sizes must
  // be >= registry kJackCount (validated by a P0 test).
  std::uint8_t inputCable[kDevicePatchCapacity] = {};
  JackId cableSource[kDevicePatchCapacity] = {};

  // Normalized-route override per route index (>= registry route count).
  std::uint8_t routeOverridden[kDeviceRouteCapacity] = {};
};

}  // namespace lunar24::core
