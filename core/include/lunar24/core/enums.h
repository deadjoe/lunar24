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
  none = 0,      // switch / discrete index — no smoothing
  linear = 1,    // linear ramp over a short fixed window
  seconds = 2,   // first-order smoothing with a time constant in seconds
};

// Where a parameter's live value lives across sessions.
enum class Persistence : std::uint8_t {
  transient = 0,    // runtime-only, not saved (e.g. modulation depth relative to patching)
  preset = 1,       // belongs to a keyboard/processor preset context
  deviceState = 2,  // part of the single whole-device state (DeviceStateV1)
};

// Evidence status for an auditable descriptor or a single field within it.
enum class EvidenceStatus : std::uint8_t {
  confirmed = 0,    // directly evidenced by a primary source
  unverified = 1,   // explicitly unknown pending first-hand evidence
  provisional = 2,  // documented assumption, must be frozen before P2
};

// Recommended signal use of a jack (advisory; cross-type patching is allowed).
// `unknown` is appended (never reorders existing values): it is the honest record
// for a jack whose recommended signal use is not evidenced by the manual/panel.
// unknown-enum biconditional (Codex 587f5e72): value == `unknown` ⇔ field evidence ==
// `unverified`. A concrete value (audio/cv/gate/clock) can carry only confirmed/provisional
// provenance — never `unverified`, which would present a guess as a fact. Signal class is
// NEVER inferred from a stable-id suffix.
enum class SignalType : std::uint8_t {
  audio = 0,
  cv = 1,
  gate = 2,
  clock = 3,
  unknown = 4,  // not evidenced; value==unknown ⇔ evidence==unverified
};

// Electrical polarity of a jack. `unknown` is appended, same biconditional contract as above.
enum class Polarity : std::uint8_t {
  unipolar = 0,  // 0 … +V
  bipolar = 1,   // -V … +V
  unknown = 2,   // not evidenced; value==unknown ⇔ evidence==unverified
};

// AC / DC coupling of a jack. `unknown` is appended, same biconditional contract as above.
enum class Coupling : std::uint8_t {
  ac = 0,
  dc = 1,
  unknown = 2,  // not evidenced; value==unknown ⇔ evidence==unverified
};

// Whether a program asserts an output even with no input signal. Append-only
// three-state (unknown/no/yes) so "not yet evidenced" is distinct from a measured
// "no" — a program without evidence is `unknown`, never silently `no`. Same
// biconditional contract: value==unknown ⇔ field evidence==unverified; a concrete
// no/yes carries only confirmed/provisional provenance.
enum class SelfOscillating : std::uint8_t {
  unknown = 0,  // not evidenced; value==unknown ⇔ evidence==unverified
  no = 1,       // evidenced to NOT self-oscillate
  yes = 2,      // evidenced to self-oscillate
};

// Jack signal direction on the physical panel.
enum class PinDirection : std::uint8_t {
  input = 0,
  output = 1,
};

// Role of a parameter within its owning processor program.
enum class ParamRole : std::uint8_t {
  knob = 0,  // a plain module knob/switch
  x = 1,     // effector program X
  y = 2,     // effector program Y
  z = 3,     // effector program Z
};

// Per-volt modulation depth / transfer curve at a jack (design/07 §3). "Every
// JackDescriptor must define polarity, nominal/tolerated range, DC coupling,
// per-volt modulation depth or transfer curve, ...".
enum class SignalTransfer : std::uint8_t {
  linear = 0,       // depth scales linearly with CV (1 V → 1 unit depth)
  exponential = 1,  // depth scales exponentially (e.g. V/oct pitch tracking)
  none = 2,         // no CV grooming / direct pass-through
  unknown = 3,      // not yet evidenced
};

// Input saturation / rail behaviour at a jack (design/07 §3, §5). Hardware rail
// behaviour is modelled only where evidence exists; it is NOT a global [-1,1]
// clamp. We never invent a soft clipper for "safety".
enum class SaturationType : std::uint8_t {
  none = 0,     // no modelled rail / saturation stage
  hard = 1,     // hard clamp at the rail
  soft = 2,     // soft saturation stage
  unknown = 3,  // not yet evidenced
};

}  // namespace lunar24::core
