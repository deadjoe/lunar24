// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_s3_pulse_probe.cpp — task #118 (GH #19 S3) REAL-PRODUCT pulse / PWM measurement probe.
//
// WHAT THIS IS. The pure-pulse and legal-dynamic-PWM counterpart of gh19_alias_probe.cpp. It drives
// the REAL product output through the ONE agreed entry (the same chain the host uses):
//     make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//     standalone_audio_engine.processBlock   (encapsulated by EngineHarness)
// and captures the rendered audio for the S3 matrix. It computes NO metric: the analysis is the
// separate Python instrument (tools/gh19_s3_pulse_analyze.py) applied to these real samples. This
// file exists so the measurement provably samples the product's DSP rather than an ideal() stand-in,
// and so every cell is either produced or an explicit hard failure (exit non-zero) — never a silent
// pass.
//
// WHY A SEPARATE PROBE AND NOT MORE CELLS IN gh19_alias_probe. The S3 arm is measured on TWO trees
// (the merged baseline d9991872 with no pulse correction, and the candidate arm with one). Adding
// cells to the shipped probe would change a committed required-cell manifest and force the existing
// S2/S5 gates to be re-run to reach a verdict on an unrelated slice. This probe carries its own
// matrix, its own scenario file and no gate, so the S2/S5 contract is untouched, and the SAME
// source file is compiled against both trees — which is what makes the two arms a like-for-like
// comparison of one measurement instrument rather than of two.
//
// OBSERVATION POINTS (real production taps):
//   dryA = VCO A, tapped BEFORE the chain (machine_runtime.h:117)
//   dryB = VCO B, tapped BEFORE the chain (machine_runtime.h:118)
//   lfo_a_cv_out = LFO A's published CV OUT jack (read through the runtime's public
//                  controlVoltageAt accessor) — the SAME jack the PWM cable carries, sampled
//                  once per frame. This is the dynamic-PWM stimulus's own edge/level record.
//
// CELLS (id encodes the lane; a cell id that does not name its duty/morph/depth is not used):
//   * vco_{a,b}_pulse_{sr}_{f}_pw{P}   — PURE pulse. morph = 1.0 is the ring's pulse NODE exactly
//       (wave_map::sampleAt at norm = 1.0 evaluates `0.0*node3 + 1.0*node4`, bit-identical to
//       pulseShape), so no triangle BLAMP weight is in force there (wave_map::triangleWeight
//       returns exactly 0.0 on that stretch). depth = 0: the PWM knob is at its default, no cable,
//       so effectiveDuty() == duty_ == clamp(pw). 4 sr x 3 f x 3 duty x 2 sides.
//   * vco_a_ringmix_{sr}_{f}_pw050_m{M} — the S6 BASELINE: pulse only PARTIALLY weighted. morph on
//       stretch 3 (triangle -> pulse) at local u = M_local, so the pulse node carries weight
//       u and the triangle node carries 1-u. These cells are NOT pure pulse and are never reported
//       as such; they exist so S6 has a mixed coordinate that already contains a pulse term.
//   * vco_a_dynpwm_{sr}_{f}_{tri|sq}_d{D} — MOVING duty from a REAL CABLE. LFO A's CV OUT is
//       patched into vco_a.pwm_in through the runtime's public connect() + rebuild(), the same
//       entry a user cable takes; the PWM knob vco_a.pwm is at depth D and the base width is 0.5,
//       so effectiveDuty(t) = clamp(0.5 + D*cv(t)/10, 1e-3, 1-1e-3) with cv the 0..+10 V LFO jack.
//       `tri` = lfo_a_wave 1.0 (triangle, a smooth duty sweep); `sq` = lfo_a_wave 0.0 (square, a
//       duty STEP, which is a different regime and is reported as a boundary case, not as covered).
//       The per-frame source trace is written next to the raw so the analyzer builds the duty
//       trajectory from the RECORDED SOURCE, not from a re-derived model.
//
// STATE IS APPLIED WHOLE, SO THE STATIC CELLS ARE EXACTLY STATIONARY. applyDeviceState routes the
// vco panel knobs (pw, morph, pwm depth) through applyDspParam's isContinuousSmoothingParam_ branch
// (machine_runtime.h:1295-1310), which is SNAP-ONLY: reset(v) + applySmoothedControl_(id, v), never
// a ramp. So after load() the DSP holds the EXACT state value (the GH#21 smoother is settled and
// inert), and duty_/morph_ do not drift across the analysis window. Without that property a
// per-sample band-limited reference could not be aligned to the product at all; it is relied on
// here and it is checkable by re-reading Vco::shape()/morph() from the runtime.
//
// OUTPUT (--out <dir>, default ./report/gh19-s3-pulse-probe):
//   gh19_s3_plan.tsv        THE DECLARED STIMULUS PLAN (block, id), written before any render
//   gh19_s3_scenarios.tsv   one metadata row per cell
//   gh19_s3_scnNNN.raw      little-endian f64 samples of that cell's observation window
//   gh19_s3_srcNNN.tsv      dynamic cells only: the per-frame LFO-A CV-OUT record, same window
//
// EXIT CODE: 0 = every declared cell was produced, every trace-hook guard held, and the produced set
// matched the declared plan; non-zero = a cell was not produced (bit 0), a raw/trace write failed
// (bit 1), a trace-hook invariance guard failed (bit 2), or the produced cell set differs from the
// declared stimulus plan -- missing, extra, orphaned or duplicated (bit 3). There is no empty-exit-0.

// This probe writes .raw/.tsv with std::fopen, which MSVC's secure-CRT deprecation (C4996) promotes
// to an error under this repo's /W4 /WX policy. Scoped to this translation unit only.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <lunar24/core/device_state.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/vco.h>
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
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::ParameterId;
using lunar24::core::make_default_device_state;

namespace {

constexpr std::uint64_t kProbeSeed = 0x4C554E4152ULL;     // same LUNAR hex as the harness default
constexpr std::size_t kWarm = 8192;                       // settle transients (see the state note above)
constexpr std::size_t kWin = 16384;                       // analysis window
const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};
const std::vector<double> kFs = {220.0, 440.0, 880.0};
const std::vector<double> kDuties = {0.1, 0.5, 0.9};
// Stretch-3 local coordinates for the mixed S6-baseline cells. morph = 0.75 + 0.25*u.
const std::vector<double> kMixU = {0.25, 0.5, 0.75};

