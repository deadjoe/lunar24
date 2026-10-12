// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// lcd_display.h — how the panel's small LCDs behave, framework-free (UI thread only).
//
// Backlight: like a hardware LCD, the screen lights up brighter whenever what it shows
// changes, stays bright for kLcdHoldSeconds after the LAST change (a new change restarts
// the hold), then fades back over kLcdFadeSeconds. The first content shown is not a change.
//
// Marquee: text wider than its line slides slowly to show its end, pauses, and slides
// back, forever; text that fits stays still.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace lunar24::host {

inline constexpr double kLcdHoldSeconds = 3.0;   // owner's choice (2026-10-12)
inline constexpr double kLcdFadeSeconds = 0.4;   // tuned by eye
inline constexpr double kLcdScrollSpeed = 25.0;  // design px per second, tuned by eye
inline constexpr double kLcdScrollPause = 1.5;   // seconds at each end, tuned by eye

class LcdBacklight {
 public:
  // Feed what the screen shows now. Returns true when it differs from the last call.
  bool show(const std::string& content, double now) {
    if (seen_ && content == content_) return false;
    const bool first = !seen_;
    seen_ = true;
    content_ = content;
    if (!first) litAt_ = now;
    return !first;
  }
  // 1 = fully bright, 0 = normal backlight.
  double glow(double now) const {
    if (litAt_ < 0.0) return 0.0;
    const double age = now - litAt_;
    if (age < kLcdHoldSeconds) return 1.0;
    return std::clamp(1.0 - (age - kLcdHoldSeconds) / kLcdFadeSeconds, 0.0, 1.0);
  }
  // Still changing brightness: the screen needs redrawing.
  bool animating(double now) const {
    return litAt_ >= 0.0 && now - litAt_ < kLcdHoldSeconds + kLcdFadeSeconds;
  }

 private:
  std::string content_;
  double litAt_ = -1.0;
  bool seen_ = false;
};

// How far (design px, >= 0) to shift text left that is `overflow` px wider than its line,
// `t` seconds after it first appeared: pause, slide to the end, pause, slide back.
inline double lcd_marquee_offset(double overflow, double t) {
  if (!(overflow > 0.0) || !(t > 0.0)) return 0.0;
  const double slide = overflow / kLcdScrollSpeed;
  const double period = 2.0 * (kLcdScrollPause + slide);
  double p = std::fmod(t, period);
  if (p < kLcdScrollPause) return 0.0;
  p -= kLcdScrollPause;
  if (p < slide) return p * kLcdScrollSpeed;
  p -= slide;
  if (p < kLcdScrollPause) return overflow;
  p -= kLcdScrollPause;
  return std::max(0.0, overflow - p * kLcdScrollSpeed);
}

}  // namespace lunar24::host
