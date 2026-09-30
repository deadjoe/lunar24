#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Generate host/include/host/panel_art.generated.h from the official Solar 42N panel PDF.

Extracts, in 2400 x 1552 panel units:
  * the panel labels (position, size, colour, orientation),
  * the module frame outlines and the black title tabs,
  * the printed icons and marks (waveforms, arrows, LEDs, dotted rules, stripes, trimmer
    screws ...) — everything the panel prints except what the app draws itself.

Left out: brand marks (SOLAR 42N, ELTA MUSIC, the "AMBIENT MACHINE" logotype; Lunar 24 draws
its own name plates, see gen_panel_logo.py), the rear connector labels, and the hardware
controls (knobs, jacks, buttons, plates ...): any shape of the controls layer that sits inside
a widget of host/include/host/panel_ui_layout.h is skipped. The widget list comes from the
preview tool, so build it first.

The PDF itself is third-party material and is not committed. Usage:
    pip install pymupdf
    ./build/panel_preview --widgets > widgets.json
    python3 tools/gen_panel_art.py path/to/Solar42_panel.pdf widgets.json
"""

import json
import math
import os
import sys

import pymupdf

OUT = os.path.join(os.path.dirname(__file__), "..", "host", "include", "host",
                   "panel_art.generated.h")
DROP_TEXT = {"POWER", "12V DC", "12V", "DC", "EXT.", "AUDIO", "EXT. AUDIO", "R", "L", "WET", "OUT", "WET OUT"}
# How much of each widget's box counts as "the control" when skipping hardware shapes.
WIDGET_MARGIN = {0: 1.45, 1: 1.3, 3: 1.3, 4: 1.0}   # knob, button, jack, plate; others 1.2
JOYSTICK = 5


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def rgb(c):
    return (int(round(c[0] * 255)) << 16) | (int(round(c[1] * 255)) << 8) | int(round(c[2] * 255))


def bezier(p0, c1, c2, p3, n=8):
    out = []
    for k in range(1, n + 1):
        t = k / n
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
        out.append((a * p0.x + b * c1.x + c * c2.x + d * p3.x, a * p0.y + b * c1.y + c * c2.y + d * p3.y))
    return out


def subpaths(g, z):
    """The drawing's outline as a list of (points, closed) in panel units."""
    subs, cur = [], None
    for it in g["items"]:
        kind = it[0]
        if kind in ("l", "c"):
            start = it[1]
            if cur is None or abs(cur[-1][0] - start.x) > 1e-3 or abs(cur[-1][1] - start.y) > 1e-3:
                cur = [(start.x, start.y)]
                subs.append([cur, False])
            if kind == "l":
                cur.append((it[2].x, it[2].y))
            else:
                cur.extend(bezier(*it[1:5]))
        elif kind == "re":
            r = it[1]
            subs.append([[(r.x0, r.y0), (r.x1, r.y0), (r.x1, r.y1), (r.x0, r.y1)], True])
            cur = None
        elif kind == "qu":
            q = it[1]
            subs.append([[(q.ul.x, q.ul.y), (q.ur.x, q.ur.y), (q.lr.x, q.lr.y), (q.ll.x, q.ll.y)], True])
            cur = None
    if g.get("closePath") and subs:
        subs[-1][1] = True
    return [([(x * z, y * z) for x, y in pts], closed) for pts, closed in subs]


def inside(pt, poly):
    x, y = pt
    c = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def area(poly):
    return abs(sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]))) / 2


def holes(subs):
    """Which sub-paths are holes: those nested inside an odd number of the others. The app's
    renderer (NanoVG) needs this per sub-path; it has no even-odd fill rule."""
    out = []
    for i, (p, _) in enumerate(subs):
        depth = sum(1 for j, (q, _) in enumerate(subs)
                    if j != i and len(q) > 2 and area(q) > area(p) and inside(p[0], q))
        out.append(depth % 2 == 1)
    return out


def dash_pattern(g):
    d = g.get("dashes")
    if not d or d.startswith("[]"):
        return None
    nums = [float(v) for v in d[d.index("[") + 1:d.index("]")].split()]
    return nums if any(nums) else None


