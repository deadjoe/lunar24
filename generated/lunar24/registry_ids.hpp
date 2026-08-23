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
    vco_a_morph = 1,
    vco_a_pw = 2,
    vco_a_oct_sel = 3,
    vco_a_sub_sel = 5,
    vco_a_cv_amt = 6,
    vco_a_lin_exp = 7,
    vco_a_pwm = 8,
    vco_b_tune = 9,
    vco_b_morph = 10,
    vco_b_pw = 11,
    vco_b_cv_amt = 12,
    vcf_l_freq = 13,
    vcf_l_res = 14,
    vcf_l_mod = 15,
    vcf_dist = 16,
    vcf_gain = 17,
    vcf_r_freq = 18,
    vcf_l_bp_lp = 19,
    vcf_link = 20,
    keyboard_behaviour = 100,
    keyboard_mode = 101,
    keyboard_arp_hold = 102,
    keyboard_arp_clock = 103,
    keyboard_arp_direction = 104,
    keyboard_arp_variation = 105,
    keyboard_arp_interval = 106,
    keyboard_arp_rhythm = 107,
    keyboard_arp_length = 108,
    keyboard_seq_run = 109,
    keyboard_seq_length = 110,
    keyboard_seq_clock = 111,
    keyboard_seq_direction = 112,
    keyboard_seq_cv_output = 113,
    keyboard_seq_rhythm = 114,
    keyboard_seq_rhythm_length = 115,
    keyboard_portamento_speed = 117,
    keyboard_portamento_legato = 118,
    keyboard_vibrato_speed = 119,
    keyboard_vibrato_depth = 120,
    keyboard_vibrato_delay = 121,
    keyboard_vibrato_pressure = 122,
    keyboard_pressure_output = 123,
    keyboard_pressure_rise = 124,
    keyboard_pressure_fall = 125,
    keyboard_quantise_load_scale = 127,
    keyboard_root_note = 128,
    keyboard_clock_bpm = 129,
    keyboard_calibration_v_oct = 130,
    keyboard_calibration_pressure = 131,
    keyboard_dac_vref = 132,
    keyboard_touch_threshold = 133,
    keyboard_release_threshold = 134,
    keyboard_pressure_min = 135,
    keyboard_pressure_max = 136,
    keyboard_mpr121_charge = 137,
    keyboard_mpr121_discharge = 138,
    keyboard_debounce = 139,
    keyboard_encoder_direction = 140,
    envelope_a_a = 22,
    envelope_a_r = 23,
    program_cathedral_1_x = 24,
    program_cathedral_1_y = 25,
    program_cathedral_1_z = 26,
    program_magic_1_x = 27,
    program_magic_1_y = 28,
    program_magic_1_z = 29
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
inline constexpr std::uint32_t kParameterCount = 67;
inline constexpr std::uint32_t kJackCount = 18;
inline constexpr std::uint32_t kProgramCount = 2;
inline constexpr std::uint32_t kRouteCount = 3;

