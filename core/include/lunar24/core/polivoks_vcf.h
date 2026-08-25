// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// PolivoksFilter — P3-⑤: the DUAL LP/BP VCF, a double 12 dB (2-pole) POLIVOKS
// filter (manual L1118-1153, registry module id2 "Dual 12dB Polivoks VCF").
// Signal chain: → MIX/PAN → VCF → DISTORTION → WET OUT L/R (design/01 §1).
//
// PROVENANCE (the frozen registry, generated/lunar24/registry.hpp, is the
// implementation basis; every claim below mirrors it and marks evidence strength):
//
//   * The FILTER is the Polivoks, NOT a generic LP/BP. The manual (L1120-1122) is
//     the DEFINITIONAL behaviour: "Double 12 dB filter... Still, you will not lose
//     low frequencies when increasing resonance." A filter that loses lows at high
//     resonance is NOT a Polivoks. This header implements a Chamberlin state-variable
//     filter (low + band states), whose lowpass has unity DC gain INDEPENDENT of the
//     resonance feedback — so raising resonance adds a peak near the cutoff but does
//     not drop the low band. That is the property must-test #1 verifies.
//   * FREQ (vcf.l_freq id13 / vcf.r_freq id18) — "manual filter cutoff frequency"
//     (L1138). unit "norm" 0..1, default 0.3, CONFIRMED. The manual gives NO cutoff
//     range; the norm→Hz mapping (20..20 kHz log, capped at sr/8 for SVF stability)
//     is PROVISIONAL.
//   * RESONANCE (vcf.l_res id14 / vcf.r_res id34) — "Filter resonance (boost the
//     frequency near cutoff point)" (L1140). unit "norm" 0..1, default 0.0, CONFIRMED
//     (semantics — boost near cutoff). The damping/curve and the non-self-oscillation
//     floor are PROVISIONAL (no manual number).
//   * MOD L/MOD R (vcf.l_mod id15 / vcf.r_mod id35) — "MOD L and MOR R – CV amount
//     control" (L1145). unit "norm" 0..1, default 0.0, CONFIRMED. CV shifting the
//     cutoff by 2^(mod*cv/oct) is a PROVISIONAL model (no CV range / V-oct figure).
//   * BP-LP (vcf.l_bp_lp id19 / vcf.r_bp_lp id36) — "BP-LP – filter mode" (L1149).
//     unit "selector", range 0..1, step 1.0, positions["bp","lp"], CONFIRMED.
//     TWO-STATE (a selector, NOT a continuous morph — @Claude 187d8313 (b)). The
//     generated registry literal is `default: 0.0`, which selects positions[0]="bp"
//     (filter defaults to BP). NOTE the adjudicator's GO cited "default 1": the
//     frozen-registry literal is authoritative and is followed here; the
//     discrepancy is deliberately kept visible (not silently resolved).
//   * LINK (vcf.link id20) — "when switched on links CV on filter 1 to control both
//     filters 1 and 2" (L1147). unit "selector" positions["off","on"], default off,
//     CONFIRMED. An ACTIVE internal switch (distinct from the passive route below).
//   * CV L / CV R (vcf.cv_l_in id10 / vcf.cv_r_in id11) — "you can control/automate
//     filter cutoff via CV using CV in. CV L is normally connected to CV R... if
//     there is no CV-signal in the CV R" (L1142-1143). The NORMALLING (CV L -> CV R
//     when CV R is unplugged) is the P2-② NormalizedRoute
//     route.vcf_cv_l_to_cv_r (registry, confirmed). It is a GRAPH fact, re-presented
//     here and NOT re-coded (the graph resolves which voltage reaches cv_r_in; this
//     header only consumes the resolved voltages). The INTERACTION
//     "LINK on + CV R plugged" has NO manual ruling; per @Claude (a) the provisional
//     default is "LINK overrides plugging", marked UN-EVIDENCED and added to the
//     P3-exit 待取证 list.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// Dual 2-pole Polivoks filter (one per stereo channel), with per-channel FREQ/RES/
// MOD/BP-LP and a shared LINK switch. L and R state are fully independent.
class PolivoksFilter {
 public:
  static constexpr int kChannelLeft = 0;
  static constexpr int kChannelRight = 1;

  PolivoksFilter() {
    for (int i = 0; i < 2; ++i) {
      channel_[i] = Channel();
      channel_[i].freq = 0.3;  // registry default.
      channel_[i].res = 0.0;   // registry default.
      channel_[i].mod = 0.0;   // registry default.
      channel_[i].mode = 0.0;  // registry default 0.0 -> positions[0]="bp".
    }
  }

  void setSampleRate(double sr) {
    if (sr > 0.0) sr_ = sr;
  }

