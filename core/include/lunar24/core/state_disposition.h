// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DeviceStateV1 parameter disposition (design/07 §9, task #75 revision 2).
//
// The SINGLE truth source for the disposition of every landed ParameterId: which
// of the five classes a parameter falls into. The table is sparse (keyed by the
// stable ParameterId — array position != id, so it is NOT a dense index), holes /
// slack are deliberately NOT in it, and a hole/slack/unknown id resolves to
// invalid_unlanded. There is no copied module field and no second
// has_real_unit_transfer truth source: the registry ParameterDescriptor
// (registry.hpp) remains the one source for a parameter's range/unit/initial,
// read through find_parameter() below.
//
// Classification (Codex revision-2, msg 40bef2eb — supersedes 135/35/125/50/0):
// exactly five classes with counts. GH#15 D1 (mod) moved 2 and D2 (hi_low + rate_switch)
// moved 4 from transfer_unavailable -> applied_to_dsp:
//   175 applied_to_DSP / 35 applied_to_keyboard / 125 preserved_deferred_P6_P8 /
//   10 transfer_unavailable / 0 invalid_unlanded == 345 landed.
// The remaining 10 transfer_unavailable are precisely the parameters with NO real
// runtime consumer: vco_a.pwm(8), vco_b.pwm(30), and the drone3/6 DIVIDER, ATT, RLS,
// HOLD pair (each voice's {divider, att, rls, hold}). The other 175 must reach a real
// DSP apply in later slices; the software norm->DSP mappings are marked PROVISIONAL
// there, not as unavailable.

#pragma once

#include <cstdint>

#include <lunar24/registry.hpp>     // registry::kParameters (ParameterDescriptor)
#include <lunar24/registry_ids.hpp>  // ParameterId, counts

