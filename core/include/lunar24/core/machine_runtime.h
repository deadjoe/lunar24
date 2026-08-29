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
#include <cmath>
#include <utility>

#include <lunar24/core/control_event.h>
#include <lunar24/core/device_state.h>
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
#include <lunar24/core/unit_identity_profile.h>
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
//
// This enum is retained as the LEGACY dispatch surface for the synthetic test
// fixture and as the readable form returned by chainExecRoleAt(). The canonical
// product path drives the executor through ExecutionKind (below), which is the
// post-B′ representation: distortion is an intra-vcf sub-stage (kVcfPath) and EXT
// AUDIO is a host terminal, never a chain slot. bindFixedRole maps legacy roles
// onto their matching ExecutionKind so the synthetic provider keeps working.
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

// The canonical execution kind a ModuleId plays in the unified per-module executor
// (@Codex B′ ruling msg 190173bb). This is the ONE dispatch set the product path
// uses. Notes:
//   * @Codex 7C2 (msg 4e600057) forbid ExecutionKind dedup: CompiledRegion.modules
//     identity is the ModuleId, and the six drones are six independently-wireable
//     modules. One ExecutionSlot per compiled ModuleId; the SAME kind may repeat;
//     step(slot.id, kind) selects the instance/group by id (see step_ kDroneBank).
//   * kVcfPath is one slot for the VCF->distortion chain (distortion is intra-vcf,
//     never a separate slot in the canonical path); every classic/new drone is its
//     OWN slot (per-group tick), never "one tick whole bank".
//   * The kExtIn/kVcf/kDistortion members are legacy-compat kinds the synthetic
//     fixture maps onto (never produced by the canonical table — ext is a host
//     terminal there, distortion lives inside kVcfPath).
enum class ExecutionKind : std::uint8_t {
  kUnsupported = 0, // declared-deferred / control-only: a strict plan carrying one fail-closes rebuild.
  kVcoA,
  kVcoB,
  kDroneBank,    // one classic drone 1/2/4/5 (DroneBank group) OR NEW drone 3/6 (PapaVoice) per slot.
  kPreamp,
  kEnvFollower,
  kMixer,
  kVcfPath,      // PolivoksFilter L/R -> calibration staging -> Distortion -> WET (ONE slot).
  // Legacy-compat kinds (synthetic fixture only; never emitted by the canonical table).
  kExtIn,
  kVcf,
  kDistortion,
};

// One executable step in the unified per-module executor: a compiled ModuleId plus the
// execution kind it plays. ONE slot per compiled ModuleId (no ExecutionKind dedup), so
// the plan order is exactly the module identity order and step(slot.id, kind) picks the
// instance/group by id (e.g. classic drone_2 -> group 1, drone_3 -> PapaVoice 3).
struct ExecutionSlot {
  ModuleId id;
  ExecutionKind kind;
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

  // MANUAL (panel-control) setters for the WET chain — the actual knobs a host applies
  // from the DeviceState parameters. These are the USER's knob positions and are kept
  // independent from the identity profile: the profile (configureVcfIdentity) carries
  // the per-machine L/R micro-difference, while these set the shared user controls. A
  // host can (and does) set them before/after config without disturbing the identity.
  void setVcfFreq(int ch, double freq) { vcf_.setFreq(ch, freq); }
  void setVcfRes(int ch, double res)   { vcf_.setRes(ch, res); }
  void setVcfMode(int ch, bool bp)     { vcf_.setMode(ch, bp); }
  void setDistortion(double dist, double gain) {
    distortion_.setDist(dist);
    distortion_.setGain(gain);
  }

  // ---------------------------------------------------------------------------
  // GH#6: atomic VCF identity / calibration config entry.
  // ---------------------------------------------------------------------------
  // Consumes the existing DeviceStateV1 identity+calibration triple directly:
  // identityModelVersion + identitySeed.seed + calibration. In ONE call it
  // derives the per-L/R identity profile (unit_identity_profile.h) and configures
  // the WHOLE VCF→distortion→gain staging path — VCF input-stage drive, Distortion
  // drive/rail micro-diff from the SAME profile, and the near-unity path-gain
  // staging micro-diff. The calibration trim is applied ONCE, at the clear
  // VCF→distortion staging point, so it genuinely changes the level the post-filter
  // distortion folds (design/07 §7: the whole level-dependent path is calibrated;
  // L/R calibration/nonlinear state are independent).
  //
  // FAIL-CLOSED (fixed policy: "keep old complete profile"): if the version is
  // unsupported or either calibration trim is not finite/positive, this returns
  // false and makes NO change — the previously-applied complete profile stays fully
  // intact (no half-profile, no residue, no partial update). The caller must treat
  // a false return as "the request is rejected"; nothing is applied.
  bool configureVcfIdentity(std::uint32_t identityModelVersion, std::uint64_t seed,
                            const CalibrationState& calibration) {
    if (!isSupportedIdentityVersion(identityModelVersion)) return false;
    const float lT = calibration.vcfLeftTrim;
    const float rT = calibration.vcfRightTrim;
    if (!(std::isfinite(lT) && lT > 0.0f)) return false;
    if (!(std::isfinite(rT) && rT > 0.0f)) return false;

    // Derive the complete profile (v1 supported path). Atomic: only after the
    // validation above AND a successful derivation do we mutate any state below.
    const VcfIdentityProfile profile = deriveVcfIdentityProfile(seed, identityModelVersion);

    // VCF input-stage drive (independent per L/R).
    vcf_.setInputDrive(PolivoksFilter::kChannelLeft, profile.left.vcfDrive);
    vcf_.setInputDrive(PolivoksFilter::kChannelRight, profile.right.vcfDrive);
    // Distortion drive/rail micro-diff from the SAME profile.
    distortion_.setChannelDrive(0, Distortion::kDriveFold * (1.0 + profile.left.distDrive));
    distortion_.setChannelRail(0, Distortion::kSaturationVoltage *
                                      (1.0 + kDistRailStiffness * profile.left.distDrive));
    distortion_.setChannelDrive(1, Distortion::kDriveFold * (1.0 + profile.right.distDrive));
    distortion_.setChannelRail(1, Distortion::kSaturationVoltage *
                                      (1.0 + kDistRailStiffness * profile.right.distDrive));
    // Path staging micro-diff (near-unity) × calibration trim, applied once at the
    // VCF→distortion staging point. Independent per L/R.
    vcfPathStageL_ = static_cast<double>(lT) * profile.left.pathGain;
    vcfPathStageR_ = static_cast<double>(rT) * profile.right.pathGain;

    identityConfigured_ = true;
    return true;
  }

