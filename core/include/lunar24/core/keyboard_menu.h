// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ encoder menu (design/00 §P4-⑤, @Claude msg 87862933).
//
// @Claude Decis ④: the menu is the ONLY control surface for the keyboard
// parameters that have NO dedicated panel knob (P5 "no new controls" — the frozen
// registry gives the keyboard one knob per per-side parameter; the panel has no
// separate knob for, say, arp hold or the calibration thresholds). So this menu
// MUST give every such parameter a reachable path. It is not permitted to expose
// only preset+calibration: that would strand a whole batch of parameters with no
// way to reach them, and the P5 completeness gate would fail.
//
// Two properties are the contract:
//   * REACHABILITY — every keyboard menu parameter from the manual (solar42N
//     manual L637-1093) is present in the item table, so a parameter with no
//     panel knob is still reachable. Re-ordered? No; grouped by the manual's
//     pages. A missing item is a real defect the reachability test catches.
//   * SIDE-CONTEXT WRITE ROUTING — the menu writes an already-per-side parameter
//     through the SAME side bank that the live/MIDI path uses (side_bank(mode,
//     side)), NEVER to the global parameters[] for a split right side. Mixing
//     the two would make the menu path and the MIDI path disagree. That is the
//     negative test (msg 87862933): a menu that writes a split right-side param
//     to the global bank diverges from the MIDI path and goes red.
//
// This owns the DISPLAY SNAPSHOT too (design/00 §P4-⑤ Decis ①): the menu fills a
// pure-data keyboard_display.h struct. No query interface, no getter onto the
// menu's internals — the struct IS the render contract.
//
// FINDINGS (never guess-filled, @Claude msgs 87862933 + 90e14dfd):
//   * the ENCODER STEP AMOUNT is PROVISIONAL. Every `step` here is a labelled
//     placeholder (the manual gives ranges, not detents); no test asserts a step
//     as design truth.
//   * the multi-point calibration (0/2/5/8V) is not modelled (see output
//     calibration FINDINGS): each calibration output is ONE scalar here.
//   * MPR121 charge/discharge/debounce are real settable settings stored in
//     parameters[] but have no modelled runtime effect (the core models no
//     touch-chip debounce). Recorded as FINDINGS, not a fabricated behaviour.
//   * the manual's submenu NESTING is reconstructed as pages (Main/Arp/Seq/Preset
//     /Calibration); the exact UX is a rendering decision, the core only exposes
//     the parameters.

#pragma once

#include <cstdint>
#include <utility>

#include <lunar24/core/device_state.h>
#include <lunar24/core/keyboard_display.h>
#include <lunar24/core/keyboard_mode.h>
#include <lunar24/core/keyboard_side_bank.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// Which menu page a row appears on. Main is the top-level scroll; Arp/Seq/Preset/
// Calibration are submenus (a submenu row on Main enters them). Calibration is the
// hold-while-boot gate (manual L1048-1050).
enum class MenuGroup : std::uint8_t {
  Main = 0, Arp = 1, Seq = 2, Preset = 3, Calibration = 4,
};
inline constexpr std::uint8_t kMenuGroupCount = 5u;

// Where a menu item's value lives. The routing (read_item_value / write_item_value)
// dispatches on this — and the per-side scalar routing is the contract the
// negative test guards.
enum class MenuStore : std::uint8_t {
  PerSideScalar,    // a 22-keyboard-scalar param, per side via side_bank(mode, side)
  GlobalParam,      // a deviceState parameter in parameters[] (clock_bpm, calibration 130-140)
  NoDomainSelector, // a four no-domain clock/rhythm selector in keyboardClockSelectors[bank]
  NonScalar,        // a compound (scale editor / seq steps / plate tune / push button), subIndex = element
  GlobalSettings,   // KeyboardSettings.pressureBehaviour (single/twin/split) — no ParameterId
  PresetAction,     // preset load/save/initialise — subIndex = preset slot
  CalibrationAction,// init/save calibration — subIndex = action
};

