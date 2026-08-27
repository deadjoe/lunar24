// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SynthRuntime — #38 (GH#4 A03): the framework-free MACHINE RUNTIME that the
// product (the host's realtime audio callback) actually drives. It is the ONE
// execution surface in the product path that consumes a compile_graph() plan —
// the hard rule from the acceptance (msg 43da88a5): the criteria MUST be run
// against this runtime, never a test-internal second executor.
//
// ARCHITECTURE — two layers, mirroring the split the P2-③/P3-⑤ design already
// drew (design/07 §4, §5; design/01 §1):
//
//   * CONTROL graph (the PATCHABLE layer, `compile_graph`). The CV patch bay:
//     which module output (keyboard V/OCT, envelope, LFO, joystick, sequencer,
//     drone CV, ext) patches into which module input (VCO v_oct/cv, VCF cv_l/
//     cv_r, preamp, drone cv+gate). This is the graph the runtime genuinely
//     COMPILES (rebuild()) and whose resolved CV it applies to the voice sources.
//   * VOICE chain (the FIXED layer, `SignalPath`). VCO A/B + drone + preamp
//     sources -> VoiceMixer (10 ch) -> PolivoksFilter -> Distortion -> WET L/R,
//     with DRY A/B tapped at VCO A/B pre-chain. The mixer/VCF/dist are INTERNAL
//     endpoints (not patchable jacks — registry explicit), so they never enter
//     compile_graph's edge set; this runtime owns that fixed chain directly.
//
// CV RESOLUTION (the executor). rebuild() enumerates the effective edges and
// compiles them. Each Frame with driveGraph=true resolves every bound voice-input
// jack from the value at the source jack of its incoming edge (a control generator
// writes its output there — in the product that is a control module's DSP; tests
// set it directly), and applies it to the corresponding voice-source parameter.
// driveGraph=false is the NEGATIVE control for criterion ①: the compiled graph is
// ignored, so no CV reaches a voice source and a patched modulation does not occur.
//
// The voice-input jack -> parameter mapping (which patch jack is "VCO B v_oct" vs
// "VCF cv_l", ...) is a REGISTRY semantic, not a compile_graph fact. The runtime
// holds it as an explicit binding table the host fills from the registry; it is
// deliberately NOT hardcoded to numeric ids here. (The drone CLASSIC/NEW split is
// design/01 §3-CONFIRMED — see aggregateDrone_; the full control-module executor
// is the standing P3/P4 work, separate from this #38 runtime.)
//
// RT CONTRACT (criterion ⑤): processFrame()/processBlock() allocate nothing, take
// no lock, touch no file/log. All state is preallocated; vectors live only in
// CompiledGraph, which rebuild() builds OFF the audio thread and the audio path
// merely reads.
//
// DETERMINISM (criterion ④): no randomness in the audio path. A given seed +
// identical source inputs + identical sample rate yields bit-identical output;
// because the DSP is per-frame, any block partition reproduces the same sequence.
//
// SAMPLE RATE — a constructor parameter, because the voice sources pin it at
// creation (Vco/Preamp/DroneBank carry a fixed sr), exactly as the real product is
// instantiated once for the device's configured rate. The host passes the target sr
// at construction; the 44.1/48/88.2/96k acceptance builds one runtime per rate.

#pragma once

#include <cstddef>
#include <cstdint>

#include <lunar24/core/distortion.h>
#include <lunar24/core/drone_bank.h>
#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/polivoks_vcf.h>
#include <lunar24/core/preamp.h>
#include <lunar24/core/vco.h>
#include <lunar24/core/voice_mixer.h>
#include <lunar24/core/wet_dry.h>

namespace lunar24::core {

// The four logic outputs of one rendered frame (WET L/R + DRY A/B).
struct RuntimeOutput {
  double wetL = 0.0;  // chain: mixer -> vcf -> distortion.
  double wetR = 0.0;
  double dryA = 0.0;  // VCO A, tapped BEFORE the chain.
  double dryB = 0.0;  // VCO B, tapped BEFORE the chain.
};

class SynthRuntime {
 public:
  static constexpr int kNumChannels = VoiceMixer::kNumChannels;  // 10
  static constexpr int kNumOutputs = 4;

  // Fixed upper bounds for the framework-free state. A graph that exceeds these is
  // rejected at rebuild() (never silently truncated). Generous for the machine
  // patch space.
  static constexpr std::uint32_t kMaxEdges = 128;

