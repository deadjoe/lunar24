// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// vco_wave_map.h — Raft task #116 (GH #19 S0) EXPERIMENTAL software candidate for the
// single-knob continuous waveform mapping. NOT PRODUCTION: nothing in the product calls
// this by default (see Vco::setWaveMap; the default mode is kLegacy, bit-identical).
//
// ============================ WHY THIS SHAPE ================================
// Owner fact (@bearbone, Raft msg b4ffcd85, 2026-09-13): on the Elta Solar 42 / 42N the
// MORPHING WAVEFORM knob of each main V/Oct VCO turns CONTINUOUSLY and changes the
// waveform gradually. The manual (design/reference/solar42N_instruct_08_04_v15.pdf p.9)
// names six waveforms in total: four traditional shapes plus two variable morphing ones
// (saw -> inverted saw, sine -> triangle). The manual does NOT say how a waveform is
// selected. @Codex msg d2b3f787 fixes the direction: ONE knob, reuse the EXISTING per-side
// `morph` parameter, no new wave selector, no second morph control quantity.
//
// ONE coordinate, so the six named features must lie on ONE continuous sweep. This header
// expresses that sweep as a partition of unity over shape nodes:
//
//      norm:  0         c1            c2            c3         1
//      node:  saw --- invSaw ------ sine ------- triangle -- pulse
//      stretch: \__S0__/ \___S1____/ \___S2____/ \___S3___/
//      name:    morph-1   (none)      morph-2     (none)
//
//   * S0 (saw -> invSaw)   = the manual's first NAMED morphing waveform.
//   * S2 (sine -> triangle) = the manual's second NAMED morphing waveform.
//   * S1 (invSaw -> sine) and S3 (triangle -> pulse) are UNNAMED software connectors: they
//     exist only because one continuous sweep has to get from one named feature to the next.
//     They are reported, never presented as hardware structure.
//   * The six manual names are therefore 4 NODES (saw, sine, triangle, pulse) + 2 STRETCHES
//     (S0, S2) = 6. They are NOT six fixed anchors: "6 waveforms in total" counts named
//     features on the sweep, not detents and not equal intervals.
//
// WHY FIVE NODES AND NOT SIX -- the accounting, because it is not obvious and was queried:
// the manual's two morphs each contribute a FAR ENDPOINT, and that is where the extra node
// comes from.  "saw to inverted saw": saw is already one of the four traditional shapes,
// invSaw is NEW -> +1 node.  "sine to triangle": BOTH ends are already traditional shapes
// -> +0 nodes.  So 4 + 1 = 5.  invSaw is the only node with no separate glyph on the panel,
// because it is reached inside a named morph stretch rather than at a labelled position.
//
// EXACTLY the two existing morph implementations are reused for the two named stretches, at
// morph = the local coordinate u (see stretchSample below): S0 is Vco::waveformSampleAt's
// kMorphSawInvSaw closed form, S2 is its kMorphSineTriangle closed form. No new waveform math.
//
// ============================ WHAT IS PROVISIONAL ===========================
// SOFTWARE PROVISIONAL (no hardware claim, must be ruled on by the supervisor):
//   (P1) the node ORDER [saw, invSaw, sine, triangle, pulse]. Only the two NAMED stretches'
//        ENDPOINT PAIRS are manual-fixed ("saw to inverted saw", "sine to triangle"); the
//        manual does NOT fix which end sits at the lower norm, i.e. it does not state the
//        knob's rotation direction, so even S0's internal direction is a software choice.
//        THE PANEL IS NOT EVIDENCE FOR THIS ORDER -- it is evidence AGAINST it. The #115
//        glyph readings (report §"layer C", on the same annotated image) identify the
//        leftmost glyph (about -75 deg) as SINE and the fourth (about +16 deg) as
//        SQUARE/PULSE, both with HIGH confidence, and the two right-hand glyphs (about +48
//        and +75 deg) as MORPH symbols. That assignment contradicts this order, which puts
//        saw at -75 deg and triangle at +48 deg. The panel's six glyphs therefore do NOT
//        support any boundary derivation, and none is claimed (see P2).
//   (P2) the boundary VECTORS. Two complete candidates are provided, and NEITHER has panel
//        provenance:
//          kRingEqual  -- five nodes at k/4; a pure software equal division. Equal division
//                         is explicitly allowed as a provisional candidate (@Codex ad68abb6);
//                         what is forbidden is presenting equal division AS hardware
//                         structure.
//          kRingSpaced -- a NON-UNIFORM spacing specimen, retained ONLY so the supervisor can
//                         compare "norm 0.5 IS a node" against "norm 0.5 falls inside a
//                         stretch". Its values are an arithmetic rescaling of the six
//                         measured glyph ANGLES and are labelled `kRingSpaced`, not
//                         "panel-derived", because that derivation is WITHDRAWN: the glyph
//                         identification it needs contradicts the #115 readings, and the
//                         rescaling has no measured basis either (the knob's mechanical
//                         travel was never measured). That is the whole claim -- the evidence
//                         shows THIS DERIVATION HAS NO BASIS. It does NOT show that the
//                         panel is incompatible with any monotone mapping: whether a glyph
//                         marks a stretch's interior, a whole morph type, or one
//                         representative waveform is itself unknown, so glyph positions alone
//                         cannot exclude anything.
//   (P3) the BLAMP scaling: the existing triangle slope correction is scaled by the triangle's
//        weight in the mix (wave_map::triangleWeight). First-order, provisional, and only
//        exact at a pure-triangle node.
//   (P4) the pulse node reads `duty_` (= the `pw` panel parameter), so the sweep makes the
//        pulse's duty audible. APPROVED AS IN SCOPE by @Codex (Raft task #116, 2026-09-13):
//        this is the normal consumption of an EXISTING parameter and needs no separate owner
//        ruling. It is NOT a reclassification of vco_a_pwm/vco_b_pwm: both stay
//        transfer_unavailable, both live lanes stay fail-closed, and the registry, the state
//        dispositions and the state bytes are untouched.
//
// NOT A PRODUCTION ENTRY POINT. The switch below is reachable only from tests
// (SynthRuntime::setVcoWaveMap); it is not a ParameterId, is not in the registry, and is not
// persisted, so nothing in a SAVE can select a candidate. The production slice that adopts one
// of these must REMOVE this test-injection step and give the mapping a real, persisted,
// ruled-on entry point.
//
// ============================ KNOWN, REPORTED, NOT HIDDEN ====================
// S0's two endpoints are antipodal (invSaw(p) = -saw(p)), so at the exact centre of S0 the
// convex blend is identically 0.0 for EVERY phase: the sweep passes through silence there.
// This is measured and reported (probe matrix `silence`), and it is deliberately NOT
// compensated by any hidden normalisation or gain curve. The same is true of the existing
// kMorphSawInvSaw today, whose comment already records "passes through 0 at mid".
//
// The blend is a partition of unity of piecewise-linear weights: the value is continuous in
// `norm` at every boundary (left limit = right limit = the node shape) but its derivative is
// not, so the sweep corners at the nodes. Continuity is the requirement; C1 is not claimed.
//
// The mixed output is a convex combination of a band-limited triangle and NAIVE saw / sine /
// pulse / invSaw. It is NOT band-limited and is never called band-limited. Only the pure
// triangle component is corrected, and only in proportion to its weight.

