// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// SynthRuntime — #38 (GH#4 A03): the framework-free MACHINE RUNTIME that the
// product (the host's realtime audio callback) actually drives. It is the ONE
// execution surface in the product path that consumes a compile_graph() plan —
// the hard rule from the acceptance (msg 43da88a5): the criteria MUST be run
// against this runtime, never a test-internal second executor.
//
// ARCHITECTURE — @Claude ruling 2 (option B): the switch layer is GONE. There is
// no separate "voice-chain order" and no SignalPath hard-code. The pluggable
// (JackId) control patches and the FIXED internal routes (design/07 §4 Decision B:
// `module.port` identity, no JackId) are merged into ONE compile plan by
// compile_graph() in rebuild(). The runtime derives its render order and its
// cycle-breaking from THAT single plan:
//
//   * Ordering. rebuild() flattens the plan's regions (which the compiler
//     topological-sorts across the merged pluggable+fixed graph) into the ordered
//     fixed-chain role sequence the render dispatches. The mixer/vcf/dist run when
//     and where the plan says — not in a hard-coded fixed order.
//   * Cycle-breaking. A cycle that crosses a fixed leg (preamp -> env_follower) +
//     a pluggable cable (env_follower.env_out -> preamp.ext_source_in) is broken on
//     the PLUGGABLE edge (it has a JackId + a contract path). The compiler picks
//     the break; the runtime feeds the value through a per-edge delay line of
//     depth `delaySamples` (the consume-rule of graph_compiler.h; off-by-one vs.
//     reading the source's last output is exactly the defect this discriminates).
//
// FIXED-CHAIN ROLES. The runtime does not hard-code which module is the mixer or
// the VCF: the host binds ModuleId -> FixedChainRole (registry semantic) and the
// plan's module membership decides which roles are actually present. UNBOUND
// modules are skipped (they are control-only modules the plan may still carry).
// This is a binding table, not numeric ids — see setVoctBindings()'s comment.
//
// CV RESOLUTION (unchanged from ruling 1). rebuild() compiles the effective edges.
// Each Frame with driveGraph=true resolves every bound CONTROL voice-input jack
// (VCO v_oct, VCF cv) from the value at the source jack of its incoming edge.
// driveGraph=false is the NEGATIVE control for criterion ①.
//
// The voice-input jack -> parameter mapping IS a registry semantic, held as an
// explicit binding table. The drone CLASSIC/NEW split is design/01 §3-CONFIRMED.
//
// RT CONTRACT (criterion ⑤): processFrame()/processBlock() allocate nothing, take
// no lock, touch no file/log. All state is preallocated; the plan is built by
// rebuild() OFF the audio thread and the audio path only reads the derived
// chainExecOrder_/feedback_ tables.
//
// DETERMINISM (criterion ④): no randomness in the audio path. The per-frame DSP
// makes any block partition reproduce the same sequence.
//
// SAMPLE RATE — a constructor parameter, because the voice sources pin it at
// creation (Vco/Preamp/DroneBank/EnvFollower/Distortion carry a fixed sr).
//
// FINDINGS — FIXED-ROUTE INJECTION/OMISSION LEDGER (requiredFixedRoutes, 28 total).
// The design (design/00-status.md §"所有省略逐条登记") requires every route be
// classified by whether #38 loads it or defers it, and by which later slice picks it
// up. The runtime consumes ONLY module->module dependency routes for the plan; the
// two DRY taps are terminal outputs the runtime resolves from the VCO roles, and WET
// is the distortion output directly (the effector is out-of-P6, see below).
//
//   INJECTED (17, carried by this slice):
//     drone{1..6}_to_mixer, ext_audio_to_mixer, vco_a_to_mixer, vco_b_to_mixer,
//     preamp_to_mixer, mixer_to_vcf_l, mixer_to_vcf_r, vcf_l_to_dist_l,
//     vcf_r_to_dist_r, preamp_to_env_follower            (15 module->module plan edges)
//     vco_a_to_dry_a, vco_b_to_dry_b                     (2 DRY A/B terminal taps)
//
//   OMITTED (11, deferred; NOT wired here):
//     dist_l_to_eff_l, dist_r_to_eff_r, eff_l_to_wet_l, eff_r_to_wet_r
//       (4 effector routes — P6 out of scope. DECLARED OMISSION, asserted absent by
//        criterion ⑥(d). WET is therefore the distortion output, never a post-effector
//        tap — a future P6 patch must replace this and the WET criterion together.)
//     piezzo_to_preamp                                  (1 later slice — piezzo not here)
//     voice{1..6}_gate_to_drone{1..6}                    (6 keyboard/voice slice)
//
//   #38 wires NO effector. The 4 effector routes are the only ones whose absence is a
//   contract, not a staging gap: the acceptance asserts WET L/R come straight from the
//   distortion while the eff chain is absent, so re-adding them is a P6 task, not a
//   bug in the fixed chain.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <lunar24/core/control_event.h>
#include <lunar24/core/distortion.h>
#include <lunar24/core/drone_bank.h>
#include <lunar24/core/drone_noise.h>
#include <lunar24/core/envelope_follower.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/fm_am.h>
#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/polivoks_vcf.h>
#include <lunar24/core/preamp.h>
#include <lunar24/core/sample_hold.h>
#include <lunar24/core/schmitt_osc.h>
#include <lunar24/core/vco.h>
#include <lunar24/core/voice_mixer.h>
#include <lunar24/registry_ids.hpp>

