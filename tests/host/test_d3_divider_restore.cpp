// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_d3_divider_restore.cpp — task #98 (GH #15 D3 divider) REAL-RESTORE directional acceptance.
//
// This is the @Codex dd57c783 gap-fill: the live-lane d3 divider acceptance
// (d3_div_actual_timing_acceptance in tests/core/test_machine_runtime.cpp) drives rate + divider
// through a controlled SynthRuntime fixture (makeRuntime + enqueueControlEvent + processBlock).
// That entry is real for the live *ControlEvent* lane, but it does NOT exercise the
// codec -> owner -> StandaloneAudioEngine restore path, and its capture frames are chunk-boundary
// aligned (approximate). This file supplies exactly the two missing points:
//
//   1. REAL RESTORE ENTRY — the divider's RATE + DIVIDER are carried by a legal DeviceState and
//      restored by encode -> decode -> StandaloneAudioEngine::applyDeviceState (the SAME single
//      stopped-stream apply boundary the host uses), through the shared tests/host/test_engine_harness.h.
//      Only then are the real S&H lanes rendered, so "the restored RATE/DIVIDER actually drives the
//      S&H" is asserted on the product entry, not on a hand-built fixture.
//   2. FULL CAPTURE SEQUENCE — the S&H is observed at the harness's PER-SAMPLE seam (renderSampled:
//      one processBlock(1) per frame + an onSample hook after each), so each capture is recorded at
//      its ABSOLUTE frame with its held value — not an approximate chunk-tail frame. The same
//      per-sample observation is then used to compare (a) restoring the SAME state twice and
//      (b) ONE renderSampled(all) vs TWO renderSampled(half)+renderSampled(half), so a bug that only
//      mis-samples mid-capture (invisible to a final-getter comparison) REDS here.
//
// The divider RATIO is measured against an INDEPENDENT reference: round(win * liveRateHz / sr),
// computed from the LIVE rate getter — never the divider getter — so the count is anchored outside
// the divider, which is what a `+=2.0` accumulator mutation defeats (captures double -> N=16 trips).
// The default N=8.5 is described as a MEAN (intervals alternate integer 8 and 9 edges; never a
// literal "8.5 edges per capture"), matching the current software remainder policy (each capture
// subtracts divN_ 8.5 from a running float accumulator and the 0.5 carries forward). No hardware
// basis is invented here.
//
// The +2.0 negative control lives in tests/mutation/run_d3_div_timing_mutation.sh, which splices a
// detached `lfEdgeAcc_ += 1.0 -> += 2.0` shadow and proves this N=16 assertion turns RED there.
// Intentionally NOT an every-push test: tagged `slow` (probe/acceptance gate, PR/merge time only).

#include "test_engine_harness.h"
#include <lunar24/core/device_state.h>
#include <lunar24/core/state_default.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;
using lunar24::testengine::EngineHarness;
using lunar24::testengine::StandaloneAudioEngine;  // via testengine: host::StandaloneAudioEngine.
using lunar24::testengine::SynthRuntime;
using lunar24::testengine::kInCh;
using lunar24::testengine::kOutCh;
using lunar24::testengine::kSeed;
using lunar24::testengine::kSr;

int g_checks = 0;
int g_fail = 0;
void check(bool cond, const char* label) {
  ++g_checks;
  std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", label);
  if (!cond) ++g_fail;
}

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::size_t>(id)];
}

// One captured schedule over a window, observed per-sample through the real restore entry.
struct Seq {
  std::size_t totalEdges = 0;  // round(win * liveRateHz / sr) — INDEPENDENT reference (rate getter).
  std::size_t caps = 0;        // observed S&H held-value steps over the window.
  std::size_t win = 0;
  double rateHz = 0.0;         // the live LF rate (independent of the divider).
  double divN = 0.0;           // the divider ratio that was restored (readback).
  std::vector<long> cf;        // ABSOLUTE frame of each capture (per-sample, not chunk-tail).
  std::vector<double> h;       // the held S&H CV value at each capture.
};

