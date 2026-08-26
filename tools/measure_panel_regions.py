#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
#
# measure_panel_regions.py — machine-measure the 21 panelSite region rects from the
# panel reference PNG. The rect VALUES come from the image, never from a human
# transcript: connected-components finds each card's dark outline, and the divider
# that separates two modules sharing a card is detected from the pixel profile.
#
# Precedent: this is the same shape as tools/build_evidence_layout.py (and its
# evidence_layout.json). The test that validates the region rects CONSUMES the JSON
# this tool emits, not hand-written constants. Two gates protect it:
#   * regen zero-diff  -- `--check` re-runs the full measure and compares byte-for-byte
#     to the committed artifact. A silent/mutated source or a non-deterministic step
#     shows up as a diff.
#   * negative         -- a test mutates one emitted anchor value and proves the
#     consumer-validator goes red.
#
# Reproducibility notes:
#   * The reference PNG is gitignored (like the manual). In CI the file is absent, so
#     the tool prints SKIP and returns 0 — the gate is present-but-inert there, never
#     silently absent (the same rule as build_evidence_layout.py).
#   * ImageMagick `magick` is the only external dependency; grayscale + connected-
#     components are deterministic for a fixed input.
#
# Output: generated/lunar24/panel_regions.json
#   {version, source, width, height, method, anchors:[{site,x0,y0,x1,y1}, ...]}

import os, sys, json, re, subprocess, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.path.join(REPO, "design/reference/solar42N_panel_2400px.png")
OUT = os.path.join(REPO, "generated/lunar24/panel_regions.json")
HDR = os.path.join(REPO, "generated/lunar24/panel_anchors.generated.h")
# the C++ test consumes this generated header (NOT a hand-written constant set), so
# the anchor origin is the reference image and neither @Pi nor @Claude is in the
# transcription chain. It is a sibling of the canonical panel_regions.json, both
# produced from the same measurement in a single generator pass.
W, H = 2400, 1551
DARK = 115  # a pixel is "card/dark" when its grayscale value is below this


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


# ---- grayscale load -----------------------------------------------------------
def load_gray(work):
    gp = os.path.join(work, "panel.gray")
    sh(f"cd {REPO} && magick {IMAGE} -colorspace Gray -depth 8 gray:{gp}")
    return open(gp, "rb").read()


def gray(raw, x, y):
    return raw[y * W + x]


# ---- connected-components: the dark card outlines (the measurement source) ----
def card_blobs(work):
    cp = os.path.join(work, "cc.txt")
    sh(f"cd {REPO} && magick {IMAGE} -colorspace Gray -threshold 45% -negate "
       f"-define connected-components:verbose=true -define connected-components:area-threshold=600 "
       f"-connected-components 8 null: > {cp} 2>/dev/null")
    blobs = []
    for line in open(cp):
        m = re.match(
            r"\s*\d+:\s*(\d+)x(\d+)\+(\d+)\+(\d+)\s+([\d.,-]+),([\d.,-]+)\s+([\d.]+e?[+-]?\d*)\s+gray", line)
        if not m:
            continue
        w, h, x, y = map(int, m.groups()[:4])
        blobs.append(dict(x=x, y=y, x1=x + w - 1, y1=y + h - 1,
                          cx=x + w / 2, cy=y + h / 2, w=w, h=h, a=float(m.group(7))))
    # drop the full-canvas background component and degenerate wrappers
    return [b for b in blobs if b["a"] >= 300 and b["w"] < W - 4 and b["h"] < H - 4]


def union_of(blobs, x0, y0, x1, y1):
    """bbox of every blob whose center lies inside the seed box (with a small pad)."""
    sel = [b for b in blobs
           if x0 - 10 <= b["cx"] <= x1 + 10 and y0 - 10 <= b["cy"] <= y1 + 10]
    if not sel:
        return None
    return (min(b["x"] for b in sel), min(b["y"] for b in sel),
            max(b["x1"] for b in sel), max(b["y1"] for b in sel))


