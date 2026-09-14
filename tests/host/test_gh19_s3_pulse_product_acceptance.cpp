// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// test_gh19_s3_pulse_product_acceptance.cpp — task #119 (GH #19 S3): the ACCEPTANCE surface for the
// pulse TWO-EDGE correction, at the PRODUCT entry.
//
// Everything here runs through the ONE agreed entry (test_engine_harness.h):
//     encode_device_state -> decode_device_state -> StandaloneAudioEngine::applyDeviceState ->
//     StandaloneAudioEngine::processBlock,
// and asserts on the REAL captured output channels (WET_L/WET_R/DRY_A/DRY_B) and on the read-only
// published runtime. No test here calls Vco / buildMachineRuntimeCandidate / processFrame directly,
// and no test here slices a Vco render loop into blocks: task #118's `VCO-BLOCK` evidence is
// Vco-level, and a Vco-level partition claim cannot stand in for a product-block one (#119 order).
//
// WHAT IS BEING ACCEPTED. The production delta is ONE change (core/include/lunar24/core/
// pulse_blep_kernel.h): the pulse node's correction is
//     correction(t, duty, dt) = R(t, w) - R(frac(t - duty), w),   w = min(dt, kPolyblepMaxDt)
// added into the existing continuous mix at the pulse node's EXISTING weight, with the phase
// accumulator still advancing by the TRUE step. So the claims a product test can make are:
//   (a) the DRY outputs really carry that formula — sample by sample, not "a pulse came out";
//   (b) the same formula in its NEIGHBOURING WRONG FORMS does NOT describe the output (the
//       correction is really there, with the right sign, the right edge, and the bounded width);
//   (c) the driven step is exactly the step the output is a function of (phase advance unchanged);
//   (d) the block partition and the PWM/sync/cable plumbing are untouched;
//   (e) nothing on this path allocates, and every output stays finite and inside the declared range.
//
// HOW (a) AND (b) ARE MEASURED, AND WHY THE REFERENCE IS WRITTEN OUT LONGHAND. There is no phase
// getter on the product's published runtime, so the emitted sample sequence is compared against a
// reference MODEL of it: `refPulse(frac(phi + frame*step), duty, step, variant)`, with the step
// DERIVED from the driven inputs (the V/OCT volts the runtime itself publishes) and the one
// remaining unknown — the capture's constant phase offset `phi`, which is equally the whole-sample
// delay the audio pipeline introduces — recovered by a grid search ONCE PER CELL. Every variant is
// then judged at that ONE alignment, so no variant can buy itself a better alignment than another.
// Recovering one common alignment and then asking which MODEL fits is not the forbidden "re-fit the
// phase until the residual looks good": no threshold moves, the step is never fitted, and the
// alignment is never re-optimised per variant.
//
// ⚠ TWO CLASSES OF ALIGNMENT EVIDENCE, AND THEY ARE NOT INTERCHANGEABLE (task #119 director note).
// A fitted phase is a FREE PARAMETER, and `frac(phi + i*step)` with `phi' = phi - step` is the same
// sequence: a capture whose WHOLE emission is one frame late — or one frame early — fits the model
// EXACTLY at a shifted phi, so nothing that searches over phi can ever reject it. Therefore:
//   * VARIANT ALIGNMENT AT ITS OWN BEST PHASE (`bestVariantWorst`) is DIAGNOSTICS. It is the
//     strongest form of "this variant cannot explain the capture", and it is what keeps a negative
//     control from failing open when the accepted fit's phi drifts on a mutated product. It proves
//     nothing about absolute timing, and is not asked to.
//   * THE ABSOLUTE ORIGIN ANCHOR (`theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance`) is a GATE.
//     The capture starts at the engine's frame 0, the product's per-frame loop advances the phase
//     and then publishes what it advanced to, so frame 0's phase is `frac(step)` — fixed by the
//     product's own frame convention, not searched. Judging the accepted model at THAT phase, with
//     no free parameter, is what makes a uniformly late (or early) emission fail: one frame is a
//     whole step of misalignment (5.0e-3 .. 1.8 in these cells) against a measured residual ~1e-8.
//   The fitted and pinned numbers, and their difference, are printed per cell by `origin[...]`.
//
// The reference is NOT lunar24::core::polyblepResidual and NOT polyblepPulseCorrection. Those are
// the code under test; a defect there would move the product AND the reference together and the
// check would compare the defect with itself. The residual, the sign rule, the edge argument and
// the width cap are all transcribed from the documented formula, and only the cap CONSTANT is
// taken from the header (via a static_assert, never a call).
//
// MEASURED BASIS for the two tolerances. Neither is a table lookup: both are printed per cell by
// this test, so the separation is read off the run rather than trusted.
//   * kFitTol = 1e-5 device units — an order-of-magnitude ceiling, not a fitted value. A healthy
//     fit's residual is dominated by the product's own phase accumulation (`cumPitch_ += step`),
//     which no model reproduces bit for bit without repeating that addition order.
//   * kFitMargin = 1e-2 device units — every variant that MUST be rejected misses by an appreciable
//     fraction of the swing, never by a hair: the naive shape misses by up to the full half-swing,
//     and the wrong-edge / wrong-sign / uncapped forms displace whole windows.

#include "mini_test.h"

#include <lunar24/core/device_adapter.h>    // kDeviceScaleProvisional / device_normalized_from_volts
#include <lunar24/core/device_state.h>      // DeviceStateV1
#include <lunar24/core/machine_definition.h>  // kVcoBaseHzProvisional (the product's f0)
#include <lunar24/core/state_default.h>       // make_default_device_state
#include <lunar24/core/state_disposition.h>   // find_parameter / ParameterDescriptor
#include <lunar24/core/state_serializer.h>    // encode/decode_device_state
#include <lunar24/core/state_validation.h>    // find_jack (the jack's OWN declared gate threshold/hysteresis)
#include <lunar24/registry.hpp>               // kNormalizedRoutes (the patch-override rule)
#include <lunar24/core/vco.h>               // Vco::kLowOctave / kZeroOctave / kPlus3Octave ONLY
#include <lunar24/registry_ids.hpp>         // ParameterId / JackId full enums

#include "test_engine_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

// The allocator counters live in the SHARED allocator TU (test_state_apply_oracle_allocator.cpp,
// built into this target alongside this file — see CMakeLists.txt). Reused rather than copied so
// the project keeps ONE replacement pair to audit, and defined in a different TU from this file's
// ::operator new call sites (GCC's -Wmismatched-new-delete is an error in this build).
extern std::size_t g_allocCount;
extern std::size_t g_freeCount;

using namespace lunar24::testengine;
using lunar24::core::DeviceStateV1;
using lunar24::core::JackId;
using lunar24::core::ParameterId;
using lunar24::host::StandaloneAudioEngine;
using lunar24::core::make_default_device_state;

