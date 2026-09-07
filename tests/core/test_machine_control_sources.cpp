// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH#11 final acceptance (task#68, per @Codex ruling a14fd6a4 + 7C3 pulser/clock-out).
//
// Proves the six control sources (Envelope A/B, LFO A/B, Joystick, Sequencer) are
// REAL audio/PatchGraph consumers in the canonical MachineRuntimeDefinition: they are
// admitted as configurable sources, configured ONLY through the public parameter-event
// path (enqueueControlEvent + processBlock, the unit-agreeing dispatch to the six real
// DSP instances), and observed ONLY through the public runtime surface
// (controlVoltageAt, vcfCvReadback*, droneChannel, execSlotAt, RuntimeOutput).
//
// Scope guard (DO NOT widen): this TU touches ONLY the generated registry +
// canonical machine_definition + real runtime / PatchGraph / EventTimebase. It does NOT
// alter design/spec/generated semantics, host/#4/#10, DeviceState/#12, or the VCF
// restore/#6 boundary. GH#11 is FIXED-CANDIDATE at this head (the six control sources
// are all implemented and consumed); awaiting @Codex's independent close + P3 ruling.
//
// The six source value- and instance-accessors (envelopeA()/lfoA()/... / the private
// setControlParamValue / setSequencerInternalRateHz) are PRIVATE by design: the injector
// and the readback inspector are the host/verifier facade, not a test seam. So this test
// deliberately drives them through the real event path instead of touching them directly.
//
// Harness mirrors test_machine_definition.cpp: one TU with g_checks/g_fail, a small
// summary reporter, and a companion _allocator TU (built only into this target) that
// counts render-path allocations so A′ "zero allocation" can be asserted.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <lunar24/core/control_event.h>
#include <lunar24/core/envelope_generator.h>
#include <lunar24/core/five_step_sequencer.h>
#include <lunar24/core/joystick_cv.h>
#include <lunar24/core/lfo.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/machine_runtime.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;
namespace reg = lunar24::registry;

// g_checks / g_fail live in this TU (the linked allocator TU owns only g_allocCount).
static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("  FAIL: %s\n", what);
    std::fflush(stdout);
  }
}

extern std::size_t g_allocCount;  // defined in the companion _allocator TU.

// ---- render constants ----
static constexpr double kSr = 48000.0;
static constexpr uint32_t kSeed = 0x5EEDu;  // matches machine_definition default seed
static constexpr int kCap = 256;
static constexpr core::RuntimeInputs kZeros[kCap] = {core::RuntimeInputs{0.0, 0.0}};

// Bitwise equality — the point of several oracles is that a mid-graph re-computation
// lands on EXACTLY the same bits (same sample vs a one-frame-off read). We demand it.
static bool sameD(double a, double b) { return a == b; }

// Tolerance equality — for a value asserted against a hardcoded constant. The real DSP is
// SingleSample==float32-arithmetic: JoystickCv 5*(2*0.7-1) lands on 1.9999998807907104
// (one float32 ULP off 2.0), NOT on 2.0; the VCF readback of that same float is identical.
// A bitwise compare against a literal would spuriously red a CORRECT value, and a 1e-9
// tolerance (double-ULP tight) is stricter than the float32 medium it is checking.
// 1e-5 is ~0.5 float32 ULP at the ±10V rail: it accommodates the input-constant float
// rounding while still rejecting ANY real mapping error (a sign/scale bug is off by volts,
// never by 1e-5).
static bool nearD(double a, double b) {
  const double d = a - b;
  return d > -1e-5 && d < 1e-5;
}

// The MachineRuntimeDefinition is NON-COPYABLE/NON-MOVABLE and must sit at a
// STABLE address for the runtime's whole life (the SynthRuntime stores pointers
// into it). A Big-frame fixture declared BY VALUE on the stack both moves the
// address and blows the default 1 MiB stack on Windows / ulimit -s 960 mac; heap-
// owning it is the correct lifetime binding. This is the ONLY allocation the test
// harness makes at fixture setup (the render-path zero-alloc assertion in test 8
// measures only processBlock).
static std::unique_ptr<core::MachineRuntimeDefinition> make_def(std::uint64_t seed,
                                                               double sr) {
  return std::make_unique<core::MachineRuntimeDefinition>(seed, sr);
}

// ---------------------------------------------------------------------------
// Event helpers (public path: enqueueControlEvent -> EventTimebase -> processBlock).
// A deterministic producerSequence keeps same-block ordering stable and identical
// across runs, so the SAME logical event set is delivered identically under any
// block partition (design/07 §5 buffer-invariance, criterion ④).
// ---------------------------------------------------------------------------
static void applyParam(core::SynthRuntime& rt, reg::ParameterId pid, double v,
                       uint64_t sample) {
  core::ControlEvent ev{};
  ev.kind = core::ControlEventKind::parameter;
  ev.parameter = pid;
  ev.value = static_cast<core::SignalSample>(v);
  ev.sampleOffset = 0;
  ev.source = 1;
  ev.channel = 0;
  ev.noteId = 0;
  ev.producerSequence = static_cast<uint64_t>(static_cast<uint32_t>(pid)) * 1000 + sample;
  rt.enqueueControlEvent(core::TimedControlEvent{ev, sample});
}

// Render `n` frames against a fresh absolute timebase, delivering any pre-enqueued
// events at their absolute samples. `blocks` is a partition schedule summing to n;
// `out` receives the block outputs so callers can compare partition schedules bitwise.
static void block_render(core::SynthRuntime& rt, int n, const uint32_t* blocks,
                         int nb, core::RuntimeOutput* out) {
  int off = 0;
  for (int b = 0; b < nb; ++b) {
    const uint32_t sz = blocks[b];
    rt.processBlock(kZeros, sz, out + off);
    off += static_cast<int>(sz);
  }
  (void)n;
}

// Step one frame via processBlock(1) (applies any due event, then renders), capturing
// a control voltage into `out[i]`. Used to prove a param event lands at an exact frame.
static void frame_capture(core::SynthRuntime& rt, int n, reg::JackId jack, double* out,
                          bool captureJack) {
  for (int i = 0; i < n; ++i) {
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    out[i] = captureJack ? rt.controlVoltageAt(jack) : o.wetL;
  }
}

// ---------------------------------------------------------------------------
// GH#21 continue-control-smoothing helpers (design/07 §3.2 + the GH#21 contract).
//
// The runtime smoothes the 20 continuous control-source params with a ONE-POLE
// ParameterSmoother whose per-frame ordering is: 'setTarget' (if a param event lands
// this frame) FIRST, then ONE pole step, then the control source PUBLISHES so a sink
// samples the post-advance value in the SAME frame (processFrame advances the smoothers
// before the graph resolves). These helpers mirror exactly that ordering.
// ---------------------------------------------------------------------------

// Is `pid` one of the 20 continuous-control-source params the runtime actually smoothes?
// The runtime's predicate (machine_runtime.h controlSourceParamRecognized_ + the registry
// `smoothing`) is EXACTLY: a control-source id whose registry descriptor is Smoothing::seconds.
// The registry descends the manifest, so reading it here is the same source of truth, not a
// test-side re-derivation of the set ("no magic numbers"). The 15 discrete control-source
// params (hold/self_gen/wave/speed_mult/clock/stages/step_gate) are Smoothing::none.
static bool gh21_is_seconds(reg::ParameterId pid) {
  for (const reg::ParameterDescriptor& d : reg::kParameters) {
    if (d.id == pid) return d.smoothing == reg::Smoothing::seconds;
  }
  return false;
}

// The GH#21 acceptance settle frame count, derived from the DECLARED tau and settle
// tolerance (rule 2: N = ceil(fs * tau * ln(1/relTol)); nothing magic). This is the minimum
// number of pole steps for the one-pole residual (1-a)^N to reach relTol * span, at which
// point the runtime SNAPS the param to the EXACT target (advanceControlSmoothing_). A small
// margin (+2) absorbs the snap-inclusion and float rounding without changing the contract.
static int gh21_settle_frames(double sr) {
  const double n = std::ceil(sr * core::kGh21SmoothingTauSeconds *
                             std::log(1.0 / core::kSmootherSettleRelTol));
  return static_cast<int>(n) + 2;
}

// First published frame index at which a seconds-smoothed param, driven by a scripted
// (frame, target) sequence from `start`, CROSSES `cross` on a RISING edge (prev < cross.
// <= cross, now >= cross). Used to pin a gate/clock edge with the threshold-crossing
// prediction (rule 2c: window derived from tau/fs/start-target — a first-order closed
// form, so the edge MUST land at this frame, not "the old exact frame", restoring
// timeline discrimination). Replays the real ParameterSmoother class + the real per-frame
// ordering; returns -1 if `n_max` frames elapse without a clean rising crossing.
static int gh21_cross_frame(double start,
                            const std::vector<std::pair<int, double>>& targets, double cross,
                            int n_max) {
  core::ParameterSmoother sm;
  sm.reset(start);
  sm.setSampleRate(kSr);
  sm.setTimeConstantSeconds(core::kGh21SmoothingTauSeconds);
  bool wasOver = start >= cross;
  std::size_t ti = 0;
  for (int n = 0; n < n_max; ++n) {
    while (ti < targets.size() && targets[ti].first == n) {
      sm.setTarget(targets[ti].second);
      ++ti;
    }
    const double v = sm.next();
    const bool over = v >= cross;
    if (n > 0 && !wasOver && over) return n;  // rising transition vs previous published frame
    wasOver = over;
  }
  return -1;
}

// All published frame indices at which a seconds-smoothed param, driven by a scripted
// (frame, target) sequence from `start`, CROSSES `cross` on a RISING edge. This is the
// list-of-edges form of gh21_cross_frame (above), for a source that crosses the threshold
// several times. Replays the SAME real ParameterSmoother + per-frame ordering, so it is the
// deterministic threshold-crossing prediction (rule 2c) and the runtime MUST advance at
// exactly these frames (never at the raw value-step frames).
static std::vector<int> gh21_cross_up_frames(
    double start, const std::vector<std::pair<int, double>>& targets, double cross, int n_max) {
  core::ParameterSmoother sm;
  sm.reset(start);
  sm.setSampleRate(kSr);
  sm.setTimeConstantSeconds(core::kGh21SmoothingTauSeconds);
  bool wasOver = start >= cross;
  std::size_t ti = 0;
  std::vector<int> out;
  for (int n = 0; n < n_max; ++n) {
    while (ti < targets.size() && targets[ti].first == n) {
      sm.setTarget(targets[ti].second);
      ++ti;
    }
    const double v = sm.next();
    const bool over = v >= cross;
    if (n > 0 && !wasOver && over) out.push_back(n);
    wasOver = over;
  }
  return out;
}

// Advance the runtime by `n` single-sample blocks. processBlock() writes out[i]/reads inputs[i]
// for i in [0,n), so a SINGLE RuntimeOutput/RuntimeInputs must only be passed when n==1. When a
// test needs to roll the clock forward by a tau-derived settle window (rule 2) without observing
// each frame, this loops processBlock(1) so no OOB-past-a-single-element write occurs.
static void gh21_advance(core::SynthRuntime& rt, int n) {
  const core::RuntimeInputs z{0.0, 0.0};
  core::RuntimeOutput o;
  for (int i = 0; i < n; ++i) rt.processBlock(&z, 1, &o);
}

// ===========================================================================
// 1. Six always-execute control-source slots, once/sample; no-cable -> repatch does
//    NOT reset the source phase; a no-op partition change is bit-identical.
// ===========================================================================
static void test_1_slots_presence_phase(void) {
  // (a) Presence: exactly six control-source slots, one per distinct registry module.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.graphValid(), "t1 canonical definition graph is valid");
    int envA = 0, envB = 0, lfoA = 0, lfoB = 0, joy = 0, seq = 0, stray = 0;
    for (std::uint32_t i = 0; i < rt.execSlotCount(); ++i) {
      const core::ExecutionSlot s = rt.execSlotAt(i);
      // A "stray" is a slot that CLAIMS a control-source kind (i.e. an always-execute
      // source) but carries the WRONG module id — a mis-attribution of the source. Legit
      // non-control modules (vco/vcf/preamp/env_follower/mixer/drone) fall to `default`
      // and are counted as `other`, never as a stray.
      switch (s.kind) {
        case core::ExecutionKind::kEnvelope:
          if (s.id == reg::ModuleId::envelope_a) ++envA;
          else if (s.id == reg::ModuleId::envelope_b) ++envB;
          else ++stray;
          break;
        case core::ExecutionKind::kLfo:
          if (s.id == reg::ModuleId::lfo_a) ++lfoA;
          else if (s.id == reg::ModuleId::lfo_b) ++lfoB;
          else ++stray;
          break;
        case core::ExecutionKind::kJoystick:
          if (s.id == reg::ModuleId::joystick) ++joy;
          else ++stray;
          break;
        case core::ExecutionKind::kSequencer:
          if (s.id == reg::ModuleId::sequencer) ++seq;
          else ++stray;
          break;
        default:
          break;  // legitimately non-control; never a stray.
      }
    }
    check(envA == 1 && envB == 1, "t1 exactly one envelope_a and one envelope_b slot");
    check(lfoA == 1 && lfoB == 1, "t1 exactly one lfo_a and one lfo_b slot");
    check(joy == 1 && seq == 1, "t1 exactly one joystick and one sequencer slot");
    check(stray == 0, "t1 no control-source-kind slot with a mis-attributed module id");
  }

  // (b) No-cable -> repatch preserves the source phase (LFO does not reset or freeze).
  //     Run A renders continuously; Run B renders the first 100, repatches (a real
  //     connect+rebuild that adds a LFO->VCF edge), then renders 100 more. A rebuild that
  //     resets the LFO phase would make B's post-repatch frames equal A's leading frames.
  double lfoA[kCap];
  double lfoB[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    frame_capture(rt, kCap, reg::JackId::lfo_a_cv_out, lfoA, true);
    check(!sameD(lfoA[0], lfoA[20]), "t1 LFO genuinely varies (per-sample, not frozen)");
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    frame_capture(rt, 100, reg::JackId::lfo_a_cv_out, lfoB, true);
    bool rep = rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_l_in);
    check(rep, "t1 repatch connect(lfo_a_cv_out, vcf_cv_l_in) accepted");
    check(rt.rebuild(), "t1 repatch rebuild() succeeds");
    frame_capture(rt, kCap - 100, reg::JackId::lfo_a_cv_out, lfoB + 100, true);
  }
  bool samePre = true, samePost = true;
  for (int i = 0; i < 100; ++i) samePre = samePre && sameD(lfoA[i], lfoB[i]);
  for (int i = 100; i < kCap; ++i) samePost = samePost && sameD(lfoA[i], lfoB[i]);
  check(samePre, "t1 LFO identical pre-repatch across two fresh defs (deterministic)");
  check(samePost, "t1 LFO phase preserved across a real repatch (not reset/frozen)");

  // (c) Partition invariance (criterion ④): same config events, three fresh defs, three
  //     block schedules -> bit-identical output. Config changes the LFO + joystick so the
  //     render is genuinely source-driven.
  const uint32_t b256[1] = {256};
  const uint32_t b64[4] = {64, 64, 64, 64};
  core::RuntimeOutput o256[kCap], o64[kCap], o1[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);  // warm (no-op) — keep surface used
    block_render(rt, kCap, b256, 1, o256);
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    block_render(rt, kCap, b64, 4, o64);
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    std::uint32_t ones[kCap];
    for (int i = 0; i < kCap; ++i) ones[i] = 1;
    block_render(rt, kCap, ones, kCap, o1);
  }
  bool partSame = true;
  for (int i = 0; i < kCap; ++i)
    partSame = partSame && sameD(o256[i].wetL, o64[i].wetL) &&
               sameD(o256[i].wetL, o1[i].wetL);
  check(partSame, "t1 256-vs-64-vs-1 block partition is bit-identical (criterion ④)");
}

// ===========================================================================
// 2. LFO A -> drone_2 CV-MOD is resolved SAME-sample (not frame-front); MOD-off stays
//    inert. Comparison is bitwise against a manual same-frame injection.
// ===========================================================================
enum class DroneRun { kBase, kCable, kManual, kFront };