namespace lunar24::core {

// The four logic outputs of one rendered frame (WET L/R + DRY A/B).
struct RuntimeOutput {
  double wetL = 0.0;  // chain: mixer -> vcf -> distortion.
  double wetR = 0.0;
  double dryA = 0.0;  // VCO A, tapped BEFORE the chain.
  double dryB = 0.0;  // VCO B, tapped BEFORE the chain.
};

// The role a fixed-chain module plays in the render. A module bound to kNone (or
// left unbound) does not participate in the chain — it is a control-only module the
// plan may still carry for the patch graph. Host binds via bindFixedRole().
enum class FixedChainRole : std::uint8_t {
  kNone = 0,
  kVcoA,       // VCO A oscillator -> DRY A tap + mixer ch5.
  kVcoB,       // VCO B oscillator -> DRY B tap + mixer ch6.
  kDrone,      // DroneBank -> mixer ch1/2/4/5 (CLASSIC/NEW split).
  kExtIn,      // EXT.AUDIO terminal -> mixer ch4.
  kPreamp,     // preamp (reads ext_source_in break ring or the ext terminal) -> ch7.
  kEnvFollower,// envelope follower; consumes preamp output, feeds env_out (cycle leg).
  kMixer,      // 10ch VoiceMixer -> L/R.
  kVcf,        // PolivoksFilter L/R.
  kDistortion, // post-filter Distortion L/R -> WET.
};

class SynthRuntime {
 public:
  static constexpr int kNumChannels = VoiceMixer::kNumChannels;  // 10
  static constexpr int kNumOutputs = 4;

  // Fixed upper bounds for the framework-free state. A graph that exceeds these is
  // rejected at rebuild() (never silently truncated). Generous for the machine
  // patch space.
  static constexpr std::uint32_t kMaxEdges = 128;
  static constexpr std::uint32_t kMaxFixedModules = 32;
  static constexpr std::uint32_t kMaxFeedback = 16;
  static constexpr std::uint32_t kMaxFeedbackDelay = 1024;
  // Classic drone voices in the bank (drone 1/2/4/5), each a 5-generator group.
  static constexpr int kClassicDroneVoices = DroneBank::kClassicVoices;
  // NEW drone voice 6 (Papa Srapa NoiseSource) amplitude. PROVISIONAL: the noise
  // level is not in the manual; exposed so the product path can bind a NOISE knob
  // and a test can compare the executed channel to a same-seed NoiseSource.
  static constexpr double kNewDroneNoiseAmp = 0.5;
  // NEW-voice (Papa Srapa) FM/AM modulation amounts, taken from the FmAmVoice that
  // expresses the LF-square->audio modulation relationship. PROVISIONAL: the manual
  // gives no depth, these are seed/circuit constants to be tuned on sound.
  static constexpr double kNewDroneFmDev = 120.0;   // FM peak deviation (Hz).
  static constexpr double kNewDroneDepth = 0.5;     // AM index [0,1).
  // NEW-voice S&H hold period (seconds) — the clocked form the product path uses
  // actually takes the speed from the external clock; this is the standalone
  // self-timed fallback. PROVISIONAL.
  static constexpr double kNewDroneShSeconds = 0.05;
  // LF square modulator default frequency (RATE). PROVISIONAL (RATE range not in
  // the manual); the RATE control overrides it.
  static constexpr double kNewDroneLfFreqHz = 6.0;
  // Voice-6 (drone 6) seed mix, so the two Papa Srapa voices are independent.
  static constexpr std::uint64_t kNewSndSeed6Xor = 0x9E3779B97F4A7C15ULL;
  // Per-source sub-seed mix so the audio/LF/FmAm/noise stems within one voice are
  // independent draws from the shared voice seed.
  static constexpr std::uint64_t kNewSourceSeedMix = 0x2595DB9F3D276D2BULL;
  // PITCH-position -> semitone mapping (0 = tone off / pure-noise recipe, 1 = max
  // pitch). PROVISIONAL (pitch range = C0..E7 per manual, not numerically bound).
  static constexpr double kNewPitchMinSt = 0.0;
  static constexpr double kNewPitchMaxSt = 24.0;
  // Source indices for newVoiceSeed/newSourceSeed derivation.
  static constexpr int kNewSrcAudio = 0;
  static constexpr int kNewSrcLf = 1;
  static constexpr int kNewSrcFm = 2;
  static constexpr int kNewSrcNoise = 3;

  // Deterministic, documented sub-seed derivation for the NEW (Papa Srapa) voices so
  // a test oracle can construct the SAME standalone modules. Voice 0 = drone 3,
  // voice 1 = drone 6; source kNewSrcAudio/Lf/Fm/Noise selects the stem. A fixed seed
  // is reproducible; the two voices are independent (different root), and the four
  // stems within a voice draw distinct randomness.
  static std::uint64_t newVoiceSeed(std::uint64_t root, int voice) {
    return voice == 1 ? (root ^ kNewSndSeed6Xor) : root;
  }
  static std::uint64_t newSourceSeed(std::uint64_t voiceSeed, int source) {
    return voiceSeed ^ (kNewSourceSeedMix * static_cast<std::uint64_t>(source + 1));
  }

