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

#include "lunar24/core/blamp_kernel.h"
#include "lunar24/core/pulse_blep_kernel.h"
#include "lunar24/core/vco_wave_map.h"

namespace lunar24::core {

// The raw waveform shapes. The traditional four are an AS3340-convention inference (see
// PROVENANCE); the two morphing are manual-evidenced.
//
// ⚠️ PRODUCTION vs MODULE-DEV. `kMorphRing` is the PRODUCTION rendering law and the CONSTRUCTOR
// DEFAULT (see wave_ below): the continuous single-knob mapping of wave_map, driven by the
// EXISTING `morph` parameter. The other six values are the MODULE-DEVELOPMENT raw-waveform
// interface — each is one fixed shape, reachable only by an explicit setWaveform() call, and no
// product path makes one. They exist because the module's own unit tests drive the raw shapes
// directly (tests/core/test_vco.cpp), and they are kept for exactly that.
//
// There is deliberately NO runtime switch back to the pre-#117 fixed-triangle behaviour and no
// opt-in flag: the mapping IS the default, so it takes effect on every Vco the runtime builds
// with no extra call. See vco_wave_map.h for what is claimed, what is software-provisional, and
// what is a known reported property (the S0 mid-stretch silence, the missing AA on the
// mixed/saw/pulse paths).
enum class VcoWaveform : std::uint8_t {
  kSaw,               // module-dev raw shape (AS3340 inference)
  kTriangle,          // module-dev raw shape (AS3340 inference; the triangle core itself)
  kSine,              // module-dev raw shape (AS3340 inference)
  kPulse,             // module-dev raw shape; duty from SHAPE (pw)
  kMorphSawInvSaw,    // module-dev raw shape; manual-evidenced: saw <-> inverted saw
  kMorphSineTriangle, // module-dev raw shape; manual-evidenced: sine <-> triangle
  kMorphRing          // ⭐ PRODUCTION DEFAULT: the continuous single-knob sweep (wave_map)
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
  // MODULE-DEVELOPMENT raw-waveform selection. NOT a product control and NOT wired to any panel
  // control, jack, route or persisted byte: no product path calls this, and a Vco the runtime
  // builds renders kMorphRing without it. Its only callers are the module's own unit tests, which
  // need to isolate one raw shape at a time. Setting it does not disable the mapping's BLAMP
  // scaling — each shape keeps exactly the band-limiting it had before #117.
  void setWaveform(VcoWaveform w) { wave_ = w; }
  // Morph 0..1, the PRODUCTION waveform control (the panel's MORPHING WAVEFORM knob, one per
  // side; clamped). On the default kMorphRing rendering law it is the position along the whole
  // continuous sweep; on the module-dev raw shapes kMorphSawInvSaw / kMorphSineTriangle it is
  // that shape's own crossfade. Same parameter, same ID, same 0..1 range, same default 0.5.
  void setMorph(double m);
  // SHAPE = pulse-width duty for kPulse — the BASE pulse width. Clamped into a small (0,1) window
  // so an extreme setting can never collapse the pulse to a flat DC line / silence break (the
  // two-rail swing is always present; the must-test verifies it). This is the CANONICAL value the
  // panel knob (and its GH#21 smoothing) drives; PWM modulation must NOT write it back (see
  // setPwDepth/setPwCv below and effectiveDuty()).
  void setShape(double duty) {
    duty_ = duty < kPwDutyMin ? kPwDutyMin : (duty > kPwDutyMax ? kPwDutyMax : duty);
  }