#ifndef LUNAR24_CORE_VCO_WAVE_MAP_H
#define LUNAR24_CORE_VCO_WAVE_MAP_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lunar24::core::wave_map {

// ---------------------------------------------------------------- the ring ----
// Node order (P1, software provisional). Directions inside S0 and S2 are manual-fixed.
enum class Node : int {
  kSaw = 0,     // 2p - 1
  kInvSaw = 1,  // -(2p - 1)
  kSine = 2,    // sin(2*pi*p)
  kTriangle = 3,// 4*|p - 0.5| - 1
  kPulse = 4,   // (p < duty) ? +1 : -1
  kCount = 5
};

inline constexpr int kNodeCount = 5;
inline constexpr int kStretchCount = kNodeCount - 1;  // 4 boundaries-1 stretches
inline constexpr int kBoundaryCount = kNodeCount;     // c0 .. c4, inclusive ends

// A boundary vector: c[0] = 0, c[4] = 1, strictly increasing (contract, checked below).
using Boundaries = std::array<double, kBoundaryCount>;

// Candidate 1 (P2): five nodes evenly spaced. Pure software equal division.
inline constexpr Boundaries kRingEqual = {0.0, 0.25, 0.5, 0.75, 1.0};

// Candidate 2 (P2): NON-UNIFORM SPACING SPECIMEN. NOT derived from the panel -- the previous
// provenance for these numbers is WITHDRAWN (see the P1/P2 block above). Its only role now is
// to let the supervisor compare "norm 0.5 IS a node" (kRingEqual) against "norm 0.5 falls
// inside a stretch" (this one), because the boundary choice is measurable in the output
// (max |delta rmsA| = 0.088968 over 4 rates x 41 norms; probe matrix m1_sweep.tsv).
//
// The single recomputable calculation behind the NUMBERS, with its source stated, is a plain
// rescaling of the six measured glyph ANGLES onto [0,1] (report #115: pdftoppm -r 2000 of
// solar42N_panel_2400px.png, glyph centres as angles from the knob centre, 0 deg = straight
// up): -75, -53, -20, +16, +48, +75 deg. arc = 150 deg, so t = (theta + 75)/150:
//     -75 -> 0.0000   -53 -> 0.1467   -20 -> 0.3667   +16 -> 0.6067   +48 -> 0.8200   +75 -> 1.0000
// Read the values below off that as t = {t0, 2*t1, t2, t4, t5} and nothing else. There is NO
// "independent redundancy evidence" here: an earlier revision of this file and the report
// cross-checked 2*t3 - c2 against c3, but the report substituted t3 = 0.5933, which is the
// interval MIDPOINT (t2+t4)/2 = 0.5933 computed FROM c2 and c3, so the "check" was circular
// (0.8199 vs 0.8200). That claim is deleted. Using the MEASURED t3 = 0.6067 instead gives
// 0.8467 against 0.8200, a 0.0267 discrepancy -- which is itself a sign that neither the
// "glyph marks the stretch centre" assumption nor the angle->norm linearity holds, and is one
// more reason not to present any of this as evidence.
//
// Two things are NOT claimed. (a) No claim that the panel is incompatible with any monotone
// mapping: what a glyph denotes (a stretch interior, a whole morph type, or one
// representative waveform) is unknown, so glyph positions alone exclude nothing. (b) No claim
// that the rescaling is "refuted" -- only that it is UNSUPPORTED. An earlier revision argued
// the last glyph at norm 1.0 must be wrong because a morph glyph has to sit inside its
// stretch; that premise is itself unproven, so the argument is withdrawn along with it.
// Treat the numbers as an arbitrary non-uniform spacing and nothing more.
inline constexpr Boundaries kRingSpaced = {0.0, 0.2933, 0.3667, 0.8200, 1.0};