// Drive the real codec -> owner -> processBlock entry: a legal DeviceState's RATE/DIVIDER is
// restored by applyDeviceState (Accepted), then the window is rendered one frame at a time via the
// harness renderSampled seam; the PUBLISHED S&H CV is sampled after each frame (a held-value step =
// one capture, at its absolute frame). The independent LF-edge reference round(win*rateHz/sr) comes
// from the round-tripped live rate getter, so a `+=2.0` divider mutation (which doubles the capture
// count) breaks the absolute ratio.
Seq measure(const DeviceStateV1& st, bool six, long win) {
  Seq s;
  s.win = static_cast<std::size_t>(win);
  EngineHarness h;
  const bool ok = h.load(st, kSr, 4096, kInCh, kOutCh);
  const SynthRuntime* rt = h.runtime();
  if (!ok || !rt) {
    check(false, "restore: legal DeviceState is Accepted by applyDeviceState (codec round-trip)");
    return s;
  }
  check(ok, "restore: legal DeviceState is Accepted by applyDeviceState (codec round-trip)");
  check(h.applyStatus() == StandaloneAudioEngine::StateApplyStatus::Accepted,
        "restore: applyDeviceState reports Accepted (typed, not bare bool)");
  s.rateHz = six ? rt->drone6RateHz() : rt->drone3RateHz();
  s.divN = six ? rt->drone6Divider() : rt->drone3Divider();
  s.totalEdges = static_cast<std::size_t>(std::round(double(win) * s.rateHz / kSr));
  double pv = six ? rt->sampleHold6Cv() : rt->sampleHold3Cv();
  long fc = 0;
  h.renderSampled(static_cast<int>(win), 0.0, [&](const SynthRuntime& r) {
    const double cv = six ? r.sampleHold6Cv() : r.sampleHold3Cv();
    if (std::fabs(cv - pv) > 1e-9) {  // one real S&H capture = one held-CV step, at absolute frame fc.
      s.cf.push_back(fc);
      s.h.push_back(cv);
      pv = cv;
    }
    ++fc;
  });
  s.caps = s.cf.size();
  return s;
}

constexpr long kWin = 800000;  // ~16.7 s at the legal default 6 Hz -> ~100 LF edges (long window).
constexpr long kSlack = 2;     // boundary/phase tolerance: the analytic reference is within ±1 edge.

// (A) INTEGER divider endpoints (norm 1 -> divN=16, norm 0 -> divN=1) on BOTH drones through the
//     real restore entry: the restored RATE/DIVIDER drives the S&H, and the capture count tracks the
//     independent LF-edge reference divided by divN. A `+=2.0` accumulator mutation doubles the
//     capture rate and breaks this absolute reference — the N=16 line is the discriminator.
void integer_endpoints() {
  for (bool six : {false, true}) {
    const ParameterId rateId = six ? ParameterId::drone_6_rate : ParameterId::drone_3_rate;
    const ParameterId divId = six ? ParameterId::drone_6_divider : ParameterId::drone_3_divider;
    const char* who = six ? "drone6" : "drone3";

    DeviceStateV1 st = make_default_device_state(kSeed);
    slot(st, rateId) = 0.5;  // LEGAL rate norm -> 6 Hz (never the old illegal 60).
    slot(st, divId) = 1.0;   // norm=1 -> divN=16.
    Seq s = measure(st, six, kWin);
    check(std::fabs(s.rateHz - 6.0) < 1e-9, "restore: RATE restored to the legal default 6 Hz");
    check(s.divN == 16.0, "restore: divider norm 1 -> exactly divN=16");
    check(std::fabs(double(s.caps) - double(s.totalEdges) / 16.0) <= kSlack,
          "d3 restore N=16: real captures == LF-edges/16 (integer division)");
    check(s.caps > 0, "restore: restored drone S&H is actually driven (captures observed)");

    DeviceStateV1 st1 = make_default_device_state(kSeed);
    slot(st1, rateId) = 0.5;
    slot(st1, divId) = 0.0;  // norm=0 -> divN=1.
    Seq b = measure(st1, six, kWin);
    check(b.divN == 1.0, "restore: divider norm 0 -> exactly divN=1");
    check(std::fabs(double(b.caps) - double(b.totalEdges) / 1.0) <= kSlack,
          "d3 restore N=1: real captures == LF-edges (capture every edge)");
    std::printf("  (restore %s N16: %zu captures / %zu edges; N1: %zu captures)\n", who,
                s.caps, s.totalEdges, b.caps);
  }
}