// ---------------------------------------------------------------------------------------------
// THE DECLARED STIMULUS PLAN (@Codex 8aba3aeb / 32b13ced, item 1)
// ---------------------------------------------------------------------------------------------
// THE DEFECT THIS EXISTS FOR. The baseline/candidate delta tool used to lock coverage by
// comparing the two arms' own analyzer output: same id set, same self-reported counts. That is a
// statement about the two arms agreeing with EACH OTHER, and it is satisfied by deleting a cell
// from both of them -- @Codex reproduced exactly that (drop `vco_a_pulse_44100_220_pw10` from both
// arms, decrement each fence 80 -> 79, tool exits 0 and prints "every declared cell is present in
// BOTH arms"). The expectation has to come from somewhere the run cannot edit.
//
// WHAT THIS IS. The plan is the COMPLETE stimulus set the probe declares, enumerated from the
// declared axes above (and from kDynSpecs/kPulseHi*/kAsym* below) by buildPlan(): no rendering, no
// capture, no DeviceState, no file I/O. main() writes it BEFORE it renders anything and writes it
// unconditionally, so a cell that fails to render is a MISSING plan entry, never an entry that was
// never expected. A run whose own output is empty still produces the full plan.
//
// WHAT IT IS NOT. It is not derived from, filtered by, or reconciled against the produced rows at
// generation time. The reconciliation runs the other way -- after rendering, the produced id set is
// checked against the plan and the probe exits non-zero on any difference (bit 3). And the plan is
// version-pinned as a checked-in artifact: tools/run_gh19_s3_coverage.py regenerates it and
// requires byte-identity with report/gh19-s3-pulse-aa/expected_cells.tsv, so editing an axis
// without re-pinning the file is itself a failure.
//
// The block names are the analyzer's matrix names, one per fence. `dynamic` holds every
// moving-duty cell (the referenced subset AND the ones the analyzer reports as un-referenced);
// `connect` and `ab_asymmetry` are the two families that until this revision were produced by the
// probe and consumed by NOTHING, while the comments below them claimed otherwise.
constexpr int kPlanRev = 1;
const std::vector<double> kPulseHiFs = {3520.0, 5000.0};
const std::vector<double> kPulseHiSrs = {44100.0, 96000.0};
const std::vector<double> kPulseHiDuties = {0.1, 0.5};
const std::vector<double> kMixSrs = {44100.0, 96000.0};
const std::vector<double> kDynSrs = {44100.0, 96000.0};
const std::vector<double> kDynFs = {220.0, 440.0, 880.0};
const std::vector<double> kConnectSrs = {44100.0};
const std::vector<double> kConnectFs = {440.0};
const double kSubsetWave = 0.0;     // square LFO
const double kSubsetDepth = 0.5;    // declared B-side subset: the two square specs
const std::vector<double> kSubsetSrs = {44100.0, 96000.0};
const std::vector<double> kSubsetFs = {220.0, 880.0};
const std::vector<double> kAsymFs = {220.0, 880.0};
const double kAsymSr = 44100.0;

// One moving-duty stimulus. Declared here rather than inside main() so the plan and the render
// loop read the SAME table -- a second copy of it is how a plan drifts away from what is rendered.
struct DynSpec { double lfoWave; double pwBase; double pwDepth; double rate; };
const std::vector<DynSpec> kDynSpecs = {
    {1.0, 0.50, 0.5, 2.0}, {1.0, 0.50, 0.5, 20.0},
    {1.0, 0.25, 0.5, 2.0}, {1.0, 0.25, 0.5, 20.0},
    {0.0, 0.50, 0.5, 2.0}, {0.0, 0.50, 0.5, 20.0},
    {1.0, 0.25, 1.0, 20.0},   // depth-1 clamp boundary (duty saturates at 0.999)
    {0.0, 0.25, 1.0, 20.0},
};
// The A/B asymmetry cells' stimulus. The `sq_r20_b50_d50` spec above is square-wave 20 Hz, base
// 0.5, depth 0.5; stated as its own literal so a change to kDynSpecs cannot silently move it.
const DynSpec kAsymSpec = {0.0, 0.50, 0.5, 20.0};

std::string staticCellId(const std::string& side, bool hi, double sr, double ft, double duty) {
  return "vco_" + side + (hi ? "_pulsehi_" : "_pulse_") + std::to_string((int)sr) + "_" +
         std::to_string((int)ft) + "_pw" + std::to_string((int)std::lround(duty * 100.0));
}
std::string mixCellId(double sr, double morph) {
  return "vco_a_ringmix_" + std::to_string((int)sr) + "_440_pw050_m" +
         std::to_string((int)std::lround(morph * 100000.0));
}
std::string dynCellId(const DynSpec& d, double sr, double ft, const std::string& side,
                      const std::string& cableTag) {
  const std::string waveTok = (d.lfoWave == 1.0) ? "tri" : "sq";
  return "vco_" + side + "_dynpwm_" + std::to_string((int)sr) + "_" + std::to_string((int)ft) +
         "_" + waveTok + "_r" + std::to_string((int)std::lround(d.rate)) + "_b" +
         std::to_string((int)std::lround(d.pwBase * 100.0)) + "_d" +
         std::to_string((int)std::lround(d.pwDepth * 100.0)) + cableTag;
}

struct PlanEntry { std::string block; std::string id; };

std::vector<PlanEntry> buildPlan() {
  std::vector<PlanEntry> p;
  for (const std::string& side : {std::string("a"), std::string("b")}) {
    for (double sr : kSrs) {
      for (double ft : kFs) {
        for (double duty : kDuties) p.push_back({"static", staticCellId(side, false, sr, ft, duty)});
      }
    }
  }
  for (double ft : kPulseHiFs) {
    for (double sr : kPulseHiSrs) {
      for (double duty : kPulseHiDuties) p.push_back({"static", staticCellId("a", true, sr, ft, duty)});
    }
  }
  for (double u : kMixU) {
    for (double sr : kMixSrs) p.push_back({"mixed", mixCellId(sr, 0.75 + 0.25 * u)});
  }
  for (const DynSpec& d : kDynSpecs) {
    for (double sr : kDynSrs) {
      for (double ft : kDynFs) p.push_back({"dynamic", dynCellId(d, sr, ft, "a", "")});
    }
    for (double sr : kConnectSrs) {
      for (double ft : kConnectFs) p.push_back({"connect", dynCellId(d, sr, ft, "a", "_viaConnect")});
    }
    if (!(d.lfoWave == kSubsetWave && d.pwDepth == kSubsetDepth)) continue;
    for (double sr : kSubsetSrs) {
      for (double ft : kSubsetFs) p.push_back({"dynamic", dynCellId(d, sr, ft, "b", "")});
    }
  }
  for (double ft : kAsymFs) {
    p.push_back({"ab_asymmetry", dynCellId(kAsymSpec, kAsymSr, ft, "b", "_unpatched_under_a_cable")});
  }
  return p;
}

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
// Sign changes per window / 2 -> cycle count -> measured frequency.
double zcrFreq(const std::vector<double>& c, double sr) {
  int z = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  }
  return static_cast<double>(z) * sr / (2.0 * static_cast<double>(c.size()));
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
  std::fclose(f);
  return true;
}
// 17 significant digits: the trace is compared bit-for-bit by the analyzer's own re-render guard, so
// a %.6g round-trip would silently coarsen it.
std::string fmtExact(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.17g", v);
  return std::string(b);
}
std::string safeTsv(const std::string& s) { return s.empty() ? "-" : s; }

