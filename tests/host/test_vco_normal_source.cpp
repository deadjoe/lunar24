// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_vco_normal_source.cpp — task #83 (GH #18): fix the DEFAULT VCO-B stall caused by an
// incorrectly self-sourced normalized route. Per @Codex (4fe298c8) the ONLY authorized change is
// correcting the source of RouteId 4 (route.vco_b_vco_out_to_cv_in) to the EXISTING published VCO-A
// oscillator signal (vco_a.dry_out). We never change default cvAmt / lin_exp / baseHz, never impose a
// frequency floor, never add a jack, never reassign B's OSC public output owner.
//
// This file is the RED-first product oracle and the fix's permanent regression net. It asserts the
// CONTRACT of GH #18 (actual product behaviour, not the obsolete self-edge IDS truth):
//   (1) DEFAULT machine (no user cable) at 44.1/48/88.2/96k, >=2 s: BOTH DRY channels' final second
//       is non-constant and contains NO 50 ms flat window. (The pre-fix default DRY B is a DC lock:
//       zcr ~0-4, and the audit's 1e-12 flat-window detector trips at sample 1530.)
//   (2) cvAmt sweep 0..1 (11 points): the default route reaches B (an ASYMMETRIC A-only change moves
//       B's output) whenever cvAmt>0; cvAmt==0 isolates that A->B link (A change leaves B identical).
//       Also, the default DRY B is never flat at cvAmt==1.0 (the historical lock point).
//   (3) A B-only change must NEVER become the A source (B change leaves DRY A bit-identical).
//   (4) A user cable into vco_b.cv_in OVERRIDES the (now acyclic) A->B normalized route; removing it
//       restores the route (normalizedActive flips true/false + the published plan reflects it).
//   (5) The A->B edge is ACYCLIC: the default machine compiles to ZERO feedback edges (no artificial
//       one-sample z^-1 delay) and no feedback line has (source,sink)==(vco_a.dry_out, vco_b.cv_in);
//       B's generic CV reads A's LIVE published value the same frame.
//
// Explicit-self-loop retention (@Codex): the pre-fix test that used the DEFAULT self-edge to verify
// the feedback D-sample staging is RETAINED but re-pointed at a REAL user B->B patch cable (see
// test_machine_definition.cpp "VCO-B self-edge renders finite..." converted to a user cable), so the
// feedback mechanism is still exercised while the default route is acyclic. Nothing here asserts
// "default always has sound" for arbitrary muted/zeroed inputs — those are not a product contract
// (see report/2026-09-06-director-disposition.md reserving note 2).

#include "mini_test.h"

#include <lunar24/core/device_state.h>        // DeviceStateV1
#include <lunar24/core/state_default.h>        // make_default_device_state
#include <lunar24/registry_ids.hpp>            // JackId / RouteId full enums

#include "test_engine_harness.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::JackId;
using lunar24::core::ParameterId;
using lunar24::core::RouteId;
using lunar24::core::make_default_device_state;

