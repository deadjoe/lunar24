// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ArEnvelope — the minimal LINEAR attack/release VCA envelope (GH#15 D4).
//
// WHY THIS EXISTS AND WHY IT IS NOT EnvelopeGenerator: the classic drone voices
// (drone_1/2/4/5) already ship a landed group VCA envelope whose law is LINEAR in
// time — `DroneBank::GroupEnv` + `DroneBank::tickGroup` (core/include/lunar24/core/
// drone_bank.h): target = gate ? 1 : 0, then `level += dt/attSeconds` toward the
// target (clamped), or `level -= dt/rlsSeconds` away from it. The Papa Srapa voices
// (drone_3/drone_6) get the SAME audible behaviour, so this primitive reproduces that
// law EXACTLY rather than reusing core/include/lunar24/core/envelope_generator.h,
// whose ADSR is a ONE-POLE EXPONENTIAL. Mixing the two would put two different
// envelope laws and two different sets of time constants under one panel section.
//
// NOT IN THIS SLICE: HOLD. The classic group envelope ORs a PROVISIONAL `hold` term
// into its target (`gate || hold`). The D4 contract deliberately scopes the
// drone_3/6 `hold` parameters (registry 288/300) OUT, so this envelope's target is
// the gate alone. When/if HOLD lands it is an OR on `target`, not a second envelope.
//
// GATE SEMANTICS (the provisional convention, stated once here): the gate level is
// resolved OUTSIDE this primitive (the runtime resolves drone_N.gate_in through the
// patch graph and a per-voice GateClockSinkState). An UNPATCHED gate input does not
// mean "closed" — the caller passes the named provisional default
// `DroneBank::kDefaultGroupGateOpen`, which is the same constant the classic groups
// use, so the default-open behaviour has exactly ONE source and cannot drift between
// the classic and Papa Srapa sections. With that default the envelope stays at the
// open level, so an unpatched voice is bit-identical to the pre-D4 path (the D4
// regression lock). Once a cable lands on gate_in the voice follows the gate.
//
// Framework-free, header-only, no heap, no locks, realtime-safe.

#pragma once

namespace lunar24::core {

class ArEnvelope {
 public:
  // Floor for the stage times. mapAttSeconds/mapRlsSeconds already bottom out at
  // kAttNormMinSeconds/kRlsNormMinSeconds (0.001 s), so the product path can never
  // reach this; it exists only so a caller passing 0 (or a negative) yields the
  // fastest legal stage instead of a division by zero / a non-monotonic jump.
  static constexpr double kMinStageSeconds = 1e-6;

  // sampleRate must be > 0. `gateInitial` is the level the gate starts at before any
  // cable resolves — the product passes DroneBank::kDefaultGroupGateOpen so an
  // unpatched voice starts OPEN (see the header note).
  ArEnvelope(double sampleRate, bool gateInitial, double attSeconds, double rlsSeconds)
      : sampleRate_(sampleRate),
        gate_(gateInitial),
        attSeconds_(clampStage_(attSeconds)),
        rlsSeconds_(clampStage_(rlsSeconds)) {}

  // The two stages are set independently and never share state, so changing the
  // ATTACK time can never perturb a release in flight (and vice versa). Both come
  // from DroneBank::mapAttSeconds / mapRlsSeconds — the single product mapping.
  void setAttSeconds(double s) { attSeconds_ = clampStage_(s); }
  void setRlsSeconds(double s) { rlsSeconds_ = clampStage_(s); }

  // The gate level for the NEXT tick. Idempotent: calling it every sample with the
  // same value is the product path, and it never resets `level` (the classic group
  // envelope never resets either — a re-gate during a release resumes from wherever
  // the level currently is, so the VCA gain is always continuous).
  void setGate(bool high) { gate_ = high; }

  // Advance one sample. This is the classic GroupEnv law verbatim: move toward the
  // target at the stage's rate and CLAMP at the target so the level can overshoot
  // neither 1.0 nor 0.0.
  void tick() {
    const double dt = 1.0 / sampleRate_;
    const double target = gate_ ? 1.0 : 0.0;
    if (level_ < target) {
      level_ += dt / attSeconds_;
      if (level_ > target) level_ = target;
    } else if (level_ > target) {
      level_ -= dt / rlsSeconds_;
      if (level_ < target) level_ = target;
    }
  }

  // Current VCA gain 0..1 — the value the product multiplies the voice's audio by.
  double level() const { return level_; }
  // The gate level the last tick consumed (readback, not a shadow bank).
  bool gate() const { return gate_; }
  double attSeconds() const { return attSeconds_; }
  double rlsSeconds() const { return rlsSeconds_; }

 private:
  static double clampStage_(double s) { return s > kMinStageSeconds ? s : kMinStageSeconds; }

  double sampleRate_;
  bool gate_;
  double attSeconds_;
  double rlsSeconds_;
  // Starts OPEN (1.0), exactly like GroupEnv::level — so with the provisional
  // default-open gate the envelope is a transparent ×1.0 gain on the very first
  // sample and the unpatched voice is bit-identical to the pre-D4 path.
  double level_ = 1.0;
};

}  // namespace lunar24::core
