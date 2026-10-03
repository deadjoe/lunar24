// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_art.h — the static panel artwork, drawn the same way by the app (IGraphics) and by
// the SVG preview tool. Framework-free: the caller supplies a small drawing "sink".
//
// The data comes from two generated headers:
//   panel_art.generated.h  — labels, frames, title tabs, icons, LEDs and dotted rules taken
//                            from the official panel drawing (tools/gen_panel_art.py)
//   panel_logo.generated.h — the Lunar 24 name plates, as outlines of the Saira font
//                            (tools/gen_panel_logo.py)
//
// Sink interface (all coordinates in 2400 x 1552 panel units, colours 0xRRGGBB):
//   void fillRect(float x0, float y0, float x1, float y1, std::uint32_t rgb, float radius);
//   void fillCircle(float cx, float cy, float r, std::uint32_t rgb);
//   void moveTo(float x, float y); void lineTo(float x, float y); void closePath();
//   void markHole();   // the sub-path just closed is a hole (NanoVG has no even-odd rule)
//   void fillPath(std::uint32_t rgb, bool evenOdd);           // fills and clears the path
//   void strokePath(std::uint32_t rgb, float width);          // strokes and clears the path
//   void text(float x, float y, float size, std::uint32_t rgb, bool vertical, const char* s);
//   void circle(float cx, float cy, float r);                  // adds a circle sub-path
//   void fillGrad(const Grad& g);                              // fills and clears the path
//   void strokeGrad(const Grad& g, float width);               // strokes and clears the path
//
// Light falls from the top left: every raised part gets a soft drop shadow to the lower
// right, a top-lit gradient and a small highlight. Shading amounts are tuned by eye.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace lunar24::host::art {

struct Text { float x, y, size; std::uint32_t rgb; bool vertical; const char* text; };
struct Box { float x0, y0, x1, y1; };
struct Frame { std::uint32_t first, count; };
// A vector shape: `subCount` sub-paths starting at `firstSub`; filled and/or stroked.
struct SubPath { std::uint32_t first, count; bool closed, hole; };
enum ShapeFlag : std::uint8_t { kFill = 1, kStroke = 2, kEvenOdd = 4 };
struct Shape { std::uint32_t firstSub, subCount, fill, stroke; float width; std::uint8_t flags; };
struct Dot { float x, y, r; std::uint32_t rgb; };
// Indicator LEDs printed on the panel (drawLed; the app lights them from the engine).
struct Led { float x, y, r; std::uint32_t rgb; };
// A two-colour paint with alpha. Linear: from (x0, y0) to (x1, y1). Radial: centre (x0, y0),
// colour c0 up to radius x1, blending to c1 at radius y1. Solid: c0 == c1.
struct Grad {
  bool radial;
  float x0, y0, x1, y1;
  std::uint32_t c0, c1;
  float a0, a1;
};

}  // namespace lunar24::host::art

#include <host/panel_art.generated.h>
#include <host/panel_logo.generated.h>