// ------------------------------------------------------- shape primitives -----
// The phase convention is Vco::waveformSampleAt's (core/include/lunar24/core/vco.h:303-326):
// p is the normalised cycle position in [0,1), one cycle per unit. These four closed forms are
// copied from that function verbatim; the two morph stretches are defined by delegation (below).
inline double sawShape(double p) { return 2.0 * p - 1.0; }
inline double invSawShape(double p) { return -(2.0 * p - 1.0); }
inline double sineShape(double p) { return std::sin(6.28318530717958647692528676655900577 * p); }
inline double triangleShape(double p) { return 4.0 * std::fabs(p - 0.5) - 1.0; }
inline double pulseShape(double p, double duty) { return (p < duty) ? 1.0 : -1.0; }

// Node sample. duty is the pulse duty (Vco::setShape's clamped `duty_`); it is ignored by the
// other nodes, so the map stays a pure function of (norm, p, duty).
inline double nodeSample(Node n, double p, double duty) {
  switch (n) {
    case Node::kSaw:      return sawShape(p);
    case Node::kInvSaw:   return invSawShape(p);
    case Node::kSine:     return sineShape(p);
    case Node::kTriangle: return triangleShape(p);
    case Node::kPulse:    return pulseShape(p, duty);
    case Node::kCount:    break;
  }
  return 0.0;
}

// ------------------------------------------------------------ interval map ----
// Which stretch a norm falls in, and the local coordinate u in [0,1] inside it.
// Contract: c is strictly increasing with c[0] == 0 and c[kBoundaryCount-1] == 1; norm is
// already clamped to [0,1] by Vco::setMorph. A degenerate (non-increasing) vector returns
// stretch 0 with u = 0 rather than dividing by zero.
struct Position {
  int stretch;  // 0 .. kStretchCount-1
  double u;     // local coordinate inside the stretch, [0,1]
};

