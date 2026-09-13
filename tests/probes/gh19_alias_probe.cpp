// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_alias_probe.cpp — task #85 (GH #19) REAL-PRODUCT anti-aliasing probe.
//
// Drives the REAL product output through the ONE agreed entry (the same chain the host uses):
//     make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//     standalone_audio_engine.processBlock (encapsulated by EngineHarness),
// and captures the rendered audio for the FULL required-scenario matrix declared by
// tools/gh19_manifest.tsv (BLOCK item ①: a committed contract the analyzer enforces). It does NOT
// compute the aliasing analysis — that is the verified Python method (tools/gh19_alias_analyze.py)
// applied to these real samples. This file is the guard that the measurement truly samples the
// product's DSP, not an ideal() stand-in or a silent buffer, and that every REQUIRED cell is either
// produced or an explicit hard failure (exit non-zero), never a silent pass.
//
// OBSERVATION POINTS (each is a real production tap, verified against the runtime):
//   dryA  = VCO A, tapped BEFORE the chain  (core/include/lunar24/core/machine_runtime.h:117)
//   dryB  = VCO B, tapped BEFORE the chain  (machine_runtime.h:118)
//   droneChannel(0) = pre-mixer classic drone-1 channel bus (machine_runtime.h:1633)
//   droneChannel(2) = pre-mixer classic drone-4 channel bus (classicGroupOfDrone_(drone_4) == 2)
//   drone3Channel()/drone6Channel() = new-drone PapaVoice audio bus (machine_runtime.h:1642-1643)
//   wetL  = the WET mix output channel (device_layout WET_L=0)
//
// CELLS (all required=1 in the manifest; every one must be produced — none are declared blocked):
//   * vco_a_tri / vco_b_tri — VCO A/B default triangle, f by oct_sel x tune, tap dryA / dryB.
//   * drone1_classic        — classic drone gen-1 single carrier (mute_2..5=1), tap droneChannel(0).
//                             Role LOW: group 0 gen 0 (drone_bank.h:373-378 band 30-120 Hz).
//   * drone4_classic        — classic drone gen-4 single carrier on drone_4 (mute_1..3,5=1), tap
//                             droneChannel(2). Group 2 gen 3, but at the same 30-semis-down VOLT as
//                             the LOW cell its generators land at 97-224 Hz — i.e. inside the LOW
//                             band, so this row alone does NOT discharge RULING ③ (@Kimi cd345dbb);
//                             it is kept as a non-vacuity probe of the same accessor.
//   * drone4_classic_v0     — the row that DOES discharge RULING ③: same drone_4 gen 4 recipe with
//                             the VOLT knob at 0.0 => 0 semis down, so the excited generators land at
//                             ~551/835/1266 Hz inside the 420-1800 Hz kHigh band (@Kimi e6e645d2
//                             ruling 1, approving these 12 cells). The `_v0` suffix is load-bearing:
//                             a cell id must name its volt lane or baseline and acceptance rows can
//                             be paired across lanes and still look entirely plausible.
//   * drone3_schmitt / drone6_schmitt — new-drone clean periodic Schmitt tone (pitch>0, fm/am/noise=0).
//   * preamp_ac             — preamp tanh input stage on the WET bus, driven by an AC sine on ch1.
//   * wet_chain             — composite kVcfPath WET output (COMPOSITE cell, not a module attribution).
//
// A cell that produces no legal stimulus is recorded with signal=<reason> AND counts as a hard
// failure (exit non-zero) because it is a REQUIRED manifest cell — the manifest's `required=0` rows
// (sub, saw/pulse/morph) are deferred module probes deliberately NOT produced here.
//
// OUTPUT (--out <dir>, default ./report/gh19-probe):
//   gh19_scenarios.tsv    one metadata row per recorded cell (with measured f0 / peak)
//   gh19_scnNNN.raw       little-endian f64 samples of that cell's observation window
//   gh19_cpu.tsv          four-output finite/block-consistency + block-partition + prep/callback cost.
//   gh19_fidelity.tsv     classic cells only: runtime raw vs probe-built drift-ON raw (must be bit
//                         identical) and drift-ON vs drift-OFF (must differ) -- the provenance proof
//                         for ruling B, where the classic cells' emitted raw is the drift-OFF
//                         self-built render. See the classic block below.
//
// EXIT CODE (BLOCK item ⑤: production failure / raw-write failure / block mismatch must surface):
//   0 = all good; non-zero = any required cell not produced, a raw write failed, a four-output
//   finite/block-consistency check failed, a block-partition mismatch was observed, or (bit 16) a
//   classic-cell probe-fidelity assertion failed.

// This probe writes .raw/.tsv with std::fopen, which MSVC's secure-CRT deprecation
// (C4996) promotes to an error under this repo's /W4 /WX warning policy. No other test
// target uses fopen, so the suppression is scoped to this single test/probe translation
// unit only (defined before any CRT header is included).
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <lunar24/core/device_state.h>
#include <lunar24/core/drone_bank.h>   // probe-built classic bank (ruling B, driftEnabled=false)
#include <lunar24/core/state_default.h>
#include <lunar24/registry_ids.hpp>

#include <host/standalone_audio_engine.h>
#include "test_engine_harness.h"   // the ONE shared real entry (tests/host/)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#if !defined(_WIN32)
#include <sys/utsname.h>   // POSIX-only; MSVC has no equivalent (machine info falls back below)
#endif
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;

namespace {

constexpr std::uint64_t kProbeSeed = 0x4C554E4152ULL;     // same LUNAR hex as the harness default
constexpr std::size_t kWarm = 8192;                       // settle transients
constexpr std::size_t kWin = 16384;                       // analysis window (after warmup)
const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

double peakOf(const std::vector<double>& c) {
  double m = 0.0;
  for (double v : c) {
    const double a = std::fabs(v);
    if (a > m) m = a;
  }
  return m;
}
bool allFinite(const std::vector<double>& c) {
  for (double v : c) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}
// Zero crossings per window -> measured frequency (2 crossings per period for a triangle).
double zcrFreq(const std::vector<double>& c, double sr) {
  int z = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  }
  return static_cast<double>(z) * sr / (2.0 * static_cast<double>(c.size()));
}

double medianOf(std::vector<double> v) {
  if (v.empty()) return -1.0;
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}
double stdevOf(const std::vector<double>& v, double mean) {
  if (v.size() < 2) return 0.0;
  double s = 0.0;
  for (double x : v) { const double d = x - mean; s += d * d; }
  return std::sqrt(s / (static_cast<double>(v.size()) - 1.0));
}

struct Cap {
  bool ok = false;
  std::string signal;        // "" = produced; else a hard-failure reason ("silent", "rejected-*")
  std::vector<double> x;
  double peak = 0.0;
  double f0 = 0.0;
};

// Capture `tap` from a single engine render of `frames` frames at `sr` under the given state.
// `sampled` selects a runtime getter tap (drone buses); otherwise it reads an output channel buffer.
// `domain` = the tap's voltage domain, which fixes the scale-guard bounds:
//   "device" -> physical device output (normalized/0.5 scale): peak must be in [1e-4, 0.55].
//   "volt"   -> a pre-mixer diagnostic bus (volts): peak must be in [1e-3, 5.5].
// A "device" tap whose peak breaches 0.55 is exactly the un-scaled-ideal substitution detector.
Cap capture(const DeviceStateV1& st, double sr, std::size_t frames, const std::string& tap,
            double preampV, const std::string& domain = "device") {
  const double lo = (domain == "volt") ? 1e-3 : 1e-4;
  const double hi = (domain == "volt") ? 5.5 : 0.55;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  if (tap == "drone1" || tap == "drone4" || tap == "drone3" || tap == "drone6") {
    std::vector<double> d;
    d.reserve(frames);
    bool ok = h.renderSampled(static_cast<int>(frames), preampV,
        [&](const lunar24::core::SynthRuntime& rt) {
          double v;
          if (tap == "drone1") v = rt.droneChannel(0);
          // classicGroupOfDrone_(drone_4) == 2 (machine_runtime.h:3456-3463), so the drone-4 cell
          // reads VoiceMixer::kChannelDrone4 via droneChannel(2) -- the same accessor the drone-1
          // cell uses, not a re-derivation.
          else if (tap == "drone4") v = rt.droneChannel(2);
          else if (tap == "drone3") v = rt.drone3Channel();
          else v = rt.drone6Channel();
          d.push_back(v);
        });
    if (!ok) { c.signal = "render-failed"; return c; }
    if (d.size() != frames) { c.signal = "short-sampled"; return c; }
    c.x = std::move(d);
  } else {
    if (!h.render(static_cast<int>(frames), preampV)) { c.signal = "render-failed"; return c; }
    const std::vector<double>* src = nullptr;
    if (tap == "dry_a") src = &h.dryA();
    else if (tap == "dry_b") src = &h.dryB();
    else if (tap == "wet_l") src = &h.wetL();
    else { c.signal = "bad-tap"; return c; }
    if (src->size() < frames) { c.signal = "short-render"; return c; }
    c.x.assign(src->begin(), src->begin() + frames);
  }
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; c.ok = false; return c; }
  if (c.peak < lo) { c.signal = "silent"; c.ok = false; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; c.ok = false; return c; }   // substitution detector
  c.ok = true;
  return c;
}

// ---------------------------------------------------------------------------------------------
// GH#19 S5 (task #111): the HARD-SYNC stimulus.
//
// The stimulus is a PRODUCT CABLE, not a synthetic injection: LFO A's own CV OUT jack patched into
// vco_a.sync_in through the runtime's public connect() — the same entry a user cable takes, and one
// the registry already declares as a real jack of this machine. Nothing about the slave's own
// configuration is special-cased for the test.
//
// MASTER RATE / SAMPLE-EXACTNESS. LFO A is configured as a SQUARE (lfo_a.wave = 0) at
// sr/(M*10) Hz with SPEED MULT = x10, so its per-sample phase step is EXACTLY 1/M with M a power of
// two. The accumulator (lfo.h tick(): phase_ += step; if (phase_ >= 1.0) phase_ -= floor(phase_))
// then holds exact binary fractions, so its rising edge falls exactly ON a sample-grid point and
// repeats every M samples. Two consequences, both of which the acceptance criterion depends on:
//   * the DETECTED edge sample and the PHYSICAL edge instant coincide — there is no sub-sample
//     crossing ambiguity inside the stimulus, which is what makes {r_j} a pure function of the
//     stimulus and not of the signal under test;
//   * the slave is re-phased every M samples with nothing else entering between resets, so the
//     composite is EXACTLY M-periodic (the steady-state / pinned-frequency case, not the
//     aperiodic event-anchored fallback).
// M is the largest power of two whose master rate stays inside the registry's 0.1..20 Hz RATE
// domain, and it is chosen per sample rate (M, master Hz):
//   44100 -> 256, 172.265625 | 48000 -> 256, 187.5 | 88200 -> 512, 172.265625 | 96000 -> 512, 187.5
// (baseHz = sr/(M*10), all exactly representable; the 88.2/96 kHz pair needs the larger M because
// sr/200 — the fastest in-domain master rate divided by x10 — is above 441 samples there).
int syncMasterM(double sr) { return sr > 48000.0 ? 512 : 256; }
double syncMasterBaseHz(double sr) { return sr / (static_cast<double>(syncMasterM(sr)) * 10.0); }

