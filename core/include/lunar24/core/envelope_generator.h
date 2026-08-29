// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// EnvelopeGenerator — the Envelope A / Envelope B ADSR generator (GH #11, P3
// item 6 "patchable control source"). This is a NEW, independent framework-
// agnostic sound-core unit; it deliberately does NOT inherit or wrap
// EnvelopeFollower (envelope_follower.h), which is a preamp-related L2 detector/
// control source with its own rectifying one-pole behaviour and a -10..+10V CV
// rail. The EG shares only the pure-math seconds->coefficient convention, and is
// otherwise an independent per-sample ADSR/HOLD/SELF-GEN state machine.
//
// Evidence boundary (see design/06 P3 item 6): manual L385-408 confirms the
// A/D/S/R phase semantics, HOLD == "VCA always open", and SELF-GEN makes the
// envelope run like an LFO; OUTS L156 confirms the ENV output is 0..8V. The
// ATT/DEC/RLS exact curve and real seconds, the HOLD/SELF-GEN transfer, and the
// VCA-CV voltage transfer are all UNMEASURED. Everything below marked PROVISIONAL
// is a deliberate, centralized modeling choice — the tests pin behaviour / trend /
// determinism, they do not fake a measured hardware curve.
//
// Input contract: the generator consumes the ALREADY-INTERPRETED gate LEVEL
// (a bool produced by sink_gate_interpret() against the real sink JackDescriptor,
// see sink_interpret.h). It never reads raw gate volts and does not hardcode a
// threshold/hysteresis — any future CV->gate path flows through the canonical
// sink semantics. Framework-free, fixed state, no heap, no locks.

#pragma once

#include <cmath>
#include <cstdint>

namespace lunar24::core {

// ---------------------------------------------------------------------------
// PROVISIONAL modelling constants. Centrally named so the public outputs and the
// tests reference the SAME value, and so a future hardware measurement only has
// to relabel these (never re-derive a curve elsewhere).
// ---------------------------------------------------------------------------

// ENV output peak. CONFIRMED (manual OUTS L156: ENV output is 0..8V, unipolar).
inline constexpr double kEnvelopeEnvPeakVolt = 8.0;

// VCA-CV output open ("always open" under HOLD) voltage. PROVISIONAL: the
// vca_cv_out descriptor is 0..8V with polarity unknown and everything unverified
// (ref L87); this uses the upper rail bound as the open value but makes no claim
// that the real hardware open voltage is exactly 8V — that transfer is unmeasured.
inline constexpr double kEnvelopeVcaOpenVolt = 8.0;

// Snap epsilon used to decide a one-pole segment has effectively reached its
// asymptote (attack->1.0, decay->sustain, release->0.0). PROVISIONAL: chosen small
// enough to be inaudible and to keep the monotonic/trend tests honest; it is a
// modelling tolerance, not a hardware constant.
inline constexpr double kEnvelopeSnap = 1e-4;

// ---------------------------------------------------------------------------
// EnvelopeGenerator
// ---------------------------------------------------------------------------
class EnvelopeGenerator {
 public:
  // Dynamic phase of the ADSR machine. The SELF-GEN self-oscillation reuses the
  // attack(rise)/release(fall) stages rather than introducing a distinct phase.
  enum class Phase : std::uint8_t {
    idle = 0,
    attack = 1,
    decay = 2,
    sustain = 3,
    release = 4,
  };

  EnvelopeGenerator() = default;
  explicit EnvelopeGenerator(double sampleRate) { setSampleRate(sampleRate); }

  // --- configuration (NOT cleared by reset()) -----------------------------
  // sampleRate must be set before the generator is ticked. Until then the
  // coefficients are 1.0 (instant) — deterministic, never a NaN; there is NO
  // fixed 48 kHz default (the DSP is honoured identically at 44.1/48/88.2/96k).
  void setSampleRate(double sr) {
    if (sr_ != sr) {
      sr_ = sr;
      recomputeCoefficients();
    }
  }
  void setAttackSeconds(double s) {
    if (attack_ != s) {
      attack_ = s;
      recomputeCoefficients();
    }
  }
  void setDecaySeconds(double s) {
    if (decay_ != s) {
      decay_ = s;
      recomputeCoefficients();
    }
  }
  void setReleaseSeconds(double s) {
    if (release_ != s) {
      release_ = s;
      recomputeCoefficients();
    }
  }
  void setSustain(double norm) { sustain_ = clamp01(norm); }
  void setHold(bool on) { hold_ = on; }
  void setSelfGen(bool on) {
    if (selfGen_ == on) return;
    selfGen_ = on;
    if (on) {
      // From the current level, continuous rise. No arbitrary clear.
      selfGenRising_ = true;
    } else {
      // Back to ADSR per the CURRENT real gate state; level is preserved.
      phase_ = gateLatch_ ? Phase::attack : Phase::release;
    }
  }

  double sampleRate() const { return sr_; }
  double attackSeconds() const { return attack_; }
  double decaySeconds() const { return decay_; }
  double releaseSeconds() const { return release_; }
  double sustain() const { return sustain_; }
  bool hold() const { return hold_; }
  bool selfGen() const { return selfGen_; }

  // --- reset(): clears dynamic state / level / gate latch, keeps config -----
  void reset() {
    level_ = 0.0;
    phase_ = Phase::idle;
    gateLatch_ = false;
    selfGenRising_ = true;
  }

