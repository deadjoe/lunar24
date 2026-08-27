// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// DroneBank — the CLASSIC drone bank of P3-① / #39 (structure fidelity).
//
// The Solar 42N manual DRONE VOICES 1, 2, 4, 5 "CLASSIC SOLAR 50":
//   * each voice = 5 simple SAWTOOTH oscillators, "NO VOLT OCTAVE" (free-running).
//   * the circuit of each oscillator includes a NEGISTOR oscillator (single
//     transistor) — the real, non-identity nonlinearity.
//   * the 1st & 2nd oscillators are LOW pitched, the 3rd MEDIUM, the 4th & 5th HIGH.
//   * each generator has MUTE / TUNE / MOD; a shared VOLT knob transposes all five
//     down and, past half its stroke, starts the generators modulating each other
//     (mutual FM).
//
// STRUCTURE vs CONSTANTS (design/07 §3): the structure above is real and present
// (roles, sawtooth, a genuine nonlinear transfer, per-generator controls, shared
// VOLT transpose, mutual FM). The exact circuit constants — negistor curve
// coefficients, the tune/VOLT law, the FM depth — are NOT in the manual and are
// therefore PROVISIONAL, every one flagged `provisional` here and recorded in the
// FINDINGS ledger. We never pretend to a measured model ("参数只能随证据校准").
//
// Bank = kMaxVoices (20) free-running oscillators, read as 4 classic voices × 5
// generators (drone 1/2/4/5); drone 3/6 are NEW (Papa Srapa, P3-②) and are not in
// this bank (the runtime mutes those channels). voiceCount < 20 is still allowed
// for the P3-① tolerance/drift/sr must-tests, which probe the frequency model
// rather than the full voice group.
//
// FREQUENCY MODEL (kept separable so tolerance vs drift stays testable):
//   effFreq(t) = roleBase * tuneScale * voltScale * (1 + tolerance) + drift(t)
//             + mod(t) + fm(t)
//   * roleBase  — seeded within a role band (low < medium < high), the per-part
//                 nominal pitch.
//   * tuneScale — per-generator TUNE, semitone offset: 2^(tune/12).
//   * voltScale — shared VOLT transpose (down): 2^(-volt/12).
//   * tolerance — STATIC seeded fraction of the role base.
//   * drift(t)  — dynamic slow sub-acoustic sum (unchanged from P3-①).
//   * mod(t)    — per-generator MOD: modAmount * modCv (button on => CV/photo
//                 detunes; button off => the generator runs stably).
//   * fm(t)     — mutual FM between generators, active only past the VOLT halfturn.
//
// WAVEFORM: sapply_() is a true sawtooth (2*(phase/2pi)-1). nonlinear_() is a real
// negistor-shaped transfer, NOT identity (a monotonic saturation proxy for the
// single-transistor negative-resistance region; coefficients provisional).
//
// Realtime-safe: tick() never allocates or blocks; all control writes (setMute/
// setTune/setMod/setVolt) are plain field stores. SPDX.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "lunar24/core/seeded_random.h"

namespace lunar24::core {

class DroneBank {
 public:
  static constexpr std::size_t kMaxVoices = 20;
  static constexpr std::size_t kGensPerVoice = 5;  // a classic voice is 5 generators.
  static constexpr std::size_t kClassicVoices = 4;  // drone 1/2/4/5.

  enum class Role : std::uint8_t { kLow = 0, kMedium = 1, kHigh = 2 };

  // A single generator's state. Static terms are set once (seeded, never changed
  // by tick); the accumulator is the only state tick() advances.
  struct Voice {
    double freqBaseHz;   // nominal frequency from the ROLE BAND, seeded, static.
    double tolerance;    // STATIC fractional tolerance of freqBase, seeded.
    double amplitude;    // output gain, seeded.
    double driftF1Hz;    // drift sine-1 rate, seeded (Hz of the sub-drift osc).
    double driftF2Hz;    // drift sine-2 rate, seeded.
    double driftA1;      // drift sine-1 amplitude, seeded (fraction of base).
    double driftA2;      // drift sine-2 amplitude, seeded.
    double driftPh1;     // drift sine-1 phase, seeded.
    double driftPh2;     // drift sine-2 phase, seeded.
    double phase;        // phase accumulator, radians, advanced each tick.
    double driftNow;     // current drifted Hertz offset.
    Role role;           // role within the voice (1,2=low; 3=med; 4,5=high).
    bool muted;          // per-generator MUTE (on => contributes 0).
    double tune;         // per-generator TUNE, semitones (provisional mapping).
    double modAmount;    // per-generator MOD button depth (0 => runs stably).
    double modCv;        // live CV/photo detune input (0 => no external detune).
    double volt;         // shared VOLT transpose of the whole 5-gen group (semitones).
  };

