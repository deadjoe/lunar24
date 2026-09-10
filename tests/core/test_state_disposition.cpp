// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Disposition table oracle (task #75 revision 3 — Codex BLOCK bd53b76a).
//
// The revision-3 oracle is NOT self-proving. Each landed id is classified by an
// INDEPENDENT rule derived only from the registry descriptor (owner / role) plus a
// pinned 2-member unavailable set — never by reading the disposition table itself —
// and the table's disposition_of(id) must agree with that rule for EVERY id, and the
// five per-class counts must be exactly 183/35/125/2/0.
//
// This catches the "equal-quantity separation" false-green: swapping the class of a
// DSP-landed id with a differently-classed id (e.g. an effector id) leaves the class
// COUNTS unchanged, yet the per-id walk below turns red because each id's observed
// class differs from the independent rule. The old oracle only counted classes, so
// such a swap was invisible; revision 3 pins it per-id.

#include "mini_test.h"

#include <cstdint>
#include <string_view>

#include <lunar24/core/device_capacities.h>
#include <lunar24/core/state_disposition.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

// The 2 REMAINING transfer-unavailable parameters (no real runtime consumer), by stable
// ParameterId: vco_a.pwm(8) and vco_b.pwm(30) — both still deferred to GH#19.
// GH#15 D1 moved drone_3/6_mod, D2 moved drone_3/6_hi_low + drone_3/6_rate_switch,
// D3 moved drone_3/6_divider, D4 moved the drone3/6 ATT/RLS pair, and D5 moved the
// drone3/6 HOLD pair (an OR term on the envelope TARGET, not a second envelope)
// (16 -> 14 -> 10 -> 8 -> 4 -> 2).
static constexpr core::ParameterId kUnavailablePids[] = {
    core::ParameterId::vco_a_pwm,
    core::ParameterId::vco_b_pwm,
};

static bool is_unavailable(core::ParameterId id) noexcept {
  for (std::uint32_t i = 0; i < sizeof(kUnavailablePids) / sizeof(kUnavailablePids[0]); ++i)
    if (kUnavailablePids[i] == id) return true;
  return false;
}

// INDEPENDENT classification, from the registry descriptor only. This is the oracle
// the table must reproduce exactly; it never consults the disposition table. The
// deferred class is pinned by PRODUCT OWNERSHIP, not the generic `role != knob`
// heuristic: a parameter is deferred iff its owner is the effector module (8) or a
// program-owner range (owner.starts_with("program."), 117) — the "cartridge
// programme not yet wired" contract. It deliberately uses the owner string, never a
// guessed role, so a live DSP control that happens to be a selector can never be
// mis-filed as deferred.
static core::StateDisposition oracle_classify(core::ParameterId id) {
  if (is_unavailable(id)) return core::StateDisposition::transfer_unavailable;
  const core::ParameterDescriptor* d = core::find_parameter(id);
  if (d == nullptr) return core::StateDisposition::invalid_unlanded;
  if (d->owner == "keyboard") return core::StateDisposition::applied_to_keyboard;
  if (d->owner == "effector" || d->owner.find("program.") == 0)
    return core::StateDisposition::preserved_deferred_p6_p8;
  return core::StateDisposition::applied_to_dsp;
}

static void class_counts_and_sum() {
  CHECK_EQ(core::kDeviceStateDispositionCount, 345u);
  CHECK_EQ(core::count_disposition(core::StateDisposition::applied_to_dsp), 183u);
  CHECK_EQ(core::count_disposition(core::StateDisposition::applied_to_keyboard), 35u);
  CHECK_EQ(core::count_disposition(core::StateDisposition::preserved_deferred_p6_p8), 125u);
  CHECK_EQ(core::count_disposition(core::StateDisposition::transfer_unavailable), 2u);
  CHECK_EQ(core::count_disposition(core::StateDisposition::invalid_unlanded), 0u);
  const std::uint32_t sum =
      core::count_disposition(core::StateDisposition::applied_to_dsp) +
      core::count_disposition(core::StateDisposition::applied_to_keyboard) +
      core::count_disposition(core::StateDisposition::preserved_deferred_p6_p8) +
      core::count_disposition(core::StateDisposition::transfer_unavailable) +
      core::count_disposition(core::StateDisposition::invalid_unlanded);
  CHECK_EQ(sum, core::kDeviceStateDispositionCount);
}

