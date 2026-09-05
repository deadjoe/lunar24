// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// standalone_audio_engine.h — the framework-free host audio-engine owner (GH#4 8B2).
//
// This is the ONE bridge between the iPlug2 host (LunarHostPlugin) and the product
// synth. The host holds it BY VALUE and calls EXACTLY two things: prepare() (on the
// non-audio / stopped-stream thread) and processBlock() (the audio callback). It must
// therefore:
//
//   * be framework-free — it only includes lunar24::core headers (never an iPlug2 /
//     RtAudio / IPlugPlugin type), so it works in a plain C++ test with no host plumbing.
//   * own an ADDRESS-STABLE runtime — the product SynthRuntime stores POINTERS into its
//     MachineRuntimeDefinition (`machine_definition.h` says the definition is deliberately
//     NON-COPYABLE / NON-MOVABLE and must be held at a stable address for its whole life).
//     So the owner keeps the definition behind a unique_ptr; the ~160 KiB owned tables
//     live on the heap, NEVER on the audio-callback stack.
//   * prepare() ONLY on the stopped-stream boundary — the whole candidate definition +
//     device plan is built on LOCAL (heap-owning) state and committed in ONE shot; any
//     failure leaves the engine NOT-READY (never a half-written plan, never a stale
//     different-sample-rate runtime left behind).
//   * processBlock() be a pure delegate — it forwards the whole block to the frozen
//     task#71 DeviceAdapter::renderBlock and does NO frame loop / scale / mapping /
//     pass-through / second output bank of its own.
//
// The GH#4 frozen policy (design/07 §5, task#71) is consumed, not re-derived here: the
// device scale (0.5), the output strategy (<2 reject, 2-3 WET, >=4 WET+DRY), and the
// input-route rules (<2 route = explicit, never an implicit copy) live in device_adapter.h.
// This owner only CHOOSES the default plan the host needs given the REAL connected
// channel counts (the mandate's "default-plan helper"), and feeds it to the adapter.

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>

#include <lunar24/core/device_adapter.h>
#include <lunar24/core/machine_candidate.h>   // buildMachineRuntimeCandidate (state-aware builder)
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/state_default.h>       // make_default_device_state (safe-boot default)
#include <lunar24/core/state_validation.h>    // StateValidationResult (inspectable reject family/field)

namespace lunar24::host {

// The owner consumes (never re-derives) the frozen core policy, so its body reads in terms of
// the core types directly. These are using-DECLARATIONS (one name each), not a `using namespace`
// — the owner header stays narrow and does not blanket-import core into host scope.
using lunar24::core::BufferLayoutKind;
using lunar24::core::DeviceAdapter;
using lunar24::core::DeviceLayout;
using lunar24::core::DevicePlan;
using lunar24::core::DeviceStateV1;
using lunar24::core::InputRoute;
using lunar24::core::MachineCandidateResult;
using lunar24::core::MachineCandidateStatus;
using lunar24::core::MachineRuntimeDefinition;
using lunar24::core::OutputMapping;
using lunar24::core::StateValidationResult;
using lunar24::core::SynthRuntime;
using lunar24::core::buildMachineRuntimeCandidate;
using lunar24::core::make_default_device_state;
using lunar24::core::ParameterId;
using lunar24::core::ParameterApplyStatus;
using lunar24::core::kParameterCount;

// The centralized, deterministic provisional safe-startup seed. This is what the host
// uses to boot the machine BEFORE the #12 identity / state layer can apply a saved
// program. It is deliberately NOT a third truth source — it is a named, stable,
// reproducible default, not an authoritative machine-program identity (that is #12's
// job, out of scope for this slice). "LUNAR" as hex, so it is greppable and stable.
inline constexpr std::uint64_t kLunarStartupSeed = 0x4C554E4152ULL;

// The one repo-owned standalone host runtime owner. Held BY VALUE by LunarHostPlugin.
class StandaloneAudioEngine {
 public:
  // A fixed, inspectable outcome of a processBlock() call. The host (and the oracle)
  // read this AFTER the call; it is never logged / allocated on the audio path.
  enum class Status : std::uint8_t {
    Idle = 0,               // no successful prepare() yet.
    Rendered,               // the block went through the production delegate.
    DroppedNotReady,        // prepare() not done or failed -> deterministic silence.
    DroppedFormatMismatch,  // the requested channel config differs from the prepared one.
    DroppedIllegal,         // frames <= 0 OR frames > prepared maxBlock (cannot render / block overrun).
    DroppedInvalidDefinition,  // the active definition's graph did not compile -> never render it.
  };

