// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0

// Exercise the actual GDI memory-font path used before NanoVG gets the font bytes.
// Geometry tests on Linux cannot tell us whether Windows accepts the embedded face.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mini_test.h"
#include <host/ui_font.generated.h>
#include <cwchar>

static void check_face(HDC dc, int weight) {
  HFONT font = CreateFontW(0, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                           DEFAULT_PITCH, L"Noto Sans");
  CHECK(font != nullptr);
  if (!font) return;
  const auto old = SelectObject(dc, font);
  wchar_t name[64]{};
  CHECK(GetTextFaceW(dc, 64, name) > 0);
  CHECK(std::wcscmp(name, L"Noto Sans") == 0);  // no silent system substitution
  TEXTMETRICW metrics{};
  CHECK(GetTextMetricsW(dc, &metrics));
  CHECK(metrics.tmWeight == weight);
  const DWORD size = GetFontData(dc, 0, 0, nullptr, 0);
  CHECK(size != GDI_ERROR && size > 0);
  constexpr wchar_t text[] = L"LUNAR 24 env MIDI 0123456789\u00b0\u00b1\u00b7\u2013\u2014\u2022\u2026";
  constexpr int count = static_cast<int>(sizeof(text) / sizeof(text[0])) - 1;
  WORD glyphs[count]{};
  CHECK(GetGlyphIndicesW(dc, text, count, glyphs, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR);
  for (WORD glyph : glyphs) CHECK(glyph != 0xffff);
  SelectObject(dc, old);
  DeleteObject(font);
}

int main() {
  using namespace lunar24::host::font;
  DWORD regularCount = 0, boldCount = 0;
  HANDLE regular = AddFontMemResourceEx(const_cast<unsigned char*>(kRegular),
                                       static_cast<DWORD>(kRegularSize), nullptr, &regularCount);
  HANDLE bold = AddFontMemResourceEx(const_cast<unsigned char*>(kBold),
                                    static_cast<DWORD>(kBoldSize), nullptr, &boldCount);
  CHECK(regular != nullptr && regularCount == 1);
  CHECK(bold != nullptr && boldCount == 1);
  HDC dc = CreateCompatibleDC(nullptr);
  CHECK(dc != nullptr);
  if (dc && regular && bold) {
    check_face(dc, FW_REGULAR);
    check_face(dc, FW_BOLD);
  }
  if (dc) DeleteDC(dc);
  if (bold) RemoveFontMemResourceEx(bold);
  if (regular) RemoveFontMemResourceEx(regular);
  return test::finish("ui_font_win");
}
