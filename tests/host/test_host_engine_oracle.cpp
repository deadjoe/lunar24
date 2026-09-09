// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_host_engine_oracle.cpp — the strong product oracle for GH#4 8B2 (task#72).
//
// This ONE CTest drives the framework-free StandaloneAudioEngine (the host runtime owner)
// — the SAME type LunarHostPlugin holds BY VALUE and the SAME delegate ProcessBlock calls.
// It does NOT re-implement the ProcessBlock algorithm; it drives the production owner
// through its public `prepare()` / `processBlock()` surface, and the owner forwards the
// block to the task#71 DeviceAdapter::renderBlock. Every render acceptance criterion below
// is exercised on that exact path, so a host that bypasses the owner (or the owner that
// bypasses the adapter) has nowhere to hide.
//
// What is asserted (the @Codex 8B2 mandate, §3, narrowed to the host-owner slice):
//   1. DEFAULT PLAN HONESTY      — prepare maps the REAL channel counts to a frozen route:
//                                  0-in -> Zero, 1-in -> ExtOnly (PREAMP NEVER an implicit
//                                  copy), >=2-in -> Distinct(0,1); 2-3-out -> WET L/R only,
//                                  >=4-out -> WET L/R + DRY A/B. This is the mutation-⑤
//                                  detector (a "1-in silently copied to EXT+PREAMP" makes
//                                  inputCh[1]==0 -> RED).
//   2. REAL 1-in/2-out RENDER    — prepare -> multi-block render; the 2 outputs are NOT
//                                  silence, and the trace follows the applied input (it is a
//                                  function of the runtime, not a fixed sink).
//   3. REAL 4-WAY WET+DRY        — prepare 1-in/4-out; ALL FOUR logicals carry signal, DRY
//                                  A/B are VCO taps (bit-identical across a different EXT),
//                                  and WET L/R respond to the EXT input. This is the DSP
//                                  truth that a host-side frame loop / scale / channel
//                                  reorder / second output bank FAILS.
//   4. SAMPLE-RATE DETERMINISM   — 44.1/48/88.2/96 kHz sampleRate() is the REAL value (not
//                                  a fixed 48000) and same-seed renders are bit-identical.
//   5. BLOCK PARTITIONS          — one 64-frame block == four 16-frame blocks (the owner
//                                  delegate is frame-indexed, not chunk-relative).
//   6. PREPARE ATOMIC FAIL       — NaN/Inf/<=0 rate, illegal block size, <2 outputs, <0
//                                  inputs -> prepare() false, isReady() false, EVERY
//                                  inspectable field cleared (no half-write), and the next
//                                  render is DroppedNotReady with zeroed outputs.
//   7. ALLOCATOR (separate TU)   — the full prepared render path allocates 0 bytes.
//   8. CHURN                     — repeated prepare() (release+re-install of the definition)
//                                  leaves the owner able to render (no dangling pointer),
//                                  and the owner address is stable.

#include "mini_test.h"

#include <host/standalone_audio_engine.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>

// Defined by the isolated allocator TU built into this target.
extern std::size_t g_allocCount;
extern std::size_t g_freeCount;

