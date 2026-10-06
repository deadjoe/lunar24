// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Live edits from the UI thread: a 16-step sequencer step edit lands in the saved state
// and reaches the audio thread through the live queue; states saved before the step
// editor existed get their (never-edited) sequence gates opened on load. Plate pressure
// played live reaches a patched CV input on the PRESSURE jack's 0..8 V range.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "mini_test.h"
#include <host/standalone_audio_engine.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>
#include <lunar24/core/state_serializer.h>

using namespace lunar24;

static void scale_and_root_reach_the_quantiser() {
  // SCALE / ROOT reach the quantiser: picking a scale loads its notes into the scale
  // editor (saved state and audio thread alike), and a played note comes out on the scale.
  // 0 V = A3: C major from a C# (+4) plays C (+3, ties go down); D major plays the C#.
  {
    // Heap: the engine is ~170 KB, and Windows' 1 MB main-thread stack already holds the others.
    auto engine = std::make_unique<host::StandaloneAudioEngine>();
    host::StandaloneAudioEngine& e = *engine;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    std::vector<double> l(256), r(256);
    double* outs[2] = {l.data(), r.data()};
    core::InputStateMachine in{nullptr, 0};
    std::uint64_t seq = 0;
    const core::JackId vOct = core::find_jack_by_name("keyboard.v_oct_out")->id;
    auto play = [&](double semitones) {
      for (core::PerfInputKind kind : {core::PerfInputKind::note_on, core::PerfInputKind::note_off}) {
        core::PerformanceInput p{};
        p.kind = kind;
        p.pitch = static_cast<core::SignalSample>(semitones / 12.0);
        p.value = static_cast<core::SignalSample>(0.8);
        p.noteId = 9;
        p.source = 1;
        p.seq = ++seq;
        core::ControlEvent ev[3];
        const std::uint32_t n = in.translate(p, ev, 3);
        for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
        for (int b = 0; b < 4; ++b)
          CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
        if (kind == core::PerfInputKind::note_on) return e.runtime()->controlVoltageAt(vOct) * 12.0;
      }
      return 0.0;
    };
    CHECK(std::fabs(play(4.0) - 4.0) < 1e-3);  // factory SEMITONES: no change
    CHECK(e.postParameter(core::ParameterId::keyboard_quantise_load_scale, 1.0));  // IONIAN
    CHECK_EQ(e.canonicalState()->keyboardScaleEditor, core::kScaleIonian);
    CHECK(std::fabs(play(4.0) - 3.0) < 1e-3);
    CHECK(e.postParameter(core::ParameterId::keyboard_root_note, 2.0 / 11.0));  // ROOT D
    CHECK(std::fabs(play(4.0) - 4.0) < 1e-3);
    CHECK(std::fabs(play(3.0) - 2.0) < 1e-3);  // C -> B in D major
    CHECK(e.postParameter(core::ParameterId::keyboard_quantise_load_scale, 14.0));  // GAMELAN: not modelled
    CHECK_EQ(e.canonicalState()->keyboardScaleEditor, core::kMicrotonalScaleMask);
    CHECK(std::fabs(play(3.0) - 3.0) < 1e-3);
    // SPLIT: the right half's SCALE loads the right editor only.
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));
    CHECK(e.postKeyboardRightParameter(core::ParameterId::keyboard_quantise_load_scale, 18.0));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK_EQ(e.canonicalState()->keyboardScaleEditorR, core::kScaleWholeTone);
    CHECK_EQ(e.canonicalState()->keyboardScaleEditor, core::kMicrotonalScaleMask);
    CHECK_EQ(e.runtime()->keyboardBehaviourParams(core::KeyboardSide::Right).scaleEditor, core::kScaleWholeTone);
    CHECK_EQ(e.runtime()->keyboardBehaviourParams(core::KeyboardSide::Left).scaleEditor, core::kMicrotonalScaleMask);
  }
  // States saved before SCALE reached the quantiser: a chosen SCALE next to an empty editor
  // (live, right side and presets) gets its notes loaded; an empty editor for SEMITONES or a
  // not-modelled scale stays empty.
  {
    auto state = std::make_unique<core::DeviceStateV1>(core::make_default_device_state(1));
    core::DeviceStateV1& s = *state;
    s.parameters[static_cast<std::size_t>(core::ParameterId::keyboard_quantise_load_scale)] = 6.0;
    s.keyboardScalarRight[static_cast<std::size_t>(
        core::keyboard_scalar_index(core::ParameterId::keyboard_quantise_load_scale))] = 13.0;
    s.keyboardPresets[1].quantiseLoadScale = 10;
    s.keyboardPresets[2].quantiseLoadScale = 2;
    s.keyboardPresets[2].quantiseScaleEditor = 0x0001;  // already loaded: kept
    core::load_unloaded_scale_editors(s);
    CHECK_EQ(s.keyboardScaleEditor, core::kScaleAeolian);
    CHECK_EQ(s.keyboardScaleEditorR, core::kMicrotonalScaleMask);
    CHECK_EQ(s.keyboardPresets[0].quantiseScaleEditor, core::kMicrotonalScaleMask);
    CHECK_EQ(s.keyboardPresets[1].quantiseScaleEditor, core::kScalePentatonicMajor);
    CHECK_EQ(s.keyboardPresets[2].quantiseScaleEditor, 0x0001);
    CHECK(core::validate_device_state(s).ok);
  }
}

