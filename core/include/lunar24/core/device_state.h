// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1: the single whole-device current state (design/07 §6).
//
// It is the ONLY "last-exit state" for the whole machine and stays strictly
// separate from the keyboard's own four presets (which it nevertheless stores,
// because they are keyboard-owned, not whole-device presets).
//
// IMPORTANT — capacities are PROVISIONAL UPPER BOUNDS, not a frozen schema.
// They are not declared final until the full P0 inventory proves them, and the
// on-disk wire format is the separately-declared DeviceStorageSchema (a
// versioned, named record), never a memcpy of this raw C++ layout.

#pragma once

#include <cstddef>
#include <cstdint>

#include <lunar24/core/id_types.h>

namespace lunar24::core {

// Parameter value bank capacity. Must be >= registry kParameterCount.
// PROVISIONAL until the full P0 inventory.
inline constexpr std::size_t kDeviceParamCapacity = 256;

// Patch-cable and normalized-route-override bank capacities. Must be >= registry
// kJackCount and route count. PROVISIONAL until the full P0 inventory.
inline constexpr std::size_t kDevicePatchCapacity = 64;
inline constexpr std::size_t kDeviceRouteCapacity = 32;

// The keyboard subsystem owns exactly four native presets.
inline constexpr std::size_t kDeviceKeyboardPresetCount = 4;

// ---------------------------------------------------------------------------
// Portable storage schema (design/07 §6) — SEPARATE from the raw C++ layout.
//
// The real on-disk format is a versioned record with explicit field names and
// sizes, generated/derived from this descriptor. The in-memory DeviceStateV1
// below is a framework-free working copy; a device-authoritative serialization
// layer is responsible for mapping it to this schema (P0/P2). It is never a
// `memcpy` of the C++ struct across macOS/Windows.
// ---------------------------------------------------------------------------
struct DeviceStorageSchema {
  std::uint32_t schemaVersion = 1;
  std::uint32_t paramCapacity = kDeviceParamCapacity;
  std::uint32_t patchCapacity = kDevicePatchCapacity;
  std::uint32_t routeCapacity = kDeviceRouteCapacity;
  std::uint32_t keyboardPresetCapacity = kDeviceKeyboardPresetCount;
};

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

// Keyboard's current live settings. The manual names 'Behaviour' and 'Pressure
// output' as the two parameters steering the PRESSURE jack; the concrete value
// model is keyboard-subsystem-owned and completed later. PROVISIONAL.
struct KeyboardSettings {
  std::uint8_t pressureBehaviour = 0;  // PROVISIONAL decode
  std::uint8_t pressureOutput = 0;     // PROVISIONAL decode
};

// A KeyboardPreset is owned by the keyboard subsystem only; it is never
// serialized as a whole-device preset. (Declared; populated later.)
struct KeyboardPreset {
  std::uint32_t id = 0;
};

// Dual-effector selection. Each slot holds a cartridge/slot/program selection;
// the indexes reference the (cartridge, program) registry and are validated at
// load. Left/right instances and their non-shared state never merge.
struct EffectorSelection {
  std::uint32_t cartridgeIndex = 0;  // index into the cartridge registry
  std::uint32_t slot = 1;            // slot within the cartridge (1..3)
  std::uint32_t programIndex = 0;    // index into the program registry
};

// Sequencer physical settings only — NOT transient gate/playhead/envelope/delay
// state (design/07 §6 excludes those). The concrete fields are populated once
// the sequencer subsystem is designed; the packed record is PROVISIONAL.
struct SequencerSettings {
  std::uint32_t packedPhysicalSettings = 0;  // PROVISIONAL decode
};

struct DeviceStateV1 {
  std::uint32_t schemaVersion = 1;
  std::uint32_t identityModelVersion = 1;
  UnitIdentitySeed identitySeed;
  CalibrationState calibration;

  // All physical knob/switch parameters, indexed by ParameterId. No allocation.
  double parameters[kDeviceParamCapacity] = {};

  // Patch facts. `inputCable[i]` is nonzero if a user cable is plugged into the
  // i-th input jack; `cableSource[i]` is that cable's source jack id.
  std::uint8_t inputCable[kDevicePatchCapacity] = {};
  JackId cableSource[kDevicePatchCapacity] = {};

  // Normalized-route override per route index.
  std::uint8_t routeOverridden[kDeviceRouteCapacity] = {};

  // Keyboard current settings + its native four presets.
  KeyboardSettings keyboardSettings;
  KeyboardPreset keyboardPresets[kDeviceKeyboardPresetCount] = {};

  // Dual-effector cartridge/program selection (left and right slots).
  EffectorSelection leftEffector;
  EffectorSelection rightEffector;

  // Sequencer physical settings (transient state is deliberately absent).
  SequencerSettings sequencer;
};

}  // namespace lunar24::core
