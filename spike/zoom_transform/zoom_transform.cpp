// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// spike/zoom_transform/zoom_transform.cpp
//
// Lunar24 P1 slice ④ — zoom + fit-to-window + retina on the Design Coordinate
// Space, self-verifying. The master plan (design/06-master-plan.md) calls for a
// UNIFIED, INVERTIBLE transform on the 2400×1551 Design Coordinate Space that
// geometry, hit-testing, touch plate and patch/cable ALL share, exercised across
// 50/67/75/100/125/150/200% zoom and fit-to-window:
//
//   > 以 2400×1551 Design Coordinate Space 统一变换，验证 50/67/75/100/125/150/200%
//    与 fit-to-window。  (06-master-plan.md:77)
//   > 缩放不能改变逻辑坐标或状态。  (03-ui-framework.md:119)
//   > geometry、hit testing、touch plate、patch point/cable 共用一个可逆 transform。
//     (06-master-plan.md:117)
//
// This file VERIFIES the transform is correct and, crucially, that the 2× retina
// is applied EXPLICITLY (the failure mode flagged in ①b: a design point must
// invert through BOTH zoom and retina or hit-test/cable endpoints land off by
// half).
//
// The transform is derived verbatim from the pinned iPlug2 IGraphics source
// (spike/third_party/iPlug2/IGraphics/IGraphics.h and IGraphicsIOS.mm), the same
// framework ①b proved renders on this machine:
//   WindowWidth()          = mWidth * mDrawScale                     (IGraphics.h:1106)
//   GetBackingPixelScale() = GetScreenScale() * GetDrawScale()       (IGraphics.h:1803)
//   GetTotalScale()        = mDrawScale * mScreenScale               (IGraphics.h:1130)
//   GetScaleForScreen(w,h) = min(w/designW, h/designH)  (fit-to-window; IGraphicsIOS.mm:37)
// So:
//   design ->[drawScale (zoom/fit)]-> logical window ->[mScreenScale=2 (retina)]-> backing
//   backing = design * drawScale * mScreenScale
// The '2× retina explicit' rule: retina is a SEPARATE multiplier — it is never
// folded into drawScale, and never dropped from the forward or the inverse
// (hit-test) path.
//
// Disposable spike. Self-contained C++17. Never referenced by core/ or
// generated/. Verify rule (from @Claude ② review): every BAD control must ALARM,
// not just the GOOD silently pass.

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kDesignW = 2400.0;               // Design Coordinate Space width
constexpr double kDesignH = 1551.0;               // Design Coordinate Space height
constexpr double kAspect  = kDesignW / kDesignH;  // 1.547:1 (03-ui-framework.md:66)
constexpr double kRetina  = 2.0;                  // screenScale on this Mac (①b: 1200×664 = 2× 600×332)

// The 7 mandated zoom levels: 50/67/75/100/125/150/200 %.
constexpr double kZoomLevels[7] = {0.50, 0.67, 0.75, 1.00, 1.25, 1.50, 2.00};
constexpr double kZoomLo = 0.50, kZoomHi = 2.00;  // SetScaleConstraints(lo, hi)
constexpr int    kN = 7;

struct Pt { double x, y; };

// Forward: design -> backing. back = design * drawScale(z) * screenScale(s).
Pt toBacking(Pt d, double z, double s) { return {d.x * z * s, d.y * z * s}; }
// Inverse (hit-test): backing -> design. design = back / (z * s).
Pt toDesign(Pt b, double z, double s) { return {b.x / (z * s), b.y / (z * s)}; }

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---------------------------------------------------------------- verdicts
int g_failures = 0;
void verdict_pair(const char* name, long good, long bad) {
  bool pass = (good == 0) && (bad > 0);
  if (!pass) g_failures++;
  std::printf("  %-52s good=%-3ld bad=%-3ld  %s\n", name, good, bad, pass ? "PASS" : "FAIL");
}
void verdict_zero(const char* name, long actual) {
  bool ok = (actual == 0);
  if (!ok) g_failures++;
  std::printf("  %-52s actual(expect 0)=%-3ld  %s\n", name, actual, ok ? "PASS" : "FAIL");
}

// Count how many of the 7 zoom levels a predicate flags as BAD (true).
int flagged(const std::vector<bool>& f) { int n = 0; for (bool b : f) if (b) ++n; return n; }

// ---- Req A: forward applies retina 2× ----
// per-level ok of the CORRECT forward; a level is BAD if backing != design*z*2.
std::vector<bool> forward_2x_ok() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt b = toBacking({kDesignW, kDesignH}, z, kRetina);
    f.push_back(!(near(b.x, kDesignW * z * kRetina, 0.5) && near(b.y, kDesignH * z * kRetina, 0.5)));
  }
  return f;
}
// BAD-1: fold retina as 1× (screenScale dropped) -> backing is half of target,
// every level Z ALARMS (level flagged true if it now MISMATCHES the 2× target).
std::vector<bool> forward_no_retina_alarm() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt b = toBacking({kDesignW, kDesignH}, z, 1.0);   // bug: s=1
    Pt e = toBacking({kDesignW, kDesignH}, z, kRetina);
    f.push_back(!(near(b.x, e.x, 0.5) && near(b.y, e.y, 0.5)));  // true = caught
  }
  return f;
}

