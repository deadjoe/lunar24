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

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lunar24::host::art {

struct Text { float x, y, size; std::uint32_t rgb; bool vertical; const char* text; };
struct Box { float x0, y0, x1, y1; };
struct Frame { std::uint32_t first, count; };
// A vector shape: `subCount` sub-paths starting at `firstSub`; filled and/or stroked.
struct SubPath { std::uint32_t first, count; bool closed, hole; };
enum ShapeFlag : std::uint8_t { kFill = 1, kStroke = 2, kEvenOdd = 4 };
struct Shape { std::uint32_t firstSub, subCount, fill, stroke; float width; std::uint8_t flags; };
struct Dot { float x, y, r; std::uint32_t rgb; };
// Indicator LEDs printed on the panel. Drawn unlit (Lunar 24 does not drive them yet).
struct Led { float x, y, r; std::uint32_t rgb; };

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
  const std::uint32_t frame = lit ? 0xffc478 : 0xfafafa;
  s.fillRect(x0, y0, x1, y1, frame, 3.f);
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
  s.fillCircle(cx, cy, 44, 0xbcbcbc);
  s.fillCircle(cx, cy, 36, 0x1a1a1a);
  s.fillCircle(cx, cy, 33, 0x333333);
  for (int i = 0; i < 24; ++i) {  // knurling
    const float a = 6.2831853f * float(i) / 24.f;
    s.moveTo(cx + 26 * std::cos(a), cy + 26 * std::sin(a));
    s.lineTo(cx + 33 * std::cos(a), cy + 33 * std::sin(a));
    s.strokePath(0x1a1a1a, 1.5f);
  }
  s.fillCircle(cx, cy, 23, 0x111111);
  s.fillCircle(cx, cy, 21.5f, active ? 0xebaa00 : 0xdc3a2c);
}

template <class Sink>
void drawOctaveKey(Sink& s, float cx, float cy, bool hover) {
  s.fillCircle(cx, cy, 24, 0x141414);
  s.fillCircle(cx, cy, 22.5f, hover ? 0xffe2b8 : 0xfcfcfa);
}

template <class Sink>
void drawDroneKey(Sink& s, float x0, float y0, float x1, float y1, bool open, bool hover) {
  s.fillRect(x0, y0, x1, y1, 0x2e2e2e, 3.f);
  s.fillRect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0x262626, 3.f);                    // shadowed skirt
  s.fillRect(x0 + 9, y0 + 3, x1 - 9, y1 - 11, hover ? 0x4a4a4a : 0x3c3c3c, 7.f);  // key top
  const float cx = (x0 + x1) / 2;
  s.fillRect(cx - 7, y0 + 3, cx + 7, y0 + 13, open ? 0xeb3c32 : 0xd8d8d8, 0);   // LED window
}

template <class Sink>
void drawDisplay(Sink& s, float x0, float y0, float x1, float y1) {
  s.fillRect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 0x121212, 2.f);
  s.fillRect(x0, y0, x1, y1, 0x4252d0, 0);
}

// Everything that does not move: panel, keybed, frames, tabs, printed icons, labels, logos.
template <class Sink>
void drawPanelArt(Sink& s) {
  s.fillRect(0, 0, 2400, 1552, kPanelRgb, 0);
  s.fillRect(400, 1103, 1998, 1490, kKeybedRgb, 0);
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
  for (const auto& l : kLeds) s.fillCircle(l.x, l.y, l.r, unlit(l.rgb));
  for (const auto& t : kTexts) s.text(t.x, t.y, t.size * 0.92f, t.rgb, t.vertical, t.text);
  drawShapes(s, kLogoPoints, kLogoSubPaths, kLogoShapes, sizeof(kLogoShapes) / sizeof(kLogoShapes[0]));
  drawKeybedMarks(s);
}

}  // namespace lunar24::host::art