// Capture dry_a from a single render with LFO A's CV OUT cabled into vco_a.sync_in. Identical to
// capture() in every other respect (same tap, same scale guards, same frame count).
// `srcTrace`, when supplied, additionally receives the MASTER'S OWN published output -- LFO A's
// CV OUT jack, read with the runtime's public const accessor -- sampled once per frame. This is the
// independent edge index item 2 needs: it is read off the source, not off the residual and not off
// any assumed constant. The sampler is `renderFeedSampled`'s post-frame const hook, which only reads,
// so it cannot perturb the render it measures; render() and renderSampled() are the SAME per-frame
// processBlock path (test_engine_harness.h:117-128), so supplying a trace does not change the audio.
Cap captureSync(const DeviceStateV1& st, double sr, std::size_t frames,
                std::vector<double>* srcTrace = nullptr, bool cableFromState = false) {
  const double lo = 1e-4, hi = 0.55;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  lunar24::core::SynthRuntime* rt = h.producerRuntime();
  if (rt == nullptr) { c.signal = "no-runtime"; return c; }
  if (!cableFromState) {
    if (!rt->connect(lunar24::core::JackId::lfo_a_cv_out,
                     lunar24::core::JackId::vco_a_sync_in)) {
      c.signal = "sync-connect-rejected";
      return c;
    }
    // The patch cable is only real once the plan is rebuilt off the audio path (the same
    // off-thread rebuild the host performs after a repatch).
    if (!rt->rebuild()) { c.signal = "sync-rebuild-rejected"; return c; }
  }
  // ITEM 4 (task #111): with `cableFromState`, NOTHING is connected here. The cable can only be
  // present because the state's own apply landed it (`inputCable`/`cableSource` -> the definition's
  // cable restore), so this render is the USER LINE: save a patch, restore it, hear the sync.
  if (srcTrace != nullptr) {
    srcTrace->clear();
    srcTrace->reserve(frames);
    if (!h.renderSampled(static_cast<int>(frames), 0.0, [&](const lunar24::core::SynthRuntime& r) {
          srcTrace->push_back(r.controlVoltageAt(lunar24::core::JackId::lfo_a_cv_out));
        })) { c.signal = "render-failed"; return c; }
  } else if (!h.render(static_cast<int>(frames), 0.0)) { c.signal = "render-failed"; return c; }
  const std::vector<double>& src = h.dryA();
  if (src.size() < frames) { c.signal = "short-render"; return c; }
  c.x.assign(src.begin(), src.begin() + frames);
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; c.ok = false; return c; }
  if (c.peak < lo) { c.signal = "silent"; c.ok = false; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; c.ok = false; return c; }
  c.ok = true;
  return c;
}

// ---------------------------------------------------------------------------------------------
// task #111 item 2: the INDEPENDENT master-edge vs slave-reset FRAME reconciliation.
//
// WHY THIS EXISTS. The acceptance figure the gate judges is a residual against an analytic
// band-limited composite, and that composite is built on the premise "the slave is re-phased on the
// MASTER'S RISING EDGES", i.e. on frames congruent to 0 modulo M. Nothing in the residual proves the
// premise: a criterion computed over a periodic signal is invariant under a global sample shift, so a
// small residual is equally consistent with the reset landing one frame late (@Codex 908f36e7 showed
// exactly that — shifting the same periodic samples by one whole cell left the analyzer's answer
// bit-for-bit unchanged, gap 14.0525637166, per=0). The frame therefore has to be pinned by an index
// that does NOT come from the residual, which is what this function is.
//
// METHOD. Render the SAME state and the SAME frame count twice through the product path — once with
// the patch cable (LFO A CV OUT -> vco_a.sync_in) and once without it. Both renders take the same
// entry, the same block schedule and the same feed, so they are byte-identical until the first reset;
// the first frame index where they differ IS the first reset frame.
//
// THE MASTER'S EDGE INDEX IS MEASURED, NOT ASSUMED. The obvious shortcut — derive the grid from the
// documented 1/M phase step and assert "resets at frames == 0 (mod M)" — is WRONG, and WRONG BY
// EXACTLY ONE SAMPLE: the LFO advances before it publishes (lfo.h:149-150), so the value emitted at
// frame k is square(frac((k+1)/M)), i.e. HIGH on frames 0..M/2-2 and again from frame M-1 onward
// (lfo.h:191, HIGH on p < 0.5). Measured on this tree, the first reset is at frame M-1, and a
// hardcoded ==0 rule would have gone RED on a perfectly correct product while a real one-frame
// deferral would have made it GREEN. So the index is read off the master's OWN published output --
// LFO A CV OUT via controlVoltageAt(), sampled per frame -- and the criterion is set equality between
// the two event sets, which needs no constant at all.
//
// The criteria, each with its own failure mode (the reason string names it, so a red arm can assert
// WHICH criterion fired rather than "non-zero exit"):
//   R1 a reset is applied at all              -> no divergence over the whole render
//   R2 the reset is a real re-phasing         -> `depart` no bigger than float noise
//   R3 the slave is not re-reset per sample   -> a LEVEL-triggered reset pins the slave across the
//                                                HIGH half, giving a byte-equal run of M/2 - 1
//                                                samples (the half's first reset sample still carries
//                                                the jump of the phase it interrupted) against the
//                                                maxRun = 1 a correct edge reset measures
//   R4 every reset lands on a SOURCE edge, and every source edge gets a reset (set equality over the
//      observed span). This is the frame reconciliation. A one-frame deferral gives resets at
//      edge+1; a falling-edge-triggered impl fires on the FALLING edges; a missed or doubled edge
//      shows up as a source edge with no reset, or a reset with no source edge.
// The reset EVENT SET is reconstructed from the product OUTPUT alone (the post-reset trajectory
// signature), so the comparison is product-observable event vs source-observable event -- neither
// side is the residual, and neither side is taken from the runtime's own detector.
// Diagnostics (`depart`, `maxRun`, `perM`, `perHalf`, `occ`) are reported whether or not the criteria
// hold, so a reviewer reads the measurement rather than trusting the verdict.
struct SyncTiming {
  bool ok = false;
  std::string reason;                 // "" when every criterion held; else the NAMED failure
  int m = 0;                          // master period in frames (syncMasterM) -- PERIOD, not the grid
  std::size_t d = 0;                  // first frame at which the synced render departs from free-run
  double depart = 0.0;                // max |synced - free| over [d, d+M)
  double peak = 0.0;                  // the synced render's own peak (the scale `depart` is judged on)
  std::size_t maxRun = 0;             // longest byte-equal adjacent run over [d, d+2M)
  std::size_t edgeRun = 0;            // byte-equal run STARTING at the first master rising edge >= d.
                                      // Reported next to maxRun because the pair IS the R3 mechanism:
                                      // that frame is the half's first reset sample and carries the
                                      // interrupted phase's jump, so it is not byte-equal to what follows.
  double perM = -1.0;                 // max |S[i] - S[i+M]|   over the steady window
  double perHalf = -1.0;              // max |S[i] - S[i+M/2]| over the same window
  std::vector<std::size_t> occ;       // frames where the post-reset trajectory signature recurs
  std::vector<std::size_t> resetFrames;  // reset events reconstructed from the product output
  std::vector<std::size_t> srcEdges;  // rising edges of the master's OWN published output
  double blockDiff = -1.0;            // max |per-frame render - real-block render|, same state+cable
  std::size_t blockFirst = 0;         // first frame where those two renders differ
  std::size_t blockCount = 0;         // blocks in the irregular schedule (non-vacuity of ③④)
  Cap synced;                         // reused as the cell's own render (no extra render cost)
};

// An IRREGULAR block schedule whose boundaries sit exactly where a boundary bug would show, with the
// first three chosen deliberately: a boundary ON the master edge frame (M-1), one exactly one frame
// after it (M), and one inside the HIGH half. The rest is lopsided on purpose, including a block that
// spans two master edges, so ③ (adjacent block boundary) and ④ (irregular blocking) are both covered
// rather than a uniform partition that could hide a boundary-relative defect.
std::vector<int> syncBlockSchedule(std::size_t total, std::size_t M) {
  std::vector<int> s;
  std::size_t done = 0;
  auto push = [&](std::size_t n) {
    if (done >= total || n == 0) return;
    const std::size_t k = (n < (total - done)) ? n : (total - done);
    s.push_back(static_cast<int>(k));
    done += k;
  };
  push(M - 1);            // boundary lands ON the first master edge frame
  push(1);                // boundary lands one frame after it
  push(M / 2);            // boundary inside the next HIGH half
  push(2 * M + 1);        // one block spanning TWO master edges
  push(7); push(64); push(3); push(1000); push(4096);
  while (done < total) push(1 + ((total - done) / 7) % 4096);
  return s;
}

// Render the synced configuration through the REAL block entry (EngineHarness::renderBlock, i.e. ONE
// processBlock) under `schedule`, and return dryA. Same state, same cable, same total length as the
// per-frame render, so the two must be byte-identical.
Cap renderSyncedBlocks(const DeviceStateV1& st, double sr, const std::vector<int>& schedule,
                       bool cableFromState = false) {
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  SynthRuntime* rt = h.producerRuntime();
  if (rt == nullptr) { c.signal = "no-runtime"; return c; }
  if (!cableFromState) {
    if (!rt->connect(lunar24::core::JackId::lfo_a_cv_out, lunar24::core::JackId::vco_a_sync_in)) {
      c.signal = "sync-connect-rejected"; return c;
    }
    if (!rt->rebuild()) { c.signal = "sync-rebuild-rejected"; return c; }
  }
  std::size_t total = 0;
  for (int n : schedule) total += static_cast<std::size_t>(n);
  h.reserve(total);
  for (int n : schedule) {
    if (!h.renderBlock(n, [](std::size_t, double& in0, double& in1) { in0 = 0.0; in1 = 0.0; })) {
      c.signal = "render-failed";
      return c;
    }
  }
  c.x = h.dryA();
  if (c.x.size() != total) { c.signal = "short-render"; return c; }
  c.peak = peakOf(c.x);
  if (!allFinite(c.x)) { c.signal = "non-finite"; return c; }
  c.ok = true;
  return c;
}