  // sampleRate must be > 0. voiceCount is clamped to [1, kMaxVoices].
  // driftEnabled=false makes the whole dynamic drift model inert (tolerance only).
  DroneBank(std::uint64_t seed, double sampleRate,
            std::size_t voiceCount = kMaxVoices, bool driftEnabled = true)
      : sampleRate_(sampleRate),
        voiceCount_(voiceCount == 0 ? 1 : (voiceCount > kMaxVoices ? kMaxVoices : voiceCount)),
        driftEnabled_(driftEnabled) {
    SeededRandom rng(seed);
    for (std::size_t i = 0; i < voiceCount_; ++i) {
      Voice& v = voices_[i];
      const Role role = roleOfIndex_(i);
      v.role = role;
      // Role band (provisional; manual says only "approximate data"). The ORDER
      // is what is structural: low < medium < high, guaranteed by the bands.
      switch (role) {
        case Role::kLow:    v.freqBaseHz = 30.0 + rng.nextUnit(0.0, 1.0) * 90.0; break;   // 30-120
        case Role::kMedium: v.freqBaseHz = 140.0 + rng.nextUnit(0.0, 1.0) * 240.0; break; // 140-380
        case Role::kHigh:   v.freqBaseHz = 420.0 + rng.nextUnit(0.0, 1.0) * 1380.0; break;// 420-1800
      }
      v.tolerance = rng.nextUnit(0.0, 0.02);
      v.amplitude = rng.nextUnit(0.05, 1.0);
      v.driftF1Hz = rng.nextUnit(0.01, 0.3);
      v.driftF2Hz = rng.nextUnit(0.01, 0.3);
      v.driftA1 = rng.nextUnit(0.0005, 0.01);
      v.driftA2 = rng.nextUnit(0.0005, 0.01);
      v.driftPh1 = rng.nextUnit(0.0, 6.283185307179586);
      v.driftPh2 = rng.nextUnit(0.0, 6.283185307179586);
      v.phase = 0.0;
      v.driftNow = driftEnabled_
                       ? v.freqBaseHz * (v.driftA1 * std::sin(v.driftPh1) + v.driftA2 * std::sin(v.driftPh2))
                       : 0.0;
      // Controls default neutral: nothing muted, no tune, MOD off, VOLT nominal.
      v.muted = false;
      v.tune = 0.0;
      v.modAmount = 0.0;
      v.modCv = 0.0;
      v.volt = 0.0;
      lastSample_[i] = 0.0;
    }
  }

  // ---------- panel CONTROL binding (structure; the runtime forwards these) ------
  void setMute(std::size_t gen, bool on) { if (gen < voiceCount_) voices_[gen].muted = on; }
  void setTune(std::size_t gen, double semitones) { if (gen < voiceCount_) voices_[gen].tune = semitones; }
  void setMod(std::size_t gen, double amount) { if (gen < voiceCount_) voices_[gen].modAmount = amount; }
  void setModCv(std::size_t gen, double cv) { if (gen < voiceCount_) voices_[gen].modCv = cv; }
  // Shared VOLT transpose: a single turn applies to all glider generators of that
  // classic voice (5-gen group). Past half-stroke the group starts mutual-FM-ing.
  void setVolt(std::size_t voiceGroup, double semitonesDown) {
    const std::size_t gs = voiceGroup * kGensPerVoice;
    const std::size_t end = std::min(voiceCount_, gs + kGensPerVoice);
    for (std::size_t i = gs; i < end; ++i) voices_[i].volt = semitonesDown;
  }

