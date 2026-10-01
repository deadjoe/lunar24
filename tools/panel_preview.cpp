// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_preview — render the panel (art + controls at their default positions) to SVG,
// so the UI can be reviewed on any machine without building the app:
//
//   panel_preview > panel.svg          (add --menu / --seq to show a keyboard menu page,
//                                       --leds to show the indicator LEDs lit)
//   panel_preview --widgets > w.json   (control boxes, for tools/gen_panel_art.py)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <host/panel_art.h>
#include <host/panel_format.h>
#include <host/panel_theme.h>
#include <host/panel_ui_layout.h>
#include <lunar24/core/state_default.h>

using namespace lunar24;
using namespace lunar24::host;

namespace {


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

// Draws the static panel art (host/panel_art.h) as SVG.
struct SvgSink {
  std::string d;
  static std::string hex(std::uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", c & 0xffffffu);
    return b;
  }
  void fillRect(float x0, float y0, float x1, float y1, std::uint32_t c, float radius) {
    std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='%.1f' fill='%s'/>\n", x0, y0, x1 - x0, y1 - y0,
                radius, hex(c).c_str());
  }
  void fillCircle(float cx, float cy, float r, std::uint32_t c) {
    std::printf("<circle cx='%.1f' cy='%.1f' r='%.2f' fill='%s'/>\n", cx, cy, r, hex(c).c_str());
  }
  void moveTo(float x, float y) { point('M', x, y); }
  void lineTo(float x, float y) { point('L', x, y); }
  void closePath() { d += "Z"; }
  void markHole() {}
  void fillPath(std::uint32_t c, bool evenOdd) {
    std::printf("<path d='%s' fill='%s' fill-rule='%s'/>\n", d.c_str(), hex(c).c_str(), evenOdd ? "evenodd" : "nonzero");
    d.clear();
  }
  void strokePath(std::uint32_t c, float width) {
    std::printf("<path d='%s' fill='none' stroke='%s' stroke-width='%.2f' stroke-linejoin='round'/>\n", d.c_str(),
                hex(c).c_str(), width);
    d.clear();
  }
  void text(float x, float y, float size, std::uint32_t c, bool vertical, const char* s) {
    ::text(x, y, size, theme::rgb(c), s, vertical);
  }
  void circle(float cx, float cy, float r) {
    char b[160];
    std::snprintf(b, sizeof b, "M%.2f %.2fa%.2f %.2f 0 1 0 %.2f 0a%.2f %.2f 0 1 0 %.2f 0Z", cx - r, cy, r, r, 2 * r, r,
                  r, -2 * r);
    d += b;
  }
  // Emits the gradient definition and returns the paint reference for it.
  std::string paint(const art::Grad& g) {
    auto rgba = [](std::uint32_t c, float a) {
      char b[64];
      std::snprintf(b, sizeof b, "stop-color='%s' stop-opacity='%.3f'", hex(c).c_str(), a);
      return std::string(b);
    };
    if (g.c0 == g.c1 && g.a0 == g.a1 && !g.radial) {
      char b[80];
      std::snprintf(b, sizeof b, "%s' fill-opacity='%.3f' stroke-opacity='%.3f", hex(g.c0).c_str(), g.a0, g.a0);
      return b;
    }
    char id[16];
    std::snprintf(id, sizeof id, "g%d", ++grads);
    if (g.radial)
      std::printf("<defs><radialGradient id='%s' gradientUnits='userSpaceOnUse' cx='%.2f' cy='%.2f' r='%.2f'>"
                  "<stop offset='%.3f' %s/><stop offset='1' %s/></radialGradient></defs>\n",
                  id, g.x0, g.y0, g.y1, g.y1 > 0 ? g.x1 / g.y1 : 0.f, rgba(g.c0, g.a0).c_str(), rgba(g.c1, g.a1).c_str());
    else
      std::printf("<defs><linearGradient id='%s' gradientUnits='userSpaceOnUse' x1='%.2f' y1='%.2f' x2='%.2f' "
                  "y2='%.2f'><stop offset='0' %s/><stop offset='1' %s/></linearGradient></defs>\n",
                  id, g.x0, g.y0, g.x1, g.y1, rgba(g.c0, g.a0).c_str(), rgba(g.c1, g.a1).c_str());
    return std::string("url(#") + id + ")";
  }
  void fillGrad(const art::Grad& g) {
    std::printf("<path d='%s' fill='%s'/>\n", d.c_str(), paint(g).c_str());
    d.clear();
  }
  void strokeGrad(const art::Grad& g, float width) {
    std::printf("<path d='%s' fill='none' stroke='%s' stroke-width='%.2f' stroke-linecap='round' "
                "stroke-linejoin='round'/>\n",
                d.c_str(), paint(g).c_str(), width);
    d.clear();
  }
  int grads = 0;
  void point(char op, float x, float y) {
    char b[40];
    std::snprintf(b, sizeof b, "%c%.1f %.1f", op, x, y);
    d += b;
  }
};

