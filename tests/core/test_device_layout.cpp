// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// P3-⑥ Debt 1 — real output layout, per-channel four-logical verification.
//
// WHY THIS EXISTS. P1-③'s device model writes signals into a PLANAR channel-major
// `float phys[kMaxPhys][kFrames]` and reads them back the same way. That model is
// SELF-CONSISTENT: it writes channel-major and reads channel-major, so it can never
// see row-major-vs-interleaved mistakes. A real output device describes its channel
// arrangement with an AudioBufferList (device_probe.cpp authority), and an adapter
// that ignores it and writes channel-major into an INTERLEAVED buffer scrambles
// every frame. This test proves that error is caught here where it can't be in the
// planar model:
//
//   GOOD   interleaved (1×N) and non-interleaved (N×1) layout addressing -> 0 faults.
//   SILENT the interleaved-as-planar writer read back PLANARLY (P1-③'s model)
//          -> 0 faults. That is the gap: a real interleaved device would be fed
//          scrambled data and the planar checker would still report "all good".
//   RED    the SAME interleaved-as-planar writer read back via the device's REAL
//          interleaved layout -> >0 faults. This is Debt 1's negative.
//   4x     a mapping error (two logicals on one physical) still red under the
//          layout-aware checker (P1-③'s guarantee preserved).
//
// The read-back judge here is an EXACT-MATCH purity test (every frame of a channel
// that claims logical L must equal logical_signal(L, f)), not a dominant-frequency
// correlation. Correlation is too forgiving: after a layout transposition a channel
// block is a FRANKENTEIN of several logicals and can still correlate to one of them,
// giving a false pass. Exact-match is deterministic and cannot be fooled — the exact
// thing Debt 1 needs when it asks "did logical L land cleanly on channel p".
//
// @Claude's adjudications folded in: the logical→physical mapping is DATA
// (`OutputMapping::canonical()` is the default, a different wiring is a value), and
// the read-back validates the adapter-FILLED buffer (an in-memory check, not a
// hardware echo); it does NOT prove the driver later sent those slots to the intended
// jack (declared boundary, recorded in FINDINGS).

#include "mini_test.h"

#include <lunar24/core/device_layout.h>

#include <cmath>
#include <vector>