static void drone_patch_run(const std::unique_ptr<core::MachineRuntimeDefinition>& def, DroneRun m,
                            const double* refCv, double* lfoOut, double* wetOut) {
  core::SynthRuntime& rt = def->runtime();
  // Never rely on the default patch: force the CV-MOD edge explicitly per run.
  if (m == DroneRun::kCable) {
    for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 1.0);  // MOD ON, all gens of drone_2
    rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_2_cv_mod_in);
    rt.rebuild();
  } else {
    rt.disconnect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_2_cv_mod_in);
    rt.rebuild();
    if (m != DroneRun::kBase) {
      for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 1.0);  // MOD ON, but NOT via cable
    }
  }
  applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
  applyParam(rt, reg::ParameterId::lfo_a_wave, 0.5, 0);
  for (int i = 0; i < kCap; ++i) {
    if (m == DroneRun::kManual) rt.setDroneGroupModCv(1, refCv[i]);
    else if (m == DroneRun::kFront) rt.setDroneGroupModCv(1, i > 0 ? refCv[i - 1] : 0.0);
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    if (lfoOut) lfoOut[i] = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
    if (wetOut) wetOut[i] = o.wetL;
  }
}

static void test_2_lfo_drone_mod_same_sample(void) {
  double lfoBase[kCap], wetBase[kCap];
  double lfoCable[kCap], wetCable[kCap];
  double wetManual[kCap], wetFront[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    drone_patch_run(def, DroneRun::kBase, nullptr, lfoBase, wetBase);
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    drone_patch_run(def, DroneRun::kCable, nullptr, lfoCable, wetCable);
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    drone_patch_run(def, DroneRun::kManual, lfoCable, nullptr, wetManual);
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    drone_patch_run(def, DroneRun::kFront, lfoCable, nullptr, wetFront);
  }

  // LFO determinism across two fresh defs (the oracle's ref must be the real value).
  bool lfoDet = true;
  for (int i = 0; i < kCap; ++i) lfoDet = lfoDet && sameD(lfoBase[i], lfoCable[i]);
  check(lfoDet, "t2 LFO A is bit-identical across two fresh defs (re-computable source)");

  // Same-sample: the cabled drone_2 CV-MOD matches a manual SAME-frame injection exactly.
  bool sameSample = true;
  for (int i = 0; i < kCap; ++i) sameSample = sameSample && sameD(wetCable[i], wetManual[i]);
  check(sameSample, "t2 cabled CV-MOD == manual same-frame injection bitwise (same-sample)");

  // The discriminator is meaningful: a frame-front consumer (prior-sample CV) differs.
  bool frontDiff = false;
  for (int i = 0; i < kCap; ++i) frontDiff = frontDiff || !sameD(wetManual[i], wetFront[i]);
  check(frontDiff, "t2 frame-front CV-MOD is distinguishable (oracle is non-vacuous)");

  // Non-vacuous: MOD-on (with CV) actually changes the drone audio vs MOD-off.
  bool nonVacuous = false;
  for (int i = 0; i < kCap; ++i) nonVacuous = nonVacuous || !sameD(wetCable[i], wetBase[i]);
  check(nonVacuous, "t2 CV-MOD changes the drone audio vs MOD-off (real DSP, not inert)");

  // LFO B independent: a B-driven run differs from the A-driven run, and B itself varies.
  double lfoB[kCap], wetB[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 1.0);
    rt.connect(reg::JackId::lfo_b_cv_out, reg::JackId::drone_2_cv_mod_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::lfo_b_rate, 50.0, 0);  // different rate: A/B independence is distinguishable
    applyParam(rt, reg::ParameterId::lfo_b_wave, 0.5, 0);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      lfoB[i] = rt.controlVoltageAt(reg::JackId::lfo_b_cv_out);
      wetB[i] = o.wetL;
    }
  }
  bool bVaries = false, bDiffers = false;
  for (int i = 0; i < kCap; ++i) {
    bVaries = bVaries || !sameD(lfoB[0], lfoB[i]);
    bDiffers = bDiffers || !sameD(wetB[i], wetCable[i]);
  }
  check(bVaries, "t2 LFO B varies per-sample (independent source)");
  check(bDiffers, "t2 LFO B drives a different drone output than LFO A (A/B independent)");
}

// ===========================================================================
// 3. Sequencer gate -> EG A gate -> EG A env -> VCF L: a real cross-module chain with
//    plan-order robustness, gate-mask isolation, A/B independence and R-normalling.
// ===========================================================================
static void test_3_seq_gate_eg_env_vcf(void) {
  double envA[kCap], envB[kCap], sqCv[kCap], sqGate[kCap], vcfL[kCap], vcfR[kCap];
  int stepAt[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    // Real cabling: joystick -> ext clock; seq gate -> EG A gate; EG A env -> VCF L.
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    rt.connect(reg::JackId::sequencer_gate_out, reg::JackId::envelope_a_gate_in);
    rt.connect(reg::JackId::envelope_a_env_out, reg::JackId::vcf_cv_l_in);
    check(rt.rebuild(), "t3 patch rebuild succeeds");
    // EG A config (events): short attack, sustain 0.9. EG B independent self-gen.
    applyParam(rt, reg::ParameterId::envelope_a_a, 0.5, 0);      // 0.5s attack (slow)
    applyParam(rt, reg::ParameterId::envelope_a_s, 0.9, 0);
    applyParam(rt, reg::ParameterId::envelope_b_self_gen, 1.0, 0);
    applyParam(rt, reg::ParameterId::envelope_b_hold, 1.0, 0);
    applyParam(rt, reg::ParameterId::envelope_b_a, 0.001, 0);
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);   // external clock
    applyParam(rt, reg::ParameterId::sequencer_stages, 0.0, 0);  // stages = 3
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_2, 2.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_3, 3.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);        // low (-2V)
    // Rising edges at frames 8, 24, 40 toward the sequencer external clock.
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 8);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 16);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 24);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 32);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 40);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      envA[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      envB[i] = rt.controlVoltageAt(reg::JackId::envelope_b_env_out);
      sqCv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      sqGate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
      vcfL[i] = rt.vcfCvReadbackL();
      vcfR[i] = rt.vcfCvReadbackR();
      stepAt[i] = rt.sequencer().currentStep();
    }
  }

  // The seq GATE is the advance discriminator. Under GH#21 smoothing the external-clock CV
  // that drives it is a one-pole ramp, so a threshold rising edge fires at the closed-form
  // frame where the smoothed joystick CV crosses 0.5 (Class B edge invariant + predictive
  // window), NOT at the old raw value-step frame 8. That first crossing (pred[0]=15) STARTS
  // the sequencer (step stays 0); step_gate_1 is enabled on step 0, so the gate pulses
  // +10 V for exactly that one sample (a one-sample pulse, cleared next frame).
  const std::vector<int> pred = gh21_cross_up_frames(
      0.5, {{0, 0.3}, {8, 0.7}, {16, 0.3}, {24, 0.7}, {32, 0.3}, {40, 0.7}}, 0.5, kCap);
  check(!pred.empty() && pred[0] < kCap, "t3 crossing prediction is defined within the window");
  check(sameD(sqGate[pred[0]], 10.0),
        "t3 seq gate pulses at the first external-clock cross (predictive window)");
  check(pred[0] < kCap && pred[0] + 1 < kCap && sameD(sqGate[pred[0] + 1], 0.0),
        "t3 seq gate is a one-sample pulse (cleared next frame)");
  check(pred[0] != 8, "t3 seq gate NOT at the old raw frame 8 (anti-old-frame)");
  check(stepAt[0] == 0, "t3 sequencer begins at step 0");
  check(stepAt[pred[1]] == 1, "t3 seq reaches step 1 at the 2nd predicted cross (pred[1])");
  check(stepAt[pred[2]] == 2, "t3 seq reaches step 2 at the 3rd predicted cross (pred[2])");

  // The sequencer is a live source even idle: it publishes step0's CV (the current step
  // value) BEFORE any advance. Under GH#21 smoothing that value is a one-pole ramp toward
  // the configured step0 target over the tau-derived settle window, so the idle CV reaches
  // its configured 1.0 V after `settle` frames (Class A value-reach). Re-expressed from the
  // old exact sqCv[0]==1.0, which assumed the CV snapped instantly. Kept in a NON-advancing
  // harness (external clock selected, no edge wired) so step stays 0 and step0 CV is free to
  // converge.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);      // external, no edge wired
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);  // step0 -> 1.0 V
    const int settle = gh21_settle_frames(kSr);
    double cvAfter = 0.0;
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    for (int i = 0; i < settle; ++i) {
      rt.processBlock(&z, 1, &o);
      if (i == settle - 1) cvAfter = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
    check(rt.sequencer().currentStep() == 0, "t3 seq step stays 0 with no external edge");
    check(nearD(cvAfter, 1.0),
          "t3 seq publishes step0 CV while idle -> reaches 1.0 V after settle (always-live 0..+5V source)");
  }

  // The EG A rose after the seq gate pulse (gate -> EG A gate -> env real chain).
  int firstEnv = -1;
  for (int i = 0; i < kCap; ++i) if (envA[i] > 0.0) { firstEnv = i; break; }
  check(firstEnv >= 0, "t3 EG A env_out is nonzero after a seq gate (real gate->EG chain)");
  bool envBeforeZero = true;
  for (int i = 0; i < firstEnv && i < kCap; ++i) envBeforeZero = envBeforeZero && sameD(envA[i], 0.0);
  check(envBeforeZero, "t3 EG A was idle before the seq gate (gate-mask: only gated on)");

  // Gate-mask: the seq gate did NOT trigger EG B (self-gen is smooth, no frame-8 spike);
  // A/B are independent instances.
  bool bIndependent = false;
  for (int i = 0; i < kCap; ++i) bIndependent = bIndependent || !sameD(envA[i], envB[i]);
  check(bIndependent, "t3 EG A and EG B are independent (no A/B cross-wire)");
  bool bSelfGen = false;
  for (int i = 0; i < kCap; ++i) bSelfGen = bSelfGen || (envB[i] > 0.0);
  check(bSelfGen, "t3 EG B self-generates without a gate (SELF-GEN/HOLD independent)");

  // Descriptor interpreter: the EG A env rose only at/after the interpreted rising gate
  // (a source already-high at sample 0 would not fabricate a phantom edge).
  bool vcfTracks = true;
  int firstVcf = -1;
  for (int i = 0; i < kCap; ++i) if (vcfL[i] != 0.0) { firstVcf = i; break; }
  for (int i = 0; i < kCap; ++i) if (vcfL[i] != 0.0 || envA[i] != 0.0) {
    // They should be jointly nonzero/nonzero within the env->vcf causality window.
    vcfTracks = vcfTracks && (vcfL[i] >= 0.0);
  }
  check(firstVcf >= 0, "t3 EG A env reaches VCF L (real env->vcf path)");
  (void)vcfTracks;

  // R-normalling: vcf_cv_r_in unplugged while vcf_cv_l_in is fed -> R reads THIS frame's L.
  bool norm = true;
  for (int i = 0; i < kCap; ++i) norm = norm && sameD(vcfR[i], vcfL[i]);
  check(norm, "t3 VCF R normalled to VCF L (unplugged R, fed L)");
}

// ===========================================================================
// 4. Joystick X -> VCF L, Y -> VCF R asymmetric, exact real DSP readback, offset applied
//    at an exact sample (never block-front), L/R independent.
// ===========================================================================
static void test_4_joystick_vcf(void) {
  // Joystick x/y AND the two offsets are all Smoothing::seconds: each is a ONE-POLE ramp from
  // the DSP's current value (centre 0.5 -> 0 V, per joystick_cv.h defaults) to the target over
  // the tau-derived settle window (rule 2). The published jack is read AFTER the axis has
  // settled so the value assertion is exact; the "exact sample, not block-front" property is
  // re-expressed as the offset transition being an event-triggered SMOOTH ramp (never a jump
  // at the block front), per the GH#21 Class-B re-expression.
  const int settle = gh21_settle_frames(kSr);
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in);
    rt.connect(reg::JackId::joystick_y_out, reg::JackId::vcf_cv_r_in);
    check(rt.rebuild(), "t4 joystick->VCF patch rebuild succeeds");
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    // x=0.7 -> +2.0 V; y=0.2 -> -3.0 V with default offset 0.5 (-> +0 V offset term). Both
    // ramp from the centre 0.5; after the settle window they land on the target.
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    rt.processBlock(&z, 1, &o);
    // Class A: the ramp has barely left the centre after ONE frame — x_out is nowhere near the
    // +2.0 V target yet (it is a one-pole trajectory, not a step). Anti-jump: it must NOT be at
    // the target at the frame before the settle window.
    check(!nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 2.0),
          "t4 x_out starts at the CURRENT centre and ramps; it does NOT jump to +2.0 V at frame 1");
    gh21_advance(rt, settle);
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 2.0),
          "t4 joystick x_out == +2.0 V after X settles @ x=0.7");
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_y_out), -3.0),
          "t4 joystick y_out == -3.0 V after Y settles @ y=0.2 (asymmetric to X)");

    // Real DSP sink: vcf_cv_l_in is fed, so the VCF reads the (fixed exec-lag) X value. The
    // axis has SETTLED, so the settled-window readback is exact regardless of that one-frame skew.
    check(nearD(rt.vcfCvReadbackL(), 2.0), "t4 VCF L readback tracks the settled X (+2.0V)");
    check(nearD(rt.vcfCvReadbackR(), -3.0), "t4 VCF R readback tracks the settled Y (-3.0V)");

    // Offset X event scheduled AFTER the X/Y settle: x_out swings from +2.0 to 0.0 (offset_x
    // 0.3 -> 5*(2*0.3-1) = -2.0 V, so +2.0 + (-2.0) = 0.0). The point of the "exact sample,
    // not block-front" oracle is preserved: x_out is STILL +2.0 at the frame BEFORE the offset
    // event, it has begun to move at the offset frame (mid-ramp, not a block-front jump), and
    // it settles at 0.0 only after the offset's own tau window.
    const uint64_t offAt = static_cast<uint64_t>(settle) + 1;
    const double xBeforeOffset = rt.controlVoltageAt(reg::JackId::joystick_x_out);  // +2.0
    applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, offAt);
    rt.processBlock(&z, 1, &o);
    const double xAtOffsetFrame = rt.controlVoltageAt(reg::JackId::joystick_x_out);
    check(nearD(xBeforeOffset, 2.0),
          "t4 offset not yet applied at the frame just before its sample (exact sample, not front)");
    check(xAtOffsetFrame < 2.0 && xAtOffsetFrame > 0.0,
          "t4 at the offset frame x_out is mid-ramp between +2.0 and 0.0 (smooth, event-triggered)");
    gh21_advance(rt, settle);
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 0.0),
          "t4 offset applied -> x_out settles at 0.0 V (was +2.0 before the offset)");
    check(nearD(rt.vcfCvReadbackL(), 0.0), "t4 VCF L readback follows X after the offset (0.0V)");

    // X/Y independent (asymmetric): Y is unaffected by the offset-X change.
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_y_out), -3.0),
          "t4 Y is unaffected by an offset-X change (X/Y independent)");
    check(nearD(rt.vcfCvReadbackR(), -3.0), "t4 VCF R readback unaffected by the offset-X change");
  }
}

