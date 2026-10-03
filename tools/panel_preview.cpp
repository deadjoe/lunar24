// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// panel_preview — render the panel (art + controls at their default positions) to SVG,
// so the UI can be reviewed on any machine without building the app:
//
//   panel_preview > panel.svg          (--leds shows the indicator LEDs lit)
//   panel_preview --menu[=play|expression|arp|seq|steps|service]   keyboard menu, factory values
//   panel_preview --menu-example=<tab> / --menu-split / --menu-armed
//   panel_preview --midi / --midi-example / --midi-learn / --midi-wait / --midi-error
//   panel_preview --widgets > w.json   (control boxes, for tools/gen_panel_art.py)

#include <algorithm>
#include <utility>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <host/panel_art.h>
#include <host/keyboard_menu_view.h>
#include <host/midi_settings_view.h>
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
  // Text cells use the same bounds and alignment (0 left, 1 centre, 2 right) as the native
  // overlays. SVG retains the font size and clips overflow; the native sink measures and
  // ellipsizes with the embedded Noto font. data-fit lets export tools do likewise.
  void label(midi_ui::Box b, float size, std::uint32_t color, const char* value, bool bold, int align) {
    const float w = b.r - b.l;
    std::printf("<svg x='%.1f' y='%.1f' width='%.1f' height='%.1f' overflow='hidden'>"
                "<text x='%.1f' y='%.1f' font-size='%.1f' fill='%s' text-anchor='%s' "
                "dominant-baseline='central' font-family='Noto Sans, sans-serif' font-weight='%d' "
                "data-fit='%.1f'>%s</text></svg>\n",
                b.l,b.t,w,b.b-b.t,align==1?w/2:align==2?w:0.f,(b.b-b.t)/2,size,hex(color).c_str(),
                align==1?"middle":align==2?"end":"start",bold?700:400,w,esc(value).c_str());
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
  const theme::Rgb c = theme::cap(w.cap), t = theme::kSkirt;
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
  const std::string option = argc > 1 ? argv[1] : "";
  const bool showMidi = option.rfind("--midi", 0) == 0;
  const bool midiExample = showMidi && option != "--midi";
  const bool showMenu = option.rfind("--menu", 0) == 0;
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
      case WidgetKind::MidiSettings:
        art::drawButton(sink, float(w.cx), float(w.cy), float(w.w / 2), showMidi, false);
        text(w.cx, w.cy - 42, 15, theme::kInk, "MIDI");
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
  if (showMenu) {
    using P = core::ParameterId;
    std::map<P, double> v;  // values that differ from the factory ones
    kb_ui::State st;
    st.value = [&v](P id) {
      const auto it = v.find(id);
      return it != v.end() ? it->second : defaults().parameters[static_cast<std::size_t>(id)];
    };
    const std::size_t eq = option.find('=');
    const std::string tab = eq == std::string::npos ? "play" : option.substr(eq + 1);
    const char* tabs[kb_ui::kTabCount] = {"play", "expression", "arp", "seq", "steps", "service"};
    for (int i = 0; i < kb_ui::kTabCount; ++i)
      if (tab == tabs[i]) st.tab = i;
    for (int i = 0; i < kb_ui::kSeqSteps; ++i) {
      const auto& q = defaults().keyboardSeqCurrent.steps[static_cast<std::size_t>(i)];
      st.steps[i] = {int(q.note), q.gate != 0};
    }
    if (option.rfind("--menu-example", 0) == 0) {  // a played-in patch (valid values)
      v = {{P::keyboard_quantise_load_scale, 2}, {P::keyboard_root_note, 2.0 / 11.0},
           {P::keyboard_portamento_speed, 60 / 255.0}, {P::keyboard_vibrato_speed, 40 / 127.0},
           {P::keyboard_vibrato_depth, 22 / 127.0}, {P::keyboard_vibrato_delay, 64 / 127.0},
           {P::keyboard_vibrato_pressure, 0.5}, {P::keyboard_pressure_output, 1},
           {P::keyboard_pressure_rise, 40 / 255.0}, {P::keyboard_pressure_fall, 120 / 255.0},
           {P::keyboard_arp_hold, 1}, {P::keyboard_arp_direction, 2}, {P::keyboard_arp_variation, 1},
           {P::keyboard_arp_interval, 6.0 / 11.0}, {P::keyboard_arp_length, 1.0},
           {P::keyboard_seq_length, 10.0 / 14.0}, {P::keyboard_seq_rhythm_length, 5.0 / 7.0}};
      st.arpMask = 0x12;
      st.seqMask = 0x04;
      const int notes[kb_ui::kSeqSteps] = {0, 7, 12, 7, 3, 10, 15, 10, 0, 5, 12, 19, 24, 12, 7, 0};
      for (int i = 0; i < kb_ui::kSeqSteps; ++i) st.steps[i] = {notes[i], i != 3 && i != 9};
    }
    if (option == "--menu-split") {  // PLAY = SPLIT, editing the right half, preset C
      v[P::keyboard_behaviour] = 2;
      st.split = true;
      st.side = 1;
      st.presetSlot = 2;
    }
    if (option == "--menu-armed") st.resetArmed = st.initArmed = true;
    kb_ui::draw(sink, st);
  }
  if (showMidi) {
    core::MidiMap map;
    midi_ui::State state;
    state.map = &map;
    if (midiExample) {
      state.device = "MPK mini IV";
      core::MidiBinding b{};
      std::snprintf(b.key.device,sizeof(b.key.device),"MPK mini IV");
      b.key.channel=1; b.key.number=21; b.parameter=core::ParameterId::vcf_l_freq; map.bind(b);
      b.key.number=22; b.parameter=core::ParameterId::effector_blend;
      b.mode=core::MidiInputMode::relativeBinOffset; map.bind(b);
      b.mode=core::MidiInputMode::absolute; b.key.kind=core::MidiBindingKind::note;
      b.key.number=36; b.targetKind=core::MidiTargetKind::action; b.action=core::MidiAction::drone_key_1; map.bind(b);
      b.key.number=37; b.action=core::MidiAction::master_mute; map.bind(b);
    }
    if (option == "--midi-learn") { state.armed=true; state.awaitTarget=true; }
    if (option == "--midi-wait") state.armed=true;
    if (option == "--midi-error") state.editable=false;
    if (option == "--midi-long") {
      state.device = std::string(63,'W');
      core::MidiBinding b{};
      std::snprintf(b.key.device,sizeof(b.key.device),"%s",state.device.c_str());
      b.key.number=99; b.parameter=core::ParameterId::vcf_l_freq; map.bind(b);
      state.offset=4;
    }
    if (option == "--midi-page") {
      for (int i=0; i<5; ++i) {
        core::MidiBinding b{}; b.key.number=static_cast<std::uint8_t>(30+i);
        b.parameter=core::ParameterId::vco_a_tune; map.bind(b);
      }
      state.offset=4;
    }
    if (option == "--midi-min") { state.octave=-36; state.curve=2; }
    midi_ui::draw(sink,state);
  }
  std::printf("</svg>\n");
  return 0;
}