namespace {

// =================================================================================================
// The reference model. INDEPENDENT of the code under test — see the header note.
// =================================================================================================

// The header's cap constant is pinned here so a change to IT cannot silently leave this file
// modelling a different contract. `pulse_blep_kernel.h` is included for this assertion ONLY: no
// function from it is called anywhere in this file, deliberately.
static_assert(lunar24::core::kPolyblepMaxDt == 0.5,
              "the S3 reference below hard-codes the documented cap; update this file with it");

double refFrac(double v) { return v - std::floor(v); }

// polyblep_kernel.h's residual, transcribed from its documented formula (NOT called):
//   R(t, dt) = 2u - u^2 - 1, u = t/dt,        for t < dt
//            = u^2 + 2u + 1, u = (t-1)/dt,    for t > 1 - dt
//            = 0,                             otherwise
double refResidual(double t, double dt) {
  if (!(dt > 0.0)) return 0.0;
  if (t < dt) {
    const double u = t / dt;
    return u + u - u * u - 1.0;
  }
  if (t > 1.0 - dt) {
    const double u = (t - 1.0) / dt;
    return u * u + u + u + 1.0;
  }
  return 0.0;
}

// The variants the product output must match EXACTLY ONE of. Each is a one-line neighbour of the
// accepted formula, i.e. exactly the shape of the isolated source mutations the acceptance runner
// compiles against the SAME gate.
enum class RefVariant {
  kCapped,         // THE ACCEPTED FORMULA: R(t,w) - R(frac(t-duty),w), w = min(dt, 0.5)
  kNaive,          // no correction at all (the "bypass the correction" mutation)
  kUncapped,       // w = dt with no cap (the "remove the kernel width cap" mutation)
  kWrongSign,      // +R(frac(t-duty),w) instead of -R(...) (the "wrong sign" mutation)
  kWrongPosition,  // frac(t+duty) instead of frac(t-duty) (the "wrong edge position" mutation)
  kDutyGuarded,    // the WITHDRAWN duty hard switch: correction zeroed when windows overlap
};

double refPulse(double t, double duty, double dt, RefVariant v) {
  const double naive = (t < duty) ? 1.0 : -1.0;
  if (v == RefVariant::kNaive) return naive;
  if (v == RefVariant::kDutyGuarded) {
    // The withdrawn guard: `min(duty, 1-duty) >= 2*dt` was READ as a licence to switch the
    // correction off. At a narrow duty it zeroes the correction where the accepted formula keeps
    // it, which is the whole reason this variant is in the list.
    const double m = (duty < 1.0 - duty) ? duty : (1.0 - duty);
    if (!(dt > 0.0) || !(m >= 2.0 * dt)) return naive;
  }
  const double w = (v == RefVariant::kUncapped) ? dt : ((dt < 0.5) ? dt : 0.5);
  const double b = refFrac((v == RefVariant::kWrongPosition) ? (t + duty) : (t - duty));
  const double sign = (v == RefVariant::kWrongSign) ? 1.0 : -1.0;
  return naive + refResidual(t, w) + sign * refResidual(b, w);
}

// The product's own output choke point, called rather than re-implemented: what the test compares
// against is what the DeviceAdapter actually writes (device_adapter.h:101).
double refDeviceSample(double t, double duty, double dt, RefVariant v) {
  return lunar24::core::device_normalized_from_volts(refPulse(t, duty, dt, v));
}

// =================================================================================================
// The fit: one alignment per cell, then every variant judged at that same alignment.
// =================================================================================================
constexpr double kFitTol = 1e-5;     // "this model describes the output"
constexpr double kFitMargin = 1e-2;  // "this model does NOT describe the output"

// WHY THERE IS NO INTEGER-FRAME-OFFSET SEARCH. A whole-sample pipeline delay is mathematically the
// same thing as a phase offset: shifting the capture index by k moves the model's phase by k*step,
// which the continuous `phi` search below absorbs exactly. Searching an integer k as well would add
// a second parameter that cannot change any residual — it would only make `phi`'s recovered value
// harder to read. So the model is `frac(phi + frame*step)` with `frame` the plain capture index and
// `phi` the single alignment constant, recovered once per cell.

// Max |model - captured| over capture frames [first, last) for one variant at one phase offset.
double worstAt(const std::vector<double>& a, std::size_t first, std::size_t last, double step,
               double duty, RefVariant v, double phi) {
  double worst = 0.0;
  for (std::size_t i = first; i < last; ++i) {
    const double want = refDeviceSample(refFrac(phi + double(i) * step), duty, step, v);
    const double d = std::fabs(want - a[i]);
    if (d > worst) worst = d;
  }
  return worst;
}

// Three-stage grid refinement of phi on [0,1), then two halvings of the bracket. The final
// resolution is 1/(2048^3) ~ 1.2e-10 cycles, which the model's slope (at most 2/w per cycle)
// turns into a value error far below kFitTol even at the narrowest kernel in the cell set.
double fitPhi(const std::vector<double>& a, std::size_t first, std::size_t last, double step,
              double duty, RefVariant v, double* worstOut) {
  const int kGrid = 2048;
  double lo = 0.0, hi = 1.0;
  double best = 1e30, bestPhi = 0.0;
  for (int stage = 0; stage < 3; ++stage) {
    const double span = hi - lo;
    double sBest = 1e30, sPhi = lo;
    for (int j = 0; j < kGrid; ++j) {
      const double phi = lo + span * (double(j) / double(kGrid));
      const double w = worstAt(a, first, last, step, duty, v, phi);
      if (w < sBest) {
        sBest = w;
        sPhi = phi;
      }
    }
    best = sBest;
    bestPhi = sPhi;
    lo = sPhi - span / double(kGrid);
    hi = sPhi + span / double(kGrid);
  }
  if (worstOut != nullptr) *worstOut = best;
  return bestPhi;
}

// EVERY VARIANT IS ALIGNED ON ITS OWN BEST PHASE. This is a STRENGTHENING, not a convenience.
//
// The accepted formula's `phi` is the alignment the S3 column is measured at and the primary check
// uses it. The variant checks ask a DIFFERENT question — "could this variant explain the captured
// output AT ALL?" — and answering it at the accepted fit's `phi` answers it at an alignment chosen
// for another model. That is not a hypothetical weakness: a product mutated to the WRONG SIGN moves
// the accepted fit's `phi` somewhere meaningless (measured: at duty 0.5 the objective is flat
// because w = 0.5 makes the residual continuous, and the fit drifted to `phi` ~= step), and at THAT
// alignment the wrong-sign variant no longer matched the output — so the check whose whole job is to
// reject it passed while the product was in fact the wrong-sign variant. Alignment-coupled negative
// controls can fail open. Aligning each variant on its own best phase removes the coupling: the
// variant is now rejected at the alignment MOST favourable to it, and a product that really is that
// variant fits to within kFitTol there and trips the check.
double bestVariantWorst(const std::vector<double>& a, std::size_t first, std::size_t last,
                        double step, double duty, RefVariant v) {
  double worst = 0.0;
  static_cast<void>(fitPhi(a, first, last, step, duty, v, &worst));
  return worst;
}

// =================================================================================================
// State fixtures (through the codec, like every other host acceptance test).
// =================================================================================================

double& slot(DeviceStateV1& st, ParameterId id) {
  return st.parameters[static_cast<std::uint32_t>(id)];
}

double declaredInitial(ParameterId id) {
  const auto* d = lunar24::core::find_parameter(id);
  return d != nullptr ? d->initial : -1.0;
}

// A user cable is a PATCH, and patching into the sink of a normalised route OVERRIDES that route:
// the state must carry `routeOverridden[routeId] == (its sink has a user cable)`
// (state_validation.h check_routes). The bit is DERIVED here from the route table rather than
// hard-coded per call site, because forgetting it is not a cosmetic omission — the candidate is
// refused as route_incoherent and the cell silently never renders. Only the sinks of
// `vco_a_v_oct_in` (joystick / LFO) are affected in this file, but the rule is the validator's,
// applied to whichever sink the caller patches.
void setCable(DeviceStateV1& st, JackId src, JackId sink) {
  const auto s = static_cast<std::uint32_t>(sink);
  st.inputCable[s] = 1;
  st.cableSource[s] = src;
  for (const auto& r : lunar24::registry::kNormalizedRoutes) {
    if (static_cast<std::uint32_t>(r.sinkJack) == s) {
      st.routeOverridden[static_cast<std::uint32_t>(r.id)] = 1;
    }
  }
}

// The pure-pulse cell of S0: morph = 1.0 on BOTH sides (the pulse node, weight exactly 1.0), both
// PWM depths explicitly at 0 so no CV can reach the duty, and B isolated from the DEFAULT A->B
// normalised route with vco_b.cv_amt = 0 — without that isolation B's own frequency follows A's
// output and no constant-step model can describe it (S0's
// `theDefaultAToBRouteLegitimatelyMovesBAudioOffItsDuty`). A static model is legitimate here because
// a whole-state apply SNAPS every seconds-smoothed parameter to its state value rather than ramping
// from the old one, so morph / pw / tune / oct / depth / joystick are exactly at the state's values
// for the whole render.
DeviceStateV1 pulseState(double pwA, double pwB) {
  DeviceStateV1 st = make_default_device_state(kSeed);
  slot(st, ParameterId::vco_a_morph) = 1.0;
  slot(st, ParameterId::vco_b_morph) = 1.0;
  slot(st, ParameterId::vco_a_pwm) = 0.0;
  slot(st, ParameterId::vco_b_pwm) = 0.0;
  slot(st, ParameterId::vco_b_cv_amt) = 0.0;
  slot(st, ParameterId::vco_a_pw) = pwA;
  slot(st, ParameterId::vco_b_pw) = pwB;
  return st;
}

// The joystick's published DC law, `(x - 0.5) * 10` V (joystick_cv.h normToHalfVolt with the 0.5
// offset), is used ONLY to choose the cells below. The step a cell drives is never taken from it:
// `drivenStep` reads the volts the runtime PUBLISHES, so a mis-published or mis-consumed volt moves
// the model instead of hiding inside it. The callers assert the published volts are constant over
// the fitted window, so a moving stimulus can never reach a fit unnoticed.

// The product's frequency law (vco.h frequencyHz) for a side whose generic CV input is unrouted
// (cvAmt = 0 for B; there is no route at all into A's cv_in, so A's cv_ term is always 0):
//   f0 = base * 2^(octs[octSel] + tune + vOct),   octs = {-1, 0, +3}
double drivenHz(int octIndex, double tune, double vOct) {
  const double octs[3] = {lunar24::core::Vco::kLowOctave, lunar24::core::Vco::kZeroOctave,
                          lunar24::core::Vco::kPlus3Octave};
  return lunar24::core::kVcoBaseHzProvisional * std::pow(2.0, octs[octIndex] + tune + vOct);
}

// Rising zero crossings — an INDEPENDENT INDEX into the buffer, used for the timing claims (a
// residual/gap metric is blind to a whole-sample time shift).
std::vector<std::size_t> risingEdges(const std::vector<double>& c) {
  std::vector<std::size_t> e;
  for (std::size_t i = 1; i < c.size(); ++i) {
    if (c[i - 1] <= 0.0 && c[i] > 0.0) e.push_back(i);
  }
  return e;
}

// =================================================================================================
// A rendered cell: the state, the four real outputs, and the per-frame PUBLISHED source volts.
// =================================================================================================
struct Cell {
  const char* id = "";
  double sr = 48000.0;
  int oct = 0;        // oct_sel, applied to BOTH sides (0 = "low" = -1 oct, 1 = "0", 2 = "+3")
  double tune = 0.0;  // tune in octaves, applied to BOTH sides
  double joyX = 0.5;  // joystick.x, cabled to vco_a.v_oct_in when useVoct
  bool useVoct = false;
  double pwA = 0.5;
  double pwB = 0.5;
  int frames = 0;
};

struct Render {
  bool ok = false;
  Cell cell;
  std::vector<double> a, b;        // DRY_A, DRY_B
  std::vector<double> voctVolts;   // published joystick_x_out per frame (when useVoct)
  double dutyA = -1.0;             // runtime readback AT THE END of the render
  double dutyB = -1.0;
  // WHY A CELL DID NOT RENDER. A bare `false` cannot distinguish "the state was rejected" from
  // "a processBlock was not Rendered" from "the codec roundtrip failed", and those demand
  // completely different responses — so the typed outcome is carried out of here too. This is the
  // same discipline the agreed entry already applies to `load()` (task#80: never collapse a
  // rejection into a bare false).
  const char* failStage = "";
  StandaloneAudioEngine::StateApplyStatus status =
      StandaloneAudioEngine::StateApplyStatus::NotAttempted;
  lunar24::core::StateValidationResult validation;
};

// A bounded, DIAGNOSTIC-ONLY trace of renderCell failures: the first few reasons, so a systemic
// fixture error announces itself once instead of arriving as N identical silent CHECK failures.
int g_renderDiagnosticsLeft = 16;

Render renderCell(const Cell& c, int frames = 0) {
  Render r;
  r.cell = c;
  DeviceStateV1 st = pulseState(c.pwA, c.pwB);
  slot(st, ParameterId::vco_a_oct_sel) = double(c.oct);
  slot(st, ParameterId::vco_b_oct_sel) = double(c.oct);
  slot(st, ParameterId::vco_a_tune) = c.tune;
  slot(st, ParameterId::vco_b_tune) = c.tune;
  if (c.useVoct) {
    slot(st, ParameterId::joystick_x) = c.joyX;
    setCable(st, JackId::joystick_x_out, JackId::vco_a_v_oct_in);
  }
  const int n = frames > 0 ? frames : c.frames;
  EngineHarness h;
  h.reserve(std::size_t(n));
  if (!h.load(st, c.sr)) {
    r.failStage = "load";
    r.status = h.applyStatus();
    r.validation = h.validation();
    if (g_renderDiagnosticsLeft-- > 0) {
      std::printf("  renderCell[%s] LOAD REFUSED status=%d family=%d field=%u sr=%.1f oct=%d "
                  "tune=%.3f pwA=%.3f pwB=%.3f voct=%d\n",
                  c.id, int(r.status), int(r.validation.family), r.validation.field, c.sr, c.oct,
                  c.tune, c.pwA, c.pwB, int(c.useVoct));
    }
    return r;
  }
  if (c.useVoct) {
    r.voctVolts.reserve(std::size_t(n));
    if (!h.renderSampled(n, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
          r.voctVolts.push_back(rt.controlVoltageAt(JackId::joystick_x_out));
        })) {
      r.failStage = "renderSampled";
      if (g_renderDiagnosticsLeft-- > 0) {
        std::printf("  renderCell[%s] PROCESS BLOCK NOT RENDERED (sampled) sr=%.1f\n", c.id, c.sr);
      }
      return r;
    }
  } else if (!h.render(n)) {
    r.failStage = "render";
    if (g_renderDiagnosticsLeft-- > 0) {
      std::printf("  renderCell[%s] PROCESS BLOCK NOT RENDERED sr=%.1f\n", c.id, c.sr);
    }
    return r;
  }
  r.a = h.dryA();
  r.b = h.dryB();
  r.dutyA = h.runtime()->vcoAEffectiveDuty();
  r.dutyB = h.runtime()->vcoBEffectiveDuty();
  r.ok = true;
  return r;
}

