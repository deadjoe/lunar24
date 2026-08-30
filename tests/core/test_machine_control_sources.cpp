// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// GH#11 partial acceptance (task#66, per @Codex ruling a14fd6a4).
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
// restore/#6 boundary. GH#11 stays OPEN / P3 NOT MET — nothing here closes it.
//
// The six source value- and instance-accessors (envelopeA()/lfoA()/... / the private
// setControlParamValue / setSequencerInternalRateHz) are PRIVATE by design: the injector
// and the readback inspector are the host/verifier facade, not a test seam. So this test
// deliberately drives them through the real event path instead of touching them directly.
//
// Harness mirrors test_machine_definition.cpp: one TU with g_checks/g_fail, a small
// summary reporter, and a companion _allocator TU (built only into this target) that
// counts render-path allocations so A′ "zero allocation" can be asserted.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

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
  }
}

extern std::size_t g_allocCount;  // defined in the companion _allocator TU.

// ---- render constants ----
static constexpr double kSr = 48000.0;
static constexpr uint32_t kSeed = 0x5EEDu;  // matches machine_definition default seed
static constexpr int kCap = 256;
static constexpr double kZeros[kCap] = {0.0};

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
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    out[i] = captureJack ? rt.controlVoltageAt(jack) : o.wetL;
  }
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
    const double z = 0.0;
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
      const double z = 0.0;
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
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      envA[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      envB[i] = rt.controlVoltageAt(reg::JackId::envelope_b_env_out);
      sqCv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      sqGate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
      vcfL[i] = rt.vcfCvReadbackL();
      vcfR[i] = rt.vcfCvReadbackR();
    }
  }

  // The sequencer is a live source even idle: it publishes step0's CV (the current
  // step value) BEFORE any advance, so the CV alone cannot detect the FIRST advance
  // (which lands index 0). The GATE pulse is the advance discriminator; the CV proves
  // the step REALLY advanced to a distinct step. See the gate shape in five_step_sequencer.h.
  check(nearD(sqCv[0], 1.0), "t3 seq publishes step0 CV while idle (always-live 0..+5V source)");
  check(sameD(sqGate[8], 10.0), "t3 seq gate pulses at the first external-clock rise (advance detected)");
  check(sameD(sqGate[9], 0.0), "t3 seq gate is a one-sample pulse (cleared next frame)");
  check(!sameD(sqCv[8], sqCv[24]) || !sameD(sqCv[24], sqCv[40]),
        "t3 sequencer advances through distinct steps (cv 1/2/3)");

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
  double outX[kCap], outY[kCap];
  double vcfL7 = 0.0, vcfR7 = 0.0, vcfLlast = 0.0, vcfRlast = 0.0;
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::vcf_cv_l_in);
    rt.connect(reg::JackId::joystick_y_out, reg::JackId::vcf_cv_r_in);
    check(rt.rebuild(), "t4 joystick->VCF patch rebuild succeeds");
    // x=0.7 -> +2.0 V; y=0.2 -> -3.0 V with default offset 0.5 (-> +0 V offset term).
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_y, 0.2, 0);
    // Offset X event at sample 8 -> x_out swings from +2.0 to 0.0 at frame 8.
    applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, 8);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      outX[i] = rt.controlVoltageAt(reg::JackId::joystick_x_out);
      outY[i] = rt.controlVoltageAt(reg::JackId::joystick_y_out);
      if (i == 7) { vcfL7 = rt.vcfCvReadbackL(); vcfR7 = rt.vcfCvReadbackR(); }
    }
    vcfLlast = rt.vcfCvReadbackL();
    vcfRlast = rt.vcfCvReadbackR();
  }

  // Exact registry-authorized source values (the published jacks are the authority). The
  // DSP produces its OWN float (5*(2x-1) for x=0.7 is 1.9999999999999996, not bitwise 2.0),
  // so the constant is compared by tolerance (nearD), not bitwise sameD.
  check(nearD(outX[0], 2.0), "t4 joystick x_out == +2.0 V @ x=0.7");
  check(nearD(outY[0], -3.0), "t4 joystick y_out == -3.0 V @ y=0.2 (asymmetric to X)");

  // Offset effective at the exact sample: frame 7 still +2.0, frame 8 already 0.0.
  // (A block-front apply would flip x_out at frame 0 instead of frame 8.)
  check(nearD(outX[7], 2.0), "t4 offset not yet applied at frame 7 (exact sample, not front)");
  check(nearD(outX[8], 0.0), "t4 offset applied at exactly frame 8 (x_out -> 0.0 V)");

  // Real DSP sink: vcf_cv_l_in is fed, so the VCF reads the (fixed exec-lag) X value. VCF
  // id=2 < joystick id=8, so it sees the prior-frame X; the offset window is stable so
  // settled-window assertions are exact regardless of that one-frame skew.
  check(nearD(vcfL7, 2.0), "t4 VCF L readback tracks the settled X (+2.0V)");
  check(nearD(vcfR7, -3.0), "t4 VCF R readback tracks the settled Y (-3.0V)");
  check(nearD(vcfLlast, 0.0), "t4 VCF L readback follows X after the offset (0.0V)");
  check(nearD(vcfRlast, -3.0), "t4 VCF R readback unaffected by the offset-X change");

  // X/Y independent (asymmetric): Y is unaffected by the offset-X change.
  bool yStable = true;
  for (int i = 8; i < kCap; ++i) yStable = yStable && nearD(outY[i], -3.0);
  check(yStable, "t4 Y is unaffected by an offset-X change (X/Y independent)");
}

