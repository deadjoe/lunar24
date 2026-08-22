// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared category enums for the machine registry. Each enumerator declared here
// is serialized by value and must not be reordered once released.

#pragma once

#include <cstdint>

namespace lunar24::core {

// How a continuous parameter moves toward a new target.
enum class Smoothing : std::uint8_t {
  none,      // switch / discrete index — no smoothing
  linear,    // linear ramp over a short fixed window
  seconds,   // first-order smoothing with a time constant in seconds
};

// Where a parameter's live value lives across sessions.
enum class Persistence : std::uint8_t {
  transient,    // runtime-only, not saved (e.g. modulation depth relative to patching)
  preset,       // belongs to a keyboard/processor preset context
  deviceState,  // part of the single whole-device state (DeviceStateV1)
};

// Evidence status for an auditable descriptor or a single field within it.
enum class EvidenceStatus : std::uint8_t {
  confirmed,    // directly evidenced by a primary source
  unverified,   // explicitly unknown pending first-hand evidence
  provisional,  // documented assumption, must be frozen before P2
};

// Recommended signal use of a jack (advisory; cross-type patching is allowed).
enum class SignalType : std::uint8_t {
  audio,
  cv,
  gate,
  clock,
};

// Electrical polarity of a jack.
enum class Polarity : std::uint8_t {
  unipolar,  // 0 … +V
  bipolar,   // -V … +V
};

// AC / DC coupling of a jack.
enum class Coupling : std::uint8_t {
  ac,
  dc,
};

// Jack signal direction on the physical panel.
enum class PinDirection : std::uint8_t {
  input,
  output,
};

// Role of a parameter within its owning processor program.
enum class ParamRole : std::uint8_t {
  knob,  // a plain module knob/switch
  x,     // effector program X
  y,     // effector program Y
  z,     // effector program Z
};

// Per-volt modulation depth / transfer curve at a jack (design/07 §3). "Every
// JackDescriptor must define polarity, nominal/tolerated range, DC coupling,
// per-volt modulation depth or transfer curve, ...".
enum class SignalTransfer : std::uint8_t {
  linear,       // depth scales linearly with CV (1 V → 1 unit depth)
  exponential,  // depth scales exponentially (e.g. V/oct pitch tracking)
  none,         // no CV grooming / direct pass-through
  unknown,      // not yet evidenced
};

// Input saturation / rail behaviour at a jack (design/07 §3, §5). Hardware rail
// behaviour is modelled only where evidence exists; it is NOT a global [-1,1]
// clamp. We never invent a soft clipper for "safety".
enum class SaturationType : std::uint8_t {
  none,     // no modelled rail / saturation stage
  hard,     // hard clamp at the rail
  soft,     // soft saturation stage
  unknown,  // not yet evidenced
};

}  // namespace lunar24::core
