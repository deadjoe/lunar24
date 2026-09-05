// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// device_adapter.h — the single, framework-free product DeviceAdapter (GH#4).
//
// The core (SynthRuntime) emits the four logic outputs (RuntimeOutput) in VIRTUAL
// volts; the real audio device consumes DEVICE-NORMALIZED samples. This adapter is
// the ONLY production bridge between those two worlds: it takes an already-prepared
// device plan plus planar device buffers and, per block, converts device normalized
// samples -> runtime virtual volts (input direction, producing RuntimeInputs) and
// writes a RuntimeOutput back to device normalized samples at the logical -> physical
// mapping the plan chose (output direction). It is header-only, fixed-buffer,
// no-heap, no-lock, no-log, realtime-safe, and framework-free (it never touches a
// CoreAudio / RtAudio / AudioBufferList type).
//
// THE HOST CALLS EXACTLY ONE RENDER: `renderBlock(SynthRuntime&, const double* const*
// planarIn, double* const* planarOut, int frames)`. It matches iPlug2's ProcessBlock
// surface (`sample**`, `sample=double`): per-channel (`double*`) buffers, per block,
// and it is the sole public render path. The per-frame input-resolve and output-write
// logic lives in ONE pair of private primitives (`resolveInput_`/`writeOutput_`) that
// `renderBlock` calls per frame — there is no second, separately hand-written mapping
// to drift. Tests drive `renderBlock` directly (never replicating the frame loop), so
// every accept criterion below is exercised on the exact path the host will run.
//
// Frozen contracts (design/07 §5, the GH#4 mandate):
//   OUTPUT capability:  <2  => prepare() REJECTS (fail-closed; keeps the previous plan
//                               or leaves the adapter with no plan).
//                       2-3 => exactly two channels opened/written (WET L/R).
//                       >=4 => user-selected DISTINCT 4-channel WET+DRY; extra physical
//                              channels are left untouched.
//   No mono downmix, no three-way half output, and no silent DRY drop that still
//   reports a 4-way success.
//   INPUT routes:        0 => both terminals read 0.
//                        1 => the plan explicitly picks EXT only, PREAMP only, or an
//                             EXPLICIT duplicate-one (same physical channel wired to both);
//                             a default / omitted route is NEVER an implicit copy.
//                        >=2 => each terminal bound to an explicit valid physical channel;
//                              using the SAME channel for both is allowed only via an
//                              explicit duplication policy (DualSame).
//   LEVEL:               the four outputs share ONE provisional scale
//                             deviceNormalized = clamp(virtualVolts * 0.5, -1, +1),
//                       preserving WET (max 2V) : DRY (max 1V) = 2:1. There is NO
//                       per-output peak normalization. The input inverse is the SAME
//                       choke point:
//                             virtualVolts = finite(n) ? clamp(n,-1,+1)/0.5 : 0.
//                       The 0.5 is the ONE frozen policy — `kDeviceScaleProvisional` —
//                       a named constant, NOT a configurable gain. Both directions are
//                       marked software-provisional: they are not a claim of hardware
//                       input-rail measurement.
//
// RT boundary (GH#4): the plan is formed COMPLETELY in the non-audio prepare(); a
// failure keeps the previous valid plan (or leaves the adapter with no plan). The
// audio render reads only the stable plan; it does not allocate, lock, log, or swap
// an owner. The host owner swap / ProcessBlock wiring is deliberately NOT part of this
// slice (the GH#4 mandate defers it).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

#include <lunar24/core/device_layout.h>
#include <lunar24/core/machine_runtime.h>

