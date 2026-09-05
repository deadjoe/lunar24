// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Preamp — P3-④: the contact-microphone preamplifier (manual L518-549). This is
// the input front-end for the built-in PIEZZO contact mic and the 3.5 mm EXT
// SOURCE jack; it feeds the voice mixer AND the envelope follower (signal chain:
// PIEZZO → PREAMP → ENVELOPE FOLLOWER, voice-mixer PREAMP channel being the audio
// tap; design/01 §1, §3).
//
// PROVENANCE (the frozen registry, generated/lunar24/registry.hpp, is the
// implementation basis; every claim below mirrors it and marks evidence strength):
//
//   * GAIN (preamp.gain, id 157) — unit "norm", range 0..1, default 0.5. ALL six
//     ParameterFieldEvidence are UNVERIFIED. The manual (L544) writes "Audio
//     amplifier ... up to 40dB of gain": the 40 dB ceiling is a CONFIRMED manual
//     range endpoint, but the knob is NORMALIZED here, so the exact taper between
//     the two fixed ends (mute at minimum — L527 "set GAIN to minimum" — vs +40 dB
//     at maximum) is a PROVISIONAL modeling choice. This header maps the normalized
//     knob LINEARLY IN AMPLITUDE (gain = norm * x100), which lands on both ends
//     exactly; the in-between curve is provisional, not manual evidence.
//   * Soft saturation ("Amplifier clipping indicator", L538-539): a clipping
//     indicator EXISTS, so a nonlinear stage is real and evidence-backed. The exact
//     saturation curve/volt rail is NOT in the manual. The tanh soft-knee and
//     kSaturationVoltage below are PROVISIONAL, and are the nonlinearity that the
//     preamp-aliasing must-test measures (recorded in FINDINGS, not fixed here).
//   * Inputs: mic_in (built-in PIEZZO — internal FixedEndpoint) and ext_source_in
//     (3.5 mm, "bypassing the built-in contact microphone", the one patchable audio
//     jack; nominal ±1 V). The DSP sees the already-selected source as a single
//     voltage; "no source" is 0 V, which the amp passes at zero output — design/04
//     §3 I/O mapping: "PIEZO/PREAMP ... 无输入时静音" (mute on no input). So an
//     unconnected EXT SOURCE yields a clean zero, not noise/DC/NaN.
//
// Framework-free, header-only, no heap, no locks, realtime-safe. Memoryless
// (gain has Smoothing::none in the registry; the amp adds no state).

#pragma once

#include <cmath>

namespace lunar24::core {

// The contact-mic preamplifier. A single voltage in (the selected source) is
// amplified by a normalized GAIN and softly saturated by the amp's clip stage.
class Preamp {
 public:
  // sampleRate is carried for parity with the other P3 DSP headers and to keep a
  // single configuration point if gain smoothing is ever added; the current
  // memoryless path does not use it. Must be > 0.
  explicit Preamp(double sampleRate) : sr_(sampleRate) {}

  // ------------------------------------------------------------------- gain --
  // GAIN knob, normalized 0..1 (the registry's unit "norm"). 0 => mute, 1 => +40 dB
  // (x100). The in-between taper is PROVISIONAL (see provenance).
  void setGainNorm(double norm) {
    gainNorm_ = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
  }

  // ----------------------------------------------------------------- render --
  // Advance one sample and return the amplified, softly-saturated output (volts).
  // in=0 (no source / muted) returns exactly 0 — the nonlinearity is pass-through
  // at zero, so there is no residual noise, DC, or NaN.
  double tick(double in) {
    const double linear = gainLinear() * in;
    return kSaturationVoltage * std::tanh(linear / kSaturationVoltage);
  }

  // --------------------------------------------------------------- inspect --
  double sampleRate() const { return sr_; }
  double gainNorm() const { return gainNorm_; }
  // PROVISIONAL taper: normalized knob -> dB (linear in amplitude, not dB).
  double gainDb() const { return gainNorm_ * kMaxGainDb; }
  double gainLinear() const { return gainNorm_ * kMaxGainLinear; }

  static constexpr double kMaxGainDb = 40.0;      // manual CONFIRMED ceiling (L544).
  static constexpr double kMaxGainLinear = 100.0;  // x100 == +40 dB.
  // PROVISIONAL amp rail that the tanh soft-knee saturates toward (volts). NOT in
  // the manual; a practical clip indicator level in the panel's voltage domain.
  static constexpr double kSaturationVoltage = 10.0;

 private:
  double sr_ = 0.0;
  double gainNorm_ = 0.5;  // registry default.
};

}  // namespace lunar24::core
