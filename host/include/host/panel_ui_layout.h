// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_ui_layout.h — where every control sits on the 2400 x 1552 panel.
//
// Positions are taken from the official Solar 42N panel drawing (control centres and
// sizes measured from the PDF's vector shapes); labels, frames and title tabs come from
// panel_art.generated.h. Framework-free: the IGraphics editor draws from this list, the
// SVG preview tool renders it, and tests check it.
//
// The keyboard's 36 menu settings live behind the display/encoder on the hardware; here they
// are on a KEYBOARD MENU overlay that the encoder opens (`menu` = true; keyboard_menu_view.h).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <host/keyboard_menu_view.h>
#include <lunar24/registry.hpp>

namespace lunar24::host {

using core::JackId;
using core::ParameterId;

enum class WidgetKind : std::uint8_t {
  Knob,        // continuous parameter (id = ParameterId)
  Button,      // round latching push button for a 2-position parameter
  Toggle,      // lever switch for a 2- or 3-position parameter
  Jack,        // patch point (id = JackId)
  Plate,       // one of the 12 touch plates (id = 0..11)
  Joystick,    // the stick (id = joystick.x, id2 = joystick.y)
  Cartridge,   // effector cartridge slot (id 0) and its button (id 1): click = next cartridge
  DroneKey,    // DRONE VOICES key (id = 0..5 -> drone 1..6)
  Encoder,     // keyboard encoder: opens the keyboard menu
  OctaveKey,   // keyboard arrow buttons (id 0 = down, 1 = up)
  Display,     // keyboard display (shows octave / menu state)
  MasterMute,  // app-level MUTE button next to DRONE VOICES: silences every output
  MidiSettings,  // app-level MIDI button below MUTE: opens the MIDI settings overlay
  Record,        // app-level REC button, where the hardware's headphone socket is
  RecordSource,  // app-level WET / DRY / ALL selector for REC, where the PHONE knob is
  PhotoSensor,   // a classic drone's light-sensitive eye (id = group 0..3 = drone 1/2/4/5)
  OscStatus,     // a classic drone's OSC STATUS lamps, one per generator (id = group 0..3)
};

// The printed indicator LEDs Lunar 24 lights, in StandaloneAudioEngine::PanelLed order: centre of
// each LED on the panel (each is one of art::kLeds). The joystick's two LEDs stay unlit: the
// on-screen stick already shows its position.
struct LedPos { double x, y; };
inline constexpr LedPos kPanelLedPos[] = {
    {62.1, 485.7}, {456.2, 485.7}, {62.1, 793.8}, {1638.2, 485.7}, {2032.3, 485.7}, {2032.5, 793.8},  // drones 1-6
    {1042.4, 744.3}, {1515.3, 744.3},                       // envelope A, B
    {137.7, 977.4}, {2220.0, 977.1},                        // LFO A, B
    {936.5, 910.6}, {1054.7, 910.6}, {1173.0, 910.6}, {1291.1, 910.6}, {1409.4, 910.6},  // steps 1-5
    {1706.2, 932.8},                                        // preamp clip
    {1950.0, 932.8}, {2023.9, 932.8},                       // envelope follower level, gate
    {370.0, 762.8}, {2340.4, 762.8},                        // drone 3, 6 S&H
};

// Knob cap colours of the panel sections.
enum class Cap : std::uint8_t { Black, Teal, Green, Orange, Red, Grey, Dark };

struct Widget {
  WidgetKind kind = WidgetKind::Knob;
  double cx = 0, cy = 0;       // centre (panel units)
  double w = 0, h = 0;         // hit box, centred on (cx, cy)
  std::uint32_t id = 0;
  std::uint32_t id2 = 0;
  Cap cap = Cap::Black;
  bool menu = false;           // lives on the keyboard menu overlay
  // Lever switches: the parameter index at each lever position, top to bottom, so the
  // lever points at the matching panel label (e.g. VCO octave: up = "+3", down = "low").
  std::uint8_t leverIndex[3] = {0, 1, 2};
  std::string label;           // drawn for menu items (panel labels come from the art)
  double x() const { return cx - w / 2; }
  double y() const { return cy - h / 2; }
};

// The keyboard menu overlay area (covers the touch plates while open); its tabs, settings,
// presets, sequencer steps and rhythm pads are laid out in keyboard_menu_view.h.
inline constexpr double kMenuX0 = 410, kMenuY0 = 1112, kMenuX1 = 1990, kMenuY1 = 1482;

// The keyboard menu and the MIDI settings overlay share this area. While either is open, a
// cable dropped on it lands on the overlay, never on a keyboard jack hidden underneath.
inline bool overlay_hides_point(bool overlayOpen, double x, double y) {
  return overlayOpen && x >= kMenuX0 && x < kMenuX1 && y >= kMenuY0 && y < kMenuY1;
}

// Mouse wheel / trackpad on the red encoder -> octave steps. A MacBook trackpad sends a
// stream of small deltas, zero-delta events (sideways swipes, gesture start/end) and more
// "momentum" events after the fingers lift, so counting every event as a step ran the
// octave to the end of its range. One gesture (events less than kGap apart) moves at most
// one octave; zero deltas are ignored. Up = octave up, unless the keyboard menu's
// SERVICE > ENCODER DIRECTION is set to reversed (1).
class EncoderWheel {
 public:
  static constexpr double kGap = 0.25;       // seconds of quiet that end a gesture (tuned by hand)
  static constexpr double kThreshold = 0.5;  // summed delta that makes a step (one mouse notch = 1)
  // secondsSincePrevious: time since the previous wheel event. Returns -1, 0 or +1.
  int step(double delta, double secondsSincePrevious, double encoderDirection) {
    if (secondsSincePrevious > kGap) {  // a new gesture
      sum_ = 0.0;
      stepped_ = false;
    }
    if (delta == 0.0 || stepped_) return 0;
    sum_ += delta;
    if (sum_ > -kThreshold && sum_ < kThreshold) return 0;
    stepped_ = true;
    const int up = sum_ > 0 ? 1 : -1;
    return encoderDirection > 0.5 ? -up : up;
  }