  // Build the runtime for the machine's descriptor tables (the registry in
  // production; synthetic tables in tests) at the device's sample rate. `modules`
  // supplies each compiled module's prepared scheduling contract (as compile_graph
  // expects). The voice layer is seeded from `seed` (determinism, criterion ④).
  SynthRuntime(const JackDescriptor* jacks, std::uint32_t jackCount,
               const NormalizedRoute* routes, std::uint32_t routeCount,
               const GraphModule* modules, std::uint32_t moduleCount,
               std::uint64_t seed, double sampleRate = 48000.0)
      : jacks_(jacks),
        jackCount_(jackCount),
        modules_(modules),
        moduleCount_(moduleCount),
        seed_(seed),
        patch_(jacks, jackCount, routes, routeCount),
        vcA_(sampleRate),
        vcB_(sampleRate),
        preamp_(sampleRate),
        drone_(seed_, sampleRate) {
    chain_.setSampleRate(sampleRate);
  }

  // Voice source base frequency (the f0 the VCO chain references). Applied to BOTH
  // VCOs; a patched v_oct/cv adds on top.
  void setVcoBaseHz(double hz) {
    vcA_.setBaseHz(hz);
    vcB_.setBaseHz(hz);
  }

  // VOICE-INPUT BINDINGS (registry semantics, provided by the host). Tell the
  // runtime which patch jacks control which voice parameter. Unbound jacks are
  // ignored by the CV resolver.
  void setVoctBindings(JackId aVoct, JackId bVoct) { voctA_ = aVoct; voctB_ = bVoct; }
  void setVcfCvBindings(JackId cvL, JackId cvR) { vcfCvL_ = cvL; vcfCvR_ = cvR; }

  // A control generator's resolved CV output at `jack` (a control module writes
  // its output jack here each frame; the test provides it directly). The runtime
  // reads this to resolve voice inputs patched from `jack`.
  void setControlVoltage(JackId jack, double volts) {
    const std::uint32_t j = static_cast<std::uint32_t>(jack);
    if (j < kMaxEdges) cvOut_[j] = volts;
  }

  // Patch-graph mutation (criterion ②). Each mutation marks the plan stale; the
  // NEXT Process* rebuilds it.
  bool connect(JackId source, JackId sink) {
    if (patch_.connect(source, sink)) { graphDirty_ = true; return true; }
    return false;
  }
  bool disconnect(JackId source, JackId sink) {
    if (patch_.disconnect(source, sink)) { graphDirty_ = true; return true; }
    return false;
  }

  // Off-audio-thread build: recompute the effective edges and recompile the
  // control graph (criterion ① — the product path that consumes compile_graph).
  // Returns false if the graph is rejected (cycle_unsafe_module) or exceeds a
  // fixed capacity. Must not run on the audio thread.
  bool rebuild() {
    if (!graphDirty_ && graphValid_) return true;
    edgeCount_ = patch_.effectiveEdges(edges_, kMaxEdges);
    if (edgeCount_ >= kMaxEdges) { graphValid_ = false; return false; }
    CompileResult r = compile_graph(jacks_, jackCount_, edges_, edgeCount_,
                                    modules_, moduleCount_);
    if (r.status != CompileStatus::ok) { graphValid_ = false; return false; }
    graph_ = r.graph;
    graphValid_ = true;
    graphDirty_ = false;
    return true;
  }

  // The product-path render. `extSource` is the EXT.AUDIO voltage (the one
  // patchable audio input); it drives mixer ch4 and the preamp (ch7). `driveGraph`
  // selects the CONTROL layer: true = run the compiled graph (product path),
  // false = IGNORE it (the NEGATIVE control for criterion ① — a patched CV is not
  // applied). Rendering is independent of block partition.
  RuntimeOutput processFrame(double extSource, bool driveGraph = true) {
    if (driveGraph) applyControlCv_();
    double a = 0.0, b = 0.0;
    vcA_.tick(&a);
    vcB_.tick(&b);
    // Fixed chain input frame (10 ch, panel channel order).
    double chIn[kNumChannels] = {};
    chIn[VoiceMixer::kChannelVcoA] = a;
    chIn[VoiceMixer::kChannelVcoB] = b;
    // Drone: design/01 §3 CLASSIC/NEW split — voices 1/2/4/5 each carry 5 of the
    // bank's 20 oscillators; voices 3/6 are NEW (not in this bank) and stay silent.
    double drone[DroneBank::kMaxVoices] = {};
    drone_.tick(drone);
    aggregateDrone_(drone, chIn);
    // Preamp / ext audio.
    chIn[VoiceMixer::kChannelExtAudio] = extSource;
    chIn[VoiceMixer::kChannelPreamp] = preamp_.tick(extSource);
    WetDryOutput o = chain_.process(chIn);
    return RuntimeOutput{o.wetL, o.wetR, o.dryA, o.dryB};
  }

