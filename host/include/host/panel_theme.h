// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_theme.h — the "moon" look: graphite panel, moon-dust text, one muted accent
// colour per function family. Shared by the IGraphics editor and the SVG preview.

#pragma once

#include <cstdint>

#include <host/panel_ui_layout.h>

namespace lunar24::host::theme {

struct Rgb { std::uint8_t r, g, b; };

constexpr Rgb kBackground{29, 31, 35};
constexpr Rgb kCard{40, 43, 49};
constexpr Rgb kCardEdge{58, 62, 70};
constexpr Rgb kText{216, 212, 203};      // moon dust
constexpr Rgb kTextDim{141, 138, 132};
constexpr Rgb kKnobBody{22, 24, 27};
constexpr Rgb kKnobTrack{64, 68, 76};
constexpr Rgb kJackRing{154, 151, 143};
constexpr Rgb kJackHole{11, 12, 14};
constexpr Rgb kPlate{52, 55, 62};
constexpr Rgb kPlateLit{92, 70, 74};

inline Rgb accent(Accent a) {
  switch (a) {
    case Accent::Drone: return {214, 164, 90};       // amber
    case Accent::Vco: return {98, 181, 176};         // teal
    case Accent::Filter: return {161, 145, 217};     // violet
    case Accent::Modulation: return {143, 191, 128}; // sage
    case Accent::Keyboard: return {217, 140, 140};   // rose
    case Accent::Neutral: break;
  }
  return {185, 180, 170};
}

// Patch cable colours, cycled per cable.
constexpr Rgb kCables[6] = {{224, 108, 92}, {236, 190, 90}, {110, 190, 230},
                            {150, 210, 120}, {200, 140, 220}, {235, 235, 235}};

// Knob sweep: 270 degrees, from 7:30 (min) to 4:30 (max), clockwise. Angles in degrees,
// 0 = 12 o'clock, clockwise positive (the IGraphics convention).
constexpr double kKnobMinDeg = -135.0;
constexpr double kKnobMaxDeg = 135.0;

}  // namespace lunar24::host::theme