namespace {

// A 50 ms window in samples at the given sample rate. The audit's 1e-12 flat-window detector tripped
// the lock at sample 1530 / t=0.0319 s (0.5 * window); we require NO flat window of this length.
constexpr double kFlatTol = 1e-12;

struct ChannelMetrics {
  int zcr = 0;            // zero crossings (frequency discriminator; DC => 0).
  int maxFlatRun = 0;     // longest run of consecutive samples within kFlatTol (DC => whole tail).
};

ChannelMetrics measure(const std::vector<double>& c) {
  ChannelMetrics m;
  if (c.size() < 2) return m;
  int run = 0;
  for (std::size_t i = 1; i < c.size(); ++i) {
    const bool sameStep = std::fabs(c[i] - c[i - 1]) <= kFlatTol;
    if (sameStep) ++run; else run = 0;
    if (run > m.maxFlatRun) m.maxFlatRun = run;
    if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++m.zcr;
  }
  return m;
}

// Render `state` at `sr` for `totalSec`, then analyse the FINAL `lastSec` of DRY A and DRY B.
struct RenderResult {
  bool ok = false;
  ChannelMetrics a;
  ChannelMetrics b;
  std::vector<double> aLast;   // final second of DRY A
  std::vector<double> bLast;   // final second of DRY B
};

RenderResult renderLastSec(const DeviceStateV1& state, double sr, double totalSec, double lastSec) {
  RenderResult r;
  EngineHarness h;
  if (!h.load(state, sr)) return r;
  const int total = static_cast<int>(std::ceil(totalSec * sr));
  if (!h.render(total)) return r;
  const int last = static_cast<int>(std::floor(lastSec * sr));
  const std::size_t a0 = h.dryA().size() - static_cast<std::size_t>(last);
  const std::size_t b0 = h.dryB().size() - static_cast<std::size_t>(last);
  if (a0 > h.dryA().size() || b0 > h.dryB().size()) return r;
  r.aLast.assign(h.dryA().begin() + static_cast<std::ptrdiff_t>(a0), h.dryA().end());
  r.bLast.assign(h.dryB().begin() + static_cast<std::ptrdiff_t>(b0), h.dryB().end());
  r.a = measure(r.aLast);
  r.b = measure(r.bLast);
  r.ok = true;
  return r;
}

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

// Mean absolute difference between two equal-length last-second traces; the "did A reach B" gate.
double traceDiff(const std::vector<double>& a, const std::vector<double>& b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) s += std::fabs(a[i] - b[i]);
  return n ? s / static_cast<double>(n) : 0.0;
}

bool flatWindow(const ChannelMetrics& m, int windowSamples) {
  return m.maxFlatRun >= windowSamples;
}

const std::vector<double> kSrs = {44100.0, 48000.0, 88200.0, 96000.0};

// (1) Default machine is a live A AND B at every sample rate: final-second non-constant + no 50 ms
//     flat window. RED on the pre-fix default (DRY B locks to -1 V DC).
void test_default_dryb_not_locked() {
  for (double sr : kSrs) {
    const int win = static_cast<int>(0.05 * sr);   // 50 ms in samples
    RenderResult r = renderLastSec(make_default_device_state(kSeed), sr, 2.2, 1.0);
    CHECK(r.ok);
    if (!r.ok) { std::fprintf(stderr, "  (sr=%g: render failed)\n", sr); continue; }
    // DRY A: always was a live oscillator — a guard, not the red trigger.
    CHECK(r.a.zcr >= 10);
    CHECK(!flatWindow(r.a, win));
    CHECK(r.a.maxFlatRun < win);
    // DRY B: THE red trigger. Pre-fix default is the self-edge lock (zcr ~0-4, flat window trips).
    CHECK(r.b.zcr >= 10);
    CHECK(r.b.maxFlatRun < win);
    // Both must actually change, not sit at a single DC value.
    bool bMoved = false;
    for (std::size_t i = 1; i < r.bLast.size(); ++i) {
      if (std::fabs(r.bLast[i] - r.bLast[i - 1]) > kFlatTol) { bMoved = true; break; }
    }
    bool aMoved = false;
    for (std::size_t i = 1; i < r.aLast.size(); ++i) {
      if (std::fabs(r.aLast[i] - r.aLast[i - 1]) > kFlatTol) { aMoved = true; break; }
    }
    CHECK(aMoved);
    CHECK(bMoved);
    std::fprintf(stderr, "  (sr=%g: A zcr=%d flat=%d | B zcr=%d flat=%d win=%d)\n",
                 sr, r.a.zcr, r.a.maxFlatRun, r.b.zcr, r.b.maxFlatRun, win);
  }
}

