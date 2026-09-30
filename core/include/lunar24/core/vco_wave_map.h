// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// vco_wave_map.h — the single MORPHING WAVEFORM knob of the main V/Oct VCOs.
//
// The manual (p.9) names six waveforms: four traditional shapes (sine, triangle, saw, pulse) and
// two morphing ones (saw -> inverted saw, sine -> triangle), and says the knob changes the shape
// continuously. The panel prints six icons around the knob. This map puts each shape where its
// icon is, so the knob sounds like what it points at, and crossfades between neighbours:
//
//   knob:   0 ...... 0.261   0.344    0.450  0.552   0.655     0.739 ........... 1
//   icon:            sine    triangle saw    pulse   saw<>inv  sine<>tri
//   shape:  sine     sine    triangle saw    pulse   invSaw    sine ---------> triangle
//
// * Below the first icon the knob stays on sine.
// * Between two icons the output is a linear crossfade of the two shapes.
// * The "saw <> inverted saw" icon is the inverted saw; turning from the saw icon towards it
//   passes through the pulse, turning past it fades to the sine.
// * From the "sine <> triangle" icon to the end of travel the knob morphs sine -> triangle
//   (morphSineTriangle), the manual's second morphing waveform.
// Node positions are the icon angles measured on the panel drawing ((angle + 150) / 300).
//
// The value is continuous in the knob position (corners at the nodes). Anti-aliasing: the
// triangle (BLAMP) and the pulse, saw and inverted saw (BLEP) corrections each run scaled by
// that shape's weight in the mix (shapeWeight), so each is exact where the output is that pure
// shape. The sine has nothing to correct. The mixed output is not fully band-limited.

#ifndef LUNAR24_CORE_VCO_WAVE_MAP_H
#define LUNAR24_CORE_VCO_WAVE_MAP_H

#include <array>
#include <cmath>
#include <cstddef>

namespace lunar24::core::wave_map {

enum class Node : int {
  kSaw = 0,      // 2p - 1
  kInvSaw = 1,   // -(2p - 1)
  kSine = 2,     // sin(2*pi*p)
  kTriangle = 3, // 4*|p - 0.5| - 1
  kPulse = 4,    // (p < duty) ? +1 : -1
};

inline constexpr int kNodeCount = 8;
inline constexpr int kStretchCount = kNodeCount - 1;

// A ring: node positions (strictly increasing, first 0, last 1) and the shape at each node.
struct Ring {
  std::array<double, kNodeCount> at;
  std::array<Node, kNodeCount> shape;
};

// The production ring: shapes at the panel icon positions.
inline constexpr Ring kRingPanel = {
    {0.0, 0.261, 0.344, 0.450, 0.552, 0.655, 0.739, 1.0},
    {Node::kSine, Node::kSine, Node::kTriangle, Node::kSaw, Node::kPulse, Node::kInvSaw,
     Node::kSine, Node::kTriangle}};

// Knob positions pointing straight at each icon.
inline constexpr double kIconSine = 0.261;
inline constexpr double kIconTriangle = 0.344;
inline constexpr double kIconSaw = 0.450;
inline constexpr double kIconPulse = 0.552;
inline constexpr double kIconSawInvSaw = 0.655;
inline constexpr double kIconSineTriangle = 0.739;

// ------------------------------------------------------- shape primitives -----
// p is the normalised cycle position in [0,1) (Vco::waveformSampleAt's convention).
inline double sawShape(double p) { return 2.0 * p - 1.0; }
inline double invSawShape(double p) { return -(2.0 * p - 1.0); }
inline double sineShape(double p) { return std::sin(6.28318530717958647692528676655900577 * p); }
inline double triangleShape(double p) { return 4.0 * std::fabs(p - 0.5) - 1.0; }
inline double pulseShape(double p, double duty) { return (p < duty) ? 1.0 : -1.0; }

inline double nodeSample(Node n, double p, double duty) {
  switch (n) {
    case Node::kSaw:      return sawShape(p);
    case Node::kInvSaw:   return invSawShape(p);
    case Node::kSine:     return sineShape(p);
    case Node::kTriangle: return triangleShape(p);
    case Node::kPulse:    return pulseShape(p, duty);
  }
  return 0.0;
}

// Which stretch (node k -> node k+1) a norm falls in, and the local coordinate u in [0,1].
struct Position {
  int stretch;
  double u;
};

inline Position locate(const Ring& r, double norm) {
  for (int k = 0; k < kStretchCount; ++k) {
    const double lo = r.at[static_cast<std::size_t>(k)];
    const double hi = r.at[static_cast<std::size_t>(k + 1)];
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

inline bool ringValid(const Ring& r) {
  if (r.at[0] != 0.0 || r.at[kNodeCount - 1] != 1.0) return false;
  for (int k = 0; k + 1 < kNodeCount; ++k) {
    if (!(r.at[static_cast<std::size_t>(k + 1)] > r.at[static_cast<std::size_t>(k)])) return false;
  }
  return true;
}

// The knob's output: (1 - u) * node_k(p) + u * node_{k+1}(p).
inline double sampleAt(const Ring& r, double norm, double p, double duty) {
  const Position pos = locate(r, norm);
  const auto k = static_cast<std::size_t>(pos.stretch);
  const double a = nodeSample(r.shape[k], p, duty);
  const double b = nodeSample(r.shape[k + 1], p, duty);
  return (1.0 - pos.u) * a + pos.u * b;
}

// The weight a shape carries in the mix at this norm (sum over the nodes with that shape).
// 1.0 where the output is that pure shape, 0.0 where it is absent. Scales the shape's own
// anti-aliasing correction; it is not a gain on the output.
inline double shapeWeight(const Ring& r, double norm, Node n) {
  const Position pos = locate(r, norm);
  const auto k = static_cast<std::size_t>(pos.stretch);
  double w = 0.0;
  if (r.shape[k] == n) w += 1.0 - pos.u;
  if (r.shape[k + 1] == n) w += pos.u;
  return w;
}

inline double sawWeight(const Ring& r, double norm) { return shapeWeight(r, norm, Node::kSaw); }
inline double invSawWeight(const Ring& r, double norm) {
  return shapeWeight(r, norm, Node::kInvSaw);
}
inline double triangleWeight(const Ring& r, double norm) {
  return shapeWeight(r, norm, Node::kTriangle);
}
inline double pulseWeight(const Ring& r, double norm) { return shapeWeight(r, norm, Node::kPulse); }

// The manual's two morphing waveforms as closed forms (== Vco's kMorphSawInvSaw /
// kMorphSineTriangle at morph = u).
inline double morphSawInvSaw(double u, double p) {
  return (1.0 - u) * sawShape(p) + u * invSawShape(p);
}
inline double morphSineTriangle(double u, double p) {
  return (1.0 - u) * sineShape(p) + u * triangleShape(p);
}

}  // namespace lunar24::core::wave_map

#endif  // LUNAR24_CORE_VCO_WAVE_MAP_H