namespace lunar24::host::art {

inline constexpr std::uint32_t kPanelRgb = 0xe9e0d2, kKeybedRgb = 0x282828, kInkRgb = 0x0c0a0a;
inline constexpr std::uint32_t kKeybedMarkRgb = 0xf2f2f2;

// An LED that is off: a dark tint of its colour.
inline std::uint32_t unlit(std::uint32_t rgb) {
  auto ch = [rgb](int shift) { return std::uint32_t(((rgb >> shift) & 0xff) * 0.45 + 40) & 0xff; };
  return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Blend two colours: t = 0 gives a, t = 1 gives c.
inline std::uint32_t mix(std::uint32_t a, std::uint32_t c, float t) {
  t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
  auto ch = [&](int shift) {
    const float lo = float((a >> shift) & 0xff), hi = float((c >> shift) & 0xff);
    return std::uint32_t(lo + (hi - lo) * t + 0.5f) & 0xff;
  };
  return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
// An LED at brightness b (0 = unlit .. 1 = full colour).
inline std::uint32_t litLed(std::uint32_t rgb, float b) { return mix(unlit(rgb), rgb, b); }

// Mix towards white (k > 0) or black (k < 0).
inline std::uint32_t shade(std::uint32_t rgb, float k) {
  auto ch = [rgb, k](int shift) {
    const float v = float((rgb >> shift) & 0xff);
    const float o = k >= 0 ? v + (255.f - v) * k : v * (1.f + k);
    return std::uint32_t(std::lround(std::fmin(255.f, std::fmax(0.f, o))));
  };
  return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
inline Grad solid(std::uint32_t rgb, float a = 1.f) { return {false, 0, 0, 0, 1, rgb, rgb, a, a}; }
inline Grad vertical(float y0, float y1, std::uint32_t top, std::uint32_t bottom, float a0 = 1.f, float a1 = 1.f) {
  return {false, 0, y0, 0, y1, top, bottom, a0, a1};
}
inline Grad radialGrad(float cx, float cy, float r0, float r1, std::uint32_t c0, float a0, std::uint32_t c1, float a1) {
  return {true, cx, cy, r0, r1, c0, c1, a0, a1};
}

// A soft round shadow cast by a part of radius r standing `height` above the panel.
template <class Sink>
void dropShadow(Sink& s, float cx, float cy, float r, float height, float strength = 0.45f) {
  const float x = cx + height * 0.45f, y = cy + height * 0.8f, spread = r + height * 1.1f;
  s.circle(x, y, spread);
  s.fillGrad(radialGrad(x, y, r * 0.75f, spread, 0x000000, strength, 0x000000, 0.f));
}
// A domed disc: top-lit body, bevelled rim and a highlight towards the top left.
template <class Sink>
void dome(Sink& s, float cx, float cy, float r, std::uint32_t rgb, float relief = 0.35f) {
  s.circle(cx, cy, r);
  s.fillGrad(vertical(cy - r, cy + r, shade(rgb, relief), shade(rgb, -relief * 1.2f)));
  s.circle(cx, cy, r - 0.75f);
  s.strokeGrad(vertical(cy - r, cy + r, 0xffffff, 0x000000, 0.35f, 0.3f), 1.5f);
  const float hx = cx - r * 0.28f, hy = cy - r * 0.34f, hr = r * 0.55f;
  s.circle(hx, hy, hr);
  s.fillGrad(radialGrad(hx, hy, 0.f, hr, 0xffffff, 0.32f, 0xffffff, 0.f));
}

// An indicator LED at brightness b (0 = off .. 1 = full): a small clear dome sunk in the panel.
// Lit, the light comes from the die in the middle, so the lens is pale and hot at the centre and
// saturated towards the edge, and a little of it spills onto the panel around the bezel. The glass
// keeps its highlight whether lit or not. Amounts tuned by eye.
template <class Sink>
void drawLed(Sink& s, float cx, float cy, float r, std::uint32_t rgb, float b) {
  b = b < 0.f ? 0.f : (b > 1.f ? 1.f : b);
  if (b > 0.01f) {  // spill: a wide faint ring and a tighter brighter one read as a smooth falloff
    s.circle(cx, cy, r * 2.8f);
    s.fillGrad(radialGrad(cx, cy, r, r * 2.8f, rgb, 0.16f * b, rgb, 0.f));
    s.circle(cx, cy, r * 1.7f);
    s.fillGrad(radialGrad(cx, cy, r, r * 1.7f, rgb, 0.30f * b, rgb, 0.f));
  }
  // The bezel: a recessed hole, shadowed at the top, catching light at the bottom.
  s.circle(cx, cy, r + 1.5f);
  s.fillGrad(vertical(cy - r, cy + r, 0x1c1614, 0x857a6e));
  // The lens.
  const std::uint32_t off = unlit(rgb);
  const std::uint32_t core = mix(shade(off, 0.08f), mix(rgb, 0xfff4e6, 0.6f), b);
  const std::uint32_t edge = mix(shade(off, -0.35f), shade(rgb, -0.12f), b);
  s.circle(cx, cy, r);
  s.fillGrad(radialGrad(cx, cy + r * 0.05f, r * 0.18f, r, core, 1.f, edge, 1.f));
  // The glass highlight, towards the light (top left).
  const float hx = cx - r * 0.33f, hy = cy - r * 0.38f, hr = r * 0.42f;
  s.circle(hx, hy, hr);
  s.fillGrad(radialGrad(hx, hy, 0.f, hr, 0xffffff, 0.55f, 0xffffff, 0.f));
}

template <class Sink>
void drawShapes(Sink& s, const float* pts, const SubPath* subs, const Shape* shapes, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    const Shape& sh = shapes[i];
    for (int pass = 0; pass < 2; ++pass) {
      const bool fill = pass == 0;
      if (fill ? !(sh.flags & kFill) : !(sh.flags & kStroke)) continue;
      for (std::uint32_t k = 0; k < sh.subCount; ++k) {
        const SubPath& sp = subs[sh.firstSub + k];
        for (std::uint32_t j = 0; j < sp.count; ++j) {
          const float x = pts[2 * (sp.first + j)], y = pts[2 * (sp.first + j) + 1];
          if (j == 0) s.moveTo(x, y);
          else s.lineTo(x, y);
        }
        if (sp.closed) s.closePath();
        if (fill && sp.hole) s.markHole();
      }
      if (fill) s.fillPath(sh.fill, (sh.flags & kEvenOdd) != 0);
      else s.strokePath(sh.stroke, sh.width);
    }
  }
}

// Marks printed on the keybed (drawn here: the official keybed is a bitmap). Positions
// measured from the official drawing.
template <class Sink>
void drawKeybedMarks(Sink& s) {
  const std::uint32_t c = kKeybedMarkRgb;
  auto tri = [&s](float cx, float tipY, float baseY, float halfW, float line) {  // outline triangle
    const float h = std::fabs(baseY - tipY), side = std::sqrt(h * h + halfW * halfW);
    const float r = halfW * h / (halfW + side);                   // inradius
    const float iy = baseY + (tipY < baseY ? -r : r);             // incentre
    const float k = (r - line) / r;                               // inner = outer scaled about it
    const float xs[3] = {cx, cx + halfW, cx - halfW}, ys[3] = {tipY, baseY, baseY};
    for (int pass = 0; pass < 2; ++pass) {
      const float m = pass == 0 ? 1.f : k;
      for (int i = 0; i < 3; ++i) {
        const float x = cx + (xs[i] - cx) * m, y = iy + (ys[i] - iy) * m;
        if (i == 0) s.moveTo(x, y);
        else s.lineTo(x, y);
      }
      s.closePath();
      if (pass == 1) s.markHole();
    }
  };
  // Outline triangles under plates 3 and 10.
  tri(700, 1380, 1463, 49, 9); s.fillPath(c, true);
  tri(1700, 1380, 1463, 49, 9); s.fillPath(c, true);
  // Octave arrows above the white buttons: down (left) and up (right).
  tri(1093, 1157, 1146, 7, 1.8f); s.fillPath(c, true);
  tri(1306, 1145, 1156, 7, 1.8f); s.fillPath(c, true);
  // Jack symbols. Left: clock, reset (lightning), gate. Right: gate, v/oct, pressure.
  for (int i = 0; i <= 24; ++i) {  // clock face
    const float a = 6.2831853f * float(i) / 24.f;
    if (i == 0) s.moveTo(925 + 6 * std::cos(a), 1146 + 6 * std::sin(a));
    else s.lineTo(925 + 6 * std::cos(a), 1146 + 6 * std::sin(a));
  }
  s.strokePath(c, 1.6f);
  s.moveTo(925, 1142); s.lineTo(925, 1146); s.lineTo(929, 1146); s.strokePath(c, 1.6f);
  s.moveTo(989, 1155); s.lineTo(984, 1163); s.lineTo(989, 1163); s.lineTo(984, 1171); s.strokePath(c, 1.8f);
  for (float x : {1048.f, 1350.f}) {  // gate pulse
    s.moveTo(x - 6, 1183); s.lineTo(x - 3, 1183); s.lineTo(x - 3, 1171); s.lineTo(x + 3, 1171); s.lineTo(x + 3, 1183);
    s.lineTo(x + 7, 1183);
    s.strokePath(c, 1.8f);
  }
  s.moveTo(1415, 1155); s.lineTo(1415, 1166); s.strokePath(c, 1.8f);  // v/oct: arrow onto a bar
  s.moveTo(1411, 1162); s.lineTo(1415, 1167); s.lineTo(1419, 1162); s.strokePath(c, 1.6f);
  s.moveTo(1409, 1170); s.lineTo(1421, 1170); s.strokePath(c, 1.8f);
  s.moveTo(1483, 1142); s.lineTo(1473, 1142); s.lineTo(1473, 1149); s.lineTo(1481, 1149);  // return arrow
  s.strokePath(c, 1.8f);
  s.moveTo(1478, 1146); s.lineTo(1482, 1149); s.lineTo(1478, 1152); s.strokePath(c, 1.6f);
  // Mounting screws along the keybed edges.
  for (float x : {412.f, 806.f, 1200.f, 1592.f, 1986.f}) {
    s.fillCircle(x, 1115, 4, 0xd0d0d0);
    s.fillCircle(x, 1477, 4, 0xd0d0d0);
  }
}

// ---- keyboard-area controls, drawn the same in the app and the preview ------------------

template <class Sink>
void drawPlate(Sink& s, float x0, float y0, float x1, float y1, bool lit) {
  s.moveTo(x0 + 3, y0 + 5); s.lineTo(x1 + 4, y0 + 5); s.lineTo(x1 + 4, y1 + 6); s.lineTo(x0 + 3, y1 + 6);
  s.closePath();
  s.fillGrad(solid(0x000000, 0.45f));  // the plate stands off the keybed
  s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y1); s.lineTo(x0, y1);
  s.closePath();
  s.fillGrad(vertical(y0, y1, lit ? 0xffd9a0 : 0xffffff, lit ? 0xe09a40 : 0xb4b4b0));  // brushed metal frame
  s.fillRect(x0 + 8.5f, y0 + 8.5f, x1 - 8.5f, y1 - 8.5f, 0x1c1a16, 0);
  int i = 0;
  for (float y = y0 + 14.f; y < y1 - 11.f; y += 12.f, ++i) {
    s.moveTo(x0 + 10.f, y);
    s.lineTo(x1 - 11.f, y);
    s.strokePath(lit ? (i % 2 ? 0xffd9a0 : 0xe8a860) : (i % 2 ? 0xfafafa : 0xbdbdbd), 4.f);
  }
}

template <class Sink>
void drawEncoder(Sink& s, float cx, float cy, bool active) {
  dropShadow(s, cx, cy, 44, 8);
  s.circle(cx, cy, 44);
  s.fillGrad(vertical(cy - 44, cy + 44, 0xe4e4e4, 0x8a8a8a));
  s.fillCircle(cx, cy, 36, 0x1a1a1a);
  s.fillCircle(cx, cy, 33, 0x333333);
  for (int i = 0; i < 24; ++i) {  // knurling
    const float a = 6.2831853f * float(i) / 24.f;
    s.moveTo(cx + 26 * std::cos(a), cy + 26 * std::sin(a));
    s.lineTo(cx + 33 * std::cos(a), cy + 33 * std::sin(a));
    s.strokePath(0x1a1a1a, 1.5f);
  }
  s.fillCircle(cx, cy, 23, 0x111111);
  dome(s, cx, cy, 21.5f, active ? 0xebaa00 : 0xdc3a2c, 0.3f);
}

template <class Sink>
void drawOctaveKey(Sink& s, float cx, float cy, bool hover) {
  dropShadow(s, cx, cy, 24, 5);
  s.fillCircle(cx, cy, 24, 0x141414);
  s.circle(cx, cy, 22.5f);
  s.fillGrad(vertical(cy - 22.5f, cy + 22.5f, hover ? 0xffecd0 : 0xffffff, hover ? 0xe0b880 : 0xc8c8c4));
}

// A rectangle with rounded corners as a path (r = corner radius; arcs as short polylines).
template <class Sink>
void roundRectPath(Sink& s, float x0, float y0, float x1, float y1, float r) {
  constexpr int kSteps = 5;
  const float cx[4] = {x1 - r, x1 - r, x0 + r, x0 + r}, cy[4] = {y0 + r, y1 - r, y1 - r, y0 + r};
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i <= kSteps; ++i) {
      const float a = 1.5707963f * (float(c) - 1.f + float(i) / kSteps);  // -90 deg .. 180 deg
      const float x = cx[c] + r * std::cos(a), y = cy[c] + r * std::sin(a);
      if (c == 0 && i == 0) s.moveTo(x, y);
      else s.lineTo(x, y);
    }
  }
  s.closePath();
}

