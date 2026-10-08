// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_theme.h — the panel look, following the hardware's visual language: warm light
// panel, black frames and title tabs, section-coloured knob caps, hex-nut jacks. Shared by
// the IGraphics editor and the SVG preview.

#pragma once

#include <cstdint>

#include <host/panel_ui_layout.h>

namespace lunar24::host::theme {

struct Rgb { std::uint8_t r, g, b; };

inline constexpr Rgb rgb(std::uint32_t hex) {
  return {static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex)};
}

constexpr Rgb kPanel{233, 224, 210};       // warm light panel (the default face)

// The one-pixel gap behind the panel on Windows (WM_ERASEBKGND). The KEYBOARD MENU
// face row updates this on the UI thread. Not machine state; starts as warm cream.
inline Rgb& windowGap() {
  static Rgb color = kPanel;
  return color;
}
constexpr Rgb kInk{12, 10, 10};            // frames, tabs, text
constexpr Rgb kRed{203, 32, 38};           // "env", output markers
constexpr Rgb kSkirt{52, 52, 52};          // knob skirts / ticks
constexpr Rgb kPointer{245, 245, 245};
constexpr Rgb kNutLight{196, 196, 196};    // jack hex nut
constexpr Rgb kNutDark{120, 120, 120};
constexpr Rgb kHole{8, 8, 8};
constexpr Rgb kLedOn{235, 60, 50};
constexpr Rgb kLedOff{160, 160, 160};
constexpr Rgb kAmber{235, 170, 0};
constexpr Rgb kMenuBg{30, 30, 34};
constexpr Rgb kMenuText{230, 228, 220};

inline Rgb cap(Cap c) {
  switch (c) {
    case Cap::Teal: return {0, 94, 122};
    case Cap::Green: return {0, 94, 59};
    case Cap::Orange: return {235, 140, 64};
    case Cap::Red: return {217, 54, 43};
    case Cap::Grey: return {196, 196, 196};
    case Cap::Dark: return {26, 26, 26};
    case Cap::Black: break;
  }
  return {0, 0, 0};
}

// Patch cable colours, cycled per cable.
constexpr Rgb kCables[6] = {{224, 70, 50}, {240, 180, 30}, {40, 130, 210},
                            {70, 160, 80}, {150, 80, 190}, {30, 30, 30}};

// Knob sweep: 300 degrees (7 o'clock to 5 o'clock). 0 = 12 o'clock, clockwise positive.
constexpr double kKnobMinDeg = -150.0;
constexpr double kKnobMaxDeg = 150.0;

}  // namespace lunar24::host::theme
