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
#include <lunar24/core/machine_definition.h>

namespace lunar24::host {

// The owner consumes (never re-derives) the frozen core policy, so its body reads in terms of
// the core types directly. These are using-DECLARATIONS (one name each), not a `using namespace`
// — the owner header stays narrow and does not blanket-import core into host scope.
using lunar24::core::BufferLayoutKind;
using lunar24::core::DeviceAdapter;
using lunar24::core::DeviceLayout;
using lunar24::core::DevicePlan;
using lunar24::core::InputRoute;
using lunar24::core::MachineRuntimeDefinition;
using lunar24::core::OutputMapping;

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
};

// ---- prepare --------------------------------------------------------------
inline bool StandaloneAudioEngine::prepare(std::uint64_t seed, double sampleRate,
                                           int maxBlockSize, int inputCapability,
                                           int outputCapability) {
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

  // (2) Heap-construct the FULL candidate definition at the REAL sample rate. The ~160 KiB
  // owned tables (contracts_ / modules_ / fixedEdges_ / routes) live on the heap; the audio
  // callback never needs to build or touch them again. If the graph did not compile, fail
  // closed (never install a partial / broken definition).
  auto cand = std::make_unique<MachineRuntimeDefinition>(seed, sampleRate);
  if (!cand->valid()) {
    clearState_();
    return false;
  }

  // (3) Complete the default plan on a LOCAL candidate adapter. The owner's one honest
  // default-plan helper maps the REAL channel counts to a frozen route (never a silent copy):
  //
  //   inputCapability 0  -> Zero          (both terminals read 0; no channel consumed)
  //   inputCapability 1  -> ExtOnly       (EXT = ch0, PREAMP = none; NO implicit copy)
  //   inputCapability >=2-> Distinct      (EXT = ch0, PREAMP = ch1)
  //
  //   outputCapability 2-3 -> outputCount 2  (WET L/R only)
  //   outputCapability >=4-> outputCount 4  (WET L/R + DRY A/B; extra physical untouched)
  //
  // Every route it can produce is a relation already validated by device_adapter.h; this
  // helper never invents a route the adapter would reject.
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

  DeviceAdapter candAdapter;
  if (!candAdapter.prepare(layout, inputCapability, mapping, outputCount, route, extCh,
                           preampCh)) {
    clearState_();
    return false;
  }

  // (4) COMMIT in one shot: releases the OLD definition (its destructor runs ONCE here, the
  // stopped-stream boundary) and installs the new definition + adapter + format together. No
  // intermediate state is observable — from the caller's view this is atomic.
  definition_ = std::move(cand);
  adapter_ = candAdapter;
  sampleRate_ = sampleRate;
  blockSize_ = maxBlockSize;
  inputCapability_ = inputCapability;
  outputCapability_ = outputCapability;
  ready_ = true;
  return true;
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

}  // namespace lunar24::host