namespace {
using lunar24::core::BufferLayoutKind;
using lunar24::core::DeviceLayout;
using lunar24::core::Logical;
using lunar24::core::OutputMapping;
using lunar24::core::device_slot;
using lunar24::core::extract_channel;
using lunar24::core::logical_signal;
using lunar24::core::render_device_output;

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;

// Count faults on a buffer filled by `writer`. For every physical channel that
// `intent` claims, extract that channel's block under `reader_layout` and assert
// it matches logical_signal(claim, f) exactly (the intended logical, clean).
// `reader_layout` is how the verification reads the buffer — it should be the
// DEVICE's real layout, so a writer that used a different addressing surfaces.
template <typename Writer>
int verify_faults(const DeviceLayout& reader_layout, const OutputMapping& intent,
                  Writer&& writer) {
  std::vector<float> out(static_cast<std::size_t>(kFrames) *
                         static_cast<std::size_t>(reader_layout.totalChannels), 0.0f);
  writer(out.data());

  int faults = 0;
  for (int p = 0; p < reader_layout.totalChannels; ++p) {
    int claim = -1;  // logical that physical channel p SHOULD carry (from intent)
    for (int o = 0; o < 4; ++o) {
      if (intent.channel[o] == p) { claim = o; break; }
    }
    if (claim == -1) continue;

    std::vector<float> blk(kFrames);
    extract_channel(out.data(), kFrames, reader_layout, p, blk.data());
    for (int f = 0; f < kFrames; ++f) {
      const float expect = logical_signal(claim, f, kSr);
      if (std::fabs(blk[f] - expect) > 1e-4f) { ++faults; break; }  // purity broke
    }
  }
  return faults;
}

int run(const char* suite) {
  const DeviceLayout interleaved{BufferLayoutKind::Interleaved, 4};
  const DeviceLayout planar{BufferLayoutKind::NonInterleaved, 4};   // channel-major blocks
  const DeviceLayout dev8{BufferLayoutKind::Interleaved, 8};

  const OutputMapping canon = OutputMapping::canonical();           // {WET_L=0,..,DRY_B=3}
  const OutputMapping upper{ {4, 5, 6, 7} };                        // upper face (8-out)
  const OutputMapping swapL{ {1, 0, 2, 3} };                        // WET L/R swapped
  const OutputMapping collide{ {0, 1, 2, 2} };                      // DRY B collides DRY A

  // WRONG writer: P1-③'s planar as-if-channel-major, regardless of device layout.
  // This is the bug — on an interleaved device it scrambles every frame.
  // (The GOOD cases below render via render_device_output, which is layout-aware.)
  auto planar_writer = [](float* out) {
    for (int o = 0; o < 4; ++o) {
      for (int f = 0; f < kFrames; ++f) out[o * kFrames + f] = logical_signal(o, f, kSr);
    }
  };

  // GOOD: layout-respecting writer, read back under the SAME (device) layout.
  {
    const int i_good = verify_faults(interleaved, canon, [&](float* out) {
      render_device_output(out, kFrames, interleaved, canon, kSr);
    });
    const int n_good = verify_faults(planar, canon, [&](float* out) {
      render_device_output(out, kFrames, planar, canon, kSr);
    });
    std::printf("  interleaved GOOD canonical=%d | non-interleaved GOOD canonical=%d\n", i_good, n_good);
    CHECK(i_good == 0);
    CHECK(n_good == 0);
  }

  // SILENT (the gap): interleaved-as-planar writer read back PLANARLY = P1-③'s model
  // is self-consistent and reports 0 faults. This is exactly why Debt 1 exists.
  {
    const int silent = verify_faults(planar, canon, planar_writer);
    std::printf("  SILENT  interleaved-as-planar writer, planar reader (P1-③ model)=%d (0 = the gap)\n", silent);
    // This is a demonstration of the GAP — it is intentionally 0 (false pass).
    // We assert it equals 0 so the contrast is explicit and self-documenting.
    CHECK(silent == 0);
  }

  // RED (Debt 1's negative): the SAME interleaved-as-planar writer, read back via the
  // device's REAL interleaved layout -> the layout error surfaces as >0 faults.
  {
    const int red = verify_faults(interleaved, canon, planar_writer);
    std::printf("  RED     interleaved-as-planar writer, INTERLEAVED reader=%d (expect >0)\n", red);
    CHECK(red > 0);
  }

  // Mapping errors still red under the layout-aware checker (P1-③ guarantee kept):
  // upper-face canonical clean, upper swap + cross-half + collision all caught.
  {
    const int up_good = verify_faults(dev8, upper, [&](float* out) {
      render_device_output(out, kFrames, dev8, upper, kSr);
    });
    const int up_swap = verify_faults(dev8, upper, [&](float* out) {
      render_device_output(out, kFrames, dev8, swapL, kSr);
    });
    const int up_half = verify_faults(dev8, upper, [&](float* out) {
      render_device_output(out, kFrames, dev8, canon, kSr);
    });
    const int up_coll = verify_faults(dev8, upper, [&](float* out) {
      render_device_output(out, kFrames, dev8, collide, kSr);
    });
    std::printf("  upper-face(4-7) canonical=%d vs swap=%d cross-half=%d collision=%d\n",
                up_good, up_swap, up_half, up_coll);
    CHECK(up_good == 0);
    CHECK(up_swap > 0);
    CHECK(up_half > 0);
    CHECK(up_coll > 0);
  }

  // A layout-aware checker must not alarm on a clean, correctly-layouted buffer:
  // the two GOOD cases above already confirm that, and the lower-half canonical is
  // the default wiring on a 4-out device = plain music, no faults. Re-assert once.
  CHECK(verify_faults(interleaved, canon, [&](float* out) {
          render_device_output(out, kFrames, interleaved, canon, kSr);
        }) == 0);

  return ::test::finish(suite);
}
}  // namespace

int main() {
  std::printf("== P3-⑥ Debt 1: real output layout, per-channel four-logical ==\n");
  return run("device_layout");
}