// A DRONE VOICES key: a square keycap standing in a well in the panel. The cap's sides show
// below and to the right of its top (light from the top left), the top is slightly dished and
// catches light at its upper edge, and the LED window near the top is a small red lens that
// glows while the voice is open. Amounts tuned by eye.
template <class Sink>
void drawDroneKey(Sink& s, float x0, float y0, float x1, float y1, bool open, bool hover) {
  // Shadow cast on the panel, then the well the key sits in.
  for (int i = 4; i >= 1; --i) {  // stacked faint layers read as a soft-edged shadow
    const float g = float(i) * 2.f;
    roundRectPath(s, x0 + 2 - g * 0.3f, y0 + 4 - g * 0.3f, x1 + 2 + g, y1 + 4 + g, 6.f + g);
    s.fillGrad(solid(0x000000, 0.08f));
  }
  roundRectPath(s, x0, y0, x1, y1, 6.f);
  s.fillGrad(vertical(y0, y1, 0x0c0c0c, 0x2a2a2a));
  // The cap's sides (the skirt), lit from above: light at the top, dark towards the bottom.
  const float sx0 = x0 + 3, sy0 = y0 + 2, sx1 = x1 - 3, sy1 = y1 - 3;
  roundRectPath(s, sx0, sy0, sx1, sy1, 6.f);
  s.fillGrad(vertical(sy0, sy1, 0x3c3c3c, 0x0c0c0c));
  // The top face, set in from the skirt more at the bottom and right (the cap tapers).
  const float tx0 = sx0 + 6, ty0 = sy0 + 3, tx1 = sx1 - 8, ty1 = sy1 - 11;
  roundRectPath(s, tx0, ty0, tx1, ty1, 7.f);
  s.fillGrad(vertical(ty0, ty1, hover ? 0x646464 : 0x575757, hover ? 0x3c3c3c : 0x333333));
  // Dish: a soft darker hollow in the middle of the top.
  const float mx = (tx0 + tx1) / 2, my = (ty0 + ty1) / 2 + 3;
  s.circle(mx, my, (tx1 - tx0) * 0.55f);
  s.fillGrad(radialGrad(mx, my, 0.f, (tx1 - tx0) * 0.55f, 0x000000, 0.18f, 0x000000, 0.f));
  // Edges of the top: a highlight along the upper edge, a dark line along the lower one.
  s.moveTo(tx0 + 6, ty0 + 1); s.lineTo(tx1 - 6, ty0 + 1);
  s.strokeGrad(solid(0xffffff, 0.30f), 1.5f);
  s.moveTo(tx0 + 6, ty1 - 0.5f); s.lineTo(tx1 - 6, ty1 - 0.5f);
  s.strokeGrad(solid(0x000000, 0.45f), 1.5f);
  // LED window.
  const float cx = (tx0 + tx1) / 2, ly0 = ty0 + 6, ly1 = ty0 + 15;
  constexpr std::uint32_t kRed = 0xff2a1a;
  if (open) {  // light spilling onto the key top
    s.circle(cx, (ly0 + ly1) / 2, 18.f);
    s.fillGrad(radialGrad(cx, (ly0 + ly1) / 2, 6.f, 18.f, kRed, 0.30f, kRed, 0.f));
  }
  roundRectPath(s, cx - 9, ly0 - 1.5f, cx + 9, ly1 + 1.5f, 3.f);
  s.fillGrad(solid(0x0a0a0a));
  roundRectPath(s, cx - 7.5f, ly0, cx + 7.5f, ly1, 2.f);
  const std::uint32_t off = unlit(kRed);
  s.fillGrad(vertical(ly0, ly1, open ? mix(kRed, 0xfff0e0, 0.55f) : shade(off, 0.05f),
                      open ? shade(kRed, -0.1f) : shade(off, -0.4f)));
  s.moveTo(cx - 5, ly0 + 2); s.lineTo(cx + 5, ly0 + 2);  // glass highlight
  s.strokeGrad(solid(0xffffff, open ? 0.45f : 0.30f), 1.5f);
}

