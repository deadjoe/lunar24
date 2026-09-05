// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Lfo — the LFO A / LFO B low-frequency oscillator sound core (GH #11, P3
// item 6 "patchable control source"). A free-standing, framework-agnostic,
// fixed-memory, per-sample oscillator. It is NOT a wrapper over any audio-rate
// modulation helper; A/B are two real independent Lfo instances carrying their
// OWN phase + config (the caller instantiates one per LFO A and one per LFO B).
//
// Evidence boundary (manual L425-442 + design/06:106): CONFIRMED are two
// independent LFOs, a WAVE knob tunable square<->triangle (a centre-positioned
// knob that mixes the two), a RATE knob, an x1/x6/x10 SPEED switch, and a
// CV OUT that is unipolar POSITIVE 0..+10V (manual L158 + design/06:106,
// polarity + unipolar + range confirmed). The exact wave-blend curve, the
// RATE->Hz transfer, and the power-on phase are all UNMEASURED. Everything
// marked PROVISIONAL below is a centralized modeling choice — the tests pin
// behaviour / trend / determinism, and never fake a measured hardware curve.
//
// The public CV OUT uses virtual volts as the registry's lfo_*_cv_out descriptor
// (0..+10V, unipolar, SignalType::cv) — the tests additionally cross-check that
// real descriptor (see tests/core/test_lfo.cpp), so the rail is grounded in the
// registry, not a test-side shadow. The generator advances its own phase with the
// CURRENT real sample rate; no fixed 48 kHz, no per-block update, no block-boundary
// reset.

#pragma once

#include <cmath>
#include <cstdint>

namespace lunar24::core {

// ---------------------------------------------------------------------------
// PROVISIONAL modelling constants. Centrally named so the public outputs and the
// tests reference the SAME value, and so a future hardware measurement only has
// to relabel these (never re-derive a curve elsewhere).
// ---------------------------------------------------------------------------

// LFO CV OUT rail peak. CONFIRMED (manual L158 + design/06:106: CV OUT is
// unipolar positive swing, 0..+10V). A waveform fundamental is bias-shifted and
// scaled onto this rail: outVolts = (fund + 1.0) * 0.5 * kLfoOutputPeakVolt.
inline constexpr double kLfoOutputPeakVolt = 10.0;

// Speed multiplier selector x1 / x6 / x10 (registry lfo_*.speed_mult option
// labels). The registry carries these as PROVISIONAL field evidence — the exact
// per-position behaviour and default are unverified — so the 1/6/10 factors are a
// centralized provisional mapping, not a decoded hardware curve. x1 is the numeric
// value 0 (a VALID position); any out-of-band / invalid selector code is rejected
// by the fail-closed setSpeedMult so it can never alias a valid position.
enum class LfoSpeedMult : std::uint8_t { x1 = 0, x6 = 1, x10 = 2 };

// Round-trip the selector to its multiplier factor. Returns 1.0 for a value that
// is not one of the three valid positions — the fail-closed setSpeedMult rejects
// such a value, so a configured Lfo never reaches this fallback.
inline constexpr double lfoSpeedMultFactor(LfoSpeedMult m) {
  return m == LfoSpeedMult::x6 ? 6.0
         : m == LfoSpeedMult::x10 ? 10.0
         : 1.0;
}

// ---------------------------------------------------------------------------
// Lfo
// ---------------------------------------------------------------------------
class Lfo {
 public:
  Lfo() = default;
  explicit Lfo(double sampleRate) { setSampleRate(sampleRate); }

