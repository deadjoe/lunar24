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
//      per-sample observation is then used to compare (a) restoring the SAME state twice and (b) ONE
//      renderSampled(all) vs TWO renderSampled calls over the same window. (b) is a RENDER-CALL
//      BOUNDARY invariance check — the divider state must not reset between render calls. It is NOT a
//      block-size check: renderSampled is processBlock(1) on both sides (@Codex e40bdb0b).
//   3. BLOCK-SIZE INVARIANCE (@Codex e40bdb0b) — a SECOND same-state owner is driven through the
//      harness's real-block renderBlock path with uniform 64 / 256 / 4096 and irregular partitions;
//      after EVERY block the published S&H CV of BOTH lanes must equal this file's per-sample
//      reference trajectory at that block's last absolute frame, with boundaries landing exactly one
//      frame before and one frame after every reference capture. The audio block-partition check (E)
//      cannot cover this: the S&H CV is never summed into WET.
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

#include <algorithm>
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
  double initH = 0.0;          // held CV BEFORE frame 0 (the piecewise-constant reference's 1st piece).
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
  s.initH = pv;  // the reference trajectory's value before frame 0.
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

// (D) RESTORE DETERMINISM + RENDER-CALL BOUNDARY invariance, at per-sample precision:
//     (a) restoring the SAME legal state into TWO freshly-loaded owners must reproduce the SAME
//         capture-frame / held-value sequence (real restore is deterministic — the codec round-trip
//         and applyDeviceState are single-valued);
//     (b) rendering the SAME window as ONE renderSampled(all) vs TWO renderSampled calls must give
//         the SAME per-sample capture sequence — the divider state must carry across a render-call
//         boundary (no reset between calls). NOTE (@Codex e40bdb0b): both sides are processBlock(1),
//         so this is a RENDER-CALL boundary check, NOT a block-size check — the block-size invariance
//         is (F), which drives genuinely different audio block sizes through renderBlock.
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
        "d3 restore render-call boundary: one render(all) vs two render calls -> same capture frames");
  check(split.h == x.h, "d3 restore render-call boundary: same held-value sequence across the calls");
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

// (F) BLOCK-SIZE INVARIANCE on the real owner (@Codex e40bdb0b). The divider/S&H state is
//     frame-indexed, so the held CV PUBLISHED at a block boundary must equal this file's per-sample
//     reference trajectory at that block's LAST absolute frame, for ANY audio partition. The reference
//     is the block=1 trajectory from measure(); the block owner is a SECOND same-state owner driven
//     through the harness's real-block renderBlock path (engine -> adapter -> processFrame(frames)),
//     reading BOTH lanes' sampleHold CV after EVERY actual block. Partitions: uniform 64 / 256 / 4096
//     frames plus two IRREGULAR partitions whose boundaries land EXACTLY one frame before and one
//     frame after every reference capture, so the comparison is sensitive at the capture edge and not
//     only at far block tails or the final value. A regression that resets the divider remainder at a
//     block boundary shifts every capture frame; it is invisible to a final-getter comparison and to
//     the audio-only check (E) (the S&H CV is never summed into WET), but it breaks this.
constexpr long kWinBlock = 400000;  // ~8.3 s at the legal 6 Hz -> ~50 LF edges: several captures.

// The held CV the per-sample reference carries at absolute frame f: piecewise-constant — h[j] from
// capture frame cf[j] until the next capture, initH before the first one.
double heldAt(const Seq& s, long f) {
  double v = s.initH;
  for (std::size_t i = 0; i < s.cf.size() && s.cf[i] <= f; ++i) v = s.h[i];
  return v;
}

// Uniform partition: blocks of `B` frames (last block trimmed to the window).
std::vector<int> uniform_schedule(long W, int B) {
  std::vector<int> s;
  for (long f = 0; f < W;) {
    const int b = static_cast<int>(std::min<long>(B, W - f));
    s.push_back(b);
    f += b;
  }
  return s;
}

// Irregular partition: blocks of `cap` frames, but the last block of each gap is trimmed so an
// END-EXCLUSIVE boundary lands exactly on every frame in `specials`.
std::vector<int> irregular_schedule(long W, std::vector<long> specials, int cap) {
  std::sort(specials.begin(), specials.end());
  std::vector<int> s;
  long f = 0;
  for (long t : specials) {
    if (t <= f || t > W) continue;
    while (f < t) {
      const int b = static_cast<int>(std::min<long>(cap, t - f));
      s.push_back(b);
      f += b;
    }
  }
  while (f < W) {
    const int b = static_cast<int>(std::min<long>(cap, W - f));
    s.push_back(b);
    f += b;
  }
  return s;
}

struct PartitionResult {
  long boundaries = 0;        // blocks driven (one S&H read per block, both lanes).
  long bad3 = 0;              // boundaries where drone3's CV != the per-sample reference.
  long bad6 = 0;              // boundaries where drone6's CV != the per-sample reference.
  double worst3 = 0.0;
  double worst6 = 0.0;
  long firstBad = -1;         // absolute frame of the first mismatch (diagnostic).
  std::vector<double> seen3;  // every observed drone3 CV (vacuity guard: must take >1 value).
};

