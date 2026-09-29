// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_preview — render the panel layout to an SVG file (default knob positions), so the
// UI can be reviewed on any machine without building the app:
//
//   panel_preview > panel.svg

#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <string>

#include <host/panel_theme.h>
#include <host/panel_ui_layout.h>
#include <lunar24/core/state_default.h>

using namespace lunar24;
using namespace lunar24::host;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string rgb(theme::Rgb c, double a = 1.0) {
  char b[64];
  std::snprintf(b, sizeof b, "rgba(%d,%d,%d,%.2f)", c.r, c.g, c.b, a);
  return b;
}

const core::DeviceStateV1& defaults() {
  static const core::DeviceStateV1 st = core::make_default_device_state(1);
  return st;
}

double norm01(std::uint32_t id) {
  const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(id));
  if (d == nullptr || d->max <= d->min) return 0.0;
  return (defaults().parameters[id] - d->min) / (d->max - d->min);
}

void text(double x, double y, double size, theme::Rgb c, const std::string& s, const char* anchor = "middle",
          const char* weight = "500") {
  std::printf("<text x='%.1f' y='%.1f' font-size='%.1f' fill='%s' text-anchor='%s' font-weight='%s' "
              "font-family='Helvetica Neue, Arial, sans-serif' letter-spacing='0.5'>%s</text>\n",
              x, y, size, rgb(c).c_str(), anchor, weight, s.c_str());
}

void arc(double cx, double cy, double r, double a0, double a1, theme::Rgb c, double width) {
  auto pt = [&](double deg, double& x, double& y) {
    const double t = (deg - 90.0) * kPi / 180.0;
    x = cx + r * std::cos(t);
    y = cy + r * std::sin(t);
  };
  double x0, y0, x1, y1;
  pt(a0, x0, y0);
  pt(a1, x1, y1);
  const int large = (a1 - a0) > 180.0 ? 1 : 0;
  std::printf("<path d='M%.1f %.1f A%.1f %.1f 0 %d 1 %.1f %.1f' stroke='%s' stroke-width='%.1f' fill='none' "
              "stroke-linecap='round'/>\n",
              x0, y0, r, r, large, x1, y1, rgb(c).c_str(), width);
}

void knob(const Widget& w) {
  const double d = w.w, cx = w.x + w.w / 2, cy = w.y + d / 2, r = d / 2 - 5;
  const theme::Rgb a = theme::accent(w.accent);
  const double v = norm01(w.id);
  const double ang = theme::kKnobMinDeg + v * (theme::kKnobMaxDeg - theme::kKnobMinDeg);
  arc(cx, cy, r + 2, theme::kKnobMinDeg, theme::kKnobMaxDeg, theme::kKnobTrack, 3);
  if (v > 0.001) arc(cx, cy, r + 2, theme::kKnobMinDeg, ang, a, 3);
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s' stroke='%s' stroke-width='1.5'/>\n", cx, cy, r - 3,
              rgb(theme::kKnobBody).c_str(), rgb(theme::kCardEdge).c_str());
  const double t = (ang - 90.0) * kPi / 180.0;
  std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='2.5' stroke-linecap='round'/>\n",
              cx + (r - 14) * std::cos(t) * 0.2, cy + (r - 14) * std::sin(t) * 0.2, cx + (r - 5) * std::cos(t),
              cy + (r - 5) * std::sin(t), rgb(theme::kText).c_str());
  text(cx, w.y + w.h - 3, w.compact ? 9.5 : 10.5, theme::kTextDim, w.label);
}

void selector(const Widget& w) {
  const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(w.id));
  const theme::Rgb a = theme::accent(w.accent);
  const double bh = w.h - 16;
  std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='5' fill='%s' stroke='%s' stroke-width='1.2'/>\n",
              w.x + 2, w.y + 2, w.w - 4, bh - 4, rgb(theme::kKnobBody).c_str(), rgb(a, 0.8).c_str());
  std::string opt;
  if (d != nullptr && d->optionCount > 0) {
    const int idx = static_cast<int>(std::lround(defaults().parameters[w.id] - d->min));
    if (idx >= 0 && idx < static_cast<int>(d->optionCount)) opt = d->options[idx];
  }
  for (auto& ch : opt) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const double size = opt.size() > 6 ? std::max(7.5, 10.0 - static_cast<double>(opt.size() - 6) * 0.6) : 10.0;
  if (opt.size() > 10) opt = opt.substr(0, 10);
  text(w.x + w.w / 2, w.y + bh / 2 + 3.5, size, a, opt, "middle", "700");
  text(w.x + w.w / 2, w.y + w.h - 3, 9.5, theme::kTextDim, w.label);
}