// The constant step the cell drives: derived from what the RUNTIME PUBLISHED (not from the
// joystick parameter), so a defect that mis-publishes or mis-consumes a volt changes the model
// instead of hiding inside it. Requires the published volts to be constant over the window — the
// caller asserts that separately, so a moving stimulus can never reach this fit unnoticed.
double drivenStep(const Render& r) {
  double vOct = 0.0;
  if (r.cell.useVoct) {
    if (r.voctVolts.empty()) return -1.0;
    vOct = r.voctVolts.front();
  }
  return drivenHz(r.cell.oct, r.cell.tune, vOct) / r.cell.sr;
}

bool voltsAreConstant(const std::vector<double>& v, double* out = nullptr) {
  if (v.empty()) return false;
  for (double x : v) {
    if (x != v.front()) return false;
  }
  if (out) *out = v.front();
  return true;
}

// =================================================================================================
// (1) The pulse node really is what the four outputs carry, on BOTH sides, through a SAVED cable
//     for the PWM jack, with the two sides asymmetric and B isolated from the default A->B route.
// =================================================================================================
void pulse_reaches_the_product_output() {
  // A/B asymmetric: A at 25% duty, B at 70%; B isolated (cv_amt = 0) so its own duty is its own.
  const Render r = renderCell(Cell{"ab_asym", 48000.0, 0, 0.0, 0.5, false, 0.25, 0.70, 24000});
  const bool pulseArmsRendered = r.ok && r.a.size() == 24000 && r.b.size() == 24000;
  CHECK(pulseArmsRendered);
  if (!pulseArmsRendered) return;

  const bool theReadbackDutiesAreTheDrivenOnes =
      std::fabs(r.dutyA - 0.25) < 1e-12 && std::fabs(r.dutyB - 0.70) < 1e-12;
  CHECK(theReadbackDutiesAreTheDrivenOnes);

  // The pulse node is bipolar and two-valued: away from the correction windows the emitted sample is
  // exactly +/-1 volt, i.e. exactly +/- the device scale after normalisation. Measured as the
  // fraction of samples AT that magnitude. At 220 Hz / 48 kHz each edge's window is 2*step = 0.92%
  // of the cycle, so a correct render sits ~98.2% on the rails; the bound is 95%, which leaves room
  // for the sample rates and duty cycle this cell uses without weakening what it rejects: a silent,
  // mixed, decayed or DC channel sits nowhere near it, and neither does a render whose edges are
  // spread over many samples.
  const double rail = lunar24::core::kDeviceScaleProvisional;
  std::size_t onRailA = 0, onRailB = 0, hiA = 0, hiB = 0;
  for (double v : r.a) {
    if (std::fabs(std::fabs(v) - rail) <= 1e-12) ++onRailA;
    if (v > 0.0) ++hiA;
  }
  for (double v : r.b) {
    if (std::fabs(std::fabs(v) - rail) <= 1e-12) ++onRailB;
    if (v > 0.0) ++hiB;
  }
  const double railA = double(onRailA) / double(r.a.size());
  const double railB = double(onRailB) / double(r.b.size());
  const double dutyAudioA = double(hiA) / double(r.a.size());
  const double dutyAudioB = double(hiB) / double(r.b.size());
  std::printf(
      "  ab_asym: readback A=%.4f B=%.4f | rail fraction A=%.4f B=%.4f | audio duty A=%.4f B=%.4f\n",
      r.dutyA, r.dutyB, railA, railB, dutyAudioA, dutyAudioB);
  const bool theDryAOutputIsTheTwoRailPulse = railA > 0.95;
  CHECK(theDryAOutputIsTheTwoRailPulse);
  const bool theDryBOutputIsTheTwoRailPulse = railB > 0.95;
  CHECK(theDryBOutputIsTheTwoRailPulse);

  // Asymmetry, measured on the AUDIO (not on the readback): a common-mode defect that drives both
  // sides from one duty makes the two high-sample fractions collapse onto each other, and B is
  // isolated from A's default route so nothing legitimately couples them.
  const bool theTwoSidesAreAsymmetric = std::fabs(dutyAudioA - dutyAudioB) > 0.30;
  CHECK(theTwoSidesAreAsymmetric);

  // depth 0 must leave the duty at the base pw (the S0 inertness claim, re-asserted here because
  // this test's model is only meaningful while it holds).
  const bool theDepthZeroDefaultLeavesTheBaseDuty =
      std::fabs(r.dutyA - 0.25) < 1e-12 && std::fabs(declaredInitial(ParameterId::vco_a_pwm)) < 1e-12;
  CHECK(theDepthZeroDefaultLeavesTheBaseDuty);
}

// =================================================================================================
// (2) THE CORE CLAIM, SAMPLE BY SAMPLE: the emitted DRY_A / DRY_B sequence is the accepted
//     formula, and is NOT any of its one-line neighbours.
// =================================================================================================
struct ModelResult {
  bool rendered = false;
  bool voltsConstant = true;
  double step = 0.0;
  double duty = 0.0;
  double phi = 0.0;          // fitted alignment — DIAGNOSTIC, see modelAt's note
  double phiPinned = 0.0;    // the product's own frame-0 phase, NOT fitted: frac(step)
  double phiOffset = 0.0;    // fitted phi minus the pinned one: the 源帧 reconciliation number
  double worstPinned = 1e30; // the accepted model at the PINNED phase, no free parameter
  double worstCapped = 1e30;
  double worstNaive = 0.0;
  double worstUncapped = 0.0;
  double worstSign = 0.0;
  double worstPosition = 0.0;
  double worstGuarded = 0.0;
};

ModelResult modelAt(const Cell& c, int frames, std::size_t first, std::size_t last,
                    char which = 'A') {
  ModelResult m;
  const Render r = renderCell(c, frames);
  if (!r.ok) return m;
  const std::vector<double>& audio = (which == 'B') ? r.b : r.a;
  if (audio.size() != std::size_t(frames)) return m;
  m.voltsConstant = c.useVoct ? voltsAreConstant(r.voctVolts) : true;
  m.step = drivenStep(r);
  m.duty = (which == 'B') ? r.dutyB : r.dutyA;
  if (!(m.step > 0.0) || !(m.duty > 0.0)) return m;

  // The alignment is recovered ON THE ACCEPTED MODEL and the accepted formula is judged there.
  // ⚠ THE FITTED PHASE IS A DIAGNOSTIC, NOT THE TIMING CRITERION. A phase is a free parameter, so a
  // capture whose WHOLE sequence is one frame late is the same capture one fitted step to the left:
  // `frac(phi + i*step)` with phi' = phi - step reproduces it exactly. Nothing that searches over
  // phi can therefore reject a uniform one-frame latency — which is why the criterion is the PINNED
  // fit below, and why the fitted value is reported only to be compared against it.
  m.phi = fitPhi(audio, first, last, m.step, m.duty, RefVariant::kCapped, &m.worstCapped);
  const double raw = m.step - std::floor(m.step);
  m.phiPinned = (raw < 0.0) ? raw + 1.0 : raw;
  // THE ABSOLUTE ORIGIN ANCHOR (源帧/相位推进对账). The capture starts at the engine's frame 0, and
  // the product's per-frame loop ADVANCES the phase accumulator and then publishes what it advanced
  // to (Vco::tick: `cumPitch_ += step` ... then `emittedAt_(cumPitch_, step)`), so frame 0 is
  // emitted at phase = one step, not at phase 0. The alignment is therefore NOT free: it is
  // `frac(step)`, fixed by the product's own frame convention and by nothing else. Fitting the
  // accepted model at THAT phase — no search, no free parameter — is what makes a uniformly
  // delayed (or advanced) emission fail: one frame of latency moves the true alignment by a whole
  // step (5.0e-3 .. 1.8 in these cells), while the measured residual here is ~1e-8.
  m.phiOffset = m.phi - m.phiPinned;
  m.worstPinned = worstAt(audio, first, last, m.step, m.duty, RefVariant::kCapped, m.phiPinned);
  // The variants are then each aligned on their OWN best phase — see bestVariantWorst: these are
  // BEST-CASE numbers for the variant, i.e. the strongest form of "this variant cannot explain the
  // capture", and they no longer inherit an alignment that a mutated product has moved.
  m.worstNaive = bestVariantWorst(audio, first, last, m.step, m.duty, RefVariant::kNaive);
  m.worstUncapped = bestVariantWorst(audio, first, last, m.step, m.duty, RefVariant::kUncapped);
  m.worstSign = bestVariantWorst(audio, first, last, m.step, m.duty, RefVariant::kWrongSign);
  m.worstPosition = bestVariantWorst(audio, first, last, m.step, m.duty, RefVariant::kWrongPosition);
  m.worstGuarded = bestVariantWorst(audio, first, last, m.step, m.duty, RefVariant::kDutyGuarded);
  std::printf(
      "  model[%s/%c] step=%.9f duty=%.6f phi=%.9f | capped=%.3e naive=%.3e uncapped=%.3e "
      "sign=%.3e pos=%.3e guarded=%.3e (variants at their own best phase)\n",
      c.id, which, m.step, m.duty, m.phi, m.worstCapped, m.worstNaive, m.worstUncapped,
      m.worstSign, m.worstPosition, m.worstGuarded);
  std::printf("  origin[%s/%c] pinned phi=frac(step)=%.12f  fitted-pinned=%+.3e  "
              "residual AT the pinned phase=%.3e\n",
              c.id, which, m.phiPinned, m.phiOffset, m.worstPinned);
  m.rendered = true;
  return m;
}