// One menu row. Values are edited as `min..max` in `step` increments; selectors use
// their option index as the value. `submenu` + `submenuPage` mark a row that
// ENTERS a submenu on click instead of starting an edit.
struct KeyboardMenuItem {
  MenuGroup page = MenuGroup::Main;
  const char* label = nullptr;
  MenuStore store = MenuStore::PerSideScalar;
  ParameterId scalarId = ParameterId{0};   // PerSideScalar / GlobalParam / NoDomainSelector
  std::uint8_t subIndex = 0;               // no-domain idx / compound element / preset slot / action id
  bool submenu = false;
  MenuGroup submenuPage = MenuGroup::Main;
  double min = 0.0;
  double max = 0.0;
  double step = 1.0;                       // PROVISIONAL (FINDINGS)
};

// constexpr factory so the big table stays readable (C++17 has no designated
// initializers). Defaults keep the call short.
constexpr KeyboardMenuItem menu_item(MenuGroup page, const char* label, MenuStore store,
                                     ParameterId id, std::uint8_t sub, bool submenu,
                                     MenuGroup dst, double mn, double mx, double step) noexcept {
  KeyboardMenuItem i;
  i.page = page; i.label = label; i.store = store; i.scalarId = id; i.subIndex = sub;
  i.submenu = submenu; i.submenuPage = dst; i.min = mn; i.max = mx; i.step = step;
  return i;
}

