// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vco — P3-③: a single triangle-core analog VCO (AS3340 family) that models BOTH
// VCO A and VCO B. This is the DSP core. The A/B *module shape* (which jacks
// exist) is a registry fact already landed: vco_a has a SYNC input, vco_b has a
// VCO output, and VCO A's output is normalised into VCO B's CV input (CV AMT knob)
// — the latter is P2-②'s NormalizedRoute, re-presented here as a fact, NOT re-coded
// in this header. The core therefore carries the shared oscillator DSP; the
// sync-only-on-A asymmetry is guarded by test (see syncPulse below).
//
// PROVENANCE — the registry (generated/lunar24/registry.hpp) is the frozen
// implementation basis; every claim below mirrors it and marks evidence strength
// precisely (@Claude GO 604ed080). "Confirmed" == all relevant field evidence
// confirmed; "provisional" == position/value is evidenced in name but the exact
// number is not written in the manual; "unverified" == the field evidence is
// unverified and it may conflict with the manual's literal wording.
//
//   * V/OCT (v_oct_in) — SignalType::cv, polarity unipolar, range 0..8 V,
//     SignalTransfer::exponential, direction/type/transfer CONFIRMED.
//     pitch = fBase * 2^vOct ; every 1 V exactly doubles across the whole 0..8 V.
//   * Octave selector (oct_sel) — 3 positions "low"/"0"/"+3", default index 1="0"
//     (all six field evidence PROVISIONAL). The position NAMES are evidenced; the
//     exact octave COUNT per position is not (manual writes only "+3 / low"; what
//     "low" gives is not in the manual). Encoded here as {low:-1, "0":0, "+3":+3}
//     octaves — PROVISIONAL, and the value for "low" is a standing guess, not fact.
//   * Sub selector (sub_sel) — 2 positions "0"/"-1", default index 1="-1"
//     (PROVISIONAL). Sub is one octave down.
//   * Tune (tune) — unit "oct", range -1..+1, default 0, but ALL six field evidence
//     are UNVERIFIED and this conflicts with the manual literal "tune knob controls
//     the pitch continuously over ONE octave" (registry reads -1..+1 = two octaves).
//     This conflict is recorded in FINDINGS as a MUST-RESOLVE item and is NOT
//     silently resolved here. Implemented per registry (oct, -1..+1).
//   * CV input lin/exp (lin_exp) — selector "lin"/"exp". The SELECTOR's documented default index is
//     1="exp"; the registry initial for the DeviceState default is 0="lin" (a separate layer). The
//     module CONSTRUCTOR default is kExponential; a state apply decides the effective mode.
//     (PROVISIONAL). The generic CV input is a modulation input with UNKNOWN
//     transfer (cv_in is -5..+5). The linear-vs-exponential law below is a
//     PROVISIONAL modeling choice. V/OCT (confirmed) is the calibrated 1 V/oct
//     pitch input; the generic CV input is an uncalibrated modulation input.
//   * SHAPE (pw / pulse width) — 0..1, default 0.5 (unverified). Duty, for kPulse.
//
// WAVEFORM PROVENANCE — the "traditional four" (saw / triangle / sine / pulse) is
// an INFERENCE from AS3340 convention. The manual says only "6 waveforms, two
// morphing (saw<->inverted saw, sine<->triangle)" and never enumerates the four.
// This is recorded in FINDINGS as inference, NOT manual evidence. The two morphing
// waveforms are evidenced by the manual's own parenthetical.
//
// SUB PHASE-LOCK — the sub is derived from the SAME unwrapped pitch accumulator
// (subPhase = frac(cumPitch/2)), so it is exactly one octave down AND phase-locked
// to the parent with a fixed, deterministic relationship. This is the OPPOSITE of
// the drone-bank requirement (voices must be independent); an independent
// free-running sub is the negative control for the phase-lock test.
//
// HARD SYNC — syncPulse() resets the unwrapped phase to 0 (the slave restarts).
// This is the sync input that exists on VCO A only. VCO B has NO sync input, so a
// correct B path never calls syncPulse(); the test asserts B's phase is continuous
// and that a B given a sync pulse shows a discontinuity the continuity detector
// fires on. Hard sync is a prime aliasing source (measured in FINDINGS).
//
// Value source = control inputs only (no seed / no hidden randomness); identical
// inputs + identical sample rate + identical buffer partition => bit-identical
// output. Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>
#include <cstdint>

