// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// EnvelopeFollower — P3-④: the PREAMP's envelope follower (manual L551-558). It
// rectifies the preamplified source, smooths it with an asymmetric one-pole
// (independent attack / release), and derives an ENVELOPE CV output plus a GATE
// output. Signal chain: PIEZZO → PREAMP → ENVELOPE FOLLOWER (design/01 §1, §3).
//
// PROVENANCE (the frozen registry, generated/lunar24/registry.hpp, is the
// implementation basis; every claim below mirrors it and marks evidence strength):
//
//   * ATTACK / RELEASE (env_follower.attack id 158, .release id 159) — unit "norm",
//     0..1, default 0.5. ALL ParameterFieldEvidence are UNVERIFIED. The manual
//     (L551, L552) writes only "envelope follower with attack and release"; NO time
//     values are given. The core therefore works in SECONDS (design/07 §5: "时间常数
//     用秒", 时间常数 = time constants in seconds; §4: "时间常数以秒为单位"). The
//     registry's NORMALIZED knob is a control/UI mapping between the panel and this
//     core, resolved OUTSIDE this header as a PROVISIONAL curve — the DSP core
//     carries the physical seconds. This is what the cross-sr must-test exercises.
//   * ENVELOPE CV output (env_out id 34) — cv output, unipolar, nominal 0..+10.0 V
//     CONFIRMED (design/07 §2 voltage spec: "ENV FOLLOWER CV 0…10 V"). The follower's
//     internal value is clamped to this range at the output.
//   * GATE output (gate_out id 35) — gate output, unipolar, nominal 0..+8.0 V
//     CONFIRMED (design/07 §2: "GATE 0…8 V"). Active gate = +8 V, inactive = 0 V.
//   * GATE DETECTOR: the manual shows a "gate detector activity" indicator (L557-558)
//     but gives NO trigger level or hysteresis. kGateThreshold and kGateHysteresis
//     are PROVISIONAL. The gate is based on the SMOOTHED envelope level — the
//     standard "activity" measure — with a threshold and a hysteresis margin back; a
//     documented modeling choice (an alternative on the raw rectified transient would
//     trigger faster on attack, which the manual does not evidence either way).
//   * Rectify-then-smooth is the DSL structure (见 P3-② Schmitt/FM which also
//     split to new headers rather than reusing parameter_smoothing.h). The
//     seconds->one-pole coefficient mapping is REUSED from the shared convention
//     (a = 1 - exp(-1/(sr * tau))), but the rectifier + asymmetric A/R + gate
//     detector constitute a NEW structure, so this is a deliberate new-write.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// Envelope follower producing an ENVELOPE CV (0..+10 V) and a GATE (0..+8 V) from
// a rectified source. Attack and release are in seconds.
class EnvelopeFollower {
 public:
  explicit EnvelopeFollower(double sampleRate) { setSampleRate(sampleRate); }

  // ---------------------------------------------------------------- configure --
  void setSampleRate(double sr) {
    if (sr > 0.0) sr_ = sr;
    recomputeCoefficients_();
  }

  // Seconds (design/07 §5). Clamped to >= kMinTime. The registry's normalized
  // attack/release knob maps onto these seconds via a PROVISIONAL control curve.
  void setAttackSeconds(double s) {
    attackSec_ = s < kMinTime ? kMinTime : s;
    recomputeCoefficients_();
  }
  void setReleaseSeconds(double s) {
    releaseSec_ = s < kMinTime ? kMinTime : s;
    recomputeCoefficients_();
  }

  // ----------------------------------------------------------------- render --
  // Advance one sample with the rectified |in| voltage, return the ENVELOPE CV
  // clamped to 0..+10 V. A silent input drives the envelope toward 0 V (and the
  // gate toward inactive), with no residual floor.
  double tick(double in) {
    const double rect = std::fabs(in);
    const double coeff = rect >= env_ ? attackCoeff_ : releaseCoeff_;
    env_ += coeff * (rect - env_);
    updateGate_();
    return envCv();
  }

  // ----------------------------------------------------------------- inspect --
  double sampleRate() const { return sr_; }
  double attackSeconds() const { return attackSec_; }
  double releaseSeconds() const { return releaseSec_; }
  // Raw internal follower value (not yet clamped).
  double envRaw() const { return env_; }
  // ENVELOPE CV output, clamped to 0..+10 V (CONFIRMED nominal range).
  double envCv() const {
    return env_ < 0.0 ? 0.0 : (env_ > kEnvMaxVolt ? kEnvMaxVolt : env_);
  }
  // GATE output, +8 V when active else 0 V (CONFIRMED nominal range).
  double gate() const { return gateActive_ ? kGateVolt : 0.0; }
  bool gateActive() const { return gateActive_; }

  // CONFIRMED voltage-spec ranges.
  static constexpr double kEnvMaxVolt = 10.0;  // ENV FOLLOWER CV 0…10 V.
  static constexpr double kGateVolt = 8.0;     // GATE 0…8 V.
  // PROVISIONAL gate-detector tuning: no manual threshold/hysteresis value exists.
  static constexpr double kGateThreshold = 0.5;
  static constexpr double kGateHysteresis = 0.05;
  // PROVISIONAL floor on the time constants (seconds) to avoid a zero/NaN coefficient.
  static constexpr double kMinTime = 1e-4;

 private:
  void recomputeCoefficients_() {
    attackCoeff_ = (sr_ > 0.0 && attackSec_ > 0.0)
                       ? 1.0 - std::exp(-1.0 / (sr_ * attackSec_))
                       : 0.0;
    releaseCoeff_ = (sr_ > 0.0 && releaseSec_ > 0.0)
                        ? 1.0 - std::exp(-1.0 / (sr_ * releaseSec_))
                        : 0.0;
  }
  void updateGate_() {
    if (!gateActive_ && env_ >= kGateThreshold) {
      gateActive_ = true;
    } else if (gateActive_ && env_ < kGateThreshold - kGateHysteresis) {
      gateActive_ = false;
    }
  }

  double sr_ = 0.0;
  double attackSec_ = kMinTime;
  double releaseSec_ = kMinTime;
  double attackCoeff_ = 0.0;
  double releaseCoeff_ = 0.0;
  double env_ = 0.0;
  bool gateActive_ = false;
};

}  // namespace lunar24::core