  // A single break-edge delay line. The SOURCE module writes the loop-forward value
  // (e.g. env_follower's env_out), the CONSUMING module reads the value from
  // `delaySamples` frames ago via the circular buffer (graph_compiler.h consume-rule).
  struct FeedbackLine {
    JackId sourceJack = JackId{0};
    JackId sinkJack = JackId{0};
    double delaySamples = 1.0;
    double buf[kMaxFeedbackDelay] = {};
    std::uint32_t writePos = 0;
    bool active = false;
  };

  // Build the runtime for the machine's descriptor tables (the registry in
  // production; synthetic tables in tests) at the device's sample rate. `modules`
  // supplies each compiled module's prepared scheduling contract (as compile_graph
  // expects). `fixedEdges` carries the fixed internal routes merged into the plan.
  // The voice layer is seeded from `seed` (determinism, criterion ④).
  SynthRuntime(const JackDescriptor* jacks, std::uint32_t jackCount,
               const NormalizedRoute* routes, std::uint32_t routeCount,
               const GraphModule* modules, std::uint32_t moduleCount,
               std::uint64_t seed, double sampleRate = 48000.0,
               const FixedEdge* fixedEdges = nullptr,
               std::uint32_t fixedEdgeCount = 0)
      : jacks_(jacks),
        jackCount_(jackCount),
        modules_(modules),
        moduleCount_(moduleCount),
        fixedEdges_(fixedEdges),
        fixedEdgeCount_(fixedEdgeCount),
        seed_(seed),
        patch_(jacks, jackCount, routes, routeCount),
        vcA_(sampleRate),
        vcB_(sampleRate),
        preamp_(sampleRate),
        drone_(seed_, sampleRate),
        pv3_(newVoiceSeed(seed_, 0), sampleRate),  // NEW drone 3 (Papa Srapa voice).
        pv6_(newVoiceSeed(seed_, 1), sampleRate),  // NEW drone 6 (Papa Srapa voice).
        envFol_(sampleRate),
        distortion_(sampleRate) {
    vcf_.setSampleRate(sampleRate);
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
  // Which patch jack is the preamp's external audio input (the break sink of the
  // env_follower cycle) and which is the env_follower's env_out (the break source).
  void setPreampExtIn(JackId j) { preampExtIn_ = j; }
  void setEnvFolOut(JackId j) { envFolOut_ = j; }

  // FIXED-CHAIN ROLE BINDING (registry semantic). Maps a compiled module id to the
  // fixed-chain role it plays. Unbound modules are skipped by the chain render.
  void bindFixedRole(ModuleId id, FixedChainRole role) {
    for (std::uint32_t i = 0; i < roleBindingCount_; ++i) {
      if (roleBindings_[i].id == id) { roleBindings_[i].role = role; return; }
    }
    if (roleBindingCount_ < kMaxFixedModules) {
      roleBindings_[roleBindingCount_].id = id;
      roleBindings_[roleBindingCount_].role = role;
      ++roleBindingCount_;
    }
  }

  // A control generator's resolved CV output at `jack` (a control module writes
  // its output jack here each frame; the test provides it directly). The runtime
  // reads this to resolve voice inputs patched from `jack`.
  void setControlVoltage(JackId jack, double volts) {
    const std::uint32_t j = static_cast<std::uint32_t>(jack);
    if (j < kMaxEdges) cvOut_[j] = volts;
  }

  // #46 admission point for timed control events. A ControlEvent enters core here
  // keyed by its ABSOLUTE sample; EventTimebase resolves the block-relative
  // sampleOffset at render time. This is the ONE time-based event path the runtime
  // carries — it CONSUMES the canonical EventTimebase rather than re-sorting events
  // at the host boundary (no second executor / off-patch ordering, the P2-③
  // "real_path" repair). Returns false only if the fixed event queue is full (the
  // audio thread is never blocked — design/07 §5).
  bool enqueueControlEvent(const TimedControlEvent& e) { return eventTimebase_.enqueue(e); }

  // DRONE panel controls (#39 panel-binding half): knob -> bank. `voiceGroup` is
  // 0..3 (classic drone voices 1/2/4/5), `gen` is 0..4. The runtime owns the
  // classic-grouping mapping (bank voice index = voiceGroup*5 + gen); a shared VOLT
  // applies to all 5 generators of the group. These forward directly to the bank —
  // the executed product path reads them every frame. Out-of-range is a no-op.
  void setDroneMute(int voiceGroup, int gen, bool on) {
    if (inDroneRange_(voiceGroup, gen)) drone_.setMute(flatGen_(voiceGroup, gen), on);
  }
  void setDroneTune(int voiceGroup, int gen, double semitones) {
    if (inDroneRange_(voiceGroup, gen)) drone_.setTune(flatGen_(voiceGroup, gen), semitones);
  }
  void setDroneMod(int voiceGroup, int gen, double amount) {
    if (inDroneRange_(voiceGroup, gen)) drone_.setMod(flatGen_(voiceGroup, gen), amount);
  }
  // The MOD external CV/photo detune input (modCv). MOD is audible as
  // modAmount*modCv, so a nonzero CV must be present for the amount knob to detune.
  void setDroneModCv(int voiceGroup, int gen, double cv) {
    if (inDroneRange_(voiceGroup, gen)) drone_.setModCv(flatGen_(voiceGroup, gen), cv);
  }
  void setDroneVolt(int voiceGroup, double semitonesDown) {
    if (voiceGroup >= 0 && voiceGroup < kClassicDroneVoices)
      drone_.setVolt(static_cast<std::size_t>(voiceGroup), semitonesDown);
  }

  // NEW drone voice controls (design/01 §3, #45 panel-binding half) — knob ->
  // source, one set per Papa Srapa voice (drone 3, drone 6). PITCH is a 0..1
  // position (0 = tone off => the "clean noise" recipe; 1 = max pitch). RATE drives
  // the LF square modulator; FM/AM are the two factory switches; NOISE is the mix
  // amount; SH_CLOCK drives the Sample & Hold clock (a constant = unclocked, so it
  // does not self-run). The rest (RANGE, MOD, DIVIDER, GATE/HOLD, ATT/RLS, env out)
  // have no dedicated runtime source yet and stay PROVISIONAL (see 00-status).
  // These forward directly to the NEW sources, read every frame by step_(kDrone).
  void setDrone3Pitch(double pct) { pv3_.setPitch(pct); }
  void setDrone3Rate(double hz) { pv3_.setRate(hz); }
  void setDrone3Fm(bool on) { pv3_.setFm(on); }
  void setDrone3Am(bool on) { pv3_.setAm(on); }
  void setDrone3Noise(double amp) { pv3_.setNoise(amp); }
  void setDrone3ShClock(double clk) { pv3_.setShClock(clk); }
  void setDrone6Pitch(double pct) { pv6_.setPitch(pct); }
  void setDrone6Rate(double hz) { pv6_.setRate(hz); }
  void setDrone6Fm(bool on) { pv6_.setFm(on); }
  void setDrone6Am(bool on) { pv6_.setAm(on); }
  void setDrone6Noise(double amp) { pv6_.setNoise(amp); }
  void setDrone6ShClock(double clk) { pv6_.setShClock(clk); }

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

  // Why rebuild() succeeded or refused. The bool return alone cannot distinguish
  // "feedback plan exceeded capacity" (GH#13) from "graph rejected" — this keeps a
  // fixed, no-log/no-alloc, inspectable reason.
  enum class RebuildStatus : std::uint8_t {
    ok,                          // plan published, runtime lines built
    graph_unchanged,             // cached, not dirty -> nothing recomputed
    edge_capacity,               // effective edges >= kMaxEdges
    compile_cycle_unsafe,        // compile_graph -> cycle_unsafe_module
    compile_invalid_contract,    // compile_graph -> invalid_module_contract
    feedback_capacity_exceeded,  // full compiled feedback plan > kMaxFeedback
  };

  // Off-audio-thread build: recompute the effective edges and recompile the
  // control graph (criterion ① — the product path that consumes compile_graph).
  // The fixed internal routes are passed in and merged into the SAME plan. Returns
  // false if the graph is rejected (cycle_unsafe_module / invalid_module_contract)
  // or exceeds a fixed capacity. GH#13: an over-capacity COMPILED feedback plan is
  // rejected here, BEFORE any graph_ / graphValid_ / runtime-line publishing, so it
  // is never silently truncated to kMaxFeedback; lastRebuildStatus() gives the
  // exact reason. Must not run on the audio thread.
  bool rebuild() {
    if (!graphDirty_ && graphValid_) { rebuildStatus_ = RebuildStatus::graph_unchanged; return true; }
    edgeCount_ = patch_.effectiveEdges(edges_, kMaxEdges);
    if (edgeCount_ >= kMaxEdges) {
      graphValid_ = false;
      rebuildStatus_ = RebuildStatus::edge_capacity;
      return false;
    }
    CompileResult r = compile_graph(jacks_, jackCount_, edges_, edgeCount_,
                                    modules_, moduleCount_, fixedEdges_, fixedEdgeCount_);
    if (r.status != CompileStatus::ok) {
      graphValid_ = false;
      rebuildStatus_ = r.status == CompileStatus::cycle_unsafe_module
                           ? RebuildStatus::compile_cycle_unsafe
                           : RebuildStatus::compile_invalid_contract;
      return false;
    }
    // GH#13 capacity preflight: the FULL compiled feedback plan must fit before we
    // publish anything. Rejecting here leaves graph_/feedback_ untouched, so no
    // partial "16/18" outcome can ever be reported as valid.
    if (countFeedback_(r.graph) > kMaxFeedback) {
      graphValid_ = false;
      rebuildStatus_ = RebuildStatus::feedback_capacity_exceeded;
      return false;
    }
    graph_ = r.graph;
    graphValid_ = true;
    graphDirty_ = false;
    rebuildStatus_ = RebuildStatus::ok;
    if (!rebuildChainExec_()) {
      // Provably unreachable given the preflight above, but fail closed: never leave
      // a half-built runtime line set behind a valid flag.
      graphValid_ = false;
      feedbackCount_ = 0;
      rebuildStatus_ = RebuildStatus::feedback_capacity_exceeded;
      return false;
    }
    return true;
  }

  // The product-path render. `extSource` is the EXT.AUDIO voltage (the one
  // patchable audio input); it drives mixer ch4 and (unless a cable feeds the
  // preamp's ext_source_in) the preamp. `driveGraph` selects the CONTROL layer:
  // true = run the compiled graph (product path), false = IGNORE it (the NEGATIVE
  // control for criterion ①). Rendering is independent of block partition.
  RuntimeOutput processFrame(double extSource, bool driveGraph = true) {
    if (driveGraph) applyControlCv_();
    extSource_ = extSource;
    for (int i = 0; i < kNumChannels; ++i) chIn_[i] = 0.0;
    for (std::uint32_t i = 0; i < chainExecCount_; ++i) step_(chainExecOrder_[i]);
    return RuntimeOutput{wetL_, wetR_, dryA_, dryB_};
  }

  // Block render: advance `n` frames. The per-frame DSP makes this equal a
  // per-block run of the same sequence (partition-invariance, criterion ④).
  //
  // #46: the block FIRST drains due control events from EventTimebase (each resolved
  // to a block-relative sampleOffset), then applies each at the exact frame it was
  // scheduled for, so the same event set acts at the SAME absolute sample under any
  // 64/128/256 (or mixed) block partition. Events are handed out sorted by absolute
  // sample; EventTimebase owns the ordering, this loop only matches offset -> frame.
  void processBlock(const double* extSource, std::size_t n, RuntimeOutput* out,
                    bool driveGraph = true) {
    const std::uint32_t nEvents =
        eventTimebase_.processBlock(static_cast<std::uint32_t>(n), blockEvents_,
                                    kEventDispatchCapacity);
    std::uint32_t ei = 0;
    for (std::size_t i = 0; i < n; ++i) {
      while (ei < nEvents && blockEvents_[ei].event.sampleOffset == i) {
        applyControlEvent_(blockEvents_[ei].event);
        ++ei;
      }
      out[i] = processFrame(extSource[i], driveGraph);
    }
  }

  // Inspectors (read-only product/diagnostic surface).
  bool graphValid() const { return graphValid_; }
  RebuildStatus lastRebuildStatus() const { return rebuildStatus_; }
  std::uint32_t graphModuleCount() const { return graph_.moduleCount; }
  const CompiledGraph& graph() const { return graph_; }
  std::uint32_t edgeCount() const { return edgeCount_; }
  // Derived fixed-chain order (from the plan) + feedback count, for tests/verify.
  std::uint32_t chainExecCount() const { return chainExecCount_; }
  FixedChainRole chainExecRoleAt(std::uint32_t i) const { return chainExecOrder_[i]; }
  std::uint32_t feedbackCount() const { return feedbackCount_; }
  const FeedbackLine& feedbackAt(std::uint32_t i) const { return feedback_[i]; }

  // Diagnostic: the drone channel the PRODUCT path computed for the last processed
  // frame and fed to the mixer (@Claude rule: "钉在真正被执行的那份数据上" — this is
  // the executed value, not a test-side re-derivation). `classicIndex` is 0..3,
  // mapping to VoiceMixer::kChannelDrone1/2/4/5 (the CLASSIC drone voices 1/2/4/5);
  // out-of-range returns 0. Read-only: the audio path already produces this value
  // each frame via the kDrone step.
  double droneChannel(int classicIndex) const {
    static constexpr int classic[4] = {VoiceMixer::kChannelDrone1, VoiceMixer::kChannelDrone2,
                                       VoiceMixer::kChannelDrone4, VoiceMixer::kChannelDrone5};
    if (classicIndex < 0 || classicIndex >= 4) return 0.0;
    return chIn_[classic[classicIndex]];
  }
  // NEW drone voices (design/01 §3): 3/6 are Papa Srapa, NOT in the DroneBank. Read
  // the EXECUTED channel value the mixer consumes — a same-seed standalone SchmittOsc
  // / NoiseSource is the oracle for what the product path produces.
  double drone3Channel() const { return chIn_[VoiceMixer::kChannelDrone3]; }
  double drone6Channel() const { return chIn_[VoiceMixer::kChannelDrone6]; }
  // S&H CV outputs (design/01 §3, #45). The Sample & Hold is NOT in the audio
  // channel — it is a CV source out of the voice (manual: "you will get -5 to +5
  // volts"). It feeds nothing in the fixed chain yet (no patch jack), so it is a
  // diagnostic read of the held level the product path computed last frame. An
  // unclocked S&H does not self-run (see sample_hold.h): a constant clock level
  // captures nothing, so these stay at the last held value.
  double sampleHold3Cv() const { return sh3Cv_; }
  double sampleHold6Cv() const { return sh6Cv_; }

 private:
  // NEW (Papa Srapa) composite voice — the @Claude-corrected topology (msg 3e21f284,
  // from manual L344-366): a voice has TWO Schmitt oscillators, not one. An LF Schmitt
  // is used as a SQUARE-WAVE modulator (RATE/RATE-SWITCH/CV OUT); an audio-frequency
  // Schmitt does the tone (PITCH/RANGE, C0-E7). FM and AM are SWITCHES (not a third
  // source): they route the LF square onto the audio oscillator in four combinations
  // (drone / FM / AM / FM+AM). Noise adds independently into the mixer channel. The
  // S&H runs noise->IN with the LF/mod source as its clock and yields a -5..+5 V CV
  // OUT of the voice (never summed into the audio channel); unclocked it does NOT
  // self-run. All four stems draw independent sub-seeds from the shared voice seed so
  // a test oracle can rebuild the same standalone modules.
  struct PapaVoice {
    PapaVoice(std::uint64_t voiceSeed, double sr)
        : audio(newSourceSeed(voiceSeed, kNewSrcAudio), sr),
          lf(newSourceSeed(voiceSeed, kNewSrcLf), sr),
          fm(newSourceSeed(voiceSeed, kNewSrcFm), sr, kNewDroneFmDev, kNewDroneDepth),
          noise(newSourceSeed(voiceSeed, kNewSrcNoise), kNewDroneNoiseAmp),
          sh(sr, kNewDroneShSeconds) {
      lf.setFreqHz(kNewDroneLfFreqHz);
    }
    void setPitch(double pct) {
      audio.setPitchSemitones(pct <= 0.0 ? SchmittOsc::kSilenceSt
                                         : kNewPitchMinSt + pct * (kNewPitchMaxSt - kNewPitchMinSt));
    }
    void setRate(double hz) { lf.setFreqHz(hz); }
    void setFm(bool on) { fmOn_ = on; }
    void setAm(bool on) { amOn_ = on; }
    void setNoise(double amp) { noise.setAmplitude(amp); }
    void setShClock(double clk) { shClock_ = clk; }
    // The S&H level the product path computed last frame (CV out of the voice).
    double lastShCv() const { return shCv_; }
    void tick(double* out) {
      double lv = 0.0;
      lf.tick(&lv);
      audio.setMod(lf.square());
      audio.setFmDevHz(fmOn_ ? fm.fDevHz() : 0.0);
      audio.setAmDepth(amOn_ ? fm.depth() : 0.0);
      double a = 0.0;
      audio.tick(&a);
      double n = 0.0;
      noise.tick(&n);
      sh.tick(n, shClock_, &shCv_);
      *out = a + n;  // noise adds; S&H CV is NOT summed here.
    }
    SchmittOsc audio;   // audio-frequency oscillator (PITCH/RANGE) -> tone.
    SchmittOsc lf;      // LF oscillator used as a square-wave modulator (RATE).
    FmAmVoice fm;       // expresses the LF->audio modulation relationship (FM/AM).
    NoiseSource noise;  // independent noise mix (NOISE amount).
    SAndHold sh;        // noise->in, LF/mod->clock; CV out, not in the audio channel.
    bool fmOn_ = false;
    bool amOn_ = false;
    double shClock_ = 0.0;
    double shCv_ = 0.0;
  };

  // #46 ControlEvent dispatch. Consumes a parameter ControlEvent that EventTimebase
  // resolved to a block-relative sampleOffset and applies it to the matching product
  // control at that sample. Only controls whose registry unit AGREES with the setter
  // unit are wired here (no invented conversion): drone_3/6 pitch (norm 0..1 -> pct
  // 0..1), noise (norm 0..1 -> amp 0..1), fm/am (selector 0/1 -> bool). Controls whose
  // registry unit differs from the setter (tune/volt/rate: norm -> semitones/Hz) would
  // need an UNEVIDENCED scale, so they are deliberately NOT wired here (FINDINGS §7) —
  // that is the separately-scheduled parameter-mapping work, not this dispatch pass.
  void applyControlEvent_(const ControlEvent& e) {
    if (e.kind != ControlEventKind::parameter) return;
    const double v = static_cast<double>(e.value);
    switch (e.parameter) {
      case ParameterId::drone_3_pitch: setDrone3Pitch(v); break;
      case ParameterId::drone_3_noise: setDrone3Noise(v); break;
      case ParameterId::drone_3_fm:    setDrone3Fm(v != 0.0); break;
      case ParameterId::drone_3_am:    setDrone3Am(v != 0.0); break;
      case ParameterId::drone_6_pitch: setDrone6Pitch(v); break;
      case ParameterId::drone_6_noise: setDrone6Noise(v); break;
      case ParameterId::drone_6_fm:    setDrone6Fm(v != 0.0); break;
      case ParameterId::drone_6_am:    setDrone6Am(v != 0.0); break;
      default: break;  // no unit-agreeing setter (or not a wired control): not dispatched.
    }
  }

  // Drone panel-control range/linearization helpers (the classic grouping).
  bool inDroneRange_(int voiceGroup, int gen) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices && gen >= 0 &&
           gen < static_cast<int>(DroneBank::kGensPerVoice);
  }
  std::size_t flatGen_(int voiceGroup, int gen) const {
    return static_cast<std::size_t>(voiceGroup) * DroneBank::kGensPerVoice +
           static_cast<std::size_t>(gen);
  }