// Independent per-id walk. The table must agree with the oracle for every landed id;
// the five counts must be exactly the pinned numbers (re-derived from the oracle, not
// the table, so a self-consistent-but-wrong table cannot pass).
static void per_id_matches_independent_oracle() {
  std::uint32_t dsp = 0, kbd = 0, deferred = 0, unavailable = 0, unlanded = 0;
  for (std::uint32_t a = 0; a < core::kDeviceStateDispositionCount; ++a) {
    const core::DispositionEntry& ea = core::kDeviceStateDisposition[a];
    CHECK(core::find_parameter(ea.id) != nullptr);  // landed (registry agrees)
    const core::StateDisposition expected = oracle_classify(ea.id);
    CHECK(ea.disposition == expected);              // table == independent oracle
    CHECK(ea.disposition != core::StateDisposition::invalid_unlanded);
    switch (expected) {
      case core::StateDisposition::applied_to_dsp: ++dsp; break;
      case core::StateDisposition::applied_to_keyboard: ++kbd; break;
      case core::StateDisposition::preserved_deferred_p6_p8: ++deferred; break;
      case core::StateDisposition::transfer_unavailable: ++unavailable; break;
      case core::StateDisposition::invalid_unlanded: ++unlanded; break;
    }
    // No duplicate ParameterId (table is sparse, keyed by stable id).
    for (std::uint32_t b = a + 1; b < core::kDeviceStateDispositionCount; ++b)
      CHECK(core::kDeviceStateDisposition[b].id != ea.id);
  }
  CHECK_EQ(dsp, 183u);
  CHECK_EQ(kbd, 35u);
  CHECK_EQ(deferred, 125u);
  CHECK_EQ(unavailable, 2u);
  CHECK_EQ(unlanded, 0u);
}

// Equal-quantity separation is DETECTABLE: an applied_to_DSP id and a deferred
// (effector) id classify differently, so swapping their classes would flip both to
// red even though every class count stays unchanged. We pin one representative of each
// so the oracle can never be "equal counts, wrong labels".
static void equal_quantity_swap_is_detectable() {
  const core::ParameterId dsp_id = core::ParameterId::vco_a_tune;       // DSP knob
  const core::ParameterId eff_id = core::ParameterId::program_orche_3_z;  // deferred (x/y/z)
  CHECK(core::disposition_of(dsp_id) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(eff_id) == core::StateDisposition::preserved_deferred_p6_p8);
  CHECK(oracle_classify(dsp_id) != oracle_classify(eff_id));
  // Swapping would each disagree with the independent oracle => both red.
  CHECK(core::StateDisposition::preserved_deferred_p6_p8 != oracle_classify(dsp_id));
  CHECK(core::StateDisposition::applied_to_dsp != oracle_classify(eff_id));
}

// Pinned product ownership contract for the deferred class: it must be EXACTLY the 8
// effector-module parameters plus the 117 program-owner parameters (125 in total).
// This is deliberately not derived from the disposition table — it walks the same
// independent owner rule, proving the 125 is not an arbitrary count. If the effector
// or a program range ever changes owner (or a deferred row is not effector/program),
// this stays red.
static void deferred_is_effector_plus_program_ranges() {
  std::uint32_t effector = 0, program = 0, other = 0;
  for (std::uint32_t i = 0; i < core::kDeviceStateDispositionCount; ++i) {
    const core::DispositionEntry& ea = core::kDeviceStateDisposition[i];
    if (ea.disposition != core::StateDisposition::preserved_deferred_p6_p8) continue;
    const core::ParameterDescriptor* d = core::find_parameter(ea.id);
    CHECK(d != nullptr);
    if (d->owner == "effector") {
      ++effector;
    } else if (d->owner.find("program.") == 0) {
      ++program;
    } else {
      ++other;  // a deferred row that is neither effector nor program — contract broken
    }
  }
  CHECK_EQ(effector, 8u);
  CHECK_EQ(program, 117u);
  CHECK_EQ(other, 0u);
  CHECK_EQ(effector + program, 125u);
}