  // FIXED-CHAIN ROLE BINDING (registry semantic). Maps a compiled module id to the
  // fixed-chain role it plays. Unbound modules are skipped by the chain render.
  // This is a legacy surface: it maps each role onto the matching ExecutionKind so
  // the synthetic test fixture (which thinks in roles, including a standalone ext_in
  // and distortion module) keeps driving the same per-module executor. The canonical
  // product path binds ExecutionKind directly (bindExecutionKind).
  void bindFixedRole(ModuleId id, FixedChainRole role) {
    bindExecutionKind(id, toExecutionKind_(role));
  }

  // CANONICAL execution-kind binding (post-B′). Maps a compiled module id to the
  // ExecutionKind it plays in the unified per-module executor. Unbound (kUnsupported)
  // modules are skipped by the chain render — but a plan that PULLS a kUnsupported
  // module into a compiled region FAILS the rebuild (fail-closed, never a silent skip).
  void bindExecutionKind(ModuleId id, ExecutionKind kind) {
    for (std::uint32_t i = 0; i < roleBindingCount_; ++i) {
      if (roleBindings_[i].id == id) {
        // Update branch: a kind change is a real plan mutation that must re-preflight. A
        // successful rebuild followed by a kind flip would otherwise short-circuit to
        // graph_unchanged and keep the OLD preflight/slots (@Codex 263cb3ca point 2) — the
        // explicit strict policy is violated. Only dirty on an actual change.
        if (roleBindings_[i].kind != kind) {
          roleBindings_[i].kind = kind;
          graphDirty_ = true;
        }
        return;
      }
    }
    if (roleBindingCount_ < kMaxFixedModules) {
      roleBindings_[roleBindingCount_].id = id;
      roleBindings_[roleBindingCount_].kind = kind;
      ++roleBindingCount_;
      graphDirty_ = true;  // add branch: never-before-bound id mutates the plan too.
    }
  }

