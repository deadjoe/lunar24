// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_device_adapter_oracle.cpp — the strong product oracle for GH#4 (task#71).
//
// This ONE CTest walks the real chain `MachineRuntimeDefinition -> SynthRuntime ->
// DeviceAdapter` and asserts the frozen contracts of the @Codex GH#4 partial mandate
// (msg 6c74e22d). It does NOT re-implement the ProcessBlock algorithm; it drives the
// production runtime through the SAME public entry the host uses:
//
//     DeviceAdapter::renderBlock(SynthRuntime&, const double* const* planarIn,
//                                double* const* planarOut, int frames)
//
// which is the ONE production pixel (`sample**`, `sample=double`). Every rendering
// acceptance criterion below is exercised THROUGH that block delegate — the tests never
// call a second, leaf renderer, so there is nowhere for the host path to diverge.
//
// What is asserted:
//   A. OUTPUT capability gate — cap<2 rejects (fail-closed, keeps no plan); 2-3 device
//      writes EXACTLY WET; >=4 device writes EXACTLY the user-chosen 4-way WET+DRY. A
//      >=4 device reporting a 2-channel WET-only half-success is a defect (RED). 错序/
//      截断/额外写 are detected via a dual-instance cross-mapping render plus a sentinel
//      fill that proves the adapter wrote ONLY the plan's opened channels.
//   B. INPUT route gate — 0/1/2 routes with NO silent copy; an omitted/default route is
//      zero, never an implicit duplication; duplicate terminals must be EXPLICIT.
//   C. SCALE — the ONE shared provisional scale clamp(v*0.5,-1,+1), so WET 2V->+1.0 and
//      DRY 1V->0.5 (2:1). Per-output norm rules are caught by asserting DRY 1V is NOT 1.0.
//   D. NON-FINITE — a non-finite device sample reads as 0 and increments the counter.
//   E. PARTITION INVARIANCE — processFrame-per-frame == processBlock chunked, AND the
//      adapter render is frame-indexed (a 64-frame block == 4x16-frame blocks).
//   F. ZERO-ALLOC — the render path (renderBlock, which drives processFrame internally)
//      allocates nothing (measured against the isolated operator-new probe).
//   G. RUNTIME TERMINAL DISTINCTNESS — feeding {extAudio, preamp} separately proves the two
//      terminals are NOT conflated: EXT only feeds mixer ch4, PREAMP only feeds preamp, and
//      BOTH land in WET only (DRY A/B are untouched). The return-SCC override and the
//      cross-wire negative are both asserted. (Single physical input, inputCapability=1.)
//   H. PRODUCTION BLOCK DELEGATE reflects the runtime, not a fixed/synthetic sink.

#include "mini_test.h"

#include <lunar24/core/device_adapter.h>
#include <lunar24/core/machine_definition.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

// Defined by the isolated allocator TU built into this target.
extern std::size_t g_allocCount;

