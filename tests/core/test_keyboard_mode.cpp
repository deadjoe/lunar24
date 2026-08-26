// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-③ tests for the keyboard-mode side-context foundation (design/00 §2d, msg
// 695564a7; design/06 §P4; design/07 §3, §6).
//
// The contract the foundation exists to enforce:
//   * ParameterId identifies WHICH parameter (the one physical knob).
//   * Side identifies WHICH value bank is read.
//   * The id space never grows a `_r` id for the right side — split is two banks
//     under one id, NOT a new parameter (P5: no new controls).
//
// Per @Claude the information is in the negation, so every negative here is a
// REAL, representative wrong implementation the abstraction exists to prevent:
//   * split per-side independence — a reader that ignores side would read bank 0
//     for the right side too (wrong).
//   * twin shared bank — a per-side reader would give each side its own bank and
//     diverge (wrong).
//   * single one-bank — a per-side reader would split the single bank (wrong).
//   * no `_r` ParameterId — a side-aware reader that synthesized an offset id for
//     the right side would feed a different id to the bank (wrong).
//
// Tests exercise the TYPED KeyboardMode/Side enums (grounded semantics). The raw
// pressureBehaviour -> mode decode is PROVISIONAL (design/00 §3.2), so it is
// asserted only for totality + the documented-unknown default, never as proof.

#include "mini_test.h"

#include <array>
#include <cstdint>
#include <vector>

#include <lunar24/core/keyboard_mode.h>

namespace core = lunar24::core;

// ---------------------------------------------------------------- constants ----

static constexpr core::ParameterId kCutoffParam = core::ParameterId{42};
static constexpr core::ParameterId kResParam = core::ParameterId{87};
static constexpr core::IdValue kCutoffId = 42;
static constexpr core::IdValue kResId = 87;

// A minimal two-bank scalar store. Bank index (0/1) is the "which copy" the
// foundation resolves; id is passed through unchanged. Real DeviceState wiring is
// the next slice — this is ground truth for the BANK-resolution contract.
struct RecordingBanks {
  std::array<std::array<float, 128>, 2> bank{};  // [bankIndex][idValue]
  std::vector<core::IdValue> idsSeen;            // ids actually fed to a bank

  float get(std::uint8_t bankIndex, core::IdValue id) {
    idsSeen.push_back(id);
    return bank[bankIndex][id];
  }
  void set(std::uint8_t bankIndex, core::IdValue id, float v) {
    bank[bankIndex][id] = v;
  }
};

// ----------------------------------------------------- bank-resolution rules ----

static void single_resolves_a_single_bank() {
  // single: both sides act on the ONE bank regardless of side. A per-side reader
  // (bank index == side) would wrongly give the right side its own copy.
  CHECK_EQ(core::side_bank(core::KeyboardMode::Single, core::KeyboardSide::Left), 0u);
  CHECK_EQ(core::side_bank(core::KeyboardMode::Single, core::KeyboardSide::Right), 0u);
  CHECK(core::sides_share_bank(core::KeyboardMode::Single));  // one bank, shared
  CHECK_FALSE(core::side_bank(core::KeyboardMode::Single, core::KeyboardSide::Right)
              == core::side_bank(core::KeyboardMode::Single, core::KeyboardSide::Left) + 1u);
}

static void twin_shares_one_bank() {
  // twin: the two sides SHARE one bank ("…the two sides… share… same parame-").
  // Both resolve to bank 0.
  CHECK_EQ(core::side_bank(core::KeyboardMode::Twin, core::KeyboardSide::Left), 0u);
  CHECK_EQ(core::side_bank(core::KeyboardMode::Twin, core::KeyboardSide::Right), 0u);
  CHECK(core::sides_share_bank(core::KeyboardMode::Twin));
}

static void split_has_independent_banks() {
  // split: each side has an independent bank ("each having separate parameters").
  // Left reads bank 0, Right reads bank 1 — they must NOT resolve to the same bank.
  CHECK_EQ(core::side_bank(core::KeyboardMode::Split, core::KeyboardSide::Left), 0u);
  CHECK_EQ(core::side_bank(core::KeyboardMode::Split, core::KeyboardSide::Right), 1u);
  CHECK_FALSE(core::sides_share_bank(core::KeyboardMode::Split));
  // The two sides genuinely resolve apart — a same-bank bug would trip this.
  CHECK(core::side_bank(core::KeyboardMode::Split, core::KeyboardSide::Left)
        != core::side_bank(core::KeyboardMode::Split, core::KeyboardSide::Right));
}

// --------------------------------------------------- side-context scalar reads ----

