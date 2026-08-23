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
// one-past-the-last serialized ParameterId). This is now the P0 FINAL parameter
// id-space: the frozen 357-parameter target plus an append-only id plan — new ids
// continue from 376 and the 0..375 holes are never reused — so the space is
// one-past last id 423 = 424. Capacity freeze (Codex msg deab14b7), a
// metadata-only ruling: it does NOT implement the B-tier deferred self-oscillating
// DSP. Future id allocation (registry metadata numbering only): SYNTEX-1 376-384,
// DIGITAL 385-393, GENERATOR 394-402, ORCHE 403-411, 12 keyboard complex 412-423.
// The completeness gate stays capacity >= id-space until the full inventory lands
// (then it is tightened to ==), so this slice never fakes early full coverage.
inline constexpr std::size_t kDeviceParamCapacity = 424;

// Patch-cable / normalized-route-override bank capacities. Must be >= the
// registry id-space of JackId and RouteId respectively. PROVISIONAL until the
// full P0 inventory.
// kDevicePatchCapacity = 65: the frozen patchable-jack inventory is 64 jacks, and
// the DRONE 6 slice (Codex msg cc68ab2b — Option A narrow release) completes 64/64 of them. Their stable
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
