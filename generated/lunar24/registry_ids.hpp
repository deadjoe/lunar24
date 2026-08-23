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
    envelope_a = 4,
    envelope_b = 5,
    lfo_a = 6,
    lfo_b = 7,
    joystick = 8,
    preamp = 9,
    env_follower = 10,
    sequencer = 11,
    mixer = 12,
    effector = 13,
    voices = 14,
    drone_1 = 15,
    drone_2 = 16,
    drone_4 = 17,
    drone_5 = 18,
    drone_3 = 19,
    drone_6 = 20
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
    vco_b_pwm = 30,
    vco_b_lin_exp = 31,
    vco_b_oct_sel = 32,
    vco_b_sub_sel = 33,
    vcf_l_freq = 13,
    vcf_l_res = 14,
    vcf_l_mod = 15,
    vcf_dist = 16,
    vcf_gain = 17,
    vcf_r_freq = 18,
    vcf_l_bp_lp = 19,
    vcf_link = 20,
    vcf_r_res = 34,
    vcf_r_mod = 35,
    vcf_r_bp_lp = 36,
    keyboard_behaviour = 100,
    keyboard_mode = 101,
    keyboard_arp_hold = 102,
    keyboard_arp_direction = 104,
    keyboard_arp_variation = 105,
    keyboard_arp_interval = 106,
    keyboard_arp_length = 108,
    keyboard_seq_run = 109,
    keyboard_seq_length = 110,
    keyboard_seq_direction = 112,
    keyboard_seq_cv_output = 113,
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
    envelope_a_d = 37,
    envelope_a_s = 38,
    envelope_a_hold = 39,
    envelope_a_self_gen = 40,
    envelope_b_a = 141,
    envelope_b_r = 142,
    envelope_b_d = 143,
    envelope_b_s = 144,
    envelope_b_hold = 145,
    envelope_b_self_gen = 146,
    lfo_a_wave = 147,
    lfo_a_rate = 148,
    lfo_a_speed_mult = 149,
    lfo_b_wave = 150,
    lfo_b_rate = 151,
    lfo_b_speed_mult = 152,
    joystick_x = 153,
    joystick_y = 154,
    joystick_offset_x = 155,
    joystick_offset_y = 156,
    preamp_gain = 157,
    env_follower_attack = 158,
    env_follower_release = 159,
    sequencer_pulser = 160,
    sequencer_clock = 161,
    sequencer_stages = 162,
    sequencer_step_cv_1 = 163,
    sequencer_step_cv_2 = 164,
    sequencer_step_cv_3 = 165,
    sequencer_step_cv_4 = 166,
    sequencer_step_cv_5 = 167,
    sequencer_step_gate_1 = 168,
    sequencer_step_gate_2 = 169,
    sequencer_step_gate_3 = 170,
    sequencer_step_gate_4 = 171,
    sequencer_step_gate_5 = 172,
    mixer_ch1_pan = 173,
    mixer_ch1_vol = 174,
    mixer_ch2_pan = 175,
    mixer_ch2_vol = 176,
    mixer_ch3_pan = 177,
    mixer_ch3_vol = 178,
    mixer_ch4_pan = 179,
    mixer_ch4_vol = 180,
    mixer_ch5_pan = 181,
    mixer_ch5_vol = 182,
    mixer_ch6_pan = 183,
    mixer_ch6_vol = 184,
    mixer_ch7_pan = 185,
    mixer_ch7_vol = 186,
    mixer_ch8_pan = 187,
    mixer_ch8_vol = 188,
    mixer_ch9_pan = 189,
    mixer_ch9_vol = 190,
    mixer_ch10_pan = 191,
    mixer_ch10_vol = 192,
    effector_x = 193,
    effector_y = 194,
    effector_z = 195,
    effector_blend = 196,
    effector_master = 197,
    effector_phone = 198,
    effector_select_l = 199,
    effector_select_r = 200,
    drone_1_tune_1 = 201,
    drone_1_tune_2 = 202,
    drone_1_tune_3 = 203,
    drone_1_tune_4 = 204,
    drone_1_tune_5 = 205,
    drone_1_mute_1 = 206,
    drone_1_mute_2 = 207,
    drone_1_mute_3 = 208,
    drone_1_mute_4 = 209,
    drone_1_mute_5 = 210,
    drone_1_mod_1 = 211,
    drone_1_mod_2 = 212,
    drone_1_mod_3 = 213,
    drone_1_mod_4 = 214,
    drone_1_mod_5 = 215,
    drone_1_volt = 216,
    drone_1_att = 217,
    drone_1_rls = 218,
    drone_1_gate_hold = 219,
    drone_2_tune_1 = 220,
    drone_2_tune_2 = 221,
    drone_2_tune_3 = 222,
    drone_2_tune_4 = 223,
    drone_2_tune_5 = 224,
    drone_2_mute_1 = 225,
    drone_2_mute_2 = 226,
    drone_2_mute_3 = 227,
    drone_2_mute_4 = 228,
    drone_2_mute_5 = 229,
    drone_2_mod_1 = 230,
    drone_2_mod_2 = 231,
    drone_2_mod_3 = 232,
    drone_2_mod_4 = 233,
    drone_2_mod_5 = 234,
    drone_2_volt = 235,
    drone_2_att = 236,
    drone_2_rls = 237,
    drone_2_gate_hold = 238,
    drone_4_tune_1 = 239,
    drone_4_tune_2 = 240,
    drone_4_tune_3 = 241,
    drone_4_tune_4 = 242,
    drone_4_tune_5 = 243,
    drone_4_mute_1 = 244,
    drone_4_mute_2 = 245,
    drone_4_mute_3 = 246,
    drone_4_mute_4 = 247,
    drone_4_mute_5 = 248,
    drone_4_mod_1 = 249,
    drone_4_mod_2 = 250,
    drone_4_mod_3 = 251,
    drone_4_mod_4 = 252,
    drone_4_mod_5 = 253,
    drone_4_volt = 254,
    drone_4_att = 255,
    drone_4_rls = 256,
    drone_4_gate_hold = 257,
    drone_5_tune_1 = 258,
    drone_5_tune_2 = 259,
    drone_5_tune_3 = 260,
    drone_5_tune_4 = 261,
    drone_5_tune_5 = 262,
    drone_5_mute_1 = 263,
    drone_5_mute_2 = 264,
    drone_5_mute_3 = 265,
    drone_5_mute_4 = 266,
    drone_5_mute_5 = 267,
    drone_5_mod_1 = 268,
    drone_5_mod_2 = 269,
    drone_5_mod_3 = 270,
    drone_5_mod_4 = 271,
    drone_5_mod_5 = 272,
    drone_5_volt = 273,
    drone_5_att = 274,
    drone_5_rls = 275,
    drone_5_gate_hold = 276,
    drone_3_rate = 277,
    drone_3_mod = 278,
    drone_3_divider = 279,
    drone_3_pitch = 280,
    drone_3_noise = 281,
    drone_3_att = 282,
    drone_3_rls = 283,
    drone_3_hi_low = 284,
    drone_3_fm = 285,
    drone_3_am = 286,
    drone_3_rate_switch = 287,
    drone_3_hold = 288,
    drone_6_rate = 289,
    drone_6_mod = 290,
    drone_6_divider = 291,
    drone_6_pitch = 292,
    drone_6_noise = 293,
    drone_6_att = 294,
    drone_6_rls = 295,
    drone_6_hi_low = 296,
    drone_6_fm = 297,
    drone_6_am = 298,
    drone_6_rate_switch = 299,
    drone_6_hold = 300,
    program_cathedral_1_x = 24,
    program_cathedral_1_y = 25,
    program_cathedral_1_z = 26,
    program_cathedral_2_x = 301,
    program_cathedral_2_y = 302,
    program_cathedral_2_z = 303,
    program_cathedral_3_x = 304,
    program_cathedral_3_y = 305,
    program_cathedral_3_z = 306,
    program_magic_1_x = 27,
    program_magic_1_y = 28,
    program_magic_1_z = 29,
    program_magic_2_x = 307,
    program_magic_2_y = 308,
    program_magic_2_z = 309,
    program_magic_3_x = 310,
    program_magic_3_y = 311,
    program_magic_3_z = 312,
    program_time_1_x = 313,
    program_time_1_y = 314,
    program_time_1_z = 315,
    program_time_2_x = 316,
    program_time_2_y = 317,
    program_time_2_z = 318,
    program_time_3_x = 319,
    program_time_3_y = 320,
    program_time_3_z = 321,
    program_vibrotrem_1_x = 322,
    program_vibrotrem_1_y = 323,
    program_vibrotrem_1_z = 324,
    program_vibrotrem_2_x = 325,
    program_vibrotrem_2_y = 326,
    program_vibrotrem_2_z = 327,
    program_vibrotrem_3_x = 328,
    program_vibrotrem_3_y = 329,
    program_vibrotrem_3_z = 330,
    program_filter_1_x = 331,
    program_filter_1_y = 332,
    program_filter_1_z = 333,
    program_filter_2_x = 334,
    program_filter_2_y = 335,
    program_filter_2_z = 336,
    program_filter_3_x = 337,
    program_filter_3_y = 338,
    program_filter_3_z = 339,
    program_vibe_1_x = 340,
    program_vibe_1_y = 341,
    program_vibe_1_z = 342,
    program_vibe_2_x = 343,
    program_vibe_2_y = 344,
    program_vibe_2_z = 345,
    program_vibe_3_x = 346,
    program_vibe_3_y = 347,
    program_vibe_3_z = 348,
    program_pitch_shifter_1_x = 349,
    program_pitch_shifter_1_y = 350,
    program_pitch_shifter_1_z = 351,
    program_pitch_shifter_2_x = 352,
    program_pitch_shifter_2_y = 353,
    program_pitch_shifter_2_z = 354,
    program_pitch_shifter_3_x = 355,
    program_pitch_shifter_3_y = 356,
    program_pitch_shifter_3_z = 357
};