  // Block render: advance `n` frames. The per-frame DSP makes this equal a
  // per-block run of the same sequence (partition-invariance, criterion ④).
  void processBlock(const double* extSource, std::size_t n, RuntimeOutput* out,
                    bool driveGraph = true) {
    for (std::size_t i = 0; i < n; ++i) out[i] = processFrame(extSource[i], driveGraph);
  }

  // Inspectors (read-only product/diagnostic surface).
  bool graphValid() const { return graphValid_; }
  std::uint32_t graphModuleCount() const { return graph_.moduleCount; }
  const CompiledGraph& graph() const { return graph_; }
  std::uint32_t edgeCount() const { return edgeCount_; }

 private:
  void applyControlCv_() {
    // Resolve each bound voice-input jack from the source jack of its incoming
    // effective edge (a control generator's output written via setControlVoltage).
    for (std::uint32_t i = 0; i < edgeCount_; ++i) {
      const JackId src = edges_[i].source;
      const JackId snk = edges_[i].sink;
      if (snk == voctA_) {
        vcA_.setVoct(cvAt_(src));
      } else if (snk == voctB_) {
        vcB_.setVoct(cvAt_(src));
      } else if (snk == vcfCvL_) {
        chain_.vcf().setCvL(cvAt_(src));
      } else if (snk == vcfCvR_) {
        chain_.vcf().setCvR(cvAt_(src));
      }
    }
  }

  double cvAt_(JackId jack) const {
    const std::uint32_t j = static_cast<std::uint32_t>(jack);
    return j < kMaxEdges ? cvOut_[j] : 0.0;
  }

  // Drone grouping — design/01 §3 (CONFIRMED, not provisional): six drone voices,
  // 1/2/4/5 = "CLASSIC" (5 oscillators each, i.e. the DroneBank's 20 voices),
  // 3/6 = "NEW" (Papa Srapa noise, P3-②, NOT part of the DroneBank). The 20 flat
  // bank oscillators split 5-per-CLASSIC-voice. The bank-index -> channel ordering
  // is the bank's own layout (its voices are unlabeled, so this is a deterministic
  // index convention ascending by channel), NOT a manual fact; the DESIGN-CONFIRMED
  // claim here is the CLASSIC/NEW split, not the within-bank ordering.
  static void aggregateDrone_(const double* drone, double* chIn) {
    // drone[] holds voiceCount_ (= kMaxVoices=20) samples for the seeded bank.
    // DRONE1 <- voices 0..4, DRONE2 <- 5..9, DRONE4 <- 10..14, DRONE5 <- 15..19.
    const int classicChannels[5] = {VoiceMixer::kChannelDrone1, VoiceMixer::kChannelDrone2,
                                    VoiceMixer::kChannelDrone4, VoiceMixer::kChannelDrone5};
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      const int begin = c * 5;
      const int end = begin + 5;
      for (int v = begin; v < end; ++v) s += drone[v];
      chIn[classicChannels[c]] = s;
    }
    // NEW drone voices (3 and 6) are not sourced from the DroneBank — leave their
    // mixer channels silent (a later P3-② NEW voice implementation drives them).
    chIn[VoiceMixer::kChannelDrone3] = 0.0;
    chIn[VoiceMixer::kChannelDrone6] = 0.0;
  }

  const JackDescriptor* jacks_ = nullptr;
  std::uint32_t jackCount_ = 0;
  const GraphModule* modules_ = nullptr;
  std::uint32_t moduleCount_ = 0;
  std::uint64_t seed_ = 0;

  PatchGraph patch_;
  CompiledGraph graph_;
  bool graphValid_ = false;
  bool graphDirty_ = false;

  // Effective-edge + CV state (framework-free fixed capacity).
  PatchEdge edges_[kMaxEdges];
  std::uint32_t edgeCount_ = 0;
  double cvOut_[kMaxEdges] = {};

  // Voice-input bindings (registry semantics). Unbound = JackId{0} sentinel.
  JackId voctA_{0};
  JackId voctB_{0};
  JackId vcfCvL_{0};
  JackId vcfCvR_{0};

  // Voice sources + fixed chain.
  Vco vcA_;
  Vco vcB_;
  Preamp preamp_;
  DroneBank drone_;
  SignalPath chain_;
};

}  // namespace lunar24::core