// ---------------------------------------------------------------------------
// The reachable item table (every keyboard menu parameter per the manual).
// Grouped by page. Order within a page is presentational; what matters is that
// every no-knob keyboard parameter appears. Duplicating a parameter is a defect
// the reachability test flags (it would double the control for one id).
// ---------------------------------------------------------------------------
//
// Ranges: selector max = optionCount-1 (from the registry OptionLabels);
// continuous = the manual's nominal range. Steps are PROVISIONAL (FINDINGS).
inline constexpr KeyboardMenuItem kMenuItems[] = {
    // ---- Main page (top-level scroll) ----
    menu_item(MenuGroup::Main, "BEHAVIOUR", MenuStore::GlobalSettings,
              ParameterId{0}, 0, false, MenuGroup::Main, 0.0, 2.0, 1.0),
    menu_item(MenuGroup::Main, "MODE", MenuStore::PerSideScalar,
              ParameterId::keyboard_mode, 0, false, MenuGroup::Main, 0.0, 2.0, 1.0),
    menu_item(MenuGroup::Main, "ARPEGGIATOR", MenuStore::PerSideScalar,
              ParameterId{0}, 0, true, MenuGroup::Arp, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Main, "SEQUENCER", MenuStore::PerSideScalar,
              ParameterId{0}, 0, true, MenuGroup::Seq, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Main, "PORTAMENTO SPEED", MenuStore::PerSideScalar,
              ParameterId::keyboard_portamento_speed, 0, false, MenuGroup::Main, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Main, "PORTAMENTO LEGATO", MenuStore::PerSideScalar,
              ParameterId::keyboard_portamento_legato, 0, false, MenuGroup::Main, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Main, "VIBRATO SPEED", MenuStore::PerSideScalar,
              ParameterId::keyboard_vibrato_speed, 0, false, MenuGroup::Main, 0.0, 127.0, 1.0),
    menu_item(MenuGroup::Main, "VIBRATO DEPTH", MenuStore::PerSideScalar,
              ParameterId::keyboard_vibrato_depth, 0, false, MenuGroup::Main, 0.0, 127.0, 1.0),
    menu_item(MenuGroup::Main, "VIBRATO DELAY", MenuStore::PerSideScalar,
              ParameterId::keyboard_vibrato_delay, 0, false, MenuGroup::Main, 0.0, 127.0, 1.0),
    menu_item(MenuGroup::Main, "VIBRATO PRESSURE", MenuStore::PerSideScalar,
              ParameterId::keyboard_vibrato_pressure, 0, false, MenuGroup::Main, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Main, "PRESSURE OUTPUT", MenuStore::PerSideScalar,
              ParameterId::keyboard_pressure_output, 0, false, MenuGroup::Main, 0.0, 4.0, 1.0),
    menu_item(MenuGroup::Main, "PRESSURE RISE", MenuStore::PerSideScalar,
              ParameterId::keyboard_pressure_rise, 0, false, MenuGroup::Main, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Main, "PRESSURE FALL", MenuStore::PerSideScalar,
              ParameterId::keyboard_pressure_fall, 0, false, MenuGroup::Main, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Main, "QUANTISE SCALE EDITOR", MenuStore::NonScalar,
              ParameterId{0}, 0, false, MenuGroup::Main, 0.0, 4095.0, 1.0),
    menu_item(MenuGroup::Main, "QUANTISE LOAD SCALE", MenuStore::PerSideScalar,
              ParameterId::keyboard_quantise_load_scale, 0, false, MenuGroup::Main, 0.0, 18.0, 1.0),
    menu_item(MenuGroup::Main, "ROOT NOTE", MenuStore::PerSideScalar,
              ParameterId::keyboard_root_note, 0, false, MenuGroup::Main, 0.0, 11.0, 1.0),
    menu_item(MenuGroup::Main, "CLOCK BPM", MenuStore::GlobalParam,
              ParameterId::keyboard_clock_bpm, 0, false, MenuGroup::Main, 10.0, 300.0, 1.0),
    menu_item(MenuGroup::Main, "PRESETS", MenuStore::PresetAction,
              ParameterId{0}, 0, true, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Main, "PLATE/BUTTON EDITOR", MenuStore::NonScalar,
              ParameterId{0}, 0, false, MenuGroup::Main, 1.0, 12.0, 1.0),

    // ---- Arp page (manual L816-829) ----
    menu_item(MenuGroup::Arp, "HOLD", MenuStore::PerSideScalar,
              ParameterId::keyboard_arp_hold, 0, false, MenuGroup::Arp, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Arp, "CLOCK", MenuStore::NoDomainSelector,
              ParameterId{0}, 0, false, MenuGroup::Arp, 0.0, 0.0, 1.0),
    menu_item(MenuGroup::Arp, "DIRECTION", MenuStore::PerSideScalar,
              ParameterId::keyboard_arp_direction, 0, false, MenuGroup::Arp, 0.0, 3.0, 1.0),
    menu_item(MenuGroup::Arp, "VARIATION", MenuStore::PerSideScalar,
              ParameterId::keyboard_arp_variation, 0, false, MenuGroup::Arp, 0.0, 2.0, 1.0),
    menu_item(MenuGroup::Arp, "INTERVAL", MenuStore::PerSideScalar,
              ParameterId::keyboard_arp_interval, 0, false, MenuGroup::Arp, 0.0, 12.0, 1.0),
    menu_item(MenuGroup::Arp, "RHYTHM", MenuStore::NoDomainSelector,
              ParameterId{0}, 1, false, MenuGroup::Arp, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Arp, "LENGTH", MenuStore::PerSideScalar,
              ParameterId::keyboard_arp_length, 0, false, MenuGroup::Arp, 1.0, 16.0, 1.0),

    // ---- Seq page (manual L866-910) ----
    menu_item(MenuGroup::Seq, "RUN", MenuStore::PerSideScalar,
              ParameterId::keyboard_seq_run, 0, false, MenuGroup::Seq, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Seq, "LENGTH", MenuStore::PerSideScalar,
              ParameterId::keyboard_seq_length, 0, false, MenuGroup::Seq, 2.0, 16.0, 1.0),
    menu_item(MenuGroup::Seq, "CLOCK", MenuStore::NoDomainSelector,
              ParameterId{0}, 2, false, MenuGroup::Seq, 0.0, 0.0, 1.0),
    menu_item(MenuGroup::Seq, "DIRECTION", MenuStore::PerSideScalar,
              ParameterId::keyboard_seq_direction, 0, false, MenuGroup::Seq, 0.0, 3.0, 1.0),
    menu_item(MenuGroup::Seq, "CV OUTPUT", MenuStore::PerSideScalar,
              ParameterId::keyboard_seq_cv_output, 0, false, MenuGroup::Seq, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Seq, "RHYTHM", MenuStore::NoDomainSelector,
              ParameterId{0}, 3, false, MenuGroup::Seq, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Seq, "RHYTHM LENGTH", MenuStore::PerSideScalar,
              ParameterId::keyboard_seq_rhythm_length, 0, false, MenuGroup::Seq, 1.0, 8.0, 1.0),
    menu_item(MenuGroup::Seq, "EDITOR", MenuStore::NonScalar,
              ParameterId{0}, 0, false, MenuGroup::Seq, 1.0, 16.0, 1.0),

    // ---- Preset page (manual L1041-1047) ----
    menu_item(MenuGroup::Preset, "PRESET A", MenuStore::PresetAction,
              ParameterId{0}, 0, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "PRESET B", MenuStore::PresetAction,
              ParameterId{0}, 1, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "PRESET C", MenuStore::PresetAction,
              ParameterId{0}, 2, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "PRESET D", MenuStore::PresetAction,
              ParameterId{0}, 3, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "LOAD", MenuStore::PresetAction,
              ParameterId{0}, 0, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "SAVE", MenuStore::PresetAction,
              ParameterId{0}, 1, false, MenuGroup::Preset, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Preset, "INITIALISE", MenuStore::PresetAction,
              ParameterId{0}, 2, false, MenuGroup::Preset, 0.0, 0.0, 0.0),

    // ---- Calibration page (manual L1048-1093) ----
    menu_item(MenuGroup::Calibration, "V/OCT OUT CAL", MenuStore::GlobalParam,
              ParameterId::keyboard_calibration_v_oct, 0, false, MenuGroup::Calibration, 0.0, 1.0, 0.01),
    menu_item(MenuGroup::Calibration, "PRESSURE OUT CAL", MenuStore::GlobalParam,
              ParameterId::keyboard_calibration_pressure, 0, false, MenuGroup::Calibration, 0.0, 1.0, 0.01),
    menu_item(MenuGroup::Calibration, "INIT CALIBRATION", MenuStore::CalibrationAction,
              ParameterId{0}, 0, false, MenuGroup::Calibration, 0.0, 0.0, 0.0),
    menu_item(MenuGroup::Calibration, "DAC VREF SOURCE", MenuStore::GlobalParam,
              ParameterId::keyboard_dac_vref, 0, false, MenuGroup::Calibration, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Calibration, "TOUCH THRESHOLD", MenuStore::GlobalParam,
              ParameterId::keyboard_touch_threshold, 0, false, MenuGroup::Calibration, 0.0, 1024.0, 1.0),
    menu_item(MenuGroup::Calibration, "RELEASE THRESHOLD", MenuStore::GlobalParam,
              ParameterId::keyboard_release_threshold, 0, false, MenuGroup::Calibration, 0.0, 1024.0, 1.0),
    menu_item(MenuGroup::Calibration, "PRESSURE MINIMUM", MenuStore::GlobalParam,
              ParameterId::keyboard_pressure_min, 0, false, MenuGroup::Calibration, 0.0, 1024.0, 1.0),
    menu_item(MenuGroup::Calibration, "PRESSURE MAXIMUM", MenuStore::GlobalParam,
              ParameterId::keyboard_pressure_max, 0, false, MenuGroup::Calibration, 0.0, 1024.0, 1.0),
    menu_item(MenuGroup::Calibration, "MPR121 CHARGE", MenuStore::GlobalParam,
              ParameterId::keyboard_mpr121_charge, 0, false, MenuGroup::Calibration, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Calibration, "MPR121 DISCHARGE", MenuStore::GlobalParam,
              ParameterId::keyboard_mpr121_discharge, 0, false, MenuGroup::Calibration, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Calibration, "DEBOUNCE", MenuStore::GlobalParam,
              ParameterId::keyboard_debounce, 0, false, MenuGroup::Calibration, 0.0, 255.0, 1.0),
    menu_item(MenuGroup::Calibration, "ENCODER DIRECTION", MenuStore::GlobalParam,
              ParameterId::keyboard_encoder_direction, 0, false, MenuGroup::Calibration, 0.0, 1.0, 1.0),
    menu_item(MenuGroup::Calibration, "SAVE CALIBRATION", MenuStore::CalibrationAction,
              ParameterId{0}, 1, false, MenuGroup::Calibration, 0.0, 0.0, 0.0),
};
inline constexpr std::uint32_t kMenuItemCount =
    static_cast<std::uint32_t>(sizeof(kMenuItems) / sizeof(kMenuItems[0]));