SyncTiming syncTimingReconcile(const DeviceStateV1& st, double sr, std::size_t frames,
                              bool cableFromState = false) {
  SyncTiming t;
  t.m = syncMasterM(sr);
  const std::size_t M = static_cast<std::size_t>(t.m);

  std::vector<double> src;
  Cap sy = captureSync(st, sr, frames, &src, cableFromState);   // the patch cable IS present + source trace
  // The free-running reference must be the SAME state with NO cable. With `cableFromState` the cable
  // lives in the state, so it cannot be dropped by simply not calling connect(): it has to be taken
  // out of the state, or the "reference" would be synced too and `d` (the first frame where the two
  // renders part) would collapse to a meaningless value.
  Cap fr;
  if (cableFromState) {
    DeviceStateV1 freeRun = st;
    const std::uint32_t sink = static_cast<std::uint32_t>(lunar24::core::JackId::vco_a_sync_in);
    freeRun.inputCable[sink] = 0u;
    freeRun.cableSource[sink] = lunar24::core::JackId{0};
    fr = capture(freeRun, sr, frames, "dry_a", 0.0);
  } else {
    fr = capture(st, sr, frames, "dry_a", 0.0);   // same state/schedule, cable ABSENT
  }
  if (!sy.ok) { t.reason = "synced render unavailable [" + sy.signal + "]"; return t; }
  if (!fr.ok) { t.reason = "free-running reference unavailable [" + fr.signal + "]"; return t; }
  t.synced = sy;
  t.peak = sy.peak;

  const std::vector<double>& S = sy.x;
  const std::vector<double>& U = fr.x;

  // The independent master-edge index: frames at which the master's own published value rises through
  // the gate threshold. The square swings between -1 and +1, so every threshold in (-1, +1) selects
  // the same frames; frame 0 has no predecessor inside this render and is not an edge candidate.
  for (std::size_t i = 1; i < frames; ++i) {
    if (src[i] > 0.0 && src[i - 1] <= 0.0) t.srcEdges.push_back(i);
  }

  // R1 — the first frame at which the cable changes the output is the first reset frame.
  std::size_t d = 0;
  while (d < frames && S[d] == U[d]) ++d;
  t.d = d;
  if (d == frames) {
    t.reason = "no reset was ever applied (the sync cable changes nothing over " +
               std::to_string(frames) + " frames)";
    return t;
  }

  // R2 — a reset RE-PHASES the slave, so within one master period the two renders must differ by a
  // substantial fraction of the signal's own amplitude. A difference confined to float noise would
  // mean the frame index above is measuring rounding, not a reset.
  const std::size_t w = (M < frames - d) ? M : (frames - d);
  for (std::size_t i = d; i < d + w; ++i) {
    const double a = std::fabs(S[i] - U[i]);
    if (a > t.depart) t.depart = a;
  }

  // R3 — the level-trigger signature, MEASURED (2026-09-13: level-triggered mutant vs the 12 shipped
  // cells). A reset requested on every sample of the HIGH half pins the slave at phase 0 for that
  // whole half, but the half's FIRST reset sample is not byte-equal to the rest: it carries the
  // `-0.5*jmp` (vco.h tick()) of the jump out of the phase the slave happened to arrive with, while
  // every later sample of the half carries the same `-0.5*jmp` for the same 0 -> step advance. The
  // byte-equal run therefore covers the REMAINING M/2 - 1 samples of the M/2-sample half. `maxRun`
  // counts SAMPLES — the counter starts at 1 and increments once per equal neighbour, so a run of k
  // equal samples reads k, not k-1 — hence the signature is M/2 - 1 and not M/2: measured 127 at
  // M=256 and 255 at M=512 on the mutant, against maxRun = 1 on all 12 shipped cells of the correct
  // product. The threshold below is M/2 - 1; the earlier M/2 was unreachable.
  const std::size_t rw = (2 * M < frames - d) ? (2 * M) : (frames - d);
  if (rw > 0) {
    t.maxRun = 1;
    std::size_t run = 1;
    for (std::size_t i = 1; i < rw; ++i) {
      if (S[d + i] == S[d + i - 1]) { ++run; if (run > t.maxRun) t.maxRun = run; }
      else run = 1;
    }
    // The run that STARTS at the half's first reset sample -- the first master rising edge at or
    // after d. Reported so the M/2 - 1 signature is read off a measurement of the first-vs-subsequent
    // reset samples rather than inferred from it: on the level-triggered mutant this measures 1 (that
    // frame is the half's one non-conforming sample) while maxRun measures M/2 - 1.
    std::size_t e0 = d;
    for (std::size_t i = 0; i < t.srcEdges.size(); ++i) {
      if (t.srcEdges[i] >= d) { e0 = t.srcEdges[i]; break; }
    }
    const std::size_t eo = (e0 > d) ? (e0 - d) : 0;
    if (eo < rw) {
      t.edgeRun = 1;
      while (eo + t.edgeRun < rw && S[d + eo + t.edgeRun] == S[d + eo + t.edgeRun - 1]) ++t.edgeRun;
    }
  }

  // Reconstruct the reset EVENT SET from the product output alone. After a reset the slave runs free
  // from phase 0, so the samples at d+1, d+2, ... follow a trajectory fixed by the slave's own
  // parameters and repeat EXACTLY one master period later -- but the reset sample d itself carries the
  // BLAMP correction for the jump it interrupted, whose size depends on where the slave happened to be,
  // so the signature deliberately starts ONE FRAME AFTER the reset, where that dependence is gone.
  const std::size_t K = 16;
  if (frames >= d + 1 + K) {
    for (std::size_t s2 = d + 1; s2 + K <= frames; ++s2) {
      bool eq = true;
      for (std::size_t k = 0; k < K; ++k) {
        if (S[s2 + k] != S[d + 1 + k]) { eq = false; break; }
      }
      if (eq) t.occ.push_back(s2);
    }
  }
  // The occurrence set already contains the first reset (its signature starts at d+1), so the first
  // occurrence is the same event as `d` -- take it from the occurrence set and fall back to `d` only
  // if no occurrence was observable at all, rather than listing the first reset twice.
  if (!t.occ.empty()) {
    for (std::size_t i = 0; i < t.occ.size(); ++i) t.resetFrames.push_back(t.occ[i] - 1);
  } else {
    t.resetFrames.push_back(d);
  }

  // Steady-window periodicity diagnostics (not a criterion on their own; the event reconciliation is
  // the exact statement).
  if (frames >= d + 2 * M + M / 2) {
    double pm = 0.0, ph = 0.0;
    for (std::size_t i = d + M; i + M < frames; ++i) {
      const double a = std::fabs(S[i] - S[i + M]);
      if (a > pm) pm = a;
      const double b = std::fabs(S[i] - S[i + M / 2]);
      if (b > ph) ph = b;
    }
    t.perM = pm;
    t.perHalf = ph;
  }

  // ---- verdicts ----
  if (!(t.depart >= 0.1 * t.peak)) {
    t.reason = "first divergence at frame " + std::to_string(d) + " is within float noise "
               "(depart " + std::to_string(t.depart) + " vs peak " + std::to_string(t.peak) +
               ") -- no re-phasing occurred";
    return t;
  }
  // `M >= 8` keeps the rule defined only where its signature (M/2 - 1) is still longer than the
  // maxRun = 1 a correct edge reset measures; every shipped cell has M = 256 or 512, so the guard
  // never weakens this slice — it only stops a caller with a much faster master from reading a
  // saturation artefact of a too-short window as a level-triggered reset.
  if (M >= 8 && t.maxRun >= M / 2 - 1) {
    t.reason = "longest byte-equal run is " + std::to_string(t.maxRun) + " >= M/2-1 = " +
               std::to_string(M / 2 - 1) + " -- the reset is LEVEL-triggered, not edge-triggered "
               "(the half's first reset sample still carries the jump of the phase it interrupted, "
               "so the byte-equal run is the half minus that one sample)";
    return t;
  }
  if (t.srcEdges.empty()) {
    t.reason = "the master's own published output has NO rising edge in " + std::to_string(frames) +
               " frames -- the stimulus is not the square the slice documents";
    return t;
  }
  // R4 — set equality over the span both sides can observe. The last K frames of the render cannot
  // carry an observable post-reset signature, so an edge landing there is outside the comparison; the
  // non-vacuity floor (>=4 events) keeps that from silently emptying the rule.
  const std::size_t spanEnd = (frames > K) ? (frames - K) : 0;
  if (t.resetFrames.size() < 4) {
    t.reason = "only " + std::to_string(t.resetFrames.size()) + " reset event(s) reconstructed -- "
               "too few to reconcile against the master edge index";
    return t;
  }
  for (std::size_t i = 0; i < t.resetFrames.size(); ++i) {
    const std::size_t r = t.resetFrames[i];
    // nearest source edge, for the message
    std::size_t near = 0;
    bool haveNear = false;
    for (std::size_t e : t.srcEdges) {
      if (e > r) break;
      near = e;
      haveNear = true;
    }
    bool onEdge = false;
    for (std::size_t e : t.srcEdges) { if (e == r) { onEdge = true; break; } }
    if (!onEdge) {
      t.reason = "reset #" + std::to_string(i) + " reconstructed at frame " + std::to_string(r) +
                 " but the master's own published output has NO rising edge there" +
                 (haveNear ? (" (the preceding master edge is at frame " + std::to_string(near) +
                              ", i.e. the reset is " + std::to_string(r - near) + " frame(s) LATE)")
                           : std::string(" (no preceding master edge at all)"));
      return t;
    }
  }
  for (std::size_t e : t.srcEdges) {
    if (e < d || e > spanEnd) continue;
    bool got = false;
    for (std::size_t r : t.resetFrames) { if (r == e) { got = true; break; } }
    if (!got) {
      t.reason = "the master's own published output rises at frame " + std::to_string(e) +
                 " but NO reset was applied there -- the edge was missed";
      return t;
    }
  }
  // ③④ — block-partition invariance. The reset decision is taken INSIDE a block, so a block boundary
  // that falls on (or beside) a master edge is where an off-by-one in the block loop would hide: the
  // slave would be re-phased one sample early or late only in the partition that straddles the edge.
  // The real block entry must therefore reproduce the per-frame render BYTE-FOR-BYTE under an
  // irregular schedule, not merely a uniform one.
  {
    const std::vector<int> sched = syncBlockSchedule(frames, M);
    t.blockCount = sched.size();
    Cap b = renderSyncedBlocks(st, sr, sched, cableFromState);
    if (!b.ok) {
      t.reason = "real-block render unavailable [" + b.signal + "]";
      return t;
    }
    double m = 0.0;
    std::size_t first = 0;
    bool found = false;
    for (std::size_t i = 0; i < frames; ++i) {
      const double a = std::fabs(b.x[i] - S[i]);
      if (a > 0.0 && !found) { first = i; found = true; }
      if (a > m) m = a;
    }
    t.blockDiff = m;
    t.blockFirst = first;
    if (m != 0.0) {
      std::size_t near = 0;
      bool haveNear = false;
      for (std::size_t e : t.srcEdges) { if (e > first) break; near = e; haveNear = true; }
      t.reason = "block-partition invariance broken: the irregular-schedule render first differs at "
                 "frame " + std::to_string(first) + " (max |diff| = " + std::to_string(m) + ")" +
                 (haveNear ? (" -- the preceding master edge is at frame " + std::to_string(near) +
                              ", offset " + std::to_string(first - near)) : std::string());
      return t;
    }
  }
  t.ok = true;
  return t;
}