// The keyboard display: a backlit blue LCD behind glass, set in a raised black bezel. Drawn in
// two parts so the caller can put the text between them: drawDisplay (bezel and screen) and
// drawDisplayGlass (the reflection over the text). Amounts tuned by eye.
template <class Sink>
void drawDisplay(Sink& s, float x0, float y0, float x1, float y1) {
  roundRectPath(s, x0 - 5, y0 - 2, x1 + 9, y1 + 11, 6.f);  // shadow on the keybed
  s.fillGrad(solid(0x000000, 0.35f));
  roundRectPath(s, x0 - 7, y0 - 7, x1 + 7, y1 + 7, 5.f);    // the bezel, lit from above
  s.fillGrad(vertical(y0 - 7, y1 + 7, 0x505050, 0x0c0c0c));
  roundRectPath(s, x0 - 5.5f, y0 - 5.5f, x1 + 5.5f, y1 + 5.5f, 4.f);
  s.fillGrad(vertical(y0 - 5.5f, y1 + 5.5f, 0x2c2c2c, 0x161616));
  // The recess: shadowed at the top, catching light at the bottom.
  roundRectPath(s, x0 - 1.5f, y0 - 1.5f, x1 + 1.5f, y1 + 1.5f, 2.f);
  s.fillGrad(vertical(y0 - 1.5f, y1 + 1.5f, 0x000000, 0x3a3a3a));
  // The screen: brightest in the middle where the backlight is, falling off to the edges.
  s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y1); s.lineTo(x0, y1); s.closePath();
  s.fillGrad(vertical(y0, y1, 0x3341b8, 0x2a379e));
  const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rr = (x1 - x0) * 0.55f;
  s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y1); s.lineTo(x0, y1); s.closePath();
  s.fillGrad(radialGrad(cx, cy, 0.f, rr, 0x6a7cff, 0.55f, 0x6a7cff, 0.f));
  // The top edge of the recess shades the screen just below it.
  s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y0 + 6); s.lineTo(x0, y0 + 6); s.closePath();
  s.fillGrad(vertical(y0, y0 + 6, 0x000000, 0x000000, 0.45f, 0.f));
}
template <class Sink>
void drawDisplayGlass(Sink& s, float x0, float y0, float x1, float y1) {
  // A broad, faint reflection across the upper part of the glass, ending in a soft diagonal.
  const float h = y1 - y0;
  s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y0 + h * 0.18f); s.lineTo(x0, y0 + h * 0.62f); s.closePath();
  s.fillGrad(vertical(y0, y0 + h * 0.62f, 0xffffff, 0xffffff, 0.16f, 0.03f));
}

