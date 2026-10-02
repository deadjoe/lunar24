// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/input_state_machine.h>

namespace lunar24::host {

// Audio-thread-owned CC64 state. MIDI channels are zero-based here.
class MidiSustain {
 public:
  void noteOn(unsigned channel, unsigned note) {
    if (channel < 16 && note < 128) held_[channel][note] = false;
  }
  bool deferNoteOff(unsigned channel, unsigned note) {
    if (channel >= 16 || note >= 128 || !on_[channel]) return false;
    held_[channel][note] = true;
    return true;
  }
  // release receives the same source/channel/note identity as a normal note-off.
  // The caller supplies its sequence number and sample offset.
  template <class Release>
  void pedal(unsigned channel, bool on, core::ControlSourceId source, Release release) {
    if (channel >= 16) return;
    const bool wasOn = on_[channel];
    on_[channel] = on;
    if (!wasOn || on) return;
    for (unsigned note = 0; note < 128; ++note) {
      if (!held_[channel][note]) continue;
      held_[channel][note] = false;
      core::PerformanceInput in{};
      in.kind = core::PerfInputKind::note_off;
      in.source = source;
      in.channel = static_cast<std::uint8_t>(channel);
      in.noteId = note + 1;
      release(in);
    }
  }

  // Clear pedal and deferred-note state. The notes a pedal was holding belong to the
  // audio stream that carried them; when that stream is replaced (device reopen /
  // RESET PANEL) its notes die with it, and a stale pedal-down would otherwise defer
  // every later note-off forever. Call at the stopped-stream boundary (OnReset) or on
  // the audio thread.
  void reset() {
    for (bool& on : on_) on = false;
    for (auto& row : held_) for (bool& h : row) h = false;
  }

 private:
  bool on_[16] = {};
  bool held_[16][128] = {};
};

}  // namespace lunar24::host