namespace lunar24::core {

// How many / which device input channels the runtime is fed. Frozen input strategy:
// a 0/1/2-route plan that must never silently copy. See the file header.
enum class InputRoute : std::uint8_t {
  Zero = 0,          // 0 route: both terminals read 0 (no input channel consumed).
  ExtOnly = 1,       // 1 route: EXT reads inputCh[0]; PREAMP terminal reads 0.
  PreampOnly = 2,    // 1 route: PREAMP reads inputCh[0]; EXT terminal reads 0.
  DuplicateOne = 3,  // 1 route, EXPLICIT duplication: both terminals read inputCh[0].
  Distinct = 4,      // 2 route: EXT=inputCh[0], PREAMP=inputCh[1] (must differ).
  DualSame = 5,      // 2 route, EXPLICIT duplication: both terminals read inputCh[0].
};

namespace detail {

inline double clamp_val(double v, double lo, double hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

}  // namespace detail

// The ONE frozen provisional scale, software-provisional (design/07 §5; NOT a claim of
// hardware input-rail measurement). Shared by BOTH output directions. This is the single
// driftable truth source for the 0.5 policy — a named constant, never a runtime parameter.
inline constexpr double kDeviceScaleProvisional = 0.5;

// The frozen output choke point: virtual volts -> device-normalized (shared scale).
inline double device_normalized_from_volts(double volts) {
  return detail::clamp_val(volts * kDeviceScaleProvisional, -1.0, 1.0);
}

// The SAME choke point inverted: device-normalized -> virtual volts. A non-finite
// device sample is deterministically zero here (the anomaly is counted by the adapter,
// so the NaN never reaches the DSP).
inline double volts_from_device_normalized(double n) {
  if (!std::isfinite(n)) return 0.0;
  return detail::clamp_val(n, -1.0, 1.0) / kDeviceScaleProvisional;
}

// The complete, immutable device plan. Formed fully in non-audio prepare(); the audio
// render only reads it. Value type, fixed arrays, no heap.
struct DevicePlan {
  DeviceLayout layout;             // output topology (interleaved / non-interleaved).
  int outputCapability = 0;        // physical output channel count (== layout.totalChannels).
  int outputCount = 0;             // channels opened/written: 2 (WET) or 4 (WET+DRY).
  OutputMapping output = OutputMapping::canonical();  // logical -> physical (outputCount used).
  int inputCapability = 0;         // physical input channel count (>=0; negative invalid).
  InputRoute input = InputRoute::Zero;
  int inputCh[2] = {-1, -1};       // [0]=EXT physical ch, [1]=PREAMP physical ch; -1=none.

  bool hasOutput() const { return outputCapability >= 2; }

  // Structural validity of the plan as a whole (capability, mapping, input route).
  bool valid() const;
};

// The single production DeviceAdapter. NOT copyable/movable is unnecessary (it is a
// plain value); it is realtime-friendly by construction.
class DeviceAdapter {
 public:
  DeviceAdapter() = default;

  // NON-audio prepare. Validate the complete candidate; on ANY failure return false and
  // keep the previous valid plan (or leave hasPlan() false). No heap / lock / log.
  //
  // `layout`          — output device topology (its totalChannels is the output capability).
  // `inputCapability` — physical input channel count available (must be >= 0).
  // `output`          — logical -> physical output mapping (outputCount entries meaningful).
  // `outputCount`     — 2 (WET only, capability 2-3) or 4 (WET+DRY, capability >=4).
  // `input`           — the frozen input route.
  // `extCh`/`preampCh`— physical input channel for the EXT / PREAMP terminals (-1 = none);
  //                     ignored where the route does not consume them.
  bool prepare(const DeviceLayout& layout, int inputCapability, const OutputMapping& output,
               int outputCount, InputRoute input, int extCh, int preampCh);

  bool hasPlan() const { return hasPlan_; }
  const DevicePlan& plan() const { return plan_; }

