// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Device output topology + slot addressing — P3-⑥ Debt 1. The P1-③ device model
// wrote signals into a PLANAR `float phys[kMaxPhys][kFrames]` (channel-major
// blocks). That abstraction hides the exact bug this file exists to catch: real
// output devices describe their channel arrangement with an AudioBufferList, and
// the slot a (channel, frame) sample occupies depends on it. An interleaved
// device packs samples frame-major (`[f0ch0, f0ch1, f1ch0, f1ch1, ...]`); a
// non-interleaved device keeps one buffer per channel. An adapter that ignores the
// layout and writes channel-major into an interleaved buffer silently scrambles
// every frame — invisible to a planar reader, and exactly what Debt 1 must prove
// is caught.
//
// The real topology authority is device_probe.cpp (kAudioDeviceProperty
// StreamConfiguration → AudioBufferList): 1 buffer × N channels ⇒ Interleaved;
// N buffers × 1 channel ⇒ NonInterleaved. This header is FRAMEWORK-FREE (no
// CoreAudio types) so the core can consume a DeviceLayout it is handed, and the
// test-side/platform artifact does the actual AudioBufferList → DeviceLayout
// conversion (design/07 §5: Core never touches AudioBufferList/device float).
//
// The logical→physical mapping is DATA, not a constant: `OutputMapping::canonical()`
// is the P1-③ default {WET_L=0, WET_R=1, DRY_A=2, DRY_B=3}, but a caller passes
// whatever the device/patch resolved (e.g. upper face {4,5,6,7}) — never a
// hardcoded branch (design/03 L57 "所选").
//
// Header-only, no heap, no locks, realtime-safe.

#pragma once

#include <cmath>

namespace lunar24::core {

// Two canonical output topologies (AudioBufferList-derived). See file header.
enum class BufferLayoutKind { Interleaved, NonInterleaved };

struct DeviceLayout {
  BufferLayoutKind kind = BufferLayoutKind::Interleaved;
  int totalChannels = 0;  // physical output channel count
  bool valid() const { return totalChannels > 0; }
};

// Logical outputs the core always emits (design/01 §1, design/07 §5): the dual
// VCO's WET out L/R plus the two dry taps. Order matches P1-③'s `Logical`.
enum Logical : int { WET_L = 0, WET_R = 1, DRY_A = 2, DRY_B = 3, NONE = -1 };

// logical output -> physical channel. A VALUE, not an authority constant.
struct OutputMapping {
  int channel[4];

  static OutputMapping canonical() { return OutputMapping{{0, 1, 2, 3}}; }
};

// The slot (flat index) of (channel, frame) in a device buffer described by
// `layout`. `frames` is the per-run frame count, needed only for NonInterleaved
// (per-channel contiguous blocks). This is the ONE addressing function a
// layout-aware renderer must use — using a different one (or assuming planar on
// an interleaved device) is precisely the bug Debt 1's negative drives at.
inline int device_slot(const DeviceLayout& layout, int channel, int frame, int frames) {
  switch (layout.kind) {
    case BufferLayoutKind::NonInterleaved:
      // Each channel owns a contiguous frames-long block.
      return channel * frames + frame;
    case BufferLayoutKind::Interleaved:
    default:
      // Frame-major: all channels of frame `frame` are adjacent.
      return frame * layout.totalChannels + channel;
  }
}

// Progress: a DISTINGUISHABLE signal for each logical, so a read-back can
// classify which logical a physical channel actually carried (P1-③ premise).
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

// Classify a received physical block back to a logical output (the read-back).
// DC first (±0.5 → DRY A/B); otherwise pick the dominant reference table by
// normalized correlation with 440/550. Same judges as P1-③.
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

// The layout-AWARE renderer: write each logical (mapped to a physical channel)
// into a flat device buffer using the real slot addition. This is the behavior
// the DeviceAdapter must have; filling `out` via any other addressing is the bug.
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

// Read physical channel `ch`'s block out of a device buffer under `layout` into
// `dst` (which must hold `frames` floats). Uses the SAME slot function as render,
// so a layout mismatch between writer and reader surfaces as garbage.
inline void extract_channel(const float* src, int frames, const DeviceLayout& layout,
                            int ch, float* dst) {
  for (int f = 0; f < frames; ++f) {
    dst[f] = src[device_slot(layout, ch, f, frames)];
  }
}

}  // namespace lunar24::core