// (B) DEFAULT N=8.5 (live product default, norm 0.5) on BOTH drones, restored through the real
//     entry: the ratio is a MEAN — each per-capture interval is an integer LF-edge span that
//     ALTERNATES 8 / 9 (never a literal 8.5 edges per capture), so sum(intervals)/count is the 8.5
//     average. This documents the CURRENT software remainder policy (0.5 carries forward between
//     captures, yielding the 8/9 alternation); no hardware basis is invented.
void default_mean() {
  for (bool six : {false, true}) {
    const ParameterId rateId = six ? ParameterId::drone_6_rate : ParameterId::drone_3_rate;
    const ParameterId divId = six ? ParameterId::drone_6_divider : ParameterId::drone_3_divider;
    const char* who = six ? "drone6" : "drone3";
    DeviceStateV1 st = make_default_device_state(kSeed);
    slot(st, rateId) = 0.5;
    slot(st, divId) = 0.5;  // default norm 0.5 -> divN=8.5.
    Seq t = measure(st, six, kWin);
    check(t.divN == 8.5, "restore: default norm 0.5 -> exactly divN=8.5");
    check(std::fabs(double(t.caps) - double(t.totalEdges) / 8.5) <= kSlack,
          "d3 restore N=8.5: real captures == LF-edges/8.5 (mean division ratio)");

    // Per-interval edge count (from ABSOLUTE capture frames) must be integer 8 or 9 and alternate
    // strictly, mean 8.5 — asserted on the full sequence, not a final getter.
    std::size_t n8 = 0, n9 = 0, intervals = 0;
    double sumEdges = 0.0;
    bool alt = true;
    long prev = 0;
    for (std::size_t i = 1; i < t.cf.size(); ++i) {
      const double e = double(t.cf[i] - t.cf[i - 1]) * t.rateHz / kSr;
      const long ei = static_cast<long>(std::round(e));
      ++intervals;
      sumEdges += e;
      if (ei == 8) ++n8;
      else if (ei == 9) ++n9;
      if (i > 1 && ei == prev) alt = false;
      prev = ei;
    }
    const double mean = intervals ? sumEdges / double(intervals) : 0.0;
    check(intervals > 0, "restore: enough capture intervals to measure the mean ratio");
    check(n8 > 0 && n9 > 0, "restore: intervals are integer 8 AND 9 edge spans");
    check(alt, "restore: 8/9 intervals strictly alternate (order checked on the real sequence)");
    check(std::fabs(mean - 8.5) <= 0.5, "restore: mean interval is 8.5 edges");
    std::printf("  (restore %s N8.5: %zu captures, mean %.3f, %zu×8 + %zu×9, alternate=%s)\n",
                who, t.caps, mean, n8, n9, alt ? "yes" : "no");
  }
}

// (C) ASYMMETRIC two lanes, no cross-talk: ONE program (restored through the real entry) with
//     drone3 divider=16 (N=16) and drone6 divider=1 (N=1) live together. Each PapaVoice owns an
//     independent LF / divider / noise / S&H, so drone6 (capture every LF edge) yields ~16x the
//     capture count of drone3 (every 16th edge); a cross-voice clock/divider leak collapses them.
void asymmetric() {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::drone_3_rate) = 0.5; slot(st, ParameterId::drone_3_divider) = 1.0;  // N=16.
  slot(st, ParameterId::drone_6_rate) = 0.5; slot(st, ParameterId::drone_6_divider) = 0.0;  // N=1.
  EngineHarness h;
  h.load(st, kSr, 4096, kInCh, kOutCh);
  const SynthRuntime* rt = h.runtime();
  const double r3 = rt->drone3RateHz();
  const double r6 = rt->drone6RateHz();
  double p3 = rt->sampleHold3Cv(), p6 = rt->sampleHold6Cv();
  long c3 = 0, c6 = 0, fc = 0;
  h.renderSampled(static_cast<int>(kWin), 0.0, [&](const SynthRuntime& r) {
    const double v3 = r.sampleHold3Cv();
    if (std::fabs(v3 - p3) > 1e-9) { ++c3; p3 = v3; }
    const double v6 = r.sampleHold6Cv();
    if (std::fabs(v6 - p6) > 1e-9) { ++c6; p6 = v6; }
    ++fc;
  });
  const double edges3 = double(kWin) * r3 / kSr;
  const double edges6 = double(kWin) * r6 / kSr;
  check(std::fabs(double(c3) - edges3 / 16.0) <= kSlack,
        "d3 restore asymmetric: drone3 captures == LF-edges/16");
  check(std::fabs(double(c6) - edges6 / 1.0) <= kSlack,
        "d3 restore asymmetric: drone6 captures == LF-edges (capture every edge)");
  check(double(c6) > 8.0 * double(c3),
        "d3 restore asymmetric: two lanes do not cross-talk (drone6 count != drone3 count)");
  std::printf("  (restore asymmetric: drone3 %ld / %ld, drone6 %ld / %ld)\n", c3, (long)std::lround(edges3), c6, (long)std::lround(edges6));
}

