// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh12_keyboard_side_restore_probe.cpp — GH#12 task#101 keyboard SIDE-RESTORE acceptance.
//
// WHAT THIS PINS. task#100 found that the 35-item keyboard state is stored but has no
// true two-sided consumer: the runtime held ONE KeyboardBehaviour / ONE ArpSeq, so a
// Split device could not restore two independent performances, and a note carried no
// side at all. task#101 adds an explicit internal KeyboardSide to the performance
// pipeline and instantiates one behaviour + one arp/seq per side. This probe is the
// acceptance for that slice, entered through the REAL canonical chain:
//
//   make_default_device_state / hand-built DeviceStateV1
//     -> encode_device_state -> decode_device_state
//     -> buildMachineRuntimeCandidate (validate_device_state + graph + identity + DSP)
//     -> definition.runtime().enqueueControlEvent / processBlock
//
// and, for the families that must hold on the owner the product actually renders (Q/R/S/U),
// the SAME states and event scripts re-entered through the real host entry:
//
//   EngineHarness -> StandaloneAudioEngine::applyDeviceState -> processBlock
//     -> DeviceAdapter::renderBlock -> SynthRuntime (the ONE EventTimebase drain, F-1)
//
// No test seam, no private access: the same public objects the audio engine wraps.
//
// CRITERIA (contract @Codex msg 57a5ab2a §5, rulings msg 9943ae28):
//   A  Single compatibility + right-collapse: a default (Single) device plays a note on
//      the existing v_oct/main-gate path and publishes the REAL pressure; an unused right
//      gate stays an explicit low rail; a right-side event under Single merges onto Left
//      BEFORE identity handling (a left release of the same identity releases it).
//   B  Twin: both sides read bank 0 (shared CONFIG) while each keeps its own held notes
//      (independent performance) — a bank-1-only portamento value must NOT reach the right
//      side, and releasing one side must not release the other.
//   C  Split: asymmetric bank configuration is restored per side (left reads bank 0,
//      right reads bank 1) and is behaviourally discriminating, not just parsed.
//   D  Same (source, channel, noteId) on both sides = two notes: neither release may
//      release the other.
//   E  pressure_out per mode: Single = the real pressure; Twin/Split = the RIGHT pitch;
//      the right pitch and the pressure are never summed into one jack.
//   F  Explicit external clock reaches the EXISTING arp/seq paths (arp family + seq
//      family discriminators) and NO internal BPM clock exists (a held chord with no
//      clock edge produces no note).
//   G  reset clears BOTH sides even when the reset event's side is the default Left.
//   H  The 19 runtime-effect parameters are read back per item and each behaviour family
//      has a discriminating output; the decoded-but-unconsumed items (108/115/127) are
//      parsed ONLY and are proven to change no output.
//   I  Default prepare == default restore (same state, same outputs); the one documented
//      difference (parsed clock_bpm) has no consumer and changes no sample.
//   J  64/256/irregular partitions produce bit-identical dryA and identical final
//      keyboard outputs; the same state restored twice is bit-identical.
//   K  An invalid state is rejected by the candidate chain and yields NO runtime.
//   Q  The REAL host single-commit entry (EngineHarness: encode -> decode ->
//      StandaloneAudioEngine::applyDeviceState -> processBlock) accepts every mode, is
//      atomic against an invalid/illegal-format commit on a LIVE owner (typed rejection,
//      state/format/plan/trace preserved), and a legal commit really replaces them.
//   R  The two new right-side outputs drive real consumers over LEGAL user cables
//      (pressure_out -> VCO-B pitch; gate_right_out -> EG-B), with override + bit-identical
//      removal — measured on the REAL host render entry, never on a published value.
//   S  Mid-block key events land at their exact absolute frame: real 256-frame HOST blocks
//      equal a per-frame reference on all four audio channels and at every block boundary.
//   U  F-1: the REAL host render entry (StandaloneAudioEngine::processBlock ->
//      DeviceAdapter::renderBlock) drains the EventTimebase, so a queued keyboard/MIDI event
//      acts at its exact ABSOLUTE frame on the product path; the host entry equals the
//      canonical per-frame entry; 64/256/irregular HOST blocks agree; a re-commit resets the
//      time base; and the per-frame drain preserves the defined EventTimebase semantics
//      (same-frame order, future retention, late delivery, capacity boundaries).
//
// EXIT CODE. 0 = all criteria green; non-zero = any criterion red.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <lunar24/core/device_adapter.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/keyboard_behaviour.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/core/machine_candidate.h>
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/machine_runtime.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_serializer.h>
#include <lunar24/registry_ids.hpp>

// The ONE shared real host entry (tests/host/): encode -> decode -> StandaloneAudioEngine
// ::applyDeviceState -> processBlock, capturing the four real output channels. task#101 review
// (group 1) requires the mode/restore/failure checks to be re-entered HERE, not only at
// buildMachineRuntimeCandidate, so the atomic-apply contract is pinned on the owner that is
// actually rendering.
#include "test_engine_harness.h"

namespace core = lunar24::core;
namespace testengine = lunar24::testengine;

static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static constexpr double kSr = 48000.0;
static constexpr std::uint64_t kSeed = 0x4C554E4152ull;  // "LUNAR"
static constexpr double kGateHigh = 10.0;                // the registered 0..10V gate rail.

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- state helpers -----------------------------------------------------------

// bank 0 (left/shared) = parameters[ParameterId]; bank 1 (right) = keyboardScalarRight.
static void set_left(core::DeviceStateV1& st, core::ParameterId id, double v) {
  st.parameters[static_cast<std::size_t>(id)] = v;
}
static void set_right(core::DeviceStateV1& st, core::ParameterId id, double v) {
  const std::int32_t i = core::keyboard_scalar_index(id);
  if (i >= 0) st.keyboardScalarRight[static_cast<std::size_t>(i)] = v;
}

// keyboardSettings.pressureBehaviour is the canonical single/twin/split selector and
// validate_device_state requires it to MIRROR parameters[keyboard_behaviour] (9002).
// Likewise keyboardSettings.pressureOutput mirrors parameters[keyboard_pressure_output]
// (9003). These helpers set both sides of the coherence rule so a hand-built state is
// a legal state — the probe must never bypass the real validator.
static void set_mode(core::DeviceStateV1& st, std::uint8_t mode) {
  st.keyboardSettings.pressureBehaviour = mode;
  st.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_behaviour)] =
      static_cast<double>(mode);
}
static void set_pressure_out(core::DeviceStateV1& st, std::uint8_t v) {
  st.keyboardSettings.pressureOutput = v;
  st.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_pressure_output)] =
      static_cast<double>(v);
}

// The full canonical chain. Returns the accepted definition or null (and prints why).
static std::unique_ptr<core::MachineRuntimeDefinition> chain(const core::DeviceStateV1& in) {
  std::vector<std::uint8_t> buf(core::kDeviceStorageSchema.totalBytesHint);
  std::size_t written = 0;
  if (!core::encode_device_state(in, buf.data(), buf.size(), &written)) {
    std::printf("  [WARN] encode failed\n");
    return nullptr;
  }
  core::DeviceStateV1 decoded;
  if (!core::decode_device_state(buf.data(), written, &decoded)) {
    std::printf("  [WARN] decode failed\n");
    return nullptr;
  }
  core::MachineCandidateResult r = core::buildMachineRuntimeCandidate(decoded, kSr);
  if (r.status != core::MachineCandidateStatus::accepted) {
    std::printf("  [WARN] candidate rejected status=%d\n", static_cast<int>(r.status));
    return nullptr;
  }
  return std::move(r.definition);
}

// ---- event helpers -----------------------------------------------------------

static void push(core::SynthRuntime& rt, core::ControlEventKind kind, double value,
                 core::KeyboardSide side, core::NoteId id, std::uint64_t sample) {
  core::ControlEvent e{};
  e.kind = kind;
  e.value = static_cast<core::SignalSample>(value);
  e.source = 1;          // one stable producer
  e.channel = 0;         // and one stable channel: identity is (source, channel, noteId)
  e.noteId = id;
  e.producerSequence = sample;
  e.side = side;
  rt.enqueueControlEvent(core::TimedControlEvent{e, sample});
}
// The canonical note triple translate() emits: pitch (phase 1), pressure (phase 1), gate_on.
static void note(core::SynthRuntime& rt, core::KeyboardSide side, double pitch,
                 double pressure, core::NoteId id, std::uint64_t sample) {
  push(rt, core::ControlEventKind::pitch, pitch, side, id, sample);
  push(rt, core::ControlEventKind::pressure, pressure, side, id, sample);
  push(rt, core::ControlEventKind::gate_on, 1.0, side, id, sample);
}
static void release(core::SynthRuntime& rt, core::KeyboardSide side, core::NoteId id,
                    std::uint64_t sample) {
  push(rt, core::ControlEventKind::gate_off, 0.0, side, id, sample);
}
static void clock_edge(core::SynthRuntime& rt, std::uint64_t sample) {
  push(rt, core::ControlEventKind::clock, 1.0, core::KeyboardSide::Left, 0, sample);
}
static void reset_edge(core::SynthRuntime& rt, core::KeyboardSide side, std::uint64_t sample) {
  push(rt, core::ControlEventKind::reset, 0.0, side, 0, sample);
}

// The REAL producer path: a normalized PerformanceInput goes through
// InputStateMachine::translate() — the one place a performance message becomes
// canonical ControlEvents — and the emitted events are enqueued unmodified. This is
// the seam the side metadata actually has to survive, so criterion P enters here.
static core::PerformanceInput perf(core::PerfInputKind kind, double pitch, double value,
                                   core::KeyboardSide side, core::NoteId id,
                                   std::uint64_t sample) {
  core::PerformanceInput in{};
  in.kind = kind;
  in.sample = sample;
  in.pitch = static_cast<core::SignalSample>(pitch);
  in.value = static_cast<core::SignalSample>(value);
  in.channel = 0;
  in.source = 1;
  in.noteId = id;
  in.seq = sample;
  in.side = side;
  return in;
}
static void enqueue_translated(core::SynthRuntime& rt, const core::PerformanceInput& in) {
  static const core::InputStateMachine kSm(nullptr, 0u);  // no CC bindings
  core::ControlEvent evs[3];
  const std::uint32_t n = kSm.translate(in, evs, 3u);
  for (std::uint32_t i = 0; i < n; ++i)
    rt.enqueueControlEvent(core::TimedControlEvent{evs[i], in.sample});
}

// ---- render helpers ----------------------------------------------------------

struct Snap {
  double vOct = 0.0, gateL = 0.0, gateR = 0.0, press = 0.0;
};

static Snap snap(core::SynthRuntime& rt) {
  Snap s;
  s.vOct = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
  s.gateL = rt.controlVoltageAt(core::JackId::keyboard_gate_left_main_out);
  s.gateR = rt.controlVoltageAt(core::JackId::keyboard_gate_right_out);
  s.press = rt.controlVoltageAt(core::JackId::keyboard_pressure_out);
  return s;
}

static void render(core::SynthRuntime& rt, int n, std::vector<double>* dryA = nullptr) {
  std::vector<core::RuntimeInputs> in(static_cast<std::size_t>(n));
  std::vector<core::RuntimeOutput> o(static_cast<std::size_t>(n));
  rt.processBlock(in.data(), static_cast<std::size_t>(n), o.data());
  if (dryA != nullptr)
    for (const auto& r : o) dryA->push_back(r.dryA);
}

// One frame at a time: a per-frame trace of the published pitch (v_oct).
static std::vector<double> trace_pitch(core::SynthRuntime& rt, int n) {
  std::vector<double> t;
  t.reserve(static_cast<std::size_t>(n));
  std::vector<core::RuntimeInputs> in(1);
  std::vector<core::RuntimeOutput> o(1);
  for (int i = 0; i < n; ++i) {
    rt.processBlock(in.data(), 1, o.data());
    t.push_back(rt.controlVoltageAt(core::JackId::keyboard_v_oct_out));
  }
  return t;
}
static double range_of(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double lo = v[0], hi = v[0];
  for (double x : v) { lo = x < lo ? x : lo; hi = x > hi ? x : hi; }
  return hi - lo;
}
// Zero crossings of (x - reference): the frequency discriminator for a modulated pitch.
static int crossings_of(const std::vector<double>& v, double reference) {
  int z = 0;
  for (std::size_t i = 1; i < v.size(); ++i)
    if (((v[i - 1] - reference) < 0.0) != ((v[i] - reference) < 0.0)) ++z;
  return z;
}

// ---- host-entry / cable helpers (task#101 review groups 1 and 3) --------------

// A LEGAL user cable: presence bit + source, and the routeOverridden coherence every landed
// route whose sink is `sink` must carry (validate_device_state::check_routes). Nothing here
// bypasses the validator — every state built with this helper goes through the real chain.
static void set_cable(core::DeviceStateV1& st, core::JackId src, core::JackId sink) {
  const std::size_t s = static_cast<std::size_t>(sink);
  st.inputCable[s] = 1;
  st.cableSource[s] = src;
  for (const auto& r : lunar24::registry::kNormalizedRoutes) {
    if (r.sinkJack == sink) st.routeOverridden[static_cast<std::size_t>(r.id)] = 1;
  }
}