def expand_dashes(subs, pattern, z):
    """Dashed stroke -> (dash segments, dot centres)."""
    pat = [v * z for v in pattern]
    if pat[0] == 0:  # round-capped dots every sum(pattern)
        step, dots = sum(pat), []
        for pts, _ in subs:
            walked, nxt = 0.0, 0.0
            for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
                seg = math.hypot(x1 - x0, y1 - y0)
                while nxt <= walked + seg + 1e-6 and seg > 0:
                    t = (nxt - walked) / seg
                    dots.append((x0 + t * (x1 - x0), y0 + t * (y1 - y0)))
                    nxt += step
                walked += seg
        return [], dots
    on, off = pat[0], pat[1] if len(pat) > 1 else pat[0]
    dashes = []
    for pts, _ in subs:
        pos, drawing, left = 0.0, True, on
        cur = [pts[0]]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            seg = math.hypot(x1 - x0, y1 - y0)
            t0 = 0.0
            while seg > 0 and (seg - t0) > left:
                t0 += left
                p = (x0 + (x1 - x0) * t0 / seg, y0 + (y1 - y0) * t0 / seg)
                if drawing:
                    cur.append(p)
                    dashes.append(cur)
                cur = [p]
                drawing = not drawing
                left = on if drawing else off
            left -= seg - t0
            if drawing:
                cur.append((x1, y1))
        if drawing and len(cur) > 1:
            dashes.append(cur)
    return [(d, False) for d in dashes], []


def main(pdf, widgets_json):
    doc = pymupdf.open(pdf)
    page = doc[0]
    z = 2400.0 / page.rect.width

    boxes = []
    for w in json.load(open(widgets_json)):
        k, cx, cy, ww, hh = w["kind"], w["cx"], w["cy"], w["w"], w["h"]
        if k == JOYSTICK:
            ww = hh = 130  # only the stick; its printed arrows are kept
        m = WIDGET_MARGIN.get(k, 1.2)
        boxes.append((cx - ww * m / 2, cy - hh * m / 2, cx + ww * m / 2, cy + hh * m / 2))

    def on_widget(x0, y0, x1, y1):
        return any(x0 >= a - 0.5 and y0 >= b - 0.5 and x1 <= c + 0.5 and y1 <= d + 0.5 for a, b, c, d in boxes)

    texts = []
    for block in page.get_text("dict")["blocks"]:
        for line in block.get("lines", []):
            dx, dy = line.get("dir", (1, 0))
            vertical = abs(dx) < 0.5
            for span in line["spans"]:
                t = span["text"].strip()
                if not t or span["font"].startswith("Magistral"):
                    continue  # brand logotypes
                x0, y0, x1, y1 = span["bbox"]
                cx, cy = (x0 + x1) / 2 * z, (y0 + y1) / 2 * z
                if cy < 120 and t in DROP_TEXT:
                    continue  # rear-panel connector labels (EXT AUDIO, WET OUT, POWER)
                t = t.encode("ascii", "ignore").decode()
                if not t:
                    continue
                texts.append((cx, cy, span["size"] * z, span["color"], vertical, t))

    frames, tabs, shapes, dots, leds = [], [], [], [], []
    for g in page.get_drawings():
        r = g["rect"]
        x0, y0, x1, y1 = r.x0 * z, r.y0 * z, r.x1 * z, r.y1 * z
        w, h = x1 - x0, y1 - y0
        fill, col, layer = g.get("fill"), g.get("color"), g.get("layer")
        if layer == "BG" or (w < 0.4 and h < 0.4):
            continue
        if g.get("type") == "s" and col and max(col) < 0.2 and w > 250 and h > 100:
            frames.append([p for pts, _ in subpaths(g, z) for p in pts])
            continue
        glyph = 805 < y0 < 830 and (95 < x0 < 145 or 2065 < x0 < 2115)  # drone 3/6 antenna glyph
        logo = y0 < 235 and not (1100 < x0 < 1130)                      # brand letters (keep DUAL EFFECTOR tab)
        if fill and max(fill) < 0.15 and not glyph and not logo:
            if (40 < w and 12 < h < 60 and not (44 <= w <= 48 and 44 <= h <= 48)) or (14 < w < 45 and 40 < h < 400):
                tabs.append((x0, y0, x1, y1))
                continue
        # ---- printed icons and marks ----
        if y1 < 240 and (x1 < 820 or x0 > 1780):
            continue  # SOLAR 42N, AMBIENT MACHINE
        if y1 < 120:
            continue  # rear connector brackets
        if 1150 < x0 and x1 < 1250 and 770 < y0 and y1 < 862:
            continue  # ELTA MUSIC letters (its striped rules stay)
        if layer not in ("BLACK", "RED") and on_widget(x0, y0, x1, y1):
            continue  # the hardware controls: the app draws those
        if (layer not in ("BLACK", "RED") and fill and g.get("type") == "f" and 12 < w < 22 and abs(w - h) < 2
                and max(fill) - min(fill) > 0.3):
            leds.append(((x0 + x1) / 2, (y0 + y1) / 2, w / 2, rgb(fill)))
            continue
        subs = subpaths(g, z)
        if not subs:
            continue
        kind = g.get("type")
        stroke_w = (g.get("width") or 1.0) * z
        pat = dash_pattern(g) if "s" in kind else None
        if pat:
            dsubs, dcentres = expand_dashes(subs, pat, z)
            dots += [(x, y, stroke_w / 2, rgb(col)) for x, y in dcentres]
            if dsubs:
                shapes.append((dsubs, 0, rgb(col), stroke_w, 2))
            continue
        flags = (1 if "f" in kind else 0) | (2 if "s" in kind else 0) | (4 if g.get("even_odd") else 0)
        shapes.append((subs, rgb(fill) if fill else 0, rgb(col) if col and "s" in kind else 0, stroke_w, flags))
    tabs = sorted(set((round(a, 1), round(b, 1), round(c, 1), round(d, 1)) for a, b, c, d in tabs))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("// Copyright (c) 2026 Lunar 24 contributors\n// SPDX-License-Identifier: Apache-2.0\n//\n")
        f.write("// GENERATED by tools/gen_panel_art.py from the official Solar 42N panel drawing.\n")
        f.write("// Panel labels, module frames, title tabs and printed marks in 2400 x 1552 panel units.\n")
        f.write("// Types: host/panel_art.h.\n\n#pragma once\n\nnamespace lunar24::host::art {\n\n")
        f.write("inline constexpr Text kTexts[] = {\n")
        for cx, cy, size, c, vert, t in texts:
            f.write(f"  {{{cx:.1f}f, {cy:.1f}f, {size:.1f}f, 0x{c:06x}u, {'true' if vert else 'false'}, {c_str(t)}}},\n")
        f.write("};\n\ninline constexpr Box kTabs[] = {\n")
        for a, b, c, d in tabs:
            f.write(f"  {{{a:.1f}f, {b:.1f}f, {c:.1f}f, {d:.1f}f}},\n")
        f.write("};\n\ninline constexpr float kFramePoints[] = {\n")
        index, first = [], 0
        for fr in frames:
            f.write("  " + ", ".join(f"{x:.1f}f, {y:.1f}f" for x, y in fr) + ",\n")
            index.append((first, len(fr)))
            first += len(fr)
        f.write("};\n\ninline constexpr Frame kFrames[] = {\n")
        for a, n in index:
            f.write(f"  {{{a}u, {n}u}},\n")
        f.write("};\n\n")
        write_shapes(f, "Decor", shapes)
        f.write("inline constexpr Dot kDots[] = {\n")
        for x, y, rr, c in dots:
            f.write(f"  {{{x:.1f}f, {y:.1f}f, {rr:.2f}f, 0x{c:06x}u}},\n")
        f.write("};\n\ninline constexpr Led kLeds[] = {\n")
        for x, y, rr, c in leds:
            f.write(f"  {{{x:.1f}f, {y:.1f}f, {rr:.1f}f, 0x{c:06x}u}},\n")
        f.write("};\n\n}  // namespace lunar24::host::art\n")
    print(f"wrote {OUT}: {len(texts)} texts, {len(tabs)} tabs, {len(frames)} frames, "
          f"{len(shapes)} shapes, {len(dots)} dots, {len(leds)} LEDs")


