// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1 validator (design/07 §9, task #75 revision 3 — Codex BLOCK bd53b76a).
//
// Deterministic first-failure: validate_device_state() walks the candidate in a
// fixed order and returns the FIRST validation family that is violated, together
// with a field/id identifying the offending member. It is fixed-heap (no
// allocation, no recursion, one small stack buffer in check_cables), takes the
// candidate by constant reference, and never mutates the caller — a rejected
// candidate is bitwise unchanged.
//
// Layering (Codex): `decode -> migrate(if supported) -> validate_device_state ->
// build/apply`. This header validates an ALREADY-decoded DeviceStateV1, never
// raw bytes/size. It does NOT accept a caller-supplied expected schema version:
// the schema must equal kDeviceStorageSchemaVersion directly (a caller-parameterized
// schema was a false-green — the validator knew the signature's own version even
// when the state carried a wrong one).
//
// Revision-3 correctness fixes (each was a false-green in revision 2):
//   * identity uses unit_identity_profile.h's single supported-version truth
//     (isSupportedIdentityVersion), not a locally copied `1`.
//   * calibration trims must be finite AND >0 (0 was accepted as finite).
//   * one unified parameter-value check (finite + [min,max] + selector exact-integer
//     + step-grid) is shared by parameters[], the RIGHT-22 bank, and preset L/R
//     scalars — no three drifting copies, and a fractional selector (e.g. 1.5) is red.
//   * keyboard coherence: KeyboardSettings.pressureOutput == canonical LEFT
//     parameters[keyboard_pressure_output], and parameters[keyboard_behaviour] ==
//     KeyboardSettings.pressureBehaviour (the #57 canonical).
//   * cable/route true coherence: inputCable is a presence bit (0/1); no-cable ->
//     cableSource==JackId{0}; with-cable -> landed source(output)/sink(input) plus
//     aggregate source cardinality; routeOverridden is looked up by stable RouteId
//     (kNormalizedRoutes), NOT the dense i<kRouteCount assumption, and
//     routeOverridden[id] == (its sink has a user cable).
//   * ProgramId is validated by descriptor lookup (find_program), not <kProgramCount.

#pragma once

#include <cmath>
#include <cstdint>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_side_bank.h>  // kKeyboardScalarParameterIds, read_preset_scalar_pair
#include <lunar24/core/state_disposition.h>   // find_parameter, find_jack, find_program
#include <lunar24/core/unit_identity_profile.h>  // isSupportedIdentityVersion
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

enum class ValidationFamily : std::uint8_t {
  ok = 0,
  wrong_schema_version,
  identity_invalid,
  calibration_invalid,
  non_finite_state,
  parameter_out_of_range,
  invalid_parameter_slot,
  illegal_jack_id,
  cable_direction,
  cable_cardinality,
  cable_incoherent,
  route_incoherent,
  illegal_program_id,
  keyboard_live_invalid,
  preset_invalid,
};

struct StateValidationResult {
  bool ok = true;
  ValidationFamily family = ValidationFamily::ok;
  std::uint32_t field = 0;  // first failing member; domain is family-specific
};