  // ---- production block render (the ONE delegate the host calls) ----
  // Streaming, per frame, `planar device input -> RuntimeInputs -> SynthRuntime::processFrame
  // -> RuntimeOutput -> planar device output`, using the SAME frozen scale as the plan.
  // Framework-free `double* const*` per-channel buffers (iPlug2 `sample**`, `sample=double`);
  // no staging buffer, no heap, no lock, no log. The input route and the logical -> physical
  // output mapping are both carried by the plan; non-finite device samples are zeroed +
  // counted. Writes ONLY the plan's opened channels; every other physical output channel is
  // left untouched. NOT const: the input direction owns the non-finite anomaly counter.
  void renderBlock(SynthRuntime& rt, const double* const* planarIn, double* const* planarOut,
                   int frames);

  // Diagnostic: number of non-finite device samples seen in the input direction.
  std::uint64_t nonFiniteSamples() const { return nonFinite_; }

 private:
  // The ONE per-frame input primitive (shared; renderBlock is its only caller). Reads the
  // plan's input channels from a planar double block at `frame`, converts via the SAME
  // choke point as the output direction, and emits the RuntimeInputs for that frame.
  // Non-finite samples -> 0 (counted). NOT const: owns the non-finite anomaly counter.
  void resolveInput_(const double* const* planarIn, int frame, RuntimeInputs& out);

  // The ONE per-frame output primitive (shared; renderBlock is its only caller). Writes a
  // RuntimeOutput (virtual volts) into the plan's opened channels of a planar double block
  // at `frame`, via the unified 0.5 scale, leaving every other physical output channel
  // untouched. Const: it never touches the anomaly counter.
  void writeOutput_(const RuntimeOutput& out, double* const* planarOut, int frame) const;