// ---------------------------------------------------------------------------
// Side-context write routing — the contract the negative test guards.
// ---------------------------------------------------------------------------

// Write to the per-side scalar bank for a (mode, side). bank 0 (single/twin, or
// split-left) writes parameters[id]; bank 1 (split-right) writes
// keyboardScalarRight[index]. A "menu path" and the "MIDI/live path" MUST both
// funnel through this, so a split right-side parameter lands in the same place
// regardless of which surface edited it.
inline void write_side_scalar_value(DeviceStateV1& state, ParameterId id, double value,
                                    KeyboardMode mode, KeyboardSide side) noexcept {
  const std::uint8_t bank = side_bank(mode, side);
  if (bank == 0u) {
    state.parameters[static_cast<IdValue>(id)] = value;
  } else {
    const std::int32_t idx = keyboard_scalar_index(id);
    if (idx >= 0) state.keyboardScalarRight[idx] = value;
  }
}

// Read back the same per-side scalar through the bank.
inline double read_side_scalar_value(const DeviceStateV1& state, ParameterId id,
                                     KeyboardMode mode, KeyboardSide side) noexcept {
  const std::uint8_t bank = side_bank(mode, side);
  if (bank == 0u) return state.parameters[static_cast<IdValue>(id)];
  const std::int32_t idx = keyboard_scalar_index(id);
  return (idx >= 0) ? state.keyboardScalarRight[idx] : 0.0;
}