SvgSink& svg() {
  static SvgSink sink;
  return sink;
}

void knob(const Widget& w) {
  const double ang = theme::kKnobMinDeg + norm01(w.id) * (theme::kKnobMaxDeg - theme::kKnobMinDeg);
  const theme::Rgb c = theme::cap(w.cap), t = w.menu ? theme::kMenuText : theme::kSkirt;
  art::drawKnob(svg(), float(w.cx), float(w.cy), float(w.w / 2), (std::uint32_t(c.r) << 16) | (c.g << 8) | c.b,
                w.cap != Cap::Black, float(ang), (std::uint32_t(t.r) << 16) | (t.g << 8) | t.b,
                float(theme::kKnobMinDeg), float(theme::kKnobMaxDeg), false);
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
  int pos = 0;
  for (int p = 0; p < n && p < 3; ++p)
    if (w.leverIndex[p] == idx) pos = p;
  art::drawToggle(svg(), float(w.cx), float(w.cy), n <= 1 ? 0.f : float(pos) / float(n - 1), false);
}

void jack(const Widget& w) { art::drawJack(svg(), float(w.cx), float(w.cy), float(w.w / 2), false); }

}  // namespace

int main(int argc, char** argv) {
  const bool showMenu = argc > 1 && std::strcmp(argv[1], "--menu") == 0;
  const bool showSeq = argc > 1 && std::strcmp(argv[1], "--seq") == 0;
  const bool showLeds = argc > 1 && std::strcmp(argv[1], "--leds") == 0;
  const auto ws = build_panel_layout();
  if (argc > 1 && std::strcmp(argv[1], "--widgets") == 0) {
    std::printf("[\n");
    bool first = true;
    for (const Widget& w : ws) {
      if (w.menu) continue;
      std::printf("%s  {\"kind\": %d, \"cx\": %.1f, \"cy\": %.1f, \"w\": %.1f, \"h\": %.1f}", first ? "" : ",\n",
                  static_cast<int>(w.kind), w.cx, w.cy, w.w, w.h);
      first = false;
    }
    std::printf("\n]\n");
    return 0;
  }
  std::printf("<svg xmlns='http://www.w3.org/2000/svg' width='2400' height='1552' viewBox='0 0 2400 1552'>\n");
  SvgSink& sink = svg();
  art::drawPanelArt(sink);
  if (showLeds) {  // alternate full and half brightness
    int i = 0;
    for (const auto& l : art::kLeds) art::drawLed(sink, l.x, l.y, l.r, l.rgb, (i++ % 2) ? 0.5f : 1.f);
  }

  for (const Widget& w : ws) {
    if (w.menu) continue;
    switch (w.kind) {
      case WidgetKind::Knob: knob(w); break;
      case WidgetKind::Button:
        art::drawButton(sink, float(w.cx), float(w.cy), float(w.w / 2), norm01(w.id) > 0.5, false);
        break;
      case WidgetKind::Toggle: toggle(w); break;
      case WidgetKind::Jack: jack(w); break;
      case WidgetKind::Plate:
        art::drawPlate(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h), false);
        break;
      case WidgetKind::Joystick:
        art::drawJoystick(sink, float(w.cx), float(w.cy), 55.f, 90.f, float(w.cx), float(w.cy), false);
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
        art::drawDroneKey(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h), true, false);
        break;
      case WidgetKind::MasterMute:
        art::drawButton(sink, float(w.cx), float(w.cy), float(w.w / 2), false, false);
        text(w.cx, w.cy - 42, 15, theme::kInk, "MUTE");
        break;
      case WidgetKind::Encoder: art::drawEncoder(sink, float(w.cx), float(w.cy), false); break;
      case WidgetKind::OctaveKey: art::drawOctaveKey(sink, float(w.cx), float(w.cy), false); break;
      case WidgetKind::Display:
        art::drawDisplay(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h));
        text(w.cx, w.cy, 18, {235, 240, 255}, "OCT +0");
        art::drawDisplayGlass(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h));
        break;
      case WidgetKind::Decor:
        if (w.id == 3) {
          jack(w);
        } else if (w.id == 0) {
          art::drawSensor(sink, float(w.cx), float(w.cy), float(w.w / 2));
        } else {
          for (std::uint32_t i = 0; i < 5; ++i)
            std::printf("<rect x='%.1f' y='%.1f' width='9' height='30' fill='%s'/>\n", w.x() + 5 + i * 12.5, w.y() + 8,
                        col(defaults().parameters[w.id2 + i] > 0.5 ? theme::kLedOff : theme::kLedOn).c_str());
        }
        break;
    }
  }
  if (showMenu || showSeq) {
    std::printf("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='10' fill='%s'/>\n", kMenuX0,
                kMenuY0, kMenuX1 - kMenuX0, kMenuY1 - kMenuY0, col(theme::kMenuBg).c_str());
    text((kMenuX0 + kMenuX1) / 2, kMenuY0 + 22, 18, theme::kMenuText, "KEYBOARD MENU");
    const Rect a = kMenuTabSettings, b = kMenuTabSequencer;
    art::drawMenuTab(sink, float(a.x0), float(a.y0), float(a.x1), float(a.y1), "SETTINGS", showMenu, false);
    art::drawMenuTab(sink, float(b.x0), float(b.y0), float(b.x1), float(b.y1), "SEQUENCER", showSeq, false);
    const Rect c = kMenuClose;
    art::drawMenuTab(sink, float(c.x0), float(c.y0), float(c.x1), float(c.y1), "CLOSE", false, false);
    const Rect rs = kMenuReset;
    art::drawMenuTab(sink, float(rs.x0), float(rs.y0), float(rs.x1), float(rs.y1), "RESET PANEL", false, false);
  }
  if (showSeq) {
    const Rect sw = kSeqSideSwitch;
    art::drawMenuTab(sink, float(sw.x0), float(sw.y0), float(sw.x1), float(sw.y1), "EDIT: LEFT", false, false);
    text(440, (kSeqSliderTop + kSeqSliderBottom) / 2, 12, theme::kMenuText, "NOTE", true);
    text(440, kSeqGateY, 12, theme::kMenuText, "GATE");
    for (int i = 0; i < kSeqSteps; ++i) {
      const Rect r = seq_step_rect(i);
      const auto& st = defaults().keyboardSeqCurrent.steps[static_cast<std::size_t>(i)];
      art::drawSeqStep(sink, float(r.x0), float(r.x1), float(kSeqSliderTop), float(kSeqSliderBottom),
                       float(kSeqGateY), i, st.note, 24, st.gate != 0, false);
    }
  }
  if (showMenu) {
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
      if (w.kind == WidgetKind::Knob) text(w.cx, w.cy + 58, 11, theme::kAmber, formatParam(w.id, defaults().parameters[w.id]));
    }
  }
  std::printf("</svg>\n");
  return 0;
}