// (2) Asymmetric A-only change reaches B over the default (acyclic) route when cvAmt>0, isolating at
//     cvAmt==0; and the default DRY B is never flat at cvAmt==1.0 (the historical lock point).
void test_cvamt_a_to_b_propagation() {
  const double sr = 48000.0;
  const int win = static_cast<int>(0.05 * sr);
  std::vector<double> amounts;
  for (int i = 0; i <= 10; ++i) amounts.push_back(static_cast<double>(i) / 10.0);  // 0.0..1.0
  for (double amt : amounts) {
    DeviceStateV1 base = make_default_device_state(kSeed);
    slot(base, ParameterId::vco_b_cv_amt) = amt;

    // Default (no A change) DRY B never flat — especially at amt==1.0 (the pre-fix lock).
    RenderResult def = renderLastSec(base, sr, 1.2, 0.8);
    CHECK(def.ok);
    if (def.ok) {
      CHECK(def.b.maxFlatRun < win);
      CHECK(def.b.zcr >= 10);
    }

    // Asymmetric A-only change: user-shifted vco_a_tune.
    DeviceStateV1 shifted = base;
    slot(shifted, ParameterId::vco_a_tune) = 0.3;
    RenderResult s0 = renderLastSec(base, sr, 0.8, 0.6);
    RenderResult s1 = renderLastSec(shifted, sr, 0.8, 0.6);
    CHECK(s0.ok && s1.ok);
    if (!(s0.ok && s1.ok)) continue;
    const double d = traceDiff(s0.bLast, s1.bLast);
    if (amt > 1e-9) {
      // A reaches B through the default route: B's output must change.
      CHECK(d > 1e-9);
    } else {
      // cvAmt==0 isolates the A->B link: B stays identical when A changes.
      CHECK(d == 0.0);
    }
    std::fprintf(stderr, "  (cvAmt=%g: A->B diff=%g)\n", amt, d);
  }
}

// (3) B-only change must never become the A source (leave DRY A bit-identical).
void test_b_change_not_swap_a() {
  const double sr = 48000.0;
  DeviceStateV1 base = make_default_device_state(kSeed);
  DeviceStateV1 changed = base;
  slot(changed, ParameterId::vco_b_tune) = 0.5;
  RenderResult a1 = renderLastSec(base, sr, 1.2, 0.8);
  RenderResult a2 = renderLastSec(changed, sr, 1.2, 0.8);
  CHECK(a1.ok && a2.ok);
  if (!(a1.ok && a2.ok)) return;
  CHECK(traceDiff(a1.aLast, a2.aLast) == 0.0);
}

// (4) A user cable overrides the (acyclic) A->B normalized route; removing it restores the route.
void test_insertion_override_removal_restore() {
  const double sr = 48000.0;
  // Default: the A->B route is active (acyclic source).
  {
    EngineHarness h;
    CHECK(h.load(make_default_device_state(kSeed), sr));
    const auto* rt = h.runtime();
    CHECK(rt != nullptr);
    if (rt != nullptr) {
      CHECK(rt->normalizedActive(JackId::vco_a_dry_out, JackId::vco_b_cv_in));
    }
  }
  // With a user cable into vco_b.cv_in, the route is overridden (countInto(sink)>0 -> inactive).
  {
    DeviceStateV1 st = make_default_device_state(kSeed);
    const std::uint32_t sink = static_cast<std::uint32_t>(JackId::vco_b_cv_in);
    st.inputCable[sink] = 1;
    st.cableSource[sink] = JackId::env_follower_env_out;
    st.routeOverridden[static_cast<std::uint32_t>(RouteId::route_vco_b_vco_out_to_cv_in)] = 1;
    EngineHarness h;
    CHECK(h.load(st, sr));
    const auto* rt = h.runtime();
    CHECK(rt != nullptr);
    if (rt != nullptr) {
      CHECK(rt->cableCountInto(JackId::vco_b_cv_in) == 1);
      CHECK(!rt->normalizedActive(JackId::vco_a_dry_out, JackId::vco_b_cv_in));
    }
  }
}