// (D) RESTORE DETERMINISM + BLOCK SPLIT, at per-sample precision:
//     (a) restoring the SAME legal state into TWO freshly-loaded owners must reproduce the SAME
//         capture-frame / held-value sequence (real restore is deterministic — the codec round-trip
//         and applyDeviceState are single-valued);
//     (b) rendering the SAME window as ONE renderSampled(all) vs TWO renderSampled(half)+
//         renderSampled(half) must give the SAME per-sample capture sequence — a bug that only
//         mis-samples mid-capture (invisible to a final-getter comparison) trips here.
void restore_and_block_split() {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::drone_3_rate) = 0.5;
  slot(st, ParameterId::drone_3_divider) = 0.5;  // N=8.5 keeps several captures in the window.
  Seq x = measure(st, false, kWin);
  Seq y = measure(st, false, kWin);
  check(x.caps == y.caps && x.cf == y.cf, "restore determinism: same state twice -> same capture frames");
  check(x.h == y.h, "restore determinism: same state twice -> same held-value sequence");

  // One-shot vs split-into-two renderSampled calls; both use the SAME per-sample seam.
  Seq split;
  split.win = static_cast<std::size_t>(kWin);
  {
    EngineHarness h;
    h.load(st, kSr, 4096, kInCh, kOutCh);
    const SynthRuntime* rt = h.runtime();
    split.rateHz = rt->drone3RateHz();
    split.divN = rt->drone3Divider();
    split.totalEdges = static_cast<std::size_t>(std::round(double(kWin) * split.rateHz / kSr));
    double pv = rt->sampleHold3Cv();
    long fc = 0;
    auto obs = [&](const SynthRuntime& r) {
      const double cv = r.sampleHold3Cv();
      if (std::fabs(cv - pv) > 1e-9) { split.cf.push_back(fc); split.h.push_back(cv); pv = cv; }
      ++fc;
    };
    // Two render calls, each driving the same divider state forward (no reset between them).
    h.renderSampled(static_cast<int>(kWin / 2), 0.0, obs);
    h.renderSampled(static_cast<int>(kWin - kWin / 2), 0.0, obs);
    split.caps = split.cf.size();
  }
  check(split.caps == x.caps && split.cf == x.cf,
        "d3 restore block-split: one render(all) vs two render(half) -> same per-sample capture frames");
  check(split.h == x.h, "d3 restore block-split: same held-value sequence across the split");
}

// (E) AUDIO block-partition bit-identity on the same restored state: rendering a window as one
//     processBlock vs per-frame processBlock(1) gives bit-identical WET outputs (the owner delegate
//     is frame-indexed, not chunk-relative) — the harness's native block-partition check.
void audio_partition() {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::drone_3_rate) = 0.5;
  slot(st, ParameterId::drone_3_divider) = 0.5;
  const int B = 4096;  // <= the blockFrames the engine was loaded with.
  EngineHarness h1;
  h1.load(st, kSr, B, kInCh, kOutCh);
  h1.renderSampled(B, 0.0, [](const SynthRuntime&) {});
  EngineHarness h2;
  h2.load(st, kSr, B, kInCh, kOutCh);
  h2.renderBlock(B, [](std::size_t, double& i0, double& i1) { i0 = 0.0; i1 = 0.0; });
  check(h1.wetL().size() == static_cast<std::size_t>(B) && h1.wetL() == h2.wetL(),
        "d3 restore audio block-partition: per-frame vs one-block -> bit-identical WET_L");
  check(h1.wetR() == h2.wetR(), "d3 restore audio block-partition: bit-identical WET_R");
}

}  // namespace

int main() {
  std::printf("D3 divider: REAL codec->owner->processBlock restore acceptance (per-sample capture sequence).\n");
  integer_endpoints();
  default_mean();
  asymmetric();
  restore_and_block_split();
  audio_partition();
  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