  // Fixed-role -> id lookup (linear over the small binding table).
  FixedChainRole roleOf_(ModuleId id) const {
    for (std::uint32_t i = 0; i < roleBindingCount_; ++i)
      if (roleBindings_[i].id == id) return roleBindings_[i].role;
    return FixedChainRole::kNone;
  }
  int feedbackSinkIndex_(JackId sink) const {
    for (std::uint32_t i = 0; i < feedbackCount_; ++i)
      if (feedback_[i].active && feedback_[i].sinkJack == sink) return static_cast<int>(i);
    return -1;
  }
  int feedbackSourceIndex_(JackId src) const {
    for (std::uint32_t i = 0; i < feedbackCount_; ++i)
      if (feedback_[i].active && feedback_[i].sourceJack == src) return static_cast<int>(i);
    return -1;
  }
  std::uint32_t clampDelay_(double d) const {
    std::uint32_t n = d < 1.0 ? 1u : static_cast<std::uint32_t>(d + 0.999);
    return n > kMaxFeedbackDelay ? kMaxFeedbackDelay : n;
  }

  // GH#13: total feedback edges across the whole compiled plan (all cyclic
  // regions). Used as a capacity preflight in rebuild() so an over-capacity plan is
  // rejected BEFORE publishing — never silently truncated to kMaxFeedback.
  static std::uint32_t countFeedback_(const CompiledGraph& g) {
    std::uint32_t n = 0;
    for (const CompiledRegion& region : g.regions)
      if (region.kind == RegionKind::cyclic) n +=
          static_cast<std::uint32_t>(region.feedback.size());
    return n;
  }