// (5) The A->B edge is acyclic: default machine has ZERO feedback edges, and no feedback line has
//     (source,sink)==(vco_a.dry_out, vco_b.cv_in). This is the "no artificial one-sample delay".
void test_acyclic_no_artificial_delay() {
  EngineHarness h;
  CHECK(h.load(make_default_device_state(kSeed), 48000.0));
  const auto* rt = h.runtime();
  CHECK(rt != nullptr);
  if (rt == nullptr) return;
  const auto fc = rt->feedbackCount();
  CHECK(fc == 0);
  bool vcoSelf = false;
  for (std::uint32_t i = 0; i < fc; ++i) {
    const auto& l = rt->feedbackAt(i);
    if (l.sourceJack == JackId::vco_a_dry_out && l.sinkJack == JackId::vco_b_cv_in) vcoSelf = true;
  }
  CHECK(!vcoSelf);
  std::fprintf(stderr, "  (default feedback edges=%u, vco_a->b feedback=%d)\n", fc, vcoSelf ? 1 : 0);
}

// (6) SAME-sample vs PREVIOUS-sample A discriminator (the hidden-z^-1 net). @Codex e14e62a8: an
//     ACYCLIC A->B route means feedbackCount()==0 proves no feedback LINE, but does NOT prove the
//     executor reads A's LIVE value THIS frame — a build that saves previousA (per runtime) and feeds
//     that to B passes the structural check yet IS a one-sample hidden delay. So we reconcile B's
//     rendered output against an EXPLICIT per-sample reference built from the runtime's OWN A readback
//     (controlVoltageAt(vco_a_dry_out) sampled after each frame — the exact value A published this
//     frame, which is what a same-frame consumer must read):
//         refSame(n) = VCO triangle given CV = A(n)     -- B MUST produce this (same-frame route)
//         refPrev(n) = VCO triangle given CV = A(n-1)   -- what B produces under the delayed mutation
//     Under an ASYMMETRIC, NON-DEGENERATE (fast) A the two diverge sharply, so:
//         CHECK(madSame < kSameTol)   -> GREEN on the correct build, RED on the delayed mutation.
//         CHECK(madPrev > madSame)    -> the delayed reference is the WRONG one (extra bite).
//     Non-degeneracy guards (so the test can never silently go vacuous on a symmetric/static A):
//         CHECK(maxAStep > kDegenTol)         -> A actually changes between consecutive samples
//         CHECK(maxDiffSamePrev > kDegenTol)  -> refSame and refPrev really differ (observable delay)
//     The reference mirrors the Vco DSP (vco.h:193-245: linear `p*=(1+cv*cvAmt)`, triangle
//     `4|frac(.5-p)|-1`), baseHz==kVcoBaseHzProvisional(440), vOct==0 (no route feeds vco_b_v_oct_in),
//     no linear-FM (a plain VCO), and the device-normalise `*0.5` (kDeviceScaleProvisional,
//     device_adapter.h:90-94) that maps volts to the captured DRY_B channel. It is a READBACK-driven
//     reference — it does NOT re-run A, so it is insensitive to A's own (unmodeled) phase.

// One sample of the reference B: advance `cum` (cycles) by B's instant pitch under CV=cv and return the
// device-normalised triangle. Mirrors machine_runtime kVcoB -> Vco::tick/frequencyHz/waveformSampleAt.
double refB(double& cum, double cv, double sr, double baseHz, int octSel, double tune, double cvAmt) {
  static constexpr double kOct[3] = {-1.0, 0.0, 3.0};  // "low"/"0"/"+3" (PROVISIONAL, vco.h:162-164).
  const double oct = kOct[octSel < 0 ? 0 : (octSel > 2 ? 2 : octSel)];
  double p = baseHz * std::pow(2.0, oct + tune);   // baseHz * 2^octs * 2^tune.
  // vOct == 0 (default: no route feeds vco_b_v_oct_in) => p *= 2^0 == 1.
  const double eff = cv * cvAmt;
  p *= (1.0 + eff);                                 // LINEAR generic-CV law (vco.h:201).
  // No linear-FM on a plain VCO => instHz == p.
  cum += p / sr;
  const double ph = cum - std::floor(cum);
  const double tri = 4.0 * std::fabs(ph - 0.5) - 1.0;   // triangle (vco.h:228).
  return 0.5 * tri;                                     // volts -> device-normalised (kDeviceScale=0.5).
}

