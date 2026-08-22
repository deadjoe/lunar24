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

// Per-field evidence status for a JackDescriptor (and reused for the path-level
// evidence in a ModulePathDelay). Each member records whether that specific
// value is evidenced, rather than inheriting the descriptor-wide status.
//
// The voltage range is split into two independent facts: the NOMINAL recommended
// range (what the manual states a jack is meant to carry) and the TOLERATED /
// absolute limit (what it survives without malformed behaviour). The manual
// frequently evidences the first and is silent on the second; marking them as one
// field would overclaim the tolerated limit as evidenced whenever the nominal is.
struct FieldEvidence {
  EvidenceStatus nominalRange = EvidenceStatus::unverified;    // nominal voltage range
  EvidenceStatus toleratedRange = EvidenceStatus::unverified;  // absolute tolerant limit
  EvidenceStatus threshold = EvidenceStatus::unverified;       // gate/clock threshold + hysteresis
  EvidenceStatus saturation = EvidenceStatus::unverified;      // input rail / saturation behaviour
  EvidenceStatus transfer = EvidenceStatus::unverified;        // modulation depth / transfer curve

  bool nominalRangeEvidenced() const { return nominalRange == EvidenceStatus::confirmed; }
  bool anyUnverified() const {
    return nominalRange != EvidenceStatus::confirmed || toleratedRange != EvidenceStatus::confirmed ||
           threshold != EvidenceStatus::confirmed || saturation != EvidenceStatus::confirmed ||
           transfer != EvidenceStatus::confirmed;
  }
};

}  // namespace lunar24::core