// The effector cartridge slot: picking a cartridge loads nothing; a side's 1-2-3 switch loads
// it into that side only, so L and R can run programs from different cartridges.
static void slot_cartridge_loads_one_side() {
  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  host::StandaloneAudioEngine& e = *engine;
  CHECK(e.prepare(1, 48000.0, 256, 0, 2));
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};
  auto cart = [&](int side) {
    const auto& fx = side == 0 ? e.canonicalState()->leftEffector : e.canonicalState()->rightEffector;
    return static_cast<int>(static_cast<std::uint32_t>(fx.program) / 3u);
  };
  const int start = cart(0);
  CHECK_EQ(e.slotCartridge(), start);          // the slot shows the left side's cartridge
  e.stepSlotCartridge(1);
  e.stepSlotCartridge(1);                      // e.g. CATHEDRAL -> MAGIC -> TIME
  CHECK_EQ(e.slotCartridge(), (start + 2) % 13);
  CHECK_EQ(cart(0), start);                    // nothing loaded yet
  CHECK_EQ(cart(1), start);
  CHECK(e.loadSlotCartridge(1));               // flip R
  CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
  CHECK_EQ(cart(1), (start + 2) % 13);
  CHECK_EQ(cart(0), start);
  CHECK_EQ(e.runtime()->effectorProgram(1) / 3, (start + 2) % 13);  // the audio side follows
  CHECK_EQ(e.runtime()->effectorProgram(0) / 3, start);
  const auto edits = e.editCount();
  CHECK(e.loadSlotCartridge(1));               // already loaded: no reload (no tail cut)
  CHECK_EQ(e.editCount(), edits);
  e.stepSlotCartridge(-1);
  CHECK_EQ(e.slotCartridge(), (start + 1) % 13);
  e.stepSlotCartridge(-1);
  e.stepSlotCartridge(-1);                     // wraps backwards
  CHECK_EQ(e.slotCartridge(), (start + 12) % 13);
}

