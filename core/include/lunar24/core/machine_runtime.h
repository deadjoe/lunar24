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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <utility>

#include <lunar24/core/control_event.h>
#include <lunar24/core/enums.h>
#include <lunar24/core/device_state.h>
#include <lunar24/core/distortion.h>
#include <lunar24/core/state_disposition.h>  // task #78: the 169 applied_to_dsp table
#include <lunar24/core/drone_bank.h>
#include <lunar24/core/drone_noise.h>
#include <lunar24/core/envelope_follower.h>
#include <lunar24/core/event_timebase.h>
#include <lunar24/core/fm_am.h>
#include <lunar24/core/graph_compiler.h>
#include <lunar24/core/patch_graph.h>
#include <lunar24/core/parameter_smoothing.h>
#include <lunar24/core/polivoks_vcf.h>
#include <lunar24/core/preamp.h>
#include <lunar24/core/sample_hold.h>
#include <lunar24/core/schmitt_osc.h>
#include <lunar24/core/sink_interpret.h>
#include <lunar24/core/envelope_generator.h>
#include <lunar24/core/lfo.h>
#include <lunar24/core/joystick_cv.h>
#include <lunar24/core/five_step_sequencer.h>
#include <lunar24/core/arp_sequencer.h>
#include <lunar24/core/keyboard_behaviour.h>
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

// The two real external-input terminals of one rendered frame. They are DISTINCT
// terminals (the GH#4 conflation fix): `extAudio` is the EXT.AUDIO host input and
// drives ONLY mixer ch4; `preamp` is the PREAMP host input and is ONLY the preamp's
// `ext_source_in` fallback when that jack is unpatched. There is no shared variable
// and no implicit copy between them — a caller must set both explicitly (setting the
// same value on both is an explicit, documented compatibility baseline, never a
// default). Fixed-size value type: no heap, trivially copyable.
struct RuntimeInputs {
  double extAudio = 0.0;  // EXT.AUDIO terminal -> mixer ch4 (virtual volts).
  double preamp = 0.0;    // PREAMP terminal -> preamp.ext_source_in fallback (volts).
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
  // Control sources (@Codex D1): the six always-execute panel
  // control sources. A/B dispatch is by slot.id inside step_ (never a FixedChainRole),
  // and they are NEVER a second source loop outside processFrame(). Each is admitted as
  // an explicit always-execute source in the compile plan even with no cable (per-sample
  // LFO / EG-SELF-GEN / PULSER phase continuity).
  kEnvelope,   // envelope_a/b: A/R/D/S + HOLD/SELF-GEN, gate_in resolve -> env/vca cv publish.
  kLfo,        // lfo_a/b: no-input source, tick once/sample, publish cv_out (0..+10V).
  kJoystick,   // joystick: stateless X/Y+offset, read + publish x/y (±10V).
  kSequencer,  // five-step seq: ext_clock_in -> rising to core, publish cv/gate/clock_out.
  // GH#12 keyboard product owner: the keyboard module is now a REAL executed control
  // source (no longer kUnsupported). It receives canonical note ControlEvents
  // (pitch/pressure/gate_on/gate_off/reset) via applyControlEvent_ -> the in-owner
  // ArpSeq (default Keyboard mode = transparent pass-through) -> KeyboardBehaviour, and
  // per-sample ticks the behaviour to publish note CV -> keyboard_v_oct_out and the
  // engaged gate (0/+10V rail) -> keyboard_gate_left_main_out. The four keyboard
  // normalized routes (pitch/gate -> VCO A/B + EG A/B) are flipped kActive so the
  // PatchGraph consumes them, and the module is always-executed so its glide advances
  // every sample even unwired.
  kKeyboard,   // keyboard: note ControlEvents -> note CV/gate publish.
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

// ---------------------------------------------------------------------------
// Exact-feedback-pair value selection (task#65 correction 6).
// ---------------------------------------------------------------------------
// A consuming sink reads its incoming value from exactly ONE of two places, and the
// discriminator is EXACT (source,sink) edge identity — never source alone:
//   * a sink whose (src,sink) IS a selected feedback edge reads that edge's OWN
//     D-sample delay line (the graph_compiler.h consume-rule — off by one vs a
//     live-last-written read; design/07 §4);
//   * any other fed sink reads the live source bank (the value a control producer
//     published THIS frame).
// The historical defect the primitive pins is SOURCE-ONLY matching (any feedback line
// whose sourceJack == src, ignoring the sink): it makes a same-source NORMAL downstream
// read a delay line it does not own, so its value arrives one sample late. The match and
// the value lookup therefore BOTH live inside this single primitive — it receives the
// query (src,sink) and a candidate line's (src,sink), decides exact vs not, and returns
// {matched, value}. The runtime's feedback scan is the only other participant, so this is
// the executor's ONE delayed-vs-live decision point with no second bank or test seam. It
// stays pure (no runtime state), so it is directly assertable with distinct `delayed` !=
// `live` and a D>1-shaped feedback line, and removing the sink comparison makes a
// same-source/different-sink candidate return matched=true with the delayed value.
struct FeedbackResolve {
  bool matched;  // true only when the query (source,sink) exactly equals a line's pair.
  double value;  // the delayed D-sample read when matched, else the live source value.
};

inline FeedbackResolve feedbackSinkValue(JackId querySrc, JackId querySink,
                                         JackId candSrc, JackId candSink, double delayed,
                                         double live) {
  const bool exact = (querySrc == candSrc && querySink == candSink);
  return exact ? FeedbackResolve{true, delayed} : FeedbackResolve{false, live};
}

// Outcome of one control-value transfer into the six control-source parameters
// (@Codex BLOCKED #1). Discriminates the cases that `setControlParamValue`'s old
// bool collapsed — an applied value, a recognised-but-blocked (currently no param uses
// `transfer_unavailable` after the 7C3 pulser ruling), and a malformed value / unknown id.
// The runtime records the most recent transfer's id+status so the product surface (and a
// test oracle) can read back the real applied state without a separate shadow param bank.
enum class ParameterApplyStatus : std::uint8_t {
  applied,               // unit-domain-valid value admitted and the sound-core setter accepted it.
  transfer_unavailable,  // recognised but deliberately UNMAPPED (no current param uses this after 7C3).
  invalid_value,         // malformed for its unit domain (out-of-range / non-exact / non-finite): kept old.
  unsupported_parameter, // not a known control-source param: no transfer.
  apply_count_mismatch,  // exactly-169 gate: the whole applied_to_DSP batch did not reach the
                         // contract count (a mutated path that skipped an id), NOT a per-value
                         // defect — firstFailId pinpoints the first skipped id.
};

// GH#21 continuous control smoothing time constant (design/07 §3.2: knob/joystick/MIDI CC
// targets move in SECONDS toward the new value, not in one sample). A SINGLE named global
// constant, not a buried magic number (contract hard-req #2). PROVISIONAL software policy:
// this is a knob-automation one-pole value, NOT a measured hardware response — there is no
// Solar 42N panel evidence for a knob slew time, so it stays a documented model. 50 ms is a
// typical production control-smoothing sigma (≈0.23 s to 99%); a future measurement only has
// to relabel this line, never re-derive a curve elsewhere.
inline constexpr double kGh21SmoothingTauSeconds = 0.050;
// Upper bound on the ACTIVE seconds-smoothed set. The runtime derives the live smoothed set
// from the registry at construction (Smoothing::seconds && applied_to_dsp), and that set must
// not overflow this fixed array. Surface-1 contributed 20 continuous control-source params;
// Surface-2 adds the 16 vco/vcf panel-knob seconds params -> 36 total. The array stays
// zero-alloc / no map, so the bound must be a known-at-compile-time ceiling >= 36; 40 leaves
// headroom for a future registry addition without going back and growing a fixed array.
// controlSmoothers_ (below) is indexed by full ParameterId ordinal (kParameterIdSpace), so it
// needs no such cap — only this ordinals list does.
inline constexpr std::uint32_t kMaxControlSmoothParams = 40;

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
  // MOD knob -> audio-oscillator modulation depth. depth = modNorm * kModDepthFromNorm
  // (linear, kModDepthFromNorm = 1.0). PROVISIONAL: the norm->depth model is software
  // (no manual/DSP circuit evidence), like the pulser model. GH#15 D1.
  static constexpr double kModDepthFromNorm = 1.0;
  // GH#15 D3: DIVIDER knob (drone_3/6.divider, norm [0,1]) -> S&H clock division ratio.
  // divN = 1 + (kNewDroneDivMax-1)*norm (linear). The divided LF square drives the S&H
  // clock socket (the voice comment at the SAndHold member: "LF/mod->clock"), so the S&H
  // captures the noise once every divN LF cycles. PROVISIONAL: the max ratio is a
  // software model (no manual/DSP circuit evidence for the norm->divisor model).
  static constexpr double kNewDroneDivMax = 16.0;
  // Voice-6 (drone 6) seed mix, so the two Papa Srapa voices are independent.
  static constexpr std::uint64_t kNewSndSeed6Xor = 0x9E3779B97F4A7C15ULL;
  // Per-source sub-seed mix so the audio/LF/FmAm/noise stems within one voice are
  // independent draws from the shared voice seed.
  static constexpr std::uint64_t kNewSourceSeedMix = 0x2595DB9F3D276D2BULL;
  // PITCH-position -> semitone mapping (0 = tone off / pure-noise recipe, 1 = max
  // pitch). PROVISIONAL (pitch range = C0..E7 per manual, not numerically bound).
  static constexpr double kNewPitchMinSt = 0.0;
  static constexpr double kNewPitchMaxSt = 24.0;
  // RANGE selector (hi/low) -> semitone band offset. hi (0, default) = 0 offset
  // (bit-identical to pre-D2); low (1) shifts the whole PITCH band this many semitones
  // DOWN (a 2-octave product-range shift). PROVISIONAL: a software model (the manual
  // gives no numeric RANGE shift; no circuit/DSP evidence), flagged like the pulser.
  // Solo-implementation note: the acceptance proves the LOW position against this
  // single constant, so calibrating it later needs only one edit.
  static constexpr double kNewDroneRangeLowShiftSt = -24.0;
  // RATE SWITCH selector (off/on) -> LF-square modulator frequency multiplier. off
  // (0, default) = x1 (LF keeps running at the RATE-knob-derived frequency — it NEVER
  // stops, preserving the FM/AM default behavior); on (1) = x2 speed. PROVISIONAL: a
  // software model (no manual number for the "switch" ratio; no evidence). GH#15 D2.
  static constexpr double kNewDroneRateSwitchMult = 2.0;
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

  // ---- control-source READ surface (public; @Codex BLOCKED #1/#4) ----
  // The 34-dispatch readback, the most-recent apply status, and the sequencer direct-Hz
  // surface are PUBLIC so a product path / test oracle can read the REAL applied DSP state
  // from the six instances (const/no-alloc, never a shadow param bank) and can drive the
  // sequencer's internal PULSER with a direct DSP-domain Hz (not a ParameterId transfer).
  ParameterApplyStatus lastApplyStatus() const { return lastApplyStatus_; }
  ParameterId lastApplyParamId() const { return lastApplyParamId_; }

