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
// inventory. Bumped past Phase B DRONE 4 (Codex msg 483a9dc5): the landed param
// id-space is now 258 (max id drone_4.gate_hold 257), and the value is sized to
// also cover the remaining declared drone_3/5/6 + keyboard complex params.
inline constexpr std::size_t kDeviceParamCapacity = 384;

// Patch-cable / normalized-route-override bank capacities. Must be >= the
// registry id-space of JackId and RouteId respectively. PROVISIONAL until the
// full P0 inventory.
// kDevicePatchCapacity = 65: the frozen patchable-jack inventory is 64 jacks, and
// the DRONE 6 slice (Codex msg 2a4b0c19) completes 64/64 of them. Their stable
// serialized JackId space (one-past max id 64) is 65 — NOT 64 — because the
// id-space is max-id+1 and jack id 12 is a legacy hole (non-dense): the bank is
// indexed by serialized JackId, so it must be sized to the id-space (65), never
// to the entity count (64). Sized to exactly 65 (not rounded to 96/128): the id
// space here is final and the ruling is precision, not headroom.
inline constexpr std::size_t kDevicePatchCapacity = 65;
inline constexpr std::size_t kDeviceRouteCapacity = 32;

// The keyboard subsystem owns exactly four native presets.
inline constexpr std::size_t kDeviceKeyboardPresetCount = 4;

}  // namespace lunar24::core