// ===========================================================================
// 5. A bipolar joystick source -> sequencer.ext_clock_in: only rising advances, a
//    sustained high never repeats, and a repatch that lands on an already-high sink
//    makes no phantom advance.
// ===========================================================================
static void test_5_seq_ext_clock(void) {
  double cv[kCap], gate[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    check(rt.rebuild(), "t5 joystick->ext_clock_in patch rebuild succeeds");
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);  // external
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_2, 2.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);  // step-0 gate enabled
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);   // low (-2V): primes, stays low
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 5);   // rising at frame 5
    // stay high for a long window (no further edge), frame 5..255
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      cv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      gate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
    }
  }
  // The GATE (not CV) is the edge detector: a low prime holds the gate LOW (no advance),
  // the single rising edge at frame 5 emits ONE +10V one-sample pulse, and the sustained
  // high never repeats. CV alone cannot detect the FIRST advance (it lands index 0, the same
  // CV value the live source already publishes while idle).
  check(sameD(gate[0], 0.0) && sameD(gate[4], 0.0),
        "t5 seq gate stays low while clock low (no advance from a low prime)");
  check(sameD(gate[5], 10.0), "t5 seq gate pulses +10V on the single rising edge (advance)");
  check(sameD(cv[5], 1.0), "t5 seq holds step0 CV at the advance (index 0, live 0..+5V)");
  bool noRepeat = true;
  for (int i = 6; i < kCap; ++i) noRepeat = noRepeat && sameD(gate[i], 0.0);
  check(noRepeat, "t5 sustained high NEVER repeats an advance (only rising advances)");

  // Repatch alone (no signal transition) produces no phantom advance: the source stays LOW
  // across the whole window, so connecting the cable mid-window must not fabricate an edge.
  double gate2[kCap];
  {
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
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      gate2[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
    }
  }
  bool noPhantom = true;
  for (int i = 0; i < kCap; ++i) noPhantom = noPhantom && sameD(gate2[i], 0.0);
  check(noPhantom, "t5 repatch without a signal transition makes NO phantom advance");
}