// ---- Req B: hit-test inverts through zoom AND retina (round-trip) ----
std::vector<bool> inverse_2x_ok() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt probe = {600, 300};
    Pt d = toDesign(toBacking(probe, z, kRetina), z, kRetina);
    f.push_back(!(near(d.x, probe.x, 0.5) && near(d.y, probe.y, 0.5)));
  }
  return f;
}
// BAD-2: inverse forgot retina -> design off by 2× -> every level alarms.
std::vector<bool> inverse_no_retina_alarm() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt probe = {600, 300};
    Pt b = toBacking(probe, z, kRetina);
    Pt d = toDesign(b, z, 1.0);                     // bug: inverse s=1
    f.push_back(!(near(d.x, probe.x, 0.5) && near(d.y, probe.y, 0.5)));
  }
  return f;
}

// ---- Req C: one unified transform => aspect invariant, no non-uniform stretch ----
std::vector<bool> aspect_ok() {
  std::vector<bool> f;
  const double dw = 200.0, dh = 129.0;
  for (double z : kZoomLevels) {
    Pt tl = toBacking({100, 100}, z, kRetina);
    Pt br = toBacking({100 + dw, 100 + dh}, z, kRetina);
    double asp = (br.x - tl.x) / (br.y - tl.y);
    f.push_back(!near(asp, dw / dh, 0.02));
  }
  return f;
}
// BAD-3: non-uniform fit (x scales 1.0×, y scales 1.5×) breaks the aspect.
std::vector<bool> aspect_nonuniform_alarm() {
  std::vector<bool> f;
  const double dw = 200.0, dh = 129.0;
  for (double z : kZoomLevels) {
    double w = dw * z * 1.0 * kRetina;   // bug: x uses 1.0×
    double h = dh * z * 1.5 * kRetina;   // bug: y uses 1.5×
    f.push_back(!near(w / h, dw / dh, 0.02));   // true = aspect broke = caught
  }
  return f;
}
// Linearity: a cable's mid-point maps to the midpoint of its endpoints' mappings
// (affine, no warp). Returns the number of levels that break it (0 expected).
int affine_midpoint_break() {
  int n = 0;
  Pt a = {100, 100}, c = {2300, 1451};
  for (double z : kZoomLevels) {
    Pt ma = toBacking({(a.x + c.x) / 2, (a.y + c.y) / 2}, z, kRetina);
    Pt b1 = toBacking(a, z, kRetina), b2 = toBacking(c, z, kRetina);
    if (!near(ma.x, (b1.x + b2.x) / 2, 0.5) || !near(ma.y, (b1.y + b2.y) / 2, 0.5)) ++n;
  }
  return n;
}

// ---- Req D: zoom is view-only; design state (logical coord) is invariant ----
std::vector<bool> state_invariant_ok() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt probe = {777, 555};
    Pt d = toDesign(toBacking(probe, z, kRetina), z, kRetina);
    f.push_back(!(near(d.x, probe.x, 0.5) && near(d.y, probe.y, 0.5)));
  }
  return f;
}
// BAD-4: a buggy path writes back state *= zoom (zoom leaks into the logical
// coordinate) -> state drifts with the level -> every level alarms.
std::vector<bool> state_mutated_alarm() {
  std::vector<bool> f;
  for (double z : kZoomLevels) {
    Pt probe = {777, 555};
    Pt d = toDesign(toBacking(probe, z, kRetina), z, kRetina);
    Pt bug = {d.x * z, d.y * z};        // bug: state scaled by zoom
    f.push_back(!(near(bug.x, probe.x, 0.5) && near(bug.y, probe.y, 0.5)));
  }
  return f;
}

// ---- Req E: fit-to-window, retina explicit in the fit ----
struct Fit { double z; bool overflow; };
Fit compute_fit(double backingW, double backingH, double s) {
  // IGraphics GetScaleForScreen(w,h)=min(w/designW,h/designH) in LOGICAL px. A
  // backing content area already includes retina, so logical = backing / s.
  double lw = backingW / s, lh = backingH / s;
  double z = std::min(lw / kDesignW, lh / kDesignH);
  if (z < kZoomLo) z = kZoomLo;
  if (z > kZoomHi) z = kZoomHi;
  bool overflow = (kDesignW * z * s > backingW) || (kDesignH * z * s > backingH);
  return {z, overflow};
}
int fit_bad_count(double backingW, double backingH) {
  int n = 0;
  Fit f = compute_fit(backingW, backingH, kRetina);
  if (f.overflow) ++n;
  if (!near(kDesignW / kDesignH, kAspect, 1e-9)) ++n;   // (aspect is a constant; sanity)
  return n;
}
// BAD-5: fit dropped the retina (took backing as if logical) -> drawScale 2× too
// big -> content overflows by ~2× -> must alarm (returns 1).
int fit_no_retina_alarm(double backingW, double backingH) {
  double lw = backingW, lh = backingH;               // bug: did NOT /retina
  double z = std::min(lw / kDesignW, lh / kDesignH);
  bool overflow = (kDesignW * z * kRetina > backingW) || (kDesignH * z * kRetina > backingH);
  return overflow ? 1 : 0;
}

} // namespace