  // --- per-sample process ------------------------------------------------
  // `gateHigh` is the ALREADY-INTERPRETED gate level (bool) from the canonical
  // sink gate interpreter, not a raw voltage. Edge detection is internal.
  void tick(bool gateHigh) {
    const bool rising = gateHigh && !gateLatch_;
    const bool falling = !gateHigh && gateLatch_;
    gateLatch_ = gateHigh;

    if (selfGen_) {
      // SELF-GEN: a self-oscillation using ONLY the Attack rise and Release fall
      // (like an LFO); Decay and Sustain are not involved. PROVISIONAL: the exact
      // waveform / phase / gate interaction is unverified — this is a minimal,
      // deterministic model, not a decoded hardware curve.
      if (selfGenRising_) {
        level_ += attackCoeff_ * (1.0 - level_);
        if (1.0 - level_ < kEnvelopeSnap) {
          level_ = 1.0;
          selfGenRising_ = false;
        }
      } else {
        level_ -= releaseCoeff_ * level_;
        if (level_ < kEnvelopeSnap) {
          level_ = 0.0;
          selfGenRising_ = true;
        }
      }
      return;
    }

    // ADSR: preempt with edges so a transition takes effect THIS sample and is
    // continuous — a gate dropping during Attack, and a gate rising during
    // Release, change direction from the current level, never jump.
    if (falling) {
      switch (phase_) {
        case Phase::attack:
        case Phase::decay:
        case Phase::sustain:
          phase_ = Phase::release;
          break;
        case Phase::release:
        case Phase::idle:
          break;
      }
    } else if (rising) {
      if (phase_ == Phase::release || phase_ == Phase::idle) {
        phase_ = Phase::attack;
      }
    }

    switch (phase_) {
      case Phase::idle:
        level_ = 0.0;
        break;
      case Phase::attack:
        level_ += attackCoeff_ * (1.0 - level_);
        if (1.0 - level_ < kEnvelopeSnap) {
          level_ = 1.0;
          phase_ = Phase::decay;
        }
        break;
      case Phase::decay:
        level_ += decayCoeff_ * (sustain_ - level_);
        if (std::fabs(level_ - sustain_) < kEnvelopeSnap) {
          level_ = sustain_;
          phase_ = Phase::sustain;
        }
        break;
      case Phase::sustain:
        level_ = sustain_;
        break;
      case Phase::release:
        level_ -= releaseCoeff_ * level_;
        if (level_ < kEnvelopeSnap) {
          level_ = 0.0;
          phase_ = Phase::idle;
        }
        break;
    }
  }

  // --- outputs ------------------------------------------------------------
  // ENV output: confirmed virtual volts, 0..8V, finite and bounded in any state.
  double envVolts() const { return level_ * kEnvelopeEnvPeakVolt; }

  // VCA CV output: a SEPARATE output (not an ENV alias). With HOLD off it tracks
  // the envelope level scaled by the (provisional) open voltage; with HOLD on it
  // stays at the open voltage while the patchable ENV continues its real
  // ADSR/SELF-GEN. The EG state is never reset/retriggered by a HOLD change.
  double vcaCvVolts() const {
    return hold_ ? kEnvelopeVcaOpenVolt : level_ * kEnvelopeVcaOpenVolt;
  }

  // --- read-only state introspection (for strong behaviour tests) ----------
  Phase phase() const { return phase_; }
  double level01() const { return level_; }
  bool gateLatch() const { return gateLatch_; }
  double attackCoeff() const { return attackCoeff_; }
  double releaseCoeff() const { return releaseCoeff_; }

 private:
  static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

  // One-pole seconds->coefficient convention (shared with envelope_follower.h as
  // pure math only). Contributes the single, centralised PROVISIONAL curve model:
  // each ADSR segment moves toward its target by `a*(target - level)` per sample.
  // A zero/negative seconds clamp makes the step 1.0 (instant) — never a 1/0 NaN.
  static double coeffSeconds(double sr, double sec) {
    return (sr > 0.0 && sec > 0.0) ? (1.0 - std::exp(-1.0 / (sr * sec))) : 1.0;
  }

  void recomputeCoefficients() {
    attackCoeff_ = coeffSeconds(sr_, attack_);
    decayCoeff_ = coeffSeconds(sr_, decay_);
    releaseCoeff_ = coeffSeconds(sr_, release_);
  }

  // Configuration (persists across reset()).
  double sr_ = 0.0;
  double attack_ = 0.01;   // seconds, PROVISIONAL (registry default, unverified)
  double decay_ = 0.1;     // seconds, PROVISIONAL (registry default, unverified)
  double release_ = 0.1;   // seconds, PROVISIONAL (registry default, unverified)
  double sustain_ = 0.5;   // normalized 0..1, PROVISIONAL (registry default)
  bool hold_ = false;      // HOLD: VCA always open (PROVISIONAL default = off)
  bool selfGen_ = false;   // SELF-GEN: LFO-like self-oscillation (default = off)

  // Derived coefficients (recomputed on config/sample-rate change).
  double attackCoeff_ = 1.0;
  double decayCoeff_ = 1.0;
  double releaseCoeff_ = 1.0;

  // Dynamic state (cleared by reset()).
  double level_ = 0.0;              // normalized envelope level 0..1
  Phase phase_ = Phase::idle;
  bool gateLatch_ = false;          // previous interpreted gate level (edge detector)
  bool selfGenRising_ = true;       // SELF-GEN self-oscillation direction
};

}  // namespace lunar24::core
