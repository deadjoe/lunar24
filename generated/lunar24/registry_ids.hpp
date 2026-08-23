// GENERATED FILE - DO NOT EDIT. Regenerate with tools/generate_registry.py.
// Source: spec/machine/lunar24.json
// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/id_types.h>
#include <cstdint>
#include <string_view>

namespace lunar24::core {

enum class ModuleId : std::uint32_t {
    vco_a = 0,
    vco_b = 1,
    vcf = 2,
    keyboard = 3,
    envelope_a = 4
};

enum class ParameterId : std::uint32_t {
    vco_a_tune = 0,
    vco_a_wave = 1,
    vco_a_shape = 2,
    vco_a_oct_high = 3,
    vco_a_oct_low = 4,
    vco_a_sub = 5,
    vco_a_cv_amt = 6,
    vco_a_lin_exp = 7,
    vco_a_fm_amt = 8,
    vco_b_tune = 9,
    vco_b_wave = 10,
    vco_b_shape = 11,
    vco_b_cv_amt = 12,
    vcf_l_freq = 13,
    vcf_l_res = 14,
    vcf_l_mod = 15,
    vcf_l_dist = 16,
    vcf_l_gain = 17,
    vcf_r_freq = 18,
    vcf_mode = 19,
    vcf_link = 20,
    keyboard_pressure_signal = 21,
    envelope_a_attack = 22,
    envelope_a_release = 23,
    program_cathedral_1_octave_up = 24,
    program_cathedral_1_octave_down = 25,
    program_cathedral_1_decay = 26,
    program_magic_1_feedback = 27,
    program_magic_1_delay = 28,
    program_magic_1_pitch = 29
};

enum class JackId : std::uint32_t {
    vco_a_cv_in = 0,
    vco_a_v_oct_in = 1,
    vco_a_fm_in = 2,
    vco_a_sync_in = 3,
    vco_a_vca_ctl = 4,
    vco_a_dry_out = 5,
    vco_b_cv_in = 6,
    vco_b_v_oct_in = 7,
    vco_b_vco_out = 8,
    vco_b_dry_out = 9,
    vcf_cv_l_in = 10,
    vcf_cv_r_in = 11,
    vcf_audio_in = 12,
    keyboard_v_oct_out = 13,
    keyboard_gate_left_main_out = 14,
    keyboard_gate_right_out = 15,
    keyboard_clock_in = 16,
    envelope_a_gate_in = 17,
    envelope_a_env_out = 18
};

enum class ProgramId : std::uint32_t {
    program_cathedral_1 = 0,
    program_magic_1 = 1
};

enum class RouteId : std::uint32_t {
    route_keyboard_v_oct_to_vco = 0,
    route_keyboard_gate_to_eg = 1,
    route_vcf_cv_l_to_cv_r = 2
};

inline constexpr std::uint32_t kModuleCount = 5;
inline constexpr std::uint32_t kParameterCount = 30;
inline constexpr std::uint32_t kJackCount = 19;
inline constexpr std::uint32_t kProgramCount = 2;
inline constexpr std::uint32_t kRouteCount = 3;

inline constexpr std::uint64_t kModuleIdSpace = 5;
inline constexpr std::uint64_t kParameterIdSpace = 30;
inline constexpr std::uint64_t kJackIdSpace = 19;
inline constexpr std::uint64_t kProgramIdSpace = 2;
inline constexpr std::uint64_t kRouteIdSpace = 3;

static_assert(kDeviceParamCapacity >= kParameterIdSpace, "parameter bank too small for ParameterId space");
static_assert(kDevicePatchCapacity >= kJackIdSpace, "patch bank too small for JackId space");
static_assert(kDeviceRouteCapacity >= kRouteIdSpace, "route bank too small for RouteId space");

inline constexpr std::string_view module_id_string(ModuleId id) {
  switch (id) {
    case ModuleId::vco_a: return "vco_a";
    case ModuleId::vco_b: return "vco_b";
    case ModuleId::vcf: return "vcf";
    case ModuleId::keyboard: return "keyboard";
    case ModuleId::envelope_a: return "envelope_a";
  }
  return "(unknown Module)";
}

inline constexpr std::string_view parameter_id_string(ParameterId id) {
  switch (id) {
    case ParameterId::vco_a_tune: return "vco_a.tune";
    case ParameterId::vco_a_wave: return "vco_a.wave";
    case ParameterId::vco_a_shape: return "vco_a.shape";
    case ParameterId::vco_a_oct_high: return "vco_a.oct_high";
    case ParameterId::vco_a_oct_low: return "vco_a.oct_low";
    case ParameterId::vco_a_sub: return "vco_a.sub";
    case ParameterId::vco_a_cv_amt: return "vco_a.cv_amt";
    case ParameterId::vco_a_lin_exp: return "vco_a.lin_exp";
    case ParameterId::vco_a_fm_amt: return "vco_a.fm_amt";
    case ParameterId::vco_b_tune: return "vco_b.tune";
    case ParameterId::vco_b_wave: return "vco_b.wave";
    case ParameterId::vco_b_shape: return "vco_b.shape";
    case ParameterId::vco_b_cv_amt: return "vco_b.cv_amt";
    case ParameterId::vcf_l_freq: return "vcf.l.freq";
    case ParameterId::vcf_l_res: return "vcf.l.res";
    case ParameterId::vcf_l_mod: return "vcf.l.mod";
    case ParameterId::vcf_l_dist: return "vcf.l.dist";
    case ParameterId::vcf_l_gain: return "vcf.l.gain";
    case ParameterId::vcf_r_freq: return "vcf.r.freq";
    case ParameterId::vcf_mode: return "vcf.mode";
    case ParameterId::vcf_link: return "vcf.link";
    case ParameterId::keyboard_pressure_signal: return "keyboard.pressure_signal";
    case ParameterId::envelope_a_attack: return "envelope_a.attack";
    case ParameterId::envelope_a_release: return "envelope_a.release";
    case ParameterId::program_cathedral_1_octave_up: return "program.cathedral.1.octave_up";
    case ParameterId::program_cathedral_1_octave_down: return "program.cathedral.1.octave_down";
    case ParameterId::program_cathedral_1_decay: return "program.cathedral.1.decay";
    case ParameterId::program_magic_1_feedback: return "program.magic.1.feedback";
    case ParameterId::program_magic_1_delay: return "program.magic.1.delay";
    case ParameterId::program_magic_1_pitch: return "program.magic.1.pitch";
  }
  return "(unknown Parameter)";
}

inline constexpr std::string_view jack_id_string(JackId id) {
  switch (id) {
    case JackId::vco_a_cv_in: return "vco_a.cv_in";
    case JackId::vco_a_v_oct_in: return "vco_a.v_oct_in";
    case JackId::vco_a_fm_in: return "vco_a.fm_in";
    case JackId::vco_a_sync_in: return "vco_a.sync_in";
    case JackId::vco_a_vca_ctl: return "vco_a.vca_ctl";
    case JackId::vco_a_dry_out: return "vco_a.dry_out";
    case JackId::vco_b_cv_in: return "vco_b.cv_in";
    case JackId::vco_b_v_oct_in: return "vco_b.v_oct_in";
    case JackId::vco_b_vco_out: return "vco_b.vco_out";
    case JackId::vco_b_dry_out: return "vco_b.dry_out";
    case JackId::vcf_cv_l_in: return "vcf.cv_l_in";
    case JackId::vcf_cv_r_in: return "vcf.cv_r_in";
    case JackId::vcf_audio_in: return "vcf.audio_in";
    case JackId::keyboard_v_oct_out: return "keyboard.v_oct_out";
    case JackId::keyboard_gate_left_main_out: return "keyboard.gate_left_main_out";
    case JackId::keyboard_gate_right_out: return "keyboard.gate_right_out";
    case JackId::keyboard_clock_in: return "keyboard.clock_in";
    case JackId::envelope_a_gate_in: return "envelope_a.gate_in";
    case JackId::envelope_a_env_out: return "envelope_a.env_out";
  }
  return "(unknown Jack)";
}

inline constexpr std::string_view program_id_string(ProgramId id) {
  switch (id) {
    case ProgramId::program_cathedral_1: return "program.cathedral.1";
    case ProgramId::program_magic_1: return "program.magic.1";
  }
  return "(unknown Program)";
}

inline constexpr std::string_view route_id_string(RouteId id) {
  switch (id) {
    case RouteId::route_keyboard_v_oct_to_vco: return "route.keyboard_v_oct_to_vco";
    case RouteId::route_keyboard_gate_to_eg: return "route.keyboard_gate_to_eg";
    case RouteId::route_vcf_cv_l_to_cv_r: return "route.vcf_cv_l_to_cv_r";
  }
  return "(unknown Route)";
}

}  // namespace lunar24::core