enum class JackId : std::uint32_t {
    vco_a_cv_in = 0,
    vco_a_v_oct_in = 1,
    vco_a_fm_in = 2,
    vco_a_sync_in = 3,
    vco_a_vca_ctl = 4,
    vco_a_dry_out = 5,
    vco_a_wave_out = 19,
    vco_a_pwm_in = 20,
    vco_b_cv_in = 6,
    vco_b_v_oct_in = 7,
    vco_b_vco_out = 8,
    vco_b_dry_out = 9,
    vco_b_wave_out = 21,
    vco_b_pwm_in = 22,
    vcf_cv_l_in = 10,
    vcf_cv_r_in = 11,
    keyboard_v_oct_out = 13,
    keyboard_gate_left_main_out = 14,
    keyboard_gate_right_out = 15,
    keyboard_clock_in = 16,
    keyboard_pressure_out = 23,
    keyboard_reset_in = 24,
    envelope_a_gate_in = 17,
    envelope_a_env_out = 18,
    envelope_a_vca_cv_out = 25,
    envelope_b_gate_in = 26,
    envelope_b_env_out = 27,
    envelope_b_vca_cv_out = 28,
    lfo_a_cv_out = 29,
    lfo_b_cv_out = 30,
    joystick_x_out = 31,
    joystick_y_out = 32,
    preamp_ext_source_in = 33,
    env_follower_env_out = 34,
    env_follower_gate_out = 35,
    sequencer_ext_clock_in = 36,
    sequencer_clock_out = 37,
    sequencer_cv_out = 38,
    sequencer_gate_out = 39,
    effector_cv_x_in = 40,
    effector_cv_y_in = 41,
    effector_cv_z_in = 42,
    drone_1_cv_mod_in = 43,
    drone_1_gate_in = 44,
    drone_1_env_out = 45,
    drone_2_cv_mod_in = 46,
    drone_2_gate_in = 47,
    drone_2_env_out = 48,
    drone_4_cv_mod_in = 49,
    drone_4_gate_in = 50,
    drone_4_env_out = 51,
    drone_5_cv_mod_in = 52,
    drone_5_gate_in = 53,
    drone_5_env_out = 54,
    drone_3_cv_out = 55,
    drone_3_env_out = 56,
    drone_3_gate_in = 57,
    drone_3_clock_in = 58,
    drone_3_noise_in = 59,
    drone_6_cv_out = 60,
    drone_6_env_out = 61,
    drone_6_gate_in = 62,
    drone_6_clock_in = 63,
    drone_6_noise_in = 64
};

