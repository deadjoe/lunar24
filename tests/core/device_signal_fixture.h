// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// device_signal_fixture.h — TEST-ONLY synthetic signal generators.
//
// The GH#4 mandate (@Codex 6c74e22d) requires the production DeviceAdapter to be the
// bridge between runtime virtual volts and device-normalized samples, and it forbids
// the adapters ever being driven by a synthetic test generator living in the production
// core. These three helpers are exactly that: they SYNTHESIZE a distinguishable
// per-logical test signal (P1-③ premise) and CLASSIFY a received block back to a
// logical, so a layout read-back test / the platform-half test can observe which
// physical channel carried which logical. They are NOT an adapter, do NOT touch
// RuntimeOutput / RuntimeInputs / virtual volts, and must never be used by
// device_adapter.h or the product render.
//
// They were moved out of core/include/lunar24/core/device_layout.h so the production
// core keeps only real layout/address/mapping primitives (DeviceLayout, BufferLayoutKind,
// Logical, OutputMapping, device_slot, extract_channel). This file lives in tests/core/,
// is included only by tests, and keeps the functions in `lunar24::core` so the existing
// `using lunar24::core::logical_signal;` / `render_device_output;` lines in the tests
// stay valid unchanged. Framework-free (matches the core convention), header only.

#pragma once

#include <cmath>

#include <lunar24/core/device_layout.h>

namespace lunar24::core {

// Progress: a DISTINGUISHABLE signal for each logical, so a read-back can classify
// which logical a physical channel actually carried (P1-③ premise). TEST-ONLY.
inline float logical_signal(int logical, int frame, double sr) {
  const double kPi = 3.14159265358979323846;
  const double t = static_cast<double>(frame) / sr;
  switch (logical) {
    case WET_L: return 0.5f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * t));
    case WET_R: return 0.5f * static_cast<float>(std::sin(2.0 * kPi * 550.0 * t));
    case DRY_A: return 0.5f;   // DC +0.5
    case DRY_B: return -0.5f;  // DC -0.5
    default:    return 0.0f;
  }
}

// Classify a received physical block back to a logical output (the read-back). DC
// first (±0.5 -> DRY A/B); otherwise pick the dominant reference table by normalized
// correlation with 440/550. TEST-ONLY.
inline int classify_block(const float* b, int n) {
  const double kPi = 3.14159265358979323846;
  const double kSr = 48000.0;
  double mean = 0.0;
  for (int i = 0; i < n; ++i) mean += b[i];
  mean /= n;
  if (std::fabs(mean - 0.5) < 0.05) return DRY_A;
  if (std::fabs(mean + 0.5) < 0.05) return DRY_B;
  double c440 = 0.0, e440 = 0.0, c550 = 0.0, e550 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double s440 = std::sin(2.0 * kPi * 440.0 * t);
    const double s550 = std::sin(2.0 * kPi * 550.0 * t);
    c440 += b[i] * s440; e440 += s440 * s440;
    c550 += b[i] * s550; e550 += s550 * s550;
  }
  const double n440 = std::fabs(c440 / e440);
  const double n550 = std::fabs(c550 / e550);
  if (n440 > n550) return WET_L;
  if (n550 > n440) return WET_R;
  return NONE;
}

// The layout-AWARE renderer: write each logical (mapped to a physical channel) into a
// flat device buffer using the real slot addition — a SYNTHETIC fixture that pre-dates
// (and must never replace) the production DeviceAdapter. TEST-ONLY.
inline void render_device_output(float* out, int frames, const DeviceLayout& layout,
                                 const OutputMapping& mapping, double sr) {
  for (int o = 0; o < 4; ++o) {
    const int p = mapping.channel[o];
    if (p < 0 || p >= layout.totalChannels) continue;  // clamped out by device-native
    for (int f = 0; f < frames; ++f) {
      out[device_slot(layout, p, f, frames)] = logical_signal(o, f, sr);
    }
  }
}

}  // namespace lunar24::core
