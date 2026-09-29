// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_ui_layout.h — where every control sits on the 2400 x 1551 panel.
//
// Framework-free (no iPlug2): the IGraphics editor draws from this list, the offline
// preview tool renders it to SVG, and tests check it (every panel parameter and jack
// has exactly one control, nothing overlaps, everything stays inside its module).
//
// Module rectangles come from the measured panel regions (generated/.../panel_anchors);
// inside each module the controls are laid out in rows: knobs on top, switches in the
// middle, jacks at the bottom — the usual Eurorack-style reading order.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lunar24/registry.hpp>

namespace lunar24::host {

using core::JackId;
using core::ParameterId;

enum class WidgetKind : std::uint8_t {
  Module,     // a module card (title + frame); id unused
  Knob,       // continuous parameter
  Selector,   // switch with 2..N positions (click cycles)
  Jack,       // patch point (id = JackId)
  Plates,     // the 12 touch plates of the keyboard
  Joystick,   // XY pad driving joystick.x / joystick.y (id = x param, id2 = y param)
  Cartridge,  // effector cartridge slot (id = side 0/1)
  DroneKey,   // one of the six DRONE VOICE keys (id = 0..5)
  Title,      // the LUNAR 24 name plate
};

// Function colours (the five accent families + neutral).
enum class Accent : std::uint8_t { Neutral, Drone, Vco, Filter, Modulation, Keyboard };

struct Widget {
  WidgetKind kind = WidgetKind::Knob;
  double x = 0, y = 0, w = 0, h = 0;  // design-space rectangle (label included)
  std::uint32_t id = 0;               // ParameterId / JackId / index, per kind
  std::uint32_t id2 = 0;
  Accent accent = Accent::Neutral;
  std::string label;
  bool compact = false;                 // smaller knob (dense modules)
};

namespace layout_detail {

struct Rect { double x0, y0, x1, y1; double w() const { return x1 - x0; } double h() const { return y1 - y0; } };

// One item to place in a row.
struct Item {
  WidgetKind kind;
  std::uint32_t id;
  const char* label;
  bool compact = false;
};

inline Item knob(ParameterId id, const char* label, bool compact = false) {
  return {WidgetKind::Knob, static_cast<std::uint32_t>(id), label, compact};
}
inline Item sel(ParameterId id, const char* label) {
  return {WidgetKind::Selector, static_cast<std::uint32_t>(id), label, false};
}
inline Item jack(JackId id, const char* label) {
  return {WidgetKind::Jack, static_cast<std::uint32_t>(id), label, false};
}

// Cell sizes (design units). A knob cell is square plus a label line.
constexpr double kKnob = 58.0, kKnobSmall = 44.0, kSelW = 56.0, kSelH = 30.0, kJack = 34.0;
constexpr double kLabelH = 16.0;

inline void cellSize(const Item& it, double& w, double& h) {
  switch (it.kind) {
    case WidgetKind::Knob: w = it.compact ? kKnobSmall : kKnob; h = w + kLabelH; break;
    case WidgetKind::Selector: w = kSelW; h = kSelH + kLabelH; break;
    case WidgetKind::Jack: w = kJack + 18.0; h = kJack + kLabelH; break;
    default: w = h = 40.0; break;
  }
}

class Builder {
 public:
  explicit Builder(std::vector<Widget>& out) : out_(out) {}

  void module(const Rect& r, const char* title, Accent a) {
    Widget m;
    m.kind = WidgetKind::Module;
    m.x = r.x0; m.y = r.y0; m.w = r.w(); m.h = r.h();
    m.accent = a;
    m.label = title;
    out_.push_back(m);
    accent_ = a;
  }

  // Spread `items` evenly across [x0, x1], vertically centred on `cy`.
  void row(double cy, double x0, double x1, const std::vector<Item>& items) {
    if (items.empty()) return;
    const double step = (x1 - x0) / static_cast<double>(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
      double w = 0, h = 0;
      cellSize(items[i], w, h);
      const double cx = x0 + step * (static_cast<double>(i) + 0.5);
      Widget wd;
      wd.kind = items[i].kind;
      wd.id = items[i].id;
      wd.x = cx - w / 2;
      wd.y = cy - h / 2;
      wd.w = w;
      wd.h = h;
      wd.accent = accent_;
      wd.label = items[i].label;
      wd.compact = items[i].compact;
      out_.push_back(wd);
    }
  }

