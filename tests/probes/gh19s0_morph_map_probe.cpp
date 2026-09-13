// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19s0_morph_map_probe.cpp — Raft task #116 (GH #19 S0) measurement probe for the single-knob
// continuous waveform mapping candidate. It is a RUNNABLE CANDIDATE, not another inventory: it
// drives the REAL product chain and reports measured numbers.
//
// THE CHAIN (the same one the host uses — no stand-in, no hand-built oscillator):
//     make_default_device_state -> encode_device_state -> decode_device_state ->
//     StandaloneAudioEngine::applyDeviceState -> processBlock
// encapsulated by tests/host/test_engine_harness.h (EngineHarness), whose DRY_A / DRY_B output
// channels are the real VCO A / VCO B taps (device_layout.h DRY_A / DRY_B). The candidate mapping
// itself lives in core/include/lunar24/core/vco_wave_map.h and is switched on through the owner
// (SynthRuntime::setVcoWaveMap) — an experiment-only mapping switch, not a ParameterId, not in the
// registry, not persisted.
//
// WHAT IS MEASURED
//   M1  four sample rates (44100/48000/88200/96000) x full norm sweep: per-norm peak/RMS of the
//       REAL rendered VCO A and VCO B output, both candidates.
//   M2  interval boundaries: at each boundary the boundary VALUE must equal the closed-form node
//       shape, and both one-sided limits must exist — measured as |f(b +- d) - node| / d. Plus a
//       full-sweep continuity scan at a fixed phase. Continuity is decided by limits, not by eye.
//   M3  repeated restore of the SAME norm. State lane (codec->owner->render, reloaded 3x) must be
//       BIT-IDENTICAL: a fresh load restarts phase at 0. The live lane re-applies the same target 3x
//       inside ONE session; it CANNOT be bit-identical by construction, because it never resets phase
//       and the oscillator's period is not an integer number of samples (measured and reported: the
//       period is in the TSV), so aligning two captures by a zero crossing cannot make them bit-equal
//       either — the alignment itself quantises to the sample grid. The live lane is therefore judged
//       against a floor MEASURED FROM THE SAME CAPTURE: sliding the analysis window inside one repeat
//       rotates the phase across several full periods, and the largest rms deviation that produces is
//       this statistic's own phase-sampling sensitivity. The floor is printed next to every verdict.
//   M4  block partition 1 / 64 / 256 / irregular through the REAL block entry renderBlock():
//       identical samples. The mapping holds no cross-block state, so a partition may not change it.
//   M5  A/B independence, split by what the graph actually allows. The canonical product graph
//       carries the documented NORMALISED route vco_a.dry_out -> vco_b.cv_in (machine_definition.h:
//       "The active A->B normalised route ... makes vco_b.cv_in an ACYCLIC edge from vco_a.dry_out
//       ... reading A's LIVE published value the same frame"), so VCO B is audio-rate FREQUENCY
//       modulated by VCO A's output. Independence is asserted where it is observable:
//       (a) the morph COORDINATES never leak (exact readback, both directions);
//       (b) driving B must not move A's output at all (there is no B->A route) — bit-identical;
//       (c) driving A moves A, and B's own SUP must not move. A sup is invariant under every
//           reparameterisation of time, so it falsifies a change of B's waveform function without
//           being confounded by the FM route — which is exactly the confound that rules out the
//           amplitude histogram here (measured: B's period moves 7e-5 while its histogram moves
//           0.24, i.e. density distortion, not shape). Both the sup's verdict and its own measured
//           estimator floor are printed; the histogram is reported, never asserted.
//   M8  per-VCO mapping isolation with NO graph at all: driving one Vco's morph across the whole
//       range leaves a second, identically configured Vco bit-identical to an untouched third. This
//       is where the "the mapping is per-VCO state" claim is unconfounded; M5(c) is its chain echo.
//   M9  one full period at nine norms per candidate plus the legacy triangle, so the SHAPE (not just
//       the boundary values) can be reviewed for panel plausibility.
//   M6  the two named stretches reproduce the EXISTING implementations (kMorphSawInvSaw /
//       kMorphSineTriangle at morph = u) sample-for-sample, and the triangle node reproduces the
//       legacy pure-triangle path INCLUDING its BLAMP bit-for-bit through the real chain.
//   M7  the S0 silence: at the exact centre of saw->invSaw the blend is identically zero for every
//       phase, and the sweep carries NO gain compensation. Reported as measured; not normalised.
//
// NEGATIVE CONTROLS (each names the ONE check it must make fire, and each runs the SAME predicate
// as that check, so a green check is evidence rather than the absence of a check):
//   N1 the REAL chain on the legacy path (morph swept, `wave_` still pinned to kTriangle) is a
//      fixed-waveform source: M1's "the output moves with the norm" fires. That is the S0 defect
//      itself, measured through the product rather than modelled.
//   N2 a source that reads the OTHER side's coordinate: M5's coordinate check fires.
//   N3 a stepped (non-partition-of-unity) source: M2's continuity scan fires.
//   N4 a silence-free saw<->invSaw source: M7's "the S0 centre is silent" fires.
//
// PROVENANCE SPLIT (why the two kinds of expected value are NOT the same evidence):
//   * EXPECTED-FROM-CONVENTION (can falsify the candidate): the closed-form node values are written
//     out HERE from the documented phase convention at core/include/lunar24/core/vco.h:303-326
//     (a(p)=2p-1, tri(p)=4|p-0.5|-1, sin(p)=sin(2*pi*p), pulse(p)=(p<duty)?+1:-1).
//   * EXPECTED-FROM-MAP (proves WIRING only, never correctness): rendered audio vs the same
//     wave_map the DSP calls. Labelled as such wherever it is used.
//
// EXIT CODE: 0 = every check produced its documented verdict; non-zero = a check FAILED (a red is
// attributable by its printed id).
//
// OUTPUT: --out <dir>, default ./report/gh19s0-morph-map:
//   m1_sweep.tsv m2_bounds.tsv m3_restore.tsv m4_blocks.tsv m5_ab.tsv m6_reuse.tsv
//   m7_silence.tsv m8_isolation.tsv m9_waveform.tsv neg_controls.tsv

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <lunar24/core/device_state.h>
#include <lunar24/registry_ids.hpp>
#include <lunar24/core/state_default.h>
#include <lunar24/core/vco_wave_map.h>

#include "test_engine_harness.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

namespace {

using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::SynthRuntime;
using lunar24::core::Vco;
using lunar24::core::VcoControlMode;
using lunar24::core::VcoWaveMap;
using lunar24::testengine::EngineHarness;
using lunar24::testengine::peakOf;
namespace wm = lunar24::core::wave_map;

constexpr double kTwoPi = 6.28318530717958647692528676655900577;
constexpr double kDuty = 0.5;   // setShape's default; this probe never moves pw.
constexpr double pProbe = 0.3;  // a fixed probe phase (not a node, not a zero crossing).
constexpr std::uint64_t kSeed = 0x4C554E4152ULL;

// ------------------------------------------------------------------ framework --
int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* id, const std::string& msg) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL [%s] %s\n", id, msg.c_str());
  } else {
    std::printf("ok   [%s] %s\n", id, msg.c_str());
  }
}