// Structural equality of the committed device plan (the "state/format/plan" a rejected apply
// must leave untouched).
static bool same_plan(const core::DevicePlan& a, const core::DevicePlan& b) {
  return a.layout.kind == b.layout.kind && a.layout.totalChannels == b.layout.totalChannels &&
         a.outputCapability == b.outputCapability && a.outputCount == b.outputCount &&
         a.output.channel[0] == b.output.channel[0] &&
         a.output.channel[1] == b.output.channel[1] &&
         a.output.channel[2] == b.output.channel[2] &&
         a.output.channel[3] == b.output.channel[3] && a.inputCapability == b.inputCapability &&
         a.input == b.input && a.inputCh[0] == b.inputCh[0] && a.inputCh[1] == b.inputCh[1];
}

// The canonical storage bytes of a state (the wire a round-trip must reproduce).
static std::vector<std::uint8_t> wire_of(const core::DeviceStateV1& st) {
  std::vector<std::uint8_t> buf(core::kDeviceStorageSchema.totalBytesHint, 0);
  std::size_t written = 0;
  if (!core::encode_device_state(st, buf.data(), buf.size(), &written)) return {};
  buf.resize(written);
  return buf;
}

static bool same_snap(const Snap& a, const Snap& b) {
  return a.vOct == b.vOct && a.gateL == b.gateL && a.gateR == b.gateR && a.press == b.press;
}

// The same four published jacks, read from a CONST runtime (the host harness exposes only a
// const runtime()). Same values, no mutation.
static Snap read_snap(const core::SynthRuntime& rt) {
  Snap s;
  s.vOct = rt.controlVoltageAt(core::JackId::keyboard_v_oct_out);
  s.gateL = rt.controlVoltageAt(core::JackId::keyboard_gate_left_main_out);
  s.gateR = rt.controlVoltageAt(core::JackId::keyboard_gate_right_out);
  s.press = rt.controlVoltageAt(core::JackId::keyboard_pressure_out);
  return s;
}

// A raw absolute-timeline event for the standalone EventTimebase checks (U5).
static core::TimedControlEvent tev(core::ControlEventKind kind, double value, std::uint64_t sample,
                                   std::uint32_t source = 1, std::uint64_t seq = 0,
                                   core::ParameterId pid = core::ParameterId{0}) {
  core::ControlEvent e{};
  e.kind = kind;
  e.value = static_cast<core::SignalSample>(value);
  e.parameter = pid;
  e.source = source;
  e.producerSequence = seq;
  core::TimedControlEvent t;
  t.event = e;
  t.sample = sample;
  return t;
}

// ============================================================ A. Single + collapse

static void accept_single_compat() {
  std::printf("A  -- Single (default) note path + right-collapse\n");

  // An unplayed default device publishes nothing on any of the four keyboard outputs.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(core::make_default_device_state(kSeed));
    check(def != nullptr, "A0 default state is accepted by the canonical chain");
    if (def == nullptr) return;
    core::SynthRuntime& rt = def->runtime();
    check(rt.keyboardMode() == core::KeyboardMode::Single, "A0 default mode decodes to Single");
    render(rt, 4096);
    const Snap s = snap(rt);
    check(s.vOct == 0.0 && s.gateL == 0.0 && s.gateR == 0.0 && s.press == 0.0,
          "A0 an unplayed keyboard publishes no signal (all four outputs at zero)");
  }

  // A left note under Single: existing v_oct / main-gate path + the REAL pressure.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(core::make_default_device_state(kSeed));
    if (def == nullptr) { check(false, "A1 default build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    std::vector<double> dryA;
    render(rt, 48000, &dryA);
    const Snap s = snap(rt);
    check(near(s.vOct, 1.0, 0.05), "A1 Single: v_oct publishes the note pitch (+1.0 V)");
    check(s.gateL == kGateHigh, "A1 Single: main gate is the 0/10V rail when held");
    check(s.gateR == 0.0, "A1 Single: the unused right gate is an EXPLICIT low rail");
    check(near(s.press, 0.6, 0.02), "A1 Single: pressure_out is the REAL pressure behaviour output");
    check(range_of(dryA) > 0.01, "A1 Single: the note is audible (VCO-A dry out moves)");
  }

  // A RIGHT-side event under Single is the SAME performer: it merges onto Left before
  // identity handling, so a left release of the same identity releases it.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(core::make_default_device_state(kSeed));
    if (def == nullptr) { check(false, "A2 default build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Right, 1.0, 0.6, 7, 0);
    render(rt, 4800);
    Snap s = snap(rt);
    check(near(s.vOct, 1.0, 0.05) && s.gateL == kGateHigh,
          "A2 Single: a right-plate note collapses onto the Left performance identity");
    release(rt, core::KeyboardSide::Left, 7, 4800);  // LEFT release of the same identity
    render(rt, 4800);
    s = snap(rt);
    check(s.gateL == 0.0, "A2 Single: a left release releases the collapsed note (one identity)");
  }
}

// ============================================================ P. producer path

static void accept_translate_side_path() {
  std::printf("P  -- the REAL producer path: PerformanceInput -> translate() -> runtime\n");
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  set_mode(st, 2);  // Split: the two sides are independent performances
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
  if (def == nullptr) { check(false, "P build"); return; }
  core::SynthRuntime& rt = def->runtime();
  enqueue_translated(rt, perf(core::PerfInputKind::note_on, 1.0, 0.5,
                              core::KeyboardSide::Left, 1, 0));
  enqueue_translated(rt, perf(core::PerfInputKind::note_on, 2.0, 0.5,
                              core::KeyboardSide::Right, 2, 0));
  render(rt, 4800);
  Snap s = snap(rt);
  check(s.gateL == kGateHigh && s.gateR == kGateHigh && near(s.vOct, 1.0, 1e-6) &&
            near(s.press, 2.0, 1e-6),
        "P1 translate(): a left note and a right note are two independent performances");
  enqueue_translated(rt, perf(core::PerfInputKind::note_off, 0.0, 0.0,
                              core::KeyboardSide::Left, 1, 4800));
  render(rt, 4800);
  s = snap(rt);
  check(s.gateL == 0.0 && s.gateR == kGateHigh,
        "P2 translate(): the left note-off releases only the left performance");
}

// ============================================================ B. Twin

static void accept_twin() {
  std::printf("B  -- Twin: shared bank-0 CONFIG, independent per-side PERFORMANCE\n");

  // Shared configuration: bank 1 carries a DIFFERENT portamento speed; Twin must ignore
  // it (both sides read bank 0). The behavioural check is the right side's glide.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 1);  // Twin
    set_left(st, core::ParameterId::keyboard_portamento_speed, 1.0);   // bank 0: slow
    set_right(st, core::ParameterId::keyboard_portamento_speed, 0.0);  // bank 1: instant
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "B1 twin build"); return; }
    core::SynthRuntime& rt = def->runtime();
    check(rt.keyboardMode() == core::KeyboardMode::Twin, "B1 Twin mode decodes");
    check(near(rt.keyboardBehaviourParams(core::KeyboardSide::Left).portamentoSpeed, 1.0, 1e-9) &&
              near(rt.keyboardBehaviourParams(core::KeyboardSide::Right).portamentoSpeed, 1.0, 1e-9),
          "B1 Twin: BOTH sides install bank-0 config (bank 1 is not read)");

    note(rt, core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.0, 2, 0);
    render(rt, 12000);  // 0.25 s: tau = 2.5 s -> the pitch has only just started to move
    const Snap s = snap(rt);
    check(s.gateL == kGateHigh && s.gateR == kGateHigh,
          "B1 Twin: two plates are two independently gated notes");
    check(s.vOct < 0.3 && s.press < 0.6,
          "B1 Twin: BOTH sides glide with the shared bank-0 speed (right is not instant)");
    // Independent performance: releasing the left note must not release the right.
    release(rt, core::KeyboardSide::Left, 1, 12000);
    render(rt, 12000);
    const Snap s2 = snap(rt);
    check(s2.gateL == 0.0 && s2.gateR == kGateHigh,
          "B1 Twin: releasing the left note leaves the right note held");
  }

  // Independent performance with instant portamento: pressure_out is the right pitch.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 1);  // Twin
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "B2 twin build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.6, 2, 0);
    render(rt, 4800);
    const Snap s = snap(rt);
    check(near(s.vOct, 1.0, 0.02), "B2 Twin: v_oct carries the LEFT pitch");
    check(near(s.press, 2.0, 0.02),
          "B2 Twin: pressure_out carries the RIGHT pitch (manual BEHAVIOUR)");
  }
}

// ============================================================ C. Split

static void accept_split() {
  std::printf("C  -- Split: independent per-side bank (asymmetric configuration)\n");

  // The scale editor is a NON-scalar side path: left bank chromatic (quantises), right
  // bank microtonal (pass-through). A 0.04 V note therefore quantises to 0.0 on the left
  // and survives at 0.04 on the right — a discriminator that cannot pass if the right
  // side read the left bank.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split
    st.keyboardScaleEditor = 0x0FFFu;           // left bank: all 12 semitones (chromatic)
    st.keyboardScaleEditorR = 0x0000u;          // right bank: microtonal passthrough
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "C1 split build"); return; }
    core::SynthRuntime& rt = def->runtime();
    check(rt.keyboardMode() == core::KeyboardMode::Split, "C1 Split mode decodes");
    check(rt.keyboardBehaviourParams(core::KeyboardSide::Left).scaleEditor == 0x0FFFu &&
              rt.keyboardBehaviourParams(core::KeyboardSide::Right).scaleEditor == 0x0000u,
          "C1 Split: each side installs ITS OWN bank's scale editor");
    note(rt, core::KeyboardSide::Left, 0.04, 0.5, 1, 0);
    note(rt, core::KeyboardSide::Right, 0.04, 0.5, 2, 0);
    render(rt, 4800);
    const Snap s = snap(rt);
    check(near(s.vOct, 0.0, 1e-9),
          "C1 Split: LEFT pitch is quantised by the left bank (0.04 V -> 0.0 V)");
    check(near(s.press, 0.04, 1e-9),
          "C1 Split: RIGHT pitch is the right bank's microtonal passthrough (not left's)");
  }

  // Asymmetric portamento: bank 0 slow (2.5 s), bank 1 instant.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split
    set_left(st, core::ParameterId::keyboard_portamento_speed, 1.0);
    set_right(st, core::ParameterId::keyboard_portamento_speed, 0.0);
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "C2 split build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    note(rt, core::KeyboardSide::Right, 1.0, 0.0, 2, 0);
    render(rt, 12000);
    const Snap s = snap(rt);
    check(s.vOct < 0.3, "C2 Split: the LEFT pitch glides with the left bank's slow speed");
    check(near(s.press, 1.0, 1e-6),
          "C2 Split: the RIGHT pitch is instant with the right bank's zero speed");
  }
}

// ============================================================ D. same identity

static void accept_same_identity() {
  std::printf("D  -- the SAME (source, channel, noteId) on both sides is two notes\n");
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  set_mode(st, 2);  // Split
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
  if (def == nullptr) { check(false, "D build"); return; }
  core::SynthRuntime& rt = def->runtime();
  note(rt, core::KeyboardSide::Left, 1.0, 0.0, 9, 0);
  note(rt, core::KeyboardSide::Right, 2.0, 0.0, 9, 0);  // SAME identity, other side
  render(rt, 4800);
  Snap s = snap(rt);
  check(s.gateL == kGateHigh && s.gateR == kGateHigh,
        "D1 the same identity on both sides latches TWO notes");
  release(rt, core::KeyboardSide::Left, 9, 4800);
  render(rt, 4800);
  s = snap(rt);
  check(s.gateL == 0.0 && s.gateR == kGateHigh,
        "D2 releasing the left identity does NOT release the right note");
  release(rt, core::KeyboardSide::Right, 9, 9600);
  render(rt, 4800);
  s = snap(rt);
  check(s.gateR == 0.0, "D3 releasing the right identity closes the right note");
}

// ============================================================ E. pressure jack

static void accept_pressure_jack() {
  std::printf("E  -- pressure_out is mode-correct and never sums right pitch + pressure\n");

  // Single: the real pressure (already asserted in A1) and NOT the right pitch.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def =
        chain(core::make_default_device_state(kSeed));
    if (def == nullptr) { check(false, "E1 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.25, 1, 0);
    render(rt, 4800);
    const Snap s = snap(rt);
    check(near(s.press, 0.25, 0.01) && !near(s.press, s.vOct, 0.1),
          "E1 Single: pressure_out is the pressure (0.25 V), not the pitch");
  }

  // Split: right pitch on the pressure jack; the left pressure (0.9) must NOT be added.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "E2 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.9, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.9, 2, 0);
    render(rt, 4800);
    const Snap s = snap(rt);
    check(near(s.press, 2.0, 0.02),
          "E2 Split: pressure_out is the right pitch (2.0 V), not pitch + pressure");
    check(!near(s.press, 2.9, 0.05), "E2 Split: the right pitch and pressure are never summed");
  }
}

