// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh19_prb_degen_probe.cpp — task #121 (GH #19 S6 PR-B) DEGENERATE-DISPATCH driver.
//
// WHAT THIS IS FOR. The S6/PR-B acceptance criteria assert that the STATIC pulse route is a SPECIAL
// CASE of the DYNAMIC route: on a cell whose duty track is CONSTANT, the dynamic route's rendered
// output must be BYTE-IDENTICAL to the static route's. That is a claim about WIRING (one
// normalisation, one denominator, one bandwidth declaration, one dispatch), not about accuracy, and
// it must be reported as wiring evidence.
//
// THE ARM CANNOT SUPPLY THE CELLS. The S3 probe's cell table is a COMPILED-IN literal
// (gh19_s3_pulse_probe.cpp:159-165 kDynSpecs, read by buildPlan() at :189-210) and its depth axis is
// {0.5, 1.0} — there is NO depth-0 spec, so no constant-duty dynamic cell exists in the arm to
// enumerate. Shadow-include cannot reach it either: shadow-include replaces HEADERS, and this cell
// table is a literal inside a .cpp, not a header. (Both facts were established before this file
// existed; the director ruled that a separate minimal driver is the route, and that `tests/probes/`
// must stay byte-for-byte untouched so the two arms remain a like-for-like comparison of ONE
// measurement instrument.) So this file renders the constructed cells, and `tests/probes/` keeps a
// ZERO diff.
//
// PRODUCT DISPATCH POINT — why a constant-duty cell is reachable without touching the product.
// The product does not dispatch on whether the duty MOVES; it dispatches on whether the PWM cable is
// BOUND, per sample:
//     machine_runtime.h:3098-3100 (VCO A) / :3128-3130 (VCO B):
//         double pwm = 0.0;
//         if (pwmInBoundA_) resolveControlSink_(pwmInA_, pwm, driveGraph);
//         vcA_.setPwCv(pwm);
// and the binding itself is set at :502-512 (`setVcoPwmBindings`). The duty formula is
//     vco.h:182-184:  effectiveDuty = clamp(duty_ + pwDepth_ * pwCv_ / kPwmCvFullScaleVolts, .001, .999)
// with `setPwCv` forcing a non-finite input to 0.0 (vco.h:179) and `setPwDepth` forcing a non-finite
// input to 0.0 (vco.h:178). So at pwDepth_ == 0 the product computes `duty_ + 0.0 * finite` == `duty_`
// EXACTLY, in IEEE-754, on every sample — the cable may carry a fully LIVE moving signal and the duty
// still never moves. That is the degenerate trajectory, and it is the strongest form of it: the
// degeneracy is produced by the depth multiplication alone, not by parking the source.
//
// THE CELLS (12 = 6 pairs). Both cells of a pair share sr, f0, base width 0.5, morph 1.0, the same
// LFO (triangle, 2 Hz) and the same cable, and differ in EXACTLY ONE thing — the depth knob:
//   dynroute_const_duty_<sr>_<f0>_b50_d0    depth 0.0  -> duty constant, source still moving
//   dynroute_moving_duty_<sr>_<f0>_b50_d50  depth 0.5  -> duty genuinely moves
// (b = base width, d = modulation depth — the id tokens are DERIVED from the constants in this file
// by dynrouteId(), not typed, so an id cannot drift away from the stimulus it names.)
// sr in {44100, 96000}, f0 in {220, 440, 880}: the grid of this slice, and the (sr, f0) of the six
// static `vco_a_pulse_<sr>_<f0>_pw50` cells the identity is measured against.
// The moving cell is state-identical to the arm's own `vco_a_dynpwm_<sr>_<f0>_tri_r2_b50_d50`
// (kDynSpecs[0] = {1.0, 0.50, 0.5, 2.0}), which is deliberate: it gives the pipeline a cell BOTH
// instruments cover, so this driver's fidelity can be checked against the S3 probe directly instead
// of only against itself.
//
// THREE CONTROLS, ALL MANDATORY (each catches a different way this file could be lying):
//   1. TRACE-HOOK INVARIANCE (S3's own guard, :732-756). The per-frame hook only READS, so the
//      traced and untraced renders of the same state must be byte-identical. Without this, a cell's
//      trace would describe a different stimulus from the one its audio came from.
//   2. COMPANION: STATE-FACT vs connect() (probe :311-316 kConnect/kState). The cable-as-a-state-fact
//      arm and the post-load connect()+rebuild() arm must be byte-identical, and their published CV
//      traces too. THIS IS THE ONE THAT DISSOLVES THE DUPLICATION FACE: this file contains its own
//      copy of the two-line cable encoding (:322-329 of the probe) because it cannot include a .cpp;
//      proving the two admission routes are ONE SIGNAL changes "I copied it" into "measured
//      equivalent". Both raws are written so a later reader re-diffs them independently rather than
//      trusting this file's own number.
//   3. DECLARATION-AS-DECLARED, on two separately-named surfaces (bit 5 and bit 6; see EXIT CODE).
//      a. STIMULUS (bit 5, an input): every cell's traced SOURCE span must be > 0. A dead source
//         cannot produce a moving duty, so a const cell whose "degeneracy" came from a dead LFO would
//         be static-identical FOR THE WRONG REASON — the vacuity the criteria's non-vacuity control
//         exists to catch, caught here first.
//      b. PREMISE (bit 6, a product-derived quantity): the const cell's effectiveDuty() span must be
//         EXACTLY 0, and the moving cell's must be > 0.01. This is the identity criterion's own
//         ANTECEDENT ("on a cell where the duty track is constant"), so when it fails there is no
//         constant-duty cell to judge and the correct outcome is a refusal rather than a verdict.
//      Reported as two quantities as well as two bits (`src_span`/`duty_span` on stdout and in the
//      scenario table), so a failure can be quoted as a number and not only as a code.
//
// OUTPUT (--out <dir>, default ./report/gh19-prb-degen-probe):
//   gh19_prb_degen_plan.tsv        THE DECLARED STIMULUS PLAN (block, id), written BEFORE any render
//   gh19_prb_degen_scenarios.tsv   one row per cell: the arm's 20 collector columns, then this
//                                  driver's own evidence columns (duty, companion_raw, hook_diff,
//                                  companion_diff, src_span_v, duty_span)
//   gh19_prb_degen_scnNNN.raw      little-endian f64 samples of the cell's kState window
//   gh19_prb_degen_srcNNN.tsv      per-frame LFO-A CV-OUT record, same 2-column format as the arm's
//   gh19_prb_degen_dutyNNN.tsv     per-frame effectiveDuty() record (the degeneracy's own witness)
//   gh19_prb_degen_cmpNNN.raw      the kConnect companion's window, for independent re-diff
//
// EXIT CODE (bits 0-3 mirror gh19_s3_pulse_probe.cpp; 4-6 are this file's own named surfaces):
//   bit 0 = a declared cell was not produced
//   bit 1 = a raw/tsv write failed
//   bit 2 = a trace-hook invariance guard failed
//   bit 3 = the produced cell set differs from the declared stimulus plan
//   bit 4 = the companion identity (state-fact vs connect()) failed
//   bit 5 = STIMULUS-AS-DECLARED failed: the traced source does not match the cell's declaration
//   bit 6 = PREMISE-AS-DECLARED failed: the product's effectiveDuty() does not match the declaration
// There is no empty-exit-0: a run that renders nothing still writes the full plan and fails bit 3.
//
// WHY 5 AND 6 ARE TWO BITS AND NOT ONE. They read two different quantities -- bit 5 the traced source
// (an INPUT to the product), bit 6 the traced effectiveDuty() (what the product DID with that input) --
// and folding them together would leave a reader unable to tell "the stimulus was not what we declared"
// from "the product did not do what we declared" without reading prose. The two numbers separate them
// mechanically, which matters for exactly one arm: the degen-dispatch control (tools/
// stage_gh19_prb_shadow.py) fires bit 6, so its exit code IS the classification -- it demonstrates that
// the premise check is live, and says nothing about the identity criterion, which it never reaches.

