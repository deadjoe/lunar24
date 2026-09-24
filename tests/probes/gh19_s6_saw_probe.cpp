// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_s6_saw_probe.cpp — task #120 (GH #19 S6) REAL-PRODUCT saw / invSaw morph-ring measurement
// probe.
//
// WHAT THIS IS. The saw/invSaw counterpart of gh19_s3_pulse_probe.cpp, and the S3 probe is its
// template on purpose: the plumbing below (the real entry chain, the declared stimulus plan, the
// per-cell metadata row, the plan reconciliation, the peak envelope) is the SAME plumbing with the
// same semantics, so a difference between an S3 number and an S6 number is attributable to the
// WAVEFORM FAMILY rather than to drift in the instrument. It drives the REAL product output through
// the ONE agreed entry (the same chain the host uses):
//     make_default_device_state -> encode -> decode -> engine.applyDeviceState ->
//     standalone_audio_engine.processBlock   (encapsulated by EngineHarness)
// and captures the rendered audio. It computes NO metric: the analysis is the separate instrument
// (tools/gh19_s6_saw_analyze.py) applied to these real samples. Every cell is either produced or an
// explicit hard failure (exit non-zero) — never a silent pass.
//
// WHY A SEPARATE PROBE (the same reason S3 gives, restated because it still applies). S3's evidence is
// SHA-pinned: tools/gh19_s3_pulse_acceptance.py refuses to measure if the pinned baseline report has
// moved (REFUSE BASELINE-REPORT-PIN). Adding cells to the S3 probe would invalidate that pin and force
// the S3 verdict to be re-run to reach a conclusion about an unrelated slice. This probe carries its
// own matrix and its own scenario file, so the S3 contract is untouched, and the SAME source file is
// compiled against both the uncorrected tree and the candidate tree — which is what makes the two arms
// a like-for-like comparison of one instrument rather than of two.
//
// OBSERVATION POINTS (real production taps, identical to S3):
//   dryA = VCO A, tapped BEFORE the chain
//   dryB = VCO B, tapped BEFORE the chain
//
// THE CELLS ARE THE MORPH RING, ON THE TWO STRETCHES THAT HAVE NO CORRECTION.
// The ring renders  y = (1-u) * node_k(p) + u * node_{k+1}(p)  on each of four stretches delimited by
// kRingEqual = {0, 0.25, 0.5, 0.75, 1}. Stretches 2 and 3 already carry a triangle BLAMP and a pulse
// BLEP; stretches 0 (saw -> invSaw) and 1 (invSaw -> sine) carry NOTHING, which is what S6 is about.
// The probe walks the morph knob and records the product's own output at each position:
//
//   * vco_{a,b}_sawmix_{sr}_{f}_m{TAG}   — the improvement and equality coordinates. `m{TAG}` is the
//       morph value x 100000, so `m6250` is morph = 0.0625. Positions and their stretch:
//         0.0000 m0      stretch 0 u=0.00  saw node exactly
//         0.0625 m6250   stretch 0 u=0.25
//         0.1250 m12500  stretch 0 u=0.50  THE ANTI-PHASE MIDPOINT
//         0.1875 m18750  stretch 0 u=0.75
//         0.2500 m25000  stretch 0 u=1.00  invSaw node exactly
//         0.3125 m31250  stretch 1 u=0.25
//         0.3750 m37500  stretch 1 u=0.50
//         0.4375 m43750  stretch 1 u=0.75
//         0.5000 m50000  stretch 1 u=1.00  sine node exactly
//   * vco_{a,b}_sawmixhi_{sr}_{f}_m{TAG} — the same measurement past the point where a two-point step
//       kernel has a band-limited referent. Rendered and reported, never gated.
//
// WHY THE ANTI-PHASE MIDPOINT GETS A DIFFERENT LOWER BOUND, AND IT IS NOT A CONVENIENCE.
// At morph = 0.125 the mixture is  0.5*sawShape(p) + 0.5*(-sawShape(p))  which is EXACTLY zero for
// every phase (0.5*a is exact, and x + (-x) is exact, for any finite a). The same point is where the
// mixture's true jump  J(u) = (1-u)(-2) + u(+2) = -2 + 4u  vanishes, because both go to zero together.
// So on that family "the output is silent" is the CORRECT reading, not a failure: the S3 probe's
// lower bound would flag it `silent` and refuse to produce the cell, and the arm would fail for a
// reason that has nothing to do with the waveform. The bound is therefore 0.0 for that family only,
// and the demand that the output really is zero is made explicitly by the criterion
// `antiphase_midpoint_max_abs_output = 0.0` in report/gh19-s6-saw-aa/acceptance_criteria.tsv. That
// criterion is a NECESSARY regression guard, not a discriminating one — see the criteria file's
// header for why the unscaled arm also evaluates to zero there and cannot be told apart by it.
//
// STATE IS APPLIED WHOLE, SO THE CELLS ARE EXACTLY STATIONARY. applyDeviceState routes the vco panel
// knobs (pw, morph, pwm depth) through applyDspParam's isContinuousSmoothingParam_ branch, which is
// SNAP-ONLY: reset(v) + applySmoothedControl_(id, v), never a ramp. So after load() the DSP holds the
// EXACT state value and morph_ does not drift across the analysis window. Without that property a
// per-sample band-limited reference could not be aligned to the product at all; it is relied on here
// and it is checkable by re-reading Vco::morph() from the runtime.
//
// OUTPUT (--out <dir>, default ./report/gh19-s6-saw-probe):
//   gh19_s6_plan.tsv        THE DECLARED STIMULUS PLAN (block, id), written before any render
//   gh19_s6_scenarios.tsv   one metadata row per cell
//   gh19_s6_scnNNN.raw      little-endian f64 samples of that cell's observation window
//
// EXIT CODE: 0 = every declared cell was produced and the produced set matched the declared plan;
// non-zero = a cell was not produced (bit 0 = 1), a raw/trace write failed (bit 1 = 2), a cell left
// the validity envelope (bit 2 = 4), or the produced cell set differs from the declared stimulus plan
// -- missing, extra, orphaned or duplicated (bit 3 = 8). There is no empty-exit-0. The bit numbering
// is deliberately the S3 probe's, so a reader comparing the two log formats is not comparing two
// different alphabets. Bit 2 is the S3 probe's trace-hook invariance guard: this probe has no traced
// cells in this revision, so bit 2 is unused here and reserved rather than renumbered.

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