  // ---------------------------------------------------------------------- PWM --
  // GH#19 S0 (task #117): the PWM jack's product consumer. On the hardware, PWM modulates the
  // pulse width; here it modulates the duty the pulse node (and only the pulse node) reads.
  //
  //   effectiveDuty = clamp(basePW + depth * cvVolts / 10, 0.001, 0.999)
  //
  // ⚠️ SOFTWARE PROVISIONAL TRANSFER. The ratio is NOT a hardware fact: the jack's nominal range
  // is ±5 V (registry descriptor vco_a.pwm_in), and /10 makes a full ±5 V swing at depth 1
  // contribute ±0.5 of duty. Positive CV RAISES the duty. Nothing in the manual or the panel
  // evidences this law; it is this implementation's declared model, reported as such.
  //
  // The two inputs are deliberately separate quantities with separate timing:
  //   * setPwDepth(depth) is the SMOOTHED PWM knob (0..1, default 0). It goes through the SAME
  //     GH#21 seconds-smoothing family as the other panel knobs — the runtime smooths it, not this
  //     class.
  //   * setPwCv(volts) is THIS SAMPLE's PWM CV read from the patch graph. It is EXTERNAL audio-rate
  //     modulation and is deliberately NOT smoothed (a smoother here would low-pass the patch and
  //     break the same-frame consumption the graph contract guarantees).
  // Neither writes duty_. depth = 0 therefore leaves the emitted samples BIT-IDENTICAL to the
  // pre-#117 behaviour, exactly (0 * cv == 0, and duty_ is already inside the clamp window).
  // No NaN passthrough: a non-finite depth or CV is treated as 0, and a non-finite sum falls back
  // to the canonical base width rather than propagating.
  void setPwDepth(double depth) { pwDepth_ = std::isfinite(depth) ? depth : 0.0; }
  void setPwCv(double volts) { pwCv_ = std::isfinite(volts) ? volts : 0.0; }
  double pwDepth() const { return pwDepth_; }
  double pwCv() const { return pwCv_; }
  double effectiveDuty() const {
    const double d = duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts;
    if (!std::isfinite(d)) return duty_;   // defensive: never emit a non-finite duty.
    return d < kPwDutyMin ? kPwDutyMin : (d > kPwDutyMax ? kPwDutyMax : d);
  }

  // ----------------------------------------------------------- linear FM input --
  // instFreq = pitch + fmDevHz * fmCv. fDev is the Hz depth, fmCv the FM CV (-5..+5).
  void setFmCv(double volts) { fmCv_ = volts; }
  void setFmDepth(double fDevHz) { fmDevHz_ = fDevHz; }

  // HARD SYNC (VCO A only): reset the unwrapped pitch phase to 0. VCO B has no
  // sync input and must never call this; test guards the asymmetry.
  //
  // syncPulse() is the RAW primitive: it zeroes the accumulator mid-sample, so the next
  // tick() advances once and reports phase `step` — the discontinuity and the cycle start
  // then sit one sample apart. It is kept unchanged because the VCO's own tests drive it
  // directly and assert that raw behaviour.
  void syncPulse() { cumPitch_ = 0.0; }