// This driver writes .raw/.tsv with std::fopen, which MSVC's secure-CRT deprecation (C4996) promotes
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

constexpr std::uint64_t kProbeSeed = 0x4C554E4152ULL;   // same LUNAR hex as the harness default
constexpr std::size_t kWarm = 8192;                     // settle transients
constexpr std::size_t kWin = 16384;                     // analysis window (frames [warm, warm+win))
constexpr int kPlanRev = 1;

// The declared axes. kSrs x kFs x {const, moving} = 12 cells.
const std::vector<double> kSrs = {44100.0, 96000.0};
const std::vector<double> kFs = {220.0, 440.0, 880.0};
const double kBaseWidth = 0.50;      // the PWM knob (vco_a_pw) = the base duty, same as the static cell
const double kConstDepth = 0.0;      // the depth knob (vco_a_pwm) at 0: the degeneracy
const double kMovingDepth = 0.5;     // the depth knob live: the duty really moves
const double kLfoWave = 1.0;         // triangle (registry: 1.0 = tri, 0.0 = square)
const double kLfoRate = 2.0;         // within the registry's rate range
// The moving cell's duty span must exceed this to count as "genuinely moving". At depth 0.5 with a
// 0..+10 V triangle the span is ~0.5, so 0.01 is two orders of magnitude of headroom: this catches a
// DEAD stimulus, not a marginal one, and it is not a fitted threshold.
const double kMovingDutySpanMin = 0.01;