// ---------------------------------------------------------------------------------------------
// THE DECLARED AXES. These are the numbers the pinned criteria file is reconciled against:
// report/gh19-s6-saw-aa/acceptance_criteria.tsv declares the same axis_set rows, and the gate
// RE-DERIVES the required cell set from them and requires it to equal the enumeration in that file.
// Editing an axis here without re-pinning the criteria file makes the two disagree, which is a
// refusal rather than a smaller green matrix.
//
// The grid is the approved 9-point position set cut in two, NOT a reduced frequency dimension:
// every A-layer position keeps all three frequencies.
// ---------------------------------------------------------------------------------------------
const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};

struct SawPos { double morph; const char* tag; };

// A layer: positions on stretches 0 and 1 whose uncorrected mixture has a genuine jump, at all three
// frequencies. 4 positions x 3 f x 4 sr x 2 sides = 96.
const std::vector<SawPos> kAPos = {
    {0.0, "m0"}, {0.0625, "m6250"}, {0.1875, "m18750"}, {0.25, "m25000"}};
const std::vector<double> kAFreqs = {220.0, 440.0, 880.0};

// B layer: the remaining off-centre positions, one frequency. 3 positions x 1 f x 4 sr x 2 sides = 24.
const std::vector<SawPos> kBPos = {{0.3125, "m31250"}, {0.375, "m37500"}, {0.4375, "m43750"}};
const std::vector<double> kBfreqs = {440.0};

// The equality family: the two positions where a 6 dB improvement criterion is UNSATISFIABLE, so it
// is asserted as an exact equality instead. 2 positions x 1 f x 4 sr x 2 sides = 16.
//   0.125 m12500 — amplitude 0 AND jump 0; the correct output is identically zero.
//   0.500 m50000 — the SINE node (NOT the anti-phase midpoint); already smooth, and the new
//                  correction terms have coefficient exactly 0 there, so the output must be
//                  bit-identical to the pre-S6 output.
const std::vector<SawPos> kEqPos = {{0.125, "m12500"}, {0.5, "m50000"}};
const std::vector<double> kEqFreqs = {440.0};

// The report-only family: past the point where a two-point step kernel has a band-limited referent.
// 2 sides x 2 sr x 2 f x 4 positions = 32. Rendered, reported, never gated.
const std::vector<SawPos> kHiPos = {
    {0.0, "m0"}, {0.0625, "m6250"}, {0.25, "m25000"}, {0.3125, "m31250"}};
