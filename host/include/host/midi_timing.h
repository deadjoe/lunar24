// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// midi_timing.h — sample-accurate placement of live MIDI.
//
// MIDI arrives on the MIDI driver's thread at any moment, but the audio callback renders a
// whole block at once. Placing every message at the start of the next block makes note
// timing jitter by up to one buffer (~11 ms at 512 frames / 48 kHz). Instead the driver
// callback stamps each message with its arrival time, and the audio callback places it
// the same distance before the END of the block as it arrived before now: a constant one-block
// latency with no jitter, which keeps arpeggios and clocked patterns even.

#pragma once

#include <chrono>
#include <cstdint>

namespace lunar24::host {

// Arrival time in microseconds, wrapped to 30 bits (fits IMidiMsg::mOffset; wraps every ~18 min,
// handled by the modular difference below).
inline int midiArrivalStamp() {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  return static_cast<int>(static_cast<std::uint64_t>(us) & 0x3FFFFFFFu);
}

// Sample offset inside a block of `frames` for a message stamped `stamp` (see above).
inline int midiBlockOffset(int stamp, int now, double sampleRate, int frames) {
  if (frames <= 1 || sampleRate <= 0.0) return 0;
  const std::uint32_t ageUs = (static_cast<std::uint32_t>(now) - static_cast<std::uint32_t>(stamp)) & 0x3FFFFFFFu;
  const double ageFrames = ageUs * 1e-6 * sampleRate;
  if (ageFrames >= frames) return 0;               // older than a block: as early as possible
  const int offset = frames - 1 - static_cast<int>(ageFrames);
  return offset < 0 ? 0 : offset;
}

// MIDI clock and transport (system real-time) for the keyboard arpeggiator / sequencer.
// 24 clocks per quarter note: every 6th one is a 16th-note step, like the internal clock.
// START goes back to the first step (like the RESET jack: held notes stay); CONTINUE carries
// on; STOP needs nothing: the steps stop with the clock, and the last step's gate closes
// half a pulse later on its own. No transport message ever stops held notes.
class MidiClockFollower {
 public:
  enum class Action { none, step, restart };
  // Several inputs: the first one to send clock or START leads. Another input's clock is
  // ignored until the leader has sent nothing for kHandOverUs (`stamp` is midiArrivalStamp()).
  static constexpr std::uint32_t kHandOverUs = 1000000;  // tuned by ear
  // Once per audio block (`now` is midiArrivalStamp()): a leader silent for kHandOverUs steps
  // down, so its last stamp never ages past the stamp's 30-bit wrap (~18 minutes).
  void expire(int now) {
    if (hasLeader_ && ((static_cast<std::uint32_t>(now) - lastStamp_) & 0x3FFFFFFFu) >= kHandOverUs)
      hasLeader_ = false;
  }
  Action onRealtime(std::uint8_t status, unsigned device = 0, int stamp = 0) {
    if (status != 0xF8 && status != 0xFA) return Action::none;  // continue, stop, active sensing, ...
    const std::uint32_t age = (static_cast<std::uint32_t>(stamp) - lastStamp_) & 0x3FFFFFFFu;
    if (hasLeader_ && device != leader_ && age < kHandOverUs) return Action::none;
    hasLeader_ = true;
    leader_ = device;
    lastStamp_ = static_cast<std::uint32_t>(stamp);
    if (status == 0xFA) { ticks_ = 0; return Action::restart; }  // start
    return ticks_++ % 6 == 0 ? Action::step : Action::none;       // clock
  }

 private:
  std::uint32_t ticks_ = 0;
  std::uint32_t lastStamp_ = 0;
  unsigned leader_ = 0;
  bool hasLeader_ = false;
};

}  // namespace lunar24::host