  // A fixed, inspectable outcome of applyDeviceState(). This is deliberately the INVERSE failure
  // contract of prepare(): prepare() leaves the engine NOT-READY on any failure (task#72, the
  // host is re-configuring for a NEW stream), while an applyDeviceState rejection is ATOMIC — the
  // prior complete active definition/plan/format/canonical state is left UNCHANGED. Both share
  // the same state-aware candidate builder (buildMachineRuntimeCandidate); only the commitment
  // differs. This is the GH#12 9B state-apply contract (@Codex msg f7860189, card rev 2 point 2).
  enum class StateApplyStatus : std::uint8_t {
    NotAttempted = 0,     // no applyDeviceState() / successful prepare()-only yet.
    Accepted,             // the state validated, the machine compiled, identity configured, committed.
    RejectedFormat,       // illegal sample rate / block size / channel capability.
    RejectedInvalidState, // validate_device_state failed (family+field via lastStateValidation()).
    RejectedGraph,        // state validated but the candidate graph did not compile.
    RejectedIdentity,     // state+graph ok but the GH#6 identity/calibration did not configure.
    RejectedDspApply,     // state+graph+identity ok but the whole 169-parameter applied_to_DSP
                          // apply was not complete (first failure via the DspApply accessor).
    RejectedAdapter,      // state+graph+identity ok but the channel plan could not be built.
  };

  StandaloneAudioEngine() = default;
  // Owns a unique_ptr<MachineRuntimeDefinition> plus the single DeviceAdapter (both are
  // non-copyable for different reasons): the definition is non-movable by contract, and the
  // owner is held by value in the plugin and must never be identity-aliased. Deleting the
  // copy/move is the explicit intent, not an accidental non-copyability.
  StandaloneAudioEngine(const StandaloneAudioEngine&) = delete;
  StandaloneAudioEngine& operator=(const StandaloneAudioEngine&) = delete;
  StandaloneAudioEngine(StandaloneAudioEngine&&) = delete;
  StandaloneAudioEngine& operator=(StandaloneAudioEngine&&) = delete;

  // NON-audio prepare, called ONLY at the stopped-stream boundary (CloseAudio callbacks done
  // -> SetBlockSize/SetSampleRate -> OnReset -> openStream/startStream). Builds a complete
  // candidate MachineRuntimeDefinition at the REAL sample rate on the heap, verifies the
  // graph compiled, completes the default plan on a LOCAL candidate adapter, then commits
  // owner+adapter+format in ONE shot. On ANY failure this leaves the engine NOT-READY and
  // releases the OLD definition (a stale different-sample-rate runtime is never kept — the
  // host is re-configuring for a NEW stream, and a half-write is never observable).
  //
  //   seed            — the deterministic startup seed (kLunarStartupSeed for a safe boot).
  //   sampleRate      — the REAL device rate; must be finite and > 0.
  //   maxBlockSize    — the device max block size (host GetBlockSize()); must be > 0.
  //   inputCapability — physical input channels the device opened (>= 0).
  //   outputCapability— physical output channels the device opened (>= 2; <2 cannot satisfy
  //                     the frozen WET output strategy and is REJECTED).
  bool prepare(std::uint64_t seed, double sampleRate, int maxBlockSize, int inputCapability,
               int outputCapability);

  // GH#12 9B: publish a validated DeviceStateV1 candidate at the stopped-stream boundary. Builds
  // the definition + default plan as LOCAL candidates (same builder as prepare), then commits
  // definition + adapter + format + canonical state together ONCE. A REJECTION is atomic — the
  // prior complete active definition/plan/format/canonical state is unchanged and the engine stays
  // ready (it does NOT go NOT-READY). Returns the inspectable StateApplyStatus.
  StateApplyStatus applyDeviceState(const DeviceStateV1& state, double sampleRate, int maxBlockSize,
                                    int inputCapability, int outputCapability);