// ---------------------------------------------------------------------------
// MenuStore dispatch — the reach/side-routing contract.
// ---------------------------------------------------------------------------

// Whether a menu item maps to a single editable scalar value (as opposed to a
// submenu entry or an action row). Only these are value-edited; the rest act on
// click/hold (submenu enter, preset/calibration action).
//
// A SUBMENU entry is not a scalar even though it carries a ParameterId store type:
// it navigates on click, it never maps to a value. Excluding it here keeps the
// no-duplicate-control guard from counting two submenu entries (e.g. ARPEGGIATOR
// and SEQUENCER, both with a placeholder id) as a duplicate editable scalar.
inline bool menu_item_is_scalar(const KeyboardMenuItem& i) noexcept {
  if (i.submenu) return false;
  return i.store == MenuStore::PerSideScalar || i.store == MenuStore::GlobalParam ||
         i.store == MenuStore::NoDomainSelector;
}

// A no-domain clock/rhythm selector stores in keyboardClockSelectors[bank][slot].
// slot is the item's subIndex; bank 0 slots are (arp_clock, arp_rhythm,
// seq_clock, seq_rhythm) and bank 1 mirrors into keyboardClockSelectorsR.
inline void write_no_domain_selector(DeviceStateV1& state, std::uint8_t slot,
                                     double value, KeyboardMode mode,
                                     KeyboardSide side) noexcept {
  const std::uint8_t bank = side_bank(mode, side);
  if (bank == 0u) state.keyboardClockSelectors[slot] = static_cast<std::uint8_t>(value);
  else state.keyboardClockSelectorsR[slot] = static_cast<std::uint8_t>(value);
}