  // The PRODUCT hard-sync entry (GH#19 S5). requestSync() records the reset and lets tick()
  // apply it AFTER its advance, which makes the reset sample itself read phase 0: the value
  // discontinuity and the cycle start then coincide on one sample instead of straddling two,
  // and the new cycle's phase is exactly f0*(i - r)/sr for the reset sample r. tick() also
  // sizes that discontinuity and band-limits it (see tick()). The runtime calls THIS, never
  // syncPulse(), on the jack consumer path.
  void requestSync() { syncPending_ = true; }

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
  // Which rendering law this Vco is on. INSPECT ONLY, in the same readback group as the knobs
  // below: it is the observable form of the GH#19 S0 contract that a runtime-built Vco renders
  // kMorphRing (the continuous single-knob mapping) and has no product path back to a raw
  // module-development shape. No product path calls setWaveform; setting it does not change the
  // mapping's BLAMP scaling (each shape keeps the band-limiting it had before #117).
  VcoWaveform waveform() const { return wave_; }
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
  // The pulse-duty clamp window (SHAPE and effectiveDuty share it, so they cannot drift apart).
  static constexpr double kPwDutyMin = 1e-3;
  static constexpr double kPwDutyMax = 1.0 - 1e-3;
  // PWM full-scale divisor: 10 V == ±0.5 duty at depth 1 (jack nominal ±5 V). PROVISIONAL.
  static constexpr double kPwmCvFullScaleVolts = 10.0;

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
  // ⭐ PRODUCTION DEFAULT (GH#19 S0, task #117). The mapping is the rendering law, not an opt-in:
  // constructing a Vco is enough. `morph_ = 0.5` then lands exactly on the sweep's sine node, so
  // the default SOUND changes from triangle to sine. That is the intended, reported consequence of
  // adopting the mapping — the parameter ID, its 0..1 range, its 0.5 default value and the saved
  // bytes are all unchanged (the mapping is a pure function of the EXISTING morph value and is
  // never persisted).
  VcoWaveform wave_ = VcoWaveform::kMorphRing;
  double morph_ = 0.5;
  double duty_ = 0.5;                  // CANONICAL base pulse width (SHAPE); PWM never writes it.
  double pwDepth_ = 0.0;               // smoothed PWM knob depth, 0..1, default 0 (GH#19 S0).
  double pwCv_ = 0.0;                  // this sample's PWM CV in volts, NOT smoothed (GH#19 S0).
  double fmCv_ = 0.0;
  double fmDevHz_ = 0.0;
  double cumPitch_ = 0.0;              // unwrapped pitch phase (cycles).
  bool syncPending_ = false;           // hard-sync reset requested, applied in tick() (GH#19 S5).

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

  // The sample this path EMITS at an unwrapped phase `cp`: the waveform shape at frac(cp)
  // plus whatever band-limiting correction is already in force for the active waveform.
  // tick() uses it both for the sample it writes and to size a hard-sync reset's jump, so
  // the two can never drift apart.
  double emittedAt_(double cp, double step) const {
    double v = waveformSampleAt(frac(cp));
    const double tw = triangleBlampWeight_();
    if (tw > 0.0) v += tw * triangleBlampCorr(cp, step);
    const double pw = pulseBlepWeight_();
    if (pw > 0.0) v += pw * pulseBlepCorr_(cp, step);
    return v;
  }

  // GH#19 S3 (#118): the pulse's value-jump correction at an unwrapped phase, in the SAME
  // normalize-by-phase terms as the triangle correction above: the kernel wants a normalized
  // phase in [0,1) and the normalized per-sample increment, and `frac(cp)` is exactly the phase
  // the naive shape was read at, so the correction is read at that same phase and not at a
  // neighbouring one. `step` is the increment the phase accumulator actually advanced by, so a
  // corrected and an uncorrected render share one phase trajectory (see pulse_blep_kernel.h).
  //
  // The duty is EFFECTIVE duty, the same value waveformSampleAt feeds the pulse node: the two
  // edges of the waveform being corrected move with PWM, and a correction read against the raw
  // `duty_` would place the B edge at the wrong phase whenever PWM depth is non-zero. This is the
  // static-vs-moving-duty distinction -- a fixed-duty phase formula says nothing about a moving
  // edge, so the correction has to be driven by the duty the sampler used.
  double pulseBlepCorr_(double cp, double step) const {
    return polyblepPulseCorrection(frac(cp), effectiveDuty(), step);
  }

  // How much of the pulse value-jump correction is in force for the active waveform.
  //  * kMorphRing: the pulse NODE's weight in the mix (wave_map::pulseWeight). Exactly 1.0 at the
  //    pure-pulse node (morph = 1.0) and 0.0 outside stretch 3, so the correction runs in full
  //    where the output IS the pulse, not at all where it is not, and scaled by the node's weight
  //    in between -- first-order and software-provisional (vco_wave_map.h P5).
  //  * every other waveform: 0.0. The module-dev raw shapes are unchanged from before #118, so the
  //    existing kTriangle / kMorph* / drone paths emit bit-identical samples.
  double pulseBlepWeight_() const {
    switch (wave_) {
      case VcoWaveform::kMorphRing:
        return wave_map::pulseWeight(wave_map::kRingEqual, morph_);
      default:
        return 0.0;
    }
  }

