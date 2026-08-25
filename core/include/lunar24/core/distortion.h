// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Distortion — P3-⑤: the post-filter distortion on the FILTER panel (manual
// L1151-1153, registry vcf.dist id16 / vcf.gain id17, both under module id2 vcf).
// Signal chain: → VCF → DISTORTION → WET OUT L/R (design/01 §1). The register groups
// dist/gain under the vcf module because they sit on the same FILTER panel; the
// non-linear PROCESSING is its own header so L and R carry independent state.
//
// PROVENANCE (the frozen registry is the implementation basis; evidence strength
// is marked):
//
//   * dist (vcf.dist id16) — "DIST knob controls the balance between normal and
//     distorted signal" (manual L1151-1152). unit "norm", 0..1, default 0.0,
//     CONFIRMED (the manual's own sentence is the evidence). dist=0 → fully dry
//     (the distorted term vanishes); dist=1 → fully distorted.
//   * gain (vcf.gain id17) — "GAIN knob controls the amount of distortion"
//     (manual L1152). unit "norm", 0..1, default 0.0, CONFIRMED as a semantic
//     (amount of distortion, NOT a balance). gain=0 → the distorted term is
//     identical to the dry term (no distortion to add); gain=max → heavy saturation.
//   * DIST and GAIN are TWO INDEPENDENT controls. Nothing in the manual couples
//     them: changing GAIN must not change the mix ratio, and dist=0 must be dry
//     regardless of gain. This is must-test #2.
//   * NONLINEAR CURVE / RAIL — the manual gives NO saturation curve / rail / fold
//     figure. The tanh soft-clip with rail kSaturationVoltage and the small-signal
//     unity-gain fold (1 + kDrive*drive) and the per-channel one-pole drive
//     SMOOTHER are all PROVISIONAL modeling choices, NOT manual facts. Recorded.
//   * PER-CHANNEL STATE — the nonlinear state is a SIGNAL-DRIVEN drive (a smoothed
//     gain_ × |x|) per channel. L and R keep SEPARATE states; a shared state would
//     let a hot left signal raise the right channel's drive (must-test #3). The
//     drive is smoothed toward gain_×|x| (not toward gain_ alone), so a hot channel
//     saturates only its own non-linearity and the amount of distortion genuinely
//     depends on both GAIN and the instantaneous signal level.
//   * The MASTER VOLUME / PHONE VOLUME suggestion in the manual (L1152-1153) is a
//     separate control, out of this header's scope.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// Post-filter distortion. DIST = dry/wet balance (0..1), GAIN = distortion amount
// (0..1). The two are independent. L and R each carry their own smoothed-drive
// (nonlinear) state.
class Distortion {
 public:
  explicit Distortion(double sampleRate) { setSampleRate(sampleRate); }

  void setSampleRate(double sr) {
    if (sr > 0.0) sr_ = sr;
    recomputeCoefficient_();
  }

  // dist (id16) dry<->wet balance. 0.0 = fully dry. Clamped to [0,1].
  void setDist(double d) { dist_ = clamp01_(d); }
  // gain (id17) distortion amount. 0.0 = no distortion (distorted term = dry).
  void setGain(double g) { gain_ = clamp01_(g); }

  double dist() const { return dist_; }
  double gain() const { return gain_; }

  // Process one LEFT-channel sample with the left's own state.
  double tickL(double x) { return tick_(channelL_, x); }
  // Process one RIGHT-channel sample with the right's own state.
  double tickR(double x) { return tick_(channelR_, x); }

  // PROVISIONAL distortion rail (no manual curve/rail). Chosen to sit at the WET
  // OUT nominal max (design/07 WET max 2 V); the exact rail is un-evidenced.
  static constexpr double kSaturationVoltage = 2.0;
  // PROVISIONAL fold at gain=1 (small-signal unity at gain=0). Un-evidenced.
  static constexpr double kDriveFold = 8.0;
  // PROVISIONAL smoothing time (seconds) for the per-channel drive moved between
  // target and state (a modest AC smoothing; no manual value).
  static constexpr double kSmoothSeconds = 0.005;

 private:
  struct Channel {
    double driveState = 0.0;  // per-channel filter state (the nonlinearity state).
  };

  // dist=0 -> output is exactly the dry term `x` (gain has NO effect).
  // gain=0 -> the distorted term reduces to ~x (unity small-signal), so dist no
  // longer changes the level. Independent axes. The nonlinear state is a
  // SIGNAL-DRIVEN per-channel drive (gain_ × |x| smoothed), so a hot channel
  // saturates only its own non-linearity (must-test #3).
  double tick_(Channel& c, double x) {
    const double driveTarget = gain_ * std::fabs(x);
    c.driveState += coeff_ * (driveTarget - c.driveState);
    const double fold = 1.0 + kDriveFold * c.driveState;
    const double wet = kSaturationVoltage *
        std::tanh(fold * x / kSaturationVoltage);
    return (1.0 - dist_) * x + dist_ * wet;
  }

  void recomputeCoefficient_() {
    coeff_ = (sr_ > 0.0 && kSmoothSeconds > 0.0)
                 ? 1.0 - std::exp(-1.0 / (sr_ * kSmoothSeconds))
                 : 0.0;
  }
  static double clamp01_(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  }

  double sr_ = 0.0;
  double dist_ = 0.0;  // registry default 0.0.
  double gain_ = 0.0;  // registry default 0.0.
  double coeff_ = 0.0;
  Channel channelL_;
  Channel channelR_;
};

}  // namespace lunar24::core
