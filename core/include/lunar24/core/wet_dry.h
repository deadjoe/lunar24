// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SignalPath — P3-⑤: the WET/DRY output stage. The mandate ("十路 mixer/pan + 双
// 12dB Polivoks VCF + post-filter distortion + WET/DRY — P3 最后一片") closes the
// design/01 §1 chain:
//
//   VOICE → MIX/PAN (VoiceMixer) → DUAL 12dB POLIVOKS VCF (PolivoksFilter)
//         → 后滤波 DISTORTION (Distortion) → WET OUT L/R
//
// and the design/01 📌 output rule: "输出是三路：DRY OUT(VCO A) | WET OUT L/R |
// DRY OUT(VCO B)." design/03 §2 keeps the DSP at FOUR logic outputs (WET L/R +
// DRY A/B): a 2-out device connects WET L/R only; a ≥4-out device maps DRY A/B to
// the physical channels. This header wires those four taps and holds them.
//
// PROVENANCE (frozen registry + design/01/03/07 are the implementation basis;
// evidence strength is marked):
//
//   * DRY A / DRY B — the VCO A / VCO B signals tapped BEFORE the chain (design/01,
//     literal "DRY OUT (VCO A)"). They BYPASS the mixer, the VCF and the distortion
//     entirely — a pure oscillator tap, not a mixed path. dryA = input channel 5
//     (kChannelVcoA), dryB = input channel 6 (kChannelVcoB).
//   * WET L / WET R — the chain output: mixer → VCF → distortion. This is the path
//     that the mixer pan/vol and the VCF/DIST actually shape.
//   * HEADROOM (design/07 §"voltage table"): DRY V4/V5 max 1 V, WET max 2 V —
//     a 2:1 ratio. This is a DECLARED-BOUNDARY item, NOT a -6.02 dB constant: the
//     max's peak/RMS/pk-pk basis is unknown, so no scaling factor is invented here.
//     The downstream device/UI headroom mapping is out of this header's scope
//     (the chain modules produce the signal levels; this header only labels the
//     declared headroom so it is never silently promoted to an exact dB figure).
//   * The DUAL EFFECTOR (id13) is explicitly OUT OF SCOPE for P3 (design/01 §1,
//     "P6 外"); it sits after this output stage and is not wired here.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

#include "lunar24/core/distortion.h"
#include "lunar24/core/polivoks_vcf.h"
#include "lunar24/core/voice_mixer.h"

namespace lunar24::core {

// The four logic outputs (WET L/R + DRY A/B) produced by one full-frame render.
struct WetDryOutput {
  double wetL = 0.0;  // chain: mixer -> vcf -> distortion (left).
  double wetR = 0.0;  // chain: mixer -> vcf -> distortion (right).
  double dryA = 0.0;  // VCO A, tapped BEFORE the chain.
  double dryB = 0.0;  // VCO B, tapped BEFORE the chain.
};

// Composes VoiceMixer + PolivoksFilter + Distortion into the design/01 §1 chain
// and exposes the four logic outputs. One call to `process` advances the DSP by
// one frame; the modules keep their own internal state between frames.
class SignalPath {
 public:
  SignalPath() : distortion_(48000.0) {
    // Default sample rate is set only to keep the module valid before the host
    // calls setSampleRate; the host MUST configure it for real audio.
  }

  // Configure the sample rate for the chain. Must be set once before rendering.
  void setSampleRate(double sr) {
    vcf_.setSampleRate(sr);
    distortion_.setSampleRate(sr);
  }

  // Convenience access to the sub-modules (for panel knobs / CV patch points).
  PolivoksFilter& vcf() { return vcf_; }
  Distortion& distortion() { return distortion_; }

  // Render one frame. `chIn` is the 10-channel mono input (the VCO A/B channels
  // feed both the path AND the DRY taps). Returns the four logic outputs.
  WetDryOutput process(const double chIn[VoiceMixer::kNumChannels]) {
    WetDryOutput o;
    // DRY taps are pre-chain, from the raw oscillator channels.
    o.dryA = chIn[VoiceMixer::kChannelVcoA];
    o.dryB = chIn[VoiceMixer::kChannelVcoB];
    // WET path: mixer -> vcf -> distortion.
    double mixL = 0.0, mixR = 0.0;
    mixer_.tick(chIn, mixL, mixR);
    double vL = 0.0, vR = 0.0;
    vcf_.process(mixL, mixR, vL, vR);
    o.wetL = distortion_.tickL(vL);
    o.wetR = distortion_.tickR(vR);
    return o;
  }

  // Declared headroom (design/07): DRY max 1 V, WET max 2 V, ratio 2:1. The
  // peak/RMS/pk-pk basis is unknown — recorded as a boundary, NOT a dB constant.
  static constexpr double kDryMaxVolts = 1.0;
  static constexpr double kWetMaxVolts = 2.0;
  static constexpr double kWetToDryRatio = kWetMaxVolts / kDryMaxVolts;

 private:
  VoiceMixer mixer_;
  PolivoksFilter vcf_;
  Distortion distortion_;
};

}  // namespace lunar24::core
