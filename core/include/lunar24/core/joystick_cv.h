// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// JoystickCv — the Joystick X/Y + OFFSET X/Y bipolar CV sound core (GH #11, P3
// item 6 "patchable control source"). A free-standing, framework-agnostic,
// fixed-memory (no heap, no lock), per-sample CV mapping core. It is NOT a
// wrapper over any audio-rate modulation helper and NOT an A/B pair: the panel
// has ONE joystick module with TWO independent axes/outputs X and Y, driven by
// four normalized controls (x, y, offset_x, offset_y).
//
// Evidence boundary (manual L446-477 + design/01:90 + design/06:107): CONFIRMED
// are a position-locking joystick with X/Y axes, two voltage offset regulators,
// two patchable outputs (X OUT / Y OUT), and an output rail that is bipolar
// -10..+10V (registry joystick.x_out / joystick.y_out: direction=output,
// SignalType=cv, Polarity=bipolar, nominal -10..+10). design/01 also records
// the left/centre/right offset windows as [-10,0] / [-5,+5] / [0,+10]. The
// mechanical taper, the physical centre, and the offset continuous transfer are
// all UNMEASURED; the registry's normalized range/default/step/smoothing field
// evidence is unverified. Everything marked PROVISIONAL below is a centralized
// modelling choice — the tests pin behaviour / monotonic trend / determinism / a
// truthful end-to-end window, and never fake a measured hardware curve.
//
// Input contract: this core consumes the CONTROL LAYER's already-decided /
// already-smoothed normalized value at the current sample. It does NOT reinvent
// ParameterSmoother or invent a mechanical time constant — the future runtime
// feeds ParameterId / time semantics in. Position-locking in software means: a
// value persists once set until the corresponding control sample changes; there
// is NO spring-return, NO block-boundary reset, NO implicit decay.
//
// There is NO hardware reset jack and NO sample-rate dependency: this is a
// stateless audio-rate CV mapping core, so there is deliberately no tick()/reset()
// to invent. The two outputs are pure per-sample reads of the current four
// control samples — a value set at sample N is reflected by sample N's read
// immediately (no block-cache).

#pragma once

#include <cmath>

namespace lunar24::core {

// ---------------------------------------------------------------------------
// PROVISIONAL modelling constants. Centrally named so the public outputs and the
// tests reference the SAME value, so a future hardware measurement only has to
// relabel these (never re-derive a curve elsewhere).
// ---------------------------------------------------------------------------

// Output rail peak (per side). CONFIRMED (manual + design/06:107 + registry
// joystick.x_out / joystick.y_out: bipolar, nominal -10..+10V). Both outputs are
// bounded to [-kJoystickRailPeakVolt, +kJoystickRailPeakVolt].
inline constexpr double kJoystickRailPeakVolt = 10.0;

// Per-contributor half-rail. PROVISIONAL linear model: each axis position
// contributes [-5,+5]V and the corresponding offset contributes [-5,+5]V; the two
// are summed then bounded to the confirmed rail. This is what yields the offset
// left / centre / right windows [-10,0] / [-5,+5] / [0,+10] as the position
// sweeps. It is a modelling choice, NOT a measured taper.
inline constexpr double kJoystickHalfRailVolt = 5.0;

// ---------------------------------------------------------------------------
// JoystickCv
// ---------------------------------------------------------------------------
class JoystickCv {
 public:
  JoystickCv() = default;

  // --- normalized control inputs ------------------------------------------
  // Each consumes a normalized [0,1] value (default 0.5 = centre). Every setter
  // is FAIL-CLOSED on the WHOLE control sample: a non-finite value (NaN/Inf) is
  // rejected — the setter returns false and leaves the prior value AND the rest of
  // the four-control state untouched, so persistence and the later dual output are
  // intact. A KNOWN-finite value is bounds-clamped to [0,1] (an out-of-band finite
  // value never escapes the rail). A non-finite value is never silently substituted
  // with a neighbouring value.
  bool setX(double v) { return setNorm_(x_, v); }
  bool setY(double v) { return setNorm_(y_, v); }
  bool setOffsetX(double v) { return setNorm_(offsetX_, v); }
  bool setOffsetY(double v) { return setNorm_(offsetY_, v); }

  double x() const { return x_; }
  double y() const { return y_; }
  double offsetX() const { return offsetX_; }
  double offsetY() const { return offsetY_; }

  // --- per-sample outputs (pure reads, no state advance) --------------------
  // X OUT is determined ONLY by X + OFFSET X; Y OUT ONLY by Y + OFFSET Y. The two
  // are genuinely independent computed results: neither copies the other axis, and
  // there is no shared shadow inspector. Each returns virtual volts bounded to the
  // confirmed -10..+10V rail and always finite. Because there is no dynamic state,
  // these are sampled once per sample by the caller; the value set at sample N is
  // already reflected by sample N's read (per-sample update, no block cache).
  double xOut() const { return bipolarSum(x_, offsetX_); }
  double yOut() const { return bipolarSum(y_, offsetY_); }

 private:
  static bool setNorm_(double& member, double v) {
    if (!std::isfinite(v)) return false;
    member = clamp01(v);
    return true;
  }
  static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
  static double normToHalfVolt(double v) { return kJoystickHalfRailVolt * (2.0 * v - 1.0); }
  // Position + same-axis offset, each mapped to [-5,+5], summed then bounded to the
  // confirmed [-10,+10] rail. Also used as the single authoritative per-axis
  // computation so the two axes can never drift apart in formula.
  static double bipolarSum(double position, double offset) {
    const double sum = normToHalfVolt(position) + normToHalfVolt(offset);
    return sum < -kJoystickRailPeakVolt ? -kJoystickRailPeakVolt
           : (sum > kJoystickRailPeakVolt ? kJoystickRailPeakVolt : sum);
  }

  // Current normalized control samples.
  // These are LOCAL SAFE / PROVISIONAL DSP DEFAULTS (centre -> 0V), NOT the
  // generated registry's canonical defaults (the registry is a separate source of
  // truth). They exist only so an unconfigured JoystickCv is deterministic and
  // evaluates to the centre 0V. Runtime integration must set these from canonical
  // state; tests set the values they depend on explicitly.
  double x_ = 0.5;
  double y_ = 0.5;
  double offsetX_ = 0.5;
  double offsetY_ = 0.5;
};

}  // namespace lunar24::core