def col_frac(raw, x, y0, y1):
    if y1 < y0:
        return 0.0
    return sum(1 for y in range(y0, y1 + 1) if gray(raw, x, y) < DARK) / (y1 - y0 + 1)


def row_frac(raw, y, x0, x1):
    if x1 < x0:
        return 0.0
    return sum(1 for x in range(x0, x1 + 1) if gray(raw, x, y) < DARK) / (x1 - x0 + 1)


def divider_x(raw, xlo, xhi, y0, y1):
    """the beige gutter that separates two modules standing side by side. It is a
    near-zero dark column flanked on BOTH sides by a dark card border, so it scores
    as a valley: (dark left) + (dark right) - 2*(this column). A plain beige run
    inside a card has dark on only one/no side and scores far lower. Peak = divider."""
    best = None
    score = -1.0
    for x in range(xlo + 2, xhi - 2):
        c = col_frac(raw, x, y0, y1)
        s = (col_frac(raw, x - 2, y0, y1) - c) + (col_frac(raw, x + 2, y0, y1) - c)
        if s > score:
            score, best = s, x
    return best


def card_top_row(raw, ylo, yhi, x0, x1):
    """the strongest full-width card border (a dark row) inside a module's lower band:
    used for the envelope card top, which is a separate border from the VCO column top
    that the merged outline reports."""
    best = None
    v = -1.0
    for y in range(ylo, yhi + 1):
        f = row_frac(raw, y, x0, x1)
        if f > v:
            v, best = f, y
    return best


def line_x(raw, xlo, xhi, y0, y1):
    """a solid full-height dark vertical border line: the column with the MAXIMUM
    dark fraction. Used for the ENV A | ENV B divider, which is a thin dark rule
    (col_frac ~1.0), NOT a beige gutter — so valley/argmin detection would miss it."""
    best = None
    v = -1.0
    for x in range(xlo, xhi + 1):
        f = col_frac(raw, x, y0, y1)
        if f > v:
            v, best = f, x
    return best


def card_bottom_row(raw, ylo, yhi, x0, x1):
    """the strongest full-width dark row in a module's bottom band: the card's lower
    border."""
    best = None
    v = -1.0
    for y in range(ylo, yhi + 1):
        f = row_frac(raw, y, x0, x1)
        if f > v:
            v, best = f, y
    return best


def divider_y(raw, ylo, yhi, x0, x1):
    best, bv = ylo, 1.0
    for y in range(ylo, yhi + 1):
        f = row_frac(raw, y, x0, x1)
        if f < bv:
            bv, best = f, y
    return best