inline double read_no_domain_selector(const DeviceStateV1& state, std::uint8_t slot,
                                      KeyboardMode mode, KeyboardSide side) noexcept {
  const std::uint8_t bank = side_bank(mode, side);
  return (bank == 0u) ? static_cast<double>(state.keyboardClockSelectors[slot])
                      : static_cast<double>(state.keyboardClockSelectorsR[slot]);
}

// Write one scalar menu item through its store. PerSideScalar funnels through
// write_side_scalar_value — the SAME choke the MIDI/live path uses — never to the
// global bank for a split right side. A menu path that instead wrote
// parameters[id] for a split-right per-side param would diverge from the MIDI
// path; that is the negative test in test_keyboard_menu.cpp.
inline void write_item_value(DeviceStateV1& state, const KeyboardMenuItem& item,
                             double value, KeyboardMode mode, KeyboardSide side) noexcept {
  switch (item.store) {
    case MenuStore::PerSideScalar:
      write_side_scalar_value(state, item.scalarId, value, mode, side);
      return;
    case MenuStore::GlobalParam:
      state.parameters[static_cast<IdValue>(item.scalarId)] = value;
      return;
    case MenuStore::NoDomainSelector:
      write_no_domain_selector(state, item.subIndex, value, mode, side);
      return;
    case MenuStore::NonScalar:
    case MenuStore::GlobalSettings:
    case MenuStore::PresetAction:
    case MenuStore::CalibrationAction:
      return;  // no single scalar value to write (action/submenu/composite)
  }
}

inline double read_item_value(const DeviceStateV1& state, const KeyboardMenuItem& item,
                              KeyboardMode mode, KeyboardSide side) noexcept {
  switch (item.store) {
    case MenuStore::PerSideScalar:
      return read_side_scalar_value(state, item.scalarId, mode, side);
    case MenuStore::GlobalParam:
      return state.parameters[static_cast<IdValue>(item.scalarId)];
    case MenuStore::NoDomainSelector:
      return read_no_domain_selector(state, item.subIndex, mode, side);
    case MenuStore::NonScalar:
    case MenuStore::GlobalSettings:
    case MenuStore::PresetAction:
    case MenuStore::CalibrationAction:
      return 0.0;  // no single scalar value to read
  }
}

// How many rows a page has (reachability: Main has top-level + submenu entries;
// the submenu pages hold their own rows).
inline std::uint32_t menu_page_row_count(MenuGroup page) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < kMenuItemCount; ++i) {
    if (kMenuItems[i].page == page) ++n;
  }
  return n;
}

// Clamp a value into an item's edit window and round to its (PROVISIONAL) step.
// A selector uses its option-count range; the step is a labelled placeholder
// (FINDINGS) and no test asserts it as design truth.
inline double clamp_item_value(const KeyboardMenuItem& item, double value) noexcept {
  if (value < item.min) value = item.min;
  if (value > item.max) value = item.max;
  return value;
}

// ---------------------------------------------------------------------------
// Menu navigation — produces the pure-data display snapshot (design/00 Decis ①).
// ---------------------------------------------------------------------------
//
// The navigation semantics are a RENDERING decision (PROVISIONAL, FINDINGS): the
// manual describes what the menu does, not the encoder's exact detent/click
// choreography. This engine exposes the structural state (page, cursor, editing,
// calibration gate); it does not guess a UX the manual leaves open.
class KeyboardMenuNav {
 public:
  explicit KeyboardMenuNav(DeviceStateV1& state) : state_(state) {}

  void set_context(KeyboardMode mode, KeyboardSide side) noexcept {
    mode_ = mode;
    side_ = side;
  }

