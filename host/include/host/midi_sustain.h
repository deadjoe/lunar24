// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/input_state_machine.h>

namespace lunar24::host {

// Audio-thread-owned CC64 state, per lane (input device * 16 + zero-based MIDI channel; see
// midi_input_queue.h). One input device's lanes are 0-15, the channels as they always were.
inline constexpr unsigned kMidiSustainLanes = 64;
class MidiSustain {
 public:
  void noteOn(unsigned lane, unsigned note) {
    if (lane < kMidiSustainLanes && note < 128) held_[lane][note] = false;
  }
  bool deferNoteOff(unsigned lane, unsigned note) {
    if (lane >= kMidiSustainLanes || note >= 128 || !on_[lane]) return false;
    held_[lane][note] = true;
    return true;
  }
  // release receives the same source/channel/note identity as a normal note-off.
  // The caller supplies its sequence number and sample offset.
  template <class Release>
  void pedal(unsigned lane, bool on, core::ControlSourceId source, Release release) {
    if (lane >= kMidiSustainLanes) return;
    const bool wasOn = on_[lane];
    on_[lane] = on;
    if (!wasOn || on) return;
    for (unsigned note = 0; note < 128; ++note) {
      if (!held_[lane][note]) continue;
      held_[lane][note] = false;
      core::PerformanceInput in{};
      in.kind = core::PerfInputKind::note_off;
      in.source = source;
      in.channel = static_cast<std::uint8_t>(lane);
      in.noteId = note + 1;
      release(in);
    }
  }

  // Release every note a pedal is holding back (all lanes) and lift every pedal, as
  // if each pedal came up. `release` gets a note_off like pedal() sends.
  template <class Release>
  void releaseAll(core::ControlSourceId source, Release release) {
    for (unsigned lane = 0; lane < kMidiSustainLanes; ++lane) pedal(lane, true, source, release);
    for (unsigned lane = 0; lane < kMidiSustainLanes; ++lane) pedal(lane, false, source, release);
  }
  // The same for one input device's 16 lanes.
  template <class Release>
  void releaseDevice(unsigned device, core::ControlSourceId source, Release release) {
    if (device * 16u >= kMidiSustainLanes) return;
    for (unsigned ch = 0; ch < 16; ++ch) pedal(device * 16u + ch, true, source, release);
    for (unsigned ch = 0; ch < 16; ++ch) pedal(device * 16u + ch, false, source, release);
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
  bool on_[kMidiSustainLanes] = {};
  bool held_[kMidiSustainLanes][128] = {};
};

}  // namespace lunar24::host