namespace lunar24::core {

enum class StateDisposition : std::uint8_t {
  applied_to_dsp = 0,
  applied_to_keyboard = 1,
  preserved_deferred_p6_p8 = 2,
  transfer_unavailable = 3,
  invalid_unlanded = 4,
};

struct DispositionEntry {
  ParameterId id;
  StateDisposition disposition;
};

// The disposition table covers EXACTLY the landed parameters: its row count is tied
// directly to the generated registry count (kParameterCount), not a hand-written 345.
// A sparse table that ever disagrees with the registry's landed set becomes a
// compile-time excess-initializer error (too many rows) or a zero-filled duplicate-id
// row + class-count static_assert failure (too few rows), never a silent n.
inline constexpr std::uint32_t kDeviceStateDispositionCount = kParameterCount;

inline constexpr DispositionEntry kDeviceStateDisposition[kDeviceStateDispositionCount] = {
    { ParameterId::vco_a_tune, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_morph, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_pw, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_oct_sel, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_sub_sel, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_cv_amt, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_lin_exp, StateDisposition::applied_to_dsp },
    { ParameterId::vco_a_pwm, StateDisposition::transfer_unavailable },
    { ParameterId::vco_b_tune, StateDisposition::applied_to_dsp },
    { ParameterId::vco_b_morph, StateDisposition::applied_to_dsp },
    { ParameterId::vco_b_pw, StateDisposition::applied_to_dsp },
    { ParameterId::vco_b_cv_amt, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_l_freq, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_l_res, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_l_mod, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_dist, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_gain, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_r_freq, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_l_bp_lp, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_link, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_a, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_r, StateDisposition::applied_to_dsp },
    { ParameterId::program_cathedral_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::vco_b_pwm, StateDisposition::transfer_unavailable },
    { ParameterId::vco_b_lin_exp, StateDisposition::applied_to_dsp },
    { ParameterId::vco_b_oct_sel, StateDisposition::applied_to_dsp },
    { ParameterId::vco_b_sub_sel, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_r_res, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_r_mod, StateDisposition::applied_to_dsp },
    { ParameterId::vcf_r_bp_lp, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_d, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_s, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_hold, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_a_self_gen, StateDisposition::applied_to_dsp },
    { ParameterId::keyboard_behaviour, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_mode, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_arp_hold, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_arp_direction, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_arp_variation, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_arp_interval, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_arp_length, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_seq_run, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_seq_length, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_seq_direction, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_seq_cv_output, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_seq_rhythm_length, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_portamento_speed, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_portamento_legato, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_vibrato_speed, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_vibrato_depth, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_vibrato_delay, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_vibrato_pressure, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_pressure_output, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_pressure_rise, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_pressure_fall, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_quantise_load_scale, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_root_note, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_clock_bpm, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_calibration_v_oct, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_calibration_pressure, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_dac_vref, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_touch_threshold, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_release_threshold, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_pressure_min, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_pressure_max, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_mpr121_charge, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_mpr121_discharge, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_debounce, StateDisposition::applied_to_keyboard },
    { ParameterId::keyboard_encoder_direction, StateDisposition::applied_to_keyboard },
    { ParameterId::envelope_b_a, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_b_r, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_b_d, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_b_s, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_b_hold, StateDisposition::applied_to_dsp },
    { ParameterId::envelope_b_self_gen, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_a_wave, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_a_rate, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_a_speed_mult, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_b_wave, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_b_rate, StateDisposition::applied_to_dsp },
    { ParameterId::lfo_b_speed_mult, StateDisposition::applied_to_dsp },
    { ParameterId::joystick_x, StateDisposition::applied_to_dsp },
    { ParameterId::joystick_y, StateDisposition::applied_to_dsp },
    { ParameterId::joystick_offset_x, StateDisposition::applied_to_dsp },
    { ParameterId::joystick_offset_y, StateDisposition::applied_to_dsp },
    { ParameterId::preamp_gain, StateDisposition::applied_to_dsp },
    { ParameterId::env_follower_attack, StateDisposition::applied_to_dsp },
    { ParameterId::env_follower_release, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_pulser, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_clock, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_stages, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_cv_1, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_cv_2, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_cv_3, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_cv_4, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_cv_5, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_gate_1, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_gate_2, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_gate_3, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_gate_4, StateDisposition::applied_to_dsp },
    { ParameterId::sequencer_step_gate_5, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch1_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch1_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch2_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch2_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch3_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch3_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch4_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch4_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch5_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch5_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch6_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch6_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch7_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch7_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch8_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch8_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch9_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch9_vol, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch10_pan, StateDisposition::applied_to_dsp },
    { ParameterId::mixer_ch10_vol, StateDisposition::applied_to_dsp },
    { ParameterId::effector_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_blend, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_master, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_phone, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_select_l, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::effector_select_r, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::drone_1_tune_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_tune_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_tune_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_tune_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_tune_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mute_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mute_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mute_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mute_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mute_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mod_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mod_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mod_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mod_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_mod_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_volt, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_att, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_rls, StateDisposition::applied_to_dsp },
    { ParameterId::drone_1_gate_hold, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_tune_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_tune_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_tune_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_tune_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_tune_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mute_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mute_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mute_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mute_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mute_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mod_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mod_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mod_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mod_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_mod_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_volt, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_att, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_rls, StateDisposition::applied_to_dsp },
    { ParameterId::drone_2_gate_hold, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_tune_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_tune_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_tune_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_tune_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_tune_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mute_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mute_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mute_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mute_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mute_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mod_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mod_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mod_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mod_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_mod_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_volt, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_att, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_rls, StateDisposition::applied_to_dsp },
    { ParameterId::drone_4_gate_hold, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_tune_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_tune_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_tune_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_tune_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_tune_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mute_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mute_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mute_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mute_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mute_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mod_1, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mod_2, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mod_3, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mod_4, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_mod_5, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_volt, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_att, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_rls, StateDisposition::applied_to_dsp },
    { ParameterId::drone_5_gate_hold, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_rate, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_mod, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_divider, StateDisposition::transfer_unavailable },
    { ParameterId::drone_3_pitch, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_noise, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_att, StateDisposition::transfer_unavailable },
    { ParameterId::drone_3_rls, StateDisposition::transfer_unavailable },
    { ParameterId::drone_3_hi_low, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_fm, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_am, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_rate_switch, StateDisposition::applied_to_dsp },
    { ParameterId::drone_3_hold, StateDisposition::transfer_unavailable },
    { ParameterId::drone_6_rate, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_mod, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_divider, StateDisposition::transfer_unavailable },
    { ParameterId::drone_6_pitch, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_noise, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_att, StateDisposition::transfer_unavailable },
    { ParameterId::drone_6_rls, StateDisposition::transfer_unavailable },
    { ParameterId::drone_6_hi_low, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_fm, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_am, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_rate_switch, StateDisposition::applied_to_dsp },
    { ParameterId::drone_6_hold, StateDisposition::transfer_unavailable },
    { ParameterId::program_cathedral_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_cathedral_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_magic_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_time_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibrotrem_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_filter_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_vibe_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_pitch_shifter_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_infinity_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_string_ringer_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_syntex_1_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_digital_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_generator_3_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_1_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_1_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_1_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_2_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_2_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_2_z, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_3_x, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_3_y, StateDisposition::preserved_deferred_p6_p8 },
    { ParameterId::program_orche_3_z, StateDisposition::preserved_deferred_p6_p8 },
};

// Compile-time class counts lock the classification (Codex 169/35/125/16/0).
inline constexpr std::uint32_t count_disposition(StateDisposition d) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < kDeviceStateDispositionCount; ++i)
    if (kDeviceStateDisposition[i].disposition == d) ++n;
  return n;
}
static_assert(count_disposition(StateDisposition::applied_to_dsp) == 175, "applied_to_DSP count");
static_assert(count_disposition(StateDisposition::applied_to_keyboard) == 35, "applied_to_keyboard count");
static_assert(count_disposition(StateDisposition::preserved_deferred_p6_p8) == 125, "preserved_deferred_P6_P8 count");
static_assert(count_disposition(StateDisposition::transfer_unavailable) == 10, "transfer_unavailable count");
static_assert(count_disposition(StateDisposition::invalid_unlanded) == 0, "invalid_unlanded count");