def write_shapes(f, name, shapes):
    """Emit k<name>Points / k<name>SubPaths / k<name>Shapes."""
    pts, subs, out = [], [], []
    for sub_list, fill, stroke, width, flags in shapes:
        first_sub = len(subs)
        hole = holes(sub_list) if flags & 1 else [False] * len(sub_list)
        for (p, closed), h in zip(sub_list, hole):
            subs.append((len(pts), len(p), closed, h))
            pts.extend(p)
        out.append((first_sub, len(sub_list), fill, stroke, width, flags))
    f.write(f"inline constexpr float k{name}Points[] = {{\n")
    for i in range(0, len(pts), 8):
        f.write("  " + " ".join(f"{x:.1f}f, {y:.1f}f," for x, y in pts[i:i + 8]) + "\n")
    f.write(f"}};\n\ninline constexpr SubPath k{name}SubPaths[] = {{\n")
    for a, n, closed, h in subs:
        f.write(f"  {{{a}u, {n}u, {'true' if closed else 'false'}, {'true' if h else 'false'}}},\n")
    f.write(f"}};\n\ninline constexpr Shape k{name}Shapes[] = {{\n")
    for a, n, fill, stroke, width, flags in out:
        f.write(f"  {{{a}u, {n}u, 0x{fill:06x}u, 0x{stroke:06x}u, {width:.2f}f, {flags}}},\n")
    f.write("};\n\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
