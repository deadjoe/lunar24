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
// one-past-the-last serialized ParameterId). This is the P0 FINAL parameter
// id-space: the frozen 357-parameter target plus an append-only id plan — new ids
// continue from 376 and the 0..375 holes are never reused. Capacity freeze
// (Codex msg deab14b7), a metadata-only ruling: it does NOT implement the B-tier
// deferred self-oscillating DSP. Id allocation (registry metadata numbering only):
// SYNTEX-1 376-384, DIGITAL 385-393, GENERATOR 394-402, ORCHE 403-411. The
// 12 keyboard-complex params (which would have been 412-423) do NOT land: 8 are
// non-scalar (vector/record/mask — Root A must-gap; they belong in DeviceState
// structured fields per design/07 §6) and 4 are selector-toggles whose frozen
// target omits positions (no evidenced value domain, and the manual gives no
// options). So the last landed id is 411 and kParameterIdSpace == 412.
// The completeness gate PERMANENTLY holds capacity >= idSpace (never tightened to
// ==): the 12 empty slots (424-412) are exactly these structural keyboard-complex
// left-outs, NOT reserved expansion (@Claude fb7841e6, 方案1 land 0 / gap 12).
inline constexpr std::size_t kDeviceParamCapacity = 424;

// Patch-cable / normalized-route-override bank capacities. Must be >= the
// registry id-space of JackId and RouteId respectively. PROVISIONAL until the
// full P0 inventory.
// kDevicePatchCapacity = 67: the frozen patchable-jack inventory is 64 jacks.
// The vco_b registry correction (task #24, append-only per @Claude
// "只追加，绝不重排") adds vco_b.vca_ctl (id 66) for the mirror rule (manual L411:
// VCO A/B mirror each other except sync(A)/vco_out(B)). The other mirror-rule
// candidate vco_b.fm_in (id 65) was REMOVED as over-recorded (task #32, frozen-P0
// removal): manual L388 "linear FM input with attenuator" is the cv jack + the cv
// amt attenuator + the lin/exp switch, NOT a dedicated fm_in — see the FINDINGS
// note. So the frozen jack count is back to 64. APPENDED/REMOVED, never
// renumbering an existing id. The stable serialized JackId space (one-past max id
// 66, vco_b.vca_ctl) is 67 — NOT 64 — because the id-space is max-id+1 and jack
// id 12 is a legacy hole (non-dense): the bank is indexed by serialized JackId,
// so it must be sized to the id-space (67), never to the entity count (64). Sized
// to exactly 67 (not rounded to 96/128): the id space here is final and the ruling
// is precision, not headroom.
inline constexpr std::size_t kDevicePatchCapacity = 67;
inline constexpr std::size_t kDeviceRouteCapacity = 32;

// The keyboard subsystem owns exactly four native presets.
inline constexpr std::size_t kDeviceKeyboardPresetCount = 4;

// Keyboard-owned fixed widths (design/07 §6 non-scalars, and the preset's own
// count/width facts). PROVISIONAL until the keyboard subsystem's full P0
// inventory proves them; the wire widths live in the separately-declared schema.
// Keyboard-owned 16-step sequencer run (P4 keyboard performance system). This is
// NOT the physical 5-step sequencer, which is a SEPARATE P3 control source
// (ModuleId 11, STAGES 3/4/5) whose full persistent state lives in parameters[]
// (pulser rate, clock selector, stage count, 5×step CV, 5×gate-enable — 13
// persistent controls) and only whose transient playhead + currently generated
// gate / clock phase / output are not persisted.
inline constexpr std::size_t kKeyboardSeqStepCount = 16;
inline constexpr std::size_t kKeyboardSeqStepBytes = 6;    // note u8 + value f32 + gate u8, packed
inline constexpr std::size_t kKeyboardSeqBytes = kKeyboardSeqStepCount * kKeyboardSeqStepBytes;  // 96
inline constexpr std::size_t kKeyboardPlateTuneCount = 12; // vector, 1 per plate key
inline constexpr std::size_t kKeyboardPushbuttonCount = 8; // vector, 1 per pushbutton row
inline constexpr std::size_t kKeyboardScaleMaskBytes = 2;  // quantise scale editor: 12-bit mask -> u16

}  // namespace lunar24::core