  // Derive the RT-safe fixed-chain order + delay lines from the compiled plan.
  // Called only from rebuild() (off the audio thread); the audio path only reads.
  // Returns false (fail-closed) if the plan would exceed the fixed delay-line
  // storage — the plan is then NOT partially published, matching the capacity
  // preflight in rebuild(). The preflight guarantees this is never reached, but a
  // provably-no-truncation contract is a hard rule here, not a comment.
  bool rebuildChainExec_() {
    chainExecCount_ = 0;
    feedbackCount_ = 0;
    for (const CompiledRegion& region : graph_.regions) {
      for (ModuleId id : region.modules) {
        const FixedChainRole role = roleOf_(id);
        if (role != FixedChainRole::kNone && chainExecCount_ < kMaxFixedModules)
          chainExecOrder_[chainExecCount_++] = role;
      }
    }
    for (const CompiledRegion& region : graph_.regions) {
      if (region.kind != RegionKind::cyclic) continue;
      for (const CompiledFeedbackEdge& fe : region.feedback) {
        // GH#13: NEVER truncate. If the compiled plan does not fit, return false so
        // the caller can mark the rebuild failed instead of publishing a 16/18 lie.
        if (feedbackCount_ >= kMaxFeedback) return false;
        FeedbackLine& l = feedback_[feedbackCount_];
        l.sourceJack = fe.sourceJack;
        l.sinkJack = fe.sinkJack;
        l.delaySamples = fe.delaySamples;
        const std::uint32_t d = clampDelay_(fe.delaySamples);
        for (std::uint32_t k = 0; k < d; ++k) l.buf[k] = 0.0;
        l.writePos = 0;
        l.active = true;
        ++feedbackCount_;
      }
    }
    return true;
  }