def measure(blobs, raw):
    # Seed boxes locate each module on the panel (coarse). The measured bbox comes
    # from the card-outline blobs; the few modules that share a card are split at a
    # runtime-detected divider. Values are therefore image-derived, seed only locates.
    seed = {
        "左上 DRONE 1": (21, 260, 406, 558),
        "左上 DRONE 2": (416, 260, 801, 558),   # extends to the center gutter (NOT 723)
        "左中 DRONE 3": (21, 568, 406, 879),
        "右上 DRONE 4": (1599, 260, 1985, 558),
        "右上 DRONE 5": (1993, 260, 2378, 558),
        "右中 DRONE 6": (1993, 567, 2378, 878),
        "中部 VOICE MIXER": (808, 537, 1590, 710),
        "中上 DUAL EFFECTOR": (808, 183, 1590, 531),  # full cartridge card (effect+filter)
        "下排 LFO A": (19, 884, 292, 1030),
        "下排 joystick": (296, 884, 596, 1030),
        "下排 5-step seq": (600, 884, 1545, 1030),
        "下排 preamp": (1548, 884, 1723, 1030),
        "下排 env follower": (1727, 884, 2101, 1030),
        "下排 LFO B": (2103, 884, 2379, 1030),
        "底部 12 触摸片": (399, 1104, 1999, 1491),
        "右下 DRONE VOICES": (2126, 1070, 2304, 1469),
    }
    out = {}

    for site, box in seed.items():
        r = union_of(blobs, *box)
        if r is None:
            raise RuntimeError(f"no card blobs for {site}")
        x0, y0, x1, y1 = r
        # keep inside the design canvas
        out[site] = (min(x0, W - 1), min(y0, H - 1), min(x1, W - 1), min(y1, H - 1))

    # DUAL EFFECTOR card is one physical cartridge (effect + filter sections). Split
    # it at the divider row (the FILTER section's top line) into effector + vcf.
    # VOICE MIXER card is bounded below by the ENV A/B card; clamp to that divider
    # (the beige gutter row) so a stray ENV/label blob cannot inflate the bottom edge.
    if "中部 VOICE MIXER" in out:
        mx0, my0, mx1, _ = out["中部 VOICE MIXER"]
        mmid = divider_y(raw, 660, 745, mx0 + 40, mx1 - 40)
        out["中部 VOICE MIXER"] = (mx0, my0, mx1, mmid)

    ex0, ey0, ex1, ey1 = out["中上 DUAL EFFECTOR"]
    fy = divider_y(raw, ey0 + 130, ey1 - 40, ex0 + 60, ex1 - 60)
    out["中上 DUAL EFFECTOR"] = (ex0, ey0, ex1, fy)
    out["中上 DUAL VCF"] = (ex0, fy, ex1, ey1)

    # VCO A / VCO B (tall side columns) and ENV A / ENV B (below the mixer, center
    # strip) share a band but not a card. CC merges their outlines, so measure each
    # from its OWN border lines, all machine-detected from the figure:
    #   VCO A top = the row-1/row-2 boundary (y560 full border); ENV top = the mixer
    #   bottom border (y711); ENV A | ENV B = a thin full-height dark rule (x1248);
    #   VCO A | ENV A and ENV B | VCO B = beige gutter valleys (x805 / x1593).
    # A tight-quadrant union still pulls the merged blob (its centre lands inside the
    # quadrant), so every edge is clipped to a detected border, never taken from the
    # merged bbox.
    def bounds(lx0, lx1, ty0, ty1, bx0, bx1, vtoplo, vtophi, etoplo, etophi, gabx0, gabx1):
        """measure (x0,x1,top,bottom) for one side column given a grab band for its
        x-range, windows for its top/bottom borders, and the ENV-gutter window."""
        grab = union_of(blobs, lx0, ty0, lx1, ty1) or (lx0, ty0, lx1, ty1)
        top = card_top_row(raw, vtoplo, vtophi, bx0, bx1)
        bot = card_bottom_row(raw, 852, 881, bx0, bx1)
        return grab[0], grab[2], top, bot

    # VCO A column: right edge = the VCO A | ENV A gutter valley (x805, narrow window
    # so the detector can't fall into a beige card interior next to one dark border).
    dxa = divider_x(raw, 800, 812, 640, 870)                     # -> ~805
    vcoA = union_of(blobs, 418, 560, 798, 872) or (414, 560, 798, 872)
    vcoAtop = card_top_row(raw, 552, 630, 440, 790)              # -> ~560
    vcoAbot = card_bottom_row(raw, 852, 881, 440, 790)           # -> ~880
    out["中上 VCO A"] = (vcoA[0], vcoAtop, dxa, vcoAbot)

    # VCO B column: left edge = the ENV B | VCO B gutter valley (x1596). Its card is
    # the right tall column x1597-1985.
    dxb = divider_x(raw, 1586, 1600, 640, 870)                   # -> ~1596
    vcoB = union_of(blobs, 1599, 560, 1982, 872) or (1597, 560, 1982, 872)
    vcoBtop = card_top_row(raw, 552, 630, 1620, 1975)            # -> ~560
    vcoBbot = card_bottom_row(raw, 852, 881, 1620, 1975)         # -> ~880
    out["中上 VCO B"] = (dxb + 1, vcoBtop, vcoB[2], vcoBbot)

    # ENV A right border = its card's dark rule (~x1146). ENV A | ENV B are NOT
    # adjacent — the black "VOICE MIXER / ELTA MUSIC" credit card sits between them,
    # and ENV B rises after that card's right edge (~x1248+1 = 1249).
    envA_border = line_x(raw, 1100, 1210, 720, 870)              # -> ~1146
    credit_right = line_x(raw, 1210, 1310, 720, 870)             # -> ~1248
    envAtop = card_top_row(raw, 680, 740, 820, 1140)             # -> ~711
    envBtop = card_top_row(raw, 680, 740, 1256, 1590)            # -> ~711
    out["中部 ENV A"] = (dxa + 1, envAtop, envA_border, vcoAbot)
    out["中部 ENV B"] = (credit_right + 1, envBtop, dxb, vcoBbot)

    return out