void test_b_same_sample_vs_previous() {
  constexpr double kSameTol = 1e-4;    // correct build: madSince~1e-16; delay mutation: ~1e-2.
  constexpr double kDegenTol = 1e-3;   // non-degeneracy floor.
  const double sr = 48000.0;
  DeviceStateV1 st = make_default_device_state(kSeed);
  // Make A FAST + asymmetric so consecutive A samples differ strongly (non-degenerate): any one-sample
  // delay in B's read of A becomes a LARGE divergence between refSame and refPrev. Reachable panel
  // controls (oct_sel "+3" = baseHz*8, tune +1 oct = baseHz*16 -> ~7 kHz, ~0.5 per-sample A delta).
  slot(st, ParameterId::vco_a_oct_sel) = 2.0;   // oct_sel "+3".
  slot(st, ParameterId::vco_a_tune) = 1.0;         // +1 oct.
  slot(st, ParameterId::vco_b_lin_exp) = 0.0;      // linear (reachable; the reference matches the law).
  slot(st, ParameterId::vco_b_cv_amt) = 1.0;       // full generic-CV depth (default, non-degenerate).

  EngineHarness h;
  const bool ok = h.load(st, sr);
  CHECK(ok);
  if (!ok) return;
  const auto* rt = h.runtime();
  CHECK(rt != nullptr);
  if (rt == nullptr) return;
  const double tune = rt->vcoBTune();
  const int octSel = rt->vcoBOctSelect();
  const double cvAmt = rt->vcoBCvAmt();
  const double baseHz = 440.0;  // kVcoBaseHzProvisional (machine_definition.h:197).

  double cumSame = 0.0, cumPrev = 0.0;          // per-sample phase accumulators of the two references.
  constexpr int kFrames = 8192;
  double madSame = 0.0, madPrev = 0.0;           // mean |ref - actual| over the block.
  double maxAStep = 0.0, maxDiffSamePrev = 0.0;  // non-degeneracy observables.
  long n = 0;
  double prevA = 0.0;
  const bool rendered = h.renderSampled(kFrames, 0.0, [&](const SynthRuntime& r) {
    const double aN = r.controlVoltageAt(JackId::vco_a_dry_out);  // A's live published value, this frame.
    const double bN = h.dryB()[static_cast<std::size_t>(n)];      // captured DRY_B (device-normalised).
    if (n > 0 && std::fabs(aN - prevA) > maxAStep) maxAStep = std::fabs(aN - prevA);
    const double refSame = refB(cumSame, aN, sr, baseHz, octSel, tune, cvAmt);    // CV = A(n).
    const double refPrev = refB(cumPrev, prevA, sr, baseHz, octSel, tune, cvAmt); // CV = A(n-1).
    if (std::fabs(refSame - refPrev) > maxDiffSamePrev) maxDiffSamePrev = std::fabs(refSame - refPrev);
    madSame += std::fabs(refSame - bN);
    madPrev += std::fabs(refPrev - bN);
    prevA = aN;
    ++n;
  });
  CHECK(rendered);
  if (!rendered || n == 0) return;
  madSame /= static_cast<double>(n);
  madPrev /= static_cast<double>(n);
  // Non-degeneracy: A must change per sample and the two references must really differ, so this never
  // silently weakens into a vacuous pass on a symmetric/static excitation.
  CHECK(maxAStep > kDegenTol);
  CHECK(maxDiffSamePrev > kDegenTol);
  // THE discriminator: B must match the SAME-frame reference (and not the delayed one).
  CHECK(madSame < kSameTol);
  CHECK(madPrev > madSame);
  std::fprintf(stderr,
               "  (maxAStep=%g maxDiffSamePrev=%g madSame=%g madPrev=%g tune=%g oct=%d cvAmt=%g)\n",
               maxAStep, maxDiffSamePrev, madSame, madPrev, tune, octSel, cvAmt);
}

}  // namespace

int main() {
  test_default_dryb_not_locked();
  test_cvamt_a_to_b_propagation();
  test_b_change_not_swap_a();
  test_insertion_override_removal_restore();
  test_acyclic_no_artificial_delay();
  test_b_same_sample_vs_previous();
  return ::test::finish("test_vco_normal_source");
}