  void applyControlCv_() {
    // Resolve each bound CONTROL voice-input jack from the source jack of its
    // incoming effective edge (a control generator's output written via
    // setControlVoltage). A voice-chain jack bound for the env_follower cycle is
    // NOT a control sink, so the cycle cable is never misapplied as a CV here.
    for (std::uint32_t i = 0; i < edgeCount_; ++i) {
      const JackId src = edges_[i].source;
      const JackId snk = edges_[i].sink;
      if (snk == voctA_) {
        vcA_.setVoct(cvAt_(src));
      } else if (snk == voctB_) {
        vcB_.setVoct(cvAt_(src));
      } else if (snk == vcfCvL_) {
        vcf_.setCvL(cvAt_(src));
      } else if (snk == vcfCvR_) {
        vcf_.setCvR(cvAt_(src));
      }
    }
  }

  // One fixed-chain step, dispatched in the plan's order. Kept as a switch over
  // the (small, fixed) role set so the render path is allocation-free and bounds
  // the latencies the compiler assumes.
  void step_(FixedChainRole role) {
    switch (role) {
      case FixedChainRole::kVcoA: {
        double a = 0.0;
        vcA_.tick(&a);
        dryA_ = a;
        chIn_[VoiceMixer::kChannelVcoA] = a;
        break;
      }
      case FixedChainRole::kVcoB: {
        double b = 0.0;
        vcB_.tick(&b);
        dryB_ = b;
        chIn_[VoiceMixer::kChannelVcoB] = b;
        break;
      }
      case FixedChainRole::kDrone: {
        double drone[DroneBank::kMaxVoices] = {};
        drone_.tick(drone);
        aggregateDrone_(drone, chIn_);          // classic 1/2/4/5 (divided into 5-gen groups).
        // NEW voices (design/01 §3, #45): drone 3/6 are Papa Srapa composite voices
        // (two Schmitt oscillators + a noise source + a sample & hold), a peer source
        // seeded like drone_. The composite's audio is the audio-Schmitt + noise; the
        // S&H CV is NOT summed into the channel (it is a CV out of the voice).
        double n3 = 0.0;
        pv3_.tick(&n3);
        chIn_[VoiceMixer::kChannelDrone3] = n3;
        sh3Cv_ = pv3_.lastShCv();
        double n6 = 0.0;
        pv6_.tick(&n6);
        chIn_[VoiceMixer::kChannelDrone6] = n6;
        sh6Cv_ = pv6_.lastShCv();
        break;
      }
      case FixedChainRole::kExtIn:
        chIn_[VoiceMixer::kChannelExtAudio] = extSource_;
        break;
      case FixedChainRole::kPreamp: {
        // The ext_source_in break sink (cycle) reads the delayed env_follower value;
        // otherwise the preamp's external input is the EXT.AUDIO terminal.
        const int fb = feedbackSinkIndex_(preampExtIn_);
        const double in = (fb >= 0) ? feedback_[fb].buf[feedback_[fb].writePos]
                                    : extSource_;
        preampOut_ = preamp_.tick(in);
        chIn_[VoiceMixer::kChannelPreamp] = preampOut_;
        break;
      }
      case FixedChainRole::kEnvFollower: {
        envOut_ = envFol_.tick(preampOut_);
        const int fb = feedbackSourceIndex_(envFolOut_);
        if (fb >= 0) {
          FeedbackLine& l = feedback_[fb];
          const std::uint32_t d = clampDelay_(l.delaySamples);
          l.buf[l.writePos] = envOut_;
          l.writePos = (l.writePos + 1) % d;
        }
        break;
      }
      case FixedChainRole::kMixer:
        mixer_.tick(chIn_, mixL_, mixR_);
        break;
      case FixedChainRole::kVcf:
        vcf_.process(mixL_, mixR_, vcfL_, vcfR_);
        break;
      case FixedChainRole::kDistortion:
        wetL_ = distortion_.tickL(vcfL_);
        wetR_ = distortion_.tickR(vcfR_);
        break;
      case FixedChainRole::kNone:
        break;
    }
  }