namespace {

using lunar24::core::BufferLayoutKind;
using lunar24::core::ControlEvent;
using lunar24::core::ControlEventKind;
using lunar24::core::DevicePlan;
using lunar24::core::DRY_A;
using lunar24::core::DRY_B;
using lunar24::core::InputRoute;
using lunar24::core::KeyboardSide;
using lunar24::core::NoteId;
using lunar24::core::ParameterId;
using lunar24::core::SignalSample;
using lunar24::core::SynthRuntime;
using lunar24::core::TimedControlEvent;
using lunar24::core::WET_L;
using lunar24::core::WET_R;
using lunar24::host::StandaloneAudioEngine;
using lunar24::host::kLunarStartupSeed;

using EngineStatus = StandaloneAudioEngine::Status;

constexpr int kF = 64;          // the block size used by most cases.
constexpr double kTiny = 1e-6;  // a delta well below the 0.5-scale quantization.

// ---- helpers ---------------------------------------------------------------------------
double maxAbs(const double* ch, int frames) {
  double m = 0.0;
  for (int f = 0; f < frames; ++f) {
    const double a = std::fabs(ch[f]);
    if (a > m) m = a;
  }
  return m;
}

// Render `frames` through `e` (already prepared) with a per-channel input signal, capturing
// `outCh` output channels. Returns the engine Status.
EngineStatus render(StandaloneAudioEngine& e, const double* const* in, double* const* out, int inCh,
                    int outCh, int frames) {
  return e.processBlock(in, out, inCh, outCh, frames);
}

// ---- 1. default-plan honesty ------------------------------------------------------------
void default_plan() {
  constexpr std::uint64_t kSeed = 101u;
  {  // 0-in / 2-out -> Zero, no channel consumed.
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 0, 2));
    CHECK(e.isReady());
    const DevicePlan& p = e.plan();
    CHECK(p.outputCount == 2);
    CHECK(p.input == InputRoute::Zero);
    CHECK(p.inputCh[0] == -1 && p.inputCh[1] == -1);
  }
  {  // 1-in / 2-out -> ExtOnly, PREAMP explicitly none (mutation-⑤ killer).
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 1, 2));
    CHECK(e.isReady());
    const DevicePlan& p = e.plan();
    CHECK(p.outputCount == 2);
    CHECK(p.input == InputRoute::ExtOnly);
    CHECK(p.inputCh[0] == 0 && p.inputCh[1] == -1);
  }
  {  // 2-in / 2-out -> Distinct(0,1).
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 2, 2));
    CHECK(e.isReady());
    const DevicePlan& p = e.plan();
    CHECK(p.outputCount == 2);
    CHECK(p.input == InputRoute::Distinct);
    CHECK(p.inputCh[0] == 0 && p.inputCh[1] == 1);
  }
  {  // 3-in / 2-out -> Distinct, outputCount 2 (<=3).
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 3, 2));
    CHECK(e.isReady());
    const DevicePlan& p = e.plan();
    CHECK(p.outputCount == 2);
    CHECK(p.inputCh[0] == 0 && p.inputCh[1] == 1);
  }
  {  // 1-in / 4-out -> ExtOnly, outputCount 4 (>=4).
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 1, 4));
    CHECK(e.isReady());
    const DevicePlan& p = e.plan();
    CHECK(p.outputCount == 4);
    CHECK(p.input == InputRoute::ExtOnly);
    CHECK(p.inputCh[0] == 0 && p.inputCh[1] == -1);
  }
  {  // 0-in / 5-out -> outputCount 4 (>=4), extra physical channel untouched.
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, 48000.0, kF, 0, 5));
    CHECK(e.isReady());
    CHECK(e.plan().outputCount == 4);
  }
}