namespace lunar24::core {
namespace {

double dev_norm(double volts) { return device_normalized_from_volts(volts); }

// A single physical block span used by the render-block test helpers. All helpers are
// bounded to this frame ceiling; test callers must keep `frames` <= kRouteMax.
constexpr int kRouteMax = 256;
constexpr double kTiny = 1e-6;

// Up to 2 physical device-input channels (the EXT / PREAMP plan channels). A helper
// drives instance B through these; only channels present in the plan are read.
struct PlanarBuf {
  double ch[2][kRouteMax];
};

// Per-frame device-output delta of a dual-instance render at a measurement frame.
struct CaseDiff { double wetL, wetR, dryA, dryB; };

// Render `frames` through TWO SAME-seed instances driving the SAME plan (route + mapping),
// and return the per-sample device-output difference at `measF`. Instance A receives
// all-zero device input (the baseline); instance B receives `inB` (per physical channel).
// Both start bit-identical and share an identical prefix, so a non-zero delta at `measF`
// is attributable SOLELY to the routed device input — proving that input either reaches the
// runtime (delta>0) or is NOT silently mirrored anywhere it was not bound (delta==0). DRY
// A/B are VCO taps and must never move under an EXT/PREAMP input.
CaseDiff routedDiff(std::uint64_t seed, const DeviceLayout& layout, int inputCap,
                    const OutputMapping& mapping, int outputCount, InputRoute route,
                    int extCh, int preampCh, const PlanarBuf& inB, int measF, int frames) {
  MachineRuntimeDefinition da(seed, 48000.0);
  MachineRuntimeDefinition db(seed, 48000.0);
  SynthRuntime& ra = da.runtime();
  SynthRuntime& rb = db.runtime();

  DeviceAdapter ada, adb;
  CHECK(ada.prepare(layout, inputCap, mapping, outputCount, route, extCh, preampCh));
  CHECK(adb.prepare(layout, inputCap, mapping, outputCount, route, extCh, preampCh));

  // Instance A: all-zero device input (baseline). Instance B: the injected sample, placed on
  // the physical channel(s) the caller chose in `inB`. Channel 0/1 are the only physical
  // channels a plan with inputCapability<=2 can read, and our tests only use those.
  double inA[2][kRouteMax] = {{0}};
  double inBuffB[2][kRouteMax] = {{0}};
  for (int c = 0; c < 2; ++c)
    for (int f = 0; f < frames; ++f) inBuffB[c][f] = inB.ch[c][f];

  double outA4[4][kRouteMax] = {{0}};
  double outB4[4][kRouteMax] = {{0}};

  double* inPA[2] = {inA[0], inA[1]};
  double* inPB[2] = {inBuffB[0], inBuffB[1]};
  // Output layout: the helper always renders a 4-output WET+DRY plan; the plan's mapping
  // routes logicals to physical channels. We keep the plan at outputCount==4 and write into
  // the adapter's opened channels (at most `outputCapability`), reading back the LOGICAL
  // slots (physical == logical only for the canonical mapping — see output_placement for
  // the cross-mapping fidelity check). For routedDiff we compare the two instances' outputs
  // at the SAME physical slot, so the mapping cancels out.
  double* outPA[4] = {outA4[0], outA4[1], outA4[2], outA4[3]};
  double* outPB[4] = {outB4[0], outB4[1], outB4[2], outB4[3]};

  ada.renderBlock(ra, (const double* const*)inPA, (double* const*)outPA, frames);
  adb.renderBlock(rb, (const double* const*)inPB, (double* const*)outPB, frames);

  CaseDiff d;
  d.wetL = std::fabs(outA4[0][measF] - outB4[0][measF]);
  d.wetR = std::fabs(outA4[1][measF] - outB4[1][measF]);
  d.dryA = std::fabs(outA4[2][measF] - outB4[2][measF]);
  d.dryB = std::fabs(outA4[3][measF] - outB4[3][measF]);
  return d;
}

// ---- A. output capability gate -----------------------------------------------------------
void output_capability() {
  DeviceAdapter ad;
  DeviceLayout stereo{BufferLayoutKind::Interleaved, 2};
  DeviceLayout quad{BufferLayoutKind::Interleaved, 4};
  DeviceLayout six{BufferLayoutKind::Interleaved, 6};
  OutputMapping canon = OutputMapping::canonical();
  OutputMapping noncontig{{5, 3, 1, 2}};
  OutputMapping dup{{0, 0, 1, 2}};

  // cap 0 / 1 -> prepare REJECTS (fail-closed), no plan left behind.
  {
    DeviceLayout none{BufferLayoutKind::Interleaved, 0};
    ad = DeviceAdapter{};
    CHECK(!ad.prepare(none, 0, canon, 2, InputRoute::Zero, -1, -1));
    CHECK(!ad.hasPlan());
    ad = DeviceAdapter{};
    DeviceLayout mono{BufferLayoutKind::Interleaved, 1};
    CHECK(!ad.prepare(mono, 0, canon, 2, InputRoute::Zero, -1, -1));
    CHECK(!ad.hasPlan());
  }
  // A prior valid plan survives a failed later prepare (keep-old-on-fail).
  {
    ad = DeviceAdapter{};
    CHECK(ad.prepare(stereo, 0, canon, 2, InputRoute::Zero, -1, -1));
    CHECK(ad.hasPlan());
    // Now a bogus remap (duplicate channel) -> rejected, old plan kept.
    CHECK(!ad.prepare(quad, 0, dup, 4, InputRoute::Zero, -1, -1));
    CHECK(ad.hasPlan());  // still the stereo WET plan.
    CHECK(ad.plan().outputCount == 2);
  }

  // 2-3 channel device: EXACTLY WET (outputCount==2). A 3-channel device is valid.
  {
    ad = DeviceAdapter{};
    DeviceLayout three{BufferLayoutKind::Interleaved, 3};
    CHECK(ad.prepare(three, 0, canon, 2, InputRoute::Zero, -1, -1));
    CHECK(ad.plan().outputCount == 2);
  }
  // A 4-channel device may NOT report a 2-channel half-success (frozen strategy).
  {
    ad = DeviceAdapter{};
    CHECK(!ad.prepare(quad, 0, canon, 2, InputRoute::Zero, -1, -1));
    CHECK(!ad.hasPlan());
  }
  // >=4 device: EXACTLY 4-way. Validate on a 4 and a 6 channel device.
  {
    ad = DeviceAdapter{};
    CHECK(ad.prepare(quad, 0, canon, 4, InputRoute::Zero, -1, -1));
    CHECK(ad.plan().outputCount == 4);
    ad = DeviceAdapter{};
    CHECK(ad.prepare(six, 0, noncontig, 4, InputRoute::Zero, -1, -1));
    CHECK(ad.plan().outputCount == 4);
  }
  // Non-contiguous self-chosen mapping {5,3,1,2} must be allowed; it is NOT an error.
  {
    ad = DeviceAdapter{};
    CHECK(ad.prepare(six, 0, noncontig, 4, InputRoute::Zero, -1, -1));
    const DevicePlan& p = ad.plan();
    CHECK(p.output.channel[WET_L] == 5);  // extra / non-contiguous slots honoured.
    CHECK(p.output.channel[DRY_B] == 2);
  }
  // Duplicate mapping (two logicals to one physical) -> invalid.
  {
    ad = DeviceAdapter{};
    CHECK(!ad.prepare(quad, 0, dup, 4, InputRoute::Zero, -1, -1));
    CHECK(!ad.hasPlan());
  }
}

// Render placement + mapping fidelity, through renderBlock (the host path). The dual-instance
// cross-mapping render proves each logical lands on the physical channel a non-canonical
// mapping chose (a 错序 write is a per-sample mismatch), and a channel-major sentinel fill
// proves the adapter wrote ONLY the plan's opened channels (no extra / no truncation).
void output_placement() {
  constexpr int kFrames = 96;
  OutputMapping canon = OutputMapping::canonical();
  OutputMapping noncontig{{2, 0, 3, 1}};  // quad: distinct 4-way, non-contiguous.
  OutputMapping nc6{{5, 3, 1, 2}};         // six: distinct 4-way, on channels 5/3/1/2.

  // (1) Cross-mapping fidelity on a 4-channel device: canonical vs non-contiguous, same seed,
  // same constant EXT input -> each logical must land on the physical channel its mapping
  // chose. A wrong (错序) write makes slot c of one device equal a DIFFERENT logical.
  {
    constexpr std::uint64_t kSeed = 33u;
    constexpr double kExtNorm = 0.25;  // +0.5V EXT at every frame.
    MachineRuntimeDefinition da(kSeed, 48000.0);
    MachineRuntimeDefinition db(kSeed, 48000.0);

    DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};
    DeviceAdapter ada, adb;
    CHECK(ada.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
    CHECK(adb.prepare(quad, 1, noncontig, 4, InputRoute::ExtOnly, 0, -1));

    double in[2][kFrames] = {{0}};
    for (int f = 0; f < kFrames; ++f) in[0][f] = kExtNorm;
    double ca[4][kFrames] = {{0}}, cb[4][kFrames] = {{0}};
    double* inP[2] = {in[0], in[1]};
    double* outPA[4] = {ca[0], ca[1], ca[2], ca[3]};
    double* outPB[4] = {cb[0], cb[1], cb[2], cb[3]};
    ada.renderBlock(da.runtime(), (const double* const*)inP, (double* const*)outPA, kFrames);
    adb.renderBlock(db.runtime(), (const double* const*)inP, (double* const*)outPB, kFrames);

    for (int f = 0; f < kFrames; ++f) {
      // non-contiguous {2,0,3,1}: logical -> physical channel.
      CHECK(std::fabs(cb[2][f] - ca[0][f]) < 1e-12);  // WET_L -> ch2
      CHECK(std::fabs(cb[0][f] - ca[1][f]) < 1e-12);  // WET_R -> ch0
      CHECK(std::fabs(cb[3][f] - ca[2][f]) < 1e-12);  // DRY_A -> ch3
      CHECK(std::fabs(cb[1][f] - ca[3][f]) < 1e-12);  // DRY_B -> ch1
    }
  }

  // (2) Untouched-channel proof on a 6-channel device: sentinel-fill every channel, render
  // once, and prove ONLY the plan's opened channels were written. The opened channels carry
  // the WET/DRY logicals; channels 0 and 4 stay sentinel (no silent extra write).
  {
    constexpr std::uint64_t kSeed = 34u;
    constexpr double kSentinel = 0.123;
    constexpr double kExtNorm = 0.25;
    MachineRuntimeDefinition da(kSeed, 48000.0);
    MachineRuntimeDefinition db(kSeed, 48000.0);

    DeviceLayout six{BufferLayoutKind::NonInterleaved, 6};
    DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};
    DeviceAdapter ada, adb;
    // ada renders the actual 6-channel device through the non-contiguous mapping; adb is a
    // same-seed REFERENCE that renders the SAME logical outputs through a quad canonical
    // mapping (the runtime output is independent of device layout, so ref[0..3] == the four
    // logicals). This lets us assert each opened 6-channel slot == its exact logical.
    CHECK(ada.prepare(six, 1, nc6, 4, InputRoute::ExtOnly, 0, -1));
    CHECK(adb.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));

