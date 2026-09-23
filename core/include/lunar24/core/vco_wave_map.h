// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// vco_wave_map.h — the continuous single-knob waveform mapping of the main V/Oct VCOs,
// as adopted by Raft task #117 (GH #19 S0). THIS IS THE PRODUCTION LAW: Vco's default
// waveform (VcoWaveform::kMorphRing) renders through wave_map::sampleAt below. There is
// no opt-in and no switch back to the pre-#117 fixed-triangle behaviour on any product
// path; Vco::setWaveform remains only as the module-development raw-waveform interface.
//
// ============================ WHAT IS CLAIMED ===============================
// OWNER FACT (@bearbone, Raft msg b4ffcd85, 2026-09-13), and the ONLY thing the owner
// confirmed: on the Elta Solar 42 / 42N the MORPHING WAVEFORM knob of each main V/Oct VCO
// turns CONTINUOUSLY and changes the waveform gradually. The manual
// (design/reference/solar42N_instruct_08_04_v15.pdf p.9) names six waveforms in total: four
// traditional shapes plus two variable morphing ones (saw -> inverted saw, sine -> triangle).
// The manual does NOT state how a waveform is selected, and it does NOT state the knob's
// rotation direction. @Codex msg d2b3f787 fixes the product shape: ONE knob, reuse the
// EXISTING per-side `morph` parameter, no new wave selector, no second morph-control quantity.
//
// ONE coordinate, so the six named features lie on ONE continuous sweep:
//
//      norm:  0         c1            c2            c3         1
//      node:  saw --- invSaw ------ sine ------- triangle -- pulse
//      stretch: \__S0__/ \___S1____/ \___S2____/ \___S3___/
//      name:    morph-1   (none)      morph-2     (none)
//
//   * S0 (saw -> invSaw)    = the manual's first NAMED morphing waveform.
//   * S2 (sine -> triangle) = the manual's second NAMED morphing waveform.
//   * S1 (invSaw -> sine) and S3 (triangle -> pulse) are UNNAMED software connectors: they
//     exist only because one continuous sweep has to get from one named feature to the next.
//     They are reported, never presented as hardware structure.
//   * The six manual names are therefore 4 NODES (saw, sine, triangle, pulse) + 2 STRETCHES
//     (S0, S2) = 6. They are NOT six fixed detents: "6 waveforms in total" counts named
//     features on the sweep. The accounting for five nodes: the two morphs each contribute a
//     FAR ENDPOINT — "saw to inverted saw" adds invSaw (NEW, +1 node), "sine to triangle" adds
//     nothing (both ends are already traditional shapes, +0). 4 + 1 = 5, and invSaw is the only
//     node with no separate glyph on the panel.
//
// EXACTLY the two existing morph implementations are reused for the two named stretches (see
// morphSawInvSaw / morphSineTriangle below): S0 is Vco::waveformSampleAt's kMorphSawInvSaw
// closed form, S2 is its kMorphSineTriangle closed form, each at morph = the local coordinate
// u. No new waveform math is introduced.
//
// ============================ WHAT IS SOFTWARE PROVISIONAL ==================
//   (P1) the node ORDER [saw, invSaw, sine, triangle, pulse]. Only the two NAMED stretches'
//        ENDPOINT PAIRS are manual-fixed ("saw to inverted saw", "sine to triangle"); the manual
//        does not fix which end sits at the lower norm (the knob direction is unstated), so even
//        S0's internal direction is a software choice. An earlier task (#116) attempted to
//        derive this order from panel glyph angles; THAT DERIVATION IS WITHDRAWN and is not
//        carried here — the current derivation cannot support the conclusion. No hardware
//        structure is claimed for the order.
//   (P2) the boundary vector kRingEqual: five nodes at k/4, a pure software equal division
//        (@Codex ad68abb6 allows equal division as a provisional candidate; what is forbidden
//        is presenting it as hardware structure). The owner confirmed only the CONTINUOUS
//        behaviour, not this curve.
//   (P3) the BLAMP scaling: the existing triangle slope correction is scaled by the triangle's
//        weight in the mix (triangleWeight below). First-order, provisional, and exact only at
//        the pure-triangle node.
//   (P5) the pulse-BLEP scaling (added by task #118, GH#19 S3): the pulse's value-jump correction
//        (pulse_blep_kernel.h) is scaled by the pulse's weight in the mix (pulseWeight below),
//        with the same first-order provisional status and the same exactness at its own node.
//        Exact only at the pure-pulse node (norm = 1.0). It is NOT a band-limiting of the mixed
//        output, and it does not correct the saw / invSaw / sine nodes, which remain naive.
//   (P4) the pulse node reads `duty_` (= the `pw` panel parameter), so the sweep makes the
//        pulse's duty audible. Approved as in scope by @Codex (task #116): the normal
//        consumption of an EXISTING parameter.
//
// This is a SOFTWARE PROVISIONAL transfer, not a hardware fact. The three frozen facts it must
// not disturb are: the wire bytes (the mapping is not persisted — it is a pure function of the
// EXISTING `morph` value), the parameter IDs, and the parameter DEFAULT VALUES. Default
// `morph = 0.5` lands exactly on the sine node, so the default sound changes from triangle to
// sine — that is the intended, reported consequence of adopting the mapping, not an accident.
//
// ============================ KNOWN, REPORTED, NOT HIDDEN ===================
// S0's two endpoints are antipodal (invSaw(p) = -saw(p)), so at the exact centre of S0 the
// convex blend is identically 0.0 for EVERY phase: the sweep passes through silence there.
// This is measured and reported, and it is deliberately NOT compensated by any hidden
// normalisation or gain curve. The same is already true of the existing kMorphSawInvSaw, whose
// comment records "passes through 0 at mid".
//
// The blend is a partition of unity of piecewise-linear weights: the value is continuous in
// `norm` at every boundary (left limit = right limit = the node shape) but its derivative is
// not, so the sweep corners at the nodes. Continuity is the requirement; C1 is not claimed.
//
// The mixed output is a convex combination of band-limited triangle and pulse nodes with NAIVE
// saw / invSaw / sine. It is NOT band-limited and is never called band-limited. Only the two
// corrected nodes (triangle via BLAMP, pulse via BLEP) are corrected, each in proportion to its
// own weight in the mix, and each exactly at its own node.