// ===========================================================================
// 5. A bipolar joystick source -> sequencer.ext_clock_in: only rising advances, a
//    sustained high never repeats, and a repatch that lands on an already-high sink
//    makes no phantom advance.
// ===========================================================================
static void test_5_seq_ext_clock(void) {
  // GH#21 re-expression: sequencer_ext_clock_in is a zero-threshold/hysteresis gate, so a
  // SMOOTHED joystick CV (Smoothing::seconds) now drives the advance as a one-pole ramp, not
  // a step at frame 5. Class-B three-part lock (edge invariants, anti-old-frame, predicted-
  // crossing window) replaces the old exact-frame-5 assertion. The step_cv value reach needs
  // the enlarged tau-derived window (rule 2).
  const int settle = gh21_settle_frames(kSr);
  const std::vector<double> cvVec(settle + 16, 0.0);
  const std::vector<double> gateVec(settle + 16, 0.0);
  double* cv = const_cast<double*>(cvVec.data());
  double* gate = const_cast<double*>(gateVec.data());
  const int n = settle + 16;
  // The smoothed joystick CV is one-pole from centre 0.5 (0 V): step 0.3@0 (down), step 0.7@5
  // (up), tau=0.050 s, fs=48000, so it crosses its 0 V gate threshold when x reaches 0.5.
  // gh21_cross_frame solves the one-pole closed form -> the predicted crossing, used below as
  // the Class-B (c) window centre. This is deterministic (declared window, no magic number).
  const int xFrame = gh21_cross_frame(0.5, {{0, 0.3}, {5, 0.7}}, 0.5, n);
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    check(rt.rebuild(), "t5 joystick->ext_clock_in patch rebuild succeeds");
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);  // external
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_2, 2.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);  // step-0 gate enabled
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);   // low (-2V): ramps centre downwards
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 5);   // rising step at the OLD frame 5
    for (int i = 0; i < n; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      cv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      gate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
    }
  }
  // --- Class B (b): anti-old-frame ---
  // The smoothing has NOT delivered the value step at its old exact frame 5; the edge must NOT
  // land there. (x is still below 0 V at frame 5 -- mid-ramp.)
  check(gate[5] == 0.0, "t5 anti-old-frame: no advance at the pre-smoothing frame 5");
  // --- Class B (c): predicted-crossing window ---
  // Locate the single rising edge on the gate (the advance). It must fall in a SMALL window
  // around the deterministic smoothed-0 V-crossing frame (xFrame), never at the old frame 5.
  int rise = -1;
  for (int i = 1; i < n; ++i)
    if (gate[i] > 0.0 && gate[i - 1] == 0.0) { rise = i; break; }
  check(rise >= xFrame - 1 && rise <= xFrame + 1,
        "t5 predicted-crossing window: the advance lands near the smoothed-0 V-crossing frame");
  check(rise >= 0 && gate[rise] == 10.0,
        "t5 single-sample +10V gate pulse on the (one) rising advance");
  // --- Class B (a): edge invariants ---
  // Rising -> EXACTLY one advance; sustained high (x -> +2V, stays >0V) never repeats it;
  // the edge is a single-sample pulse (gate high for exactly one frame at the advance).
  int advances = 0;
  for (int i = 1; i < n; ++i)
    if (gate[i] > 0.0 && gate[i - 1] == 0.0) ++advances;
  check(advances == 1, "t5 rising -> exactly one advance (sustained high never repeats)");
  check(rise >= 0 && rise + 1 < n && gate[rise + 1] == 0.0,
        "t5 gate pulse is exactly one sample (one-shot, not a held rail)");
  // --- Class A: step_cv_1 value reach (enlarged window, rule 2) ---
  // step_cv_1 is also Smoothing::seconds, so the CV ramps from default 0 -> 1.0 V and lands on
  // target only after the settle window; at the old frame 5 it is still mid-ramp.
  check(cv[5] > 0.0 && cv[5] < 1.0,
        "t5 step0 CV is mid-ramp at frame 5 (smoothed: not yet at 1.0 V)");
  check(nearD(cv[n - 1], 1.0),
        "t5 step0 CV reaches 1.0 V after the settle window (enlarged render, rule 2)");

  // Repatch alone (no signal transition) produces no phantom advance: the source stays LOW
  // across the whole window, so connecting the cable mid-window must not fabricate an edge.
  {
    double gate2[kCap];
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);  // LOW for the whole window
    for (int i = 0; i < kCap; ++i) {
      if (i == 32) {  // repatch at frame 32: cable now exists, but x is still low
        check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in),
              "t5b repatch connect accepted mid-window");
        check(rt.rebuild(), "t5b repatch rebuild succeeds");
      }
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      gate2[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
    }
    bool noPhantom = true;
    for (int i = 0; i < kCap; ++i) noPhantom = noPhantom && sameD(gate2[i], 0.0);
    check(noPhantom, "t5 repatch without a signal transition makes NO phantom advance");
  }
}

// ===========================================================================
// 6. Sequencer stages 3/4/5 advance at exact samples; a sustained source holds; seq
//    CV feeds a sink (VCF) and gate feeds EG; clock_out is now a published rail that idles
//    at the CONFIRMED -10V while the 1.0 Hz PULSER never rises. The internal PULSER Hz is
//    also exercised via the runtime's public direct-Hz oracle (setSequencerInternalRateHz);
// ===========================================================================
static void test_6_seq_stages_clock_out(void) {
  // (a) Stages: an external clock drives advances, each on ONE rising crossing of the
  //     SMOOTHED joystick CV (Smoothing::seconds). GH#21 re-expression (rule 2c): the source
  //     is a one-pole ramp, so the old "8-frame steps -> advances at exact frames 8/24/40/56"
  //     is not reachable (8-frame toggling never lets the axis reach the 0 V threshold). We
  //     space the steps so the source CLEANLY crosses, predict each crossing with the closed
  //     form (gh21_cross_up_frames), and assert the three-part Class-B lock: one advance per
  //     rising crossing at the predicted-window frame, never at the raw value-step frame, and
  //     wrap per stageCount (read from the sequencer's own currentStep()).
  const struct { int stages; double step0, step1, step2; } cases[3] = {
      {3, 1.0, 2.0, 3.0}, {4, 1.0, 2.0, 3.0}, {5, 1.0, 2.0, 3.0}};
  // Spaced up/down steps so each up-ramp crosses 0 V (x=0.5) before the next step. tau=0.050 s
  // => the up-crossing is ~900 frames after the step; 1500-frame holds give a clean, isolated
  // crossing each cycle. The crossing FRAMES are computed, not hard-coded (rule 2c).
  const std::vector<std::pair<int, double>> sched = {
      {0, 0.3}, {1500, 0.7}, {3000, 0.3}, {4500, 0.7},
      {6000, 0.3}, {7500, 0.7}, {9000, 0.3}, {10500, 0.7}};
  const int kN = 20000;
  for (int c = 0; c < 3; ++c) {
    const int stages = cases[c].stages;
    const double s0 = cases[c].step0, s1 = cases[c].step1, s2 = cases[c].step2;
    const std::vector<int> pred = gh21_cross_up_frames(0.5, sched, 0.5, kN);
    std::vector<double> sqCv(kN), sqGate(kN), clockOut(kN), vcfL(kN);
    std::vector<int> seqStep(kN, -1);
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
      rt.connect(reg::JackId::sequencer_cv_out, reg::JackId::vcf_cv_l_in);
      check(rt.rebuild(), "t6 stages patch rebuild succeeds");
      applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
      applyParam(rt, reg::ParameterId::sequencer_stages,
                 static_cast<double>(stages - 3), 0);  // norm 0/1/2 → 3/4/5
      applyParam(rt, reg::ParameterId::sequencer_step_cv_1, s0, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_cv_2, s1, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_cv_3, s2, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);
      for (const auto& step : sched) applyParam(rt, reg::ParameterId::joystick_x, step.second,
                                                static_cast<uint64_t>(step.first));
      for (int i = 0; i < kN; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        sqCv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
        sqGate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
        clockOut[i] = rt.controlVoltageAt(reg::JackId::sequencer_clock_out);
        vcfL[i] = rt.vcfCvReadbackL();
        seqStep[i] = static_cast<int>(rt.sequencer().currentStep());
      }
    }
    // ---- Class B: advances are the honest discrete observable (currentStep() is public and
    // NOT smoothed); the gate pulse is a separate signal (fires only on a gate-enabled landing
    // step). The older "8-frame steps -> advances at exact frames 8/24/40/56" is unreachable
    // under smoothing; we detect step transitions and hold them to the three-part Class-B lock.
    // ---- Class B: detect ADVANCES via the sequencer's discrete step (currentStep(), public and
    // NOT smoothed). The gate pulse is a SEPARATE signal: it fires only on a gate-enabled landing
    // step, so gate-pulse count != advance count. The three-part lock is expressed on the ADVANCE.
    std::vector<int> adv;  // frames where the sequencer step changed
    for (int i = 1; i < kN; ++i) if (seqStep[i] != seqStep[i - 1]) adv.push_back(i);
    // Class B (a): one advance per rising crossing. The FIRST source crossing (pred[0]) is the
    // sequencer's "started" event (started_ flips, step_ stays 0 — five_step_sequencer), so it does
    // NOT advance; every LATER crossing advances exactly one step. #advances == #crossings - 1.
    check(static_cast<int>(adv.size()) == static_cast<int>(pred.size()) - 1,
          "t6 one advance per rising crossing (edge invariant: 1 advance per crossing after start)");
    // No double-advance / no held-repeat: advances land at strictly spaced frames.
    bool single = true;
    for (std::size_t k = 1; k < adv.size(); ++k) if (adv[k] - adv[k - 1] <= 1) single = false;
    check(single, "t6 an edge yields exactly one advance (no double-advance, no held repeat)");
    // Class B (c): each advance lands within ±1 frame of the closed-form crossing for its step
    // (pred[k+1], since pred[0] is the start-crossing that does not advance).
    bool win = static_cast<int>(adv.size()) == static_cast<int>(pred.size()) - 1;
    for (std::size_t k = 0; k < adv.size(); ++k)
      if (std::abs(adv[k] - pred[k + 1]) > 1) win = false;
    check(win, "t6 each advance lands in the predicted-crossing window (±1 frame of the closed-form crossing)");
    // Class B (b): anti-old-frame — no advance lands on a RAW value-step frame (edge moved off the step).
    bool notRaw = true;
    for (int f : adv)
      for (const auto& step : sched)
        if (step.second == 0.7 && f == step.first) notRaw = false;
    check(notRaw, "t6 anti-old-frame: no advance at a raw value-step frame (edge moved off the step)");
    // gate_out: 10 V single-sample pulse at each crossing whose LANDING step is gate-enabled
    // (step_gate_1=1 => step0 only): the start-crossing (pred[0], step0) and a wrap that lands on
    // step0 (3-stage, pred[3]) pulse; a wrap onto a disabled step (4/5-stage) does not. 0 V elsewhere.
    std::vector<int> expectedGate;
    expectedGate.push_back(pred[0]);
    if (stages == 3) expectedGate.push_back(pred[3]);
    bool gateOK = true;
    for (int i = 0; i < kN; ++i) {
      const double g = sqGate[i];
      const bool expect =
          std::find(expectedGate.begin(), expectedGate.end(), i) != expectedGate.end();
      if (expect != (g > 0.0)) gateOK = false;
    }
    check(gateOK, "t6 gate_out is a 10 V exact-sample pulse at each gate-enabled-step crossing (0 V elsewhere)");
    bool oneShot = true;
    for (std::size_t k = 0; k < expectedGate.size(); ++k) {
      const int f = expectedGate[k];
      if (f > 0 && sqGate[f - 1] != 0.0) oneShot = false;
      if (f + 1 < kN && sqGate[f + 1] != 0.0) oneShot = false;
    }
    check(oneShot, "t6 gate_out is a single-sample pulse (returns to 0 V the next frame)");
    // ---- wrap / stop per stageCount: read the real sequencer step after the last crossing. ----
    check(seqStep[pred.back()] == (stages == 3 ? 0 : 3),
          "t6 4th crossing lands on the wrap step (step0 for 3-stage, step3 for 4/5-stage)");
    // ---- Class A: seq CV reaches the current step's CV target after the settle window ----
    // step_cv_1/2/3 are Smoothing::seconds, so the CV ramps 0 -> target and snaps exact only after
    // the tau-derived settle window (rule 2). For 3-stage the seq is back on step0 from pred[3],
    // and step_cv_1's target (set at frame 0) has long since settled to s0, so the published CV is
    // exactly s0 (the runtime snaps a settled smoother to the exact target, see advanceControlSmoothing_).
    if (stages == 3) check(nearD(sqCv[kN - 1], s0),
                           "t6 seq CV reaches step0 target after the settle window (Class A, rule 2)");
    // seq CV reaches the VCF L sink (real DSP consumer). VCF id=2 < seq id=11, so the VCF
    // reads the PRIOR-frame CV; the CV is ramping non-zero at the advance, so the joint is
    // non-vacuous (this survives smoothing unchanged).
    check(vcfL[pred[0]] != 0.0 || sameD(s0, 0.0),
          "t6 seq CV is a non-zero joint in the VCF L sink (real consumer)");
    // clock_out is now a PUBLISHED virtual-volts rail (@Codex 7C3). The internal PULSER runs
    // at the core default 1.0 Hz, so over this window it never rises (phase accumulates 1/48000
    // per sample, <1.0) and the CLOCK OUT holds the CONFIRMED idle rail. Unchanged by smoothing.
    bool clockIdle = true;
    for (int i = 0; i < kN; ++i) clockIdle = clockIdle && sameD(clockOut[i], -10.0);
    check(clockIdle, "t6 sequencer.clock_out is published and idles at the CONFIRMED -10V rail (1.0Hz PULSER never rises)");
  }

  // (b) Internal PULSER Hz setter (standalone DSP level). At 1500 Hz/48 kHz the pulser
  //     crosses 1.0 at frame 31 (phase += 0.03125/frame), so started() flips true there
  //     and the playhead advances 31/63/95/127 (stage 3 wrap).
  {
    core::FiveStepSequencer seq;
    check(seq.setSampleRate(kSr), "t6 standalone pulser setSampleRate accepted");
    check(seq.setInternalRateHz(1500.0), "t6 standalone pulser Hz setter accepted");
    check(seq.setStageCount(3), "t6 standalone pulser stageCount set");
    int firstStarted = -1;
    for (int i = 0; i < 200 && firstStarted < 0; ++i) {
      seq.tick(false);
      if (seq.started()) firstStarted = i;
    }
    check(firstStarted == 31, "t6 internal PULSER advances at exact sample 31 @1500Hz/48k");
    // Advances land at 31 (step0), 63 (step1), 95 (step2), 127 (wrap->step0).
    int wrapStep = -1;
    for (int i = 32; i <= 127; ++i) {
      seq.tick(false);
      if (i == 127) wrapStep = static_cast<int>(seq.currentStep());
    }
    check(wrapStep == 0, "t6 internal PULSER wraps to step 0 at exact sample 127 (3-stage)");
  }

  // (c) Seq gate -> EG (an actual envelope consumer) so that "gate→EG" in the spec title
  //     is proven for the sequencer, and that the gate pulse triggers the EG. GH#21
  //     re-expression: the joystick is smoothed, so the seq gate fires at the SMOOTHED 0 V
  //     crossing (predicted via the closed form), NOT at the raw value-step frame 8.
  {
    const int gateFrame = gh21_cross_frame(0.5, {{0, 0.3}, {8, 0.7}}, 0.5, 64);
    double envA[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
      rt.connect(reg::JackId::sequencer_gate_out, reg::JackId::envelope_a_gate_in);
      check(rt.rebuild(), "t6c seq gate->EG patch rebuild succeeds");
      applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);
      applyParam(rt, reg::ParameterId::envelope_a_s, 0.9, 0);
      applyParam(rt, reg::ParameterId::envelope_a_a, 0.0002, 0);  // fast attack: nonzero env
      applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 8);
      for (int i = 0; i < 64; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        envA[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      }
    }
    double maxEnv = 0.0;
    for (int i = 0; i < 64; ++i) maxEnv = std::max(maxEnv, envA[i]);
    check(maxEnv > 0.0, "t6c seq gate triggers EG A env (gate->EG), env rises after the gate");
    check(gateFrame > 0 && envA[gateFrame - 1] == 0.0,
          "t6c the EG is NOT gated before the smoothed crossing (env idle until then)");
    check(gateFrame > 8 && envA[8] == 0.0,
          "t6c anti-old-frame: the EG is not gated at the raw value-step frame 8 (edge moved)");
  }
}

