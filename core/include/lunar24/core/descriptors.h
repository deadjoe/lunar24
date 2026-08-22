// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Canonical descriptor structs for the machine registry (design/07 §2).
//
// One definition feeds DSP, UI, MIDI adapter, serialization and tests. These are
// framework-free: no iPlug2, IGraphics, platform, window, or filesystem types.
// The concrete id enums are completed by <lunar24/registry_ids.hpp>.

#pragma once

#include <cstdint>
#include <string_view>

#include <lunar24/core/enums.h>
#include <lunar24/core/evidence.h>
#include <lunar24/core/id_types.h>

namespace lunar24::core {

// A panel module (VCO, VCF, keyboard, drone voice, effector slot, ...).
// Its parameters and jacks are contiguous ranges into the flat registry arrays.
struct ModuleDescriptor {
  ModuleId id;
  std::string_view stable_id;   // canonical, frozen string id
  std::string_view name;        // display name (not stable)
  std::string_view category;    // source / control / processing / output / performance / interface
  std::string_view description;
  std::uint32_t paramBegin;     // index into kParameters[]
  std::uint32_t paramCount;
  std::uint32_t jackBegin;      // index into kJacks[]
  std::uint32_t jackCount;
  EvidenceRef evidence;
  EvidenceStatus status;
};

// A single continuous or discrete parameter (module knob/switch, or program X/Y/Z).
struct ParameterDescriptor {
  ParameterId id;
  std::string_view stable_id;
  std::string_view name;
  std::string_view owner;       // stable id of owning module or program
  std::string_view unit;        // "norm" | "volts" | "seconds" | "oct" | "semitones" | "bool" | ...
  double min;
  double max;
  double step;                  // 0 == continuous
  double initial;               // default value
  Smoothing smoothing;
  Persistence persistence;
  ParamRole role;
  EvidenceRef evidence;
  EvidenceStatus status;
};

// A physical patch point (3.5mm jack). Electrical + signal semantics are fixed.
struct JackDescriptor {
  JackId id;
  std::string_view stable_id;
  std::string_view name;
  ModuleId module;              // owning module
  PinDirection direction;
  SignalType signalType;        // advisory
  Polarity polarity;
  double nominalMin;            // virtual volts, recommended range
  double nominalMax;
  double gateThresholdVolts;    // only meaningful for gate/clock
  double hysteresisVolts;       // only meaningful for gate/clock
  Coupling coupling;
  EvidenceRef evidence;
  EvidenceStatus status;
};

// A hidden signal edge in the patching graph. Does not consume user-cable
// cardinality; plugging into the sink honours the "patch overrides" rule.
struct NormalizedRoute {
  std::string_view stable_id;
  JackId sourceJack;
  JackId sinkJack;
  std::string_view description;
  EvidenceRef evidence;
  EvidenceStatus status;
};

// A dual-effector processor program (cartridge × slot). Owns a contiguous range
// of its own program-local parameters (X/Y/Z).
struct ProgramDescriptor {
  ProgramId id;
  std::string_view stable_id;
  std::string_view cartridge;
  std::uint32_t slot;           // 1..3
  std::string_view name;
  std::string_view family;      // reverb / pitched_delay / ...
  bool selfOscillating;         // program that sounds without an input
  std::uint32_t paramBegin;     // index into kParameters[]
  std::uint32_t paramCount;
  EvidenceRef evidence;
  EvidenceStatus status;
};

}  // namespace lunar24::core
