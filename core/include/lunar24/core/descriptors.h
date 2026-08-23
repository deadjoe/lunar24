// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Canonical descriptor structs for the machine registry (design/07 §2, §3).
//
// One definition feeds DSP, UI, MIDI adapter, serialization and tests. These are
// framework-free: no iPlug2, IGraphics, platform, window, or filesystem types.
// The concrete id enums are completed by <lunar24/registry_ids.hpp>.
//
// Evidence discipline (design/07 §10): a descriptor may be `confirmed` to exist
// on the panel while individual numeric fields carry their own FieldEvidence so
// invented ranges are never presented as facts.

#pragma once

#include <cstdint>
#include <string_view>

#include <lunar24/core/enums.h>
#include <lunar24/core/evidence.h>
#include <lunar24/core/evidence_policy.h>
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
  std::string_view unit;        // "norm" | "volts" | "seconds" | "oct" | "semitones" | "milliseconds" | ...
  double min;
  double max;
  double step;                  // 0 == continuous
  double initial;               // default value
  Smoothing smoothing;
  Persistence persistence;
  ParamRole role;
  EvidenceRef evidence;
  EvidenceStatus status;
  // Range/default provenance is split from identity: numeric min/max/initial and the
  // unit/step/smoothing/persistence policies are each independently evidenced, never
  // inherited from the descriptor-wide status (design/07 §10, Codex 03848819). The numeric
  // values themselves are software-normalized when the manual states only a symbolic set.
  ParameterFieldEvidence fieldEvidence;
  // Discrete selector positions. optionCount==0 for a continuous parameter; otherwise
  // `options` points at optionCount labels (this parameter's slice of the generated
  // kParameterOptionLabels table). UI/MIDI decode the integer index via this table, so a
  // 0/1/2 value is never offered without knowing what it means.
  std::uint32_t optionCount = 0;
  const char* const* options = nullptr;
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

  // Nominal recommended range (virtual volts). Tolerated range is the absolute
  // hardware-tolerant limit; 0 when unstated (then fieldEvidence.nominalRange is
  // the only range fact evidenced — tolerated is never implied from it).
  double nominalMin;
  double nominalMax;
  double toleratedMin;
  double toleratedMax;

  // Per-volt modulation depth / transfer curve (design/07 §3).
  double modulationDepthPerVolt;  // 1.0 == 1 V drives 1 unit of depth
  SignalTransfer transfer;

  // Input rail / saturation behaviour (design/07 §3, §5). Modelled only where
  // evidence exists; never a global [-1,1] clamp.
  SaturationType saturation;

  // Cable cardinality (design/07 §4): default 1 (a jack takes/receives at most
  // one cable). Stackable/multiple only if hardware evidence exists.
  std::uint8_t maxCables;

  double gateThresholdVolts;    // only meaningful for gate/clock
  double hysteresisVolts;       // only meaningful for gate/clock
  Coupling coupling;
  EvidenceRef evidence;
  EvidenceStatus status;
  FieldEvidence fieldEvidence;  // per-field provenance (range/threshold/saturation/transfer/signal class)
};

// A hidden signal edge in the patching graph. Does not consume user-cable
// cardinality; plugging into the sink honours the "patch overrides" rule.
// It carries its own stable numeric RouteId, so a route is identified wherever it
// is persisted (e.g. DeviceStateV1's route-override bank) by id, never by array
// position in kNormalizedRoutes[].
struct NormalizedRoute {
  RouteId id;
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
  std::string_view family;           // effect family (reverb / pitched_delay / ...); "unknown" if un-evidenced
  SelfOscillating selfOscillating;   // three-state assertion (not evidenced => unknown, NOT a silent no)
  std::uint32_t paramBegin;          // index into kParameters[]
  std::uint32_t paramCount;
  EvidenceRef evidence;
  EvidenceStatus status;
  ProgramFieldEvidence fieldEvidence;  // per-field provenance (family, selfOscillating)
};

}  // namespace lunar24::core