  // --- configuration (NOT cleared by reset()) -----------------------------
  // Every numeric setter is FAIL-CLOSED across the WHOLE candidate config: a
  // value that is non-finite, out-of-domain, or would make the effective phase
  // step (baseHz * speedMult / sampleRate) non-finite is rejected — the setter
  // returns false and leaves the prior configuration intact, so persistence /
  // later output are untouched. The effective-step check is the reason a finite
  // input can still be rejected: huge Hz times a multiplier, or a huge rate over
  // a tiny sample rate, can overflow to +/-Inf even though each input is finite,
  // and an infinite phase step would drive the wrapped phase to NaN. A non-finite
  // input is never silently substituted with a neighbouring value; the ONLY
  // transformation applied is a bounds-clamp on a KNOWN-finite wave to [0,1].
  // An invalid speed-multiplier enum is also rejected (fail-closed).
  bool setSampleRate(double sr) {
    if (!(std::isfinite(sr) && sr > 0.0)) return false;  // reject NaN/Inf/0/negative
    if (sr_ == sr) return true;
    if (!effectiveStepFinite(sr, baseHz_, speedMult_)) return false;  // keep prior config
    sr_ = sr;
    return true;
  }
  // baseHz is the LFO rate BEFORE the speed multiplier; the actual oscillation is
  // strictly baseHz * {x1|x6|x10}. 0 is accepted ONLY as a deterministic DSP
  // defensive value (no oscillation / constant output, still finite); it is NOT a
  // claim the hardware RATE knob reaches 0 (registry: 0.1..20 Hz).
  bool setBaseHz(double hz) {
    if (!(std::isfinite(hz) && hz >= 0.0)) return false;
    if (baseHz_ == hz) return true;
    if (!effectiveStepFinite(sr_, hz, speedMult_)) return false;  // keep prior config
    baseHz_ = hz;
    return true;
  }
  // WAVE morph norm: 0 = square, 1 = triangle, between = linear crossfade.
  // Reject non-finite, THEN bounds-clamp a finite value to [0,1]. A wave value
  // never changes the phase, so it can never make the effective step non-finite.
  bool setWave(double w) {
    if (!std::isfinite(w)) return false;
    wave_ = clamp01(w);
    return true;
  }
  // Speed multiplier selector. Reject an out-of-band enum value OR a value whose
  // candidate effective step would be non-finite; keep the prior config.
  bool setSpeedMult(LfoSpeedMult m) {
    if (!validSpeedMult(m)) return false;
    if (speedMult_ == m) return true;
    if (!effectiveStepFinite(sr_, baseHz_, m)) return false;  // keep prior config
    speedMult_ = m;
    return true;
  }

  double sampleRate() const { return sr_; }
  double baseHz() const { return baseHz_; }
  double wave() const { return wave_; }
  LfoSpeedMult speedMult() const { return speedMult_; }

  // --- reset(): clears dynamic phase, keeps config ------------------------
  // There is NO reset jack on the panel — this is a DSP lifecycle reset only.
  // The post-reset phase (== 0.0) and the first-sample convention are PROVISIONAL
  // / deterministic; they are NOT a claim about the real power-on phase (which is
  // unmeasured). reset() touches ONLY this instance's dynamic phase — it never
  // affects another Lfo, and it leaves all configuration untouched.
  void reset() { phase_ = 0.0; }

  // --- per-sample process ----------------------------------------------------
  // Advance the phase by exactly one sample using the CURRENT real sample rate:
  // phase_ += baseHz * speedMult / sr_, then wrap into [0,1). NO fixed 48 kHz, NO
  // per-block update, NO block-boundary reset. The internal fundamental is a
  // deterministic [-1,+1] waveform (square/triangle crossfade), then uniformly
  // mapped to the confirmed 0..+10V unipolar rail. Every returned sample is finite
  // and bounded to [0,+10]. First-sample convention (provisional/deterministic):
  // phase begins at 0.0 and each tick advances before reading, so the first
  // produced sample is the waveform at phase = (baseHz*speedMult/sr).
  //
  // An unconfigured / degenerate Lfo (no valid positive sample rate, or a
  // non-finite effective phase step — the admission guards reject any config that
  // could reach that) must never emit NaN. When the timebase is not valid, tick()
  // does NOT advance the phase (an undefined timebase must not move the cycle
  // position) and returns a deterministic finite no-modulation value of 0V (the
  // bottom of the unipolar rail). This is a centralized, documented fallback — it
  // does NOT invent a hidden 48 kHz default or a hard-coded phase.
  double tick() {
    if (!timebaseValid()) return 0.0;
    phase_ += (baseHz_ * lfoSpeedMultFactor(speedMult_)) / sr_;
    if (phase_ >= 1.0) phase_ -= std::floor(phase_);
    return (fundamental() + 1.0) * 0.5 * kLfoOutputPeakVolt;
  }

