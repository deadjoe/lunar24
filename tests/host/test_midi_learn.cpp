// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/midi_learn.h>

using namespace lunar24;
static std::uint32_t message(int value, int channel = 3, int number = 24) {
  return (static_cast<std::uint32_t>(value) << 21) | (1u << 20) |
         (static_cast<std::uint32_t>(channel) << 8) | static_cast<std::uint32_t>(number);
}
int main() {
  core::MidiBinding binding;
  binding.key.channel = 3;
  binding.key.number = 24;
  binding.parameter = core::ParameterId::vcf_l_freq;
  core::MidiMap map;
  CHECK(map.bind(binding));
  host::MidiRelativeLearn learn;
  core::MidiBinding result;
  // Learn first commits ABS. Subsequent controller messages, independent of brand,
  // identify each supported encoding without requiring another Learn gesture.
  learn.start(binding, 1, 10, 1);
  CHECK(!learn.observe(map, 1, 10, message(1), result));  // do not count one event twice
  CHECK(!learn.observe(map, 1, 11, message(1, 4), result));
  CHECK(!learn.observe(map, 1, 12, message(1, 3, 25), result));
  CHECK(learn.observe(map, 1, 13, message(1), result));
  CHECK(result.mode == core::MidiInputMode::relativeTwosComplement);
  CHECK(map.bind(result));
  learn.start(binding, 1, 20, 65);
  CHECK(!learn.observe(map, 1, 21, message(65), result));  // do not overwrite a chosen mode
  CHECK(map.bind(binding));
  learn.start(binding, 1, 30, 65);
  CHECK(learn.observe(map, 1, 31, message(65), result));
  CHECK(result.mode == core::MidiInputMode::relativeBinOffset);
  learn.start(binding, 1, 40, 1);
  CHECK(!learn.observe(map, 1, 41, message(65), result));
  CHECK(learn.observe(map, 1, 42, message(65), result));
  CHECK(result.mode == core::MidiInputMode::relativeSignMagnitude);
  learn.start(binding, 1, 50, 1);
  CHECK(!learn.observe(map, 2, 51, message(1), result));  // switched to a profile with the same CC
  CHECK(!learn.observe(map, 1, 52, message(1), result));  // switching back does not revive it
  learn.start(binding, 2, 60, 1);
  learn.cancel();
  CHECK(!learn.observe(map, 2, 61, message(1), result));
  learn.start(binding, 2, 70, 1);
  learn.skipToSequence(71);                            // browsing does not learn its last message
  CHECK(!learn.observe(map, 2, 71, message(1), result));
  CHECK(learn.observe(map, 2, 72, message(1), result));
  learn.start(binding, 2, 80, 1);
  auto reassigned = binding;
  reassigned.parameter = core::ParameterId::effector_blend;
  CHECK(map.bind(reassigned));
  CHECK(!learn.observe(map, 2, 81, message(1), result));
  CHECK(map.bind(binding));
  learn.start(binding, 2, 100, 20);
  for (int v = 21; v < 90; ++v)
    CHECK(!learn.observe(map, 2, static_cast<std::uint64_t>(101 + v), message(v), result));
  CHECK(!learn.observe(map, 2, 300, message(1), result)); // exhausted absolute observation stays off
  CHECK(!learn.observe(map, 2, 301, message(1), result));
  return test::finish("test_midi_learn");
}
