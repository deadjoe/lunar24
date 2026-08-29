// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) "5-step sequencer" sound-core: a framework-agnostic,
// fixed-memory, per-sample 5-stage sequential voltage source state machine.
// This is the SOURCE-level sound core only — it is NOT wired into any canonical
// factory / PatchGraph / SynthRuntime product path (that consumer is a later,
// separate GH #11 slice), it is NOT a wrapper over arp_sequencer.h, and it does
// not gain a CLOCK-out volts rail. It follows the established per-sample DSP
// idiom of Lfo/EnvelopeGenerator: mandatory real sample-rate, fail-closed
// configuration, no block cache, reset()-free (there is no hardware / panel
// reset and no transient playhead persistence).
//
// Manual semantics (design/reference/solar42N_manual_text.txt L485-510): a
// classic Buchla-inspired 5-stage sequential voltage source; each step's output
// adjusts 0V..+5V; an internal clock (the PULSER) or any external clock/trigger
// (EXT. CLOCK in) drives the sequence; each step's GATE switch turns its GATE
// output off (the CV is unaffected); the STAGES switch sets 3/4/5 steps; the
// PULSER pot sets the internal clock speed, whose periodic signal is available
// at CLOCK OUT.
//
// PROVISIONAL modelling: the internal PULSER normalized->Hz mapping (panel pot 0..1
// --> Hz, and the registry's nominal 0..5 placeholder) is left to the runtime
// consumer; this core accepts the internal rate IN HERTZ directly. The CLOCK OUT
// rail is a discrete PULSER rising event exposed as a bool — it is NOT a volts
// output and deliberately has no rail constant (the registry marks the two clock
// jacks' polarity as unknown and its nominal 0..5 volts is unverified, a known
// unpublished conflict; we never back-derive an input threshold or output rail
// from it). The EXTERNAL clock input enters as an already-interpreted __gate__
// level (produced by sink_gate_interpret() against the real sequencer.ext_clock_in
// JackDescriptor); we detect the rising edge ourselves but never hardcode volts /
// threshold / hysteresis. Power-on playhead / phase / gate / clock state below is
// a single centrally-labelled deterministic PROVISIONAL lifecycle policy, not a
// claimed known hardware power-on behaviour, and it is never persisted.

#pragma once

#include <cmath>
#include <cstdint>

namespace lunar24 {
namespace core {

// Confirmed rails (manual L159-160 + registry step_cv / gate_out nominal).
// step CV: 0..+5 V (unipolar). gate output: 0..+10 V (unipolar). These rails are
// the only confirmed electrical facts the core depends on.
inline constexpr double kFiveStepCvPeakVolt = 5.0;    // 0..+5V CONFIRMED
inline constexpr double kFiveStepGatePeakVolt = 10.0; // 0..+10V CONFIRMED

// Structural limits (manual L497: stages 3/4/5; module is a 5-step sequencer).
inline constexpr int kFiveStepMinStages = 3;
inline constexpr int kFiveStepMaxStages = 5;
inline constexpr int kFiveStepStepCount = 5; // CV / gate per stage, indices 0..4

class FiveStepSequencer {
 public:
  // Which source advances the sequence. internal = the on-board PULSER clock;
  // external = the EXT. CLOCK in jack (already gate-interpreted by the caller).
  enum class ClockSource : std::uint8_t { kInternal = 0, kExternal = 1 };

  FiveStepSequencer() = default;

  // ----- configuration (fail-closed: an invalid call returns false and leaves
  // the held configuration unchanged; no partial / half-mutated state) -----

  // Real, positive sample rate in Hz. Required before tick()/setInternalRateHz().
  bool setSampleRate(double sampleRate) {
    if (!(std::isfinite(sampleRate) && sampleRate > 0.0)) return false;
    if (sr_ == sampleRate) return true;
    sr_ = sampleRate;
    return true;
  }

  // Internal PULSER clock speed in Hz (panel norm->Hz mapping is the runtime's
  // job; the core takes Hz directly). Non-negative. 0Hz is a defensive stop
  // value (the PULSER never wraps, so the internal clock produces no edges).
  bool setInternalRateHz(double hz) {
    if (!(std::isfinite(hz) && hz >= 0.0)) return false;
    if (internalRateHz_ == hz) return true;
    if (!stepFinite_(hz, sr_)) return false; // needs a valid timebase
    internalRateHz_ = hz;
    return true;
  }

  // Which clock source advances the sequence.
  bool setClockSource(ClockSource source) {
    if (source != ClockSource::kInternal && source != ClockSource::kExternal)
      return false;
    clockSource_ = source;
    return true;
  }

  // Stage count 3/4/5. Shrinking below the currently-active step clamps the
  // playhead so it stays in range (a deterministic boundary consequence — the
  // step CV *values* themselves are never altered, no spurious advance occurs,
  // and out-of-bounds is impossible).
  bool setStageCount(int count) {
    if (count != kFiveStepMinStages && count != 4 && count != kFiveStepMaxStages)
      return false;
    if (stageCount_ == count) return true;
    if (step_ >= count) step_ = count - 1;
    stageCount_ = count;
    return true;
  }

