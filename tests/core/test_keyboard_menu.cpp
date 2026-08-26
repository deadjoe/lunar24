// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ menu tests (design/00 §P4-⑤, @Claude msg 87862933; Decis ④ + the bonus
// negative). @Claude's mandates, referenced here:
//
//   Decis ④ — "菜单项按手册全做,不要只做 preset+校准". Every keyboard menu
//   parameter gets a reachable path; those keyboard settings have NO dedicated
//   panel knob, so the menu is their only control surface. Only preset+calibration
//   would strand a whole batch of parameters with no reachable route and the P5
//   gate would fail. The reachability test asserts FULL coverage (all 22 per-side
//   scalars + the calibration/clock globals + the four no-domain selectors + the
//   submenu pages), not a fragment.
//
//   Bonus negative — "菜单路径写错地方". The menu must write an already-per-side
//   parameter through the SAME side bank the MIDI/live path uses, never to the
//   global parameters[] for a split right side. If it did, the menu path and the
//   MIDI path would DIVERGE for a split-right per-side param, and the P4 exit
//   criterion (which compares surfaces) goes red. The negative test builds the
//   buggy routing (a menu that writes the global bank for a split-right per-side
//   param) and asserts it DIVERGES from the correct MIDI path — proving the
//   divergence is detectable, so a real regression in write_item_value would not
//   silently converge.
//
// FINDINGS (never guess-filled): the ENCODER STEP amounts in the item table and the
// NAVIGATION UX (detent/click choreography) are PROVISIONAL — this menu engine
// exposes structural state, not a measured UX. No test asserts a step or a nav
// gesture as design truth.

#include "mini_test.h"

#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_menu.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/registry_ids.hpp>

namespace core = lunar24::core;

// Forward declarations (these are defined later in the file but used by the nav
// helpers below, which walk the item table to find the row the cursor is on).
static const core::KeyboardMenuItem& nav_pick(const core::KeyboardMenuNav& n);
static std::uint32_t menu_item_index(core::ParameterId id);

// -------------------------------------------------------------------------------
// Reachability (Decis ④) — every keyboard menu parameter is present
// -------------------------------------------------------------------------------