  // The production block delegate — the ONLY render entry the host's ProcessBlock calls. The
  // channel counts and block size are the ACTUAL device facts right now. Returns the Status; a
  // dropped block has already had deterministic silence written into the OUTPUT buffers, so the
  // host's ProcessBlock is a pure delegate with no further work. This path allocates nothing,
  // locks nothing, and logs nothing.
  Status processBlock(const double* const* inputs, double* const* outputs, int inCh, int outCh,
                      int frames);

  // ---- inspectable (never mutated on the audio path beyond the monotonic counters) ----
  bool isReady() const { return ready_; }
  double sampleRate() const { return sampleRate_; }
  int blockSize() const { return blockSize_; }
  int inputCapability() const { return inputCapability_; }
  int outputCapability() const { return outputCapability_; }
  const DevicePlan& plan() const { return adapter_.plan(); }
  std::uint64_t nonFiniteSamples() const { return adapter_.nonFiniteSamples(); }
  std::uint64_t renderedBlocks() const { return renderedBlocks_; }
  std::uint64_t droppedBlocks() const { return droppedBlocks_; }

  // The canonical DeviceStateV1 of the active definition, read straight from the owned definition
  // (never a second snapshot living elsewhere — task#76 card rev 2 point 3). nullptr iff not ready.
  const DeviceStateV1* canonicalState() const {
    return definition_ ? &definition_->deviceState() : nullptr;
  }
  // Whether the GH#6 identity/calibration profile was applied to the active definition. Named
  // precisely: this is the identity/calibration apply, NOT a whole-DeviceState "applied" claim.
  bool identityApplied() const { return definition_ && definition_->identityApplied(); }
  // The last applyDeviceState() outcome (or NotAttempted). A RejectedInvalidState's family+field
  // detail is exposed by lastStateValidation().
  StateApplyStatus stateApplyStatus() const { return stateApplyStatus_; }
  // The active machine's SynthRuntime, read straight from the owned definition (non-shadow truth,
  // self-consistent with canonicalState()). nullptr iff not ready. For the GH#6 wired-path check:
  // the caller reads the actual derived profile consumers via the SynthRuntime getters.
  //
  // ⚠️ LIFETIME (REV-3): the returned pointer is valid only until the NEXT commit_() — i.e. until
  // the next successful prepare()/applyDeviceState() — because each apply swaps `definition_` for a
  // freshly-built candidate and RELEASES the prior definition. A pointer held across a churn apply
  // would then dangle. For a test that must read getters across multiple applies, use the by-value
  // observeRuntime() snapshot instead (copying scalars out at the moment of the call); this accessor
  // is for read-at-this-instant use with NO intervening apply.
  const SynthRuntime* runtime() const { return definition_ ? &definition_->runtime() : nullptr; }

  // A by-value snapshot of the LIVE GH#6 identity/calibration consumers plus the seeded-voice
  // outputs of the active definition, read via the SynthRuntime getters ONCE at call time. This is
  // the safe way for a caller (the oracle) to inspect runtime getters across multiple applies: it
  // never holds a pointer across an apply, so a long-lived `runtime()` pointer that dangles after
  // the next applyDeviceState/commit cannot be captured. Scalars only — nothing here aliases the
  // owned definition.
  struct RuntimeObservation {
    bool identityConfigured = false;
    double vcfInputDrive[2] = {0.0, 0.0};
    double distortionDrive[2] = {0.0, 0.0};
    double distortionRail[2] = {0.0, 0.0};
    double vcfPathStagingGain[2] = {0.0, 0.0};
    double seededVoice[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};  // droneChannel(0..3), drone3, drone6
  };
  RuntimeObservation observeRuntime() const {
    RuntimeObservation obs;
    const SynthRuntime* rt = definition_ ? &definition_->runtime() : nullptr;
    if (rt == nullptr) return obs;
    obs.identityConfigured = rt->vcfIdentityConfigured();
    for (int c = 0; c < 2; ++c) {
      obs.vcfInputDrive[c] = rt->vcfInputDrive(c);
      obs.distortionDrive[c] = rt->distortionDrive(c);
      obs.distortionRail[c] = rt->distortionRail(c);
      obs.vcfPathStagingGain[c] = rt->vcfPathStagingGain(c);
    }
    for (int c = 0; c < 6; ++c) {
      if (c < 4) obs.seededVoice[c] = rt->droneChannel(c);
      else if (c == 4) obs.seededVoice[c] = rt->drone3Channel();
      else obs.seededVoice[c] = rt->drone6Channel();
    }
    return obs;
  }
  const StateValidationResult& lastStateValidation() const { return lastStateValidation_; }
  // task #78: the first applied_to_DSP parameter whose write was rejected, and the reason, on
  // a RejectedDspApply outcome (sentinel kParameterCount / applied otherwise). Lets a caller
  // surface the exact failing id/status instead of only a coarse "rejected" bit.
  ParameterId dspApplyFirstFailParamId() const { return dspApplyFirstFailParamId_; }
  ParameterApplyStatus dspApplyFirstFailStatus() const { return dspApplyFirstFailStatus_; }