 private:
  double sum_ = 0.0;
  bool stepped_ = false;
};

// Registry jacks that the official panel does not show (kept in the engine, not patchable
// from the UI): the VCOs' separate wave outputs and the envelopes' VCA-CV outputs.
inline constexpr JackId kJacksNotOnPanel[] = {
    JackId::vco_a_wave_out, JackId::vco_b_wave_out,
    JackId::envelope_a_vca_cv_out, JackId::envelope_b_vca_cv_out};

inline std::vector<Widget> build_panel_layout() {
  using P = ParameterId;
  using J = JackId;
  std::vector<Widget> ws;
  ws.reserve(420);

  auto add = [&ws](WidgetKind k, double cx, double cy, double w, double h, std::uint32_t id,
                   Cap cap = Cap::Black, std::uint32_t id2 = 0) {
    Widget wd;
    wd.kind = k; wd.cx = cx; wd.cy = cy; wd.w = w; wd.h = h; wd.id = id; wd.id2 = id2; wd.cap = cap;
    ws.push_back(wd);
  };
  // Knob sizes: skirted knob (cap 46), small plain knob (cap 39), big knobs.
  auto knob = [&](P id, double cx, double cy, Cap cap, double size = 60) {
    add(WidgetKind::Knob, cx, cy, size, size, static_cast<std::uint32_t>(id), cap);
  };
  auto plain = [&](P id, double cx, double cy, double size = 40) {
    add(WidgetKind::Knob, cx, cy, size, size, static_cast<std::uint32_t>(id), Cap::Black);
  };
  auto button = [&](P id, double cx, double cy, double size = 32) {
    add(WidgetKind::Button, cx, cy, size, size, static_cast<std::uint32_t>(id));
  };
  auto toggle = [&](P id, double cx, double cy, std::uint8_t top = 0, std::uint8_t mid = 1, std::uint8_t bottom = 2) {
    add(WidgetKind::Toggle, cx, cy, 24, 40, static_cast<std::uint32_t>(id));
    ws.back().leverIndex[0] = top;
    ws.back().leverIndex[1] = mid;
    ws.back().leverIndex[2] = bottom;
  };
  auto jack = [&](J id, double cx, double cy, double size = 40) {
    add(WidgetKind::Jack, cx, cy, size, size, static_cast<std::uint32_t>(id));
  };

  // ---- classic drones 1 / 2 / 4 / 5 ---------------------------------------------------------
  struct Classic { double x[5]; double volt; P tune[5], mute[5], mod[5], vol, att, rls, hold, cvAmt; J cv, gate, env; };
  const Classic classic[4] = {
      {{62, 124, 185, 247, 308}, 370,
       {P::drone_1_tune_1, P::drone_1_tune_2, P::drone_1_tune_3, P::drone_1_tune_4, P::drone_1_tune_5},
       {P::drone_1_mute_1, P::drone_1_mute_2, P::drone_1_mute_3, P::drone_1_mute_4, P::drone_1_mute_5},
       {P::drone_1_mod_1, P::drone_1_mod_2, P::drone_1_mod_3, P::drone_1_mod_4, P::drone_1_mod_5},
       P::drone_1_volt, P::drone_1_att, P::drone_1_rls, P::drone_1_gate_hold, P::drone_1_cv_amt,
       J::drone_1_cv_mod_in, J::drone_1_gate_in, J::drone_1_env_out},
      {{456, 518, 580, 641, 703}, 764,
       {P::drone_2_tune_1, P::drone_2_tune_2, P::drone_2_tune_3, P::drone_2_tune_4, P::drone_2_tune_5},
       {P::drone_2_mute_1, P::drone_2_mute_2, P::drone_2_mute_3, P::drone_2_mute_4, P::drone_2_mute_5},
       {P::drone_2_mod_1, P::drone_2_mod_2, P::drone_2_mod_3, P::drone_2_mod_4, P::drone_2_mod_5},
       P::drone_2_volt, P::drone_2_att, P::drone_2_rls, P::drone_2_gate_hold, P::drone_2_cv_amt,
       J::drone_2_cv_mod_in, J::drone_2_gate_in, J::drone_2_env_out},
      {{1638, 1700, 1761, 1823, 1885}, 1946,
       {P::drone_4_tune_1, P::drone_4_tune_2, P::drone_4_tune_3, P::drone_4_tune_4, P::drone_4_tune_5},
       {P::drone_4_mute_1, P::drone_4_mute_2, P::drone_4_mute_3, P::drone_4_mute_4, P::drone_4_mute_5},
       {P::drone_4_mod_1, P::drone_4_mod_2, P::drone_4_mod_3, P::drone_4_mod_4, P::drone_4_mod_5},
       P::drone_4_volt, P::drone_4_att, P::drone_4_rls, P::drone_4_gate_hold, P::drone_4_cv_amt,
       J::drone_4_cv_mod_in, J::drone_4_gate_in, J::drone_4_env_out},
      {{2033, 2094, 2156, 2217, 2279}, 2340,
       {P::drone_5_tune_1, P::drone_5_tune_2, P::drone_5_tune_3, P::drone_5_tune_4, P::drone_5_tune_5},
       {P::drone_5_mute_1, P::drone_5_mute_2, P::drone_5_mute_3, P::drone_5_mute_4, P::drone_5_mute_5},
       {P::drone_5_mod_1, P::drone_5_mod_2, P::drone_5_mod_3, P::drone_5_mod_4, P::drone_5_mod_5},
       P::drone_5_volt, P::drone_5_att, P::drone_5_rls, P::drone_5_gate_hold, P::drone_5_cv_amt,
       J::drone_5_cv_mod_in, J::drone_5_gate_in, J::drone_5_env_out},
  };
  for (std::uint32_t group = 0; group < 4; ++group) {
    const Classic& c = classic[group];
    for (int i = 0; i < 5; ++i) {
      button(c.mute[i], c.x[i], 295);
      plain(c.tune[i], c.x[i], 344);
      button(c.mod[i], c.x[i], 406);
    }
    knob(c.vol, c.volt, 381, Cap::Teal);
    add(WidgetKind::OscStatus, c.volt, 295, 69, 54, group);
    button(c.hold, c.x[0], 455);
    plain(c.att, c.x[1], 455);
    plain(c.rls, c.x[2], 455);
    jack(c.gate, c.x[0], 529);
    jack(c.env, c.x[2], 529);
    jack(c.cv, c.x[3], 529);
    add(WidgetKind::PhotoSensor, c.volt - 31, 498, 113, 113, group);  // the MOD light input
    plain(c.cvAmt, c.x[3], 455);                               // CV amount for the CV MOD jack
  }

  // ---- NEW drones 3 / 6 ---------------------------------------------------------------------------
  struct Papa { double dx; P rate, rateSw, fm, am, mod, divider, pitch, hilo, hold, att, rls, noise;
                J gate, env, noiseIn, clock, lfoOut, shOut, cvIn; };
  const Papa papa[2] = {
      {0, P::drone_3_rate, P::drone_3_rate_switch, P::drone_3_fm, P::drone_3_am, P::drone_3_mod,
       P::drone_3_divider, P::drone_3_pitch, P::drone_3_hi_low, P::drone_3_hold, P::drone_3_att,
       P::drone_3_rls, P::drone_3_noise, J::drone_3_gate_in, J::drone_3_env_out, J::drone_3_noise_in,
       J::drone_3_clock_in, J::drone_3_cv_out, J::drone_3_sh_out, J::drone_3_cv_in},
      {1971, P::drone_6_rate, P::drone_6_rate_switch, P::drone_6_fm, P::drone_6_am, P::drone_6_mod,
       P::drone_6_divider, P::drone_6_pitch, P::drone_6_hi_low, P::drone_6_hold, P::drone_6_att,
       P::drone_6_rls, P::drone_6_noise, J::drone_6_gate_in, J::drone_6_env_out, J::drone_6_noise_in,
       J::drone_6_clock_in, J::drone_6_cv_out, J::drone_6_sh_out, J::drone_6_cv_in},
  };
  for (const Papa& d : papa) {
    const double o = d.dx;
    knob(d.rate, 62 + o, 652, Cap::Teal);
    button(d.rateSw, 113 + o, 615, 28);
    button(d.fm, 216 + o, 615, 28);
    button(d.am, 318 + o, 615, 28);
    knob(d.mod, 165 + o, 652, Cap::Teal);
    knob(d.divider, 267 + o, 652, Cap::Teal);
    knob(d.pitch, 369 + o, 652, Cap::Teal);
    button(d.hilo, 318 + o, 689, 28);
    button(d.hold, 62 + o, 763);
    plain(d.att, 124 + o, 763);
    plain(d.rls, 185 + o, 763);
    knob(d.noise, 278 + o, 763, Cap::Teal);
    jack(d.gate, 62 + o, 837);
    jack(d.env, 185 + o, 837);
    jack(d.noiseIn, 247 + o, 837);
    jack(d.clock, 308 + o, 837);
    jack(d.shOut, 370 + o, 837);   // S&H: IN (noise_in), CLOCK, OUT
    jack(d.lfoOut, 114 + o, 707);  // the LFO square (manual: the modulator's CV OUT)
    jack(d.cvIn, 216 + o, 707);    // CV into the tone's pitch
  }

  // ---- VCO A / VCO B ------------------------------------------------------------------------------
  struct Vco { double dx; P cvAmt, oct, sub, tune, linExp, pwm, morph, pw; J voct, cv, pwmIn, last; };
  const Vco vcos[2] = {
      {0, P::vco_a_cv_amt, P::vco_a_oct_sel, P::vco_a_sub_sel, P::vco_a_tune, P::vco_a_lin_exp,
       P::vco_a_pwm, P::vco_a_morph, P::vco_a_pw, J::vco_a_v_oct_in, J::vco_a_cv_in, J::vco_a_pwm_in,
       J::vco_a_sync_in},
      {1182, P::vco_b_cv_amt, P::vco_b_oct_sel, P::vco_b_sub_sel, P::vco_b_tune, P::vco_b_lin_exp,
       P::vco_b_pwm, P::vco_b_morph, P::vco_b_pw, J::vco_b_v_oct_in, J::vco_b_cv_in, J::vco_b_pwm_in,
       J::vco_b_vco_out},
  };
  for (const Vco& v : vcos) {
    const double o = v.dx;
    knob(v.cvAmt, 487 + o, 652, Cap::Green);
    toggle(v.oct, 573 + o, 652, 2, 1, 0);      // +3 / 0 / low
    toggle(v.sub, 648 + o, 652, 1, 0);         // -1 / off
    knob(v.tune, 735 + o, 652, Cap::Green);
    toggle(v.linExp, 445 + o, 708);
    knob(v.pwm, 487 + o, 763, Cap::Green);
    knob(v.morph, 610 + o, 763, Cap::Green, 92);
    knob(v.pw, 735 + o, 763, Cap::Green);
    jack(v.voct, 456 + o, 837);
    jack(v.cv, 567 + o, 837);
    jack(v.pwmIn, 653 + o, 837);
    jack(v.last, 766 + o, 837);
  }

  // ---- envelopes A / B (with the VCO VCA input and the VCO output) ---------------------------------
  button(P::envelope_a_hold, 885, 744);
  button(P::envelope_a_self_gen, 963, 744);
  knob(P::envelope_a_a, 845, 790, Cap::Green, 54);
  knob(P::envelope_a_d, 924, 790, Cap::Green, 54);
  knob(P::envelope_a_s, 1003, 790, Cap::Green, 54);
  knob(P::envelope_a_r, 1082, 790, Cap::Green, 54);
  jack(J::envelope_a_gate_in, 885, 850);
  jack(J::envelope_a_env_out, 964, 850);
  jack(J::vco_a_vca_ctl, 1042, 850);
  jack(J::vco_a_dry_out, 1122, 850);
  button(P::envelope_b_hold, 1358, 744);
  button(P::envelope_b_self_gen, 1437, 744);
  knob(P::envelope_b_a, 1319, 790, Cap::Green, 54);
  knob(P::envelope_b_d, 1398, 790, Cap::Green, 54);
  knob(P::envelope_b_s, 1477, 790, Cap::Green, 54);
  knob(P::envelope_b_r, 1555, 790, Cap::Green, 54);
  jack(J::vco_b_dry_out, 1279, 850);
  jack(J::envelope_b_gate_in, 1358, 850);
  jack(J::envelope_b_env_out, 1436, 850);
  jack(J::vco_b_vca_ctl, 1515, 850);

  // ---- voice mixer ------------------------------------------------------------------------------------
  {
    const double mx[10] = {845, 924, 1003, 1082, 1161, 1239, 1318, 1397, 1476, 1555};
    const P pan[10] = {P::mixer_ch1_pan, P::mixer_ch2_pan, P::mixer_ch3_pan, P::mixer_ch4_pan,
                       P::mixer_ch5_pan, P::mixer_ch6_pan, P::mixer_ch7_pan, P::mixer_ch8_pan,
                       P::mixer_ch9_pan, P::mixer_ch10_pan};
    const P vol[10] = {P::mixer_ch1_vol, P::mixer_ch2_vol, P::mixer_ch3_vol, P::mixer_ch4_vol,
                       P::mixer_ch5_vol, P::mixer_ch6_vol, P::mixer_ch7_vol, P::mixer_ch8_vol,
                       P::mixer_ch9_vol, P::mixer_ch10_vol};
    for (int i = 0; i < 10; ++i) {
      knob(pan[i], mx[i], 578, Cap::Dark, 58);
      knob(vol[i], mx[i], 675, Cap::Grey);
    }
  }

  // ---- dual effector + dual filter ------------------------------------------------------------------
  jack(J::effector_cv_x_in, 846, 279);
  jack(J::effector_cv_y_in, 964, 279);
  jack(J::effector_cv_z_in, 1081, 279);
  knob(P::effector_x, 845, 356, Cap::Orange);
  knob(P::effector_y, 964, 356, Cap::Orange);
  knob(P::effector_z, 1082, 356, Cap::Orange);
  add(WidgetKind::Cartridge, 1200, 264, 116, 54, 0);   // the slot (click: next cartridge)
  add(WidgetKind::Cartridge, 1200, 323, 34, 34, 1);    // the button between the switches (same)
  toggle(P::effector_select_l, 1157, 323);
  toggle(P::effector_select_r, 1243, 323);
  knob(P::effector_blend, 1318, 356, Cap::Orange);
  knob(P::effector_master, 1436, 356, Cap::Orange, 96);
  // The headphone corner (no headphone output in software) holds the recorder: REC where
  // the socket is, its WET / DRY / ALL selector where the PHONE knob is. PHONE keeps its
  // stored value but has no control.
  add(WidgetKind::Record, 1555, 279, 40, 40, 0);
  add(WidgetKind::RecordSource, 1554, 356, 52, 52, 0, Cap::Orange);
  plain(P::vcf_l_mod, 1158, 382, 40);
  plain(P::vcf_r_mod, 1244, 382, 40);
  knob(P::vcf_l_freq, 885, 481, Cap::Orange);
  button(P::vcf_l_bp_lp, 944, 454);
  knob(P::vcf_l_res, 1003, 481, Cap::Orange);
  jack(J::vcf_cv_l_in, 1059, 481);
  knob(P::vcf_dist, 1117, 481, Cap::Orange);
  button(P::vcf_link, 1200, 481);
  knob(P::vcf_gain, 1283, 481, Cap::Orange);
  jack(J::vcf_cv_r_in, 1341, 481);
  knob(P::vcf_r_freq, 1397, 481, Cap::Orange);
  button(P::vcf_r_bp_lp, 1456, 454);
  knob(P::vcf_r_res, 1515, 481, Cap::Orange);

  // ---- bottom row -------------------------------------------------------------------------------------
  knob(P::lfo_a_wave, 66, 933, Cap::Red);
  jack(J::lfo_a_cv_out, 137, 933);
  toggle(P::lfo_a_speed_mult, 193, 945, 1, 0, 2);  // x6 / x1 / x10
  knob(P::lfo_a_rate, 252, 933, Cap::Red);
  knob(P::joystick_offset_x, 354, 933, Cap::Red);
  jack(J::joystick_x_out, 452, 908);
  jack(J::joystick_y_out, 452, 958);
  knob(P::joystick_offset_y, 548, 933, Cap::Red);
  knob(P::sequencer_pulser, 651, 933, Cap::Red);
  jack(J::sequencer_clock_out, 739, 908);
  jack(J::sequencer_ext_clock_in, 739, 958);
  toggle(P::sequencer_stages, 807, 933, 1, 2, 0);  // 4 / 5 / 3 steps
  {
    const double sx[5] = {877, 996, 1114, 1233, 1351};
    const double gx[5] = {937, 1055, 1173, 1291, 1409};
    const P cv[5] = {P::sequencer_step_cv_1, P::sequencer_step_cv_2, P::sequencer_step_cv_3,
                     P::sequencer_step_cv_4, P::sequencer_step_cv_5};
    const P gt[5] = {P::sequencer_step_gate_1, P::sequencer_step_gate_2, P::sequencer_step_gate_3,
                     P::sequencer_step_gate_4, P::sequencer_step_gate_5};
    for (int i = 0; i < 5; ++i) {
      knob(cv[i], sx[i], 933, Cap::Red);
      toggle(gt[i], gx[i], 945, 1, 0);  // gate on / off
    }
  }
  jack(J::sequencer_cv_out, 1487, 908);
  jack(J::sequencer_gate_out, 1487, 958);
  jack(J::preamp_ext_source_in, 1576, 933);
  knob(P::preamp_gain, 1647, 933, Cap::Red);
  knob(P::env_follower_attack, 1765, 933, Cap::Red);
  knob(P::env_follower_release, 1884, 933, Cap::Red);
  jack(J::env_follower_env_out, 1987, 933);
  jack(J::env_follower_gate_out, 2061, 933);
  knob(P::lfo_b_wave, 2149, 933, Cap::Red);
  jack(J::lfo_b_cv_out, 2221, 933);
  toggle(P::lfo_b_speed_mult, 2275, 945, 1, 0, 2);
  knob(P::lfo_b_rate, 2334, 933, Cap::Red);

  // ---- keyboard: stick, plates, jacks, encoder, display, DRONE VOICES ----------------------------
  add(WidgetKind::Joystick, 180, 1297, 250, 250, static_cast<std::uint32_t>(P::joystick_x), Cap::Black,
      static_cast<std::uint32_t>(P::joystick_y));
  {
    // Staggered plates, left to right (x0, y0, x1, y1).
    const double pl[12][4] = {{420, 1190, 509, 1430},  {536, 1152, 628, 1394},  {655, 1118, 746, 1358},
                              {774, 1152, 863, 1394},  {892, 1190, 982, 1430},  {1010, 1226, 1099, 1466},
                              {1302, 1226, 1391, 1466}, {1420, 1190, 1508, 1430}, {1537, 1152, 1627, 1394},
                              {1656, 1118, 1745, 1358}, {1774, 1152, 1864, 1394}, {1892, 1190, 1981, 1430}};
    for (std::uint32_t i = 0; i < 12; ++i)
      add(WidgetKind::Plate, (pl[i][0] + pl[i][2]) / 2, (pl[i][1] + pl[i][3]) / 2, pl[i][2] - pl[i][0],
          pl[i][3] - pl[i][1], i);
  }
  jack(J::keyboard_clock_in, 894, 1137, 34);
  jack(J::keyboard_reset_in, 955, 1151, 34);
  jack(J::keyboard_gate_left_main_out, 1016, 1167, 34);
  jack(J::keyboard_gate_right_out, 1384, 1167, 34);
  jack(J::keyboard_pressure_out, 1445, 1151, 34);
  jack(J::keyboard_v_oct_out, 1506, 1137, 34);
  add(WidgetKind::OctaveKey, 1093, 1186, 46, 46, 0);
  add(WidgetKind::Encoder, 1200, 1188, 90, 90, 0);
  add(WidgetKind::OctaveKey, 1307, 1186, 46, 46, 1);
  add(WidgetKind::Display, 1200, 1274, 116, 38, 0);
  {
    const double kx[2] = {2167, 2264};
    const double ky[3] = {1166, 1296, 1427};
    for (std::uint32_t i = 0; i < 6; ++i)  // keys 1,2,3 left column; 4,5,6 right column
      add(WidgetKind::DroneKey, kx[i / 3], ky[i % 3], 82, 80, i);
  }
  // MUTE and MIDI (neither is on the hardware): a vertical pair right of DRONE VOICES,
  // centered on the six-key block (its centre y is 1296.5). The gap keeps each
  // button's label clear of the other button.
  add(WidgetKind::MasterMute, 2352, 1244.5, 48, 48, 0);
  add(WidgetKind::MidiSettings, 2352, 1348.5, 48, 48, 0);

  // ---- keyboard menu overlay (opened by the encoder) --------------------------------------------------
  // The overlay draws and hit-tests these from kb_ui::kItems; they are listed here so every
  // keyboard setting still has exactly one control (and stays out of MIDI learn).
  for (const kb_ui::Item& it : kb_ui::kItems) {
    Widget wd;
    wd.kind = it.kind == kb_ui::Kind::Knob || it.kind == kb_ui::Kind::Trimmer ? WidgetKind::Knob : WidgetKind::Toggle;
    wd.cx = (it.box.l + it.box.r) / 2.0;
    wd.cy = (it.box.t + it.box.b) / 2.0;
    wd.w = it.box.r - it.box.l;
    wd.h = it.box.b - it.box.t;
    wd.id = static_cast<std::uint32_t>(it.id);
    wd.cap = Cap::Red;
    wd.menu = true;
    wd.label = it.label;
    ws.push_back(wd);
  }
  return ws;
}

// Which half of the 12 touch plates a plate (semitone from C, any octave) belongs to. Under
// TWIN / SPLIT the keyboard is two 6-plate controllers (manual p.15): C..F play the LEFT side,
// F#..B the RIGHT side (its pitch on the PRESSURE jack, its gate on GATE R). Under SINGLE the
// engine merges both halves into one performer.
inline bool plate_is_right_side(int semitone) { return ((semitone % 12) + 12) % 12 >= 6; }

}  // namespace lunar24::host
