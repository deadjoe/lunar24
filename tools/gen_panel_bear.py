#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Generate host/include/host/panel_bear.generated.h: the Bearbone.Studio bear, as line art.

The Solar 42F prints a line-drawn figure round its joystick; Lunar 24 prints the studio's bear
there (owner, 2026-10-05). The bear is traced from the logo (host/resources/bearbone_logo.png):

  * the head-and-body outline as a double line (a heavy outer line and a thin line inside it),
    the ears drawn where they show behind the head, with an inner arc;
  * the eyes and nose filled, the muzzle outlined, the mouth and the leg lines drawn by hand
    over the logo's (the leg lines are shortened so the belly stays clear for the stick).

The joystick sits in the belly. Placement and line widths are tuned by eye.

Usage:
    pip install pillow numpy scipy scikit-image
    python3 tools/gen_panel_bear.py
"""

import os

import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from skimage import measure

HERE = os.path.dirname(__file__)
LOGO = os.path.join(HERE, "..", "host", "resources", "bearbone_logo.png")
OUT = os.path.join(HERE, "..", "host", "include", "host", "panel_bear.generated.h")

INK = 0x0c0a0a
JOYSTICK = (180.0, 1297.0)   # the stick's centre in panel units (panel_ui_layout.h)
BELLY = (268.0, 322.0)       # the point of the logo (512 px tall) that sits on the stick
SCALE = 0.88                 # panel units per logo pixel: head clear of LFO A, belly round the stick
OUTER_W, INNER_W, DETAIL_W = 4.6, 1.8, 3.0   # line widths, panel units
INNER_GAP = 8.5              # logo pixels between the outer and the inner line

# Hand-drawn strokes in logo pixels: quadratic curves (start, control, end).
LEG_LINES = [
    ((171, 369), (192, 388), (218, 398)),   # front leg, left
    ((258, 392), (257, 420), (255, 448)),   # between the front legs
    ((356, 372), (348, 412), (339, 455)),   # hind leg
    ((398, 342), (382, 368), (353, 386)),   # haunch
]
MOUTH = [
    ((190, 203), (190, 208), (190, 213)),
    ((190, 212), (186, 217), (181, 217)),
    ((190, 212), (194, 217), (199, 217)),
]


def to_panel(x, y):
    return (JOYSTICK[0] + (x - BELLY[0]) * SCALE, JOYSTICK[1] + (y - BELLY[1]) * SCALE)


def contours(mask, sigma=1.2, tol=0.35):
    """Smooth outlines of a mask, as (x, y) logo-pixel point lists."""
    field = ndi.gaussian_filter(mask.astype(float), sigma)
    out = []
    for c in measure.find_contours(np.pad(field, 2), 0.5):
        c = measure.approximate_polygon(c, tol)
        if len(c) >= 4:
            out.append([(p[1] - 2, p[0] - 2) for p in c])
    return out


def quad(p0, c, p1, n=12):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1])
            for t in (k / n for k in range(n + 1))]


def outside(points, mask):
    """Runs of an outline that lie outside a mask (for the ears behind the head)."""
    runs, cur = [], []
    h, w = mask.shape
    for x, y in points + points[:1]:
        xi, yi = int(round(x)), int(round(y))
        inside = 0 <= xi < w and 0 <= yi < h and mask[yi, xi]
        if inside:
            if len(cur) > 1:
                runs.append(cur)
            cur = []
        else:
            cur.append((x, y))
    if len(cur) > 1:
        runs.append(cur)
    return runs


def main():
    a = np.array(Image.open(LOGO).convert("RGBA")).astype(int)
    alpha, rgb = a[:, :, 3], a[:, :, :3]
    lum = rgb.mean(axis=2)
    sil = alpha > 128
    muzzle = sil & (rgb[:, :, 0] > 180) & (rgb[:, :, 2] < 140)
    dark = sil & (lum < 70) & ~muzzle

    labels, n = ndi.label(dark)
    sizes = ndi.sum(dark, labels, range(1, n + 1))
    blobs = [(i + 1, s) for i, s in enumerate(sizes) if s > 150]
    def box(i):
        ys, xs = np.nonzero(labels == i)
        return xs.mean(), ys.mean(), xs.min(), ys.min()
    ears = [i for i, _ in blobs if box(i)[3] < 60]                    # the two blobs on top
    nose = max((i for i, _ in blobs if i not in ears), key=lambda i: sizes[i - 1])
    eyes = [i for i in range(1, n + 1)
            if 100 < sizes[i - 1] < 260 and box(i)[1] < 170 and i not in ears]

    ear_mask = ndi.binary_fill_holes(np.isin(labels, ears))
    ear_mask = ndi.binary_closing(ear_mask, iterations=3)
    body = ndi.binary_fill_holes(sil & ~ear_mask)
    body = ndi.binary_opening(body, iterations=2)
    body_near = ndi.binary_dilation(body, iterations=2)

    strokes = []   # (points, closed, width)
    fills = []     # points
    body_out = max(contours(body), key=len)
    strokes.append((body_out, True, OUTER_W))
    inner = ndi.distance_transform_edt(body) > INNER_GAP
    strokes.append((max(contours(inner), key=len), True, INNER_W))
    for e in ears:
        one = ndi.binary_fill_holes(ndi.binary_closing(labels == e, iterations=3))
        for run in outside(max(contours(one), key=len), body_near):
            strokes.append((run, False, OUTER_W))
        ring = ndi.distance_transform_edt(one) > INNER_GAP
        near = ndi.binary_dilation(body, iterations=int(INNER_GAP))
        for run in outside(max(contours(ring), key=len), near):
            strokes.append((run, False, INNER_W))
    strokes.append((max(contours(ndi.binary_fill_holes(muzzle | (labels == nose))), key=len), True, DETAIL_W))
    nose_only = ndi.binary_opening(labels == nose, iterations=3)
    fills.append(max(contours(nose_only), key=len))
    for e in eyes:
        fills.append(max(contours(labels == e, sigma=0.8), key=len))
    for p0, c, p1 in LEG_LINES:
        strokes.append((quad(p0, c, p1), False, DETAIL_W))
    for p0, c, p1 in MOUTH:
        strokes.append((quad(p0, c, p1, 6), False, DETAIL_W * 0.7))

    points, subs, shapes = [], [], []
    def add_sub(pts, closed):
        subs.append((len(points), len(pts), closed))
        points.extend(to_panel(x, y) for x, y in pts)
    for pts, closed, width in strokes:
        shapes.append((len(subs), 1, INK, width, 2))
        add_sub(pts, closed)
    for pts in fills:
        shapes.append((len(subs), 1, INK, 0.0, 1))
        add_sub(pts, True)

    with open(OUT, "w") as f:
        f.write("// Copyright (c) 2026 Lunar 24 contributors\n// SPDX-License-Identifier: Apache-2.0\n//\n")
        f.write("// GENERATED by tools/gen_panel_bear.py from host/resources/bearbone_logo.png.\n")
        f.write("// The Bearbone.Studio bear round the joystick, as line art, in 2400 x 1552 panel units.\n")
        f.write("// Types: host/panel_art.h.\n\n#pragma once\n\nnamespace lunar24::host::art {\n\n")
        f.write("inline constexpr float kBearPoints[] = {\n")
        for i in range(0, len(points), 6):
            f.write("  " + " ".join(f"{x:.1f}f, {y:.1f}f," for x, y in points[i:i + 6]) + "\n")
        f.write("};\n\ninline constexpr SubPath kBearSubPaths[] = {\n")
        for first, count, closed in subs:
            f.write(f"  {{{first}u, {count}u, {'true' if closed else 'false'}, false}},\n")
        f.write("};\n\ninline constexpr Shape kBearShapes[] = {\n")
        for first, count, rgb_, width, flags in shapes:
            fill = rgb_ if flags & 1 else 0
            stroke = rgb_ if flags & 2 else 0
            f.write(f"  {{{first}u, {count}u, 0x{fill:06x}u, 0x{stroke:06x}u, {width:.2f}f, {flags}}},\n")
        f.write("};\n\n}  // namespace lunar24::host::art\n")
    print(f"{OUT}: {len(shapes)} shapes, {len(points)} points")


if __name__ == "__main__":
    main()