namespace validate_detail {

inline StateValidationResult pass() noexcept { return {}; }

inline StateValidationResult fail(ValidationFamily f, std::uint32_t field) noexcept {
  return StateValidationResult{false, f, field};
}

inline bool finite_d(double v) noexcept { return std::isfinite(v); }
inline bool finite_f(float v) noexcept { return std::isfinite(v); }

// 12-bit quantise scale-editor mask limit (keyboard_behaviour.h kChromaticScaleMask).
inline constexpr std::uint16_t kScaleEditorMaskLimit = 0x0FFFu;

// A landed jack id (0..kDevicePatchCapacity-1), or nullptr if that slot is a hole
// (e.g. jack id 2, 12, 65) or beyond the landed set.
inline const JackDescriptor* find_jack(std::uint32_t id) noexcept {
  for (std::uint32_t i = 0; i < kJackCount; ++i)
    if (static_cast<std::uint32_t>(registry::kJacks[i].id) == id) return &registry::kJacks[i];
  return nullptr;
}

// A landed route looked up by its stable RouteId, or nullptr if id is not a landed
// RouteId. The route array is sparse by stable RouteId (not a dense 0..kRouteCount
// index), so a caller can never assume i<kRouteCount==landed.
inline const NormalizedRoute* find_route(std::uint32_t id) noexcept {
  for (std::uint32_t i = 0; i < kRouteCount; ++i)
    if (static_cast<std::uint32_t>(registry::kNormalizedRoutes[i].id) == id)
      return &registry::kNormalizedRoutes[i];
  return nullptr;
}

// Unified per-scalar check shared by parameters[], the RIGHT-22 live bank, and
// preset L/R scalars (ONE logic, not three drifting copies). A value is acceptable
// iff it is finite, in [min,max], and — for a selector (optionCount>0) — an exact
// integer in [0, optionCount); when step>0 it must additionally sit on the
// min+n*step grid. Fractional selector values and off-grid steps are corruption
// even though they satisfy [min,max], so they are red.
inline StateValidationResult check_scalar_value(double v, const ParameterDescriptor& d,
                                                std::uint32_t field) noexcept {
  if (!finite_d(v)) return fail(ValidationFamily::non_finite_state, field);
  if (v < d.min || v > d.max) return fail(ValidationFamily::parameter_out_of_range, field);
  if (d.optionCount > 0u) {
    if (std::floor(v) != v) return fail(ValidationFamily::parameter_out_of_range, field);
    if (v < 0.0 || v >= static_cast<double>(d.optionCount))
      return fail(ValidationFamily::parameter_out_of_range, field);
  }
  if (d.step > 0.0) {
    const double k = (v - d.min) / d.step;
    if (std::fabs(k - std::round(k)) > 1e-5)
      return fail(ValidationFamily::parameter_out_of_range, field);
  }
  return pass();
}

// Schema must equal kDeviceStorageSchemaVersion directly — no caller-supplied
// expected version (a caller-parameterized schema was the revision-2 false-green).
inline StateValidationResult check_schema(const DeviceStateV1& st) noexcept {
  if (st.schemaVersion != kDeviceStorageSchemaVersion)
    return fail(ValidationFamily::wrong_schema_version, st.schemaVersion);
  return pass();
}

inline StateValidationResult check_identity(const DeviceStateV1& st) noexcept {
  if (!isSupportedIdentityVersion(st.identityModelVersion))
    return fail(ValidationFamily::identity_invalid, st.identityModelVersion);
  return pass();
}

inline StateValidationResult check_calibration(const DeviceStateV1& st) noexcept {
  if (!finite_f(st.calibration.vcfLeftTrim) || st.calibration.vcfLeftTrim <= 0.0f)
    return fail(ValidationFamily::calibration_invalid, 0u);
  if (!finite_f(st.calibration.vcfRightTrim) || st.calibration.vcfRightTrim <= 0.0f)
    return fail(ValidationFamily::calibration_invalid, 1u);
  return pass();
}

// parameters[kDeviceParamCapacity], indexed by ParameterId. Landed id -> unified
// value check via its descriptor; hole/slack id (not landed) -> must be exactly 0.
inline StateValidationResult check_parameters(const DeviceStateV1& st) noexcept {
  for (std::uint32_t i = 0; i < kDeviceParamCapacity; ++i) {
    const ParameterId pid = static_cast<ParameterId>(i);
    const ParameterDescriptor* d = find_parameter(pid);
    if (d != nullptr) {
      const StateValidationResult r = check_scalar_value(st.parameters[i], *d, i);
      if (!r.ok) return r;
    } else if (st.parameters[i] != 0.0) {
      return fail(ValidationFamily::invalid_parameter_slot, i);
    }
  }
  return pass();
}

// Right-bank live scalars (the per-side keyboard scalar values under split mode).
// Same unified check as the left, keyed by the same ParameterId.
inline StateValidationResult check_right_bank(const DeviceStateV1& st) noexcept {
  for (std::uint32_t j = 0; j < kKeyboardScalarRightCount; ++j) {
    const ParameterDescriptor* d = find_parameter(kKeyboardScalarParameterIds[j]);
    if (d == nullptr) {
      if (st.keyboardScalarRight[j] != 0.0)
        return fail(ValidationFamily::invalid_parameter_slot, kDeviceParamCapacity + j);
      continue;
    }
    const StateValidationResult r = check_scalar_value(st.keyboardScalarRight[j], *d,
                                                       kDeviceParamCapacity + j);
    if (!r.ok) return r;
  }
  return pass();
}

inline StateValidationResult check_keyboard_live(const DeviceStateV1& st) noexcept {
  // 12-bit quantise masks (both banks)
  if (st.keyboardScaleEditor > kScaleEditorMaskLimit)
    return fail(ValidationFamily::keyboard_live_invalid, 0u);
  if (st.keyboardScaleEditorR > kScaleEditorMaskLimit)
    return fail(ValidationFamily::keyboard_live_invalid, 1u);
  // plate-tune / pushbutton vectors (both banks): finite
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    if (!finite_f(st.keyboardPlateTune[i])) return fail(ValidationFamily::non_finite_state, 1000u + i);
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    if (!finite_f(st.keyboardPushbutton[i])) return fail(ValidationFamily::non_finite_state, 2000u + i);
  for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
    if (!finite_f(st.keyboardPlateTuneR[i])) return fail(ValidationFamily::non_finite_state, 3000u + i);
  for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
    if (!finite_f(st.keyboardPushbuttonR[i])) return fail(ValidationFamily::non_finite_state, 4000u + i);
  // 16-step sequencer runs (both banks): value finite, gate boolean (0/1)
  for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
    if (!finite_f(st.keyboardSeqCurrent.steps[i].value))
      return fail(ValidationFamily::non_finite_state, 5000u + i);
    if (st.keyboardSeqCurrent.steps[i].gate > 1u)
      return fail(ValidationFamily::keyboard_live_invalid, 6000u + i);
  }
  for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
    if (!finite_f(st.keyboardSeqCurrentR.steps[i].value))
      return fail(ValidationFamily::non_finite_state, 7000u + i);
    if (st.keyboardSeqCurrentR.steps[i].gate > 1u)
      return fail(ValidationFamily::keyboard_live_invalid, 8000u + i);
  }
  // Live global pressure selectors are within their option counts (evidenced via
  // the registry descriptor's optionCount, 3 for behaviour, 5 for pressure output).
  const ParameterDescriptor* b = find_parameter(ParameterId::keyboard_behaviour);
  const ParameterDescriptor* po = find_parameter(ParameterId::keyboard_pressure_output);
  if (b != nullptr && st.keyboardSettings.pressureBehaviour >= b->optionCount)
    return fail(ValidationFamily::keyboard_live_invalid, 9000u);
  if (po != nullptr && st.keyboardSettings.pressureOutput >= po->optionCount)
    return fail(ValidationFamily::keyboard_live_invalid, 9001u);
  // Keyboard coherence (revision-3): KeyboardSettings.pressureOutput is the #57
  // canonical LEFT selector and MUST equal parameters[keyboard_pressure_output];
  // parameters[keyboard_behaviour] is only a compatibility mirror of the canonical
  // KeyboardSettings.pressureBehaviour and MUST equal it.
  if (b != nullptr &&
      static_cast<double>(st.keyboardSettings.pressureBehaviour) !=
          st.parameters[static_cast<std::uint32_t>(ParameterId::keyboard_behaviour)])
    return fail(ValidationFamily::keyboard_live_invalid, 9002u);
  if (po != nullptr &&
      st.parameters[static_cast<std::uint32_t>(ParameterId::keyboard_pressure_output)] !=
          static_cast<double>(st.keyboardSettings.pressureOutput))
    return fail(ValidationFamily::keyboard_live_invalid, 9003u);
  return pass();
}