    double in[2][kFrames] = {{0}};
    for (int f = 0; f < kFrames; ++f) in[0][f] = kExtNorm;
    double c6[6][kFrames];
    for (int c = 0; c < 6; ++c)
      for (int f = 0; f < kFrames; ++f) c6[c][f] = kSentinel;
    double* inP[2] = {in[0], in[1]};
    double* outP6[6] = {c6[0], c6[1], c6[2], c6[3], c6[4], c6[5]};
    ada.renderBlock(da.runtime(), (const double* const*)inP, (double* const*)outP6, kFrames);

    double ref[4][kFrames] = {{0}};
    double* outPB4[4] = {ref[0], ref[1], ref[2], ref[3]};
    adb.renderBlock(db.runtime(), (const double* const*)inP, (double* const*)outPB4, kFrames);

    for (int f = 0; f < kFrames; ++f) {
      // Opened channels (mapping {5,3,1,2}) carry the exact WET/DRY logicals.
      CHECK(std::fabs(c6[5][f] - ref[0][f]) < 1e-12);  // WET_L -> ch5
      CHECK(std::fabs(c6[3][f] - ref[1][f]) < 1e-12);  // WET_R -> ch3
      CHECK(std::fabs(c6[1][f] - ref[2][f]) < 1e-12);  // DRY_A -> ch1
      CHECK(std::fabs(c6[2][f] - ref[3][f]) < 1e-12);  // DRY_B -> ch2
      // Closed channels stay sentinel: no extra write, no truncation, no scrambling.
      CHECK(std::fabs(c6[0][f] - kSentinel) < 1e-12);
      CHECK(std::fabs(c6[4][f] - kSentinel) < 1e-12);
    }
  }
}