namespace lunar24::core {

// The six selectable waveforms. The traditional four are an AS3340-convention
// inference (see PROVENANCE); the two morphing are manual-evidenced.
enum class VcoWaveform : std::uint8_t {
  kSaw,               // traditional (AS3340 inference)
  kTriangle,          // traditional (AS3340 inference; the triangle core itself)
  kSine,              // traditional (AS3340 inference)
  kPulse,             // traditional; duty from SHAPE (pw)
  kMorphSawInvSaw,    // manual-evidenced: saw <-> inverted saw
  kMorphSineTriangle  // manual-evidenced: sine <-> triangle
};

// CV input lin/exp mode (lin_exp selector). The MODULE CONSTRUCTOR default here is index 1 =
// kExponential (the registry selector's documented default index); the DeviceState DEFAULT
// (registry initial field) for the lin_exp parameter is 0 = kLinear, which overrides this on the
// canonical power-on state. The two defaults are DISTINCT layers and are kept separate — a state
// apply, not this constructor, decides the effective mode. (@Codex 44369539 task#78: clarify, no
// registry or sound-behavior change.)
enum class VcoControlMode : std::uint8_t { kLinear, kExponential };

class Vco {
 public:
  // sampleRate must be > 0. Reproducible: no seed, value comes only from inputs.
  explicit Vco(double sampleRate) : sr_(sampleRate) {}

  // ---------------------------------------------------------------- frequency --
  // Baseline oscillator frequency (the f0 the whole chain is referenced to).
  void setBaseHz(double hz) { baseHz_ = hz; }

  // V/OCT jack: 0..8 V, exponential (CONFIRMED). Every 1 V doubles.
  void setVoct(double volts) { vOct_ = volts; }

  // Generic CV input with a lin/exp mode switch (cv_in -5..+5). The scaling is a
  // PROVISIONAL modeling choice (cv_in transfer is UNKNOWN). cvAmt scales it.
  void setCvInput(double volts, VcoControlMode mode) { cv_ = volts; cvMode_ = mode; }
  void setCvAmt(double amt) { cvAmt_ = amt; }  // CV AMT knob, 0..1, default 1.

  // Octave selector: 0="low", 1="0", 2="+3". Octave COUNTS are PROVISIONAL.
  void setOctaveSelect(int index);
  // Tune knob: unit "oct", range -1..+1 (UNVERIFIED + conflicts with manual's
  // "one octave"; see FINDINGS — must-resolve, NOT silently resolved here).
  void setTune(double oct) { tune_ = oct; }
  // Sub selector: 0="0" (off), 1="-1" (one octave down). PROVISIONAL.
  void setSubSelect(int index) { subSelect_ = index; }

  // ---------------------------------------------------------------- waveform --
  void setWaveform(VcoWaveform w) { wave_ = w; }
  // Morph 0..1; only meaningful for the two morphing waveforms (clamped).
  void setMorph(double m);
  // SHAPE = pulse-width duty for kPulse. Clamped into a small (0,1) window so an
  // extreme setting can never collapse the pulse to a flat DC line / silence break
  // (the two-rail swing is always present; the must-test verifies it).
  void setShape(double duty) {
    constexpr double kMinDuty = 1e-3, kMaxDuty = 1.0 - 1e-3;
    duty_ = duty < kMinDuty ? kMinDuty : (duty > kMaxDuty ? kMaxDuty : duty);
  }

  // ----------------------------------------------------------- linear FM input --
  // instFreq = pitch + fmDevHz * fmCv. fDev is the Hz depth, fmCv the FM CV (-5..+5).
  void setFmCv(double volts) { fmCv_ = volts; }
  void setFmDepth(double fDevHz) { fmDevHz_ = fDevHz; }

  // HARD SYNC (VCO A only): reset the unwrapped pitch phase to 0. VCO B has no
  // sync input and must never call this; test guards the asymmetry.
  void syncPulse() { cumPitch_ = 0.0; }