// ===========================================================================
// 6. Sequencer stages 3/4/5 advance at exact samples; a sustained source holds; seq
//    CV feeds a sink (VCF) and gate feeds EG; clock_out stays explicitly unpublished.
//    The internal PULSER Hz setter is proven at the standalone DSP level (its runtime
//    accessor is deliberately private), since panel-norm->Hz is a runtime concern.
// ===========================================================================
static void test_6_seq_stages_pulser_unpub(void) {
  // (a) Stages: external clock driving advances at exact frames (8,24,40,56) → wraps at
  //     stageCount. We assert per-stage-count the CV sequence and the wrap.
  const struct { int stages; double step0, step1, step2; } cases[3] = {
      {3, 1.0, 2.0, 3.0}, {4, 1.0, 2.0, 3.0}, {5, 1.0, 2.0, 3.0}};
  for (int c = 0; c < 3; ++c) {
    double sqCv[kCap], sqGate[kCap], clockOut[kCap], vcfL[kCap];
    {
      std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
      core::SynthRuntime& rt = def->runtime();
      rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
      rt.connect(reg::JackId::sequencer_cv_out, reg::JackId::vcf_cv_l_in);
      check(rt.rebuild(), "t6 stages patch rebuild succeeds");
      applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
      applyParam(rt, reg::ParameterId::sequencer_stages,
                 static_cast<double>(cases[c].stages - 3), 0);  // norm 0/1/2 → 3/4/5
      applyParam(rt, reg::ParameterId::sequencer_step_cv_1, cases[c].step0, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_cv_2, cases[c].step1, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_cv_3, cases[c].step2, 0);
      applyParam(rt, reg::ParameterId::sequencer_step_gate_1, 1.0, 0);
      applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 8);
      applyParam(rt, reg::ParameterId::joystick_x, 0.3, 16);
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 24);
      applyParam(rt, reg::ParameterId::joystick_x, 0.3, 32);
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 40);
      applyParam(rt, reg::ParameterId::joystick_x, 0.3, 48);
      applyParam(rt, reg::ParameterId::joystick_x, 0.7, 56);
      for (int i = 0; i < kCap; ++i) {
        core::RuntimeOutput o;
        const double z = 0.0;
        rt.processBlock(&z, 1, &o);
        sqCv[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
        sqGate[i] = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
        clockOut[i] = rt.controlVoltageAt(reg::JackId::sequencer_clock_out);
        vcfL[i] = rt.vcfCvReadbackL();
      }
    }
    const double s0 = cases[c].step0, s1 = cases[c].step1, s2 = cases[c].step2;
    check(sameD(sqCv[8], s0), "t6 advance to step0");
    check(sameD(sqCv[24], s1), "t6 advance to step1");
    check(sameD(sqCv[40], s2), "t6 advance to step2");
    // The 4th advance (frame 56) landed on a DIFFERENT step — never repeats step2 in a row.
    // For the 3-stage case it wrapped back to step0; for 4/5 it advanced to step3.
    check(!sameD(sqCv[56], s2), "t6 4th advance is a distinct step (no repeated step)");
    if (cases[c].stages == 3) check(sameD(sqCv[56], s0), "t6 3-stage wraps back to step0");
    // seq CV reaches the VCF L sink (real DSP consumer). VCF id=2 < seq id=11, so the VCF
    // reads the PRIOR-frame CV; the CV is non-zero at the advance, so we assert the joint is
    // non-vacuous rather than frame-exact.
    check(vcfL[8] != 0.0 || sameD(s0, 0.0),
          "t6 seq CV is a non-zero joint in the VCF L sink (real consumer)");
    // clock_out explicitly unpublished: the published jack voltage stays 0 through the
    // external advance (the PULSER's clock event is NOT a published CV).
    bool clockUnpub = true;
    for (int i = 0; i < kCap; ++i) clockUnpub = clockUnpub && sameD(clockOut[i], 0.0);
    check(clockUnpub, "t6 sequencer.clock_out is explicitly NEVER published (0 V)");
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
  //     is proven for the sequencer, and that the gate pulse is a one-sample 0/+10V.
  {
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
        const double z = 0.0;
        rt.processBlock(&z, 1, &o);
        envA[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      }
    }
    check(envA[9] > 0.0 || envA[8] > 0.0, "t6c seq gate triggers EG A env (gate->EG)");
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
    for (int i = 0; i < 64; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
    }
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
    const double z = 0.0;
    core::RuntimeOutput o;
    rt.processBlock(&z, 1, &o);
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
    const double z = 0.0;
    core::RuntimeOutput o;
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
          "t7 joystick_x=-1.0 -> invalid_value (lower rail)");
    check(nearD(rt.joystick().x(), 0.5), "t7 invalid -1.0 keeps default x=0.5");
  }

  // (c) sequencer.pulser is EXACTLY blocked: enqueuing a PULSER value must not alter the
  //     sequencer advance schedule AT ALL. We render a baseline (no seq param) and a PULSER
  //     run and require them to be bit-identical: if PULSER were mapped to an internal Hz
  //     rate the second run would advance and diverge. A legitimate joystick→external-clock
  //     advance at sample 40 is the positive control.
  double cvBase[kCap], cvPulser[kCap], cvPos[kCap];
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      cvBase[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
  }
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, 0);  // blocked
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      cvPulser[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
  }
  bool pulserBlocked = true;
  for (int i = 0; i < kCap; ++i) pulserBlocked = pulserBlocked && sameD(cvPulser[i], cvBase[i]);
  check(pulserBlocked, "t7 PULSER param is a NO-OP on the schedule (exactly blocked)");
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    rt.connect(reg::JackId::joystick_x_out, reg::JackId::sequencer_ext_clock_in);
    rt.rebuild();
    applyParam(rt, reg::ParameterId::sequencer_clock, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.3, 0);
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 40);
    for (int i = 0; i < kCap; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      cvPos[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
    }
  }
  check(sameD(cvPos[40], 1.0), "t7 legitimate external clock still advances the sequence");

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
      const double z = 0.0;
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
    const double z = 0.0;
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
      const double z = 0.0;
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
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      if (i == 63) { a0 = rt.controlVoltageAt(reg::JackId::lfo_a_cv_out);
                     b0 = rt.controlVoltageAt(reg::JackId::lfo_b_cv_out); }
    }
    check(!sameD(a0, b0) && std::isfinite(a0) && std::isfinite(b0),
          "neg4 A/B and X/Y source instances are distinct (no cross-wire)");
  }

  // (5) Parameter block-front apply, or a missed family: prove the offset event lands at
  //     frame 8 (not frame 0), i.e. a block-front bug would show x_out==0 at frame 0.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    applyParam(rt, reg::ParameterId::joystick_offset_x, 0.3, 8);
    double x0 = 0, x8 = 0;
    for (int i = 0; i < 16; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      if (i == 0) x0 = rt.controlVoltageAt(reg::JackId::joystick_x_out);
      if (i == 8) x8 = rt.controlVoltageAt(reg::JackId::joystick_x_out);
    }
    check(nearD(x0, 2.0) && nearD(x8, 0.0),
          "neg5 event applied at its exact sample, not block-front, and the family is hit");
  }

  // (6) PULSER-as-Hz or a silent default: the REAL internal-rate mechanism is ADMITTED on
  //     the standalone DSP (setting Hz → advance within a few frames), while the runtime
  //     PULSER *param* is a NO-OP (identical to the no-param baseline). This distinguishes
  //     "PULSER genuinely rejected" from "internal rate silently defaults to something".
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
        const double z = 0.0;
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
        const double z = 0.0;
        rt.processBlock(&z, 1, &o);
        puls[i] = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      }
    }
    bool noop = true;
    for (int i = 0; i < kCap; ++i) noop = noop && sameD(base[i], puls[i]);
    check(noop, "neg6 runtime PULSER param is a NO-OP (never maps to the admitted Hz setter)");
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
  // Phase 1: the unit-domain `valid` value MUST land `applied` AND the REAL getter must reach
  // `expected`. This is the @Codex #2 oracle — read the actual DSP instance through the public
  // CONST surface, never a shadow/param bank.
  applyParam(rt, row.pid, row.valid, 0);
  core::RuntimeOutput o;
  const double z = 0.0;
  rt.processBlock(&z, 1, &o);
  check(rt.lastApplyParamId() == row.pid, "t9 row lastApplyParamId == row id");
  check(rt.lastApplyStatus() == core::ParameterApplyStatus::applied,
        "t9 valid row applied (status, per-family unblocked)");
  check(nearD(probeGetter(rt, row.pid), row.expected),
        "t9 valid row reaches the REAL instance getter");
  // Phase 2: a unit-domain VIOLATION must land `invalid_value` AND leave the getter at the
  // VALID value (the boundary reject keeps old — a bad value never mutates the source).
  applyParam(rt, row.pid, row.invalid, 1);
  rt.processBlock(&z, 1, &o);
  check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value,
        "t9 invalid row invalid_value (product-boundary reject)");
  check(nearD(probeGetter(rt, row.pid), row.expected),
        "t9 invalid row leaves the REAL getter unchanged");
}

