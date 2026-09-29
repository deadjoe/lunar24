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
//   effFreq(t) = base * tuneScale * voltScale * (1 + tolerance) + drift(t) + mod(t)
//                then x (1 + mutualFM(t)) x (1 + cycleJitter)
//   * base      — default just-intonation stack per voice (1 : 1.5 : 2 : 3 : 4 on the
//                 voice root), keeping the manual's low / medium / high roles.
//   * tolerance — STATIC seeded component error (about +-1.2 %): the slow beating.
//   * drift(t)  — slow Ornstein-Uhlenbeck random walk (a few cents over ~20 s).
//   * mod(t)    — per-generator MOD: CV/photo detune when the MOD button is on.
//   * mutualFM  — past half the VOLT stroke, each generator frequency-modulates the
//                 next one in its group (relative depth, so it never stalls).
//   * cycleJitter — the negistor's noisy firing threshold: each period differs a bit.
//
// WAVEFORM: a capacitor-charge ramp (exponentially bent sawtooth) with a band-limited
// discharge step, through a soft cubic saturation. All constants are tuned by ear.
//
// Realtime-safe: tick() never allocates or blocks; all control writes (setMute/
// setTune/setMod/setVolt) are plain field stores. SPDX.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "lunar24/core/polyblep_kernel.h"
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
    double driftState;   // slow random-walk pitch deviation (fraction of base), OU process.
    double driftDepth;   // this oscillator's drift depth (fraction), seeded.
    double shapeCurve;   // capacitor-charge curvature of the ramp, seeded (0 = straight saw).
    double cycleJitter;  // this cycle's small period deviation (fraction), redrawn each cycle.
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
  static constexpr std::uint64_t kStreamDrift = 0x44524946ULL;  // "DRIF"
  static constexpr std::uint64_t kStreamCycle = 0x4359434CULL;  // "CYCL"
  static constexpr double kDriftPullPerSecond = 1.0 / 20.0;     // drift wanders over ~20 s
  static constexpr double kCycleJitter = 0.0015;                // +-0.15 % period noise

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
      // Default tuning: each classic voice is a just-intonation stack on its own root
      // (1 : 1.5 : 2 : 3 : 4 = root, fifth, octave, twelfth, two octaves), so the low /
      // medium / high roles of the manual hold and the machine starts on a consonant
      // A-minor-pentatonic drone. The TUNE knobs move each generator from here.
      // Tuned by ear; the manual gives no frequencies.
      {
        static constexpr double kVoiceRootHz[kClassicVoices] = {55.0, 82.41, 73.42, 65.41};
        static constexpr double kGenRatio[kGensPerVoice] = {1.0, 1.5, 2.0, 3.0, 4.0};
        const std::size_t group = (i / kGensPerVoice) % kClassicVoices;
        v.freqBaseHz = kVoiceRootHz[group] * kGenRatio[i % kGensPerVoice];
      }
      // Component tolerance: each generator sits a little off its nominal pitch, which
      // gives the slow beating of a hand-tuned analog drone.
      v.tolerance = vrng.nextUnit(-0.012, 0.012);
      v.amplitude = vrng.nextUnit(0.55, 1.0);
      v.driftDepth = vrng.nextUnit(0.0015, 0.0045);   // ~3-8 cents of wander
      v.shapeCurve = vrng.nextUnit(0.8, 2.2);
      v.driftState = vrng.nextUnit(-1.0, 1.0) * v.driftDepth;
      v.cycleJitter = 0.0;
      v.phase = 0.0;
      v.driftNow = driftEnabled_ ? v.freqBaseHz * v.driftState : 0.0;
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
  //
  // tick(all) is now a thin loop over the classic groups: it calls each group's
  // narrow per-group tick in ascending group order — the same one-ModuleId-at-a-time
  // order the unified executor will use. Because the groups are independent (mutual-FM
  // peers stay within a group, no cross-group edge) and each group carries its OWN
  // sample counter in lockstep, this is bit-identical to a single all-voices tick.
  void tick(double* out) {
    for (std::size_t g = 0; g < groupCount_; ++g)
      tickGroup(static_cast<int>(g), out + g * kGensPerVoice);
  }

  // Tick ONLY classic group `group` (0..3) and write that group's generator samples into
  // out[0..kGensPerVoice) (fewer for a partial test fixture). This is the per-ModuleId
  // entry the unified executor uses: each of the four classic drone modules
  // (drone 1/2/4/5) runs exactly ITS OWN group per slot, following resolve -> tick ->
  // publish, rather than "one tick whole bank" pretending a single plan node. Each group
  // owns an independent sample counter (groupSample_), so per-group jitter/drift/env
  // phase advance exactly as the old single blockSample_ did — bit-identical, in any
  // group execution order. Realtime-safe.
  void tickGroup(int group, double* out) {
    const std::size_t g = static_cast<std::size_t>(group);
    if (group < 0 || g >= groupCount_) return;
    // Advance THIS group's gate/ATT/RLS/HOLD VCA gain toward its target (open =
    // gate||hold — the PROVISIONAL HOLD keeps the target open while held). The
    // oscillators keep free-running; the envelope only scales the group's final audio
    // below, never resetting phase (design/07 §7). Linear + monotonic.
    const double dt = 1.0 / sampleRate_;
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
    const double sample = groupSample_[g];   // this group's own sample counter.
    const double gLvl = e.level;             // group VCA gain 0..1 (advanced above).
    const std::size_t begin = g * kGensPerVoice;
    const std::size_t end = std::min(begin + kGensPerVoice, voiceCount_);
    for (std::size_t i = begin; i < end; ++i) {
      Voice& v = voices_[i];
      if (driftEnabled_) {
        // Ornstein-Uhlenbeck random walk: wanders slowly (tens of seconds) and is pulled
        // back toward the nominal pitch. Deterministic per (seed, generator, sample).
        const double u = jitterUnit_(seed_ ^ kStreamDrift, static_cast<double>(i), sample);
        v.driftState += -kDriftPullPerSecond * v.driftState * dt +
                        v.driftDepth * std::sqrt(2.0 * kDriftPullPerSecond * dt) * u * 1.732;
        v.driftNow = v.freqBaseHz * v.driftState;
      } else {
        v.driftNow = 0.0;
      }
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
      // recompute.
      lastJitterHz_[i] = kOscNoiseAmpHz * jitterUnit_(seed_, static_cast<double>(i), sample);
      effFreq += lastJitterHz_[i];
      // Mutual FM: active only past half the VOLT stroke (manual). Pair generators
      // within a voice by a 5-ring. Provisional depth law.
      // The depth is relative to the generator's own pitch, so a deep FM swings the
      // frequency between ~0.1x and ~1.9x and can never stall the oscillator ring.
      if (v.volt > kVvoltMid && fmDepth_(v.volt) > 0.0) {
        effFreq *= 1.0 + fmDepth_(v.volt) * std::max(-1.0, std::min(1.0, lastSample_[peerIndex_(i)]));
      }
      if (effFreq < 0.0) effFreq = 0.0;

      // task#110 (GH#19 S2): the classic sawtooth is a VALUE jump, so it is band-limited with
      // the polyBLEP family (core/polyblep_kernel.h), not BLAMP -- a sloped kernel leaves a
      // value jump in place. `phaseInc` is the SAME increment the accumulator takes two lines
      // below, so the correction windows sit exactly on the rollover this sample is about to
      // cross. `dt` is deliberately NOT reused: drone_bank.h:258 already names the ENVELOPE
      // time step `dt`, and the two are different quantities by five orders of magnitude.
      // polyblepSaw is a pure function of (t, phaseInc) -- it does NOT touch v.phase, so the
      // phase trajectory (and the pinned-phi comparison) is unchanged (@Kimi 69b64ff6 pin 2).
      // Outside +/-phaseInc the residual is exactly 0, so this is bit-identical to the naive
      // ramp everywhere else; measured on the emitted signal the correction moves at most 2
      // consecutive samples per generator (scratch/s2_integration_preview.txt).
      effFreq *= 1.0 + v.cycleJitter;
      const double phaseInc = effFreq / sampleRate_;
      const double t = v.phase / twoPi_;
      // Negistor relaxation: the capacitor charges along an exponential curve and then
      // discharges almost at once. The curve bends the ramp; the band-limited discharge
      // step is the same polyBLEP-corrected jump a sawtooth has (the curve itself is
      // continuous across the wrap because shape(0)=0 and shape(1)=1).
      // chargeShape bends the ramp upward, so remove its mean (the curve's DC offset).
      const double bent = polyblepSaw(t, phaseInc) +
                          2.0 * (chargeShape(t, v.shapeCurve) - t - chargeShapeMeanOffset(v.shapeCurve));
      const double s = v.muted ? 0.0 : v.amplitude * nonlinearity(bent);
      lastSample_[i] = s;          // RAW pre-group-VCA: mutual-FM peers use the oscillator value.
      out[i - begin] = s * gLvl;   // the group's envelope/VCA gates the final audio.
      v.phase += twoPi_ * effFreq / sampleRate_;
      if (driftEnabled_ && v.phase >= twoPi_) {
        // New cycle: the firing threshold is noisy, so each period differs a little.
        v.cycleJitter = kCycleJitter * jitterUnit_(seed_ ^ kStreamCycle, static_cast<double>(i), sample);
      }
      v.phase = std::fmod(v.phase, twoPi_);
      if (v.phase < 0.0) v.phase += twoPi_;
    }
    ++groupSample_[g];
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
  // Normalised capacitor-charge curve on [0,1): shape(0)=0, shape(1)=1, concave for k>0.
  static double chargeShape(double t, double k) {
    if (k < 1e-6) return t;
    return (1.0 - std::exp(-k * t)) / (1.0 - std::exp(-k));
  }
  // Mean of (chargeShape(t,k) - t) over one cycle.
  static double chargeShapeMeanOffset(double k) {
    if (k < 1e-6) return 0.0;
    const double e = std::exp(-k);
    return (1.0 - (1.0 - e) / k) / (1.0 - e) - 0.5;
  }

  // ---- centralized, PROVISIONAL, monotonic norm→seconds mappings (design/07: no
  // hardware value claimed; a larger normalized knob always yields a longer stage) ----
  // PUBLIC so the Papa Srapa voice AR envelope (drone_3/drone_6) consumes the SAME
  // mapping and the SAME kAtt/kRlsNormMin/MaxSeconds constants as the classic groups
  // instead of carrying a second, drifting copy. There is exactly ONE source of the
  // mapping in the product; ATT and RLS share the same shape but separate ranges so the
  // two stages never share state.
  static double mapAttSeconds(double norm) {
    return kAttNormMinSeconds + clamp01_(norm) * (kAttNormMaxSeconds - kAttNormMinSeconds);
  }
  static double mapRlsSeconds(double norm) {
    return kRlsNormMinSeconds + clamp01_(norm) * (kRlsNormMaxSeconds - kRlsNormMinSeconds);
  }

 private:
  static constexpr double twoPi_ = 6.283185307179586;
  static constexpr double kVvoltMid = 30.0;         // provisional: VOLT halfturn (semitones down).
  static constexpr double kFmDepthPerSemitone = 0.06;  // relative FM depth per semitone past half (by ear).
  static constexpr double kFmDepthMax = 0.9;           // never reaches 1: the pitch stays > 0.

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
    return (volt > kVvoltMid) ? std::min(kFmDepthMax, kFmDepthPerSemitone * (volt - kVvoltMid)) : 0.0;
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
  double groupSample_[kMaxGroups] = {};  // per-classic-group sample counter (bit-exact per-group tick).
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