// ---- 2. real 1-in/2-out render ----------------------------------------------------------
void real_1in_2out() {
  constexpr std::uint64_t kSeed = 202u;
  // Two SAME-seed engines, different EXT input. The output must track the input in WET
  // (EXT -> mixer -> WET) and be non-silent; two different inputs must give two different
  // traces (a fixed/synthetic sink would not).
  double inA[1][kF] = {{0}}, inB[1][kF] = {{0}};
  for (int f = 0; f < kF; ++f) {
    inA[0][f] = 0.25;   // +0.5V EXT
    inB[0][f] = -0.35;  // -0.7V EXT
  }
  const double* inpA[1] = {inA[0]};
  const double* inpB[1] = {inB[0]};

  StandaloneAudioEngine eA, eB;
  CHECK(eA.prepare(kSeed, 48000.0, kF, 1, 2));
  CHECK(eB.prepare(kSeed, 48000.0, kF, 1, 2));

  double outA[2][kF] = {{0}}, outB[2][kF] = {{0}};
  double* outPA[2] = {outA[0], outA[1]};
  double* outPB[2] = {outB[0], outB[1]};

  const EngineStatus stA = render(eA, inpA, outPA, 1, 2, kF);
  const EngineStatus stB = render(eB, inpB, outPB, 1, 2, kF);
  CHECK(stA == EngineStatus::Rendered);
  CHECK(stB == EngineStatus::Rendered);
  CHECK(eA.renderedBlocks() == 1);
  CHECK(eA.droppedBlocks() == 0);

  // Non-silence on BOTH delivered WET channels.
  CHECK(maxAbs(outA[0], kF) > 1e-4);
  CHECK(maxAbs(outA[1], kF) > 1e-4);

  // The trace reflects the applied input (not a fixed / synthetic sink): different EXT input
  // -> different WET output at some frame. (The device-normalized EXTs differ by ~0.6, and the
  // VMOS mixer + post chain carry the difference into WET.)
  bool wetDiffers = false;
  for (int f = 0; f < kF; ++f) {
    if (std::fabs(outA[0][f] - outB[0][f]) > kTiny ||
        std::fabs(outA[1][f] - outB[1][f]) > kTiny) {
      wetDiffers = true;
      break;
    }
  }
  CHECK(wetDiffers);
}

// ---- 3. real 4-way WET+DRY ---------------------------------------------------------------
void real_4way() {
  constexpr std::uint64_t kSeed = 303u;
  double inA[1][kF] = {{0}}, inB[1][kF] = {{0}};
  for (int f = 0; f < kF; ++f) {
    inA[0][f] = 0.25;
    inB[0][f] = -0.35;
  }
  const double* inpA[1] = {inA[0]};
  const double* inpB[1] = {inB[0]};

  StandaloneAudioEngine eA, eB;
  CHECK(eA.prepare(kSeed, 48000.0, kF, 1, 4));
  CHECK(eB.prepare(kSeed, 48000.0, kF, 1, 4));
  CHECK(eA.plan().outputCount == 4);

  double outA[4][kF] = {{0}}, outB[4][kF] = {{0}};
  double* outPA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* outPB[4] = {outB[0], outB[1], outB[2], outB[3]};
  render(eA, inpA, outPA, 1, 4, kF);
  render(eB, inpB, outPB, 1, 4, kF);

  // ALL FOUR logicals carry signal (a host loop that only wrote WET would leave DRY silent).
  CHECK(maxAbs(outA[WET_L], kF) > 1e-4);
  CHECK(maxAbs(outA[WET_R], kF) > 1e-4);
  CHECK(maxAbs(outA[DRY_A], kF) > 1e-4);
  CHECK(maxAbs(outA[DRY_B], kF) > 1e-4);

  // DRY A/B are VCO taps — bit-identical across a DIFFERENT EXT input (the EXT path lands in
  // WET only). This is the owner-level proof that EXT is NOT conflated into DRY, and it is the
  // exact detector a host-side channel-reorder / second-output-bank bypass FAILS.
  for (int f = 0; f < kF; ++f) {
    CHECK(outA[DRY_A][f] == outB[DRY_A][f]);
    CHECK(outA[DRY_B][f] == outB[DRY_B][f]);
  }

  // WET L/R respond to the EXT difference.
  bool wetDiffers = false;
  for (int f = 0; f < kF; ++f) {
    if (std::fabs(outA[WET_L][f] - outB[WET_L][f]) > kTiny ||
        std::fabs(outA[WET_R][f] - outB[WET_R][f]) > kTiny) {
      wetDiffers = true;
      break;
    }
  }
  CHECK(wetDiffers);
}