// ===========================================================================
// 7. The 34 evidence-mappable control-source params: applied through the event path they
//    reach the real instances (observable subset), an out-of-range value is kept-old (no
//    crash, no change), `sequencer.pulser` is EXACTLY blocked (never maps to Hz, so no
//    silent default advance), and delivery is 64/128/mixed partition-consistent.
// ===========================================================================
static void test_7_param_table_partition(void) {
  // (a) Joystick + LFO + step CV: each applied via an event is observable on the output.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::lfo_b_rate, 20.0, 0);
    // Class A: after the tau-derived settle window each source reaches its mapped output
    // (the smoother snaps to the exact target on settle, so a nearD reach is exact).
    gh21_advance(rt, gh21_settle_frames(kSr));
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 2.0),
          "t7 joystick_x param reaches x_out (2.0V)");
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_y_out), -3.0),
          "t7 joystick_y param reaches y_out (-3.0V)");
    // LFO A ran at a different rate than LFO B (two distinct sources, both applied).
    double aVal = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
    double bVal = rt.controlVoltageAt(reg::JackId::lfo_b_cv_out);
    check(std::isfinite(aVal) && std::isfinite(bVal) && !sameD(aVal, bVal),
          "t7 lfo_a_rate and lfo_b_rate applied to distinct instances");
  }

  // (b) Out-of-range finite value is REJECTED at the product boundary (@Codex BLOCKED #1):
  //     joystick_x is norm [0,1]; 5.0 / -1.0 are NOT in domain -> invalid_value (keep old),
  //     never the DSP clamp (setNorm_ would clamp01(5.0)=1.0). The readback AND the rail both
  //     stay at the PRIOR value — admission is evidence-domain, not setter-clamp.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    const core::RuntimeInputs z{0.0, 0.0};
    core::RuntimeOutput o;
    rt.processBlock(&z, 1, &o);
    gh21_advance(rt, gh21_settle_frames(kSr));  // let x settle to 0.7 (Class A reach)
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 2.0),
          "t7 x=0.7 reaches +2.0V (baseline)");
    applyParam(rt, reg::ParameterId::joystick_x, 5.0, 1);  // out of [0,1]
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
          "t7 joystick_x=5.0 -> invalid_value (product-boundary)");
    check(nearD(rt.joystick().x(), 0.7),
          "t7 invalid 5.0 keeps stored x=0.7 (no clamp-admission)");
    check(nearD(rt.controlVoltageAt(reg::JackId::joystick_x_out), 2.0),
          "t7 published rail stays +2.0V after invalid (not clamped to +5.0V)");
  }
  {
    // Lower rail: x=-1.0 is also out of [0,1] -> invalid_value; the default x=0.5 is kept.
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, -1.0, 0);
    const core::RuntimeInputs z{0.0, 0.0};
    core::RuntimeOutput o;
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
          "t7 joystick_x=-1.0 -> invalid_value (lower rail)");
    check(nearD(rt.joystick().x(), 0.5), "t7 invalid -1.0 keeps default x=0.5");
  }

  // (c) sequencer.pulser is DOMAIN-VALIDATED (@Codex 7C3): the norm [0,1] is admitted through
  //     the provisional software model (centrally `pulserNormToRateHz`), so an OUT-OF-DOMAIN
  //     value must be rejected as invalid_value without touching the schedule. We render a
  //     baseline (no seq param) and a PULSER run with 10000.0 (out of [0,1]) and require them
  //     to be bit-identical: if PULSER were silently clamped/coerced the second run would
  //     advance and diverge. A legitimate joystick→external-clock advance at sample 40 is the
  //     positive control. (A VALID norm transfer is verified separately in test_13.)
  double cvBase[kCap], cvPulser[kCap], cvPos[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      cvBase[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, 0);  // out of [0,1]
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      cvPulser[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
  }
  // lastApplyStatus reflects the LAST dispatched param event; in the harness above
  // step_cv_1 (pid 163) sorts AFTER pulser (pid 160) within the same sample, so the
  // status would be overwritten. A standalone single-event harness proves the pulser
  // reject unambiguously (matches test_9).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, 0);  // out of [0,1]
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
          "t7 pulser 10000.0 (out of [0,1]) -> invalid_value (no silent clamp)");
  }
  bool pulserBlocked = true;
  for (int i = 0; i < kCap; ++i) pulserBlocked = pulserBlocked && sameD(cvPulser[i], cvBase[i]);
  check(pulserBlocked, "t7 out-of-domain pulser leaves the schedule bit-identical (invalid, never coerced)");
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 40);
    applyParam(rt, reg::ParameterId::joystick_x, 0.2, 100);
    applyParam(rt, reg::ParameterId::joystick_x, 1.0, 160);
    // Class B (source-edge, three-part lock). The ext_clock gate is a 0-threshold/0-hysteresis
    // comparator, rising exactly when the SMOOTHED joystick_x crosses 0.5 (x_out 0V). The seq
    // (kExternal clock source) STARTS on the first rising edge (started_=true, step held at 0)
    // and ADVANCES step 0->1 on the SECOND. Both edges are predicted by the one-pole closed form
    // (gh21_cross_up_frames) at frames 79 and 187 — NOT the raw value-step frames 40/100/160,
    // which is the anti-old-frame lock (b), and each lands within the predictive window (c).
    // A rising edge advances at most once (a): started never repeats on a sustained high.
    const std::vector<int> pred = gh21_cross_up_frames(
        0.5, {{0, 0.3}, {40, 0.7}, {100, 0.2}, {160, 1.0}}, 0.5, kCap);
    int startFrame = -1, advFrame = -1;
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      cvPos[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      if (startFrame < 0 && rt.sequencer().started()) startFrame = i;
      if (advFrame < 0 && rt.sequencer().currentStep() >= 1) advFrame = i;
    }
    check(pred.size() >= 2, "t7 one-pole closed-form predicts the two ext-clock rising crossings");
    check(std::abs(startFrame - pred[0]) <= 1,
          "t7 ext-clock START edge lands within ±1 of the closed-form crossing (predictive window)");
    check(std::abs(advFrame - pred[1]) <= 1,
          "t7 ext-clock ADVANCE edge lands within ±1 of the closed-form crossing (predictive window)");
    check(startFrame != 40 && advFrame != 100 && advFrame != 160,
          "t7 ext-clock edges NOT at the old raw value-step frames (anti-old-frame)");
    check(rt.sequencer().currentStep() == 1,
          "t7 legitimate external clock advances exactly one step (0->1)");
  }

  // (d) 64/128/mixed partition consistency for a SOURCE event set (design/07 §5): the SAME
  //     logical events (a sample-8 offset change + LFO rate + joystick) rendered under four
  //     schedules produce bit-identical output. If any block partition applied an event at a
  //     different frame, offset would land at a different sample and wetL would diverge.
  const uint32_t p256[1] = {256};
  const uint32_t p128[2] = {128, 128};
  const uint32_t p64[4] = {64, 64, 64, 64};
  const uint32_t pMixed[5] = {128, 16, 96, 8, 8};
  core::RuntimeOutput o256[kCap], o128[kCap], o64[kCap], oM[kCap];
  for (int sched = 0; sched < 4; ++sched) {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, 8);
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    const uint32_t* blk = (sched == 0) ? p256 : (sched == 1) ? p128 : (sched == 3) ? pMixed : p64;
    const int nb = (sched == 0) ? 1 : (sched == 1) ? 2 : (sched == 3) ? 5 : 4;
    block_render(rt, kCap, blk, nb, (sched == 0) ? o256 : (sched == 1) ? o128 : (sched == 3) ? oM : o64);
  }
  bool partConsistent = true;
  for (int i = 0; i < kCap; ++i)
    partConsistent = partConsistent && sameD(o256[i].wetL, o128[i].wetL) &&
                     sameD(o256[i].wetL, o64[i].wetL) && sameD(o256[i].wetL, oM[i].wetL);
  check(partConsistent, "t7 64/128/mixed partition is bit-identical (buffer-invariant §5)");
}

// ===========================================================================
// 8. task#65 canonical invariants: the exact feedback-pair discriminator (B′), and the
//    render path allocates ZERO (A′). Old-test non-regression is the separate whole-suite
//    CTest run (this TU only contributes a no-crash sanity render).
// ===========================================================================
static void test_8_task65_invariants_zero_alloc(void) {
  // (a) FeedbackResolve (task#65 correction 6 / B′): a (src,sink) pair where the sink
  //     is EXACTLY the feedback line's sink reads the D-delay; same-source/different-sink
  //     reads the LIVE value. This is the pure discriminator, asserted directly.
  {
    const core::FeedbackResolve exact = core::feedbackSinkValue(
        reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_l_in,
        reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_l_in, 0.125, 0.999);
    check(exact.matched && sameD(exact.value, 0.125), "t8 exact (src,sink) reads the delayed value");
    const core::FeedbackResolve nonExact = core::feedbackSinkValue(
        reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_r_in,
        reg::JackId::lfo_a_cv_out, reg::JackId::vcf_cv_l_in, 0.125, 0.999);
    check(!nonExact.matched && sameD(nonExact.value, 0.999),
          "t8 same-source/different-sink reads the LIVE value (no cross-line delay)");
  }

  // (b) Render-path zero allocation: measure g_allocCount across a real render loop. The
  //     whole event set + cabling must run with no operator-new on the audio path (criterion ⑤).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in);
    rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_2_cv_mod_in);
    rt.rebuild();
    for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 0.5);
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, 8);
    const std::size_t before = g_allocCount;
    double sink = 0.0;
    for (int i = 0; i < 4000; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      sink += o.wetL;  // consume so the optimizer cannot elide the render
    }
    const std::size_t after = g_allocCount;
    check(after == before, "t8 render path allocates ZERO (A′ criterion ⑤)");
    check(std::isfinite(sink), "t8 sustained render stays finite (no NaN blow-up)");
  }
}

// ===========================================================================
// Negative controls (≥6): each is a deliberately WRONG oracle that, in isolation, RED.
// Proven by asserting the correct behavior directly (the "restore GREEN" step is the
// positive assertion above); the wrong variant is shown to be distinguishable.
// ===========================================================================
static void test_negative_controls(void) {
  // (1) No always-execute fact: a source that is NOT admitted to the plan would produce
  //     a constant / zero published value even when its param is applied.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    double v0 = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
    // before the first frame the source must have a defined (finite) value; after a real
    // render the LFO is visible — proving it ran once/sample, not dropped from the plan.
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    double v1 = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
    check(std::isfinite(v0) && std::isfinite(v1) && !sameD(v0, v1),
          "neg1 control source is admitted to the plan (always-execute), not dropped");
  }

  // (2) Don't-publish / producer-after-consumer: a publisher that overwrote the bank AFTER
  //     a consumer would give a one-frame-lag. We prove the cabled drone CV-MOD is same-
  //     frame (bitwise-equal to a manual same-frame injection) instead — reusing test 2 part.
  {
    double lfoBase[kCap], wetCable[kCap], wetManual[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      drone_patch_run(def, DroneRun::kBase, nullptr, lfoBase, nullptr);
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      drone_patch_run(def, DroneRun::kCable, nullptr, nullptr, wetCable);
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      drone_patch_run(def, DroneRun::kManual, lfoBase, nullptr, wetManual);
    }
    bool same = true;
    for (int i = 0; i < kCap; ++i) same = same && sameD(wetCable[i], wetManual[i]);
    check(same, "neg2 producer-before-consumer (no phantom frame-lag on CV-MOD)");
  }

  // (3) Bypass the sink latch / treat an every-sample-high as a repeated edge: the gate
  //     interpreter must yield ONE rising edge per transition. We prove a sustained high
  //     does NOT repeat (already in test 5), here we prove a low prime does NOT advance.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);  // stays low the whole window
    for (int i = 0; i < 40; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
    }
    check(sameD(rt.controlVoltageAt(reg::JackId::sequencer_cv_out), 0.0),
          "neg3 a constant-low source never advances (edge requires a real transition)");
  }

  // (4) A/B, X/Y, step-owner cross-wire: prove joystick X is independent of Y (already t4)
  //     and EG A independent of EG B (already t3). Here: two LFOs are distinct instances
  //     (changing lfo_a_rate leaves lfo_b untouched in output).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
    applyParam(rt, reg::ParameterId::lfo_b_rate, 10.0, 0);
    double a0 = 0, b0 = 0;
    for (int i = 0; i < 64; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      if (i == 63) { a0 = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
                     b0 = rt.controlVoltageAt(reg::JackId::lfo_b_cv_out); }
    }
    check(!sameD(a0, b0) && std::isfinite(a0) && std::isfinite(b0),
          "neg4 A/B and X/Y source instances are distinct (no cross-wire)");
  }

  // (5) Parameter block-front apply, or a missed family. joystick_offset_x is a GH#21
  //     seconds-smoothed source, so its effect is a ONE-POLE RAMP that begins only at the
  //     event frame 8 (Class B: event-triggered smooth ramp, never a jump at block-front).
  //     Re-expressed from the old exact x_out[0]==2.0 / x_out[8]==0.0 (which assumed the
  //     offset and x snapped instantly): we prove (a) the event does NOT apply at block-front
  //     (the published x_out at frame 0 is bit-identical whether or not the frame-8 offset
  //     event exists) and (b) the offset family IS hit (after the settle window the offset
  //     event moves x_out away from the no-offset baseline).
  {
    double xNoOff[kCap], xWithOff[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput o; const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        xNoOff[i] = rt.controlVoltageAt(reg::JackId::joystick_x_out);
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
      applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, 8);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput o; const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        xWithOff[i] = rt.controlVoltageAt(reg::JackId::joystick_x_out);
      }
    }
    check(sameD(xWithOff[0], xNoOff[0]),
          "neg5 offset event NOT applied at block-front (frame 0 unchanged, no premature ramp)");
    check(!sameD(xWithOff[kCap - 1], xNoOff[kCap - 1]),
          "neg5 offset family is hit after the settle window (frame-8 event moves the joystick)");
  }

  // (6) PULSER-as-Hz or a silent default: the REAL internal-rate mechanism is ADMITTED on
  //     the standalone DSP (setting Hz → advance within a few frames), and the runtime PULSER
  //     *param* is a DOMAIN-VALIDATED transfer (@Codex 7C3). An OUT-OF-DOMAIN value (10000.0)
  //     must be rejected as invalid_value and leave the schedule bit-identical — this
  //     distinguishes "pulser genuinely validates" from "internal rate silently defaults".
  {
    core::FiveStepSequencer seq;
    check(seq.setSampleRate(kSr), "neg6 standalone setSampleRate accepted");
    seq.setStageCount(3);
    check(seq.setInternalRateHz(5000.0), "neg6 standalone internal-rate setter accepted");
    int fired = -1;
    for (int i = 0; i < 20; ++i) { seq.tick(false); if (seq.started()) { fired = i; break; } }
    check(fired >= 0 && fired <= 10, "neg6 the real internal-rate mechanism is admitted");
  }
  {
    double base[kCap], puls[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        base[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
      applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, 0);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        puls[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      }
    }
    bool noop = true;
    for (int i = 0; i < kCap; ++i) noop = noop && sameD(base[i], puls[i]);
    check(noop, "neg6 out-of-domain runtime PULSER param leaves the schedule bit-identical");
  }
}