void the_emitted_pulse_equals_the_two_edge_formula() {
  const std::size_t first = 512, last = 1536;  // ~4.6 periods at 220 Hz; no warm-up needed
  struct CellSpec {
    Cell cell;
    char which;                     // 'A' or 'B'
    bool guardedVariantIsDistinct;  // min(duty, 1-duty) < 2*step: the withdrawn guard would bite
  };
  // The four cells below are all INSIDE the kernel's domain (step <= 0.5), i.e. exactly the regime
  // the S3 acceptance numbers apply to. The last one is the NARROW duty cell: at 0.005 duty and
  // 440 Hz / 44.1 kHz, min(duty, 1-duty) = 0.005 < 2*step = 0.01995, which is the cell where the
  // withdrawn duty hard switch changes the output and the correct formula does not branch.
  const CellSpec specs[5] = {
      {Cell{"a_48k_220_pw50", 48000.0, 0, 0.0, 0.5, false, 0.5, 0.5, 4096}, 'A', false},
      {Cell{"a_44k1_440_pw10", 44100.0, 1, 0.0, 0.5, false, 0.1, 0.5, 4096}, 'A', false},
      {Cell{"a_44k1_440_pw005", 44100.0, 1, 0.0, 0.5, false, 0.005, 0.5, 4096}, 'A', true},
      {Cell{"a_96k_880_pw90", 96000.0, 1, 1.0, 0.5, false, 0.9, 0.5, 8192}, 'A', false},
      {Cell{"b_48k_220_pw25", 48000.0, 0, 0.0, 0.5, false, 0.5, 0.25, 4096}, 'B', false},
  };
  for (const CellSpec& s : specs) {
    const ModelResult m = modelAt(s.cell, s.cell.frames, first, last, s.which);
    const bool thisCellRendered = m.rendered && m.voltsConstant;
    CHECK(thisCellRendered);
    if (!thisCellRendered) continue;

    const bool theProductPulseEqualsTheTwoEdgeFormula = m.worstCapped <= kFitTol;
    CHECK(theProductPulseEqualsTheTwoEdgeFormula);
    // THE SAME MODEL, JUDGED AT THE PRODUCT'S OWN FRAME-0 PHASE INSTEAD OF AT A FITTED ONE — the
    // absolute-timing criterion. `theProductPulseEqualsTheTwoEdgeFormula` above searches over phi
    // and therefore CANNOT see a uniform shift of the whole emitted sequence; this one has no free
    // parameter to hide it in. Both must hold; the fitted number is diagnostics, this is the gate.
    const bool theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance = m.worstPinned <= kFitTol;
    CHECK(theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance);
    const bool theProductPulseIsNotTheNaiveUncorrectedShape = m.worstNaive > kFitMargin;
    CHECK(theProductPulseIsNotTheNaiveUncorrectedShape);
    const bool theProductPulseIsNotTheWrongSignVariant = m.worstSign > kFitMargin;
    CHECK(theProductPulseIsNotTheWrongSignVariant);
    // AT A 50% DUTY THE WRONG-EDGE-POSITION VARIANT IS NOT A WRONG GUESS — IT IS THE SAME
    // FUNCTION. The variant substitutes frac(t + duty) for frac(t - duty); because frac(x + 1) ==
    // frac(x), the two coincide identically exactly when duty == 0.5 (the 1e-3..1-1e-3 clamp
    // excludes 0 and 1). So this cell must not be asked to reject it, and it must not be silently
    // skipped either: the branch below INVERTS the assertion there, requiring the variant to fit to
    // the same kFitTol the correct formula does — which is itself the evidence that the two are one
    // function. The distinction is made on the MEASURED duty, never on the pw parameter.
    if (std::fabs(m.duty - 0.5) > 1e-9) {
      const bool theProductPulseIsNotTheWrongEdgePositionVariant = m.worstPosition > kFitMargin;
      CHECK(theProductPulseIsNotTheWrongEdgePositionVariant);
    } else {
      const bool atAFiftyPercentDutyTheWrongEdgePositionVariantIsTheSameFunction =
          m.worstPosition <= kFitTol;
      CHECK(atAFiftyPercentDutyTheWrongEdgePositionVariantIsTheSameFunction);
    }
    if (s.guardedVariantIsDistinct) {
      const bool theProductPulseIsNotTheWithdrawnDutyGuardedVariant = m.worstGuarded > kFitMargin;
      CHECK(theProductPulseIsNotTheWithdrawnDutyGuardedVariant);
    }
  }
}

// =================================================================================================
// (3) ABOVE kPolyblepMaxDt THE WIDTH IS CAPPED AND THE PHASE IS NOT. The step itself crosses 0.5
//     through the product's own V/OCT input, driven by a joystick DC cable in the saved state.
// =================================================================================================
void the_kernel_width_cap_holds_above_half_a_sample() {
  const std::size_t first = 512, last = 1536;
  // vco_a.oct_sel = 2 ("+3") with tune = +1 gives a 7040 Hz carrier, so V/OCT volts map to
  // 7040*2^v Hz: 1.9 V -> 0.55, 2.6 V -> 0.89, 3.5 V -> 1.81, 3.0 V -> 0.59 at the stated rates.
  const Cell cells[4] = {
      Cell{"voct_48k_step055", 48000.0, 2, 1.0, 0.69, true, 0.5, 0.5, 8192},
      Cell{"voct_48k_step089", 48000.0, 2, 1.0, 0.76, true, 0.5, 0.5, 8192},
      Cell{"voct_96k_step059", 96000.0, 2, 1.0, 0.80, true, 0.5, 0.5, 16384},
      Cell{"voct_44k1_step181", 44100.0, 2, 1.0, 0.85, true, 0.5, 0.5, 8192},
  };
  double minReportedStep = 1e30, maxReportedStep = 0.0;
  for (const Cell& c : cells) {
    const ModelResult m = modelAt(c, c.frames, first, last, 'A');
    const bool thisCellRendered = m.rendered && m.voltsConstant;
    CHECK(thisCellRendered);
    if (!thisCellRendered) continue;
    minReportedStep = std::min(minReportedStep, m.step);
    maxReportedStep = std::max(maxReportedStep, m.step);

    // The cell set is only meaningful if the step really left the kernel's domain.
    const bool theDrivenStepIsAboveTheKernelCap = m.step > 0.5;
    CHECK(theDrivenStepIsAboveTheKernelCap);

    const bool theProductPulseEqualsTheCappedFormulaAboveHalfASample = m.worstCapped <= kFitTol;
    CHECK(theProductPulseEqualsTheCappedFormulaAboveHalfASample);
    // The absolute-origin anchor again, here in the CAPPED domain (see modelAt): same criterion,
    // same name, so a uniform emission delay is rejected above the cap as well as below it.
    const bool theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance = m.worstPinned <= kFitTol;
    CHECK(theEmittedOriginPhaseIsTheProductsOwnPhaseAdvance);
    const bool theProductPulseIsNotTheUncappedKernelVariant = m.worstUncapped > kFitMargin;
    CHECK(theProductPulseIsNotTheUncappedKernelVariant);
    const bool theProductPulseIsNotTheNaiveUncorrectedShapeAboveTheCap = m.worstNaive > kFitMargin;
    CHECK(theProductPulseIsNotTheNaiveUncorrectedShapeAboveTheCap);
  }
  std::printf("  cap cells: step range %.6f .. %.6f\n", minReportedStep, maxReportedStep);
  const bool theCapCellSetCrossedTheBoundaryInTheLegallyReachableBox =
      minReportedStep > 0.5 && maxReportedStep > 1.0;
  CHECK(theCapCellSetCrossedTheBoundaryInTheLegallyReachableBox);

  // THE CONTINUOUS SWEEP ACROSS THE BOUNDARY. lfo_a.cv_out is unipolar 0..+10 V; at 0.5 Hz the same
  // 7040 Hz carrier is swept from step ~0.15 to far above 1 within one render, i.e. through the cap
  // in both directions and past a whole period per sample. The bounded continuous extension claims
  // exactly two things here: every output stays FINITE at every step on the way (no division by a
  // shrinking dt, no NaN, no inf), and nothing escapes the adapter's declared output range. The
  // published sweep volts are recorded per frame so the crossing is MEASURED, not assumed. What the
  // FORMULA does at those steps is pinned by the four fitted cells above — that is where a
  // wrong-but-finite extension fails; this sweep is the domain coverage around them.
  DeviceStateV1 st = pulseState(0.5, 0.5);
  slot(st, ParameterId::vco_a_oct_sel) = 2.0;
  slot(st, ParameterId::vco_a_tune) = 1.0;
  slot(st, ParameterId::lfo_a_rate) = 0.5;
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_v_oct_in);
  const int sweepFrames = 96000;  // 2 s at 0.5 Hz: one whole LFO cycle
  EngineHarness h;
  h.reserve(std::size_t(sweepFrames));
  double loV = 1e30, hiV = -1e30;
  const bool sweepRendered =
      h.load(st, 48000.0) &&
      h.renderSampled(sweepFrames, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
        const double v = rt.controlVoltageAt(JackId::lfo_a_cv_out);
        loV = std::min(loV, v);
        hiV = std::max(hiV, v);
      }) &&
      h.dryA().size() == std::size_t(sweepFrames);
  CHECK(sweepRendered);
  if (!sweepRendered) return;

  const double loStep = drivenHz(2, 1.0, loV) / 48000.0;
  const double hiStep = drivenHz(2, 1.0, hiV) / 48000.0;
  std::printf("  voct sweep: published volts %.4f .. %.4f -> driven steps %.4f .. %.4f\n", loV, hiV,
              loStep, hiStep);
  const bool theSweepCrossedTheKernelCapInBothDirections = loStep < 0.5 && hiStep > 0.5;
  CHECK(theSweepCrossedTheKernelCapInBothDirections);
  const bool theSweepReachedStepsAboveOneWholePeriodPerSample = hiStep > 1.0;
  CHECK(theSweepReachedStepsAboveOneWholePeriodPerSample);

  bool finite = true;
  double peakAny = 0.0, peakDry = 0.0;
  for (int ch = 0; ch < kOutCh; ++ch) {
    const bool isDry = (ch == 2 || ch == 3);  // kOutCh order: WET_L, WET_R, DRY_A, DRY_B
    for (double v : h.out(ch)) {
      if (!std::isfinite(v)) finite = false;
      peakAny = std::max(peakAny, std::fabs(v));
      if (isDry) peakDry = std::max(peakDry, std::fabs(v));
    }
  }
  std::printf("  voct sweep: peak |any output| = %.9f, peak |DRY| = %.9f (device scale %.3f)\n",
              peakAny, peakDry, lunar24::core::kDeviceScaleProvisional);
  const bool theSweepAcrossTheCapIsFinite = finite;
  CHECK(theSweepAcrossTheCapIsFinite);
  // The adapter's declared output range. This check is deliberately WEAK, and is labelled as such:
  // device_normalized_from_volts clamps at +/-1, so a finite value cannot escape it — and the
  // non-finite value that COULD escape it is what the check above already rejects. It is here so
  // that the declared range is verified rather than assumed, not because it carries the weight.
  const bool nothingEscapedTheAdaptersDeclaredOutputRange = peakAny <= 1.0 + 1e-12;
  CHECK(nothingEscapedTheAdaptersDeclaredOutputRange);
}