// ---- 4. sample-rate determinism ---------------------------------------------------------
void sample_rate_determinism() {
  constexpr std::uint64_t kSeed = 404u;
  const double rates[4] = {44100.0, 48000.0, 88200.0, 96000.0};

  // (a) sampleRate() is the REAL value (a fixed-48000 engine -> RED).
  for (int i = 0; i < 4; ++i) {
    StandaloneAudioEngine e;
    CHECK(e.prepare(kSeed, rates[i], kF, 1, 4));
    CHECK(e.isReady());
    CHECK(e.sampleRate() == rates[i]);
  }

  // (b) Same seed + same rate -> bit-identical render (per-sample determinism).
  {
    double in[1][kF] = {{0}};
    for (int f = 0; f < kF; ++f) in[0][f] = 0.2 + 0.002 * f;
    const double* inp[1] = {in[0]};
    StandaloneAudioEngine e1, e2;
    CHECK(e1.prepare(kSeed, 48000.0, kF, 1, 4));
    CHECK(e2.prepare(kSeed, 48000.0, kF, 1, 4));
    double o1[4][kF] = {{0}}, o2[4][kF] = {{0}};
    double* p1[4] = {o1[0], o1[1], o1[2], o1[3]};
    double* p2[4] = {o2[0], o2[1], o2[2], o2[3]};
    render(e1, inp, p1, 1, 4, kF);
    render(e2, inp, p2, 1, 4, kF);
    for (int c = 0; c < 4; ++c)
      for (int f = 0; f < kF; ++f)
        CHECK(o1[c][f] == o2[c][f]);  // bit-identical.
  }

  // (c) DIFFERENT rate -> the trace differs (the sample rate is honored, not ignored). The
  // runtime LFO/EG rates scale with sampleRate, so 44.1k vs 96k diverge.
  {
    double in[1][kF] = {{0}};
    for (int f = 0; f < kF; ++f) in[0][f] = 0.2;
    const double* inp[1] = {in[0]};
    StandaloneAudioEngine e1, e2;
    CHECK(e1.prepare(kSeed, 44100.0, kF, 1, 4));
    CHECK(e2.prepare(kSeed, 96000.0, kF, 1, 4));
    double o1[4][kF] = {{0}}, o2[4][kF] = {{0}};
    double* p1[4] = {o1[0], o1[1], o1[2], o1[3]};
    double* p2[4] = {o2[0], o2[1], o2[2], o2[3]};
    render(e1, inp, p1, 1, 4, kF);
    render(e2, inp, p2, 1, 4, kF);
    bool differs = false;
    for (int c = 0; c < 4 && !differs; ++c)
      for (int f = 0; f < kF; ++f)
        if (std::fabs(o1[c][f] - o2[c][f]) > kTiny) { differs = true; break; }
    CHECK(differs);
  }
}

// ---- 5. block partitions ----------------------------------------------------------------
void block_partitions() {
  constexpr std::uint64_t kSeed = 505u;
  double in[1][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = 0.3 + 0.01 * f;
  const double* inp[1] = {in[0]};

  // One 64-frame call.
  StandaloneAudioEngine full;
  CHECK(full.prepare(kSeed, 48000.0, kF, 1, 4));
  double oFull[4][kF] = {{0}};
  double* pFull[4] = {oFull[0], oFull[1], oFull[2], oFull[3]};
  render(full, inp, pFull, 1, 4, kF);
  CHECK(full.renderedBlocks() == 1);
  CHECK(full.droppedBlocks() == 0);

  // Four 16-frame calls, advancing BOTH input and output pointers (the owner delegate is
  // frame-indexed, so the result must be bit-identical).
  StandaloneAudioEngine chunked;
  CHECK(chunked.prepare(kSeed, 48000.0, kF, 1, 4));
  double oChunks[4][kF] = {{0}};
  for (int off = 0; off < kF; off += 16) {
    const double* sliceIn[1] = {in[0] + off};
    double* sliceOut[4] = {oChunks[0] + off, oChunks[1] + off, oChunks[2] + off,
                           oChunks[3] + off};
    const EngineStatus st = render(chunked, sliceIn, sliceOut, 1, 4, 16);
    CHECK(st == EngineStatus::Rendered);
  }
  CHECK(chunked.renderedBlocks() == 4);
  CHECK(chunked.droppedBlocks() == 0);

  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f)
      CHECK(oFull[c][f] == oChunks[c][f]);  // bit-identical.
}