  // ---------------------------------------------------------------- configure --
  // Everything is per-channel (kChannelLeft / kChannelRight).
  void setFreq(int ch, double freq) { if (idx_(ch)) channel_[ch].freq = clamp01_(freq); }
  void setRes(int ch, double res)   { if (idx_(ch)) channel_[ch].res   = clamp01_(res); }
  void setMod(int ch, double mod)   { if (idx_(ch)) channel_[ch].mod   = clamp01_(mod); }
  // Two-state mode: true = bandpass, false = lowpass. Registry positions ["bp","lp"].
  void setMode(int ch, bool bp)     { if (idx_(ch)) channel_[ch].mode = bp ? 0.0 : 1.0; }

  // LINK (active switch): when on, the L-side CV controls BOTH filters.
  void setLink(bool on) { link_ = on; }
  bool link() const { return link_; }

  // Resolved CV voltages at the two jacks (the graph has already applied the
  // route.vcf_cv_l_to_cv_r normalling for the unplugged case).
  void setCvL(double volts) { cvL_ = volts; }
  void setCvR(double volts) { cvR_ = volts; }
  double cvL() const { return cvL_; }
  double cvR() const { return cvR_; }

  // ----------------------------------------------------------------- render --
  // Process one stereo frame: inL through FILTER L, inR through FILTER R.
  void process(double inL, double inR, double& outL, double& outR) {
    outL = tick_(channel_[0], inL);
    outR = tick_(channel_[1], inR);
  }

  // CONFIRMED ranges / nominal (registry).
  static constexpr double kModeBp = 0.0;  // positions[0] = "bp".
  static constexpr double kModeLp = 1.0;  // positions[1] = "lp".
  // PROVISIONAL filter parameters (no manual number exists for any of these).
  static constexpr double kFreqMinHz = 20.0;       // cutoff norm 0 (log map).
  static constexpr double kFreqMaxHz = 20000.0;    // cutoff norm 1 (log map).
  static constexpr double kCvVoltsPerOctave = 1.0; // CV -> octave depth (provisional).
  static constexpr double kDampMax = 2.0;          // res=0 (flat response).
  static constexpr double kDampMin = 0.1;          // res=1 (max resonance, kept >0).
  // PROVISIONAL stability cap: the Chamberlin SVF is capped at sr/8 (kept well away
  // from Nyquist so the resonance floor stays stable). Not a manual spec.
  static constexpr double kCutoffCapRatio = 1.0 / 8.0;

 private:
  struct Channel {
    double freq = 0.3, res = 0.0, mod = 0.0;
    double mode = 0.0;   // 0.0=bp, 1.0=lp (positions[0/1]).
    double low = 0.0, band = 0.0;
    double sr = 0.0;
  };

  static bool idx_(int ch) { return ch == 0 || ch == 1; }
  static double clamp01_(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

  double baseFreqHz_(double freqNorm, double sr) const {
    double fc = kFreqMinHz * std::pow(kFreqMaxHz / kFreqMinHz, freqNorm);
    const double cap = sr * kCutoffCapRatio;
    if (fc > cap) fc = cap;
    if (fc < kFreqMinHz) fc = kFreqMinHz;
    return fc;
  }

  // Effective cutoff for a channel, given the resolved CV for THAT channel and the
  // channel's own MOD depth. CV shifts cutoff multiplicatively (2^cv/oct, provisional).
  double effCutoffHz_(const Channel& c, double cvEff) const {
    const double baseFc = baseFreqHz_(c.freq, sr_);
    const double shift = c.mod * cvEff / kCvVoltsPerOctave;
    double fc = baseFc * std::pow(2.0, shift);
    const double cap = sr_ * kCutoffCapRatio;
    if (fc > cap) fc = cap;
    if (fc < kFreqMinHz) fc = kFreqMinHz;
    return fc;
  }

  double tick_(Channel& c, double x) {
    if (c.sr != sr_) { c.sr = sr_; }
    const double fc = effCutoffHz_(c, cvEffFor_(c));
    const double f = 2.0 * std::sin(3.14159265358979323846 * fc / sr_);
    // Chamberlin state-variable filter: lowpass has unity DC gain independent of
    // the resonance feedback `damp`, so raising resonance does not drop the low end.
    const double damp = kDampMax + (kDampMin - kDampMax) * c.res;
    c.low += f * c.band;
    const double high = x - c.low - damp * c.band;
    c.band += f * high;
    // Two-state output selection (bp | lp), not a continuous morph.
    return (c.mode < 0.5) ? c.band : c.low;
  }

  // Resolve the effective CV for a channel. LINK (active) overrides the plugged CV R
  // (provisional, un-evidenced). The route normalling happened at the graph layer.
  double cvEffFor_(const Channel& c) const {
    if (&c == &channel_[0]) return cvL_;
    return link_ ? cvL_ : cvR_;
  }

  double sr_ = 0.0;
  bool link_ = false;             // registry selector default "off".
  double cvL_ = 0.0, cvR_ = 0.0;
  Channel channel_[2];
};

}  // namespace lunar24::core