// ============================================================ G. reset

static void accept_reset() {
  std::printf("G  -- a reset clears BOTH sides (default-Left must not leak)\n");
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  set_mode(st, 2);  // Split
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
  if (def == nullptr) { check(false, "G build"); return; }
  core::SynthRuntime& rt = def->runtime();
  note(rt, core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
  note(rt, core::KeyboardSide::Right, 2.0, 0.5, 2, 0);
  render(rt, 4800);
  Snap s = snap(rt);
  check(s.gateL == kGateHigh && s.gateR == kGateHigh, "G1 both sides held before the reset");
  reset_edge(rt, core::KeyboardSide::Left, 4800);  // default-Left reset event
  render(rt, 480);
  s = snap(rt);
  check(s.gateL == 0.0 && s.gateR == 0.0, "G2 reset clears BOTH sides (no stuck right note)");
}

// ============================================================ F. explicit clock

static void accept_explicit_clock() {
  std::printf("F  -- the EXISTING arp/seq paths are reachable from an explicit clock\n");

  // F1: Arpeggiator, forward, interval 0 (1 semitone), no variation.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 1.0);  // Arpeggiator
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F1 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    check(rt.keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Arpeggiator,
          "F1 keyboard.mode restores the arp/seq mux (Arpeggiator)");
    note(rt, core::KeyboardSide::Left, 0.0, 0.0, 1, 0);
    note(rt, core::KeyboardSide::Left, 1.0, 0.0, 2, 0);  // 2-note chord
    render(rt, 1000);
    check(snap(rt).gateL == 0.0,
          "F1 NO internal BPM clock: a held chord with no clock edge produces no note");
    clock_edge(rt, 1500);
    render(rt, 1000);
    double p1 = snap(rt).vOct;
    check(near(p1, 1.0 / 12.0, 0.01) && snap(rt).gateL == kGateHigh,
          "F1 explicit clock edge 1 arpeggiates the first chord note (+1 semitone)");
    clock_edge(rt, 2500);
    render(rt, 1000);
    const double p2 = snap(rt).vOct;
    check(near(p2, 1.0 + 1.0 / 12.0, 0.01),
          "F1 explicit clock edge 2 arpeggiates the second chord note");
    clock_edge(rt, 3500);
    render(rt, 1000);
    check(near(snap(rt).vOct, p1, 0.01), "F1 explicit clock edge 3 wraps forward to the first note");
  }

  // F2: arp direction = backward (first clock takes the LAST chord member).
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 1.0);              // Arpeggiator
    set_left(st, core::ParameterId::keyboard_arp_direction, 1.0);     // backward
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F2 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 0.0, 0.0, 1, 0);
    note(rt, core::KeyboardSide::Left, 1.0, 0.0, 2, 0);
    clock_edge(rt, 500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 1.0 + 1.0 / 12.0, 0.01),
          "F2 arp_direction=backward takes the last chord member first");
  }

  // F3: arp variation = +1 octave.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 1.0);           // Arpeggiator
    set_left(st, core::ParameterId::keyboard_arp_variation, 1.0);  // x1 octave
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F3 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 0.0, 0.0, 1, 0);
    clock_edge(rt, 500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 1.0 + 1.0 / 12.0, 0.01),
          "F3 arp_variation=+1 octave transposes the arp note");
  }

  // F4: arp hold keeps the chord after the plates are released.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 1.0);   // Arpeggiator
    set_left(st, core::ParameterId::keyboard_arp_hold, 1.0);
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F4 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 0.0, 0.0, 1, 0);
    release(rt, core::KeyboardSide::Left, 1, 200);
    clock_edge(rt, 500);
    render(rt, 1000);
    check(snap(rt).gateL == kGateHigh, "F4 arp_hold=on keeps the chord alive after release");
  }

  // F5: keyboard.mode = Keyboard is the transparent pass-through (no arp stepping).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def =
        chain(core::make_default_device_state(kSeed));
    if (def == nullptr) { check(false, "F5 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 0.5, 0.0, 1, 0);
    clock_edge(rt, 500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 0.5, 1e-6),
          "F5 mode=Keyboard passes the plate pitch through unchanged");
  }

  // F6: Sequencer — free run, forward, 2-step length, all gates on.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 2.0);        // Sequencer
    set_left(st, core::ParameterId::keyboard_seq_length, 0.0);  // -> 2 steps
    for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
      st.keyboardSeqCurrent.steps[i].note = static_cast<std::uint8_t>(i);
      st.keyboardSeqCurrent.steps[i].value = 0.0f;
      st.keyboardSeqCurrent.steps[i].gate = 1;
    }
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F6 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    clock_edge(rt, 500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 0.0, 1e-6) && snap(rt).gateL == kGateHigh,
          "F6 sequencer free-run steps to step 0 on the first explicit clock");
    clock_edge(rt, 1500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 1.0 / 12.0, 1e-6), "F6 sequencer steps forward to step 1");
    clock_edge(rt, 2500);
    render(rt, 1000);
    check(near(snap(rt).vOct, 0.0, 1e-6), "F6 seq_length=2 wraps back to step 0");
  }

  // F7: seq_cv_output = gated suppresses a step whose gate is off.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 2.0);             // Sequencer
    set_left(st, core::ParameterId::keyboard_seq_cv_output, 1.0);    // gated
    for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
      st.keyboardSeqCurrent.steps[i].note = static_cast<std::uint8_t>(i);
      st.keyboardSeqCurrent.steps[i].gate = 0;  // every step gate OFF
    }
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F7 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    clock_edge(rt, 500);
    render(rt, 1000);
    check(snap(rt).gateL == 0.0, "F7 seq_cv_output=gated suppresses steps with gate off");
  }

  // F8: seq_run = keyboard requires a held plate (no plate -> no step).
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_mode, 2.0);   // Sequencer
    set_left(st, core::ParameterId::keyboard_seq_run, 1.0);  // keyboard-gated
    for (std::uint32_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
      st.keyboardSeqCurrent.steps[i].note = static_cast<std::uint8_t>(i);
      st.keyboardSeqCurrent.steps[i].gate = 1;
    }
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "F8 build"); return; }
    core::SynthRuntime& rt = def->runtime();
    clock_edge(rt, 500);
    render(rt, 1000);
    check(snap(rt).gateL == 0.0, "F8 seq_run=keyboard emits nothing with no plate held");
  }
}

// ============================================================ H. 19-item readback

static void accept_readback_19() {
  std::printf("H  -- the 19 runtime-effect parameters are read back per item\n");
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  // Every value below is non-default, inside its registry range, and distinct enough to
  // catch a swapped/mis-mapped field.
  set_left(st, core::ParameterId::keyboard_mode, 2.0);
  set_left(st, core::ParameterId::keyboard_arp_hold, 1.0);
  set_left(st, core::ParameterId::keyboard_arp_direction, 3.0);
  set_left(st, core::ParameterId::keyboard_arp_variation, 2.0);
  set_left(st, core::ParameterId::keyboard_arp_interval, 0.5);
  set_left(st, core::ParameterId::keyboard_seq_run, 1.0);
  set_left(st, core::ParameterId::keyboard_seq_length, 0.5);
  set_left(st, core::ParameterId::keyboard_seq_direction, 2.0);
  set_left(st, core::ParameterId::keyboard_seq_cv_output, 1.0);
  set_left(st, core::ParameterId::keyboard_portamento_speed, 0.5);
  set_left(st, core::ParameterId::keyboard_portamento_legato, 1.0);
  set_left(st, core::ParameterId::keyboard_vibrato_speed, 0.25);
  set_left(st, core::ParameterId::keyboard_vibrato_depth, 0.5);
  set_left(st, core::ParameterId::keyboard_vibrato_delay, 0.75);
  set_left(st, core::ParameterId::keyboard_vibrato_pressure, 1.0);
  set_pressure_out(st, 3);  // 123: sets the canonical selector + its parameters[] mirror
  set_left(st, core::ParameterId::keyboard_pressure_rise, 0.5);
  set_left(st, core::ParameterId::keyboard_pressure_fall, 0.25);
  set_left(st, core::ParameterId::keyboard_root_note, 0.5);
  st.keyboardScaleEditor = core::kScaleIonian;  // a RESOLVED non-scalar mask (must install)
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
  if (def == nullptr) { check(false, "H build"); return; }
  core::SynthRuntime& rt = def->runtime();
  const core::ArpSeqParams& a = rt.keyboardArpSeqParams(core::KeyboardSide::Left);
  // H10..H19 read the EXECUTED configuration (KeyboardBehaviour::executed()), i.e. the
  // per-behaviour state tick() actually runs off — not the decoded request `params_`.
  const core::KeyboardBehaviour::Executed e =
      rt.keyboardBehaviourExecuted(core::KeyboardSide::Left);

  check(a.mode == 2, "H01 keyboard.mode (101) read back");
  check(a.arpHold == 1, "H02 keyboard.arp_hold (102) read back");
  check(a.arpDirection == 3, "H03 keyboard.arp_direction (104) read back");
  check(a.arpVariation == 2, "H04 keyboard.arp_variation (105) read back");
  check(near(a.arpInterval, 0.5, 1e-9), "H05 keyboard.arp_interval (106) read back");
  check(a.seqRun == 1, "H06 keyboard.seq_run (109) read back");
  check(near(a.seqLength, 0.5, 1e-9), "H07 keyboard.seq_length (110) read back");
  check(a.seqDirection == 2, "H08 keyboard.seq_direction (112) read back");
  check(a.seqCvOutput == 1, "H09 keyboard.seq_cv_output (113) read back");
  check(near(e.portamentoTauSeconds, 0.5 * core::kPortamentoMaxSeconds, 1e-12) && e.portamentoLegato,
        "H10 keyboard.portamento_speed (117) EXECUTED as the installed glide time constant");
  check(e.portamentoLegato,
        "H11 keyboard.portamento_legato (118) EXECUTED as the installed legato flag");
  check(near(e.vibratoSpeedHz, 0.25 * core::kVibratoMaxHz, 1e-12),
        "H12 keyboard.vibrato_speed (119) EXECUTED as the installed LFO rate (Hz)");
  check(near(e.vibratoDepthCv, 0.5 * core::kVibratoMaxDepthCv, 1e-12),
        "H13 keyboard.vibrato_depth (120) EXECUTED as the installed depth (V)");
  check(near(e.vibratoDelaySeconds, 0.75 * core::kVibratoMaxDelaySec, 1e-12),
        "H14 keyboard.vibrato_delay (121) EXECUTED as the installed ramp time (s)");
  check(e.vibratoPressureControl && near(e.vibratoPressureAmount, 1.0, 1e-12),
        "H15 keyboard.vibrato_pressure (122) EXECUTED as the installed pressure-control scale");
  check(e.pressureMode == core::PressureOutput::Loop,
        "H16 keyboard.pressure_output (123) EXECUTED as the installed outlet mode");
  check(near(e.pressureRiseSeconds, 0.5 * core::kPressureMaxSeconds, 1e-12),
        "H17 keyboard.pressure_rise (124) EXECUTED as the installed rise time (s)");
  check(near(e.pressureFallSeconds, 0.25 * core::kPressureMaxSeconds, 1e-12),
        "H18 keyboard.pressure_fall (125) EXECUTED as the installed fall time (s)");
  check(e.rootSemitone == 6 && e.scaleMask == core::kScaleIonian && near(e.sampleRate, kSr, 1e-9),
        "H19 keyboard.root_note (128) + scale editor EXECUTED (root 6, resolved mask, live rate)");
}

// Decoded-but-UNCONSUMED items: they are parsed into the side parameter set but drive no
// behaviour (contract §3: keep the parse, never count it as applied). Proven by building
// two states that differ ONLY in these three and requiring identical output.
static void accept_parsed_but_unconsumed() {
  std::printf("H'  -- the decoded-but-unconsumed items (108/115/127) change NO output\n");
  core::DeviceStateV1 base = core::make_default_device_state(kSeed);
  set_left(base, core::ParameterId::keyboard_mode, 1.0);  // Arpeggiator (exercises arp params)
  core::DeviceStateV1 alt = base;
  set_left(alt, core::ParameterId::keyboard_arp_length, 1.0);         // 108
  set_left(alt, core::ParameterId::keyboard_seq_rhythm_length, 1.0);  // 115
  set_left(alt, core::ParameterId::keyboard_quantise_load_scale, 7.0);  // 127

  std::unique_ptr<core::MachineRuntimeDefinition> d0 = chain(base);
  std::unique_ptr<core::MachineRuntimeDefinition> d1 = chain(alt);
  if (d0 == nullptr || d1 == nullptr) { check(false, "H' build"); return; }
  // The parse is preserved (readback), so the fields are not silently dropped.
  const core::ArpSeqParams& a1 = d1->runtime().keyboardArpSeqParams(core::KeyboardSide::Left);
  const core::KeyboardBehaviourParams& b1 =
      d1->runtime().keyboardBehaviourParams(core::KeyboardSide::Left);
  check(near(a1.arpLength, 1.0, 1e-9) && near(a1.seqRhythmLength, 1.0, 1e-9) &&
            b1.quantiseLoadScale == 7,
        "H'1 arp_length / seq_rhythm_length / quantise_load_scale are still PARSED");

  std::vector<double> t0, t1;
  core::SynthRuntime& r0 = d0->runtime();
  core::SynthRuntime& r1 = d1->runtime();
  note(r0, core::KeyboardSide::Left, 0.0, 0.5, 1, 0);
  note(r1, core::KeyboardSide::Left, 0.0, 0.5, 1, 0);
  clock_edge(r0, 500);
  clock_edge(r1, 500);
  clock_edge(r0, 1500);
  clock_edge(r1, 1500);
  render(r0, 2400, &t0);
  render(r1, 2400, &t1);
  check(t0 == t1,
        "H'2 the three unconsumed items change NO sample (honest 'parsed, not applied')");
}

