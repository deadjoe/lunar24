// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/midi_map.h>

namespace lunar24::host {
// The last note/CC message, as the audio thread packs it for the Learn overlay: number in
// bits 0-7, channel (1-16) in 8-12, input device (0-3) in 13-14, the CC flag in bit 20 and
// the value in 21-27.
inline unsigned midi_learn_device(std::uint32_t message) { return (message >> 13) & 3u; }

// The post-Learn observation outlives the Learn gesture, but never its profile.
class MidiRelativeLearn {
 public:
  void start(const core::MidiBinding& binding, std::uint64_t generation,
             std::uint64_t sequence, int firstValue, unsigned device = 0) {
    binding_ = binding;
    device_ = device;
    generation_ = generation;
    sequence_ = sequence;
    detector_ = {};
    active_ = binding.key.kind == core::MidiBindingKind::cc &&
              core::midi_parameter_drive(binding) == core::MidiParameterDrive::follow;
    if (active_) (void)detector_.feed(firstValue);
  }
  void cancel() { active_ = false; }
  void skipToSequence(std::uint64_t sequence) { sequence_ = sequence; }
  bool observe(const core::MidiMap& map, std::uint64_t generation, std::uint64_t sequence,
               std::uint32_t message, core::MidiBinding& result) {
    if (generation != generation_) cancel();
    if (!active_ || sequence == sequence_) return false;
    sequence_ = sequence;
    if (((message >> 20) & 1u) == 0 || midi_learn_device(message) != device_ ||
        (binding_.key.channel != 0 && ((message >> 8) & 0x1fu) != binding_.key.channel) ||
        (message & 0xffu) != binding_.key.number) return false;
    const auto mode = detector_.feed(static_cast<int>((message >> 21) & 0x7fu));
    if (mode == core::MidiInputMode::absolute) {
      if (detector_.exhausted()) cancel();
      return false;
    }
    cancel();
    const int row = map.find(binding_.key);
    if (row < 0) return false;
    const auto& current = map.at(static_cast<std::uint32_t>(row));
    if (current.mode != core::MidiInputMode::absolute || current.targetKind != binding_.targetKind ||
        current.parameter != binding_.parameter || current.action != binding_.action) return false;
    result = current;
    result.mode = mode;
    return true;
  }
 private:
  core::MidiBinding binding_;
  core::MidiRelativeDetector detector_;
  std::uint64_t generation_ = 0, sequence_ = 0;
  unsigned device_ = 0;  // the input device the learned control is on
  bool active_ = false;
};
}  // namespace lunar24::host