// 2/3-output render, through the host `renderBlock` path, with sentinel-fill. On a 2 or 3
// channel device the ONLY opened channels are WET L/R (the plan's `outputCount==2`); the
// remaining (3rd) channel must stay sentinel and the DRY logicals must not reach a pointer
// or slot. A same-seed 4-channel reference captures the four logicals so each WET channel is
// asserted against its exact logical. A 3-channel plan is rendered with EXACTLY three valid
// output pointers so an out-of-range 4th write (the outputCount==2-still-writes-4 mutation)
// lands on a real third channel (sentinel destroyed) or is OOB — never masked by a phantom
// 4th pointer.
void output_2_3_render() {
  constexpr int kF = 96;
  constexpr std::uint64_t kSeed = 55u;
  constexpr double kSentinel = 0.777;
  constexpr double kExtNorm = 0.4;  // +0.8V EXT at every frame.
  OutputMapping canon = OutputMapping::canonical();

  // Same-seed 4-channel WET+DRY reference -> the four logicals (layout-independent).
  MachineRuntimeDefinition dref(kSeed, 48000.0);
  DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};
  DeviceAdapter adref;
  CHECK(adref.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
  double in[2][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = kExtNorm;
  double ref[4][kF] = {{0}};
  double* inP[2] = {in[0], in[1]};
  double* refP[4] = {ref[0], ref[1], ref[2], ref[3]};
  adref.renderBlock(dref.runtime(), (const double* const*)inP, (double* const*)refP, kF);

  // (a) 2-channel device: WET L/R land on ch0 / ch1 (canonical). There is no third channel;
  // this proves a 2-output plan writes the WET logicals, not a 4-way.
  {
    MachineRuntimeDefinition d2(kSeed, 48000.0);
    DeviceLayout stereo{BufferLayoutKind::NonInterleaved, 2};
    DeviceAdapter ad2;
    CHECK(ad2.prepare(stereo, 1, canon, 2, InputRoute::ExtOnly, 0, -1));
    double c2[2][kF];
    for (int c = 0; c < 2; ++c)
      for (int f = 0; f < kF; ++f) c2[c][f] = kSentinel;
    double* outP2[2] = {c2[0], c2[1]};
    ad2.renderBlock(d2.runtime(), (const double* const*)inP, (double* const*)outP2, kF);
    for (int f = 0; f < kF; ++f) {
      CHECK(std::fabs(c2[0][f] - ref[0][f]) < 1e-9);  // WET_L -> ch0
      CHECK(std::fabs(c2[1][f] - ref[1][f]) < 1e-9);  // WET_R -> ch1
    }
  }

  // (b) 3-channel device: only WET L/R are written (ch0 / ch1); ch2 (the third channel) must
  // stay sentinel. This is the exact place an outputCount==2-still-writes-4 mutation shows up
  // (it would write DRY_A into ch2). Only THREE valid output pointers are supplied, so an
  // out-of-bounds 4th write is caught by the sanitizer, not hidden behind an extra pointer.
  {
    MachineRuntimeDefinition d3(kSeed, 48000.0);
    DeviceLayout three{BufferLayoutKind::NonInterleaved, 3};
    DeviceAdapter ad3;
    CHECK(ad3.prepare(three, 1, canon, 2, InputRoute::ExtOnly, 0, -1));
    double c3[3][kF];
    for (int c = 0; c < 3; ++c)
      for (int f = 0; f < kF; ++f) c3[c][f] = kSentinel;
    double* outP3[3] = {c3[0], c3[1], c3[2]};
    ad3.renderBlock(d3.runtime(), (const double* const*)inP, (double* const*)outP3, kF);
    for (int f = 0; f < kF; ++f) {
      CHECK(std::fabs(c3[0][f] - ref[0][f]) < 1e-9);  // WET_L -> ch0
      CHECK(std::fabs(c3[1][f] - ref[1][f]) < 1e-9);  // WET_R -> ch1
      CHECK(std::fabs(c3[2][f] - kSentinel) < 1e-12);  // ch2 untouched (no DRY write).
    }
  }
}

// ---- B. input route gate -----------------------------------------------------------------
void input_route() {
  DeviceAdapter ad;
  DeviceLayout quad{BufferLayoutKind::Interleaved, 4};
  OutputMapping canon = OutputMapping::canonical();

  // Invalid plans are REJECTED (and no plan is formed). These are pure prepare-level gates;
  // they do not require a render.
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::ExtOnly, 1, 2));
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::ExtOnly, 7, -1));
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::PreampOnly, 1, 2));
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::Distinct, 1, 1));
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::DuplicateOne, 1, 2));
  CHECK(!ad.prepare(quad, 2, canon, 4, InputRoute::Zero, 1, -1));
  // A NEGATIVE input capability is rejected OUTRIGHT — even for the Zero route, which
  // reads no channel — so a plan can never silently skip the capability check. (A shared
  // or driftable truth that let -1 through as "no input" would be a false pass.)
  CHECK(!ad.prepare(quad, -1, canon, 4, InputRoute::Zero, -1, -1));
  CHECK(!ad.prepare(quad, -2, canon, 4, InputRoute::Distinct, 0, 1));

  constexpr int kF = 96;
  constexpr int kM = 80;  // measurement frame.

  // ExtOnly: bound channel (ch0) ENTERS; the unbound other (ch1) does NOT silently copy.
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5;
    const CaseDiff d = routedDiff(101u, quad, 2, canon, 4, InputRoute::ExtOnly, 0, -1,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);              // EXT -> mixer ch4 -> WET.
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);  // never leaks into DRY.
  }
  {
    PlanarBuf inB{}; inB.ch[1][kM] = 0.5;
    const CaseDiff d = routedDiff(101u, quad, 2, canon, 4, InputRoute::ExtOnly, 0, -1,
                                  inB, kM, kF);
    CHECK(d.wetL == 0.0 && d.wetR == 0.0 && d.dryA == 0.0 && d.dryB == 0.0);
  }

  // PreampOnly: bound channel (ch0) ENTERS; the unbound other (ch1) does NOT.
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5;
    const CaseDiff d = routedDiff(102u, quad, 2, canon, 4, InputRoute::PreampOnly, -1, 0,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }
  {
    PlanarBuf inB{}; inB.ch[1][kM] = 0.5;
    const CaseDiff d = routedDiff(102u, quad, 2, canon, 4, InputRoute::PreampOnly, -1, 0,
                                  inB, kM, kF);
    CHECK(d.wetL == 0.0 && d.wetR == 0.0 && d.dryA == 0.0 && d.dryB == 0.0);
  }

  // DuplicateOne (explicit duplication): the shared channel ENTERS (both terminals).
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5;
    const CaseDiff d = routedDiff(103u, quad, 2, canon, 4, InputRoute::DuplicateOne, 0, 0,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }

  // DualSame (2-route explicit duplication): the shared ch0 ENTERS.
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5;
    const CaseDiff d = routedDiff(104u, quad, 2, canon, 4, InputRoute::DualSame, 0, 0,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }

  // Distinct: either routed channel ENTERS (ch0->EXT, ch1->PREAMP).
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5;
    const CaseDiff d = routedDiff(105u, quad, 2, canon, 4, InputRoute::Distinct, 0, 1,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }
  {
    PlanarBuf inB{}; inB.ch[1][kM] = 0.5;
    const CaseDiff d = routedDiff(106u, quad, 2, canon, 4, InputRoute::Distinct, 0, 1,
                                  inB, kM, kF);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }

  // Zero (default / omitted): a device present on a physical channel is NOT implicitly copied
  // in. The plan reads nothing, so a signal placed there must not enter the runtime.
  {
    PlanarBuf inB{}; inB.ch[0][kM] = 0.5; inB.ch[1][kM] = -0.5;
    const CaseDiff d = routedDiff(107u, quad, 2, canon, 4, InputRoute::Zero, -1, -1,
                                  inB, kM, kF);
    CHECK(d.wetL == 0.0 && d.wetR == 0.0 && d.dryA == 0.0 && d.dryB == 0.0);
  }
}