// ============================================================ behaviour families

static void accept_portamento_family() {
  std::printf("I  -- portamento family (117 speed / 118 legato) has discriminating output\n");
  // 117: a slow glide vs an instant jump.
  {
    core::DeviceStateV1 slow = core::make_default_device_state(kSeed);
    set_left(slow, core::ParameterId::keyboard_portamento_speed, 1.0);
    core::DeviceStateV1 fast = core::make_default_device_state(kSeed);
    std::unique_ptr<core::MachineRuntimeDefinition> ds = chain(slow);
    std::unique_ptr<core::MachineRuntimeDefinition> df = chain(fast);
    if (ds == nullptr || df == nullptr) { check(false, "I1 build"); return; }
    note(ds->runtime(), core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    note(df->runtime(), core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    const std::vector<double> ts = trace_pitch(ds->runtime(), 12000);
    const std::vector<double> tf = trace_pitch(df->runtime(), 12000);
    check(ts.back() < 0.3 && near(tf.back(), 1.0, 1e-9),
          "I1 portamento_speed: slow glides, zero jumps instantly");
  }
  // 118: legato ON with a single plate jumps even at the slow speed.
  {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_portamento_speed, 1.0);
    set_left(st, core::ParameterId::keyboard_portamento_legato, 1.0);
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) { check(false, "I2 build"); return; }
    note(def->runtime(), core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    const std::vector<double> t = trace_pitch(def->runtime(), 64);
    check(near(t.back(), 1.0, 1e-9), "I2 portamento_legato=on jumps on a single plate");
  }
}

static void accept_vibrato_family() {
  std::printf("J  -- vibrato family (119/120/121/122) has discriminating output\n");
  const auto build_vib = [](double speed, double depth, double delay, double pressure,
                            std::unique_ptr<core::MachineRuntimeDefinition>* out) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_left(st, core::ParameterId::keyboard_vibrato_speed, speed);
    set_left(st, core::ParameterId::keyboard_vibrato_depth, depth);
    set_left(st, core::ParameterId::keyboard_vibrato_delay, delay);
    set_left(st, core::ParameterId::keyboard_vibrato_pressure, pressure);
    *out = chain(st);
  };

  // 120 depth: full-depth modulation vs none.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> on, off;
    build_vib(1.0, 1.0, 0.0, 0.0, &on);
    build_vib(1.0, 0.0, 0.0, 0.0, &off);
    if (on == nullptr || off == nullptr) { check(false, "J1 build"); return; }
    note(on->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(off->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    const double rOn = range_of(trace_pitch(on->runtime(), 6400));
    const double rOff = range_of(trace_pitch(off->runtime(), 6400));
    check(rOn > 0.2 && rOff < 1e-9, "J1 vibrato_depth: full depth modulates, zero does not");
  }
  // 119 speed: 15 Hz vs 1.5 Hz -> a different number of modulation cycles in the window.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> fast, slow;
    build_vib(1.0, 1.0, 0.0, 0.0, &fast);
    build_vib(0.1, 1.0, 0.0, 0.0, &slow);
    if (fast == nullptr || slow == nullptr) { check(false, "J2 build"); return; }
    note(fast->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(slow->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    const int cFast = crossings_of(trace_pitch(fast->runtime(), 6400), 1.0);
    const int cSlow = crossings_of(trace_pitch(slow->runtime(), 6400), 1.0);
    check(cFast >= 3 && cSlow <= 1, "J2 vibrato_speed: 15 Hz cycles far more often than 1.5 Hz");
  }
  // 121 delay: a 2.5 s ramp keeps the modulation tiny over the first 0.13 s.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> delayed, immediate;
    build_vib(1.0, 1.0, 1.0, 0.0, &delayed);
    build_vib(1.0, 1.0, 0.0, 0.0, &immediate);
    if (delayed == nullptr || immediate == nullptr) { check(false, "J3 build"); return; }
    note(delayed->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(immediate->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    const double rDelayed = range_of(trace_pitch(delayed->runtime(), 6400));
    const double rImmediate = range_of(trace_pitch(immediate->runtime(), 6400));
    check(rDelayed < 0.05 && rImmediate > 0.2,
          "J3 vibrato_delay: a long ramp holds the modulation back");
  }
  // 122 pressure: the depth scales with the live key pressure.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> ctrl, plain;
    build_vib(1.0, 1.0, 0.0, 1.0, &ctrl);
    build_vib(1.0, 1.0, 0.0, 0.0, &plain);
    if (ctrl == nullptr || plain == nullptr) { check(false, "J4 build"); return; }
    note(ctrl->runtime(), core::KeyboardSide::Left, 1.0, 1.0, 1, 0);
    note(plain->runtime(), core::KeyboardSide::Left, 1.0, 1.0, 1, 0);
    const double rCtrl = range_of(trace_pitch(ctrl->runtime(), 6400));
    const double rPlain = range_of(trace_pitch(plain->runtime(), 6400));
    check(rCtrl > rPlain * 1.5,
          "J4 vibrato_pressure=on scales the depth with key pressure");
  }
}

static void accept_pressure_family() {
  std::printf("K  -- pressure family (123 mode / 124 rise / 125 fall) discriminates\n");
  const auto build_press = [](double mode, double rise, double fall,
                              std::unique_ptr<core::MachineRuntimeDefinition>* out) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_pressure_out(st, static_cast<std::uint8_t>(mode));
    set_left(st, core::ParameterId::keyboard_pressure_rise, rise);
    set_left(st, core::ParameterId::keyboard_pressure_fall, fall);
    *out = chain(st);
  };

  // 123: Pressure (holds) vs Ad (decays away) with identical times.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> hold, ad;
    build_press(0.0, 0.0, 0.0, &hold);
    build_press(2.0, 0.0, 0.0, &ad);
    if (hold == nullptr || ad == nullptr) { check(false, "K1 build"); return; }
    note(hold->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    note(ad->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    render(hold->runtime(), 4800);
    render(ad->runtime(), 4800);
    const double vh = snap(hold->runtime()).press;
    const double va = snap(ad->runtime()).press;
    check(near(vh, 1.0, 1e-9) && va < 1e-6,
          "K1 pressure_output: Pressure holds the level, Ad decays to zero");
  }
  // 124 rise: a 2.5 s attack vs an instant one.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> slow, fast;
    build_press(0.0, 1.0, 0.0, &slow);
    build_press(0.0, 0.0, 0.0, &fast);
    if (slow == nullptr || fast == nullptr) { check(false, "K2 build"); return; }
    note(slow->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    note(fast->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    render(slow->runtime(), 12000);
    render(fast->runtime(), 12000);
    check(snap(slow->runtime()).press < 0.3 && near(snap(fast->runtime()).press, 1.0, 1e-9),
          "K2 pressure_rise: a slow rise has only just started after 0.25 s");
  }
  // 125 fall: an Ad decay with a slow fall vs an instant one.
  {
    std::unique_ptr<core::MachineRuntimeDefinition> slow, fast;
    build_press(2.0, 0.0, 1.0, &slow);
    build_press(2.0, 0.0, 0.0, &fast);
    if (slow == nullptr || fast == nullptr) { check(false, "K3 build"); return; }
    note(slow->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    note(fast->runtime(), core::KeyboardSide::Left, 0.0, 1.0, 1, 0);
    render(slow->runtime(), 12000);
    render(fast->runtime(), 12000);
    check(snap(slow->runtime()).press > 0.8 && snap(fast->runtime()).press < 1e-6,
          "K3 pressure_fall: a slow fall still holds most of the level after 0.25 s");
  }
}

static void accept_quantiser_root() {
  std::printf("L  -- quantiser root (128) discriminates a sparse (Ionian) scale\n");
  const auto build_q = [](double root, std::unique_ptr<core::MachineRuntimeDefinition>* out) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    // A SPARSE scale: the root only moves the result when the mask omits notes. A
    // chromatic mask would be root-independent by construction, so it could not
    // discriminate this parameter at all.
    st.keyboardScaleEditor = core::kScaleIonian;
    set_left(st, core::ParameterId::keyboard_root_note, root);
    *out = chain(st);
  };
  std::unique_ptr<core::MachineRuntimeDefinition> r0, r6;
  build_q(0.0, &r0);
  build_q(0.5, &r6);
  if (r0 == nullptr || r6 == nullptr) { check(false, "L build"); return; }
  note(r0->runtime(), core::KeyboardSide::Left, 0.06, 0.0, 1, 0);
  note(r6->runtime(), core::KeyboardSide::Left, 0.06, 0.0, 1, 0);
  render(r0->runtime(), 480);
  render(r6->runtime(), 480);
  const double p0 = snap(r0->runtime()).vOct;
  const double p6 = snap(r6->runtime()).vOct;
  check(near(p0, 0.0, 1e-9) && near(p6, 1.0 / 12.0, 1e-9),
        "L1 root_note shifts the quantiser root (0 -> 0.0 V, 0.5 -> +1 semitone)");
}

// ============================================================ default equivalence

static void accept_default_equivalence() {
  std::printf("M  -- default prepare == default restore (one documented parse difference)\n");
  core::MachineRuntimeDefinition prepared(kSeed, kSr);  // the seed-convenience ctor
  std::unique_ptr<core::MachineRuntimeDefinition> restored =
      chain(core::make_default_device_state(kSeed));
  if (restored == nullptr) { check(false, "M build"); return; }
  core::SynthRuntime& rp = prepared.runtime();
  core::SynthRuntime& rr = restored->runtime();

  check(prepared.valid() && restored->valid(), "M1 both default paths compile a valid graph");
  check(rp.keyboardMode() == rr.keyboardMode(), "M2 both default paths decode the same mode");
  for (int s = 0; s < 2; ++s) {
    const core::KeyboardSide side = (s == 0) ? core::KeyboardSide::Left : core::KeyboardSide::Right;
    const core::ArpSeqParams& ap = rp.keyboardArpSeqParams(side);
    const core::ArpSeqParams& ar = rr.keyboardArpSeqParams(side);
    const core::KeyboardBehaviourParams& bp = rp.keyboardBehaviourParams(side);
    const core::KeyboardBehaviourParams& br = rr.keyboardBehaviourParams(side);
    const bool same =
        ap.mode == ar.mode && ap.arpHold == ar.arpHold && ap.arpDirection == ar.arpDirection &&
        ap.arpVariation == ar.arpVariation && near(ap.arpInterval, ar.arpInterval, 0.0) &&
        near(ap.arpLength, ar.arpLength, 0.0) && ap.seqRun == ar.seqRun &&
        near(ap.seqLength, ar.seqLength, 0.0) && ap.seqDirection == ar.seqDirection &&
        ap.seqCvOutput == ar.seqCvOutput && near(ap.seqRhythmLength, ar.seqRhythmLength, 0.0) &&
        near(bp.portamentoSpeed, br.portamentoSpeed, 0.0) &&
        bp.portamentoLegato == br.portamentoLegato &&
        near(bp.vibratoSpeed, br.vibratoSpeed, 0.0) && near(bp.vibratoDepth, br.vibratoDepth, 0.0) &&
        near(bp.vibratoDelay, br.vibratoDelay, 0.0) &&
        near(bp.vibratoPressure, br.vibratoPressure, 0.0) &&
        bp.pressureOutput == br.pressureOutput && near(bp.pressureRise, br.pressureRise, 0.0) &&
        near(bp.pressureFall, br.pressureFall, 0.0) && near(bp.rootNote, br.rootNote, 0.0) &&
        bp.scaleEditor == br.scaleEditor;
    check(same, s == 0 ? "M3 left side: prepare == restore, field by field"
                       : "M3 right side: prepare == restore, field by field");
  }
  // The ONE documented difference: clock_bpm (129) is a norm with no evidenced norm->BPM
  // law. Pre-#101 the arp/seq carried its struct default (120) with no consumer; the
  // restore now parses the registry norm (0.0) into the same unused field. It drives no
  // clock, so the samples are identical — asserted below.
  check(rr.keyboardParsedBpm(core::KeyboardSide::Left) ==
            rr.keyboardParsedBpm(core::KeyboardSide::Right),
        "M4 the parsed (unconsumed) clock_bpm is one shared global value");
  check(rr.keyboardParsedBpm(core::KeyboardSide::Left) == 0.0,
        "M4 the parsed clock_bpm is the registry norm 0.0 (NOT a BPM, no consumer)");

  // Identical control sequence + identical samples.
  note(rp, core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
  note(rr, core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
  clock_edge(rp, 500);
  clock_edge(rr, 500);
  std::vector<double> ap, ar;
  render(rp, 4800, &ap);
  render(rr, 4800, &ar);
  check(ap == ar, "M5 prepare and restore render bit-identical dryA");
  const Snap sp = snap(rp), sr = snap(rr);
  check(sp.vOct == sr.vOct && sp.gateL == sr.gateL && sp.gateR == sr.gateR &&
            sp.press == sr.press,
        "M5 prepare and restore publish identical keyboard outputs");
}

// ============================================================ block invariance

static void accept_block_invariance() {
  std::printf("N  -- 64/256/irregular partitions are bit-identical\n");
  const auto run = [](const std::vector<int>& schedule, std::vector<double>* dryA) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split: both sides active
    set_left(st, core::ParameterId::keyboard_portamento_speed, 0.3);
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
    if (def == nullptr) return Snap{};
    core::SynthRuntime& rt = def->runtime();
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 0);
    clock_edge(rt, 500);
    release(rt, core::KeyboardSide::Left, 1, 1500);
    for (int n : schedule) render(rt, n, dryA);
    return snap(rt);
  };
  std::vector<double> a, b, c;
  const Snap sa = run(std::vector<int>(128, 64), &a);          // 8192 frames in 64s
  const Snap sb = run(std::vector<int>(32, 256), &b);          // 8192 frames in 256s
  // Same 8192 frames, an irregular partition (the schedule sums to 8192 exactly).
  const Snap sc = run(std::vector<int>{300, 100, 1024, 7, 501, 700, 400, 1000, 900, 400, 486, 2374}, &c);
  check(!a.empty() && a == b && b == c, "N1 dryA is bit-identical across 64/256/irregular");
  check(sa.vOct == sb.vOct && sb.vOct == sc.vOct && sa.gateL == sb.gateL &&
            sb.gateL == sc.gateL && sa.gateR == sb.gateR && sb.gateR == sc.gateR &&
            sa.press == sb.press && sb.press == sc.press,
        "N2 the four keyboard outputs are identical across partitions");
}

// ============================================================ determinism + reject

static void accept_repeat_and_reject() {
  std::printf("O  -- same state restored twice is identical; an invalid state is rejected\n");
  // O1: EVERY mode — the same state restored twice must be bit-identical (both sides driven,
  // so a per-side instance that carried state across restores would show up here).
  for (int mode = 0; mode <= 2; ++mode) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, mode);
    set_left(st, core::ParameterId::keyboard_portamento_speed, 0.7);
    set_right(st, core::ParameterId::keyboard_arp_direction, 2.0);
    std::unique_ptr<core::MachineRuntimeDefinition> d1 = chain(st);
    std::unique_ptr<core::MachineRuntimeDefinition> d2 = chain(st);
    if (d1 == nullptr || d2 == nullptr) { check(false, "O1 build"); continue; }
    note(d1->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(d2->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(d1->runtime(), core::KeyboardSide::Right, 2.0, 0.3, 2, 0);
    note(d2->runtime(), core::KeyboardSide::Right, 2.0, 0.3, 2, 0);
    std::vector<double> a, b;
    render(d1->runtime(), 4800, &a);
    render(d2->runtime(), 4800, &b);
    check(a == b && snap(d1->runtime()).vOct == snap(d2->runtime()).vOct &&
              snap(d1->runtime()).gateR == snap(d2->runtime()).gateR,
          mode == 0   ? "O1 Single: the same state restored twice is bit-identical"
          : mode == 1 ? "O1 Twin: the same state restored twice is bit-identical"
                      : "O1 Split: the same state restored twice is bit-identical");
  }

  // An invalid state (keyboard.mode out of its 0..2 range) must be rejected by the chain
  // and produce NO runtime — the product never publishes a definition from a bad state.
  core::DeviceStateV1 bad = core::make_default_device_state(kSeed);
  set_left(bad, core::ParameterId::keyboard_mode, 99.0);
  std::vector<std::uint8_t> buf(core::kDeviceStorageSchema.totalBytesHint);
  std::size_t written = 0;
  core::DeviceStateV1 decoded;
  const bool enc = core::encode_device_state(bad, buf.data(), buf.size(), &written);
  const bool dec = enc && core::decode_device_state(buf.data(), written, &decoded);
  core::MachineCandidateResult r =
      dec ? core::buildMachineRuntimeCandidate(decoded, kSr)
          : core::MachineCandidateResult{core::MachineCandidateStatus::rejected_state, {}, nullptr};
  check(r.status == core::MachineCandidateStatus::rejected_state && r.definition == nullptr,
        "O2 an out-of-range keyboard.mode is rejected_state with no definition");

  // The rejected path leaves the caller's state untouched (re-encode compares equal).
  std::vector<std::uint8_t> after(core::kDeviceStorageSchema.totalBytesHint);
  std::size_t written2 = 0;
  const bool enc2 = core::encode_device_state(bad, after.data(), after.size(), &written2);
  check(enc2 && written == written2 && buf == after,
        "O3 a rejected candidate leaves the caller's state byte-identical");

  // O4: a rejected candidate has NO effect on an ALREADY-ACTIVE runtime — the active
  // state/format/plan and the subsequent trace are preserved. Two identical runtimes are
  // driven in lockstep; the rejected attempt is made between the halves of one of them.
  {
    core::DeviceStateV1 good = core::make_default_device_state(kSeed);
    set_mode(good, 2);
    std::unique_ptr<core::MachineRuntimeDefinition> g = chain(good);
    std::unique_ptr<core::MachineRuntimeDefinition> c = chain(good);
    if (g == nullptr || c == nullptr) { check(false, "O4 build"); return; }
    std::vector<double> ta, tb;
    note(g->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    note(c->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
    render(g->runtime(), 2400, &ta);
    render(c->runtime(), 2400, &tb);
    const core::MachineCandidateResult r2 = core::buildMachineRuntimeCandidate(bad, kSr);
    check(r2.status == core::MachineCandidateStatus::rejected_state,
          "O4 the mid-flight rejected attempt is still rejected_state");
    render(g->runtime(), 2400, &ta);
    render(c->runtime(), 2400, &tb);
    check(ta == tb,
          "O4 a rejected candidate leaves an ACTIVE runtime's subsequent trace bit-identical");
  }
}

// ============================================================ Q. host single-commit entry

// Q: the SAME mode/restore/failure facts, re-entered through the host's ONE commit entry —
// EngineHarness (encode -> decode -> StandaloneAudioEngine::applyDeviceState -> processBlock,
// capturing the four real output channels). O2/O4 above only prove that a rejected CANDIDATE is
// null and that the caller's state object is untouched; they never touch an owner that is
// already rendering. Q2/Q3 pin the atomic contract where it matters: a LIVE owner keeps its
// prior state/format/plan AND its subsequent trace after a rejected commit attempt.
//
// FINDING F-1 (fixed in this slice, @Codex msg 0fd75e9f): the host render path
// (StandaloneAudioEngine::processBlock -> DeviceAdapter::renderBlock) used to call
// SynthRuntime::processFrame per frame and never drained EventTimebase, so the notes enqueued
// below were invisible on this entry and the compared traces were a free-running machine. The
// adapter now drives each frame through the EXISTING SynthRuntime::processBlock(&in, 1, &out,
// true) — one drain, one frame of time, one timebase (see U). Q's queued events therefore ACT
// here, and the checks below are statements about an event-driven host render, not a free run.
static void accept_host_entry() {
  std::printf("Q  -- the host single-commit entry (EngineHarness) is accepted and atomic\n");

  // Q1: every mode is accepted through the host entry, and a re-commit of the SAME state on the
  // same owner is output-identical (the host's stopped-stream re-apply path).
  for (int mode = 0; mode <= 2; ++mode) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, static_cast<std::uint8_t>(mode));
    set_left(st, core::ParameterId::keyboard_portamento_speed, 0.4);
    set_right(st, core::ParameterId::keyboard_portamento_speed, 0.1);
    testengine::EngineHarness once, twice;
    const bool ok1 = once.load(st);
    const bool ok2 = twice.load(st) && twice.load(st);
    if (!ok1 || !ok2) { check(false, "Q1 host load"); continue; }
    const auto script = [](core::SynthRuntime& rt) {
      note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
      note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 240);
      release(rt, core::KeyboardSide::Left, 1, 1200);
    };
    script(*once.producerRuntime());
    script(*twice.producerRuntime());
    const bool rendered = once.render(4800) && twice.render(4800);
    const bool fmt = once.ready() && once.sampleRate() == kSr && once.blockSize() == 4096 &&
                     once.inputCapability() == 2 && once.outputCapability() == 4 &&
                     once.plan().outputCount == 4 &&
                     once.plan().input == core::InputRoute::Distinct;
    check(rendered && fmt && twice.ready() && once.out(0) == twice.out(0) &&
              once.out(1) == twice.out(1) && once.out(2) == twice.out(2) &&
              once.out(3) == twice.out(3),
          mode == 0 ? "Q1 Single: host entry accepted; re-commit of the same state is output-identical"
          : mode == 1
              ? "Q1 Twin: host entry accepted; re-commit of the same state is output-identical"
              : "Q1 Split: host entry accepted; re-commit of the same state is output-identical");
  }

  // Q1b (non-vacuity): the host-path trace these checks compare is a LIVE free-running machine,
  // not four silent buffers — so "identical" is a real statement about rendered audio.
  {
    testengine::EngineHarness live;
    if (!live.load(core::make_default_device_state(kSeed)) || !live.render(4800)) {
      check(false, "Q1b live render");
    } else {
      check(range_of(live.out(3)) > 0.01 && range_of(live.out(2)) > 0.001,
            "Q1b the host-path four-channel trace is a live render (DRY_B / DRY_A both move)");
    }
  }

  // Q2: an INVALID state committed to a LIVE, rendering owner is rejected atomically.
  core::DeviceStateV1 a = core::make_default_device_state(kSeed);
  set_mode(a, 2);
  set_left(a, core::ParameterId::keyboard_portamento_speed, 0.4);
  set_right(a, core::ParameterId::keyboard_portamento_speed, 0.1);
  testengine::EngineHarness subj, ctrl;
  if (!subj.load(a) || !ctrl.load(a)) { check(false, "Q2 load"); return; }
  const core::DevicePlan planA = subj.plan();
  const core::DeviceStateV1* canonA = subj.canonicalState();
  const std::vector<std::uint8_t> wireA = wire_of(a);
  const auto script2 = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 100);
    release(rt, core::KeyboardSide::Left, 1, 2000);
    note(rt, core::KeyboardSide::Right, 1.5, 0.4, 2, 3000);
  };
  script2(*subj.producerRuntime());
  script2(*ctrl.producerRuntime());
  const bool p1 = subj.render(2400) && ctrl.render(2400);
  core::DeviceStateV1 bad = a;
  set_mode(bad, 99);  // out of the canonical 0..2 selector range
  const bool rejected = !subj.load(bad);
  check(p1 && rejected &&
            subj.applyStatus() ==
                testengine::StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState,
        "Q2 a live owner's invalid commit is RejectedInvalidState (typed, not a bare false)");
  check(!subj.validation().ok,
        "Q2 the rejection carries the validation family/field for the caller");
  check(subj.ready() && same_plan(subj.plan(), planA) && subj.sampleRate() == kSr &&
            subj.blockSize() == 4096 && subj.inputCapability() == 2 &&
            subj.outputCapability() == 4,
        "Q2 the live owner's state/format/plan are UNCHANGED by the rejected commit");
  check(subj.canonicalState() == canonA && wire_of(*subj.canonicalState()) == wireA,
        "Q2 the canonical state is the SAME object holding the SAME bytes (A, never the rejected B)");
  const bool p2 = subj.render(2400) && ctrl.render(2400);
  check(p2 && subj.out(0) == ctrl.out(0) && subj.out(1) == ctrl.out(1) &&
            subj.out(2) == ctrl.out(2) && subj.out(3) == ctrl.out(3) &&
            range_of(ctrl.out(3)) > 0.01,
        "Q2 the live owner's SUBSEQUENT trace still equals the untouched A control (4 channels)");

  // Q3: an ILLEGAL FORMAT commit on a live owner is likewise atomic (RejectedFormat), and the
  // owner keeps rendering with its prior format.
  testengine::EngineHarness fctrl;
  if (!fctrl.load(a)) { check(false, "Q3 control load"); return; }
  script2(*fctrl.producerRuntime());
  if (!fctrl.render(1200)) { check(false, "Q3 control render"); return; }
  const auto try_format = [&](double sr, int block, int outCh, const char* what) {
    testengine::EngineHarness h;
    if (!h.load(a)) { check(false, what); return; }
    const core::DevicePlan p0 = h.plan();
    const core::DeviceStateV1* c0 = h.canonicalState();
    script2(*h.producerRuntime());
    const bool rejected = !h.load(a, sr, block, 2, outCh) &&
                          h.applyStatus() ==
                              testengine::StandaloneAudioEngine::StateApplyStatus::RejectedFormat;
    const bool preserved = h.ready() && same_plan(h.plan(), p0) && h.canonicalState() == c0 &&
                           h.sampleRate() == kSr && h.blockSize() == 4096 &&
                           h.inputCapability() == 2 && h.outputCapability() == 4;
    const bool renders = h.render(1200) && h.out(0) == fctrl.out(0) && h.out(1) == fctrl.out(1) &&
                         h.out(2) == fctrl.out(2) && h.out(3) == fctrl.out(3);
    check(rejected && preserved && renders, what);
  };
  try_format(0.0, 4096, 4, "Q3 sampleRate=0 is RejectedFormat; the live owner keeps format+plan+trace");
  try_format(kSr, 0, 4, "Q3 blockSize=0 is RejectedFormat; the live owner keeps format+plan+trace");
  try_format(kSr, 4096, 1, "Q3 outCh=1 is RejectedFormat; the live owner keeps format+plan+trace");

  // Q4 (anti-vacuity): a LEGAL commit through the SAME entry DOES install B — the canonical state
  // object and its bytes change, the committed format changes, and the free-running audio changes
  // (VCO-B is re-tuned two octaves up) — so the "unchanged after rejection" checks above cannot
  // be true merely because nothing on this entry ever changes.
  testengine::EngineHarness legal;
  if (!legal.load(a)) { check(false, "Q4 load A"); return; }
  if (!legal.render(2400)) { check(false, "Q4 render A"); return; }
  const std::vector<double> outA = legal.out(3);
  const std::size_t nA = outA.size();
  core::DeviceStateV1 b = core::make_default_device_state(kSeed);
  set_left(b, core::ParameterId::vco_b_oct_sel, 2.0);  // VCO-B +2 octaves, a free-run change
  const std::vector<std::uint8_t> wireB = wire_of(b);
  const bool accepted =
      legal.load(b, kSr, 2048) &&
      legal.applyStatus() == testengine::StandaloneAudioEngine::StateApplyStatus::Accepted;
  if (!legal.render(2400)) { check(false, "Q4 render B"); return; }
  const std::vector<double> outB(legal.out(3).begin() + static_cast<std::ptrdiff_t>(nA),
                                 legal.out(3).end());
  check(accepted && legal.canonicalState() != canonA &&
            wire_of(*legal.canonicalState()) == wireB && wireB != wireA && legal.blockSize() == 2048 &&
            legal.ready() && outB.size() == nA &&
            crossings_of(outB, 0.0) > 3 * crossings_of(outA, 0.0),
        "Q4 a LEGAL re-commit replaces state + format + free-run output (not vacuous)");
}