// The upper edge of the cell-validity envelope, in the device domain (same value and same reasoning
// as gh19_s3_pulse_probe.cpp:345). A pure pulse at norm = 1.0 sits at +/-0.5, so 0.55 accepts the
// product's own waveform and rejects a cell that is not one.
//
// WHY IT IS A FLAG AND NOT A CONSTANT. The envelope is a validity guard for the CANDIDATE arm. It is
// NOT a physical invariant of the render, and the directed negative controls are mutations that are
// *predicted* to leave the envelope by construction — a perturbation of the shared denominator or a
// removed degenerate dispatch can drive the pulse step outside [-1, +1], and there is no bounded way
// to express that error, because the correct dispatch is exactly what keeps it inside. With the
// default bound such a mutant arm would abort as `over-scale` and the named criterion would never be
// evaluated, leaving the control vacuous (an aborted arm proves the toolchain reacted, not that the
// metric sees the defect). The negative-control runner therefore widens the bound FOR THOSE ARMS
// ONLY and prints the bound it used next to the per-cell peak. Widening it for a mutation arm cannot
// flatter the candidate: the candidate's own numbers come from runs that never pass this flag.
static double gPeakHi = 0.55;

struct PlanEntry { std::string block; std::string id; };

// The id encodes the stimulus, and it is BUILT FROM the constants rather than written out, so the
// `_b<base>_d<depth>` tokens cannot disagree with the state this file renders. (The arm's own probe
// does the same at :178-185; reading those tokens as names instead of reading their generator is a
// mistake this repo has already paid for once.)
std::string dynrouteId(const std::string& kind, double sr, double ft, double pwBase, double pwDepth) {
  return "dynroute_" + kind + "_" + std::to_string(static_cast<int>(sr)) + "_" +
         std::to_string(static_cast<int>(ft)) +
         "_b" + std::to_string(static_cast<int>(std::lround(pwBase * 100.0))) +
         "_d" + std::to_string(static_cast<int>(std::lround(pwDepth * 100.0)));
}

struct CellSpec { double sr; double ft; double pwDepth; std::string block; std::string id; };

