// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lunar24/core/midi_map.h>

namespace lunar24::host {
// The post-Learn observation outlives the Learn gesture, but never its profile.
class MidiRelativeLearn {
 public:
  void start(const core::MidiBinding& binding, std::uint64_t generation,
             std::uint64_t sequence, int firstValue) {
    binding_ = binding;
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
    if (((message >> 20) & 1u) == 0 || ((message >> 8) & 0x1fu) != binding_.key.channel ||
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
  bool active_ = false;
};
}  // namespace lunar24::host