static void no_hole_or_slack_row() {
  for (std::uint32_t i = 0; i < core::kDeviceStateDispositionCount; ++i)
    CHECK(core::kDeviceStateDisposition[i].disposition != core::StateDisposition::invalid_unlanded);
}

static void hole_and_slack_resolve_invalid() {
  // pid 4 is an ID hole (vco_a.oct_sel = 3, vco_a.sub_sel = 5). pid 412 is the
  // first capacity-slack id (> last landed 411). Both must be unlanded.
  CHECK(core::find_parameter(static_cast<core::ParameterId>(4)) == nullptr);
  CHECK(core::find_parameter(static_cast<core::ParameterId>(412)) == nullptr);
  CHECK(core::disposition_of(static_cast<core::ParameterId>(4)) == core::StateDisposition::invalid_unlanded);
  CHECK(core::disposition_of(static_cast<core::ParameterId>(412)) == core::StateDisposition::invalid_unlanded);
  CHECK(core::is_landed_parameter(static_cast<core::ParameterId>(4)) == false);
  CHECK(core::is_landed_parameter(static_cast<core::ParameterId>(412)) == false);
  // The 10 unavailable are exactly the pinned set, none of them a hole/slack id.
  for (std::uint32_t i = 0; i < sizeof(kUnavailablePids) / sizeof(kUnavailablePids[0]); ++i)
    CHECK(core::is_landed_parameter(kUnavailablePids[i]));
}

static void spot_check_known_dispositions() {
  CHECK(core::disposition_of(core::ParameterId::vco_a_tune) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::keyboard_behaviour) == core::StateDisposition::applied_to_keyboard);
  CHECK(core::disposition_of(core::ParameterId::program_orche_3_z) == core::StateDisposition::preserved_deferred_p6_p8);
  CHECK(core::disposition_of(core::ParameterId::vco_a_pwm) == core::StateDisposition::transfer_unavailable);
  CHECK(core::disposition_of(core::ParameterId::vco_b_pwm) == core::StateDisposition::transfer_unavailable);
  // GH#15 D1: drone_3/6_mod moved from transfer_unavailable to applied_to_dsp.
  CHECK(core::disposition_of(core::ParameterId::drone_3_mod) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_mod) == core::StateDisposition::applied_to_dsp);
  // GH#15 D2: drone_3/6_hi_low + drone_3/6_rate_switch moved from
  // transfer_unavailable to applied_to_dsp (16 -> 14 -> 10).
  CHECK(core::disposition_of(core::ParameterId::drone_3_hi_low) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_3_rate_switch) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_hi_low) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_rate_switch) == core::StateDisposition::applied_to_dsp);
  // GH#15 D4: drone_3/6_att + drone_3/6_rls moved from transfer_unavailable to
  // applied_to_dsp; GH#15 D5: the drone_3/6 HOLD pair followed (16 -> 14 -> 10 -> 8 ->
  // 4 -> 2). Both steps leave only the GH#19 pwm pair unavailable.
  CHECK(core::disposition_of(core::ParameterId::drone_3_att) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_3_rls) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_att) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_rls) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_3_hold) == core::StateDisposition::applied_to_dsp);
  CHECK(core::disposition_of(core::ParameterId::drone_6_hold) == core::StateDisposition::applied_to_dsp);
}

static void landed_slot_arithmetic() {
  std::uint32_t landed = 0;
  for (std::uint32_t i = 0; i < core::kParameterIdSpace; ++i)
    if (core::is_landed_parameter(static_cast<core::ParameterId>(i))) ++landed;
  CHECK_EQ(landed, core::kParameterCount);  // 345
  CHECK_EQ(core::kDeviceParamCapacity, core::kParameterIdSpace + 12u);
}

int main() {
  class_counts_and_sum();
  per_id_matches_independent_oracle();
  equal_quantity_swap_is_detectable();
  deferred_is_effector_plus_program_ranges();
  no_hole_or_slack_row();
  hole_and_slack_resolve_invalid();
  spot_check_known_dispositions();
  landed_slot_arithmetic();
  return ::test::finish("state disposition (task #75 r4)");
}