struct Cap {
  bool ok = false;
  std::string signal;        // "" = produced; else a hard-failure reason
  std::vector<double> x;
  std::vector<double> src;   // per-frame LFO-A CV OUT (dynamic cells; empty otherwise)
  double peak = 0.0;
  double f0 = 0.0;
};

// The VCO base frequency the product is configured with, reached by oct_sel x tune — the same recipe
// gh19_alias_probe's triangle cells use. `vco_a_oct_sel` is a 3-position selector over octaves
// {-1, 0, +3} (registry.hpp:261, 391) and `tune` is a continuous knob over [-1, +1] octaves, so the
// PANEL-reachable base set is [220, 880] U [3520, 7040] Hz — 100 Hz and 1000..3400 Hz are NOT
// panel-reachable, and no cell below claims them. The candidate is solved rather than hand-listed so
// a frequency that is not reachable fails loudly instead of landing on some other octave.
void applyFTarget(DeviceStateV1& st, const std::string& side, double ft) {
  const double need = std::log2(ft / 440.0);          // octaves above the 440 Hz base
  int sel = -1;
  double tune = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double oct = (i == 0) ? -1.0 : (i == 1) ? 0.0 : 3.0;
    const double t = need - oct;
    if (t >= -1.0 && t <= 1.0) { sel = i; tune = t; break; }
  }
  if (sel < 0) {
    std::fprintf(stderr, "FATAL: %.3f Hz is not reachable by oct_sel x tune on this panel\n", ft);
    std::abort();
  }
  if (side == "a") {
    slot(st, ParameterId::vco_a_oct_sel) = sel;
    slot(st, ParameterId::vco_a_tune) = tune;
  } else {
    slot(st, ParameterId::vco_b_oct_sel) = sel;
    slot(st, ParameterId::vco_b_tune) = tune;
  }
}

// HOW A CABLE REACHES THE RENDER. @Codex 582c74ba requires the moving-duty cells to carry at least
// one group whose cable is a fact OF THE STATE, so it travels encode -> decode -> apply and is
// re-established by the owner's own apply (machine_definition.h:449-477) rather than by a test
// calling connect() after the load. `kConnect` is kept as the companion: the two must render
// byte-identically, which is the claim that the saved patch and the patched one are one signal.
enum class CableMode { kNone, kConnect, kState };   // kState = the restore path (primary)

// Declare the cable as a state fact. The index is the SERIALIZED JackId, not a dense index
// (device_state.h:465-468 + machine_definition.h:452-458), exactly as gh19_alias_probe's sync line
// does for `vco_a_sync_in`. `side` selects which PWM sink the LFO drives, which is how the A/B
// asymmetry cells are built without any test-side patching.
DeviceStateV1 withPwmCable(const DeviceStateV1& st, const std::string& side) {
  DeviceStateV1 out = st;
  const std::uint32_t sink = static_cast<std::uint32_t>(
      (side == "b") ? lunar24::core::JackId::vco_b_pwm_in : lunar24::core::JackId::vco_a_pwm_in);
  out.inputCable[sink] = 1u;
  out.cableSource[sink] = lunar24::core::JackId::lfo_a_cv_out;
  return out;
}

// The upper edge of the cell-validity envelope, in the device domain. A pure pulse at norm = 1.0 sits
// at +/-0.5, so 0.55 accepts the product's own waveform and rejects a cell that is not one.
//
// WHY IT IS A FLAG AND NOT A CONSTANT. The envelope is a validity guard for the CANDIDATE arm: it
// says "this cell is the waveform the reference describes", and the candidate runs always use the
// default. It is NOT a physical invariant of the render, and the directed negative controls are
// mutations that are *predicted* to leave the envelope by construction -- a wrong-sign edge
// correction drives the +-1 jump to +-2, and there is no bounded way to express that error, because
// the correct correction is exactly what keeps the step inside [-1, +1]. With the default bound the
// mutated arms would abort as `over-scale` and the named criterion would never be evaluated, leaving
// the control vacuous (an aborted arm proves the toolchain reacted, not that the metric sees the
// defect). The negative-control runner therefore widens the bound for those arms only and prints the
// bound it used next to the per-cell peak. Widening it for a mutation arm cannot flatter the
// candidate: the candidate's own numbers come from runs that never pass this flag.
static double gPeakHi = 0.55;