  // @Codex #3 (BLOCKED): the six control-source instances are surfaced as an INVARIANT,
  // READ-ONLY view. Configuration goes through the public parameter-event path
  // (setControlParamValue / enqueueControlEvent) or the single sanctioned DSP-domain
  // setter setSequencerInternalRateHz(); the mutating non-const accessors were the seam
  // a test could (and did) use to bypass the runtime dispatch. Const-only readback keeps
  // the "the DEFINITION owns the sources, the runtime dispatches into them" contract.
  const EnvelopeGenerator& envelopeA() const { return envGenA_; }
  const EnvelopeGenerator& envelopeB() const { return envGenB_; }
  const Lfo& lfoA() const { return lfoA_; }
  const Lfo& lfoB() const { return lfoB_; }
  const JoystickCv& joystick() const { return joystick_; }
  const FiveStepSequencer& sequencer() const { return sequencer_; }
  // sequencer.pulser is a DOMAIN-VALIDATED provisional transfer (@Codex 7C3); the direct
  // DSP-domain rate setter below remains an independent fail-closed surface (not a
  // ParameterId transfer). Report the real applied internal rate Hz.
  bool setSequencerInternalRateHz(double hz) { return sequencer_.setInternalRateHz(hz); }
  double sequencerInternalRateHz() const { return sequencer_.internalRateHz(); }

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
        distortion_(sampleRate),
        envGenA_(sampleRate),
        envGenB_(sampleRate),
        lfoA_(sampleRate),
        lfoB_(sampleRate) {
    vcf_.setSampleRate(sampleRate);
    // FiveStepSequencer has only a default ctor (no sampleRate overload), so set the
    // rate in the body like vcf_. JoystickCv is stateless (no sampleRate at all).
    sequencer_.setSampleRate(sampleRate);
    // GH#12 keyboard product owner: configure the in-owner ArpSeq (default Keyboard
    // mode = transparent pass-through) and the KeyboardBehaviour (default params =
    // microtonal passthrough, portamento legato=false/vibrato off) with the machine's
    // sample rate. They are only ticked when a kKeyboard slot is in the compiled plan
    // (canonical definition active routes + always-execute), so an unbound fixture that
    // never ticks them remains untouched. Default params are safe for both.
    keyboardArpSeq_.configure(ArpSeqParams{}, sampleRate);
    keyboardBeh_.configure(KeyboardBehaviourParams{}, sampleRate);

    // GH#21 continuous control smoothing: build the ACTIVE smoothing set from the registry
    // (params whose `smoothing == Smoothing::seconds` AND `disposition == applied_to_dsp`),
    // and prime each smoother to the DSP core's CURRENT value so the first target transition
    // interpolates from where the machine actually is (zero single-sample step). The sound-core
    // default values are documented LOCAL-SAFE defaults (NOT necessarily the registry `initial`),
    // so priming from the live getter — never the registry initial — is the invariant that
    // guarantees zero-jump. Kernel is fixed at construction (sampleRate is ctor-only), so the
    // smoothers never recompute mid-stream and the per-sample sequence is partition-independent.
    controlSmoothOrdinalCount_ = 0;
    for (const ParameterDescriptor& d : registry::kParameters) {
      if (!isContinuousSmoothingParam_(d.id)) continue;  // Smoothing::seconds && applied_to_dsp
      if (controlSmoothOrdinalCount_ >= kMaxControlSmoothParams) continue;  // defensive cap
      controlSmoothOrdinals_[controlSmoothOrdinalCount_++] = d.id;
      ParameterSmoother& sm = controlSmoothers_[static_cast<std::uint32_t>(d.id)];
      sm.setTimeConstantSeconds(kGh21SmoothingTauSeconds);
      sm.setSampleRate(sampleRate);
      sm.reset(currentControlParam_(d.id));  // current DSP value, not registry initial
    }
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
  // Generic CV input bindings (vco_a.cv_in / vco_b.cv_in, -5..+5) — a SECOND independent
  // CV/transfer path on each VCO, SEPARATE from V/OCT (vco_a.v_oct_in / vco_b.v_oct_in).
  // vco_b.cv_in is fed by the A->B route (vco_a.dry_out -> vco_b.cv_in in the default
  // registry, task #83), so the canonical definition binds it here; the step_ kVcoB slot
  // resolves it through setCvInput(v, held mode) — never through the confirmed V/OCT setVoct
  // path (@Codex 7C2: two independent bindings/transfers). JackId{0} is a REAL jack
  // (vco_a.cv_in), so the *_Bound_ flag is the authoritative admission state, not a
  // JackId{0} sentinel (drone ENV/CV-MOD pattern).
  void setVcoCvBindings(JackId aCv, JackId bCv) {
    cvInA_ = aCv; cvInB_ = bCv;
    cvInBoundA_ = true; cvInBoundB_ = true;
  }
  // VCO output jacks the product publishes as a source. vco_b.vco_out is VCO-B's output
  // jack, so the canonical definition binds it and the VCO-B slot publishes it through the
  // ONE write (any downstream — a normal consumer or a user-established feedback edge — then
  // reads that frame's value). Unbound = no publish (legacy synthetic fixture).
  void setVcoOutBindings(JackId aOut, JackId bOut) {
    vcoAOut_ = aOut; vcoBOut_ = bOut;
    vcoAOutBound_ = true; vcoBOutBound_ = true;
  }
  // Runtime-held lin/exp mode for each VCO's GENERIC CV input (cv_in transfer is UNKNOWN;
  // the scaling law is a PROVISIONAL modeling choice). The mode is a runtime decision the
  // tests choose explicitly — never a hardcoded law in the executor. Independent per side.
  void setVcoControlModes(VcoControlMode aMode, VcoControlMode bMode) {
    cvModeA_ = aMode; cvModeB_ = bMode;
  }
  // CV AMT knob depth per VCO (the registry's vco_a_cv_amt / vco_b_cv_amt, "CV AMT",
  // 0..1, default 1). This is the depth of the GENERIC CV (cv_in) contribution separate
  // from the confirmed V/OCT law — the mechanism that can neutralize a mod contribution
  // (e.g. the VCO-B self-edge) to zero WITHOUT removing the route/binding/or executor
  // slot (the self-edge stays in the graph and still executes; only the contributed
  // modulation is scaled to zero). Independent per side. Not one of the frozen
  // six-control-source 35 params; does not change spec/generated semantics.
  void setVcoCvAmounts(double aAmt, double bAmt) {
    vcA_.setCvAmt(aAmt);
    vcB_.setCvAmt(bAmt);
    cvAmtA_ = aAmt; cvAmtB_ = bAmt;
  }
  // Per-side VCO panel knobs (the registry's vco_a_* / vco_b_*). VCO A and B are two
  // independent Vco instances, so a single-param apply reaches exactly ONE side (never
  // both). tune is oct [-1,+1]; morph 0..1; pw duty in (0,1); oct_sel index 0..2;
  // sub_sel index 0..1; cv_amt 0..1 (updates the mirror the executor's kVco step reads
  // through setVcoCvAmounts); lin_exp is the generic-CV mode the kVcoA/kVcoB step
  // consumes directly (task #78: one param -> one side).
  void setVcoATune(double oct) { vcA_.setTune(oct); }
  void setVcoBTune(double oct) { vcB_.setTune(oct); }
  void setVcoAMorph(double m) { vcA_.setMorph(m); }
  void setVcoBMorph(double m) { vcB_.setMorph(m); }
  void setVcoAPw(double duty) { vcA_.setShape(duty); }
  void setVcoBPw(double duty) { vcB_.setShape(duty); }
  void setVcoAOctSelect(int idx) { vcA_.setOctaveSelect(idx); }
  void setVcoBOctSelect(int idx) { vcB_.setOctaveSelect(idx); }
  void setVcoASubSelect(int idx) { vcA_.setSubSelect(idx); }
  void setVcoBSubSelect(int idx) { vcB_.setSubSelect(idx); }
  void setVcoACvAmt(double amt) { vcA_.setCvAmt(amt); cvAmtA_ = amt; }
  void setVcoBCvAmt(double amt) { vcB_.setCvAmt(amt); cvAmtB_ = amt; }
  void setVcoAControlMode(VcoControlMode m) { cvModeA_ = m; }
  void setVcoBControlMode(VcoControlMode m) { cvModeB_ = m; }
  // Readback: the applied per-side knob positions (post-clamp where the setter clamps).
  double vcoATune() const { return vcA_.tune(); }
  double vcoBTune() const { return vcB_.tune(); }
  double vcoAMorph() const { return vcA_.morph(); }
  double vcoBMorph() const { return vcB_.morph(); }
  double vcoAPw() const { return vcA_.shape(); }
  double vcoBPw() const { return vcB_.shape(); }
  int vcoAOctSelect() const { return vcA_.octaveSelect(); }
  int vcoBOctSelect() const { return vcB_.octaveSelect(); }
  int vcoASubSelect() const { return vcA_.subSelectIndex(); }
  int vcoBSubSelect() const { return vcB_.subSelectIndex(); }
  double vcoACvAmt() const { return vcA_.cvAmt(); }
  double vcoBCvAmt() const { return vcB_.cvAmt(); }
  VcoControlMode vcoAControlMode() const { return cvModeA_; }
  VcoControlMode vcoBControlMode() const { return cvModeB_; }
  void setVcfCvBindings(JackId cvL, JackId cvR) { vcfCvL_ = cvL; vcfCvR_ = cvR; }
  // Which patch jack is the preamp's external audio input (the break sink of the
  // env_follower cycle) and which is the env_follower's env_out (the break source).
  void setPreampExtIn(JackId j) { preampExtIn_ = j; }
  void setEnvFolOut(JackId j) { envFolOut_ = j; }

  // ---- CONTROL-SOURCE BINDINGS (@Codex D1/D2) ----
  // Jacks the six always-run control sources READ (resolved through the single sink
  // resolver) and WRITE (published through the single source bank). Unbound = no resolve /
  // no publish (legacy synthetic fixture). A/B dispatch in step_ is by slot.id.
  void setEnvelopeBindings(JackId gateA, JackId envA, JackId vcaA,
                           JackId gateB, JackId envB, JackId vcaB) {
    gateInA_ = gateA; envOutA_ = envA; vcaOutA_ = vcaA;
    gateInB_ = gateB; envOutB_ = envB; vcaOutB_ = vcaB;
  }
  void setLfoBindings(JackId aOut, JackId bOut) { lfoAOut_ = aOut; lfoBOut_ = bOut; }
  void setJoystickBindings(JackId xOut, JackId yOut) { joyXOut_ = xOut; joyYOut_ = yOut; }
  void setSequencerBindings(JackId extClockIn, JackId cvOut, JackId gateOut, JackId clockOut) {
    seqExtClockIn_ = extClockIn; seqCvOut_ = cvOut; seqGateOut_ = gateOut; seqClockOut_ = clockOut;
  }
  // GH#12 keyboard product owner: the note-CV and gate output jacks the keyboard module
  // publishes as a source. The canonical definition binds the real generated jacks
  // keyboard.v_oct_out (V/OCT 0..8V) and keyboard.gate_left_main_out (GATE, 0/+10V rail).
  void setKeyboardBindings(JackId vOctOut, JackId gateOut) {
    kbdVOctOut_ = vOctOut; kbdGateOut_ = gateOut;
  }
  // The plan must ALWAYS execute these sources even when they carry no cable (per-sample
  // LFO / EG-SELF-GEN / PULSER phase continuity). Admission happens INSIDE compile_graph
  // (same compiler plan — never a plan-external补跑 nor a runtime slot pre-append): the
  // ids are force-included in the compiler's active-module set, so an unwired source still
  // becomes an isolated executable region. The owning definition passes the six ids.
  // Always-execute admission (@Codex BLOCKED #5): atomic/fail-closed. Returns false and
  // makes NO change when the request is malformed (null ids with count>0, over-capacity, or
  // a duplicate id) — never a silent truncate and never a deref-crash on ids==null. On a
  // genuinely NEW list it commits AND marks the plan dirty so a subsequent rebuild()
  // actually re-compiles (a change must not short-circuit to graph_unchanged). An identical
  // list is a no-op (returns true, no dirty — the plan already reflects it).
  bool setAlwaysExecute(const ModuleId* ids, std::uint32_t count) {
    if (count > kMaxAlwaysExecuteSources) return false;  // over-capacity: no silent truncate.
    if (count > 0 && ids == nullptr) return false;       // null with count: no deref.
    for (std::uint32_t i = 0; i < count; ++i) {
      for (std::uint32_t j = i + 1; j < count; ++j)
        if (ids[i] == ids[j]) return false;              // duplicate: ambiguous ownership.
      // @Codex #5: an id that is NOT a real module in the owning definition must be refused
      // at the admission boundary (before commit/dirty), never accepted and later swallowed
      // by compile_graph. A ModuleId{999} list used to return true here.
      bool found = false;
      for (std::uint32_t k = 0; k < moduleCount_; ++k)
        if (modules_[k].id == ids[i]) { found = true; break; }
      if (!found) return false;
    }
    if (count == alwaysExecCount_) {                     // same length: no dirty on a no-op.
      bool same = true;
      for (std::uint32_t i = 0; i < count; ++i)
        if (alwaysExecIds_[i] != ids[i]) { same = false; break; }
      if (same) return true;
    }
    alwaysExecCount_ = count;
    for (std::uint32_t i = 0; i < count; ++i) alwaysExecIds_[i] = ids[i];
    graphDirty_ = true;
    return true;
  }

  // MANUAL (panel-control) setters for the WET chain — the actual knobs a host applies
  // from the DeviceState parameters. These are the USER's knob positions and are kept
  // independent from the identity profile: the profile (configureVcfIdentity) carries
  // the per-machine L/R micro-difference, while these set the shared user controls. A
  // host can (and does) set them before/after config without disturbing the identity.
  void setVcfFreq(int ch, double freq) { vcf_.setFreq(ch, freq); }
  void setVcfRes(int ch, double res)   { vcf_.setRes(ch, res); }
  void setVcfMode(int ch, bool bp)     { vcf_.setMode(ch, bp); }
  void setVcfMod(int ch, double mod)   { vcf_.setMod(ch, mod); }
  void setVcfLink(bool on)             { vcf_.setLink(on); }
  void setDistortion(double dist, double gain) {
    distortion_.setDist(dist);
    distortion_.setGain(gain);
  }
  // INDEPENDENT single-parameter distortion controls (task #78): one DeviceState param
  // must touch exactly ONE knob — vcf_dist -> DIST amount, vcf_gain -> output gain — never
  // the combined setDistortion(dist,gain) which would clobber the sibling.
  void setDistortionAmount(double dist) { distortion_.setDist(dist); }
  void setDistortionGain(double gain)   { distortion_.setGain(gain); }

  // Panel-control READBACK (task #78): the applied VCF per-channel knobs and the
  // two-way selection / LINK, plus the distortion amount+gain, read from the same DSP
  // members the render path consumes (never a shadow mirror). modeIsBp: true=bandpass,
  // false=lowpass (the position selection). L/R independent.
  double vcfFreq(int ch) const { return vcf_.freq(ch); }
  double vcfRes(int ch) const { return vcf_.res(ch); }
  double vcfMod(int ch) const { return vcf_.mod(ch); }
  bool vcfBp(int ch) const { return vcf_.modeIsBp(ch); }
  bool vcfLink() const { return vcf_.link(); }
  double distortionAmount() const { return distortion_.dist(); }
  double distortionGain() const { return distortion_.gain(); }

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
  // amount. MOD (D1) scales the LF square onto the audio oscillator; DIVIDER (D3) is
  // the S&H clock division ratio (the S&H captures noise once per divN_ LF edges). The
  // GATE/HOLD, ATT/RLS and env-out sources remain PROVISIONAL (see 00-status).
  // These forward directly to the NEW sources, read every frame by step_(kDrone).
  void setDrone3Pitch(double pct) { pv3_.setPitch(pct); }
  void setDrone3Rate(double hz) { pv3_.setRate(hz); }
  void setDrone3Fm(bool on) { pv3_.setFm(on); }
  void setDrone3Am(bool on) { pv3_.setAm(on); }
  void setDrone3Noise(double amp) { pv3_.setNoise(amp); }
  void setDrone3Divider(double norm) { pv3_.setDivider(norm); }
  // GH#15 D1 (mod knob): norm [0,1] -> audio-oscillator modulation depth = modNorm
  // (linear, marked PROVISIONAL below as kModDepthFromNorm). Registry-AGREEING unit:
  // both the registry unit and the setter take norm 0..1, so no invented scale.
  void setDrone3Mod(double depth) { pv3_.setMod(depth); }
  void setDrone6Pitch(double pct) { pv6_.setPitch(pct); }
  void setDrone6Rate(double hz) { pv6_.setRate(hz); }
  void setDrone6Fm(bool on) { pv6_.setFm(on); }
  void setDrone6Am(bool on) { pv6_.setAm(on); }
  void setDrone6Noise(double amp) { pv6_.setNoise(amp); }
  void setDrone6Divider(double norm) { pv6_.setDivider(norm); }
  void setDrone6Mod(double depth) { pv6_.setMod(depth); }
  // GH#15 D2 (RANGE / RATE SWITCH selectors, both Papa Srapa voices). Selector index
  // 0/1 (the batch lane validates it via dspParamValid_ before the switch; the live
  // lane forwards the ControlEvent value). Both default positions are bit-identical to
  // pre-D2, so a default state reproduces the shipped sound exactly.
  void setDrone3HiLow(int sel) { pv3_.setRangeHiLow(sel); }
  void setDrone3RateSwitch(int sel) { pv3_.setRateSwitch(sel); }
  void setDrone6HiLow(int sel) { pv6_.setRangeHiLow(sel); }
  void setDrone6RateSwitch(int sel) { pv6_.setRateSwitch(sel); }

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
  // ---------------------------------------------------------------------------
  // task #78 panel-control READBACK for the whole applied_to_dsp set. These read the
  // real DSP members the render path consumes (never a shadow mirror), so a product
  // oracle can verify a state restore truly reached this instance. Classic drones are
  // addressed by (voiceGroup 0..3, gen 0..4) -> DroneBank flat index via flatGen_;
  // new drones (PapaVoice 3/6) by their own knob/state getters. Out-of-range -> neutral.
  double droneFreqBaseHz(int voiceGroup, int gen) const {
    return inDroneRange_(voiceGroup, gen) ? drone_.freqBaseHz(flatGen_(voiceGroup, gen)) : 0.0;
  }
  bool droneMuted(int voiceGroup, int gen) const {
    return inDroneRange_(voiceGroup, gen) && drone_.mutedOf(flatGen_(voiceGroup, gen));
  }
  double droneTuneSemis(int voiceGroup, int gen) const {
    return inDroneRange_(voiceGroup, gen) ? drone_.tuneOf(flatGen_(voiceGroup, gen)) : 0.0;
  }
  double droneModAmount(int voiceGroup, int gen) const {
    return inDroneRange_(voiceGroup, gen) ? drone_.modAmountOf(flatGen_(voiceGroup, gen)) : 0.0;
  }
  double droneVoltSemisDown(int voiceGroup) const {
    // VOLT is a SHARED group transpose: setVolt(group, semis) writes all 5 gens of the group
    // (gen index group*5..group*5+4), so the group's single value is read back as gen 0 of the
    // group via flatGen_ — the same (voiceGroup, gen) addressing every other classic inspector
    // uses. Reading voltOf(voiceGroup) directly would index the WRONG group's per-gen slot.
    return inDroneRange_(voiceGroup, 0) ? drone_.voltOf(flatGen_(voiceGroup, 0)) : 0.0;
  }
  bool droneGroupGate(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices && drone_.groupGate(voiceGroup);
  }
  bool droneGroupHold(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices && drone_.groupHold(voiceGroup);
  }
  double droneGroupAttSeconds(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices
               ? drone_.groupAttSeconds(voiceGroup) : 0.0;
  }
  double droneGroupRlsSeconds(int voiceGroup) const {
    return voiceGroup >= 0 && voiceGroup < kClassicDroneVoices
               ? drone_.groupRlsSeconds(voiceGroup) : 0.0;
  }
  // NEW drone (PapaVoice) per-knob readback: RATE = LF square modulator effective Hz
  // (0 = stop), PITCH = core audio effective Hz (0..), FM/AM = factory switches (state),
  // NOISE = mix amplitude. Independent for drone 3 and drone 6.
  double drone3RateHz() const { return pv3_.rateHz(); }
  double drone3PitchHz() const { return pv3_.pitchHz(); }
  bool drone3Fm() const { return pv3_.fmOn(); }
  bool drone3Am() const { return pv3_.amOn(); }
  double drone3NoiseAmp() const { return pv3_.noiseAmp(); }
  // GH#15 D1: MOD = applied audio-oscillator modulation depth (modNorm, linear). Reads the
  // REAL PapaVoice field the render path consumes (modApplied()), not a shadow bank.
  double drone3ModApplied() const { return pv3_.modApplied(); }
  // GH#15 D3: DIVIDER = the S&H clock division ratio the render path actually drives
  // (divN_ = 1 + (kNewDroneDivMax-1)*norm). Reads the real PapaVoice field, like the
  // pitch/rate getters, so the panel knob -> divided-clock link is observable.
  double drone3Divider() const { return pv3_.divider(); }
  double drone6RateHz() const { return pv6_.rateHz(); }
  double drone6PitchHz() const { return pv6_.pitchHz(); }
  bool drone6Fm() const { return pv6_.fmOn(); }
  bool drone6Am() const { return pv6_.amOn(); }
  double drone6NoiseAmp() const { return pv6_.noiseAmp(); }
  double drone6ModApplied() const { return pv6_.modApplied(); }
  double drone6Divider() const { return pv6_.divider(); }
  // Mixer channel VOL/PAN wrappers + readback (0-based channel slot, 1:1 with the
  // registry's mixer_<N> index offset by one). Reuses VoiceMixer's clamp01.
  void setMixerChannelVol(int ch, double v) { mixer_.setChannelVol(ch, v); }
  void setMixerChannelPan(int ch, double p) { mixer_.setChannelPan(ch, p); }
  double mixerChannelVol(int ch) const { return mixer_.channelVol(ch); }
  double mixerChannelPan(int ch) const { return mixer_.channelPan(ch); }
  // Preamp gain (registry's preamp_1, "output gain", normalized 0..1 -> 0..40 dB).
  void setPreampGainNorm(double norm) { preamp_.setGainNorm(norm); }
  double preampGainNorm() const { return preamp_.gainNorm(); }
  // Envelope follower attack/release (registry's env_follower_attack / _release,
  // SECONDS after the lone monotonic norm->seconds transfer — see the helper below).
  void setEnvFollowerAttackSeconds(double s) { envFol_.setAttackSeconds(s); }
  void setEnvFollowerReleaseSeconds(double s) { envFol_.setReleaseSeconds(s); }
  double envFollowerAttackSeconds() const { return envFol_.attackSeconds(); }
  double envFollowerReleaseSeconds() const { return envFol_.releaseSeconds(); }
  // General read of a control generator's resolved CV output (the whole CV source bank).
  double controlVoltageAt(JackId jack) const { return cvAt_(jack); }
  // ---- task #78: full 169-parameter applied_to_DSP apply (commit ②) ----
  // The ONE public apply choke for a whole DeviceState. applyDspParam routes each id:
  //   (1) the 35 control-source params -> either a seconds-smoothed SNAP (Smoothing::seconds)
  //       or setControlParamValue (byte-identical reuse for the non-seconds control-source ids);
  //   (2) the 16 vco/vcf panel-knob seconds params (isContinuousSmoothingParam_) -> the shared
  //       family SNAP (dspParamValid_ FIRST, then reset + applySmoothedControl_), never a ramp;
  //   (3) any non-applied_to_dsp id -> unsupported_parameter;
  //   (4) the remaining 118 applied_to_dsp ids (the former 134 minus the 16 moved to (2)) ->
  //       the explicit dispatch below, after dspParamValid_ admits the state value against its
  //       registry unit (fail-closed keep-old on a malformed value, never a silent setter coercion);
  //   (5) an id none of the above cover -> unsupported_parameter (a fail-closed guard that
  //       makes the batch's "exactly 169" check real, never a silent skip).
  ParameterApplyStatus applyDspParam(ParameterId id, double v) {
    lastApplyParamId_ = id;
    if (controlSourceParamRecognized_(id)) {
      // GH#21 layering (boundary): a whole-state candidate build / reset SNAPS a
      // seconds-smoothed control source to the applied value instead of ramping it.
      // Live knob-automation ControlEvents (setControlParamValue) are the ONLY ramp
      // path (design/07 §3.2); a preset/state restore is the exact stopped-stream
      // initialization and must land at the value immediately, never relax from the
      // old value toward the new one. reset() also sets the smoother current=target
      // so it is settled/inert afterwards — no residual drift onto a later direct
      // setter or a later ramp's starting point.
      const ParameterDescriptor* desc = find_parameter(id);
      if (desc != nullptr && desc->smoothing == Smoothing::seconds) {
        // Fail-closed like setControlParamValue: the snap path must not let a
        // malformed / out-of-domain value reach a sound-core setter. Mirror the
        // live lane's recognized→valid order, so a NaN or out-of-range whole-state
        // apply rejects (keep-old) instead of silently resetting the smoother to
        // an invalid level and calling applySmoothedControl_ with it.
        if (!controlParamValid_(id, v)) {
          lastApplyStatus_ = ParameterApplyStatus::invalid_value;
          return lastApplyStatus_;
        }
        const std::uint32_t ord = static_cast<std::uint32_t>(id);
        controlSmoothers_[ord].reset(v);
        applySmoothedControl_(id, v);
        lastApplyStatus_ = ParameterApplyStatus::applied;
        return lastApplyStatus_;
      }
      return setControlParamValue(id, v);
    }
    // Surface-2: the 16 vco/vcf panel-knob seconds params (isContinuousSmoothingParam_) reach
    // the shared family here. Whole-state apply is snap-only, mirroring the control-source
    // seconds path above (a preset / state restore is the exact stopped-stream initialisation
    // and must land at the value immediately). Validate via dspParamValid_ FIRST — it is the
    // ONLY validator spanning oct [-1,1] (controlParamValid_'s default:return false rejects
    // tune) — then reset + applySmoothedControl_, and NEVER skip the check (fail-closed
    // keep-old on a malformed whole-state value, so the batch "exactly 169" stays honest).
    if (isContinuousSmoothingParam_(id)) {
      if (!dspParamValid_(id, v)) {
        lastApplyStatus_ = ParameterApplyStatus::invalid_value;
        return lastApplyStatus_;
      }
      const std::uint32_t ord = static_cast<std::uint32_t>(id);
      controlSmoothers_[ord].reset(v);
      applySmoothedControl_(id, v);
      lastApplyStatus_ = ParameterApplyStatus::applied;
      return lastApplyStatus_;
    }
    if (disposition_of(id) != StateDisposition::applied_to_dsp) {
      lastApplyStatus_ = ParameterApplyStatus::unsupported_parameter;
      return lastApplyStatus_;
    }
    if (!dspParamValid_(id, v)) {
      lastApplyStatus_ = ParameterApplyStatus::invalid_value;
      return lastApplyStatus_;
    }
    switch (id) {
      // (the vco_a/b tune/morph/pw/cv_amt and vcf_l/r freq/res/mod and vcf dist/gain ids are the
      // 16 Surface-2 panel-knob seconds params — handled by the isContinuousSmoothingParam_ snap
      // branch above, so they no longer fall through to the direct dispatch switch.)
      case ParameterId::vco_a_oct_sel:
        setVcoAOctSelect(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vco_a_sub_sel:
        setVcoASubSelect(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vco_a_lin_exp:
        setVcoAControlMode(v == 1.0 ? VcoControlMode::kExponential : VcoControlMode::kLinear);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vco_b_oct_sel:
        setVcoBOctSelect(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vco_b_sub_sel:
        setVcoBSubSelect(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vco_b_lin_exp:
        setVcoBControlMode(v == 1.0 ? VcoControlMode::kExponential : VcoControlMode::kLinear);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vcf_l_bp_lp:
        setVcfMode(0, v == 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vcf_r_bp_lp:
        setVcfMode(1, v == 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::vcf_link:
        setVcfLink(v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::preamp_gain:
        setPreampGainNorm(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::env_follower_attack:
        setEnvFollowerAttackSeconds(envFollowerSecondsFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::env_follower_release:
        setEnvFollowerReleaseSeconds(envFollowerSecondsFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch1_vol:
        setMixerChannelVol(0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch1_pan:
        setMixerChannelPan(0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch2_vol:
        setMixerChannelVol(1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch2_pan:
        setMixerChannelPan(1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch3_vol:
        setMixerChannelVol(2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch3_pan:
        setMixerChannelPan(2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch4_vol:
        setMixerChannelVol(3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch4_pan:
        setMixerChannelPan(3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch5_vol:
        setMixerChannelVol(4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch5_pan:
        setMixerChannelPan(4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch6_vol:
        setMixerChannelVol(5, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch6_pan:
        setMixerChannelPan(5, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch7_vol:
        setMixerChannelVol(6, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch7_pan:
        setMixerChannelPan(6, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch8_vol:
        setMixerChannelVol(7, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch8_pan:
        setMixerChannelPan(7, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch9_vol:
        setMixerChannelVol(8, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch9_pan:
        setMixerChannelPan(8, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch10_vol:
        setMixerChannelVol(9, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::mixer_ch10_pan:
        setMixerChannelPan(9, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_tune_1:
        setDroneTune(0, 0, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mute_1:
        setDroneMute(0, 0, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mod_1:
        setDroneMod(0, 0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_tune_2:
        setDroneTune(0, 1, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mute_2:
        setDroneMute(0, 1, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mod_2:
        setDroneMod(0, 1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_tune_3:
        setDroneTune(0, 2, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mute_3:
        setDroneMute(0, 2, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mod_3:
        setDroneMod(0, 2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_tune_4:
        setDroneTune(0, 3, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mute_4:
        setDroneMute(0, 3, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mod_4:
        setDroneMod(0, 3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_tune_5:
        setDroneTune(0, 4, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mute_5:
        setDroneMute(0, 4, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_mod_5:
        setDroneMod(0, 4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_volt:
        setDroneVolt(0, classicDroneVoltSemisDownFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_att:
        setDroneGroupAtt(0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_rls:
        setDroneGroupRls(0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_1_gate_hold:
        setDroneGroupHold(0, v == 1.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_tune_1:
        setDroneTune(1, 0, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mute_1:
        setDroneMute(1, 0, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mod_1:
        setDroneMod(1, 0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_tune_2:
        setDroneTune(1, 1, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mute_2:
        setDroneMute(1, 1, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mod_2:
        setDroneMod(1, 1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_tune_3:
        setDroneTune(1, 2, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mute_3:
        setDroneMute(1, 2, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mod_3:
        setDroneMod(1, 2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_tune_4:
        setDroneTune(1, 3, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mute_4:
        setDroneMute(1, 3, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mod_4:
        setDroneMod(1, 3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_tune_5:
        setDroneTune(1, 4, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mute_5:
        setDroneMute(1, 4, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_mod_5:
        setDroneMod(1, 4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_volt:
        setDroneVolt(1, classicDroneVoltSemisDownFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_att:
        setDroneGroupAtt(1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_rls:
        setDroneGroupRls(1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_2_gate_hold:
        setDroneGroupHold(1, v == 1.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_tune_1:
        setDroneTune(2, 0, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mute_1:
        setDroneMute(2, 0, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mod_1:
        setDroneMod(2, 0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_tune_2:
        setDroneTune(2, 1, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mute_2:
        setDroneMute(2, 1, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mod_2:
        setDroneMod(2, 1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_tune_3:
        setDroneTune(2, 2, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mute_3:
        setDroneMute(2, 2, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mod_3:
        setDroneMod(2, 2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_tune_4:
        setDroneTune(2, 3, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mute_4:
        setDroneMute(2, 3, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mod_4:
        setDroneMod(2, 3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_tune_5:
        setDroneTune(2, 4, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mute_5:
        setDroneMute(2, 4, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_mod_5:
        setDroneMod(2, 4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_volt:
        setDroneVolt(2, classicDroneVoltSemisDownFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_att:
        setDroneGroupAtt(2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_rls:
        setDroneGroupRls(2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_4_gate_hold:
        setDroneGroupHold(2, v == 1.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_tune_1:
        setDroneTune(3, 0, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mute_1:
        setDroneMute(3, 0, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mod_1:
        setDroneMod(3, 0, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_tune_2:
        setDroneTune(3, 1, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mute_2:
        setDroneMute(3, 1, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mod_2:
        setDroneMod(3, 1, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_tune_3:
        setDroneTune(3, 2, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mute_3:
        setDroneMute(3, 2, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mod_3:
        setDroneMod(3, 2, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_tune_4:
        setDroneTune(3, 3, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mute_4:
        setDroneMute(3, 3, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mod_4:
        setDroneMod(3, 3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_tune_5:
        setDroneTune(3, 4, classicDroneTuneSemisFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mute_5:
        setDroneMute(3, 4, v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_mod_5:
        setDroneMod(3, 4, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_volt:
        setDroneVolt(3, classicDroneVoltSemisDownFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_att:
        setDroneGroupAtt(3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_rls:
        setDroneGroupRls(3, v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_5_gate_hold:
        setDroneGroupHold(3, v == 1.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_rate:
        setDrone3Rate(newDroneRateHzFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_pitch:
        setDrone3Pitch(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_noise:
        setDrone3Noise(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_fm:
        setDrone3Fm(v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_am:
        setDrone3Am(v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_mod:
        setDrone3Mod(v * kModDepthFromNorm);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_hi_low:
        setDrone3HiLow(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_rate_switch:
        setDrone3RateSwitch(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_3_divider:
        setDrone3Divider(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_rate:
        setDrone6Rate(newDroneRateHzFromNorm(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_pitch:
        setDrone6Pitch(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_noise:
        setDrone6Noise(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_fm:
        setDrone6Fm(v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_am:
        setDrone6Am(v != 0.0);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_mod:
        setDrone6Mod(v * kModDepthFromNorm);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_hi_low:
        setDrone6HiLow(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_rate_switch:
        setDrone6RateSwitch(static_cast<int>(v));
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::drone_6_divider:
        setDrone6Divider(v);
        lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      default:
        lastApplyStatus_ = ParameterApplyStatus::unsupported_parameter;
        return lastApplyStatus_;
    }
  }

  // Batch-apply EVERY applied_to_dsp parameter (exactly 169) from a validated
  // DeviceStateV1 into the live DSP. On the first non-applied id it returns false and
  // fills firstFailId / firstFailStatus (the typed rejection the candidate builder
  // carries); it applies every id BEFORE the failure and bails immediately, so a failing
  // candidate is discarded whole — it never yields a partial-success path. On full success
  // it returns true (dspApplyOk_ set) and dspAppliedCount_ == 169. An id that is not
  // applied_to_dsp is skipped (out of contract scope), never counted. After the loop it
  // re-verifies with an independent exactly-169 gate (finding 1): if fewer than the
  // contract count was admitted (a mutated path that skipped an id) it returns false with
  // apply_count_mismatch and firstFailId pointing at the first skipped id — never a partial
  // success masquerading as the count==169 sentinel.
  bool applyDspState(const DeviceStateV1& state, ParameterId& firstFailId,
                     ParameterApplyStatus& firstFailStatus) {
    // Sentinel contract (finding 5): firstFail* is ALWAYS fully rewritten before we return, so
    // a prior rejection can never leak into a later success or into a different rejection. On
    // success / skip-only they carry the no-failure sentinel; on a real rejection they carry the
    // FIRST failing id/status.
    firstFailId = static_cast<ParameterId>(kParameterCount);
    firstFailStatus = ParameterApplyStatus::applied;

    // Which applied_to_DSP ids applyDspParam actually admitted, so the exactly-169 gate can
    // LOCATE the first missing id rather than reporting a bare count mismatch (finding 1).
    std::uint32_t appliedBits[(kParameterCount + 31u) / 32u] = {};

    std::uint32_t applied = 0;
    for (std::uint32_t i = 0; i < kDeviceStateDispositionCount; ++i) {
      if (kDeviceStateDisposition[i].disposition != StateDisposition::applied_to_dsp) continue;
      const ParameterId id = kDeviceStateDisposition[i].id;
      const double v = state.parameters[static_cast<std::uint32_t>(id)];
      const ParameterApplyStatus s = applyDspParam(id, v);
      if (s != ParameterApplyStatus::applied) {
        // A per-value defect: this id was rejected by its own unit-domain choke. Report it.
        dspApplyOk_ = false;
        dspAppliedCount_ = applied;
        firstFailId = id;
        firstFailStatus = s;
        return false;
      }
      const std::uint32_t u = static_cast<std::uint32_t>(id);
      appliedBits[u / 32u] |= (1u << (u % 32u));
      ++applied;
    }

    // Exactly-169 product gate (finding 1): the loop must admit EVERY applied_to_DSP id, not just
    // produce a plausible count. A mutated path that skips one (e.g. a stray `continue`) is a
    // REJECTED candidate with the first missing id locatable via the admitted-bit mask — it never
    // hides behind the count==169 success sentinel.
    constexpr std::uint32_t kAppliedToDsp =
        count_disposition(StateDisposition::applied_to_dsp);  // 169, compile-time locked.
    if (applied != kAppliedToDsp) {
      dspApplyOk_ = false;
      dspAppliedCount_ = applied;
      firstFailId = firstUnadmittedAppliedId_(appliedBits);
      firstFailStatus = ParameterApplyStatus::apply_count_mismatch;
      return false;
    }

    dspApplyOk_ = true;
    dspAppliedCount_ = applied;
    // firstFailId / firstFailStatus are already the no-failure sentinel from the entry reset.
    return true;
  }

  // First applied_to_DSP id whose admitted-bit is clear — the missing id an exactly-169
  // count-mismatch rejection reports. Returns the kParameterCount sentinel if none differ (a
  // count gate that cannot happen, but fail-closed so the caller always gets a locatable id).
  ParameterId firstUnadmittedAppliedId_(const std::uint32_t* bits) const {
    for (std::uint32_t i = 0; i < kDeviceStateDispositionCount; ++i) {
      if (kDeviceStateDisposition[i].disposition != StateDisposition::applied_to_dsp) continue;
      const std::uint32_t u = static_cast<std::uint32_t>(kDeviceStateDisposition[i].id);
      if (!(bits[u / 32u] & (1u << (u % 32u)))) return kDeviceStateDisposition[i].id;
    }
    return static_cast<ParameterId>(kParameterCount);
  }

  // task #78: whether the whole 169-parameter apply succeeded and how many were applied.
  // dspApplyOk() is the candidate-builder gate; dspAppliedCount() is a diagnostic equal to
  // count_disposition(applied_to_dsp) on success (169) and partial on a rejection.
  bool dspApplyOk() const { return dspApplyOk_; }
  std::uint32_t dspAppliedCount() const { return dspAppliedCount_; }

  // ---- GH#6 read-only inspectors (executed value, never a shadow mirror) ----
  // Read back the profile the runtime is ACTUALLY executing, straight from the live
  // DSP (PolivoksFilter::inputDrive, Distortion::channelDrive/Rail, and the staging
  // gain step_(kDistortion) multiplies). An inspector that reports a value the DSP
  // does not run is a test-shadow and is caught by the config-oracle (only-change-
  // inspector is red).
  bool vcfIdentityConfigured() const { return identityConfigured_; }
  double vcfInputDrive(int ch) const { return vcf_.inputDrive(ch); }
  // Real-filter CV readback (route.vcf_cv_l_to_cv_r normalling oracle). These return the
  // ACTUAL control volts the PolivoksFilter ran this frame (vcf_.cvL()/cvR() — the DSP
  // state, never a shadow mirror), so a test can pin exactly what the filter executed for
  // the L source and the normalled/overridden R.
  double vcfCvReadbackL() const { return vcf_.cvL(); }
  double vcfCvReadbackR() const { return vcf_.cvR(); }
  // The resolved input the preamp actually ran this frame — the exact value passed to
  // preamp_.tick(in) at the ext_source_in break sink (delayed env when a return cable
  // feeds it, host EXT terminal fallback when unfed). This is the real DSP feed, never a
  // shadow mirror. Two distinct reset semantics, by rebuild type (@Codex ce765d6d fix comment):
  //   * A REAL rebuild (the graph changed, graphDirty_ true): the cycle break edge's own
  //     D-sample line is reset (buf[k]=0, writePos=0), so the FIRST post-RESET frame reads
  //     0.0 — a "reads live" bug would hand the preamp the nonzero env instead.
  //   * A graph_unchanged rebuild (no graph change, cached no-op): the reset is NOT run; the
  //     feedback line PRESERVES the ongoing running state. That preserved value is legitimate
  //     running state, not an erroneous stale value — this keeps phase continuity over a no-op
  //     rebuild, so a stale-looking read here is a real running-state read, not a defect.
  // KNOWN behavior (current rebuild strategy, documented not fixed): because a real rebuild
  // resets EVERY feedback line (not only the changed ones), an unrelated patch that forces a
  // real rebuild can transient the VCO-B self-edge buffer. Recorded as a known behavior; it is
  // NOT expanded into a per-line migration fix here, and no "no transient" claim is made.
  double preampResolvedInput() const { return preampInResolved_; }
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

  // Read-only patch queries (task#80, GH#12 9D C3): forward to the private PatchGraph so the
  // state-aware definition can (a) VERIFY the restored user-cable bank exactly equals the
  // requested cable set — never trusting a lone connect()==true, which can atomically displace a
  // prior requested cable at a saturated source/sink port — and (b) let tests observe the derived
  // override/restore rule. cableCount()/cableCountInto()/cableConnected() count USER cables only
  // (patch_graph.h): a normalized route never consumes user-cable cardinality, and a route is
  // ACTIVE at a sink iff that sink has no user cable (countInto(sink)==0). All are const-read and
  // draw directly from the live patch_ (no recompile, no side effect).
  std::uint32_t cableCount() const { return patch_.cableCount(); }
  bool cableConnected(JackId source, JackId sink) const {
    return patch_.cableConnected(source, sink);
  }
  std::uint32_t cableCountInto(JackId sink) const { return patch_.countInto(sink); }
  bool normalizedActive(JackId source, JackId sink) const {
    return patch_.normalizedActive(source, sink);
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
    compile_invalid_always_execute,  // compile_graph -> invalid_always_execute (admission refused)
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
    // Always-execute sources are admitted INSIDE compile_graph (same plan) so an unwired
    // control source is still compiled into an isolated executable region — never a
    // plan-external pre-append (GH#11). When none are registered (legacy fixtures), the
    // counts are 0 and the plan is unchanged.
    CompileResult r = compile_graph(jacks_, jackCount_, edges_, edgeCount_,
                                    modules_, moduleCount_, fixedEdges_, fixedEdgeCount_,
                                    alwaysExecIds_, alwaysExecCount_);
    if (r.status != CompileStatus::ok) {
      graphValid_ = false;
      rebuildStatus_ = r.status == CompileStatus::cycle_unsafe_module
                           ? RebuildStatus::compile_cycle_unsafe
                           : (r.status == CompileStatus::invalid_always_execute
                                  ? RebuildStatus::compile_invalid_always_execute
                                  : RebuildStatus::compile_invalid_contract);
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
      // never leave a half-built runtime line set behind a valid flag, and never leave a
      // NEW graph_ alongside PARTIAL exec slots / legacy chain order / feedback lines. A
      // failed chain rebuild must read as fully invalid (graph cleared, no slots, no
      // legacy order, no feedback), so a subsequent rebuild() starts from a clean slate.
      graph_ = CompiledGraph{};
      graphValid_ = false;
      graphDirty_ = false;
      execSlotCount_ = 0;
      chainExecCount_ = 0;
      feedbackCount_ = 0;
      rebuildStatus_ = RebuildStatus::feedback_capacity_exceeded;
      return false;
    }
    // Deterministic repatch behaviour: a genuinely rebuilt plan resets the runtime-owned
    // gate/clock interpreter latches (a cable being plugged in must not see a stale
    // "high" from a prior plan, so the first rising edge is never a phantom advance). A
    // no-change rebuild (graph_unchanged early-out) does NOT run this, so phase continuity
    // over a cached rebuild is preserved. (@Codex D3: sink_gate_interpret provisional
    // canonical sink semantics; real-machine-precision is not claimed.)
    sink_gate_reset(envA_gate_);
    sink_gate_reset(envB_gate_);
    sink_gate_reset(seqClockLatch_);
    return true;
  }

  // The product-path render. `inputs.extAudio` is the EXT.AUDIO voltage (drives mixer
  // ch4 ONLY); `inputs.preamp` is the PREAMP terminal (the preamp's ext_source_in
  // fallback ONLY when that jack is unpatched). Two DISTINCT terminals — no shared
  // variable, no implicit copy (a caller must set both explicitly). `driveGraph`
  // selects the CONTROL layer: true = run the compiled graph (product path), false =
  // IGNORE it (the NEGATIVE control for criterion ①). Rendering is independent of
  // block partition.
  RuntimeOutput processFrame(RuntimeInputs inputs, bool driveGraph = true) {
    lastIn_ = inputs;
    for (int i = 0; i < kNumChannels; ++i) chIn_[i] = 0.0;
    // GH#21: advance the continuous-control smoothers BEFORE the graph resolves so the
    // control sources (joystick X/Y, env, lfo, sequencer) publish THIS frame's smoothed value.
    // A target set at the current block's sampleOffset (via setControlParamValue -> setTarget)
    // transitions from the smoother's current state here — one coefficient fraction, no jump.
    advanceControlSmoothing_();
    // Drive the per-module executor: exactly ONE resolve->step->publish per ModuleId, in
    // the compiled plan (region topo) order, with NO ExecutionKind dedup (the contract is
    // one ModuleId one slot; deduping by kind would drop a module). driveGraph=false
    // bypasses the CONTROL layer: slots still run, but no CV sink is resolved from the
    // graph (criterion-① negative).
    for (std::uint32_t i = 0; i < execSlotCount_; ++i) step_(execSlots_[i], driveGraph);
    return RuntimeOutput{wetL_, wetR_, dryA_, dryB_};
  }

  // N-6 frame-structure array contract: inputs points to n RuntimeInputs frames;
  // out points to n writable RuntimeOutput frames (one struct per frame).
  //
  // Block render: advance `n` frames. The per-frame DSP makes this equal a
  // per-block run of the same sequence (partition-invariance, criterion ④).
  //
  // #46: the block FIRST drains due control events from EventTimebase (each resolved
  // to a block-relative sampleOffset), then applies each at the exact frame it was
  // scheduled for, so the same event set acts at the SAME absolute sample under any
  // 64/128/256 (or mixed) block partition. Events are handed out sorted by absolute
  // sample; EventTimebase owns the ordering, this loop only matches offset -> frame.
  void processBlock(const RuntimeInputs* inputs, std::size_t n, RuntimeOutput* out,
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
      out[i] = processFrame(inputs[i], driveGraph);
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
      // GH#15 D2: the two NEW selectors default to their bit-identical positions
      // (hi_low=hi -> rangeBaseSt_=0; rate_switch=off -> rateMult_=1), so this
      // constructor value reproduces the pre-D2 sound exactly.
      baseRateHz_ = kNewDroneLfFreqHz;
      applyRate();
    }
    void setPitch(double pct) {
      pitchSemis_ = pct <= 0.0 ? SchmittOsc::kSilenceSt
                                : kNewPitchMinSt + pct * (kNewPitchMaxSt - kNewPitchMinSt);
      applyPitch();
    }
    void setRate(double hz) { baseRateHz_ = hz; applyRate(); }
    // GH#15 D2 (RANGE selector). Shifts the audible PITCH band by the RANGE offset:
    // hi (0, default) -> 0 (bit-identical to pre-D2); low -> kNewDroneRangeLowShiftSt
    // (2 oct down). PROVISIONAL offset (see constant).
    void setRangeHiLow(int sel) {
      rangeBaseSt_ = (sel == 1) ? kNewDroneRangeLowShiftSt : 0.0;
      applyPitch();
    }
    // GH#15 D2 (RATE SWITCH selector). x1 (off, default) / x2 (on) on the RATE-derived
    // LF frequency. off NEVER stops the LF — the FM/AM modulation the default sound
    // relies on keeps running at the RATE-knob-derived frequency. PROVISIONAL (see const).
    void setRateSwitch(int sel) {
      rateMult_ = (sel == 1) ? kNewDroneRateSwitchMult : 1.0;
      applyRate();
    }
    void applyPitch() {
      // PITCH-at-floor silence is absolute; the RANGE offset only shades an active band
      // (never turns a silent PITCH into an audible tone).
      audio.setPitchSemitones(pitchSemis_ == SchmittOsc::kSilenceSt
                                  ? SchmittOsc::kSilenceSt
                                  : rangeBaseSt_ + pitchSemis_);
    }
    void applyRate() { lf.setFreqHz(baseRateHz_ * rateMult_); }
    void setFm(bool on) { fmOn_ = on; }
    void setAm(bool on) { amOn_ = on; }
    void setNoise(double amp) { noise.setAmplitude(amp); }
    // GH#15 D3 (DIVIDER knob). The lane OWNS the S&H clock source; the old setShClock
    // field-injection seam (a pure test hook) is voided. divN = 1 + (kNewDroneDivMax-1)*norm
    // (linear). Default norm 0.5 -> divN = 8.5: the S&H CV readback (sampleHold*Cv) goes from
    // a constant 0.0 to a stepped noise sequence, but the audio channel is byte-identical
    // (the S&H CV is never summed into *out). PROVISIONAL max (see constant).
    void setDivider(double norm) { divN_ = 1.0 + (kNewDroneDivMax - 1.0) * norm; }
    // MOD knob (GH#15 D1). Depth = modNorm (linear, kModDepthFromNorm): scales the
    // LF-square modulation the audio oscillator consumes. BEFORE the knob was wired
    // tick() fed audio a raw ±1 square (depth 1.0); after wiring it scales by the
    // knob's value, so the DEFAULT drone_3/6 mod depth is 1.0 -> 0.5 (registry default
    // drone_3/6.mod = 0.5). PROVISIONAL: the norm->depth mapping is a software model
    // (no manual/DSP circuit evidence), so it is marked provisional like the pulser.
    void setMod(double depth) { mod_ = depth; }
    // The modulation depth the product path actually drives the audio oscillator with
    // (mod_ is read every tick), a real DSP lever, not a shadow bank.
    double modApplied() const { return mod_; }
    // The S&H level the product path computed last frame (CV out of the voice).
    double lastShCv() const { return shCv_; }
    // Panel-control READBACK (task #78): the NEW-drone knob positions the render path
    // drives from. rateHz/pitchHz read the sub-oscillators' effective frequency (a
    // real DSP value, not a shadow), fmOn/amOn read the switch state, noiseAmp reads
    // the noise source amplitude.
    double rateHz() const { return lf.effectiveFreqHz(); }
    double pitchHz() const { return audio.effectiveFreqHz(); }
    // GH#15 D3: DIVIDER = the S&H clock division ratio divN_ the render path consumes.
    double divider() const { return divN_; }
    bool fmOn() const { return fmOn_; }
    bool amOn() const { return amOn_; }
    double noiseAmp() const { return noise.amplitude(); }
    void tick(double* out) {
      double lv = 0.0;
      lf.tick(&lv);
      const double sq = lf.square();  // ±1 LF-square level (read-only tap).
      // MOD knob: scale the ±1 LF square by the mod depth before feeding the audio
      // oscillator. depth=modNorm linear (kModDepthFromNorm, PROVISIONAL). Default
      // drone_3/6 mod depth changed 1.0 (raw square) -> 0.5 (registry default).
      audio.setMod(sq * mod_);
      audio.setFmDevHz(fmOn_ ? fm.fDevHz() : 0.0);
      audio.setAmDepth(amOn_ ? fm.depth() : 0.0);
      double a = 0.0;
      audio.tick(&a);
      double n = 0.0;
      noise.tick(&n);
      // GH#15 D3: the S&H clock socket is fed by the LF square, edge-count divided by the
      // DIVIDER ratio divN_. The lane owns this clock (the setShClock injection seam is
      // voided). A rising edge of the LF square advances a fractional edge counter; every
      // divN_ LF edges a one-sample clock pulse is fed to the S&H, so it captures the noise
      // exactly once per divN_ LF cycles. Between groups the clock is low and the S&H holds
      // (it never self-runs). shCv_ is the CV-out readback, never summed into the channel.
      shClock_ = 0.0;
      if (sq >= 0.5 && lfPrevLevel_ < 0.5) {  // LF square rising edge (-1 -> +1).
        lfEdgeAcc_ += 1.0;
        if (lfEdgeAcc_ >= divN_) {            // this edge completes a div-by-N group.
          lfEdgeAcc_ -= divN_;
          shClock_ = 1.0;                     // one-sample capture pulse -> S&H rising edge.
        }
      }
      lfPrevLevel_ = sq;
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
    double shClock_ = 0.0;  // S&H clock level (derived from the divided LF square, GH#15 D3).
    double divN_ = 1.0;     // S&H division ratio (1 + (kNewDroneDivMax-1)*norm), GH#15 D3.
    double lfPrevLevel_ = 0.0;  // previous LF-square level, for rising-edge detection.
    double lfEdgeAcc_ = 0.0;    // fractional LF-edge counter, scaled by divN_ into captures.
    double shCv_ = 0.0;
    // MOD knob depth (GH#15 D1). Default 0.5 = the registered drone_3/6.mod default,
    // so the post-wire default sound is half-depth modulation (was the raw ±1 square).
    double mod_ = 0.5;
    // RANGE keyboard/panel-band offset in semitones (GH#15 D2). 0 = hi (default,
    // bit-identical to pre-D2); otherwise the LOW position's kNewDroneRangeLowShiftSt.
    double rangeBaseSt_ = 0.0;
    // PITCH knob-derived semitones (pre-RANGE-offset), held so changing the RANGE
    // selector recomputes the same band with a different offset.
    double pitchSemis_ = 0.0;
    // RATE knob-derived LF frequency (Hz, pre-multiplier), held so changing the RATE
    // SWITCH recomputes the same rate with a different multiplier. Constructor-initialised
    // to kNewDroneLfFreqHz so a fresh voice reproduces the pre-D2 default rate.
    double baseRateHz_ = 0.0;
    // RATE SWITCH multiplier (1 = off/default; otherwise kNewDroneRateSwitchMult). The
    // LF is NEVER gated off by this selector — off is x1, not silence.
    double rateMult_ = 1.0;
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
    // GH#12 keyboard product owner (@Codex direction, @Kimi option A): the keyboard is
    // a REAL executed control source that consumes the canonical note ControlEvents
    // (pitch/pressure/gate_on/gate_off/reset) — InputStateMachine::translate emits exactly
    // this set for a note press. Forward them to the in-owner ArpSeq (default Keyboard
    // mode = transparent pass-through) which passes each event unchanged to the
    // KeyboardBehaviour. The behaviour carries ALL per-note semantics (note identity by
    // (source, channel, noteId), legato/retrigger etc.) — this lane adds no new gate
    // semantics (per @Kimi e8b073c2). `parameter` events must NOT be forwarded (they are
    // control-source param transfer below, handled by setControlParamValue).
    switch (e.kind) {
      case ControlEventKind::pitch:
      case ControlEventKind::pressure:
      case ControlEventKind::gate_on:
      case ControlEventKind::gate_off:
      case ControlEventKind::reset: {
        // ArpSeq's Arp/Seq branches take the sink by lvalue ref, so pass a named lvalue
        // (never a prvalue lambda — the template instantiates every branch). In the default
        // Keyboard mode this sink forwards the event unchanged to the KeyboardBehaviour.
        auto kbdSink = [this](const ControlEvent& nkb) { keyboardBeh_.handleControlEvent(nkb); };
        keyboardArpSeq_.handleControlEvent(e, kbdSink);
        return;  // a note event is fully consumed by the keyboard owner, never a parameter.
      }
      default:
        break;  // parameter / clock / sync fall through to the existing path below.
    }
    if (e.kind != ControlEventKind::parameter) return;
    const double v = static_cast<double>(e.value);
    lastApplyParamId_ = e.parameter;
    switch (e.parameter) {
      // Existing drone_3/6 path: the setter here is void and already admits the
      // registry-AGREEING units quoted above, so a handled drone case records `applied`.
      case ParameterId::drone_3_pitch: setDrone3Pitch(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_3_noise: setDrone3Noise(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_3_fm:    setDrone3Fm(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_3_am:    setDrone3Am(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_pitch: setDrone6Pitch(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_noise: setDrone6Noise(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_fm:    setDrone6Fm(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_am:    setDrone6Am(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_3_mod:   setDrone3Mod(v * kModDepthFromNorm); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_mod:   setDrone6Mod(v * kModDepthFromNorm); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      // GH#15 D2 (selectors, live lane). The ControlEvent carries the selector index 0/1;
      // forward it to the voice (the batch lane validated it via dspParamValid_).
      case ParameterId::drone_3_hi_low:       setDrone3HiLow(static_cast<int>(v)); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_3_rate_switch:  setDrone3RateSwitch(static_cast<int>(v)); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_hi_low:       setDrone6HiLow(static_cast<int>(v)); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_rate_switch:  setDrone6RateSwitch(static_cast<int>(v)); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      // GH#15 D3 (DIVIDER, both voices). The batch lane validated the norm range [0,1] via
      // dspParamValid_ before the switch, so the knob value reaches the voice unchanged.
      case ParameterId::drone_3_divider:   setDrone3Divider(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      case ParameterId::drone_6_divider:   setDrone6Divider(v); lastApplyStatus_ = ParameterApplyStatus::applied; break;
      // GH#11 FIXED-CANDIDATE (@Codex D3): the 34 evidence-mappable control-source params dispatch
      // unit-agreeing (never an invented scale) to the six real DSP instances. A malformed
      // value stays fail-closed (keep old) and is reported real-time through the
      // const/no-alloc readback surface (lastApplyStatus_) — no separate param bank.
      default: setControlParamValue(e.parameter, v); break;  // records its own precise status.
    }
  }

  // ---- 35 control-source + 16 Surface-2 panel-knob parameter dispatch (@Codex 7C3) ----
  // Live parameter lane (ControlEvent). Wires the 35 evidence-mappable control-source params
  // AND the 16 Surface-2 vco/vcf panel-knob seconds params (via isContinuousSmoothingParam_)
  // to the sound-core unit-agreeing setters, returning a ParameterApplyStatus that
  // discriminates applied / invalid / unsupported. A malformed value is rejected HERE (keep
  // old) BEFORE it reaches a setter that might clamp/coerce — e.g. LFO setWave clamps a finite
  // value to [0,1] and a `v != 0.0` transfer would turn 0.5 into gate-high — so a value outside
  // the registry unit-domain is reported `invalid_value`, never a silent coercion. Step params
  // are 1-indexed (sequencer_step_cv_N -> idx N-1).
  // @Codex final ruling 7C3: `sequencer.pulser` is a DOMAIN-VALIDATED provisional transfer.
  // The norm [0,1] is admitted and mapped through the centrally-named PULSER software model
  // (FiveStepSequencer::pulserNormToRateHz) into the core's direct-Hz DSP setter; the
  // provisional nature is a SOFTWARE policy (0.05..20 Hz, centre 1 Hz), never a claimed
  // hardware measurement. A malformed value (non-finite / out of [0,1]) is rejected with
  // keep-old (rate unchanged, invalid_value).
  ParameterApplyStatus setControlParamValue(ParameterId id, double v) {
    lastApplyParamId_ = id;
    // Recognition now covers BOTH lanes of the smoothing family: the 35 乐音 control-source
    // params AND the Surface-2 panel-knob seconds params (the 16 vco/vcf, via
    // isContinuousSmoothingParam_). Deliberately NOT merged into controlSourceParamRecognized_ —
    // that predicate is 乐音 control-source semantics; the 16 are panel knobs. Control-source ids
    // may go direct or smoothed; a panel-knob id is ALWAYS Smoothing::seconds, so it only ever
    // takes the ramp path below (never a direct dispatch).
    const bool ctrlSource = controlSourceParamRecognized_(id);
    if (!ctrlSource && !isContinuousSmoothingParam_(id)) {
      lastApplyStatus_ = ParameterApplyStatus::unsupported_parameter;
      return lastApplyStatus_;
    }
    // Validation is set-specific: control-source params admit against controlParamValid_ (their
    // own registry-unit domain); panel-knob seconds params admit against the generic registry
    // gate dspParamValid_ — the only validator spanning oct [-1,1] (controlParamValid_'s default
    // return false rejects tune). A malformed value rejects keep-old, no setter touched.
    const bool valid = ctrlSource ? controlParamValid_(id, v) : dspParamValid_(id, v);
    if (!valid) {                                          // malformed: keep old, no setter touched.
      lastApplyStatus_ = ParameterApplyStatus::invalid_value;
      return lastApplyStatus_;
    }
    // GH#21 continuous control smoothing (design/07 §3.2): a SECONDS-smoothed control target
    // is not dispatched to the DSP directly — it becomes the smoother's target and is applied
    // one-smoothed-value-per-frame in advanceControlSmoothing_(). This is the ControlEvent
    // parameter lane (Surface-1 control-source + Surface-2 panel-knob). Boundary (contract
    // hard-req #3): exposed AUDIO-RATE CV (a jack patch-signal modulation) is NOT smoothed here.
    // Gate/note/clock edges and the 15 discrete control params keep the direct single-sample
    // dispatch below. Partition-independent: the smoothers persist across processBlock and
    // fs/tau are fixed, so 64/128/256 reproduce the same per-sample sequence.
    const ParameterDescriptor* desc = find_parameter(id);
    if (desc != nullptr && desc->smoothing == Smoothing::seconds) {
      const std::uint32_t ord = static_cast<std::uint32_t>(id);
      controlSmoothers_[ord].setTarget(v);
      lastApplyStatus_ = ParameterApplyStatus::applied;
      return lastApplyStatus_;
    }
    switch (id) {
      // Envelope A (6).
      case ParameterId::envelope_a_a: lastApplyStatus_ = transferStatus_(envGenA_.setAttackSeconds(v));  return lastApplyStatus_;
      case ParameterId::envelope_a_d: lastApplyStatus_ = transferStatus_(envGenA_.setDecaySeconds(v));    return lastApplyStatus_;
      case ParameterId::envelope_a_r: lastApplyStatus_ = transferStatus_(envGenA_.setReleaseSeconds(v));  return lastApplyStatus_;
      case ParameterId::envelope_a_s: lastApplyStatus_ = transferStatus_(envGenA_.setSustain(v));          return lastApplyStatus_;
      case ParameterId::envelope_a_hold:     envGenA_.setHold(v != 0.0);    lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::envelope_a_self_gen: envGenA_.setSelfGen(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      // Envelope B (6).
      case ParameterId::envelope_b_a: lastApplyStatus_ = transferStatus_(envGenB_.setAttackSeconds(v));  return lastApplyStatus_;
      case ParameterId::envelope_b_d: lastApplyStatus_ = transferStatus_(envGenB_.setDecaySeconds(v));    return lastApplyStatus_;
      case ParameterId::envelope_b_r: lastApplyStatus_ = transferStatus_(envGenB_.setReleaseSeconds(v));  return lastApplyStatus_;
      case ParameterId::envelope_b_s: lastApplyStatus_ = transferStatus_(envGenB_.setSustain(v));          return lastApplyStatus_;
      case ParameterId::envelope_b_hold:     envGenB_.setHold(v != 0.0);    lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      case ParameterId::envelope_b_self_gen: envGenB_.setSelfGen(v != 0.0); lastApplyStatus_ = ParameterApplyStatus::applied; return lastApplyStatus_;
      // LFO A (3) / LFO B (3).
      case ParameterId::lfo_a_rate: lastApplyStatus_ = transferStatus_(lfoA_.setBaseHz(v));  return lastApplyStatus_;
      case ParameterId::lfo_a_wave: lastApplyStatus_ = transferStatus_(lfoA_.setWave(v));    return lastApplyStatus_;
      case ParameterId::lfo_a_speed_mult: {
        LfoSpeedMult m;
        lastApplyStatus_ = (speedMultFromIndex_(v, m) && lfoA_.setSpeedMult(m))
            ? ParameterApplyStatus::applied : ParameterApplyStatus::invalid_value;
        return lastApplyStatus_;
      }
      case ParameterId::lfo_b_rate: lastApplyStatus_ = transferStatus_(lfoB_.setBaseHz(v));  return lastApplyStatus_;
      case ParameterId::lfo_b_wave: lastApplyStatus_ = transferStatus_(lfoB_.setWave(v));    return lastApplyStatus_;
      case ParameterId::lfo_b_speed_mult: {
        LfoSpeedMult m;
        lastApplyStatus_ = (speedMultFromIndex_(v, m) && lfoB_.setSpeedMult(m))
            ? ParameterApplyStatus::applied : ParameterApplyStatus::invalid_value;
        return lastApplyStatus_;
      }
      // Joystick (4) — norm 0..1 into the stateless X/Y/offset state.
      case ParameterId::joystick_x: lastApplyStatus_ = transferStatus_(joystick_.setX(v));        return lastApplyStatus_;
      case ParameterId::joystick_y: lastApplyStatus_ = transferStatus_(joystick_.setY(v));        return lastApplyStatus_;
      case ParameterId::joystick_offset_x: lastApplyStatus_ = transferStatus_(joystick_.setOffsetX(v)); return lastApplyStatus_;
      case ParameterId::joystick_offset_y: lastApplyStatus_ = transferStatus_(joystick_.setOffsetY(v)); return lastApplyStatus_;
      // Sequencer (12) — step params are 1-indexed (step_cv_N -> index N-1).
      case ParameterId::sequencer_clock: {
        SequencerClockSource src;
        lastApplyStatus_ = (sequencerClockFromIndex_(v, src) && sequencer_.setClockSource(src))
            ? ParameterApplyStatus::applied : ParameterApplyStatus::invalid_value;
        return lastApplyStatus_;
      }
      case ParameterId::sequencer_stages: {
        const int n = static_cast<int>(v);
        lastApplyStatus_ = (n >= 0 && n <= 2 && sequencer_.setStageCount(3 + n))
            ? ParameterApplyStatus::applied : ParameterApplyStatus::invalid_value;
        return lastApplyStatus_;
      }
      case ParameterId::sequencer_step_cv_1:  lastApplyStatus_ = transferStatus_(sequencer_.setStepCv(0, v)); return lastApplyStatus_;
      case ParameterId::sequencer_step_cv_2:  lastApplyStatus_ = transferStatus_(sequencer_.setStepCv(1, v)); return lastApplyStatus_;
      case ParameterId::sequencer_step_cv_3:  lastApplyStatus_ = transferStatus_(sequencer_.setStepCv(2, v)); return lastApplyStatus_;
      case ParameterId::sequencer_step_cv_4:  lastApplyStatus_ = transferStatus_(sequencer_.setStepCv(3, v)); return lastApplyStatus_;
      case ParameterId::sequencer_step_cv_5:  lastApplyStatus_ = transferStatus_(sequencer_.setStepCv(4, v)); return lastApplyStatus_;
      case ParameterId::sequencer_step_gate_1: lastApplyStatus_ = transferStatus_(sequencer_.setStepGate(0, v != 0.0)); return lastApplyStatus_;
      case ParameterId::sequencer_step_gate_2: lastApplyStatus_ = transferStatus_(sequencer_.setStepGate(1, v != 0.0)); return lastApplyStatus_;
      case ParameterId::sequencer_step_gate_3: lastApplyStatus_ = transferStatus_(sequencer_.setStepGate(2, v != 0.0)); return lastApplyStatus_;
      case ParameterId::sequencer_step_gate_4: lastApplyStatus_ = transferStatus_(sequencer_.setStepGate(3, v != 0.0)); return lastApplyStatus_;
      case ParameterId::sequencer_step_gate_5: lastApplyStatus_ = transferStatus_(sequencer_.setStepGate(4, v != 0.0)); return lastApplyStatus_;
      // sequencer.pulser (@Codex 7C3): DOMAIN-VALIDATED provisional transfer. Admit the
      // norm [0,1] (validated above), map through the centrally-named PULSER software model,
      // and drive the core's direct-Hz DSP setter. The status is `applied` (the provisional
      // nature is a SOFTWARE policy, not transfer_unavailable). A malformed value already
      // yielded invalid_value above with the rate unchanged.
      case ParameterId::sequencer_pulser:
        lastApplyStatus_ = transferStatus_(sequencer_.setInternalRateHz(
            FiveStepSequencer::pulserNormToRateHz(v)));
        return lastApplyStatus_;
      default: break;  // unreachable: unrecognised ids were rejected above.
    }
    lastApplyStatus_ = ParameterApplyStatus::unsupported_parameter;
    return lastApplyStatus_;
  }

  // Is `id` one of the 35 recognised control-source params (the 34 mappable + pulser)?
  // Separates UNKNOWN (-> unsupported_parameter) from KNOWN-but-malformed (-> invalid_value).
  bool controlSourceParamRecognized_(ParameterId id) const {
    switch (id) {
      case ParameterId::envelope_a_a: case ParameterId::envelope_a_d: case ParameterId::envelope_a_r:
      case ParameterId::envelope_a_s: case ParameterId::envelope_a_hold: case ParameterId::envelope_a_self_gen:
      case ParameterId::envelope_b_a: case ParameterId::envelope_b_d: case ParameterId::envelope_b_r:
      case ParameterId::envelope_b_s: case ParameterId::envelope_b_hold: case ParameterId::envelope_b_self_gen:
      case ParameterId::lfo_a_rate: case ParameterId::lfo_a_wave: case ParameterId::lfo_a_speed_mult:
      case ParameterId::lfo_b_rate: case ParameterId::lfo_b_wave: case ParameterId::lfo_b_speed_mult:
      case ParameterId::joystick_x: case ParameterId::joystick_y:
      case ParameterId::joystick_offset_x: case ParameterId::joystick_offset_y:
      case ParameterId::sequencer_clock: case ParameterId::sequencer_stages:
      case ParameterId::sequencer_step_cv_1: case ParameterId::sequencer_step_cv_2:
      case ParameterId::sequencer_step_cv_3: case ParameterId::sequencer_step_cv_4:
      case ParameterId::sequencer_step_cv_5:
      case ParameterId::sequencer_step_gate_1: case ParameterId::sequencer_step_gate_2:
      case ParameterId::sequencer_step_gate_3: case ParameterId::sequencer_step_gate_4:
      case ParameterId::sequencer_step_gate_5:
      case ParameterId::sequencer_pulser:
        return true;
      default:
        return false;
    }
  }

  // The one predicate marking an id as a member of the ACTIVE continuous-smoothing set:
  // `smoothing == Smoothing::seconds` AND `disposition == applied_to_dsp`. This is the
  // GH#21 Surface-1+Surface-2 union — the 20 control-source seconds params (envelope a/b
  // attack/decay/release/sustain, lfo a/b rate, joystick x/y/offset_x/offset_y, sequencer
  // step_cv_1..5, pulser) plus the 16 vco/vcf panel-knob seconds params (vco a/b tune/morph/
  // pw/cv_amt, vcf l/r freq/res/mod, vcf dist/gain) = 36. It is deliberately NOT folded into
  // controlSourceParamRecognized_ (that predicate is 乐音 control-source semantics; the 16
  // are panel knobs). Applied_to_dsp naturally excludes keyboard (35), p6/p8-preserved (125)
  // and transfer-unavailable (16, incl. vco a/b pwm) params — a declared-but-unwired seconds
  // param is NOT in any smoothing route, which keeps the "exactly 169 apply" gate honest.
  bool isContinuousSmoothingParam_(ParameterId id) const {
    const ParameterDescriptor* desc = find_parameter(id);
    return desc != nullptr &&
           desc->smoothing == Smoothing::seconds &&
           disposition_of(id) == StateDisposition::applied_to_dsp;
  }

  // Product-boundary unit-domain admission (@Codex BLOCKED #1). Rejects a value malformed for
  // its registry unit BEFORE it reaches a sound-core setter, so the setter's legal clamp /
  // coercion is never mistaken for a valid transfer. Domains are the registry evidence, not
  // the DSP tolerance. Finite everywhere; norm [0,1]; boolean exact {0,1}; selector exact
  // integer; step CV [0,+5V]; pulser norm [0,1]. Caller guarantees id is recognised.
  bool controlParamValid_(ParameterId id, double v) const {
    if (!std::isfinite(v)) return false;
    switch (id) {
      // EG time (attack/decay/release) seconds: finite && >= 0.
      case ParameterId::envelope_a_a: case ParameterId::envelope_a_d: case ParameterId::envelope_a_r:
      case ParameterId::envelope_b_a: case ParameterId::envelope_b_d: case ParameterId::envelope_b_r:
        return v >= 0.0;
      // EG sustain norm [0,1].
      case ParameterId::envelope_a_s: case ParameterId::envelope_b_s:
        return v >= 0.0 && v <= 1.0;
      // EG hold/self-gen boolean: EXACT {0,1} — never `v != 0.0` coercion (0.5 is invalid).
      case ParameterId::envelope_a_hold: case ParameterId::envelope_a_self_gen:
      case ParameterId::envelope_b_hold: case ParameterId::envelope_b_self_gen:
        return v == 0.0 || v == 1.0;
      // LFO rate Hz: finite && >= 0.
      case ParameterId::lfo_a_rate: case ParameterId::lfo_b_rate:
        return v >= 0.0;
      // LFO WAVE morph norm [0,1] (square->triangle crossfade).
      case ParameterId::lfo_a_wave: case ParameterId::lfo_b_wave:
        return v >= 0.0 && v <= 1.0;
      // LFO speed_mult selector index: EXACT integer {0,1,2}.
      case ParameterId::lfo_a_speed_mult: case ParameterId::lfo_b_speed_mult: {
        const int i = static_cast<int>(v);
        return static_cast<double>(i) == v && i >= 0 && i <= 2;
      }
      // Joystick norm [0,1].
      case ParameterId::joystick_x: case ParameterId::joystick_y:
      case ParameterId::joystick_offset_x: case ParameterId::joystick_offset_y:
        return v >= 0.0 && v <= 1.0;
      // Sequencer clock selector index: EXACT integer {0,1}.
      case ParameterId::sequencer_clock: {
        const int i = static_cast<int>(v);
        return static_cast<double>(i) == v && (i == 0 || i == 1);
      }
      // Sequencer stages selector index: EXACT integer {0,1,2}.
      case ParameterId::sequencer_stages: {
        const int i = static_cast<int>(v);
        return static_cast<double>(i) == v && i >= 0 && i <= 2;
      }
      // Sequencer step CV volts [0,+5V].
      case ParameterId::sequencer_step_cv_1: case ParameterId::sequencer_step_cv_2:
      case ParameterId::sequencer_step_cv_3: case ParameterId::sequencer_step_cv_4:
      case ParameterId::sequencer_step_cv_5:
        return v >= 0.0 && v <= 5.0;
      // Sequencer step gate boolean: EXACT {0,1} — never gate-high on 0.5.
      case ParameterId::sequencer_step_gate_1: case ParameterId::sequencer_step_gate_2:
      case ParameterId::sequencer_step_gate_3: case ParameterId::sequencer_step_gate_4:
      case ParameterId::sequencer_step_gate_5:
        return v == 0.0 || v == 1.0;
      // sequencer.pulser norm [0,1] (@Codex 7C3): admitted as a domain-valid provisional
      // value; the eventual Hz is the centrally-named software model, never a transfer_unavailable.
      case ParameterId::sequencer_pulser:
        return v >= 0.0 && v <= 1.0;
      default:
        return false;  // unknown ids are handled before reaching here.
    }
  }

  // GH#21 per-frame smoother advance. Iterate the ACTIVE continuous-smoothing set (control-source
  // + panel-knob, 36), pull each smoother's next per-sample output, and feed it to the sound-core
  // setter. Runs once per frame in processFrame before the graph resolves, so the control source
  // publishes this frame's smoothed value. No allocation; the set was built at construction.
  void advanceControlSmoothing_() {
    for (std::uint32_t k = 0; k < controlSmoothOrdinalCount_; ++k) {
      const ParameterId id = controlSmoothOrdinals_[k];
      const std::uint32_t ord = static_cast<std::uint32_t>(id);
      ParameterSmoother& sm = controlSmoothers_[ord];
      // Once settled, DO NOT write the DSP setter. Leaving a settled smoother in
      // place is the GH#21 fix for the direct-setter clobber: a sanctioned direct
      // DSP-domain setter (e.g. setSequencerInternalRateHz) that bypasses the
      // parameter lane must NOT be overwritten back to the smoother's own target on
      // the next frame. The smoother is inert between live target changes; the DSP
      // keeps whatever the caller (parameter lane OR direct setter) last wrote.
      if (sm.settled()) continue;
      // Converging: advance one pole step, publish it to the sound-core setter.
      applySmoothedControl_(id, sm.next());
      // First frame that crosses the settle tolerance: snap to the EXACT target so
      // the DSP holds the precise (not approx +1%) value, then go inert above.
      if (sm.settled()) applySmoothedControl_(id, sm.target());
    }
  }

  // Apply a smoothed control value (already in the registry unit domain: seconds for EG
  // times, Hz for LFO rate, norm for joystick + pulser + panel-knob, volts for step CV) to the
  // sound-core setter. Called from advanceControlSmoothing_; the in-domain smoother output is
  // guaranteed accepted by the setters (the bool return is ignored — only a non-finite /
  // out-of-range value would reject, which a smoother bounded between current and an in-domain
  // target never emits).
  void applySmoothedControl_(ParameterId id, double v) {
    switch (id) {
      case ParameterId::envelope_a_a: envGenA_.setAttackSeconds(v);  break;
      case ParameterId::envelope_a_d: envGenA_.setDecaySeconds(v);    break;
      case ParameterId::envelope_a_r: envGenA_.setReleaseSeconds(v);  break;
      case ParameterId::envelope_a_s: envGenA_.setSustain(v);          break;
      case ParameterId::envelope_b_a: envGenB_.setAttackSeconds(v);  break;
      case ParameterId::envelope_b_d: envGenB_.setDecaySeconds(v);    break;
      case ParameterId::envelope_b_r: envGenB_.setReleaseSeconds(v);  break;
      case ParameterId::envelope_b_s: envGenB_.setSustain(v);          break;
      case ParameterId::lfo_a_rate: lfoA_.setBaseHz(v);  break;
      case ParameterId::lfo_b_rate: lfoB_.setBaseHz(v);  break;
      case ParameterId::joystick_x: joystick_.setX(v);        break;
      case ParameterId::joystick_y: joystick_.setY(v);        break;
      case ParameterId::joystick_offset_x: joystick_.setOffsetX(v); break;
      case ParameterId::joystick_offset_y: joystick_.setOffsetY(v); break;
      case ParameterId::sequencer_step_cv_1: sequencer_.setStepCv(0, v); break;
      case ParameterId::sequencer_step_cv_2: sequencer_.setStepCv(1, v); break;
      case ParameterId::sequencer_step_cv_3: sequencer_.setStepCv(2, v); break;
      case ParameterId::sequencer_step_cv_4: sequencer_.setStepCv(3, v); break;
      case ParameterId::sequencer_step_cv_5: sequencer_.setStepCv(4, v); break;
      case ParameterId::sequencer_pulser:
        sequencer_.setInternalRateHz(FiveStepSequencer::pulserNormToRateHz(v)); break;
      // Surface-2: the 16 vco/vcf panel-knob seconds params feed the SAME setters used by the
      // whole-state applyDspParam dispatch. The smoother output is in the registry unit domain
      // (oct for tune, norm otherwise), so it is passed straight to each existing setter.
      case ParameterId::vco_a_tune: setVcoATune(v); break;
      case ParameterId::vco_a_morph: setVcoAMorph(v); break;
      case ParameterId::vco_a_pw:    setVcoAPw(v);    break;
      case ParameterId::vco_a_cv_amt: setVcoACvAmt(v); break;
      case ParameterId::vco_b_tune: setVcoBTune(v); break;
      case ParameterId::vco_b_morph: setVcoBMorph(v); break;
      case ParameterId::vco_b_pw:    setVcoBPw(v);    break;
      case ParameterId::vco_b_cv_amt: setVcoBCvAmt(v); break;
      case ParameterId::vcf_l_freq: setVcfFreq(0, v); break;
      case ParameterId::vcf_l_res:  setVcfRes(0, v);  break;
      case ParameterId::vcf_l_mod:  setVcfMod(0, v);  break;
      case ParameterId::vcf_r_freq: setVcfFreq(1, v); break;
      case ParameterId::vcf_r_res:  setVcfRes(1, v);  break;
      case ParameterId::vcf_r_mod:  setVcfMod(1, v);  break;
      case ParameterId::vcf_dist:   setDistortionAmount(v); break;
      case ParameterId::vcf_gain:   setDistortionGain(v);   break;
      default: break;  // never reached: only seconds Smoothing::seconds && applied_to_dsp ids advance.
    }
  }

  // Read the DSP core's CURRENT value for a continuous smoothing param (control-source or
  // panel-knob), in the param's registry unit domain. Used to prime each smoother to the live
  // sound value so the first target transition interpolates from where the machine actually is
  // (zero-jump). The sequencer.pulser core stores Hz, so the norm is recovered by inverting the
  // centrally-named PULSER software model (norm = log(Hz/min)/log(base); centre 1 Hz -> 0.5).
  double currentControlParam_(ParameterId id) const {
    switch (id) {
      case ParameterId::envelope_a_a:   return envGenA_.attackSeconds();
      case ParameterId::envelope_a_d:   return envGenA_.decaySeconds();
      case ParameterId::envelope_a_r:   return envGenA_.releaseSeconds();
      case ParameterId::envelope_a_s:   return envGenA_.sustain();
      case ParameterId::envelope_b_a:   return envGenB_.attackSeconds();
      case ParameterId::envelope_b_d:   return envGenB_.decaySeconds();
      case ParameterId::envelope_b_r:   return envGenB_.releaseSeconds();
      case ParameterId::envelope_b_s:   return envGenB_.sustain();
      case ParameterId::lfo_a_rate:     return lfoA_.baseHz();
      case ParameterId::lfo_b_rate:     return lfoB_.baseHz();
      case ParameterId::joystick_x:        return joystick_.x();
      case ParameterId::joystick_y:        return joystick_.y();
      case ParameterId::joystick_offset_x: return joystick_.offsetX();
      case ParameterId::joystick_offset_y: return joystick_.offsetY();
      case ParameterId::sequencer_step_cv_1: return sequencer_.stepCv(0);
      case ParameterId::sequencer_step_cv_2: return sequencer_.stepCv(1);
      case ParameterId::sequencer_step_cv_3: return sequencer_.stepCv(2);
      case ParameterId::sequencer_step_cv_4: return sequencer_.stepCv(3);
      case ParameterId::sequencer_step_cv_5: return sequencer_.stepCv(4);
      case ParameterId::sequencer_pulser:
        return pulserRateHzToNorm_(sequencer_.internalRateHz());
      // Surface-2: the 16 vco/vcf panel-knob seconds params prime from the same live getters
      // used by the whole-state applyDspParam snapshot, so a construction build starts from
      // where the machine actually is (zero-jump).
      case ParameterId::vco_a_tune:   return vcoATune();
      case ParameterId::vco_a_morph:  return vcoAMorph();
      case ParameterId::vco_a_pw:     return vcoAPw();
      case ParameterId::vco_a_cv_amt: return vcoACvAmt();
      case ParameterId::vco_b_tune:   return vcoBTune();
      case ParameterId::vco_b_morph:  return vcoBMorph();
      case ParameterId::vco_b_pw:     return vcoBPw();
      case ParameterId::vco_b_cv_amt: return vcoBCvAmt();
      case ParameterId::vcf_l_freq:   return vcfFreq(0);
      case ParameterId::vcf_l_res:    return vcfRes(0);
      case ParameterId::vcf_l_mod:    return vcfMod(0);
      case ParameterId::vcf_r_freq:   return vcfFreq(1);
      case ParameterId::vcf_r_res:    return vcfRes(1);
      case ParameterId::vcf_r_mod:    return vcfMod(1);
      case ParameterId::vcf_dist:     return distortionAmount();
      case ParameterId::vcf_gain:     return distortionGain();
      default: return 0.0;  // never reached: only Smoothing::seconds && applied_to_dsp ids are primed.
    }
  }

  // Inverse of FiveStepSequencer::pulserNormToRateHz (hz = min * base^norm). Recover the panel
  // norm from the core's stored Hz so the pulser smoother can be primed from the live DSP value.
  static double pulserRateHzToNorm_(double hz) {
    return std::log(hz / kFiveStepPulserMinRateHz) / std::log(kFiveStepPulserLogBase);
  }

  // task #78: shared registry-unit-domain admission for the 134 NEW applied_to_dsp ids.
  // Mirrors controlParamValid_ for the control-source set: reject a value malformed for its
  // registry descriptor BEFORE it reaches a setter that might clamp/coerce (a malformed norm
  // stays invalid; a non-integer selector stays invalid — never a silent `v != 0.0` gate-high
  // on 0.5). Selector = descriptor.step > 0 (exact integer in [min,max]); continuous =
  // step == 0 (in [min,max]). Finite everywhere.
  bool dspParamValid_(ParameterId id, double v) const {
    if (!std::isfinite(v)) return false;
    const ParameterDescriptor* d = find_parameter(id);
    if (d == nullptr) return false;
    if (d->step > 0.0) {
      const int i = static_cast<int>(v);
      return static_cast<double>(i) == v && v >= d->min && v <= d->max;
    }
    return v >= d->min && v <= d->max;
  }

  // Map a sound-core setter's bool to the status: true -> applied; false -> the value was in
  // unit-domain but the setter still rejected it (e.g. LFO effective-step non-finite).
  static ParameterApplyStatus transferStatus_(bool accepted) {
    return accepted ? ParameterApplyStatus::applied : ParameterApplyStatus::invalid_value;
  }

  // ---- selector-index mapping helpers (fail-closed keep-old on out-of-band) ----
  using SequencerClockSource = FiveStepSequencer::ClockSource;
  // LFO speed_mult selector index 0/1/2 -> {x1, x6, x10}. Out of band -> false (no apply).
  static bool speedMultFromIndex_(double v, LfoSpeedMult& out) {
    const int i = static_cast<int>(v);
    if (i == 1) { out = LfoSpeedMult::x6; return true; }
    if (i == 2) { out = LfoSpeedMult::x10; return true; }
    if (i == 0) { out = LfoSpeedMult::x1; return true; }
    return false;
  }
  // sequencer clock selector index 0/1 -> {internal, external}. Out of band -> false.
  static bool sequencerClockFromIndex_(double v, SequencerClockSource& out) {
    const int i = static_cast<int>(v);
    if (i == 0) { out = SequencerClockSource::kInternal; return true; }
    if (i == 1) { out = SequencerClockSource::kExternal; return true; }
    return false;
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

  // ---- task #78 centralized norm->physical transfers (ALL software-provisional; the
  // hardware calibration of every one is untested and flagged as such in the mandate).
  // These are the ONLY places a normalized registry value becomes a real DSP unit, so a
  // product oracle can verify each apply against ONE definition and a test can isolate-mutate
  // a single helper to turn an entire family's applies RED. The PULSER reuses the already-
  // existing FiveStepSequencer::pulserNormToRateHz (see setControlParamValue).
  static double envFollowerSecondsFromNorm(double n) {
    return 0.001 + 0.999 * n;
  }  // s, n in [0,1].
  static double classicDroneTuneSemisFromNorm(double n) {
    return (n - 0.5) * 24.0;
  }  // -12..+12 semis.
  static double classicDroneVoltSemisDownFromNorm(double n) {
    return 60.0 * n;
  }  // 0.5 -> 30 semis down.
  static double newDroneRateHzFromNorm(double n) {
    return 12.0 * n;
  }  // 0.5 -> 6 Hz; 0 -> stop.

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
    // The ONE delayed-vs-live decision (@Codex correction 6): a single scan whose only
    // exact-pair test is the (src,sink) match INSIDE feedbackSinkValue — never source-only.
    // An exact line returns matched=true with its own D-sample read; the scan returns that
    // value; nothing matches -> the live source value.
    const double live = cvAt_(src);
    for (std::uint32_t i = 0; i < feedbackCount_; ++i) {
      const FeedbackLine& l = feedback_[i];
      if (!l.active) continue;
      const FeedbackResolve r = feedbackSinkValue(src, sink, l.sourceJack, l.sinkJack,
                                                  l.buf[l.writePos], live);
      if (r.matched) return r.value;
    }
    return live;
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
  // `effector`/`voices` to kUnsupported (declared-deferred, no runtime instance yet);
  // `keyboard` is now kKeyboard (GH#12 owner) and the six control sources are real DSP
  // (GH#11 D1/D2/D4, machine_definition.h), so they are never kUnsupported here. Patching
  // any kUnsupported module into the graph is
  // a real semantics violation: REFUSE with unsupported_module, never silently skip to zero
  // slots. Distinct from
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
        // V/OCT (v_oct_in): confirmed 1 V/oct exponential pitch input.
        double v = 0.0;
        if (resolveControlSink_(voctA_, v, driveGraph)) vcA_.setVoct(v);
        // Generic CV (cv_in, -5..+5): independent from V/OCT, mode from the runtime-held
        // setVcoControlModes (never a hardcoded law). Ignored when unbound.
        if (cvInBoundA_) {
          double g = 0.0;
          if (resolveControlSink_(cvInA_, g, driveGraph)) vcA_.setCvInput(g, cvModeA_);
        }
        double a = 0.0;
        vcA_.tick(&a);
        dryA_ = a;
        chIn_[VoiceMixer::kChannelVcoA] = a;
        if (vcoAOutBound_) publishSourceValue_(vcoAOut_, a);
        break;
      }
      case ExecutionKind::kVcoB: {
        // V/OCT (v_oct_in): confirmed 1 V/oct exponential pitch input.
        double v = 0.0;
        if (resolveControlSink_(voctB_, v, driveGraph)) vcB_.setVoct(v);
        // Generic CV (cv_in, -5..+5): resolved from the EXACT source feeding vco_b.cv_in.
        // task #83 default = the acyclic A->B route vco_a.dry_out -> vco_b.cv_in, read LIVE
        // same-frame (B consumes A's published value this frame). Only an edge the compiled
        // graph marks as a FEEDBACK line (e.g. a user B->B cable) is consumed by the exact-pair
        // reader from that edge's own D-sample line (off by one vs live-last-written — the
        // graph_compiler consume-rule). The mode comes from the runtime-held setVcoControlModes.
        // This is the fitted setCvInput path, NEVER the confirmed V/OCT setVoct path (@Codex
        // 7C2: the generic CV is an independent binding/transfer, not a masquerade of the
        // V/OCT law).
        if (cvInBoundB_) {
          double g = 0.0;
          if (resolveControlSink_(cvInB_, g, driveGraph)) vcB_.setCvInput(g, cvModeB_);
        }
        double b = 0.0;
        vcB_.tick(&b);
        dryB_ = b;
        chIn_[VoiceMixer::kChannelVcoB] = b;
        // Publish the real vco_b.vco_out so any downstream (a normal consumer, or a
        // user-established B->B feedback edge) reads THIS frame's value through the single
        // write (@Codex correction 4).
        if (vcoBOutBound_) publishSourceValue_(vcoBOut_, b);
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
        chIn_[VoiceMixer::kChannelExtAudio] = lastIn_.extAudio;
        break;
      case ExecutionKind::kPreamp: {
        // The ext_source_in break sink goes through the SINGLE sink resolver: a cycle
        // reads the delayed env_follower value, a normal edge reads the live source, and
        // an UNFED sink falls back to the EXT.AUDIO terminal. No feedbackSinkIndex_
        // special-case (@Codex 7C2 req. 3 — preamp uses the same exact-edge resolver).
        const double in = resolveSinkValue_(preampExtIn_, lastIn_.preamp);
        preampInResolved_ = in;
        preampOut_ = preamp_.tick(in);
        chIn_[VoiceMixer::kChannelPreamp] = preampOut_;
        break;
      }
      case ExecutionKind::kEnvFollower: {
        envOut_ = envFol_.tick(preampOut_);
        publishSourceValue_(envFolOut_, envOut_);  // cycle leg -> its own delay line.
        break;
      }
      case ExecutionKind::kMixer: {
        // Canonical host-terminal injection (28-route `ext_audio_to_mixer`, per @Codex
        // 190173bb / 07bfb061): EXT.AUDIO terminal -> mixer channel EXT.AUDIO (ch4). The
        // legacy synthetic `kExtIn` slot was correctly REMOVED in the canonical machine,
        // but its injection must be preserved in the mixer's resolve stage — otherwise a
        // host ext drive is silently dropped and WET wrongly collapses to the no-drive
        // result. This is the real execution of the host-terminal route, not new semantics.
        // The preamp KEEPS its break-ring resolver (delayed env) and is NOT changed to read
        // the host EXT — cable feeding ext_source_in overrides it, ch4 carries the terminal.
        chIn_[VoiceMixer::kChannelExtAudio] = lastIn_.extAudio;
        mixer_.tick(chIn_, mixL_, mixR_);
        break;
      }
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
      case ExecutionKind::kEnvelope: {  // envelope_a/b — A/B by slot.id.
        // resolve real gate_in (JackDescriptor + runtime-owned GateClockSinkState) -> gate
        // level -> tick -> publish env_out (0..+8V confirmed) + vca_cv_out (provisional open
        // transfer requirement, P3). A single pulse source (threshold 0 on the gate jack)
        // never rises after the first sample, so the SAME-sample gate→env path is a LEVEL
        // rule; the rising-edge advance is the domain of the ext-clock (sequencer) below.
        const bool isB = (slot.id == ModuleId::envelope_b);
        EnvelopeGenerator& eg = isB ? envGenB_ : envGenA_;
        const JackId gateIn = isB ? gateInB_ : gateInA_;
        const JackId envOut = isB ? envOutB_ : envOutA_;
        const JackId vcaOut = isB ? vcaOutB_ : vcaOutA_;
        GateClockSinkState& latch = isB ? envB_gate_ : envA_gate_;
        bool gateHigh = false;
        double volts = 0.0;
        if (resolveControlSink_(gateIn, volts, driveGraph)) {
          const JackDescriptor* d = findJackDescriptor_(gateIn);
          gateHigh = (d != nullptr) ? sink_gate_interpret(*d, latch, volts).gateHigh : false;
        }
        eg.tick(gateHigh);
        publishSourceValue_(envOut, eg.envVolts());
        publishSourceValue_(vcaOut, eg.vcaCvVolts());
        break;
      }
      case ExecutionKind::kLfo: {  // lfo_a/b — no-input source, tick once/sample, publish.
        const bool isB = (slot.id == ModuleId::lfo_b);
        Lfo& lfo = isB ? lfoB_ : lfoA_;
        const double v = lfo.tick();
        publishSourceValue_(isB ? lfoBOut_ : lfoAOut_, v);  // 0..+10V (confirmed).
        break;
      }
      case ExecutionKind::kJoystick: {  // stateless: read X/Y + offset same sample, publish.
        publishSourceValue_(joyXOut_, joystick_.xOut());  // ±10V (confirmed).
        publishSourceValue_(joyYOut_, joystick_.yOut());
        break;
      }
      case ExecutionKind::kSequencer: {  // ext_clock_in -> real descriptor rising -> tick.
        // Resolve the continuous ext_clock_in volts through the REAL jack descriptor gate
        // interpreter (provisional canonical sink semantics); advance ONLY on a real rising
        // edge (first sample primes, never a phantom advance). The internal PULSER still
        // runs every sample (phase continuity) and now publishes CLOCK OUT as its
        // virtual-volts one-sample -10/+10 pulse (rail confirmed, width provisional); the
        // discrete rising bool (clockOutRising()) remains the single edge truth.
        bool clockRising = false;
        double volts = 0.0;
        if (resolveControlSink_(seqExtClockIn_, volts, driveGraph)) {
          const JackDescriptor* d = findJackDescriptor_(seqExtClockIn_);
          clockRising = (d != nullptr) &&
                        (sink_gate_interpret(*d, seqClockLatch_, volts).edge == GateEdge::rising);
        }
        sequencer_.tick(clockRising);
        publishSourceValue_(seqCvOut_, sequencer_.cvOut());      // 0..+5V (confirmed).
        publishSourceValue_(seqGateOut_, sequencer_.gateOut());  // 0/+10V one-sample (provisional pulse).
        publishSourceValue_(seqClockOut_, sequencer_.clockOutVolts());  // -10/+10 one-sample (rail confirmed, width provisional).
        break;
      }
      case ExecutionKind::kKeyboard: {  // GH#12 keyboard product owner.
        // Advance the note voice ONE sample (portamento glide continuity — GH#8: the glide
        // must advance every sample, so a note pitch glides to its target and the note CV is
        // the glided/vibrato pitch, not a frozen current()). The gate is the live engaged
        // level: publish the 0/10V GATE rail (keyboard.gate_left_main_out is unipolar 0..10)
        // so the downstream EG A/B gate_in interpreter (threshold 0.5V + hysteresis) reads it
        // HIGH when notes are held and LOW otherwise. Pitch CV goes to keyboard.v_oct_out
        // (V/OCT 0..8V); VCO A/B v_oct_in consume it via the two active keyboard routes.
        double pitchCv = 0.0;
        double pressureCv = 0.0;
        keyboardBeh_.tick(&pitchCv, &pressureCv);
        publishSourceValue_(kbdVOctOut_, pitchCv);
        publishSourceValue_(kbdGateOut_, keyboardBeh_.gate() ? 10.0 : 0.0);
        break;
      }
      case ExecutionKind::kUnsupported:
        break;
    }
  }

  // Resolve the VCF L/R control CV (needed by both the merged kVcfPath slot and the
  // legacy standalone kVcf slot).
  //
  // NORMALLING (route.vcf_cv_l_to_cv_r, item 2 @Codex eaaf08cc): the registry route says
  // "CV L is normally connected to CV R... if there is no CV-signal in the CV R. Plugging
  // into CV R overrides this." The GRAPH resolving each jack independently leaves R at 0
  // when it has no cable — that is the unmigrated gap. Here the product executes the
  // normalling: L fed AND R unplugged => R uses THIS FRAME's already-resolved L; an
  // explicit R cable (fed) takes precedence. The value written into the DFT-SPSS
  // PolivoksFilter is observed through the REAL DSP state accessors cvL()/cvR() (never a
  // shadow mirror), so an oracle can pin exactly what the filter ran.
  void resolveVcfCv_(bool driveGraph) {
    double l = 0.0, r = 0.0;
    const bool lFed = resolveControlSink_(vcfCvL_, l, driveGraph);
    const bool rFed = resolveControlSink_(vcfCvR_, r, driveGraph);
    if (lFed) vcf_.setCvL(l);
    if (rFed) {
      vcf_.setCvR(r);           // explicit R cable takes precedence over the normalling.
    } else if (lFed) {
      vcf_.setCvR(l);           // R unplugged: R follows the already-resolved L this frame.
    }
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
      case ExecutionKind::kEnvelope:   return FixedChainRole::kNone;  // control-only, not a chain role.
      case ExecutionKind::kLfo:        return FixedChainRole::kNone;
      case ExecutionKind::kJoystick:   return FixedChainRole::kNone;
      case ExecutionKind::kSequencer:  return FixedChainRole::kNone;
      case ExecutionKind::kKeyboard:   return FixedChainRole::kNone;  // control-only, not a chain role.
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
  // Generic CV input + VCO output bindings. Id 0 is a REAL jack (vco_a.cv_in), so a
  // JackId{0} sentinel is NOT a valid unbound test — the *_Bound_ flags are the
  // authoritative admission state (same rule as the drone ENV/CV-MOD cohort above).
  JackId cvInA_{0};           bool cvInBoundA_ = false;
  JackId cvInB_{0};           bool cvInBoundB_ = false;
  JackId vcoAOut_{0};         bool vcoAOutBound_ = false;
  JackId vcoBOut_{0};         bool vcoBOutBound_ = false;
  VcoControlMode cvModeA_ = VcoControlMode::kExponential;
  VcoControlMode cvModeB_ = VcoControlMode::kExponential;
  double cvAmtA_ = 1.0;   // CV AMT depth per VCO (setVcoCvAmounts). Default 1 (no attenuation).
  double cvAmtB_ = 1.0;
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

  // Derived (rebuild_, off audio thread): the per-module execution slots — exactly one
  // slot per compiled ModuleId, in the compiled plan (region topo) order, NO ExecutionKind
  // dedup — plus the legacy FixedChainRole inspector array and the break-edge delay
  // lines. Audio path reads these only.
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
  double preampInResolved_ = 0.0;
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
  RuntimeInputs lastIn_;
  double sh3Cv_ = 0.0;  // NEW drone 3 Sample & Hold CV out (not in the audio channel).
  double sh6Cv_ = 0.0;  // NEW drone 6 Sample & Hold CV out (not in the audio channel).

  // Voice sources + fixed chain DSP. The runtime no longer uses SignalPath: the
  // mixer/vcf/dist roll into the plan-driven order instead of a hard-coded chain.
  // task #78 full-apply result (the candidate-builder gate + the applied count).
  bool dspApplyOk_ = false;
  std::uint32_t dspAppliedCount_ = 0;

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

  // Six control sources. The runtime OWNS one stable
  // instance each (D2); the owning definition binds the registry jacks and the
  // always-execute admission list. envGenA_/envGenB_/lfoA_/lfoB_ take sampleRate in
  // the ctor (order matches the init list); joystick_ (stateless) and sequencer_
  // (default ctor, rate set in the body) are default-initialized here.
  EnvelopeGenerator envGenA_;
  EnvelopeGenerator envGenB_;
  Lfo lfoA_;
  Lfo lfoB_;
  JoystickCv joystick_;
  FiveStepSequencer sequencer_;

  // GH#21 continuous control smoothing. One ParameterSmoother per ParameterId-space slot
  // (O(1) index by static_cast<uint32_t>(id)) — a fixed preallocated array, NEVER a map
  // (zero-callback-allocation, contract hard-req #1). Only params whose registry
  // `smoothing == Smoothing::seconds` AND `disposition == applied_to_dsp` advance each
  // frame (the ACTIVE set — 36: 20 control-source + 16 vco/vcf panel-knob); the active
  // ordinals are built at construction from the registry. Persistent across processBlock
  // (partition-independent); tau/sr are fixed at construction.
  std::array<ParameterSmoother, registry::kParameterIdSpace> controlSmoothers_;
  std::array<ParameterId, kMaxControlSmoothParams> controlSmoothOrdinals_;
  std::uint32_t controlSmoothOrdinalCount_ = 0;

  // Envelope A/B published jack ids (gate resolve-input, env out, vca-cv out).
  JackId gateInA_ = JackId{0}, envOutA_ = JackId{0}, vcaOutA_ = JackId{0};
  JackId gateInB_ = JackId{0}, envOutB_ = JackId{0}, vcaOutB_ = JackId{0};
  // LFO / joystick / sequencer published jack ids.
  JackId lfoAOut_ = JackId{0}, lfoBOut_ = JackId{0};
  JackId joyXOut_ = JackId{0}, joyYOut_ = JackId{0};
  JackId seqExtClockIn_ = JackId{0}, seqCvOut_ = JackId{0}, seqGateOut_ = JackId{0},
         seqClockOut_ = JackId{0};
  // GH#12 keyboard product owner: the note-CV / gate output jacks the keyboard publishes.
  JackId kbdVOctOut_ = JackId{0}, kbdGateOut_ = JackId{0};
  // The in-owner note chain (approved option A): an ArpSeq in default Keyboard mode
  // (a transparent pass-through) feeding a KeyboardBehaviour. Both are configured with the
  // machine sample rate in the ctor; only the keyboard behaviour carries per-note state.
  ArpSeq keyboardArpSeq_;
  KeyboardBehaviour keyboardBeh_;

  // Always-execute admission list (six control-source module ids). Unwired sources
  // must still execute once per sample, so the compiler force-includes them. Sized
  // for kMaxAlwaysExecuteSources; tracked by count (ModuleId{0} = vco_a is a REAL
  // module, so it must NOT double as a terminator).
  ModuleId alwaysExecIds_[kMaxAlwaysExecuteSources] = {};
  std::uint32_t alwaysExecCount_ = 0;

  // Most recent control-source transfer outcome (@Codex BLOCKED #1). Set by every
  // applyControlEvent_ parameter event (and precisely by setControlParamValue). The product
  // surface / test oracle reads it back; there is no separate param bank to drift.
  ParameterApplyStatus lastApplyStatus_ = ParameterApplyStatus::unsupported_parameter;
  ParameterId lastApplyParamId_ = ParameterId{};   // == enum value 0 (drone_3_pitch) first.

  // ONE fixed/no-heap gate-clock sink latch per JackId (gate in / seq ext clock).
  // Repatch/rebuild resets the affected latches only; module-internal edges must
  // NOT be re-imported, and a first-high must not cause a phantom advance.
  GateClockSinkState envA_gate_;
  GateClockSinkState envB_gate_;
  GateClockSinkState seqClockLatch_;
};

}  // namespace lunar24::core