// ============================================================ R. real graph consumption

// R: the two NEW right-side outputs must drive real, existing consumers through LEGAL user
// cables — not just read back. Override and removal are both checked, and the evidence is the
// consumer's own behaviour (VCO-B audio / the EG-B envelope), never a published value. These run
// on the REAL host render path (EngineHarness -> StandaloneAudioEngine::processBlock ->
// DeviceAdapter::renderBlock), the entry the product runs, so the enqueued keyboard events are
// delivered by the same EventTimebase drain (F-1 fixed; see U). VCO-B is read from the host's
// DRY_B channel: the one frozen device scale (x0.5) is positive, so zero-crossing counts are
// exactly those of the canonical render.
static void accept_cable_consumption() {
  std::printf("R  -- legal user cables make the new right outputs drive real consumers\n");

  // R1: keyboard.pressure_out (in Split = the RIGHT pitch, criterion E) into vco_b.v_oct_in
  // overrides route.keyboard_v_oct_to_vco_b (whose normalised source is keyboard_v_oct_out).
  const auto dryB = [](bool cable, double leftPitch, double rightPitch, std::vector<double>* out) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);
    if (cable)
      set_cable(st, core::JackId::keyboard_pressure_out, core::JackId::vco_b_v_oct_in);
    testengine::EngineHarness h;
    if (!h.load(st, kSr, 9600)) return false;  // one real 9600-frame host block below.
    note(*h.producerRuntime(), core::KeyboardSide::Left, leftPitch, 0.0, 1, 0);
    note(*h.producerRuntime(), core::KeyboardSide::Right, rightPitch, 0.0, 2, 0);
    if (!h.renderBlock(9600, [](std::size_t, double& a, double& b) { a = 0.0; b = 0.0; }))
      return false;
    *out = h.out(3);
    return true;
  };
  std::vector<double> cabled, plain, rightZeroCabled, rightZeroPlain, removed;
  if (!dryB(true, 1.0, 2.0, &cabled) || !dryB(false, 1.0, 2.0, &plain) ||
      !dryB(true, 1.0, 0.0, &rightZeroCabled) || !dryB(false, 1.0, 0.0, &rightZeroPlain) ||
      !dryB(false, 1.0, 2.0, &removed)) {
    check(false, "R1 render");
    return;
  }
  const int zc = crossings_of(cabled, 0.0), zp = crossings_of(plain, 0.0);
  const int zr = crossings_of(rightZeroCabled, 0.0), zq = crossings_of(rightZeroPlain, 0.0);
  check(zp > 100 && zc > 1.7 * zp && zc < 2.3 * zp,
        "R1 pressure->VCO-B: the cabled VCO-B tracks the RIGHT pitch (dryB ~2x at +1 octave)");
  check(zr > 0 && zq > 100 && zq > 1.7 * zr && zq < 2.3 * zr,
        "R1 the cabled VCO-B follows the right side, not the left/main normalised pitch");
  check(removed == plain && cabled != plain,
        "R1 removing the cable restores the normalised route bit-identically");

  // R2: keyboard.gate_right_out into envelope_b.gate_in overrides route.keyboard_gate_to_eg_b
  // (normalised source = keyboard_gate_left_main_out). The consumer read is EG-B's own env_out.
  const auto envB = [](bool cable, bool playRight, std::vector<double>* out) {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);
    if (cable)
      set_cable(st, core::JackId::keyboard_gate_right_out, core::JackId::envelope_b_gate_in);
    testengine::EngineHarness h;
    if (!h.load(st, kSr, 4800)) return false;
    if (playRight) note(*h.producerRuntime(), core::KeyboardSide::Right, 1.0, 0.0, 2, 0);
    else note(*h.producerRuntime(), core::KeyboardSide::Left, 1.0, 0.0, 1, 0);
    out->reserve(4800);
    // Per-frame host renders (block=1) with the published EG-B output sampled after each frame.
    return h.renderSampled(4800, 0.0, [out](const core::SynthRuntime& rt) {
      out->push_back(rt.controlVoltageAt(core::JackId::envelope_b_env_out));
    });
  };
  std::vector<double> egLeftDefault, egRightDefault, egRightCabled, egLeftCabled, egLeftRemoved;
  if (!envB(false, false, &egLeftDefault) || !envB(false, true, &egRightDefault) ||
      !envB(true, true, &egRightCabled) || !envB(true, false, &egLeftCabled) ||
      !envB(false, false, &egLeftRemoved)) {
    check(false, "R2 render");
    return;
  }
  check(range_of(egLeftDefault) > 0.05,
        "R2 EG-B opens from the normalised LEFT gate with no cable (the default route is real)");
  check(range_of(egRightDefault) < 1e-9,
        "R2 the right gate does NOT reach EG-B without a cable (no implicit second feed)");
  check(range_of(egRightCabled) > 0.05,
        "R2 the gate_right->EG-B cable makes the RIGHT gate drive EG-B");
  check(range_of(egLeftCabled) < 1e-9,
        "R2 the cable OVERRIDES (does not sum with) the normalised LEFT gate");
  check(egLeftRemoved == egLeftDefault,
        "R2 removing the cable restores the normalised LEFT-gate route bit-identically");
}

