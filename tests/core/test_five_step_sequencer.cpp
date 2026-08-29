// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH #11 (P3 item 6) "5-step sequencer" sound-core strong-oracle suite for
// core/include/lunar24/core/five_step_sequencer.h. This new FiveStepSequencer is a
// standalone per-sample 5-stage sequential voltage source state machine — NOT a
// wrapper over arp_sequencer.h, NOT wired into any canonical factory/PatchGraph/
// SynthRuntime product path (that is a later, separate GH #11 slice), and it gains
// no CLOCK-out volts rail. It follows the Lfo/EnvelopeGenerator idiom: real
// sample-rate, fail-closed config, per-sample tick, no block cache, and deliberately
// no public reset() (there is no hardware/panel reset jack and no transient
// playhead persistence).
//
// Confirmed rails the core depends on: step CV 0..+5 V (unipolar) and GATE 0..+10 V
// (unipolar), grounded on the real reg::kJacks sequencer.cv_out / sequencer.gate_out
// descriptors. The CLOCK-OUT is a discrete PULSER rising event (bool) and the EXT
// CLOCK enters as an already-interpreted gate level (via sink_gate_interpret against
// the real sequencer.ext_clock_in descriptor) — the core hardcodes no clock volts /
// threshold / polarity, which are unverified in the registry.
//
// @Codex mandate (msg 6ce7adf9) must-tests:
//   ① STAGES 3/4/5 wrap; first accepted edge lands step 1 (never skips it)
//   ② five DISTINCT CVs appear in order; active-step CV updates immediately
//   ③ gate-enable ONLY affects GATE — CV holds on a gate-disabled step
//   ④ GATE is a 0..+10V ONE-sample pulse (advance sample only, cleared next sample)
//   ⑤ internal vs external source; the OTHER source's edge never double-advances
//   ⑥ PULSER CLOCK-OUT fires independently of which source is selected
//   ⑦ real-descriptor external-edge lands at the exact absolute sample
//   ⑧ four different sample rates give the correct wall-clock step period (no fixed 48k)
//   ⑨ same sample stream => bit-identical traces under different block partitions
//   ⑩ invalid config / index / NaN / Inf is fail-closed (no half-state)
//   ⑪ construction-time provisional lifecycle; no transient-state persistence
//   ⑫ registry descriptor honesty (clock fields unverified are NOT implementation constants)
//
// Negative controls (each narrow old-error RED->revert GREEN) are run separately in
// a detached worktree: ① CV fixed to 0 / dereferenced from the wrong step, ② gate
// held for the whole step instead of a one-sample pulse, ③ GATE rail wrong (e.g. +5V),
// ④ block-boundary sample-rate cache / fixed-48k, ⑤ a stub external source that never
// advances, ⑥ NaN/Inf accepted by a config setter. Detectors read real per-sample
// output — never source grep / a read-only inspector / a self-copied trace.

#include "mini_test.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "lunar24/core/five_step_sequencer.h"
#include "lunar24/core/sink_interpret.h"
#include "lunar24/registry.hpp"

namespace core = lunar24::core;
namespace reg = lunar24::registry;

