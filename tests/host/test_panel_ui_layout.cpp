// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// The panel layout: every panel parameter has exactly one control, every jack on the
// official panel has exactly one socket, controls stay on the panel and do not overlap,
// and the keyboard-menu controls sit inside the menu overlay without overlapping. The static
// panel art (panel_art.h) draws with every path inside the panel.

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "mini_test.h"
#include <host/panel_art.h>
#include <host/panel_ui_layout.h>
#include <lunar24/core/state_disposition.h>

using namespace lunar24;
using host::Widget;
using host::WidgetKind;

namespace {
bool overlaps(const Widget& a, const Widget& b) {
  return a.x() < b.x() + b.w && b.x() < a.x() + a.w && a.y() < b.y() + b.h && b.y() < a.y() + a.h;
}
bool onPanel(const Widget& w) {
  return w.x() >= 0 && w.y() >= 0 && w.x() + w.w <= 2400 && w.y() + w.h <= 1552;
}
bool inMenu(const Widget& w) {
  return w.x() >= host::kMenuX0 && w.y() >= host::kMenuY0 && w.x() + w.w <= host::kMenuX1 &&
         w.y() + w.h + 30 <= host::kMenuY1;  // + label line
}
bool notOnPanel(std::uint32_t jack) {
  for (auto j : host::kJacksNotOnPanel)
    if (static_cast<std::uint32_t>(j) == jack) return true;
  return false;
}
// Counts what the panel art draws and flags points off the panel or unbalanced paths.
struct CheckSink {
  int paths = 0, texts = 0, bad = 0;
  bool open = false;
  void pt(float x, float y) { if (x < -1 || y < -1 || x > 2401 || y > 1553) ++bad; }
  void fillRect(float x0, float y0, float x1, float y1, std::uint32_t, float) { pt(x0, y0); pt(x1, y1); }
  void fillCircle(float cx, float cy, float, std::uint32_t) { pt(cx, cy); }
  void moveTo(float x, float y) { pt(x, y); open = true; }
  void lineTo(float x, float y) { pt(x, y); if (!open) ++bad; }
  void closePath() {}
  void markHole() { if (!open) ++bad; }
  void fillPath(std::uint32_t, bool) { ++paths; open = false; }
  void strokePath(std::uint32_t, float) { ++paths; open = false; }
  void text(float x, float y, float, std::uint32_t, bool, const char*) { pt(x, y); ++texts; }
  void circle(float cx, float cy, float) { pt(cx, cy); open = true; }
  void fillGrad(const host::art::Grad&) { if (!open) ++bad; ++paths; open = false; }
  void strokeGrad(const host::art::Grad&, float) { if (!open) ++bad; ++paths; open = false; }
};
}  // namespace

