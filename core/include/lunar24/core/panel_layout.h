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

// Measured region rects, one per distinct panelSite label. Ordered to match the
// "ring" a human reads the panel in (left column top->bottom, then center, then right).
// Measured region rects, one per DISTINCT panelSite label (19 — the registry keeps
// VCO A / VCO B and ENV A / ENV B as separate sites, so they are separate regions
// here too). Ordered the way a human reads the panel: left column, center strip,
// right column, bottom row, bottom band.
//
// ⚠️ 2026-08-26 re-measurement (task #36, 裁决 0843a838): the previous rects for the
// mid strip + bottom row were a large-scale mismatch with the figure. Corrected below
// with @Claude's separated-column detection (design/00-status.md §2k): the 10ch VOICE
// MIXER is the narrow center strip y560-700 (NOT the bottom row), ENV A/B sit y700-880
// as two separate sites, VCO A / VCO B are the tall side columns y560-880, and the
// bottom-row six cards are y880-1030 (y-bottom was wrongly 1080) in the true physical
// order LFO A→JOYSTICK→5-STEP→PREAMP→ENV-FOLL→LFO B. The two center-top bands were
// also top/bottom-swapped vs the figure title bar ("DUAL EFFECTOR" is on top, "FILTER
// L/R" below), so their labels are reassigned here.
inline constexpr PanelLayoutRegion kPanelRegions[] = {
    // left column
    {"左上 DRONE 1/2", DesignRect{22, 180, 724, 700}},    // DRONE 1 + DRONE 2 (two cards + gate/hold)
    {"左中 DRONE 3", DesignRect{22, 560, 407, 880}},      // DRONE 3 card
    // center strip — VCO A | VOICE MIXER | VCO B across y560-880, flanked by the
    // DUAL EFFECTOR (top) / DUAL VCF (below) cartridges, then ENV A | ENV B.
    {"中上 VCO A", DesignRect{415, 560, 805, 880}},        // tall left column of the mid band
    {"中部 VOICE MIXER", DesignRect{807, 560, 1592, 700}},  // 10ch PAN/VOL strip (narrow, top of mid band)
    {"中上 VCO B", DesignRect{1598, 560, 1985, 880}},      // tall right column of the mid band
    {"中上 DUAL EFFECTOR", DesignRect{808, 183, 1591, 537}},  // cartridge/X/Y/Z/MOD (top center-top band)
    {"中上 DUAL VCF", DesignRect{808, 537, 1591, 710}},    // FILTER L/R, FREQ/RES (below the effector)
    {"中部 ENV A", DesignRect{807, 700, 1146, 880}},        // envelope A (hold/A-D-S-R/gate)
    {"中部 ENV B", DesignRect{1248, 700, 1591, 880}},       // envelope B
    // right column
    {"右上 DRONE 4/5", DesignRect{1599, 180, 2301, 561}},  // DRONE 4 + DRONE 5
    {"右中 DRONE 6", DesignRect{1993, 561, 2379, 879}},    // DRONE 6 card
    // bottom row, true physical order LFO A → JOYSTICK → 5-STEP → PREAMP → ENV-FOLL → LFO B
    {"下排 LFO A", DesignRect{19, 880, 292, 1030}},
    {"下排 joystick", DesignRect{296, 880, 596, 1030}},
    {"下排 5-step seq", DesignRect{600, 880, 1546, 1030}},
    {"下排 preamp", DesignRect{1549, 880, 1723, 1030}},
    {"下排 env follower", DesignRect{1727, 880, 2103, 1030}},
    {"下排 LFO B", DesignRect{2104, 880, 2377, 1030}},
    // bottom band (touch plates) + bottom-right (drone voices)
    {"底部 12 触摸片", DesignRect{399, 1104, 2000, 1492}},    // sensor keyboard / touch plates
    {"右下 DRONE VOICES", DesignRect{1700, 1104, 2400, 1492}},  // drone voices 1-6 grid
};
inline constexpr int kPanelRegionCount = 19;

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