  // STRICT BINDING POLICY (@Codex 7C2 msg 4e600057). Canonical strictness must not be
  // inferred by "is there any executable binding" guesswork — the OWNING definition
  // explicitly enables it. Off by default (the legacy synthetic fixtures drive a
  // permissive mode: an unbound module is silently outside the executor's scope). When
  // ON, rebuild() fail-closes TWO distinct ways: a compiled-region module explicitly
  // bound to kUnsupported -> unsupported_module, and a compiled-region module with NO
  // binding at all -> missing_execution_binding. Never a silent skip, never a guessed
  // gate. The canonical owning definition (MachineRuntimeDefinition, next slice) turns it
  // on.
  void setStrictBindings(bool on) {
    if (on != strictBindings_) {
      strictBindings_ = on;
      graphDirty_ = true;  // a permissive plan must be re-preflighted under the new policy.
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

  // ---- CLASSIC group GATE/HOLD/ATT/RLS + CV MOD + environment (batch 4A, GH#5) ----
  // Panel/control entries for the 4 CLASSIC drone voices (voiceGroup 0..3 == drone
  // 1/2/4/5). Each forwards to the DroneBank, whose gate/ATT/RLS/HOLD envelope the
  // product path reads every frame in step_(kDrone). ATT/RLS take the registry's
  // NORMALIZED 0..1 control (the bank does the single monotonic norm->seconds map).
  void setDroneGroupGate(int voiceGroup, bool on) { drone_.setGroupGate(voiceGroup, on); }
  void setDroneGroupHold(int voiceGroup, bool on) { drone_.setGroupHold(voiceGroup, on); }
  void setDroneGroupAtt(int voiceGroup, double norm) { drone_.setGroupAtt(voiceGroup, norm); }
  void setDroneGroupRls(int voiceGroup, double norm) { drone_.setGroupRls(voiceGroup, norm); }
  // Per-group shared CV MOD input: RAW virtual volts from the runtime CV source bank (the
  // value the control layer resolved for the group's cv_mod_in jack), NOT a normalized
  // 0..1 upstream scale. The bank applies it only to generators whose MOD button is on
  // (MOD-off generators are unresponsive to CV AND environment — design/07 §7).
  void setDroneGroupModCv(int voiceGroup, double cv) { drone_.setGroupModCv(voiceGroup, cv); }
  // Shared/correlated environment term a desktop host can provide (design/07 §7). It
  // detunes the MOD-on generators of every classic group together; MOD-off unchanged.
  void setDroneEnvironment(double hz) { drone_.setEnvironment(hz); }

  // Registry CV binding for the 4 CLASSIC drone groups (order 0..3 == drone 1/2/4/5):
  // env_out jacks the product WRITES virtual volts to, cv_mod_in jacks the product READS
  // as the group's shared CV MOD (via the control layer). @Codex 方案2b: descriptor-driven
  // provisional volts; the test binds the four REAL generated-registry jacks (descriptors
  // stay source-of-evidence, unchanged).
  //
  // ATOMIC FAIL-CLOSED ADMISSION (batch 4A convergence, @Codex 52d3c620): each setter first
  // validates ALL FOUR jacks against the COMMON rule (descriptor exists, id indexes cvOut_
  // (< kMaxEdges), owning module is exactly this group's classic voice drone_1/2/4/5) plus
  // the per-kind rule (ENV OUT: direction=output, signal=cv, finite & non-inverted range;
  // CV MOD: direction=input, signal=cv). If ANY fails the WHOLE cohort is rejected: every
  // group is left UNBOUND (bound-state false, jack cleared to JackId{0}, no partial and no
  // stale readback). Returns true only when all four were admitted and are now live.
  bool setDroneEnvOutBindings(JackId g0, JackId g1, JackId g2, JackId g3) {
    const JackId ids[4] = {g0, g1, g2, g3};
    for (int g = 0; g < kClassicDroneVoices; ++g)
      if (!envOutBindingValid_(g, ids[g])) { releaseEnvOut_(); return false; }
    // All four valid: atomically release the old cohort first (which deterministically
    // zeroes its env_out CV source slots), then commit the new cohort. See releaseEnvOut_.
    releaseEnvOut_();
    for (int g = 0; g < kClassicDroneVoices; ++g) {
      envOutJack_[g] = ids[g];
      envOutBound_[g] = true;
    }
    return true;
  }
  bool setDroneCvModInBindings(JackId g0, JackId g1, JackId g2, JackId g3) {
    const JackId ids[4] = {g0, g1, g2, g3};
    for (int g = 0; g < kClassicDroneVoices; ++g)
      if (!cvModInBindingValid_(g, ids[g])) { releaseCvModIn_(); return false; }
    // All four valid: atomically release the old cohort first (which resets the retained
    // shared MOD of every group to 0), then commit the new cohort. See releaseCvModIn_.
    releaseCvModIn_();
    for (int g = 0; g < kClassicDroneVoices; ++g) {
      cvModInJack_[g] = ids[g];
      cvModInBound_[g] = true;
    }
    return true;
  }

  // ---- batch 4A read-only inspectors (executed value, never a shadow mirror) ----
  // Internal normalized group envelope level 0..1 (what step_(kDrone) multiplied the
  // group's audio by last frame; == DroneBank::groupEnvLevel).
  double droneEnvLevel(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices
               ? drone_.groupEnvLevel(voiceGroup)
               : 0.0;
  }
  // The group's shared CV MOD value the DroneBank is actually applying (== DroneBank::
  // groupModCv, the executed modCvG_, never a shadow mirror). A CV MOD cohort that was
  // released after consuming a patched CV must leave 0 here — the @Codex 7a42d10a #2
  // release-reset is directly observable. Unbound/out-of-range -> 0.
  double droneGroupModCv(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices
               ? drone_.groupModCv(voiceGroup)
               : 0.0;
  }
  // The virtual volts the PRODUCT actually wrote into the runtime CV source bank for the
  // group's env_out jack last frame (== the value at cvOut_[envOutJack], not a rewrite).
  // An unbound group's env_out is never written, so this reads 0 (not a stale jack 0).
  double droneEnvOutVolts(int voiceGroup) const {
    if (voiceGroup < 0 || voiceGroup >= kClassicDroneVoices) return 0.0;
    // Authoritative bound flag decides, NOT a JackId{0} sentinel (id 0 is a real
    // vco_a.cv_in jack). A never-bound / release-cohort group is unbound ⇒ never written.
    if (!envOutBound_[voiceGroup]) return 0.0;
    return cvAt_(envOutJack_[voiceGroup]);
  }
  // Fail-closed bound/validity: true only after a SUCCESSFUL atomic admission (the
  // explicit post-admission flag). A group that was never bound, or whose cohort was
  // rejected, is UNBOUND — step_ never writes it, so the readback stays 0. This reflects
  // real post-admission state, not "is JackId{0}" (that sentinel would false-positive
  // against the real vco_a.cv_in jack id 0).
  bool droneEnvOutBound(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices && envOutBound_[voiceGroup];
  }
  // General read of a control generator's resolved CV output (the whole CV source bank).
  double controlVoltageAt(JackId jack) const { return cvAt_(jack); }

  // ---- GH#6 read-only inspectors (executed value, never a shadow mirror) ----
  // Read back the profile the runtime is ACTUALLY executing, straight from the live
  // DSP (PolivoksFilter::inputDrive, Distortion::channelDrive/Rail, and the staging
  // gain step_(kDistortion) multiplies). An inspector that reports a value the DSP
  // does not run is a test-shadow and is caught by the config-oracle (only-change-
  // inspector is red).
  bool vcfIdentityConfigured() const { return identityConfigured_; }
  double vcfInputDrive(int ch) const { return vcf_.inputDrive(ch); }
  double distortionDrive(int ch) const { return distortion_.channelDrive(ch); }
  double distortionRail(int ch) const { return distortion_.channelRail(ch); }
  // The near-unity staging gain the distortion sees its input scaled by: the
  // calibration trim × the profile path-gain micro-diff, applied at the VCF→dist.
  // point. L/R independent (calibration-state independence, design/07 §7).
  double vcfPathStagingGain(int ch) const { return ch == 0 ? vcfPathStageL_ : vcfPathStageR_; }

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
    unsupported_module,          // strict: a compiled-region module explicitly bound to kUnsupported
    missing_execution_binding,   // strict: a compiled-region module has NO ExecutionKind binding
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
    // STRICT binding + slot-capacity preflight, ALL before graph_=r.graph (never a
    // fail-after-branch). With strict policy ON the owning definition has exactly one
    // disposition per module, so a plan that routes ANY mis-dispositioned module into a
    // compiled region must REFUSE. Two distinct fail-closed statuses, per @Codex 7C2:
    //   * unsupported_module        — explicitly bound to kUnsupported (declared-deferred).
    //   * missing_execution_binding — NO binding at all. Distinct from the above: there
    //                                 is no "the binding was inferred" patch in a strict plan.
    // The slot count is then capped here (the plan must fit the RT-safe arrays), so
    // rebuildChainExec_ below can never silently drop a slot.
    if (strictBindings_) {
      if (hasMissingBinding_(r.graph)) {
        graphValid_ = false;
        rebuildStatus_ = RebuildStatus::missing_execution_binding;
        return false;
      }
      if (hasUnsupportedModule_(r.graph)) {
        graphValid_ = false;
        rebuildStatus_ = RebuildStatus::unsupported_module;
        return false;
      }
    }
    if (countExecSlots_(r.graph) > kMaxFixedModules) {
      graphValid_ = false;
      rebuildStatus_ = RebuildStatus::edge_capacity;  // plan exceeds the fixed slot array.
      return false;
    }
    graph_ = r.graph;
    graphValid_ = true;
    graphDirty_ = false;
    rebuildStatus_ = RebuildStatus::ok;
    if (!rebuildChainExec_()) {
      // Unreachable given the slot + feedback capacity preflight above, but fail closed:
      // never leave a half-built runtime line set behind a valid flag.
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
    extSource_ = extSource;
    for (int i = 0; i < kNumChannels; ++i) chIn_[i] = 0.0;
    // Drive the per-module executor: one resolve->step->publish per slot, in the plan
    // order with ExecutionKind deduped. driveGraph=false bypasses the CONTROL layer:
    // slots still run, but no CV sink is resolved from the graph (criterion-① negative).
    for (std::uint32_t i = 0; i < execSlotCount_; ++i) step_(execSlots_[i], driveGraph);
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
  // Per-module executor slots (the canonical surface): count + the (ModuleId, ExecutionKind)
  // slot at `i`, ONE slot per compiled ModuleId in the plan order (no kind dedup).
  std::uint32_t execSlotCount() const { return execSlotCount_; }
  const ExecutionSlot& execSlotAt(std::uint32_t i) const { return execSlots_[i]; }

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

  // Fixed-kind -> id lookup (linear over the small binding table).
  ExecutionKind kindOf_(ModuleId id) const {
    for (std::uint32_t i = 0; i < roleBindingCount_; ++i)
      if (roleBindings_[i].id == id) return roleBindings_[i].kind;
    return ExecutionKind::kUnsupported;
  }
  // Source jack of the effective edge feeding `sink`, or no-is-a-sink (`!found`) if
  // none. The per-slot CV resolver uses this to pull the value a control producer
  // published for the sink's incoming edge (same-sample when that producer ran earlier
  // in the plan order).
  JackId sourceOfSink_(JackId sink, bool& found) const {
    for (std::uint32_t i = 0; i < edgeCount_; ++i)
      if (edges_[i].sink == sink) { found = true; return edges_[i].source; }
    found = false;
    return JackId{0};
  }

  // ---- single resolve/publish pair (the ONE value-movement entry in the executor) ----
  // Resolve the value a consuming module should read for a SINK jack, by EXACT edge
  // identity (source,sink) — never by source alone (@Codex 7C2 msg 4e600057: a same-source
  // NORMAL downstream must not misread a feedback delay line). A sink fed by a SELECTED
  // feedback edge is read from that edge's own D-sample delay line at the current write
  // position (graph_compiler.h consume-rule — off-by-one vs live-last-written is the defect
  // this discriminates); any other fed sink is read from the live CV source bank (the value
  // a control producer published). An UNFED sink returns `fallback` (e.g. the preamp's
  // EXT.AUDIO terminal when no cable feeds its ext_source_in).
  double resolveSinkValue_(JackId sink, double fallback) const {
    bool found = false;
    const JackId src = sourceOfSink_(sink, found);
    if (!found) return fallback;
    const int fb = feedbackExactIndex_(src, sink);
    if (fb >= 0) return feedback_[fb].buf[feedback_[fb].writePos];
    return cvAt_(src);
  }
  // Publish the value a source module JUST computed for its output `src`. ALWAYS writes
  // the live CV source bank (so a normal downstream reads the CURRENT value), AND advances
  // EVERY feedback line whose sourceJack is `src` (an exact-pair consumer reads the D-sample
  // delay). @Codex 7C2: publish must not stop at the first source match.
  void publishSourceValue_(JackId src, double v) {
    const std::uint32_t j = static_cast<std::uint32_t>(src);
    if (j < kMaxEdges) cvOut_[j] = v;
    for (std::uint32_t i = 0; i < feedbackCount_; ++i) {
      FeedbackLine& l = feedback_[i];
      if (!l.active || l.sourceJack != src) continue;
      const std::uint32_t d = clampDelay_(l.delaySamples);
      l.buf[l.writePos] = v;
      l.writePos = (l.writePos + 1) % d;
    }
  }
  int feedbackExactIndex_(JackId src, JackId sink) const {
    for (std::uint32_t i = 0; i < feedbackCount_; ++i)
      if (feedback_[i].active && feedback_[i].sourceJack == src &&
          feedback_[i].sinkJack == sink) return static_cast<int>(i);
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
  // Does `id` carry an EXPLICIT ExecutionKind binding (via bindFixedRole /
  // bindExecutionKind)? A module that was never bound is a MISSING binding under strict
  // policy; one that WAS bound to kUnsupported is a DECLARED unsupported module.
  bool hasBinding_(ModuleId id) const {
    for (std::uint32_t i = 0; i < roleBindingCount_; ++i)
      if (roleBindings_[i].id == id) return true;
    return false;
  }

  // Strict fail-closed preflight (rebuild_, @Codex 7C2): a compiled-region module with NO
  // binding at all. Only consulted when strictBindings_ is ON — legacy fixtures drive the
  // permissive mode where an unbound module is outside the executor's scope.
  // @Codex 67dc06c6: scan the compiler's OWN module set. compile_graph() already collects
  // region.modules only from candidate PatchEdge/FixedEdge endpoints, so it is already the
  // active set — do NOT re-derive "active" here from edges_/fixedEdges_ (that duplicate truth
  // source drifts from the compiler and false-fails or false-passes; previously moduleActive_).
  bool hasMissingBinding_(const CompiledGraph& g) const {
    for (const CompiledRegion& region : g.regions)
      for (ModuleId id : region.modules)
        if (!hasBinding_(id)) return true;
    return false;
  }

  // Strict fail-closed preflight (rebuild_, @Codex 7C2): a compiled-region module
  // EXPLICITLY bound to ExecutionKind::kUnsupported. The canonical fixed-chain table binds
  // the six control sources + effector/voices to kUnsupported (not integrated in this
  // slice), so patching any of them into the graph is a real semantics violation: REFUSE
  // with unsupported_module, never silently skip to zero slots. Distinct from
  // hasMissingBinding_ (an unbound module is not a "kUnsupported" module). Only consulted
  // when strictBindings_ is ON. @Codex 67dc06c6: same direct scan — an isolated kUnsupported
  // module is not in region.modules (compile_graph excludes it), so it stays LEGAL, while
  // the same module once patched into a real edge enters regions and REFUSES. No moduleActive_.
  bool hasUnsupportedModule_(const CompiledGraph& g) const {
    for (const CompiledRegion& region : g.regions)
      for (ModuleId id : region.modules)
        if (hasBinding_(id) && kindOf_(id) == ExecutionKind::kUnsupported) return true;
    return false;
  }

  // Count of execution slots the plan would generate (region modules that are NOT
  // kUnsupported, i.e. each has a real kind). Used as the slot-capacity preflight so
  // rebuildChainExec_ never silently drops a slot.
  std::uint32_t countExecSlots_(const CompiledGraph& g) const {
    std::uint32_t n = 0;
    for (const CompiledRegion& region : g.regions)
      for (ModuleId id : region.modules)
        if (kindOf_(id) != ExecutionKind::kUnsupported) ++n;
    return n;
  }

  // Derive the RT-safe per-module execution slots + delay lines from the compiled
  // plan. Called only from rebuild() (off the audio thread); the audio path only reads.
  // The slots are ONE per compiled ModuleId in the plan order (@Codex 7C2: no ExecutionKind
  // dedup — the six drones are six independently-wireable modules, so each gets its own
  // slot and step_ dispatches by id). chainExecOrder_ (the legacy FixedChainRole inspector
  // array) is derived from the slots so the read-only surface still reports the roles in
  // plan order. Returns false (fail-closed) only if the plan would exceed the fixed
  // delay-line storage — never partially published (slot capacity is preflighted).
  bool rebuildChainExec_() {
    chainExecCount_ = 0;
    execSlotCount_ = 0;
    feedbackCount_ = 0;
    for (const CompiledRegion& region : graph_.regions) {
      for (ModuleId id : region.modules) {
        const ExecutionKind kind = kindOf_(id);
        if (kind == ExecutionKind::kUnsupported) continue;  // refused by strict preflight.
        // ONE slot per compiled ModuleId, in the plan order — no ExecutionKind dedup.
        if (execSlotCount_ < kMaxFixedModules) {
          execSlots_[execSlotCount_].id = id;
          execSlots_[execSlotCount_].kind = kind;
          ++execSlotCount_;
          chainExecOrder_[chainExecCount_] = fixedChainRoleOf_(kind);
          ++chainExecCount_;
        }
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

  // Resolve a CONTROL sink's incoming value — SAME-SAMPLE when the producer ran earlier
  // in the plan order. `sink` is a voice-input jack (VCO v_oct, VCF cv) or a classic
  // drone group's cv_mod_in; the value comes from the effective edge that feeds it
  // (sourceOfSink_) via resolveSinkValue_ (the single exact-edge source-of-truth read).
  // Returns false (and leaves `out` untouched) when the graph is bypassed (driveGraph=false
  // — the criterion-① negative, a patched CV then has no effect) or `sink` is not fed.
  bool resolveControlSink_(JackId sink, double& out, bool driveGraph) const {
    if (!driveGraph) return false;
    bool found = false;
    static_cast<void>(sourceOfSink_(sink, found));
    if (!found) return false;
    out = resolveSinkValue_(sink, 0.0);
    return true;
  }

  // One per-module step, dispatched in the plan's order. Kept as a switch over the
  // (small, fixed) ExecutionKind set so the render path is allocation-free and bounds
  // the latencies the compiler assumes. Every CV/feedback value movement goes through
  // resolveSinkValue_ / publishSourceValue_ — the ONE source-of-truth pair (@Codex 7C2
  // req. 3: resolve by exact (src,sink) edge, publish updates live source + ALL its
  // feedback lines, never a same-source stop-at-first-match). The B′ kVcfPath slot runs
  // VCF -> calibration staging -> Distortion -> WET inside a single step (distortion is
  // an intra-vcf sub-stage, never a separate slot in the canonical path); kExtIn remains
  // only as a legacy-compat kind the synthetic fixture maps onto.
  // driveGraph=false bypasses the CONTROL layer (criterion-① negative).
  void step_(const ExecutionSlot& slot, bool driveGraph) {
    switch (slot.kind) {
      case ExecutionKind::kVcoA: {
        double v = 0.0;
        if (resolveControlSink_(voctA_, v, driveGraph)) vcA_.setVoct(v);
        double a = 0.0;
        vcA_.tick(&a);
        dryA_ = a;
        chIn_[VoiceMixer::kChannelVcoA] = a;
        break;
      }
      case ExecutionKind::kVcoB: {
        double v = 0.0;
        if (resolveControlSink_(voctB_, v, driveGraph)) vcB_.setVoct(v);
        double b = 0.0;
        vcB_.tick(&b);
        dryB_ = b;
        chIn_[VoiceMixer::kChannelVcoB] = b;
        break;
      }
      case ExecutionKind::kDroneBank: {
        // @Codex 7C2 (msg 4e600057): NEVER "one tick whole bank". ONE slot per compiled
        // ModuleId; step(slot.id, kind) selects the instance/group by id. Each classic
        // drone module (drone_1/2/4/5) runs exactly ITS OWN group per slot and resolves
        // its cv_mod BEFORE ticking (same-sample), then publishes ITS env_out. drone 3/6
        // are separate PapaVoice slots. The legacy synthetic aggregate drone (kM_Drone,
        // NOT a registry drone_N) falls through to a clearly-marked whole-bank compat path.
        const int classicGroup = classicGroupOfDrone_(slot.id);
        if (slot.id == ModuleId::drone_3) {
          tickPapaVoice_(pv3_, VoiceMixer::kChannelDrone3, sh3Cv_);
          break;
        }
        if (slot.id == ModuleId::drone_6) {
          tickPapaVoice_(pv6_, VoiceMixer::kChannelDrone6, sh6Cv_);
          break;
        }
        if (classicGroup >= 0) {
          // (1) resolve ONLY this group's shared CV MOD before the tick — never the
          // one-sample-old value from a post-tick set. MOD-off generators stay inert.
          if (cvModInBound_[classicGroup]) {
            double m = 0.0;
            if (resolveControlSink_(cvModInJack_[classicGroup], m, driveGraph))
              drone_.setGroupModCv(classicGroup, m);
          }
          // (2) tick ONLY this group (advances exactly this group's sample counter).
          double out5[DroneBank::kGensPerVoice] = {};
          drone_.tickGroup(classicGroup, out5);
          // (3) publish this group's channel + ENV OUT through the single write. The
          // gate/ATT/RLS/HOLD envelope is inside tickGroup; the ENV OUT transfer is
          // descriptor-driven: nominalMin + level*(nominalMax-nominalMin), range read
          // ONLY from the bound JackDescriptor (方案2b). A consumer running LATER in the
          // plan reads the SAME-frame value (oracle ②).
          double s = 0.0;
          for (std::size_t i = 0; i < DroneBank::kGensPerVoice; ++i) s += out5[i];
          chIn_[classicChannel(classicGroup)] = s;
          if (envOutBound_[classicGroup]) {
            const JackId envOut = envOutJack_[classicGroup];
            const JackDescriptor* d = findJackDescriptor_(envOut);
            const double lvl = drone_.groupEnvLevel(static_cast<std::size_t>(classicGroup));
            if (d != nullptr)
              publishSourceValue_(envOut, d->nominalMin + lvl * (d->nominalMax - d->nominalMin));
          }
          break;
        }
        // LEGACY whole-bank compat path (synthetic fixture ONLY: kM_Drone is one aggregate
        // module, never a registry drone_N). Kept bit-identical with the old all-voices
        // tick, but still resolves every bound CV MOD before ticking (resolve -> tick ->
        // publish). This path never exists in a canonical strict plan (which drives 6
        // distinct drone slots), so per-group correctness is unaffected.
        {
          double drone[DroneBank::kMaxVoices] = {};
          for (int g = 0; g < kClassicDroneVoices; ++g) {
            if (!cvModInBound_[g]) continue;
            double m = 0.0;
            if (resolveControlSink_(cvModInJack_[g], m, driveGraph)) drone_.setGroupModCv(g, m);
          }
          drone_.tick(drone);
          aggregateDrone_(drone, chIn_);  // classic 1/2/4/5 (divided into 5-gen groups).
          for (int g = 0; g < kClassicDroneVoices; ++g) {
            if (!envOutBound_[g]) continue;
            const JackId envOut = envOutJack_[g];
            const JackDescriptor* d = findJackDescriptor_(envOut);
            const double lvl = drone_.groupEnvLevel(static_cast<std::size_t>(g));
            if (d != nullptr)
              publishSourceValue_(envOut, d->nominalMin + lvl * (d->nominalMax - d->nominalMin));
          }
          tickPapaVoice_(pv3_, VoiceMixer::kChannelDrone3, sh3Cv_);
          tickPapaVoice_(pv6_, VoiceMixer::kChannelDrone6, sh6Cv_);
          break;
        }
      }
      case ExecutionKind::kExtIn:  // legacy synthetic only; canonical EXT is the host terminal.
        chIn_[VoiceMixer::kChannelExtAudio] = extSource_;
        break;
      case ExecutionKind::kPreamp: {
        // The ext_source_in break sink goes through the SINGLE sink resolver: a cycle
        // reads the delayed env_follower value, a normal edge reads the live source, and
        // an UNFED sink falls back to the EXT.AUDIO terminal. No feedbackSinkIndex_
        // special-case (@Codex 7C2 req. 3 — preamp uses the same exact-edge resolver).
        const double in = resolveSinkValue_(preampExtIn_, extSource_);
        preampOut_ = preamp_.tick(in);
        chIn_[VoiceMixer::kChannelPreamp] = preampOut_;
        break;
      }
      case ExecutionKind::kEnvFollower: {
        envOut_ = envFol_.tick(preampOut_);
        publishSourceValue_(envFolOut_, envOut_);  // cycle leg -> its own delay line.
        break;
      }
      case ExecutionKind::kMixer:
        mixer_.tick(chIn_, mixL_, mixR_);
        break;
      case ExecutionKind::kVcfPath: {
        // B′ merged VCF path (canonical): VCF -> calibration staging -> Distortion -> WET
        // in ONE slot-only dispatch. The distortion is an intra-vcf sub-stage, so it is
        // never a separate compiler edge or a second slot.
        resolveVcfCv_(driveGraph);
        vcf_.process(mixL_, mixR_, vcfL_, vcfR_);
        wetL_ = distortion_.tickL(vcfL_ * vcfPathStageL_);
        wetR_ = distortion_.tickR(vcfR_ * vcfPathStageR_);
        break;
      }
      case ExecutionKind::kVcf: {  // legacy synthetic standalone VCF.
        resolveVcfCv_(driveGraph);
        vcf_.process(mixL_, mixR_, vcfL_, vcfR_);
        break;
      }
      case ExecutionKind::kDistortion: {  // legacy synthetic standalone Distortion.
        // GH#6: the calibration trim × identity path-gain micro-diff is applied ONCE
        // here — the clear VCF→distortion staging point — BEFORE the post-filter fold.
        wetL_ = distortion_.tickL(vcfL_ * vcfPathStageL_);
        wetR_ = distortion_.tickR(vcfR_ * vcfPathStageR_);
        break;
      }
      case ExecutionKind::kUnsupported:
        break;
    }
  }

  // Resolve the VCF L/R control CV (needed by both the merged kVcfPath slot and the
  // legacy standalone kVcf slot).
  void resolveVcfCv_(bool driveGraph) {
    double l = 0.0, r = 0.0;
    if (resolveControlSink_(vcfCvL_, l, driveGraph)) vcf_.setCvL(l);
    if (resolveControlSink_(vcfCvR_, r, driveGraph)) vcf_.setCvR(r);
  }

  double cvAt_(JackId jack) const {
    const std::uint32_t j = static_cast<std::uint32_t>(jack);
    return j < kMaxEdges ? cvOut_[j] : 0.0;
  }

  // Look up a registered JackDescriptor by id (linear over the small registry). Returns
  // nullptr when absent — fail-closed callers never assume a jack is registered.
  const JackDescriptor* findJackDescriptor_(JackId id) const {
    for (std::uint32_t i = 0; i < jackCount_; ++i)
      if (jacks_[i].id == id) return &jacks_[i];
    return nullptr;
  }
  // A DESCRIPTOR has a usable (finite, non-inverted) nominal range. Only then is the
  // descriptor-driven ENV OUT transfer safe to compute (方案2b fail-closed: never write
  // a NaN/garbage/inverted-range voltage into the CV source bank).
  static bool validRange_(const JackDescriptor* d) {
    return d != nullptr && std::isfinite(d->nominalMin) && std::isfinite(d->nominalMax) &&
           d->nominalMax >= d->nominalMin;
  }
  // Index (0..3) of the classic group whose cv_mod_in jack is `sink`, or -1 if none.
  // Only groups admitting a live cv_mod_in binding match; a released cohort's jacks are
  // all JackId{0} so they never match (no stale read).
  int cvModInGroupOf_(JackId sink) const {
    // Decided solely by the explicit cvModInBound_ flag (NOT a JackId{0} sentinel, which
    // is a real vco_a.cv_in jack id). A group bound to a legitimately-valid jack id 0
    // must match and consume the patched CV.
    for (int g = 0; g < kClassicDroneVoices; ++g)
      if (cvModInBound_[g] && cvModInJack_[g] == sink) return g;
    return -1;
  }

  // The ONE classic voice that classic drone group `g` (0..3 == drone 1/2/4/5) must own.
  // A jack whose module is not exactly this is a wrong-owner binding → admission fails.
  static ModuleId classicDroneOwner(int g) {
    switch (g) {
      case 0: return ModuleId::drone_1;
      case 1: return ModuleId::drone_2;
      case 2: return ModuleId::drone_4;
      case 3: return ModuleId::drone_5;
      default: return ModuleId::drone_1;  // g is always [0, kClassicDroneVoices); defensive.
    }
  }
  // Common admission rule for a classic drone group binding: descriptor exists, id indexes
  // cvOut_ (< kMaxEdges), and the owning module is exactly this group's classic voice.
  bool commonBindingValid_(int g, JackId id) const {
    const std::uint32_t j = static_cast<std::uint32_t>(id);
    if (j >= kMaxEdges) return false;      // id can't index cvOut_ (out-of-capacity).
    const JackDescriptor* d = findJackDescriptor_(id);
    if (d == nullptr) return false;        // not a registered jack (missing id).
    return d->module == classicDroneOwner(g);  // wrong-owner fails here.
  }
  // ENV OUT extra rule: an OUTPUT cv jack with a finite, non-inverted nominal range.
  bool envOutBindingValid_(int g, JackId id) const {
    if (!commonBindingValid_(g, id)) return false;
    const JackDescriptor* d = findJackDescriptor_(id);
    return d->direction == PinDirection::output && d->signalType == SignalType::cv &&
           validRange_(d);
  }
  // CV MOD extra rule: an INPUT cv jack.
  bool cvModInBindingValid_(int g, JackId id) const {
    if (!commonBindingValid_(g, id)) return false;
    const JackDescriptor* d = findJackDescriptor_(id);
    return d->direction == PinDirection::input && d->signalType == SignalType::cv;
  }
  void releaseEnvOut_() {
    for (int g = 0; g < kClassicDroneVoices; ++g) {
      // Deterministically clear the old env_out CV source slot so a released cohort
      // leaves NO stale voltage consumable through the patch graph (@Codex 7a42d10a #1).
      if (envOutBound_[g]) {
        const std::uint32_t j = static_cast<std::uint32_t>(envOutJack_[g]);
        if (j < kMaxEdges) cvOut_[j] = 0.0;
      }
      envOutJack_[g] = JackId{0};
      envOutBound_[g] = false;
    }
  }
  void releaseCvModIn_() {
    for (int g = 0; g < kClassicDroneVoices; ++g) {
      // Reset each group's SHARED MOD to 0 so a released cohort keeps no applied
      // modulation (@Codex 7a42d10a #2): DroneBank::modCvG_ would otherwise retain the
      // last driven value and keep modulating even after the binding is released.
      drone_.setGroupModCv(g, 0.0);
      cvModInJack_[g] = JackId{0};
      cvModInBound_[g] = false;
    }
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

  // @Codex 7C2 per-id dispatch: map a compiled DroneBank ModuleId to its 0-based CLASSIC
  // group index (drone_1/2/4/5 -> g0/g1/g2/g3), or -1 for the NEW Papa voices (drone 3/6)
  // and for any non-registry synthetic id (e.g. legacy kM_Drone {5}, which is envelope_b
  // in the canonical enum — never a drone_N). A canonical strict plan drives exactly six
  // distinct drone slots (classicGroupOfDrone_ identifies the four classic ones).
  static int classicGroupOfDrone_(ModuleId id) {
    switch (id) {
      case ModuleId::drone_1: return 0;
      case ModuleId::drone_2: return 1;
      case ModuleId::drone_4: return 2;
      case ModuleId::drone_5: return 3;
      default:               return -1;
    }
  }

  // Classic group index -> VoiceMixer channel (1/2/4/5, ascending by channel).
  static int classicChannel(int group) {
    const int classicChannels[4] = {VoiceMixer::kChannelDrone1, VoiceMixer::kChannelDrone2,
                                    VoiceMixer::kChannelDrone4, VoiceMixer::kChannelDrone5};
    return classicChannels[group];
  }

  // NEW-voice (drone 3/6) per-slot tick: advance the PapaVoice, write the channel, and
  // latch the S&H CV out of the voice (NOT summed into the channel — it is a CV out).
  void tickPapaVoice_(PapaVoice& pv, int channel, double& shCvOut) {
    double n = 0.0;
    pv.tick(&n);
    chIn_[channel] = n;
    shCvOut = pv.lastShCv();
  }

  struct FixedRoleBinding {
    ModuleId id;
    ExecutionKind kind;
  };

  // Legacy FixedChainRole -> ExecutionKind (1:1 for the synthetic fixture's dispatch
  // kinds). kNone -> kUnsupported. The canonical table never routes through this — it
  // binds kVcfPath/kDroneBank/post-B′ kinds directly via bindExecutionKind.
  static ExecutionKind toExecutionKind_(FixedChainRole role) {
    switch (role) {
      case FixedChainRole::kVcoA:       return ExecutionKind::kVcoA;
      case FixedChainRole::kVcoB:       return ExecutionKind::kVcoB;
      case FixedChainRole::kDrone:      return ExecutionKind::kDroneBank;
      case FixedChainRole::kExtIn:      return ExecutionKind::kExtIn;
      case FixedChainRole::kPreamp:     return ExecutionKind::kPreamp;
      case FixedChainRole::kEnvFollower:return ExecutionKind::kEnvFollower;
      case FixedChainRole::kMixer:      return ExecutionKind::kMixer;
      case FixedChainRole::kVcf:        return ExecutionKind::kVcf;
      case FixedChainRole::kDistortion: return ExecutionKind::kDistortion;
      case FixedChainRole::kNone:       return ExecutionKind::kUnsupported;
    }
    return ExecutionKind::kUnsupported;
  }
  // ExecutionKind -> FixedChainRole (for the chainExecRoleAt() inspector). The merged
  // kVcfPath reads as the VCF role (a synthetic-only inspector never sees it; the
  // canonical ordering oracle uses the slots directly). kUnsupported reads kNone.
  static FixedChainRole fixedChainRoleOf_(ExecutionKind kind) {
    switch (kind) {
      case ExecutionKind::kVcoA:       return FixedChainRole::kVcoA;
      case ExecutionKind::kVcoB:       return FixedChainRole::kVcoB;
      case ExecutionKind::kDroneBank:  return FixedChainRole::kDrone;
      case ExecutionKind::kExtIn:      return FixedChainRole::kExtIn;
      case ExecutionKind::kPreamp:     return FixedChainRole::kPreamp;
      case ExecutionKind::kEnvFollower:return FixedChainRole::kEnvFollower;
      case ExecutionKind::kMixer:      return FixedChainRole::kMixer;
      case ExecutionKind::kVcfPath:    return FixedChainRole::kVcf;  // merged reads as VCF.
      case ExecutionKind::kVcf:        return FixedChainRole::kVcf;
      case ExecutionKind::kDistortion: return FixedChainRole::kDistortion;
      case ExecutionKind::kUnsupported:return FixedChainRole::kNone;
    }
    return FixedChainRole::kNone;
  }

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
  // CLASSIC drone group CV bindings (batch 4A): env_out jacks the product writes virtual
  // volts to (per group), cv_mod_in jacks the product reads as the group's shared CV MOD.
  // The EXPLICIT envOutBound_/cvModInBound_ flags are the authoritative post-admission
  // state (a JackId{0} sentinel alone is NOT the bound test — id 0 is a real vco_a.cv_in
  // jack). A failed atomic admission clears the jack to JackId{0} AND the bound flag.
  JackId envOutJack_[DroneBank::kClassicVoices] = {JackId{0}, JackId{0}, JackId{0}, JackId{0}};
  JackId cvModInJack_[DroneBank::kClassicVoices] = {JackId{0}, JackId{0}, JackId{0}, JackId{0}};
  bool envOutBound_[DroneBank::kClassicVoices] = {false, false, false, false};
  bool cvModInBound_[DroneBank::kClassicVoices] = {false, false, false, false};

  // Fixed-chain role binding (registry semantics).
  FixedRoleBinding roleBindings_[kMaxFixedModules] = {};
  std::uint32_t roleBindingCount_ = 0;

  // Derived (rebuild_, off audio thread): the per-module execution slots (dedup by
  // kind, plan order) + the legacy FixedChainRole inspector array + the break-edge
  // delay lines. Audio path reads these only.
  ExecutionSlot execSlots_[kMaxFixedModules] = {};
  std::uint32_t execSlotCount_ = 0;
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

  // GH#6 VCF→distortion staging gains (calibration trim × identity path-gain micro).
  // Default 1.0 (no staging adjustment) so the runtime is bit-identical to the
  // pre-GH#6 hardware path until configureVcfIdentity is called. Independent per L/R.
  double vcfPathStageL_ = 1.0, vcfPathStageR_ = 1.0;
  bool identityConfigured_ = false;
  // @Codex 7C2 req. 4: an owning definition must EXPLICITLY enable strict binding policy.
  // Default OFF = legacy permissive (synthetic fixtures that bind no kind are exempt and
  // auto-allowed). Only when ON does rebuild() fail-closed with a DISTINCT status:
  // missing_execution_binding (a compiled-region module has NO kind binding) vs
  // unsupported_module (explicitly bound to kUnsupported). setStrictBindings() toggles it.
  bool strictBindings_ = false;
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