void jack(const Widget& w) {
  const core::JackDescriptor* j = nullptr;
  for (const auto& d : registry::kJacks)
    if (static_cast<std::uint32_t>(d.id) == w.id) j = &d;
  const bool out = j != nullptr && j->direction == core::PinDirection::output;
  const double cx = w.x + w.w / 2, cy = w.y + 17, r = 13;
  const theme::Rgb a = theme::accent(w.accent);
  if (out)
    std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='6' fill='%s'/>\n", cx - r - 4, cy - r - 4,
                2 * r + 8, 2 * r + 8, rgb(a, 0.35).c_str());
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", cx, cy, r, rgb(theme::kJackRing).c_str());
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", cx, cy, r * 0.55, rgb(theme::kJackHole).c_str());
  text(cx, w.y + w.h - 2, 9, out ? a : theme::kTextDim, w.label, "middle", out ? "700" : "500");
}

}  // namespace

int main() {
  const auto ws = build_panel_layout();
  std::printf("<svg xmlns='http://www.w3.org/2000/svg' width='2400' height='1551' viewBox='0 0 2400 1551'>\n");
  std::printf("<rect width='2400' height='1551' fill='%s'/>\n", rgb(theme::kBackground).c_str());
  for (const Widget& w : ws) {
    const theme::Rgb a = theme::accent(w.accent);
    switch (w.kind) {
      case WidgetKind::Module:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='10' fill='%s' stroke='%s' stroke-width='1.5'/>\n",
                    w.x + 1, w.y + 1, w.w - 2, w.h - 2, rgb(theme::kCard).c_str(), rgb(theme::kCardEdge).c_str());
        text(w.x + 12, w.y + 21, 13, a, w.label, "start", "700");
        std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='1.5'/>\n", w.x + 12,
                    w.y + 28, w.x + w.w - 12, w.y + 28, rgb(a, 0.35).c_str());
        break;
      case WidgetKind::Title:
        std::printf("<circle cx='%.1f' cy='%.1f' r='52' fill='%s'/>\n", w.x + 70, w.y + 75, rgb({205, 202, 194}).c_str());
        std::printf("<circle cx='%.1f' cy='%.1f' r='52' fill='%s'/>\n", w.x + 92, w.y + 62, rgb(theme::kBackground).c_str());
        text(w.x + 150, w.y + 92, 64, theme::kText, "LUNAR 24", "start", "300");
        text(w.x + 154, w.y + 124, 16, theme::kTextDim, "AMBIENT  DRONE  MACHINE", "start", "500");
        break;
      case WidgetKind::Knob: knob(w); break;
      case WidgetKind::Selector: selector(w); break;
      case WidgetKind::Jack: jack(w); break;
      case WidgetKind::Plates: {
        static const char* kNote[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const double pw = w.w / 12.0;
        for (int i = 0; i < 12; ++i) {
          std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='8' fill='%s' stroke='%s'/>\n",
                      w.x + i * pw + 4, w.y, pw - 8, w.h, rgb(theme::kPlate).c_str(), rgb(theme::kCardEdge).c_str());
          text(w.x + i * pw + pw / 2, w.y + w.h - 14, 14, theme::kTextDim, kNote[i]);
        }
        break;
      }
      case WidgetKind::Joystick:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='8' fill='%s' stroke='%s'/>\n", w.x, w.y,
                    w.w, w.h, rgb(theme::kKnobBody).c_str(), rgb(a, 0.7).c_str());
        std::printf("<circle cx='%.1f' cy='%.1f' r='9' fill='%s'/>\n", w.x + w.w * norm01(w.id),
                    w.y + w.h * (1.0 - norm01(w.id2)), rgb(a).c_str());
        break;
      case WidgetKind::Cartridge: {
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='8' fill='%s' stroke='%s' stroke-width='1.5'/>\n",
                    w.x, w.y, w.w, w.h, rgb(theme::kKnobBody).c_str(), rgb(a, 0.8).c_str());
        const auto prog = w.id == 0 ? defaults().leftEffector.program : defaults().rightEffector.program;
        const core::ProgramDescriptor* p = core::find_program(prog);
        text(w.x + w.w / 2, w.y + 20, 10, theme::kTextDim, w.label + " CARTRIDGE");
        text(w.x + w.w / 2, w.y + 50, 18, a, p ? std::string(p->cartridge) : "?", "middle", "700");
        text(w.x + w.w / 2, w.y + 74, 12, theme::kText, p ? std::string(p->name) : "");
        text(w.x + 14, w.y + 50, 18, theme::kTextDim, "&#9664;");
        text(w.x + w.w - 14, w.y + 50, 18, theme::kTextDim, "&#9654;");
        break;
      }
      case WidgetKind::DroneKey:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='8' fill='%s' stroke='%s'/>\n", w.x, w.y,
                    w.w, w.h, rgb(theme::kPlate).c_str(), rgb(a, 0.6).c_str());
        text(w.x + w.w / 2, w.y + w.h / 2 + 7, 20, a, w.label, "middle", "700");
        break;
    }
  }
  std::printf("</svg>\n");
  return 0;
}