// Drive a SAME-STATE owner through the real-block path with `sched`; after every block compare the
// published S&H CV of BOTH lanes with the per-sample reference at that block's last absolute frame.
PartitionResult run_partition(const DeviceStateV1& st, const std::vector<int>& sched,
                              const Seq& r3, const Seq& r6) {
  PartitionResult pr;
  EngineHarness h;
  h.load(st, kSr, 4096, kInCh, kOutCh);
  const SynthRuntime* rt = h.runtime();
  long f = 0;
  for (int B : sched) {
    h.renderBlock(B, [](std::size_t, double& i0, double& i1) { i0 = 0.0; i1 = 0.0; });
    f += B;
    const long last = f - 1;  // the absolute frame this block just processed.
    const double v3 = rt->sampleHold3Cv();
    const double v6 = rt->sampleHold6Cv();
    const double e3 = std::fabs(v3 - heldAt(r3, last));
    const double e6 = std::fabs(v6 - heldAt(r6, last));
    if (e3 > 0.0) { ++pr.bad3; if (pr.firstBad < 0) pr.firstBad = last; }
    if (e6 > 0.0) ++pr.bad6;
    if (e3 > pr.worst3) pr.worst3 = e3;
    if (e6 > pr.worst6) pr.worst6 = e6;
    pr.seen3.push_back(v3);
    ++pr.boundaries;
  }
  return pr;
}

void block_size_invariance() {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::drone_3_rate) = 0.5;     // legal 6 Hz on both lanes.
  slot(st, ParameterId::drone_6_rate) = 0.5;
  slot(st, ParameterId::drone_3_divider) = 0.5;  // N=8.5 -> ~6 captures in the window.
  slot(st, ParameterId::drone_6_divider) = 1.0;  // N=16  -> ~3 captures in the window.
  const Seq r3 = measure(st, false, kWinBlock);  // per-sample reference trajectory (block=1).
  const Seq r6 = measure(st, true, kWinBlock);
  check(r3.caps >= 3, "d3 restore block-size: drone3 reference spans multiple captures (not one tail)");
  check(r6.caps >= 3, "d3 restore block-size: drone6 reference spans multiple captures (not one tail)");

  // Boundaries (end-exclusive) at every reference capture frame AND the frame after it, for BOTH
  // lanes — the state just before and just after each capture.
  std::vector<long> specials;
  for (long c : r3.cf) { specials.push_back(c); specials.push_back(c + 1); }
  for (long c : r6.cf) { specials.push_back(c); specials.push_back(c + 1); }
  std::sort(specials.begin(), specials.end());
  specials.erase(std::unique(specials.begin(), specials.end()), specials.end());

  struct P { const char* who; std::vector<int> sched; };
  std::vector<P> parts;
  parts.push_back({"uniform 64", uniform_schedule(kWinBlock, 64)});
  parts.push_back({"uniform 256", uniform_schedule(kWinBlock, 256)});
  parts.push_back({"uniform 4096", uniform_schedule(kWinBlock, 4096)});
  parts.push_back({"irregular cap 64", irregular_schedule(kWinBlock, specials, 64)});
  parts.push_back({"irregular cap 256", irregular_schedule(kWinBlock, specials, 256)});

  for (const P& p : parts) {
    const PartitionResult pr = run_partition(st, p.sched, r3, r6);
    char lbl[320];
    std::snprintf(lbl, sizeof lbl,
                  "d3 restore block-size invariance: %s partition -> S&H CV == per-sample reference at "
                  "every block boundary (drone3)", p.who);
    check(pr.bad3 == 0, lbl);
    std::snprintf(lbl, sizeof lbl,
                  "d3 restore block-size invariance: %s partition -> S&H CV == per-sample reference at "
                  "every block boundary (drone6)", p.who);
    check(pr.bad6 == 0, lbl);
    // Vacuity guard: a constant observed CV would make the equality above trivially true.
    std::vector<double> uniq = pr.seen3;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    std::snprintf(lbl, sizeof lbl,
                  "d3 restore block-size: %s partition observes a STEPPED held CV (>1 value, not a "
                  "constant tail)", p.who);
    check(uniq.size() >= 2, lbl);
    std::printf("  (block-size %-16s boundaries=%ld bad3=%ld bad6=%ld worst=%.3g/%.3g distinct=%zu)\n",
                p.who, pr.boundaries, pr.bad3, pr.bad6, pr.worst3, pr.worst6, uniq.size());
  }

  // Sensitivity guard: the irregular partitions must actually compare the held CV immediately BEFORE
  // and AFTER every reference capture (by construction) — the criterion is not only "block tails".
  for (std::size_t k = 3; k < parts.size(); ++k) {
    std::vector<long> ends;
    long f = 0;
    for (int B : parts[k].sched) { f += B; ends.push_back(f); }
    bool allAdj = true;
    for (long c : r3.cf) {
      if (!std::binary_search(ends.begin(), ends.end(), c) ||
          !std::binary_search(ends.begin(), ends.end(), c + 1)) allAdj = false;
    }
    for (long c : r6.cf) {
      if (!std::binary_search(ends.begin(), ends.end(), c) ||
          !std::binary_search(ends.begin(), ends.end(), c + 1)) allAdj = false;
    }
    char lbl[320];
    std::snprintf(lbl, sizeof lbl,
                  "d3 restore block-size: %s partition has a boundary exactly before AND after every "
                  "capture (both lanes)", parts[k].who);
    check(allAdj, lbl);
  }
}

}  // namespace

int main() {
  std::printf("D3 divider: REAL codec->owner->processBlock restore acceptance (per-sample capture sequence).\n");
  integer_endpoints();
  default_mean();
  asymmetric();
  restore_and_block_split();
  audio_partition();
  block_size_invariance();
  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