  // Advance every generator by one sample and write its sample into out[i]
  // (0 if muted). out must have room for voiceCount_ values. Realtime-safe.
  void tick(double* out) {
    for (std::size_t i = 0; i < voiceCount_; ++i) {
      Voice& v = voices_[i];
      v.driftNow = driftEnabled_
                       ? v.freqBaseHz *
                             (v.driftA1 * std::sin(driftPhase1_(v)) +
                              v.driftA2 * std::sin(driftPhase2_(v)))
                       : 0.0;
      const double tuneScale = std::pow(2.0, v.tune / 12.0);      // TUNE (semitones up).
      const double voltScale = std::pow(2.0, -v.volt / 12.0);     // VOLT transposes down.
      const double base = v.freqBaseHz * tuneScale * voltScale;
      double effFreq = base * (1.0 + v.tolerance) + v.driftNow;
      // MOD: button on => CV/photo detune scales the pitch; button off => stable.
      effFreq += v.modAmount * v.modCv;
      // Mutual FM: active only past half the VOLT stroke (manual). Pair generators
      // within a voice by a 5-ring. Provisional depth law.
      if (v.volt > kVvoltMid && fmDepth_(v.volt) > 0.0) {
        effFreq += fmDepth_(v.volt) * lastSample_[peerIndex_(i)];
      }
      if (effFreq < 0.0) effFreq = 0.0;

      const double s = v.muted ? 0.0 : v.amplitude * nonlinearity(sawtooth(v.phase));
      out[i] = s;
      lastSample_[i] = s;
      v.phase += twoPi_ * effFreq / sampleRate_;
      v.phase = std::fmod(v.phase, twoPi_);
      if (v.phase < 0.0) v.phase += twoPi_;
    }
    ++blockSample_;
  }

  // Inspectors (tests + read-only DSP supervision).
  double phaseOf(std::size_t i) const { return voices_[i].phase; }
  double driftOf(std::size_t i) const { return voices_[i].driftNow; }
  double freqBaseHz(std::size_t i) const { return voices_[i].freqBaseHz; }
  double toleranceOf(std::size_t i) const { return voices_[i].tolerance; }
  double effectiveFreqHz(std::size_t i) const {
    const Voice& v = voices_[i];
    return v.freqBaseHz * std::pow(2.0, v.tune / 12.0) * std::pow(2.0, -v.volt / 12.0) +
           v.tolerance * v.freqBaseHz + v.driftNow;
  }
  std::size_t voiceCount() const { return voiceCount_; }
  bool driftEnabled() const { return driftEnabled_; }
  Role roleOf(std::size_t i) const { return i < voiceCount_ ? voices_[i].role : Role::kLow; }
  bool mutedOf(std::size_t i) const { return voices_[i].muted; }
  double tuneOf(std::size_t i) const { return voices_[i].tune; }
  double voltOf(std::size_t i) const { return voices_[i].volt; }
  double modAmountOf(std::size_t i) const { return voices_[i].modAmount; }

  // The product waveform + nonlinearity, exposed read-only so a test can probe the
  // exact transfer the audio path applies every sample (no test-only copy). This is
  // the product path, not a shadow judge.
  static double sawtooth(double phase) { return 2.0 * (phase / twoPi_) - 1.0; }
  static double nonlinearity(double x) { return x - (1.0 / 3.0) * x * x * x; }

 private:
  static constexpr double twoPi_ = 6.283185307179586;
  static constexpr double kVvoltMid = 30.0;         // provisional: VOLT halfturn (semitones down).
  static constexpr double kFmDepthMax = 4.0;        // provisional: FM Hz at max excess.

  Role roleOfIndex_(std::size_t i) const {
    const std::size_t gen = i % kGensPerVoice;      // 0,1=low; 2=med; 3,4=high.
    if (gen <= 1) return Role::kLow;
    if (gen == 2) return Role::kMedium;
    return Role::kHigh;
  }
  std::size_t peerIndex_(std::size_t i) const {
    const std::size_t gs = (i / kGensPerVoice) * kGensPerVoice;
    const std::size_t inGroup = (i - gs);
    const std::size_t groupCount =
        std::min(kGensPerVoice, voiceCount_ - gs);  // handles partial test fixtures.
    return gs + (inGroup + 1) % groupCount;
  }
  double fmDepth_(double volt) const {
    return (volt > kVvoltMid) ? kFmDepthMax * (volt - kVvoltMid) : 0.0;
  }

  double driftPhase1_(const Voice& v) const {
    const double t = blockSample_ / sampleRate_;
    return twoPi_ * v.driftF1Hz * t + v.driftPh1;
  }
  double driftPhase2_(const Voice& v) const {
    const double t = blockSample_ / sampleRate_;
    return twoPi_ * v.driftF2Hz * t + v.driftPh2;
  }

  double sampleRate_;
  std::size_t voiceCount_;
  bool driftEnabled_;
  double blockSample_ = 0.0;
  Voice voices_[kMaxVoices];
  double lastSample_[kMaxVoices];
};

}  // namespace lunar24::core