  double cvAt_(JackId jack) const {
    const std::uint32_t j = static_cast<std::uint32_t>(jack);
    return j < kMaxEdges ? cvOut_[j] : 0.0;
  }

  // Drone grouping — design/01 §3 (CONFIRMED, not provisional): six drone voices,
  // 1/2/4/5 = "CLASSIC" (5 oscillators each, i.e. the DroneBank's 20 voices),
  // 3/6 = "NEW" (Papa Srapa, P3-②, NOT part of the DroneBank). The 20 flat bank
  // oscillators split 5-per-CLASSIC-voice, ascending by channel. NEW 3/6 are the
  // PapaVoice composites set by step_ (pv3_/pv6_), separately from the bank.
  static void aggregateDrone_(const double* drone, double* chIn) {
    const int classicChannels[4] = {VoiceMixer::kChannelDrone1, VoiceMixer::kChannelDrone2,
                                    VoiceMixer::kChannelDrone4, VoiceMixer::kChannelDrone5};
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      const int begin = c * 5;
      const int end = begin + 5;
      for (int v = begin; v < end; ++v) s += drone[v];
      chIn[classicChannels[c]] = s;
    }
  }

  struct FixedRoleBinding {
    ModuleId id;
    FixedChainRole role;
  };

  const JackDescriptor* jacks_ = nullptr;
  std::uint32_t jackCount_ = 0;
  const GraphModule* modules_ = nullptr;
  std::uint32_t moduleCount_ = 0;
  const FixedEdge* fixedEdges_ = nullptr;
  std::uint32_t fixedEdgeCount_ = 0;
  std::uint64_t seed_ = 0;