  // How much of the EXISTING triangle slope correction is in force for the active waveform.
  //  * kTriangle (module-dev raw triangle): 1.0 — bit-identical to the pre-#117 behaviour.
  //  * kMorphRing: the triangle NODE's weight in the mix (wave_map::triangleWeight). It is exactly
  //    1.0 at the pure-triangle node (morph = 0.75) and 0.0 outside stretches 2 and 3, so the
  //    correction is applied in full where the output IS the triangle and not at all where it is
  //    not. First-order and software-provisional (vco_wave_map.h P3): it is NOT a normalisation of
  //    the output level, and it changes no other node's amplitude.
  //  * every other waveform: 0.0 — unchanged from before #117 (the naive morph shapes stay naive).
  double triangleBlampWeight_() const {
    switch (wave_) {
      case VcoWaveform::kTriangle:
        return 1.0;
      case VcoWaveform::kMorphRing:
        return wave_map::triangleWeight(wave_map::kRingEqual, morph_);
      default:
        return 0.0;
    }
  }
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
  // GH#19 S5: a requested hard-sync reset lands HERE, after the advance, so this sample
  // reads phase 0 (see requestSync()). `jmp` is the value discontinuity the reset creates,
  // measured on the EMITTED signal (shape + the band-limiting correction already in force),
  // not on the raw shape: the correction removes the step the emitted signal actually has.
  bool synced = false;
  double jmp = 0.0;
  if (syncPending_) {
    jmp = emittedAt_(0.0, step) - emittedAt_(cumPitch_, step);
    cumPitch_ = 0.0;
    syncPending_ = false;
    synced = true;
  }
  *out = emittedAt_(cumPitch_, step);
  // Band-limit the reset's VALUE jump: a hard sync truncates the cycle wherever the master
  // fires, so the emitted sample steps by an arbitrary J (up to the full peak-to-peak swing),
  // not by the fixed -2 of a saw wrap. The term applied is `out -= 0.5*J` (equivalently
  // `+= 0.5*J*R`) — one half of the jump, on the post-reset sample.
  //
  // SCOPE OF THAT FORMULA (do not over-read it): `-0.5*J` is a single-point MEDIAN correction at
  // the reset sample, applied as the approximation this implementation adopts. It is NOT the
  // complete Si step residual: the full response is `b(n) = 1/2 + Si(pi*n)/pi`, whose tail at the
  // SUBSEQUENT integer samples is not all zero, and this implementation deliberately does not
  // compute that tail -- it is left uncorrected (see the BLAMP note below for the measured
  // reason). The master's edge does land on a sample grid point in these cells (probe: per-sample
  // step 1/M, period M samples), but that alignment is NOT what makes the tail go away. Nothing in
  // this comment generalises to arbitrary f0 / FM / event phase: the evidence on the record is the
  // 12 declared sync cells only, and an off-grid edge is NOT covered.
  // NOT `polyblepSaw`-style full-scale subtraction and NOT the other sign: both were measured
  // on the record (see report/2026-09-12-task111-gh19-s5-hard-sync.md), and `-=0.5*J` is the only
  // one that lowers the residual. Adding the BLAMP slope-jump term at the same instant was
  // MEASURED TO DEGRADE this residual on all 12 cells (+2.26..+4.89 dB), not to lower it --
  // consistent with re-correcting the peak corner that emittedAt_() already band-limits, since
  // the reset lands on phase 0. It is therefore deliberately NOT covered here and the
  // second-order term is left UNCOVERED (same report).
  if (synced) *out -= 0.5 * jmp;
  // The triangle slope-jump (peak/valley corner) band-limiting correction is applied inside
  // emittedAt_() above — for the product-reachable A/B-shared triangle only. Morphing
  // sine<->triangle stays the naive blend (out of scope). It must NOT be added again here:
  // doing so double-counts it on every triangle sample while leaving every other waveform
  // correct, which is exactly what the GH#19 S5 "unwired => byte-identical" regression lock
  // caught (see report/2026-09-12-task111-gh19-s5-hard-sync.md).
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
      // bipolar pulse, duty in (0,1). The duty is effectiveDuty(), i.e. the base width plus the
      // PWM modulation; with the PWM depth at its 0 default this is bit-identical to duty_.
      return (p < effectiveDuty()) ? 1.0 : -1.0;
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
    case VcoWaveform::kMorphRing:
      // ⭐ PRODUCTION LAW. The whole sweep is wave_map's, evaluated on the EXISTING morph value
      // and the EXISTING duty (the pulse node reads the SHAPE knob). The phase convention is the
      // one already in force here: p = frac(cumPitch_), one cycle per unit.
      // Morph 0.25 / 0.75 land exactly ON the ring's inverted-saw node and triangle node. Those are
      // the ENDPOINT shapes of the two stretches, so they equal the module-dev raw shapes
      // kMorphSawInvSaw / kMorphSineTriangle at THEIR OWN morph = 1 — NOT at 0.25 / 0.75. A raw
      // shape evaluated at the same numeric morph is a different signal: kMorphSawInvSaw at 0.25 is
      // (1-0.25)*saw + 0.25*(-saw) = 0.5*saw, a half-amplitude saw, not the inverted saw. Inside a
      // stretch the ring is that stretch's closed form at the stretch's LOCAL coordinate u in [0,1]
      // (vco_wave_map.h morphSawInvSaw / morphSineTriangle, documented there as "== kMorphSawInvSaw
      // with morph_ = u"), never at the global morph value.
      // The pulse node reads effectiveDuty() (the base width plus this sample's PWM modulation);
      // depth 0 leaves it bit-identical to the base width.
      return wave_map::sampleAt(wave_map::kRingEqual, morph_, p, effectiveDuty());
  }
  return 0.0;
}

