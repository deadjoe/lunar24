# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Build .build/tutorial_preview: a copy of the app's tools/panel_preview.cpp with a few tutorial
fixtures (GENERATOR loaded, a 4-step sequence) and --widgets-named (control ids + boxes for the
red outlines). Never edits the app's own sources. Run from anywhere: python3 learn/source/build-preview-tool.py
"""
from pathlib import Path
import subprocess

repo = Path(__file__).resolve().parents[2]
build = repo / 'learn/.build'
s = (repo / 'tools/panel_preview.cpp').read_text()


def sub(old, new):
    global s
    assert old in s, f'panel_preview.cpp changed, update this patch: {old[:60]}'
    s = s.replace(old, new)


sub('const core::DeviceStateV1& defaults()', 'core::DeviceStateV1& defaults()')
sub('static const core::DeviceStateV1 st =', 'static core::DeviceStateV1 st =')
sub('#include <host/panel_ui_layout.h>', '#include <host/panel_ui_layout.h>\n#include <lunar24/registry.hpp>')
# The app lights a DRONE VOICES key only while that drone is on; at start-up all six are off.
sub('art::drawDroneKey(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h), true, false);',
    'art::drawDroneKey(sink, float(w.x()), float(w.y()), float(w.x() + w.w), float(w.y() + w.h), false, false);')
sub('st.arpMask = 0x12;', 'v[P::keyboard_seq_length] = 2.0 / 14.0; st.arpMask = 0x12;')
sub('st.steps[i] = {notes[i], i != 3 && i != 9};', 'st.steps[i] = {notes[i], true};')
sub('  const auto ws = build_panel_layout();\n', '''  if (option == "--generator") {
    defaults().leftEffector.program = core::ProgramId::program_generator_1;
    defaults().rightEffector.program = core::ProgramId::program_generator_1;
  }
  const auto ws = build_panel_layout();
  if (option == "--widgets-named") {
    std::printf("[\\n");
    bool first = true;
    for (const Widget& w : ws) {
      if (w.menu) continue;
      std::string name;
      const int k = static_cast<int>(w.kind);
      if (w.kind == WidgetKind::Jack) {
        for (const auto& j : registry::kJacks) if (static_cast<std::uint32_t>(j.id) == w.id) name = std::string(j.stable_id);
      } else if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::Button || w.kind == WidgetKind::Toggle) {
        for (const auto& q : registry::kParameters) if (static_cast<std::uint32_t>(q.id) == w.id) name = std::string(q.stable_id);
      } else {
        name = "kind" + std::to_string(k) + "." + std::to_string(w.id);
      }
      std::printf("%s  {\\"name\\": \\"%s\\", \\"kind\\": %d, \\"cx\\": %.1f, \\"cy\\": %.1f, \\"w\\": %.1f, \\"h\\": %.1f}",
                  first ? "" : ",\\n", name.c_str(), k, w.cx, w.cy, w.w, w.h);
      first = false;
    }
    std::printf("\\n]\\n");
    return 0;
  }
''')
build.mkdir(parents=True, exist_ok=True)
src = build / 'tutorial_preview.cpp'
src.write_text(s)
exe = build / 'tutorial_preview'
subprocess.run(['c++', '-O2', '-std=c++17', f'-I{repo}/host/include', f'-I{repo}/core/include', f'-I{repo}/generated',
                str(src), '-o', str(exe)], check=True)
(build / 'widgets.json').write_text(subprocess.run([str(exe), '--widgets-named'], check=True, capture_output=True, text=True).stdout)
print(exe)