// Render `frames` frames from one engine load and capture `tap`.
//   domain "device": physical device output; peak guard [1e-4, gPeakHi] (the un-scaled-ideal detector).
//   `srcTrace`, when non-null, additionally receives LFO A's published CV OUT once per frame via
//   renderSampled's post-frame const hook. That hook only READS, so it cannot perturb what it
//   measures; the callers below assert that by re-rendering the same state with no hook and
//   requiring the two audio streams to be byte-identical. The hook runs on the SAME per-frame
//   processBlock path as render() (test_engine_harness.h:117-128).
Cap capturePulse(const DeviceStateV1& st, double sr, std::size_t frames, const std::string& tap,
                 CableMode cable, const std::string& cableSide = "a",
                 std::vector<double>* srcTrace = nullptr) {
  const double lo = 1e-4, hi = gPeakHi;
  Cap c;
  EngineHarness h;
  // kState: the cable is already inside `st`, so the load itself is the whole restore — no
  // post-load graph edit happens on this arm at all.
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  lunar24::core::SynthRuntime* rt = h.producerRuntime();
  if (rt == nullptr) { c.signal = "no-runtime"; return c; }
  if (cable == CableMode::kConnect) {
    const auto sink = (cableSide == "b") ? lunar24::core::JackId::vco_b_pwm_in
                                         : lunar24::core::JackId::vco_a_pwm_in;
    if (!rt->connect(lunar24::core::JackId::lfo_a_cv_out, sink)) {
      c.signal = "pwm-connect-rejected";
      return c;
    }
    // The cable is only real once the plan is rebuilt off the audio path (the same off-thread
    // rebuild the host performs after a repatch).
    if (!rt->rebuild()) { c.signal = "pwm-rebuild-rejected"; return c; }
  }
  if (srcTrace != nullptr) {
    srcTrace->clear();
    srcTrace->reserve(frames);
    if (!h.renderSampled(static_cast<int>(frames), 0.0, [&](const lunar24::core::SynthRuntime& r) {
          srcTrace->push_back(r.controlVoltageAt(lunar24::core::JackId::lfo_a_cv_out));
        })) { c.signal = "render-failed"; return c; }
  } else if (!h.render(static_cast<int>(frames), 0.0)) {
    c.signal = "render-failed";
    return c;
  }
  const std::vector<double>* src = nullptr;
  if (tap == "dry_a") src = &h.dryA();
  else if (tap == "dry_b") src = &h.dryB();
  else { c.signal = "bad-tap"; return c; }
  if (src->size() < frames) { c.signal = "short-render"; return c; }
  c.x.assign(src->begin(), src->begin() + frames);
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; return c; }
  if (c.peak < lo) { c.signal = "silent"; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; return c; }
  c.ok = true;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// --bench: SAME-MACHINE CALLBACK COST
