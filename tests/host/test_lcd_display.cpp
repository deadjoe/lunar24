// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The panel LCDs: 3 s bright backlight after the last change, fade back; ping-pong marquee.

#include "mini_test.h"

#include <host/lcd_display.h>

using namespace lunar24;

int main() {
  {
    host::LcdBacklight b;
    CHECK(!b.show("OCT +0", 0.0));  // what the screen shows at start is not a change
    CHECK_EQ(b.glow(0.1), 0.0);
    CHECK(!b.animating(0.1));
    CHECK(!b.show("OCT +0", 1.0));
    CHECK(b.show("OCT +1", 2.0));
    CHECK_EQ(b.glow(2.0), 1.0);
    CHECK_EQ(b.glow(4.9), 1.0);
    CHECK(b.show("OCT +2", 4.9));   // a new change restarts the hold
    CHECK_EQ(b.glow(7.8), 1.0);
    CHECK(b.glow(8.1) < 1.0 && b.glow(8.1) > 0.0);  // fading
    CHECK(b.animating(8.1));
    CHECK_EQ(b.glow(8.4), 0.0);
    CHECK(!b.animating(8.4));
  }
  {
    using host::lcd_marquee_offset;
    CHECK_EQ(lcd_marquee_offset(0.0, 5.0), 0.0);       // fits: never moves
    CHECK_EQ(lcd_marquee_offset(-3.0, 5.0), 0.0);
    const double overflow = 50.0;                       // 2 s of sliding each way
    CHECK_EQ(lcd_marquee_offset(overflow, 1.0), 0.0);   // pause at the start
    CHECK(std::fabs(lcd_marquee_offset(overflow, 2.5) - 25.0) < 1e-9);
    CHECK_EQ(lcd_marquee_offset(overflow, 4.0), overflow);  // pause at the end
    CHECK(std::fabs(lcd_marquee_offset(overflow, 6.0) - 25.0) < 1e-9);  // sliding back
    CHECK_EQ(lcd_marquee_offset(overflow, 7.0 + 0.5), 0.0);  // next cycle's pause
    for (double t = 0.0; t < 30.0; t += 0.013) {
      const double o = lcd_marquee_offset(overflow, t);
      CHECK(o >= 0.0 && o <= overflow);
    }
  }
  return test::finish("test_lcd_display");
}