// ============================================================ S. block boundaries

// S: the N-section fix. N compared only dryA and the four keyboard values at the END of the run,
// which cannot see a RIGHT-side error that occurs and is corrected mid-run, and the right side is
// not wired to VCO-B by default so left DRY_A cannot stand in for it. Here the same state + event
// script is rendered (a) one frame per block (the reference, canonical runtime entry) and (b) in
// REAL 256-frame blocks through the REAL HOST ENTRY (EngineHarness -> applyDeviceState ->
// StandaloneAudioEngine::processBlock -> DeviceAdapter::renderBlock), with key events landing
// mid-block; all four host audio channels are compared (through the one frozen 0.5 device scale),
// and the four published values are compared AT EVERY REAL BLOCK BOUNDARY. Both entries drain the
// ONE EventTimebase (F-1 fixed), so the partition is a delivery detail and nothing else.
static void accept_block_boundary_traces() {
  std::printf("S  -- real block boundaries: left/right control traces vs a per-frame reference\n");
  constexpr int kBlock = 256;
  constexpr int kBlocks = 8;
  const auto build = []() {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split: the right side publishes its own pitch (pressure jack) and gate
    set_left(st, core::ParameterId::keyboard_portamento_speed, 0.4);
    set_right(st, core::ParameterId::keyboard_portamento_speed, 0.0);
    set_left(st, core::ParameterId::keyboard_vibrato_speed, 0.6);
    set_left(st, core::ParameterId::keyboard_vibrato_depth, 1.0);
    // Both NEW right outputs drive real consumers, so a mid-block right-side error is audible in
    // dryB (pitch) and visible at the boundary (EG-B, gated by the right gate).
    set_cable(st, core::JackId::keyboard_pressure_out, core::JackId::vco_b_v_oct_in);
    set_cable(st, core::JackId::keyboard_gate_right_out, core::JackId::envelope_b_gate_in);
    return st;
  };
  // Key events land MID-block (0, 130, 700, 1030, 1500) — never on a 256-frame boundary. The
  // left side takes a SECOND note at a lower pitch so the left control trace itself moves by a
  // whole interval (the left portamento is then carried across a block boundary too).
  const auto script = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.0, 2, 130);
    release(rt, core::KeyboardSide::Left, 1, 700);
    note(rt, core::KeyboardSide::Left, 0.5, 0.6, 1, 1030);
    note(rt, core::KeyboardSide::Right, 1.5, 0.0, 2, 1030);
    release(rt, core::KeyboardSide::Right, 2, 1500);
  };
  std::unique_ptr<core::MachineRuntimeDefinition> defRef = chain(build());
  if (defRef == nullptr) { check(false, "S build"); return; }
  core::SynthRuntime& ref = defRef->runtime();
  script(ref);
  std::vector<Snap> frames;
  std::vector<double> refL, refR, refA, refB;
  frames.reserve(static_cast<std::size_t>(kBlock * kBlocks));
  {
    std::vector<core::RuntimeInputs> in(1);
    std::vector<core::RuntimeOutput> o(1);
    for (int i = 0; i < kBlock * kBlocks; ++i) {
      ref.processBlock(in.data(), 1, o.data());
      refL.push_back(o[0].wetL);
      refR.push_back(o[0].wetR);
      refA.push_back(o[0].dryA);
      refB.push_back(o[0].dryB);
      frames.push_back(snap(ref));
    }
  }
  // (b) the SAME state + script through the REAL host entry, in real 256-frame blocks.
  testengine::EngineHarness h;
  if (!h.load(build(), kSr, kBlock)) { check(false, "S host load"); return; }
  script(*h.producerRuntime());
  std::vector<Snap> ends;
  for (int i = 0; i < kBlocks; ++i) {
    if (!h.renderBlock(kBlock, [](std::size_t, double& a, double& b) { a = 0.0; b = 0.0; })) {
      check(false, "S host block render");
      return;
    }
    ends.push_back(read_snap(*h.runtime()));  // published control values at this block END.
  }
  const std::vector<double>& blkL = h.out(0);
  const std::vector<double>& blkR = h.out(1);
  const std::vector<double>& blkA = h.out(2);
  const std::vector<double>& blkB = h.out(3);
  check(ends.size() == static_cast<std::size_t>(kBlocks) &&
            blkL.size() == static_cast<std::size_t>(kBlock * kBlocks),
        "S1 all 8 real 256-frame host blocks rendered");
  // The host channels are the same virtual volts through the ONE frozen device scale, so this is
  // an exact per-sample comparison of the product path against the per-frame reference.
  bool audioSame = blkL.size() == refL.size();
  for (std::size_t i = 0; audioSame && i < refL.size(); ++i) {
    audioSame = blkL[i] == core::device_normalized_from_volts(refL[i]) &&
                blkR[i] == core::device_normalized_from_volts(refR[i]) &&
                blkA[i] == core::device_normalized_from_volts(refA[i]) &&
                blkB[i] == core::device_normalized_from_volts(refB[i]);
  }
  check(audioSame,
        "S2 all FOUR host audio channels equal the per-frame reference through the device scale");
  bool boundary = ends.size() == static_cast<std::size_t>(kBlocks);
  for (int i = 0; i < kBlocks && boundary; ++i) {
    boundary = same_snap(ends[static_cast<std::size_t>(i)],
                         frames[static_cast<std::size_t>((i + 1) * kBlock - 1)]);
  }
  check(boundary,
        "S3 every real block boundary equals the per-frame reference (all 4 published values)");
  std::vector<double> leftPitch, rightGate, rightPitch, leftGate;
  leftPitch.reserve(frames.size());
  for (const Snap& s : frames) {
    leftPitch.push_back(s.vOct);
    rightGate.push_back(s.gateR);
    rightPitch.push_back(s.press);
    leftGate.push_back(s.gateL);
  }
  int firstRightGate = -1;
  for (std::size_t i = 0; i < rightGate.size(); ++i) {
    if (rightGate[i] > 1.0) { firstRightGate = static_cast<int>(i); break; }
  }
  // Non-vacuity: every compared quantity must actually move. The left pitch moves by its vibrato
  // (depth 1.0 = +-2/12V, observed span ~0.19V over this window) and by the second left note, the
  // right pitch by 2.0 -> 1.5 plus the release, both gates by the full 0..10V rail. A flat trace
  // would make "equal at every boundary" meaningless.
  check(range_of(leftPitch) > 0.1 && range_of(rightPitch) > 0.4 && range_of(leftGate) > 5.0 &&
            range_of(rightGate) > 5.0,
        "S4 the left/right pitch and gate traces all move (the comparison is not vacuous)");
  check(firstRightGate == 130,
        "S4 the mid-block right key event is carried at its exact sample (frame 130)");
}
// ============================================================ U. host event drain (F-1)