int main() {
  const std::vector<Widget> ws = host::build_panel_layout();

  std::map<std::uint32_t, int> params, jacks;
  for (const Widget& w : ws) {
    if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::Button || w.kind == WidgetKind::Toggle) ++params[w.id];
    if (w.kind == WidgetKind::Jack) ++jacks[w.id];
    if (w.kind == WidgetKind::Joystick) { ++params[w.id]; ++params[w.id2]; }
  }
  int missingP = 0, dupP = 0;
  for (const auto& p : registry::kParameters) {
    if (p.owner.substr(0, 8) == "program.") continue;  // per-program labels, not panel controls
    const int n = params[static_cast<std::uint32_t>(p.id)];
    if (n == 0) { ++missingP; std::printf("  no control for %s\n", std::string(p.stable_id).c_str()); }
    if (n > 1) { ++dupP; std::printf("  %d controls for %s\n", n, std::string(p.stable_id).c_str()); }
  }
  CHECK_EQ(missingP, 0);
  CHECK_EQ(dupP, 0);

  int missingJ = 0, dupJ = 0;
  for (const auto& j : registry::kJacks) {
    const auto id = static_cast<std::uint32_t>(j.id);
    const int n = jacks[id];
    if (notOnPanel(id)) { CHECK_EQ(n, 0); continue; }
    if (n == 0) { ++missingJ; std::printf("  no socket for %s\n", std::string(j.stable_id).c_str()); }
    if (n > 1) ++dupJ;
  }
  CHECK_EQ(missingJ, 0);
  CHECK_EQ(dupJ, 0);

  int off = 0, clash = 0, menuOut = 0;
  for (std::size_t i = 0; i < ws.size(); ++i) {
    const Widget& a = ws[i];
    if (!onPanel(a)) { ++off; std::printf("  off panel: kind %d id %u\n", int(a.kind), a.id); }
    if (a.menu && !inMenu(a)) { ++menuOut; std::printf("  outside menu: %s\n", a.label.c_str()); }
    for (std::size_t k = i + 1; k < ws.size(); ++k) {
      const Widget& b = ws[k];
      if (a.menu != b.menu) continue;  // the menu overlay covers the plates on purpose
      if (overlaps(a, b)) {
        ++clash;
        std::printf("  overlap: kind %d id %u @(%.0f,%.0f) / kind %d id %u @(%.0f,%.0f)\n", int(a.kind), a.id, a.cx, a.cy,
                    int(b.kind), b.id, b.cx, b.cy);
      }
    }
  }
  CHECK_EQ(off, 0);
  CHECK_EQ(menuOut, 0);
  CHECK_EQ(clash, 0);

  CheckSink art;
  host::art::drawPanelArt(art);
  CHECK_EQ(art.bad, 0);
  CHECK(art.paths > 500);   // frames, printed marks, name plates
  CHECK(art.texts > 300);   // panel labels
  // Every control, drawn at its place: all paths are started before they are painted.
  CheckSink ctl;
  for (const Widget& w : ws) {
    if (w.menu) continue;
    const float cx = float(w.cx), cy = float(w.cy);
    if (w.kind == WidgetKind::Knob) host::art::drawKnob(ctl, cx, cy, float(w.w / 2), 0x006080, w.cap != host::Cap::Black, 30.f, 0x333333, -150.f, 150.f, true);
    if (w.kind == WidgetKind::Button) host::art::drawButton(ctl, cx, cy, float(w.w / 2), true, true);
    if (w.kind == WidgetKind::Toggle) host::art::drawToggle(ctl, cx, cy, 0.f, true);
    if (w.kind == WidgetKind::Jack) host::art::drawJack(ctl, cx, cy, float(w.w / 2), true);
    if (w.kind == WidgetKind::Joystick) host::art::drawJoystick(ctl, cx, cy, 55.f, 90.f, cx + 90.f, cy - 90.f, true);
  }
  CHECK_EQ(ctl.bad, 0);
  CHECK(ctl.paths > 1000);
  for (const auto& sh : host::art::kDecorShapes)
    CHECK(sh.firstSub + sh.subCount <= sizeof(host::art::kDecorSubPaths) / sizeof(host::art::kDecorSubPaths[0]));
  for (const auto& sp : host::art::kDecorSubPaths)
    CHECK(2 * (sp.first + sp.count) <= sizeof(host::art::kDecorPoints) / sizeof(float));
  for (const auto& sp : host::art::kLogoSubPaths)
    CHECK(2 * (sp.first + sp.count) <= sizeof(host::art::kLogoPoints) / sizeof(float));
    // Every lit indicator LED sits exactly on a printed LED, each on a different one, and its order
  // matches the engine's PanelLed list (20 LEDs; the joystick's two are not driven).
  {
    std::map<int, int> used;
    const int n = int(sizeof(host::kPanelLedPos) / sizeof(host::kPanelLedPos[0]));
    CHECK_EQ(n, 20);
    for (int i = 0; i < n; ++i) {
      int hit = -1;
      for (int k = 0; k < int(sizeof(host::art::kLeds) / sizeof(host::art::kLeds[0])); ++k) {
        const auto& l = host::art::kLeds[k];
        if (std::fabs(l.x - host::kPanelLedPos[i].x) < 1.0 && std::fabs(l.y - host::kPanelLedPos[i].y) < 1.0) hit = k;
      }
      CHECK(hit >= 0);
      CHECK(used.count(hit) == 0);
      used[hit] = i;
    }
    CHECK(host::art::litLed(0xff2308u, 1.f) == 0xff2308u);
    CHECK(host::art::litLed(0xff2308u, 0.f) == host::art::unlit(0xff2308u));
  }
  return test::finish("test_panel_ui_layout");
}