  // ---------------------------------------------------------------- render --
  // Advance one sample. Writes the main waveform into *out; if subOut is non-null,
  // the sub waveform (one octave down, phase-locked) into *subOut.
  void tick(double* out, double* subOut = nullptr);

  // ---------------------------------------------------------------- inspect --
  double frequencyHz() const;   // core pitch (before FM), at current inputs.
  double subFrequencyHz() const { return frequencyHz() * 0.5; }
  double phase() const { return frac(cumPitch_); }       // parent phase [0,1).
  double subPhase() const { return frac(cumPitch_ * 0.5); }  // sub phase [0,1).
  bool subEnabled() const { return subSelect_ == 1; }    // index 1 = "-1".
  double waveformSampleAt(double p) const;  // shape at a phase, for morph checks.

  // Panel-control READBACK (task #78): the applied knob positions, so a product
  // oracle can verify a state restore truly reached THIS DSP instance (return the raw
  // stored knob — post-clamp where the setter clamps). Never a shadow parameter bank;
  // these read the same members the render path consumes.
  double tune() const { return tune_; }        // oct, raw store (no clamp).
  double morph() const { return morph_; }      // 0..1, post-clamp.
  double shape() const { return duty_; }       // pulse-width duty, post-clamp.
  int octaveSelect() const { return octSelect_; }   // 0..2, post-clamp.
  int subSelectIndex() const { return subSelect_; } // 0..1.
  double cvAmt() const { return cvAmt_; }      // 0..1.
  VcoControlMode controlMode() const { return cvMode_; }  // lin/exp mode.

  static constexpr double kLowOctave = -1.0;  // oct_sel "low" (PROVISIONAL).
  static constexpr double kZeroOctave = 0.0;  // oct_sel "0".
  static constexpr double kPlus3Octave = 3.0; // oct_sel "+3" (approximately +3 oct).
  static constexpr double kTwoPi = 6.28318530717958647692528676655900577;

 private:
  static double frac(double v) { return v - std::floor(v); }

  double sr_;
  double baseHz_ = 0.0;
  double vOct_ = 0.0;
  double cv_ = 0.0;
  double cvAmt_ = 1.0;
  int octSelect_ = 1;                  // default "0".
  double tune_ = 0.0;
  int subSelect_ = 1;                  // default "-1".
  VcoControlMode cvMode_ = VcoControlMode::kExponential;  // default index 1.
  VcoWaveform wave_ = VcoWaveform::kTriangle;
  double morph_ = 0.5;
  double duty_ = 0.5;
  double fmCv_ = 0.0;
  double fmDevHz_ = 0.0;
  double cumPitch_ = 0.0;              // unwrapped pitch phase (cycles).