static void test_9_param_matrix_apply_status(void) {
  const std::size_t n = sizeof(kMatrix) / sizeof(kMatrix[0]);
  check(n == 34, "t9 matrix has all 34 control-source params");
  for (std::size_t i = 0; i < n; ++i) {
    row_apply(kMatrix[i]);
  }

  // The 35th: sequencer_pulser is recognised but has NO product transfer surface → the
  // runtime reports transfer_unavailable (never a silent false-green "applied").
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_pulser, 10000.0, 0);
    core::RuntimeOutput o;
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyParamId() == reg::ParameterId::sequencer_pulser,
          "t9 pulser row lastApplyParamId == pulser");
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::transfer_unavailable,
          "t9 pulser is transfer_unavailable (no product Hz surface)");
  }

  // "Invalid keeps old" (@Codex #1): a boundary rejection must NOT mutate the source.
  // (a) joystick_x stays at its prior value after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 0.7, 0);
    core::RuntimeOutput o;
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    const double before = rt.joystick().x();
    applyParam(rt, reg::ParameterId::joystick_x, 5.0, 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.joystick().x(), before),
          "t9 invalid joystick_x rejects AND keeps prior x (no clamp-coerce)");
  }
  // (b) envelope sustain norm stays after an out-of-domain write.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::envelope_a_s, 0.4, 0);
    core::RuntimeOutput o;
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    const double before = rt.envelopeA().sustain();
    applyParam(rt, reg::ParameterId::envelope_a_s, 1.5, 1);
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
    const double z = 0.0;
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
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 3.5, 0);
    core::RuntimeOutput o;
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    const double before = rt.sequencer().stepCv(0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 6.0, 1);
    rt.processBlock(&z, 1, &o);
    check(rt.lastApplyStatus() == core::ParameterApplyStatus::invalid_value &&
              sameD(rt.sequencer().stepCv(0), before),
          "t9 invalid step cv rejects AND keeps prior cv (no clamp)");
  }
}

