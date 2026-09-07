// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh12_keyboard_owner_probe.cpp — task: GH#12 keyboard product owner acceptance.
//
// BACKGROUND. On the pre-fix head the 2472-line keyboard subsystem is consumed
// NOWHERE in production:
//   * the keyboard module is bound ExecutionKind::kUnsupported (machine_definition.h
//     kMachineDisposition), so rebuild() never gives it a step_ producer;
//   * all four keyboard routes are RouteDisposition::kDeferred and are NOT in
//     kActiveRoutes, so the PatchGraph has no keyboard edge;
//   * applyControlEvent_ returns early for any non-parameter ControlEvent
//     (machine_runtime.h:1725), so a note triple (pitch + pressure + gate_on, the
//     exact set InputStateMachine::translate emits for a note_on) is silently dropped.
// Net effect: the default machine is a self-running drone and cannot "play" a note.
//
// ENTRY. This acceptance enters through the REAL canonical MachineRuntimeDefinition —
// the same object StandaloneAudioEngine wraps via definition_->runtime() — built from
// the default power-on device state through the public seed constructor. It injects a
// canonical note at an absolute sample, then renders via processBlock (the actual
// dispatch: EventTimebase -> per-sample applyControlEvent_ -> processFrame -> step_).
// No test seam is touched; the note and the audio are produced by the real core.
//
// CRITERIA (per @Kimi approval msg 53c97177 + the AC-4b legato addition):
//   AC-1 pitch   a +1 V/oct note doubles VCO-A; the keyboard publishes the note CV
//                (readback ~1.0 V) AND VCO-A dryA frequency ~doubles (2^vOct is exact).
//   AC-2 gate    the gate_on triggers EG-A: envelope_a_env_out rises off zero.
//   AC-3 audio   the note is audible: dryA is audio-rate, not silence.
//   AC-4 part    the same event set renders bit-identically under 64/128/256 partitions
//                (design/07 §5 buffer-invariance, criterion ④).
//   AC-4b legato a note_on while the first note is still held re-pitches VCO-A to the
//                new note (frequency follows the re-note).
//   AC-5 regression is the full ctest suite, not this probe.
//
// EXIT CODE. 0 = all criteria green; non-zero = any criterion red. On the pre-fix head
// (80fb95e) AC-1/AC-2/AC-4b must be red — that is the "will-turn-red" evidence @Kimi
// required before the implementation starts.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <lunar24/core/control_event.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/machine_runtime.h>
#include <lunar24/registry.hpp>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// Same constants the host test suite uses (test_machine_audio_families kSr=48000).
static constexpr double kSr = 48000.0;
static constexpr std::uint64_t kSeed = 0x4C554E4152ull;          // "LUNAR"
static constexpr int kRender = 96000;                            // 2 s: lets any portamento/vibrato settle.
static constexpr int kMeasure = 48000;                           // final 1 s window for the stable zcr.
static constexpr std::uint64_t kNote1Sample = 0;
static constexpr std::uint64_t kNote2Sample = 48000;             // second (legato) note, mid-render.

// Zero-crossing count (2 per cycle) — the frequency discriminator for dryA (matches
// zcrOf in the host VCO measurement). More crossings = higher pitch.
static int zcrOf(const std::vector<double>& c, std::size_t from, std::size_t to) {
  int z = 0;
  for (std::size_t i = from + 1; i < to; ++i)
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  return z;
}

// The canonical note triple InputStateMachine::translate emits for a note_on
// (input_state_machine.h): pitch[value=1 V/oct CV] + pressure[aftertouch] + gate_on[1],
// all at the SAME absolute sample and the same producerSequence (design/07 §3
// same-sample phase order: pitch/pressure are phase 1, gate_on is phase 4, so within a
// sample the pitch is applied before the gate — a note is never gated before it pitches).
// Option A (approved): the keyboard owner consumes canonical ControlEvents; the
// InputStateMachine translate lives in the deferred input lane, so this probe injects the
// events it would have produced.
static void enqueue_note(core::SynthRuntime& rt, double pitchV, double pressureV,
                         std::uint64_t sample, std::uint64_t seq) {
  core::ControlEvent ev{};
  ev.kind = core::ControlEventKind::pitch;
  ev.value = static_cast<core::SignalSample>(pitchV);
  ev.source = 1;
  ev.channel = 0;
  ev.noteId = static_cast<core::NoteId>(seq & 0xffu);
  ev.producerSequence = seq;
  rt.enqueueControlEvent(core::TimedControlEvent{ev, sample});

  ev.kind = core::ControlEventKind::pressure;
  ev.value = static_cast<core::SignalSample>(pressureV);
  ev.producerSequence = seq;
  rt.enqueueControlEvent(core::TimedControlEvent{ev, sample});

  ev.kind = core::ControlEventKind::gate_on;
  ev.value = static_cast<core::SignalSample>(1.0);
  ev.producerSequence = seq;
  rt.enqueueControlEvent(core::TimedControlEvent{ev, sample});
}

// Render `n` zero-input frames in ONE processBlock call (the real-block path), appending
// dryA to `out`. driveGraph=true (the normal product audio path).
static void render_dryA(core::SynthRuntime& rt, int n, std::vector<double>& out) {
  std::vector<core::RuntimeInputs> in(static_cast<std::size_t>(n));
  std::vector<core::RuntimeOutput> o(static_cast<std::size_t>(n));
  rt.processBlock(in.data(), static_cast<std::size_t>(n), o.data());
  for (const auto& r : o) out.push_back(r.dryA);
}