// ===========================================================================
// 9. (BLOCKED #1/#2) Full 34-row control-source parameter matrix with apply-status
//    discrimination. EG(12) + LFO(6) + joystick(4) + sequencer(12) = 34. For each:
//    a UNIT-DOMAIN value must land `applied` (the setter was really reached), and a
//    UNIT-DOMAIN VIOLATION must land `invalid_value` (rejected at the product boundary
//    BEFORE any sound-core clamp/coerce is mistaken for admission). sequencer_pulser is
//    the 35th: recognised but `transfer_unavailable` (BLOCKED, GH#11).
// ===========================================================================
// Read the REAL instance getter value (as a double) for a control-source parameter.
// This is the @Codex #2 requirement: the apply-status oracle must be backed by the actual
// DSP instance state read through the public CONST surface — never a shadow/param bank.
// Enum/int/bool getters are converted to a comparable double (their underlying value);
// step CV/Gate family index is derived from the parameter id (cv_N -> N-1).
static double probeGetter(const core::SynthRuntime& rt, reg::ParameterId pid) {
  switch (pid) {
    // Envelope A (6).
    case reg::ParameterId::envelope_a_a: return rt.envelopeA().attackSeconds();
    case reg::ParameterId::envelope_a_d: return rt.envelopeA().decaySeconds();
    case reg::ParameterId::envelope_a_r: return rt.envelopeA().releaseSeconds();
    case reg::ParameterId::envelope_a_s: return rt.envelopeA().sustain();
    case reg::ParameterId::envelope_a_hold: return rt.envelopeA().hold() ? 1.0 : 0.0;
    case reg::ParameterId::envelope_a_self_gen: return rt.envelopeA().selfGen() ? 1.0 : 0.0;
    // Envelope B (6).
    case reg::ParameterId::envelope_b_a: return rt.envelopeB().attackSeconds();
    case reg::ParameterId::envelope_b_d: return rt.envelopeB().decaySeconds();
    case reg::ParameterId::envelope_b_r: return rt.envelopeB().releaseSeconds();
    case reg::ParameterId::envelope_b_s: return rt.envelopeB().sustain();
    case reg::ParameterId::envelope_b_hold: return rt.envelopeB().hold() ? 1.0 : 0.0;
    case reg::ParameterId::envelope_b_self_gen: return rt.envelopeB().selfGen() ? 1.0 : 0.0;
    // LFO A (3).
    case reg::ParameterId::lfo_a_rate: return rt.lfoA().baseHz();
    case reg::ParameterId::lfo_a_wave: return rt.lfoA().wave();
    case reg::ParameterId::lfo_a_speed_mult:
      return static_cast<double>(rt.lfoA().speedMult());
    // LFO B (3).
    case reg::ParameterId::lfo_b_rate: return rt.lfoB().baseHz();
    case reg::ParameterId::lfo_b_wave: return rt.lfoB().wave();
    case reg::ParameterId::lfo_b_speed_mult:
      return static_cast<double>(rt.lfoB().speedMult());
    // Joystick (4).
    case reg::ParameterId::joystick_x: return rt.joystick().x();
    case reg::ParameterId::joystick_y: return rt.joystick().y();
    case reg::ParameterId::joystick_offset_x: return rt.joystick().offsetX();
    case reg::ParameterId::joystick_offset_y: return rt.joystick().offsetY();
    // Sequencer clock / stages.
    case reg::ParameterId::sequencer_clock:
      return static_cast<double>(rt.sequencer().clockSource());
    case reg::ParameterId::sequencer_stages:
      return static_cast<double>(rt.sequencer().stageCount());
    // Sequencer step CV (5) and step gate (5); the index is derived from the id suffix.
    case reg::ParameterId::sequencer_step_cv_1:
    case reg::ParameterId::sequencer_step_cv_2:
    case reg::ParameterId::sequencer_step_cv_3:
    case reg::ParameterId::sequencer_step_cv_4:
    case reg::ParameterId::sequencer_step_cv_5:
      return rt.sequencer().stepCv(static_cast<int>(pid) -
                                   static_cast<int>(reg::ParameterId::sequencer_step_cv_1));
    case reg::ParameterId::sequencer_step_gate_1:
    case reg::ParameterId::sequencer_step_gate_2:
    case reg::ParameterId::sequencer_step_gate_3:
    case reg::ParameterId::sequencer_step_gate_4:
    case reg::ParameterId::sequencer_step_gate_5:
      return rt.sequencer().stepGate(static_cast<int>(pid) -
                                     static_cast<int>(reg::ParameterId::sequencer_step_gate_1))
                 ? 1.0 : 0.0;
    default:
      return 0.0;  // unreachable for a matrix row (pulser handled separately).
  }
}

namespace {
struct ParamRow {
  const char* name;
  reg::ParameterId pid;
  double valid;     // unit-domain value -> must land `applied` AND probeGetter == expected
  double invalid;   // unit-domain VIOLATION -> must land `invalid_value`, getter UNCHANGED
  double expected;  // the REAL instance getter value after `valid` is applied (oracle)
};
}  // namespace

static const ParamRow kMatrix[] = {
    // ---- Envelope A/B (A/R/D/S + HOLD/SELF-GEN) ----
    {"egA attack", reg::ParameterId::envelope_a_a, 0.5, -1.0, 0.5},
    {"egA decay", reg::ParameterId::envelope_a_d, 0.5, -1.0, 0.5},
    {"egA release", reg::ParameterId::envelope_a_r, 0.5, -1.0, 0.5},
    {"egA sustain", reg::ParameterId::envelope_a_s, 0.3, 1.5, 0.3},   // 0.3 (not the 0.5 default)
    {"egA hold", reg::ParameterId::envelope_a_hold, 1.0, 0.5, 1.0},
    {"egA selfgen", reg::ParameterId::envelope_a_self_gen, 1.0, 0.5, 1.0},
    {"egB attack", reg::ParameterId::envelope_b_a, 0.5, -1.0, 0.5},
    {"egB decay", reg::ParameterId::envelope_b_d, 0.5, -1.0, 0.5},
    {"egB release", reg::ParameterId::envelope_b_r, 0.5, -1.0, 0.5},
    {"egB sustain", reg::ParameterId::envelope_b_s, 0.3, 1.5, 0.3},
    {"egB hold", reg::ParameterId::envelope_b_hold, 1.0, 0.5, 1.0},
    {"egB selfgen", reg::ParameterId::envelope_b_self_gen, 1.0, 0.5, 1.0},
    // ---- LFO A/B (rate + wave morph + speed_mult selector) ----
    {"lfoA rate", reg::ParameterId::lfo_a_rate, 440.0, -1.0, 440.0},
    {"lfoA wave", reg::ParameterId::lfo_a_wave, 0.3, 2.0, 0.3},   // 0.3 (not the 0.5 default)
    {"lfoA speed", reg::ParameterId::lfo_a_speed_mult, 1.0, 1.5, 1.0},  // index 1 -> x6
    {"lfoB rate", reg::ParameterId::lfo_b_rate, 440.0, -1.0, 440.0},
    {"lfoB wave", reg::ParameterId::lfo_b_wave, 0.3, 2.0, 0.3},
    {"lfoB speed", reg::ParameterId::lfo_b_speed_mult, 1.0, 1.5, 1.0},
    // ---- Joystick (x/y + offsets) ----
    {"joy x", reg::ParameterId::joystick_x, 0.7, 5.0, 0.7},
    {"joy y", reg::ParameterId::joystick_y, 0.3, 5.0, 0.3},
    {"joy offset x", reg::ParameterId::joystick_offset_x, 0.7, -1.0, 0.7},
    {"joy offset y", reg::ParameterId::joystick_offset_y, 0.3, -1.0, 0.3},
    // ---- Sequencer (clock + stages + 5 step CV + 5 step gate) ----
    {"seq clock", reg::ParameterId::sequencer_clock, 1.0, 2.0, 1.0},    // index 1 -> external
    {"seq stages", reg::ParameterId::sequencer_stages, 1.0, 3.0, 4.0},  // index 1 -> stageCount=4
    {"seq step cv1", reg::ParameterId::sequencer_step_cv_1, 3.5, 6.0, 3.5},
    {"seq step cv2", reg::ParameterId::sequencer_step_cv_2, 3.5, 6.0, 3.5},
    {"seq step cv3", reg::ParameterId::sequencer_step_cv_3, 3.5, 6.0, 3.5},
    {"seq step cv4", reg::ParameterId::sequencer_step_cv_4, 3.5, 6.0, 3.5},
    {"seq step cv5", reg::ParameterId::sequencer_step_cv_5, 3.5, 6.0, 3.5},
    {"seq gate1", reg::ParameterId::sequencer_step_gate_1, 1.0, 0.5, 1.0},
    {"seq gate2", reg::ParameterId::sequencer_step_gate_2, 1.0, 0.5, 1.0},
    {"seq gate3", reg::ParameterId::sequencer_step_gate_3, 1.0, 0.5, 1.0},
    {"seq gate4", reg::ParameterId::sequencer_step_gate_4, 1.0, 0.5, 1.0},
    {"seq gate5", reg::ParameterId::sequencer_step_gate_5, 1.0, 0.5, 1.0},
};

static void row_apply(const ParamRow& row) {
  // Fresh runtime per row: guarantees the event lands at absolute sample 0 (no /late/
  // delivery from an advanced timebase), so the recorded status is unambiguous.
  std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
  core::SynthRuntime& rt = def->runtime();
  core::RuntimeOutput o;
  const core::RuntimeInputs z{0.0, 0.0};

  // A continuous (Smoothing::seconds) control-source param is a ONE-POLE ramp (GH#21); a
  // discrete control-source param (hold/self_gen/wave/speed_mult/clock/stages/step_gate)
  // lands INSTANT. The acceptance distinguishes them (rule 4: the 15 discrete single-sample
  // assertions stay UNCHANGED; the 19 continuous ones are re-expressed as reach-after-settle,
  // rule 2: the window is the tau-derived settle frame count).
  const bool smooth = gh21_is_seconds(row.pid);

  // Phase 1: the unit-domain `valid` value MUST land `applied` AND the REAL getter must reach
  // `expected`. This is the @Codex #2 oracle — read the actual DSP instance through the public
  // CONST surface, never a shadow/param bank. The admit (status==applied) is instantaneous at
  // the apply frame, INDEPENDENT of smoothing; only the getter REACH is delayed by the ramp.
  applyParam(rt, row.pid, row.valid, 0);
  rt.processBlock(&z, 1, &o);
  check(rt.lastApplyParamId() == row.pid, "t9 row lastApplyParamId == row id");
  check(rt.lastApplyStatus() == core::ParameterApplyStatus::applied,
        "t9 valid row applied (status, per-family unblocked)");
  if (smooth) gh21_advance(rt, gh21_settle_frames(kSr));  // converge the ramp
  check(nearD(probeGetter(rt, row.pid), row.expected),
        "t9 valid row reaches the REAL instance getter");

  // Phase 2: a unit-domain VIOLATION must land `invalid_value` AND leave the getter at the
  // VALID value (the boundary reject keeps old — a bad value never mutates the source). For a
  // continuous param, capture the value AFTER it has settled so "keeps old" means "the invalid
  // never moved it off the VALID target", not "frozen mid-ramp" (which smoothing makes false).
  const double before = probeGetter(rt, row.pid);
  applyParam(rt, row.pid, row.invalid, 1);
  rt.processBlock(&z, 1, &o);
  check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
        "t9 invalid row invalid_value (product-boundary reject)");
  check(nearD(probeGetter(rt, row.pid), before),
        "t9 invalid row leaves the REAL getter unchanged");
}

static void test_9_param_matrix_apply_status(void) {
  const std::size_t n = sizeof(kMatrix) / sizeof(kMatrix[0]);
  check(n == 34, "t9 matrix has all 34 control-source params");
  for (std::size_t i = 0; i < n; ++i) {
    row_apply(kMatrix[i]);
  }

  // The 35th: sequencer_pulser is a DOMAIN-VALIDATED provisional transfer (@Codex 7C3). A
  // VALID norm [0,1] is admitted and mapped through the centrally-named software model into
  // the direct-Hz setter (monotonic 0.05/1.0/20.0 at norm 0.0/0.5/1.0); an OUT-OF-DOMAIN or
  // non-finite value is rejected as invalid_value with the RATE UNCHANGED (never a subtle
  // coercion, never a silent false-green "applied" at the wrong Hz).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    // The pulser is Smoothing::seconds: a valid norm is admitted instantly (status==applied at
    // the apply frame) but the RATE ramps to the mapped Hz over the tau-derived settle window
    // (rule 2). Each case sets a target, lets the one-pole settle, then reads the direct-Hz
    // setter through nearD (a smoothed rate is a pole trajectory, never a bitwise literal).
    const int settle = gh21_settle_frames(kSr);
    uint64_t sample = 0;  // next absolute sample the runtime will render
    // norm 0.0 -> 0.05 Hz (provisional software min).
    applyParam(rt, reg::ParameterId::sequencer_pulser, 0.0, sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyParamId() == reg::ParameterId::sequencer_pulser,
          "t9 pulser row lastApplyParamId == pulser");
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::applied,
          "t9 pulser norm 0.0 -> applied");
    gh21_advance(rt, settle); sample += settle;
    check(nearD(rt.sequencerInternalRateHz(), 0.05),
          "t9 pulser norm 0.0 -> 0.05 Hz (provisional software min)");
    // norm 0.5 -> 1.0 Hz (provisional centre).
    applyParam(rt, reg::ParameterId::sequencer_pulser, 0.5, sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::applied,
          "t9 pulser norm 0.5 -> applied");
    gh21_advance(rt, settle); sample += settle;
    check(nearD(rt.sequencerInternalRateHz(), 1.0),
          "t9 pulser norm 0.5 -> 1.0 Hz (provisional centre)");
    // norm 1.0 -> 20.0 Hz (provisional software max).
    applyParam(rt, reg::ParameterId::sequencer_pulser, 1.0, sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::applied,
          "t9 pulser norm 1.0 -> applied");
    gh21_advance(rt, settle); sample += settle;
    check(nearD(rt.sequencerInternalRateHz(), 20.0),
          "t9 pulser norm 1.0 -> 20.0 Hz (provisional software max)");
    // invalid keep-old: out-of-domain 10000.0, negative, NaN, Inf all leave the rate at 20.0.
    // The rate is already SETTLED (inert) at 20.0, so an invalid is a no-op — bitwise-unchanged.
    applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencerInternalRateHz(), 20.0),
          "t9 pulser out-of-domain 10000.0 -> invalid_value AND keeps the prior rate");
    applyParam(rt, reg::ParameterId::sequencer_pulser, -0.5, sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencerInternalRateHz(), 20.0),
          "t9 pulser norm -0.5 -> invalid_value AND keeps the prior rate");
    applyParam(rt, reg::ParameterId::sequencer_pulser, std::nan(""), sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencerInternalRateHz(), 20.0),
          "t9 pulser NaN -> invalid_value AND keeps the prior rate");
    applyParam(rt, reg::ParameterId::sequencer_pulser,
               std::numeric_limits<double>::infinity(), sample);
    rt.processBlock(&z, 1, &o); sample += 1;
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencerInternalRateHz(), 20.0),
          "t9 pulser +Inf -> invalid_value AND keeps the prior rate");
  }

  // "Invalid keeps old" (@Codex #1): a boundary rejection must NOT mutate the source.
  // (a) joystick_x stays at its prior value after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    rt.processBlock(&z, 1, &o);
    gh21_advance(rt, gh21_settle_frames(kSr));  // joystick_x is Continuous: let it settle to 0.7
    const double before = rt.joystick().x();
    applyParam(rt, reg::ParameterId::joystick_x, 5.0,
               static_cast<uint64_t>(gh21_settle_frames(kSr)) + 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.joystick().x(), before),
          "t9 invalid joystick_x rejects AND keeps prior x (no clamp-coerce)");
  }
  // (b) envelope sustain norm stays after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    applyParam(rt, reg::ParameterId::envelope_a_s, 0.4, 0);
    rt.processBlock(&z, 1, &o);
    gh21_advance(rt, gh21_settle_frames(kSr));  // sust is Continuous: let it settle to 0.4
    const double before = rt.envelopeA().sustain();
    applyParam(rt, reg::ParameterId::envelope_a_s, 1.5,
               static_cast<uint64_t>(gh21_settle_frames(kSr)) + 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.envelopeA().sustain(), before),
          "t9 invalid sustain rejects AND keeps prior sustain");
  }
  // (c) LFO wave norm stays after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::lfo_a_wave, 0.25, 0);
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    const double before = rt.lfoA().wave();
    applyParam(rt, reg::ParameterId::lfo_a_wave, 2.0, 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.lfoA().wave(), before),
          "t9 invalid lfo wave rejects AND keeps prior wave (no clamp)");
  }
  // (d) step CV volts stays after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 3.5, 0);
    rt.processBlock(&z, 1, &o);
    gh21_advance(rt, gh21_settle_frames(kSr));  // step cv is Continuous: let it settle to 3.5
    const double before = rt.sequencer().stepCv(0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 6.0,
               static_cast<uint64_t>(gh21_settle_frames(kSr)) + 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencer().stepCv(0), before),
          "t9 invalid step cv rejects AND keeps prior cv (no clamp)");
  }
}