 private:
  // Write deterministic silence into the caller's ACTUAL output channels (bounded to what the
  // caller owns). Used on every dropped path so a dropped block is never a stale / partial /
  // repeated buffer. The audio path may touch no heap — this is a plain memset-like loop.
  void writeSilence_(double* const* outputs, int outCh, int frames) const;

  // Clear the ENTIRE committed state back to the "no prepare done" sentinel. Called on every
  // prepare() failure so no half-written plan / stale format / old definition is observable:
  // the engine returns to NOT-READY with every inspectable field at its empty value, and the
  // old definition (if any) is released here — the ONLY place its destructor runs (the
  // stopped-stream boundary).
  void clearState_();

  // Complete the frozen default plan (the owner's one honest default-plan helper) on a LOCAL
  // adapter. Shared by prepare() and applyDeviceState(); never mutates committed state itself.
  // Returns false if the channel configuration cannot be planned (never invents a route).
  static bool completeDefaultPlan_(DeviceAdapter& adapter, int inputCapability, int outputCapability);

  // Commit a built candidate + adapter + format in ONE shot (releases the old definition at this
  // stopped-stream boundary). The caller has already decided success; this only installs.
  void commit_(std::unique_ptr<MachineRuntimeDefinition> cand, const DeviceAdapter& candAdapter,
               double sampleRate, int maxBlockSize, int inputCapability, int outputCapability);

  // The address-stable runtime owner. Lives on the heap (never the callback stack), and is
  // released ONLY at the prepare()/OnReset stopped-stream boundary, never inside processBlock.
  std::unique_ptr<MachineRuntimeDefinition> definition_;
  // The single production DeviceAdapter (task#71). The preferred default plan is installed by
  // prepare(); processBlock only ever forwards to it.
  DeviceAdapter adapter_;

  // The committed format (set only by a successful prepare; the processBlock format gate reads
  // these). Zero/empty values mean "no committed format" -> not-ready.
  double sampleRate_ = 0.0;
  int blockSize_ = 0;
  int inputCapability_ = 0;
  int outputCapability_ = 0;
  bool ready_ = false;

  // Monotonic RT counters (plain, no lock).
  std::uint64_t renderedBlocks_ = 0;
  std::uint64_t droppedBlocks_ = 0;

