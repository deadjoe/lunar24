// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_engine_harness.h — the ONE shared entry for the task#78 host-family behavior harnesses.
//
// @Codex 1e34b7bb requires the gap#2/gap#3 harnesses to actually execute the agreed entry instead of
// calling buildMachineRuntimeCandidate/processFrame directly:
//     encode_device_state -> decode_device_state -> StandaloneAudioEngine::applyDeviceState ->
//     StandaloneAudioEngine::processBlock,
// and to observe (a) the PUBLISHED control voltage/gate through owner.runtime() (read-only) and
// (b) the REAL output buffers the DeviceAdapter writes. This header encapsulates exactly that path
// so no test can silently drift back onto the disallowed direct-builder route: the codec roundtrip,
// the engine apply, the render loop and the buffer capture all live here and only here.
//
// The engine is the framework-free StandaloneAudioEngine (host/standalone_audio_engine.h), the same
// production path the host uses. `render()` drives the whole chain per frame and captures the four
// real output channels (WET_L=0, WET_R=1, DRY_A=2, DRY_B=3 from device_layout.h); the caller asserts
// on those captured buffers (audio families) and/or reads owner.runtime()->controlVoltageAt(JackId)
// between frame steps (control families). NEVER call load() a second time on a harness whose
// runtime() pointer you still hold — runtime() is valid only until the next commit (= the next load).

#pragma once

#include <host/standalone_audio_engine.h>
#include <lunar24/core/device_layout.h>       // WET_L / WET_R / DRY_A / DRY_B, kDeviceStorageSchema
#include <lunar24/core/state_default.h>       // make_default_device_state
#include <lunar24/core/state_serializer.h>    // encode/decode_device_state

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lunar24::testengine {

using lunar24::core::DeviceStateV1;
using lunar24::core::SynthRuntime;
using lunar24::core::kDeviceStorageSchema;
using lunar24::host::StandaloneAudioEngine;

// Deterministic, repeatable inputs (fixed seed / sample rate / block); identical on every run.
inline constexpr std::uint64_t kSeed = 0x4C554E4152ULL;
inline constexpr double kSr = 48000.0;
inline constexpr std::size_t kWire = kDeviceStorageSchema.totalBytesHint;

// The format the harness prepares and renders: >=2 physical/in (preamp feed on ch1), 4 physical/out
// (WET L/R + DRY A/B). Kept uniform so the processBlock format gate never fires mid-test.
inline constexpr int kInCh = 2;
inline constexpr int kOutCh = 4;

// The ONE real entry. The owner forwards the live codec -> engine -> processBlock chain.
class EngineHarness {
 public:
  EngineHarness() = default;

  // Load a validated state through the codec roundtrip and prepare the engine for render.
  // Returns true only on StateApplyStatus::Accepted. After a successful load, runtime() points at
  // the committed definition and remains valid across render() calls (no commit happens in render).
  bool load(const DeviceStateV1& state, double sr = kSr, int blockFrames = 4096,
            int inCh = kInCh, int outCh = kOutCh) {
    std::vector<std::uint8_t> wire(kWire, 0);
    std::size_t written = 0;
    if (!encode_device_state(state, wire.data(), wire.size(), &written)) return false;
    DeviceStateV1 decoded;
    if (!decode_device_state(wire.data(), wire.size(), &decoded)) return false;
    return engine_.applyDeviceState(decoded, sr, blockFrames, inCh, outCh)
        == StandaloneAudioEngine::StateApplyStatus::Accepted;
  }

  // Render `frames` frames, appending the four real output channels. `preampV` feeds physical
  // input ch1 (the preamp feed under the >=2 input route). Returns false if any processBlock is
  // not Rendered.
  bool render(int frames, double preampV = 0.0) {
    return renderImpl(frames, preampV, [](const SynthRuntime&) {});
  }

  // Render `frames` frames and run `onSample(runtime)` after each one. This is how a control family
  // samples the PUBLISHED CV/gate jack (controlVoltageAt) at every step without re-committing.
  template <class Fn>
  bool renderSampled(int frames, double preampV, Fn&& onSample) {
    return renderImpl(frames, preampV, std::forward<Fn>(onSample));
  }

  // Read-only published state (valid only until the next load()). Never null after a successful load.
  const SynthRuntime* runtime() const { return engine_.runtime(); }

  // The real captured output channels (indexed by device_layout.h WET_L/WET_R/DRY_A/DRY_B).
  const std::vector<double>& out(int channel) const {
    switch (channel) {
      case 0: return wetL_;
      case 1: return wetR_;
      case 2: return dryA_;
      default: return dryB_;
    }
  }
  const std::vector<double>& wetL() const { return wetL_; }
  const std::vector<double>& wetR() const { return wetR_; }
  const std::vector<double>& dryA() const { return dryA_; }
  const std::vector<double>& dryB() const { return dryB_; }

 private:
  template <class Fn>
  bool renderImpl(int frames, double preampV, Fn&& onSample) {
    for (int f = 0; f < frames; ++f) {
      double in0 = 0.0, in1 = preampV;
      const double* in[kInCh] = {&in0, &in1};
      double o0 = 0.0, o1 = 0.0, o2 = 0.0, o3 = 0.0;
      double* out[kOutCh] = {&o0, &o1, &o2, &o3};
      const auto s = engine_.processBlock(in, out, kInCh, kOutCh, 1);
      if (s != StandaloneAudioEngine::Status::Rendered) return false;
      wetL_.push_back(o0);
      wetR_.push_back(o1);
      dryA_.push_back(o2);
      dryB_.push_back(o3);
      onSample(*engine_.runtime());
    }
    return true;
  }

  StandaloneAudioEngine engine_;
  std::vector<double> wetL_, wetR_, dryA_, dryB_;
};

// Frequency of a captured channel as the number of zero crossings (sign changes). A useful
// discriminator for audio-rate families (VCO, VCF sweep) where the peak is scale-clamped at ±0.5.
inline int zcrOf(const std::vector<double>& c) {
  int z = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  }
  return z;
}

// Peak magnitude of a captured channel. Discriminates LEVEL for mixer / preamp / gain families.
inline double peakOf(const std::vector<double>& c) {
  double m = 0.0;
  for (double x : c) {
    const double a = std::fabs(x);
    if (a > m) m = a;
  }
  return m;
}

}  // namespace lunar24::testengine
