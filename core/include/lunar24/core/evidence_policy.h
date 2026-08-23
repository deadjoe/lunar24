// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Field-level evidence discipline (design/07 §3, §10). Documented/derived values
// are split from identity facts: a descriptor may be `confirmed` to EXIST on the
// panel while specific numeric fields (voltage range, threshold/hysteresis,
// saturation, transfer) are individually `unverified`/`provisional` until first-
// hand evidence exists. This prevents overclaiming invented ranges as facts.

#pragma once

#include <cstdint>

#include <lunar24/core/enums.h>

namespace lunar24::core {

// Per-field evidence status for a JackDescriptor. Each member records whether
// that specific value is evidenced, rather than inheriting the descriptor-wide
// status. (This is jack-field provenance; the per-path scheduling facts of a
// ModuleExecutionContract use their own PathEvidence in module_execution_contract.h.)
//
// The voltage range is split into two independent facts: the NOMINAL recommended
// range (what the manual states a jack is meant to carry) and the TOLERATED /
// absolute limit (what it survives without malformed behaviour). The manual
// frequently evidences the first and is silent on the second; marking them as one
// field would overclaim the tolerated limit as evidenced whenever the nominal is.
//
// The three signal-class facts (recommended signal use, polarity, AC/DC coupling)
// each carry their own provenance too. They are NOT inferred from a stable-id
// suffix: where the manual/panel does not evidence the value, the descriptor must
// carry the `unknown` enum value with `unverified` provenance (Codex 2026-08-23).
struct FieldEvidence {
  EvidenceStatus nominalRange = EvidenceStatus::unverified;    // nominal voltage range
  EvidenceStatus toleratedRange = EvidenceStatus::unverified;  // absolute tolerant limit
  EvidenceStatus threshold = EvidenceStatus::unverified;       // gate/clock threshold + hysteresis
  EvidenceStatus saturation = EvidenceStatus::unverified;      // input rail / saturation behaviour
  EvidenceStatus transfer = EvidenceStatus::unverified;        // modulation depth / transfer curve
  EvidenceStatus signalType = EvidenceStatus::unverified;      // recommended signal use
  EvidenceStatus polarity = EvidenceStatus::unverified;        // electrical polarity
  EvidenceStatus coupling = EvidenceStatus::unverified;        // AC / DC coupling

  bool nominalRangeEvidenced() const { return nominalRange == EvidenceStatus::confirmed; }
  bool anyUnverified() const {
    return nominalRange != EvidenceStatus::confirmed || toleratedRange != EvidenceStatus::confirmed ||
           threshold != EvidenceStatus::confirmed || saturation != EvidenceStatus::confirmed ||
           transfer != EvidenceStatus::confirmed || signalType != EvidenceStatus::confirmed ||
           polarity != EvidenceStatus::confirmed || coupling != EvidenceStatus::confirmed;
  }
};

// Per-field evidence status for a ParameterDescriptor (Codex 03848819). The numeric
// range, the unit label, the default, the step and the smoothing/persistence policies
// are each independently evidenced, rather than inheriting the descriptor-wide status.
// This keeps a software-normalized 0..1 encoding of a multi-position selector from ever
// masquerading as a confirmed hardware fact: an unknown value domain is provisionally
// mapped, never silently concrete, and every member carries its own provenance.
struct ParameterFieldEvidence {
  EvidenceStatus range = EvidenceStatus::unverified;        // min/max numeric range
  EvidenceStatus unit = EvidenceStatus::unverified;         // unit label (V/OCT, seconds, ...)
  EvidenceStatus initial = EvidenceStatus::unverified;      // default value
  EvidenceStatus step = EvidenceStatus::unverified;         // step (0 == continuous)
  EvidenceStatus smoothing = EvidenceStatus::unverified;    // smoothing policy
  EvidenceStatus persistence = EvidenceStatus::unverified;  // persistence policy

  bool anyUnverified() const {
    return range != EvidenceStatus::confirmed || unit != EvidenceStatus::confirmed ||
           initial != EvidenceStatus::confirmed || step != EvidenceStatus::confirmed ||
           smoothing != EvidenceStatus::confirmed || persistence != EvidenceStatus::confirmed;
  }
};

// Per-field evidence status for a ProgramDescriptor (Codex 2026-08-23). A
// program's effect `family` and its `selfOscillating` three-state are independent
// audited facts: a `family` or `selfOscillating` recorded as `unknown` MUST carry
// `unverified` provenance, and a concrete value is only reported where the manual /
// effector-family catalog evidences it.
struct ProgramFieldEvidence {
  EvidenceStatus family = EvidenceStatus::unverified;            // effect family (reverb, pitched_delay, ...)
  EvidenceStatus selfOscillating = EvidenceStatus::unverified;   // self-oscillation assertion

  bool anyUnverified() const {
    return family != EvidenceStatus::confirmed || selfOscillating != EvidenceStatus::confirmed;
  }
};

}  // namespace lunar24::core