  // The inspectable state-apply outcome (GH#12 9B) and the last validation detail. Only set by
  // applyDeviceState() (and Accepted by a successful prepare(), which publishes the default state).
  StateApplyStatus stateApplyStatus_ = StateApplyStatus::NotAttempted;
  StateValidationResult lastStateValidation_;
  // task #78 first applied_to_DSP failure detail (sentinel kParameterCount / applied iff ok).
  ParameterId dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;
};

// ---- prepare --------------------------------------------------------------
inline bool StandaloneAudioEngine::prepare(std::uint64_t seed, double sampleRate,
                                           int maxBlockSize, int inputCapability,
                                           int outputCapability) {
  // Reset the host DSP first-fail diagnostic at the START of every prepare()-only, exactly as
  // applyDeviceState() does below (337-338), so a prior RejectedDspApply never leaks a stale
  // failure into a later successful/other prepare() on the same owner.
  dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;
  // (1) Impossible / illegal format -> fail-closed. On ANY of these the OLD definition is
  // released and the engine goes NOT-READY: the host is re-configuring for a new stream, and
  // keeping a stale sample-rate runtime would be exactly the "old runtime kept" defect. A
  // non-finite sample rate is rejected before it can be passed to the runtime.
  if (!std::isfinite(sampleRate) || !(sampleRate > 0.0)) {
    clearState_();
    return false;
  }
  if (maxBlockSize <= 0 || inputCapability < 0 || outputCapability < 2) {
    clearState_();
    return false;
  }

  // (2) Safe boot = the power-on DEFAULT DeviceState, built through the SAME state-aware candidate
  // builder that applyDeviceState() uses (card rev 2 point 2: "successful safe boot must delegate
  // to the same builder"). This is what makes a seed denote the exact make_default_device_state(seed)
  // canonical state, and what applies the GH#6 identity/calibration profile to the default too.
  // The ~160 KiB owned tables live on the heap (definition behind a unique_ptr); the audio callback
  // never builds or touches them.
  MachineCandidateResult res = buildMachineRuntimeCandidate(make_default_device_state(seed),
                                                            sampleRate);
  if (res.status != MachineCandidateStatus::accepted) {
    clearState_();
    return false;
  }

  // (3) Complete the default plan on a LOCAL candidate adapter. The owner's one honest default-plan
  // helper maps the REAL channel counts to a frozen route (never a silent copy); every route it can
  // produce is already validated by device_adapter.h.
  DeviceAdapter candAdapter;
  if (!completeDefaultPlan_(candAdapter, inputCapability, outputCapability)) {
    clearState_();
    return false;
  }

  // (4) COMMIT in one shot: releases the OLD definition (its destructor runs ONCE here, the
  // stopped-stream boundary) and installs the new definition + adapter + format together. No
  // intermediate state is observable — from the caller's view this is atomic.
  commit_(std::move(res.definition), candAdapter, sampleRate, maxBlockSize,
          inputCapability, outputCapability);
  lastStateValidation_ = res.validation;   // ok (the default state validated)
  stateApplyStatus_ = StateApplyStatus::Accepted;
  return true;
}

// ---- applyDeviceState ------------------------------------------------------
inline StandaloneAudioEngine::StateApplyStatus StandaloneAudioEngine::applyDeviceState(
    const DeviceStateV1& state, double sampleRate, int maxBlockSize, int inputCapability,
    int outputCapability) {
  // INVERSE failure contract of prepare() (card rev 2 point 2): a rejection here is ATOMIC — the
  // prior complete active definition / plan / format / canonical state is left unchanged and the
  // engine STAYS ready. It never goes NOT-READY on a bad state: this is a "state" problem, not a
  // stream re-configuration problem, so the current stream keeps running with the prior canonical
  // state. A rejected candidate is never observable as an intermediate or partial install.

  // @Codex BLOCK #5: reset the DSP first-fail diagnostics to the "no failure" sentinel at the entry
  // of every apply. Only a RejectedDspApply outcome overwrites them below; every other terminal
  // outcome (Accepted or any other Rejected*) leaves them at the sentinel, so a prior DSP failure
  // never leaks into a later apply's diagnostics. Without this, the stale failure survived into a
  // subsequent Accepted apply (the "otherwise sentinel" contract was never honoured).
  dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;

  // Strict-format gate mirrors prepare()'s first two checks — an illegal rate / block / channel set
  // cannot possibly honour the requested state, so it is a FORMAT rejection, not a state rejection.
  if (!std::isfinite(sampleRate) || !(sampleRate > 0.0)) {
    stateApplyStatus_ = StateApplyStatus::RejectedFormat;
    lastStateValidation_ = StateValidationResult{};   // no validation performed
    return StateApplyStatus::RejectedFormat;
  }
  if (maxBlockSize <= 0 || inputCapability < 0 || outputCapability < 2) {
    stateApplyStatus_ = StateApplyStatus::RejectedFormat;
    lastStateValidation_ = StateValidationResult{};
    return StateApplyStatus::RejectedFormat;
  }

  // The candidate builder already reports a SPLIT outcome (format / state / graph / identity).
  // Map each to its own StateApplyStatus; only an accepted candidate proceeds to the adapter plan,
  // whose own failure is RejectedAdapter (never collapsed into a "machine" rejection).
  MachineCandidateResult res = buildMachineRuntimeCandidate(state, sampleRate);
  switch (res.status) {
    case MachineCandidateStatus::rejected_format:
      stateApplyStatus_ = StateApplyStatus::RejectedFormat;
      lastStateValidation_ = res.validation;
      return StateApplyStatus::RejectedFormat;
    case MachineCandidateStatus::rejected_state:
      stateApplyStatus_ = StateApplyStatus::RejectedInvalidState;
      lastStateValidation_ = res.validation;   // family+field for the caller
      return StateApplyStatus::RejectedInvalidState;
    case MachineCandidateStatus::rejected_graph:
      stateApplyStatus_ = StateApplyStatus::RejectedGraph;
      lastStateValidation_ = res.validation;
      return StateApplyStatus::RejectedGraph;
    case MachineCandidateStatus::rejected_identity:
      stateApplyStatus_ = StateApplyStatus::RejectedIdentity;
      lastStateValidation_ = res.validation;
      return StateApplyStatus::RejectedIdentity;
    case MachineCandidateStatus::rejected_dsp_apply:
      stateApplyStatus_ = StateApplyStatus::RejectedDspApply;
      lastStateValidation_ = res.validation;
      dspApplyFirstFailParamId_ = res.firstFailParamId;
      dspApplyFirstFailStatus_ = res.firstFailStatus;
      return StateApplyStatus::RejectedDspApply;
    case MachineCandidateStatus::accepted:
      break;
  }

  // Accepted: complete the default plan on a LOCAL adapter, then commit ALL in one shot.
  DeviceAdapter candAdapter;
  if (!completeDefaultPlan_(candAdapter, inputCapability, outputCapability)) {
    stateApplyStatus_ = StateApplyStatus::RejectedAdapter;
    lastStateValidation_ = res.validation;
    return StateApplyStatus::RejectedAdapter;
  }
  commit_(std::move(res.definition), candAdapter, sampleRate, maxBlockSize,
          inputCapability, outputCapability);
  lastStateValidation_ = res.validation;   // ok
  stateApplyStatus_ = StateApplyStatus::Accepted;
  return StateApplyStatus::Accepted;
}

// ---- processBlock ---------------------------------------------------------
inline StandaloneAudioEngine::Status StandaloneAudioEngine::processBlock(
    const double* const* inputs, double* const* outputs, int inCh, int outCh, int frames) {
  // Illegal frame count: nothing to render, nothing to silence.
  if (frames <= 0) {
    ++droppedBlocks_;
    return Status::DroppedIllegal;
  }
  // Not ready: the mandatory deterministic-silence fallback (never a stale buffer).
  if (!ready_ || definition_ == nullptr) {
    ++droppedBlocks_;
    writeSilence_(outputs, outCh, frames);
    return Status::DroppedNotReady;
  }
  // A callback must never exceed the prepared max block. The device batches strictly within the
  // OnReset configuration (GetBlockSize()), so frames beyond blockSize_ is a kernel/wiring defect
  // -> the block is dropped to deterministic silence with an exact counter. This runs AFTER the
  // not-ready gate (blockSize_ is only meaningful once prepared) and BEFORE the channel-mismatch
  // gate (a block overrun is the earlier, more fundamental violation).
  if (frames > blockSize_) {
    ++droppedBlocks_;
    writeSilence_(outputs, outCh, frames);
    return Status::DroppedIllegal;
  }

  // Format mismatch: the device is asking for a channel config we did NOT prepare. A host
  // that opens a different channel set after OnReset would be a real wiring bug -> silence
  // rather than a mis-scoped render.
  if (inCh != inputCapability_ || outCh != outputCapability_) {
    ++droppedBlocks_;
    writeSilence_(outputs, outCh, frames);
    return Status::DroppedFormatMismatch;
  }

  // Defense-in-depth (card rev 2 point 1): a definition whose graph did NOT compile must never be
  // rendered. buildMachineRuntimeCandidate already rejects such a candidate before commit, so in
  // practice definition_ is always valid here; this guard forbids an invalid definition reaching
  // processBlock through any other construction, closing the "mint a valid-looking machine from an
  // invalid state" counter-example. Drop to deterministic silence + counter; never a partial render.
  if (!definition_->valid()) {
    ++droppedBlocks_;
    writeSilence_(outputs, outCh, frames);
    return Status::DroppedInvalidDefinition;
  }

  // THE single production delegate (task#71). The owner forwards the whole block to the
  // frozen adapter and does nothing else — no frame loop, no scaling, no mapping, no
  // pass-through. A second output bank / a host-side scale would be a wiring defect.
  adapter_.renderBlock(definition_->runtime(), inputs, outputs, frames);
  ++renderedBlocks_;
  return Status::Rendered;
}

// ---- clearState_ ----------------------------------------------------------
inline void StandaloneAudioEngine::clearState_() {
  // Release the old definition (its destructor runs ONCE here — the stopped-stream boundary)
  // and reset every committed-format field to the empty "no prepare done" sentinel. After
  // this the engine is fully NOT-READY with nothing inspectable left half-written.
  definition_.reset();
  adapter_ = DeviceAdapter{};
  sampleRate_ = 0.0;
  blockSize_ = 0;
  inputCapability_ = 0;
  outputCapability_ = 0;
  ready_ = false;
}

// ---- writeSilence_ --------------------------------------------------------
inline void StandaloneAudioEngine::writeSilence_(double* const* outputs, int outCh,
                                                 int frames) const {
  if (outputs == nullptr) return;
  // Bounded to what the caller actually owns. `outCh` is the HOST's real channel count (it
  // may differ from the prepared format on a mismatch, so it is the correct bound for the
  // buffers the host supplied). No heap, no lock, no log.
  for (int c = 0; c < outCh; ++c) {
    if (outputs[c] == nullptr) continue;
    double* p = outputs[c];
    for (int f = 0; f < frames; ++f) p[f] = 0.0;
  }
}

// ---- completeDefaultPlan_ -------------------------------------------------
inline bool StandaloneAudioEngine::completeDefaultPlan_(DeviceAdapter& adapter, int inputCapability,
                                                        int outputCapability) {
  // The owner's ONE honest default-plan helper (the frozen GH#4 policy, consumed not re-derived).
  // Maps the REAL channel counts to a frozen route (never a silent copy):
  //
  //   inputCapability 0  -> Zero          (both terminals read 0; no channel consumed)
  //   inputCapability 1  -> ExtOnly       (EXT = ch0, PREAMP = none; NO implicit copy)
  //   inputCapability >=2-> Distinct      (EXT = ch0, PREAMP = ch1)
  //
  //   outputCapability 2-3 -> outputCount 2  (WET L/R only)
  //   outputCapability >=4-> outputCount 4  (WET L/R + DRY A/B; extra physical untouched)
  //
  // Every route it can produce is already validated by device_adapter.h; it never invents a route
  // the adapter would reject. Returns false if the channel config cannot be planned.
  const int outputCount = (outputCapability <= 3) ? 2 : 4;
  const DeviceLayout layout{BufferLayoutKind::NonInterleaved, outputCapability};
  const OutputMapping mapping = OutputMapping::canonical();

  InputRoute route = InputRoute::Zero;
  int extCh = -1;
  int preampCh = -1;
  if (inputCapability == 1) {
    route = InputRoute::ExtOnly;
    extCh = 0;
  } else if (inputCapability >= 2) {
    route = InputRoute::Distinct;
    extCh = 0;
    preampCh = 1;
  }

  return adapter.prepare(layout, inputCapability, mapping, outputCount, route, extCh, preampCh);
}

// ---- commit_ --------------------------------------------------------------
inline void StandaloneAudioEngine::commit_(std::unique_ptr<MachineRuntimeDefinition> cand,
                                           const DeviceAdapter& candAdapter, double sampleRate,
                                           int maxBlockSize, int inputCapability,
                                           int outputCapability) {
  // Install definition + adapter + format + canonical state in ONE shot. Releasing the OLD
  // definition here (its destructor runs ONCE) is the stopped-stream boundary; the new definition,
  // plan, format, and canonical state become visible atomically. No intermediate state is
  // observable from the caller's view.
  definition_ = std::move(cand);
  adapter_ = candAdapter;
  sampleRate_ = sampleRate;
  blockSize_ = maxBlockSize;
  inputCapability_ = inputCapability;
  outputCapability_ = outputCapability;
  ready_ = true;
}

}  // namespace lunar24::host