// Every per-side scalar (the 22 in kKeyboardScalarParameterIds) must have a
// menu item marked PerSideScalar with that id. A stranding gap here is a real
// defect: that parameter would have no reachable control surface.
static void every_per_side_scalar_is_reachable() {
  for (std::uint32_t i = 0; i < core::kKeyboardScalarRightCount; ++i) {
    const core::ParameterId id = core::kKeyboardScalarParameterIds[i];
    bool found = false;
    for (std::uint32_t j = 0; j < core::kMenuItemCount; ++j) {
      const core::KeyboardMenuItem& it = core::kMenuItems[j];
      if (it.store == core::MenuStore::PerSideScalar && it.scalarId == id) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }
}

// Every global keyboard param that has no panel knob must be reachable: the clock
// BPM and the whole calibration/debounce/encoder set (ids 130-140). These live in
// parameters[] and are reachable ONLY via the menu.
static void every_global_keyboard_knobless_param_is_reachable() {
  const core::ParameterId required[] = {
      core::ParameterId::keyboard_clock_bpm,
      core::ParameterId::keyboard_calibration_v_oct,
      core::ParameterId::keyboard_calibration_pressure,
      core::ParameterId::keyboard_dac_vref,
      core::ParameterId::keyboard_touch_threshold,
      core::ParameterId::keyboard_release_threshold,
      core::ParameterId::keyboard_pressure_min,
      core::ParameterId::keyboard_pressure_max,
      core::ParameterId::keyboard_mpr121_charge,
      core::ParameterId::keyboard_mpr121_discharge,
      core::ParameterId::keyboard_debounce,
      core::ParameterId::keyboard_encoder_direction,
  };
  CHECK_EQ(sizeof(required) / sizeof(required[0]), 12u);
  for (const core::ParameterId id : required) {
    bool found = false;
    for (std::uint32_t j = 0; j < core::kMenuItemCount; ++j) {
      const core::KeyboardMenuItem& it = core::kMenuItems[j];
      if (it.store == core::MenuStore::GlobalParam && it.scalarId == id) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }
}

// The four no-domain clock/rhythm selectors (arp_clock, arp_rhythm, seq_clock,
// seq_rhythm) must be reachable via the menu (slots 0-3 in keyboardClockSelectors).
static void every_no_domain_selector_is_reachable() {
  for (std::uint8_t slot = 0; slot < 4u; ++slot) {
    bool found = false;
    for (std::uint32_t j = 0; j < core::kMenuItemCount; ++j) {
      const core::KeyboardMenuItem& it = core::kMenuItems[j];
      if (it.store == core::MenuStore::NoDomainSelector && it.subIndex == slot) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }
}

// The submenu entries and the calibration gate (hold-while-boot) must be present:
// the two pages + the reachability of Arp/Seq/Preset — and specifically NOT only
// preset+calibration. Every page must be non-empty.
static void submenu_pages_and_calibration_are_present() {
  CHECK(core::menu_page_row_count(core::MenuGroup::Main) > 0u);
  CHECK(core::menu_page_row_count(core::MenuGroup::Arp) > 0u);
  CHECK(core::menu_page_row_count(core::MenuGroup::Seq) > 0u);
  CHECK(core::menu_page_row_count(core::MenuGroup::Preset) > 0u);
  CHECK(core::menu_page_row_count(core::MenuGroup::Calibration) > 0u);

  // The Main page must ENTER the submenus (submenu rows), not just list items.
  bool arpSubmenu = false, seqSubmenu = false, presetSubmenu = false;
  for (std::uint32_t j = 0; j < core::kMenuItemCount; ++j) {
    const core::KeyboardMenuItem& it = core::kMenuItems[j];
    if (it.page != core::MenuGroup::Main) continue;
    if (it.submenu && it.submenuPage == core::MenuGroup::Arp) arpSubmenu = true;
    if (it.submenu && it.submenuPage == core::MenuGroup::Seq) seqSubmenu = true;
    if (it.submenu && it.submenuPage == core::MenuGroup::Preset) presetSubmenu = true;
  }
  CHECK(arpSubmenu);
  CHECK(seqSubmenu);
  CHECK(presetSubmenu);
}

// No scalar parameter id appears in TWO editable rows: that would double the
// control for one parameter. (Compound/action rows intentionally share id 0.)
static void no_scalar_id_has_two_editable_rows() {
  for (std::uint32_t i = 0; i < core::kMenuItemCount; ++i) {
    const core::KeyboardMenuItem& a = core::kMenuItems[i];
    if (!core::menu_item_is_scalar(a)) continue;
    for (std::uint32_t j = i + 1; j < core::kMenuItemCount; ++j) {
      const core::KeyboardMenuItem& b = core::kMenuItems[j];
      if (!core::menu_item_is_scalar(b)) continue;
      CHECK_FALSE(a.store == b.store && a.scalarId == b.scalarId && a.subIndex == b.subIndex);
    }
  }
}

// -------------------------------------------------------------------------------
// Navigation + display snapshot
// -------------------------------------------------------------------------------

static void nav_rotate_wraps_and_submenu_enters() {
  core::DeviceStateV1 state;
  core::KeyboardMenuNav nav(state);
  CHECK_EQ(nav.page(), core::MenuGroup::Main);
  CHECK(nav.cursor_row() == 0u);

  // Rotating within Main wraps to the row count.
  const std::uint32_t mainRows = core::menu_page_row_count(core::MenuGroup::Main);
  nav.rotate(-1);  // backward from 0 -> last row, if >1
  if (mainRows > 1u) CHECK_EQ(nav.cursor_row(), mainRows - 1u);

  // Click a submenu row (Arp entry is on Main) -> page becomes Arp.
  // Move to the ARPEGGIATOR submenu row.
  bool entered = false;
  std::uint32_t guard = 0;
  while (!entered) {
    const core::KeyboardMenuItem& item = nav_pick(nav);
    if (item.submenu && item.submenuPage == core::MenuGroup::Arp) {
      nav.click();
      CHECK_EQ(nav.page(), core::MenuGroup::Arp);
      CHECK_EQ(nav.cursor_row(), 0);
      entered = true;
    } else {
      nav.rotate(1);
      if (++guard > core::kMenuItemCount) break;  // table is broken; avoid a hang
    }
  }
  CHECK(entered);  // the Arp submenu entry must be reachable on Main
}

// Helper: the item the cursor is on.
static const core::KeyboardMenuItem& nav_pick(const core::KeyboardMenuNav& n) {
  // Re-derive by page + cursor: the menu engine's index_of_row is private, so the
  // test walks the table itself (small, and it doubles as a cross-check).
  std::uint32_t seen = 0;
  for (std::uint32_t i = 0; i < core::kMenuItemCount; ++i) {
    if (core::kMenuItems[i].page == n.page()) {
      if (seen == n.cursor_row()) return core::kMenuItems[i];
      ++seen;
    }
  }
  return core::kMenuItems[0];
}

static void nav_edit_a_per_side_scalar_commits_to_bank() {
  core::DeviceStateV1 state;
  // Split, right side: arp_hold should land in bank 1, never the global bank.
  core::KeyboardMenuNav nav(state);
  nav.set_context(core::KeyboardMode::Split, core::KeyboardSide::Right);

  // Navigate Main -> Arp, then to the HOLD row.
  bool inArp = false;
  std::uint32_t guardNav = 0;
  while (!inArp) {
    const core::KeyboardMenuItem& it = nav_pick(nav);
    if (it.submenu && it.submenuPage == core::MenuGroup::Arp) {
      nav.click();
      inArp = true;
    } else {
      nav.rotate(1);
      if (++guardNav > core::kMenuItemCount) break;
    }
  }
  CHECK(inArp);
  // Find the HOLD row (subIndex 0 on the Arp page).
  std::uint32_t guard = 0;
  while (nav_pick(nav).scalarId != core::ParameterId::keyboard_arp_hold ||
         !core::menu_item_is_scalar(nav_pick(nav))) {
    nav.rotate(1);
    if (++guard > core::kMenuItemCount) break;
  }
  CHECK_EQ(nav_pick(nav).scalarId, core::ParameterId::keyboard_arp_hold);

  nav.click();  // start edit
  CHECK(nav.editing());
  nav.rotate(3);  // PROVISIONAL step 1 -> 3 increments
  nav.hold();     // commit
  CHECK_FALSE(nav.editing());

  const std::int32_t idx = core::keyboard_scalar_index(core::ParameterId::keyboard_arp_hold);
  CHECK(idx >= 0);
  // Split-right MUST read bank 1 (keyboardScalarRight), not the global bank.
  CHECK_EQ(state.parameters[static_cast<core::IdValue>(core::ParameterId::keyboard_arp_hold)], 0.0);
  CHECK(state.keyboardScalarRight[idx] > 0.0);
  // The menu and the MIDI path read it back identically (the choke is shared).
  CHECK_EQ(core::read_item_value(state, nav_pick(nav), core::KeyboardMode::Split,
                                 core::KeyboardSide::Right),
           state.keyboardScalarRight[idx]);
}

static void calibration_gate_sets_flag_and_page() {
  core::DeviceStateV1 state;
  core::KeyboardMenuNav nav(state);
  nav.set_context(core::KeyboardMode::Single, core::KeyboardSide::Left);
  nav.enter_calibration();
  CHECK_EQ(nav.page(), core::MenuGroup::Calibration);
  const core::KeyboardDisplay d = nav.snapshot("CAL", "OK");
  CHECK(d.isCalibration);
  CHECK_EQ(d.segment, static_cast<std::uint8_t>(core::MenuGroup::Calibration));
  CHECK(d.rowCount > 0u);
}

// -------------------------------------------------------------------------------
// Side-context wrong-bank routing negative (mandated bonus)
// -------------------------------------------------------------------------------

static void menu_path_and_midi_path_converge_split_right() {
  // Positive: for a split-right per-side param, the menu write and the MIDI/live
  // write go through the SAME bank, so the two surfaces agree. If the menu wrote
  // the global bank instead, this would DIVERGE and the exit criterion would red.
  core::DeviceStateV1 state;

  // MIDI/live path writes arp_hold = 3.3 for split-right.
  core::write_side_scalar_value(state, core::ParameterId::keyboard_arp_hold, 3.3,
                                core::KeyboardMode::Split, core::KeyboardSide::Right);
  const std::int32_t idx = core::keyboard_scalar_index(core::ParameterId::keyboard_arp_hold);
  const double midiBankValue = state.keyboardScalarRight[idx];
  CHECK_EQ(midiBankValue, 3.3);
  CHECK_EQ(state.parameters[static_cast<core::IdValue>(core::ParameterId::keyboard_arp_hold)], 0.0);

  // Menu path writes the SAME 3.3 via write_item_value -> must land identically.
  core::DeviceStateV1 menuState;
  const core::KeyboardMenuItem* holdItem = nullptr;
  for (std::uint32_t i = 0; i < core::kMenuItemCount; ++i) {
    if (core::kMenuItems[i].store == core::MenuStore::PerSideScalar &&
        core::kMenuItems[i].scalarId == core::ParameterId::keyboard_arp_hold) {
      holdItem = &core::kMenuItems[i];
      break;
    }
  }
  CHECK(holdItem != nullptr);
  core::write_item_value(menuState, *holdItem, 3.3, core::KeyboardMode::Split,
                         core::KeyboardSide::Right);
  CHECK_EQ(menuState.keyboardScalarRight[idx], 3.3);
  CHECK_EQ(menuState.parameters[static_cast<core::IdValue>(core::ParameterId::keyboard_arp_hold)], 0.0);
  // The two surfaces CONVERGE — this is what the exit criterion must see.
  CHECK_EQ(menuState.keyboardScalarRight[idx], midiBankValue);
}

static void wrong_bank_menu_write_diverges_and_is_detected() {
  // Negative (the mutation under test): a "menu path writes the wrong place" —
  // it writes a split-right per-side param to the GLOBAL bank (parameters[id])
  // instead of the current side bank. This is the exact bug @Claude's bonus
  // negative describes. It MUST diverge from the MIDI path, so a real regression
  // in write_item_value never silently converges.
  core::DeviceStateV1 state;
  const std::int32_t idx = core::keyboard_scalar_index(core::ParameterId::keyboard_arp_hold);
  const core::IdValue gid = static_cast<core::IdValue>(core::ParameterId::keyboard_arp_hold);

  // MIDI/live path (correct): split-right lands in bank 1.
  core::write_side_scalar_value(state, core::ParameterId::keyboard_arp_hold, 5.0,
                                core::KeyboardMode::Split, core::KeyboardSide::Right);
  const double midiBankValue = state.keyboardScalarRight[idx];

  // Buggy menu path: writes to the global bank, ignoring the side.
  core::DeviceStateV1 buggy;
  buggy.parameters[gid] = 5.0;

  // The buggy path lands in parameters[id] (global) and leaves bank 1 at 0 —
  // exactly what a wrong-routing menu would do. Assert the two surfaces DIVERGE:
  // this is the divergence the exit criterion catches, so a regression that leaks a
  // per-side param to the global bank would make the REAL write_item_value path
  // behave like `buggy` here and this comparison would trip.
  CHECK_EQ(buggy.parameters[gid], 5.0);
  CHECK_EQ(buggy.keyboardScalarRight[idx], 0.0);
  CHECK(buggy.keyboardScalarRight[idx] != midiBankValue);  // diverges -> detectable
  // And the correct menu path does NOT equal the buggy one.
  core::DeviceStateV1 correct;
  core::write_item_value(correct,
                         core::kMenuItems[menu_item_index(core::ParameterId::keyboard_arp_hold)],
                         5.0, core::KeyboardMode::Split, core::KeyboardSide::Right);
  CHECK(correct.keyboardScalarRight[idx] != buggy.keyboardScalarRight[idx]);
}

static std::uint32_t menu_item_index(core::ParameterId id) {
  for (std::uint32_t i = 0; i < core::kMenuItemCount; ++i) {
    if (core::kMenuItems[i].store == core::MenuStore::PerSideScalar &&
        core::kMenuItems[i].scalarId == id) {
      return i;
    }
  }
  return 0;
}

// -------------------------------------------------------------------------------

int main() {
  every_per_side_scalar_is_reachable();
  every_global_keyboard_knobless_param_is_reachable();
  every_no_domain_selector_is_reachable();
  submenu_pages_and_calibration_are_present();
  no_scalar_id_has_two_editable_rows();
  nav_rotate_wraps_and_submenu_enters();
  nav_edit_a_per_side_scalar_commits_to_bank();
  calibration_gate_sets_flag_and_page();
  menu_path_and_midi_path_converge_split_right();
  wrong_bank_menu_write_diverges_and_is_detected();
  return ::test::finish("keyboard menu (P4-⑤, reachability + side routing)");
}