// ---- C. scale ---------------------------------------------------------------------------
void scale() {
  // Unified 0.5 clamp: WET max 2V -> +1.0, DRY max 1V -> +0.5 (2:1 preserved).
  CHECK(std::fabs(dev_norm(2.0) - 1.0) < 1e-9);
  CHECK(std::fabs(dev_norm(1.0) - 0.5) < 1e-9);
  CHECK(std::fabs(dev_norm(-2.0) - (-1.0)) < 1e-9);
  CHECK(std::fabs(dev_norm(-1.0) - (-0.5)) < 1e-9);
  // clamp to rail.
  CHECK(std::fabs(dev_norm(3.0) - 1.0) < 1e-9);
  CHECK(std::fabs(dev_norm(-5.0) - (-1.0)) < 1e-9);
  // inverse uses the same choke point.
  CHECK(std::fabs(volts_from_device_normalized(1.0) - 2.0) < 1e-9);
  CHECK(std::fabs(volts_from_device_normalized(0.5) - 1.0) < 1e-9);
  CHECK(std::fabs(volts_from_device_normalized(-1.0) - (-2.0)) < 1e-9);
  // Per-output norm would send DRY(+1V) to 1.0; the frozen shared scale sends it to 0.5.
  // Asserting DRY@1V == 0.5 is exactly the check a per-output-normalizer fails.
}

