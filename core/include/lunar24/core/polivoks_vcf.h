// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// PolivoksFilter —: the DUAL LP/BP VCF, a double 12 dB (2-pole) POLIVOKS
// filter (manual L1118-1153, registry module id2 "Dual 12dB Polivoks VCF").
// Signal chain: → MIX/PAN → VCF → DISTORTION → WET OUT L/R.
//
// PROVENANCE (the frozen registry, generated/lunar24/registry.hpp, is the
// implementation basis; every claim below mirrors it and marks evidence strength):
//
//   * The FILTER is the Polivoks, NOT a generic LP/BP. The manual (L1120-1122) is
//     the DEFINITIONAL behaviour: "Double 12 dB filter... Still, you will not lose
//     low frequencies when increasing resonance." A filter that loses lows at high
//     resonance is NOT a Polivoks. This header implements a two-integrator trapezoidal
//     (TPT/ZDF) state-variable filter (one pair of integrator states per channel), the
//     bilinear-transformed analog SVF of Andrew Simper's SvfLinearTrapOptimised2 — whose
//     lowpass has unity DC gain INDEPENDENT of the resonance damping `damp`, so raising
//     resonance adds a peak near the cutoff but does not drop the low band. That is the
//     property must-test #1 verifies. (the Chamberlin recursion was
//     replaced, since its sr/8 stability cap produced the knob dead-zone.)
//   * FREQ (vcf.l_freq id13 / vcf.r_freq id18) — "manual filter cutoff frequency"
//     (L1138). unit "norm" 0..1, default 0.3, CONFIRMED. The manual gives NO cutoff
//     range; the norm→Hz mapping (20..20 kHz log, effective cap min(20000, 0.49·sr))
//     is PROVISIONAL.
//   * RESONANCE (vcf.l_res id14 / vcf.r_res id34) — "Filter resonance (boost the
//     frequency near cutoff point)" (L1140). unit "norm" 0..1, default 0.0, CONFIRMED
//     (semantics — boost near cutoff). The damping curve, the self-oscillation over
//     the last few percent of the knob and the integrator saturation are tuned by ear
//     (no manual number).
//   * MOD L/MOD R (vcf.l_mod id15 / vcf.r_mod id35) — "MOD L and MOR R – CV amount
//     control" (L1145). unit "norm" 0..1, default 0.0, CONFIRMED. CV shifting the
//     cutoff by 2^(mod*cv/oct) is a PROVISIONAL model (no CV range / V-oct figure).
//   * BP-LP (vcf.l_bp_lp id19 / vcf.r_bp_lp id36) — "BP-LP – filter mode" (L1149).
//     unit "selector", range 0..1, step 1.0, positions["bp","lp"], CONFIRMED.
//     TWO-STATE (a selector, not a continuous morph). The registry default is 1 =
//     positions[1]="lp", which the machine applies when it is built, so the panel
//     starts in LP. This class on its own starts at 0 (BP) until a mode is set.
//   * LINK (vcf.link id20) — "when switched on links CV on filter 1 to control both
//     filters 1 and 2" (L1147). unit "selector" positions["off","on"], default off,
//     CONFIRMED. An ACTIVE internal switch (distinct from the passive route below).
//   * CV L / CV R (vcf.cv_l_in id10 / vcf.cv_r_in id11) — "you can control/automate
//     filter cutoff via CV using CV in. CV L is normally connected to CV R... if
//     there is no CV-signal in the CV R" (L1142-1143). The NORMALLING (CV L -> CV R
//     when CV R is unplugged) is the NormalizedRoute
//     route.vcf_cv_l_to_cv_r (registry, confirmed). It is a GRAPH fact, re-presented
//     here and NOT re-coded (the graph resolves which voltage reaches cv_r_in; this
//     header only consumes the resolved voltages). The INTERACTION
//     "LINK on + CV R plugged" has NO manual ruling; (a) the provisional
//     default is "LINK overrides plugging", marked UN-EVIDENCED and added to the
//     P3-exit 待取证 list.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

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
      channel_[i].mode = 0.0;  // BP until set; the machine applies the registry default (LP).
    }
    channel_[1].noise ^= 0xD1B54A32D192ED03ULL;  // L and R hiss differently
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

  // per-channel VCF input-stage saturation drive (the input-level driven
  // nonlinearity of). 0 = exact passthrough; >0 folds a large input
  // toward a LOWER normalised gain while the small-signal slope stays 1, so the
  // channel is linear near zero and level-dependent at high input. L and R drives
  // are fully independent (a profile supplies the per-side value).
  void setInputDrive(int ch, double drive) {
    if (idx_(ch)) channel_[ch].inputDrive = drive < 0.0 ? 0.0 : drive;
  }
  double inputDrive(int ch) const { return idx_(ch) ? channel_[ch].inputDrive : 0.0; }

  // Panel-control READBACK: the applied per-channel knob positions, so a
  // product oracle can verify a state restore truly reached THIS filter (post-clamp
  // where the setter clamps). modeIsBp: true = bandpass, false = lowpass (position).
  double freq(int ch) const { return idx_(ch) ? channel_[ch].freq : 0.0; }
  double res(int ch) const { return idx_(ch) ? channel_[ch].res : 0.0; }
  double mod(int ch) const { return idx_(ch) ? channel_[ch].mod : 0.0; }
  bool modeIsBp(int ch) const { return idx_(ch) ? (channel_[ch].mode == kModeBp) : false; }

  // Real-filter state readback: the two integrator states, so a product
  // oracle can verify reset truly reached THIS filter. Independent per channel.
  double ic1eq(int ch) const { return idx_(ch) ? channel_[ch].ic1eq : 0.0; }
  double ic2eq(int ch) const { return idx_(ch) ? channel_[ch].ic2eq : 0.0; }
  // Effective cutoff (Hz) actually applied: post-CV shift, post-cap. Same-frame.
  double effectiveCutoffHz(int ch) const {
    return idx_(ch) ? effCutoffHz_(channel_[ch], cvEffFor_(channel_[ch])) : 0.0;
  }

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

  // ------------------------------------------------------------------ reset --
  // Reset the two integrator states (per channel, independent) to zero. The states
  // are consumed same-frame and there is no added feedback delay; reset clears any
  // retained transient. reset / real-state read seam.
  void reset() {
    for (int i = 0; i < 2; ++i) {
      channel_[i].ic1eq = 0.0;
      channel_[i].ic2eq = 0.0;
      channel_[i].u1 = 0.0;
      channel_[i].u2 = 0.0;
    }
  }

  // CONFIRMED ranges / nominal (registry).
  static constexpr double kModeBp = 0.0;  // positions[0] = "bp".
  static constexpr double kModeLp = 1.0;  // positions[1] = "lp".
  // PROVISIONAL filter parameters (no manual number exists for any of these).
  static constexpr double kFreqMinHz = 20.0;       // cutoff norm 0 (log map).
  static constexpr double kFreqMaxHz = 20000.0;    // cutoff norm 1 (log map).
  static constexpr double kCvVoltsPerOctave = 1.0; // CV -> octave depth (provisional).
  // Resonance: damping k = kDampMax * (1 - res)^kResCurve - kOscPush * res^12. The curve
  // spreads the audible range over the whole knob; the small push makes the filter sing on
  // its own (self-oscillate) over the last few percent, as a Polivoks does. // tuned by ear
  static constexpr double kDampMax = 2.0;          // res=0 (flat response).
  static constexpr double kResCurve = 1.6;
  static constexpr double kOscPush = 0.06;
  // The integrators are op-amps that run out of current: each one's input goes through a
  // soft limit at about this many volts. Quiet signals pass clean; loud ones and high
  // resonance growl, the peak stays bounded and the pitch sags a little. // tuned by ear
  static constexpr double kIntegratorVolts = 2.5;
  // The resonance itself (the feedback that undoes the damping) also runs out: beyond
  // about kResVolts at the band output the damping climbs back toward kLoudDamp. This sets
  // the self-oscillation level and keeps a loud signal from screaming. // tuned by ear
  static constexpr double kResVolts = 0.45;
  static constexpr double kLoudDamp = 0.7;
  static double dampFor(double res) {
    return kDampMax * std::pow(1.0 - res, kResCurve) - kOscPush * std::pow(res, 12.0);
  }
  // PROVISIONAL software safety cap: the effective cutoff is capped
  // at min(20000 Hz, 0.49·sr) so the prewarped `g = tan(pi·fc/sr)` stays away from its
  // pi/2 singularity (fc/sr < 0.49 < 0.5). This is a software safety POLICY, NOT a
  // stability theorem; 0.49 is not a TPT/ZDF boundary (0.45351 is not one either).
  static constexpr double kCutoffCapRatioSoft = 0.49;
  // PROVISIONAL input-stage drive floor: with a drive > 0 the fold saturates toward
  // +-1/drive; a small (< ~0.15) drive is numerically near-linear across the whole
  // nominal range, so the profile chooses drives in [0.6, 1.0] to make the
  // level dependence measurable. 0 = passthrough (no nonlinearity). No manual value.

 private:
  struct Channel {
    double freq = 0.3, res = 0.0, mod = 0.0;
    double mode = 0.0;   // 0.0=bp, 1.0=lp (positions[0/1]).
    double ic1eq = 0.0, ic2eq = 0.0;  // two-integrator trapezoidal (TPT/ZDF) states.
    double sr = 0.0;
    double inputDrive = 0.0;  //  per-channel input-stage drive (L/R independent).
    // Coefficient memo (pure caches of pow/tan results; see effCutoffHz_ / tick_).
    mutable double memoFreq = -1.0, memoSr = 0.0, memoBaseFc = 0.0;
    mutable double memoShift = 0.0, memoShiftScale = 1.0;
    mutable double memoFc = -1.0, memoG = 0.0;
    double memoRes = -1.0, memoK = kDampMax, memoBpGain = 2.0;
    double u1 = 0.0, u2 = 0.0;   // last integrator inputs (the saturation's operating point).
    std::uint64_t noise = 0x9E3779B97F4A7C15ULL;
  };

  static bool idx_(int ch) { return ch == 0 || ch == 1; }
  static double clamp01_(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

  // Effective upper cutoff cap: min(20000 Hz, 0.49·sr). 0.49 keeps the prewarped
  // tangent away from its singularity (fc/sr < 0.5) and is a software safety POLICY,
  // not a stability theorem. Never below the 20 Hz floor; if the sample rate ever made
  // the cap invert below the floor (unrealistic), clamp to the floor and stay finite.
  static double cutoffCapHz_(double sr) {
    const double cap = sr * kCutoffCapRatioSoft;
    if (cap < kFreqMinHz) return kFreqMinHz;
    return (cap < kFreqMaxHz) ? cap : kFreqMaxHz;
  }

  double baseFreqHz_(double freqNorm, double sr) const {
    double fc = kFreqMinHz * std::pow(kFreqMaxHz / kFreqMinHz, freqNorm);
    const double cap = cutoffCapHz_(sr);
    if (fc > cap) fc = cap;
    if (fc < kFreqMinHz) fc = kFreqMinHz;
    return fc;
  }

  // Effective cutoff for a channel, given the resolved CV for THAT channel and the
  // channel's own MOD depth. CV shifts cutoff multiplicatively (2^cv/oct, provisional).
  double effCutoffHz_(const Channel& c, double cvEff) const {
    // FREQ and the CV shift change far less often than every sample: memoise the pow.
    if (c.freq != c.memoFreq || c.memoSr != sr_) {
      c.memoFreq = c.freq;
      c.memoSr = sr_;
      c.memoBaseFc = baseFreqHz_(c.freq, sr_);
    }
    const double baseFc = c.memoBaseFc;
    const double shift = c.mod * cvEff / kCvVoltsPerOctave;
    if (shift != c.memoShift) {
      c.memoShift = shift;
      c.memoShiftScale = std::pow(2.0, shift);
    }
    double fc = baseFc * c.memoShiftScale;
    const double cap = cutoffCapHz_(sr_);
    if (fc > cap) fc = cap;
    if (fc < kFreqMinHz) fc = kFreqMinHz;
    return fc;
  }

  //  input-stage nonlinearity: odd, monotone, bounded (|y| <= 1/drive), and with
  // a unit small-signal slope. A large input folds toward a LOWER normalised gain,
  // so low/high amplitude sweep differ (the input-level dependence) while small
  // signals stay ~linear. drive==0 is an exact passthrough (no fold).
  double inputStage_(const Channel& c, double x) const {
    if (c.inputDrive <= 0.0) return x;
    return std::tanh(c.inputDrive * x) / c.inputDrive;
  }

  // Two-integrator trapezoidal (TPT/ZDF) state-variable filter (Simper's linear SVF,
  // "The Art of VA Filter Design") with saturating integrators. Each integrator's input u
  // drives it through Vs*tanh(u/Vs); following "mystran"'s cheap zero-delay
  // nonlinear filters (KVR, 2012), the tanh is linearised around the previous sample's input, which turns
  // it into a per-integrator gain g_i = g*tanh(u/Vs)/(u/Vs) and keeps the solve exact:
  //   v1 = (g1*(x - s2) + s1) / (1 + g1*(k + g2)),  v2 = s2 + g2*v1,
  //   s1 = 2v1 - s1, s2 = 2v2 - s2.     BP = v1, LP = v2.
  // With g1 = g2 = g this is the linear SVF exactly. The lowpass keeps unity DC gain for
  // any k (the integrators are idle at DC), so raising resonance does not drop the low end
  // (the Polivoks-defining property).
  double tick_(Channel& c, double x) {
    if (c.sr != sr_) { c.sr = sr_; }
    x = inputStage_(c, x);  // level-dependent, per-channel input nonlinearity.
    const double fc = effCutoffHz_(c, cvEffFor_(c));
    if (fc != c.memoFc) {
      c.memoFc = fc;
      c.memoG = std::tan(3.14159265358979323846 * fc / sr_);
    }
    if (c.res != c.memoRes) {
      c.memoRes = c.res;
      c.memoK = dampFor(c.res);
      c.memoBpGain = std::sqrt(2.0 * std::fmax(c.memoK, 0.02));
    }
    const double g = c.memoG;
    const double k = c.memoK;
    // A real filter past the oscillation point starts from its own noise; a whisper of
    // noise (about -140 dB) does the same here. Only in that range, so silence stays exact.
    if (k < 0.0) {
      c.noise = c.noise * 6364136223846793005ULL + 1442695040888963407ULL;
      x += 1e-7 * (static_cast<double>(c.noise >> 11) * (1.0 / 4503599627370496.0) - 1.0);
    }
    const double g1 = g * softGain_(c.u1, kIntegratorVolts);
    const double g2 = g * softGain_(c.u2, kIntegratorVolts);
    // Damping as the circuit sees it: the part of the resonance below kLoudDamp fades as
    // the band output (u2 = last v1) grows, so the loop settles instead of running away.
    const double push = kLoudDamp - k;
    const double kEff = push > 0.0 ? kLoudDamp - push * softGain_(c.u2, kResVolts) : k;
    const double v1 = (g1 * (x - c.ic2eq) + c.ic1eq) / (1.0 + g1 * (kEff + g2));
    const double v2 = c.ic2eq + g2 * v1;
    c.u1 = x - kEff * v1 - v2;  // what each integrator was driven with, for the next sample
    c.u2 = v1;
    c.ic1eq = 2.0 * v1 - c.ic1eq;
    c.ic2eq = 2.0 * v2 - c.ic2eq;
    if (!std::isfinite(c.ic1eq) || !std::isfinite(c.ic2eq)) {  // never latch a blow-up
      c.ic1eq = c.ic2eq = c.u1 = c.u2 = 0.0;
      return 0.0;
    }
    // Two-state output selection (bp | lp), not a continuous morph. The bandpass is scaled
    // so a wide (low-resonance) BP is about as loud as LP and a narrow one still rises
    // a little, rather than the raw SVF's 0.5x .. 10x. // tuned by ear
    return (c.mode < 0.5) ? c.memoBpGain * v1 : v2;
  }

  // tanh(u/V)/(u/V): 1 for small u, falling as the stage runs out of current.
  static double softGain_(double u, double volts) {
    const double a = std::fabs(u) / volts;
    if (a < 1e-4) return 1.0;
    return std::tanh(a) / a;
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
