// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// state-aware runtime candidate factory (task#76, GH#12 9B).
//
// This is the ONE semantic builder between a validated DeviceStateV1 and the production
// MachineRuntimeDefinition. Layering (Codex): `decode -> migrate(if supported) ->
// validate_device_state -> buildMachineRuntimeCandidate -> (host) single stopped-stream
// publish`. It owns the validation decision (a rejected state is never handed to the runtime)
// and the "did the machine actually come up + did the GH#6 identity configure" decision, so a
// caller gets a single typed outcome:
//
//   * accepted          — validation ok AND the graph compiled AND identity configured.
//                         `definition` is a valid, fully-applied candidate.
//   * rejected_state    — validate_device_state failed. `validation` carries the family+field.
//   * rejected_graph    — the state validated but the machine graph did not compile (a broken
//                         candidate). `definition` is null.
//   * rejected_identity — state + graph ok but the GH#6 identity/calibration did NOT configure
//                         (a degraded candidate). `definition` is null.
//   * rejected_format   — the sample rate was not a supported, finite > 0 device rate.
//
// The statuses are SPLIT per the task#76 card: state validation / graph build / identity apply /
// format are distinguishable outcomes, never collapsed into one "machine bad" bucket. A rejected
// path leaves `definition` null and makes NO change to any caller-owned state. The candidate build
// is heap-only (definition lives on the heap behind a unique_ptr; the ~160 KiB owned tables are
// never on the audio stack). The definition is deliberately NON-COPYABLE and NON-MOVABLE, so the
// factory hands back a unique_ptr and the owner holds it at a STABLE address for the definition's
// whole life.

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>

#include <lunar24/core/device_state.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/state_validation.h>

namespace lunar24::core {

enum class MachineCandidateStatus : std::uint8_t {
  accepted = 0,
  rejected_state,    // validate_device_state failed (family+field in `validation`).
  rejected_graph,    // state ok, but the machine graph did not compile.
  rejected_identity, // state+graph ok, but the GH#6 identity/calibration did not configure.
  rejected_dsp_apply, // state+graph+identity ok, but the 169-parameter applied_to_DSP apply was
                      // not complete (first failure in firstFailParamId/firstFailStatus).
  rejected_format,   // illegal sample rate.
};

struct MachineCandidateResult {
  MachineCandidateStatus status = MachineCandidateStatus::accepted;
  // Meaningful iff status == rejected_state (the first validation failure). ok otherwise.
  StateValidationResult validation;
  // Non-null iff status == accepted; the caller owns it (and therefore the definition's lifetime)
  // and must hold it at a stable address.
  std::unique_ptr<MachineRuntimeDefinition> definition;
  // Meaningful iff status == rejected_dsp_apply: the first applied_to_DSP parameter whose write
  // was rejected, and the rejection reason. Sentinel (kParameterCount / applied) on acceptance.
  ParameterId firstFailParamId = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus firstFailStatus = ParameterApplyStatus::applied;
};

// Validate `state`, build a state-aware MachineRuntimeDefinition from it at `sampleRate`, and
// confirm the graph compiled and the GH#6 identity/calibration configured. On any rejection the
// returned definition is null and `state` is never mutated; on accept the definition owns a COPY
// of `state` (readable back via definition->deviceState()).
//
// Deliberately NOT noexcept: the candidate build heap-allocates the definition (~160 KiB owned
// tables) behind a make_unique, which is the one allocation that can throw. If the caller must be
// fail-closed against an OOM in a non-audio prepare/apply path, it decides its ownership policy up
// front; making this noexcept would instead terminate the process on an unexpected OOM, which is
// the worse failure mode.
inline MachineCandidateResult buildMachineRuntimeCandidate(const DeviceStateV1& state,
                                                            double sampleRate) {
  // A non-finite / non-positive sample rate would be passed into the runtime and poison it.
  if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
    return {MachineCandidateStatus::rejected_format, {}, nullptr};

  const StateValidationResult v = validate_device_state(state);
  if (!v.ok) return {MachineCandidateStatus::rejected_state, v, nullptr};

  // Construct directly (NOT std::make_unique): the state-aware ctor is PRIVATE, and friendship is
  // granted to THIS function — but make_unique is a library template whose instantiated `new`
  // would be a non-friend access. A direct `new` here runs inside the friend's body, which is the
  // ONLY correct way to mint a MachineRuntimeDefinition from an arbitrary DeviceStateV1.
  std::unique_ptr<MachineRuntimeDefinition> definition(
      new MachineRuntimeDefinition(state, sampleRate));
  if (!definition->valid())
    return {MachineCandidateStatus::rejected_graph, v, nullptr};
  if (!definition->identityApplied())
    return {MachineCandidateStatus::rejected_identity, v, nullptr};
  if (!definition->dspApplyOk())
    return {MachineCandidateStatus::rejected_dsp_apply, v, nullptr,
            definition->dspFirstFailId(), definition->dspFirstFailStatus()};

  return {MachineCandidateStatus::accepted, v, std::move(definition)};
}

}  // namespace lunar24::core