// ---- knobs, jacks, switches: drawn the same in the app and the preview --------------------

inline void polarPoint(float cx, float cy, float r, float deg, float& x, float& y) {  // 0 deg = 12 o'clock
  const float t = (deg - 90.f) * 3.14159265f / 180.f;
  x = cx + r * std::cos(t);
  y = cy + r * std::sin(t);
}

// A knob of radius R. Skirted knobs: dark ribbed skirt with scale ticks and a coloured cap
// standing above it; plain knobs: one black dome with a pointer dot. `deg` = pointer angle.
template <class Sink>
void drawKnob(Sink& s, float cx, float cy, float R, std::uint32_t capRgb, bool skirted, float deg,
              std::uint32_t tickRgb, float minDeg, float maxDeg, bool hover) {
  float cr = R;
  if (skirted) {
    for (int i = 0; i <= 10; ++i) {  // scale ticks, printed on the panel
      float x0, y0, x1, y1;
      const float a = minDeg + float(i) * (maxDeg - minDeg) / 10.f;
      polarPoint(cx, cy, R * 0.98f, a, x0, y0);
      polarPoint(cx, cy, R * 1.16f, a, x1, y1);
      s.moveTo(x0, y0);
      s.lineTo(x1, y1);
      s.strokePath(tickRgb, 3.f);
    }
  }
  dropShadow(s, cx, cy, R, R * 0.22f);
  if (skirted) {
    s.circle(cx, cy, R);
    s.fillGrad(vertical(cy - R, cy + R, 0x5c5c5c, 0x121212));
    for (int i = 0; i < 36; ++i) {  // grip ribs round the skirt edge
      float x0, y0, x1, y1;
      polarPoint(cx, cy, R * 0.86f, float(i) * 10.f, x0, y0);
      polarPoint(cx, cy, R * 0.99f, float(i) * 10.f, x1, y1);
      s.moveTo(x0, y0);
      s.lineTo(x1, y1);
      s.strokeGrad(solid(0x000000, 0.35f), 1.2f);
    }
    s.circle(cx, cy, R - 0.75f);
    s.strokeGrad(vertical(cy - R, cy + R, 0xffffff, 0x000000, 0.3f, 0.4f), 1.5f);
    cr = R * 0.78f;
    const float sx = cx + cr * 0.06f, sy = cy + cr * 0.12f;  // the cap's shadow on the skirt
    s.circle(sx, sy, cr * 1.08f);
    s.fillGrad(radialGrad(sx, sy, cr * 0.85f, cr * 1.08f, 0x000000, 0.55f, 0x000000, 0.f));
    dome(s, cx, cy, cr, capRgb, 0.3f);
  } else {
    dome(s, cx, cy, R, 0x262626, 0.45f);
  }
  float x0, y0, x1, y1;
  polarPoint(cx, cy, cr * (skirted ? 0.35f : 0.55f), deg, x0, y0);
  polarPoint(cx, cy, cr * 0.9f, deg, x1, y1);
  if (skirted) {
    const float w = std::fmax(3.f, R * 0.12f);
    s.moveTo(x0 + 0.8f, y0 + 1.5f);
    s.lineTo(x1 + 0.8f, y1 + 1.5f);
    s.strokeGrad(solid(0x000000, 0.4f), w + 1.f);
    s.moveTo(x0, y0);
    s.lineTo(x1, y1);
    s.strokeGrad(solid(0xf5f5f5), w);
  } else {
    const float px = (x1 + x0 * 0.4f) / 1.4f, py = (y1 + y0 * 0.4f) / 1.4f;
    s.circle(px, py, R * 0.12f);
    s.fillGrad(solid(0xf5f5f5));
  }
  if (hover) {
    s.circle(cx, cy, R + 2.f);
    s.strokeGrad(solid(0xebaa00, 0.8f), 2.f);
  }
}

