// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Real-time safety of live cable edits: plugging, replacing and unplugging a cable while the
// app plays compiles the new patch plan on the UI thread, so the audio callback that applies
// the edit neither allocates nor frees, and the edit still takes effect exactly as a full
// rebuild would.
#include "mini_test.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <host/standalone_audio_engine.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>

extern std::size_t g_newCount;
extern std::size_t g_deleteCount;

namespace {
using lunar24::core::JackId;
using lunar24::host::StandaloneAudioEngine;

double l[256], r[256];
double* outs[] = {l, r};

// Allocations + frees made by one audio block.
std::size_t heapOpsInOneBlock(StandaloneAudioEngine& e) {
  const std::size_t n0 = g_newCount, d0 = g_deleteCount;
  e.processBlock(nullptr, outs, 0, 2, 256);
  return (g_newCount - n0) + (g_deleteCount - d0);
}

// The compiled plan as module order per region: what decides how the patch runs.
std::vector<std::vector<unsigned>> planShape(const StandaloneAudioEngine& e) {
  std::vector<std::vector<unsigned>> shape;
  for (const auto& region : e.runtime()->graph().regions) {
    shape.emplace_back();
    for (auto id : region.modules) shape.back().push_back(static_cast<unsigned>(id));
  }
  return shape;
}
}  // namespace

int main() {
  const JackId lfoA = JackId::lfo_a_cv_out, lfoB = JackId::lfo_b_cv_out;
  const JackId cvMod = JackId::drone_1_cv_mod_in, vcfCv = JackId::vcf_cv_l_in;
  auto e = std::make_unique<StandaloneAudioEngine>();
  CHECK(e->prepare(1, 48000.0, 256, 0, 2));
  for (int i = 0; i < 20; ++i) e->processBlock(nullptr, outs, 0, 2, 256);
  CHECK_EQ(heapOpsInOneBlock(*e), 0u);  // steady state

  // Plug: the block applying it does no heap work, and the cable is live in a valid plan.
  CHECK(e->postConnect(lfoA, cvMod));
  CHECK_EQ(heapOpsInOneBlock(*e), 0u);
  CHECK(e->runtime()->cableConnected(lfoA, cvMod));
  CHECK(e->runtime()->graphValid());
  CHECK(e->runtime()->lastRebuildStatus() == lunar24::core::SynthRuntime::RebuildStatus::ok);

  // Replace the cable at the same input, and several edits inside one block.
  CHECK(e->postConnect(lfoB, cvMod));
  CHECK_EQ(heapOpsInOneBlock(*e), 0u);
  CHECK(e->runtime()->cableConnected(lfoB, cvMod));
  CHECK(!e->runtime()->cableConnected(lfoA, cvMod));
  CHECK(e->postConnect(lfoA, vcfCv));
  CHECK(e->postDisconnect(cvMod));
  CHECK(e->postConnect(lfoB, cvMod));
  CHECK_EQ(heapOpsInOneBlock(*e), 0u);
  CHECK(e->runtime()->graphValid());

  // The live plan is the one a full rebuild of the same patch gives.
  {
    lunar24::core::DeviceStateV1 st = lunar24::core::make_default_device_state(1);
    CHECK(lunar24::core::state_connect(st, lfoA, vcfCv));
    CHECK(lunar24::core::state_connect(st, lfoB, cvMod));
    auto fresh = std::make_unique<StandaloneAudioEngine>();
    CHECK(fresh->prepare(1, 48000.0, 256, 0, 2));
    CHECK(fresh->applyDeviceState(st, 48000.0, 256, 0, 2) ==
          StandaloneAudioEngine::StateApplyStatus::Accepted);
    CHECK(planShape(*e) == planShape(*fresh));
    CHECK_EQ(e->runtime()->execSlotCount(), fresh->runtime()->execSlotCount());
  }

  // Unplug everything; the used plans come back to the UI thread and are freed there.
  CHECK(e->postDisconnect(cvMod));
  CHECK(e->postDisconnect(vcfCv));
  CHECK_EQ(heapOpsInOneBlock(*e), 0u);
  CHECK(!e->runtime()->cableConnected(lfoB, cvMod));
  const std::size_t d0 = g_deleteCount;
  (void)e->syncParametersFromAudioThread();
  CHECK(g_deleteCount > d0);
  return test::finish("test_live_cable_rt");
}