std::string g_outDir = "report/gh19s0-morph-map";
std::FILE* openTsv(const char* name, const char* header) {
  std::error_code ec;
  std::filesystem::create_directories(g_outDir, ec);
  const std::string path = g_outDir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::printf("FAIL [io] cannot write %s\n", path.c_str());
    ++g_failures;
    return nullptr;
  }
  std::fprintf(f, "%s\n", header);
  return f;
}

// ------------------------------------------------------- state / param helpers --
double rmsOf(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (double x : v) s += x * x;
  return std::sqrt(s / static_cast<double>(v.size()));
}
bool sameSamples(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// Set a per-side morph through the CODEC+OWNER (whole-state) lane: encode -> decode -> apply. This
// is the path a restored file takes, so the value used is the persisted value.
bool loadWithMorph(EngineHarness& h, double sr, int blockFrames, double morphA, double morphB) {
  DeviceStateV1 st = lunar24::core::make_default_device_state(kSeed);
  st.parameters[static_cast<std::uint32_t>(ParameterId::vco_a_morph)] = morphA;
  st.parameters[static_cast<std::uint32_t>(ParameterId::vco_b_morph)] = morphB;
  return h.load(st, sr, blockFrames);
}

// The live lane: the same public producer entry the host's keyboard/MIDI producer uses.
void applyParamLive(SynthRuntime& rt, ParameterId pid, double v, std::uint64_t sample) {
  lunar24::core::ControlEvent ev{};
  ev.kind = lunar24::core::ControlEventKind::parameter;
  ev.parameter = pid;
  ev.value = static_cast<lunar24::core::SignalSample>(v);
  ev.sampleOffset = 0;
  ev.source = 1;
  ev.channel = 0;
  ev.noteId = 0;
  ev.producerSequence = static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid)) * 1000 + sample;
  rt.enqueueControlEvent(lunar24::core::TimedControlEvent{ev, sample});
}

// ------------------------------------------------- EXPECTED-FROM-CONVENTION ----
// Closed forms written HERE from the documented convention (vco.h:303-326). Independent of
// wave_map: if the map disagrees with these, the MAP is wrong.
double convSaw(double p) { return 2.0 * p - 1.0; }
double convInvSaw(double p) { return -(2.0 * p - 1.0); }
double convSine(double p) { return std::sin(kTwoPi * p); }
double convTri(double p) { return 4.0 * std::fabs(p - 0.5) - 1.0; }
double convPulse(double p, double duty) { return (p < duty) ? 1.0 : -1.0; }
double convNodeAt(int nodeIndex, double p, double duty) {
  switch (nodeIndex) {
    case 0: return convSaw(p);
    case 1: return convInvSaw(p);
    case 2: return convSine(p);
    case 3: return convTri(p);
    default: return convPulse(p, duty);
  }
}

// ------------------------------------------- the shared check PREDICATES --------
// Every real check and its negative control run the SAME predicate, so "the control is caught" is
// evidence about the check rather than about a lookalike written for the control.
using PhaseFn = std::function<double(double norm, double p)>;

// M1: does the REAL rendered output actually MOVE with the norm? (the S0 defect = it does not).
bool movesWithNorm(double spreadA, double spreadB) { return spreadA > 1e-3 && spreadB > 1e-3; }

// M2: is the value continuous in norm at a fixed phase? (a stepped map jumps.)
bool continuousInNorm(const PhaseFn& f, double* worstOut) {
  const double d = 1e-9;
  double worst = 0.0;
  const int n = 4000;
  for (int i = 0; i <= n; ++i) {
    const double norm = static_cast<double>(i) / n;
    const double lo = std::max(0.0, norm - d);
    const double hi = std::min(1.0, norm + d);
    worst = std::max(worst, std::fabs(f(hi, pProbe) - f(lo, pProbe)));
  }
  if (worstOut) *worstOut = worst;
  return worst < 1e-6;
}

// M5.coord: each side's morph coordinate must equal the value that was set for THAT side.
struct CoordProbe {
  double appliedA;
  double appliedB;
  double readA;
  double readB;
};
bool coordsMatchApplied(const CoordProbe& p) {
  return p.readA == p.appliedA && p.readB == p.appliedB;
}

// M7: is the saw<->invSaw centre exactly silent over a full phase sweep?
bool silentAtCentreOverPhase(const wm::Boundaries& c, const PhaseFn& f, double* peakOut) {
  const double mid = 0.5 * (c[0] + c[1]);
  double peak = 0.0;
  for (int j = 0; j < 512; ++j) {
    const double p = static_cast<double>(j) / 512.0;
    peak = std::max(peak, std::fabs(f(mid, p)));
  }
  if (peakOut) *peakOut = peak;
  return peak == 0.0;
}

// ---------------------------------------------------- NEGATIVE-CONTROL SOURCES --
// Isolated sources: NOT the product, routed nowhere. Each reproduces the ONE defect its check must
// catch. N1 needs no source of its own — it runs the REAL chain on the legacy path.

// N2: a cross-wired source — side A reads coordinate B and side B reads coordinate A. Modelled at
// exactly the granularity M5.coord measures: what each side's coordinate reads back as.
CoordProbe modelCrossWiredCoord(double appliedA, double appliedB) {
  return CoordProbe{appliedA, appliedB, appliedB, appliedA};
}

// N3: the node is picked by which half of the stretch the norm is in -> a step at every midpoint
// instead of a crossfade (i.e. NOT a partition of unity).
double negStepped(double norm, double p) {
  const wm::Position pos = wm::locate(wm::kRingEqual, norm);
  const int node = (pos.u < 0.5) ? pos.stretch : pos.stretch + 1;
  return convNodeAt(node, p, kDuty);
}

// N4: the only way to remove S0's silence is to change the PAIR — crossfade the saw with a
// HALF-CYCLE-SHIFTED saw instead of the inverted saw, so the midpoint is not zero. (A
// multiplicative gain cannot lift an identically-zero stretch, which is itself the finding.)
double negSilenceFreeS0(const wm::Boundaries& c, double norm, double p) {
  const wm::Position pos = wm::locate(c, norm);
  if (pos.stretch != wm::kStretchMorphSawInvSaw) return wm::sampleAt(c, norm, p, kDuty);
  const double shifted = convSaw(p + 0.5 - std::floor(p + 0.5));
  return (1.0 - pos.u) * convSaw(p) + pos.u * shifted;
}

// ---------------------------------------------------- phase-sampling floor -----
// Any window statistic of a periodic signal depends slightly on WHERE the window starts, because
// the sample grid lands on different phases. That dependence is a property of the WINDOW, not of
// the signal, so it can be measured from the SAME data: compare the statistic over windows shifted
// by a few samples inside ONE capture. The across-repeat difference of a settled live lane must not
// exceed that self-measured floor — otherwise the difference is signal, not phase sampling. This is
// what makes the live-lane comparison falsifiable without pretending it can be bit-exact.
double windowRmsAt(const std::vector<double>& x, std::size_t start, std::size_t n) {
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = x[start + i];
    s += v * v;
  }
  return std::sqrt(s / static_cast<double>(n));
}
// max |rms(shift s) - rms(shift 0)| over s = 0..maxShift-1, i.e. the window's own phase sensitivity.
double phaseSamplingFloor(const std::vector<double>& x, std::size_t n, std::size_t maxShift,
                         double* refRmsOut) {
  const double ref = windowRmsAt(x, 0, n);
  if (refRmsOut != nullptr) *refRmsOut = ref;
  double worst = 0.0;
  for (std::size_t s = 1; s < maxShift && s + n <= x.size(); ++s) {
    worst = std::max(worst, std::fabs(windowRmsAt(x, s, n) - ref));
  }
  return worst;
}

