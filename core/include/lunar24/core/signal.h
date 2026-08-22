// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Signal / electrical domain contract (design/07-core-contract.md §3).
//
// At jack and module boundaries the uniform physical meaning of a sample is a
// VIRTUAL VOLT: the numerical value 1.0 represents 1 V. Modules may use any
// internal normalisation, but converting to/from virtual volts at every public
// port is mandatory.

#pragma once

#include <cstddef>

namespace lunar24::core {

// Audio-thread working sample type.
using SignalSample = float;

// 1.0 numerical == 1 V in the virtual-volts domain.
inline constexpr double kVoltPerUnit = 1.0;

// Pitch CV is fixed at 1 V/octave-equivalent (not per-module convention).
inline constexpr double kPitchCvVoltsPerOctave = 1.0;

// One octave of pitch-CV correction in volts (also 1.0, kept explicit).
inline constexpr double kPitchCvPerOctave = kPitchCvVoltsPerOctave * kVoltPerUnit;

}  // namespace lunar24::core