#ifndef LUNAR24_CORE_VCO_WAVE_MAP_H
#define LUNAR24_CORE_VCO_WAVE_MAP_H

#include <array>
#include <cmath>
#include <cstddef>

namespace lunar24::core::wave_map {

// ---------------------------------------------------------------- the ring ----
// Node order (P1, software provisional). Directions inside S0 and S2 are manual-fixed.
enum class Node : int {
  kSaw = 0,      // 2p - 1
  kInvSaw = 1,   // -(2p - 1)
  kSine = 2,     // sin(2*pi*p)
  kTriangle = 3, // 4*|p - 0.5| - 1
  kPulse = 4,    // (p < duty) ? +1 : -1
  kCount = 5
};

inline constexpr int kNodeCount = 5;
inline constexpr int kStretchCount = kNodeCount - 1;  // 4 boundaries-1 stretches
inline constexpr int kBoundaryCount = kNodeCount;     // c0 .. c4, inclusive ends

// A boundary vector: c[0] = 0, c[4] = 1, strictly increasing (contract, checked by
// boundariesValid below and asserted in the probe).
using Boundaries = std::array<double, kBoundaryCount>;

// The adopted boundary vector (P2): five nodes evenly spaced. Pure software equal division.
inline constexpr Boundaries kRingEqual = {0.0, 0.25, 0.5, 0.75, 1.0};

// ------------------------------------------------------- shape primitives -----
// The phase convention is Vco::waveformSampleAt's (core/include/lunar24/core/vco.h): p is the
// normalised cycle position in [0,1), one cycle per unit. These four closed forms are that
// function's verbatim; the two morph stretches are defined by delegation (below).
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

// The weight the pure-PULSE node carries in the mix at this norm, with the same contract as
// triangleWeight above and for the same reason: it is the weight of a NODE in a convex blend, not
// a normalisation of the output. kPulse is the last node, so it is the right end of the last
// stretch (3, triangle -> pulse); the weight is u there, 0 elsewhere, and exactly 1.0 at the pulse
// node itself. Used ONLY to scale the pulse BLEP correction (pulse_blep_kernel.h, task #118);
// it changes no node's level.
//
// The two weights are complementary on stretch 3: triangleWeight = 1-u and pulseWeight = u, so at
// u = 1 the triangle BLAMP is exactly 0 and the pulse BLEP exactly full, and at u = 0 the reverse.
// Only ONE of the two node corrections is ever in force at either end of the stretch, and in
// between both run scaled by their own node's weight -- the same first-order provisional law P3
// already states for the triangle, applied to the second corrected node. Neither weight makes the
// MIXED output band-limited.
inline double pulseWeight(const Boundaries& c, double norm) {
  const Position pos = locate(c, norm);
  static_assert(static_cast<int>(Node::kPulse) == kNodeCount - 1,
                "pulseWeight assumes kPulse is the last node (stretch 3)");
  if (pos.stretch == kNodeCount - 2) return pos.u;  // stretch 3: rising into pulse
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
inline constexpr int kStretchMorphSawInvSaw = 0;    // saw -> invSaw
inline constexpr int kStretchMorphSineTriangle = 2; // sine -> triangle

}  // namespace lunar24::core::wave_map

#endif  // LUNAR24_CORE_VCO_WAVE_MAP_H
