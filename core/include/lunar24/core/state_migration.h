// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1 migration hook (design/07 §9, task #75 revision 3 — Codex BLOCK
// bd53b76a).
//
// This is the CURRENT-V5 identity migration hook ONLY. It takes a decoded
// DeviceStateV1 candidate (NOT raw bytes/size — that would duplicate the wire
// decoder) and, for the current schema version, produces the migrated candidate
// in a CALLER-OWNED out slot (a bitwise copy: identity migration). A version that
// is neither current nor a supported legacy one is rejected typed-fail-closed with
// the out slot bitwise UNCHANGED. No real v4 decoder/migration is invented here:
// it exists only once a frozen old wire schema does, so for now anything other
// than v5 is "unsupported / needs newer codec".
//
// Layering: `decode -> migrate(if supported) -> validate_device_state -> apply`.
// migrate() does not validate (that is validate_device_state's job) and does not
// mutate the out slot on a rejected path.

#pragma once

#include <cstdint>

#include <lunar24/core/device_state.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

enum class MigrationStatus : std::uint8_t {
  ok = 0,
  current_version,        // already at the current schema version (v5)
  unsupported_version,    // old/unrecognized schema version (no frozen old wire schema)
  requires_newer_codec,   // a schema version newer than this build understands
};

struct MigrationResult {
  bool ok = true;
  MigrationStatus status = MigrationStatus::current_version;
  std::uint32_t fromVersion = 0;  // the candidate's schemaVersion
};

// Migrate a decoded DeviceStateV1 candidate into the caller-owned out slot. On
// success (current version) *out is a bitwise copy of `state`; on a rejected path
// (old/unknown or newer-than-known version) *out is bitwise UNCHANGED — the caller's
// previous contents are preserved, so a failed migration can never leave a
// half-written candidate. The caller then validates *out with
// validate_device_state(*out) when ok == true.
inline MigrationResult migrate_device_state(const DeviceStateV1& state,
                                            DeviceStateV1& out) noexcept {
  const std::uint32_t v = state.schemaVersion;
  if (v == kDeviceStorageSchemaVersion) {
    out = state;  // identity migration: bitwise copy
    return MigrationResult{true, MigrationStatus::current_version, v};
  }
  if (v > kDeviceStorageSchemaVersion)
    return MigrationResult{false, MigrationStatus::requires_newer_codec, v};
  return MigrationResult{false, MigrationStatus::unsupported_version, v};
}

}  // namespace lunar24::core
