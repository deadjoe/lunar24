// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_preview — render the panel (art + controls at their default positions) to SVG,
// so the UI can be reviewed on any machine without building the app:
//
//   panel_preview > panel.svg          (add --menu to show the keyboard menu overlay)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <host/panel_art.generated.h>
#include <host/panel_theme.h>
#include <host/panel_ui_layout.h>
#include <lunar24/core/state_default.h>

using namespace lunar24;
using namespace lunar24::host;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string col(theme::Rgb c, double a = 1.0) {
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

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

void text(double x, double y, double size, theme::Rgb c, const std::string& s, bool vertical = false) {
  char rot[96] = "";
  if (vertical) std::snprintf(rot, sizeof rot, " transform='rotate(-90 %.1f %.1f)'", x, y);
  std::printf("<text x='%.1f' y='%.1f' font-size='%.1f' fill='%s' text-anchor='middle' dominant-baseline='central' "
              "font-weight='700' font-family='Lucida Grande, Helvetica Neue, Arial, sans-serif'%s>%s</text>\n",
              x, y, size, col(c).c_str(), rot, esc(s).c_str());
}

void polar(double cx, double cy, double r, double deg, double& x, double& y) {
  const double t = (deg - 90.0) * kPi / 180.0;
  x = cx + r * std::cos(t);
  y = cy + r * std::sin(t);
}

void knob(const Widget& w) {
  const double cx = w.cx, cy = w.cy, R = w.w / 2;
  const double v = norm01(w.id);
  const double ang = theme::kKnobMinDeg + v * (theme::kKnobMaxDeg - theme::kKnobMinDeg);
  const bool skirted = w.cap != Cap::Black;
  double cr = R;
  if (skirted) {
    for (int i = 0; i <= 10; ++i) {  // scale ticks
      double x0, y0, x1, y1;
      const double a = theme::kKnobMinDeg + i * (theme::kKnobMaxDeg - theme::kKnobMinDeg) / 10.0;
      polar(cx, cy, R * 0.98, a, x0, y0);
      polar(cx, cy, R * 1.16, a, x1, y1);
      std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='3'/>\n", x0, y0, x1, y1,
                  col(theme::kSkirt).c_str());
    }
    std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", cx, cy, R, col(theme::kSkirt).c_str());
    cr = R * 0.78;
  }
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", cx, cy, cr, col(theme::cap(w.cap)).c_str());
  double x0, y0, x1, y1;
  polar(cx, cy, cr * (skirted ? 0.35 : 0.55), ang, x0, y0);
  polar(cx, cy, cr * 0.9, ang, x1, y1);
  if (skirted)
    std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='%.1f' stroke-linecap='round'/>\n",
                x0, y0, x1, y1, col(theme::kPointer).c_str(), std::max(3.0, R * 0.12));
  else
    std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", (x1 + x0 * 0.4) / 1.4, (y1 + y0 * 0.4) / 1.4,
                R * 0.12, col(theme::kPointer).c_str());
}

int positions(std::uint32_t id) {
  const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(id));
  if (d == nullptr) return 2;
  if (d->optionCount > 0) return static_cast<int>(d->optionCount);
  return std::max(2, static_cast<int>(std::lround((d->max - d->min) / (d->step > 0 ? d->step : 1.0))) + 1);
}
int indexOf(std::uint32_t id) {
  const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(id));
  if (d == nullptr) return 0;
  return static_cast<int>(std::lround((defaults().parameters[id] - d->min) / (d->step > 0 ? d->step : 1.0)));
}

void toggle(const Widget& w) {
  const int n = positions(w.id), idx = indexOf(w.id);
  const double t = n <= 1 ? 0.0 : static_cast<double>(idx) / (n - 1);  // 0 = up
  std::printf("<circle cx='%.1f' cy='%.1f' r='9' fill='%s'/>\n", w.cx, w.cy, col({60, 60, 60}).c_str());
  const double ly = w.cy + (t - 0.5) * 30;
  std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='8' stroke-linecap='round'/>\n",
              w.cx, w.cy, w.cx, ly, col({110, 110, 110}).c_str());
}