// End-to-end scale, through the host `renderBlock` path — the earlier scale() only exercised
// the free `dev_norm` helper, not that the block delegate actually CONSUMES /0.5 on input and
// PRODUCES the unified x0.5 on output. Two same-seed machines process the same logged sequence:
// one drives `processFrame(RuntimeInputs{known volts})` to make an independent reference, the
// other feeds device-normalized samples into renderBlock. Per frame AND per logical the device
// output must equal the independent frozen criterion clamp(reference volts * 0.5). Pins the
// /0.5 input inverse and the shared x0.5 output; DRY@1V -> 0.5 is a per-output-normalizer killer.
void render_scale_end_to_end() {
  constexpr int kF = 200;
  constexpr std::uint64_t kSeed = 999u;
  constexpr double kExtVolts = 0.8;  // +0.8V EXT -> device-normalized 0.4.
  const double kExtNorm = dev_norm(kExtVolts);  // 0.4

  // Reference machine: direct processFrame with KNOWN VOLTS at each frame.
  MachineRuntimeDefinition drf(kSeed, 48000.0);
  SynthRuntime& rtf = drf.runtime();
  RuntimeOutput ref[kF];
  for (int f = 0; f < kF; ++f) ref[f] = rtf.processFrame(RuntimeInputs{kExtVolts, 0.0}, true);

  // Render machine: device-normalized input through renderBlock (the host path).
  MachineRuntimeDefinition dren(kSeed, 48000.0);
  DeviceAdapter ad;
  DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  CHECK(ad.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
  double in[2][kF] = {{0}};
  for (int f = 0; f < kF; ++f) in[0][f] = kExtNorm;
  double out[4][kF] = {{0}};
  double* inP[2] = {in[0], in[1]};
  double* outP[4] = {out[0], out[1], out[2], out[3]};
  ad.renderBlock(dren.runtime(), (const double* const*)inP, (double* const*)outP, kF);

  for (int f = 0; f < kF; ++f) {
    // Independent frozen criterion: clamp(reference volts * 0.5). A per-output normalizer
    // (DRY x1.0) or a broken /0.5 input inverse breaks one of these exact equalities.
    CHECK(std::fabs(out[WET_L][f] - dev_norm(ref[f].wetL)) < 1e-9);
    CHECK(std::fabs(out[WET_R][f] - dev_norm(ref[f].wetR)) < 1e-9);
    CHECK(std::fabs(out[DRY_A][f] - dev_norm(ref[f].dryA)) < 1e-9);
    CHECK(std::fabs(out[DRY_B][f] - dev_norm(ref[f].dryB)) < 1e-9);
  }

  // The DRY taps must be non-trivial at least once so the x0.5 (not x1.0) scale is genuinely
  // exercised — otherwise a per-output-normalizer could hide behind a zero DRY output.
  double maxAbsDry = 0.0;
  for (int f = 0; f < kF; ++f) {
    maxAbsDry = std::max(maxAbsDry, std::fabs(out[DRY_A][f]));
    maxAbsDry = std::max(maxAbsDry, std::fabs(out[DRY_B][f]));
  }
  CHECK(maxAbsDry > 0.05);
}

// ---- D. non-finite ----------------------------------------------------------------------
void non_finite() {
  DeviceAdapter ad;
  DeviceLayout quad{BufferLayoutKind::Interleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  constexpr int kF = 12;
  CHECK(ad.prepare(quad, 2, canon, 4, InputRoute::Distinct, 0, 1));

  double ch0[kF] = {0}, ch1[kF] = {0};
  ch0[3] = std::nan("");  // frame 3 EXT is NaN
  ch1[5] = std::numeric_limits<double>::infinity();  // frame 5 PREAMP is +Inf
  double* inP[2] = {ch0, ch1};
  double out[4][kF] = {{0}};
  double* outP[4] = {out[0], out[1], out[2], out[3]};
  MachineRuntimeDefinition def(202u, 48000.0);
  ad.renderBlock(def.runtime(), (const double* const*)inP, (double* const*)outP, kF);
  CHECK(ad.nonFiniteSamples() == 2);  // NaN + Inf each counted once.

  // A clean zero block adds no anomaly (a finite sample is never a false count).
  double z0[kF] = {0}, z1[kF] = {0};
  double* zinP[2] = {z0, z1};
  ad.renderBlock(def.runtime(), (const double* const*)zinP, (double* const*)outP, kF);
  CHECK(ad.nonFiniteSamples() == 2);

  // A finite sample in the block is read and does NOT count as an anomaly.
  double f0[kF] = {0}, f1[kF] = {0};
  f0[7] = 0.4;
  double* finP[2] = {f0, f1};
  ad.renderBlock(def.runtime(), (const double* const*)finP, (double* const*)outP, kF);
  CHECK(ad.nonFiniteSamples() == 2);
}

// Non-finite fail-safe, at the rendered trace level. The earlier non_finite() only asserted the
// anomaly COUNTER; it never verified the block actually consumed zero in place of the bad sample.
// Two same-seed machines render the SAME block where ch0[3]=NaN and ch1[5]=+Inf (machine A) or
// ch0[3]=ch1[5]=0 (machine B). The output traces must be BIT-IDENTICAL sample-for-sample across
// all four channels and all-finite. A counter-only fix ("count but feed NaN into the DSP") leaves
// the runtime fed a NaN, which diverges the trace / makes it non-finite -> RED.
void nonfinite_failsafe() {
  constexpr int kF = 200;
  constexpr std::uint64_t kSeed = 202u;
  DeviceLayout quad{BufferLayoutKind::Interleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  const double zero = 0.0;

  // Need both terminals fed (EXT=ch0, PREAMP=ch1) so the non-finite positions are read.
  double inA[2][kF] = {{0}};  double inB[2][kF] = {{0}};
  inA[0][3] = std::nan("");   inA[1][5] = std::numeric_limits<double>::infinity();
  // B is the zero baseline at the SAME positions.
  inB[0][3] = zero;           inB[1][5] = zero;

  double outA[4][kF] = {{0}}, outB[4][kF] = {{0}};
  double* inAP[2] = {inA[0], inA[1]};  double* inBP[2] = {inB[0], inB[1]};
  double* outAP[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* outBP[4] = {outB[0], outB[1], outB[2], outB[3]};

  MachineRuntimeDefinition defA(kSeed, 48000.0);  DeviceAdapter adA;
  CHECK(adA.prepare(quad, 2, canon, 4, InputRoute::Distinct, 0, 1));
  adA.renderBlock(defA.runtime(), (const double* const*)inAP, (double* const*)outAP, kF);
  CHECK(adA.nonFiniteSamples() == 2);

  MachineRuntimeDefinition defB(kSeed, 48000.0);  DeviceAdapter adB;
  CHECK(adB.prepare(quad, 2, canon, 4, InputRoute::Distinct, 0, 1));
  adB.renderBlock(defB.runtime(), (const double* const*)inBP, (double* const*)outBP, kF);
  CHECK(adB.nonFiniteSamples() == 0);

  for (int c = 0; c < 4; ++c) {
    for (int f = 0; f < kF; ++f) {
      CHECK(outA[c][f] == outB[c][f]);              // bit-identical -> the bad sample was ZEROED.
      CHECK(std::isfinite(outA[c][f]));
      CHECK(std::isfinite(outB[c][f]));
    }
  }
}

// ---- E. partition invariance ------------------------------------------------------------
void partition_invariance() {
  constexpr int kFrames = 64;
  RuntimeInputs ramp[kFrames];
  for (int i = 0; i < kFrames; ++i) {
    ramp[i].extAudio = 0.05 * i;
    ramp[i].preamp = -0.03 * i;
  }

  // (a) Runtime partition independence: processFrame-per-frame == processBlock chunked.
  MachineRuntimeDefinition asOne(424242u, 48000.0);
  SynthRuntime& rtOne = asOne.runtime();
  RuntimeOutput outs[2][kFrames];
  for (int i = 0; i < kFrames; ++i) outs[0][i] = rtOne.processFrame(ramp[i], true);

  MachineRuntimeDefinition asChunks(424242u, 48000.0);
  SynthRuntime& rtChunks = asChunks.runtime();
  int cursor = 0;
  const int chunk[5] = {7, 3, 11, 5, kFrames - 26};
  int idx = 0;
  RuntimeInputs piece[kFrames];
  for (int i = 0; i < kFrames; ++i) piece[i] = ramp[i];
  while (cursor < kFrames) {
    const int n = chunk[idx++ % 5];
    const int cnt = (cursor + n <= kFrames) ? n : (kFrames - cursor);
    rtChunks.processBlock(piece + cursor, cnt, outs[1] + cursor, true);
    cursor += cnt;
  }
  for (int i = 0; i < kFrames; ++i) {
    CHECK(std::fabs(outs[0][i].wetL - outs[1][i].wetL) < 1e-9);
    CHECK(std::fabs(outs[0][i].wetR - outs[1][i].wetR) < 1e-9);
    CHECK(std::fabs(outs[0][i].dryA - outs[1][i].dryA) < 1e-9);
    CHECK(std::fabs(outs[0][i].dryB - outs[1][i].dryB) < 1e-9);
  }

  // (b) Adapter frame-indexing independence: renderBlock in ONE 64-frame block vs FOUR
  // 16-frame blocks must produce the identical device output, given the same seed and the
  // same static device input signal. (A block-wide / first-frame-only render would differ.)
  constexpr std::uint64_t kSeed = 434343u;
  OutputMapping canon = OutputMapping::canonical();
  DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};

  MachineRuntimeDefinition full(kSeed, 48000.0);
  MachineRuntimeDefinition chunked(kSeed, 48000.0);
  DeviceAdapter aFull, aChunked;
  CHECK(aFull.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
  CHECK(aChunked.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));

  double in[2][kFrames] = {{0}};
  double deviceIn[2][kFrames] = {{0}};
  for (int i = 0; i < kFrames; ++i) deviceIn[0][i] = 0.5 + 0.01 * i;  // the driver signal.
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < kFrames; ++i) in[c][i] = deviceIn[c][i];

  double fullOut[4][kFrames] = {{0}}, chunkOut[4][kFrames] = {{0}};
  double* inP[2] = {in[0], in[1]};
  double* fullP[4] = {fullOut[0], fullOut[1], fullOut[2], fullOut[3]};

  aFull.renderBlock(full.runtime(), (const double* const*)inP, (double* const*)fullP, kFrames);
  // Proper 4x16 partitioning: advance BOTH the input and the output pointer per chunk so
  // each 16-frame chunk reads the exact slice of the driver signal.
  for (int off = 0; off < kFrames; off += 16) {
    double* sliceIn[2] = {in[0] + off, in[1] + off};
    double* sliceP[4] = {chunkOut[0] + off, chunkOut[1] + off, chunkOut[2] + off,
                         chunkOut[3] + off};
    aChunked.renderBlock(chunked.runtime(), (const double* const*)sliceIn,
                         (double* const*)sliceP, 16);
  }
  for (int i = 0; i < kFrames; ++i) {
    CHECK(std::fabs(fullOut[0][i] - chunkOut[0][i]) < 1e-12);
    CHECK(std::fabs(fullOut[1][i] - chunkOut[1][i]) < 1e-12);
    CHECK(std::fabs(fullOut[2][i] - chunkOut[2][i]) < 1e-12);
    CHECK(std::fabs(fullOut[3][i] - chunkOut[3][i]) < 1e-12);
  }
}

// ---- F. zero-alloc on the host render path ----------------------------------------------
void zero_alloc() {
  MachineRuntimeDefinition def(777777u, 48000.0);
  SynthRuntime& rt = def.runtime();
  DeviceLayout quad{BufferLayoutKind::NonInterleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  DeviceAdapter ad;
  CHECK(ad.prepare(quad, 1, canon, 4, InputRoute::ExtOnly, 0, -1));

  double in[2][64] = {{0}};
  for (int f = 0; f < 64; ++f) in[0][f] = 0.5 + 0.01 * f;
  double out[4][64] = {{0}};
  double* inP[2] = {in[0], in[1]};
  double* outP[4] = {out[0], out[1], out[2], out[3]};

  // Warm up any lazy init so the measurement window is a steady-state render loop.
  ad.renderBlock(rt, (const double* const*)inP, (double* const*)outP, 64);

  const std::size_t before = g_allocCount;
  ad.renderBlock(rt, (const double* const*)inP, (double* const*)outP, 64);
  CHECK(g_allocCount == before);
  // A deliberate alloc during the window MUST be detected (the probe is live).
  g_allocCount = before;
  void* p = ::operator new(4);
  CHECK(g_allocCount > before);
  ::operator delete(p);
  g_allocCount = 0;
}

// ---- G. terminal + patch oracle: same-frame DUAL-INSTANCE rendered device buffers ------
//
// The AUTHORITATIVE measure is the actual rendered device buffer produced by the
// production block delegate (`renderBlock`). Two identical instances (same seed, same
// warmup, same compiled graph) are fed IDENTICAL input through every frame except one
// measurement frame, where a single terminal is varied. Because they start bit-identical,
// any difference at that frame is attributable SOLELY to the varied terminal — no
// oscillator-phase drift, no sequential same-instance contamination. `preampResolvedInput()`
// is corroborating only and is therefore NOT used here (it cannot see a mid-block frame).
enum class TerminalCase : std::uint8_t { PreampOnly, PreampPatched, ExtPatched };

CaseDiff renderDualTerminal(TerminalCase tc) {
  constexpr std::uint64_t kSeed = 777u;
  constexpr int kBlockFrames = 480;
  constexpr int kMeasF = 400;  // 400 warmup frames (identical input), then measure at 400.

  MachineRuntimeDefinition da(kSeed, 48000.0);
  MachineRuntimeDefinition db(kSeed, 48000.0);
  SynthRuntime& ra = da.runtime();
  SynthRuntime& rb = db.runtime();

  const bool patched = (tc == TerminalCase::PreampPatched || tc == TerminalCase::ExtPatched);
  if (patched) {
    namespace reg = lunar24::registry;
    // The return SCC removes the preamp terminal's live fallback; a broken connect() or a
    // silently-refused rebuild() would make the two instances diverge from the REAL graph,
    // so both returns are checked.
    CHECK(ra.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in));
    CHECK(rb.connect(reg::JackId::env_follower_env_out, reg::JackId::preamp_ext_source_in));
  }
  CHECK(ra.rebuild());
  CHECK(rb.rebuild());

  DeviceLayout dpl{BufferLayoutKind::NonInterleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  DeviceAdapter ada, adb;

  // ONE input channel (ch0 feeds the varied terminal); four output channels (WET L/R, DRY A/B).
  // The plan declares a single physical input (inputCapability=1) so a single INPUT buffer is
  // honest — there is no second physical channel here.
  double inA[kBlockFrames] = {0.0}, inB[kBlockFrames] = {0.0};
  inA[kMeasF] = 0.25;   // device-normalized for +0.5V (n = volts*0.5).
  inB[kMeasF] = -0.35;  // device-normalized for -0.7V.
  double outA[4][kBlockFrames] = {{0}}, outB[4][kBlockFrames] = {{0}};
  double* inP[1] = {inA};
  double* inQ[1] = {inB};
  double* outPA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* outPB[4] = {outB[0], outB[1], outB[2], outB[3]};

  if (tc == TerminalCase::ExtPatched) {
    CHECK(ada.prepare(dpl, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
    CHECK(adb.prepare(dpl, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
  } else {
    CHECK(ada.prepare(dpl, 1, canon, 4, InputRoute::PreampOnly, -1, 0));
    CHECK(adb.prepare(dpl, 1, canon, 4, InputRoute::PreampOnly, -1, 0));
  }

  ada.renderBlock(ra, (const double* const*)inP, (double* const*)outPA, kBlockFrames);
  adb.renderBlock(rb, (const double* const*)inQ, (double* const*)outPB, kBlockFrames);

  CaseDiff d;
  d.wetL = std::fabs(outA[0][kMeasF] - outB[0][kMeasF]);
  d.wetR = std::fabs(outA[1][kMeasF] - outB[1][kMeasF]);
  d.dryA = std::fabs(outA[2][kMeasF] - outB[2][kMeasF]);
  d.dryB = std::fabs(outA[3][kMeasF] - outB[3][kMeasF]);
  return d;
}

// ---- G. terminal + patch oracle assertions ----------------------------------------------
void terminal_patch_oracle() {
  // (1) UNPATCHED PREAMP: the preamp terminal is the LIVE host fallback. Varying it on two
  // identical instances changes WET. It does NOT touch DRY (the preamp path lands in the
  // WET mixer only).
  {
    const CaseDiff d = renderDualTerminal(TerminalCase::PreampOnly);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }

  // (2) PATCHED only-PREAMP: the return SCC (env_follower -> preamp.ext_source_in)
  // OVERRIDES the preamp terminal. Varying the terminal must leave all FOUR outputs
  // IDENTICAL per-sample — this is exactly the check a resolver-bypass mutation (which
  // would read the terminal fallback and hence vary) FAILS.
  {
    const CaseDiff d = renderDualTerminal(TerminalCase::PreampPatched);
    CHECK(d.wetL == 0.0 && d.wetR == 0.0 && d.dryA == 0.0 && d.dryB == 0.0);
  }

  // (3) PATCHED only-EXT: the EXT terminal drives mixer ch4 -> WET only. Varying it
  // changes WET but leaves DRY (the VCO taps) unchanged. This is the SAME-frame proof that
  // EXT is NOT conflated into the PREAMP path.
  {
    const CaseDiff d = renderDualTerminal(TerminalCase::ExtPatched);
    CHECK(d.wetL > kTiny);
    CHECK(d.dryA == 0.0 && d.dryB == 0.0);
  }
}

// ---- H. production block delegate reflects the runtime, not a fixed/synthetic sink -------
//
// The rendered-output criterion for the block delegate: the whole `device -> RuntimeInputs
// -> runtime -> RuntimeOutput -> device` path must be a genuine function of the device
// input it is given. A delegate that bypasses the runtime (or emits fixed synthetic
// signals) returns the SAME output for different inputs — this check FAILS it.
void block_delegate_reflects_runtime() {
  constexpr std::uint64_t kSeed = 808u;
  constexpr int kBlockFrames = 480;
  constexpr int kMeasF = 400;

  MachineRuntimeDefinition da(kSeed, 48000.0);
  MachineRuntimeDefinition db(kSeed, 48000.0);
  SynthRuntime& ra = da.runtime();
  SynthRuntime& rb = db.runtime();
  CHECK(ra.rebuild());
  CHECK(rb.rebuild());

  DeviceLayout dpl{BufferLayoutKind::NonInterleaved, 4};
  OutputMapping canon = OutputMapping::canonical();
  DeviceAdapter ada, adb;
  // Single physical input (inputCapability=1): one INPUT buffer feeding EXT ch0.
  CHECK(ada.prepare(dpl, 1, canon, 4, InputRoute::ExtOnly, 0, -1));
  CHECK(adb.prepare(dpl, 1, canon, 4, InputRoute::ExtOnly, 0, -1));

  double inA[kBlockFrames] = {0.0}, inB[kBlockFrames] = {0.0};
  inA[kMeasF] = 0.25;   // EXT +0.5V
  inB[kMeasF] = -0.35;  // EXT -0.7V
  double outA[4][kBlockFrames] = {{0}}, outB[4][kBlockFrames] = {{0}};
  double* inP[1] = {inA};
  double* inQ[1] = {inB};
  double* outPA[4] = {outA[0], outA[1], outA[2], outA[3]};
  double* outPB[4] = {outB[0], outB[1], outB[2], outB[3]};
  ada.renderBlock(ra, (const double* const*)inP, (double* const*)outPA, kBlockFrames);
  adb.renderBlock(rb, (const double* const*)inQ, (double* const*)outPB, kBlockFrames);

  // The rendered WET output must follow the applied device input (not be a fixed sink).
  CHECK(std::fabs(outA[0][kMeasF] - outB[0][kMeasF]) > 1e-6);
  // A fixed-synthetic overflow (all channels written indiscriminately) would also dirty DRY.
  CHECK(outA[2][kMeasF] == outB[2][kMeasF] && outA[3][kMeasF] == outB[3][kMeasF]);
}

}  // namespace
}  // namespace lunar24::core

int main() {
  using namespace lunar24::core;
  std::printf("== GH#4: production DeviceAdapter + distinct EXT/PREAMP runtime chain ==\n");
  output_capability();
  output_placement();
  output_2_3_render();
  input_route();
  scale();
  render_scale_end_to_end();
  non_finite();
  nonfinite_failsafe();
  partition_invariance();
  zero_alloc();
  terminal_patch_oracle();
  block_delegate_reflects_runtime();
  return ::test::finish("device_adapter_oracle");
}