inline StateValidationResult check_presets(const DeviceStateV1& st) noexcept {
  for (std::uint32_t k = 0; k < kDeviceKeyboardPresetCount; ++k) {
    const KeyboardPreset& p = st.keyboardPresets[k];
    if (p.id != k) return fail(ValidationFamily::preset_invalid, k);
    // The 22 per-side scalar pairs use the SAME unified value check as live params.
    for (std::uint32_t j = 0; j < kKeyboardScalarRightCount; ++j) {
      const ParameterId pid = kKeyboardScalarParameterIds[j];
      const ParameterDescriptor* d = find_parameter(pid);
      double l = 0.0, r = 0.0;
      read_preset_scalar_pair(p, pid, &l, &r);
      if (d != nullptr) {
        const StateValidationResult rl = check_scalar_value(l, *d, 10000u + j);
        if (!rl.ok) return rl;
        const StateValidationResult rr = check_scalar_value(r, *d, 20000u + j);
        if (!rr.ok) return rr;
      } else {
        if (l != 0.0) return fail(ValidationFamily::invalid_parameter_slot, 10000u + j);
        if (r != 0.0) return fail(ValidationFamily::invalid_parameter_slot, 20000u + j);
      }
    }
    // Global pressure selectors within option counts.
    const ParameterDescriptor* b = find_parameter(ParameterId::keyboard_behaviour);
    const ParameterDescriptor* po = find_parameter(ParameterId::keyboard_pressure_output);
    if (b != nullptr && p.pressureBehaviour >= b->optionCount)
      return fail(ValidationFamily::preset_invalid, 30000u + k);
    if (po != nullptr && p.pressureOutput >= po->optionCount)
      return fail(ValidationFamily::preset_invalid, 30001u + k);
    // Non-scalar masks + vector finiteness (all three composite regions, both banks).
    if (p.quantiseScaleEditor > kScaleEditorMaskLimit) return fail(ValidationFamily::preset_invalid, 30010u + k);
    if (p.quantiseScaleEditorR > kScaleEditorMaskLimit) return fail(ValidationFamily::preset_invalid, 30011u + k);
    for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
      if (!finite_f(p.plateTune[i])) return fail(ValidationFamily::non_finite_state, 30020u + k);
    for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
      if (!finite_f(p.pushbuttonValue[i])) return fail(ValidationFamily::non_finite_state, 30030u + k);
    for (std::uint32_t i = 0; i < kKeyboardPlateTuneCount; ++i)
      if (!finite_f(p.plateTuneR[i])) return fail(ValidationFamily::non_finite_state, 30040u + k);
    for (std::uint32_t i = 0; i < kKeyboardPushbuttonCount; ++i)
      if (!finite_f(p.pushbuttonValueR[i])) return fail(ValidationFamily::non_finite_state, 30050u + k);
    for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
      if (!finite_f(p.seqSteps.steps[i].value)) return fail(ValidationFamily::non_finite_state, 30060u + k);
      if (p.seqSteps.steps[i].gate > 1u) return fail(ValidationFamily::preset_invalid, 30070u + k);
    }
    for (std::uint32_t i = 0; i < kKeyboardSeqStepCount; ++i) {
      if (!finite_f(p.seqStepsR.steps[i].value)) return fail(ValidationFamily::non_finite_state, 30080u + k);
      if (p.seqStepsR.steps[i].gate > 1u) return fail(ValidationFamily::preset_invalid, 30090u + k);
    }
  }
  return pass();
}

