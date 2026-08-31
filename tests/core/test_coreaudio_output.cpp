// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P3-⑥ Debt 1 (platform half) — the four logic outputs against the REAL machine's
// output layout. The companion test_device_layout.cpp proves the CORE contract on a
// synthetic DeviceLayout; this file reads the ACTUAL default output device's
// AudioBufferList (the framework authority, device_probe.cpp's idiom) and verifies the
// four logicals (WET L/R + DRY A/B) land per-channel under that real layout.
//
// @Claude's adjudications folded in:
//  - ORACLE is a default, not a constant: OutputMapping::canonical() {0,1,2,3} is the
//    P1-③ default; the adapter could pass a different wiring (device_layout.h documents
//    it as data). We exercise the default wiring here.
//  - The read-back validates the ADAPTER-FILLED buffer memory under the real layout
//    (it is an in-memory stride/interleave check). It does NOT prove the driver then
//    delivered those slots to the intended physical jack — that is an explicit boundary,
//    recorded in FINDINGS.md. We never push an audible live stream.
//  - Device-gating: it reads the default device's real layout but never plays audio.
//    On a machine with no default device, or a default with <4 out channels, or a
//    non-macOS build, it SKIPA loudly (prints the reason); see the skip handling below.
//
// The negative (Debt 1's RED case): for an interleaved device, reading the SAME buffer
// through the PLANAR (channel-major, P1-③) addressing yields exactly the scrambled data
// the layout-aware view catches — so the "treat interleaved as planar" bug must fire RED.
// For a NON-interleaved device planar reads coincide with the real layout, so the bug is
// not distinguishable there and the negative is honestly skipped (not faked red).

#include "mini_test.h"

#include <lunar24/core/device_layout.h>
#include "device_signal_fixture.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

namespace {
using lunar24::core::BufferLayoutKind;
using lunar24::core::DeviceLayout;
using lunar24::core::OutputMapping;
using lunar24::core::extract_channel;
using lunar24::core::logical_signal;
using lunar24::core::render_device_output;

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;

#ifdef __APPLE__
// Read the default output device's real channel arrangement into `out`. Returns true
// on success; false means there is no default output device (caller skips loudly).
bool read_default_layout(DeviceLayout& out) {
  AudioObjectPropertyAddress def_addr{
      kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain};
  AudioDeviceID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &def_addr, 0, nullptr, &size, &dev) != noErr)
    return false;
  if (dev == kAudioObjectUnknown) return false;

  // The authority: kAudioDevicePropertyStreamConfiguration (Output scope) -> AudioBufferList.
  AudioObjectPropertyAddress stream{kAudioDevicePropertyStreamConfiguration,
                                    kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyDataSize(dev, &stream, 0, nullptr, &size) != noErr) return false;
  if (size < sizeof(AudioBufferList)) return false;
  std::vector<std::uint8_t> buf(size);
  if (AudioObjectGetPropertyData(dev, &stream, 0, nullptr, &size, buf.data()) != noErr) return false;
  const auto* abl = reinterpret_cast<const AudioBufferList*>(buf.data());

  if (abl->mNumberBuffers == 1) {
    // One buffer packing all channels = interleaved (frame-major).
    out.kind = BufferLayoutKind::Interleaved;
    out.totalChannels = static_cast<int>(abl->mBuffers[0].mNumberChannels);
  } else {
    // Multiple buffers, one channel each = non-interleaved (per-channel blocks).
    int ch = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i)
      ch += static_cast<int>(abl->mBuffers[i].mNumberChannels);
    out.kind = BufferLayoutKind::NonInterleaved;
    out.totalChannels = ch;
  }
  return out.valid();
}
#endif

// Count physical channels of `out` that CLAIM a logical (via `mapping`) but do not
// carry logical_signal(claim, f) exactly — read through `reader`. Exact-match purity,
// not correlation (a transposed block can still correlate; exact-match cannot be fooled).
int count_faults(const float* out, const DeviceLayout& reader, const OutputMapping& mapping, int frames) {
  int faults = 0;
  for (int o = 0; o < 4; ++o) {
    const int p = mapping.channel[o];
    if (p < 0 || p >= reader.totalChannels) continue;
    std::vector<float> blk(static_cast<std::size_t>(frames));
    extract_channel(out, frames, reader, p, blk.data());
    for (int f = 0; f < frames; ++f) {
      if (blk[f] != logical_signal(o, f, kSr) && std::fabs(blk[f] - logical_signal(o, f, kSr)) > 1e-4f) {
        ++faults;
        break;
      }
    }
  }
  return faults;
}

int run(const char* suite) {
#ifdef __APPLE__
  DeviceLayout layout;
  if (!read_default_layout(layout)) {
    std::printf("[SKIP] test_coreaudio_output: no default CoreAudio output device found");
    return ::test::finish(suite);  // loud skip; CI has no device so this never validates audio
  }
  if (layout.totalChannels < 4) {
    std::printf("[SKIP] test_coreaudio_output: default output has %d channels < 4; "
                "need 4 physical outs for the four logic outputs",
                layout.totalChannels);
    return ::test::finish(suite);  // loud skip with count+reason
  }

  // Fill a device buffer the way the adapter must: one logical per claimed physical
  // channel, at the real device's address (device_slot). This is the layout-aware path.
  const std::size_t total =
      static_cast<std::size_t>(layout.totalChannels) * static_cast<std::size_t>(kFrames);
  std::vector<float> out(total, 0.0f);
  render_device_output(out.data(), kFrames, layout, OutputMapping::canonical(), kSr);

  std::printf("  device: %d output channels, %s\n", layout.totalChannels,
              layout.kind == BufferLayoutKind::Interleaved ? "interleaved" : "non-interleaved");

  // GOOD: read back under the REAL layout -> the four logicals land per-channel cleanly.
  const int good = count_faults(out.data(), layout, OutputMapping::canonical(), kFrames);
  CHECK(good == 0);

  // NEGATIVE (Debt 1's RED): read the SAME buffer as if the device were planar/channel-
  // major (P1-③). For an interleaved device this scrambles every frame and must fire RED.
  // For a non-interleaved device planar reads == the real layout, so the bug is not
  // distinguishable: skip it honestly (never fake a red).
  int red = 0;
  if (layout.kind == BufferLayoutKind::Interleaved) {
    const DeviceLayout planar{BufferLayoutKind::NonInterleaved, layout.totalChannels};
    red = count_faults(out.data(), planar, OutputMapping::canonical(), kFrames);
    CHECK(red > 0);
    std::printf("  GOOD layout-aware read-back: 0 faults | RED interleave-as-planar read-back: %d fault(s) (negative fires)\n",
                red);
  } else {
    std::printf("  GOOD layout-aware read-back: 0 faults | [WARN] non-interleaved device: the "
                "interleave-as-planar negative is indistinguishable here (planar reads == the "
                "real layout); layout-aware read-back is still exact.\n");
  }

  // Documented boundary (findings): we validated the adapter-FILLED buffer under the real
  // layout; we did NOT open an audible stream nor verify the driver routed these slots to
  // the intended physical jack.
  std::printf("  [boundary] in-memory stride/interleave check only; no audible live stream, no "
              "hardware echo (FINDINGS.md).\n");
#else
  std::printf("[SKIP] test_coreaudio_output: not building on macOS (CoreAudio unavailable)");
#endif
  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== P3-⑥ Debt 1 (platform half): real CoreAudio output layout, four logicals per-channel ==\n");
  return run("coreaudio_output");
}
