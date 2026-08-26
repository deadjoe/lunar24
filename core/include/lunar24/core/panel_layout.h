// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P5-② — measured panel REGION layout (task #33, criterion 5).
//
// design/06 §P5 mandate, last clause: coords come from the panel reference figure
// (design/03 §3: the reference PNG/PDF is measurement authority, never a background),
// and the measured coords must be consistent with the registry's panelSite region
// labels. This header is that dataset: one measured DesignRect per DISTINCT panelSite
// label, seen on the physical panel.
//
// GRANULARITY (say it plainly): panelSite is a REGION label ("中上 VCO A/B", "右下 DRONE
// VOICES"), not a per-control coordinate. So these rects are measured at REGION level —
// the dark module-card cluster each label names, read off the reference figure. They are
// the platform the panel_transform hit-tester maps design<->screen over. Pixel-exact
// per-control placement (every knob/jack) is the P5 full-panel layout slice's concern,
// deliberately OUT of scope here and recorded in FINDINGS.
//
// MEASUREMENT RECEIPTS (how the numbers are grounded, not guessed):
//   - Reference figure: design/reference/solar42N_panel_2400px.png, exactly 2400x1551
//     (1:1 with the design coordinate space; design/03 §3).
//   - Detected the dark module cards with ImageMagick `-connected-components` on the
//     thresholded figure; each cluster's bounding box below is that measured output
//     (e.g. top-left drones cluster x22..724 / top-right drones x1599..2301 / center
//     VCF band x808..1591 / keyboard band x399..2000 y1104..1492).
//   - Cross-checked the ambiguous middle/right zones against 300dpi panel crops
//     (pdftoppm render of the panel PDF) so the y-bands that the detector merges
//     (VCO A/B vs the envelope/mixer strip) are separated by the divider that is
//     visible there, not by a guess.
// Any rect x1/x2 or y1/y2 that would wander outside [0,2400]x[0,1551] is clamped to
// the design space — the panel is the full canvas, nothing sticks out.
//
// Header-only, constexpr, no heap/locks, realtime-safe. Pure geometry (DesignRect),
// no platform API, no iPlug2/IGraphics type.

#pragma once

#include <lunar24/core/host_window_fit.h>  // DesignRect, kDesignWidth/Height