  DevicePlan plan_;
  bool hasPlan_ = false;
  // nonFinite_ is the ONLY mutable render-path field and is a plain monotonic counter
  // (no lock, no allocation). writeOutput_ never touches it.
  std::uint64_t nonFinite_ = 0;
};

// ---- DevicePlan::valid() -------------------------------------------------------------
inline bool DevicePlan::valid() const {
  // Fail-closed on an impossible standalone capability BEFORE any route logic.
  if (inputCapability < 0) return false;
  // Output capability gates first (fail-closed).
  if (!layout.valid() || outputCapability < 2) return false;
  if (outputCapability != layout.totalChannels) return false;
  // Frozen: a 2-3 channel device writes EXACTLY WET (outputCount==2); a >=4 channel
  // device writes EXACTLY the user-chosen 4-way WET+DRY (outputCount==4). A >=4 device
  // may NOT report a 2-channel WET-only half-success, and a 2-3 device can NEVER reach a
  // 4-way (there are not enough distinct in-range channels).
  if (outputCapability <= 3) {
    if (outputCount != 2) return false;
    // capability 2-3 device: exactly WET L/R, distinct, in-range.
    const int l = output.channel[WET_L];
    const int r = output.channel[WET_R];
    if (l < 0 || l >= outputCapability) return false;
    if (r < 0 || r >= outputCapability) return false;
    if (l == r) return false;
  } else {
    // capability >=4 device: write EXACTLY the 4-way WET+DRY (outputCount==4). All four
    // logicals -> four DISTINCT in-range channels. DRY_A / DRY_B are real here, so a
    // silent DRY drop cannot be represented as a "4-way success".
    if (outputCount != 4) return false;
    int seen[4] = {-1, -1, -1, -1};
    for (int o = 0; o < 4; ++o) {
      const int c = output.channel[o];
      if (c < 0 || c >= outputCapability) return false;
      for (int p = 0; p < o; ++p)
        if (seen[p] == c) return false;
      seen[o] = c;
    }
  }

  // Frozen input-route validity.
  switch (input) {
    case InputRoute::Zero:
      return inputCh[0] == -1 && inputCh[1] == -1;
    case InputRoute::ExtOnly:
      return inputCh[0] >= 0 && inputCh[0] < inputCapability && inputCh[1] == -1;
    case InputRoute::PreampOnly:
      return inputCh[1] >= 0 && inputCh[1] < inputCapability && inputCh[0] == -1;
    case InputRoute::DuplicateOne:
      return inputCh[0] >= 0 && inputCh[0] < inputCapability && inputCh[1] == inputCh[0];
    case InputRoute::Distinct:
      return inputCh[0] >= 0 && inputCh[0] < inputCapability &&
             inputCh[1] >= 0 && inputCh[1] < inputCapability && inputCh[0] != inputCh[1];
    case InputRoute::DualSame:
      return inputCh[0] >= 0 && inputCh[0] < inputCapability && inputCh[1] == inputCh[0];
  }
  return false;
}

// ---- DeviceAdapter::prepare -----------------------------------------------------------
inline bool DeviceAdapter::prepare(const DeviceLayout& layout, int inputCapability,
                                   const OutputMapping& output, int outputCount,
                                   InputRoute input, int extCh, int preampCh) {
  DevicePlan cand;
  cand.layout = layout;
  cand.outputCapability = layout.totalChannels;
  cand.outputCount = outputCount;
  cand.output = output;
  cand.inputCapability = inputCapability;
  cand.input = input;
  cand.inputCh[0] = extCh;
  cand.inputCh[1] = preampCh;
  if (!cand.valid()) return false;  // fail-closed: keep previous plan (or none).
  plan_ = cand;
  hasPlan_ = true;
  return true;
}

// ---- DeviceAdapter::resolveInput_ -----------------------------------------------------
inline void DeviceAdapter::resolveInput_(const double* const* planarIn, int frame,
                                         RuntimeInputs& out) {
  out = RuntimeInputs{0.0, 0.0};
  if (planarIn == nullptr) return;  // no device input => both terminals read 0.
  const DevicePlan& p = plan_;
  // EXT terminal -> extAudio (mixer ch4 route).
  if (p.inputCh[0] >= 0 && p.inputCh[0] < p.inputCapability) {
    const double n = planarIn[p.inputCh[0]][frame];
    if (!std::isfinite(n)) { ++nonFinite_; out.extAudio = 0.0; }
    else out.extAudio = volts_from_device_normalized(n);
  }
  // PREAMP terminal -> preamp (the preamp.ext_source_in fallback).
  if (p.inputCh[1] >= 0 && p.inputCh[1] < p.inputCapability) {
    const double n = planarIn[p.inputCh[1]][frame];
    if (!std::isfinite(n)) { ++nonFinite_; out.preamp = 0.0; }
    else out.preamp = volts_from_device_normalized(n);
  }
}

// ---- DeviceAdapter::writeOutput_ ------------------------------------------------------
inline void DeviceAdapter::writeOutput_(const RuntimeOutput& out, double* const* planarOut,
                                        int frame) const {
  if (planarOut == nullptr) return;  // no device output => nothing to write.
  const DevicePlan& p = plan_;
  const int count = (p.outputCount == 2) ? 2 : 4;  // 2 => WET only; 4 => WET+DRY.
  for (int o = 0; o < count; ++o) {
    const double v = (o == WET_L) ? out.wetL : (o == WET_R) ? out.wetR
                     : (o == DRY_A) ? out.dryA : out.dryB;
    const int ch = p.output.channel[o];
    if (ch < 0 || ch >= p.outputCapability) continue;  // defensive; plan already validated.
    planarOut[ch][frame] = device_normalized_from_volts(v);
  }
}

// ---- DeviceAdapter::renderBlock -------------------------------------------------------
inline void DeviceAdapter::renderBlock(SynthRuntime& rt, const double* const* planarIn,
                                       double* const* planarOut, int frames) {
  if (!hasPlan_) return;
  for (int f = 0; f < frames; ++f) {
    RuntimeInputs in{0.0, 0.0};
    resolveInput_(planarIn, f, in);                     // input direction (single primitive).
    const RuntimeOutput out = rt.processFrame(in, true);  // drive the product runtime.
    writeOutput_(out, planarOut, f);                    // output direction (single primitive).
  }
}

}  // namespace lunar24::core