// ----------------------------------------------------------------------------
// VCO triangle slope corrector. The KERNEL now lives in the shared
// blamp_kernel.h (task #109): the Schmitt ramp has a slope discontinuity of the
// same kind, so there is one windowed analytic BLAMP and two callers, each with
// its own shape and its own scale. The kernel's derivation, window, LUT and
// truncation caveats are documented there and are deliberately NOT duplicated here.
//
// The triangle has first-derivative (slope) discontinuities at the peak (phase =
// integer, slope jumps -8) and valley (phase = half-integer, slope jumps +8).
// A BLAMP is the correct band-limiting kernel for a slope jump (it is the
// antiderivative of a BLEP).
inline double Vco::blampG(double u) { return blampKernel(u); }

inline double Vco::triangleBlampCorr(double cp, double step) const {
  const double dt = std::fabs(step);        // |instHz| / sr; finite on reversal.
  if (!(dt > 0.0) || !std::isfinite(dt)) return 0.0;  // zero/NaN step -> none.
  // Bounded high-step fallback: once the window radius reaches half a period,
  // the peak and valley corners merge and the phase-local corner model is no
  // longer verifiable. Return the bounded naive waveform (no correction), and
  // list this range honestly (not as improved coverage). The predicate is SHARED
  // with the Schmitt ramp so both callers fall back on the same physical fact.
  if (blampSupportReachesHalfPeriod(dt)) return 0.0;
  // Paper scale: the residual is scaled by 2*mu with |mu| = 2*Tphi = 4*dt for the
  // +-1 triangle, so 2*mu = 8*dt; blampKernel already carries that 8 (corner
  // 8/pi^2), so the per-sample scale here is mag = dt, i.e. 2*mu*R = dt*(8/pi^2).
  // (A naive 8*dt here would double-count the 8 and run 8x too large.)
  const double mag = dt;                    // per-sample scale; kernel peak = 8/pi^2.
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