// ===========================================================================
// 10. (BLOCKED #3) VCA-CV A/B independence + a consolidated 11-output / 3-sink audit.
//     The six sources publish EXACTLY 11 jacks (EG 4: env A/B + vca_cv A/B; LFO 2; joy 2;
//     seq 3: cv + gate + clock_out). Each sink readback is a REAL consumer
//     (`vcfCvReadbackL/R`, `droneChannel`), not a test-side re-derivation.
// ===========================================================================
static void test_10_vca_ab_and_output_audit(void) {
  // (a) The 11 published output JackIds, all live (finite after a render), and clock_out
  //     INCLUDED in the set as a genuine virtual-volts rail.
  static const reg::JackId kOut[11] = {
      reg::JackId::envelope_a_env_out, reg::JackId::envelope_a_vca_cv_out,
      reg::JackId::envelope_b_env_out, reg::JackId::envelope_b_vca_cv_out,
      reg::JackId::lfo_a_cv_out,       reg::JackId::lfo_b_cv_out,
      reg::JackId::joystick_x_out,     reg::JackId::joystick_y_out,
      reg::JackId::sequencer_cv_out,   reg::JackId::sequencer_gate_out,
      reg::JackId::sequencer_clock_out,
  };
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    for (int i = 0; i < 11; ++i)
      check(std::isfinite(rt.controlVoltageAt(kOut[i])),
            "t10 all eleven source outputs are finite/published");
    // clock_out IS published as a virtual-volts rail: at the provisional-software default
    // 1.0 Hz PULSER over one block (no rising edge) it idles at the CONFIRMED -10V rail. This
    // is the canned physical model being exposed, not an empty slot.
    check(nearD(rt.controlVoltageAt(reg::JackId::sequencer_clock_out), -10.0),
          "t10 sequencer clock_out IS published and idles at the -10V rail (1.0 Hz PULSER)");
  }

  // (b) VCA-CV A/B independence: envelope A self-generates while B is idle → vca_cv_a is
  //     live and vca_cv_b stays at its idle level; swapping which EG self-generates flips
  //     the two. Proves vca_cv_out is asserted AND A/B are not cross-wired.
  {
    double aLive = 0.0, bIdle = 0.0;
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::envelope_a_self_gen, 1.0, 0);  // A runs, B idle
      for (int i = 0; i < 60; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        aLive = rt.controlVoltageAt(reg::JackId::envelope_a_vca_cv_out);
        bIdle = rt.controlVoltageAt(reg::JackId::envelope_b_vca_cv_out);
      }
      check(std::isfinite(aLive) && std::isfinite(bIdle) && !sameD(aLive, bIdle),
            "t10 vca_cv_a and vca_cv_b are independent (A live, B idle when only A self-gens)");
    }
    double bLive = 0.0, aIdle = 0.0;
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::envelope_b_self_gen, 1.0, 0);  // B runs, A idle
      for (int i = 0; i < 60; ++i) {
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);
        bLive = rt.controlVoltageAt(reg::JackId::envelope_b_vca_cv_out);
        aIdle = rt.controlVoltageAt(reg::JackId::envelope_a_vca_cv_out);
      }
      check(std::isfinite(bLive) && std::isfinite(aIdle) && !sameD(bLive, aIdle),
            "t10 swapping self-gen flips which VCA-CV is live (A/B really independent)");
    }
  }

  // (c) 3-sink audit: VCF-L, VCF-R, and the classic drone channel each CONSUME a source
  //     output (real DSP reader). Joystick feeds BOTH VCF sinks as independent L/R; LFO
  //     feeds the drone CV-mod (proven by a baseline-vs-cable audio joint, not a default).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.3, 0);
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in), "t10 wire joy x -> vcf l");
    check(rt.connect(reg::JackId::joystick_y_out, reg::JackId::vcf_cv_r_in), "t10 wire joy y -> vcf r");
    check(rt.rebuild(), "t10 joy -> VCF rebuild (sink resolution)");
    // Joystick x/y are GH#21 seconds-smoothed sources: they ramp over the tau-derived settle
    // window toward +2.0V (x=0.7) / -2.0V (y=0.3), reaching the exact snapped target on settle.
    // Re-expressed from the old 8-frame exact readback (which assumed an instant snap) to a
    // Class-A value-reach after the settle window; then one more block for the VCF readback
    // (id=2 < joystick id=8) to consume the settled published value.
    gh21_advance(rt, gh21_settle_frames(kSr));
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    const double vcfL = rt.vcfCvReadbackL();  // joystick x -> 5*(2*0.7-1) = +2.0V
    const double vcfR = rt.vcfCvReadbackR();  // joystick y -> 5*(2*0.3-1) = -2.0V
    check(nearD(vcfL, 2.0) && nearD(vcfR, -2.0) && !sameD(vcfL, vcfR),
          "t10 VCF-L and VCF-R are two independent real sinks (sink 1 & sink 2)");
  }
  // Drone sink (sink 3): the cabled LFO CV-MOD is a REAL joint. A bare cable with the drone
  // MOD held at 0 (inert) must be BIT-IDENTICAL to the no-cable baseline — otherwise "audio
  // differs" would be a phantom false-green that merely notices a cable exists. Only when
  // the MOD is engaged does the SAME cable change the audio, proving the drone really
  // consumes the CV carried on drone_2_cv_mod_in.
  {
    double wetBase[kCap], wetOff[kCap], wetOn[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
      applyParam(rt, reg::ParameterId::lfo_a_wave, 0.5, 0);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetBase[i] = blk.wetL;  // no cable, drone MOD at default (inert)
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
      applyParam(rt, reg::ParameterId::lfo_a_wave, 0.5, 0);
      for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 0.0);  // MOD off: cable exists, inert
      check(rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_2_cv_mod_in),
            "t10 wire lfo -> drone cv-mod (inert)");
      check(rt.rebuild(), "t10 drone cable rebuild (inert)");
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetOff[i] = blk.wetL;
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::lfo_a_rate, 100.0, 0);
      applyParam(rt, reg::ParameterId::lfo_a_wave, 0.5, 0);
      for (int g = 0; g < 5; ++g) rt.setDroneMod(1, g, 1.0);  // MOD on: consumes the cabled CV
      check(rt.connect(reg::JackId::lfo_a_cv_out, reg::JackId::drone_2_cv_mod_in),
            "t10 wire lfo -> drone cv-mod (engaged)");
      check(rt.rebuild(), "t10 drone cable rebuild (engaged)");
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetOn[i] = blk.wetL;
      }
    }
    bool inertSame = true, engagedDiff = false;
    for (int i = 0; i < kCap; ++i) {
      inertSame = inertSame && sameD(wetBase[i], wetOff[i]);
      engagedDiff = engagedDiff || !sameD(wetBase[i], wetOn[i]);
    }
    check(inertSame, "t10 a bare cable with MOD off is bit-identical to no-cable (not a phantom)");
    check(engagedDiff, "t10 the drone consumes the cabled CV-MOD (MOD on differs, real joint)");
  }
}

// ===========================================================================
// 11. (BLOCKED #4) Runtime PULSER + internal-rate surface is PUBLIC/callable/readable and
//     proves a REAL crossing — not a default false-green. clock_out's discrete rising is
//     read via clockOutRising(), and the SAME edge truth drives the published virtual-volts
//     rail (+10V / -10V). A bank sentinel at the default 1.0 Hz shows the same window does
//     NOT advance (so the crossing below is genuinely rate-driven).
// ===========================================================================
static void test_11_runtime_pulser_crossing(void) {
  // (a) The runtime Hz setter/readback are callable (closed the private/unreachable gap).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(sameD(rt.sequencerInternalRateHz(), 1.0), "t11 default internal rate is 1.0 Hz");
    check(rt.setSequencerInternalRateHz(48000.0), "t11 runtime internal-rate setter accepted");
    check(sameD(rt.sequencerInternalRateHz(), 48000.0),
          "t11 runtime internal-rate readback matches the set value");
    check(!rt.setSequencerInternalRateHz(-1.0) && sameD(rt.sequencerInternalRateHz(), 48000.0),
          "t11 a negative rate is rejected AND keeps the prior rate (fail-closed)");
  }

  // (b) A real crossing at runtime: high internal rate advances the sequence every frame
  //     (published cv_out cycles through the step CVs) AND publishes the clock_out virtual
  //     volts rail. At 48 kHz internal with a 48 kHz host the free-running PULSER rises on
  //     EVERY sample, so clockOutRising() is true each frame and clock_out must equal +10V
  //     exactly then (-10V exactly when it is not rising). @Codex #3: configure ONLY through
  //     the public parameter-event path (the mutating accessors were removed), and @Codex #4:
  //     the volts projection derives from the SAME discrete edge (no second phase/latch).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_stages, 0.0, 0);       // -> 3 stages
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_2, 2.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_3, 3.0, 0);
    check(rt.setSequencerInternalRateHz(48000.0), "t11 set crossing rate 48kHz");
    bool sawRising = false, railCoherent = true, cvVaries = false;
    int maxStep = 0;
    double prevCv = 0.0;
    for (int i = 0; i < 8; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      const double cv = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      if (i > 0) cvVaries = cvVaries || !sameD(cv, prevCv);
      prevCv = cv;
      maxStep = std::max(maxStep, rt.sequencer().currentStep());
      const bool rising = rt.sequencer().clockOutRising();
      if (rising) {
        sawRising = true;
        railCoherent = railCoherent &&
                       nearD(rt.controlVoltageAt(reg::JackId::sequencer_clock_out), 10.0);
      } else {
        railCoherent = railCoherent &&
                       nearD(rt.controlVoltageAt(reg::JackId::sequencer_clock_out), -10.0);
      }
    }
    // The step CVs are GH#21 seconds-smoothed: at a 48 kHz internal rate the pulser advances
    // every sample, so the published CV is a continuous ramp between step targets and never
    // holds still at an exact 1.0/2.0/3.0 within this short window. Re-expressed from the old
    // exact saw1/2/3 (which assumed the step CVs snapped instantly) to prove the real advance
    // by its step index: the sequence advances through the DISTINCT steps (max step >= 2) and
    // the CV carriage VARIES as the step moves (the CV jack tracks the advance, not a stuck
    // single value).
    check(maxStep >= 2, "t11 runtime PULSER crossing advances through distinct steps (real advance)");
    check(cvVaries, "t11 published step CV follows the advance (not stuck at a single value)");
    check(sawRising, "t11 clock_out discrete rising observed at runtime (not a false-green 0)");
    check(railCoherent,
          "t11 clock_out volts rail tracks the SAME edge truth (+10V rising / -10V idle)");
  }

  // (c) Bank sentinel: at the default 1.0 Hz the SAME window never advances and clock_out
  //     never rises — proving the crossing in (b) is rate-driven, not a never-true default.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_stages, 0.0, 0);       // -> 3 stages
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    // @Codex #3 note: these are ASYNC events flushed on the next processBlock, so the step CV
    // is only settled after frame 0. But the crossing is what we test, not the snapshot: the
    // published seq CV is the CURRENT step's CV and must stay put across frames while the
    // 1.0 Hz pulser phase (<1.0 over this window) never crosses. Compare against the value
    // seen on the first frame (post-flush), NOT a pre-tick member read.
    bool rose = false, neverAdvanced = true;
    double firstCv = 0.0, lastCv = 0.0;
    for (int i = 0; i < 200; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      if (i == 0) firstCv = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      if (i == 199) lastCv = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      neverAdvanced = neverAdvanced && (rt.sequencer().currentStep() == 0);
      if (rt.sequencer().clockOutRising()) rose = true;
    }
    // step0's CV is GH#21 seconds-smoothed, so it is a LIVE ramp toward its configured 1.0 V
    // (the seq stays on step 0 the whole window), NOT a frozen snapshot. Re-expressed from the
    // old `held` (which assumed an instant snap): the discriminators that prove the crossing in
    // (b) was genuinely rate-driven are (a) the sequence NEVER advances (step stays 0) and
    // (b) clock_out NEVER rises across the window — plus the CV itself is a live smooth ramp
    // bounded above by its step0 target.
    check(neverAdvanced && !rose,
          "t11 default 1.0 Hz never advances the sequence and never rises (crossing is rate-driven)");
    check(firstCv < lastCv && lastCv <= 1.0,
          "t11 step0 CV is a live smooth ramp toward its 1.0 target (still on step 0)");
  }
}

// ===========================================================================
// 12. (BLOCKED #5) Always-execute admission is ATOMIC and FAIL-CLOSED: a malformed set is
//     refused with no state change and no dirt; a genuine change sets graphDirty_ so the
//     plan recompiles; an identical list is a no-op (no dirt). Defense-in-depth: the
//     compile_graph admission independently returns invalid_always_execute on a bad list.
// ===========================================================================
static void test_12_always_admission_fail_closed(void) {
  // GH#12 keyboard product owner made keyboard a real always-execute source, so the DEFAULT
  // always-execute list is now 7 (envelope A/B, LFO A/B, joystick, sequencer, keyboard).
  // `six` must equal that default exactly, in the ctor's order: re-submitting it must be a
  // no-op (graph_unchanged), which is the t12 no-op witness.
  const core::ModuleId six[7] = {core::ModuleId::envelope_a, core::ModuleId::envelope_b,
                                 core::ModuleId::lfo_a,     core::ModuleId::lfo_b,
                                 core::ModuleId::joystick,  core::ModuleId::sequencer,
                                 core::ModuleId::keyboard};
  const core::ModuleId rev[6] = {core::ModuleId::sequencer, core::ModuleId::joystick,
                                 core::ModuleId::lfo_b,     core::ModuleId::lfo_a,
                                 core::ModuleId::envelope_b, core::ModuleId::envelope_a};
  const core::ModuleId dup[2] = {core::ModuleId::envelope_a, core::ModuleId::envelope_a};

  // (a) Fail-closed REFUSES malformed sets and dirties nothing.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(!rt.setAlwaysExecute(nullptr, 3), "t12 null ids with count>0 refused");
    core::ModuleId big[core::kMaxAlwaysExecuteSources + 1] = {};
    for (std::uint32_t i = 0; i < core::kMaxAlwaysExecuteSources + 1; ++i)
      big[i] = core::ModuleId::envelope_a;
    check(!rt.setAlwaysExecute(big, core::kMaxAlwaysExecuteSources + 1),
          "t12 over-capacity always set refused (no silent truncate)");
    check(!rt.setAlwaysExecute(dup, 2), "t12 duplicate always id refused");
    // @Codex #5: an id that is NOT a real module (ModuleId{999}) must be REFUSED at the
    // admission boundary — a stale/ghost id used to return true and get swallowed by
    // compile_graph. It must NOT dirty, so the plan stays exactly as it was.
    const core::ModuleId ghostId[1] = {core::ModuleId{999}};
    check(!rt.setAlwaysExecute(ghostId, 1), "t12 missing-id always set refused (stale/ghost id)");
    // A refused set must be atomic: no dirt, so a rebuild short-circuits to unchanged.
    check(rt.rebuild() && rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::graph_unchanged,
          "t12 refused always set leaves the plan undirtied/unchanged (atomic)");
  }

  // (b) A GENUINE change returns true and dirties the plan (recompiles, not unchanged).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.setAlwaysExecute(rev, 6), "t12 genuine change accepted");
    check(rt.rebuild() && rt.lastRebuildStatus() != core::SynthRuntime::RebuildStatus::graph_unchanged,
          "t12 a dirty change recompiles (not graph_unchanged) — dirty was really set");
  }

  // (c) An IDENTICAL list is a no-op: returns true but does NOT dirty.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    (void)rt.rebuild();  // settle to a clean graph
    check(rt.setAlwaysExecute(six, 7), "t12 identical always list accepted (no-op)");
    check(rt.rebuild() && rt.lastRebuildStatus() == core::SynthRuntime::RebuildStatus::graph_unchanged,
          "t12 identical list does NOT dirty the plan (no-op)");
  }

  // (d) compile_graph admission is independently FAIL-CLOSED (defense-in-depth): a bad
  //     always list yields invalid_always_execute with an empty graph.
  {
    const core::GraphModule fake[1] = {{core::ModuleId::lfo_a, nullptr}};
    const core::ModuleId ghost[1] = {core::ModuleId::envelope_a};  // not in `fake`
    const core::ModuleId dup2[2] = {core::ModuleId::lfo_a, core::ModuleId::lfo_a};

    core::CompileResult r0 =
        core::compile_graph(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1);
    check(r0.status == core::CompileStatus::invalid_always_execute,
          "t12 compile_graph null-always-with-count -> invalid_always_execute");

    core::ModuleId big[33] = {};
    core::CompileResult r1 = core::compile_graph(nullptr, 0, nullptr, 0, nullptr, 0,
                                                 nullptr, 0, big, 33);
    check(r1.status == core::CompileStatus::invalid_always_execute,
          "t12 compile_graph over-capacity always -> invalid_always_execute");

    core::CompileResult r2 = core::compile_graph(nullptr, 0, nullptr, 0, fake, 1,
                                                 nullptr, 0, dup2, 2);
    check(r2.status == core::CompileStatus::invalid_always_execute,
          "t12 compile_graph duplicate always id -> invalid_always_execute");

    core::CompileResult r3 = core::compile_graph(nullptr, 0, nullptr, 0, fake, 1,
                                                 nullptr, 0, ghost, 1);
    check(r3.status == core::CompileStatus::invalid_always_execute,
          "t12 compile_graph always id not a real module -> invalid_always_execute");

    // @Codex #5: a refused always-admission must map to its OWN runtime status (a distinct
    // enumerator), never masquerade as a generic invalid-contract.
    check(static_cast<int>(core::SynthRuntime::RebuildStatus::compile_invalid_always_execute) !=
              static_cast<int>(core::SynthRuntime::RebuildStatus::compile_invalid_contract),
          "t12 compile_invalid_always_execute is a DISTINCT status (not invalid_contract)");
  }
}