// A patch socket: hex nut (radius r) round a threaded bushing and the dark bore.
template <class Sink>
void drawJack(Sink& s, float cx, float cy, float r, bool hover) {
  dropShadow(s, cx, cy, r * 0.9f, r * 0.25f, 0.35f);
  for (int i = 0; i < 6; ++i) {
    const float a = 3.14159265f / 3.f * float(i);
    if (i == 0) s.moveTo(cx + r * std::cos(a), cy + r * std::sin(a));
    else s.lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  s.closePath();
  s.fillGrad(vertical(cy - r, cy + r, hover ? 0xffd27a : 0xf0f0f0, hover ? 0xa87a10 : 0x858585));
  for (int i = 0; i < 6; ++i) {
    const float a = 3.14159265f / 3.f * float(i);
    if (i == 0) s.moveTo(cx + r * std::cos(a), cy + r * std::sin(a));
    else s.lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  s.closePath();
  s.strokeGrad(solid(0x4a4a4a, 0.9f), 1.2f);
  s.circle(cx, cy, r * 0.64f);
  s.fillGrad(vertical(cy - r * 0.64f, cy + r * 0.64f, 0x3e3e3e, 0xa0a0a0));  // lit from above: a recess
  s.circle(cx, cy, r * 0.5f);
  s.fillGrad(radialGrad(cx, cy - r * 0.08f, r * 0.1f, r * 0.5f, 0x000000, 1.f, 0x2a2a2a, 1.f));
}

// Photo sensor: a milky dome in a black ring.
template <class Sink>
void drawSensor(Sink& s, float cx, float cy, float r) {
  dropShadow(s, cx, cy, r, 4.f, 0.3f);
  s.fillCircle(cx, cy, r + 1.f, 0x0c0a0a);
  s.circle(cx, cy, r - 1.f);
  s.fillGrad(radialGrad(cx - r * 0.25f, cy - r * 0.3f, r * 0.1f, r * 1.3f, 0xffffff, 1.f, 0xc9c6bd, 1.f));
}

// Round latching push button: dark bezel and a dished cap; `on` = lit amber ring.
template <class Sink>
void drawButton(Sink& s, float cx, float cy, float r, bool on, bool hover) {
  if (on) {
    s.circle(cx, cy, r + 12.f);
    s.fillGrad(radialGrad(cx, cy, r, r + 12.f, 0xffb000, 0.5f, 0xffb000, 0.f));
  }
  dropShadow(s, cx, cy, r, r * 0.25f);
  s.circle(cx, cy, r);
  s.fillGrad(vertical(cy - r, cy + r, 0x444444, 0x0a0a0a));
  const float c = r * 0.72f;
  s.circle(cx, cy, c);
  s.fillGrad(vertical(cy - c, cy + c, 0x101010, 0x3a3a3a));  // dished: darker at the top
  if (on) {
    s.circle(cx, cy, r + 3.f);
    s.strokeGrad(solid(0xebaa00), 3.f);
  }
  if (hover) {
    s.circle(cx, cy, c);
    s.strokeGrad(solid(0xffffff, 0.4f), 1.5f);
  }
}

// The panel face colour at height y, sheen included (drawPanelArt: kPanel under a white 10% ->
// black 6% vertical wash), for painting over a printed mark.
inline std::uint32_t panelFaceAt(float y) {
  const float t = std::fmin(std::fmax(y / 1552.f, 0.f), 1.f);
  const float wash = 255.f * (1.f - t), a = 0.10f - 0.04f * t;
  auto ch = [&](int base) { return static_cast<std::uint32_t>(std::lround(base * (1.f - a) + wash * a)); };
  return (ch(233) << 16) | (ch(224) << 8) | ch(210);
}

// REC (not on the hardware), in the headphone socket's place: the printed headphone icon
// above it is painted over with the label, which shows the elapsed time while recording.
template <class Sink>
void drawRecordButton(Sink& s, float cx, float cy, float r, bool recording, int seconds, bool hover) {
  s.fillRect(cx - 28.f, cy - 66.f, cx + 28.f, cy - 22.f, panelFaceAt(cy - 44.f), 0.f);  // over the headphone icon
  char label[16] = "REC";
  if (recording) {
    const int t = seconds < 0 ? 0 : seconds;
    if (t < 600) std::snprintf(label, sizeof label, "%d:%02d", t / 60, t % 60);
    else std::snprintf(label, sizeof label, "%d'", t / 60);  // 10 min and up: minutes only
  }
  s.text(cx, cy - 44.f, 15.f, recording ? 0xcb2026 : 0x0c0a0a, false, label);
  if (recording) {
    s.circle(cx, cy, r + 12.f);
    s.fillGrad(radialGrad(cx, cy, r, r + 12.f, 0xff2a1a, 0.55f, 0xff2a1a, 0.f));
  }
  drawButton(s, cx, cy, r, false, hover);
  if (recording) {
    s.circle(cx, cy, r + 3.f);
    s.strokeGrad(solid(0xe0201a), 3.f);
    s.fillCircle(cx, cy, r * 0.3f, 0xe0201a);
  }
}

// REC's source selector, in the PHONE knob's place: a three-position knob, WET / DRY / ALL
// printed above it (the chosen one dark, the others faint).
inline constexpr float kRecordSourceDeg[3] = {-40.f, 0.f, 40.f};
template <class Sink>
void drawRecordSource(Sink& s, float cx, float cy, float r, int source, std::uint32_t capRgb, bool hover) {
  static const char* names[3] = {"WET", "DRY", "ALL"};
  s.fillRect(cx - 13.f, cy - r - 21.f, cx + 13.f, cy - r - 9.f, panelFaceAt(cy - r - 15.f), 0.f);  // link line behind DRY
  for (int i = 0; i < 3; ++i) {
    const float a = kRecordSourceDeg[i] * 3.14159265f / 180.f;
    s.text(cx + (r + 15.f) * std::sin(a), cy - (r + 15.f) * std::cos(a), 11.f,
           i == source ? 0x0c0a0a : 0x9a9086, false, names[i]);
  }
  drawKnob(s, cx, cy, r, capRgb, true, kRecordSourceDeg[source < 0 || source > 2 ? 0 : source], 0x343434,
           -150.f, 150.f, hover);
}

// Bat-lever toggle switch. `t`: 0 = lever up, 0.5 = centre, 1 = down.
template <class Sink>
void drawToggle(Sink& s, float cx, float cy, float t, bool hover) {
  dropShadow(s, cx, cy, 11.f, 3.f, 0.35f);
  for (int i = 0; i < 6; ++i) {  // mounting nut
    const float a = 3.14159265f / 3.f * float(i) + 0.5236f;
    if (i == 0) s.moveTo(cx + 12.f * std::cos(a), cy + 12.f * std::sin(a));
    else s.lineTo(cx + 12.f * std::cos(a), cy + 12.f * std::sin(a));
  }
  s.closePath();
  s.fillGrad(vertical(cy - 12.f, cy + 12.f, 0xe6e6e6, 0x767676));
  s.circle(cx, cy, 7.5f);
  s.fillGrad(vertical(cy - 7.5f, cy + 7.5f, 0x303030, 0x9a9a9a));
  const float ly = cy + (t - 0.5f) * 30.f;
  s.moveTo(cx + 3.f, cy + 4.f);  // lever shadow
  s.lineTo(cx + 3.f, ly + 5.f);
  s.strokeGrad(solid(0x000000, 0.3f), 8.f);
  if (std::fabs(ly - cy) > 1.f) {
    s.moveTo(cx - 4.f, cy);
    s.lineTo(cx + 4.f, cy);
    s.lineTo(cx + 2.8f, ly);
    s.lineTo(cx - 2.8f, ly);
    s.closePath();
    s.fillGrad({false, cx - 4.f, cy, cx + 4.f, cy, 0xf6f6f6, 0x6a6a6a, 1.f, 1.f});  // chrome, lit from the left
  }
  dome(s, cx, ly, 6.f, hover ? 0xebaa00 : 0xd4d4d4, 0.4f);
}

// The joystick: recessed gate, shaft and a round knob at (x, y).
template <class Sink>
void drawJoystick(Sink& s, float cx, float cy, float gateR, float travel, float x, float y, bool hover) {
  s.circle(cx, cy, gateR);
  s.fillGrad(vertical(cy - gateR, cy + gateR, 0x161616, 0x3c3c3c));
  s.circle(cx, cy, gateR - 1.f);
  s.strokeGrad(vertical(cy - gateR, cy + gateR, 0x000000, 0xffffff, 0.5f, 0.3f), 2.f);
  s.circle(cx, cy, travel);
  s.strokeGrad(solid(0x5a5a5a), 1.5f);
  s.moveTo(cx + 4.f, cy + 7.f);
  s.lineTo(x + 6.f, y + 10.f);
  s.strokeGrad(solid(0x000000, 0.35f), 16.f);
  s.moveTo(cx, cy);
  s.lineTo(x, y);
  s.strokeGrad({false, cx - 7.f, cy, cx + 7.f, cy, 0x4a4a4a, 0x141414, 1.f, 1.f}, 14.f);
  dropShadow(s, x, y, 24.f, 10.f);
  dome(s, x, y, 24.f, hover ? 0x707070 : 0x555555, 0.45f);
}

// Everything that does not move: panel, keybed, frames, tabs, printed icons, labels, logos.
template <class Sink>
void drawPanelArt(Sink& s) {
  s.fillRect(0, 0, 2400, 1552, kPanelRgb, 0);
  auto rect = [&s](float x0, float y0, float x1, float y1) {
    s.moveTo(x0, y0); s.lineTo(x1, y0); s.lineTo(x1, y1); s.lineTo(x0, y1); s.closePath();
  };
  rect(0, 0, 2400, 1552);  // a gentle top-to-bottom falloff of the light
  s.fillGrad(vertical(0, 1552, 0xffffff, 0x000000, 0.10f, 0.06f));
  s.fillRect(400, 1103, 1998, 1490, kKeybedRgb, 0);
  rect(400, 1103, 1998, 1128);  // the keybed sits lower than the panel: shadow under its top edge
  s.fillGrad(vertical(1103, 1128, 0x000000, 0x000000, 0.55f, 0.f));
  for (const auto& f : kFrames) {
    for (std::uint32_t i = 0; i < f.count; ++i) {
      const float x = kFramePoints[2 * (f.first + i)], y = kFramePoints[2 * (f.first + i) + 1];
      if (i == 0) s.moveTo(x, y);
      else s.lineTo(x, y);
    }
    s.strokePath(kInkRgb, 3.f);
  }
  for (const auto& b : kTabs) s.fillRect(b.x0, b.y0, b.x1, b.y1, kInkRgb, 4.f);
  drawShapes(s, kDecorPoints, kDecorSubPaths, kDecorShapes, sizeof(kDecorShapes) / sizeof(kDecorShapes[0]));
  for (const auto& d : kDots) s.fillCircle(d.x, d.y, d.r, d.rgb);
  for (const auto& l : kLeds) drawLed(s, l.x, l.y, l.r, l.rgb, 0.f);
  for (const auto& t : kTexts) s.text(t.x, t.y, t.size * 0.92f, t.rgb, t.vertical, t.text);
  drawShapes(s, kLogoPoints, kLogoSubPaths, kLogoShapes, sizeof(kLogoShapes) / sizeof(kLogoShapes[0]));
  drawKeybedMarks(s);
}

}  // namespace lunar24::host::art