const std::vector<double> kHiFreqs = {3520.0, 5000.0};
const std::vector<double> kHiSrs = {44100.0, 96000.0};

// The pw knobs are not read by the saw / invSaw / sine nodes, but they are set to a fixed value so
// the state is fully determined and two runs of this probe are the same stimulus.
constexpr double kFixedPw = 0.5;
constexpr double kFixedPwmDepth = 0.0;

// ---------------------------------------------------------------------------------------------
// THE DECLARED STIMULUS PLAN (inherited from the S3 probe, item 1 of @Codex 8aba3aeb / 32b13ced)
// ---------------------------------------------------------------------------------------------
// THE DEFECT THIS EXISTS FOR. A delta tool that locked coverage by comparing the two arms' own
// analyzer output is a statement about the two arms agreeing with EACH OTHER, and it is satisfied by
// deleting a cell from both of them. The expectation has to come from somewhere the run cannot edit.
//
// WHAT THIS IS. The plan is the COMPLETE stimulus set the probe declares, enumerated from the
// declared axes above by buildPlan(): no rendering, no capture, no DeviceState, no file I/O. main()
// writes it BEFORE it renders anything and writes it unconditionally, so a cell that fails to render
// is a MISSING plan entry, never an entry that was never expected. A run whose own output is empty
// still produces the full plan.
//
// WHAT IT IS NOT. It is not derived from, filtered by, or reconciled against the produced rows at
// generation time. The reconciliation runs the other way -- after rendering, the produced id set is
// checked against the plan and the probe exits non-zero on any difference (bit 3).
constexpr int kPlanRev = 1;

std::string sawCellId(const std::string& side, bool hi, double sr, double ft, const char* tag) {
  return "vco_" + side + (hi ? "_sawmixhi_" : "_sawmix_") + std::to_string((int)sr) + "_" +
         std::to_string((int)ft) + "_" + tag;
}

struct PlanEntry { std::string block; std::string id; };