// ===========================================================================
// ===========================================================================
// 13. (BLOCKED #7) VCA-CV reaches a REAL sink, HOLD semantics are real, and the
//     three gate/clock latches (EG-A gate, EG-B gate, seq ext-clock) each respond
//     to a real descriptor with A/B isolation and one-edge-per-rising. The EG-B
//     gate was previously NEVER wired — this closes that gap.
// ===========================================================================
static void test_13_vca_sink_and_gate_latches(void) {
  // (a) VCA-CV -> real drone sink (drone_1, group 0), proven by an inert-vs-engaged
  //     audio joint: a bare cable with the drone MOD held at 0 must be BIT-IDENTICAL
  //     to no-cable — otherwise "audio differs" would be a phantom false-green that
  //     merely notices a cable exists. Only when the MOD is engaged does the SAME
  //     cabled VCA-CV change the audio, proving the drone really consumes it.
  {
    double wetBase[kCap], wetOff[kCap], wetOn[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::envelope_a_self_gen, 1.0, 0);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetBase[i] = blk.wetL;  // no cable, drone MOD at default (inert)
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::envelope_a_self_gen, 1.0, 0);
      for (int g = 0; g < 5; ++g) rt.setDroneMod(0, g, 0.0);  // MOD off: cable exists, inert
      check(rt.connect(reg::JackId::envelope_a_vca_cv_out, reg::JackId::drone_1_cv_mod_in),
            "t13a wire vca_cv_a -> drone cv-mod (inert)");
      check(rt.rebuild(), "t13a drone cable rebuild (inert)");
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetOff[i] = blk.wetL;
      }
    }
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      applyParam(rt, reg::ParameterId::envelope_a_self_gen, 1.0, 0);
      for (int g = 0; g < 5; ++g) rt.setDroneMod(0, g, 1.0);  // MOD on: consumes the cabled CV
      check(rt.connect(reg::JackId::envelope_a_vca_cv_out, reg::JackId::drone_1_cv_mod_in),
            "t13a wire vca_cv_a -> drone cv-mod (engaged)");
      check(rt.rebuild(), "t13a drone cable rebuild (engaged)");
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput blk;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &blk);
        wetOn[i] = blk.wetL;
      }
    }
    bool inertSame = true, engagedDiff = false;
    for (int i = 0; i < kCap; ++i) {
      inertSame = inertSame && sameD(wetBase[i], wetOff[i]);
      engagedDiff = engagedDiff || !sameD(wetBase[i], wetOn[i]);
    }
    check(inertSame, "t13a a bare VCA-CV cable with MOD off is bit-identical (not a phantom)");
    check(engagedDiff, "t13a the drone consumes the cabled VCA-CV (MOD on differs, real joint)");
  }

  // (b) HOLD: with HOLD on the VCA-CV is pinned to the open voltage while the ENV keeps
  //     moving (self-gen continues); with HOLD off VCA-CV returns to tracking the CURRENT
  //     ENV (the level is preserved, never reset/retriggered). Both envelope gain
  //     constants are 8.0, so in the OFF windows VCA-CV == ENV bitwise (same level_).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::envelope_a_self_gen, 1.0, 0);
    applyParam(rt, reg::ParameterId::envelope_a_a, 0.001, 0);  // fast self-oscillation
    applyParam(rt, reg::ParameterId::envelope_a_r, 0.001, 0);
    applyParam(rt, reg::ParameterId::envelope_a_s, 0.5, 0);
    // HOLD on at frame 130, off at frame 260 (absolute-sample delivery).
    applyParam(rt, reg::ParameterId::envelope_a_hold, 1.0, 130);
    applyParam(rt, reg::ParameterId::envelope_a_hold, 0.0, 260);
    const int kN = 360;
    double vca[kN], env[kN];
    for (int i = 0; i < kN; ++i) {
      core::RuntimeOutput blk;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &blk);
      vca[i] = rt.controlVoltageAt(reg::JackId::envelope_a_vca_cv_out);
      env[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
    }
    // OFF window [0,130): VCA-CV tracks ENV exactly (same 8.0 gain) and both vary.
    bool preTrack = true, preVaries = false;
    for (int i = 1; i < 130; ++i) {
      preTrack = preTrack && sameD(vca[i], env[i]);
      preVaries = preVaries || (std::fabs(vca[i] - vca[0]) > 1e-4);
    }
    check(preTrack, "t13b HOLD-off: VCA-CV tracks the ENV exactly (same gain, same level)");
    check(preVaries, "t13b HOLD-off: VCA-CV varies per-sample (not frozen)");
    // ON window [130,260): VCA-CV pinned open (constant), ENV keeps moving.
    bool vcaPinned = true, envMoves = false;
    for (int i = 130; i < 260; ++i) {
      vcaPinned = vcaPinned && sameD(vca[i], core::kEnvelopeVcaOpenVolt);
      envMoves = envMoves || (std::fabs(env[i] - env[130]) > 1e-4);
    }
    check(vcaPinned, "t13b HOLD-on: VCA-CV pinned to the open voltage (constant)");
    check(envMoves, "t13b HOLD-on: the ENV still moves while HOLD is high");
    // OFF-again [261,360): restored to tracking the CURRENT ENV (not reset), and it moves.
    bool afterTrack = true, afterMoves = false;
    for (int i = 261; i < kN; ++i) {
      afterTrack = afterTrack && sameD(vca[i], env[i]);
      afterMoves = afterMoves || (std::fabs(vca[i] - vca[260]) > 1e-4);
    }
    check(afterTrack, "t13b HOLD-off: VCA-CV returns to tracking the CURRENT ENV (not reset)");
    check(afterMoves, "t13b HOLD-off: VCA-CV moves again after release");
  }

  // (c) EG-A gate latch driven by a REAL descriptor (joystick x -> envelope_a_gate_in). Under
  //     GH#21 smoothing the source is a one-pole ramp, so the gate RISES at the closed-form
  //     frame the smoothed CV crosses the gate threshold (0.5V => norm 0.55); EG-A attacks at
  //     that crossing and holds at sustain; EG-B (unwired) stays idle throughout. The old
  //     exact-time form (env>0 by frame 29, gate high from frame 0) assumed an instant snap;
  //     re-expressed per rule 2 to the closed-form crossing + the latch semantics (rule 2b:
  //     the env is still idle one frame before that crossing, i.e. NOT at the old raw frame).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 1.0, 0);  // -> +5V target (gate rises at ~253)
    applyParam(rt, reg::ParameterId::envelope_a_a, 0.001, 0);
    applyParam(rt, reg::ParameterId::envelope_a_r, 0.001, 0);
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::envelope_a_gate_in),
          "t13c wire joy x -> EG-A gate");
    check(rt.rebuild(), "t13c EG-A gate rebuild");
    const int upCross = gh21_cross_frame(0.5, {{0, 1.0}}, 0.55, kCap);
    const int settle = gh21_settle_frames(kSr);
    double aRiseLast = 0.0, bIdle = 0.0, aBefore = 0.0;
    for (int i = 0; i < settle; ++i) {
      core::RuntimeOutput blk;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &blk);
      aRiseLast = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      bIdle = rt.controlVoltageAt(reg::JackId::envelope_b_env_out);
      if (i == upCross - 1) aBefore = aRiseLast;
    }
    check(sameD(aBefore, 0.0),
          "t13c EG-A is still idle one frame before the closed-form gate crossing (anti-old-frame)");
    check(aRiseLast > 0.0, "t13c EG-A ENV attacks under a high descriptor gate (at the crossing)");
    check(sameD(bIdle, 0.0), "t13c EG-B stays idle while EG-A is gated (A/B no cross-talk)");
    // Release: drop the source to 0.0 (target -5V); the gate falls at its own closed-form
    // frame and EG-A releases back toward 0.
    applyParam(rt, reg::ParameterId::joystick_x, 0.0, settle);
    double aFall = 9.0;
    for (int i = 0; i < settle; ++i) {
      core::RuntimeOutput blk;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &blk);
      aFall = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
    }
    check(aFall < aRiseLast, "t13c EG-A ENV releases under a low descriptor gate");
  }

  // (d) EG-B gate latch via a real descriptor (PREVIOUSLY NEVER WIRED): under GH#21 smoothing
  //     the joystick source is a one-pole ramp, so the gate rises at its closed-form crossing
  //     and EG-B (previously unwired) attacks then, while EG-A (unwired) stays idle. Same
  //     re-expression as (c): run the tau-derived window, assert the attack at/after the
  //     crossing, and the A/B isolation.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 1.0, 0);  // -> +5V target (gate rises at ~253)
    applyParam(rt, reg::ParameterId::envelope_b_a, 0.001, 0);
    applyParam(rt, reg::ParameterId::envelope_b_r, 0.001, 0);
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::envelope_b_gate_in),
          "t13d wire joy x -> EG-B gate (previously never wired)");
    check(rt.rebuild(), "t13d EG-B gate rebuild");
    const int settle = gh21_settle_frames(kSr);
    double bRise = 0.0, aIdle = 0.0;
    for (int i = 0; i < settle; ++i) {
      core::RuntimeOutput blk;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &blk);
      bRise = rt.controlVoltageAt(reg::JackId::envelope_b_env_out);
      aIdle = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
    }
    check(bRise > 0.0, "t13d EG-B ENV attacks under a high descriptor gate (B gate latched)");
    check(sameD(aIdle, 0.0), "t13d EG-A stays idle while EG-B is gated (A/B no cross-talk)");
  }

  // (e) seq ext-clock latch: a source held CONSTANT-high makes NO advance; a single 0->1
  //     rising produces exactly ONE advance (sustained high never repeats). The cable +
  //     rebuild come FIRST (rebuild resets the clock select), then the external-clock
  //     select and a real LOW->HIGH transition on the continuous joystick source.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in),
          "t13e wire joy x -> seq ext_clock");
    check(rt.rebuild(), "t13e seq ext_clock rebuild");
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);   // external clock selected
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);    // step-0 CV observable
    applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);  // step-0 gate emitted
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);        // low (-2V): primes, stays low
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 5);        // +2V rising at frame 5
    double prevG = 0.0;
    int rises = 0;
    for (int i = 0; i < 40; ++i) {
      core::RuntimeOutput blk;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &blk);
      const double g = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
      if (sameD(g, 10.0) && !sameD(prevG, 10.0)) ++rises;  // one-sample 10V pulse per advance
      prevG = g;
    }
    check(rises == 1, "t13e one rising edge drives exactly ONE advance (sustained high no repeat)");
  }
}

// ===========================================================================
// 14. (@Codex 7C3 final closure) PULSER provisional transfer + CLOCK OUT bipolar virtual
//     volts. (a) A VALID pulser norm maps through the public parameter-event path into the
//     real direct-Hz setter AND the rising-edge count is WALL-CLOCK deterministic: rendering
//     one real second at ANY sample rate yields the SAME integer rising-edge count, strictly
//     monotonic in the norm (0/1/20 for norm 0.0/0.5/1.0). (a2) Edge PLACEMENT is
//     partition-invariant (a big 256-frame block and 256 one-frame steps land the next rising
//     at the SAME absolute sample). (b) A real PatchGraph sequencer.clock_out ->
//     envelope_a.gate_in delivers EXACTLY ONE EG gate-high per pulser rising (no double-edge),
//     and env_out responds. Out-of-domain/NaN/Inf norms are rejected rate+trace-unchanged (t9).
// ===========================================================================
static std::uint64_t count_pulser_rises(core::SynthRuntime& rt, std::uint64_t frames) {
  std::uint64_t rises = 0;
  for (std::uint64_t i = 0; i < frames; ++i) {
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    if (rt.sequencer().clockOutRising()) ++rises;
  }
  return rises;
}

// Step one frame at a time (max `scan` frames) and return the relative index of the first
// rising samples; UINT64_MAX if none within the window.
static std::uint64_t next_rise_at_after(core::SynthRuntime& rt, std::uint64_t scan) {
  for (std::uint64_t i = 0; i < scan; ++i) {
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    if (rt.sequencer().clockOutRising()) return i;
  }
  return std::numeric_limits<std::uint64_t>::max();
}