int main() {
  std::printf("== P1 slice ④, zoom + fit-to-window + retina (Design Coordinate Space) ==\n");
  std::printf("Design Coordinate Space = %.0f x %.0f (aspect %.4f:1); retina = %.0fx (this Mac).\n",
              kDesignW, kDesignH, kAspect, kRetina);
  std::printf("Transform (from pinned IGraphics): design ->[drawScale]-> logical ->[screenScale]-> backing;\n");
  std::printf("  backing = design * drawScale * screenScale; fit = min(logicalW/designW, logicalH/designH).\n");
  std::printf("  zoom = {50,67,75,100,125,150,200}%%; SetScaleConstraints(%.2f, %.2f).\n\n", kZoomLo, kZoomHi);

  // Req A
  {
    long good = flagged(forward_2x_ok());
    long bad  = flagged(forward_no_retina_alarm());
    std::printf("Req A — '2x retina explicit' in the FORWARD transform (design -> backing)\n");
    for (int i = 0; i < kN; ++i) {
      double z = kZoomLevels[i];
      Pt b = toBacking({kDesignW, kDesignH}, z, kRetina);
      std::printf("   z=%.2f: corner backing = (%.0f, %.0f)  %s\n", z, b.x, b.y,
                  forward_2x_ok()[i] ? "BAD(off 2x)" : "ok");
    }
    std::printf("  -> folding retina as 1x would make every target HALF the correct backing.\n");
    verdict_pair("forward applies retina 2x (drop-2x -> RED)", good, bad);
  }
  std::printf("\n");

  // Req B
  {
    long good = flagged(inverse_2x_ok());
    long bad  = flagged(inverse_no_retina_alarm());
    std::printf("Req B — hit-test inverts through BOTH zoom and retina (round-trip)\n");
    Pt probe = {600, 300};
    for (int i = 0; i < kN; ++i) {
      double z = kZoomLevels[i];
      Pt d = toDesign(toBacking(probe, z, kRetina), z, kRetina);
      std::printf("   z=%.2f: probe(600,300) -> design (%.3f, %.3f)  %s\n", z, d.x, d.y,
                  inverse_2x_ok()[i] ? "BAD(drift)" : "ok");
    }
    std::printf("  -> a hit-test that forgets retina lands HALF a design space away; still caught.\n");
    verdict_pair("hit-test inverts through zoom AND retina (off-by-half -> RED)", good, bad);
  }
  std::printf("\n");

  // Req C
  {
    long good = flagged(aspect_ok());
    long bad  = flagged(aspect_nonuniform_alarm());
    std::printf("Req C — one unified transform: panel aspect invariant (no non-uniform stretch)\n");
    verdict_pair("unified transform preserves panel aspect (stretch -> RED)", good, bad);
    verdict_zero("cable midpoint maps affinely (geometry == hit-test)", affine_midpoint_break());
  }
  std::printf("\n");

  // Req D
  {
    long good = flagged(state_invariant_ok());
    long bad  = flagged(state_mutated_alarm());
    std::printf("Req D — zoom is VIEW-ONLY: design state (logical coord) invariant across levels\n");
    verdict_pair("zoom does not mutate design state (state*zoom -> RED)", good, bad);
  }
  std::printf("\n");

  // Req E
  {
    const double Wb = 3840, Hb = 2160;   // a 4K-ish backing content area, retina 2x
    long good = fit_bad_count(Wb, Hb);
    long bad  = fit_no_retina_alarm(Wb, Hb);
    std::printf("Req E — fit-to-window (host backing %.0f x %.0f, retina %.0fx)\n", Wb, Hb, kRetina);
    Fit f = compute_fit(Wb, Hb, kRetina);
    std::printf("   fit drawScale = %.4f -> content %.0f x %.0f backing, overflow=%d\n",
                f.z, kDesignW * f.z * kRetina, kDesignH * f.z * kRetina, (int)f.overflow);
    verdict_zero("fit content fits without overflow (retina explicit)", good);
    std::printf("  %-52s actual(expect >0)=%-3ld  %s\n", "fit dropping retina overflows (2x too big) -> RED",
                bad, bad > 0 ? "PASS" : "FAIL");
    if (!(bad > 0)) g_failures++;
  }
  std::printf("\n");

  std::printf("== slice ④ verdict: %s ==\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