std::vector<PlanEntry> buildPlan() {
  std::vector<PlanEntry> p;
  for (const std::string& side : {std::string("a"), std::string("b")}) {
    for (const SawPos& pos : kAPos) {
      for (double sr : kSrs) {
        for (double ft : kAFreqs) p.push_back({"sawmix_a", sawCellId(side, false, sr, ft, pos.tag)});
      }
    }
    for (const SawPos& pos : kBPos) {
      for (double sr : kSrs) {
        for (double ft : kBfreqs) p.push_back({"sawmix_b", sawCellId(side, false, sr, ft, pos.tag)});
      }
    }
    for (const SawPos& pos : kEqPos) {
      for (double sr : kSrs) {
        for (double ft : kEqFreqs) {
          p.push_back({"sawmix_eq", sawCellId(side, false, sr, ft, pos.tag)});
        }
      }
    }
    for (const SawPos& pos : kHiPos) {
      for (double sr : kHiSrs) {
        for (double ft : kHiFreqs) p.push_back({"sawmix_hi", sawCellId(side, true, sr, ft, pos.tag)});
      }
    }
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
  double peak = 0.0;
  double f0 = 0.0;
};

// The VCO base frequency the product is configured with, reached by oct_sel x tune — the same recipe
// the S3 probe and gh19_alias_probe's triangle cells use. `vco_a_oct_sel` is a 3-position selector
// over octaves {-1, 0, +3} and `tune` is a continuous knob over [-1, +1] octaves, so the
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

// The upper edge of the cell-validity envelope, in the device domain. A saw / invSaw / sine node sits
// inside [-1, +1], so 1.10 accepts the product's own waveform (with the 10% headroom the S3 pulse
// probe gives its +-0.5 pulse) and rejects a cell that is not one.
//
// WHY IT IS A FLAG AND NOT A CONSTANT. The envelope is a validity guard for the CANDIDATE arm: it
// says "this cell is the waveform the reference describes". Directed negative controls are mutations
// that are *predicted* to leave the envelope by construction -- a wrong-sign edge correction drives
// the +-2 jump the wrong way and there is no bounded way to express that error, because the correct
// correction is exactly what keeps the step inside [-1, +1]. With the default bound the mutated arms
// would abort as `over-scale` and the named criterion would never be evaluated, leaving the control
// vacuous (an aborted arm proves the toolchain reacted, not that the metric sees the defect). The
// negative-control runner therefore widens the bound for those arms only and prints the bound it used
// next to the per-cell peak. Widening it for a mutation arm cannot flatter the candidate: the
// candidate's own numbers come from runs that never pass this flag.
static double gPeakHi = 1.10;

// Render `frames` frames from one engine load and capture `tap`.
//   peak guard [lo, gPeakHi]: the un-scaled-ideal detector. `lo` is a parameter and not a constant
//   because the anti-phase midpoint family is CORRECTLY silent — see the file header. Passing
//   0.0 there disables the lower bound for that family only, and the exact-zero demand is made by
//   the pinned criterion instead of by this guard.
Cap captureSaw(const DeviceStateV1& st, double sr, std::size_t frames, const std::string& tap,
               double lo) {
  const double hi = gPeakHi;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  if (!h.render(static_cast<int>(frames), 0.0)) { c.signal = "render-failed"; return c; }
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

// The lower bound for a position: 0.0 exactly at the anti-phase midpoint, the usual floor elsewhere.
double lowerBoundFor(double morph) { return (morph == 0.125) ? 0.0 : 1e-4; }

DeviceStateV1 sawState(const std::string& side, double ft, double morph) {
  DeviceStateV1 st = make_default_device_state(kProbeSeed);
  applyFTarget(st, side, ft);
  slot(st, side == "a" ? ParameterId::vco_a_morph : ParameterId::vco_b_morph) = morph;
  slot(st, side == "a" ? ParameterId::vco_a_pw : ParameterId::vco_b_pw) = kFixedPw;
  slot(st, side == "a" ? ParameterId::vco_a_pwm : ParameterId::vco_b_pwm) = kFixedPwmDepth;
  if (side == "b") {
    // The default graph feeds vco_b.cv_in from vco_a.dry_out (task #83 A->B route), so with a
    // non-zero cv_amt dryB is audio-rate FM-ed and not a steady single carrier. The legal panel knob
    // cv_amt = 0 makes B the clean carrier this comparison needs; identical to the S3 probe's and
    // gh19_alias_probe's vco_b recipe, and it is what makes the A and B cells one stimulus measured
    // on two sides rather than two different experiments.
    slot(st, ParameterId::vco_b_cv_amt) = 0.0;
  }
  return st;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh19-s6-saw-probe";
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
    if (std::strcmp(argv[i], "--peak-hi") == 0) gPeakHi = std::strtod(argv[i + 1], nullptr);
  }
  // Printed, not assumed: a reader of a mutated arm's log has to be able to see that the envelope was
  // widened for it and by how much, without reading the runner.
  if (gPeakHi != 1.10) {
    std::fprintf(stderr, "gh19_s6_saw_probe: cell-validity peak envelope widened to %.6g "
                         "(default 1.10) -- this arm's numbers are NOT candidate-arm numbers.\n",
                 gPeakHi);
  }
  std::error_code ec;
  std::filesystem::create_directories(out, ec);   // portable recursive create (MSVC has no mkdir -p)

  int gCode = 0;   // bit0 = cell not produced, bit1 = write failed, bit2 = reserved (S3's trace-hook
                   // guard; this revision has no traced cells), bit3 = produced set != declared plan

  // ---- THE DECLARED STIMULUS PLAN IS WRITTEN FIRST, BEFORE ANY RENDER ----
  // Built from the declared axes alone (buildPlan touches no DeviceState, no engine and no file), and
  // written unconditionally, so a run in which every capture fails still emits the complete expected
  // set. Everything below reconciles against THIS, not against what happened to render.
  const std::vector<PlanEntry> plan = buildPlan();
  {
    std::vector<std::string> plines;
    plines.push_back("plan_rev\t" + std::to_string(kPlanRev));
    plines.push_back("block\tid");
    for (const PlanEntry& e : plan) plines.push_back(e.block + "\t" + e.id);
    if (!writeTsv(out + "/gh19_s6_plan.tsv", plines)) {
      std::fprintf(stderr, "FATAL: could not write the declared stimulus plan\n");
      gCode |= 2;
    }
    std::printf("S6-PLAN plan_rev=%d cells=%zu\n", kPlanRev, plan.size());
  }
  std::vector<std::string> emittedIds;   // in emission order; reconciled against `plan` at the end

  std::vector<std::string> rows;
  // `warm`/`win` are emitted rather than left for the analyzer to assume: the analysis window is
  // frames [warm, warm+win) of the raw, and an analyzer that hardcoded its own guess would be reading
  // a different window than the probe wrote while still looking correct.
  rows.push_back("id\tpath\tsignal\tsr_hz\tf_target_hz\tside\tduty_param\tmorph\tpw_depth"
                 "\tlfo_wave\tf0_meas_hz\tphi0_model\tpeak\tchannel\tcable\twarm\twin"
                 "\traw\tsamples\tsrc");
  int scnIdx = 0;

  auto emit = [&](const std::string& id, const std::string& path, double sr, double ft,
                  const std::string& side, double morph, const std::string& channel, const Cap& c) {
    emittedIds.push_back(id);
    std::string raw = "";
    if (c.ok) {
      raw = "gh19_s6_scn" + std::to_string(++scnIdx) + ".raw";
      if (!writeRaw(out + "/" + raw, c.x)) {
        std::fprintf(stderr, "FATAL %s: raw write failed (%s)\n", id.c_str(), raw.c_str());
        gCode |= 2;
      }
    } else {
      std::fprintf(stderr, "FATAL %s: cell not produced [%s]\n", id.c_str(), c.signal.c_str());
      gCode |= 1;
    }
    // The MODEL's starting phase for this cell, stated so the n+1 convention is auditable from the
    // manifest alone. The engine advances cumPitch_ and THEN emits, so the phase of the sample at
    // absolute frame index n (n = 0 is the first frame after load) is frac((n+1)*f0/sr); at n = 0
    // that is frac(f0/sr), and phi0_model is exactly that, in degrees. `ft` is the nominal base the
    // oct/tune recipe selects. The S6 analyzer inherits this same convention through S3's phi_at.
    const double phi0 = 360.0 * ((ft / sr) - std::floor(ft / sr));
    rows.push_back(id + "\t" + path + "\t" + safeTsv(c.signal) +
                   "\t" + std::to_string(sr) +
                   "\t" + std::to_string(ft) +
                   "\t" + side +
                   "\t" + fmtExact(kFixedPw) +
                   "\t" + fmtExact(morph) +
                   "\t" + fmtExact(kFixedPwmDepth) +
                   "\t-" +
                   "\t" + fmtExact(c.f0) +
                   "\t" + fmtExact(phi0) +
                   "\t" + fmtExact(c.peak) +
                   "\t" + channel +
                   "\t" + "none" +
                   "\t" + std::to_string(kWarm) +
                   "\t" + std::to_string(kWin) +
                   "\t" + raw +
                   "\t" + std::to_string(c.x.size()) +
                   "\t-");
  };

  const std::size_t total = kWarm + kWin;

  // ---- THE RENDER LOOP. One pass per declared family, each reading the SAME tables buildPlan reads.
  auto renderFamily = [&](const std::string& block, const std::vector<SawPos>& pos,
                          const std::vector<double>& freqs, const std::vector<double>& srs,
                          bool hi) {
    for (const std::string& side : {std::string("a"), std::string("b")}) {
      const std::string tap = (side == "a") ? "dry_a" : "dry_b";
      for (const SawPos& p : pos) {
        for (double sr : srs) {
          for (double ft : freqs) {
            const DeviceStateV1 st = sawState(side, ft, p.morph);
            const std::string id = sawCellId(side, hi, sr, ft, p.tag);
            Cap c = captureSaw(st, sr, total, tap, lowerBoundFor(p.morph));
            const std::string path =
                "vco_" + side + (hi ? "_sawmixhi" : "_sawmix");
            emit(id, path, sr, ft, side, p.morph, tap, c);
          }
        }
      }
    }
    std::printf("S6-BLOCK %s done\n", block.c_str());
  };

  renderFamily("sawmix_a", kAPos, kAFreqs, kSrs, false);
  renderFamily("sawmix_b", kBPos, kBfreqs, kSrs, false);
  renderFamily("sawmix_eq", kEqPos, kEqFreqs, kSrs, false);
  renderFamily("sawmix_hi", kHiPos, kHiFreqs, kHiSrs, true);

  std::string tsv = out + "/gh19_s6_scenarios.tsv";
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
    std::fflush(stdout);   // keep the PLAN block on its own lines, not spliced into one
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
  // progress line and this summary race for the same byte range and splice together in a captured
  // log -- which silently truncates a value a reader needs. The summary is the authoritative line;
  // make sure everything already printed has landed before writing it.
  std::fflush(stdout);
  std::fprintf(stderr, "gh19_s6_saw_probe: %zu cells produced, %zu failed, exit_code=%d.\n",
               produced, failed, gCode);
  return gCode;
}