enum class ProgramId : std::uint32_t {
    program_cathedral_1 = 0,
    program_cathedral_2 = 1,
    program_cathedral_3 = 2,
    program_magic_1 = 3,
    program_magic_2 = 4,
    program_magic_3 = 5,
    program_time_1 = 6,
    program_time_2 = 7,
    program_time_3 = 8,
    program_vibrotrem_1 = 9,
    program_vibrotrem_2 = 10,
    program_vibrotrem_3 = 11,
    program_filter_1 = 12,
    program_filter_2 = 13,
    program_filter_3 = 14,
    program_vibe_1 = 15,
    program_vibe_2 = 16,
    program_vibe_3 = 17,
    program_pitch_shifter_1 = 18,
    program_pitch_shifter_2 = 19,
    program_pitch_shifter_3 = 20,
    program_infinity_1 = 21,
    program_infinity_2 = 22,
    program_infinity_3 = 23,
    program_string_ringer_1 = 24,
    program_string_ringer_2 = 25,
    program_string_ringer_3 = 26,
    program_syntex_1_1 = 27,
    program_syntex_1_2 = 28,
    program_syntex_1_3 = 29,
    program_digital_1 = 30,
    program_digital_2 = 31,
    program_digital_3 = 32,
    program_generator_1 = 33,
    program_generator_2 = 34,
    program_generator_3 = 35,
    program_orche_1 = 36,
    program_orche_2 = 37,
    program_orche_3 = 38
};

enum class RouteId : std::uint32_t {
    route_keyboard_v_oct_to_vco = 0,
    route_keyboard_gate_to_eg = 1,
    route_vcf_cv_l_to_cv_r = 2,
    route_keyboard_v_oct_to_vco_b = 3,
    route_vco_b_vco_out_to_cv_in = 4,
    route_keyboard_gate_to_eg_b = 5
};

inline constexpr std::uint32_t kModuleCount = 21;
inline constexpr std::uint32_t kParameterCount = 291;
inline constexpr std::uint32_t kJackCount = 64;
inline constexpr std::uint32_t kProgramCount = 39;
inline constexpr std::uint32_t kRouteCount = 6;