namespace lunar24::core {

// A named panel region with its MEASURED design rect. `site` is the registry
// panelSite label this rect was measured for; the rect is the physical cluster on
// the panel that label names. DesignRect is the canonical 2400x1551 space.
struct PanelLayoutRegion {
  const char* site;
  DesignRect rect;
};

inline constexpr DesignRect kDesignArea{0, 0, kDesignWidth, kDesignHeight};

// One measured rect per DISTINCT panelSite label (22 — the registry keeps DRONE 1 and
// DRONE 2, DRONE 4 and DRONE 5, VCO A / VCO B and ENV A / ENV B as separate sites, so
// they are separate regions here too; and @Claude's 2026-08-26 ruling splits the filter
// section into FILTER L and FILTER R, two side-by-side knob groups, because FILTER L and
// FILTER R are two independent cards separated by the `link` knob). Ordered the way a
// human reads the panel: left column, center strip, right column, bottom row, bottom band.
//
// ⚠️ 2026-08-26 re-measurement (task #36): the previous rects were a large-scale
// mismatch with the figure, and their EXTERNAL anchor was a hand-written set copied
// from the same numbers — two same-source quantities validating each other, the exact
// disease @Claude flagged (00-status §2k). These rects are now pinned to a
// MACHINE-GENERATED anchor set: the panel reference figure is measured by
// tools/measure_panel_regions.py into generated/lunar24/panel_anchors.generated.h +
// panel_regions.json, and test_panel_layout consumes THAT set. The anchor origin is
// the reference image; neither @Pi nor @Claude hand-transcribes it. A kept-in-sync
// regen gate (--check) proves the committed artifact reproduces the image, and a
// hand-edited region here no longer converges — so a wrong value reds itself.
//
// @Claude's uniform-edge contract (msg dc7f808a): each generated anchor carries ONE rule
// label, chosen by the image, never by a human — "frame" binds a complete dark card
// outline on all four sides (each border dark-frac >= 0.90), "content" binds the content
// bounding box because at least one edge is a beige gutter / content division / open
// boundary. A single rect never mixes one-edge-frame with one-edge-content. The generator
// re-measures every "frame" anchor on the live figure and rejects a frame that is not
// actually framed (the frame self-validate gate).
//
// Corrections over the FIGURE TRUTH (material to the region semantics, kept inline and
// reported to @Claude):
//   * The center-top band is ONE physical cartridge (x@~807-1592, y@~181-532) holding
//     BOTH the "DUAL EFFECTOR" section (top, y181-410) and the "FILTER L/R" section
//     (bottom, y410-511), split at the figure's content divider row (y410 — a content
//     boundary, not a frame). @Claude's §2k "DUAL VCF {808,537,1591,710}" is not the
//     filter — that band is the 10ch VOICE MIXER; the real filter section is y410-511
//     inside the effector card. FILTER L and FILTER R are the two halves of that section
//     separated by the `link` knob; the figure has NO card frame between them, so each
//     binds its own knob-group content bbox and is labelled "content".
//   * The 10ch VOICE MIXER is its own framed card BELOW the cartridge (a ~2-3px gutter
//     separates it): TOP y535 and BOTTOM y711 are its card frame, labelled "frame".
//   * Below the mixer card (y535-711), ENV A and ENV B flank the black
//     "VOICE MIXER / ELTA MUSIC" credit card: ENV A y~711-880, ENV B y~715-880.
inline constexpr PanelLayoutRegion kPanelRegions[] = {
    // left column
    {"左上 DRONE 1", DesignRect{20, 258, 408, 560}},       // DRONE 1 card
    {"左上 DRONE 2", DesignRect{414, 258, 803, 560}},      // DRONE 2 card
    {"左中 DRONE 3", DesignRect{20, 566, 408, 881}},       // DRONE 3 card
    // center strip — VCO A | VOICE MIXER | VCO B, with the DUAL EFFECTOR cartridge
    // (effect on top + FILTER L/R below) above the mixer, then ENV A | ENV B below.
    {"中上 VCO A", DesignRect{414, 559, 805, 880}},        // tall left column of the mid band
    {"中部 VOICE MIXER", DesignRect{807, 535, 1592, 711}},  // 10ch PAN/VOL strip (narrow, TOP y535 + BOTTOM y711 are its card frame)
    {"中上 VCO B", DesignRect{1597, 560, 1985, 880}},      // tall right column of the mid band
    {"中上 DUAL EFFECTOR", DesignRect{807, 181, 1592, 410}},  // cartridge/X/Y/Z/BLEND/MASTER (top of the one cartridge)
    {"中上 FILTER L", DesignRect{854, 438, 1149, 511}},    // FREQ/RES/CV L/DIST knob group (left half of the filter section)
    {"中上 FILTER R", DesignRect{1246, 438, 1543, 511}},   // GAIN/CV R/FREQ/RES knob group (right half of the filter section)
    {"中部 ENV A", DesignRect{806, 711, 1146, 880}},        // envelope A (hold/A-D-S-R/gate)
    {"中部 ENV B", DesignRect{1249, 715, 1596, 880}},       // envelope B
    // right column
    {"右上 DRONE 4", DesignRect{1597, 258, 1985, 561}},    // DRONE 4 card
    {"右上 DRONE 5", DesignRect{1991, 258, 2380, 561}},    // DRONE 5 card
    {"右中 DRONE 6", DesignRect{1991, 566, 2380, 880}},    // DRONE 6 card
    // bottom row, true physical order LFO A → JOYSTICK → 5-STEP → PREAMP → ENV-FOLL → LFO B
    {"下排 LFO A", DesignRect{19, 884, 292, 1030}},
    {"下排 joystick", DesignRect{296, 884, 596, 1030}},
    {"下排 5-step seq", DesignRect{600, 884, 1545, 1030}},
    {"下排 preamp", DesignRect{1548, 884, 1723, 1030}},
    {"下排 env follower", DesignRect{1727, 884, 2101, 1030}},
    {"下排 LFO B", DesignRect{2103, 884, 2379, 1030}},
    // bottom band (touch plates) + bottom-right (drone voices)
    {"底部 12 触摸片", DesignRect{399, 1104, 1999, 1491}},    // sensor keyboard / touch plates
    {"右下 DRONE VOICES", DesignRect{2126, 1126, 2304, 1469}},  // drone voices 1-6 grid
};
inline constexpr int kPanelRegionCount = 22;

// Locate a measured region by its registry panelSite label. Returns the index into
// kPanelRegions, or -1 if the label is not in the measured set (a mismatch worth
// escalating — the registry names a region we have not measured). Linear scan; the
// count is tiny and this is not on an audio path.
inline int panel_region_by_site(const char* site) {
  for (int i = 0; i < kPanelRegionCount; ++i) {
    // Both are author-owned constant labels; a non-equal string is a data mismatch.
    bool same = true;
    const char* a = kPanelRegions[i].site;
    const char* b = site;
    while (*a && *b) {
      if (*a != *b) { same = false; break; }
      ++a; ++b;
    }
    if (same && *a == *b) return i;
  }
  return -1;
}

}  // namespace lunar24::core