// U: FINDING F-1, fixed in this slice. The host render path
// (StandaloneAudioEngine::processBlock -> DeviceAdapter::renderBlock) used to call
// SynthRuntime::processFrame per frame and NEVER drain the EventTimebase, so events queued by
// the host's own keyboard/MIDI producer were invisible on the product entry: the adapter
// rendered a free-running machine. The authorized fix (@Codex msg 0fd75e9f) routes each frame
// through the EXISTING SynthRuntime::processBlock(&in, 1, &out, true) — ONE drain, one frame of
// time, the ONE EventTimebase, no second event loop, no dynamic buffer, no added latency, and
// no drain smuggled into processFrame (which would double-advance the time base).
//
// U pins that on the REAL host entry: an event acts at its exact ABSOLUTE frame; the host entry
// equals the canonical per-frame entry; 64/256/irregular HOST blocks agree; a re-commit resets
// the time base; and the per-frame drain does not change the EventTimebase's defined semantics
// (same-frame global order, future retention, late delivery, capacity boundaries).
static void accept_host_event_drain() {
  std::printf("U  -- the host render entry drains the EventTimebase at its exact absolute frames\n");
  const auto build = []() {
    core::DeviceStateV1 st = core::make_default_device_state(kSeed);
    set_mode(st, 2);  // Split: v_oct = left pitch, pressure_out = right pitch, both gates live
    return st;
  };
  const auto script = [](core::SynthRuntime& rt) {
    note(rt, core::KeyboardSide::Left, 1.0, 0.6, 1, 0);
    note(rt, core::KeyboardSide::Right, 2.0, 0.4, 2, 130);
    release(rt, core::KeyboardSide::Left, 1, 700);
    note(rt, core::KeyboardSide::Left, 0.5, 0.6, 1, 1030);
  };

  // U1: queued left/right note + pressure + release act at their EXACT absolute frames on the
  // real host entry, and the rendered audio is a function of those events (not free-running).
  {
    testengine::EngineHarness h, ctrl;
    if (!h.load(build()) || !ctrl.load(build())) { check(false, "U1 load"); return; }
    script(*h.producerRuntime());
    std::vector<Snap> f;
    f.reserve(1200);
    const bool ok = h.renderSampled(1200, 0.0, [&f](const core::SynthRuntime& rt) {
      f.push_back(read_snap(rt));
    }) && ctrl.render(1200);
    check(ok && f.size() == 1200, "U1 the host entry renders the sampled window");
    if (!ok || f.size() != 1200) return;
    check(f[0].gateL == kGateHigh && f[699].gateL == kGateHigh && f[700].gateL == 0.0,
          "U1 a left note at frame 0 and its release at frame 700 act at their EXACT frames");
    check(f[129].gateR == 0.0 && f[130].gateR == kGateHigh,
          "U1 the right note acts at exactly frame 130 (its absolute sample)");
    check(f[0].vOct > 0.0 && f[129].press == 0.0 && f[130].press > 0.0 &&
              near(f[1199].press, 2.0, 0.05),
          "U1 v_oct carries the left pitch and Split pressure_out the RIGHT pitch, from 130");
    check(range_of(h.out(2)) > 0.01 && range_of(h.out(3)) > 0.01 &&
              h.out(2) != ctrl.out(2) && h.out(3) != ctrl.out(3),
          "U1 the queued events change the four-channel host audio (not a free-running trace)");
  }

  // U2: the host entry and the canonical runtime per-frame entry are the SAME machine — same
  // published control trace and same audio (compared through the frozen 0.5 device scale).
  {
    std::unique_ptr<core::MachineRuntimeDefinition> def = chain(build());
    testengine::EngineHarness h;
    if (def == nullptr || !h.load(build())) { check(false, "U2 load"); return; }
    core::SynthRuntime& canon = def->runtime();
    script(canon);
    script(*h.producerRuntime());
    std::vector<Snap> canonF;
    std::vector<double> cL, cR, cA, cB;
    {
      std::vector<core::RuntimeInputs> in(1);
      std::vector<core::RuntimeOutput> o(1);
      for (int i = 0; i < 1200; ++i) {
        canon.processBlock(in.data(), 1, o.data());
        canonF.push_back(snap(canon));
        cL.push_back(o[0].wetL);
        cR.push_back(o[0].wetR);
        cA.push_back(o[0].dryA);
        cB.push_back(o[0].dryB);
      }
    }
    std::vector<Snap> hostF;
    const bool ok = h.renderSampled(1200, 0.0, [&hostF](const core::SynthRuntime& rt) {
      hostF.push_back(read_snap(rt));
    });
    bool sameControl = ok && hostF.size() == canonF.size();
    for (std::size_t i = 0; sameControl && i < canonF.size(); ++i)
      sameControl = same_snap(hostF[i], canonF[i]);
    check(sameControl, "U2 the host entry's published control trace equals the canonical entry");
    bool sameAudio = h.out(0).size() == cL.size() && range_of(cA) > 0.01;
    for (std::size_t i = 0; sameAudio && i < cL.size(); ++i) {
      sameAudio = h.out(0)[i] == core::device_normalized_from_volts(cL[i]) &&
                  h.out(1)[i] == core::device_normalized_from_volts(cR[i]) &&
                  h.out(2)[i] == core::device_normalized_from_volts(cA[i]) &&
                  h.out(3)[i] == core::device_normalized_from_volts(cB[i]);
    }
    check(sameAudio,
          "U2 all FOUR host audio channels equal the canonical render through the 0.5 scale");
  }

  // U3: 64/256/irregular HOST blocks are interchangeable, and every real block boundary equals
  // the per-frame reference — the per-frame drain must not become partition-dependent.
  {
    testengine::EngineHarness href;
    if (!href.load(build())) { check(false, "U3 reference load"); return; }
    script(*href.producerRuntime());
    std::vector<Snap> ref;
    ref.reserve(2560);
    if (!href.renderSampled(2560, 0.0, [&ref](const core::SynthRuntime& rt) {
          ref.push_back(read_snap(rt));
        })) {
      check(false, "U3 reference render");
      return;
    }
    struct Run {
      std::vector<double> ch[4];
      std::vector<Snap> ends;
      bool ok = false;
    };
    const auto run = [&](const std::vector<int>& sched) {
      Run r;
      testengine::EngineHarness h;
      if (!h.load(build())) return r;
      script(*h.producerRuntime());
      for (int n : sched) {
        if (!h.renderBlockSampled(n, 0.0, [&r](const core::SynthRuntime& rt) {
              r.ends.push_back(read_snap(rt));
            })) {
          return r;
        }
      }
      for (int c = 0; c < 4; ++c) r.ch[c] = h.out(c);
      r.ok = true;
      return r;
    };
    const Run r64 = run(std::vector<int>(40, 64));    // 2560 frames in 40 blocks
    const Run r256 = run(std::vector<int>(10, 256));  // the same 2560 frames in 10 blocks
    const int irSizes[] = {300, 100, 1024, 7, 501, 400, 100, 128};  // sums to exactly 2560
    const Run rIr = run(std::vector<int>(irSizes, irSizes + 8));
    check(r64.ok && r256.ok && rIr.ok, "U3 all three host partitions rendered");
    bool audioEq = r64.ok && r256.ok && rIr.ok;
    for (int c = 0; audioEq && c < 4; ++c)
      audioEq = r64.ch[c] == r256.ch[c] && r256.ch[c] == rIr.ch[c] && r64.ch[c] == href.out(c);
    check(audioEq, "U3 64/256/irregular host blocks are bit-identical to the per-frame render");
    // Every real boundary must equal the per-frame reference at the SAME absolute frame.
    bool b64 = true, b256 = true, bIr = true;
    {
      std::size_t cum = 0;
      for (std::size_t i = 0; i < r64.ends.size(); ++i) {
        cum += 64;
        b64 = b64 && same_snap(r64.ends[i], ref[cum - 1]);
      }
      cum = 0;
      for (std::size_t i = 0; i < r256.ends.size(); ++i) {
        cum += 256;
        b256 = b256 && same_snap(r256.ends[i], ref[cum - 1]);
      }
      cum = 0;
      for (std::size_t i = 0; i < rIr.ends.size() && i < 8; ++i) {
        cum += static_cast<std::size_t>(irSizes[i]);
        bIr = bIr && same_snap(rIr.ends[i], ref[cum - 1]);
      }
    }
    check(b64 && b256 && bIr,
          "U3 every host block boundary equals the per-frame reference (all four jacks)");
    bool gateMoves = false;
    for (const Snap& s : ref) {
      if (s.gateR > 1.0) { gateMoves = true; break; }
    }
    check(range_of(href.out(3)) > 0.01 && gateMoves && !ref.empty(),
          "U3 the compared host trace is live (audio moves and the right gate opens)");
  }

  // U4: a re-commit on the SAME owner installs a fresh runtime whose event time base starts at
  // 0 — the same script replayed is bit-identical (no double advance, no carried timebase) —
  // and a reset event on the host entry clears both sides.
  {
    testengine::EngineHarness h;
    if (!h.load(build())) { check(false, "U4 load"); return; }
    std::vector<Snap> first, second, tail;
    script(*h.producerRuntime());
    if (!h.renderSampled(2560, 0.0, [&first](const core::SynthRuntime& rt) {
          first.push_back(read_snap(rt));
        })) {
      check(false, "U4 first render");
      return;
    }
    const std::vector<double> audioA = h.out(3);
    if (!h.load(build())) { check(false, "U4 re-commit"); return; }  // same state, same owner
    script(*h.producerRuntime());
    if (!h.renderSampled(2560, 0.0, [&second](const core::SynthRuntime& rt) {
          second.push_back(read_snap(rt));
        })) {
      check(false, "U4 second render");
      return;
    }
    const std::vector<double> audioB(h.out(3).begin() + 2560, h.out(3).end());
    bool sameTrace = first.size() == second.size() && !first.empty();
    for (std::size_t i = 0; sameTrace && i < first.size(); ++i)
      sameTrace = same_snap(first[i], second[i]);
    check(sameTrace && audioA == audioB,
          "U4 a re-commit resets the event time base: the replayed script is bit-identical");
    reset_edge(*h.producerRuntime(), core::KeyboardSide::Left, 2560);  // default-Left reset
    if (!h.renderSampled(480, 0.0, [&tail](const core::SynthRuntime& rt) {
          tail.push_back(read_snap(rt));
        })) {
      check(false, "U4 reset render");
      return;
    }
    check(!tail.empty() && tail.back().gateL == 0.0 && tail.back().gateR == 0.0,
          "U4 a reset on the host entry clears BOTH sides (no stuck right note)");
  }

  // U5: the fix adds NO new scheduler — it drives the EXISTING EventTimebase once per frame, so
  // the defined semantics are identical under a 1-frame drain and under one whole block:
  // same-frame global ordering, future-event retention, late delivery at offset 0, the critical
  // overflow reconcile, and the capacity boundary.
  {
    const auto fill = [](core::EventTimebase& tb) {
      // Five due events at the SAME absolute sample 300, enqueued scrambled, plus one far-future.
      tb.enqueue(tev(core::ControlEventKind::gate_on, 1.0, 300, 3, 9));
      tb.enqueue(tev(core::ControlEventKind::pitch, 1.0, 300, 1, 7));
      tb.enqueue(tev(core::ControlEventKind::gate_off, 0.0, 300, 2, 5));
      tb.enqueue(tev(core::ControlEventKind::clock, 1.0, 500, 1, 1));
      tb.enqueue(tev(core::ControlEventKind::parameter, 0.5, 100, 1, 2,
                     core::ParameterId::drone_3_pitch));
      tb.enqueue(tev(core::ControlEventKind::pitch, 2.0, 5000, 1, 3));
    };
    core::EventTimebase perFrameTb, wholeTb;
    fill(perFrameTb);
    fill(wholeTb);
    std::vector<core::TimedControlEvent> perFrame, whole;
    core::TimedControlEvent buf[core::kEventDispatchCapacity];
    for (int f = 0; f < 600; ++f) {
      const std::uint32_t n = perFrameTb.processBlock(1, buf, core::kEventDispatchCapacity);
      for (std::uint32_t i = 0; i < n; ++i) perFrame.push_back(buf[i]);
    }
    {
      const std::uint32_t n = wholeTb.processBlock(600, buf, core::kEventDispatchCapacity);
      for (std::uint32_t i = 0; i < n; ++i) whole.push_back(buf[i]);
    }
    // Expected order = the same stable sort the scheduler's insert_sorted performs.
    std::vector<core::TimedControlEvent> expected;
    expected.push_back(tev(core::ControlEventKind::parameter, 0.5, 100, 1, 2,
                           core::ParameterId::drone_3_pitch));
    expected.push_back(tev(core::ControlEventKind::gate_off, 0.0, 300, 2, 5));
    expected.push_back(tev(core::ControlEventKind::pitch, 1.0, 300, 1, 7));
    expected.push_back(tev(core::ControlEventKind::clock, 1.0, 500, 1, 1));
    expected.push_back(tev(core::ControlEventKind::gate_on, 1.0, 300, 3, 9));
    std::stable_sort(expected.begin(), expected.end(),
                     [](const core::TimedControlEvent& a, const core::TimedControlEvent& b) {
                       return core::timed_event_before(a, b);
                     });
    const auto same_seq = [](const std::vector<core::TimedControlEvent>& a,
                             const std::vector<core::TimedControlEvent>& b) {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].sample != b[i].sample || a[i].event.kind != b[i].event.kind ||
            a[i].event.value != b[i].event.value ||
            a[i].event.parameter != b[i].event.parameter) {
          return false;
        }
      }
      return true;
    };
    check(perFrame.size() == 5 && same_seq(perFrame, expected),
          "U5 same-frame events are delivered in the defined global order (per-frame drain)");
    check(same_seq(whole, expected),
          "U5 one whole-block drain delivers the SAME sequence as the per-frame drain");
    check(perFrameTb.pending() == 1 && wholeTb.pending() == 1 && !perFrameTb.lateSeen() &&
              !wholeTb.lateSeen(),
          "U5 a future event is retained, never delivered early or late");
    // Late delivery: an event whose absolute sample is behind the block start goes out at 0.
    {
      core::EventTimebase tb;
      tb.processBlock(100, buf, core::kEventDispatchCapacity);  // blockStart_ -> 100
      tb.enqueue(tev(core::ControlEventKind::pitch, 1.0, 50));
      const std::uint32_t n = tb.processBlock(1, buf, core::kEventDispatchCapacity);
      check(n == 1 && buf[0].event.sampleOffset == 0 && tb.lateSeen() && tb.lateCount() == 1,
            "U5 a late event is delivered at offset 0 and counted (never silently dropped)");
    }
    // Continuous capacity: pitch never coalesces -> the 65th is refused and counted.
    {
      core::EventTimebase tb;
      for (int i = 0; i < 64; ++i) tb.enqueue(tev(core::ControlEventKind::pitch, 1.0, 1000 + i));
      const bool ok65 = tb.enqueue(tev(core::ControlEventKind::pitch, 1.0, 2000));
      check(!ok65 && tb.continuousOverflow() == 1 && tb.pending() == 64,
            "U5 the continuous lane's 65th non-coalescible event is refused and counted");
    }
    // Parameter capacity: the same ParameterId coalesces into the newest representative.
    {
      core::EventTimebase tb;
      for (int i = 0; i < 64; ++i)
        tb.enqueue(tev(core::ControlEventKind::parameter, 0.1 * i, 1000 + i, 1,
                       static_cast<std::uint64_t>(i), core::ParameterId::drone_3_pitch));
      const bool okC = tb.enqueue(tev(core::ControlEventKind::parameter, 9.0, 2000, 1, 99,
                                      core::ParameterId::drone_3_pitch));
      check(okC && tb.parameterCoalesced() == 64 && tb.pending() == 1,
            "U5 under pressure a parameter coalesces by ParameterId (no silent edge loss)");
    }
    // Critical capacity: an overflow raises ONE reconcile, which emits a reset first.
    {
      core::EventTimebase tb;
      for (int i = 0; i < 64; ++i)
        tb.enqueue(tev(core::ControlEventKind::gate_on, 1.0, 1000 + i));
      const bool ok65 = tb.enqueue(tev(core::ControlEventKind::gate_on, 1.0, 2000));
      const bool pendingReconcile = tb.reconcilePending() && tb.criticalOverflow() == 1 &&
                                    tb.pending() == 64;
      // The reconcile is emitted FIRST (offset 0) and deterministically clears the now-untrusted
      // critical batch, so this block delivers exactly that one reset.
      const std::uint32_t n = tb.processBlock(4096, buf, core::kEventDispatchCapacity);
      check(ok65 == false && pendingReconcile && n == 1 &&
                buf[0].event.kind == core::ControlEventKind::reset &&
                buf[0].event.sampleOffset == 0 && tb.reconcileCount() == 1 &&
                tb.criticalFlushed() == 64 && tb.pending() == 0,
            "U5 a critical overflow raises ONE reconcile reset first, never a silent drop");
    }
    // Dispatch capacity: events that do not fit stay PENDING and are counted, never dropped.
    {
      core::EventTimebase tb;
      for (int i = 0; i < 64; ++i) {
        tb.enqueue(tev(core::ControlEventKind::gate_on, 1.0, 10 + i));
        tb.enqueue(tev(core::ControlEventKind::pitch, 1.0, 10 + i));
      }
      const std::uint32_t n = tb.processBlock(100, buf, 65);
      check(n == 65 && tb.dispatchCapacity() == 63 && tb.pending() == 63,
            "U5 a full dispatch buffer leaves the remainder PENDING and counted (no drop)");
    }
  }
}