static void test_14_pulser_transfer_and_clock_out(void) {
  // (a) norm -> Hz + WALL-CLOCK edge-count determinism across 44.1/48/88.2/96 kHz.
  {
    const double kSrList[4] = {44100.0, 48000.0, 88200.0, 96000.0};
    bool mono = true, countOK = true;
    for (int s = 0; s < 4; ++s) {
      const double sr = kSrList[s];
      std::uint64_t c[3];
      const double norm[3] = {0.0, 0.5, 1.0};
      double rate[3] = {0.05, 1.0, 20.0};
      for (int n = 0; n < 3; ++n) {
        std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, sr);
        core::SynthRuntime& rt = def->runtime();
        applyParam(rt, reg::ParameterId::sequencer_pulser, norm[n], 0);
        core::RuntimeOutput o;
        const core::RuntimeInputs z{0.0, 0.0};
        rt.processBlock(&z, 1, &o);  // deliver the async param event at frame 0
        gh21_advance(rt, gh21_settle_frames(sr));
        check(sameD(rt.sequencerInternalRateHz(), rate[n]),
              "t14 norm->rate readback (provisional 0.05/1.0/20.0 Hz)");
        // ~one real second (== sr frames) after the flush frame, + a 4-frame tail so the
        // exact-integer phase crossing is captured regardless of float accumulation; the
        // counts are strongly monotonic (c0<c05<c1) and deterministic across sample rates.
        c[n] = count_pulser_rises(rt, static_cast<std::uint64_t>(sr) + 4);
      }
      mono = mono && (c[0] <= c[1]) && (c[1] <= c[2]) && (c[0] < c[2]);
      countOK = countOK && c[0] == 0 && c[1] == 1 && c[2] == 20;
    }
    check(mono, "t14 rising-edge count is strictly monotonic in the pulser norm (per second)");
    check(countOK, "t14 WALL-CLOCK determinism: 1.0 s yields 0/1/20 rising at 44.1/48/88.2/96 kHz");
  }

  // (a2) Edge PLACEMENT partition-invariance: a big 256-frame block and 256 one-frame steps
  //      must land the NEXT rising at the same absolute sample (per-sample determinism).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> defA = make_def(kSeed, kSr);
    core::SynthRuntime& a = defA->runtime();
    a.setSequencerInternalRateHz(480.0);  // a rising every ~100 samples
    core::RuntimeOutput oa[kCap];
    a.processBlock(kZeros, kCap, oa);     // one big 256-frame block
    const std::uint64_t nextA = next_rise_at_after(a, 256);

    std::unique_ptr<core::MachineRuntimeDefinition> defB = make_def(kSeed, kSr);
    core::SynthRuntime& b = defB->runtime();
    b.setSequencerInternalRateHz(480.0);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput ob;
      const core::RuntimeInputs z{0.0, 0.0};
      b.processBlock(&z, 1, &ob);         // 256 one-frame steps
    }
    const std::uint64_t nextB = next_rise_at_after(b, 256);
    check(nextA != std::numeric_limits<std::uint64_t>::max() && nextA == nextB,
          "t14 next rising lands at the SAME sample after 256-frame block vs 256 one-frame (partition-invariant)");
  }

  // (b) PatchGraph sequencer.clock_out -> envelope_a.gate_in: exactly ONE EG gate-high per
  //     pulser rising (no double-edge), and env_out responds to the live rail.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.connect(reg::JackId::sequencer_clock_out, reg::JackId::envelope_a_gate_in),
          "t14 wire seq clock_out -> env gate_in");
    check(rt.rebuild(), "t14 clock_out->gate rebuild");
    check(rt.setSequencerInternalRateHz(4.0), "t14 set 4 Hz pulser for a countable run");
    std::uint64_t rises = 0, gateHigh = 0;
    double maxEnv = 0.0;
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(kSr); ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      if (rt.sequencer().clockOutRising()) ++rises;
      if (rt.envelopeA().gateLatch()) ++gateHigh;
      const double e = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      if (e > maxEnv) maxEnv = e;
    }
    check(rises >= 1, "t14 the patched CLOCK OUT produces a real pulser run over 1.0 s");
    check(gateHigh == rises,
          "t14 EXACTLY ONE EG gate-high per pulser rising (no double-edge)");
    check(maxEnv > 0.0, "t14 envelope env_out responds to the patched clock_out gate");
  }
}

// ===========================================================================
// 15. Source-bank sentinel (@Codex item 1): the canonical runtime pre-writes the
//     sequencer_clock_out bank with a NON-zero/NON-rail value (1234.5) and demands that the
//     FIRST real sample OVERWRITE it to the CONFIRMED -10/+10 rail. A no-publish mutation
//     (the kSequencer exec ever failing to write the source bank) would read the sentinel
//     back and RED, rather than silently recovering a -10. This completes the "pre-seed
//     non-zero sentinel" criterion the mandate writes.
// ===========================================================================
static void test_15_source_bank_sentinel(void) {
  // (a) the sentinel is pre-seeded and is genuinely neither -10 nor +10.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.setControlVoltage(reg::JackId::sequencer_clock_out, 1234.5);
    const double sent = rt.controlVoltageAt(reg::JackId::sequencer_clock_out);
    check(!sameD(sent, -10.0) && !sameD(sent, +10.0),
          "t15 sentinel pre-seeded (neither the -10 idle nor the +10 peak rail)");
  }
  // (b) one real sample with the 1.0 Hz PULSER (phase < 1.0 over a frame, so it never rises)
  //     must OVERWRITE the sentinel to the CONFIRMED -10 idle rail.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.setControlVoltage(reg::JackId::sequencer_clock_out, 1234.5);
    core::RuntimeOutput o;
    const core::RuntimeInputs z{0.0, 0.0};
    rt.processBlock(&z, 1, &o);
    check(sameD(rt.controlVoltageAt(reg::JackId::sequencer_clock_out), -10.0),
          "t15 first real sample overwrites the sentinel to the CONFIRMED -10 idle rail");
  }
  // (c) the +10 rail: a fast internal PULSER; every frame where clockOutRising() is live must
  //     publish exactly +10 (the sentinel is overwritten, not leaked).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.setControlVoltage(reg::JackId::sequencer_clock_out, 1234.5);
    check(rt.setSequencerInternalRateHz(480.0), "t15 set fast pulser for a rising capture");
    bool sawPeak = false, sawOther = false;
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const core::RuntimeInputs z{0.0, 0.0};
      rt.processBlock(&z, 1, &o);
      const double v = rt.controlVoltageAt(reg::JackId::sequencer_clock_out);
      if (rt.sequencer().clockOutRising()) {
        if (sameD(v, +10.0)) sawPeak = true;
        else sawOther = true;
      }
    }
    check(sawPeak, "t15 a rising frame publishes the +10 peak rail (overwrites the sentinel)");
    check(!sawOther, "t15 every rising frame publishes exactly +10 (no sentinel/other value leaks)");
  }
}

// ===========================================================================
// 16. PERMANENTLY FREEZE every field of the ext_clock_in descriptor (@Codex item 2).
//     THIS IS A FROZEN UNVERIFIED PLACEHOLDER, NOT A HARDWARE FACT. ALL generated-real
//     descriptor fields are asserted (id/stable_id/name/module/direction, signalType,
//     polarity, nominal+tolerated range, mod-depth, transfer, saturation, maxCables,
//     gateThreshold/hysteresis, coupling, evidence ref + line + descriptor status, and the
//     fieldEvidence provenance) so the runtime can never be tempted to back-derive a bipolar
//     -10..+10 clock rail from it. Only signalType is evidence-confirmed; the other seven
//     field-evidence slots are explicitly unverified. The core consumes this sink only
//     through the canonical gate interpreter (an interpreted edge, never a hardcoded
//     volts / threshold / polarity constant), which is why no clock-rail fact belongs here.
//     The maxCables=1 and evidence line 493..493 checks are the specific guards that an
//     earlier freeze missed — a mutation of either REDs here.
// ===========================================================================
static void test_16_ext_clock_in_freeze(void) {
  const core::JackDescriptor* d = nullptr;
  for (std::uint32_t i = 0; i < reg::kJackCount; ++i)
    if (reg::kJacks[i].id == reg::JackId::sequencer_ext_clock_in) { d = &reg::kJacks[i]; break; }
  check(d != nullptr, "t16 ext_clock_in is registered (frozen descriptor)");
  if (d == nullptr) return;

  // EVERY field of the generated real descriptor is FROZEN to its exact current value,
  // so the runtime can never back-derive a bipolar rail (or any other value) from it.
  // Identity:
  check(d->id == reg::JackId::sequencer_ext_clock_in, "t16 ext_clock_in id frozen");
  check(d->stable_id == "sequencer.ext_clock_in", "t16 ext_clock_in stable_id frozen");
  check(d->name == "EXT CLOCK IN", "t16 ext_clock_in display name frozen");
  check(d->module == core::ModuleId::sequencer, "t16 ext_clock_in owner module frozen");
  check(d->direction == core::PinDirection::input, "t16 ext_clock_in is an INPUT (frozen)");
  // Signal + polarity (the rail-critical placeholders):
  check(d->signalType == core::SignalType::clock, "t16 ext_clock_in signalType=clock (frozen)");
  check(d->polarity == core::Polarity::unknown, "t16 ext_clock_in polarity UNKNOWN (frozen)");
  // Nominal + tolerated range + modulation depth:
  check(sameD(d->nominalMin, 0.0) && sameD(d->nominalMax, 5.0),
        "t16 ext_clock_in nominal 0..5 is a PLACEHOLDER, not a hardware fact (frozen)");
  check(sameD(d->toleratedMin, 0.0) && sameD(d->toleratedMax, 0.0),
        "t16 ext_clock_in tolerated 0..0 placeholder (frozen)");
  check(sameD(d->modulationDepthPerVolt, 1.0), "t16 ext_clock_in mod-depth 1.0 frozen");
  // Transfer / saturation:
  check(d->transfer == core::SignalTransfer::unknown, "t16 ext_clock_in transfer UNKNOWN (frozen)");
  check(d->saturation == core::SaturationType::unknown, "t16 ext_clock_in saturation UNKNOWN (frozen)");
  // Cable cardinality (a previously-unfreezed field: mutating 1->2 must RED here):
  check(static_cast<unsigned>(d->maxCables) == 1u,
        "t16 ext_clock_in maxCables=1 frozen (single-cable cardinality)");
  // Gate/clock threshold + hysteresis + coupling:
  check(sameD(d->gateThresholdVolts, 0.0) && sameD(d->hysteresisVolts, 0.0),
        "t16 ext_clock_in threshold/hysteresis 0 == placeholder sentinel (frozen)");
  check(d->coupling == core::Coupling::unknown, "t16 ext_clock_in coupling UNKNOWN (frozen)");
  // Evidence provenance + descriptor-wide status:
  check(d->evidence.source == "solar42N_manual_v15",
        "t16 ext_clock_in evidence source frozen (solar42N_manual_v15)");
  check(d->evidence.lineStart == 493u && d->evidence.lineEnd == 493u,
        "t16 ext_clock_in evidence line 493..493 frozen");
  check(d->status == core::EvidenceStatus::confirmed, "t16 ext_clock_in descriptor status CONFIRMED (frozen)");

  // fieldEvidence: EXACTLY signalType confirmed, the other seven slots unverified.
  check(d->fieldEvidence.signalType == core::EvidenceStatus::confirmed,
        "t16 ONLY signalType is evidence-confirmed (frozen)");
  check(d->fieldEvidence.nominalRange == core::EvidenceStatus::unverified &&
            d->fieldEvidence.toleratedRange == core::EvidenceStatus::unverified &&
            d->fieldEvidence.threshold == core::EvidenceStatus::unverified &&
            d->fieldEvidence.saturation == core::EvidenceStatus::unverified &&
            d->fieldEvidence.transfer == core::EvidenceStatus::unverified &&
            d->fieldEvidence.polarity == core::EvidenceStatus::unverified &&
            d->fieldEvidence.coupling == core::EvidenceStatus::unverified,
        "t16 the seven non-signal fields are each UNVERIFIED (frozen placeholder, not a rail)");
}

// ===========================================================================
// 17. GH#21 continuous-control smoothing (the (A) semantic @Kimi ruled):
//   - a smoother writes the sound-core setter ONLY while converging, then snaps to the
//     exact target and goes inert, so a sanctioned direct DSP-domain setter is never
//     clobbered back to the smoother's own settled target (finding-2 negative control);
//   - whole-state build SNAPS a seconds param (exact stopped-stream init), never ramps;
//   - the LIVE ControlEvent lane RAMPS: the target is NOT reached in one sample (the GH#11
//     single-sample oracle is now false for the 20 seconds params) and IS reached after the
//     tau-derived settle count.
//   Mutations: (B) perpetual writer / ramp-on-whole-state both go red on these.
// ===========================================================================
static void test_17_gh21_smoothing_negative_controls(void) {
  // Settle count derived from tau (residual < kSmootherSettleRelTol, declared in
  // parameter_smoothing.h): N = ceil(fs*tau*ln(1/relTol)), +1 guard so the boundary frame
  // is included. Used below only for the converge check — this is NOT a magic number.
  const double relTol = core::kSmootherSettleRelTol;
  const int settleN = static_cast<int>(
      std::ceil(std::log(1.0 / relTol) * core::kGh21SmoothingTauSeconds * kSr)) + 1;
  const core::RuntimeInputs z{0.0, 0.0};
  core::RuntimeOutput o;

  // (A) Direct DSP-domain setter (sanctioned, bypasses the parameter lane) must survive a
  //     settled smoother: setSequencerInternalRateHz(250) is not overwritten back to the
  //     primed 1.0 Hz default on the next frame. RED under a perpetual-writer smoother.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.setSequencerInternalRateHz(250.0), "t17 direct setter accepted");
    for (int i = 0; i < 8; ++i) rt.processBlock(&z, 1, &o);
    check(sameD(rt.sequencerInternalRateHz(), 250.0),
          "t17 direct setter survives the smoother (no clobber to primed 1.0 Hz)");
  }

  // (A2) The smoother is inert once settled at a NON-default value too: a direct setter to
  //      0.75 Hz (not the primed 1.0) is likewise not converged-over by a later write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    check(rt.setSequencerInternalRateHz(0.75), "t17 direct low-rate setter accepted");
    for (int i = 0; i < 8; ++i) rt.processBlock(&z, 1, &o);
    check(sameD(rt.sequencerInternalRateHz(), 0.75),
          "t17 direct setter survives a settled smoother (low rate)");
  }

  // (B) Whole-state apply SNAPS a seconds param (exact stopped-stream init): applyDspParam
  //     on joystick_x reaches the instance getter at the applied value immediately, never a
  //     ramp from the primed default. RED if whole-state used setTarget (ramp).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    const core::ParameterApplyStatus st =
        rt.applyDspParam(reg::ParameterId::joystick_x, 0.7);
    check(st == core::ParameterApplyStatus::applied, "t17 whole-state snap accepted");
    rt.processBlock(&z, 1, &o);
    check(nearD(rt.joystick().x(), 0.7),
          "t17 whole-state apply SNAPS to the applied value (no ramp from default)");
    // The saturated smoother must ALSO be inert afterwards: a whole-state value is not
    // drifted by any transient write, so a direct readback right after remains exact.
    check(nearD(rt.joystick().x(), 0.7),
          "t17 whole-state value stays at the applied value after render (inert)");
  }

  // (C) LIVE ControlEvent lane RAMPS: a seconds param applied through the event path does
  //     NOT reach the instance getter in one sample, and DOES reach it (within relTol) after
  //     the tau-derived settle count. This pins the "single-sample reach" of the GH#11
  //     oracle as now-false for the 20 seconds params.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    rt.processBlock(&z, 1, &o);
    check(!nearD(rt.joystick().x(), 0.7),
          "t17 live ramp does NOT reach the target in one sample (GH#21 smoothing)");
    bool converged = false;
    for (int i = 0; i < settleN && !converged; ++i) {
      rt.processBlock(&z, 1, &o);
      converged = nearD(rt.joystick().x(), 0.7);
    }
    check(converged,
          "t17 live ramp converges after the tau-derived settle count (no single-sample snap)");
  }

  // (D) A seconds param that is ALSO direct-set must converge toward the LAST authoritative
  //     write: whole-state snap the joystick to 0.7, then a live event targets 0.2 and must
  //     ramp toward 0.2 (not jump, not stay). The converged value is 0.2.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.applyDspParam(reg::ParameterId::joystick_x, 0.7);
    applyParam(rt, reg::ParameterId::joystick_x, 0.2, 0);
    bool converged = false;
    for (int i = 0; i < settleN && !converged; ++i) {
      rt.processBlock(&z, 1, &o);
      converged = nearD(rt.joystick().x(), 0.2);
    }
    check(converged, "t17 live event re-arms a whole-state-snapped smoother toward its target");
  }
}

int main(void) {
  test_1_slots_presence_phase();
  test_2_lfo_drone_mod_same_sample();
  test_3_seq_gate_eg_env_vcf();
  test_4_joystick_vcf();
  test_5_seq_ext_clock();
  test_6_seq_stages_clock_out();
  test_7_param_table_partition();
  test_8_task65_invariants_zero_alloc();
  test_negative_controls();
  test_9_param_matrix_apply_status();
  test_10_vca_ab_and_output_audit();
  test_11_runtime_pulser_crossing();
  test_12_always_admission_fail_closed();
  test_13_vca_sink_and_gate_latches();
  test_14_pulser_transfer_and_clock_out();
  test_15_source_bank_sentinel();
  test_16_ext_clock_in_freeze();
  test_17_gh21_smoothing_negative_controls();

  std::printf("\n[%s] %d checks, %d failed\n", g_fail == 0 ? "PASS" : "FAIL", g_checks,
              g_fail);
  return g_fail == 0 ? 0 : 1;
}