  // Step CV volts (indices 0..4). Known-finite out-of-range values are clamped
  // to the confirmed [0,+5V] rail; non-finite is rejected with no change.
  bool setStepCv(int index, double volts) {
    if (index < 0 || index >= kFiveStepStepCount) return false;
    if (!std::isfinite(volts)) return false;
    const double clamped = std::fmax(0.0, std::fmin(kFiveStepCvPeakVolt, volts));
    if (stepCv_[index] == clamped) return true;
    stepCv_[index] = clamped;
    return true;
  }

  // Step gate-enable (indices 0..4). Does NOT affect the CV output of the step.
  bool setStepGate(int index, bool enabled) {
    if (index < 0 || index >= kFiveStepStepCount) return false;
    if (stepGate_[index] == enabled) return true;
    stepGate_[index] = enabled;
    return true;
  }

  // ----- configuration accessors -----
  double sampleRate() const { return sr_; }
  double internalRateHz() const { return internalRateHz_; }
  ClockSource clockSource() const { return clockSource_; }
  int stageCount() const { return stageCount_; }
  int currentStep() const { return step_; }
  bool started() const { return started_; }

  double stepCv(int index) const {
    const int i = (index < 0 || index >= kFiveStepStepCount) ? 0 : index;
    return stepCv_[i];
  }
  bool stepGate(int index) const {
    const int i = (index < 0 || index >= kFiveStepStepCount) ? 0 : index;
    return stepGate_[i];
  }

  // ----- per-sample audio-domain step -----
  // externalClockHigh is the ALREADY-INTERPRETED EXT. CLOCK gate level (from
  // sink_gate_interpret() against the real sequencer.ext_clock_in descriptor).
  // We detect the rising edge ourselves; a sustained high never repeats a step.
  void tick(bool externalClockHigh) {
    // The PULSER always runs (independent of the selected clock source) so the
    // CLOCK OUT event is produced even when the sequence is external-triggered.
    bool pulserRising = false;
    if (timebaseValid_()) {
      pulserPhase_ += internalRateHz_ / sr_;
      if (pulserPhase_ >= 1.0) {
        pulserRising = true;
        pulserPhase_ = std::fmod(pulserPhase_, 1.0);
      }
    }

    // External rising edge, detected continuously so a source switch never
    // fabricates an edge from a prior sustained-and-unconsumed high.
    const bool extRising = externalClockHigh && !extLatch_;
    extLatch_ = externalClockHigh;

    // Advance comes from the SELECTED clock source only; the other source's edge
    // never advances, so concurrent edges cannot double-advance the sequence.
    const bool advance =
        (clockSource_ == ClockSource::kInternal) ? pulserRising : extRising;

    if (advance) {
      if (started_) {
        step_ = (step_ + 1) % stageCount_; // wrap 3/4/5
      } else {
        started_ = true; // first accepted edge lands step 1 (index 0), never skips
      }
      // PROVISIONAL one-sample gate pulse: +10V on the advance sample ONLY when
      // the entered step's gate is enabled, else 0V; cleared next sample.
      gateSample_ = stepGate_[step_] ? kFiveStepGatePeakVolt : 0.0;
    } else {
      gateSample_ = 0.0;
    }

    cvOut_ = stepCv_[step_];      // current step CV (holds through gate-disabled step)
    clockOutRising_ = pulserRising;
  }

  // ----- outputs -----
  double cvOut() const { return cvOut_; }                 // 0..+5V, current step
  double gateOut() const { return gateSample_; }          // 0..+10V one-sample pulse
  bool clockOutRising() const { return clockOutRising_; } // discrete PULSER event

 private:
  static bool stepFinite_(double hz, double sr) {
    return std::isfinite(sr) && sr > 0.0 && std::isfinite(hz / sr);
  }
  bool timebaseValid_() const {
    return std::isfinite(sr_) && sr_ > 0.0 && std::isfinite(internalRateHz_ / sr_);
  }

  // Configuration. Defaults are a local safe / provisional DSP default, NOT the
  // generated registry canonical default (the core does not copy the registry's
  // placeholder range/default — the PULSER norm->Hz mapping is the runtime's job).
  double sr_ = 0.0;                             // 0.0 = unconfigured; tick() won't run
  double internalRateHz_ = 1.0;                 // Hz; 0 = defensive stop
  ClockSource clockSource_ = ClockSource::kInternal;
  int stageCount_ = kFiveStepMaxStages;
  double stepCv_[kFiveStepStepCount] = {0.0, 0.0, 0.0, 0.0, 0.0};  // 0..+5V
  bool stepGate_[kFiveStepStepCount] = {false, false, false, false, false};

  // Dynamic per-sample state. Only these change during tick(). None is persisted
  // and there is intentionally NO public reset() (no hardware/panel reset jack).
  // Construction-time values are the single centrally-labelled deterministic
  // PROVISIONAL lifecycle policy; they are not a claimed known power-on state.
  double pulserPhase_ = 0.0;  // [0,1); forwarded to clock_out rising events
  bool extLatch_ = false;     // last interpreted external gate level
  int step_ = 0;              // current step index 0..(stageCount_-1)
  bool started_ = false;      // true once the first accepted edge has landed
  double gateSample_ = 0.0;   // one-sample GATE pulse value (0 off-advance)
  double cvOut_ = 0.0;        // current step CV output
  bool clockOutRising_ = false;
};

}  // namespace core
}  // namespace lunar24