// ---- 6. prepare atomic fail ---------------------------------------------------------------
void expect_bad_format(double sr, int block, int inCap, int outCap) {
  StandaloneAudioEngine e;
  CHECK(e.prepare(0x2020u, 48000.0, kF, 1, 4));  // a valid baseline first.
  CHECK(e.isReady());
  CHECK(!e.prepare(0x2020u, sr, block, inCap, outCap));  // the bad config must be refused.
  CHECK(!e.isReady());
  CHECK(e.sampleRate() == 0.0);  // fully cleared — NO half-write, NO stale old rate.
  CHECK(e.blockSize() == 0);
  CHECK(e.inputCapability() == 0);
  CHECK(e.outputCapability() == 0);

  // The next render after a failed prepare is DroppedNotReady with deterministic silence.
  double out[4][kF];
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f) out[c][f] = 0.777;  // sentinel.
  const double* inp[1] = {out[0]};
  double* outp[4] = {out[0], out[1], out[2], out[3]};
  const EngineStatus st = render(e, inp, outp, 1, 4, kF);
  CHECK(st == EngineStatus::DroppedNotReady);
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f) CHECK(out[c][f] == 0.0);  // zeroed, not a stale buffer.
}

void prepare_atomic_fail() {
  expect_bad_format(std::nan(""), kF, 1, 4);            // NaN rate
  expect_bad_format(std::numeric_limits<double>::infinity(), kF, 1, 4);  // +Inf rate
  expect_bad_format(-std::numeric_limits<double>::infinity(), kF, 1, 4); // -Inf rate
  expect_bad_format(0.0, kF, 1, 4);                     // 0 rate
  expect_bad_format(-48000.0, kF, 1, 4);                // negative rate
  expect_bad_format(48000.0, 0, 1, 4);                  // zero block size
  expect_bad_format(48000.0, -16, 1, 4);                // negative block size
  expect_bad_format(48000.0, kF, 1, 1);                 // <2 outputs
  expect_bad_format(48000.0, kF, 1, 0);                 // 0 outputs
  expect_bad_format(48000.0, kF, -1, 4);                // negative input capability
}