inline StateValidationResult check_cables(const DeviceStateV1& st) noexcept {
  // Fixed, stack-only presence->source aggregate; no heap. Every landed output jack
  // may appear as a source at most its maxCables times (PatchGraph must be
  // lossless-reconstructable from the presence bit + cableSource).
  std::uint32_t source_used[kDevicePatchCapacity] = {0};
  for (std::uint32_t i = 0; i < kDevicePatchCapacity; ++i) {
    const std::uint8_t present = st.inputCable[i];
    const JackId src = st.cableSource[i];
    if (present == 0u) {
      // Presence bit off => the source is the canonical "no cable" JackId{0}.
      if (src != JackId{0}) return fail(ValidationFamily::cable_incoherent, i);
      continue;
    }
    if (present != 1u) return fail(ValidationFamily::cable_cardinality, i);  // presence bit only
    const JackDescriptor* dest = find_jack(i);
    if (dest == nullptr) return fail(ValidationFamily::illegal_jack_id, i);
    if (dest->direction != PinDirection::input) return fail(ValidationFamily::cable_direction, i);
    if (present > dest->maxCables) return fail(ValidationFamily::cable_cardinality, i);
    const JackDescriptor* srcDesc = find_jack(static_cast<std::uint32_t>(src));
    if (srcDesc == nullptr) return fail(ValidationFamily::cable_incoherent, i);
    if (srcDesc->direction != PinDirection::output) return fail(ValidationFamily::cable_direction, i);
    const std::uint32_t si = static_cast<std::uint32_t>(src);
    if (si < kDevicePatchCapacity) {
      ++source_used[si];
      if (source_used[si] > srcDesc->maxCables)
        return fail(ValidationFamily::cable_cardinality, si);
    }
  }
  return pass();
}