// Per-plate tuning: a plate plays its own offset (saved in the state); a held plate retunes
// when its pitch is sent again; SPLIT keeps the right half's tuning in the right bank.
static void plate_tuning_reaches_v_oct() {
  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  host::StandaloneAudioEngine& e = *engine;
  CHECK(e.prepare(1, 48000.0, 256, 0, 2));
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};
  core::InputStateMachine in{nullptr, 0};
  const core::JackId vOct = core::find_jack_by_name("keyboard.v_oct_out")->id;
  auto send = [&](core::PerfInputKind kind, int plate) {
    core::PerformanceInput p{};
    p.kind = kind;
    p.pitch = static_cast<core::SignalSample>((plate - 9) / 12.0);  // A3 = 0 V, as the panel sends
    p.value = static_cast<core::SignalSample>(0.8);
    p.noteId = 7;
    p.source = 1;
    p.plate = static_cast<std::uint8_t>(plate);
    core::ControlEvent ev[3];
    const std::uint32_t n = in.translate(p, ev, 3);
    for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    for (int b = 0; b < 4; ++b)
      CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    return e.runtime()->controlVoltageAt(vOct) * 12.0;  // semitones from A3
  };
  CHECK(std::fabs(send(core::PerfInputKind::note_on, 4) - (-5.0)) < 1e-3);  // untuned E
  send(core::PerfInputKind::note_off, 4);
  CHECK(e.postKeyboardPlateTune(4, 0.5));
  CHECK(std::fabs(e.canonicalState()->keyboardPlateTune[4] - 0.5f) < 1e-6);
  CHECK(std::fabs(e.keyboardPlateTune(4) - 0.5) < 1e-6);
  CHECK(std::fabs(send(core::PerfInputKind::note_on, 4) - (-4.5)) < 1e-3);  // a quarter tone up
  // Held: a new tuning plus the plate's pitch sent again moves the sounding note.
  CHECK(e.postKeyboardPlateTune(4, -1.0));
  {
    core::ControlEvent ev{};
    ev.kind = core::ControlEventKind::pitch;
    ev.value = static_cast<core::SignalSample>((4 - 9) / 12.0);
    ev.source = 1;
    ev.noteId = 7;
    ev.plate = 4;
    CHECK(e.postEvent(ev));
    for (int b = 0; b < 4; ++b)
      CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(std::fabs(e.runtime()->controlVoltageAt(vOct) * 12.0 - (-6.0)) < 1e-3);
  }
  send(core::PerfInputKind::note_off, 4);
  CHECK(std::fabs(send(core::PerfInputKind::note_on, 5) - (-4.0)) < 1e-3);  // other plates untouched
  send(core::PerfInputKind::note_off, 5);
  CHECK(e.postKeyboardPlateTune(4, 99.0));  // clamped to the range
  CHECK(std::fabs(e.keyboardPlateTune(4) - 24.0) < 1e-6);
  // SPLIT: the right half (plates 6-11) has its own tuning, saved in the right bank.
  CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));
  CHECK(e.postKeyboardPlateTune(8, 0.25));
  CHECK(std::fabs(e.canonicalState()->keyboardPlateTuneR[8] - 0.25f) < 1e-6);
  CHECK(e.canonicalState()->keyboardPlateTune[8] == 0.0f);
  CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
  CHECK(std::fabs(e.runtime()->keyboardPlateTune(core::KeyboardSide::Right, 8) - 0.25) < 1e-6);
  CHECK(e.runtime()->keyboardPlateTune(core::KeyboardSide::Left, 8) == 0.0);
}

// SCALE EDITOR: switching single notes of the quantiser scale reaches the quantiser; the last
// note off picks SEMITONES (notes pass through, also after a restart); SPLIT keeps the right one.
static void scale_editor_switches_notes() {
  auto engine = std::make_unique<host::StandaloneAudioEngine>();
  host::StandaloneAudioEngine& e = *engine;
  CHECK(e.prepare(1, 48000.0, 256, 0, 2));
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};
  core::InputStateMachine in{nullptr, 0};
  const core::JackId vOct = core::find_jack_by_name("keyboard.v_oct_out")->id;
  auto play = [&](double semitones) {  // semitones from A3; returns the V/OCT out, same unit
    double out = 0.0;
    for (core::PerfInputKind kind : {core::PerfInputKind::note_on, core::PerfInputKind::note_off}) {
      core::PerformanceInput p{};
      p.kind = kind;
      p.pitch = static_cast<core::SignalSample>(semitones / 12.0);
      p.value = static_cast<core::SignalSample>(0.8);
      p.noteId = 5;
      p.source = 1;
      core::ControlEvent ev[3];
      const std::uint32_t n = in.translate(p, ev, 3);
      for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
      for (int b = 0; b < 4; ++b)
        CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
      if (kind == core::PerfInputKind::note_on) out = e.runtime()->controlVoltageAt(vOct) * 12.0;
    }
    return out;
  };
  CHECK(e.postParameter(core::ParameterId::keyboard_quantise_load_scale, 1.0));  // IONIAN, ROOT C
  CHECK_EQ(e.keyboardScaleEditor(0), core::kScaleIonian);
  CHECK(std::fabs(play(-5.0) - (-5.0)) < 1e-3);  // E is in C major
  // Switch E (4 above C) off: E snaps to the nearest scale note left, F (D is 2 below, F 1 above).
  CHECK(e.postKeyboardScaleEditor(0, static_cast<std::uint16_t>(core::kScaleIonian & ~(1u << 4))));
  CHECK_EQ(e.canonicalState()->keyboardScaleEditor, std::uint16_t(core::kScaleIonian & ~(1u << 4)));
  CHECK(std::fabs(play(-5.0) - (-4.0)) < 1e-3);  // E -> F
  // Add C# (1 above C): it plays as itself now.
  CHECK(std::fabs(play(-8.0) - (-9.0)) < 1e-3);  // C# -> C before
  CHECK(e.postKeyboardScaleEditor(0, static_cast<std::uint16_t>(e.keyboardScaleEditor(0) | (1u << 1))));
  CHECK(std::fabs(play(-8.0) - (-8.0)) < 1e-3);
  // The last note off: SEMITONES, an empty editor, everything passes through.
  CHECK(e.postKeyboardScaleEditor(0, 0));
  CHECK_EQ(e.keyboardScaleEditor(0), std::uint16_t(0));
  CHECK(e.parameterValue(core::ParameterId::keyboard_quantise_load_scale) == 0.0);
  CHECK(std::fabs(play(-5.0) - (-5.0)) < 1e-3);
  // SPLIT: the right half's editor is its own.
  CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));
  CHECK(e.postKeyboardScaleEditor(1, core::kScaleIonian));
  CHECK_EQ(e.canonicalState()->keyboardScaleEditorR, core::kScaleIonian);
  CHECK_EQ(e.canonicalState()->keyboardScaleEditor, std::uint16_t(0));
}

