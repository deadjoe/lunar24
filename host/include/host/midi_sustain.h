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

 private:
  bool on_[16] = {};
  bool held_[16][128] = {};
};

}  // namespace lunar24::host