  // Enter the calibration gate (the hold-while-boot path, manual L1048-1050).
  void enter_calibration() noexcept {
    page_ = MenuGroup::Calibration;
    cursorRow_ = 0;
    calibration_ = true;
  }

  // Scope the cursor to the current page (wraps).
  void rotate(int delta) noexcept {
    const std::uint32_t rows = menu_page_row_count(page_);
    if (rows == 0u) return;
    if (editing_) {
      const std::uint32_t r = cursorRow_ % rows;
      const KeyboardMenuItem& item = kMenuItems[index_of_row(page_, r)];
      if (menu_item_is_scalar(item)) {
        const double next = clamp_item_value(item, pendingValue_ + delta * item.step);
        set_pending(item, next);
      }
      return;
    }
    std::int32_t r = static_cast<std::int32_t>(cursorRow_) + delta;
    r %= static_cast<std::int32_t>(rows);
    if (r < 0) r += static_cast<std::int32_t>(rows);
    cursorRow_ = static_cast<std::uint8_t>(r);
  }

  // Click: submenu row enters its page; a scalar row starts an edit.
  void click() noexcept {
    const std::uint32_t rows = menu_page_row_count(page_);
    if (rows == 0u) return;
    const std::uint32_t r = cursorRow_ % rows;
    const KeyboardMenuItem& item = kMenuItems[index_of_row(page_, r)];
    if (item.submenu) {
      page_ = item.submenuPage;
      cursorRow_ = 0;
    } else if (menu_item_is_scalar(item)) {
      editing_ = true;
      set_pending(item, read_item_value(state_, item, mode_, side_));
    }
  }

  // Hold: commit an active edit, else leave a submenu back to Main.
  void hold() noexcept {
    if (editing_) {
      const std::uint32_t r = cursorRow_ % menu_page_row_count(page_);
      if (menu_page_row_count(page_) > 0u) {
        const KeyboardMenuItem& item = kMenuItems[index_of_row(page_, r)];
        if (menu_item_is_scalar(item)) write_item_value(state_, item, pendingValue_, mode_, side_);
      }
      editing_ = false;
      return;
    }
    if (page_ != MenuGroup::Main) {
      page_ = MenuGroup::Main;
      cursorRow_ = 0;
    }
  }

  // Fill the pure-data display struct (keyboard_display.h). The struct IS the
  // render contract; there is no query interface back into this engine.
  KeyboardDisplay snapshot(const char* title, const char* value) const noexcept {
    KeyboardDisplay d;
    d.segment = static_cast<std::uint8_t>(page_);
    d.cursorRow = cursorRow_;
    d.rowCount = menu_page_row_count(page_);
    d.editing = editing_;
    d.isCalibration = calibration_;
    d.title = title;
    d.value = value;
    return d;
  }

  MenuGroup page() const noexcept { return page_; }
  std::uint8_t cursor_row() const noexcept { return cursorRow_; }
  bool editing() const noexcept { return editing_; }
  double pending_value() const noexcept { return pendingValue_; }

 private:
  // First array position of a page's rows (rows within a page are contiguous).
  static std::uint32_t index_of_row(MenuGroup page, std::uint32_t r) noexcept {
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < kMenuItemCount; ++i) {
      if (kMenuItems[i].page == page) {
        if (seen == r) return i;
        ++seen;
      }
    }
    return 0;
  }

  void set_pending(const KeyboardMenuItem& item, double value) noexcept {
    pendingValue_ = clamp_item_value(item, value);
  }

  DeviceStateV1& state_;
  KeyboardMode mode_ = KeyboardMode::Single;
  KeyboardSide side_ = KeyboardSide::Left;
  MenuGroup page_ = MenuGroup::Main;
  std::uint8_t cursorRow_ = 0;
  bool editing_ = false;
  bool calibration_ = false;
  double pendingValue_ = 0.0;
};

}  // namespace lunar24::core