// The sampled SUP of a periodic signal has the same problem in a sharper form: the true sup is
// approached only as the sample grid lands nearer the waveform's peak, so a window's sup sits a
// little below the converged value. Sliding the window does NOT expose that here — sliding it by a
// few hundred samples leaves the closest approach unchanged — so the floor is measured by SPLITTING
// the capture instead. Every block's sup is a lower bound on the capture's sup, so the spread across
// blocks is a lower bound on how far any window's sup is from the converged value.
double windowPeakAt(const std::vector<double>& x, std::size_t start, std::size_t n) {
  double p = 0.0;
  for (std::size_t i = 0; i < n; ++i) p = std::max(p, std::fabs(x[start + i]));
  return p;
}
double supEstimatorFloor(const std::vector<double>& x, std::size_t blocks) {
  if (blocks < 2 || x.size() < blocks * 2) return 0.0;
  const std::size_t n = x.size() / blocks;
  double lo = windowPeakAt(x, 0, n);
  double hi = lo;
  for (std::size_t b = 1; b < blocks; ++b) {
    const double s = windowPeakAt(x, b * n, n);
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  return hi - lo;
}

// ------------------------------------------- phase/frequency-invariant shape ----
// Sub-sample mean period from linearly interpolated RISING zero crossings. Reports the frequency
// on a continuous scale instead of an integer crossing count, so a frequency effect can be told
// from a shape effect even when the crossing COUNT happens not to move. 0 if < 2 crossings.
double meanPeriodFrames(const std::vector<double>& x) {
  double first = 0.0, last = 0.0;
  int n = 0;
  for (std::size_t i = 1; i < x.size(); ++i) {
    if (x[i - 1] < 0.0 && x[i] >= 0.0) {
      const double span = x[i] - x[i - 1];
      const double pos =
          static_cast<double>(i - 1) + (span > 0.0 ? (-x[i - 1]) / span : 0.0);
      if (n == 0) first = pos;
      last = pos;
      ++n;
    }
  }
  if (n < 2) return 0.0;
  return (last - first) / static_cast<double>(n - 1);
}

// Total-variation distance between the amplitude histograms of two captures, on a FIXED ABSOLUTE
// bin range (no per-signal normalisation, so a gain change is not silently divided out).
// A monotone time warp — which is what an audio-rate frequency modulation does to a VCO's output —
// leaves the distribution of sample VALUES unchanged, so this number moves when the SHAPE moves and
// stays put when only the frequency or the phase moves. That is what makes it usable to tell the
// documented A->B CV route (frequency) apart from a mapping leak (shape).
double shapeDistance(const std::vector<double>& a, const std::vector<double>& b, int bins = 64) {
  if (a.empty() || a.size() != b.size()) return -1.0;
  constexpr double kLo = -1.5;
  constexpr double kHi = 1.5;
  std::vector<double> ha(static_cast<std::size_t>(bins), 0.0);
  std::vector<double> hb(static_cast<std::size_t>(bins), 0.0);
  auto fill = [&](const std::vector<double>& x, std::vector<double>& h) {
    for (const double v : x) {
      int k = static_cast<int>((v - kLo) / (kHi - kLo) * static_cast<double>(bins));
      if (k < 0) k = 0;
      if (k >= bins) k = bins - 1;
      h[static_cast<std::size_t>(k)] += 1.0;
    }
    for (double& c : h) c /= static_cast<double>(x.size());
  };
  fill(a, ha);
  fill(b, hb);
  double d = 0.0;
  for (int i = 0; i < bins; ++i) d += std::fabs(ha[static_cast<std::size_t>(i)] - hb[static_cast<std::size_t>(i)]);
  return 0.5 * d;
}

// ------------------------------------------------------------- the M1 sweep ----
struct SweepResult {
  double spreadA = 0.0;
  double spreadB = 0.0;
  double loA = 0.0;
  double hiA = 0.0;
  bool finite = true;
};

// The one sweep loop, shared by M1 (a ring candidate) and N1 (the legacy path = the S0 defect).
// `rmsSeriesOut`, when non-null, receives the per-norm rmsA values in sweep order.
SweepResult runSweep(double sr, int frames, int steps, VcoWaveMap mode, std::FILE* tsv,
                     const char* candName, std::vector<double>* rmsSeriesOut) {
  SweepResult r;
  double loA = 1e30, hiA = -1e30, loB = 1e30, hiB = -1e30;
  for (int i = 0; i < steps; ++i) {
    const double norm = static_cast<double>(i) / static_cast<double>(steps - 1);
    EngineHarness h;
    if (!loadWithMorph(h, sr, 256, norm, norm)) {
      check(false, "M1.load", "state load rejected during a sweep");
      r.finite = false;
      return r;
    }
    if (SynthRuntime* rt = h.producerRuntime()) rt->setVcoWaveMap(mode);
    h.reserve(static_cast<std::size_t>(frames));
    if (!h.render(frames)) {
      check(false, "M1.render", "render failed during a sweep");
      r.finite = false;
      return r;
    }
    const double pkA = peakOf(h.dryA()), rmA = rmsOf(h.dryA());
    const double pkB = peakOf(h.dryB()), rmB = rmsOf(h.dryB());
    loA = std::min(loA, rmA);
    hiA = std::max(hiA, rmA);
    loB = std::min(loB, rmB);
    hiB = std::max(hiB, rmB);
    if (rmsSeriesOut != nullptr) rmsSeriesOut->push_back(rmA);
    if (!std::isfinite(pkA) || !std::isfinite(rmA) || !std::isfinite(pkB) || !std::isfinite(rmB)) {
      r.finite = false;
    }
    if (tsv != nullptr) {
      std::fprintf(tsv, "%.0f\t%s\t%.4f\t%.9f\t%.9f\t%.9f\t%.9f\n", sr, candName, norm, pkA, rmA,
                   pkB, rmB);
    }
  }
  r.loA = loA;
  r.hiA = hiA;
  r.spreadA = hiA - loA;
  r.spreadB = hiB - loB;
  return r;
}

// ============================================================== main ==========
int realMain(int argc, char** argv) {
  int sweepFrames = 1024;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      g_outDir = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      sweepFrames = std::atoi(argv[++i]);
    }
  }
  std::printf("gh19s0_morph_map_probe: out=%s sweep_frames=%d\n", g_outDir.c_str(), sweepFrames);

  const wm::Boundaries kCand[2] = {wm::kRingEqual, wm::kRingPanel};
  const char* kCandName[2] = {"equal", "panel"};
  const VcoWaveMap kMode[2] = {VcoWaveMap::kRingEqual, VcoWaveMap::kRingPanel};
  const double kRates[4] = {44100.0, 48000.0, 88200.0, 96000.0};
  const int kSteps = 41;

  check(wm::boundariesValid(kCand[0]), "C0a",
        "equal-division boundaries strictly increasing, 0.0 .. 1.0");
  check(wm::boundariesValid(kCand[1]), "C0b",
        "panel-derived boundaries strictly increasing, 0.0 .. 1.0");

  // ---------------------------------------------------------------- M1 sweep --
  {
    std::FILE* f = openTsv("m1_sweep.tsv", "sr\tcandidate\tnorm\tpeakA\trmsA\tpeakB\trmsB");
    std::vector<double> series[4][2];  // [rate][candidate] -> per-norm rmsA, recorded as measured
    for (int ci = 0; ci < 2; ++ci) {
      for (int ri = 0; ri < 4; ++ri) {
        const double sr = kRates[ri];
        const SweepResult r =
            runSweep(sr, sweepFrames, kSteps, kMode[ci], f, kCandName[ci], &series[ri][ci]);
        char id[40], msg[288];
        std::snprintf(id, sizeof id, "M1.%s.%.0f", kCandName[ci], sr);
        std::snprintf(msg, sizeof msg,
                      "sr=%.0f %s: rendered output MOVES with the norm (rmsA %.6f in [%.6f,%.6f]; rmsB spread %.6f)",
                      sr, kCandName[ci], r.spreadA, r.loA, r.hiA, r.spreadB);
        check(movesWithNorm(r.spreadA, r.spreadB), id, msg);
        std::snprintf(id, sizeof id, "M1f.%s.%.0f", kCandName[ci], sr);
        std::snprintf(msg, sizeof msg, "sr=%.0f %s: every rendered sample finite", sr,
                      kCandName[ci]);
        check(r.finite, id, msg);
      }
    }
    std::fclose(f);

    // The boundary placement must be AUDIBLE, or the two candidates would be the same candidate.
    // Compared row-by-row on the numbers just recorded (same norm index, same rate).
    double worst = 0.0;
    bool havePairs = true;
    for (int ri = 0; ri < 4; ++ri) {
      if (series[ri][0].size() != static_cast<std::size_t>(kSteps) ||
          series[ri][1].size() != static_cast<std::size_t>(kSteps)) {
        havePairs = false;
        break;
      }
      for (int i = 0; i < kSteps; ++i) {
        worst = std::max(worst, std::fabs(series[ri][0][static_cast<std::size_t>(i)] -
                                         series[ri][1][static_cast<std::size_t>(i)]));
      }
    }
    if (!havePairs) {
      check(false, "M1c",
            "equal vs panel: the sweep series are incomplete, so the audible-difference claim has NO evidence");
    } else {
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "equal vs panel boundaries are audibly different (max |rmsA diff| over all 4 rates and 41 norms = %.6f) -> the boundary choice matters",
                    worst);
      check(worst > 1e-3, "M1c", msg);
    }
  }

  // ----------------------------------------------------------- M2 boundaries --
  {
    std::FILE* f = openTsv(
        "m2_bounds.tsv",
        "candidate\tboundary\tleft_d1e-6\tright_d1e-6\tslope_left\tslope_right\tat_node\tnode_expected");
    const double d = 1e-6;
    const double kSlopeBound = 100.0;  // a bounded one-sided derivative => the limit exists
    for (int ci = 0; ci < 2; ++ci) {
      const wm::Boundaries& cb = kCand[ci];
      double worstSlope = 0.0, worstNode = 0.0;
      for (int k = 0; k < wm::kBoundaryCount; ++k) {
        const double b = cb[static_cast<std::size_t>(k)];
        const double lo = std::max(0.0, b - d);
        const double hi = std::min(1.0, b + d);
        const double exp = convNodeAt(k, pProbe, kDuty);
        const double vl = wm::sampleAt(cb, lo, pProbe, kDuty);
        const double vr = wm::sampleAt(cb, hi, pProbe, kDuty);
        const double at = wm::sampleAt(cb, b, pProbe, kDuty);
        const double sl = (b - lo) > 0.0 ? std::fabs(vl - exp) / (b - lo) : 0.0;
        const double sr2 = (hi - b) > 0.0 ? std::fabs(vr - exp) / (hi - b) : 0.0;
        worstSlope = std::max(worstSlope, std::max(sl, sr2));
        worstNode = std::max(worstNode, std::fabs(at - exp));
        std::fprintf(f, "%s\t%.4f\t%.12f\t%.12f\t%.4f\t%.4f\t%.12f\t%.12f\n", kCandName[ci], b, vl,
                     vr, sl, sr2, at, exp);
      }
      char id[40], msg[288];
      std::snprintf(id, sizeof id, "M2node.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "%s: each boundary VALUE equals the CLOSED-FORM node shape (worst err %.3e)",
                    kCandName[ci], worstNode);
      check(worstNode < 1e-15, id, msg);
      std::snprintf(id, sizeof id, "M2lim.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "%s: both one-sided limits exist at EVERY boundary (worst |f(b+-d)-node|/d = %.3f, bound %.0f)",
                    kCandName[ci], worstSlope, kSlopeBound);
      check(worstSlope < kSlopeBound, id, msg);

      // The SAME predicate the N3 control fires, over the whole sweep at a fixed phase.
      const wm::Boundaries& cref = cb;
      PhaseFn real = [&cref](double norm, double p) { return wm::sampleAt(cref, norm, p, kDuty); };
      double worstJump = 0.0;
      const bool cont = continuousInNorm(real, &worstJump);
      std::snprintf(id, sizeof id, "M2cont.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "%s: continuous in norm over the WHOLE sweep at a fixed phase (worst jump %.3e over 4000 steps)",
                    kCandName[ci], worstJump);
      check(cont, id, msg);
    }
    std::fclose(f);
  }

  // -------------------------------------------------------------- M3 restore ---
  {
    std::FILE* f = openTsv(
        "m3_restore.tsv",
        "lane\tnorm\trepeat\tpeakA\trmsA\tperiod_frames\tphase_floor\tsame_or_phase_invariant");
    const double norm = 0.625;

    // State lane: three independent codec->owner->render loads of the SAME persisted value.
    {
      std::vector<double> first;
      for (int rep = 0; rep < 3; ++rep) {
        EngineHarness h;
        if (!loadWithMorph(h, 48000.0, 256, norm, norm)) {
          check(false, "M3.load", "state-lane load rejected");
          break;
        }
        h.reserve(1024);
        if (!h.render(1024)) {
          check(false, "M3.render", "state-lane render failed");
          break;
        }
        const std::vector<double>& got = h.dryA();
        if (rep == 0) first = got;
        const bool same = sameSamples(got, first);
        std::fprintf(f, "state\t%.4f\t%d\t%.9f\t%.9f\t%.3f\t%.6e\t%d\n", norm, rep, peakOf(got),
                     rmsOf(got), meanPeriodFrames(got), 0.0, same ? 1 : 0);
        if (rep > 0) {
          char msg[256];
          std::snprintf(msg, sizeof msg,
                        "state lane: restore #%d of the SAME norm=%.3f is BIT-IDENTICAL to #0 (%zu frames)",
                        rep, norm, got.size());
          check(same, "M3.state", msg);
        }
      }
    }

    // Live lane: re-apply the SAME target three times inside ONE session. Bit-identity is NOT
    // achievable here, and that is a property of the LANE, not of the candidate: the live lane never
    // resets phase, and the oscillator period is not an integer number of samples (measured below),
    // so every capture lands on a different fractional phase. Aligning two captures by their first
    // zero crossing therefore cannot make them bit-equal either — the alignment itself quantises to
    // the sample grid. The honest comparison is against a floor measured FROM THE SAME CAPTURE:
    // sliding the analysis window inside ONE repeat rotates the phase across several full periods,
    // so the largest rms deviation that produces IS this statistic's phase-sampling sensitivity.
    // Repeats must not differ by more than that self-measured floor. The floor is printed, so the
    // bound is visible evidence rather than a magic constant.
    {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 4096, 0.5, 0.5)) {
        check(false, "M3.load.live", "live-lane load rejected");
      } else {
        SynthRuntime* rt = h.producerRuntime();
        constexpr std::size_t kLiveStat = 8192;   // samples the rms statistic is computed over
        constexpr std::size_t kLiveShift = 1024;  // window starts scanned; >= 4 full periods
        double rms0 = 0.0, peak0 = 0.0, floor0 = 0.0;
        bool readbackOk = true;
        for (int rep = 0; rep < 3; ++rep) {
          h.reserve(65536);
          const std::uint64_t at = static_cast<std::uint64_t>(h.dryA().size()) + 1;
          applyParamLive(*rt, ParameterId::vco_a_morph, norm, at);
          applyParamLive(*rt, ParameterId::vco_b_morph, norm, at);
          if (!h.render(16384)) {  // settle: the EXISTING smoother still acts on this coordinate
            check(false, "M3.render.live", "live-lane settle render failed");
            break;
          }
          const std::size_t before = h.dryA().size();
          if (!h.render(16384)) {
            check(false, "M3.render.live", "live-lane capture render failed");
            break;
          }
          if (rt->vcoAMorph() != norm || rt->vcoBMorph() != norm) readbackOk = false;
          const std::vector<double> win(h.dryA().begin() + static_cast<std::ptrdiff_t>(before),
                                        h.dryA().end());
          const double pk = peakOf(win);
          double refRms = 0.0;
          const double r = windowRmsAt(win, 0, kLiveStat);
          const double floor = phaseSamplingFloor(win, kLiveStat, kLiveShift, &refRms);
          const double period = meanPeriodFrames(win);
          bool invariant = true;
          if (rep == 0) {
            rms0 = r;
            peak0 = pk;
            floor0 = floor;
          } else {
            // The window's own phase sensitivity, not a hand-picked epsilon, is the bound. If a
            // repeat deviates by MORE than the phase rotation can explain, that is real signal.
            const double bound = std::max(floor, floor0);
            const double d = std::fabs(r - rms0);
            const bool rmsOk = d <= bound;
            const bool peakOk = std::fabs(pk - peak0) <= 1e-9;
            invariant = rmsOk && peakOk;
            char msg[384];
            std::snprintf(msg, sizeof msg,
                          "live lane: repeat %d of the SAME norm=%.3f settles to the same sound "
                          "(|d rms| %.3e vs the phase-sampling floor %.3e measured from this very "
                          "capture, |d peak| %.3e, period %.3f frames)",
                          rep, norm, d, bound, std::fabs(pk - peak0), period);
            check(invariant, "M3.live", msg);
          }
          std::fprintf(f, "live\t%.4f\t%d\t%.9f\t%.9f\t%.3f\t%.6e\t%d\n", norm, rep, pk, r, period,
                       floor, invariant ? 1 : 0);
        }
        check(readbackOk, "M3.readback",
              "live lane: the coordinate readback equals the applied target exactly on every repeat");
      }
    }
    std::fclose(f);
  }

  // --------------------------------------------------------------- M4 blocks ---
  // The REAL block entry renderBlock(): the engine is loaded with blockFrames = 4096 and each
  // schedule drives it with exactly those block sizes, so the block size is genuinely varied (the
  // per-frame render() cannot test a partition).
  {
    std::FILE* f = openTsv("m4_blocks.tsv", "schedule\tframes\trmsA\tmaxabs_vs_1frame\tsame");
    const int total = 4096;
    const double norm = 0.375;
    auto renderBlocks = [&](const std::vector<int>& pat, std::vector<double>* outA) -> bool {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 4096, norm, norm)) return false;
      if (SynthRuntime* rt = h.producerRuntime()) rt->setVcoWaveMap(VcoWaveMap::kRingEqual);
      outA->clear();
      int done = 0;
      std::size_t k = 0;
      while (done < total) {
        int n = pat[k % pat.size()];
        ++k;
        if (n > total - done) n = total - done;
        if (!h.renderBlock(n, [](std::size_t, double& i0, double& i1) {
              i0 = 0.0;
              i1 = 0.0;
            })) {
          return false;
        }
        done += n;
      }
      *outA = h.dryA();
      return true;
    };

    std::vector<double> reference;
    const bool refOk = renderBlocks({1}, &reference);
    check(refOk, "M4.ref", "the 1-frame reference schedule rendered");
    std::fprintf(f, "1\t%zu\t%.9f\t%.3e\t1\n", reference.size(), rmsOf(reference), 0.0);

    const std::vector<std::vector<int>> schedules = {{64}, {256}, {7, 100, 33, 250, 61}};
    const char* names[3] = {"64", "256", "irregular(7,100,33,250,61)"};
    for (int s = 0; s < 3; ++s) {
      std::vector<double> got;
      const bool ok = renderBlocks(schedules[static_cast<std::size_t>(s)], &got);
      const bool same = ok && refOk && sameSamples(got, reference);
      double worst = 0.0;
      if (ok && got.size() == reference.size()) {
        for (std::size_t i = 0; i < got.size(); ++i)
          worst = std::max(worst, std::fabs(got[i] - reference[i]));
      }
      char id[40], msg[288];
      std::snprintf(id, sizeof id, "M4.%d", s);
      std::snprintf(msg, sizeof msg,
                    "block schedule %s renders BIT-IDENTICALLY to the 1-frame reference (%zu frames, max|diff| = %.3e)",
                    names[s], got.size(), worst);
      check(same, id, msg);
      std::fprintf(f, "%s\t%zu\t%.9f\t%.3e\t%d\n", names[s], got.size(), rmsOf(got), worst,
                   same ? 1 : 0);
    }
    std::fclose(f);
  }

  // ----------------------------------------------------------------- M5 A/B ---
  // The graph's A->B normalised route (vco_a.dry_out -> vco_b.cv_in) makes B's FREQUENCY follow
  // A's audio output, so "B's rms must not move when A is driven" is NOT a valid independence
  // criterion on this product. The valid criteria are the three below; the third is the one that
  // still falsifies a mapping leak.
  {
    std::FILE* f = openTsv(
        "m5_ab.tsv",
        "case\tdrive\tnormA\tnormB\trmsA\trmsB\tpeakA\tpeakB\tpfloorA\tpfloorB\tperiodA\tperiodB\tshapeB_vs_base\treadbackA\treadbackB");
    struct Run {
      double a;
      double b;
      bool driveA;
    };
    const Run runs[3] = {{0.5, 0.5, true}, {0.9, 0.5, true}, {0.5, 0.9, false}};
    constexpr std::size_t kSupBlocks = 16;  // blocks the sup estimator's own resolution is measured over
    double rms[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    double peak[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    double pfloor[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    double per[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    double rb[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    std::vector<double> outA[3], outB[3];
    bool rendered = true;
    for (int i = 0; i < 3; ++i) {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 256, runs[i].a, runs[i].b)) {
        check(false, "M5.load", "A/B load rejected");
        rendered = false;
        continue;
      }
      SynthRuntime* rt = h.producerRuntime();
      if (rt != nullptr) {
        rt->setVcoWaveMap(VcoWaveMap::kRingEqual);
        rb[i][0] = rt->vcoAMorph();
        rb[i][1] = rt->vcoBMorph();
      }
      h.reserve(8192);
      if (!h.render(8192)) {
        check(false, "M5.render", "A/B render failed");
        rendered = false;
      }
      outA[i] = h.dryA();
      outB[i] = h.dryB();
      rms[i][0] = rmsOf(outA[i]);
      rms[i][1] = rmsOf(outB[i]);
      peak[i][0] = peakOf(outA[i]);
      peak[i][1] = peakOf(outB[i]);
      // The sup is read over the whole capture and the estimator's own resolution is measured from
      // the same capture by splitting it (see supEstimatorFloor).
      pfloor[i][0] = supEstimatorFloor(outA[i], kSupBlocks);
      pfloor[i][1] = supEstimatorFloor(outB[i], kSupBlocks);
      per[i][0] = meanPeriodFrames(outA[i]);
      per[i][1] = meanPeriodFrames(outB[i]);
    }
    double shapeB[3] = {-1.0, -1.0, -1.0};
    if (rendered) {
      for (int i = 0; i < 3; ++i) shapeB[i] = shapeDistance(outB[i], outB[0]);
    }
    for (int i = 0; i < 3; ++i) {
      std::fprintf(f,
                   "%s\t%s\t%.3f\t%.3f\t%.9f\t%.9f\t%.9f\t%.9f\t%.3e\t%.3e\t%.3f\t%.3f\t%.6f\t%.6f\t%.6f\n",
                   i == 0 ? "base" : "run", i == 0 ? "-" : (runs[i].driveA ? "A" : "B"), runs[i].a,
                   runs[i].b, rms[i][0], rms[i][1], peak[i][0], peak[i][1], pfloor[i][0],
                   pfloor[i][1], per[i][0], per[i][1], shapeB[i], rb[i][0], rb[i][1]);
    }

    // (a) no coordinate leak, in EITHER direction: the readback is exactly the value that was set.
    bool coordsExact = rendered;
    for (int i = 0; i < 3; ++i) {
      if (!coordsMatchApplied(CoordProbe{runs[i].a, runs[i].b, rb[i][0], rb[i][1]}))
        coordsExact = false;
    }
    check(coordsExact, "M5.coord",
          "driving one side never moves the OTHER side's morph coordinate (readback exact in all 3 runs)");

    // (b) driving B must not move A at all: the graph has no B->A route, so this direction is a
    // clean output-independence test (and A's rms equality is exact, not approximate).
    {
      const bool aMoved = std::fabs(rms[2][0] - rms[0][0]) > 1e-6;
      const bool bMoved = std::fabs(rms[2][1] - rms[0][1]) > 1e-6;
      char msg[288];
      std::snprintf(msg, sizeof msg,
                    "driving B (A=0.50 B=0.90): B moved=%d, A moved=%d (|d rmsA| = %.3e) -> A is untouched",
                    bMoved ? 1 : 0, aMoved ? 1 : 0, std::fabs(rms[2][0] - rms[0][0]));
      check(!aMoved && bMoved, "M5.B", msg);
    }

    // (c) driving A does move A; and what it does to B is REPORTED, not asserted away.
    {
      const bool aMoved = std::fabs(rms[1][0] - rms[0][0]) > 1e-6;
      char msg[400];
      std::snprintf(msg, sizeof msg,
                    "driving A (A=0.90 B=0.50): A moved=%d (|d rmsA| %.3e, |d peakA| %.3e); B moved by "
                    "|d rms| %.3e with period %.3f -> %.3f frames (A->B route) and amplitude-histogram "
                    "distance %.4f",
                    aMoved ? 1 : 0, std::fabs(rms[1][0] - rms[0][0]), std::fabs(peak[1][0] - peak[0][0]),
                    std::fabs(rms[1][1] - rms[0][1]), per[0][1], per[1][1], shapeB[1]);
      check(aMoved, "M5.A.drive", msg);
    }

    // (d) THE FALSIFIABLE HALF, and the reason M5's earlier shape criterion was withdrawn.
    // Measured here: B's PERIOD barely moves (218.182 -> 218.198, 7e-5 relative) while B's rms moves
    // 14% and its amplitude histogram moves 0.246. That combination is the signature of a ZERO-MEAN
    // audio-rate frequency modulation - which is exactly what the documented vco_a.dry_out ->
    // vco_b.cv_in route does - and NOT a pitch shift. It also proves the histogram is unusable as a
    // shape test here: deep FM makes the phase non-monotone, and a non-monotone reparameterisation
    // changes how long the signal spends at each value, so the histogram moves even though the
    // waveform function did not.
    // The one quantity that IS invariant under ANY reparameterisation of time is the SUP of the
    // signal: x(t) = W(phi(t)) visits exactly the values of W, whatever phi does. So B's peak must
    // not move when A is driven, while B's own drive must move it (the control that keeps this from
    // passing vacuously). This is asserted; the histogram is reported only.
    if (rendered) {
      const double dB = std::fabs(peak[1][1] - peak[0][1]);
      const double boundB = std::max(pfloor[0][1], pfloor[1][1]);
      const bool bPeakHeld = dB <= boundB;
      const double dOwn = std::fabs(peak[2][1] - peak[0][1]);
      const bool bPeakMovedByOwnDrive = dOwn > boundB;
      char msg[440];
      std::snprintf(msg, sizeof msg,
                    "driving A leaves B's SUP untouched (|d peakB| %.3e <= its own measured grid "
                    "floor %.3e) - a sup is invariant under every reparameterisation of time, so this "
                    "falsifies a waveform-function change on B without being confounded by the FM "
                    "route - while driving B does move it (|d peakB| %.3e, %.0fx the floor). The "
                    "histogram movement is FM density distortion, reported not asserted.",
                    dB, boundB, dOwn, boundB > 0.0 ? dOwn / boundB : 0.0);
      check(bPeakHeld && bPeakMovedByOwnDrive, "M5.peak", msg);

      char msg2[288];
      std::snprintf(msg2, sizeof msg2,
                    "driving B leaves A BIT-IDENTICAL and shape-identical (|d rmsA| %.3e, |d peakA| %.3e)",
                    std::fabs(rms[2][0] - rms[0][0]), std::fabs(peak[2][0] - peak[0][0]));
      check(sameSamples(outA[2], outA[0]), "M5.shape.B", msg2);
    }
    std::fclose(f);
  }

  // -------------------------------------------- M8 per-VCO mapping isolation ---
  // Where the isolation claim is UNCONFOUNDED: on the Vco objects themselves, with no graph, no
  // route and no FM. Two instances are configured identically; one has its morph coordinate driven
  // across the entire range; the other's emitted samples must stay BIT-IDENTICAL to a third instance
  // that was never touched. A cross-VCO channel in the mapping (shared or static state, ambient
  // influence) would fire this. The last instance at the base morph is the non-vacuity control: the
  // driven instance must actually emit something different.
  {
    std::FILE* f = openTsv("m8_isolation.tsv", "normA\tsame\tmaxAbsDiffA\tisolated");
    const double kBaseMorph = 0.5;
    const double morphs[] = {0.0, 0.125, 0.25, 0.5, 0.625, 0.75, 0.9, 1.0};
    bool allIsolated = true, allMoved = true;
    for (const double ma : morphs) {
      Vco a(48000.0), aRef(48000.0), b(48000.0), bRef(48000.0);
      Vco* const all[4] = {&a, &aRef, &b, &bRef};
      for (Vco* v : all) {
        v->setWaveMap(VcoWaveMap::kRingEqual);
        v->setBaseHz(220.0);
        v->setVoct(0.0);
        v->setCvInput(0.0, VcoControlMode::kLinear);
        v->setShape(kDuty);
      }
      a.setMorph(ma);
      aRef.setMorph(kBaseMorph);
      b.setMorph(kBaseMorph);
      bRef.setMorph(kBaseMorph);
      double maxA = 0.0;
      bool same = true;
      const int n = 4096;
      for (int i = 0; i < n; ++i) {
        double sa = 0.0, saRef = 0.0, sb = 0.0, sbRef = 0.0;
        a.tick(&sa);
        aRef.tick(&saRef);
        b.tick(&sb);
        bRef.tick(&sbRef);
        maxA = std::max(maxA, std::fabs(sa - saRef));
        if (sb != sbRef) same = false;
      }
      if (!same) allIsolated = false;
      if (ma != kBaseMorph && maxA <= 0.0) allMoved = false;
      std::fprintf(f, "%.4f\t%d\t%.9e\t%d\n", ma, same ? 1 : 0, maxA, (same && maxA >= 0.0) ? 1 : 0);
    }
    char msg[352];
    std::snprintf(msg, sizeof msg,
                  "driving one Vco's morph across its whole range leaves a second, identically "
                  "configured Vco BIT-IDENTICAL to an untouched third (8 morph values x 4096 samples), "
                  "while the driven Vco's own output does change -> the mapping has no cross-VCO channel");
    check(allIsolated && allMoved, "M8.isolation", msg);
    std::fclose(f);
  }

  // ---------------------------------------------------------------- M6 reuse --
  {
    std::FILE* f = openTsv("m6_reuse.tsv", "item\tvalue\tworst_err");
    // (i) the two named stretches equal the EXISTING closed forms at morph = u.
    double worstSaw = 0.0, worstST = 0.0;
    for (int i = 0; i <= 100; ++i) {
      const double u = static_cast<double>(i) / 100.0;
      for (int j = 0; j < 97; ++j) {
        const double p = static_cast<double>(j) / 97.0;
        worstSaw = std::max(worstSaw, std::fabs(wm::morphSawInvSaw(u, p) -
                                               ((1.0 - u) * convSaw(p) + u * convInvSaw(p))));
        worstST = std::max(worstST, std::fabs(wm::morphSineTriangle(u, p) -
                                              ((1.0 - u) * convSine(p) + u * convTri(p))));
      }
    }
    check(worstSaw == 0.0, "M6.saw",
          "S0 equals the existing kMorphSawInvSaw closed form at morph = u (bit-exact)");
    check(worstST == 0.0, "M6.sintri",
          "S2 equals the existing kMorphSineTriangle closed form at morph = u (bit-exact)");
    std::fprintf(f, "S0_vs_kMorphSawInvSaw\t-\t%.3e\n", worstSaw);
    std::fprintf(f, "S2_vs_kMorphSineTriangle\t-\t%.3e\n", worstST);

    // (ii) the triangle NODE reproduces the legacy pure-triangle path bit-for-bit through the chain.
    const double triNode = wm::kRingEqual[3];  // 0.75 under this node order
    std::vector<double> legacyA, ringA;
    bool triOk = true;
    {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 256, triNode, triNode)) {
        check(false, "M6.load.legacy", "legacy triangle-node load rejected");
        triOk = false;
      }
      h.reserve(4096);
      if (!h.render(4096)) {
        check(false, "M6.render.legacy", "legacy triangle-node render failed");
        triOk = false;
      }
      legacyA = h.dryA();
    }
    {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 256, triNode, triNode)) {
        check(false, "M6.load.ring", "ring triangle-node load rejected");
        triOk = false;
      }
      if (SynthRuntime* rt = h.producerRuntime()) rt->setVcoWaveMap(VcoWaveMap::kRingEqual);
      h.reserve(4096);
      if (!h.render(4096)) {
        check(false, "M6.render.ring", "ring triangle-node render failed");
        triOk = false;
      }
      ringA = h.dryA();
    }
    double worstNode = 0.0;
    for (std::size_t i = 0; i < ringA.size() && i < legacyA.size(); ++i)
      worstNode = std::max(worstNode, std::fabs(ringA[i] - legacyA[i]));
    check(triOk && !ringA.empty() && sameSamples(ringA, legacyA), "M6.tri",
          "the ring's triangle node reproduces the legacy pure-triangle path BIT-FOR-BIT (BLAMP included)");
    std::fprintf(f, "triangle_node_vs_legacy_bytes\t%.4f\t%.3e\n", triNode, worstNode);

    // (iii) away from a node the ring must DIFFER from legacy, or nothing was wired.
    std::vector<double> ringOff, legOff;
    {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 256, 0.375, 0.375)) check(false, "M6.load.off", "load rejected");
      if (SynthRuntime* rt = h.producerRuntime()) rt->setVcoWaveMap(VcoWaveMap::kRingEqual);
      h.reserve(4096);
      if (!h.render(4096)) check(false, "M6.render.off", "render failed");
      ringOff = h.dryA();
    }
    {
      EngineHarness h;
      if (!loadWithMorph(h, 48000.0, 256, 0.375, 0.375)) check(false, "M6.load.off2", "load rejected");
      h.reserve(4096);
      if (!h.render(4096)) check(false, "M6.render.off2", "render failed");
      legOff = h.dryA();
    }
    double worstOff = 0.0;
    for (std::size_t i = 0; i < ringOff.size() && i < legOff.size(); ++i)
      worstOff = std::max(worstOff, std::fabs(ringOff[i] - legOff[i]));
    check(worstOff > 1e-6, "M6.wired",
          "away from a node the ring output DIFFERS from the legacy path (the wiring is live)");
    std::fprintf(f, "off_node_ring_vs_legacy\t0.3750\t%.3e\n", worstOff);
    std::fclose(f);
  }

  // -------------------------------------------------------------- M7 silence ---
  {
    std::FILE* f = openTsv(
        "m7_silence.tsv",
        "candidate\tnorm_at_S0_centre\tpeak_over_phase\trms_over_phase\tis_silent\tpeak_at_0\tpeak_at_1");
    for (int ci = 0; ci < 2; ++ci) {
      const wm::Boundaries& cb = kCand[ci];
      const wm::Boundaries& cref = cb;
      PhaseFn real = [&cref](double norm, double p) { return wm::sampleAt(cref, norm, p, kDuty); };

      double peak = 0.0;
      const bool silent = silentAtCentreOverPhase(cb, real, &peak);
      const double mid = 0.5 * (cb[0] + cb[1]);
      double rms = 0.0;
      for (int j = 0; j < 512; ++j) {
        const double p = static_cast<double>(j) / 512.0;
        const double v = wm::sampleAt(cb, mid, p, kDuty);
        rms += v * v;
      }
      rms = std::sqrt(rms / 512.0);
      char id[40], msg[288];
      std::snprintf(id, sizeof id, "M7.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "%s: the saw<->invSaw centre (norm=%.4f) is EXACTLY silent over phase (peak=%.3e) - REPORTED, NOT compensated",
                    kCandName[ci], mid, peak);
      check(silent, id, msg);

      // No gain compensation anywhere: the sweep endpoints must still carry the full swing.
      double peakA = 0.0, peakB = 0.0;
      for (int j = 0; j < 512; ++j) {
        const double p = static_cast<double>(j) / 512.0;
        peakA = std::max(peakA, std::fabs(wm::sampleAt(cb, 0.0, p, kDuty)));
        peakB = std::max(peakB, std::fabs(wm::sampleAt(cb, 1.0, p, kDuty)));
      }
      std::snprintf(id, sizeof id, "M7n.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "%s: no gain compensation - the sweep endpoints still carry the full swing (peaks %.6f / %.6f)",
                    kCandName[ci], peakA, peakB);
      check(peakA > 0.999 && peakB > 0.999, id, msg);

      // Evidence for the report's claim that a gain CANNOT lift the zero.
      double comp = 0.0;
      for (int j = 0; j < 512; ++j) {
        const double p = static_cast<double>(j) / 512.0;
        comp = std::max(comp, std::fabs(wm::sampleAt(cb, mid, p, kDuty) * 1e6));
      }
      std::fprintf(f, "%s\t%.4f\t%.9e\t%.9e\t%d\t%.9f\t%.9f\n", kCandName[ci], mid, peak, rms,
                   silent ? 1 : 0, peakA, peakB);
      std::fprintf(f, "%s_compensated_x1e6\t%.4f\t%.9e\t-\t%d\t-\t-\n", kCandName[ci], mid, comp,
                   comp == 0.0 ? 1 : 0);
    }
    std::fclose(f);
  }

  // ---------------------------------------------- M9 representative waveforms ---
  // A boundary-value table does not show SHAPE. This writes one full period of the mapped waveform at
  // nine norms per candidate, plus the legacy pure-triangle period for comparison, so the sweep can be
  // eyeballed for the thing the owner actually asked about: whether the panel-recognisable shapes
  // appear in a sensible order and the transitions between them look usable.
  {
    std::FILE* f = openTsv("m9_waveform.tsv", "candidate\tnorm\tphase\tvalue");
    const char* names[2] = {"equal", "panel"};
    const wm::Boundaries bounds[2] = {wm::kRingEqual, wm::kRingPanel};
    const double norms[9] = {0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0};
    const int kPts = 64;
    for (int ci = 0; ci < 2; ++ci) {
      for (const double norm : norms) {
        for (int j = 0; j < kPts; ++j) {
          const double p = static_cast<double>(j) / static_cast<double>(kPts);
          std::fprintf(f, "%s\t%.4f\t%.5f\t%.9f\n", names[ci], norm, p,
                       wm::sampleAt(bounds[ci], norm, p, kDuty));
        }
      }
    }
    // The legacy pure triangle the whole mapping has to coexist with (wave_ = kTriangle).
    for (int j = 0; j < kPts; ++j) {
      const double p = static_cast<double>(j) / static_cast<double>(kPts);
      std::fprintf(f, "legacy_triangle\t-\t%.5f\t%.9f\n", p, convTri(p));
    }
    std::fclose(f);
  }

  // --------------------------------------------------------- negative controls --
  {
    std::FILE* f = openTsv("neg_controls.tsv", "id\tdefect\tcheck_id\tcheck_fires\tdetail");

    // N1: the REAL chain on the legacy path (morph swept, `wave_` pinned to kTriangle) is a
    // fixed-waveform source, so M1's movement predicate must FIRE — this is the S0 defect itself,
    // measured on the product rather than modelled.
    {
      const SweepResult r = runSweep(48000.0, 512, 9, VcoWaveMap::kLegacy, nullptr, "legacy", nullptr);
      const bool fires = !movesWithNorm(r.spreadA, r.spreadB);
      char msg[288];
      std::snprintf(msg, sizeof msg,
                    "the REAL legacy path (morph swept, wave_ pinned to kTriangle) makes M1's movement check FIRE (rmsA spread %.3e, rmsB spread %.3e)",
                    r.spreadA, r.spreadB);
      check(fires, "N1", msg);
      std::fprintf(f, "N1\tfixed waveform (real legacy chain)\tM1\t%d\trmsA spread %.3e\n",
                   fires ? 1 : 0, r.spreadA);
    }

    // N2: cross-wired coordinates must make M5.coord's predicate fire.
    {
      const CoordProbe p = modelCrossWiredCoord(0.9, 0.5);
      const bool fires = !coordsMatchApplied(p);
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "a cross-wired source (side A reads coordinate B) makes M5.coord's predicate FIRE (set A=%.2f B=%.2f, read A=%.2f B=%.2f)",
                    p.appliedA, p.appliedB, p.readA, p.readB);
      check(fires, "N2", msg);
      std::fprintf(f, "N2\twrong side (cross-wired)\tM5.coord\t%d\tset A=%.2f B=%.2f read A=%.2f B=%.2f\n",
                   fires ? 1 : 0, p.appliedA, p.appliedB, p.readA, p.readB);
    }

    // N3: a stepped source must make M2's continuity scan fire.
    {
      double jump = 0.0;
      const bool fires = !continuousInNorm(PhaseFn(negStepped), &jump);
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "a stepped (non-partition-of-unity) source makes M2's continuity scan FIRE (worst jump %.6f)",
                    jump);
      check(fires, "N3", msg);
      std::fprintf(f, "N3\tinterval discontinuity\tM2\t%d\tworst jump %.6f\n", fires ? 1 : 0, jump);
    }

    // N4: a silence-free saw<->invSaw source must make M7's silence report fire.
    for (int ci = 0; ci < 2; ++ci) {
      const wm::Boundaries& cb = kCand[ci];
      const wm::Boundaries& cref = cb;
      PhaseFn silentFree = [&cref](double norm, double p) { return negSilenceFreeS0(cref, norm, p); };
      double pk = 0.0;
      const bool silent = silentAtCentreOverPhase(cb, silentFree, &pk);
      const bool fires = !silent;
      char id[40], msg[256];
      std::snprintf(id, sizeof id, "N4.%s", kCandName[ci]);
      std::snprintf(msg, sizeof msg,
                    "a silence-free saw<->invSaw source makes M7's silence report FIRE for %s (peak at the S0 centre %.6f)",
                    kCandName[ci], pk);
      check(fires, id, msg);
      std::fprintf(f, "N4.%s\tsilence-free S0\tM7\t%d\tpeak at S0 centre %.6f\n", kCandName[ci],
                   fires ? 1 : 0, pk);
    }
    std::fclose(f);
  }

  std::printf("\nchecks=%d failures=%d\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) { return realMain(argc, argv); }