inline StateValidationResult check_routes(const DeviceStateV1& st) noexcept {
  // routeOverridden is indexed by the stable RouteId, NOT a dense 0..kRouteCount
  // index. Look each id up in kNormalizedRoutes; an id that is not a landed RouteId
  // must be 0. For a landed route, routeOverridden[id] == (its sink has a user cable).
  for (std::uint32_t i = 0; i < kDeviceRouteCapacity; ++i) {
    const std::uint8_t v = st.routeOverridden[i];
    const NormalizedRoute* r = find_route(i);
    if (r == nullptr) {
      if (v != 0u) return fail(ValidationFamily::route_incoherent, i);
      continue;
    }
    if (v > 1u) return fail(ValidationFamily::route_incoherent, i);
    const std::uint32_t sink = static_cast<std::uint32_t>(r->sinkJack);
    if (sink >= kDevicePatchCapacity) return fail(ValidationFamily::route_incoherent, i);
    if (find_jack(sink) == nullptr) return fail(ValidationFamily::route_incoherent, i);
    const bool sink_has_cable = st.inputCable[sink] != 0u;
    if ((v == 1u) != sink_has_cable) return fail(ValidationFamily::route_incoherent, i);
  }
  return pass();
}

inline StateValidationResult check_programs(const DeviceStateV1& st) noexcept {
  // ProgramId by descriptor lookup (find_program), not a dense <kProgramCount test.
  if (find_program(st.leftEffector.program) == nullptr)
    return fail(ValidationFamily::illegal_program_id, 0u);
  if (find_program(st.rightEffector.program) == nullptr)
    return fail(ValidationFamily::illegal_program_id, 1u);
  return pass();
}

}  // namespace validate_detail

// Validates a decoded DeviceStateV1 candidate. Returns the first failed
// ValidationFamily (or ok). Never mutates the caller (const&). The schema version
// is not caller-supplied: a candidate must carry kDeviceStorageSchemaVersion.
inline StateValidationResult validate_device_state(const DeviceStateV1& st) noexcept {
  using namespace validate_detail;
  StateValidationResult r = check_schema(st);
  if (!r.ok) return r;
  r = check_identity(st);
  if (!r.ok) return r;
  r = check_calibration(st);
  if (!r.ok) return r;
  r = check_parameters(st);
  if (!r.ok) return r;
  r = check_right_bank(st);
  if (!r.ok) return r;
  r = check_keyboard_live(st);
  if (!r.ok) return r;
  r = check_presets(st);
  if (!r.ok) return r;
  r = check_cables(st);
  if (!r.ok) return r;
  r = check_routes(st);
  if (!r.ok) return r;
  r = check_programs(st);
  if (!r.ok) return r;
  return validate_detail::pass();
}

}  // namespace lunar24::core