// Disposition of a ParameterId. A landed id -> its class; a hole/slack/unknown id
// (anything not in the table, including the 412..423 capacity slack) ->
// invalid_unlanded.
inline constexpr StateDisposition disposition_of(ParameterId id) noexcept {
  for (std::uint32_t i = 0; i < kDeviceStateDispositionCount; ++i)
    if (kDeviceStateDisposition[i].id == id) return kDeviceStateDisposition[i].disposition;
  return StateDisposition::invalid_unlanded;
}

inline constexpr bool is_landed_parameter(ParameterId id) noexcept {
  return disposition_of(id) != StateDisposition::invalid_unlanded;
}

// Read a landed ParameterDescriptor by stable ParameterId from the registry (the
// single truth for range/unit/initial). Returns nullptr for a hole/slack id.
inline const ParameterDescriptor* find_parameter(ParameterId id) noexcept {
  for (std::uint32_t i = 0; i < kParameterCount; ++i)
    if (registry::kParameters[i].id == id) return &registry::kParameters[i];
  return nullptr;
}

// Read a landed ProgramDescriptor by stable ProgramId from the registry (the single
// truth for "is this ProgramId selectable"). Returns nullptr for a non-landed / out-of-
// set ProgramId, so the validator and the default never assume a dense 0..kProgramCount
// index layout — they look the id up kPrograms by its own stable value.
inline const ProgramDescriptor* find_program(ProgramId id) noexcept {
  for (std::uint32_t i = 0; i < kProgramCount; ++i)
    if (registry::kPrograms[i].id == id) return &registry::kPrograms[i];
  return nullptr;
}

}  // namespace lunar24::core