// ============================================================ T. preset payload round-trip

// T: preserved fields + the 4 keyboard presets. task#100 counted the preset payload as stored-only;
// this pins that a fully-populated keyboard payload survives the wire byte-for-byte, that the
// reserved shell bytes are preserved VERBATIM, and that the candidate build rewrites nothing.
static void accept_preset_roundtrip() {
  std::printf("T  -- preserved fields + the 4-preset keyboard payload round-trip\n");
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  set_mode(st, 2);
  set_pressure_out(st, 3);
  st.keyboardScaleEditor = core::kScaleIonian;
  st.keyboardScaleEditorR = core::kChromaticScaleMask;  // legal, distinct from the left mask
  for (std::size_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
    st.keyboardPlateTune[i] = 0.25f * static_cast<float>(i + 1);
    st.keyboardPlateTuneR[i] = -0.5f * static_cast<float>(i + 1);
  }
  for (std::size_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
    st.keyboardPushbutton[i] = 1.0f + static_cast<float>(i);
    st.keyboardPushbuttonR[i] = -1.0f - static_cast<float>(i);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    st.keyboardClockSelectors[i] = static_cast<std::uint8_t>(i + 1);
    st.keyboardClockSelectorsR[i] = static_cast<std::uint8_t>(4 - i);
  }
  for (std::size_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
    st.keyboardSeqCurrent.steps[i] = {static_cast<std::uint8_t>(i),
                                      static_cast<float>(0.1 * static_cast<double>(i)),
                                      static_cast<std::uint8_t>(i & 1u)};
    st.keyboardSeqCurrentR.steps[i] = {static_cast<std::uint8_t>(15 - i),
                                       static_cast<float>(-0.2 * static_cast<double>(i)),
                                       static_cast<std::uint8_t>((i + 1u) & 1u)};
  }
  // The four keyboard-owned presets: ids must stay 0..3 (preset_invalid otherwise), the composite
  // payloads are distinct per slot, and the 22 per-side scalar pairs are copied from THIS state's
  // live bank — legal by construction, since the same descriptor checks both.
  for (std::uint32_t k = 0; k < core::kDeviceKeyboardPresetCount; ++k) {
    core::KeyboardPreset& p = st.keyboardPresets[k];
    p.reserved[0] = static_cast<std::uint8_t>(0xA0u + k);
    p.reserved[1] = static_cast<std::uint8_t>(0xB0u + k);
    p.pressureBehaviour = static_cast<std::uint8_t>(k % 3u);
    p.pressureOutput = static_cast<std::uint8_t>(k % 5u);
    p.arpClock = static_cast<std::uint8_t>(k);
    p.arpRhythm = static_cast<std::uint8_t>(3u - k);
    p.seqClock = static_cast<std::uint8_t>(k);
    p.seqRhythm = static_cast<std::uint8_t>(k);
    p.quantiseScaleEditor = core::kScaleIonian;
    p.quantiseScaleEditorR = core::kChromaticScaleMask;
    for (std::size_t i = 0; i < core::kKeyboardSeqStepCount; ++i) {
      p.seqSteps.steps[i] = {static_cast<std::uint8_t>(i + k),
                             static_cast<float>(0.05 * static_cast<double>(i + 1)),
                             static_cast<std::uint8_t>((i + k) & 1u)};
      p.seqStepsR.steps[i] = {static_cast<std::uint8_t>(15u - i),
                              static_cast<float>(-0.05 * static_cast<double>(i + 1)),
                              static_cast<std::uint8_t>((i + k + 1u) & 1u)};
    }
    for (std::size_t i = 0; i < core::kKeyboardPlateTuneCount; ++i) {
      p.plateTune[i] = 0.1f * static_cast<float>(i + 1) + static_cast<float>(k);
      p.plateTuneR[i] = -0.1f * static_cast<float>(i + 1) - static_cast<float>(k);
    }
    for (std::size_t i = 0; i < core::kKeyboardPushbuttonCount; ++i) {
      p.pushbuttonValue[i] = 2.0f + static_cast<float>(i) + static_cast<float>(k);
      p.pushbuttonValueR[i] = -2.0f - static_cast<float>(i) - static_cast<float>(k);
    }
    core::save_live_side_bank(st, p);
  }
  const std::vector<std::uint8_t> wire = wire_of(st);
  check(!wire.empty(), "T1 the fully-populated keyboard payload encodes");
  core::DeviceStateV1 dec;
  check(core::decode_device_state(wire.data(), wire.size(), &dec), "T1 it decodes");
  check(wire_of(dec) == wire,
        "T1 encode->decode->encode is byte-identical (4 presets + both live banks)");
  check(dec.keyboardPresets[2].reserved[0] == 0xA2u &&
            dec.keyboardPresets[2].reserved[1] == 0xB2u && dec.keyboardPresets[3].id == 3u &&
            dec.keyboardPresets[3].reserved[1] == 0xB3u,
        "T2 the preset shell (id + the 2 preserved reserved bytes) survives per slot");
  check(dec.keyboardPresets[1].seqSteps.steps[5].gate == 0u &&
            near(dec.keyboardPresets[1].seqSteps.steps[5].value, 0.30, 1e-6) &&
            dec.keyboardPresets[1].plateTune[3] == st.keyboardPresets[1].plateTune[3] &&
            dec.keyboardPresets[1].pushbuttonValue[2] == st.keyboardPresets[1].pushbuttonValue[2],
        "T2 a preset's composite regions (seq steps / plate tune / pushbutton) round-trip");
  check(dec.keyboardScaleEditorR == core::kChromaticScaleMask &&
            dec.keyboardClockSelectors[2] == 3u && dec.keyboardClockSelectorsR[2] == 2u &&
            dec.keyboardSeqCurrent.steps[7].gate == 1u &&
            near(dec.keyboardSeqCurrentR.steps[3].value, -0.6, 1e-6),
        "T3 the live non-scalar keyboard fields round-trip (masks / selectors / both seq runs)");
  bool scalars = true;
  for (std::size_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    scalars = scalars && dec.keyboardScalarRight[i] == st.keyboardScalarRight[i];
  }
  check(scalars, "T3 the 22-scalar right bank round-trips exactly");
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(dec);
  if (def == nullptr) { check(false, "T4 build"); return; }
  check(wire_of(def->deviceState()) == wire,
        "T4 the candidate build preserves every preserved field byte-for-byte");
  check(def->runtime().keyboardMode() == core::KeyboardMode::Split,
        "T4 the restored side configuration still reads back after the full payload round-trip");
}

int main() {
  accept_single_compat();
  accept_translate_side_path();
  accept_twin();
  accept_split();
  accept_same_identity();
  accept_pressure_jack();
  accept_reset();
  accept_explicit_clock();
  accept_readback_19();
  accept_parsed_but_unconsumed();
  accept_portamento_family();
  accept_vibrato_family();
  accept_pressure_family();
  accept_quantiser_root();
  accept_default_equivalence();
  accept_block_invariance();
  accept_repeat_and_reject();
  accept_host_entry();
  accept_cable_consumption();
  accept_block_boundary_traces();
  accept_host_event_drain();
  accept_preset_roundtrip();

  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