// ---- ITEM 5 (task #111): VCO B has NO sync consumer. ---------------------------------------
// The static reading is unambiguous -- the registry declares exactly ONE sync jack
// (`registry_ids.hpp:390 vco_a_sync_in = 3`; there is no `vco_b_sync_in`), `setVcoSyncBindings()`
// takes a single A binding (`machine_runtime.h:517`, called once from `machine_definition.h:356`),
// the runtime holds exactly one `syncInA_` member and exactly one `syncLatchA_`, and
// `Vco::requestSync()` has exactly ONE call site in the whole product (`machine_runtime.h:3052`,
// the kVcoA slot). This check turns that reading into a measurement rather than a claim.
//
// TWO ways it could be vacuous, and both are asserted against rather than assumed:
//   * if VCO B rendered silence, two silent buffers would match trivially;
//   * if the legal A->B route were not actually carrying anything, "the cable changes nothing"
//     would not test that route.
// It is also easy to get the isolation WRONG, and this check was written that way first: with the
// legal A->B route live, patching the sync cable changes VCO A, and A's change legitimately reaches
// B through that route -- so a naive "B must not move" assertion goes RED on a correct product (it
// did, at frames 255/511/767/1023, exactly the master-edge frames). The isolation therefore mutes
// the legal route BY ITS OWN PANEL KNOB -- `vco_b_cv_amt = 0`, the same means the vco_b_tri cells
// already use to obtain a steady carrier -- and asks the question that actually discriminates:
// with the A->B route muted, does patching the sync cable move VCO B at all? If VCO B had any sync
// consumer of its own, it would reset and move. A third render keeps the route live and shows it
// bites, so the muted comparison cannot pass by having nothing connected.
struct BIsolation {
  bool ok = false;
  std::string reason;
  std::size_t first = 0;
  double maxDiff = -1.0;
  double bPeak = -1.0;     // max |dryB| with the A->B route muted (the reference's own level)
  double modDiff = -1.0;   // max |dryB(cv_amt=1) - dryB(cv_amt=0)| -- the legal route's own bite
};

// Render VCO B for `st`, with the hard-sync cable optionally patched. Returns false on any harness
// refusal with `why` naming it, so a refusal is never silently read as "no difference".
bool renderBFor(const DeviceStateV1& st, double sr, std::size_t frames, bool syncCable,
                std::vector<double>& out, std::string& why) {
  EngineHarness h;
  if (!h.load(st, sr)) { why = "load-rejected"; return false; }
  lunar24::core::SynthRuntime* rt = h.producerRuntime();
  if (rt == nullptr) { why = "no-runtime"; return false; }
  if (syncCable) {
    if (!rt->connect(lunar24::core::JackId::lfo_a_cv_out,
                     lunar24::core::JackId::vco_a_sync_in)) {
      why = "sync-connect-rejected";
      return false;
    }
    if (!rt->rebuild()) { why = "sync-rebuild-rejected"; return false; }
  }
  if (!h.render(static_cast<int>(frames), 0.0)) { why = "render-failed"; return false; }
  out = h.dryB();
  return true;
}

BIsolation isolateSyncFromB(const DeviceStateV1& st, double sr, std::size_t frames) {
  BIsolation r;
  DeviceStateV1 muted = st;
  slot(muted, ParameterId::vco_b_cv_amt) = 0.0;   // legal A->B route muted: the isolation
  DeviceStateV1 live = st;
  slot(live, ParameterId::vco_b_cv_amt) = 1.0;    // legal A->B route live: the non-vacuity control
  std::vector<double> bMutedFree, bMutedSync, bLiveFree;
  std::string why;
  if (!renderBFor(muted, sr, frames, false, bMutedFree, why)) { r.reason = why; return r; }
  if (!renderBFor(muted, sr, frames, true, bMutedSync, why)) { r.reason = why; return r; }
  if (!renderBFor(live, sr, frames, false, bLiveFree, why)) { r.reason = why; return r; }
  if (bMutedFree.size() != bMutedSync.size() || bMutedFree.size() != bLiveFree.size() ||
      bMutedFree.empty()) {
    r.reason = "length-mismatch";
    return r;
  }

  for (std::size_t i = 0; i < bMutedFree.size(); ++i) {
    const double a = std::fabs(bMutedFree[i]);
    if (a > r.bPeak) r.bPeak = a;
    const double m = std::fabs(bLiveFree[i] - bMutedFree[i]);
    if (m > r.modDiff) r.modDiff = m;
  }
  if (!(r.bPeak > 1e-3)) {
    r.reason = "vco B renders silence with the A->B route muted (peak " + std::to_string(r.bPeak) +
               ") -- byte-identity against a silent bus would prove nothing";
    return r;
  }
  if (!(r.modDiff >= 1e-3)) {
    r.reason = "the legal A->B route is inaudible on VCO B (max |diff| = " + std::to_string(r.modDiff) +
               ") -- nothing reaches B by the legal route, so muting it would not isolate anything";
    return r;
  }
  r.maxDiff = 0.0;
  for (std::size_t i = 0; i < bMutedFree.size(); ++i) {
    const double d = std::fabs(bMutedFree[i] - bMutedSync[i]);
    if (d > r.maxDiff) r.maxDiff = d;
    if (d != 0.0 && r.first == 0) r.first = i;
  }
  if (!(r.maxDiff == 0.0)) {
    r.reason = "with the legal A->B route MUTED (vco_b_cv_amt=0), patching the hard-sync cable "
               "still CHANGED VCO B: first differing frame " + std::to_string(r.first) + " of " +
               std::to_string(bMutedFree.size()) + ", max |diff| = " + std::to_string(r.maxDiff) +
               " -- VCO B has a sync consumer of its own, so the S5 premise (the sync jack is "
               "VCO A's alone) is false";
    return r;
  }
  r.ok = true;
  return r;
}

// ---- ITEM 4 (task #111): the USER LINE — the sync cable carried by the DeviceState itself. ----
// Every sync cell makes its cable with a post-load `connect()`. That is a test affordance; it is NOT
// how a user gets a patch. The user line is: the cable is part of the SAVED state and a restore
// re-establishes it. In this codebase that is literal — `DeviceStateV1::inputCable[sink] = 1` and
// `cableSource[sink] = source`, where the SINK INDEX IS THE SERIALIZED JackId, not a dense index
// (`device_state.h:465-468`, `machine_definition.h:452-458`) — and the definition's own apply calls
// `runtime_.connect(source, sink)` for each declared cable and then verifies the final user-cable
// bank EQUALS the requested set exactly, failing the whole candidate on any mismatch
// (`machine_definition.h:449-477`). So a cable declared in the state travels encode -> decode ->
// apply and nothing else; if it does not survive, the render has no reset at all.
//
// This does NOT add a cell: the shipped 96-cell matrix and the committed baseline stay untouched
// (@Codex 908f36e7: 既有改善矩阵保留). It is an assertion on the same stimulus instead.
DeviceStateV1 makeSyncStateViaCable(const DeviceStateV1& plain) {
  DeviceStateV1 st = plain;
  const std::uint32_t sink = static_cast<std::uint32_t>(lunar24::core::JackId::vco_a_sync_in);
  st.inputCable[sink] = 1u;
  st.cableSource[sink] = lunar24::core::JackId::lfo_a_cv_out;
  return st;
}

struct CableLine {
  bool ok = false;
  std::string reason;
  double maxDiff = -1.0;   // |state-cable render - connect render|, must be exactly 0
  std::size_t resets = 0;  // master-grid resets seen on the state-cable render (non-vacuity)
};

// The state-cable render must (a) actually be SYNCED -- its resets must reconcile against the
// master's own published edges, exactly as a shipped cell's do -- and (b) be BYTE-IDENTICAL to the
// render of the same stimulus whose cable was made by connect(). (a) is what stops a silently
// un-restored cable from passing on "two unsynced renders agree"; (b) is the claim that the user
// line is the same signal, not merely a signal.
CableLine syncViaStateCable(const DeviceStateV1& stPlain, double sr, std::size_t frames) {
  CableLine r;
  const DeviceStateV1 stCable = makeSyncStateViaCable(stPlain);
  const SyncTiming t = syncTimingReconcile(stCable, sr, frames, true);
  if (!t.ok) {
    r.reason = "the sync cable declared in the DeviceState did NOT produce master-grid resets: " +
               t.reason;
    return r;
  }
  r.resets = t.resetFrames.size();
  const Cap viaState = captureSync(stCable, sr, frames, nullptr, true);
  if (!viaState.ok) { r.reason = "state-cable render: " + viaState.signal; return r; }
  const Cap viaConnect = captureSync(stPlain, sr, frames, nullptr, false);
  if (!viaConnect.ok) { r.reason = "connect render: " + viaConnect.signal; return r; }
  if (viaState.x.size() != viaConnect.x.size()) { r.reason = "length-mismatch"; return r; }
  r.maxDiff = 0.0;
  for (std::size_t i = 0; i < viaState.x.size(); ++i) {
    const double d = std::fabs(viaState.x[i] - viaConnect.x[i]);
    if (d > r.maxDiff) r.maxDiff = d;
  }
  if (!(r.maxDiff == 0.0)) {
    r.reason = "the DeviceState-carried sync cable and the connect()-made one render DIFFERENTLY "
               "(max |diff| = " + std::to_string(r.maxDiff) + ") -- the saved patch does not "
               "reproduce the patched one";
    return r;
  }
  r.ok = true;
  return r;
}

std::string cableLine(double sr, const CableLine& r) {
  char nb[2][32];
  std::snprintf(nb[0], sizeof(nb[0]), "%.6g", r.maxDiff);
  return "vco_a_sync_state_cable sr=" + std::to_string((int)sr) +
         " resets=" + std::to_string(r.resets) + " connect_delta=" + nb[0] +
         (r.ok ? "  [OK]" : ("  [RED: " + r.reason + "]"));
}

// One-line evidence record, printed whether the isolation held or not.
std::string bIsolationLine(double sr, const BIsolation& r) {
  char nb[3][32];
  std::snprintf(nb[0], sizeof(nb[0]), "%.6g", r.maxDiff);
  std::snprintf(nb[1], sizeof(nb[1]), "%.6g", r.bPeak);
  std::snprintf(nb[2], sizeof(nb[2]), "%.6g", r.modDiff);
  return "vco_b_sync_isolation sr=" + std::to_string((int)sr) +
         " b_peak=" + nb[1] + " a_to_b_bite=" + nb[2] +
         " sync_cable_delta_muted=" + nb[0] + (r.ok ? "  [OK]" : ("  [RED: " + r.reason + "]"));
}

