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

// Evidence status for an auditable descriptor.
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

}  // namespace lunar24::core
