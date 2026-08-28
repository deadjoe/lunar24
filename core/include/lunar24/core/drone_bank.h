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

#include <algorithm>
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

  // ---- CLASSIC group gate/ATT/RLS/HOLD envelope + dynamic variation (batch 4A) ----
  // Each classic voice is a 5-generator GROUP with an INDEPENDENT gate/ATT/RLS/HOLD
  // envelope that VCA-gates the group's final audio. The oscillators keep free-running:
  // the envelope NEVER resets phase (design/07 §7 — envelope out, oscillators run on).
  // GATE/HOLD in the registry is a panel control; ATT/RLS are a NORMALIZED 0..1. This
  // is the single, named, PROVISIONAL monotonic map from that norm to a time range, so
  // the time constants are not scattered as magic numbers. HOLD is PROVISIONAL policy:
  // on => keep the group gate target open; off => restore the real gate state (the
  // exact hardware state/transfer is unverified; see FINDINGS).
  static constexpr std::size_t kMaxGroups = kClassicVoices;  // 4 classic voices.
  static constexpr double kAttNormMinSeconds = 0.001;        // provisional, monotonic.
  static constexpr double kAttNormMaxSeconds = 1.0;
  static constexpr double kRlsNormMinSeconds = 0.001;
  static constexpr double kRlsNormMaxSeconds = 1.0;
  static constexpr double kDefaultAttNorm = 0.0;             // neutral fast default.
  static constexpr double kDefaultRlsNorm = 0.0;
  // PROVISIONAL per-generator deterministic jitter amplitude (Hz). Kept small so it is
  // a dynamic-variation term, never the dominant pitch, and never perturbs the
  // structure tests' period-uniformity checks. NOT std::random and NOT a fixed sine: a
  // counter-based deterministic hash (see jitterUnit_).
  static constexpr double kOscNoiseAmpHz = 0.02;  // provisional: small jitter (Hz).

  // Centralized dynamic-variation MODEL VERSION (batch 4A convergence, @Codex 52d3c620).
  // This is not decorative: every tolerance/drift constant AND the per-sample jitter are
  // derived from it via deriveSeed_ (below), so bumping the version is a real, spec-visible
  // regeneration of the whole dynamic model — not a no-op. Persistence stays #12.
  static constexpr std::uint64_t kDynamicModelVersion = 1;
  // Independent stream/domain tags: the oscillator-constants stream and the per-sample
  // jitter stream must not alias, so a single (seed, unit) can't collide across domains.
  // They are arbitrary non-zero constants, not meanings.
  static constexpr std::uint64_t kStreamOsc = 0x4F534356ULL;    // "OSCV"
  static constexpr std::uint64_t kStreamJitter = 0x4A495454ULL; // "JITT"

  // Named PROVISIONAL default for the group gate: a host that never touches the gate
  // hears the voice (the pre-batch structure tests probe the raw bank). The default is
  // NOT hardware evidence — the behaviour tests set the gate explicitly before asserting
  // the envelope/open transition (@Codex 52d3c620 point 6). Keep it explicit + named.
  static constexpr bool kDefaultGroupGateOpen = true;

  // Per-classic-group envelope state. `level` is the 0..1 VCA gain applied to the
  // group's summed audio every sample. ORDINARY default is OPEN (gate on, level 1) so a
  // neutral bank still sounds — the pre-batch structure tests probe the frequency model;
  // the host closes the gate to silence a voice. All time values are the PROVISIONAL
  // norm-mapped seconds (never a claimed hardware constant).
  struct GroupEnv {
    bool gate = kDefaultGroupGateOpen;  // external gate input (named provisional default).
    bool hold = false;    // PROVISIONAL HOLD: on => keep the gate target open.
    double attSeconds = kAttNormMinSeconds;  // mapped from norm at set.
    double rlsSeconds = kRlsNormMinSeconds;
    double level = 1.0;   // current VCA gain 0..1 (open at start).
  };

  // sampleRate must be > 0. voiceCount is clamped to [1, kMaxVoices].
  // driftEnabled=false makes the whole dynamic drift model inert (tolerance only).
  DroneBank(std::uint64_t seed, double sampleRate,
            std::size_t voiceCount = kMaxVoices, bool driftEnabled = true)
      : sampleRate_(sampleRate),
        voiceCount_(voiceCount == 0 ? 1 : (voiceCount > kMaxVoices ? kMaxVoices : voiceCount)),
        driftEnabled_(driftEnabled),
        seed_(seed) {
    groupCount_ = (voiceCount_ + kGensPerVoice - 1) / kGensPerVoice;
    for (std::size_t g = 0; g < kMaxGroups; ++g) {
      groupEnv_[g].gate = kDefaultGroupGateOpen;  // named provisional default (not evidence).
      groupEnv_[g].hold = false;
      groupEnv_[g].attSeconds = mapAttSeconds(kDefaultAttNorm);
      groupEnv_[g].rlsSeconds = mapRlsSeconds(kDefaultRlsNorm);
      groupEnv_[g].level = 1.0;  // neutral default OPEN (pre-batch tests expect sound).
      modCvG_[g] = 0.0;
    }
    environmentHz_ = 0.0;
    for (std::size_t i = 0; i < voiceCount_; ++i) {
      Voice& v = voices_[i];
      const Role role = roleOfIndex_(i);
      v.role = role;
      // Each generator is its own derivation UNIT: a per-unit stream seeded from
      // (base seed, unit index, kStreamOsc) mixed with kDynamicModelVersion. Same seed +
      // same unit => identical constants; a different unit or version => a different
      // stream (deriveSeed_ avalanches, and the streams are independent from the jitter
      // stream below by domain). The per-voice draw keeps the structure tests' ROLE
      // bands and value ranges unchanged (provisional bands/orders are structural).
      SeededRandom vrng(deriveSeed_(seed, static_cast<std::uint64_t>(i), kStreamOsc));
      // Role band (provisional; manual says only "approximate data"). The ORDER
      // is what is structural: low < medium < high, guaranteed by the bands.
      switch (role) {
        case Role::kLow:    v.freqBaseHz = 30.0 + vrng.nextUnit(0.0, 1.0) * 90.0; break;   // 30-120
        case Role::kMedium: v.freqBaseHz = 140.0 + vrng.nextUnit(0.0, 1.0) * 240.0; break; // 140-380
        case Role::kHigh:   v.freqBaseHz = 420.0 + vrng.nextUnit(0.0, 1.0) * 1380.0; break;// 420-1800
      }
      v.tolerance = vrng.nextUnit(0.0, 0.02);
      v.amplitude = vrng.nextUnit(0.05, 1.0);
      v.driftF1Hz = vrng.nextUnit(0.01, 0.3);
      v.driftF2Hz = vrng.nextUnit(0.01, 0.3);
      v.driftA1 = vrng.nextUnit(0.0005, 0.01);
      v.driftA2 = vrng.nextUnit(0.0005, 0.01);
      v.driftPh1 = vrng.nextUnit(0.0, 6.283185307179586);
      v.driftPh2 = vrng.nextUnit(0.0, 6.283185307179586);
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
      lastJitterHz_[i] = 0.0;  // "no jitter applied yet" (nothing has been ticked).
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

  // ---- CLASSIC group gate/ATT/RLS/HOLD + shared CV MOD + environment (batch 4A) ----
  void setGroupGate(int group, bool on) { if (inGroup_(group)) groupEnv_[group].gate = on; }
  void setGroupHold(int group, bool on) { if (inGroup_(group)) groupEnv_[group].hold = on; }
  // ATT/RLS come from the registry as a normalized 0..1 control. The ONLY mapping in
  // the bank is mapAttSeconds/mapRlsSeconds (named, PROVISIONAL, monotonic): tests
  // clamp the norm and assert the monotonic + speed trend, never a hardware value.
  void setGroupAtt(int group, double norm) { if (inGroup_(group)) groupEnv_[group].attSeconds = mapAttSeconds(norm); }
  void setGroupRls(int group, double norm) { if (inGroup_(group)) groupEnv_[group].rlsSeconds = mapRlsSeconds(norm); }
  // Shared CV MOD / photo-detector input for the whole group, in RAW virtual volts from the
  // runtime's CV source bank (the value resolved at the group's cv_mod_in patch jack), NOT a
  // normalized 0..1 upstream scale. Applied only to generators whose MOD button is on (the
  // existing modAmount gate); MOD-off generators ignore CV and environment (design/07 §7).
  void setGroupModCv(int group, double cv) { if (inGroup_(group)) modCvG_[group] = cv; }
  // Shared/correlated environment term a desktop host can provide (design/07 §7). It
  // detunes MOD-on generators together; MOD-off generators are unchanged.
  void setEnvironment(double hz) { environmentHz_ = hz; }

  // Advance every generator by one sample and write its sample into out[i]
  // (0 if muted). out must have room for voiceCount_ values. Realtime-safe.
  void tick(double* out) {
    // Advance each classic group's gate/ATT/RLS/HOLD VCA gain toward its target
    // (open = gate||hold — the PROVISIONAL HOLD keeps the target open while held). The
    // oscillators keep free-running; the envelope only scales the group's final audio
    // below, never resetting phase (design/07 §7). Linear + monotonic, so a larger
    // ATT/RLS makes the corresponding stage slower.
    const double dt = 1.0 / sampleRate_;
    for (std::size_t g = 0; g < groupCount_; ++g) {
      GroupEnv& e = groupEnv_[g];
      const double target = (e.gate || e.hold) ? 1.0 : 0.0;
      double& lvl = e.level;
      if (lvl < target) {
        lvl += dt / e.attSeconds;
        if (lvl > target) lvl = target;
      } else if (lvl > target) {
        lvl -= dt / e.rlsSeconds;
        if (lvl < target) lvl = target;
      }
    }

    for (std::size_t i = 0; i < voiceCount_; ++i) {
      Voice& v = voices_[i];
      const std::size_t g = i / kGensPerVoice;   // classic group (0..3).
      const double gLvl = groupEnv_[g].level;    // group VCA gain 0..1.
      v.driftNow = driftEnabled_
                       ? v.freqBaseHz *
                             (v.driftA1 * std::sin(driftPhase1_(v)) +
                              v.driftA2 * std::sin(driftPhase2_(v)))
                       : 0.0;
      const double tuneScale = std::pow(2.0, v.tune / 12.0);      // TUNE (semitones up).
      const double voltScale = std::pow(2.0, -v.volt / 12.0);     // VOLT transposes down.
      const double base = v.freqBaseHz * tuneScale * voltScale;
      double effFreq = base * (1.0 + v.tolerance) + v.driftNow;
      // MOD: button on => the shared CV MOD (group) or per-gen CV detunes; off => stable.
      effFreq += v.modAmount * (v.modCv + modCvG_[g]);
      // Environment: shared/correlated term detunes MOD-on generators together; MOD-off
      // generators are unchanged (design/07 §7 — the MOD-off generator ignores CV/env).
      if (v.modAmount > 0.0) effFreq += environmentHz_;
      // Oscillator-specific small deterministic jitter (per-gen, seed-stable, hash-based).
      // Save the value actually applied THIS SAMPLE first, so the inspector reads exactly
      // what entered the frequency accumulation (lastJitterHz_), never the next sample's
      // recompute (the OLD inspector read blockSample_ after it was incremented, i.e. the
      // wrong sample — see the jitter-source negative control).
      lastJitterHz_[i] = kOscNoiseAmpHz * jitterUnit_(seed_, static_cast<double>(i), blockSample_);
      effFreq += lastJitterHz_[i];
      // Mutual FM: active only past half the VOLT stroke (manual). Pair generators
      // within a voice by a 5-ring. Provisional depth law.
      if (v.volt > kVvoltMid && fmDepth_(v.volt) > 0.0) {
        effFreq += fmDepth_(v.volt) * lastSample_[peerIndex_(i)];
      }
      if (effFreq < 0.0) effFreq = 0.0;

      const double s = v.muted ? 0.0 : v.amplitude * nonlinearity(sawtooth(v.phase));
      lastSample_[i] = s;   // RAW pre-group-VCA: mutual-FM peers use the oscillator value.
      out[i] = s * gLvl;    // the group's envelope/VCA gates the final audio.
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

  // ---- batch 4A envelope / dynamic-variation inspectors (read-only executed state) ----
  std::size_t groupCount() const { return groupCount_; }
  // Current group VCA gain 0..1 (the value the product multiplies the group's audio by).
  double groupEnvLevel(int group) const { return inGroup_(group) ? groupEnv_[group].level : 0.0; }
  bool groupGate(int group) const { return inGroup_(group) && groupEnv_[group].gate; }
  bool groupHold(int group) const { return inGroup_(group) && groupEnv_[group].hold; }
  double groupAttSeconds(int group) const { return inGroup_(group) ? groupEnv_[group].attSeconds : 0.0; }
  double groupRlsSeconds(int group) const { return inGroup_(group) ? groupEnv_[group].rlsSeconds : 0.0; }
  // The LAST-APPLIED per-generator deterministic jitter (Hz) the product added to this
  // generator's frequency (0 before the first tick). Product-executed value (like
  // driftOf/phaseOf), never a shadow mirror, and specifically never recomputed from the
  // (already-incremented) sample counter. Not std::random and not a fixed sine (see
  // jitterUnit_).
  double noiseJitterHz(std::size_t i) const { return i < voiceCount_ ? lastJitterHz_[i] : 0.0; }
  double environmentHzTerm() const { return environmentHz_; }
  double groupModCv(int group) const { return inGroup_(group) ? modCvG_[group] : 0.0; }
  int groupOfGen(std::size_t i) const { return static_cast<int>(i / kGensPerVoice); }

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

  // ---- batch 4A: group gate/ATT/RLS/HOLD + CV MOD + environment helpers ----
  bool inGroup_(int group) const {
    return group >= 0 && static_cast<std::size_t>(group) < groupCount_;
  }
  static double clamp01_(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
  }
  // Centralized, PROVISIONAL, monotonic norm→seconds mappings (design/07: no hardware
  // value claimed; a larger normalized knob always yields a longer stage). ATT and RLS
  // share the same shape but separate ranges so the two stages never share state.
  static double mapAttSeconds(double norm) {
    return kAttNormMinSeconds + clamp01_(norm) * (kAttNormMaxSeconds - kAttNormMinSeconds);
  }
  static double mapRlsSeconds(double norm) {
    return kRlsNormMinSeconds + clamp01_(norm) * (kRlsNormMaxSeconds - kRlsNormMinSeconds);
  }
  // Splitmix64-style finalizer (pure, stateless). Same construction SeededRandom uses,
  // but applied as a PURE function of (seed, generator, absolute-sample) rather than a
  // stateful per-sample stream — so the small jitter is block-partition deterministic,
  // never a shared value across generators, and has STATISTICAL EXPECTATION center ≈ 0
  // (does not bias a generator's average pitch). It is NOT exactly zero-mean per cycle:
  // each unit's value is a hash of (seed, version, unit, domain, sample), so over one
  // cycle the mean is ~0 but not identical to zero, and it is a deterministic hash, not
  // a sine or a sampled random. This keeps the P3-① "no hidden randomness" discipline.
  static std::uint64_t mix64_(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }
  // Derive a fresh, independent stream seed from (base seed, unit, domain) folded with the
  // model version. Distinct (unit, domain, version) triples give distinct streams; the same
  // triple is reproducible, so a bank and a same-seed peer stay bit-identical. This is what
  // makes kDynamicModelVersion genuinely participate (a version bump regenerates every
  // derived tolerance/drift AND the whole jitter field) rather than being a dead constant.
  static std::uint64_t deriveSeed_(std::uint64_t base, std::uint64_t unit,
                                   std::uint64_t domain) {
    return mix64_(base ^ (unit * 0x9E3779B97F4A7C15ULL) ^
                  (domain * 0xD6E8FEB86659FD93ULL) ^
                  (kDynamicModelVersion * 0xA24BAED4963EE407ULL));
  }
  // Per-generator, per-sample jitter in [-1, 1). `blockSample` is the integer sample
  // counter (always integral in practice), so it keys a unique hash per sample. The
  // jitter runs on its OWN domain stream (kStreamJitter), independent of the oscillator
  // constants, and folds the model version in via deriveSeed_.
  static double jitterUnit_(std::uint64_t seed, double gen, double blockSample) {
    const std::uint64_t h =
        mix64_(deriveSeed_(seed, static_cast<std::uint64_t>(gen), kStreamJitter) ^
               (static_cast<std::uint64_t>(blockSample) * 0x94D049BB133111EBULL));
    return (static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
  }

  double sampleRate_;
  std::size_t voiceCount_;
  bool driftEnabled_;
  double blockSample_ = 0.0;
  Voice voices_[kMaxVoices];
  double lastSample_[kMaxVoices];
  double lastJitterHz_[kMaxVoices] = {};  // last-APPLIED per-gen jitter (see noiseJitterHz).

  // ---- batch 4A state (classic group gate/ATT/RLS/HOLD + CV MOD + environment) ----
  std::uint64_t seed_ = 0;
  std::size_t groupCount_ = 0;
  GroupEnv groupEnv_[kMaxGroups];
  double modCvG_[kMaxGroups];
  double environmentHz_ = 0.0;
};

}  // namespace lunar24::core
