// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Replacing the whole machine while audio runs (a DAW loading a project, RESET PANEL): the
// audio thread fades out, sits silent behind its closed AudioScope while the UI thread installs
// the new machine, then fades back in. It never allocates, never sees a half-installed machine
// and never outputs a non-finite sample.
#include "mini_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <thread>

#include <host/standalone_audio_engine.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>

void countHeapOpsOnThisThread(bool on);
extern std::atomic<std::size_t> g_countedHeapOps;

namespace {
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::host::StandaloneAudioEngine;
using Status = StandaloneAudioEngine::StateApplyStatus;
constexpr int kBlock = 256;

DeviceStateV1 stateWithMaster(double master) {
  DeviceStateV1 st = lunar24::core::make_default_device_state(1);
  (void)lunar24::core::state_set_param(st, ParameterId::effector_master, master);
  return st;
}

double param(const StandaloneAudioEngine& e, ParameterId id) {
  return e.canonicalState()->parameters[static_cast<std::size_t>(id)];
}

// What the audio thread saw. Written by the audio thread, read after it is joined.
struct AudioStats {
  std::size_t rendered = 0;
  std::size_t closed = 0;          // callbacks that found the scope closed
  std::size_t nonFinite = 0;
  std::size_t nestedClosed = 0;    // a nested scope disagreeing with its open outer one
  double peak = 0.0;
  double lastBeforePark = 0.0;     // the louder end of any block right before a closed one
  double firstAfterResume = 0.0;   // the louder start of any block right after a closed one
};

// A host audio callback: MIDI-like work and a block, all inside one AudioScope.
void audioLoop(StandaloneAudioEngine& e, std::atomic<bool>& stop, AudioStats& st) {
  countHeapOpsOnThisThread(true);
  double l[kBlock], r[kBlock];
  double* outs[] = {l, r};
  bool prevOpen = true;
  double prevEnd = 0.0;
  while (!stop.load(std::memory_order_acquire)) {
    {
      const StandaloneAudioEngine::AudioScope scope(e);
      if (scope) {
        const StandaloneAudioEngine::AudioScope nested(e);  // MIDI handling inside the callback
        if (!nested) ++st.nestedClosed;
        if (st.rendered % 7 == 0) (void)e.parameterFromAudioThread(ParameterId::effector_blend, 0.5);
        e.processBlock(nullptr, outs, 0, 2, kBlock);
        for (int i = 0; i < kBlock; ++i) {
          if (!std::isfinite(l[i]) || !std::isfinite(r[i])) ++st.nonFinite;
          st.peak = std::max({st.peak, std::fabs(l[i]), std::fabs(r[i])});
        }
        if (!prevOpen) st.firstAfterResume = std::max({st.firstAfterResume, std::fabs(l[0]), std::fabs(r[0])});
        prevEnd = std::max(std::fabs(l[kBlock - 1]), std::fabs(r[kBlock - 1]));
        prevOpen = true;
        ++st.rendered;
      } else {
        if (prevOpen) st.lastBeforePark = std::max(st.lastBeforePark, prevEnd);
        prevOpen = false;
        ++st.closed;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  countHeapOpsOnThisThread(false);
}
}  // namespace

int main() {
  // No audio thread running (a host that is not processing): the swap does not wait for a fade
  // that never comes, and installs the state.
  {
    auto e = std::make_unique<StandaloneAudioEngine>();
    CHECK(e->prepare(1, 48000.0, kBlock, 0, 2));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(e->swapDeviceState(stateWithMaster(0.3)) == Status::Accepted);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400));
    CHECK(param(*e, ParameterId::effector_master) == 0.3);
    CHECK_EQ(e->swapCount(), 1u);
    const StandaloneAudioEngine::AudioScope scope(*e);  // audio may run again
    CHECK(static_cast<bool>(scope));
  }
  // Not prepared: nothing to swap into, so the format is rejected and nothing is counted.
  {
    auto e = std::make_unique<StandaloneAudioEngine>();
    CHECK(e->swapDeviceState(stateWithMaster(0.3)) == Status::RejectedFormat);
    CHECK_EQ(e->swapCount(), 0u);
    CHECK(!e->isReady());
  }
  // A rejected state keeps the current machine, and audio resumes.
  {
    auto e = std::make_unique<StandaloneAudioEngine>();
    CHECK(e->prepare(1, 48000.0, kBlock, 0, 2));
    DeviceStateV1 bad = stateWithMaster(0.3);
    bad.parameters[static_cast<std::size_t>(ParameterId::effector_master)] = std::nan("");
    CHECK(e->swapDeviceState(bad) != Status::Accepted);
    CHECK_EQ(e->swapCount(), 0u);
    CHECK(e->isReady());
    const StandaloneAudioEngine::AudioScope scope(*e);
    CHECK(static_cast<bool>(scope));
    double l[kBlock], r[kBlock];
    double* outs[] = {l, r};
    CHECK(e->processBlock(nullptr, outs, 0, 2, kBlock) == StandaloneAudioEngine::Status::Rendered);
  }
  // Swaps while an audio thread keeps playing.
  {
    auto e = std::make_unique<StandaloneAudioEngine>();
    CHECK(e->prepare(1, 48000.0, kBlock, 0, 2));
    double l[kBlock], r[kBlock];
    double* outs[] = {l, r};
    for (int i = 0; i < 20; ++i) e->processBlock(nullptr, outs, 0, 2, kBlock);  // warm up
    const DeviceStateV1 a = stateWithMaster(0.3), b = stateWithMaster(0.7);
    std::atomic<bool> stop{false};
    AudioStats st;
    g_countedHeapOps.store(0);
    std::thread audio(audioLoop, std::ref(*e), std::ref(stop), std::ref(st));
    constexpr int kSwaps = 24;
    int accepted = 0;
    for (int i = 0; i < kSwaps; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      (void)e->syncParametersFromAudioThread();
      (void)e->postParameter(ParameterId::vco_a_tune, 0.01 * (i % 5));
      if (e->swapDeviceState(i % 2 ? b : a) == Status::Accepted) ++accepted;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true, std::memory_order_release);
    audio.join();

    CHECK_EQ(accepted, kSwaps);
    CHECK_EQ(e->swapCount(), static_cast<std::uint64_t>(kSwaps));
    CHECK(param(*e, ParameterId::effector_master) == 0.7);  // the last swap won
    CHECK_EQ(g_countedHeapOps.load(), 0u);                  // the audio thread never allocated
    CHECK_EQ(st.nonFinite, 0u);
    CHECK_EQ(st.nestedClosed, 0u);
    CHECK(st.rendered > 0);
    CHECK(st.closed > 0);              // the audio thread really met a swap in progress
    CHECK(st.peak > 1e-4);             // the machine was sounding
    CHECK(st.lastBeforePark < 1e-12);  // faded to silence before going quiet
    CHECK(st.firstAfterResume < 0.01); // and fades back in
  }
  return test::finish("state_swap");
}
