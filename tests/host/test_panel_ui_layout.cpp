// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The panel layout: every panel parameter and every jack has exactly one control, controls
// do not overlap, and each control sits inside the 2400x1551 panel and inside a module.

#include <cstdio>
#include <map>

#include "mini_test.h"
#include <host/panel_ui_layout.h>
#include <lunar24/core/state_disposition.h>

using namespace lunar24;
using host::Widget;
using host::WidgetKind;

namespace {
bool overlaps(const Widget& a, const Widget& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool inside(const Widget& in, const Widget& out) {
  return in.x >= out.x - 0.5 && in.y >= out.y - 0.5 && in.x + in.w <= out.x + out.w + 0.5 &&
         in.y + in.h <= out.y + out.h + 0.5;
}
}  // namespace

int main() {
  const std::vector<Widget> ws = host::build_panel_layout();

  std::map<std::uint32_t, int> params, jacks;
  for (const Widget& w : ws) {
    if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::Selector) ++params[w.id];
    if (w.kind == WidgetKind::Jack) ++jacks[w.id];
    if (w.kind == WidgetKind::Joystick) { ++params[w.id]; ++params[w.id2]; }
  }
  // Every panel parameter (all registry parameters owned by a module, not by an effector
  // program) and every jack appears exactly once.
  int missingP = 0, dupP = 0;
  for (const auto& p : registry::kParameters) {
    if (p.owner.substr(0, 8) == "program.") continue;
    const int n = params[static_cast<std::uint32_t>(p.id)];
    if (n == 0) { ++missingP; std::printf("  no control for %s\n", std::string(p.stable_id).c_str()); }
    if (n > 1) { ++dupP; std::printf("  %d controls for %s\n", n, std::string(p.stable_id).c_str()); }
  }
  CHECK_EQ(missingP, 0);
  CHECK_EQ(dupP, 0);
  int missingJ = 0, dupJ = 0;
  for (const auto& j : registry::kJacks) {
    const int n = jacks[static_cast<std::uint32_t>(j.id)];
    if (n == 0) { ++missingJ; std::printf("  no jack for %s\n", std::string(j.stable_id).c_str()); }
    if (n > 1) ++dupJ;
  }
  CHECK_EQ(missingJ, 0);
  CHECK_EQ(dupJ, 0);

  // Geometry: controls inside the panel, inside some module, never overlapping each other;
  // modules never overlap each other.
  int outside = 0, orphan = 0, clash = 0;
  for (std::size_t i = 0; i < ws.size(); ++i) {
    const Widget& a = ws[i];
    if (a.x < 0 || a.y < 0 || a.x + a.w > 2400 || a.y + a.h > 1551) ++outside;
    if (a.kind == WidgetKind::Module || a.kind == WidgetKind::Title) continue;
    bool inModule = false;
    for (const Widget& m : ws)
      if (m.kind == WidgetKind::Module && inside(a, m)) inModule = true;
    if (!inModule) { ++orphan; std::printf("  outside any module: %s\n", a.label.c_str()); }
    for (std::size_t k = i + 1; k < ws.size(); ++k) {
      const Widget& b = ws[k];
      if (b.kind == WidgetKind::Module || b.kind == WidgetKind::Title) continue;
      if (overlaps(a, b)) { ++clash; std::printf("  overlap: %s / %s\n", a.label.c_str(), b.label.c_str()); }
    }
  }
  for (std::size_t i = 0; i < ws.size(); ++i)
    for (std::size_t k = i + 1; k < ws.size(); ++k)
      if (ws[i].kind == WidgetKind::Module && ws[k].kind == WidgetKind::Module && overlaps(ws[i], ws[k])) {
        ++clash;
        std::printf("  module overlap: %s / %s\n", ws[i].label.c_str(), ws[k].label.c_str());
      }
  CHECK_EQ(outside, 0);
  CHECK_EQ(orphan, 0);
  CHECK_EQ(clash, 0);
  return test::finish("test_panel_ui_layout");
}