// =================================================================================================
// (4) THE PHASE ADVANCE IS UNTOUCHED. The correction changes WAVEFORM VALUES only, so the emitted
//     sequence is a function of the DRIVEN step — a step perturbed by 1e-6 must stop fitting, and
//     the measured period must be the driven one. This is the "actual frequency unchanged" claim
//     in the form a product render can actually falsify.
// =================================================================================================
void the_driven_step_is_the_step_that_fits() {
  const std::size_t first = 512, last = 1536;
  const Cell c{"step_pin", 48000.0, 1, 0.0, 0.5, false, 0.5, 0.5, 4096};
  const Render r = renderCell(c);
  const bool stepArmsRendered = r.ok && r.a.size() == 4096;
  CHECK(stepArmsRendered);
  if (!stepArmsRendered) return;
  const double step = drivenStep(r);
  const bool stepIsTheDrivenOne = std::fabs(step - 440.0 / 48000.0) < 1e-15;
  CHECK(stepIsTheDrivenOne);

  const double phi = fitPhi(r.a, first, last, step, r.dutyA, RefVariant::kCapped, nullptr);
  const double exact = worstAt(r.a, first, last, step, r.dutyA, RefVariant::kCapped, phi);
  const double slow =
      worstAt(r.a, first, last, step * (1.0 - 1e-6), r.dutyA, RefVariant::kCapped, phi);
  const double fast =
      worstAt(r.a, first, last, step * (1.0 + 1e-6), r.dutyA, RefVariant::kCapped, phi);
  const double slow1e3 =
      worstAt(r.a, first, last, step * (1.0 - 1e-3), r.dutyA, RefVariant::kCapped, phi);
  const double fast1e3 =
      worstAt(r.a, first, last, step * (1.0 + 1e-3), r.dutyA, RefVariant::kCapped, phi);
  std::printf("  step sensitivity: exact=%.3e  -1e-6=%.3e  +1e-6=%.3e  -1e-3=%.3e  +1e-3=%.3e\n",
              exact, slow, fast, slow1e3, fast1e3);
  const bool theEmittedSequenceIsAFunctionOfTheDrivenStep = exact <= kFitTol;
  CHECK(theEmittedSequenceIsAFunctionOfTheDrivenStep);
  // A SENSITIVITY LADDER, read against the two tolerances this file already has rather than a
  // threshold invented for the occasion. A step error of one part in a million is a tiny
  // perturbation — over the 1024-frame window it accumulates ~1e-5 cycles of phase — so it is not
  // expected to look like the grossly wrong shapes kFitMargin (1e-2) separates; what it must do is
  // be EXCLUDED by the same kFitTol that accepts the exact model. It is, by ~2 orders of magnitude
  // (measured 1.2e-3). A 0.1% step error is a real mis-derived step and must clear kFitMargin. Both
  // numbers are printed above; if either collapses, the fit has stopped being a function of step.
  const bool aStepOnePartInAMillionLowIsRejectedByTheFitTolerance = slow > kFitTol;
  CHECK(aStepOnePartInAMillionLowIsRejectedByTheFitTolerance);
  const bool aStepOnePartInAMillionHighIsRejectedByTheFitTolerance = fast > kFitTol;
  CHECK(aStepOnePartInAMillionHighIsRejectedByTheFitTolerance);
  const bool aStepOnePartInAThousandLowDoesNotFitTheEmittedSequence = slow1e3 > kFitMargin;
  CHECK(aStepOnePartInAThousandLowDoesNotFitTheEmittedSequence);
  const bool aStepOnePartInAThousandHighDoesNotFitTheEmittedSequence = fast1e3 > kFitMargin;
  CHECK(aStepOnePartInAThousandHighDoesNotFitTheEmittedSequence);

  // The same fact read off an INDEPENDENT INDEX: the measured period of the emitted wave is the
  // driven one, to within the one-sample bound the correction itself can move an edge by.
  const auto e = risingEdges(r.a);
  const bool enoughEdgesWereMeasured = e.size() >= 8;
  CHECK(enoughEdgesWereMeasured);
  if (enoughEdgesWereMeasured) {
    const double measuredPeriod = double(e.back() - e.front()) / double(e.size() - 1);
    const double drivenPeriod = 1.0 / step;
    std::printf("  step timing: measured period %.6f frames, driven %.6f\n", measuredPeriod,
                drivenPeriod);
    const bool theMeasuredPeriodIsTheDrivenPeriod =
        std::fabs(measuredPeriod - drivenPeriod) <= 1.0 + 1e-9;
    CHECK(theMeasuredPeriodIsTheDrivenPeriod);
  }
}

// =================================================================================================
// (5) IRREGULAR BLOCK SIZES ARE PARTITION-INVARIANT — with a CHANGING PWM stimulus. A static
//     stimulus makes a block-rate control defect invisible, so the CV here moves every frame.
// =================================================================================================
void irregular_blocks_are_partition_invariant_with_a_changing_pwm_cable() {
  DeviceStateV1 st = pulseState(0.5, 0.5);
  slot(st, ParameterId::vco_a_pwm) = 0.5;    // non-zero depth: the cable must be live
  slot(st, ParameterId::lfo_a_rate) = 20.0;  // the registry MAXIMUM (declared 0.1 .. 20) -- every
                                             // block sees a different CV, so a block-rate consumer
                                             // cannot hide behind a slow stimulus
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
  const int kBlock = 512;
  const int kTotal = kBlock * 12;
  // 24 blocks of very different sizes covering the SAME 6144 frames the 12 uniform blocks cover.
  // Each entry must stay <= kBlock (renderBlock requires frames <= the prepared block size). The
  // list is centred on 24 x 256 = 6144 with PAIRWISE-CANCELLING deviations, plus the two block-size
  // EXTREMES (512 = a full prepared block, and 1) which together overshoot by one unit; the
  // trailing 255 absorbs it. The asserted sum is the guard, not the comment: a 12-entry list cannot
  // reach 6144 at all with every entry <= 512 (12*512 is already the total), and the first version
  // of this list was short by 4598 — the CHECK is what caught it.
  const int kIrregular[24] = {512, 1,   456, 56,  406, 106, 356, 156, 306, 206, 281, 231,
                              266, 246, 261, 251, 259, 253, 258, 254, 257, 255, 256, 255};
  int irregularSum = 0;
  for (int v : kIrregular) irregularSum += v;
  const bool theIrregularPartitionCoversTheSameWindow = irregularSum == kTotal;
  CHECK(theIrregularPartitionCoversTheSameWindow);
  if (!theIrregularPartitionCoversTheSameWindow) return;

  const auto silent = [](std::size_t, double& i0, double& i1) {
    i0 = 0.0;
    i1 = 0.0;
  };
  EngineHarness hRef, hIrr;
  hRef.reserve(std::size_t(kTotal));
  hIrr.reserve(std::size_t(kTotal));
  const bool refLoaded = hRef.load(st, 48000.0, kBlock);
  const bool irrLoaded = hIrr.load(st, 48000.0, kBlock);
  const bool partitionArmsLoaded = refLoaded && irrLoaded;
  CHECK(partitionArmsLoaded);
  if (!partitionArmsLoaded) return;

  bool rendered = true;
  for (int i = 0; i < kTotal / kBlock; ++i) {
    if (!hRef.renderBlock(kBlock, silent)) rendered = false;
  }
  for (int v : kIrregular) {
    if (!hIrr.renderBlock(v, silent)) rendered = false;
  }
  const bool irregularPartitionRendersCompleted = rendered && !hIrr.dryA().empty();
  CHECK(irregularPartitionRendersCompleted);
  if (!irregularPartitionRendersCompleted) return;

  // The stimulus must really be MOVING, otherwise this is a vacuous comparison.
  double span = 0.0;
  for (std::size_t i = 1; i < hRef.dryA().size(); ++i) {
    span = std::max(span, std::fabs(hRef.dryA()[i] - hRef.dryA()[i - 1]));
  }
  const bool thePartitionStimulusIsChanging = span > 1e-3;
  CHECK(thePartitionStimulusIsChanging);

  bool same = hRef.dryA().size() == hIrr.dryA().size();
  std::size_t differing = 0;
  double maxDiff = 0.0;
  if (same) {
    for (std::size_t i = 0; i < hIrr.dryA().size(); ++i) {
      if (hRef.dryA()[i] != hIrr.dryA()[i]) {
        same = false;
        ++differing;
        maxDiff = std::max(maxDiff, std::fabs(hRef.dryA()[i] - hIrr.dryA()[i]));
      }
    }
  }
  std::printf("  partition: uniform %zu frames, irregular %zu frames, |dryA| span %.9f, "
              "differing %zu, max |diff| %.9f\n",
              hRef.dryA().size(), hIrr.dryA().size(), span, differing, maxDiff);
  // THE INVARIANCE CLAIM CARRIES ITS OWN NON-VACUITY. "The two partitions agree" is trivially true
  // of a capture that does not move -- a frozen phase, a silent render or a dead cable all satisfy
  // it -- so the claim asserted under this name is the CONJUNCTION: the partitions agree exactly
  // AND the capture they agree on is a moving signal. The guard keeps its own separate CHECK above,
  // so a vacuous run reports BOTH names; folding it in is what stops a vacuous run from reporting
  // that partition invariance was demonstrated. (Measured on the isolated per-sample phase reset:
  // span 0.000000000, both arms equal, and only the guard fired.)
  const bool irregularBlocksReproduceTheUniformPartitionExactly =
      same && thePartitionStimulusIsChanging;
  CHECK(irregularBlocksReproduceTheUniformPartitionExactly);
}