int main() {
  slot_cartridge_loads_one_side();
  scale_editor_switches_notes();
  plate_tuning_reaches_v_oct();
  host::StandaloneAudioEngine engine;
  CHECK(engine.prepare(1, 48000.0, 256, 0, 2));

  // The factory sequence plays: every gate on.
  const core::DeviceStateV1* st = engine.canonicalState();
  CHECK(st != nullptr);
  for (const auto& step : st->keyboardSeqCurrent.steps) CHECK_EQ(int(step.gate), 1);

  CHECK(engine.postSeqStep(0, 3, 7, false));
  CHECK(engine.postSeqStep(1, 15, 99, true));   // note clamped to the top of the range
  CHECK(!engine.postSeqStep(0, 16, 0, true));   // no 17th step
  CHECK_EQ(int(st->keyboardSeqCurrent.steps[3].note), 7);
  CHECK_EQ(int(st->keyboardSeqCurrent.steps[3].gate), 0);
  CHECK_EQ(int(st->keyboardSeqCurrentR.steps[15].note), host::StandaloneAudioEngine::kSeqStepMaxNote);

  // The audio thread drains the queue without trouble.
  std::vector<double> l(256), r(256);
  double* outs[2] = {l.data(), r.data()};
  CHECK(engine.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);

  // A full UI queue rejects edits without changing the state that will be saved.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    const auto src = core::find_jack_by_name("keyboard.pressure_out")->id;
    const auto sink = core::find_jack_by_name("vcf.cv_l_in")->id;
    CHECK(e.postConnect(src, sink));
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 1.0));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    auto saved = [&]() {
      std::vector<std::uint8_t> bytes(core::kDeviceStorageSchema.totalBytesHint);
      std::size_t written = 0;
      CHECK(core::encode_device_state(*e.canonicalState(), bytes.data(), bytes.size(), &written));
      CHECK_EQ(written, bytes.size());
      return bytes;
    };
    const auto before = saved();
    // One harmless command per slot, no render until the rejection checks finish.
    for (int i = 0; i < 1024; ++i) CHECK(e.postDroneKey(0, true));
    const auto edits = e.editCount();
    auto unchanged = [&]() {
      CHECK(saved() == before);
      CHECK_EQ(e.editCount(), edits);
      CHECK(e.droneKey(0));
    };
    CHECK(!e.postParameter(core::ParameterId::keyboard_pressure_output, 3.0)); unchanged();
    CHECK(!e.postConnect(src, core::find_jack_by_name("vcf.cv_r_in")->id)); unchanged();
    CHECK(!e.postDisconnect(sink)); unchanged();
    CHECK(!e.postEffectorProgram(0, core::ProgramId::program_cathedral_2)); unchanged();
    CHECK(!e.postDroneKey(0, false)); unchanged();
    CHECK(!e.postSeqStep(1, 3, 7, false)); unchanged();
    CHECK(!e.postKeyboardRightParameter(core::ParameterId::keyboard_mode, 2.0)); unchanged();
    CHECK(!e.postKeyboardRhythm(1, true, 0x05)); unchanged();
    CHECK(!e.postKeyboardPreset(E::PresetAction::Save, 1)); unchanged();
    CHECK(!e.postKeyboardPreset(E::PresetAction::Load, 1)); unchanged();
    CHECK(!e.postKeyboardPreset(E::PresetAction::Initialise, 1)); unchanged();
    // Draining frees capacity; the same edits are accepted and reach the right bank.
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));
    CHECK(e.postKeyboardRightParameter(core::ParameterId::keyboard_mode, 2.0));
    CHECK(e.postDisconnect(sink));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Arpeggiator);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Right) == core::ArpSeqMode::Sequencer);
    CHECK(e.canonicalState()->inputCable[static_cast<std::size_t>(sink)] == 0);
  }
  // Whole-state replacement must not replay commands from the previous runtime.
  // Queue the edits without rendering: this makes the reset race deterministic.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 1.0));
    CHECK(e.parameterFromAudioThread(core::ParameterId::effector_master, 0.23));
    const auto defaults = core::make_default_device_state(1);
    CHECK(e.applyDeviceState(defaults, 48000.0, 256, 0, 2) == E::StateApplyStatus::Accepted);
    CHECK_EQ(e.syncParametersFromAudioThread(), 0);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Keyboard);
    CHECK(e.parameterValue(core::ParameterId::effector_master) ==
          defaults.parameters[static_cast<std::size_t>(core::ParameterId::effector_master)]);
    // Commands posted after replacement still work.
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 2.0));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Sequencer);
  }
  // Reopening preserves the latest UI and MIDI values in the captured state;
  // pending notes belong to the old stream and must not start in the new one.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 0.0));
    CHECK(e.parameterFromAudioThread(core::ParameterId::effector_master, 0.23));
    core::InputStateMachine input{nullptr, 0};
    core::PerformanceInput note{};
    note.kind = core::PerfInputKind::note_on;
    note.value = 0.8f;
    note.noteId = 7;
    note.source = 1;
    note.seq = 1;
    core::ControlEvent events[3];
    const auto count = input.translate(note, events, 3);
    CHECK(count > 0);
    for (std::uint32_t i = 0; i < count; ++i) CHECK(e.postEvent(events[i]));
    CHECK_EQ(e.syncParametersFromAudioThread(), 1);
    const auto captured = *e.canonicalState();
    CHECK(e.prepare(1, 44100.0, 256, 0, 2));
    CHECK(e.applyDeviceState(captured, 44100.0, 256, 0, 2) == E::StateApplyStatus::Accepted);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.parameterValue(core::ParameterId::effector_master) == 0.23);
    const auto gate = core::find_jack_by_name("keyboard.gate_left_main_out")->id;
    CHECK(e.runtime()->controlVoltageAt(gate) < 0.5);
  }
  // Rejected state replacement leaves the active runtime AND pending edits intact.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 1.0));
    CHECK(e.parameterFromAudioThread(core::ParameterId::effector_master, 0.23));
    CHECK(e.applyDeviceState(core::make_default_device_state(1), 0.0, 256, 0, 2) ==
          E::StateApplyStatus::RejectedFormat);
    CHECK_EQ(e.syncParametersFromAudioThread(), 1);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Arpeggiator);
    CHECK(e.parameterValue(core::ParameterId::effector_master) == 0.23);
  }
  // audioSyncCount counts every record a sync applies (the panel redraws on a change).
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.audioSyncCount() == 0);
    CHECK(e.parameterFromAudioThread(core::ParameterId::effector_master, 0.4));
    CHECK(e.parameterFromAudioThread(core::ParameterId::effector_blend, 0.6));
    CHECK_EQ(e.syncParametersFromAudioThread(), 2);
    CHECK(e.audioSyncCount() == 2);
    CHECK_EQ(e.syncParametersFromAudioThread(), 0);
    CHECK(e.audioSyncCount() == 2);
    CHECK(e.parameterValue(core::ParameterId::effector_master) == 0.4);
    CHECK(e.parameterValue(core::ParameterId::effector_blend) == 0.6);
  }
  // An all-zero (never edited) saved sequence gets the factory gates; an edited one is kept.
  core::DeviceStateV1 old = core::make_default_device_state(1);
  old.keyboardSeqCurrent = core::KeyboardSeq{};
  old.keyboardSeqCurrentR = core::KeyboardSeq{};
  old.keyboardSeqCurrentR.steps[2].note = 5;
  core::open_untouched_seq_gates(old);
  CHECK_EQ(int(old.keyboardSeqCurrent.steps[0].gate), 1);
  CHECK_EQ(int(old.keyboardSeqCurrentR.steps[0].gate), 0);
  CHECK_EQ(int(old.keyboardSeqCurrentR.steps[2].note), 5);
  // Plate pressure (mouse position on a plate) -> PRESSURE out -> VCF CV L: 0.1 -> 0.8 V,
  // full pressure -> 8 V (manual p.13).
  {
    core::DeviceStateV1 ps = core::make_default_device_state(1);
    CHECK(core::state_connect(ps, core::find_jack_by_name("keyboard.pressure_out")->id,
                              core::find_jack_by_name("vcf.cv_l_in")->id));
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.applyDeviceState(ps, 48000.0, 256, 0, 2) == host::StandaloneAudioEngine::StateApplyStatus::Accepted);
    core::InputStateMachine in{nullptr, 0};
    std::uint64_t seq = 0;
    auto send = [&](core::PerfInputKind kind, double value) {
      core::PerformanceInput p{};
      p.kind = kind;
      p.value = static_cast<core::SignalSample>(value);
      p.noteId = 1000;
      p.source = 1;
      p.seq = ++seq;
      core::ControlEvent ev[3];
      const std::uint32_t n = in.translate(p, ev, 3);
      for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    };
    send(core::PerfInputKind::note_on, 0.1);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(std::fabs(e.runtime()->vcfCvReadbackL() - 0.8) < 1e-6);
    send(core::PerfInputKind::aftertouch, 1.0);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(std::fabs(e.runtime()->vcfCvReadbackL() - 8.0) < 1e-6);
  }
  // Switching MODE (keyboard -> arpeggiator) while a note is held releases it: the release
  // that follows goes to the arpeggiator, which never saw the note, so without the switch
  // releasing it the gate stayed high forever (a droning stuck note).
  {
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::InputStateMachine in{nullptr, 0};
    std::uint64_t seq = 0;
    auto send = [&](core::PerfInputKind kind) {
      core::PerformanceInput p{};
      p.kind = kind;
      p.value = static_cast<core::SignalSample>(0.8);
      p.noteId = 7;
      p.source = 1;
      p.seq = ++seq;
      core::ControlEvent ev[3];
      const std::uint32_t n = in.translate(p, ev, 3);
      for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    };
    const core::JackId gate = core::find_jack_by_name("keyboard.gate_left_main_out")->id;
    send(core::PerfInputKind::note_on);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.runtime()->controlVoltageAt(gate) > 1.0);   // the held note opens the gate
    CHECK(e.postParameter(core::ParameterId::keyboard_mode, 1.0));  // -> arpeggiator
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    send(core::PerfInputKind::note_off);
    for (int i = 0; i < 4; ++i)
      CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.runtime()->controlVoltageAt(gate) < 0.5);   // no stuck note
  }
  // PLAY = SPLIT: the right half has its own settings. Setting the right side's MODE to
  // arpeggiator changes only the right side; the left side's MODE stays keyboard.
  {
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));            // SPLIT
    CHECK(e.postKeyboardRightParameter(core::ParameterId::keyboard_mode, 1.0));    // right: arp
    CHECK(!e.postKeyboardRightParameter(core::ParameterId::keyboard_behaviour, 0.0));  // not per-side
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    const core::DeviceStateV1* s2 = e.canonicalState();
    const auto mi = static_cast<std::size_t>(core::keyboard_scalar_index(core::ParameterId::keyboard_mode));
    CHECK(s2->keyboardScalarRight[mi] == 1.0);
    CHECK(s2->parameters[static_cast<std::size_t>(core::ParameterId::keyboard_mode)] == 0.0);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Right) == core::ArpSeqMode::Arpeggiator);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Keyboard);
  }
  // Keyboard presets, live: save the SPLIT + right-arp setup into B, go back to SINGLE keyboard,
  // load B (the setup returns, sound keeps running), then INIT B and load it (factory settings).
  {
    using Action = host::StandaloneAudioEngine::PresetAction;
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));
    CHECK(e.postKeyboardRightParameter(core::ParameterId::keyboard_mode, 1.0));
    CHECK(e.postKeyboardPreset(Action::Save, 1u));
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 0.0));
    CHECK(e.postKeyboardRightParameter(core::ParameterId::keyboard_mode, 0.0));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Right) == core::ArpSeqMode::Keyboard);
    CHECK(e.postKeyboardPreset(Action::Load, 1u));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.parameterValue(core::ParameterId::keyboard_behaviour) == 2.0);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Right) == core::ArpSeqMode::Arpeggiator);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Left) == core::ArpSeqMode::Keyboard);
    CHECK(!e.postKeyboardPreset(Action::Load, 4u));  // there is no preset E
    CHECK(e.postKeyboardPreset(Action::Initialise, 1u));
    CHECK(e.postKeyboardPreset(Action::Load, 1u));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.parameterValue(core::ParameterId::keyboard_behaviour) == 0.0);
    CHECK(e.runtime()->keyboardArpSeqMode(core::KeyboardSide::Right) == core::ArpSeqMode::Keyboard);
  }
  // RHYTHM patterns, live and per side: the left arp pattern and the right seq pattern
  // reach the running arp/sequencers and the saved state.
  {
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.postKeyboardRhythm(0, false, 0x05));
    CHECK(e.postKeyboardRhythm(1, true, 0x80));
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.keyboardRhythm(0, false) == 0x05 && e.keyboardRhythm(1, true) == 0x80);
    CHECK(e.runtime()->keyboardArpSeqParams(core::KeyboardSide::Left).arpRhythm == 0x05);
    CHECK(e.postParameter(core::ParameterId::keyboard_behaviour, 2.0));  // SPLIT: right reads its own bank
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
    CHECK(e.runtime()->keyboardArpSeqParams(core::KeyboardSide::Right).seqRhythm == 0x80);
  }
  // MUTE silences every output with a short fade and brings the sound back when released; the
  // machine keeps running underneath.
  {
    host::StandaloneAudioEngine e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    auto peak = [&]() {
      double p = 0.0;
      CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == host::StandaloneAudioEngine::Status::Rendered);
      for (int i = 0; i < 256; ++i) p = std::max({p, std::fabs(l[i]), std::fabs(r[i])});
      return p;
    };
    for (int i = 0; i < 40; ++i) peak();          // the default drones are sounding
    CHECK(peak() > 0.01);
    CHECK(!e.muted());
    e.setMuted(true);
    CHECK(e.muted());
    const double during = peak();                 // the first block fades (10 ms > 256 samples)
    CHECK(during > 0.0);
    for (int i = 0; i < 4; ++i) peak();
    CHECK(peak() == 0.0);                         // fully silent once the fade is done
    e.setMuted(false);
    for (int i = 0; i < 4; ++i) peak();
    CHECK(peak() > 0.01);                         // the sound is back
  }
  // Panel LEDs follow the machine: the open drones are lit, envelope A lights while a note is
  // held, the LFO LED moves, and exactly one 5-step LED is lit once the sequencer runs.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    auto run = [&](int blocks) {
      for (int i = 0; i < blocks; ++i)
        CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    };
    run(20);
    for (int v = 0; v < 6; ++v) CHECK(e.panelLed(E::kLedDrone1 + v) > 0.9f);  // default: all open
    CHECK(e.panelLed(E::kLedEnvA) < 0.01f);
    core::InputStateMachine in{nullptr, 0};
    core::PerformanceInput p{};
    p.kind = core::PerfInputKind::note_on;
    p.value = 0.8f;
    p.noteId = 3;
    p.source = 1;
    p.seq = 1;
    core::ControlEvent ev[3];
    const std::uint32_t n = in.translate(p, ev, 3);
    for (std::uint32_t i = 0; i < n; ++i) e.postEvent(ev[i]);
    run(20);
    CHECK(e.panelLed(E::kLedEnvA) > 0.5f);
    float lo = 1.f, hi = 0.f;
    for (int i = 0; i < 200; ++i) {   // about one LFO cycle at the default 1 Hz
      run(1);
      lo = std::min(lo, e.panelLed(E::kLedLfoA));
      hi = std::max(hi, e.panelLed(E::kLedLfoA));
    }
    CHECK(hi - lo > 0.5f);
    int lit = 0;
    for (int i = 0; i < 5; ++i) lit += e.panelLed(E::kLedStep1 + i) > 0.5f ? 1 : 0;
    CHECK_EQ(lit, 1);
  }
  // The event scheduler's pressure diagnostics reach the engine surface: flooding the
  // 64-deep critical lane counts the overflows and fires the reconcile failsafe.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    CHECK(e.eventDiagnostics().criticalOverflow == 0u);
    core::ControlEvent g{};
    g.kind = core::ControlEventKind::gate_on;
    g.value = 1;
    g.source = 3;
    for (int i = 0; i < 70; ++i) {   // 70 > the 64-deep critical lane
      g.noteId = static_cast<core::NoteId>(i + 1);
      g.producerSequence = static_cast<std::uint64_t>(i + 1);
      e.enqueueEventFromAudioThread(g, 0);
    }
    CHECK_EQ(e.eventDiagnostics().criticalOverflow, 6u);
    CHECK(e.eventDiagnostics().reconcilePending);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK_EQ(e.eventDiagnostics().reconcileCount, 1u);
    CHECK_EQ(e.eventDiagnostics().criticalFlushed, 64u);
  }
  // A note-off lost to a full UI queue reconciles to all-gates-off instead of hanging:
  // the backlog's note events are dropped and the held gate falls.
  {
    using E = host::StandaloneAudioEngine;
    E e;
    CHECK(e.prepare(1, 48000.0, 256, 0, 2));
    core::InputStateMachine in{nullptr, 0};
    std::uint64_t seq = 0;
    auto send = [&](core::PerfInputKind kind) {
      core::PerformanceInput p{};
      p.kind = kind;
      p.value = 0.8f;
      p.noteId = 7;
      p.source = 1;
      p.seq = ++seq;
      core::ControlEvent ev[3];
      const std::uint32_t n = in.translate(p, ev, 3);
      for (std::uint32_t i = 0; i < n; ++i) CHECK(e.postEvent(ev[i]));
    };
    const core::JackId gate = core::find_jack_by_name("keyboard.gate_left_main_out")->id;
    send(core::PerfInputKind::note_on);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->controlVoltageAt(gate) > 1.0);   // the held note opens the gate
    // Fill the UI queue to capacity with harmless clock events; the first failing
    // push already raises the reconcile request.
    core::ControlEvent clk{};
    clk.kind = core::ControlEventKind::clock;
    clk.value = 1;
    clk.source = 100;
    while (e.postEvent(clk)) {}
    CHECK_EQ(e.liveEventDropCount(), 1u);
    // The note-off finds no room: dropped and counted, never delivered.
    core::PerformanceInput off{};
    off.kind = core::PerfInputKind::note_off;
    off.noteId = 7;
    off.source = 1;
    off.seq = ++seq;
    core::ControlEvent offEv[3];
    const std::uint32_t offN = in.translate(off, offEv, 3);
    for (std::uint32_t i = 0; i < offN; ++i) CHECK(!e.postEvent(offEv[i]));
    CHECK_EQ(e.liveEventDropCount(), 1u + offN);
    // The next drain drops the untrusted backlog and reconciles: the held gate falls.
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->controlVoltageAt(gate) < 0.5);
    // The queue works again afterwards.
    send(core::PerfInputKind::note_on);
    CHECK(e.processBlock(nullptr, outs, 0, 2, 256) == E::Status::Rendered);
    CHECK(e.runtime()->controlVoltageAt(gate) > 1.0);
  }
  scale_and_root_reach_the_quantiser();
  return test::finish("test_live_edits");
}