inline constexpr std::uint64_t kModuleIdSpace = 21;
inline constexpr std::uint64_t kParameterIdSpace = 358;
inline constexpr std::uint64_t kJackIdSpace = 65;
inline constexpr std::uint64_t kProgramIdSpace = 39;
inline constexpr std::uint64_t kRouteIdSpace = 6;

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
    case ModuleId::envelope_b: return "envelope_b";
    case ModuleId::lfo_a: return "lfo_a";
    case ModuleId::lfo_b: return "lfo_b";
    case ModuleId::joystick: return "joystick";
    case ModuleId::preamp: return "preamp";
    case ModuleId::env_follower: return "env_follower";
    case ModuleId::sequencer: return "sequencer";
    case ModuleId::mixer: return "mixer";
    case ModuleId::effector: return "effector";
    case ModuleId::voices: return "voices";
    case ModuleId::drone_1: return "drone_1";
    case ModuleId::drone_2: return "drone_2";
    case ModuleId::drone_4: return "drone_4";
    case ModuleId::drone_5: return "drone_5";
    case ModuleId::drone_3: return "drone_3";
    case ModuleId::drone_6: return "drone_6";
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
    case ParameterId::vco_b_pwm: return "vco_b.pwm";
    case ParameterId::vco_b_lin_exp: return "vco_b.lin_exp";
    case ParameterId::vco_b_oct_sel: return "vco_b.oct_sel";
    case ParameterId::vco_b_sub_sel: return "vco_b.sub_sel";
    case ParameterId::vcf_l_freq: return "vcf.l_freq";
    case ParameterId::vcf_l_res: return "vcf.l_res";
    case ParameterId::vcf_l_mod: return "vcf.l_mod";
    case ParameterId::vcf_dist: return "vcf.dist";
    case ParameterId::vcf_gain: return "vcf.gain";
    case ParameterId::vcf_r_freq: return "vcf.r_freq";
    case ParameterId::vcf_l_bp_lp: return "vcf.l_bp_lp";
    case ParameterId::vcf_link: return "vcf.link";
    case ParameterId::vcf_r_res: return "vcf.r_res";
    case ParameterId::vcf_r_mod: return "vcf.r_mod";
    case ParameterId::vcf_r_bp_lp: return "vcf.r_bp_lp";
    case ParameterId::keyboard_behaviour: return "keyboard.behaviour";
    case ParameterId::keyboard_mode: return "keyboard.mode";
    case ParameterId::keyboard_arp_hold: return "keyboard.arp_hold";
    case ParameterId::keyboard_arp_direction: return "keyboard.arp_direction";
    case ParameterId::keyboard_arp_variation: return "keyboard.arp_variation";
    case ParameterId::keyboard_arp_interval: return "keyboard.arp_interval";
    case ParameterId::keyboard_arp_length: return "keyboard.arp_length";
    case ParameterId::keyboard_seq_run: return "keyboard.seq_run";
    case ParameterId::keyboard_seq_length: return "keyboard.seq_length";
    case ParameterId::keyboard_seq_direction: return "keyboard.seq_direction";
    case ParameterId::keyboard_seq_cv_output: return "keyboard.seq_cv_output";
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
    case ParameterId::envelope_a_d: return "envelope_a.d";
    case ParameterId::envelope_a_s: return "envelope_a.s";
    case ParameterId::envelope_a_hold: return "envelope_a.hold";
    case ParameterId::envelope_a_self_gen: return "envelope_a.self_gen";
    case ParameterId::envelope_b_a: return "envelope_b.a";
    case ParameterId::envelope_b_r: return "envelope_b.r";
    case ParameterId::envelope_b_d: return "envelope_b.d";
    case ParameterId::envelope_b_s: return "envelope_b.s";
    case ParameterId::envelope_b_hold: return "envelope_b.hold";
    case ParameterId::envelope_b_self_gen: return "envelope_b.self_gen";
    case ParameterId::lfo_a_wave: return "lfo_a.wave";
    case ParameterId::lfo_a_rate: return "lfo_a.rate";
    case ParameterId::lfo_a_speed_mult: return "lfo_a.speed_mult";
    case ParameterId::lfo_b_wave: return "lfo_b.wave";
    case ParameterId::lfo_b_rate: return "lfo_b.rate";
    case ParameterId::lfo_b_speed_mult: return "lfo_b.speed_mult";
    case ParameterId::joystick_x: return "joystick.x";
    case ParameterId::joystick_y: return "joystick.y";
    case ParameterId::joystick_offset_x: return "joystick.offset_x";
    case ParameterId::joystick_offset_y: return "joystick.offset_y";
    case ParameterId::preamp_gain: return "preamp.gain";
    case ParameterId::env_follower_attack: return "env_follower.attack";
    case ParameterId::env_follower_release: return "env_follower.release";
    case ParameterId::sequencer_pulser: return "sequencer.pulser";
    case ParameterId::sequencer_clock: return "sequencer.clock";
    case ParameterId::sequencer_stages: return "sequencer.stages";
    case ParameterId::sequencer_step_cv_1: return "sequencer.step_cv_1";
    case ParameterId::sequencer_step_cv_2: return "sequencer.step_cv_2";
    case ParameterId::sequencer_step_cv_3: return "sequencer.step_cv_3";
    case ParameterId::sequencer_step_cv_4: return "sequencer.step_cv_4";
    case ParameterId::sequencer_step_cv_5: return "sequencer.step_cv_5";
    case ParameterId::sequencer_step_gate_1: return "sequencer.step_gate_1";
    case ParameterId::sequencer_step_gate_2: return "sequencer.step_gate_2";
    case ParameterId::sequencer_step_gate_3: return "sequencer.step_gate_3";
    case ParameterId::sequencer_step_gate_4: return "sequencer.step_gate_4";
    case ParameterId::sequencer_step_gate_5: return "sequencer.step_gate_5";
    case ParameterId::mixer_ch1_pan: return "mixer.ch1_pan";
    case ParameterId::mixer_ch1_vol: return "mixer.ch1_vol";
    case ParameterId::mixer_ch2_pan: return "mixer.ch2_pan";
    case ParameterId::mixer_ch2_vol: return "mixer.ch2_vol";
    case ParameterId::mixer_ch3_pan: return "mixer.ch3_pan";
    case ParameterId::mixer_ch3_vol: return "mixer.ch3_vol";
    case ParameterId::mixer_ch4_pan: return "mixer.ch4_pan";
    case ParameterId::mixer_ch4_vol: return "mixer.ch4_vol";
    case ParameterId::mixer_ch5_pan: return "mixer.ch5_pan";
    case ParameterId::mixer_ch5_vol: return "mixer.ch5_vol";
    case ParameterId::mixer_ch6_pan: return "mixer.ch6_pan";
    case ParameterId::mixer_ch6_vol: return "mixer.ch6_vol";
    case ParameterId::mixer_ch7_pan: return "mixer.ch7_pan";
    case ParameterId::mixer_ch7_vol: return "mixer.ch7_vol";
    case ParameterId::mixer_ch8_pan: return "mixer.ch8_pan";
    case ParameterId::mixer_ch8_vol: return "mixer.ch8_vol";
    case ParameterId::mixer_ch9_pan: return "mixer.ch9_pan";
    case ParameterId::mixer_ch9_vol: return "mixer.ch9_vol";
    case ParameterId::mixer_ch10_pan: return "mixer.ch10_pan";
    case ParameterId::mixer_ch10_vol: return "mixer.ch10_vol";
    case ParameterId::effector_x: return "effector.x";
    case ParameterId::effector_y: return "effector.y";
    case ParameterId::effector_z: return "effector.z";
    case ParameterId::effector_blend: return "effector.blend";
    case ParameterId::effector_master: return "effector.master";
    case ParameterId::effector_phone: return "effector.phone";
    case ParameterId::effector_select_l: return "effector.select_l";
    case ParameterId::effector_select_r: return "effector.select_r";
    case ParameterId::drone_1_tune_1: return "drone_1.tune_1";
    case ParameterId::drone_1_tune_2: return "drone_1.tune_2";
    case ParameterId::drone_1_tune_3: return "drone_1.tune_3";
    case ParameterId::drone_1_tune_4: return "drone_1.tune_4";
    case ParameterId::drone_1_tune_5: return "drone_1.tune_5";
    case ParameterId::drone_1_mute_1: return "drone_1.mute_1";
    case ParameterId::drone_1_mute_2: return "drone_1.mute_2";
    case ParameterId::drone_1_mute_3: return "drone_1.mute_3";
    case ParameterId::drone_1_mute_4: return "drone_1.mute_4";
    case ParameterId::drone_1_mute_5: return "drone_1.mute_5";
    case ParameterId::drone_1_mod_1: return "drone_1.mod_1";
    case ParameterId::drone_1_mod_2: return "drone_1.mod_2";
    case ParameterId::drone_1_mod_3: return "drone_1.mod_3";
    case ParameterId::drone_1_mod_4: return "drone_1.mod_4";
    case ParameterId::drone_1_mod_5: return "drone_1.mod_5";
    case ParameterId::drone_1_volt: return "drone_1.volt";
    case ParameterId::drone_1_att: return "drone_1.att";
    case ParameterId::drone_1_rls: return "drone_1.rls";
    case ParameterId::drone_1_gate_hold: return "drone_1.gate_hold";
    case ParameterId::drone_2_tune_1: return "drone_2.tune_1";
    case ParameterId::drone_2_tune_2: return "drone_2.tune_2";
    case ParameterId::drone_2_tune_3: return "drone_2.tune_3";
    case ParameterId::drone_2_tune_4: return "drone_2.tune_4";
    case ParameterId::drone_2_tune_5: return "drone_2.tune_5";
    case ParameterId::drone_2_mute_1: return "drone_2.mute_1";
    case ParameterId::drone_2_mute_2: return "drone_2.mute_2";
    case ParameterId::drone_2_mute_3: return "drone_2.mute_3";
    case ParameterId::drone_2_mute_4: return "drone_2.mute_4";
    case ParameterId::drone_2_mute_5: return "drone_2.mute_5";
    case ParameterId::drone_2_mod_1: return "drone_2.mod_1";
    case ParameterId::drone_2_mod_2: return "drone_2.mod_2";
    case ParameterId::drone_2_mod_3: return "drone_2.mod_3";
    case ParameterId::drone_2_mod_4: return "drone_2.mod_4";
    case ParameterId::drone_2_mod_5: return "drone_2.mod_5";
    case ParameterId::drone_2_volt: return "drone_2.volt";
    case ParameterId::drone_2_att: return "drone_2.att";
    case ParameterId::drone_2_rls: return "drone_2.rls";
    case ParameterId::drone_2_gate_hold: return "drone_2.gate_hold";
    case ParameterId::drone_4_tune_1: return "drone_4.tune_1";
    case ParameterId::drone_4_tune_2: return "drone_4.tune_2";
    case ParameterId::drone_4_tune_3: return "drone_4.tune_3";
    case ParameterId::drone_4_tune_4: return "drone_4.tune_4";
    case ParameterId::drone_4_tune_5: return "drone_4.tune_5";
    case ParameterId::drone_4_mute_1: return "drone_4.mute_1";
    case ParameterId::drone_4_mute_2: return "drone_4.mute_2";
    case ParameterId::drone_4_mute_3: return "drone_4.mute_3";
    case ParameterId::drone_4_mute_4: return "drone_4.mute_4";
    case ParameterId::drone_4_mute_5: return "drone_4.mute_5";
    case ParameterId::drone_4_mod_1: return "drone_4.mod_1";
    case ParameterId::drone_4_mod_2: return "drone_4.mod_2";
    case ParameterId::drone_4_mod_3: return "drone_4.mod_3";
    case ParameterId::drone_4_mod_4: return "drone_4.mod_4";
    case ParameterId::drone_4_mod_5: return "drone_4.mod_5";
    case ParameterId::drone_4_volt: return "drone_4.volt";
    case ParameterId::drone_4_att: return "drone_4.att";
    case ParameterId::drone_4_rls: return "drone_4.rls";
    case ParameterId::drone_4_gate_hold: return "drone_4.gate_hold";
    case ParameterId::drone_5_tune_1: return "drone_5.tune_1";
    case ParameterId::drone_5_tune_2: return "drone_5.tune_2";
    case ParameterId::drone_5_tune_3: return "drone_5.tune_3";
    case ParameterId::drone_5_tune_4: return "drone_5.tune_4";
    case ParameterId::drone_5_tune_5: return "drone_5.tune_5";
    case ParameterId::drone_5_mute_1: return "drone_5.mute_1";
    case ParameterId::drone_5_mute_2: return "drone_5.mute_2";
    case ParameterId::drone_5_mute_3: return "drone_5.mute_3";
    case ParameterId::drone_5_mute_4: return "drone_5.mute_4";
    case ParameterId::drone_5_mute_5: return "drone_5.mute_5";
    case ParameterId::drone_5_mod_1: return "drone_5.mod_1";
    case ParameterId::drone_5_mod_2: return "drone_5.mod_2";
    case ParameterId::drone_5_mod_3: return "drone_5.mod_3";
    case ParameterId::drone_5_mod_4: return "drone_5.mod_4";
    case ParameterId::drone_5_mod_5: return "drone_5.mod_5";
    case ParameterId::drone_5_volt: return "drone_5.volt";
    case ParameterId::drone_5_att: return "drone_5.att";
    case ParameterId::drone_5_rls: return "drone_5.rls";
    case ParameterId::drone_5_gate_hold: return "drone_5.gate_hold";
    case ParameterId::drone_3_rate: return "drone_3.rate";
    case ParameterId::drone_3_mod: return "drone_3.mod";
    case ParameterId::drone_3_divider: return "drone_3.divider";
    case ParameterId::drone_3_pitch: return "drone_3.pitch";
    case ParameterId::drone_3_noise: return "drone_3.noise";
    case ParameterId::drone_3_att: return "drone_3.att";
    case ParameterId::drone_3_rls: return "drone_3.rls";
    case ParameterId::drone_3_hi_low: return "drone_3.hi_low";
    case ParameterId::drone_3_fm: return "drone_3.fm";
    case ParameterId::drone_3_am: return "drone_3.am";
    case ParameterId::drone_3_rate_switch: return "drone_3.rate_switch";
    case ParameterId::drone_3_hold: return "drone_3.hold";
    case ParameterId::drone_6_rate: return "drone_6.rate";
    case ParameterId::drone_6_mod: return "drone_6.mod";
    case ParameterId::drone_6_divider: return "drone_6.divider";
    case ParameterId::drone_6_pitch: return "drone_6.pitch";
    case ParameterId::drone_6_noise: return "drone_6.noise";
    case ParameterId::drone_6_att: return "drone_6.att";
    case ParameterId::drone_6_rls: return "drone_6.rls";
    case ParameterId::drone_6_hi_low: return "drone_6.hi_low";
    case ParameterId::drone_6_fm: return "drone_6.fm";
    case ParameterId::drone_6_am: return "drone_6.am";
    case ParameterId::drone_6_rate_switch: return "drone_6.rate_switch";
    case ParameterId::drone_6_hold: return "drone_6.hold";
    case ParameterId::program_cathedral_1_x: return "program.cathedral.1.x";
    case ParameterId::program_cathedral_1_y: return "program.cathedral.1.y";
    case ParameterId::program_cathedral_1_z: return "program.cathedral.1.z";
    case ParameterId::program_cathedral_2_x: return "program.cathedral.2.x";
    case ParameterId::program_cathedral_2_y: return "program.cathedral.2.y";
    case ParameterId::program_cathedral_2_z: return "program.cathedral.2.z";
    case ParameterId::program_cathedral_3_x: return "program.cathedral.3.x";
    case ParameterId::program_cathedral_3_y: return "program.cathedral.3.y";
    case ParameterId::program_cathedral_3_z: return "program.cathedral.3.z";
    case ParameterId::program_magic_1_x: return "program.magic.1.x";
    case ParameterId::program_magic_1_y: return "program.magic.1.y";
    case ParameterId::program_magic_1_z: return "program.magic.1.z";
    case ParameterId::program_magic_2_x: return "program.magic.2.x";
    case ParameterId::program_magic_2_y: return "program.magic.2.y";
    case ParameterId::program_magic_2_z: return "program.magic.2.z";
    case ParameterId::program_magic_3_x: return "program.magic.3.x";
    case ParameterId::program_magic_3_y: return "program.magic.3.y";
    case ParameterId::program_magic_3_z: return "program.magic.3.z";
    case ParameterId::program_time_1_x: return "program.time.1.x";
    case ParameterId::program_time_1_y: return "program.time.1.y";
    case ParameterId::program_time_1_z: return "program.time.1.z";
    case ParameterId::program_time_2_x: return "program.time.2.x";
    case ParameterId::program_time_2_y: return "program.time.2.y";
    case ParameterId::program_time_2_z: return "program.time.2.z";
    case ParameterId::program_time_3_x: return "program.time.3.x";
    case ParameterId::program_time_3_y: return "program.time.3.y";
    case ParameterId::program_time_3_z: return "program.time.3.z";
    case ParameterId::program_vibrotrem_1_x: return "program.vibrotrem.1.x";
    case ParameterId::program_vibrotrem_1_y: return "program.vibrotrem.1.y";
    case ParameterId::program_vibrotrem_1_z: return "program.vibrotrem.1.z";
    case ParameterId::program_vibrotrem_2_x: return "program.vibrotrem.2.x";
    case ParameterId::program_vibrotrem_2_y: return "program.vibrotrem.2.y";
    case ParameterId::program_vibrotrem_2_z: return "program.vibrotrem.2.z";
    case ParameterId::program_vibrotrem_3_x: return "program.vibrotrem.3.x";
    case ParameterId::program_vibrotrem_3_y: return "program.vibrotrem.3.y";
    case ParameterId::program_vibrotrem_3_z: return "program.vibrotrem.3.z";
    case ParameterId::program_filter_1_x: return "program.filter.1.x";
    case ParameterId::program_filter_1_y: return "program.filter.1.y";
    case ParameterId::program_filter_1_z: return "program.filter.1.z";
    case ParameterId::program_filter_2_x: return "program.filter.2.x";
    case ParameterId::program_filter_2_y: return "program.filter.2.y";
    case ParameterId::program_filter_2_z: return "program.filter.2.z";
    case ParameterId::program_filter_3_x: return "program.filter.3.x";
    case ParameterId::program_filter_3_y: return "program.filter.3.y";
    case ParameterId::program_filter_3_z: return "program.filter.3.z";
    case ParameterId::program_vibe_1_x: return "program.vibe.1.x";
    case ParameterId::program_vibe_1_y: return "program.vibe.1.y";
    case ParameterId::program_vibe_1_z: return "program.vibe.1.z";
    case ParameterId::program_vibe_2_x: return "program.vibe.2.x";
    case ParameterId::program_vibe_2_y: return "program.vibe.2.y";
    case ParameterId::program_vibe_2_z: return "program.vibe.2.z";
    case ParameterId::program_vibe_3_x: return "program.vibe.3.x";
    case ParameterId::program_vibe_3_y: return "program.vibe.3.y";
    case ParameterId::program_vibe_3_z: return "program.vibe.3.z";
    case ParameterId::program_pitch_shifter_1_x: return "program.pitch_shifter.1.x";
    case ParameterId::program_pitch_shifter_1_y: return "program.pitch_shifter.1.y";
    case ParameterId::program_pitch_shifter_1_z: return "program.pitch_shifter.1.z";
    case ParameterId::program_pitch_shifter_2_x: return "program.pitch_shifter.2.x";
    case ParameterId::program_pitch_shifter_2_y: return "program.pitch_shifter.2.y";
    case ParameterId::program_pitch_shifter_2_z: return "program.pitch_shifter.2.z";
    case ParameterId::program_pitch_shifter_3_x: return "program.pitch_shifter.3.x";
    case ParameterId::program_pitch_shifter_3_y: return "program.pitch_shifter.3.y";
    case ParameterId::program_pitch_shifter_3_z: return "program.pitch_shifter.3.z";
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
    case JackId::vco_a_wave_out: return "vco_a.wave_out";
    case JackId::vco_a_pwm_in: return "vco_a.pwm_in";
    case JackId::vco_b_cv_in: return "vco_b.cv_in";
    case JackId::vco_b_v_oct_in: return "vco_b.v_oct_in";
    case JackId::vco_b_vco_out: return "vco_b.vco_out";
    case JackId::vco_b_dry_out: return "vco_b.dry_out";
    case JackId::vco_b_wave_out: return "vco_b.wave_out";
    case JackId::vco_b_pwm_in: return "vco_b.pwm_in";
    case JackId::vcf_cv_l_in: return "vcf.cv_l_in";
    case JackId::vcf_cv_r_in: return "vcf.cv_r_in";
    case JackId::keyboard_v_oct_out: return "keyboard.v_oct_out";
    case JackId::keyboard_gate_left_main_out: return "keyboard.gate_left_main_out";
    case JackId::keyboard_gate_right_out: return "keyboard.gate_right_out";
    case JackId::keyboard_clock_in: return "keyboard.clock_in";
    case JackId::keyboard_pressure_out: return "keyboard.pressure_out";
    case JackId::keyboard_reset_in: return "keyboard.reset_in";
    case JackId::envelope_a_gate_in: return "envelope_a.gate_in";
    case JackId::envelope_a_env_out: return "envelope_a.env_out";
    case JackId::envelope_a_vca_cv_out: return "envelope_a.vca_cv_out";
    case JackId::envelope_b_gate_in: return "envelope_b.gate_in";
    case JackId::envelope_b_env_out: return "envelope_b.env_out";
    case JackId::envelope_b_vca_cv_out: return "envelope_b.vca_cv_out";
    case JackId::lfo_a_cv_out: return "lfo_a.cv_out";
    case JackId::lfo_b_cv_out: return "lfo_b.cv_out";
    case JackId::joystick_x_out: return "joystick.x_out";
    case JackId::joystick_y_out: return "joystick.y_out";
    case JackId::preamp_ext_source_in: return "preamp.ext_source_in";
    case JackId::env_follower_env_out: return "env_follower.env_out";
    case JackId::env_follower_gate_out: return "env_follower.gate_out";
    case JackId::sequencer_ext_clock_in: return "sequencer.ext_clock_in";
    case JackId::sequencer_clock_out: return "sequencer.clock_out";
    case JackId::sequencer_cv_out: return "sequencer.cv_out";
    case JackId::sequencer_gate_out: return "sequencer.gate_out";
    case JackId::effector_cv_x_in: return "effector.cv_x_in";
    case JackId::effector_cv_y_in: return "effector.cv_y_in";
    case JackId::effector_cv_z_in: return "effector.cv_z_in";
    case JackId::drone_1_cv_mod_in: return "drone_1.cv_mod_in";
    case JackId::drone_1_gate_in: return "drone_1.gate_in";
    case JackId::drone_1_env_out: return "drone_1.env_out";
    case JackId::drone_2_cv_mod_in: return "drone_2.cv_mod_in";
    case JackId::drone_2_gate_in: return "drone_2.gate_in";
    case JackId::drone_2_env_out: return "drone_2.env_out";
    case JackId::drone_4_cv_mod_in: return "drone_4.cv_mod_in";
    case JackId::drone_4_gate_in: return "drone_4.gate_in";
    case JackId::drone_4_env_out: return "drone_4.env_out";
    case JackId::drone_5_cv_mod_in: return "drone_5.cv_mod_in";
    case JackId::drone_5_gate_in: return "drone_5.gate_in";
    case JackId::drone_5_env_out: return "drone_5.env_out";
    case JackId::drone_3_cv_out: return "drone_3.cv_out";
    case JackId::drone_3_env_out: return "drone_3.env_out";
    case JackId::drone_3_gate_in: return "drone_3.gate_in";
    case JackId::drone_3_clock_in: return "drone_3.clock_in";
    case JackId::drone_3_noise_in: return "drone_3.noise_in";
    case JackId::drone_6_cv_out: return "drone_6.cv_out";
    case JackId::drone_6_env_out: return "drone_6.env_out";
    case JackId::drone_6_gate_in: return "drone_6.gate_in";
    case JackId::drone_6_clock_in: return "drone_6.clock_in";
    case JackId::drone_6_noise_in: return "drone_6.noise_in";
  }
  return "(unknown Jack)";
}