// One-line evidence record for a reconciliation, printed whether it passed or failed. The floats go
// out in %g so "per_M = 0" is distinguishable from "per_M = 3e-15" in the log.
std::string syncTimingLine(const std::string& cid, double sr, const SyncTiming& t) {
  char nb[8][32];
  std::snprintf(nb[0], sizeof(nb[0]), "%.6g", t.depart);
  std::snprintf(nb[1], sizeof(nb[1]), "%.6g", t.peak);
  std::snprintf(nb[2], sizeof(nb[2]), "%.6g", t.perM);
  std::snprintf(nb[3], sizeof(nb[3]), "%.6g", t.perHalf);
  std::snprintf(nb[4], sizeof(nb[4]), "%.6g", t.blockDiff);
  std::string s = cid + " sr=" + std::to_string((int)sr) + " M=" + std::to_string(t.m) +
                  " d=" + std::to_string(t.d) + " depart=" + nb[0] +
                  " peak=" + nb[1] +
                  " max_run=" + std::to_string(t.maxRun) +
                  " edge_run=" + std::to_string(t.edgeRun) + " per_M=" + nb[2] +
                  " per_half=" + nb[3] +
                  " src_edges=" + std::to_string(t.srcEdges.size()) +
                  " resets=" + std::to_string(t.resetFrames.size()) +
                  " blocks=" + std::to_string(t.blockCount) +
                  " block_diff=" + nb[4];
  if (!t.srcEdges.empty()) {
    s += " edge_head=";
    for (std::size_t i = 0; i < t.srcEdges.size() && i < 4; ++i) {
      s += (i ? "," : "") + std::to_string(t.srcEdges[i]);
    }
  }
  if (!t.resetFrames.empty()) {
    s += " reset_head=";
    for (std::size_t i = 0; i < t.resetFrames.size() && i < 4; ++i) {
      s += (i ? "," : "") + std::to_string(t.resetFrames[i]);
    }
  }
  s += t.ok ? "  [OK]" : ("  [RED: " + t.reason + "]");
  return s;
}

// AC-input capture: feeds a zero-centred sine on physical ch1 (the preamp feed) so the nonlinear
// input stage sees a real periodic stimulus — BLOCK item ④: production accepts an AC input via the
// processBlock input array, so the preamp is NOT a product blocker (it only needs an AC stimulus,
// not an oversample comparison, to produce a measurable cell).
Cap captureAc(const DeviceStateV1& st, double sr, std::size_t frames, double amp, double acHz,
              const std::string& tap, const std::string& domain = "device") {
  const double lo = (domain == "volt") ? 1e-3 : 1e-4;
  const double hi = (domain == "volt") ? 5.5 : 0.55;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (h.runtime() == nullptr) { c.signal = "no-runtime"; return c; }
  const double w = 2.0 * 3.14159265358979323846 * acHz / sr;
  bool ok = h.renderFeed(static_cast<int>(frames),
      [&](std::size_t f, double& in0, double& in1) {
        in0 = 0.0;
        in1 = amp * std::sin(w * static_cast<double>(f));
      });
  if (!ok) { c.signal = "render-failed"; return c; }
  const std::vector<double>* src = nullptr;
  if (tap == "wet_l") src = &h.wetL();
  else if (tap == "dry_a") src = &h.dryA();
  else if (tap == "dry_b") src = &h.dryB();
  else { c.signal = "bad-tap"; return c; }
  if (src->size() < frames) { c.signal = "short-render"; return c; }
  c.x.assign(src->begin(), src->begin() + frames);
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; c.ok = false; return c; }
  if (c.peak < lo) { c.signal = "silent"; c.ok = false; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; c.ok = false; return c; }
  c.ok = true;
  return c;
}

bool writeTsv(const std::string& path, const std::vector<std::string>& lines) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
  std::fclose(f);
  return true;
}
bool writeRaw(const std::string& path, const std::vector<double>& x) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  for (double v : x) std::fwrite(&v, sizeof(double), 1, f);
  std::fclose(f); return true;
}

std::string safeTsv(const std::string& s) { return s.empty() ? "-" : s; }

// ---------------------------------------------------------------------------------------------
// Classic-cell render off the SHIPPED runtime path (ruling B, @Kimi f260fd91).
//
// The S2 acceptance column pins phi ANALYTICALLY at the frequency the accumulator actually ran at.
// That frequency is a CONSTANT only when the drift model is inert: with drift off, tickGroup's
// `effFreq = base*(1+tolerance) + driftNow` collapses to `base*(1+tolerance)` (drone_bank.h:275-283),
// so phi enters as 2*pi*frac(warm*f0/sr) exactly and the only free parameter left is the scale. With
// drift on, the same accumulator is frequency-MODULATED and no constant-frequency reference can
// represent it (measured: the free-phase best fit on those arms is -1 dB order vs -33..-44 dB on the
// drift-off arms -- ~30 dB apart, with no oracle involved).
//
// driftEnabled=false is UNREACHABLE through the product: SynthRuntime's member is constructed with
// the default (drift ON, machine_runtime.h:428) and DroneBank exposes no setter. So the classic cells
// are rendered here by a probe-built bank. That swap is only admissible if the self-built bank IS the
// shipped path, which is not assumed: the probe renders BOTH ways and asserts the drift-ON raws are
// bit-identical (gh19_fidelity.tsv). The configuration below therefore replicates the runtime's
// control mapping and its canonical execution block line for line:
//   * mute gens 1..4                    -- runtime: drone_1_mute_2..5 = 1
//   * setTune(0, (n-0.5)*24.0)          -- runtime: classicDroneTuneSemisFromNorm (machine_runtime.h:2789)
//   * setVolt(0, 30.0)                  -- runtime: classicDroneVoltSemisDownFromNorm(0.5) (machine_runtime.h:2792)
//   * setGroupHold/setGroupGate(0,true) -- runtime: drone_1_gate_hold=1 (the gate is never set from
//                                          params, so it stays at kDefaultGroupGateOpen = true)
//   * tickGroup(0, out5) per frame, channel = sum of the 5 group samples
//                                       -- runtime: machine_runtime.h:3069-3078, droneChannel(0)
struct ClassicSelf {
  Cap cap;
  double f0_model = 0.0;   // analytic accumulator rate used below ONLY when drift is off; else 0.
};

