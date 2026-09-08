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
//
// EXIT CODE. 0 = all criteria green; non-zero = any criterion red.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <lunar24/core/device_state.h>
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

namespace core = lunar24::core;

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
  std::unique_ptr<core::MachineRuntimeDefinition> def = chain(st);
  if (def == nullptr) { check(false, "H build"); return; }
  core::SynthRuntime& rt = def->runtime();
  const core::ArpSeqParams& a = rt.keyboardArpSeqParams(core::KeyboardSide::Left);
  const core::KeyboardBehaviourParams& b = rt.keyboardBehaviourParams(core::KeyboardSide::Left);

  check(a.mode == 2, "H01 keyboard.mode (101) read back");
  check(a.arpHold == 1, "H02 keyboard.arp_hold (102) read back");
  check(a.arpDirection == 3, "H03 keyboard.arp_direction (104) read back");
  check(a.arpVariation == 2, "H04 keyboard.arp_variation (105) read back");
  check(near(a.arpInterval, 0.5, 1e-9), "H05 keyboard.arp_interval (106) read back");
  check(a.seqRun == 1, "H06 keyboard.seq_run (109) read back");
  check(near(a.seqLength, 0.5, 1e-9), "H07 keyboard.seq_length (110) read back");
  check(a.seqDirection == 2, "H08 keyboard.seq_direction (112) read back");
  check(a.seqCvOutput == 1, "H09 keyboard.seq_cv_output (113) read back");
  check(near(b.portamentoSpeed, 0.5, 1e-9), "H10 keyboard.portamento_speed (117) read back");
  check(b.portamentoLegato == 1, "H11 keyboard.portamento_legato (118) read back");
  check(near(b.vibratoSpeed, 0.25, 1e-9), "H12 keyboard.vibrato_speed (119) read back");
  check(near(b.vibratoDepth, 0.5, 1e-9), "H13 keyboard.vibrato_depth (120) read back");
  check(near(b.vibratoDelay, 0.75, 1e-9), "H14 keyboard.vibrato_delay (121) read back");
  check(near(b.vibratoPressure, 1.0, 1e-9), "H15 keyboard.vibrato_pressure (122) read back");
  check(b.pressureOutput == 3, "H16 keyboard.pressure_output (123) read back");
  check(near(b.pressureRise, 0.5, 1e-9), "H17 keyboard.pressure_rise (124) read back");
  check(near(b.pressureFall, 0.25, 1e-9), "H18 keyboard.pressure_fall (125) read back");
  check(near(b.rootNote, 0.5, 1e-9), "H19 keyboard.root_note (128) read back");
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
  core::DeviceStateV1 st = core::make_default_device_state(kSeed);
  set_mode(st, 2);
  set_left(st, core::ParameterId::keyboard_portamento_speed, 0.7);
  set_right(st, core::ParameterId::keyboard_arp_direction, 2.0);
  std::unique_ptr<core::MachineRuntimeDefinition> d1 = chain(st);
  std::unique_ptr<core::MachineRuntimeDefinition> d2 = chain(st);
  if (d1 == nullptr || d2 == nullptr) { check(false, "O build"); return; }
  note(d1->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
  note(d2->runtime(), core::KeyboardSide::Left, 1.0, 0.5, 1, 0);
  std::vector<double> a, b;
  render(d1->runtime(), 4800, &a);
  render(d2->runtime(), 4800, &b);
  check(a == b && snap(d1->runtime()).vOct == snap(d2->runtime()).vOct,
        "O1 the same state restored twice is bit-identical");

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

  std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
