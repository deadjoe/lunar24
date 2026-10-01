// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Live edits from the UI thread: a 16-step sequencer step edit lands in the saved state
// and reaches the audio thread through the live queue; states saved before the step
// editor existed get their (never-edited) sequence gates opened on load. Plate pressure
// played live reaches a patched CV input on the PRESSURE jack's 0..8 V range.

#include <algorithm>
#include <cmath>
#include <vector>

#include "mini_test.h"
#include <host/standalone_audio_engine.h>
#include <lunar24/core/input_state_machine.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>

using namespace lunar24;

int main() {
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
  return test::finish("test_live_edits");
}
