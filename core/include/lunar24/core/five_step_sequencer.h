// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) "5-step sequencer" sound-core: a framework-agnostic,
// fixed-memory, per-sample 5-stage sequential voltage source state machine.
// This sound-core is WIRED into the canonical runtime as one of the six P3
// always-execute control sources (machine_runtime.h kSequencer dispatch publishes
// this core's cv/gate/clock-out to the registry source bank; machine_definition.h
// binds the real registry JackIds). It is NOT a wrapper over arp_sequencer.h. It
// follows the established per-sample DSP idiom of Lfo/EnvelopeGenerator: mandatory
// real sample-rate, fail-closed configuration, no block cache, reset()-free (there
// is no hardware / panel reset and no transient playhead persistence).
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
// --> Hz) is a centrally-named SOFTWARE policy (pulserNormToRateHz() above): the core
// still accepts the internal rate IN HERTZ directly (the DSP-level setter), and the
// runtime consumer maps the panel norm through pulserNormToRateHz(). The CLOCK OUT
// rail is a CONFIRMED bipolar -10..+10V (kFiveStepClockIdleVolt/kFiveStepClockPeakVolt);
// the discrete rising bool (clockOutRising()) remains the single edge truth and the
// volts projection derives from the SAME pulserRising -- never a second phase/latch. The
// pulse WIDTH is PROVISIONAL one-sample. The EXTERNAL clock input enters as an
// already-interpreted __canonical __rising__ edge (produced by sink_gate_interpret()
// against the real sequencer.ext_clock_in JackDescriptor: ss.edge == GateEdge::rising).
// We consume that edge verbatim — we do NOT re-derive an edge from a raw gate level with our own
// latch (a second edge truth-source that would fabricate a phantom advance on the
// first-high sample, which the interpreter reports as edge=none priming). We hardcode
// no input volts / threshold / hysteresis (the CLOCK OUT output rail never back-derives
// the input). Power-on playhead / phase / gate / clock state below is
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
// CLOCK OUT rail (@Codex final ruling 7C3): the sequencer.clock_out OUTPUT is a
// CONFIRMED bipolar -10..+10V rail (manual out-spec table L159 `PULSERL: -10V…+10V`,
// L499 prose "this periodic signal is available at the CLOCK OUT jack", and ELTA's
// official SPECIFICATION "Pulser out -10V…+10V" at eltamusic.com/solar-42f). The PULSE
// WIDTH is PROVISIONAL (one sample): the manual gives no duty/width, so we model the
// PULSER rising as a single-sample +10V peak idling at -10V. This rail is used ONLY as
// an output projection; it NEVER back-derives the sequencer.ext_clock_in input rail,
// threshold, or coupling (those stay unverified and are interpreted by the canonical
// sink interpreter only).
inline constexpr double kFiveStepClockIdleVolt = -10.0;  // CLOCK OUT idle (rail min, CONFIRMED)
inline constexpr double kFiveStepClockPeakVolt = +10.0;  // CLOCK OUT on rising sample (rail max, CONFIRMED)
// PROVISIONAL PULSER norm->Hz software model (no hardware endpoint/taper evidence):
//   hz = kFiveStepPulserMinRateHz * pow(kFiveStepPulserLogBase, norm)
// norm [0,1] -> [0.05,20] Hz, centre 0.5 -> 1 Hz (usable software range 20 s/step ..
// 20 step/s, centre 1 Hz — a USED range, not a Solar 42N measurement). norm=0 does NOT
// declare a stop. Out-of-domain / non-finite -> fail-closed 0 (defensive stop). This is
// a documented SOFTWARE model, never a claimed real-machine curve.
inline constexpr double kFiveStepPulserMinRateHz = 0.05;
inline constexpr double kFiveStepPulserLogBase = 400.0;

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
  // WHOLE-CANDIDATE fail-closed: validates the EXISTING internal rate against the
  // candidate timebase too (same rule Lfo enforces), not just the candidate alone.
  // A naive sr that makes internalRateHz_/sr_ non-finite (e.g. hz=DBL_MAX then a
  // denormal-tiny sr) would otherwise be accepted and silently stall the clock
  // (effective step = Inf). On rejection the OLD sr is preserved with no half-state.
  bool setSampleRate(double sampleRate) {
    if (!(std::isfinite(sampleRate) && sampleRate > 0.0)) return false;
    if (!stepFinite_(internalRateHz_, sampleRate)) return false;
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

  // PROVISIONAL PULSER norm->Hz software model (@Codex final ruling 7C3): the panel PULSER
  // norm [0,1] maps logarithmically to the internal clock rate. This is the single
  // centrally-named place the mapping lives (constant + function together), so the runtime
  // dispatch and any oracle read the SAME policy. Valid norm -> [0.05,20] Hz (centre 0.5 ->
  // 1 Hz); out-of-domain / non-finite -> 0.0 (a defensive stop, fail-closed). This is a
  // documented SOFTWARE policy, never a claimed Solar 42N hardware endpoint/curve; norm=0
  // does NOT declare a stop (it means the slowest software rate).
  static double pulserNormToRateHz(double norm) {
    if (!(std::isfinite(norm) && norm >= 0.0 && norm <= 1.0)) return 0.0;
    return kFiveStepPulserMinRateHz * std::pow(kFiveStepPulserLogBase, norm);
  }

  // ----- per-sample audio-domain step -----
  // externalClockRising is the ALREADY-INTERPRETED EXT. CLOCK canonical rising edge
  // (from sink_gate_interpret() against the real sequencer.ext_clock_in descriptor:
  // ss.edge == GateEdge::rising). We consume it VERBATIM — we do NOT re-derive an
  // edge from a raw gate level via an internal latch (that second edge truth-source
  // would fabricate a phantom advance on the first-high sample, which the interpreter
  // correctly reports as edge=none priming). A sustained high is already just ONE
  // rising edge from the interpreter, so it can never be repeated here; and a source
  // switch to an already-high sink feeds edge=none, so no phantom is made.
  void tick(bool externalClockRising) {
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

    // Advance comes from the SELECTED clock source only; the other source's edge
    // never advances, so concurrent edges cannot double-advance the sequence.
    const bool advance =
        (clockSource_ == ClockSource::kInternal) ? pulserRising : externalClockRising;

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
    // CLOCK OUT projection: derived from the SAME pulserRising as clockOutRising_ (single
    // edge source — no second phase/latch). -10V idle; +10V on the rising sample; the next
    // sample falls back to -10V (one-sample width, PROVISIONAL; rail confirmed -10..+10V).
    clockOutVolts_ = pulserRising ? kFiveStepClockPeakVolt : kFiveStepClockIdleVolt;
  }

  // ----- outputs -----
  double cvOut() const { return cvOut_; }                 // 0..+5V, current step
  double gateOut() const { return gateSample_; }          // 0..+10V one-sample pulse
  bool clockOutRising() const { return clockOutRising_; } // discrete PULSER event
  double clockOutVolts() const { return clockOutVolts_; } // -10 idle / +10 on rising (one-sample, PROVISIONAL)

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
  int step_ = 0;              // current step index 0..(stageCount_-1)
  bool started_ = false;      // true once the first accepted edge has landed
  double gateSample_ = 0.0;   // one-sample GATE pulse value (0 off-advance)
  double cvOut_ = 0.0;        // current step CV output
  bool clockOutRising_ = false;
  double clockOutVolts_ = kFiveStepClockIdleVolt;  // -10 idle / +10 on rising (PROVISIONAL width)
};

}  // namespace core
}  // namespace lunar24
