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
    applyStatus_ = engine_.applyDeviceState(decoded, sr, blockFrames, inCh, outCh);
    return applyStatus_ == StandaloneAudioEngine::StateApplyStatus::Accepted;
  }

  // The TYPED outcome of the last load() (task#80 item: a rejected candidate must be distinguished
  // as RejectedGraph vs RejectedInvalidState vs RejectedFormat, never collapsed into a bare false).
  // Valid only after load(); NotAttempted before any load().
  StandaloneAudioEngine::StateApplyStatus applyStatus() const { return applyStatus_; }

  // The validation detail the engine recorded on the last apply(): a RejectedGraph outcome carries
  // ok==true here (the state VALIDATED; only the graph failed), which is exactly what distinguishes
  // a graph rejection from a state/format rejection on the agreed entry.
  const lunar24::core::StateValidationResult& validation() const {
    return engine_.lastStateValidation();
  }

  // Render `frames` frames, appending the four real output channels. `preampV` feeds physical
  // input ch1 (the preamp feed under the >=2 input route). Returns false if any processBlock is
  // not Rendered.
  bool render(int frames, double preampV = 0.0) {
    return renderFeed(frames, [preampV](std::size_t, double& in0, double& in1) {
      in0 = 0.0; in1 = preampV;
    });
  }

  // Render `frames` frames with a per-frame input feed. `feed(frame, &in0, &in1)` supplies each
  // frame's planar inputs — this is how an AC stimulus (e.g. a zero-centred sine on the preamp
  // physical ch1) reaches the input stage, which the constant-`preampV` render() cannot. Returns
  // false if any processBlock is not Rendered.
  template <class Feed>
  bool renderFeed(int frames, Feed&& feed) {
    return renderFeedSampled(frames, std::forward<Feed>(feed), [](const SynthRuntime&) {});
  }

  // Render `frames` frames and run `onSample(runtime)` after each one. This is how a control family
  // samples the PUBLISHED CV/gate jack (controlVoltageAt) at every step without re-committing.
  template <class Fn>
  bool renderSampled(int frames, double preampV, Fn&& onSample) {
    return renderFeedSampled(frames, [preampV](std::size_t, double& in0, double& in1) {
      in0 = 0.0; in1 = preampV;
    }, std::forward<Fn>(onSample));
  }

  // Reserve capacity for the four captured output vectors. The CPU-cost measurement must not time
  // render-loop vector growth (realloc on push_back), so callers pre-reserve the full window before
  // the timed region (BLOCK item ⑤). No-op-safe: harmless if never called.
  void reserve(std::size_t n) {
    wetL_.reserve(n); wetR_.reserve(n); dryA_.reserve(n); dryB_.reserve(n);
  }

  // Render `frames` frames in ONE processBlock(...) call — the real-block path, distinguished from
  // the per-frame render() above. Used to compare the four outputs under the same state + input for
  // BLOCK-PARTITION invariance. `feed(frame, &in0, &in1)` fills the planar input buffers.
  // NOTE: requires frames <= the blockFrames the engine was loaded with, else processBlock drops to
  // silence (returns false). Appends all four outputs.
  template <class Feed>
  bool renderBlock(int frames, Feed&& feed) {
    std::vector<double> in0(frames, 0.0), in1(frames, 0.0);
    for (int f = 0; f < frames; ++f) {
      feed(static_cast<std::size_t>(f), in0[static_cast<std::size_t>(f)],
           in1[static_cast<std::size_t>(f)]);
    }
    const double* in[kInCh] = {in0.data(), in1.data()};
    std::vector<double> o0(frames), o1(frames), o2(frames), o3(frames);
    double* out[kOutCh] = {o0.data(), o1.data(), o2.data(), o3.data()};
    const auto s = engine_.processBlock(in, out, kInCh, kOutCh, frames);
    if (s != StandaloneAudioEngine::Status::Rendered) return false;
    wetL_.insert(wetL_.end(), o0.begin(), o0.end());
    wetR_.insert(wetR_.end(), o1.begin(), o1.end());
    dryA_.insert(dryA_.end(), o2.begin(), o2.end());
    dryB_.insert(dryB_.end(), o3.begin(), o3.end());
    return true;
  }

  // Pure engine processBlock on CALLER-OWNED planar buffers: NO allocation, NO input generation and NO
  // captured-output insert inside this call (BLOCK item ③). The caller supplies already-sized `in[*]` /
  // `out[*]` so a CPU-cost measurement can time ONLY the DSP loop — the harness wrapper (renderBlock /
  // render) allocates six vectors, fills the inputs and inserts the outputs, which must be OUTSIDE the
  // timed region. Returns the engine Status. `frames` must be <= the blockFrames the engine was loaded
  // with; the engine must have been loaded with the same kInCh/kOutCh this call uses.
  StandaloneAudioEngine::Status processPure(const double* const* in, double* const* out, int frames) {
    return engine_.processBlock(in, out, kInCh, kOutCh, frames);
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
  // Per-frame render loop with a caller-supplied input feed and an optional per-frame sample hook.
  template <class Feed, class Fn>
  bool renderFeedSampled(int frames, Feed&& feed, Fn&& onSample) {
    for (int f = 0; f < frames; ++f) {
      double in0 = 0.0, in1 = 0.0;
      feed(static_cast<std::size_t>(f), in0, in1);
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
  StandaloneAudioEngine::StateApplyStatus applyStatus_ =
      StandaloneAudioEngine::StateApplyStatus::NotAttempted;
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