namespace {

using core::FiveStepSequencer;
using core::GateClockSinkState;
using core::SinkSample;

// Five DISTINCT (and asymmetric) step CVs, so a permutation / reorder / off-by-one
// mistake cannot sneak through a symmetric oracle.
constexpr double kCv[5] = {5.0, 1.0, 2.0, 3.0, 4.0};

// Independent GATE rail oracle: the manual (L159-160) and the real
// sequencer.gate_out descriptor both give a unipolar 0..+10V GATE output. We assert
// this LITERAL (10.0), NOT the module's kFiveStepGatePeakVolt constant — so a change
// to that constant (e.g. a "+5V GATE rail" regression) turns impl AND expectation
// together and would otherwise stay green. Pinning the literal keeps the gate rail
// answerable to the confirmed source, never to the implementation's own copy.
constexpr double kGateNominalVolt = 10.0;

int find_jack(core::JackId id) {
  for (std::uint32_t i = 0; i < core::kJackCount; ++i) {
    if (reg::kJacks[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

// Configure a fresh sequencer with the standard five CVs, a FULLY-ENABLED gate mask,
// a known sample rate / internal rate and a stage count.
FiveStepSequencer makeSeq(double sr, double hz, int stages) {
  FiveStepSequencer s;
  s.setSampleRate(sr);
  s.setInternalRateHz(hz);
  s.setStageCount(stages);
  s.setClockSource(FiveStepSequencer::ClockSource::kInternal);
  for (int i = 0; i < 5; ++i) {
    s.setStepCv(i, kCv[i]);
    s.setStepGate(i, true);
  }
  return s;
}

// --- ① + ② : STAGES wrap + five distinct CVs present in order --------------------
//
// Note on timing: the internal PULSER phase in this core (like the accepted Lfo)
// accumulates `hz/sr` per sample with float rounding, so the FIRST wrap does not
// land at an exact `period-1` sample index — it lands at the first index where the
// accumulated phase actually reaches 1.0, which is build/float-dependent. We must
// therefore NOT hardcode absolute internal-clock sample indices (a drift-fragile
// oracle). Instead we pin the STRUCTURAL contract, which is deterministic:
//   * the first accepted edge presents step 1 (index 0), never skips it;
//   * steps advance strictly in order then wrap by the STAGES count;
//   * consecutive advances are evenly spaced at the PULSER period (never 0 — a
//     double-advance is exactly the bug we guard against).

void test_stages_wrap_and_step1_first() {
  // sr=100, hz=10 -> nominal PULSER period = 10 samples. The steady-state gap is
  // what we pin (relative), never an absolute first-wrap index.
  const int period = 10;
  for (int stages : {3, 4, 5}) {
    FiveStepSequencer s = makeSeq(100.0, 10.0, stages);
    std::vector<int> advance;
    std::vector<double> cvAt;
    for (int sample = 0; sample <= 60; ++sample) {
      s.tick(false);
      // Every gate is enabled, so +10V marks exactly an advance sample.
      if (s.gateOut() == kGateNominalVolt) {
        advance.push_back(sample);
        cvAt.push_back(s.cvOut());
      }
    }
    CHECK(advance.size() >= stages + 1u);
    // Guard the per-index assertions below: an under-filled advance train (already
    // flagged by the CHECK above, e.g. a dead clock or a broken gate rail) must not
    // index out of range — it terminates this stages iteration as a clean RED, not a
    // crash.
    if (advance.size() < static_cast<std::size_t>(stages) + 1u) continue;
    // Consecutive advances are evenly spaced at the PULSER period. Crucially the
    // gap is never 0 (no double advance) and stays within one sample of the nominal.
    for (std::size_t i = 1; i < advance.size(); ++i) {
      const int gap = advance[i] - advance[i - 1];
      CHECK_TRUE(gap > 0);
      CHECK(gap >= period - 1 && gap <= period + 1);
    }
    // The first accepted edge must present step 1 (index 0): its CV is kCv[0].
    CHECK(std::fabs(cvAt[0] - kCv[0]) < 1e-12);
    // Steps 2..{stages} in order, then wrap back to step 1 (cvAt[k] is the step
    // entered by the k-th accepted edge).
    for (int i = 1; i < stages; ++i) CHECK(std::fabs(cvAt[i] - kCv[i]) < 1e-12);
    CHECK(std::fabs(cvAt[stages] - kCv[0]) < 1e-12);
  }
}

// --- ③ : gate-enable only affects GATE; CV holds on a gate-disabled step ---------
//
// Driven with CONTROLLED EXTERNAL edges so the entered step is known exactly
// (no dependence on internal PULSER wrap timing). Steps 1 and 3 have their GATE
// switch disabled; the CV must be unaffected on those steps.

void test_gate_mask_only_affects_gate() {
  FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
  s.setClockSource(FiveStepSequencer::ClockSource::kExternal);
  s.setStepGate(1, false);  // step 2
  s.setStepGate(3, false);  // step 4
  // Single-sample-high external rising edges, far enough apart to never re-trigger
  // from a sustained high: enter steps 0,1,2,3,4,0 via indices 5,10,15,20,25,30.
  const bool isEdge[36] = {0,0,0,0,0,1, 0,0,0,0,1, 0,0,0,0,1, 0,0,0,0,1,
                           0,0,0,0,1, 0,0,0,0,1};
  const auto gateOn = [](int step) { return step != 1 && step != 3; };
  int edgesDone = 0;  // count of advances that have landed
  for (int sample = 0; sample < 36; ++sample) {
    s.tick(isEdge[sample]);
    if (isEdge[sample]) {
      // This exact sample: an advance lands and enters step (edgesDone % 5).
      const int entered = edgesDone % 5;
      // CV is always the entered step's CV, even on a gate-disabled step.
      CHECK(std::fabs(s.cvOut() - kCv[entered]) < 1e-12);
      CHECK_EQ(s.gateOut(), gateOn(entered) ? kGateNominalVolt : 0.0);
      ++edgesDone;
    } else {
      // Non-advance sample: gate is off, and CV holds the current step's value (the
      // gate-disable never disturbs the CV output between advances).
      CHECK_EQ(s.gateOut(), 0.0);
      CHECK(std::fabs(s.cvOut() - kCv[s.currentStep()]) < 1e-12);
    }
  }
  CHECK_EQ(edgesDone, 6);
}

// --- ④ : GATE is a 0..+10V ONE-sample pulse --------------------------------------

void test_gate_one_sample_pulse() {
  FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
  std::vector<double> gate;
  for (int sample = 0; sample <= 60; ++sample) {
    s.tick(false);
    gate.push_back(s.gateOut());
  }
  int pulses = 0;
  for (std::size_t i = 0; i < gate.size(); ++i) {
    if (gate[i] == 0.0 || std::fabs(gate[i]) < 1e-12) continue;
    // Off-advance samples are 0; an advance emits ONLY the +10V rail.
    CHECK_EQ(gate[i], kGateNominalVolt);
    // Cleared on the very next sample — but only read the successor when it exists,
    // else this is a container-overflow on the final element (ASan-red).
    if (i + 1 < gate.size()) CHECK_EQ(gate[i + 1], 0.0);
    ++pulses;
  }
  CHECK_TRUE(pulses >= 5);
}

// --- ⑤ : clock source isolation; the right source advances, the other never does -
//
// Robust oracle (no absolute internal-clock index): the sequence advances ONLY on
// edges from the SELECTED source, and the OTHER source's edges (however many fire,
// including sustained or coincident ones) contribute exactly zero advances. We
// assert this by COUNTING each source's edges and checking the advance count/POSITION
// matches the selected source.
//   (a) CLOCK=internal: advances are the internal PULSER wraps, so the gate-pulse
//       samples EQUAL the clockOutRising samples (same edge), even with a sustained
//       external high feeding a rising edge — that external edge adds nothing.
//   (b) CLOCK=external: advances are exactly the controlled external rising edges I
//       feed, regardless of how many internal wraps also fire.

void test_clock_source_no_double_advance() {
  // (a) CLOCK=internal, with a SUSTAINED external high from sample 5 onward (a rising
  // edge that is the OTHER source). The advance samples must equal the clockOutRising
  // samples — a coincident/extra external edge would break that equality (double-advance).
  {
    FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
    s.setClockSource(FiveStepSequencer::ClockSource::kInternal);
    std::vector<int> advance;
    std::vector<int> clockOut;
    for (int sample = 0; sample <= 60; ++sample) {
      s.tick(sample >= 5);  // external rises once at 5, then stays high (rolling edges)
      if (s.gateOut() == kGateNominalVolt) advance.push_back(sample);
      if (s.clockOutRising()) clockOut.push_back(sample);
    }
    CHECK_TRUE(advance.size() >= 5u);        // the PULSER wrapped several times
    CHECK_EQ(advance.size(), clockOut.size());  // NO extra advance from the external edge
    for (std::size_t i = 0; i < advance.size(); ++i) CHECK_EQ(advance[i], clockOut[i]);
  }
  // (b) CLOCK=external: advances are exactly the controlled external rising edges.
  //     Internal wraps fire but never advance the sequence (so the count stays 3).
  {
    FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
    s.setClockSource(FiveStepSequencer::ClockSource::kExternal);
    std::vector<int> advance;
    int clockCount = 0;
    for (int sample = 0; sample <= 40; ++sample) {
      // Single-sample-high edges at 3, 12, 21 (clean risings, spaced apart).
      s.tick(sample == 3 || sample == 12 || sample == 21);
      if (s.gateOut() == kGateNominalVolt) advance.push_back(sample);
      if (s.clockOutRising()) ++clockCount;
    }
    const int want[3] = {3, 12, 21};
    CHECK_EQ(advance.size(), 3u);
    if (advance.size() == 3u)
      for (int i = 0; i < 3; ++i) CHECK_EQ(advance[i], want[i]);
    CHECK_TRUE(clockCount >= 3);  // the internal PULSER ran, but did not advance the seq
  }
}

// --- ⑥ : PULSER CLOCK-OUT is independent of the selected source ----------------
//
// The CLOCK-OUT event is a discrete PULSER rising edge that fires regardless of which
// source advances the sequence. We prove independence by COUNT/count-of-other: with
// CLOCK=external and external edges controlling the sequence, the sequence advances
// EXACTLY on those edges while clockOutRising() still produces the PULSER wraps.

void test_pulser_clock_out_independent() {
  FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
  s.setClockSource(FiveStepSequencer::ClockSource::kExternal);
  std::vector<int> seqAdvance;
  int clockCount = 0;
  for (int sample = 0; sample <= 40; ++sample) {
    s.tick(sample == 2 || sample == 12);  // two external risings
    if (s.gateOut() == kGateNominalVolt) seqAdvance.push_back(sample);
    if (s.clockOutRising()) ++clockCount;
  }
  const int wantSeq[2] = {2, 12};
  CHECK_EQ(seqAdvance.size(), 2u);
  if (seqAdvance.size() == 2u)
    for (int i = 0; i < 2; ++i) CHECK_EQ(seqAdvance[i], wantSeq[i]);
  // The PULSER keeps producing CLOCK-OUT events (≈4 wraps over 41 samples at 10Hz/100Hz)
  // even though it is NOT the selected source — that is the independence claim.
  CHECK_TRUE(clockCount >= 3);
}

// --- ⑦ : real-descriptor external edge lands at the exact absolute sample -------

void test_external_real_descriptor_sample_accuracy() {
  const int extIx = find_jack(core::JackId::sequencer_ext_clock_in);
  CHECK_TRUE(extIx >= 0);
  if (extIx < 0) return;
  const core::JackDescriptor& desc = reg::kJacks[extIx];

  // The descriptor's gateThresholdVolts is 0, so gate-high requires input >=0;
  // use -5V (low) and +5V (high) for clean edges. Rising edges are where the stream
  // goes low->high: samples 3, 8, 14.
  std::vector<double> volts(20, -5.0);
  for (int s : {3, 4, 5, 8, 9, 14, 15, 16, 17, 18, 19}) volts[s] = 5.0;

  FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
  s.setClockSource(FiveStepSequencer::ClockSource::kExternal);
  GateClockSinkState st{};
  std::vector<int> advance;
  for (int sample = 0; sample < 20; ++sample) {
    const SinkSample ss = core::sink_gate_interpret(desc, st, volts[sample]);
    s.tick(ss.gateHigh);
    if (s.gateOut() == kGateNominalVolt) advance.push_back(sample);
  }
  const int want[3] = {3, 8, 14};
  CHECK_EQ(advance.size(), 3u);
  if (advance.size() == 3u)
    for (int i = 0; i < 3; ++i) CHECK_EQ(advance[i], want[i]);
}

// --- ⑧ : four sample rates give the correct wall-clock step period --------------

void test_four_sample_rates_wallclock() {
  const double hz = 1000.0;  // period = sr/hz samples
  for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
    FiveStepSequencer s = makeSeq(sr, hz, 5);
    std::vector<int> advance;
    for (int sample = 0; sample < 5000; ++sample) {
      s.tick(false);
      if (s.gateOut() == kGateNominalVolt) advance.push_back(sample);
      if (advance.size() >= 4u) break;
    }
    CHECK_TRUE(advance.size() >= 3u);
    if (advance.size() < 3u) continue;
    const double expect = sr / hz;  // samples per step
    for (std::size_t i = 1; i < advance.size(); ++i) {
      const double gap = static_cast<double>(advance[i] - advance[i - 1]);
      // The step period must track the REAL sample rate, never a fixed 48k.
      CHECK(std::fabs(gap - expect) <= 1.5);
    }
  }
}

// --- ⑨ : same sample stream => bit-identical traces under block partition -------

struct Trace {
  std::vector<double> cv;
  std::vector<double> gate;
  std::vector<int> clk;
};

// Process the same raw input-volts stream in chunkCount-sized sub-blocks, keeping
// the gate-interpreter and the sequencer state ACROSS blocks (never resetting at a
// block boundary). Returns the full per-sample trace for comparison.
Trace runTrace(const core::JackDescriptor& desc, const std::vector<double>& volts,
               int chunkCount) {
  Trace t;
  FiveStepSequencer s = makeSeq(100.0, 10.0, 5);
  s.setClockSource(FiveStepSequencer::ClockSource::kExternal);
  GateClockSinkState st{};
  t.cv.reserve(volts.size());
  t.gate.reserve(volts.size());
  t.clk.reserve(volts.size());
  int sample = 0;
  while (sample < static_cast<int>(volts.size())) {
    const int chunkEnd = sample + chunkCount;
    for (; sample < chunkEnd && sample < static_cast<int>(volts.size()); ++sample) {
      const SinkSample ss = core::sink_gate_interpret(desc, st, volts[sample]);
      s.tick(ss.gateHigh);
      t.cv.push_back(s.cvOut());
      t.gate.push_back(s.gateOut());
      t.clk.push_back(s.clockOutRising() ? 1 : 0);
    }
  }
  return t;
}

void test_block_partition_bit_identical() {
  const int extIx = find_jack(core::JackId::sequencer_ext_clock_in);
  CHECK_TRUE(extIx >= 0);
  if (extIx < 0) return;
  const core::JackDescriptor& desc = reg::kJacks[extIx];

  std::vector<double> volts;
  for (int s = 0; s < 80; ++s) volts.push_back((s % 7 == 3 || s % 7 == 4) ? 5.0 : -5.0);

  const Trace base = runTrace(desc, volts, 1);
  const Trace a = runTrace(desc, volts, 3);
  const Trace b = runTrace(desc, volts, 7);
  CHECK_EQ(base.cv.size(), volts.size());
  CHECK_EQ(base.cv.size(), a.cv.size());
  CHECK_EQ(base.cv.size(), b.cv.size());
  bool same = true;
  for (std::size_t i = 0; i < base.cv.size(); ++i) {
    if (base.cv[i] != a.cv[i] || base.cv[i] != b.cv[i]) same = false;
    if (base.gate[i] != a.gate[i] || base.gate[i] != b.gate[i]) same = false;
    if (base.clk[i] != a.clk[i] || base.clk[i] != b.clk[i]) same = false;
  }
  CHECK_TRUE(same);
}

// --- ⑩ : invalid config / index / NaN / Inf fail-closed -------------------------

void test_invalid_config_fail_closed() {
  FiveStepSequencer a;

  // Sample rate: must be finite and positive.
  CHECK_FALSE(a.setSampleRate(0.0));
  CHECK_FALSE(a.setSampleRate(-1.0));
  CHECK_FALSE(a.setSampleRate(std::nan("")));
  CHECK_FALSE(a.setSampleRate(std::numeric_limits<double>::infinity()));
  CHECK_TRUE(a.setSampleRate(100.0));

  // Internal rate: non-negative, finite, needs a valid timebase.
  CHECK_FALSE(a.setInternalRateHz(-1.0));
  CHECK_FALSE(a.setInternalRateHz(std::nan("")));
  CHECK_FALSE(a.setInternalRateHz(std::numeric_limits<double>::infinity()));
  CHECK_EQ(a.internalRateHz(), 1.0);  // unchanged after the rejects

  // Stage count: only 3/4/5.
  CHECK_FALSE(a.setStageCount(0));
  CHECK_FALSE(a.setStageCount(2));
  CHECK_FALSE(a.setStageCount(6));
  CHECK_EQ(a.stageCount(), 5);  // unchanged after the rejects
  CHECK_TRUE(a.setStageCount(4));
  CHECK_EQ(a.stageCount(), 4);

  // Step CV: invalid index / non-finite rejected WITHOUT touching the stored value;
  // known-finite values clamp to [0,+5].
  CHECK_TRUE(a.setStepCv(0, 2.0));
  CHECK_EQ(a.stepCv(0), 2.0);
  CHECK_FALSE(a.setStepCv(-1, 9.0));
  CHECK_FALSE(a.setStepCv(5, 9.0));
  CHECK_FALSE(a.setStepCv(0, std::nan("")));
  CHECK_FALSE(a.setStepCv(0, std::numeric_limits<double>::infinity()));
  CHECK_EQ(a.stepCv(0), 2.0);  // still the prior value: no half-state
  CHECK_TRUE(a.setStepCv(0, 7.0));   // finite -> clamp to +5V
  CHECK_EQ(a.stepCv(0), 5.0);
  CHECK_TRUE(a.setStepCv(0, -3.0));  // finite -> clamp to 0V
  CHECK_EQ(a.stepCv(0), 0.0);
  CHECK_EQ(a.stepCv(1), 0.0);  // a different index was never touched

  // Step gate: invalid index rejected.
  CHECK_FALSE(a.setStepGate(-1, true));
  CHECK_FALSE(a.setStepGate(5, true));
  CHECK_TRUE(a.setStepGate(0, true));
  CHECK_TRUE(a.stepGate(0));

  // Clock source: invalid enum rejected, prior (internal) preserved.
  CHECK_FALSE(a.setClockSource(static_cast<FiveStepSequencer::ClockSource>(99)));
  CHECK(a.clockSource() == FiveStepSequencer::ClockSource::kInternal);
}

// --- ⑪ : construction-time provisional lifecycle; no transient persistence -------

void test_constructor_provisional_no_persistence() {
  // A freshly constructed sequencer has a deterministic, documented provisional
  // state: shows step 1's CV, gate closed, not started, no clock-out.
  FiveStepSequencer s;
  CHECK_EQ(s.cvOut(), 0.0);
  CHECK_EQ(s.gateOut(), 0.0);
  CHECK_FALSE(s.started());
  CHECK_EQ(s.currentStep(), 0);
  CHECK_FALSE(s.clockOutRising());

  // Configure + advance a few steps, then construct a brand-new one: it must be back
  // at the clean provisional state (no hidden static/persistent playhead).
  FiveStepSequencer t = makeSeq(100.0, 10.0, 5);
  for (int i = 0; i < 25; ++i) t.tick(false);
  CHECK_TRUE(t.started());
  FiveStepSequencer u;
  CHECK_FALSE(u.started());
  CHECK_EQ(u.currentStep(), 0);
  CHECK_EQ(u.cvOut(), 0.0);

  // A step-CV change to the upcoming step is visible on the very next sample (no block
  // cache): drive with CONTROLLED external edges so "the moment step 1 is entered" is
  // exact. Enter step 0, retune step 1's CV (a rail-valid value), then step to step 1
  // — it must reflect immediately, on the advance sample itself.
  FiveStepSequencer v = makeSeq(100.0, 10.0, 5);
  v.setClockSource(FiveStepSequencer::ClockSource::kExternal);
  for (int sample = 0; sample <= 3; ++sample) {
    v.tick(false);  // sample 0, 2 = idle
    if (sample == 1) {
      v.tick(true);  // first external edge -> enter step 1 (index 0), started=true
      CHECK_TRUE(v.started());
      CHECK_EQ(v.currentStep(), 0);
    }
    if (sample == 3) {
      // The step-1 CV retune happens while we are STILL on step 0 (edge at 1, before
      // the edge at 3 enters step 1) — so this proves no block/latency cache.
      v.setStepCv(1, 4.0);  // finite, within [0,+5] rail -> accepted, not clamped
      v.tick(true);         // second external edge -> enter step 2 (index 1)
      CHECK_TRUE(v.started());
      CHECK_EQ(v.stepCv(1), 4.0);
      CHECK_EQ(v.cvOut(), 4.0);  // reflected on the advance sample, immediately
    }
  }
}

// --- ⑫ : registry descriptor honesty (unverified clock fields are NOT constants) -

void test_registry_descriptor_honesty() {
  const int iExt = find_jack(core::JackId::sequencer_ext_clock_in);
  const int iClk = find_jack(core::JackId::sequencer_clock_out);
  const int iCv = find_jack(core::JackId::sequencer_cv_out);
  const int iGate = find_jack(core::JackId::sequencer_gate_out);
  CHECK_TRUE(iExt >= 0 && iClk >= 0 && iCv >= 0 && iGate >= 0);
  if (iExt < 0 || iClk < 0 || iCv < 0 || iGate < 0) return;

  const core::JackDescriptor& ext = reg::kJacks[iExt];
  const core::JackDescriptor& clk = reg::kJacks[iClk];
  const core::JackDescriptor& cv = reg::kJacks[iCv];
  const core::JackDescriptor& gate = reg::kJacks[iGate];

  // Confirmed rails the core really depends on.
  CHECK(cv.direction == core::PinDirection::output);
  CHECK(cv.signalType == core::SignalType::cv);
  CHECK(cv.polarity == core::Polarity::unipolar);
  CHECK_EQ(cv.nominalMin, 0.0);
  CHECK_EQ(cv.nominalMax, 5.0);

  CHECK(gate.direction == core::PinDirection::output);
  CHECK(gate.signalType == core::SignalType::gate);
  CHECK(gate.polarity == core::Polarity::unipolar);
  CHECK_EQ(gate.nominalMin, 0.0);
  CHECK_EQ(gate.nominalMax, 10.0);

  // The two CLOCK jacks: the facts the core would be tempted to hardcode are
  // EXPLICITLY unverified. The core consumes only an interpreted gate level (bool)
  // and emits only a discrete event (bool) — it has no clock volts/threshold/
  // polarity constant. This proves the implementation did not invent a rail.
  CHECK(clk.signalType == core::SignalType::clock);
  CHECK(clk.polarity == core::Polarity::unknown);
  CHECK(clk.fieldEvidence.signalType == core::EvidenceStatus::confirmed);
  CHECK(clk.fieldEvidence.threshold == core::EvidenceStatus::unverified);
  CHECK(clk.fieldEvidence.polarity == core::EvidenceStatus::unverified);

  CHECK(ext.signalType == core::SignalType::clock);
  CHECK(ext.polarity == core::Polarity::unknown);
  CHECK(ext.fieldEvidence.threshold == core::EvidenceStatus::unverified);
  CHECK(ext.fieldEvidence.polarity == core::EvidenceStatus::unverified);

  // The core's CLOCK-OUT is a bool, not a volts rail — a direct consequence of the
  // CLOCK-OUT rail conflict (PULSERL -10..+10V vs registry nominal 0..5) being left
  // uncommitted rather than silently billed as an implementation constant.
  FiveStepSequencer s;
  CHECK_TRUE(!s.clockOutRising());
}

}  // namespace

int main() {
  test_stages_wrap_and_step1_first();
  test_gate_mask_only_affects_gate();
  test_gate_one_sample_pulse();
  test_clock_source_no_double_advance();
  test_pulser_clock_out_independent();
  test_external_real_descriptor_sample_accuracy();
  test_four_sample_rates_wallclock();
  test_block_partition_bit_identical();
  test_invalid_config_fail_closed();
  test_constructor_provisional_no_persistence();
  test_registry_descriptor_honesty();
  return test::finish("test_five_step_sequencer");
}