static void split_reads_independent_bank_values() {
  // NEGATIVE: under split, the right side must read bank 1, NOT bank 0. If the
  // reader ignored side, right would come back with bank 0's cutoff (0.25) and
  // this assertion goes red.
  RecordingBanks b;
  b.set(0u, kCutoffId, 0.25f);
  b.set(1u, kCutoffId, 0.75f);

  auto read = [&](std::uint8_t bi, core::IdValue id) { return b.get(bi, id); };

  float left = core::read_side_scalar(read, core::KeyboardMode::Split,
                                      core::KeyboardSide::Left, kCutoffParam);
  float right = core::read_side_scalar(read, core::KeyboardMode::Split,
                                       core::KeyboardSide::Right, kCutoffParam);
  CHECK(left == 0.25f);   // left reads bank 0
  CHECK(right == 0.75f);  // right reads bank 1, independently
}

static void twin_reads_the_shared_bank_value() {
  // NEGATIVE: under twin, both sides must read the SAME bank (bank 0). If the
  // reader were per-side, the right side would read bank 1 and come back 0 (unset)
  // instead of the shared value — goes red.
  RecordingBanks b;
  b.set(0u, kCutoffId, 0.5f);

  auto read = [&](std::uint8_t bi, core::IdValue id) { return b.get(bi, id); };

  float left = core::read_side_scalar(read, core::KeyboardMode::Twin,
                                      core::KeyboardSide::Left, kCutoffParam);
  float right = core::read_side_scalar(read, core::KeyboardMode::Twin,
                                       core::KeyboardSide::Right, kCutoffParam);
  CHECK(left == 0.5f);
  CHECK(right == 0.5f);  // shared: same value for both sides
}

static void reading_preserves_the_parameter_id() {
  // NEGATIVE (the P5 "no new control" guard): the SAME ParameterId must reach the
  // bank regardless of side. A side-aware reader that synthesized a `_r` id for
  // the right side (id + offset) would feed a DIFFERENT id into the bank and this
  // assertion goes red — that is exactly the "new parameter without a knob" bug.
  RecordingBanks b;

  auto read = [&](std::uint8_t bi, core::IdValue id) { return b.get(bi, id); };

  (void)core::read_side_scalar(read, core::KeyboardMode::Split,
                               core::KeyboardSide::Left, kCutoffParam);
  (void)core::read_side_scalar(read, core::KeyboardMode::Split,
                               core::KeyboardSide::Right, kCutoffParam);
  CHECK_EQ(b.idsSeen.size(), 2u);
  CHECK_EQ(b.idsSeen[0], kCutoffId);  // left fed id 42
  CHECK_EQ(b.idsSeen[1], kCutoffId);  // right fed id 42 too — NOT 42+offset
}

static void a_second_parameter_keeps_its_own_id() {
  // A different parameter is a different id; the side-context reader must key on
  // id, not on the bank index. Cutoff (bank 0/1) and resonance (bank 0/1) stay
  // distinct even within the same bank.
  RecordingBanks b;
  b.set(0u, kCutoffId, 0.25f);
  b.set(0u, kResId, 0.90f);

  auto read = [&](std::uint8_t bi, core::IdValue id) { return b.get(bi, id); };

  float cutoff = core::read_side_scalar(read, core::KeyboardMode::Single,
                                        core::KeyboardSide::Left, kCutoffParam);
  float res = core::read_side_scalar(read, core::KeyboardMode::Single,
                                     core::KeyboardSide::Left, kResParam);
  CHECK(cutoff == 0.25f);
  CHECK(res == 0.90f);
}

// ------------------------------------------------------- provisional u8 decode ----

static void behaviour_decode_is_total() {
  // PROVISIONAL decode: assert totality + the documented unknown->Single default.
  // NOT asserting the 0/1/2 mapping as proven design truth (design/00 §3.2 — that
  // decode is flagged UNEVIDENCED). The point of the default is that a rogue
  // selector degrades to the conservative single-bank case, never a phantom split.
  for (std::uint32_t v = 0; v <= 255; ++v) {
    core::KeyboardMode m = core::mode_from_behaviour(static_cast<std::uint8_t>(v));
    CHECK(m == core::KeyboardMode::Single || m == core::KeyboardMode::Twin
          || m == core::KeyboardMode::Split);
  }
  CHECK_EQ(core::mode_from_behaviour(99), core::KeyboardMode::Single);  // unknown -> Single
}

int main() {
  single_resolves_a_single_bank();
  twin_shares_one_bank();
  split_has_independent_banks();
  split_reads_independent_bank_values();
  twin_reads_the_shared_bank_value();
  reading_preserves_the_parameter_id();
  a_second_parameter_keeps_its_own_id();
  behaviour_decode_is_total();
  return ::test::finish("keyboard mode (P4-③)");
}