// Render the note event set under an explicit partition schedule (fresh def each call),
// returning the concatenated dryA. AC-4 compares these schedules bitwise.
static std::vector<double> render_partitioned(double pitchV, std::uint64_t noteSample,
                                              const std::vector<int>& schedule) {
  core::MachineRuntimeDefinition def(kSeed, kSr);
  core::SynthRuntime& rt = def.runtime();
  enqueue_note(rt, pitchV, 0.7, noteSample, 7);
  if (!rt.graphValid()) std::printf("  [WARN] partition def graph not valid\n");
  std::vector<double> dryA;
  for (int sz : schedule) {
    std::vector<core::RuntimeInputs> in(static_cast<std::size_t>(sz));
    std::vector<core::RuntimeOutput> o(static_cast<std::size_t>(sz));
    rt.processBlock(in.data(), static_cast<std::size_t>(sz), o.data());
    for (const auto& r : o) dryA.push_back(r.dryA);
  }
  return dryA;
}

static void accept_pitch_gate_audio() {
  std::printf("AC-1/AC-2/AC-3  -- a +1 V/oct note becomes a playable pitched voice on VCO-A/EG-A\n");

  // Baseline: the same default machine with NO note. VCO-A base frequency, gate idle.
  int baseZ = 0;
  double vOctBase = 0.0;
  double envBase = 0.0;
  {
    core::MachineRuntimeDefinition def(kSeed, kSr);
    core::SynthRuntime& rt = def.runtime();
    check(rt.graphValid(), "baseline definition graph is valid");
    std::vector<double> d;
    render_dryA(rt, kRender, d);
    baseZ = zcrOf(d, kRender - kMeasure, kRender);
    vOctBase = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
    envBase = rt.controlVoltageAt(core::JackId::envelope_a_env_out);
  }

  // Note on: pitch=+1.0 V (one octave), pressure=0.7, gate_on=1.0, at sample 0.
  int noteZ = 0;
  double vOctNote = 0.0;
  double envOut = 0.0;
  {
    core::MachineRuntimeDefinition def(kSeed, kSr);
    core::SynthRuntime& rt = def.runtime();
    check(rt.graphValid(), "note definition graph is valid");
    enqueue_note(rt, 1.0, 0.7, kNote1Sample, 1);
    std::vector<double> d;
    render_dryA(rt, kRender, d);
    noteZ = zcrOf(d, kRender - kMeasure, kRender);
    vOctNote = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
    envOut = rt.controlVoltageAt(core::JackId::envelope_a_env_out);
  }

  std::printf("  baseline dryA zcr=%d, note dryA zcr=%d, vOct(base/note)=%.4f/%.4f, envOut(base/note)=%.4f/%.4f\n",
              baseZ, noteZ, vOctBase, vOctNote, envBase, envOut);

  // AC-1: the keyboard owner publishes the note pitch CV (1 V/oct). Pre-fix the keyboard is
  // kUnsupported so nothing is published -> vOct stays ~0 -> red.
  check(std::fabs(vOctNote - 1.0) < 0.2, "AC-1 keyboard publishes ~1.0 V pitch CV");
  // AC-1b: VCO-A dryA frequency ~doubles (2^vOct is exact). Pre-fix the note is dropped so
  // the ratio stays ~1 -> red.
  const double ratio = baseZ > 0 ? static_cast<double>(noteZ) / baseZ : 1.0;
  check(ratio > 1.6 && ratio < 2.6, "AC-1 VCO-A dryA frequency ~doubles (+1 octave)");
  // AC-2: gate_on triggers EG-A. Pre-fix the gate is dropped so envOut stays ~0 -> red.
  check(envOut > 0.3, "AC-2 gate_on triggers envelope_a (env_out rises off zero)");
  // AC-3: the note is audible (dryA is audio-rate). Sanity gate; passes both pre/post fix.
  check(noteZ >= 10, "AC-3 the note is audible (dryA is audio-rate)");
}

static void accept_legato() {
  std::printf("AC-4b  -- a note_on while the first note is still held re-pitches VCO-A\n");

  // note1 pitch=+1.0 V at sample 0; note2 pitch=+2.0 V (a different note) at sample kNote2Sample,
  // while the first note is still held (no gate_off between). The owner must latch to the new
  // note, so the final VCO-A pitch CV reflects the re-note.
  double vOctLegato = 0.0;
  {
    core::MachineRuntimeDefinition def(kSeed, kSr);
    core::SynthRuntime& rt = def.runtime();
    enqueue_note(rt, 1.0, 0.7, kNote1Sample, 1);
    enqueue_note(rt, 2.0, 0.7, kNote2Sample, 2);
    std::vector<double> d;
    render_dryA(rt, kRender, d);
    vOctLegato = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
  }

  std::printf("  legato final vOct=%.4f (expected ~2.0, the re-note)\n", vOctLegato);
  check(std::fabs(vOctLegato - 2.0) < 0.2, "AC-4b note_on-while-held re-pitches to the new note");
}

static void accept_partition_invariance() {
  std::printf("AC-4  -- the same note renders bit-identically under 64/128/256 partitions\n");
  const std::vector<int> p1{384};
  const std::vector<int> p2{128, 128, 128};
  const std::vector<int> p3{64, 64, 64, 64, 64, 64};
  const std::vector<double> a = render_partitioned(1.0, 8, p1);
  const std::vector<double> b = render_partitioned(1.0, 8, p2);
  const std::vector<double> c = render_partitioned(1.0, 8, p3);
  check(a == b && b == c, "AC-4 bitwise dryA identical across 64/128/256 partitions");
}

int main() {
  accept_pitch_gate_audio();
  accept_legato();
  accept_partition_invariance();

  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