  // Analytic-source BLAMP triangle slope correction (Esqueda, Välimäki & Bilbao,
  // "Rounding Corners with BLAMP", DAFx-16; residual R(u) from paper Eq.(6) minus
  // the trivial ramp). A triangle's discontinuities are first-DERIVATIVE (slope)
  // jumps, so the correct band-limiting tool is a BLAMP (the second integral of the
  // basis minus the ramp), NOT a step-BLEP. The residual is even about the corner,
  // is windowed by an explicit C1 Hann window cos^2(pi*u/(2L)) of support L=8
  // samples (boundary value AND slope -> 0), and is scaled by the slope-jump
  // magnitude 2µ = 8*|step| for the ±1 triangle (slope ±4 per phase). It is
  // evaluated with a linear-interpolation LUT (N=256), so the FINAL kernel is only
  // C0 / piecewise-linear — NOT C1 (the continuous window is C1; the LUT is not).
  // It is a pure function of phase (no cross-block state), causal/zero-latency.
  static double blampG(double u);        // windowed analytic residual, corner 8/pi^2.
  double triangleBlampCorr(double cp, double step) const;
};

inline void Vco::setOctaveSelect(int index) {
  octSelect_ = index < 0 ? 0 : (index > 2 ? 2 : index);
}

inline void Vco::setMorph(double m) { morph_ = m < 0.0 ? 0.0 : (m > 1.0 ? 1.0 : m); }

inline double Vco::frequencyHz() const {
  const double octs[3] = {kLowOctave, kZeroOctave, kPlus3Octave};
  double p = baseHz_ * std::pow(2.0, octs[octSelect_] + tune_);
  p *= std::pow(2.0, vOct_);                       // V/OCT: confirmed, 1 V = 1 oct.
  const double cvEff = cv_ * cvAmt_;
  if (cvMode_ == VcoControlMode::kExponential) {
    p *= std::pow(2.0, cvEff);                     // PROVISIONAL exp scaling.
  } else {
    p *= (1.0 + cvEff);                            // PROVISIONAL linear scaling.
  }
  return p > 0.0 ? p : 0.0;
}

inline void Vco::tick(double* out, double* subOut) {
  const double pitch = frequencyHz();
  const double instHz = pitch + fmDevHz_ * fmCv_;  // linear FM.
  const double step = instHz / sr_;
  cumPitch_ += step;
  *out = waveformSampleAt(phase());
  if (wave_ == VcoWaveform::kTriangle) {
    // Band-limit the slope jumps (peak/valley corners) for the product-reachable
    // A/B-shared triangle. Morphing sine<->triangle stays the naive blend (out of
    // scope); this adds the windowed analytic BLAMP approximation (linear-interp LUT), phase-local and zero-latency.
    *out += triangleBlampCorr(cumPitch_, step);
  }
  if (subOut) {
    if (subEnabled()) {
      *subOut = 2.0 * subPhase() - 1.0;   // sub = rising SAW (2*frac(cumPitch_*0.5)-1), one octave down, phase-locked.
    } else {
      *subOut = 0.0;
    }
  }
}

// Waveform level at a given phase p in [0,1). Morphing waveforms crossfade in
// phase-locked space so the morph is continuous by construction.
inline double Vco::waveformSampleAt(double p) const {
  const double a = 2.0 * p - 1.0;                     // saw (rising).
  switch (wave_) {
    case VcoWaveform::kSaw:
      return a;
    case VcoWaveform::kTriangle:
      return 4.0 * std::fabs(p - 0.5) - 1.0;          // p=0->+1 .. p=0.5->-1.
    case VcoWaveform::kSine:
      return std::sin(kTwoPi * p);
    case VcoWaveform::kPulse:
      return (p < duty_) ? 1.0 : -1.0;                // bipolar pulse, duty in (0,1).
    case VcoWaveform::kMorphSawInvSaw: {
      // crossfade saw <-> inverted saw; continuous (passes through 0 at mid).
      const double inv = -a;
      return (1.0 - morph_) * a + morph_ * inv;
    }
    case VcoWaveform::kMorphSineTriangle: {
      const double s = std::sin(kTwoPi * p);
      const double t = 4.0 * std::fabs(p - 0.5) - 1.0;
      return (1.0 - morph_) * s + morph_ * t;
    }
  }
  return 0.0;
}

// ----------------------------------------------------------------------------
// Windowed analytic BLAMP triangle slope corrector (Esqueda, Välimäki & Bilbao,
// "Rounding Corners with BLAMP", Proc. DAFx-16, Brno, 2016).
//
// The triangle has first-derivative (slope) discontinuities at the peak (phase =
// integer, slope jumps -8) and valley (phase = half-integer, slope jumps +8).
// A BLAMP is the correct band-limiting kernel for a slope jump (it is the
// antiderivative of a BLEP). The ideal 2nd-integral kernel h(2) minus its trivial
// ramp gives the dimensionless residual R(u) (paper Eq.(6) form, U-sized), which
// is EVEN about the corner (R(-u)=R(u)) with R(0)=1/pi^2. We window it with an
// explicit C1 Hann window w(u)=cos^2(pi*u/(2L)) of support L=8 samples, so the
// CONTINUOUS kernel has both value AND first derivative -> 0 at the support edge.
// The scaled kernel g(u) = 8*R(u)*w(u) has corner 8/pi^2 (~0.810569469); per the
// paper (§4.1) it is scaled by 2µ with µ = 4*|step|, so 2µ = 8*|step|, encoded here
// by calling blampG with the per-sample distance and multiplying by mag = |step|.
//
// The kernel is evaluated by a LINEAR-INTERPOLATION LUT (N=256 points over [0,L]),
// generated by tools/gen_gh19_blamp.py --L 8 --N 256. Consequently the FINAL
// discrete kernel is piecewise-linear (C0), only approximating the C1 continuous
// kernel; the residual at/beyond the support edge is exactly 0, but the LAST
// interpolation segment carries a small residual slope (~-6.9e-6) before dropping
// to 0 outside support — a tiny, quantified discontinuity in slope (not a value
// jump). It is a pure function of (phase, step) — no cross-block state, causal, no
// added latency. It is a truncated approximation, NOT the exact infinite-support
// kernel, and is labeled accordingly (no "no-droop"/"exact" claim).
inline double Vco::blampG(double u) {
// Windowed analytic BLAMP residual LUT (g = 8*R*w). R = paper Eq.(6) minus the
// trivial ramp (dimensionless residual); w = C1 Hann window cos^2(pi*u/(2L));
// corner 8/pi^2 (~0.810569469). Support radius L=8 samples; the CONTINUOUS kernel
// is C1 at the boundary (value and slope -> 0). The LUT is linear-interpolated so
// the FINAL kernel is piecewise-linear (C0), not C1: the last interpolation segment
// has a small slope (~-6.9e-6) then 0 outside support — a tiny slope discontinuity,
// not a value jump. Generated by gen_gh19_blamp.py --L 8 --N 256 --cpp.
static constexpr double kLut[] = {
  0.810569469, 0.688989014, 0.575232564, 0.469285207, 0.371103333, 0.280615065, 0.197720873, 0.122294346,
  0.054183135, -0.006789959, -0.060825742, -0.108147254, -0.148998261, -0.183641643, -0.212357672, -0.235442196,
  -0.253204759, -0.265966646, -0.274058889, -0.277820230, -0.277595079, -0.273731452, -0.266578937, -0.256486668,
  -0.243801355, -0.228865354, -0.212014815, -0.193577899, -0.173873090, -0.153207613, -0.131875955, -0.110158512,
  -0.088320366, -0.066610190, -0.045259300, -0.024480853, -0.004469186, 0.014600688, 0.032573436, 0.049313607,
  0.064705692, 0.078654032, 0.091082585, 0.101934563, 0.111171924, 0.118774758, 0.124740543, 0.129083301,
  0.131832654, 0.133032794, 0.132741377, 0.131028346, 0.127974702, 0.123671236, 0.118217223, 0.111719098,
  0.104289125, 0.096044065, 0.087103866, 0.077590364, 0.067626032, 0.057332767, 0.046830727, 0.036237238,
  0.025665762, 0.015224945, 0.005017746, -0.004859346, -0.014317007, -0.023273678, -0.031656066, -0.039399548,
  -0.046448483, -0.052756414, -0.058286188, -0.063009969, -0.066909168, -0.069974282, -0.072204653, -0.073608145,
  -0.074200751, -0.074006135, -0.073055114, -0.071385084, -0.069039410, -0.066066764, -0.062520449, -0.058457692,
  -0.053938918, -0.049027029, -0.043786674, -0.038283530, -0.032583597, -0.026752515, -0.020854906, -0.014953750,
  -0.009109803, -0.003381048, 0.002177796, 0.007515721, 0.012585825, 0.017345646, 0.021757445, 0.025788425,
  0.029410899, 0.032602391, 0.035345686, 0.037628825, 0.039445037, 0.040792631, 0.041674832, 0.042099571,
  0.042079239, 0.041630395, 0.040773450, 0.039532312, 0.037934014, 0.036008318, 0.033787304, 0.031304954,
  0.028596723, 0.025699115, 0.022649258, 0.019484494, 0.016241974, 0.012958273, 0.009669023, 0.006408574,
  0.003209671, 0.000103168, -0.002882229, -0.005720191, -0.008386956, -0.010861489, -0.013125603, -0.015164049,
  -0.016964565, -0.018517901, -0.019817794, -0.020860933, -0.021646872, -0.022177930, -0.022459057, -0.022497676,
  -0.022303513, -0.021888390, -0.021266024, -0.020451795, -0.019462516, -0.018316188, -0.017031758, -0.015628869,
  -0.014127617, -0.012548311, -0.010911238, -0.009236440, -0.007543499, -0.005851343, -0.004178056, -0.002540716,
  -0.000955242, 0.000563730, 0.002002966, 0.003350711, 0.004596771, 0.005732549, 0.006751082, 0.007647041,
  0.008416725, 0.009058028, 0.009570392, 0.009954742, 0.010213410, 0.010350043, 0.010369498, 0.010277730,
  0.010081670, 0.009789102, 0.009408523, 0.008949014, 0.008420102, 0.007831626, 0.007193602, 0.006516095,
  0.005809096, 0.005082404, 0.004345515, 0.003607524, 0.002877033, 0.002162069, 0.001470017, 0.000807559,
  0.000180630, -0.000405617, -0.000946837, -0.001439500, -0.001880896, -0.002269121, -0.002603060, -0.002882355,
  -0.003107369, -0.003279138, -0.003399321, -0.003470142, -0.003494324, -0.003475028, -0.003415778, -0.003320399,
  -0.003192939, -0.003037601, -0.002858678, -0.002660483, -0.002447289, -0.002223268, -0.001992439, -0.001758617,
  -0.001525373, -0.001295993, -0.001073452, -0.000860383, -0.000659068, -0.000471418, -0.000298975, -0.000142908,
  -0.000004024, 0.000117222, 0.000220709, 0.000306625, 0.000375447, 0.000427907, 0.000464963, 0.000487761,
  0.000497606, 0.000495920, 0.000484211, 0.000464032, 0.000436952, 0.000404521, 0.000368238, 0.000329526,
  0.000289706, 0.000249976, 0.000211391, 0.000174854, 0.000141100, 0.000110695, 0.000084032, 0.000061333,
  0.000042653, 0.000027892, 0.000016807, 0.000009024, 0.000004059, 0.000001338, 0.000000216, 0.000000000,
};
static constexpr int kN_blamp = 255;       // [0,L] -> index [0,N-1].
const double a = std::fabs(u);
if (!(a > 0.0)) return kLut[0];                 // u=0/NaN -> corner 8/pi^2.
if (a >= 8) return 0.0;                       // windowed -> exactly 0 outside support.
const double tk = a / 8 * kN_blamp;
const int i = static_cast<int>(tk);
const double fr = tk - (double)i;
return kLut[i] * (1.0 - fr) + kLut[i + 1] * fr;
}


inline double Vco::triangleBlampCorr(double cp, double step) const {
  const double dt = std::fabs(step);        // |instHz| / sr; finite on reversal.
  if (!(dt > 0.0) || !std::isfinite(dt)) return 0.0;  // zero/NaN step -> none.
  // Bounded high-step fallback: once the window radius reaches half a period,
  // the peak and valley corners merge and the phase-local corner model is no
  // longer verifiable. Return the bounded naive waveform (no correction), and
  // list this range honestly (not as improved coverage).
  if (dt * 8 >= 0.5) return 0.0;
  // Paper scale: residual scaled by 2*mu, with |mu| = 2*Tphi = 4*dt for the
  // +-1 triangle, so 2*mu = 8*dt. blampG already carries that 8 (corner 8/pi^2),
  // so the extra per-sample scale is dt: 2*mu * R = 8*dt * (1/pi^2) = dt*(8/pi^2).
  // (A naive 8*dt here would double-count the 8 and run 8x too large.)
  const double mag = dt;                    // per-sample scale; blampG peak = 8/pi^2.
  // Multi-wrap: sum ALL corners (peaks at integer phase, valleys at half-integer)
  // whose distance in samples from this sample is within the support L. The
  // previous code (single nearest peak + single nearest valley) missed corners
  // that a large step carries past a sample within the window. blampG is 0
  // outside L (windowed), so we only scan the whole periods that can touch it.
  const double frac = cp - std::floor(cp);  // phase within this period, in [0,1).
  const int radius = static_cast<int>(std::ceil(8 * dt)) + 1;
  double corr = 0.0;
  for (int n = -radius; n <= radius; ++n) {
    double d = std::fabs((double)n - frac) / dt;      // peak at integer+n phase.
    if (d <= 8) corr -= mag * blampG(d);          // -8 slope jump -> negative.
    d = std::fabs((double)n + 0.5 - frac) / dt;       // valley at half-integer+n.
    if (d <= 8) corr += mag * blampG(d);          // +8 slope jump -> positive.
  }
  return corr;
}


}  // namespace lunar24::core