inline constexpr std::uint64_t kModuleIdSpace = 5;
inline constexpr std::uint64_t kParameterIdSpace = 141;
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
    case ParameterId::vco_a_morph: return "vco_a.morph";
    case ParameterId::vco_a_pw: return "vco_a.pw";
    case ParameterId::vco_a_oct_sel: return "vco_a.oct_sel";
    case ParameterId::vco_a_sub_sel: return "vco_a.sub_sel";
    case ParameterId::vco_a_cv_amt: return "vco_a.cv_amt";
    case ParameterId::vco_a_lin_exp: return "vco_a.lin_exp";
    case ParameterId::vco_a_pwm: return "vco_a.pwm";
    case ParameterId::vco_b_tune: return "vco_b.tune";
    case ParameterId::vco_b_morph: return "vco_b.morph";
    case ParameterId::vco_b_pw: return "vco_b.pw";
    case ParameterId::vco_b_cv_amt: return "vco_b.cv_amt";
    case ParameterId::vcf_l_freq: return "vcf.l_freq";
    case ParameterId::vcf_l_res: return "vcf.l_res";
    case ParameterId::vcf_l_mod: return "vcf.l_mod";
    case ParameterId::vcf_dist: return "vcf.dist";
    case ParameterId::vcf_gain: return "vcf.gain";
    case ParameterId::vcf_r_freq: return "vcf.r_freq";
    case ParameterId::vcf_l_bp_lp: return "vcf.l_bp_lp";
    case ParameterId::vcf_link: return "vcf.link";
    case ParameterId::keyboard_behaviour: return "keyboard.behaviour";
    case ParameterId::keyboard_mode: return "keyboard.mode";
    case ParameterId::keyboard_arp_hold: return "keyboard.arp_hold";
    case ParameterId::keyboard_arp_clock: return "keyboard.arp_clock";
    case ParameterId::keyboard_arp_direction: return "keyboard.arp_direction";
    case ParameterId::keyboard_arp_variation: return "keyboard.arp_variation";
    case ParameterId::keyboard_arp_interval: return "keyboard.arp_interval";
    case ParameterId::keyboard_arp_rhythm: return "keyboard.arp_rhythm";
    case ParameterId::keyboard_arp_length: return "keyboard.arp_length";
    case ParameterId::keyboard_seq_run: return "keyboard.seq_run";
    case ParameterId::keyboard_seq_length: return "keyboard.seq_length";
    case ParameterId::keyboard_seq_clock: return "keyboard.seq_clock";
    case ParameterId::keyboard_seq_direction: return "keyboard.seq_direction";
    case ParameterId::keyboard_seq_cv_output: return "keyboard.seq_cv_output";
    case ParameterId::keyboard_seq_rhythm: return "keyboard.seq_rhythm";
    case ParameterId::keyboard_seq_rhythm_length: return "keyboard.seq_rhythm_length";
    case ParameterId::keyboard_portamento_speed: return "keyboard.portamento_speed";
    case ParameterId::keyboard_portamento_legato: return "keyboard.portamento_legato";
    case ParameterId::keyboard_vibrato_speed: return "keyboard.vibrato_speed";
    case ParameterId::keyboard_vibrato_depth: return "keyboard.vibrato_depth";
    case ParameterId::keyboard_vibrato_delay: return "keyboard.vibrato_delay";
    case ParameterId::keyboard_vibrato_pressure: return "keyboard.vibrato_pressure";
    case ParameterId::keyboard_pressure_output: return "keyboard.pressure_output";
    case ParameterId::keyboard_pressure_rise: return "keyboard.pressure_rise";
    case ParameterId::keyboard_pressure_fall: return "keyboard.pressure_fall";
    case ParameterId::keyboard_quantise_load_scale: return "keyboard.quantise_load_scale";
    case ParameterId::keyboard_root_note: return "keyboard.root_note";
    case ParameterId::keyboard_clock_bpm: return "keyboard.clock_bpm";
    case ParameterId::keyboard_calibration_v_oct: return "keyboard.calibration_v_oct";
    case ParameterId::keyboard_calibration_pressure: return "keyboard.calibration_pressure";
    case ParameterId::keyboard_dac_vref: return "keyboard.dac_vref";
    case ParameterId::keyboard_touch_threshold: return "keyboard.touch_threshold";
    case ParameterId::keyboard_release_threshold: return "keyboard.release_threshold";
    case ParameterId::keyboard_pressure_min: return "keyboard.pressure_min";
    case ParameterId::keyboard_pressure_max: return "keyboard.pressure_max";
    case ParameterId::keyboard_mpr121_charge: return "keyboard.mpr121_charge";
    case ParameterId::keyboard_mpr121_discharge: return "keyboard.mpr121_discharge";
    case ParameterId::keyboard_debounce: return "keyboard.debounce";
    case ParameterId::keyboard_encoder_direction: return "keyboard.encoder_direction";
    case ParameterId::envelope_a_a: return "envelope_a.a";
    case ParameterId::envelope_a_r: return "envelope_a.r";
    case ParameterId::program_cathedral_1_x: return "program.cathedral.1.x";
    case ParameterId::program_cathedral_1_y: return "program.cathedral.1.y";
    case ParameterId::program_cathedral_1_z: return "program.cathedral.1.z";
    case ParameterId::program_magic_1_x: return "program.magic.1.x";
    case ParameterId::program_magic_1_y: return "program.magic.1.y";
    case ParameterId::program_magic_1_z: return "program.magic.1.z";
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