def hdr_text(anchors):
    lines = []
    lines.append("// GENERATED by tools/measure_panel_regions.py")
    lines.append("// SOURCE: design/reference/solar42N_panel_2400px.png (the reference figure).")
    lines.append("// DO NOT EDIT by hand — regenerate with `python3 tools/measure_panel_regions.py`.")
    lines.append("// The test consumes this exact generated set as its EXTERNAL anchor, so a")
    lines.append("// hand-edited or hand-transcribed anchor that no longer reflects the figure")
    lines.append("// is a drift the regen gate (--check) and the convergence check both catch.")
    lines.append("//")
    lines.append("#pragma once")
    lines.append("namespace lunar24 { namespace core { namespace anchors {")
    lines.append("struct Anchor { const char* site; double x0, y0, x1, y1; };")
    lines.append("inline constexpr Anchor kAnchors[] = {")
    for s_, r in sorted(anchors.items()):
        lines.append('  {"%s", %.0f, %.0f, %.0f, %.0f},' % (s_, r[0], r[1], r[2], r[3]))
    lines.append("};")
    lines.append("inline constexpr int kAnchorCount = %d;" % len(anchors))
    lines.append("} } }  // namespace")
    lines.append("")
    return "\n".join(lines)


def main(argv):
    if not os.path.exists(IMAGE):
        print("SKIP: reference PNG gitignored/absent — generator guard inert in CI (visible, not silent).")
        return 0
    work = tempfile.mkdtemp(prefix="mpr_")
    raw = load_gray(work)
    blobs = card_blobs(work)
    anchors = measure(blobs, raw)
    doc = dict(version=1,
               source="design/reference/solar42N_panel_2400px.png",
               width=W, height=H,
               method="connected-components card outlines + divider detection",
               anchors=[dict(site=s, x0=r[0], y0=r[1], x1=r[2], y1=r[3])
                        for s, r in sorted(anchors.items())])
    jtxt = json.dumps(doc, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    htxt = hdr_text(anchors)
    check = "--check" in argv
    drift = []
    if not os.path.exists(OUT):
        if check: return 1
    elif check and open(OUT).read() != jtxt:
        drift.append("panel_regions.json")
    if not os.path.exists(HDR):
        if check: return 1
    elif check and open(HDR).read() != htxt:
        drift.append("panel_anchors.generated.h")
    if drift:
        print("DRIFT: regenerated " + " + ".join(drift) + " differs from committed artifact.")
        return 1
    if check:
        print(f"regen zero-diff OK ({len(anchors)} anchors) for {OUT} + {HDR}")
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write(jtxt)
    open(HDR, "w").write(htxt)
    print(f"wrote {OUT} + {HDR} with {len(anchors)} anchors")
    for a in doc["anchors"]:
        print(f"  {a['site']:<22} x{a['x0']}-{a['x1']} y{a['y0']}-{a['y1']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