inline Position locate(const Boundaries& c, double norm) {
  for (int k = 0; k < kStretchCount; ++k) {
    const double lo = c[static_cast<std::size_t>(k)];
    const double hi = c[static_cast<std::size_t>(k + 1)];
    if (norm < hi || k == kStretchCount - 1) {
      const double w = hi - lo;
      if (!(w > 0.0)) return Position{k, 0.0};
      double u = (norm - lo) / w;
      if (u < 0.0) u = 0.0;
      if (u > 1.0) u = 1.0;
      return Position{k, u};
    }
  }
  return Position{kStretchCount - 1, 1.0};
}

inline bool boundariesValid(const Boundaries& c) {
  if (c[0] != 0.0) return false;
  if (c[kBoundaryCount - 1] != 1.0) return false;
  for (int k = 0; k + 1 < kBoundaryCount; ++k) {
    if (!(c[static_cast<std::size_t>(k + 1)] > c[static_cast<std::size_t>(k)])) return false;
  }
  return true;
}

// ------------------------------------------------------------- the mapping ----
// The mapping. norm in [0,1], p in [0,1), duty from setShape. Returns the blended sample.
//   stretch k spans [c[k], c[k+1]] and crossfades node k -> node k+1:
//     y = (1 - u) * node_k(p) + u * node_{k+1}(p),   u = (norm - c[k]) / (c[k+1] - c[k])
// Boundary values (u = 1 on the left stretch, u = 0 on the right stretch) are both exactly
// node_k(p), so the value is continuous in norm at every c[k] and at both ends of [0,1]
// (norm = 0 -> saw, norm = 1 -> pulse).
inline double sampleAt(const Boundaries& c, double norm, double p, double duty) {
  const Position pos = locate(c, norm);
  const Node lo = static_cast<Node>(pos.stretch);
  const Node hi = static_cast<Node>(pos.stretch + 1);
  const double a = nodeSample(lo, p, duty);
  const double b = nodeSample(hi, p, duty);
  return (1.0 - pos.u) * a + pos.u * b;
}

// The weight the pure-triangle node carries in the mix at this norm. Under this node order the
// triangle is the right end of stretch 2 (sine -> triangle) and the left end of stretch 3
// (triangle -> pulse), so the weight is u on stretch 2, 1-u on stretch 3, and 0 elsewhere; it is
// exactly 1.0 at the triangle node itself. Used ONLY to scale the existing triangle BLAMP
// correction (P3); it is NOT a normalisation of the output and changes no other node's level.
inline double triangleWeight(const Boundaries& c, double norm) {
  const Position pos = locate(c, norm);
  // Node index 3 == kTriangle, so it is the RIGHT end of stretch 2 and the LEFT end of
  // stretch 3. (kNodeCount-1) - 1 == 3 with this order; assert that at compile time.
  static_assert(static_cast<int>(Node::kTriangle) == kNodeCount - 2,
                "triangleWeight assumes kTriangle is the second-to-last node (stretch 2->3)");
  if (pos.stretch == kNodeCount - 3) return pos.u;        // stretch 2: rising into triangle
  if (pos.stretch == kNodeCount - 2) return 1.0 - pos.u;  // stretch 3: falling out of triangle
  return 0.0;
}

// ---- The two NAMED stretches, written as the EXISTING morph implementations ----
// These make the reuse explicit and testable: identity with Vco::waveformSampleAt's closed
// forms for kMorphSawInvSaw and kMorphSineTriangle at morph = u.
inline double morphSawInvSaw(double u, double p) {
  const double a = sawShape(p);
  return (1.0 - u) * a + u * invSawShape(p);  // == kMorphSawInvSaw with morph_ = u
}
inline double morphSineTriangle(double u, double p) {
  const double s = sineShape(p);
  const double t = triangleShape(p);
  return (1.0 - u) * s + u * t;               // == kMorphSineTriangle with morph_ = u
}

// stretch index of each named morph under this node order
inline constexpr int kStretchMorphSawInvSaw = 0;   // saw -> invSaw
inline constexpr int kStretchMorphSineTriangle = 2; // sine -> triangle

}  // namespace lunar24::core::wave_map

#endif  // LUNAR24_CORE_VCO_WAVE_MAP_H