// `group` is a CLASSIC group index (0..3) and `gen` a generator WITHIN it (0..4). The ROLE is a
// property of the generator, not of the group (drone_bank.h:373-378: gen 0,1 = kLow 30-120 Hz,
// gen 2 = kMedium 140-380, gens 3,4 = kHigh 420-1800), so a HIGH-role cell is this same recipe with
// `gen` pointing at a high generator of its own group -- nothing else about the render changes.
ClassicSelf captureClassicSelf(int group, std::size_t gen, double sr, double tuneNorm,
                               double voltSemisDown, bool driftEnabled) {
  const double lo = 1e-3;   // same "volt"-domain scale guard as capture(...)
  const double hi = 5.5;
  ClassicSelf out;
  lunar24::core::DroneBank bank(kProbeSeed, sr, lunar24::core::DroneBank::kMaxVoices, driftEnabled);
  const std::size_t flat = static_cast<std::size_t>(group) * lunar24::core::DroneBank::kGensPerVoice + gen;
  // setMute/setTune take the FLAT voice index; setVolt takes the GROUP (drone_bank.h:201-211). Only
  // `group` is ever ticked below, so the other groups' mute flags cannot enter this raw.
  for (std::size_t i = 0; i < lunar24::core::DroneBank::kMaxVoices; ++i) bank.setMute(i, i != flat);
  bank.setTune(flat, (tuneNorm - 0.5) * 24.0);
  bank.setVolt(group, voltSemisDown);
  bank.setGroupHold(group, true);
  bank.setGroupGate(group, true);
  const std::size_t total = kWarm + kWin;
  std::vector<double> d;
  d.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    double out5[lunar24::core::DroneBank::kGensPerVoice] = {};
    bank.tickGroup(group, out5);
    double s = 0.0;
    for (std::size_t g = 0; g < lunar24::core::DroneBank::kGensPerVoice; ++g) s += out5[g];
    d.push_back(s);
  }
  // The analytic constant rate, read from the SAME instance that produced the samples (never
  // re-derived from the requested tune): base*2^(tune/12)*2^(-volt/12)*(1+tolerance). NOTE:
  // DroneBank::effectiveFreqHz (drone_bank.h:317-321) is NOT this value -- it applies tolerance to
  // freqBaseHz instead of to the transposed base, so it must not be used as the frequency oracle.
  if (!driftEnabled) {
    const double tune = bank.tuneOf(flat), volt = bank.voltOf(flat);
    out.f0_model = bank.freqBaseHz(flat) * std::pow(2.0, tune / 12.0) * std::pow(2.0, -volt / 12.0) *
                   (1.0 + bank.toleranceOf(flat));
  }
  out.cap.x = std::move(d);
  out.cap.peak = peakOf(out.cap.x);
  out.cap.f0 = zcrFreq(out.cap.x, sr);
  if (!allFinite(out.cap.x)) { out.cap.signal = "non-finite"; return out; }
  if (out.cap.peak < lo) { out.cap.signal = "silent"; return out; }
  if (out.cap.peak > hi) { out.cap.signal = "over-scale"; return out; }
  out.cap.ok = true;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh19-probe";
  for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
  // Portable recursive create — the previous "mkdir -p '" + out + "'" invokes a POSIX shell;
  // on the MSVC CI runner that resolves to cmd.exe where `mkdir` has no -p and the literal
  // single quotes are kept, so the output dir is never created and fopen("wb") fails below.
  std::error_code ec; std::filesystem::create_directories(out, ec); /* best effort */

  int gCode = 0;   // 0 = clean; OR-in failure flags on any produced-but-failed / write-fail / block mismatch.

  std::vector<std::string> rows;
  rows.push_back("id\tpath\tsignal\tsr_hz\tf0_target_hz\tf0_meas_hz\tpeak\tchannel\twave\traw\tsamples"
                 "\tf0_model_hz");
  int scnIdx = 0;

  // Classic-cell provenance evidence (ruling B): one row per classic cell, written to
  // gh19_fidelity.tsv and asserted by the analyzer's probe-contract gate.
  std::vector<std::string> frows;
  frows.push_back("id\tsamples\trt_vs_selfON_max_abs_diff\tbit_identical"
                  "\tdriftON_vs_OFF_max_abs_diff\tdrift_off_differs");

  auto fmtNum = [](double v, int prec) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*g", prec, v);
    return std::string(b);
  };

  // Emit a scenario row. Every PRODUCED cell gets an indexed raw sample file. A required cell that
  // is NOT produced is a hard failure (exit non-zero) — the analyzer is the final gate, but the probe
  // must fail loudly so a silent/over-scale/non-finite/rejected cell never masquerades as success.
  // `f0_model_hz` (classic cells only) is the analytic constant accumulator rate the emitted raw ran
  // at; the analyzer pins phi with it. It is "-" for every other cell.
  auto emit = [&](const std::string& id, const std::string& path, double sr, double ft, const Cap& c,
                  const std::string& channel, const std::string& wave,
                  const std::string& f0_model = "-") {
    std::string raw = "";
    if (c.ok) {
      raw = "gh19_scn" + std::to_string(++scnIdx) + ".raw";
      if (!writeRaw(out + "/" + raw, c.x)) {
        std::fprintf(stderr, "FATAL %s: raw write failed (%s)\n", id.c_str(), raw.c_str());
        gCode |= 2;
      }
    } else {
      std::fprintf(stderr, "FATAL %s: required cell not produced [%s]\n", id.c_str(), c.signal.c_str());
      gCode |= 1;
    }
    rows.push_back(id + "\t" + path + "\t" + safeTsv(c.signal) +
                   "\t" + std::to_string(sr) +
                   "\t" + std::to_string(ft) +
                   "\t" + std::to_string(c.f0) +
                   "\t" + std::to_string(c.peak) +
                   "\t" + channel + "\t" + wave +
                   "\t" + raw + "\t" + std::to_string(c.x.size()) +
                   "\t" + f0_model);
  };

  // The sync-cell state recipe, in ONE place: the cell loop below emits it and the --sync-diag mode
  // measures it, so a diagnostic reading can never describe a different stimulus from the shipped cell.
  auto makeSyncState = [](double sr, double ft) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::vco_a_oct_sel) = (ft == 220.0) ? 0.0 : 1.0;
    slot(st, ParameterId::vco_a_tune) = (ft == 880.0) ? 1.0 : 0.0;
    // The master: a SQUARE at sr/(M*10) Hz with SPEED MULT = x10 => per-sample phase step is
    // exactly 1/M (M a power of two), so the rising edge sits on a sample-grid point.
    slot(st, ParameterId::lfo_a_wave) = 0.0;
    slot(st, ParameterId::lfo_a_rate) = syncMasterBaseHz(sr);
    slot(st, ParameterId::lfo_a_speed_mult) = 2.0;
    return st;
  };

  // --sync-diag: print the item-2 reconciliation evidence for all 12 sync cells and exit WITHOUT
  // emitting the matrix. This exists so the criteria's bounds are read off a measurement of the
  // shipped stimulus rather than guessed, and so the same numbers can be re-read after any change.
  bool syncDiag = false;
  for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--sync-diag") == 0) syncDiag = true;
  if (syncDiag) {
    for (double sr : kSrs) {
      for (double ft : {220.0, 440.0, 880.0}) {
        const DeviceStateV1 st = makeSyncState(sr, ft);
        const SyncTiming t = syncTimingReconcile(st, sr, kWarm + kWin);
        // The reconciliation samples the master's jack through renderFeedSampled's post-frame hook,
        // and the cell's own raw now comes from that same traced render. That is only legitimate if
        // the hook cannot perturb what it measures, so render the same state WITHOUT the hook and
        // require the two to be byte-identical. A read-only const hook makes this a property, not a
        // hope -- and if it ever stops being one, the cell raws silently change, which is exactly the
        // kind of drift the gate would otherwise absorb as a baseline move.
        const Cap untraced = captureSync(st, sr, kWarm + kWin, nullptr);
        double traceDiff = -1.0;
        if (untraced.ok && t.synced.ok && untraced.x.size() == t.synced.x.size()) {
          traceDiff = 0.0;
          for (std::size_t i = 0; i < untraced.x.size(); ++i) {
            const double a = std::fabs(untraced.x[i] - t.synced.x[i]);
            if (a > traceDiff) traceDiff = a;
          }
        }
        std::printf("SYNC-TIMING %s trace_diff=%.6g\n", syncTimingLine(
            "vco_a_sync_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft), sr, t).c_str(),
            traceDiff);
      }
    }
    return 0;
  }

  // ---- VCO A default triangle: 4 SRS x 3 frequencies. Reachable, clean, periodic (analytic target).
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      double oct = (ft == 220.0) ? 0.0 : 1.0;
      double tune = (ft == 880.0) ? 1.0 : 0.0;
      slot(st, ParameterId::vco_a_oct_sel) = oct;
      slot(st, ParameterId::vco_a_tune) = tune;
      Cap c = capture(st, sr, kWarm + kWin, "dry_a", 0.0);
      emit("vco_a_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft),
           "vco_a_tri", sr, ft, c, "dry_a", "tri");
    }
  }

  // ---- VCO B triangle: 4 SRS x 3 frequencies (item ③ coverage for the B generator, and item ⑥
  //      verification of the A/B shared VCO call point). VCO-B shares the Vco class with VCO-A, but the
  //      DEFAULT graph feeds vco_b.cv_in live from vco_a.dry_out (the task#83 A->B route,
  //      machine_runtime.h:2226-2237), so with a non-zero vco_b_cv_amt VCO-B is audio-rate FM-ed by
  //      VCO-A's triangle and dryB is NOT a clean single carrier. To measure VCO-B as a STEADY
  //      carrier (comparable to VCO-A, and to verify the shared oscillator), we set the legal panel
  //      knob vco_b_cv_amt=0 so the CV input has zero depth. The default (cv_amt>0) FM-spread is a
  //      documented product finding, not the clean-triangle target — see the report.
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      double oct = (ft == 220.0) ? 0.0 : 1.0;
      double tune = (ft == 880.0) ? 1.0 : 0.0;
      slot(st, ParameterId::vco_b_oct_sel) = oct;
      slot(st, ParameterId::vco_b_tune) = tune;
      slot(st, ParameterId::vco_b_cv_amt) = 0.0;   // steady single carrier (see note above)
      Cap c = capture(st, sr, kWarm + kWin, "dry_b", 0.0);
      emit("vco_b_tri_" + std::to_string((int)sr) + "_" + std::to_string((int)ft),
           "vco_b_tri", sr, ft, c, "dry_b", "tri");
    }
  }

  // ---- VCO A HARD SYNC (GH#19 S5, task #111): LFO A's CV OUT cabled into vco_a.sync_in. ----
  // Stimulus provenance and the sample-exactness argument live on captureSync()/
  // syncMasterBaseHz() above. The slave is the SAME reachable VCO-A triangle arm as vco_a_tri
  // (identical oct/tune recipe, identical dry_a tap), so a sync cell differs from its vco_a_tri
  // sibling by exactly two things: the patch cable and the LFO A setting. The measured f0 on this
  // family is a DIAGNOSTIC only (a re-phased triangle's zero-crossing count is not its natural
  // frequency while the reset keeps truncating it) — the acceptance figure is the pinned-frequency
  // residual against an analytic band-limited composite, not this column.
  //
  // ITEM 2: every sync cell additionally carries the independent master-edge vs reset-frame
  // reconciliation (syncTimingReconcile above). A cell whose stimulus premise does not hold is NOT
  // produced — fail closed, exactly like silent/over-scale — because the analytic composite the gate
  // measures against assumes resets on the master grid, so a cell that violates that premise has NO
  // criterion to report. The reconciliation reuses the synced render, so it costs one extra render
  // per cell (the free-running reference), not two.
  for (double sr : kSrs) {
    for (double ft : {220.0, 440.0, 880.0}) {
      const DeviceStateV1 st = makeSyncState(sr, ft);
      const std::string cid = "vco_a_sync_tri_" + std::to_string((int)sr) + "_" +
                              std::to_string((int)ft);
      const SyncTiming t = syncTimingReconcile(st, sr, kWarm + kWin);
      std::fprintf(stderr, "SYNC-TIMING %s\n", syncTimingLine(cid, sr, t).c_str());
      Cap c = t.ok ? t.synced : Cap{};
      if (!t.ok) c.signal = "sync-timing: " + t.reason;
      emit(cid, "vco_a_sync_tri", sr, ft, c, "dry_a", "tri_sync");
    }
  }

  // ---- ITEM 5: the sync jack reaches VCO A and NOTHING ELSE. See isolateSyncFromB(). ----------
  // Run once per sample rate (the claim is about routing, not about a frequency), on a shorter
  // window than a cell: the question is "does the cable reach B at all", and 4096 frames already
  // span 8-16 master edges at every rate here. The legal A->B route is MUTED for the discriminating
  // comparison -- otherwise A's own sync-induced change reaches B legitimately and the assertion
  // would go RED on a correct product, which is exactly what the first version of this check did.
  // A failure is FAIL-CLOSED like any other probe-owned validity failure: the S5 premise ("the
  // hardware sync jack is VCO A's alone") would be false, so the sync cells' own reading of that
  // jack would no longer mean what the gate assumes.
  {
    bool bIsoFailed = false;
    for (double sr : kSrs) {
      const DeviceStateV1 st = makeSyncState(sr, 440.0);
      const BIsolation r = isolateSyncFromB(st, sr, 4096);
      std::fprintf(stderr, "SYNC-B-ISOLATION %s\n", bIsolationLine(sr, r).c_str());
      if (!r.ok) bIsoFailed = true;
    }
    if (bIsoFailed) gCode |= 32;
  }

  // ---- ITEM 4: the sync cable carried by the DeviceState, not by a post-load connect() ---------
  // One assertion per rate on the SAME stimulus the cells use; no cell is added, so the shipped
  // matrix and the committed baseline are untouched. Failure is fail-closed: if a saved patch does
  // not restore its sync cable, the user line does not exist and the slice's claim would be about a
  // test-only affordance.
  {
    bool cableFailed = false;
    for (double sr : kSrs) {
      const CableLine r = syncViaStateCable(makeSyncState(sr, 440.0), sr, kWarm + kWin);
      std::fprintf(stderr, "SYNC-STATE-CABLE %s\n", cableLine(sr, r).c_str());
      if (!r.ok) cableFailed = true;
    }
    if (cableFailed) gCode |= 64;
  }

  // ---- Classic drone gen-1 SINGLE carrier: mute_2..5=1 (only gen-1 audible), gate hold on. ----
  //
  // RULING B (@Kimi f260fd91): the raw this cell SHIPS is the probe-built bank's drift-OFF render,
  // not the runtime render -- the analytic phi pin is exact only where the accumulator rate is a
  // constant, and driftEnabled=false is unreachable from the product. The runtime render is still
  // taken, and `gh19_fidelity.tsv` asserts (a) runtime == self-built drift-ON bit for bit (the fork
  // detector: it is what proves the self-built bank is the same path) and (b) drift-ON != drift-OFF
  // (the non-vacuity control: it is what proves the shipped arm really is the drift-OFF arm). Both
  // are hard failures here and in the analyzer's probe-contract gate. See captureClassicSelf.
  // Roles are a property of the generator WITHIN a group (drone_bank.h:373-378). RULING ③
  // (@Kimi cd345dbb) requires a HIGH-role classic cell in the acceptance gate, so the same recipe is
  // run across three lanes: the LOW cell on drone_1's gen 1 (group 0, gen 0 -- the 30-120 Hz band),
  // and the HIGH cell on drone_4's gen 4 (group 2, gen 3, classicGroupOfDrone_(drone_4) == 2) at
  // TWO volt settings.
  //
  // `voltSemisDown` is the CLASSIC VOLT setting in semitones down, i.e. the value the runtime
  // derives from the *_volt knob via classicDroneVoltSemisDownFromNorm (machine_runtime.h:2791:
  // 60*n, so 0.5 -> 30 and 0.0 -> 0). It is a parameter rather than a constant because the HIGH
  // ROLE CELL MUST LAND IN ITS OWN BAND. This is the one place where "the same recipe at a
  // different setting" would be wrong: with the LOW lane's 30 semis down, drone_4's gen 3 lands at
  // 97-224 Hz, which duplicates the LOW band and makes the HIGH cell HIGH in name only. The probe's
  // first 12 HIGH rows did exactly that, and the honest reading of them is "a second LOW cell with
  // a different base frequency", not a HIGH-role cell (@Kimi e6e645d2 ruling 1, approving the
  // fix). At 0 semis down the same generators land at ~551/835/1266 Hz -- inside the 420-1800 Hz
  // kHigh band the role is DEFINED by. So `volt=0` is the volt setting that makes the HIGH lane an
  // actual HIGH lane; the 30-semis-down HIGH rows are kept (they are a valid LOW-adjacent probe of
  // the same accessor and cost nothing to keep) but they are NOT what discharges ruling ③.
  //
  // Consequence for the gate, stated here because it is easy to get backwards: the two HIGH volt
  // lanes are NOT interchangeable rows and their pathName prefix carries the distinction
  // (`drone4_classic` vs `drone4_classic_v0`). A cell id must say which volt it was rendered at,
  // or the baseline row and the acceptance row can be paired across lanes and every number will
  // still look plausible.
  auto classicCells = [&](const std::string& pathName, const std::string& tap, int group,
                          std::size_t gen, const ParameterId (&mute)[5], ParameterId tuneId,
                          ParameterId holdId, ParameterId voltId, double voltNorm,
                          double voltSemisDown) {
    for (double sr : kSrs) {
      for (double tuneNorm : {0.20, 0.50, 0.80}) {
        DeviceStateV1 st = make_default_device_state(kProbeSeed);
        for (int k = 0; k < 5; ++k)
          slot(st, mute[k]) = (static_cast<std::size_t>(k) == gen) ? 0.0 : 1.0;
        slot(st, holdId) = 1.0;
        slot(st, tuneId) = tuneNorm;
        // The VOLT knob must be set on the STATE as well, not only on the self-built bank: the
        // runtime capture below is what the fork detector compares against, and a state left at the
        // default 0.5 would render volt=30 while the self-built bank renders voltSemisDown. That
        // mismatch would fail the detector for the right reason but with the wrong diagnosis (looks
        // like a fork, is actually a recipe divergence), so the two are threaded from ONE argument.
        slot(st, voltId) = voltNorm;
        const std::string cid = pathName + "_" + std::to_string((int)sr) + "_t" +
                                std::to_string(std::lround(tuneNorm * 100));

        Cap c_rt = capture(st, sr, kWarm + kWin, tap, 0.0, "volt");
        ClassicSelf on = captureClassicSelf(group, gen, sr, tuneNorm, voltSemisDown,
                                            /*driftEnabled=*/true);
        ClassicSelf off = captureClassicSelf(group, gen, sr, tuneNorm, voltSemisDown,
                                             /*driftEnabled=*/false);

        // (a) fork detector: the runtime raw and the self-built drift-ON raw must be identical.
        double d_on = -1.0;
        bool same = c_rt.ok && on.cap.ok && c_rt.x.size() == on.cap.x.size();
        if (same) {
          double m = 0.0;
          for (std::size_t i = 0; i < c_rt.x.size(); ++i) {
            const double d = std::fabs(c_rt.x[i] - on.cap.x[i]);
            if (d > m) m = d;
          }
          d_on = m;
          same = (m == 0.0);
        }
        // (b) non-vacuity: flipping ONLY the drift flag must change the render.
        double d_off = -1.0;
        bool differs = false;
        if (on.cap.ok && off.cap.ok && on.cap.x.size() == off.cap.x.size()) {
          double m = 0.0;
          for (std::size_t i = 0; i < on.cap.x.size(); ++i) {
            const double d = std::fabs(on.cap.x[i] - off.cap.x[i]);
            if (d > m) m = d;
          }
          d_off = m;
          differs = (m > 0.0);
        }
        if (!same || !differs) {
          std::fprintf(stderr,
                       "FATAL %s: probe fidelity failed (bit_identical=%s, drift_off_differs=%s)\n",
                       cid.c_str(), same ? "YES" : "NO", differs ? "YES" : "NO");
          gCode |= 16;
        }
        frows.push_back(cid + "\t" + std::to_string(off.cap.x.size()) + "\t" + fmtNum(d_on, 6) + "\t" +
                        (same ? "YES" : "NO") + "\t" + fmtNum(d_off, 6) + "\t" +
                        (differs ? "YES" : "NO"));

        const std::string f0_model = (off.f0_model > 0.0) ? fmtNum(off.f0_model, 12) : std::string("-");
        emit(cid, pathName, sr, off.cap.f0, off.cap, tap, "sawcubic", f0_model);
      }
    }
  };

  // LOW role: drone_1 gen 1 (group 0, gen 0), volt knob at its runtime default 0.5 => 30 semis down.
  classicCells("drone1_classic", "drone1", 0, 0,
               {ParameterId::drone_1_mute_1, ParameterId::drone_1_mute_2, ParameterId::drone_1_mute_3,
                ParameterId::drone_1_mute_4, ParameterId::drone_1_mute_5},
               ParameterId::drone_1_tune_1, ParameterId::drone_1_gate_hold,
               ParameterId::drone_1_volt, /*voltNorm=*/0.5, /*voltSemisDown=*/30.0);
  // HIGH role at 30 semis down: drone_4 gen 4 (group 2, gen 3). Kept -- but this lane's generators
  // land at 97-224 Hz, inside the LOW band, so it does NOT discharge ruling ③ (see the block above).
  classicCells("drone4_classic", "drone4", 2, 3,
               {ParameterId::drone_4_mute_1, ParameterId::drone_4_mute_2, ParameterId::drone_4_mute_3,
                ParameterId::drone_4_mute_4, ParameterId::drone_4_mute_5},
               ParameterId::drone_4_tune_4, ParameterId::drone_4_gate_hold,
               ParameterId::drone_4_volt, /*voltNorm=*/0.5, /*voltSemisDown=*/30.0);
  // HIGH role, THE LANE THAT DISCHARGES RULING ③: same drone_4 gen 4, volt knob at 0.0 => 0 semis
  // down, so the excited generators land at ~551/835/1266 Hz inside the 420-1800 Hz kHigh band.
  // Rendered on the UNFIXED tree like every other baseline row -- that is what makes it a baseline.
  classicCells("drone4_classic_v0", "drone4", 2, 3,
               {ParameterId::drone_4_mute_1, ParameterId::drone_4_mute_2, ParameterId::drone_4_mute_3,
                ParameterId::drone_4_mute_4, ParameterId::drone_4_mute_5},
               ParameterId::drone_4_tune_4, ParameterId::drone_4_gate_hold,
               ParameterId::drone_4_volt, /*voltNorm=*/0.0, /*voltSemisDown=*/0.0);

  // ---- New-drone drone-3 Schmitt: clean periodic tone (pitch>0, fm/am/noise=0), 2 pitch levels. ----
  for (double sr : kSrs) {
    for (double pitchNorm : {0.20, 0.60}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      slot(st, ParameterId::drone_3_pitch) = pitchNorm;
      slot(st, ParameterId::drone_3_noise) = 0.0;
      slot(st, ParameterId::drone_3_fm) = 0.0;
      slot(st, ParameterId::drone_3_am) = 0.0;
      Cap c = capture(st, sr, kWarm + kWin, "drone3", 0.0, "volt");
      emit("drone3_schmitt_" + std::to_string((int)sr) + "_p" + std::to_string(std::lround(pitchNorm * 100)),
           "drone3_schmitt", sr, c.f0, c, "drone3", "schmitt");
    }
  }

  // ---- New-drone drone-6 Schmitt: clean periodic tone, single pitch level (p30). ----
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::drone_6_pitch) = 0.30;
    slot(st, ParameterId::drone_6_noise) = 0.0;
    slot(st, ParameterId::drone_6_fm) = 0.0;
    slot(st, ParameterId::drone_6_am) = 0.0;
    Cap c = capture(st, sr, kWarm + kWin, "drone6", 0.0, "volt");
    emit("drone6_schmitt_" + std::to_string((int)sr) + "_p30",
         "drone6_schmitt", sr, c.f0, c, "drone6", "schmitt");
  }

  // ---- Preamp tanh input stage on the WET bus, driven by an AC sine on ch1 (item ④). ----
  for (double sr : kSrs) {
    for (int lvl : {0, 1}) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      for (int c = 1; c <= 10; ++c) {
        const int base = static_cast<int>(ParameterId::mixer_ch1_vol);
        auto pid = static_cast<ParameterId>(base + (c - 1) * 2);  // ch(n)_vol stride = 2
        slot(st, pid) = 0.0;
      }
      // preamp channel index 6 -> mixer_ch7_vol (registry mixer_ch(n+1)_vol). Scale DOWN (0.5) so
      // the saturated tanh output stays inside the device-domain [1e-4, 0.55] scale guard.
      slot(st, ParameterId::mixer_ch7_vol) = 0.5;
      slot(st, ParameterId::preamp_gain) = (lvl == 0) ? 0.3 : 1.0;
      double amp = (lvl == 0) ? 0.2 : 0.6;   // device-normalized sine amplitude on ch1
      const double acHz = 440.0;
      Cap c = captureAc(st, sr, kWarm + kWin, amp, acHz, "wet_l");
      emit("preamp_ac_" + std::to_string((int)sr) + "_l" + std::to_string(lvl),
           "preamp_ac", sr, acHz, c, "wet_l", "tanh");
    }
  }

  // ---- Composite WET chain (default kVcfPath) — COMPOSITE cell, never a module attribution. ----
  for (double sr : kSrs) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    Cap c = capture(st, sr, kWarm + kWin, "wet_l", 0.0);
    emit("wet_chain_" + std::to_string((int)sr) + "_" + std::to_string((int)sr),
         "wet_chain", sr, c.f0, c, "wet_l", "composite");
  }

  // ---- Machine evidence: four-output finite/block-consistency, block-partition invariance, prep
  //      time, and a median/variance per-sample callback cost + real-block size. LOCAL evidence
  //      only — the mandate's CPU bound is "this machine, not a cross-machine budget". Written as a
  //      SEPARATE gh19_cpu.tsv so gh19_scenarios.tsv stays signal-only (the analyzer parses the 3rd
  //      field of a scenario row as "signal"; a cpu meta row there would be miscounted).
  {
    std::string machine = "unknown", sysrel = "unknown", compiler = "unknown";
#if !defined(_WIN32)
    struct utsname un;
    if (uname(&un) == 0) { machine = un.machine; sysrel = std::string(un.sysname) + " " + un.release; }
#else
    machine = "windows";
#endif
#ifdef _MSC_VER
    compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    compiler = __VERSION__;
#endif

    const double sr = kSrs[1];                    // 48000
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    slot(st, ParameterId::vco_a_oct_sel) = 1.0;   // 440 Hz clean VCO-A triangle
    slot(st, ParameterId::vco_a_tune) = 0.0;

    // (a) Four outputs finite + block-consistent across every sample rate. The harness render() only
    // returns true when EVERY processBlock is Rendered (block-consistency); the four shared block
    // size, so equal lengths + all-finite is the per-record contract. Any failure => exit code.
    long fourFail = 0; std::string fourNote = "clean";
    for (double s2 : kSrs) {
      EngineHarness h;
      if (!h.load(st, s2)) { ++fourFail; fourNote = "load-fail@" + std::to_string((int)s2); continue; }
      const int n = 4096;
      if (!h.render(n, 0.0)) { ++fourFail; fourNote = "render-fail@" + std::to_string((int)s2); continue; }
      if (!allFinite(h.dryA()) || !allFinite(h.dryB()) || !allFinite(h.wetL()) || !allFinite(h.wetR())) {
        ++fourFail; fourNote = "non-finite@" + std::to_string((int)s2); continue;
      }
      if (h.dryA().size() != static_cast<std::size_t>(n) || h.dryB().size() != static_cast<std::size_t>(n) ||
          h.wetL().size() != static_cast<std::size_t>(n) || h.wetR().size() != static_cast<std::size_t>(n)) {
        ++fourFail; fourNote = "len@" + std::to_string((int)s2); continue;
      }
    }
    if (fourFail) gCode |= 4;

    // (b) BLOCK-PARTITION invariance (BLOCK item ⑤): same state + same input, compare per-frame
    //      render() against a single renderBlock() (one processBlock call) across all four outputs.
    //      Any divergence is a real finding (a block-size-dependent DSP path). Requires frames <= the
    //      blockFrames the engine was loaded with, so both loads use blockFrames = frames = 4096.
    double bpMax = 0.0; std::string bpNote = "clean";
    {
      const int n = 4096;
      EngineHarness hf, hb;
      if (!hf.load(st, sr, n)) bpNote = "frame-load-fail";
      else if (!hb.load(st, sr, n)) bpNote = "block-load-fail";
      else if (!hf.render(n, 0.0)) bpNote = "frame-render-fail";
      else if (!hb.renderBlock(n, [](std::size_t, double& in0, double& in1) { in0 = 0.0; in1 = 0.0; }))
        bpNote = "block-render-fail";
      else {
        for (int ch = 0; ch < 4; ++ch) {
          const auto& a = hf.out(ch);
          const auto& b = hb.out(ch);
          if (a.size() != b.size()) { bpNote = "len@" + std::to_string(ch); break; }
          for (std::size_t i = 0; i < a.size(); ++i) {
            const double d = std::fabs(a[i] - b[i]);
            if (d > bpMax) { bpMax = d; }
          }
        }
        if (bpMax > 1e-9) { bpNote = "mismatch"; }
      }
      if (bpMax > 1e-9) gCode |= 8;
    }

    using Clock = std::chrono::steady_clock;
    // (c) preparation time: encode -> decode -> applyDeviceState (the serialization roundtrip).
    const int prepReps = 20;
    std::vector<double> prepMs; prepMs.reserve(prepReps);
    for (int i = 0; i < prepReps; ++i) {
      EngineHarness h;
      auto t0 = Clock::now();
      bool pok = h.load(st, sr);
      auto t1 = Clock::now();
      if (!pok) continue;
      prepMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    double prepMedian = medianOf(prepMs);

    // (d) single-thread per-frame HARNESS render-loop cost over a large render (per-frame
    //      processBlock(1) + the harness's input-feed lambda + per-frame output push + the sampled
    //      hook). This is the WHOLE harness render path per sample, NOT a pure processBlock cost —
    //      that is (e). Multiple reps => median + variance; buffer prep is OUTSIDE the timed region
    //      (reserve() + an un-timed warmup), so no realloc contaminates the measurement.
    const std::size_t N = 1ULL << 20;             // 1,048,576 frames
    const int cbReps = 7;
    std::vector<double> cbNs; cbNs.reserve(cbReps);
    for (int i = 0; i < cbReps; ++i) {
      EngineHarness h;
      if (!h.load(st, sr)) continue;
      h.reserve(N + (1ULL << 16));                // capacity for warmup + full window: no realloc
      if (!h.render(4096, 0.0)) continue;          // warmup (un-timed, primes cache)
      auto t0 = Clock::now();
      bool r = h.render(static_cast<int>(N), 0.0);
      auto t1 = Clock::now();
      if (!r) continue;
      cbNs.push_back(std::chrono::duration<double>(t1 - t0).count() * 1e9 / static_cast<double>(N));
    }
    double cbMed = medianOf(cbNs);
    double cbStd = stdevOf(cbNs, cbMed);
    double cbMsg = (cbMed > 0) ? (1e9 / cbMed) / 1e6 : -1.0;   // Msamples/s

    // (e) real-block CPU cost: time ONLY engine.processBlock on a NORMAL block size, repeated, with
    //     every allocation, the input fill and the output insert OUTSIDE the timed region (BLOCK item
    //     ③ — the old single renderBlock(1M) wrapped six vector allocs + input generation + output
    //     insert in the timed loop, so it was NOT a pure processBlock cost). Buffers are caller-owned
    //     (processPure does not allocate / feed / capture), reused across calls; the input is a
    //     constant zero pre-filled once. Median + stdev over `blkLanes` independent lanes.
    double cbBlockNs = -1.0; double cbBlockStd = -1.0; long blockSamples = 0;
    {
      const int block = 512;                       // normal audio block
      const int nRep = 4000;                       // processBlock calls per lane
      std::vector<double> cin0(block, 0.0), cin1(block, 0.0);     // input pre-filled OUTSIDE timing
      std::vector<double> cout0(block), cout1(block), cout2(block), cout3(block);
      const double* in[kInCh] = {cin0.data(), cin1.data()};
      double* devOut[kOutCh] = {cout0.data(), cout1.data(), cout2.data(), cout3.data()};
      const int blkLanes = 7;
      std::vector<double> lanes; lanes.reserve(blkLanes);
      for (int i = 0; i < blkLanes; ++i) {
        EngineHarness hB;
        if (!hB.load(st, sr, block, kInCh, kOutCh)) continue;
        if (hB.processPure(in, devOut, block) != StandaloneAudioEngine::Status::Rendered) continue;  // warmup
        auto t0 = Clock::now();
        bool ok = true;
        for (int r = 0; r < nRep; ++r) {
          if (hB.processPure(in, devOut, block) != StandaloneAudioEngine::Status::Rendered) { ok = false; break; }
        }
        auto t1 = Clock::now();
        if (ok) {
          const double sec = std::chrono::duration<double>(t1 - t0).count();
          lanes.push_back(sec * 1e9 / static_cast<double>(nRep * block));
        }
      }
      if (!lanes.empty()) {
        cbBlockNs = medianOf(lanes);
        cbBlockStd = stdevOf(lanes, cbBlockNs);
        blockSamples = static_cast<long>(block);
      }
    }

    std::vector<std::string> crows;
    crows.push_back("id\tmachine\tos\tcompiler\tvalue\tunit\tsr_hz");
    crows.push_back("cpu_four_output\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(fourFail) + "\tfinite_block_failcount\t" + std::to_string(sr));
    crows.push_back("cpu_four_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(fourNote) + "\tnote\t" + std::to_string(sr));
    crows.push_back("cpu_blockpartition\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(bpMax) + "\tblock_partition_maxdiff\t" + std::to_string(sr));
    crows.push_back("cpu_blockpartition_note\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + safeTsv(bpNote) + "\tnote\t" + std::to_string(sr));
    crows.push_back("cpu_prep\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(prepMedian) + "\tms\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbMed) + "\tns_per_sample_harness_render\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop_var\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbStd) + "\tns_per_sample_stdev\t" + std::to_string(sr));
    crows.push_back("cpu_render_loop_msps\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbMsg) + "\tMsamples_per_s\t" + std::to_string(sr));
    crows.push_back("cpu_block_ns\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbBlockNs) + "\tns_per_sample_pure_processblock\t" + std::to_string(sr));
    crows.push_back("cpu_block_var\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(cbBlockStd) + "\tns_per_sample_stdev\t" + std::to_string(sr));
    crows.push_back("cpu_block_size\t" + machine + "\t" + sysrel + "\t" + compiler +
                    "\t" + std::to_string(blockSamples) + "\tsamples_per_block\t" + std::to_string(sr));
    std::string cpuTsv = out + "/gh19_cpu.tsv";
    if (!writeTsv(cpuTsv, crows)) {
      std::fprintf(stderr, "FATAL: could not write %s\n", cpuTsv.c_str());
      gCode |= 2;
    }
    std::fprintf(stderr,
        "gh19_alias_probe cpu: four_output_fail=%ld (%s), blockpart_max=%.2e (%s), "
        "prep_median_ms=%.3f, callback_median=%.2f ns/s (stdev %.2f), "
        "callback_block=%.2f ns/s @ %ld samples, machine=%s/%s, compiler=%s\n",
        fourFail, fourNote.c_str(), bpMax, bpNote.c_str(), prepMedian, cbMed, cbStd,
        cbBlockNs, blockSamples, machine.c_str(), sysrel.c_str(), compiler.c_str());
  }

  std::string tsv = out + "/gh19_scenarios.tsv";
  if (!writeTsv(tsv, rows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str());
    gCode |= 2;
  }

  // Classic-cell provenance evidence (ruling B), in its own file so gh19_scenarios.tsv stays a
  // signal-cell record. The analyzer's probe-contract gate fails closed if this file is absent.
  std::string fidTsv = out + "/gh19_fidelity.tsv";
  if (!writeTsv(fidTsv, frows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", fidTsv.c_str());
    gCode |= 2;
  }

  std::size_t produced = 0, blocked = 0;
  for (const auto& r : rows) {
    if (r.rfind("id\t", 0) == 0) continue;
    auto c1 = r.find('\t'); auto c2 = r.find('\t', c1 + 1);
    auto c3 = r.find('\t', c2 + 1);
    if (c2 != std::string::npos && c3 != std::string::npos) {
      std::string sig = r.substr(c2 + 1, c3 - c2 - 1);
      if (sig.empty() || sig == "-") ++produced; else ++blocked;
    }
  }
  std::fprintf(stderr, "gh19_alias_probe: %zu cells produced, %zu blocked, exit_code=%d.\n",
               produced, blocked, gCode);
  return gCode;   // non-zero if any required cell not produced / raw-write / four-output / block
                  // mismatch / VCO-B sync-consumption leak (bit 32, item 5).
}