inline constexpr std::string_view program_id_string(ProgramId id) {
  switch (id) {
    case ProgramId::program_cathedral_1: return "program.cathedral.1";
    case ProgramId::program_cathedral_2: return "program.cathedral.2";
    case ProgramId::program_cathedral_3: return "program.cathedral.3";
    case ProgramId::program_magic_1: return "program.magic.1";
    case ProgramId::program_magic_2: return "program.magic.2";
    case ProgramId::program_magic_3: return "program.magic.3";
    case ProgramId::program_time_1: return "program.time.1";
    case ProgramId::program_time_2: return "program.time.2";
    case ProgramId::program_time_3: return "program.time.3";
    case ProgramId::program_vibrotrem_1: return "program.vibrotrem.1";
    case ProgramId::program_vibrotrem_2: return "program.vibrotrem.2";
    case ProgramId::program_vibrotrem_3: return "program.vibrotrem.3";
    case ProgramId::program_filter_1: return "program.filter.1";
    case ProgramId::program_filter_2: return "program.filter.2";
    case ProgramId::program_filter_3: return "program.filter.3";
    case ProgramId::program_vibe_1: return "program.vibe.1";
    case ProgramId::program_vibe_2: return "program.vibe.2";
    case ProgramId::program_vibe_3: return "program.vibe.3";
    case ProgramId::program_pitch_shifter_1: return "program.pitch_shifter.1";
    case ProgramId::program_pitch_shifter_2: return "program.pitch_shifter.2";
    case ProgramId::program_pitch_shifter_3: return "program.pitch_shifter.3";
    case ProgramId::program_infinity_1: return "program.infinity.1";
    case ProgramId::program_infinity_2: return "program.infinity.2";
    case ProgramId::program_infinity_3: return "program.infinity.3";
    case ProgramId::program_string_ringer_1: return "program.string_ringer.1";
    case ProgramId::program_string_ringer_2: return "program.string_ringer.2";
    case ProgramId::program_string_ringer_3: return "program.string_ringer.3";
    case ProgramId::program_syntex_1_1: return "program.syntex_1.1";
    case ProgramId::program_syntex_1_2: return "program.syntex_1.2";
    case ProgramId::program_syntex_1_3: return "program.syntex_1.3";
    case ProgramId::program_digital_1: return "program.digital.1";
    case ProgramId::program_digital_2: return "program.digital.2";
    case ProgramId::program_digital_3: return "program.digital.3";
    case ProgramId::program_generator_1: return "program.generator.1";
    case ProgramId::program_generator_2: return "program.generator.2";
    case ProgramId::program_generator_3: return "program.generator.3";
    case ProgramId::program_orche_1: return "program.orche.1";
    case ProgramId::program_orche_2: return "program.orche.2";
    case ProgramId::program_orche_3: return "program.orche.3";
  }
  return "(unknown Program)";
}

inline constexpr std::string_view route_id_string(RouteId id) {
  switch (id) {
    case RouteId::route_keyboard_v_oct_to_vco: return "route.keyboard_v_oct_to_vco";
    case RouteId::route_keyboard_gate_to_eg: return "route.keyboard_gate_to_eg";
    case RouteId::route_vcf_cv_l_to_cv_r: return "route.vcf_cv_l_to_cv_r";
    case RouteId::route_keyboard_v_oct_to_vco_b: return "route.keyboard_v_oct_to_vco_b";
    case RouteId::route_vco_b_vco_out_to_cv_in: return "route.vco_b_vco_out_to_cv_in";
    case RouteId::route_keyboard_gate_to_eg_b: return "route.keyboard_gate_to_eg_b";
  }
  return "(unknown Route)";
}

}  // namespace lunar24::core
