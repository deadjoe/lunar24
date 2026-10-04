// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// blep_kernel.h — a wide band-limited step (BLEP) residual for the VCO's saw and pulse edges.
//
// A value jump (the saw's wrap, the pulse's two edges) is band-limited by adding, around the
// jump, the difference between a band-limited step and the hard step: r(s) = Phi(s) - H(s),
// where Phi is the running integral of a windowed sinc (cut-off at Nyquist) and s is the
// signed distance from the jump in samples (s > 0 after it). It is the step counterpart of the
// triangle's BLAMP (blamp_kernel.h): the same sinc, the same Hann window cos^2(pi*s/(2L)),
// the same support L = 8 samples. Compared with the two-point polyBLEP it replaces, it pushes
// the audible aliasing of a high saw or pulse from about -40 dB to well under -60 dB.
//
// r is odd: r(-s) = -r(s), r(0+) = -1/2, and exactly 0 for |s| >= L. The table holds r for
// s in [0, L]; it is filled once at program start (no allocation, nothing on the audio thread).
#pragma once

#include <array>
#include <cmath>

namespace lunar24::core {

inline constexpr double kBlepSupport = 8.0;  // samples each side of the jump
inline constexpr int kBlepLutSize = 2048;    // cells over [0, L]

namespace blep_detail {
inline double windowedSinc(double s) {
  constexpr double kPi = 3.14159265358979323846;
  if (std::fabs(s) >= kBlepSupport) return 0.0;
  const double sinc = s == 0.0 ? 1.0 : std::sin(kPi * s) / (kPi * s);
  const double w = std::cos(kPi * s / (2.0 * kBlepSupport));
  return sinc * w * w;
}
inline std::array<double, kBlepLutSize + 1> makeLut() {
  // Integrate the kernel on a fine grid (trapezoid) from 0 to L, then normalise so the whole
  // kernel has unit area: Phi(s) = 1/2 + integral_0^s / area.
  constexpr int kSub = 32;
  const double h = kBlepSupport / (kBlepLutSize * kSub);
  std::array<double, kBlepLutSize + 1> half{};  // integral from 0 to each cell edge
  double acc = 0.0, prev = windowedSinc(0.0);
  for (int i = 1; i <= kBlepLutSize * kSub; ++i) {
    const double cur = windowedSinc(i * h);
    acc += 0.5 * (prev + cur) * h;
    prev = cur;
    if (i % kSub == 0) half[i / kSub] = acc;
  }
  const double area = 2.0 * acc;
  std::array<double, kBlepLutSize + 1> lut{};
  for (int i = 0; i <= kBlepLutSize; ++i) lut[i] = 0.5 + half[i] / area - 1.0;  // Phi - H, s > 0
  lut[kBlepLutSize] = 0.0;
  return lut;
}
inline const std::array<double, kBlepLutSize + 1> kLut = makeLut();
}  // namespace blep_detail

// r(s): what to add, per unit of upward jump, at a sample s samples after the jump.
inline double blepResidual(double s) {
  const double a = std::fabs(s);
  if (!(a < kBlepSupport)) return 0.0;  // outside the support, or NaN
  const double x = a * (kBlepLutSize / kBlepSupport);
  const int i = static_cast<int>(x);
  const double f = x - i;
  const double r = blep_detail::kLut[i] + f * (blep_detail::kLut[i + 1] - blep_detail::kLut[i]);
  return s >= 0.0 ? r : -r;
}

// Sum of the residuals of a jump that repeats once per cycle at phase `edge` (0..1), seen from
// a sample at phase `t` (0..1) with `dt` cycles per sample: every repeat of the jump within L
// samples on either side counts (at high pitch several do, and they simply add up).
inline double blepEdgeSum(double t, double edge, double dt) {
  if (!(dt > 0.0) || !std::isfinite(dt)) return 0.0;
  const int radius = static_cast<int>(std::ceil(kBlepSupport * dt)) + 1;
  if (radius > 64) return 0.0;  // far above Nyquist: nothing sensible to correct
  double sum = 0.0;
  for (int n = -radius; n <= radius; ++n) sum += blepResidual((t - edge - n) / dt);
  return sum;
}

}  // namespace lunar24::core