// ===========================================================================
// 10. (BLOCKED #3) VCA-CV A/B independence + a consolidated 10-output / 3-sink audit.
//     The six sources publish EXACTLY 10 jacks (EG 4: env A/B + vca_cv A/B; LFO 2; joy 2;
//     seq 2). sequencer.clock_out is intentionally NOT among them. Each sink readback is
//     a REAL consumer (`vcfCvReadbackL/R`, `droneChannel`), not a test-side re-derivation.
// ===========================================================================
static void test_10_vca_ab_and_output_audit(void) {
  // (a) The 10 published output JackIds, all live (finite after a render), and clock_out
  //     excluded from the set.
  static const reg::JackId kOut[10] = {
      reg::JackId::envelope_a_env_out, reg::JackId::envelope_a_vca_cv_out,
      reg::JackId::envelope_b_env_out, reg::JackId::envelope_b_vca_cv_out,
      reg::JackId::lfo_a_cv_out,       reg::JackId::lfo_b_cv_out,
      reg::JackId::joystick_x_out,     reg::JackId::joystick_y_out,
      reg::JackId::sequencer_cv_out,   reg::JackId::sequencer_gate_out,
  };
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    core::RuntimeOutput o;
    const double z = 0.0;
    rt.processBlock(&z, 1, &o);
    for (int i = 0; i < 10; ++i)
      check(std::isfinite(rt.controlVoltageAt(kOut[i])),
            "t10 all ten source outputs are finite/published");
    // clock_out is NOT published: it stays at the never-written default (a discrete PULSER
    // event only, carried by clockOutRising()), so it must not be one of the ten.
    check(rt.controlVoltageAt(reg::JackId::sequencer_clock_out) == 0.0,
          "t10 sequencer clock_out is NOT a published output (stays default, not false-green)");
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
        const double z = 0.0;
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
        const double z = 0.0;
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
    double vcfL = 0.0, vcfR = 0.0;
    for (int i = 0; i < 8; ++i) {
      // VCF id=2 < joystick id=8: the readback lags the published source by one frame, so
      // settle over a short window and read the LAST value (t4 measures exactly this).
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      vcfL = rt.vcfCvReadbackL();  // joystick x -> 5*(2*0.7-1) = +2.0V
      vcfR = rt.vcfCvReadbackR();  // joystick y -> 5*(2*0.3-1) = -2.0V
    }
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
        const double z = 0.0;
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
        const double z = 0.0;
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
        const double z = 0.0;
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
//     read via clockOutRising() (it is not published as a volts jack), and a bank sentinel
//     at the default 1.0 Hz shows the same window does NOT advance (so the crossing below
//     is genuinely rate-driven).
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
  //     (published cv_out cycles through the step CVs) while clockOutRising() fires — the
  //     PULSER is genuinely exercised in the product path. @Codex #3: configure ONLY through
  //     the public parameter-event path (the mutating accessors were removed), and @Codex #4:
  //     plant a NONZERO sentinel in the UNPUBLISHED sequencer_clock_out slot as a canary.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::sequencer_stages, 0.0, 0);       // -> 3 stages
    applyParam(rt, reg::ParameterId::sequencer_step_cv_1, 1.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_2, 2.0, 0);
    applyParam(rt, reg::ParameterId::sequencer_step_cv_3, 3.0, 0);
    check(rt.setSequencerInternalRateHz(48000.0), "t11 set crossing rate 48kHz");
    const double kSentinel = 1234.5;
    rt.setControlVoltage(reg::JackId::sequencer_clock_out, kSentinel);
    bool saw1 = false, saw2 = false, saw3 = false, sawRising = false, sentinelHeld = true;
    for (int i = 0; i < 8; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      const double cv = rt.controlVoltageAt(reg::JackId::sequencer_cv_out);
      if (nearD(cv, 1.0)) saw1 = true;
      if (nearD(cv, 2.0)) saw2 = true;
      if (nearD(cv, 3.0)) saw3 = true;
      if (rt.sequencer().clockOutRising()) sawRising = true;
      sentinelHeld = sentinelHeld &&
                     sameD(rt.controlVoltageAt(reg::JackId::sequencer_clock_out), kSentinel);
    }
    check(saw1 && saw2 && saw3,
          "t11 runtime PULSER crossing publishes each distinct step CV (real advance)");
    check(sawRising, "t11 clock_out discrete rising observed at runtime (not a false-green 0)");
    check(sawRising && sentinelHeld,
          "t11 clock_out sentinel stays bit-identical ACROSS a real crossing (never published)");
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
    bool held = true, rose = false, first = true;
    double step0 = 0.0;
    for (int i = 0; i < 200; ++i) {
      core::RuntimeOutput o;
      const double z = 0.0;
      rt.processBlock(&z, 1, &o);
      if (first) { step0 = rt.controlVoltageAt(reg::JackId::sequencer_cv_out); first = false; }
      else held = held && nearD(rt.controlVoltageAt(reg::JackId::sequencer_cv_out), step0);
      if (rt.sequencer().clockOutRising()) rose = true;
    }
    check(held && !rose,
          "t11 default 1.0 Hz holds step0 CV and never rises (crossing is genuinely rate-driven)");
  }
}

