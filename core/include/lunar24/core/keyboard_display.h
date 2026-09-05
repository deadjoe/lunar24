// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P4-⑤ bottom-display content model (design/00 §P4-⑤, @Claude msg 87862933).
//
// @Claude Decis ①: this is a PURE DATA STRUCT, and the core does NOT expose a
// query interface. The menu engine (keyboard_menu.h) owns a KeyboardDisplay and
// fills it as the user navigates / edits; a rendering layer reads it. There is no
// getter, no poll surface, no callback — the contract is the struct's fields
// written by the menu and read by a renderer. A query interface would leak the
// menu's internals to the UI and let the UI drive the menu instead of the encoder
// driving it, which the manual never does.
//
// Fixed-size, no allocation. Text is a label POINTER into static storage (the
// menu's label table), never a copied buffer, so a renderer that needs its own
// copy owns that decision. The numeric fields are the structured state a
// renderer needs.

#pragma once

#include <cstdint>

namespace lunar24::core {

// Fixed capacity for the display's text labels (a title line and a value line).
// Small: the Solar 42N's bottom display is a compact character module (manual
// panel). A renderer copies these into its own buffer with this as an upper bound.
inline constexpr std::uint8_t kDisplayTitleBytes = 32u;
inline constexpr std::uint8_t kDisplayValueBytes = 32u;

// One snapshot of the bottom display. Pure data: no methods that query the menu,
// no references into the menu. The renderer reads it directly and fully.
struct KeyboardDisplay {
  std::uint8_t segment = 0;      // which menu page/group is showing (MenuGroup id)
  std::uint8_t cursorRow = 0;    // highlighted row, 0-based, within the page
  std::uint8_t rowCount = 0;     // rows on the current page
  bool editing = false;          // an edit is active (not just navigating)
  bool isCalibration = false;    // in the calibration gate (hold-while-boot)

  // Title / value text. These are label-pointers into static storage (the menu's
  // label table), NOT owned buffers — see the file note above.
  const char* title = nullptr;   // current page / prompt label
  const char* value = nullptr;   // current editing value label / text

  // A plain fixed character copy is sometimes handy for a renderer that cannot
  // chase pointers; the menu fills these on request. They are NOT the source of
  // truth (the label pointers are) — a renderer uses one or the other.
  char titleText[kDisplayTitleBytes] = {};
  char valueText[kDisplayValueBytes] = {};
};

}  // namespace lunar24::core