// =================================================================================================
// (6) SAME-FRAME CONSUMPTION OF THE PWM CV. The runtime's PWM latch is a per-frame consumer; a
//     one-frame-late consumer flips both counts below, so the lag-1 arm is a real discriminator
//     rather than a formality.
// =================================================================================================
void the_pwm_sink_consumes_the_same_frames_published_cv() {
  DeviceStateV1 st = pulseState(0.5, 0.5);
  slot(st, ParameterId::vco_a_pwm) = 0.5;
  slot(st, ParameterId::lfo_a_rate) = 7.0;
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
  const int frames = 4800;
  EngineHarness h;
  h.reserve(std::size_t(frames));
  const bool sinkArmsLoaded = h.load(st, 48000.0);
  CHECK(sinkArmsLoaded);
  if (!sinkArmsLoaded) return;

  std::vector<double> source;
  std::vector<double> sink;
  source.reserve(std::size_t(frames));
  sink.reserve(std::size_t(frames));
  const bool sinkRendered = h.renderSampled(frames, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
    source.push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
    sink.push_back(rt.vcoAPwmCv());
  });
  const bool sinkTraceRendered = sinkRendered && source.size() == std::size_t(frames);
  CHECK(sinkTraceRendered);
  if (!sinkTraceRendered) return;

  std::size_t sameFrame = 0, previousFrame = 0;
  for (std::size_t i = 0; i < sink.size(); ++i) {
    if (sink[i] == source[i]) ++sameFrame;
    if (i > 0 && sink[i] == source[i - 1]) ++previousFrame;
  }
  std::printf("  pwm sink: same-frame %zu/%zu, previous-frame %zu\n", sameFrame, sink.size(),
              previousFrame);
  const bool thePwmSinkConsumesTheSameFramesPublishedCv = sameFrame == sink.size();
  CHECK(thePwmSinkConsumesTheSameFramesPublishedCv);
  const bool thePwmSinkDoesNotConsumeThePreviousFramesCv = previousFrame == 0;
  CHECK(thePwmSinkDoesNotConsumeThePreviousFramesCv);

  // And the modulated CV really reaches the DUTY. A latch trace that tracks a moving source would
  // equally be produced by a consumer that publishes but never applies, so this is a second,
  // independent observation: the same state rendered with and WITHOUT the cable, both at the same
  // non-zero depth, so the cable is the only difference between the two arms.
  DeviceStateV1 idle = pulseState(0.5, 0.5);
  slot(idle, ParameterId::vco_a_pwm) = 0.5;
  slot(idle, ParameterId::lfo_a_rate) = 7.0;
  EngineHarness hIdle;
  hIdle.reserve(std::size_t(frames));
  const bool dutyArmsRendered =
      hIdle.load(idle, 48000.0) && hIdle.render(frames) && h.runtime() != nullptr;
  CHECK(dutyArmsRendered);
  if (dutyArmsRendered) {
    const double modulatedDuty = h.runtime()->vcoAEffectiveDuty();
    const double idleDuty = hIdle.runtime()->vcoAEffectiveDuty();
    std::printf("  pwm sink: last source cv=%.6f | final duty modulated=%.6f idle=%.6f\n",
                source.back(), modulatedDuty, idleDuty);
    const bool theModulatedCvReachesTheDuty = std::fabs(modulatedDuty - 0.5) > 1e-6;
    CHECK(theModulatedCvReachesTheDuty);
    const bool theUncabledDepthAloneLeavesTheDutyAtTheBaseWidth = std::fabs(idleDuty - 0.5) < 1e-12;
    CHECK(theUncabledDepthAloneLeavesTheDutyAtTheBaseWidth);
  }
}

// =================================================================================================
// (7) REPEATED RESTORE. A state saved with a PWM cable must land on the DSP, twice, and a restore
//     back to a different state must track the state rather than latch.
// =================================================================================================
void repeated_restore_tracks_the_saved_pwm_cable() {
  DeviceStateV1 def = pulseState(0.5, 0.5);
  DeviceStateV1 patched = pulseState(0.5, 0.5);
  slot(patched, ParameterId::vco_a_pwm) = 0.5;
  slot(patched, ParameterId::joystick_x) = 0.9;
  setCable(patched, JackId::joystick_x_out, JackId::vco_a_pwm_in);

  EngineHarness h;
  const bool restoreArmsLoaded = h.load(def, 48000.0);
  CHECK(restoreArmsLoaded);
  if (!restoreArmsLoaded) return;

  bool ok = true;
  for (int rep = 0; rep < 2; ++rep) {
    const auto s = h.applyCanonical(patched);
    ok = ok && s == StandaloneAudioEngine::StateApplyStatus::Accepted;
    ok = ok && std::fabs(h.runtime()->vcoAPwm() - 0.5) < 1e-12;
    if (!h.render(4096)) ok = false;
  }
  const bool repeatedRestoreOfTheSavedPwmCableLandsOnTheDsp = ok;
  CHECK(repeatedRestoreOfTheSavedPwmCableLandsOnTheDsp);

  const auto back = h.applyCanonical(def);
  const bool restoreBackToDefaultTracksTheState =
      back == StandaloneAudioEngine::StateApplyStatus::Accepted &&
      std::fabs(h.runtime()->vcoAPwm() - declaredInitial(ParameterId::vco_a_pwm)) < 1e-12;
  CHECK(restoreBackToDefaultTracksTheState);

  // The cable itself must be part of what was restored. ONE state with a saved joystick -> PWM cable
  // and one without, identical in every other respect including the depth, so the cable is the only
  // difference: the cabled arm's duty must leave the base width, and the plain arm's must still be
  // exactly the base width.
  DeviceStateV1 pst = pulseState(0.5, 0.5);
  slot(pst, ParameterId::vco_a_pwm) = 0.5;
  slot(pst, ParameterId::joystick_x) = 0.9;
  setCable(pst, JackId::joystick_x_out, JackId::vco_a_pwm_in);
  DeviceStateV1 uncabled = pulseState(0.5, 0.5);
  slot(uncabled, ParameterId::vco_a_pwm) = 0.5;
  slot(uncabled, ParameterId::joystick_x) = 0.9;
  EngineHarness hp, hb;
  hp.reserve(8192);
  hb.reserve(8192);
  const bool cableArmsLoaded = hp.load(pst, 48000.0) && hb.load(uncabled, 48000.0) &&
                               hp.render(8192) && hb.render(8192);
  CHECK(cableArmsLoaded);
  if (cableArmsLoaded) {
    const double patchedDuty = hp.runtime()->vcoAEffectiveDuty();
    const double baseDuty = hb.runtime()->vcoAEffectiveDuty();
    const double basePw = declaredInitial(ParameterId::vco_a_pw);
    std::printf("  restore: cabled duty=%.6f uncabled duty=%.6f (base pw %.6f)\n", patchedDuty,
                baseDuty, basePw);
    const bool theSavedCableMovesTheDutyOffTheBaseWidth = std::fabs(patchedDuty - basePw) > 0.1;
    CHECK(theSavedCableMovesTheDutyOffTheBaseWidth);
    const bool theSameDepthWithoutTheCableLeavesTheDutyAtTheBaseWidth =
        std::fabs(baseDuty - basePw) < 1e-12;
    CHECK(theSameDepthWithoutTheCableLeavesTheDutyAtTheBaseWidth);
  }
}

// =================================================================================================
// (8) A REJECTED APPLY IS ATOMIC: state, format, plan and the SUBSEQUENT TRACE.
// =================================================================================================
void a_rejected_apply_preserves_state_format_plan_and_trace() {
  const double sr = 48000.0;
  const int bs = 512;
  DeviceStateV1 good = pulseState(0.5, 0.5);
  slot(good, ParameterId::vco_a_oct_sel) = 1.0;
  DeviceStateV1 badPw = good;
  slot(badPw, ParameterId::vco_a_pw) = 2.0;  // outside the declared [0,1]
  DeviceStateV1 badMorph = good;
  slot(badMorph, ParameterId::vco_a_morph) = 2.0;

  const DeviceStateV1* const bad[2] = {&badPw, &badMorph};
  bool rejected[2] = {false, false};
  bool formatKept[2] = {false, false};
  bool traceKept[2] = {false, false};
  bool prepared[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    EngineHarness hClean, hBad;
    hClean.reserve(6144);
    hBad.reserve(6144);
    if (!hClean.load(good, sr, bs) || !hBad.load(good, sr, bs)) continue;
    if (!hClean.render(2048) || !hBad.render(2048)) continue;
    prepared[i] = true;
    const double srBefore = hBad.sampleRate();
    const int bsBefore = hBad.blockSize();
    const int inBefore = hBad.inputCapability();
    const int outBefore = hBad.outputCapability();
    const int planOutCapBefore = hBad.plan().outputCapability;
    const int planOutCountBefore = hBad.plan().outputCount;
    const int planIn0Before = hBad.plan().inputCh[0];
    const int planIn1Before = hBad.plan().inputCh[1];

    const auto status = hBad.applyCanonical(*bad[i]);
    rejected[i] = status == StandaloneAudioEngine::StateApplyStatus::RejectedInvalidState;
    formatKept[i] = hBad.sampleRate() == srBefore && hBad.blockSize() == bsBefore &&
                    hBad.inputCapability() == inBefore && hBad.outputCapability() == outBefore &&
                    hBad.plan().valid() && hBad.plan().outputCapability == planOutCapBefore &&
                    hBad.plan().outputCount == planOutCountBefore &&
                    hBad.plan().inputCh[0] == planIn0Before && hBad.plan().inputCh[1] == planIn1Before;

    if (!hClean.render(2048) || !hBad.render(2048)) continue;
    bool same = hClean.dryA().size() == hBad.dryA().size() && !hBad.dryA().empty();
    if (same) {
      for (std::size_t k = 0; k < hBad.dryA().size(); ++k) {
        if (hClean.dryA()[k] != hBad.dryA()[k]) {
          same = false;
          break;
        }
      }
    }
    traceKept[i] = same;
  }
  const bool rejectedApplyArmsWerePrepared = prepared[0] && prepared[1];
  CHECK(rejectedApplyArmsWerePrepared);
  const bool anOutOfRangePwIsRejectedAsInvalidState = rejected[0];
  CHECK(anOutOfRangePwIsRejectedAsInvalidState);
  const bool anOutOfRangeMorphIsRejectedAsInvalidState = rejected[1];
  CHECK(anOutOfRangeMorphIsRejectedAsInvalidState);
  const bool aRejectedApplyPreservesTheCommittedFormat = formatKept[0] && formatKept[1];
  CHECK(aRejectedApplyPreservesTheCommittedFormat);
  const bool aRejectedPwApplyPreservesTheSubsequentTrace = traceKept[0];
  CHECK(aRejectedPwApplyPreservesTheSubsequentTrace);
  const bool aRejectedMorphApplyPreservesTheSubsequentTrace = traceKept[1];
  CHECK(aRejectedMorphApplyPreservesTheSubsequentTrace);
}