// ---- 8. churn + owner address stability --------------------------------------------------
void churn() {
  StandaloneAudioEngine e;
  const StandaloneAudioEngine* owner = &e;
  for (int i = 0; i < 40; ++i) {
    // Deterministic variation of seed + rate forces a full definition release/re-install.
    CHECK(e.prepare(kLunarStartupSeed + static_cast<std::uint64_t>(i), 48000.0 + i * 250.0,
                    kF, (i % 3 == 0) ? 0 : (i % 3 == 1) ? 1 : 2, (i % 2 == 0) ? 2 : 4));
    CHECK(e.isReady());
  }
  CHECK(&e == owner);  // the owner address is stable across churn (it is held by value).

  // A final render must work (no dangling pointer into a churned-away definition).
  double in[1][kF] = {{0}}, out[4][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = 0.2;
  const double* inp[1] = {in[0]};
  double* outp[4] = {out[0], out[1], out[2], out[3]};
  const int inCap = e.inputCapability();
  const int outCap = e.outputCapability();
  EngineStatus st = render(e, inp, outp, inCap, outCap, kF);
  CHECK(st == EngineStatus::Rendered);
  CHECK(maxAbs(out[0], kF) > 1e-4);  // non-silent (the machine self-oscillates).
}

// Enqueue one raw ControlEvent on a runtime through the PRODUCER seam (fixed-capacity, no heap).
// The allocator probe uses it to hold the EventTimebase under load INSIDE the measured window: a
// real host keyboard/MIDI producer enqueues while audio renders, so the drain paths that drives
// (same-frame ordering, future retention, late delivery, both capacity boundaries) must be
// alloc-0 / free-0 too (F-1, task#101).
bool enqueue_ev(SynthRuntime* rt, ControlEventKind kind, double value, NoteId id,
                std::uint64_t sample, ParameterId pid = ParameterId{0}) {
  ControlEvent e{};
  e.kind = kind;
  e.value = static_cast<SignalSample>(value);
  e.parameter = pid;
  e.source = 1;   // one stable producer
  e.channel = 0;  // and one stable channel: identity is (source, channel, noteId)
  e.noteId = id;
  e.producerSequence = sample;
  e.side = KeyboardSide::Left;
  return rt->enqueueControlEvent(TimedControlEvent{e, sample});
}

// ---- 7. allocator: the full prepared render path allocates 0 ------------------------------
void allocator_probe() {
  StandaloneAudioEngine e;
  CHECK(e.prepare(0x7070u, 48000.0, kF, 1, 4));  // definition is created OUTSIDE the window.
  double in[1][kF] = {{0}}, out[4][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = 0.25;
  const double* inp[1] = {in[0]};
  double* outp[4] = {out[0], out[1], out[2], out[3]};

  // The WHOLE owner-delegate window is measured, INCLUDING the first render. There is no lazy
  // init on the prepare->render path (the definition is fully built in prepare(); renderBlock is
  // a pure delegate), so the entire owner delegate must be alloc-0 AND free-0 from the very first
  // processBlock. Measuring the whole window (not skipping a warm-up render) is what makes a
  // single "pre-allocate once, free it in a callback" defect (the @Codex false-green scenario)
  // get caught: an alloc-only probe that skips the first block would still report zero. This is
  // the whole delegate "0 alloc / 0 free" contract — and it counts BOTH unaligned and aligned
  // (C++17 over-aligned) allocations, because the allocator TU replaces the aligned new/delete
  // pair too, so a callback that frees a pre-allocated over-aligned object is caught.
  const std::size_t before = g_allocCount;
  const std::size_t freeBefore = g_freeCount;
  for (int i = 0; i < 21; ++i) render(e, inp, outp, 1, 4, kF);  // includes the "first" block.
  CHECK(g_allocCount == before);  // the owner delegate (-> DeviceAdapter::renderBlock) allocates 0.
  CHECK(g_freeCount == freeBefore);  // ... and frees 0 (a callback reset/free is the defect @Codex flagged).

  // ---- F-1 (task#101): the SAME window with events PENDING and DELIVERED ---------------------
  // The product render entry now drains the ONE EventTimebase per frame (DeviceAdapter::renderBlock
  // -> SynthRuntime::processBlock(&in, 1, &out, true)), so the drain itself must stay alloc-0 /
  // free-0 as well. The enqueues below are deliberately INSIDE the measured window: a real host
  // producer enqueues while audio renders, and the producer seam is fixed-capacity / no-heap.
  {
    StandaloneAudioEngine ev;
    CHECK(ev.prepare(0x5151u, 48000.0, kF, 1, 4));
    SynthRuntime* rt = const_cast<SynthRuntime*>(ev.runtime());
    CHECK(rt != nullptr);
    const std::size_t a1 = g_allocCount;
    const std::size_t f1 = g_freeCount;
    // A held note at frame 0 (pitch + pressure + gate_on), then a LATE release (sample 5) queued
    // after two blocks have already advanced past it, then a second note at 1030.
    CHECK(enqueue_ev(rt, ControlEventKind::pitch, 1.0, 1, 0));
    CHECK(enqueue_ev(rt, ControlEventKind::pressure, 0.5, 1, 0));
    CHECK(enqueue_ev(rt, ControlEventKind::gate_on, 1.0, 1, 0));
    for (int i = 0; i < 2; ++i) render(ev, inp, outp, 1, 4, kF);
    CHECK(enqueue_ev(rt, ControlEventKind::gate_off, 0.0, 1, 5));  // late: blockStart_ is 128.
    for (int i = 0; i < 2; ++i) render(ev, inp, outp, 1, 4, kF);
    CHECK(enqueue_ev(rt, ControlEventKind::pitch, 2.0, 2, 1030));
    CHECK(enqueue_ev(rt, ControlEventKind::gate_on, 1.0, 2, 1030));
    for (int i = 0; i < 17; ++i) render(ev, inp, outp, 1, 4, kF);  // 21 blocks = 1344 frames.
    CHECK(g_allocCount == a1);
    CHECK(g_freeCount == f1);
    // Non-vacuity: those events really were DELIVERED inside the window (a drain that never ran
    // would leave the left gate at its low rail).
    CHECK(rt->controlVoltageAt(lunar24::core::JackId::keyboard_gate_left_main_out) > 1.0);
  }
  // Both CAPACITY boundaries under load, on the same measured window: parameter events exercise
  // the continuous lane's coalesce path, extra non-coalescible pitch events the continuous
  // overflow refusal, and clock edges the critical overflow -> reconcile-reset failsafe. Every
  // event is due inside the window; none of these paths may allocate or free.
  {
    StandaloneAudioEngine ev;
    CHECK(ev.prepare(0x5252u, 48000.0, kF, 1, 4));
    SynthRuntime* rt = const_cast<SynthRuntime*>(ev.runtime());
    CHECK(rt != nullptr);
    const std::size_t a1 = g_allocCount;
    const std::size_t f1 = g_freeCount;
    for (int i = 0; i < 70; ++i)
      (void)enqueue_ev(rt, ControlEventKind::parameter, 0.5, 0,
                       static_cast<std::uint64_t>(i) * 8, lunar24::core::ParameterId::vco_b_oct_sel);
    for (int i = 0; i < 70; ++i)
      (void)enqueue_ev(rt, ControlEventKind::pitch, 0.25, static_cast<NoteId>(i + 1),
                       static_cast<std::uint64_t>(i) * 4);
    for (int i = 0; i < 70; ++i)
      (void)enqueue_ev(rt, ControlEventKind::clock, 1.0, 0, static_cast<std::uint64_t>(i) * 6);
    for (int i = 0; i < 21; ++i) render(ev, inp, outp, 1, 4, kF);
    CHECK(g_allocCount == a1);
    CHECK(g_freeCount == f1);
  }

  // The probe is live on BOTH sides, BOTH alignednesses. A deliberate alloc AND the matching free
  // must each be detected (so the counter is not itself a silent no-op). First unaligned...
  const std::size_t aSnap = g_allocCount;
  const std::size_t fSnap = g_freeCount;
  void* p = ::operator new(4);
  CHECK(g_allocCount > aSnap);
  ::operator delete(p);
  CHECK(g_freeCount > fSnap);
  // ... then over-aligned (the C++17 `alignas(64)` path that used to bypass the counters and gave
  // the @Codex false-green). Use the DIRECT aligned allocation-function calls, not an idiomatic
  // `new OveralignedProbe()`: allocation-elision (GCC/Clang/MSVC turn a tightly-scoped `new T()` with
  // a matching `delete` into a stack object at -O1/-O3) would elide a `new OveralignedProbe()` to the
  // stack, never call the replaced aligned operator new, and leave the counter unmoved — exactly the
  // Debug-green / Release-RED asymmetry that must not ship. An explicit `::operator new(size_t,
  // align_val_t)` / `::operator delete(void*, align_val_t)` call cannot be elided, so the aligned
  // alloc and its matching aligned free MUST each move the counter. This mirrors the unaligned probe
  // directly above (which already uses `::operator new(4)` / `::operator delete(p)` for the same reason).
  const std::size_t alSnap = g_allocCount;
  const std::size_t alFreeSnap = g_freeCount;
  const std::align_val_t al64{64};
  void* ap = ::operator new(8, al64);
  CHECK(g_allocCount > alSnap);
  ::operator delete(ap, al64);
  CHECK(g_freeCount > alFreeSnap);
  g_allocCount = 0;
  g_freeCount = 0;
}

// ---- 9. format-mismatch drop -------------------------------------------------------------
void format_mismatch_drop() {
  StandaloneAudioEngine e;
  CHECK(e.prepare(0x6060u, 48000.0, kF, 1, 2));  // prepared 1-in / 2-out.
  CHECK(e.isReady());

  // Ask for a channel config we did NOT prepare (2-in / 2-out) -> DroppedFormatMismatch + silence.
  double in[2][kF] = {{0}}, out[4][kF];
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f) out[c][f] = 0.5;  // sentinel.
  const double* inp2[2] = {in[0], in[1]};
  double* outp4[4] = {out[0], out[1], out[2], out[3]};
  const EngineStatus st = render(e, inp2, outp4, 2, 4, kF);
  CHECK(st == EngineStatus::DroppedFormatMismatch);
  CHECK(e.droppedBlocks() == 1);
  for (int c = 0; c < 4; ++c)
    for (int f = 0; f < kF; ++f) CHECK(out[c][f] == 0.0);  // zeroed.

  // Illegal frame count (frames <= 0) -> DroppedIllegal, no channel written, counter bumped.
  double out2[2][1] = {{0}};
  double* outp2[2] = {out2[0], out2[1]};
  const EngineStatus stIllegal = render(e, inp2, outp2, 1, 2, 0);
  CHECK(stIllegal == EngineStatus::DroppedIllegal);
  CHECK(e.droppedBlocks() == 2);

  // A correct 1-in / 2-out render STILL works after the drops (a drop never wedges the engine).
  const double* inp1[1] = {in[0]};
  double* okOut[2] = {out2[0], out2[1]};
  const EngineStatus stOk = render(e, inp1, okOut, 1, 2, 1);
  CHECK(stOk == EngineStatus::Rendered);
  CHECK(e.renderedBlocks() == 1);
}

// ---- 10. max-block guard --------------------------------------------------------------
void max_block_guard() {
  constexpr std::uint64_t kSeed = 909u;
  StandaloneAudioEngine e;
  CHECK(e.prepare(kSeed, 48000.0, 16, 1, 2));  // prepared with maxBlock = 16.
  CHECK(e.isReady());
  CHECK(e.blockSize() == 16);

  // A block LARGER than the prepared max is a kernel/wiring defect. That is the @Codex finding
  // about maxBlockSize being only recorded, never enforced: it must be DroppedIllegal + silence
  // with an exact counter, not a silent Rendered.
  double in[1][kF] = {{0}}, out[2][kF];
  for (int c = 0; c < 2; ++c)
    for (int f = 0; f < kF; ++f) out[c][f] = 0.5;  // sentinel.
  for (int f = 0; f < kF; ++f) in[0][f] = 0.3;
  const double* inp[1] = {in[0]};
  double* outp[2] = {out[0], out[1]};
  const EngineStatus st = render(e, inp, outp, 1, 2, kF);  // 64 > 16.
  CHECK(st == EngineStatus::DroppedIllegal);
  CHECK(e.droppedBlocks() == 1);
  for (int c = 0; c < 2; ++c)
    for (int f = 0; f < kF; ++f) CHECK(out[c][f] == 0.0);  // all valid outputs silenced.

  // In-range blocks (== maxBlock, and < maxBlock) still render — the guard is not a blanket drop.
  double out2[2][16] = {{0}};
  double* okOut[2] = {out2[0], out2[1]};
  const EngineStatus stOk = render(e, inp, okOut, 1, 2, 16);  // exactly maxBlock.
  CHECK(stOk == EngineStatus::Rendered);
  CHECK(e.renderedBlocks() == 1);
  CHECK(e.droppedBlocks() == 1);
}

}  // namespace

int main() {
  std::printf("== GH#4 8B2: standalone host runtime owner (engine_ -> DeviceAdapter) ==\n");
  default_plan();
  real_1in_2out();
  real_4way();
  sample_rate_determinism();
  block_partitions();
  prepare_atomic_fail();
  allocator_probe();
  format_mismatch_drop();
  max_block_guard();
  churn();
  return ::test::finish("host_engine_oracle");
}