void jack(const Widget& w) {
  const double r = w.w / 2;
  std::string pts;
  for (int i = 0; i < 6; ++i) {
    const double a = kPi / 3 * i;
    char b[40];
    std::snprintf(b, sizeof b, "%.1f,%.1f ", w.cx + r * std::cos(a), w.cy + r * std::sin(a));
    pts += b;
  }
  std::printf("<polygon points='%s' fill='%s' stroke='%s' stroke-width='1.5'/>\n", pts.c_str(),
              col(theme::kNutLight).c_str(), col(theme::kNutDark).c_str());
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", w.cx, w.cy, r * 0.62, col({70, 70, 70}).c_str());
  std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", w.cx, w.cy, r * 0.46, col(theme::kHole).c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const bool showMenu = argc > 1 && std::strcmp(argv[1], "--menu") == 0;
  const auto ws = build_panel_layout();
  std::printf("<svg xmlns='http://www.w3.org/2000/svg' width='2400' height='1552' viewBox='0 0 2400 1552'>\n");
  std::printf("<rect width='2400' height='1552' fill='%s'/>\n", col(theme::kPanel).c_str());
  std::printf("<rect x='400' y='1103' width='1598' height='387' fill='%s'/>\n", col(theme::kKeybed).c_str());
  for (const auto& f : art::kFrames) {
    std::printf("<polyline fill='none' stroke='%s' stroke-width='3' points='", col(theme::kInk).c_str());
    for (std::uint32_t i = 0; i < f.count; ++i)
      std::printf("%.1f,%.1f ", art::kFramePoints[2 * (f.first + i)], art::kFramePoints[2 * (f.first + i) + 1]);
    std::printf("'/>\n");
  }
  for (const auto& b : art::kTabs)
    std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='4' fill='%s'/>\n", b.x0, b.y0, b.x1 - b.x0,
                b.y1 - b.y0, col(theme::kInk).c_str());
  for (const auto& t : art::kTexts) text(t.x, t.y, t.size * 0.92, theme::rgb(t.rgb), t.text, t.vertical);
  // Lunar 24 name plate and marks (in place of the original brand marks).
  std::printf("<circle cx='118' cy='180' r='62' fill='%s'/><circle cx='146' cy='164' r='60' fill='%s'/>\n",
              col(theme::kInk).c_str(), col(theme::kPanel).c_str());
  text(390, 184, 112, theme::kInk, "LUNAR");
  text(668, 184, 112, theme::kRed, "24");
  text(2010, 184, 44, theme::kInk, "AMBIENT DRONE MACHINE");
  text(1918, 1075, 26, theme::kRed, "LUNAR 24");
  std::printf("<circle cx='1199' cy='792' r='30' fill='%s'/><circle cx='1213' cy='784' r='28' fill='%s'/>\n",
              col(theme::kInk).c_str(), col(theme::kPanel).c_str());
  std::printf("<circle cx='1200' cy='1410' r='58' fill='%s'/><circle cx='1222' cy='1396' r='55' fill='%s'/>\n",
              col(theme::kPlate).c_str(), col(theme::kKeybed).c_str());

  for (const Widget& w : ws) {
    if (w.menu) continue;
    switch (w.kind) {
      case WidgetKind::Knob: knob(w); break;
      case WidgetKind::Button:
        std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='%s'/>\n", w.cx, w.cy, w.w / 2, col(theme::kInk).c_str());
        if (norm01(w.id) > 0.5)
          std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='none' stroke='%s' stroke-width='3'/>\n", w.cx, w.cy,
                      w.w / 2 + 3, col(theme::kAmber).c_str());
        break;
      case WidgetKind::Toggle: toggle(w); break;
      case WidgetKind::Jack: jack(w); break;
      case WidgetKind::Plate:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' fill='%s'/>\n", w.x(), w.y(), w.w, w.h,
                    col(theme::kPlate).c_str());
        for (double y = w.y() + 10; y < w.y() + w.h - 8; y += 12)
          std::printf("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='5'/>\n", w.x() + 9, y,
                      w.x() + w.w - 9, y, col(theme::kPlateRib).c_str());
        break;
      case WidgetKind::Joystick:
        std::printf("<circle cx='%.1f' cy='%.1f' r='55' fill='%s'/><circle cx='%.1f' cy='%.1f' r='24' fill='%s'/>\n",
                    w.cx, w.cy, col({50, 50, 50}).c_str(), w.cx, w.cy, col({85, 85, 85}).c_str());
        break;
      case WidgetKind::Cartridge: {
        if (w.id != 0) break;
        std::printf("<rect x='1143' y='238' width='114' height='51' fill='%s'/>\n", col(theme::kInk).c_str());
        std::printf("<rect x='1150' y='254' width='100' height='18' fill='%s'/>\n", col({160, 118, 46}).c_str());
        const core::ProgramDescriptor* p = core::find_program(defaults().leftEffector.program);
        text(1200, 263, 12, theme::kInk, p ? std::string(p->cartridge) : "");
        std::printf("<circle cx='1200' cy='323' r='16' fill='%s'/>\n", col(theme::kInk).c_str());
        break;
      }
      case WidgetKind::DroneKey:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='4' fill='%s'/>\n", w.x(), w.y(), w.w, w.h,
                    col({52, 52, 52}).c_str());
        std::printf("<rect x='%.1f' y='%.1f' width='10' height='12' fill='%s'/>\n", w.cx - 5, w.y() + 4,
                    col(theme::kPlate).c_str());
        break;
      case WidgetKind::Encoder:
        std::printf("<circle cx='%.1f' cy='%.1f' r='35' fill='%s'/><circle cx='%.1f' cy='%.1f' r='20' fill='%s'/>\n",
                    w.cx, w.cy, col({120, 120, 120}).c_str(), w.cx, w.cy, col(theme::kRed).c_str());
        break;
      case WidgetKind::OctaveKey:
        std::printf("<circle cx='%.1f' cy='%.1f' r='23' fill='%s'/>\n", w.cx, w.cy, col(theme::kPlate).c_str());
        break;
      case WidgetKind::Display:
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' fill='%s'/>\n", w.x(), w.y(), w.w, w.h,
                    col(theme::kDisplay).c_str());
        break;
      case WidgetKind::Decor:
        if (w.id == 2) {
          Widget k = w;
          k.kind = WidgetKind::Knob;
          k.cap = Cap::Black;
          k.id = 0;
          knob(k);
        } else if (w.id == 3) {
          jack(w);
        } else if (w.id == 0) {
          std::printf("<circle cx='%.1f' cy='%.1f' r='%.1f' fill='white' stroke='%s' stroke-width='2'/>\n", w.cx, w.cy,
                      w.w / 2, col(theme::kInk).c_str());
        } else {
          for (std::uint32_t i = 0; i < 5; ++i)
            std::printf("<rect x='%.1f' y='%.1f' width='9' height='30' fill='%s'/>\n", w.x() + 5 + i * 12.5, w.y() + 8,
                        col(defaults().parameters[w.id2 + i] > 0.5 ? theme::kLedOff : theme::kLedOn).c_str());
        }
        break;
    }
  }
  if (showMenu) {
    std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='10' fill='%s' opacity='0.97'/>\n", kMenuX0,
                kMenuY0, kMenuX1 - kMenuX0, kMenuY1 - kMenuY0, col(theme::kMenuBg).c_str());
    text((kMenuX0 + kMenuX1) / 2, kMenuY0 + 22, 18, theme::kMenuText, "KEYBOARD MENU  (click the encoder to close)");
    for (const Widget& w : ws) {
      if (!w.menu) continue;
      if (w.kind == WidgetKind::Knob) {
        knob(w);
      } else {
        std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='5' fill='none' stroke='%s'/>\n", w.x(), w.y(),
                    w.w, w.h, col(theme::kMenuText).c_str());
        const core::ParameterDescriptor* d = core::find_parameter(static_cast<core::ParameterId>(w.id));
        const int idx = indexOf(w.id);
        std::string opt =
            d && d->optionCount > 0 && idx >= 0 && idx < static_cast<int>(d->optionCount) ? d->options[idx] : "";
        for (auto& ch : opt) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        text(w.cx, w.cy, 11, theme::kMenuText, opt.substr(0, 10));
      }
      text(w.cx, w.cy + 42, 12, theme::kMenuText, w.label);
    }
  }
  std::printf("</svg>\n");
  return 0;
}