// =================================================================================================
// (9) FOUR OUTPUTS FINITE AND BOUNDED across the pulse surface, including the out-of-domain steps
//     the declared input box can reach.
// =================================================================================================
struct SurfaceResult {
  bool finite = true;
  bool inDeclaredRange = true;
  int cells = 0;
  double peakAny = 0.0;  // over all four output channels
  double peakDry = 0.0;  // over DRY_A / DRY_B only
};

SurfaceResult surfaceAt(double sr) {
  SurfaceResult res;
  const double kPw[4] = {0.005, 0.1, 0.5, 0.9};
  const double kJoy[4] = {0.5, 0.69, 0.76, 1.0};
  for (int pi = 0; pi < 4; ++pi) {
    for (int ji = 0; ji < 4; ++ji) {
      DeviceStateV1 st = pulseState(kPw[pi], kPw[(pi + 1) % 4]);
      slot(st, ParameterId::vco_a_oct_sel) = 2.0;
      slot(st, ParameterId::vco_a_tune) = 1.0;
      slot(st, ParameterId::joystick_x) = kJoy[ji];
      setCable(st, JackId::joystick_x_out, JackId::vco_a_v_oct_in);
      setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
      slot(st, ParameterId::lfo_a_rate) = 3.0;
      slot(st, ParameterId::vco_a_pwm) = 0.5;
      EngineHarness h;
      h.reserve(8192);
      if (!h.load(st, sr) || !h.render(4096)) {
        res.finite = false;
        continue;
      }
      ++res.cells;
      for (int c = 0; c < kOutCh; ++c) {
        const bool isDry = (c == 2 || c == 3);  // kOutCh order: WET_L, WET_R, DRY_A, DRY_B
        for (double x : h.out(c)) {
          if (!std::isfinite(x)) res.finite = false;
          res.peakAny = std::max(res.peakAny, std::fabs(x));
          if (isDry) res.peakDry = std::max(res.peakDry, std::fabs(x));
        }
      }
    }
  }
  res.inDeclaredRange = res.peakAny <= 1.0 + 1e-12;
  return res;
}

void four_outputs_finite_across_the_pulse_surface() {
  const SurfaceResult s44k1 = surfaceAt(44100.0);
  const SurfaceResult s48k = surfaceAt(48000.0);
  const SurfaceResult s88k2 = surfaceAt(88200.0);
  const SurfaceResult s96k = surfaceAt(96000.0);
  std::printf(
      "  surface cells %d/%d/%d/%d | peak |any| %.9f/%.9f/%.9f/%.9f | peak |DRY| "
      "%.9f/%.9f/%.9f/%.9f\n",
      s44k1.cells, s48k.cells, s88k2.cells, s96k.cells, s44k1.peakAny, s48k.peakAny, s88k2.peakAny,
      s96k.peakAny, s44k1.peakDry, s48k.peakDry, s88k2.peakDry, s96k.peakDry);
  const bool surfaceWasFullyCovered =
      s44k1.cells == 16 && s48k.cells == 16 && s88k2.cells == 16 && s96k.cells == 16;
  CHECK(surfaceWasFullyCovered);
  const bool fourOutputsFiniteAcrossPulseSurface44k1 = s44k1.finite && s44k1.cells == 16;
  const bool fourOutputsFiniteAcrossPulseSurface48k = s48k.finite && s48k.cells == 16;
  const bool fourOutputsFiniteAcrossPulseSurface88k2 = s88k2.finite && s88k2.cells == 16;
  const bool fourOutputsFiniteAcrossPulseSurface96k = s96k.finite && s96k.cells == 16;
  CHECK(fourOutputsFiniteAcrossPulseSurface44k1);
  CHECK(fourOutputsFiniteAcrossPulseSurface48k);
  CHECK(fourOutputsFiniteAcrossPulseSurface88k2);
  CHECK(fourOutputsFiniteAcrossPulseSurface96k);
  // The adapter's declared output range, on the same deliberately-weak footing as the sweep check:
  // the normaliser clamps at +/-1, so this rejects only a non-finite value -- which the four checks
  // above already reject. It is asserted so the declared contract is verified, not assumed. The
  // DRY peaks are PRINTED rather than bounded, because above the cap the two-edge correction may
  // legitimately carry the pre-clamp value past +/-1 and let the clamp do the bounding.
  const bool nothingEscapedTheDeclaredOutputRangeAtEveryLegalStep =
      s44k1.inDeclaredRange && s48k.inDeclaredRange && s88k2.inDeclaredRange && s96k.inDeclaredRange;
  CHECK(nothingEscapedTheDeclaredOutputRangeAtEveryLegalStep);
}

// =================================================================================================
// (10) HARD SYNC (S5) WITH THE PULSE CORRECTION IN FORCE. The reset's jump is measured on the
//      EMITTED signal (shape + correction), not on the raw shape, so at the first reset the reset
//      sample must be exactly half the value the FREE-RUNNING arm emits at that same frame — the
//      two arms share one phase trajectory up to the first reset, which is what makes the identity
//      checkable at all.
// =================================================================================================
void hard_sync_reset_is_sized_on_the_emitted_signal() {
  DeviceStateV1 st = pulseState(0.5, 0.5);
  slot(st, ParameterId::lfo_a_rate) = 20.0;
  DeviceStateV1 free = st;
  DeviceStateV1 synced = st;
  setCable(synced, JackId::lfo_a_cv_out, JackId::vco_a_sync_in);

  const int kFrames = 24000;
  const auto renderTo = [&](const DeviceStateV1& s, std::vector<double>* out,
                            std::vector<double>* volts) {
    EngineHarness h;
    h.reserve(std::size_t(kFrames));
    if (!h.load(s, 48000.0)) return false;
    if (volts != nullptr) {
      volts->reserve(std::size_t(kFrames));
      if (!h.renderSampled(kFrames, 0.0, [&](const lunar24::core::SynthRuntime& rt) {
            volts->push_back(rt.controlVoltageAt(JackId::lfo_a_cv_out));
          })) {
        return false;
      }
    } else if (!h.render(kFrames)) {
      return false;
    }
    *out = h.dryA();
    return true;
  };
  std::vector<double> aFree, aSync, syncVolts;
  const bool syncArmsRendered = renderTo(free, &aFree, nullptr) && renderTo(synced, &aSync, &syncVolts) &&
                                aFree.size() == aSync.size() && !aFree.empty();
  CHECK(syncArmsRendered);
  if (!syncArmsRendered) return;

  double worstDiff = 0.0;
  bool bounded = true;
  for (std::size_t i = 0; i < aSync.size(); ++i) {
    worstDiff = std::max(worstDiff, std::fabs(aFree[i] - aSync[i]));
    if (!std::isfinite(aSync[i])) bounded = false;
  }
  const bool theSyncCableIsLiveOnThePulsePath = worstDiff > 0.1;
  CHECK(theSyncCableIsLiveOnThePulsePath);
  const bool theSyncedPulseStaysFinite = bounded;
  CHECK(theSyncedPulseStaysFinite);

  // -----------------------------------------------------------------------------------------------
  // THE SYNC EVENT IS RECONSTRUCTED FROM ITS SOURCE, NOT SEARCHED FOR IN THE OUTPUT.
  //
  // Director note (msg `df6b7937`): "sync should be reconciled against the ACTUAL source
  // threshold-crossing event, not by pinning the constant 47 ... do not infer the event by scanning
  // for the first output divergence." The earlier revision of this test did exactly the wrong thing
  // twice over: it scanned the OUTPUT for the divergence, and it compared the source against the
  // bare 5 V threshold.
  //
  // The sink is a GATE, and `sink_gate_interpret` (core/include/lunar24/core/sink_interpret.h) is
  // not a bare comparison: it is a HYSTERESIS LATCH — the low state must clear `thr + hyst` to rise,
  // the high state holds while the input stays above `thr - hyst` — and its FIRST sample only
  // PRIMES, emitting no edge. With the declared 5 V threshold and 0.2 V hysteresis the rise is at
  // 5.2 V and the fall at 4.8 V, so a "first sample >= 5 V" scan names a frame that is not the edge
  // frame at all. The reference below rebuilds that law INDEPENDENTLY over the published source
  // trace, and it runs BEFORE the output is consulted: the frame it predicts is the frame the reset
  // must land on. The threshold and hysteresis are read from the jack's own descriptor, never
  // hardcoded, and the gate is asserted to be a real hysteresis gate so this arm cannot go vacuous.
  const lunar24::core::JackDescriptor* syncSinkDesc =
      lunar24::core::validate_detail::find_jack(
          static_cast<std::uint32_t>(JackId::vco_a_sync_in));
  const double syncThr = (syncSinkDesc != nullptr) ? syncSinkDesc->gateThresholdVolts : -1.0;
  const double syncHyst = (syncSinkDesc != nullptr && syncSinkDesc->hysteresisVolts >= 0.0)
                              ? syncSinkDesc->hysteresisVolts
                              : 0.0;
  const bool theSyncSinkDeclaresARealHysteresisGate =
      syncSinkDesc != nullptr && syncThr > 0.0 && syncHyst > 0.0;
  CHECK(theSyncSinkDeclaresARealHysteresisGate);

  std::size_t predictedEdgeFrame = syncVolts.size();
  if (theSyncSinkDeclaresARealHysteresisGate) {
    bool high = false, prev = false, primed = false;
    for (std::size_t r = 0; r < syncVolts.size(); ++r) {
      high = (!high) ? (syncVolts[r] >= (syncThr + syncHyst)) : (syncVolts[r] > (syncThr - syncHyst));
      if (primed && !prev && high) {
        predictedEdgeFrame = r;
        break;
      }
      prev = high;
      primed = true;
    }
  }
  const bool theRisingEdgeFrameIsPredictableFromTheSource = predictedEdgeFrame < aSync.size();
  CHECK(theRisingEdgeFrameIsPredictableFromTheSource);

  // Everything BEFORE the predicted edge must be bit-identical: the two arms share one phase
  // trajectory up to the reset, so an earlier difference would mean something else moved. This is
  // the half of the criterion that a phase fit cannot supply.
  bool identicalBeforeThePredictedEdge = true;
  for (std::size_t i = 0; theRisingEdgeFrameIsPredictableFromTheSource && i < predictedEdgeFrame;
       ++i) {
    if (aSync[i] != aFree[i]) {
      identicalBeforeThePredictedEdge = false;
      break;
    }
  }
  CHECK(identicalBeforeThePredictedEdge);

  // REPORTED cross-check, not the derivation: the first frame at which the two arms differ. It is
  // computed AFTER the prediction so that the prediction cannot be read off it.
  std::size_t firstDivergence = aSync.size();
  for (std::size_t r = 1; r < aSync.size(); ++r) {
    if (aSync[r] != aFree[r]) {
      firstDivergence = r;
      break;
    }
  }

  if (theRisingEdgeFrameIsPredictableFromTheSource) {
    std::printf(
        "  sync: hysteresis reference rise at frame %zu (thr=%.4f hyst=%.4f, holds above %.4f; "
        "frame 0 primes) | first output divergence at frame %zu | sync=%.12f  0.5*free=%.12f\n",
        static_cast<unsigned long>(predictedEdgeFrame), syncThr, syncHyst, syncThr - syncHyst,
        static_cast<unsigned long>(firstDivergence), aSync[predictedEdgeFrame],
        0.5 * aFree[predictedEdgeFrame]);
    // The reset sample is `emittedAt(0) - 0.5*(emittedAt(0) - free[r])`, and at a 50% duty with
    // w < 0.5 the two-edge formula makes emittedAt(0) exactly 0 (naive +1 cancels the residual of
    // the edge at t = 0), so the identity is `reset == 0.5 * free[r]` EXACTLY — not to a tolerance
    // that could hide a jump sized on the pre-correction shape. The frame is the PREDICTED one.
    const bool theResetLandsOnTheFrameTheHysteresisReferencePredicted =
        aSync[predictedEdgeFrame] == 0.5 * aFree[predictedEdgeFrame];
    CHECK(theResetLandsOnTheFrameTheHysteresisReferencePredicted);
    const bool theResetJumpIsSizedOnTheEmittedSignalNotTheRawShape =
        std::fabs(aSync[predictedEdgeFrame] - aFree[predictedEdgeFrame]) > 0.1;
    CHECK(theResetJumpIsSizedOnTheEmittedSignalNotTheRawShape);
    const bool theFirstDivergenceIsThePredictedFrame = firstDivergence == predictedEdgeFrame;
    CHECK(theFirstDivergenceIsThePredictedFrame);
    // ... and the reset really restarted the slave: after it, the two arms stay apart.
    double after = 0.0;
    for (std::size_t i = predictedEdgeFrame; i < aSync.size(); ++i) {
      after = std::max(after, std::fabs(aFree[i] - aSync[i]));
    }
    const bool theResetActuallyRestartedTheSlave = after > 0.1;
    CHECK(theResetActuallyRestartedTheSlave);
  }
}

