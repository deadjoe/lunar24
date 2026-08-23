// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Fixed state-bank / storage capacities (design/07 §6). These are PROVISIONAL
// UPPER BOUNDS, not a frozen schema — they are not declared final until the full
// P0 inventory proves them, and the wire format is the separately-declared
// DeviceStorageSchema (versioned, named record), never a memcpy of a C++ layout.
//
// They live in their own header so the generated registry can compute the
// serialized-id SPACE and statically assert that every state bank is large
// enough for the ids it will be indexed by — an id can never silently exceed a
// bank and read/write out of bounds in a future slice.

#pragma once

#include <cstddef>

namespace lunar24::core {

// Parameter value bank capacity. Must be >= registry kParameterIdSpace (the
// one-past-the-last serialized ParameterId). PROVISIONAL until the full P0
// inventory.
inline constexpr std::size_t kDeviceParamCapacity = 256;

// Patch-cable / normalized-route-override bank capacities. Must be >= the
// registry id-space of JackId and RouteId respectively. PROVISIONAL until the
// full P0 inventory.
inline constexpr std::size_t kDevicePatchCapacity = 64;
inline constexpr std::size_t kDeviceRouteCapacity = 32;

// The keyboard subsystem owns exactly four native presets.
inline constexpr std::size_t kDeviceKeyboardPresetCount = 4;

}  // namespace lunar24::core