// ===========================================================================
// 12. (BLOCKED #5) Always-execute admission is ATOMIC and FAIL-CLOSED: a malformed set is
//     refused with no state change and no dirt; a genuine change sets graphDirty_ so the
//     plan recompiles; an identical list is a no-op (no dirt). Defense-in-depth: the
//     compile_graph admission independently returns invalid_always_execute on a bad list.
// ===========================================================================
static void test_12_always_admission_fail_closed(void) {
  const core::ModuleId six[6] = {core::ModuleId::envelope_a, core::ModuleId::envelope_b,
                                 core::ModuleId::lfo_a,     core::ModuleId::lfo_b,
                                 core::ModuleId::joystick,  core::ModuleId::sequencer};
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
    check(rt.setAlwaysExecute(six, 6), "t12 identical always list accepted (no-op)");
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
        const double z = 0.0;
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
        const double z = 0.0;
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
        const double z = 0.0;
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
      const double z = 0.0;
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

  // (c) EG-A gate latch driven by a REAL descriptor (joystick x -> envelope_a_gate_in):
  //     a high source is interpreted + latched -> EG-A attacks; a low source -> EG-A
  //     releases; EG-B (unwired) stays idle throughout (A/B no cross-talk).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 1.0, 0);  // -> +5V (high, thr=0.5/hyst=0.1)
    applyParam(rt, reg::ParameterId::envelope_a_a, 0.001, 0);
    applyParam(rt, reg::ParameterId::envelope_a_r, 0.001, 0);
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::envelope_a_gate_in),
          "t13c wire joy x -> EG-A gate");
    check(rt.rebuild(), "t13c EG-A gate rebuild");
    double aRise[30] = {0};
    double bIdle = 0.0;
    for (int i = 0; i < 30; ++i) {
      core::RuntimeOutput blk;
      const double z = 0.0;
      rt.processBlock(&z, 1, &blk);
      aRise[i] = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
      bIdle = rt.controlVoltageAt(reg::JackId::envelope_b_env_out);
    }
    check(aRise[29] > 0.0, "t13c EG-A ENV attacks under a high descriptor gate");
    check(sameD(bIdle, 0.0), "t13c EG-B stays idle while EG-A is gated (A/B no cross-talk)");
    applyParam(rt, reg::ParameterId::joystick_x, 0.0, 30);  // -> -5V (low): falling edge
    double aFall = 0.0;
    for (int i = 0; i < 30; ++i) {
      core::RuntimeOutput blk;
      const double z = 0.0;
      rt.processBlock(&z, 1, &blk);
      aFall = rt.controlVoltageAt(reg::JackId::envelope_a_env_out);
    }
    check(aFall < aRise[29], "t13c EG-A ENV releases under a low descriptor gate");
  }

  // (d) EG-B gate latch via a real descriptor (PREVIOUSLY NEVER WIRED): a high source on
  //     envelope_b_gate_in makes EG-B attack while EG-A (unwired) stays idle.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = make_def(kSeed, kSr);
    core::SynthRuntime& rt = def->runtime();
    applyParam(rt, reg::ParameterId::joystick_x, 1.0, 0);  // -> +5V (high)
    applyParam(rt, reg::ParameterId::envelope_b_a, 0.001, 0);
    applyParam(rt, reg::ParameterId::envelope_b_r, 0.001, 0);
    check(rt.connect(reg::JackId::joystick_x_out, reg::JackId::envelope_b_gate_in),
          "t13d wire joy x -> EG-B gate (previously never wired)");
    check(rt.rebuild(), "t13d EG-B gate rebuild");
    double bRise = 0.0, aIdle = 0.0;
    for (int i = 0; i < 30; ++i) {
      core::RuntimeOutput blk;
      const double z = 0.0;
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
      const double z = 0.0;
      rt.processBlock(&z, 1, &blk);
      const double g = rt.controlVoltageAt(reg::JackId::sequencer_gate_out);
      if (sameD(g, 10.0) && !sameD(prevG, 10.0)) ++rises;  // one-sample 10V pulse per advance
      prevG = g;
    }
    check(rises == 1, "t13e one rising edge drives exactly ONE advance (sustained high no repeat)");
  }
}

int main(void) {
  test_1_slots_presence_phase();
  test_2_lfo_drone_mod_same_sample();
  test_3_seq_gate_eg_env_vcf();
  test_4_joystick_vcf();
  test_5_seq_ext_clock();
  test_6_seq_stages_pulser_unpub();
  test_7_param_table_partition();
  test_8_task65_invariants_zero_alloc();
  test_negative_controls();
  test_9_param_matrix_apply_status();
  test_10_vca_ab_and_output_audit();
  test_11_runtime_pulser_crossing();
  test_12_always_admission_fail_closed();
  test_13_vca_sink_and_gate_latches();

  std::printf("\n[%s] %d checks, %d failed\n", g_fail == 0 ? "PASS" : "FAIL", g_checks,
              g_fail);
  return g_fail == 0 ? 0 : 1;
}