std::vector<CellSpec> buildPlan() {
  std::vector<CellSpec> p;
  for (double sr : kSrs) {
    for (double ft : kFs) {
      p.push_back({sr, ft, kConstDepth, "degen_const",
                   dynrouteId("const_duty", sr, ft, kBaseWidth, kConstDepth)});
      p.push_back({sr, ft, kMovingDepth, "degen_moving",
                   dynrouteId("moving_duty", sr, ft, kBaseWidth, kMovingDepth)});
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
double spanOf(const std::vector<double>& c) {
  if (c.empty()) return 0.0;
  double lo = c[0], hi = c[0];
  for (double v : c) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return hi - lo;
}
// Sign changes per window / 2 -> cycle count -> measured frequency (same as the arm's probe).
double zcrFreq(const std::vector<double>& c, double sr) {
  int z = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++z;
  }
  return static_cast<double>(z) * sr / (2.0 * static_cast<double>(c.size()));
}
// The largest |a[i] - b[i]| over the two streams, or -1.0 when they are not even comparable (a
// different length is not a small difference, and returning 0.0 for it would be the worst possible
// reading). Used for all three controls, so "identical" means the same thing everywhere in this file.
double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return -1.0;
  double d = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double x = std::fabs(a[i] - b[i]);
    if (x > d) d = x;
  }
  return d;
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
// 17 significant digits: the identity comparison is byte-level, so a %.6g round-trip would silently
// coarsen the very thing being compared.
std::string fmtExact(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.17g", v);
  return std::string(b);
}
std::string safeTsv(const std::string& s) { return s.empty() ? "-" : s; }

// The VCO base frequency the product is configured with, reached by oct_sel x tune.
//
// COPIED, DELIBERATELY, FROM tests/probes/gh19_s3_pulse_probe.cpp:289-309. It cannot be shared
// without editing tests/probes/, which this slice is forbidden to touch. The copy is SELF-POLICING
// rather than trusted: the static cells the identity is measured against were produced by the
// probe's own copy, so if this recipe ever drifts from it the const cell stops matching the static
// cell and the identity goes red — a drifted copy cannot pass quietly.
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

// The cell state. Both cells of a pair are built by this ONE function and differ only in `pwDepth`,
// which is what makes the pair a single-variable comparison: the const and moving cells share the
// source, the base width, the morph coordinate and the cable, so any difference between them (and
// any difference between the const cell and the static cell) is attributable to the depth knob.
//
// The LFO is configured EXPLICITLY rather than left at its default: a declared stimulus can be
// audited from this file, and the point of the const cell is that its source is genuinely moving.
// (No default cable originates at lfo_a_cv_out — the only mention of that jack in the default
// machine definition is its own output binding, machine_definition.h:417 — so with the cable absent
// the LFO reaches nothing; with it present it reaches the PWM sink, and only there.)
DeviceStateV1 degenState(double ft, double pwDepth) {
  DeviceStateV1 st = make_default_device_state(kProbeSeed);
  applyFTarget(st, "a", ft);
  slot(st, ParameterId::vco_a_morph) = 1.0;      // morph 1.0 = the ring's pulse NODE exactly
  slot(st, ParameterId::vco_a_pw) = kBaseWidth;
  slot(st, ParameterId::vco_a_pwm) = pwDepth;
  slot(st, ParameterId::lfo_a_wave) = kLfoWave;
  slot(st, ParameterId::lfo_a_rate) = kLfoRate;
  slot(st, ParameterId::lfo_a_speed_mult) = 0.0;   // x1
  return st;
}

// Declare the cable as a STATE FACT (the restore path, the user line). The index is the SERIALIZED
// JackId, not a dense index, exactly as the probe's withPwmCable does at :322-329 — this is the
// duplicated two-line encoding the companion control below exists to dissolve.
DeviceStateV1 withPwmCable(const DeviceStateV1& st) {
  DeviceStateV1 out = st;
  const std::uint32_t sink = static_cast<std::uint32_t>(lunar24::core::JackId::vco_a_pwm_in);
  out.inputCable[sink] = 1u;
  out.cableSource[sink] = lunar24::core::JackId::lfo_a_cv_out;
  return out;
}

enum class CableMode { kConnect, kState };   // kState = the restore path (primary)

struct Cap {
  bool ok = false;
  std::string signal;        // "" = produced; else a hard-failure reason
  std::vector<double> x;     // the dry_a window
  std::vector<double> src;   // per-frame LFO-A CV OUT (traced runs only)
  std::vector<double> duty;  // per-frame effectiveDuty() (traced runs only)
  double peak = 0.0;
  double f0 = 0.0;
};

// Render `frames` frames from one engine load and capture dry_a.
//   `traced` runs additionally receive LFO A's published CV OUT and the live effectiveDuty() once per
//   frame via renderSampled's post-frame const hook. That hook only READS, so it cannot perturb what
//   it measures; main() asserts that by re-rendering the same state with no hook and requiring the
//   two audio streams to be byte-identical.
Cap renderCell(const DeviceStateV1& st, double sr, std::size_t frames, CableMode cm, bool traced) {
  const double lo = 1e-4, hi = gPeakHi;
  Cap c;
  EngineHarness h;
  if (!h.load(st, sr)) { c.signal = "load-rejected"; return c; }
  lunar24::core::SynthRuntime* rt = h.producerRuntime();
  if (rt == nullptr) { c.signal = "no-runtime"; return c; }
  if (cm == CableMode::kConnect) {
    if (!rt->connect(lunar24::core::JackId::lfo_a_cv_out, lunar24::core::JackId::vco_a_pwm_in)) {
      c.signal = "pwm-connect-rejected";
      return c;
    }
    // The cable is only real once the plan is rebuilt off the audio path (the same off-thread
    // rebuild the host performs after a repatch).
    if (!rt->rebuild()) { c.signal = "pwm-rebuild-rejected"; return c; }
  }
  if (traced) {
    c.src.reserve(frames);
    c.duty.reserve(frames);
    if (!h.renderSampled(static_cast<int>(frames), 0.0, [&](const lunar24::core::SynthRuntime& r) {
          c.src.push_back(r.controlVoltageAt(lunar24::core::JackId::lfo_a_cv_out));
          c.duty.push_back(r.vcoAEffectiveDuty());
        })) { c.signal = "render-failed"; return c; }
  } else if (!h.render(static_cast<int>(frames), 0.0)) {
    c.signal = "render-failed";
    return c;
  }
  if (h.dryA().size() < frames) { c.signal = "short-render"; return c; }
  c.x.assign(h.dryA().begin(), h.dryA().begin() + static_cast<std::ptrdiff_t>(frames));
  c.peak = peakOf(c.x);
  c.f0 = zcrFreq(c.x, sr);
  if (!allFinite(c.x)) { c.signal = "non-finite"; return c; }
  if (c.peak < lo) { c.signal = "silent"; return c; }
  if (c.peak > hi) { c.signal = "over-scale"; return c; }
  c.ok = true;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "report/gh19-prb-degen-probe";
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
    if (std::strcmp(argv[i], "--peak-hi") == 0) gPeakHi = std::strtod(argv[i + 1], nullptr);
  }
  // Printed, not assumed: a reader of a mutated arm's log has to be able to see that the envelope was
  // widened for it and by how much, without reading the runner.
  if (gPeakHi != 0.55) {
    std::fprintf(stderr, "gh19_prb_degen_probe: cell-validity peak envelope widened to %.6g "
                         "(default 0.55) -- this arm's numbers are NOT candidate-arm numbers.\n",
                 gPeakHi);
  }
  std::error_code ec;
  std::filesystem::create_directories(out, ec);   // portable recursive create (MSVC has no mkdir -p)

  int gCode = 0;   // bit0 cell not produced, bit1 write failed, bit2 hook guard failed,
                   // bit3 produced != plan, bit4 companion identity failed, bit5 stimulus mismatch

  // ---- THE DECLARED STIMULUS PLAN IS WRITTEN FIRST, BEFORE ANY RENDER ----
  // Built from the declared axes alone (buildPlan touches no DeviceState, no engine and no file), and
  // written unconditionally, so a run in which every capture fails still emits the complete expected
  // set. Everything below reconciles against THIS, not against what happened to render.
  const std::vector<CellSpec> plan = buildPlan();
  {
    std::vector<std::string> plines;
    plines.push_back("plan_rev\t" + std::to_string(kPlanRev));
    plines.push_back("block\tid");
    for (const CellSpec& e : plan) plines.push_back(e.block + "\t" + e.id);
    if (!writeTsv(out + "/gh19_prb_degen_plan.tsv", plines)) {
      std::fprintf(stderr, "FATAL: could not write the declared stimulus plan\n");
      gCode |= 2;
    }
    std::printf("PRB-DEGEN-PLAN plan_rev=%d cells=%zu\n", kPlanRev, plan.size());
  }
  std::vector<std::string> emittedIds;

  std::vector<std::string> rows;
  // The arm's collector columns, in the arm's order, so this table is readable by the same
  // name-keyed reader; then this driver's own evidence columns.
  rows.push_back("id\tpath\tsignal\tsr_hz\tf_target_hz\tside\tduty_param\tmorph\tpw_depth"
                 "\tlfo_wave\tf0_meas_hz\tphi0_model\tpeak\tchannel\tcable\twarm\twin"
                 "\traw\tsamples\tsrc"
                 "\tduty\tcompanion_raw\thook_diff\tcompanion_diff\tsrc_span_v\tduty_span");
  int scnIdx = 0;

  const std::size_t total = kWarm + kWin;

  for (const CellSpec& cs : plan) {
    emittedIds.push_back(cs.id);
    const DeviceStateV1 stCabled = withPwmCable(degenState(cs.ft, cs.pwDepth));

    // (1) the cell itself: cable as a state fact, traced.
    Cap c = renderCell(stCabled, cs.sr, total, CableMode::kState, true);
    // (2) TRACE-HOOK INVARIANCE: the same state with no hook must render the same audio stream.
    Cap untraced = renderCell(stCabled, cs.sr, total, CableMode::kState, false);
    // (3) COMPANION: the same stimulus with the cable made by connect() after the load instead.
    Cap companion = renderCell(degenState(cs.ft, cs.pwDepth), cs.sr, total, CableMode::kConnect, true);

    double hookDiff = -1.0, companionDiff = -1.0, srcDiff = -1.0;
    if (c.ok && untraced.ok) hookDiff = maxAbsDiff(c.x, untraced.x);
    if (c.ok && companion.ok) {
      companionDiff = maxAbsDiff(c.x, companion.x);
      srcDiff = maxAbsDiff(c.src, companion.src);
    }
    const double srcSpan = spanOf(c.src);
    const double dutySpan = spanOf(c.duty);

    std::printf("PRB-DEGEN %-46s duty=%.17g peak=%.6f src_span=%.6f duty_span=%.17g "
                "hook_diff=%.6g companion_diff=%.6g src_diff=%.6g\n",
                cs.id.c_str(), c.ok ? dutySpan : -1.0, c.peak, srcSpan, dutySpan, hookDiff,
                companionDiff, srcDiff);

    // FAIL LOUDLY ON EACH CONTROL, with its own bit: these are two different failure surfaces and
    // folding them into one would leave a reader unable to tell which one broke.
    if (c.ok && !(hookDiff == 0.0)) {
      std::fprintf(stderr, "FATAL %s: trace-hook invariance guard failed (hook_diff=%.6g)\n",
                   cs.id.c_str(), hookDiff);
      gCode |= 4;
    }
    if (c.ok && companion.ok && !(companionDiff == 0.0 && srcDiff == 0.0)) {
      std::fprintf(stderr, "FATAL %s: companion identity failed (audio=%.6g cv_trace=%.6g) -- the "
                           "state-fact and connect() routes are not one signal\n",
                   cs.id.c_str(), companionDiff, srcDiff);
      gCode |= 16;
    }
    // THE DECLARATION GUARDS -- TWO SEPARATE SURFACES, DELIBERATELY NOT FOLDED INTO ONE BIT.
    //
    // These two checks read two DIFFERENT quantities, and the difference is the whole point:
    //   * `srcSpan` is the STIMULUS: the traced LFO-A CV-OUT record, i.e. what was actually fed into
    //     the route. It is an input to the product, so a wrong value means a different stimulus was
    //     rendered than the one this cell is filed under.
    //   * `dutySpan` is the PREMISE: the traced `effectiveDuty()` record, i.e. what the product DID
    //     with that input. It is derived by the code under test, so a wrong value means the product
    //     did not behave as the cell's name declares.
    // Both are fatal -- a cell whose declaration is false cannot support any conclusion -- but they
    // are reported under DIFFERENT NAMES AND DIFFERENT EXIT BITS, because a reader must be able to
    // tell "the stimulus was not what we declared" from "the product did not do what we declared"
    // WITHOUT reading prose. That distinction is what makes the degen-dispatch control arm's result
    // classifiable: it fires the PREMISE bit, so it demonstrates the premise check is live and
    // nothing whatsoever about the identity criterion, which is never reached.
    // (Both spans are also REPORTED, in this line and in the scenario table's `src_span_v`/
    // `duty_span` columns, so the failure can be quoted as a number rather than as a verdict.)
    //
    // `expectConst` is a property of the NAME: a const-duty cell whose duty moved, or a moving-duty
    // cell whose duty did not, means the table is lying about what was rendered.
    if (c.ok) {
      const bool expectConst = (cs.pwDepth == kConstDepth);
      const bool dutyHeld = (dutySpan == 0.0);
      bool badStimulus = false, badPremise = false;
      // --- STIMULUS (bit 5): the input. ----------------------------------------------
      if (expectConst && !(srcSpan > 0.0)) {
        std::fprintf(stderr, "FATAL-STIMULUS %s: declared constant duty from a MOVING source, but the "
                             "source did not move (src_span=%.6g) -- the degeneracy would be produced "
                             "by a dead stimulus, not by the depth\n", cs.id.c_str(), srcSpan);
        badStimulus = true;
      }
      if (!expectConst && !(srcSpan > 0.0)) {
        std::fprintf(stderr, "FATAL-STIMULUS %s: declared MOVING duty but the stimulus did not move "
                             "(src_span=%.6g) -- a dead source cannot produce a moving duty\n",
                     cs.id.c_str(), srcSpan);
        badStimulus = true;
      }
      // --- PREMISE (bit 6): what the product did with it. ----------------------------
      if (expectConst && !dutyHeld) {
        std::fprintf(stderr, "FATAL-PREMISE %s: declared CONSTANT duty but the product's effective "
                             "duty moved by %.17g (src_span=%.6g, the stimulus did move) -- the cell "
                             "has no constant-duty route to judge\n", cs.id.c_str(), dutySpan, srcSpan);
        badPremise = true;
      }
      if (!expectConst && !(dutySpan > kMovingDutySpanMin)) {
        std::fprintf(stderr, "FATAL-PREMISE %s: declared MOVING duty but the product's effective duty "
                             "moved by only %.17g (floor %.6g) -- the non-vacuity control would "
                             "compare two constants\n", cs.id.c_str(), dutySpan, kMovingDutySpanMin);
        badPremise = true;
      }
      if (badStimulus) gCode |= 32;
      if (badPremise) gCode |= 64;
    }

    std::string raw = "";
    std::string cmp = "";
    std::string src = "-";
    std::string duty = "-";
    if (c.ok) {
      raw = "gh19_prb_degen_scn" + std::to_string(++scnIdx) + ".raw";
      if (!writeRaw(out + "/" + raw, c.x)) {
        std::fprintf(stderr, "FATAL %s: raw write failed (%s)\n", cs.id.c_str(), raw.c_str());
        gCode |= 2;
      }
      if (!writeRaw(out + "/gh19_prb_degen_cmp" + std::to_string(scnIdx) + ".raw", companion.x)) {
        std::fprintf(stderr, "FATAL %s: companion raw write failed\n", cs.id.c_str());
        gCode |= 2;
      } else {
        cmp = "gh19_prb_degen_cmp" + std::to_string(scnIdx) + ".raw";
      }
      if (!c.src.empty()) {
        src = "gh19_prb_degen_src" + std::to_string(scnIdx) + ".tsv";
        std::vector<std::string> l;
        l.reserve(c.src.size() + 1);
        l.push_back("frame\tlfo_a_cv_out_volts");
        for (std::size_t i = 0; i < c.src.size(); ++i) {
          l.push_back(std::to_string(i) + "\t" + fmtExact(c.src[i]));
        }
        if (!writeTsv(out + "/" + src, l)) {
          std::fprintf(stderr, "FATAL %s: trace write failed (%s)\n", cs.id.c_str(), src.c_str());
          gCode |= 2;
        }
      }
      if (!c.duty.empty()) {
        // The degeneracy's own witness: this column shows the duty the PRODUCT computed, per frame,
        // so "the duty never moved" is a measurement rather than a restatement of the depth knob.
        duty = "gh19_prb_degen_duty" + std::to_string(scnIdx) + ".tsv";
        std::vector<std::string> l;
        l.reserve(c.duty.size() + 1);
        l.push_back("frame\teffective_duty");
        for (std::size_t i = 0; i < c.duty.size(); ++i) {
          l.push_back(std::to_string(i) + "\t" + fmtExact(c.duty[i]));
        }
        if (!writeTsv(out + "/" + duty, l)) {
          std::fprintf(stderr, "FATAL %s: duty write failed (%s)\n", cs.id.c_str(), duty.c_str());
          gCode |= 2;
        }
      }
    } else {
      std::fprintf(stderr, "FATAL %s: cell not produced [%s]\n", cs.id.c_str(), c.signal.c_str());
      gCode |= 1;
    }

    // The MODEL's starting phase, stated so the n+1 convention is auditable from the manifest
    // (identical formula to the arm's probe :583).
    const double phi0 = 360.0 * ((cs.ft / cs.sr) - std::floor(cs.ft / cs.sr));
    rows.push_back(cs.id + "\t" + (cs.pwDepth == kConstDepth ? "dynroute_const_duty"
                                                             : "dynroute_moving_duty") +
                   "\t" + safeTsv(c.signal) +
                   "\t" + std::to_string(cs.sr) +
                   "\t" + std::to_string(cs.ft) +
                   "\t" + "a" +
                   "\t" + fmtExact(kBaseWidth) +
                   "\t" + fmtExact(1.0) +
                   "\t" + fmtExact(cs.pwDepth) +
                   "\t" + "tri" +
                   "\t" + fmtExact(c.f0) +
                   "\t" + fmtExact(phi0) +
                   "\t" + fmtExact(c.peak) +
                   "\t" + "dry_a" +
                   "\t" + "state:vco_a_pwm_in<-lfo_a_cv_out" +
                   "\t" + std::to_string(kWarm) +
                   "\t" + std::to_string(kWin) +
                   "\t" + raw +
                   "\t" + std::to_string(c.x.size()) +
                   "\t" + src +
                   "\t" + duty +
                   "\t" + cmp +
                   "\t" + fmtExact(hookDiff) +
                   "\t" + fmtExact(companionDiff) +
                   "\t" + fmtExact(srcSpan) +
                   "\t" + fmtExact(dutySpan));
  }

  const std::string tsv = out + "/gh19_prb_degen_scenarios.tsv";
  if (!writeTsv(tsv, rows)) {
    std::fprintf(stderr, "FATAL: could not write %s\n", tsv.c_str());
    gCode |= 2;
  }

  // ---- PRODUCED SET vs DECLARED PLAN (bit 3) ----
  // One direction only: what was produced is checked against the plan written before the first
  // render. Nothing here can add a cell to the plan, so a cell that quietly stops being produced is a
  // failure rather than a smaller green matrix. Duplicates are flagged separately because set
  // arithmetic cannot see them.
  {
    std::map<std::string, std::set<std::string>> declared, seen;
    std::map<std::string, std::string> idBlock;
    std::vector<std::string> blockOrder;
    std::set<std::string> planDup;
    for (const CellSpec& e : plan) {
      if (declared.find(e.block) == declared.end()) blockOrder.push_back(e.block);
      declared[e.block].insert(e.id);
      if (!idBlock.insert({e.id, e.block}).second) planDup.insert(e.id);
    }
    std::set<std::string> orphans, dup, once;
    for (const std::string& id : emittedIds) {
      if (!once.insert(id).second) dup.insert(id);
      const auto it = idBlock.find(id);
      if (it == idBlock.end()) { orphans.insert(id); continue; }
      seen[it->second].insert(id);
    }
    bool bad = !orphans.empty() || !dup.empty() || !planDup.empty();
    for (const std::string& pd : planDup) std::fprintf(stderr, "PLAN-INTERNAL-DUPLICATE %s\n", pd.c_str());
    std::size_t totalMissing = 0, totalExtra = 0;
    for (const std::string& b : blockOrder) {
      std::vector<std::string> missing, extra;
      for (const std::string& id : declared[b]) if (!seen[b].count(id)) missing.push_back(id);
      for (const std::string& id : seen[b]) if (!declared[b].count(id)) extra.push_back(id);
      std::fprintf(stderr, "PLAN %s declared=%zu emitted=%zu missing=%zu extra=%zu\n", b.c_str(),
                   declared[b].size(), seen[b].size(), missing.size(), extra.size());
      for (const std::string& m : missing) std::fprintf(stderr, "PLAN-MISSING %s %s\n", b.c_str(), m.c_str());
      for (const std::string& x : extra) std::fprintf(stderr, "PLAN-EXTRA %s %s\n", b.c_str(), x.c_str());
      totalMissing += missing.size();
      totalExtra += extra.size();
      if (!missing.empty() || !extra.empty()) bad = true;
    }
    for (const std::string& o : orphans) std::fprintf(stderr, "PLAN-ORPHAN %s\n", o.c_str());
    for (const std::string& d2 : dup) std::fprintf(stderr, "PLAN-DUPLICATE %s\n", d2.c_str());
    // The authoritative reconciliation line (same shape as the arm probe's PLAN-TOTAL): the per-block
    // lines above are the detail, this is the one a reader or a log-grepper takes the verdict from.
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
  // PRB-DEGEN line and this summary race for the same byte range and splice together in a captured
  // log -- which silently truncates the guard value a reader needs.
  std::fflush(stdout);
  std::fprintf(stderr, "gh19_prb_degen_probe: %zu cells produced, %zu failed, exit_code=%d.\n",
               produced, failed, gCode);
  return gCode;
}