  PatchGraph patch_;
  CompiledGraph graph_;
  bool graphValid_ = false;
  bool graphDirty_ = false;
  RebuildStatus rebuildStatus_ = RebuildStatus::ok;

  // Effective-edge + CV state (framework-free fixed capacity).
  PatchEdge edges_[kMaxEdges];
  std::uint32_t edgeCount_ = 0;
  double cvOut_[kMaxEdges] = {};

  // Voice-input bindings (registry semantics). Unbound = JackId{0} sentinel.
  JackId voctA_{0};
  JackId voctB_{0};
  JackId vcfCvL_{0};
  JackId vcfCvR_{0};
  JackId preampExtIn_{0};
  JackId envFolOut_{0};

  // Fixed-chain role binding (registry semantics).
  FixedRoleBinding roleBindings_[kMaxFixedModules] = {};
  std::uint32_t roleBindingCount_ = 0;

  // Derived (rebuild_, off audio thread): the ordered fixed-chain role sequence +
  // the break-edge delay lines. Audio path reads these only.
  FixedChainRole chainExecOrder_[kMaxFixedModules] = {};
  std::uint32_t chainExecCount_ = 0;
  FeedbackLine feedback_[kMaxFeedback];
  std::uint32_t feedbackCount_ = 0;

  // #46 timed control-event state (preallocated, RT-safe). EventTimebase owns the
  // absolute-sample pending queue; blockEvents_ is the per-block delivery scratch the
  // render loop drains each block. Both are fixed-capacity (design/07 §5: no heap, no
  // lock). Sizing to kEventDispatchCapacity avoids the product creating its own
  // output-capacity pressure: the buffer always covers a full continuous + critical
  // burst plus the single reconcile failsafe.
  EventTimebase eventTimebase_;
  TimedControlEvent blockEvents_[kEventDispatchCapacity] = {};

  // Per-frame render state (preallocated, RT-safe).
  double chIn_[VoiceMixer::kNumChannels] = {};
  double wetL_ = 0.0, wetR_ = 0.0;
  double dryA_ = 0.0, dryB_ = 0.0;
  double mixL_ = 0.0, mixR_ = 0.0;
  double vcfL_ = 0.0, vcfR_ = 0.0;
  double preampOut_ = 0.0;
  double envOut_ = 0.0;
  double extSource_ = 0.0;
  double sh3Cv_ = 0.0;  // NEW drone 3 Sample & Hold CV out (not in the audio channel).
  double sh6Cv_ = 0.0;  // NEW drone 6 Sample & Hold CV out (not in the audio channel).

  // Voice sources + fixed chain DSP. The runtime no longer uses SignalPath: the
  // mixer/vcf/dist roll into the plan-driven order instead of a hard-coded chain.
  Vco vcA_;
  Vco vcB_;
  Preamp preamp_;
  DroneBank drone_;
  PapaVoice pv3_;                // NEW drone 3 (Papa Srapa composite voice).
  PapaVoice pv6_;                // NEW drone 6 (Papa Srapa composite voice).
  EnvelopeFollower envFol_;
  VoiceMixer mixer_;
  PolivoksFilter vcf_;
  Distortion distortion_;
};

}  // namespace lunar24::core