// =================================================================================================
// (11) The audio CALLBACK allocates and frees nothing on the pulse + V/OCT + PWM + sync surface.
// =================================================================================================
void the_render_callback_allocates_and_frees_nothing() {
  const int frames = 512;
  DeviceStateV1 st = pulseState(0.9, 0.9);
  slot(st, ParameterId::vco_a_oct_sel) = 2.0;
  slot(st, ParameterId::vco_a_tune) = 1.0;
  slot(st, ParameterId::vco_a_pwm) = 0.8;
  slot(st, ParameterId::joystick_x) = 0.76;
  slot(st, ParameterId::lfo_a_rate) = 20.0;
  slot(st, ParameterId::lfo_b_rate) = 20.0;
  setCable(st, JackId::joystick_x_out, JackId::vco_a_v_oct_in);
  setCable(st, JackId::lfo_a_cv_out, JackId::vco_a_pwm_in);
  // The sync source is a THIRD jack on purpose: every one of these outputs declares maxCables = 1,
  // so feeding the gate from lfo_a.cv_out as well would be refused as cable_cardinality — the
  // candidate would never render and the "surface" would be a comment rather than a state.
  setCable(st, JackId::lfo_b_cv_out, JackId::vco_a_sync_in);
  EngineHarness h;
  const bool allocatorRenderWindowCommitted = h.load(st, 48000.0, frames);
  CHECK(allocatorRenderWindowCommitted);
  if (!allocatorRenderWindowCommitted) return;

  std::vector<double> i0(frames, 0.0), i1(frames, 0.0), o0(frames), o1(frames), o2(frames),
      o3(frames);
  const double* in[kInCh] = {i0.data(), i1.data()};
  double* out[kOutCh] = {o0.data(), o1.data(), o2.data(), o3.data()};

  // THE FREE-RUNNING ARM, so the sync cable is shown to be LIVE rather than decorative: the same
  // state without it, rendered from the same block boundaries, must part company with the sync arm
  // once the gate crosses its 5 V threshold. Both arms get one identical warm-up block so any
  // first-call lazy allocation lands outside the measured window; the free arm's own blocks are
  // rendered BEFORE the snapshot, so its setup can never be mistaken for a callback allocation.
  const auto syncSink = static_cast<std::uint32_t>(JackId::vco_a_sync_in);
  DeviceStateV1 freeSt = st;
  freeSt.inputCable[syncSink] = 0;
  freeSt.cableSource[syncSink] = JackId{0};  // the validator's no-cable sentinel
  EngineHarness hFree;
  std::vector<double> f0(frames), f1(frames), f2(frames), f3(frames);
  double* fout[kOutCh] = {f0.data(), f1.data(), f2.data(), f3.data()};
  bool freeRendered = hFree.load(freeSt, 48000.0, frames);
  bool rendered = freeRendered && h.processPure(in, out, frames) ==
                                      StandaloneAudioEngine::Status::Rendered;
  if (rendered) {
    rendered = hFree.processPure(in, fout, frames) == StandaloneAudioEngine::Status::Rendered;
  }
  for (int i = 0; i < 24 && rendered; ++i) {
    if (hFree.processPure(in, fout, frames) != StandaloneAudioEngine::Status::Rendered) {
      rendered = false;
    }
  }
  const std::size_t allocBefore = g_allocCount;
  const std::size_t freeBefore = g_freeCount;
  for (int i = 0; i < 24; ++i) {
    if (h.processPure(in, out, frames) != StandaloneAudioEngine::Status::Rendered) rendered = false;
  }
  const bool theRenderCallbackAllocatesNothing = rendered && g_allocCount == allocBefore;
  CHECK(theRenderCallbackAllocatesNothing);
  const bool theRenderCallbackFreesNothing = rendered && g_freeCount == freeBefore;
  CHECK(theRenderCallbackFreesNothing);

  double parted = 0.0;
  int firstDiff = -1, differing = 0;
  if (rendered) {
    for (int i = 0; i < frames; ++i) {
      const double d = std::fabs(o2[i] - f2[i]);
      parted = std::max(parted, d);
      if (d != 0.0) {
        ++differing;
        if (firstDiff < 0) firstDiff = i;
      }
    }
  }
  std::printf("  allocator surface: DRY_A vs free-running arm: first diff at frame %d, %d/%d frames "
              "differ, max |diff| %.6f\n",
              firstDiff, differing, frames, parted);
  // The two candidate states differ in EXACTLY ONE bit of configuration — the presence of the sync
  // cable — so a single differing output frame is proof the sync path EXECUTED on the allocator
  // surface; an unbound or ignored binding renders bit-identically and is what this rejects. The
  // magnitude is deliberately not asserted here, and it is small (~9e-4) for a reason worth stating:
  // this surface drives step ~0.89, so the kernel width is capped at 0.5 = half a period per edge,
  // the correction is non-zero almost everywhere, and a small phase offset therefore appears as a
  // small offset on every frame instead of flipping samples between the rails. What the reset's
  // JUMP is sized on is pinned by section (10), at a step inside the kernel's domain.
  const bool theSyncCableOnTheAllocatorSurfaceIsLive = rendered && differing > 0;
  CHECK(theSyncCableOnTheAllocatorSurfaceIsLive);

  // Positive control: the counters are live on this path, so a green above cannot be a dead probe.
  const std::size_t aSnap = g_allocCount;
  void* p = ::operator new(4);
  const bool theAllocatorProbeDetectsAnAllocation = g_allocCount > aSnap;
  CHECK(theAllocatorProbeDetectsAnAllocation);
  ::operator delete(p);
  const std::size_t fSnap = g_freeCount;
  void* q = ::operator new(4);
  ::operator delete(q);
  const bool theAllocatorProbeDetectsAFree = g_freeCount > fSnap;
  CHECK(theAllocatorProbeDetectsAFree);
}

}  // namespace

int main() {
  std::printf("GH#19 S3 acceptance: the pulse two-edge correction through the agreed entry\n");
  pulse_reaches_the_product_output();
  the_emitted_pulse_equals_the_two_edge_formula();
  the_kernel_width_cap_holds_above_half_a_sample();
  the_driven_step_is_the_step_that_fits();
  irregular_blocks_are_partition_invariant_with_a_changing_pwm_cable();
  the_pwm_sink_consumes_the_same_frames_published_cv();
  repeated_restore_tracks_the_saved_pwm_cable();
  a_rejected_apply_preserves_state_format_plan_and_trace();
  four_outputs_finite_across_the_pulse_surface();
  hard_sync_reset_is_sized_on_the_emitted_signal();
  the_render_callback_allocates_and_frees_nothing();
  return ::test::finish("test_gh19_s3_pulse_product_acceptance");
}