  // Internal [-1,+1] fundamental for the current (post-advance) cycle position.
  // Pointwise read-only; used by the strong behaviour tests to verify the real
  // waveform rather than asserting a separately-authored expected-trace shadow.
  double fundamental() const { return fundamentalAt(phase_, wave_); }

  // Normalized cycle position in [0,1). Read-only introspection for the tests.
  double phase() const { return phase_; }

 private:
  static bool validSpeedMult(LfoSpeedMult m) {
    return m == LfoSpeedMult::x1 || m == LfoSpeedMult::x6 || m == LfoSpeedMult::x10;
  }
  static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

  // Admission check for the WHOLE candidate config: a valid positive timebase and
  // a FINITE effective phase step. This is what rejects a finite-but-overflowing
  // combination (huge baseHz * x10 over a small sample rate -> +/-Inf step) so the
  // phase can never wrap through Inf into NaN. Wave is excluded because it does
  // not participate in the rate.
  static bool effectiveStepFinite(double sr, double baseHz, LfoSpeedMult m) {
    if (!(std::isfinite(sr) && sr > 0.0)) return false;
    if (!(std::isfinite(baseHz) && baseHz >= 0.0)) return false;
    if (!validSpeedMult(m)) return false;
    return std::isfinite((baseHz * lfoSpeedMultFactor(m)) / sr);
  }
  bool timebaseValid() const {
    return std::isfinite(sr_) && sr_ > 0.0 &&
           std::isfinite((baseHz_ * lfoSpeedMultFactor(speedMult_)) / sr_);
  }

  // Deterministic [-1,+1] fundamental at a cycle position p in [0,1), blended by
  // a morph wave in [0,1]. square: +1 on the first half, -1 on the second. triangle:
  // a linear -1..+1..-1 over the cycle (constant slope, symmetric). wave linearly
  // crossfades the two. PROVISIONAL policy: only the endpoints (0=square, 1=triangle)
  // and the continuous/monotonic blending relation are pinned; this is never called
  // a measured hardware taper, and triangle is NOT replaced by a sine.
  static double fundamentalAt(double p, double wave) {
    const double square = p < 0.5 ? 1.0 : -1.0;
    const double triangle = 1.0 - 4.0 * std::fabs(p - 0.5);
    return (1.0 - wave) * square + wave * triangle;
  }

  // Configuration (persists across reset()).
  // These are LOCAL SAFE / PROVISIONAL DSP DEFAULTS, NOT the generated registry's
  // canonical defaults (the registry is a separate source of truth). They exist
  // only so an unconfigured Lfo is deterministic and never NaN. Runtime integration
  // must configure sampleRate / baseHz / wave / speedMult from canonical state;
  // tests set the values they depend on explicitly rather than relying on these
  // coinciding with the registry. The default sr_ == 0.0 means "unconfigured":
  // tick() then returns the finite 0V no-modulation fallback (never NaN).
  double sr_ = 0.0;
  double baseHz_ = 1.0;  // Hz, local safe/provisional DSP default
  double wave_ = 0.5;    // normalized morph 0..1, local safe/provisional DSP default
  LfoSpeedMult speedMult_ = LfoSpeedMult::x1;  // local safe/provisional DSP default

  // Dynamic state (cleared by reset()).
  double phase_ = 0.0;  // normalized cycle position [0,1)
};

}  // namespace lunar24::core