// ---------------------------------------------------------------------------------------------
// The ONLY cost claim S3 makes, and it is deliberately the crudest one available: how long the
// engine's per-frame callback takes on THIS machine, for the SAME source file compiled against the
// baseline tree and against the candidate tree. It says nothing about other hardware, nothing about
// a whole-buffer figure, and it is not a real-time-factor claim for any target platform.
//
// WHAT IS TIMED. EngineHarness::render(F) issues F one-frame processBlock calls, matching how the
// host drives the engine, so a frame here IS one callback, and ns/frame is directly comparable
// between the arms. Each cell is timed kBenchReps times and the MINIMUM is reported: on a shared
// machine the noise is one-sided (other load can only make a run slower), so the minimum is the
// robust estimate of the arm's own cost. The mean is reported too, and if the two disagree by more
// than a few percent the row is not usable -- that is visible rather than hidden.
//
// FOUR CELLS, chosen so the comparison cannot be reduced to "the correction costs X":
//   pw50  : the ordinary case; both windows are live, one correction term is in force.
//   pw10  : the narrow case at a low frequency. STALE COMMENT FIXED: this used to say it was "still
//           inside the disjoint domain" and that pw10-5000 measured a "BOUNDED FALLBACK BRANCH". Both
//           described the refuted revision, where a duty-dependent switch returned 0.0 outside the
//           disjoint domain. That switch is GONE (it was itself the defect: it introduced a 0.5625
//           discontinuity at dt = 220/44100, duty = 2dt). With it gone, duty = 0.1 at dt = 0.1134 is
//           in the OVERLAPPING regime and this row computes the correction for real.
//   pw10-5000 : the narrow-duty case at a high frequency, now simply a second narrow-duty point on the
//           same continuous formula rather than a different branch. There is no fallback branch left
//           to measure; the boundedness that the fallback used to provide is asserted on the kernel's
//           own output instead (tools/gh19_s3_pulse_kernel_sweep.cpp).
// A triangle row at morph = 0.75 rides along as the arm-invariant control: it takes the BLAMP path
// and no pulse path in either arm, so its two numbers are the bench's own noise floor.
void runBench(const std::string& out) {
  struct Cell { const char* id; double sr; double ft; double duty; double morph; };
  const Cell kBenchCells[] = {
      {"bench_pulse_pw50_44100_220",   44100.0,  220.0, 0.5, 1.0},
      {"bench_pulse_pw10_44100_220",   44100.0,  220.0, 0.1, 1.0},
      {"bench_pulse_pw10_44100_5000",  44100.0, 5000.0, 0.1, 1.0},
      {"bench_triangle_44100_220",     44100.0,  220.0, 0.5, 0.75},
  };
  constexpr std::size_t kBenchFrames = 20000;
  // 41 reps, not 7. At 7 the min still moved by up to 15 ns BETWEEN PROCESSES of the SAME binary --
  // as large as the effect being measured -- because each rep reloads the engine and the process's
  // own layout/thermal draw differs run to run. More reps inside one process tightens each process's
  // minimum; the residual between-process spread is why the run must also be repeated and the
  // minimum taken ACROSS runs, and why the triangle control is reported on every run.
  constexpr int kBenchReps = 41;
  std::vector<std::string> lines;
  lines.push_back("id\tsr_hz\tf_hz\tduty\tmorph\tframes\treps\tns_per_frame_min"
                  "\tns_per_frame_mean\tus_per_100_frames");
  for (const Cell& bc : kBenchCells) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    applyFTarget(st, "a", bc.ft);
    slot(st, ParameterId::vco_a_morph) = bc.morph;
    slot(st, ParameterId::vco_a_pw) = bc.duty;
    slot(st, ParameterId::vco_a_cv_amt) = 0.0;
    double best = 0.0, total = 0.0;
    bool ran = false;
    for (int rep = 0; rep < kBenchReps; ++rep) {
      EngineHarness h;
      if (!h.load(st, bc.sr)) { std::fprintf(stderr, "BENCH %s: load rejected\n", bc.id); break; }
      if (!h.render(static_cast<int>(kWarm), 0.0)) {
        std::fprintf(stderr, "BENCH %s: warm render failed\n", bc.id);
        break;
      }
      const auto t0 = std::chrono::steady_clock::now();
      if (!h.render(static_cast<int>(kBenchFrames), 0.0)) {
        std::fprintf(stderr, "BENCH %s: timed render failed\n", bc.id);
        break;
      }
      const auto t1 = std::chrono::steady_clock::now();
      const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                        static_cast<double>(kBenchFrames);
      if (!ran || ns < best) best = ns;
      total += ns;
      ran = true;
    }
    if (!ran) continue;
    const double mean = total / static_cast<double>(kBenchReps);
    lines.push_back(std::string(bc.id) + "\t" + fmtExact(bc.sr) + "\t" + fmtExact(bc.ft) + "\t" +
                    fmtExact(bc.duty) + "\t" + fmtExact(bc.morph) + "\t" +
                    std::to_string(kBenchFrames) + "\t" + std::to_string(kBenchReps) + "\t" +
                    fmtExact(best) + "\t" + fmtExact(mean) + "\t" + fmtExact(best * 100.0));
    std::printf("BENCH %-28s ns/frame min=%8.1f mean=%8.1f (reps=%d)\n", bc.id, best, mean,
                kBenchReps);
  }
  if (lines.size() > 1 && !writeTsv(out + "/gh19_s3_bench.tsv", lines)) {
    std::fprintf(stderr, "FATAL: could not write bench tsv\n");
  }
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  std::string out = "report/gh19-s3-pulse-probe";
  bool benchOnly = false;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
    if (std::strcmp(argv[i], "--peak-hi") == 0) gPeakHi = std::strtod(argv[i + 1], nullptr);
  }
  // Printed, not assumed: a reader of a mutated arm's log has to be able to see that the envelope was
  // widened for it and by how much, without reading the runner.
  if (gPeakHi != 0.55) {
    std::fprintf(stderr, "gh19_s3_pulse_probe: cell-validity peak envelope widened to %.6g "
                         "(default 0.55) -- this arm's numbers are NOT candidate-arm numbers.\n",
                 gPeakHi);
  }
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) benchOnly = true;
  }
  std::error_code ec;
  std::filesystem::create_directories(out, ec);   // portable recursive create (MSVC has no mkdir -p)

  if (benchOnly) {
    runBench(out);
    return 0;
  }

  int gCode = 0;   // bit0 = cell not produced, bit1 = write failed, bit2 = trace-hook guard failed,
                   // bit3 = produced cell set != declared stimulus plan

  // ---- THE DECLARED STIMULUS PLAN IS WRITTEN FIRST, BEFORE ANY RENDER ----
  // Built from the declared axes alone (buildPlan touches no DeviceState, no engine and no file),
  // and written unconditionally, so a run in which every capture fails still emits the complete
  // expected set. Everything below reconciles against THIS, not against what happened to render.
  const std::vector<PlanEntry> plan = buildPlan();
  {
    std::vector<std::string> plines;
    plines.push_back("plan_rev\t" + std::to_string(kPlanRev));
    plines.push_back("block\tid");
    for (const PlanEntry& e : plan) plines.push_back(e.block + "\t" + e.id);
    if (!writeTsv(out + "/gh19_s3_plan.tsv", plines)) {
      std::fprintf(stderr, "FATAL: could not write the declared stimulus plan\n");
      gCode |= 2;
    }
    std::printf("S3-PLAN plan_rev=%d cells=%zu\n", kPlanRev, plan.size());
  }
  std::vector<std::string> emittedIds;   // in emission order; reconciled against `plan` at the end

  std::vector<std::string> rows;
  // `warm`/`win` are emitted rather than left for the analyzer to assume: the analysis window is
  // frames [warm, warm+win) of the raw, and an analyzer that hardcoded its own guess would be
  // reading a different window than the probe wrote while still looking correct.
  rows.push_back("id\tpath\tsignal\tsr_hz\tf_target_hz\tside\tduty_param\tmorph\tpw_depth"
                 "\tlfo_wave\tf0_meas_hz\tphi0_model\tpeak\tchannel\tcable\twarm\twin"
                 "\traw\tsamples\tsrc");
  int scnIdx = 0;
  int srcIdx = 0;

  auto emit = [&](const std::string& id, const std::string& path, double sr, double ft,
                  const std::string& side, double duty, double morph, double pwDepth,
                  const std::string& lfoWave, const std::string& channel, const Cap& c,
                  const std::string& cable) {
    emittedIds.push_back(id);
    std::string raw = "";
    std::string src = "-";
    if (c.ok) {
      raw = "gh19_s3_scn" + std::to_string(++scnIdx) + ".raw";
      if (!writeRaw(out + "/" + raw, c.x)) {
        std::fprintf(stderr, "FATAL %s: raw write failed (%s)\n", id.c_str(), raw.c_str());
        gCode |= 2;
      }
      if (!c.src.empty()) {
        src = "gh19_s3_src" + std::to_string(++srcIdx) + ".tsv";
        std::vector<std::string> l;
        l.reserve(c.src.size() + 1);
        l.push_back("frame\tlfo_a_cv_out_volts");
        for (std::size_t i = 0; i < c.src.size(); ++i) {
          l.push_back(std::to_string(i) + "\t" + fmtExact(c.src[i]));
        }
        if (!writeTsv(out + "/" + src, l)) {
          std::fprintf(stderr, "FATAL %s: trace write failed (%s)\n", id.c_str(), src.c_str());
          gCode |= 2;
        }
      }
    } else {
      std::fprintf(stderr, "FATAL %s: cell not produced [%s]\n", id.c_str(), c.signal.c_str());
      gCode |= 1;
    }
    // The MODEL's starting phase for this cell, stated so the n+1 convention is auditable from the
    // manifest alone (@Codex 582c74ba: record the actual frequency and the starting phase). The
    // engine advances cumPitch_ and THEN emits, so the phase of the sample at absolute frame index n
    // (n = 0 is the first frame after load) is frac((n+1)*f0/sr); at n = 0 that is frac(f0/sr), and
    // phi0_model is exactly that, in degrees. `ft` is the nominal base the oct/tune recipe selects.
    const double phi0 = 360.0 * ((ft / sr) - std::floor(ft / sr));
    rows.push_back(id + "\t" + path + "\t" + safeTsv(c.signal) +
                   "\t" + std::to_string(sr) +
                   "\t" + std::to_string(ft) +
                   "\t" + side +
                   "\t" + fmtExact(duty) +
                   "\t" + fmtExact(morph) +
                   "\t" + fmtExact(pwDepth) +
                   "\t" + lfoWave +
                   "\t" + fmtExact(c.f0) +
                   "\t" + fmtExact(phi0) +
                   "\t" + fmtExact(c.peak) +
                   "\t" + channel +
                   "\t" + cable +
                   "\t" + std::to_string(kWarm) +
                   "\t" + std::to_string(kWin) +
                   "\t" + raw +
                   "\t" + std::to_string(c.x.size()) +
                   "\t" + src);
  };

  const std::size_t total = kWarm + kWin;

  // ---- PURE PULSE: 2 sides x 4 sr x 3 f x 3 duty. depth 0, no cable, morph = 1.0 (the node). ----
  for (const std::string& side : {std::string("a"), std::string("b")}) {
    const std::string tap = (side == "a") ? "dry_a" : "dry_b";
    for (double sr : kSrs) {
      for (double ft : kFs) {
        for (double duty : kDuties) {
          DeviceStateV1 st = make_default_device_state(kProbeSeed);
          applyFTarget(st, side, ft);
          // morph = 1.0 is the ring's pulse node exactly; nothing else is mixed in, so this cell is
          // a PURE pulse and its id may say so.
          slot(st, side == "a" ? ParameterId::vco_a_morph : ParameterId::vco_b_morph) = 1.0;
          slot(st, side == "a" ? ParameterId::vco_a_pw : ParameterId::vco_b_pw) = duty;
          if (side == "b") {
            // The default graph feeds vco_b.cv_in from vco_a.dry_out (task#83 A->B route), so with a
            // non-zero cv_amt dryB is audio-rate FM-ed and not a steady single carrier. The legal
            // panel knob cv_amt = 0 makes B the clean carrier this comparison needs; identical to
            // gh19_alias_probe's vco_b_tri recipe.
            slot(st, ParameterId::vco_b_cv_amt) = 0.0;
          }
          const std::string id = staticCellId(side, false, sr, ft, duty);
          Cap c = capturePulse(st, sr, total, tap, CableMode::kNone);
          emit(id, "vco_" + side + "_pulse", sr, ft, side, duty, 1.0, 0.0, "-", tap, c, "none");
        }
      }
    }
  }

  // ---- HIGH f0 / SUPPORT-OVERLAP: the regime the removed guard used to switch OFF in. ----
  // A polyBLEP edge correction occupies a window of +-dt in phase around its edge, where dt = f0/sr.
  // A pulse has TWO edges, at phase 0 and phase D, so the two windows are disjoint only while
  // min(D, 1-D) >= 2*dt. The panel reaches f0 = 3520 and 5000 Hz (oct_sel 2), and at 44.1 kHz those
  // give dt = 0.0798 and 0.1134, i.e. 2*dt = 0.1596 and 0.2268 — both LARGER than duty 0.1. So the
  // overlap regime is reachable with legal panel settings.
  //
  // STALE RATIONALE FIXED. This block used to say it existed "to make the bounded fallback measurable
  // instead of asserted". The refuted revision returned 0.0 here (polyblepPulseWindowsDisjoint gated
  // the correction), and THAT GATE WAS THE DEFECT: it made the output jump by 0.5625 as duty crossed
  // 2*dt at a fixed phase -- a discontinuity introduced by the correction switch, at a point no pulse
  // edge passes through. Window overlap is a SUFFICIENT condition for the two correction terms to be
  // separately well-defined; it is NOT a necessary one, and non-overlap does not mean a narrow pulse
  // has no band-limited representation. The guard is gone, the formula is continuous across 2*dt, and
  // the kernel-domain sweep (tools/gh19_s3_pulse_kernel_sweep.cpp) is what now measures continuity and
  // boundedness here. These cells remain in the product probe because the regime is reachable on the
  // panel and must stay rendered and reported; the disjoint flag below is REPORTING ONLY, kept so the
  // boundary arithmetic stays auditable, and has no callers in the correction path. 96 kHz flips two
  // of them back to the disjoint side, so the family straddles the boundary rather than sitting wholly
  // outside it.
  for (double ft : kPulseHiFs) {
    for (double sr : kPulseHiSrs) {
      for (double duty : kPulseHiDuties) {
        DeviceStateV1 st = make_default_device_state(kProbeSeed);
        applyFTarget(st, "a", ft);
        slot(st, ParameterId::vco_a_morph) = 1.0;
        slot(st, ParameterId::vco_a_pw) = duty;
        const double dt = ft / sr;
        const std::string id = staticCellId("a", true, sr, ft, duty);
        Cap c = capturePulse(st, sr, total, "dry_a", CableMode::kNone);
        // The overlap verdict is printed with the cell so the analyzer's boundary table is auditable
        // against the probe's own arithmetic instead of being recomputed from a comment.
        std::printf("S3-OVERLAP %s dt=%.6f minD=%.6f disjoint=%d\n", id.c_str(), dt,
                    std::min(duty, 1.0 - duty), (std::min(duty, 1.0 - duty) >= 2.0 * dt) ? 1 : 0);
        emit(id, "vco_a_pulsehi", sr, ft, "a", duty, 1.0, 0.0, "-", "dry_a", c, "none");
      }
    }
  }


  // Stretch 3 (triangle -> pulse) at local u: pulse weight = u, triangle weight = 1-u. Reported as
  // mixed, never as pure pulse. One side (A) and one carrier suffice: both sides share the VCO call
  // point (gh19_alias_probe's vco_b_tri == vco_a_tri verified that).
  for (double u : kMixU) {
    const double morph = 0.75 + 0.25 * u;
    for (double sr : kMixSrs) {
      DeviceStateV1 st = make_default_device_state(kProbeSeed);
      applyFTarget(st, "a", 440.0);
      slot(st, ParameterId::vco_a_morph) = morph;
      slot(st, ParameterId::vco_a_pw) = 0.5;
      const std::string id = mixCellId(sr, morph);
      Cap c = capturePulse(st, sr, total, "dry_a", CableMode::kNone);
      emit(id, "vco_a_ringmix", sr, 440.0, "a", 0.5, morph, 0.0, "-", "dry_a", c, "none");
    }
  }

  // ---- MOVING DUTY from a REAL CABLE. ----
  // lfo_a_wave: 1.0 = triangle (smooth duty sweep), 0.0 = square (duty STEP: a different regime,
  // reported as the boundary case, never as covered).
  //
  // WHY THE LFO RATE IS A COVERAGE AXIS AND NOT DECORATION. The one-frame-late duty defect the
  // directed negatives must catch has a size set by the duty SLOPE: a one-frame delay moves each
  // pulse edge by (dduty/dn) * period_samples samples. At 2 Hz and 220 Hz that is 0.0045 samples —
  // below what any residual here can resolve, so a 2 Hz-only matrix would answer "not detected" for
  // a real defect and call it a pass. At 20 Hz (the registry's rate ceiling) it is 0.045 samples,
  // which is above the corrected arm's own floor. BOTH rates are therefore carried, and the pair is
  // what makes the negative's verdict a measurement instead of an assumption about sensitivity.
  //
  // The duty trajectory is derived by the ANALYZER from the recorded cv_out trace, never from the
  // nominal rate: if the registry rejected a rate, the trace still shows the real one, so the cell
  // degrades in sensitivity rather than becoming silently mislabelled.
  //
  // THE CABLE IS A STATE FACT ON THE PRIMARY ARM (@Codex 582c74ba). `kState` writes inputCable/
  // cableSource and then renders straight from the load, so the cable travels encode -> decode ->
  // apply and is re-established by the owner — the user line. `kConnect` is the post-load
  // connect()+rebuild() companion, emitted for a declared subset; the analyzer requires the two arms
  // to be byte-identical, which is what makes the companion a control rather than a substitute for
  // the restore path.
  // The stimulus table, the id builder and the plan all come from the namespace-scope declarations
  // above: a second copy of the spec table here is how a plan and a render drift apart.
  const std::vector<DynSpec>& dyn = kDynSpecs;
  auto dynState = [&](const DynSpec& d, double ft, const std::string& side) {
    DeviceStateV1 st = make_default_device_state(kProbeSeed);
    applyFTarget(st, side, ft);
    slot(st, side == "a" ? ParameterId::vco_a_morph : ParameterId::vco_b_morph) = 1.0;
    slot(st, side == "a" ? ParameterId::vco_a_pw : ParameterId::vco_b_pw) = d.pwBase;
    slot(st, side == "a" ? ParameterId::vco_a_pwm : ParameterId::vco_b_pwm) = d.pwDepth;
    slot(st, ParameterId::lfo_a_wave) = d.lfoWave;
    slot(st, ParameterId::lfo_a_rate) = d.rate;
    slot(st, ParameterId::lfo_a_speed_mult) = 0.0;   // x1
    if (side == "b") {
      // The default graph feeds vco_b.cv_in from vco_a.dry_out, so a non-zero cv_amt would FM B with
      // A's audio — and on a B cell A is precisely the side carrying the moving duty. cv_amt = 0 is
      // the legal panel knob that isolates the B measurement from that route.
      slot(st, ParameterId::vco_b_cv_amt) = 0.0;
    }
    return st;
  };
  // The id builder is the namespace-scope dynCellId, shared with buildPlan() above.
  // TRACE-HOOK INVARIANCE GUARD: render the same state with and without the read-only per-frame hook
  // and require the two audio streams to be byte-identical. If the hook ever perturbed the render,
  // the traced cell would silently measure a different stimulus from the one its trace claims to
  // describe — the exact failure the sync cells' guard exists for (gh19_alias_probe's `trace_diff`).
  auto traceGuard = [&](const std::string& id, const DeviceStateV1& st, double sr,
                        const std::string& tap, CableMode cm, const std::string& side,
                        std::vector<double>* trace) {
    Cap untraced = capturePulse(st, sr, total, tap, cm, side, nullptr);
    Cap c = capturePulse(st, sr, total, tap, cm, side, trace);
    double traceDiff = -1.0;
    if (untraced.ok && c.ok && untraced.x.size() == c.x.size()) {
      traceDiff = 0.0;
      for (std::size_t i = 0; i < untraced.x.size(); ++i) {
        const double a = std::fabs(untraced.x[i] - c.x[i]);
        if (a > traceDiff) traceDiff = a;
      }
    }
    std::printf("S3-TRACE %s trace_diff=%.6g\n", id.c_str(), traceDiff);
    if (!(traceDiff == 0.0)) {
      std::fprintf(stderr, "FATAL %s: trace-hook invariance guard failed (trace_diff=%.6g)\n",
                   id.c_str(), traceDiff);
      gCode |= 4;
    }
    return c;
  };

  // PRIMARY ARM: side A, cable carried by the DeviceState (the restore path). 220/440/880 are all
  // carried here (not just the pure-pulse set) because the connect companion below must have an exact
  // twin to be a control: a companion at a coordinate the primary arm does not cover compares nothing.
  for (const DynSpec& d : dyn) {
    for (double sr : kDynSrs) {
      for (double ft : kDynFs) {
        const std::string id = dynCellId(d, sr, ft, "a", "");
        std::vector<double> trace;
        Cap c = traceGuard(id, withPwmCable(dynState(d, ft, "a"), "a"), sr, "dry_a",
                           CableMode::kState, "a", &trace);
        c.src = std::move(trace);
        emit(id, "vco_a_dynpwm", sr, ft, "a", d.pwBase, 1.0, d.pwDepth,
             (d.lfoWave == 1.0) ? "tri" : "sq", "dry_a", c, "state:vco_a_pwm_in<-lfo_a_cv_out");
      }
    }
  }

  // CONNECT COMPANION (declared subset: every spec at 44.1 kHz, 440 Hz). The same plain state, the
  // cable made after the load instead. The analyzer's `connect` matrix requires these to be
  // byte-identical to their kState counterparts; a difference means the saved patch and the patched
  // one are not one signal. (Until the coverage revision this comment asserted a check that did not
  // exist anywhere in the tree -- the cells were produced and consumed by nothing. The matrix now
  // exists and the identity is asserted per cell.)
  for (const DynSpec& d : dyn) {
    for (double sr : kConnectSrs) {
      for (double ft : kConnectFs) {
        const std::string id = dynCellId(d, sr, ft, "a", "_viaConnect");
        std::vector<double> trace;
        Cap c = traceGuard(id, dynState(d, ft, "a"), sr, "dry_a", CableMode::kConnect, "a", &trace);
        c.src = std::move(trace);
        emit(id, "vco_a_dynpwm_connect", sr, ft, "a", d.pwBase, 1.0, d.pwDepth,
             (d.lfoWave == 1.0) ? "tri" : "sq", "dry_a", c, "connect:vco_a_pwm_in<-lfo_a_cv_out");
      }
    }
  }

  // B SIDE: the same LFO drives vco_b.pwm_in through a state-carried cable, measured on dry_b.
  // This is the isolated other-side cell (@Codex 582c74ba: dynamic input to also cover B).
  for (const DynSpec& d : dyn) {
    if (!(d.lfoWave == kSubsetWave && d.pwDepth == kSubsetDepth)) continue;   // declared B-side subset
    for (double sr : kSubsetSrs) {
      for (double ft : kSubsetFs) {
        const std::string id = dynCellId(d, sr, ft, "b", "");
        std::vector<double> trace;
        Cap c = traceGuard(id, withPwmCable(dynState(d, ft, "b"), "b"), sr, "dry_b",
                           CableMode::kState, "b", &trace);
        c.src = std::move(trace);
        emit(id, "vco_b_dynpwm", sr, ft, "b", d.pwBase, 1.0, d.pwDepth, "sq", "dry_b", c,
             "state:vco_b_pwm_in<-lfo_a_cv_out");
      }
    }
  }

  // A/B ASYMMETRY CONTRAST: patch A's PWM sink only, and require B to be untouched. B is left a pure
  // pulse at the SAME base duty as the shipped cell vco_b_pulse_<sr>_<f>_pw50 (morph 1.0, cv_amt 0),
  // so the two renders must be byte-identical. If the correction (or the cable) leaked to the other
  // side — a wrong-sink or wrong-side defect — B moves and the difference is non-zero and reportable.
  // It is also the cheapest check that "A->B modulation" is really isolated: A's waveform IS changing
  // here, and B must not see it through the cv_amt = 0 route.
  for (double ft : kAsymFs) {
    const DynSpec& d = kAsymSpec;
    DeviceStateV1 stA = withPwmCable(dynState(d, ft, "a"), "a");
    // B must be put on the SAME carrier recipe as the reference cell it is compared against, or the
    // comparison is between two different frequencies and its non-zero result says nothing about
    // leakage. (First run got exactly this wrong: B was left at the 220 Hz default, so the 880 Hz
    // contrast compared 880 Hz against 220 Hz and reported a full-scale "leak".)
    applyFTarget(stA, "b", ft);
    slot(stA, ParameterId::vco_b_morph) = 1.0;
    slot(stA, ParameterId::vco_b_pw) = 0.5;
    slot(stA, ParameterId::vco_b_cv_amt) = 0.0;
    const std::string id = dynCellId(d, kAsymSr, ft, "b", "_unpatched_under_a_cable");
    Cap c = capturePulse(stA, kAsymSr, total, "dry_b", CableMode::kState, "b", nullptr);
    emit(id, "vco_b_ab_asymmetry", kAsymSr, ft, "b", 0.5, 1.0, 0.0, "-", "dry_b", c,
         "state:vco_a_pwm_in<-lfo_a_cv_out(b-unpatched)");
  }

  std::string tsv = out + "/gh19_s3_scenarios.tsv";
  if (!writeTsv(tsv, rows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str());
    gCode |= 2;
  }

  // ---- PRODUCED SET vs DECLARED PLAN (bit 3) ----
  // The reconciliation runs in ONE direction only: what was produced is checked against the plan that
  // was written before the first render. Nothing here can add a cell to the plan, so a cell that the
  // probe quietly stops producing -- or produces under an id the plan never declared -- is a failure
  // rather than a smaller green matrix. Duplicates are flagged separately because set arithmetic
  // cannot see them: producing one cell twice still yields a set-equal result while the raw-file
  // numbering (and therefore any per-cell consumer that indexes by production order) has moved.
  {
    std::map<std::string, std::set<std::string>> declared, seen;
    std::map<std::string, std::string> idBlock;
    std::vector<std::string> blockOrder;
    std::set<std::string> planDup;
    for (const PlanEntry& e : plan) {
      if (declared.find(e.block) == declared.end()) blockOrder.push_back(e.block);
      declared[e.block].insert(e.id);
      if (!idBlock.insert({e.id, e.block}).second) planDup.insert(e.id);   // plan itself is inconsistent
    }
    std::set<std::string> orphans, dup, once;
    for (const std::string& id : emittedIds) {
      if (!once.insert(id).second) dup.insert(id);
      const auto it = idBlock.find(id);
      if (it == idBlock.end()) { orphans.insert(id); continue; }
      seen[it->second].insert(id);
    }
    bool bad = !orphans.empty() || !dup.empty() || !planDup.empty();
    std::fflush(stdout);   // keep the PLAN block after every S3-TRACE line, not spliced into one
    // A duplicate inside the plan would otherwise be invisible to the set arithmetic below: the
    // declared set stays 151 while the plan FILE holds 152 lines, both arms match, and re-pinning the
    // artifact would enshrine the duplicate. Name it.
    for (const std::string& pd : planDup)
      std::fprintf(stderr, "PLAN-INTERNAL-DUPLICATE %s\n", pd.c_str());
    std::size_t totalMissing = 0, totalExtra = 0;
    for (const std::string& b : blockOrder) {
      std::vector<std::string> missing, extra;
      for (const std::string& id : declared[b]) if (!seen[b].count(id)) missing.push_back(id);
      for (const std::string& id : seen[b]) if (!declared[b].count(id)) extra.push_back(id);
      std::fprintf(stderr, "PLAN %s declared=%zu emitted=%zu missing=%zu extra=%zu\n", b.c_str(),
                   declared[b].size(), seen[b].size(), missing.size(), extra.size());
      for (const std::string& m : missing)
        std::fprintf(stderr, "PLAN-MISSING %s %s\n", b.c_str(), m.c_str());
      for (const std::string& x : extra)
        std::fprintf(stderr, "PLAN-EXTRA %s %s\n", b.c_str(), x.c_str());
      totalMissing += missing.size();
      totalExtra += extra.size();
      if (!missing.empty() || !extra.empty()) bad = true;
    }
    for (const std::string& o : orphans) std::fprintf(stderr, "PLAN-ORPHAN %s\n", o.c_str());
    for (const std::string& d2 : dup) std::fprintf(stderr, "PLAN-DUPLICATE %s\n", d2.c_str());
    std::fprintf(stderr,
                 "PLAN-TOTAL blocks=%zu declared=%zu emitted=%zu missing=%zu extra=%zu orphans=%zu "
                 "duplicates=%zu plan_duplicates=%zu\n",
                 blockOrder.size(), idBlock.size(), emittedIds.size(), totalMissing, totalExtra,
                 orphans.size(), dup.size(), planDup.size());
    if (bad) {
      std::fprintf(stderr, "FATAL: produced cell set does not match the declared stimulus plan\n");
      gCode |= 8;
    }
  }

  std::size_t produced = 0, failed = 0;
  for (const auto& r : rows) {
    if (r.rfind("id\t", 0) == 0) continue;
    const auto c1 = r.find('\t');
    const auto c2 = r.find('\t', c1 + 1);
    const auto c3 = r.find('\t', c2 + 1);
    if (c2 != std::string::npos && c3 != std::string::npos) {
      const std::string sig = r.substr(c2 + 1, c3 - c2 - 1);
      if (sig.empty() || sig == "-") ++produced; else ++failed;
    }
  }
  // stdout is block-buffered when redirected and stderr is not, so without this flush the last
  // S3-TRACE line and this summary race for the same byte range and splice together in a captured
  // log -- which silently truncates the guard value a reader needs. The summary is the authoritative
  // line; make sure everything already printed has landed before writing it.
  std::fflush(stdout);
  std::fprintf(stderr, "gh19_s3_pulse_probe: %zu cells produced, %zu failed, exit_code=%d.\n",
               produced, failed, gCode);
  return gCode;
}