  void add(WidgetKind k, double x, double y, double w, double h, std::uint32_t id,
           std::uint32_t id2, const char* label) {
    Widget wd;
    wd.kind = k; wd.x = x; wd.y = y; wd.w = w; wd.h = h; wd.id = id; wd.id2 = id2;
    wd.accent = accent_;
    wd.label = label;
    out_.push_back(wd);
  }

 private:
  std::vector<Widget>& out_;
  Accent accent_ = Accent::Neutral;
};

constexpr double kTitleH = 30.0;  // module title strip

}  // namespace layout_detail

// Build the whole panel. Deterministic, called once when the editor opens.
inline std::vector<Widget> build_panel_layout() {
  using namespace layout_detail;
  using P = ParameterId;
  using J = JackId;
  std::vector<Widget> w;
  w.reserve(400);
  Builder b(w);

  // ---- name plate -------------------------------------------------------------------------
  b.add(WidgetKind::Title, 20, 20, 780, 150, 0, 0, "LUNAR 24");

  // ---- classic drones 1 / 2 / 4 / 5 -------------------------------------------------------
  struct ClassicIds {
    Rect r; const char* title;
    P tune[5], mute[5], mod[5], volt, att, rls, hold;
    J cv, gate, env;
  };
  const ClassicIds classic[4] = {
      {{20, 258, 408, 560}, "DRONE 1",
       {P::drone_1_tune_1, P::drone_1_tune_2, P::drone_1_tune_3, P::drone_1_tune_4, P::drone_1_tune_5},
       {P::drone_1_mute_1, P::drone_1_mute_2, P::drone_1_mute_3, P::drone_1_mute_4, P::drone_1_mute_5},
       {P::drone_1_mod_1, P::drone_1_mod_2, P::drone_1_mod_3, P::drone_1_mod_4, P::drone_1_mod_5},
       P::drone_1_volt, P::drone_1_att, P::drone_1_rls, P::drone_1_gate_hold,
       J::drone_1_cv_mod_in, J::drone_1_gate_in, J::drone_1_env_out},
      {{414, 258, 803, 560}, "DRONE 2",
       {P::drone_2_tune_1, P::drone_2_tune_2, P::drone_2_tune_3, P::drone_2_tune_4, P::drone_2_tune_5},
       {P::drone_2_mute_1, P::drone_2_mute_2, P::drone_2_mute_3, P::drone_2_mute_4, P::drone_2_mute_5},
       {P::drone_2_mod_1, P::drone_2_mod_2, P::drone_2_mod_3, P::drone_2_mod_4, P::drone_2_mod_5},
       P::drone_2_volt, P::drone_2_att, P::drone_2_rls, P::drone_2_gate_hold,
       J::drone_2_cv_mod_in, J::drone_2_gate_in, J::drone_2_env_out},
      {{1597, 258, 1985, 561}, "DRONE 4",
       {P::drone_4_tune_1, P::drone_4_tune_2, P::drone_4_tune_3, P::drone_4_tune_4, P::drone_4_tune_5},
       {P::drone_4_mute_1, P::drone_4_mute_2, P::drone_4_mute_3, P::drone_4_mute_4, P::drone_4_mute_5},
       {P::drone_4_mod_1, P::drone_4_mod_2, P::drone_4_mod_3, P::drone_4_mod_4, P::drone_4_mod_5},
       P::drone_4_volt, P::drone_4_att, P::drone_4_rls, P::drone_4_gate_hold,
       J::drone_4_cv_mod_in, J::drone_4_gate_in, J::drone_4_env_out},
      {{1991, 258, 2380, 561}, "DRONE 5",
       {P::drone_5_tune_1, P::drone_5_tune_2, P::drone_5_tune_3, P::drone_5_tune_4, P::drone_5_tune_5},
       {P::drone_5_mute_1, P::drone_5_mute_2, P::drone_5_mute_3, P::drone_5_mute_4, P::drone_5_mute_5},
       {P::drone_5_mod_1, P::drone_5_mod_2, P::drone_5_mod_3, P::drone_5_mod_4, P::drone_5_mod_5},
       P::drone_5_volt, P::drone_5_att, P::drone_5_rls, P::drone_5_gate_hold,
       J::drone_5_cv_mod_in, J::drone_5_gate_in, J::drone_5_env_out},
  };
  static const char* kTuneL[5] = {"TUNE 1", "TUNE 2", "TUNE 3", "TUNE 4", "TUNE 5"};
  static const char* kMuteL[5] = {"MUTE 1", "MUTE 2", "MUTE 3", "MUTE 4", "MUTE 5"};
  static const char* kModL[5] = {"MOD 1", "MOD 2", "MOD 3", "MOD 4", "MOD 5"};
  for (const auto& c : classic) {
    b.module(c.r, c.title, Accent::Drone);
    const double x0 = c.r.x0 + 6, x1 = c.r.x1 - 6, y = c.r.y0 + kTitleH;
    std::vector<Item> tunes, mutes, mods;
    for (int i = 0; i < 5; ++i) {
      tunes.push_back(knob(c.tune[i], kTuneL[i], true));
      mutes.push_back(sel(c.mute[i], kMuteL[i]));
      mods.push_back(sel(c.mod[i], kModL[i]));
    }
    tunes.push_back(knob(c.volt, "VOLT"));
    const double genX1 = x1 - (x1 - x0) / 6.0;  // the five generator columns; VOLT/HOLD after
    b.row(y + 40, x0, x1, tunes);
    b.row(y + 106, x0, genX1, mutes);
    b.row(y + 106, genX1, x1, {sel(c.hold, "HOLD")});
    b.row(y + 154, x0, genX1, mods);
    b.row(y + 226, x0, x1, {knob(c.att, "ATT"), knob(c.rls, "RLS"),
                            jack(c.cv, "CV MOD"), jack(c.gate, "GATE"), jack(c.env, "ENV")});
  }
  // ---- NEW drones 3 / 6 (Papa Srapa) --------------------------------------------------------
  struct NewIds {
    Rect r; const char* title;
    P rate, mod, divider, pitch, noise, att, rls, hilo, fm, am, rateSw, hold;
    J cv, env, gate, clock, noiseIn;
  };
  const NewIds news[2] = {
      {{20, 566, 408, 881}, "DRONE 3", P::drone_3_rate, P::drone_3_mod, P::drone_3_divider,
       P::drone_3_pitch, P::drone_3_noise, P::drone_3_att, P::drone_3_rls, P::drone_3_hi_low,
       P::drone_3_fm, P::drone_3_am, P::drone_3_rate_switch, P::drone_3_hold, J::drone_3_cv_out,
       J::drone_3_env_out, J::drone_3_gate_in, J::drone_3_clock_in, J::drone_3_noise_in},
      {{1991, 566, 2380, 880}, "DRONE 6", P::drone_6_rate, P::drone_6_mod, P::drone_6_divider,
       P::drone_6_pitch, P::drone_6_noise, P::drone_6_att, P::drone_6_rls, P::drone_6_hi_low,
       P::drone_6_fm, P::drone_6_am, P::drone_6_rate_switch, P::drone_6_hold, J::drone_6_cv_out,
       J::drone_6_env_out, J::drone_6_gate_in, J::drone_6_clock_in, J::drone_6_noise_in},
  };
  for (const auto& n : news) {
    b.module(n.r, n.title, Accent::Drone);
    const double x0 = n.r.x0 + 6, x1 = n.r.x1 - 6, y = n.r.y0 + kTitleH;
    b.row(y + 42, x0, x1, {knob(n.rate, "RATE"), knob(n.mod, "MOD"), knob(n.divider, "DIVIDER"),
                           knob(n.pitch, "PITCH"), knob(n.noise, "NOISE")});
    b.row(y + 116, x0, x1, {knob(n.att, "ATT", true), knob(n.rls, "RLS", true),
                            sel(n.hilo, "RANGE"), sel(n.rateSw, "RATE SW")});
    b.row(y + 176, x0, x1, {sel(n.fm, "FM"), sel(n.am, "AM"), sel(n.hold, "HOLD")});
    b.row(y + 244, x0, x1, {jack(n.gate, "GATE"), jack(n.clock, "CLOCK"), jack(n.noiseIn, "NOISE"),
                            jack(n.cv, "CV OUT"), jack(n.env, "ENV")});
  }

  // ---- VCO A / VCO B -------------------------------------------------------------------------
  {
    const Rect r{414, 566, 803, 880};
    b.module(r, "VCO A", Accent::Vco);
    const double x0 = r.x0 + 6, x1 = r.x1 - 6, y = r.y0 + kTitleH;
    b.row(y + 42, x0, x1, {knob(P::vco_a_tune, "TUNE"), knob(P::vco_a_morph, "MORPH"),
                           knob(P::vco_a_pw, "PW"), knob(P::vco_a_pwm, "PWM"),
                           knob(P::vco_a_cv_amt, "CV AMT")});
    b.row(y + 110, x0, x1, {sel(P::vco_a_oct_sel, "OCTAVE"), sel(P::vco_a_sub_sel, "SUB"),
                            sel(P::vco_a_lin_exp, "LIN/EXP")});
    b.row(y + 172, x0, x1, {jack(J::vco_a_v_oct_in, "V/OCT"), jack(J::vco_a_cv_in, "CV"),
                            jack(J::vco_a_pwm_in, "PWM"), jack(J::vco_a_sync_in, "SYNC")});
    b.row(y + 240, x0, x1, {jack(J::vco_a_vca_ctl, "VCA CV"), jack(J::vco_a_wave_out, "WAVE"),
                            jack(J::vco_a_dry_out, "DRY OUT")});
  }
  {
    const Rect r{1597, 566, 1985, 880};
    b.module(r, "VCO B", Accent::Vco);
    const double x0 = r.x0 + 6, x1 = r.x1 - 6, y = r.y0 + kTitleH;
    b.row(y + 42, x0, x1, {knob(P::vco_b_tune, "TUNE"), knob(P::vco_b_morph, "MORPH"),
                           knob(P::vco_b_pw, "PW"), knob(P::vco_b_pwm, "PWM"),
                           knob(P::vco_b_cv_amt, "CV AMT")});
    b.row(y + 110, x0, x1, {sel(P::vco_b_oct_sel, "OCTAVE"), sel(P::vco_b_sub_sel, "SUB"),
                            sel(P::vco_b_lin_exp, "LIN/EXP")});
    b.row(y + 172, x0, x1, {jack(J::vco_b_v_oct_in, "V/OCT"), jack(J::vco_b_cv_in, "CV"),
                            jack(J::vco_b_pwm_in, "PWM"), jack(J::vco_b_vca_ctl, "VCA CV")});
    b.row(y + 240, x0, x1, {jack(J::vco_b_vco_out, "VCO OUT"), jack(J::vco_b_wave_out, "WAVE"),
                            jack(J::vco_b_dry_out, "DRY OUT")});
  }

  // ---- dual effector ---------------------------------------------------------------------------
  {
    const Rect r{807, 181, 1592, 410};
    b.module(r, "DUAL EFFECTOR", Accent::Filter);
    const double y = r.y0 + kTitleH;
    b.add(WidgetKind::Cartridge, r.x0 + 14, y + 10, 190, 100, 0, 0, "LEFT");
    b.add(WidgetKind::Cartridge, r.x1 - 204, y + 10, 190, 100, 1, 0, "RIGHT");
    b.row(y + 52, r.x0 + 214, r.x1 - 214, {knob(P::effector_x, "X"), knob(P::effector_y, "Y"),
                                           knob(P::effector_z, "Z")});
    b.row(y + 150, r.x0 + 8, r.x0 + 214, {sel(P::effector_select_l, "PROGRAM L")});
    b.row(y + 150, r.x1 - 214, r.x1 - 8, {sel(P::effector_select_r, "PROGRAM R")});
    b.row(y + 150, r.x0 + 214, r.x1 - 214,
          {jack(J::effector_cv_x_in, "CV X"), jack(J::effector_cv_y_in, "CV Y"),
           jack(J::effector_cv_z_in, "CV Z"), knob(P::effector_blend, "BLEND", true),
           knob(P::effector_master, "MASTER", true), knob(P::effector_phone, "PHONE", true)});
  }

  // ---- dual VCF + distortion -----------------------------------------------------------------
  {
    const Rect r{807, 414, 1592, 531};
    b.module(r, "DUAL POLIVOKS FILTER", Accent::Filter);
    b.row(r.y0 + kTitleH + 42, r.x0 + 4, r.x1 - 4,
          {jack(J::vcf_cv_l_in, "CV L"), knob(P::vcf_l_freq, "FREQ L", true),
           knob(P::vcf_l_res, "RES L", true), knob(P::vcf_l_mod, "MOD L", true),
           sel(P::vcf_l_bp_lp, "BP/LP L"), knob(P::vcf_dist, "DIST", true),
           knob(P::vcf_gain, "GAIN", true), sel(P::vcf_link, "LINK"),
           sel(P::vcf_r_bp_lp, "BP/LP R"), knob(P::vcf_r_mod, "MOD R", true),
           knob(P::vcf_r_res, "RES R", true), knob(P::vcf_r_freq, "FREQ R", true),
           jack(J::vcf_cv_r_in, "CV R")});
  }

  // ---- voice mixer ------------------------------------------------------------------------------
  {
    const Rect r{807, 535, 1592, 707};
    b.module(r, "VOICE MIXER", Accent::Neutral);
    const P vol[10] = {P::mixer_ch1_vol, P::mixer_ch2_vol, P::mixer_ch3_vol, P::mixer_ch4_vol,
                       P::mixer_ch5_vol, P::mixer_ch6_vol, P::mixer_ch7_vol, P::mixer_ch8_vol,
                       P::mixer_ch9_vol, P::mixer_ch10_vol};
    const P pan[10] = {P::mixer_ch1_pan, P::mixer_ch2_pan, P::mixer_ch3_pan, P::mixer_ch4_pan,
                       P::mixer_ch5_pan, P::mixer_ch6_pan, P::mixer_ch7_pan, P::mixer_ch8_pan,
                       P::mixer_ch9_pan, P::mixer_ch10_pan};
    static const char* kCh[10] = {"DRONE 1", "DRONE 2", "DRONE 3", "EXT", "VCO A",
                                  "VCO B",   "PREAMP",  "DRONE 4", "DRONE 5", "DRONE 6"};
    std::vector<Item> vols, pans;
    for (int i = 0; i < 10; ++i) {
      vols.push_back(knob(vol[i], kCh[i]));
      pans.push_back(knob(pan[i], "PAN", true));
    }
    b.row(r.y0 + kTitleH + 36, r.x0 + 4, r.x1 - 4, vols);
    b.row(r.y0 + kTitleH + 106, r.x0 + 4, r.x1 - 4, pans);
  }

  // ---- envelopes A / B ----------------------------------------------------------------------------
  struct EnvIds { Rect r; const char* title; P a, d, s, rl, hold, self; J gate, env, vca; };
  const EnvIds envs[2] = {
      {{807, 711, 1188, 880}, "ENVELOPE A", P::envelope_a_a, P::envelope_a_d, P::envelope_a_s,
       P::envelope_a_r, P::envelope_a_hold, P::envelope_a_self_gen, J::envelope_a_gate_in,
       J::envelope_a_env_out, J::envelope_a_vca_cv_out},
      {{1211, 711, 1592, 880}, "ENVELOPE B", P::envelope_b_a, P::envelope_b_d, P::envelope_b_s,
       P::envelope_b_r, P::envelope_b_hold, P::envelope_b_self_gen, J::envelope_b_gate_in,
       J::envelope_b_env_out, J::envelope_b_vca_cv_out},
  };
  for (const auto& e : envs) {
    b.module(e.r, e.title, Accent::Modulation);
    const double x0 = e.r.x0 + 4, x1 = e.r.x1 - 4, y = e.r.y0 + kTitleH;
    b.row(y + 38, x0, x1, {knob(e.a, "ATTACK", true), knob(e.d, "DECAY", true),
                           knob(e.s, "SUSTAIN", true), knob(e.rl, "RELEASE", true)});
    b.row(y + 106, x0, x1, {sel(e.hold, "HOLD"), sel(e.self, "SELF-GEN"), jack(e.gate, "GATE"),
                            jack(e.env, "ENV"), jack(e.vca, "VCA CV")});
  }

  // ---- bottom row: LFO A, joystick, 5-step, preamp, env follower, LFO B ----------------------------
  const double by = 884, bh = 146, bcy = by + kTitleH + (bh - kTitleH) / 2;
  {
    const Rect r{19, by, 292, by + bh};
    b.module(r, "LFO A", Accent::Modulation);
    b.row(bcy, r.x0 + 4, r.x1 - 4, {knob(P::lfo_a_wave, "WAVE", true), knob(P::lfo_a_rate, "RATE", true),
                                    sel(P::lfo_a_speed_mult, "x1/6/10"), jack(J::lfo_a_cv_out, "CV")});
  }
  {
    const Rect r{296, by, 596, by + bh};
    b.module(r, "JOYSTICK", Accent::Modulation);
    b.add(WidgetKind::Joystick, r.x0 + 10, by + kTitleH + 4, 104, 104,
          static_cast<std::uint32_t>(P::joystick_x), static_cast<std::uint32_t>(P::joystick_y), "");
    b.row(by + kTitleH + 31, r.x0 + 118, r.x1 - 4,
          {knob(P::joystick_offset_x, "OFFSET X", true), knob(P::joystick_offset_y, "OFFSET Y", true)});
    b.row(by + bh - 28, r.x0 + 118, r.x1 - 4,
          {jack(J::joystick_x_out, "X OUT"), jack(J::joystick_y_out, "Y OUT")});
  }
  {
    const Rect r{600, by, 1545, by + bh};
    b.module(r, "5-STEP SEQUENCER", Accent::Modulation);
    const double x0 = r.x0 + 4;
    b.row(bcy, x0, x0 + 190, {knob(P::sequencer_pulser, "PULSER", true), sel(P::sequencer_clock, "CLOCK"),
                              sel(P::sequencer_stages, "STAGES")});
    const P cv[5] = {P::sequencer_step_cv_1, P::sequencer_step_cv_2, P::sequencer_step_cv_3,
                     P::sequencer_step_cv_4, P::sequencer_step_cv_5};
    const P gt[5] = {P::sequencer_step_gate_1, P::sequencer_step_gate_2, P::sequencer_step_gate_3,
                     P::sequencer_step_gate_4, P::sequencer_step_gate_5};
    static const char* kStep[5] = {"STEP 1", "STEP 2", "STEP 3", "STEP 4", "STEP 5"};
    static const char* kGate[5] = {"GATE 1", "GATE 2", "GATE 3", "GATE 4", "GATE 5"};
    std::vector<Item> steps;
    for (int i = 0; i < 5; ++i) {
      steps.push_back(knob(cv[i], kStep[i], true));
      steps.push_back(sel(gt[i], kGate[i]));
    }
    b.row(bcy, x0 + 190, r.x1 - 230, steps);
    b.row(bcy, r.x1 - 230, r.x1 - 4, {jack(J::sequencer_ext_clock_in, "CLK IN"),
                                      jack(J::sequencer_clock_out, "CLK OUT"),
                                      jack(J::sequencer_cv_out, "CV"), jack(J::sequencer_gate_out, "GATE")});
  }
  {
    const Rect r{1548, by, 1723, by + bh};
    b.module(r, "PREAMP", Accent::Neutral);
    b.row(bcy, r.x0 + 4, r.x1 - 4, {knob(P::preamp_gain, "GAIN", true), jack(J::preamp_ext_source_in, "SOURCE")});
  }
  {
    const Rect r{1727, by, 2099, by + bh};
    b.module(r, "ENVELOPE FOLLOWER", Accent::Modulation);
    b.row(bcy, r.x0 + 4, r.x1 - 4, {knob(P::env_follower_attack, "ATTACK", true),
                                    knob(P::env_follower_release, "RELEASE", true),
                                    jack(J::env_follower_env_out, "ENV"), jack(J::env_follower_gate_out, "GATE")});
  }
  {
    const Rect r{2103, by, 2379, by + bh};
    b.module(r, "LFO B", Accent::Modulation);
    b.row(bcy, r.x0 + 4, r.x1 - 4, {knob(P::lfo_b_wave, "WAVE", true), knob(P::lfo_b_rate, "RATE", true),
                                    sel(P::lfo_b_speed_mult, "x1/6/10"), jack(J::lfo_b_cv_out, "CV")});
  }

  // ---- keyboard: menu parameters (left), plates (centre), jacks (right) ------------------------------
  {
    const Rect r{19, 1040, 395, 1540};
    b.module(r, "KEYBOARD", Accent::Keyboard);
    const std::vector<Item> kb = {
        sel(P::keyboard_behaviour, "PLAY"), sel(P::keyboard_mode, "MODE"),
        sel(P::keyboard_quantise_load_scale, "SCALE"), knob(P::keyboard_root_note, "ROOT", true),
        knob(P::keyboard_clock_bpm, "BPM", true),
        knob(P::keyboard_portamento_speed, "GLIDE", true), sel(P::keyboard_portamento_legato, "LEGATO"),
        knob(P::keyboard_vibrato_speed, "VIB RATE", true), knob(P::keyboard_vibrato_depth, "VIB DEPTH", true),
        knob(P::keyboard_vibrato_delay, "VIB DELAY", true),
        knob(P::keyboard_vibrato_pressure, "VIB PRESS", true), sel(P::keyboard_pressure_output, "PRESSURE"),
        knob(P::keyboard_pressure_rise, "P RISE", true), knob(P::keyboard_pressure_fall, "P FALL", true),
        sel(P::keyboard_arp_hold, "ARP HOLD"),
        sel(P::keyboard_arp_direction, "ARP DIR"), sel(P::keyboard_arp_variation, "ARP VAR"),
        knob(P::keyboard_arp_interval, "ARP INT", true), knob(P::keyboard_arp_length, "ARP LEN", true),
        sel(P::keyboard_seq_run, "SEQ RUN"),
        knob(P::keyboard_seq_length, "SEQ LEN", true), sel(P::keyboard_seq_direction, "SEQ DIR"),
        sel(P::keyboard_seq_cv_output, "SEQ CV"), knob(P::keyboard_seq_rhythm_length, "RHYTHM", true),
        sel(P::keyboard_encoder_direction, "ENCODER"),
        knob(P::keyboard_calibration_v_oct, "CAL V/OCT", true),
        knob(P::keyboard_calibration_pressure, "CAL PRESS", true), sel(P::keyboard_dac_vref, "DAC REF"),
        knob(P::keyboard_touch_threshold, "TOUCH", true), knob(P::keyboard_release_threshold, "RELEASE", true),
        knob(P::keyboard_pressure_min, "P MIN", true), knob(P::keyboard_pressure_max, "P MAX", true),
        knob(P::keyboard_mpr121_charge, "CHARGE", true), knob(P::keyboard_mpr121_discharge, "DISCHARGE", true),
        knob(P::keyboard_debounce, "DEBOUNCE", true),
    };
    const int cols = 5;
    const double rowH = (r.h() - kTitleH - 6) / 7.0;
    for (int row = 0; row < 7; ++row) {
      std::vector<Item> items(kb.begin() + row * cols, kb.begin() + row * cols + cols);
      b.row(r.y0 + kTitleH + rowH * (row + 0.5), r.x0 + 2, r.x1 - 2, items);
    }
  }
  {
    const Rect r{399, 1104, 1999, 1491};
    b.module(r, "TOUCH PLATES", Accent::Keyboard);
    b.add(WidgetKind::Plates, r.x0 + 8, r.y0 + kTitleH + 4, r.w() - 16, r.h() - kTitleH - 12, 0, 0, "");
  }
  {
    const Rect r{2003, 1104, 2122, 1491};
    b.module(r, "KBD I/O", Accent::Keyboard);
    const double cx = (r.x0 + r.x1) / 2, top = r.y0 + kTitleH, step = (r.h() - kTitleH) / 6.0;
    const Item io[6] = {jack(J::keyboard_v_oct_out, "V/OCT"), jack(J::keyboard_gate_left_main_out, "GATE L"),
                        jack(J::keyboard_gate_right_out, "GATE R"), jack(J::keyboard_pressure_out, "PRESSURE"),
                        jack(J::keyboard_clock_in, "CLOCK"), jack(J::keyboard_reset_in, "RESET")};
    for (int i = 0; i < 6; ++i) b.row(top + step * (i + 0.5), cx - 40, cx + 40, {io[i]});
  }
  {
    const Rect r{2126, 1104, 2379, 1491};
    b.module(r, "DRONE VOICES", Accent::Drone);
    const double top = r.y0 + kTitleH + 8, kh = (r.h() - kTitleH - 16) / 6.0;
    static const char* kKey[6] = {"1", "2", "3", "4", "5", "6"};
    for (int i = 0; i < 6; ++i)
      b.add(WidgetKind::DroneKey, r.x0 + 20, top + kh * i + 4, r.w() - 40, kh - 8,
            static_cast<std::uint32_t>(i), 0, kKey[i]);
  }
  return w;
}

}  // namespace lunar24::host
