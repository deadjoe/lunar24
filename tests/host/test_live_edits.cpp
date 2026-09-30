// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Live edits from the UI thread: a 16-step sequencer step edit lands in the saved state
// and reaches the audio thread through the live queue; states saved before the step
// editor existed get their (never-edited) sequence gates opened on load.

#include <vector>

#include "mini_test.h"
#include <host/standalone_audio_engine.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/state_default.h>

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
  return test::finish("test_live_edits");
}
