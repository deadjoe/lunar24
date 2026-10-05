// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// standalone_audio_engine.h — the framework-free host audio-engine owner.
//
// This is the ONE bridge between the iPlug2 host (LunarHostPlugin) and the product
// synth. The host holds it BY VALUE and calls EXACTLY two things: prepare (on the
// non-audio / stopped-stream thread) and processBlock (the audio callback). It must
// therefore:
//
//   * be framework-free — it only includes lunar24::core headers (never an iPlug2 /
//     RtAudio / IPlugPlugin type), so it works in a plain C++ test with no host plumbing.
//   * own an ADDRESS-STABLE runtime — the product SynthRuntime stores POINTERS into its
//     MachineRuntimeDefinition (`machine_definition.h` says the definition is deliberately
//     NON-COPYABLE / NON-MOVABLE and must be held at a stable address for its whole life).
//     So the owner keeps the definition behind a unique_ptr; the ~160 KiB owned tables
//     live on the heap, NEVER on the audio-callback stack.
//   * prepare ONLY on the stopped-stream boundary — the whole candidate definition +
//     device plan is built on LOCAL (heap-owning) state and committed in ONE shot; any
//     failure leaves the engine NOT-READY (never a half-written plan, never a stale
//     different-sample-rate runtime left behind).
//   * processBlock be a pure delegate — it forwards the whole block to the frozen
//      DeviceAdapter::renderBlock and does NO frame loop / scale / mapping /
//     pass-through / second output bank of its own.
//
// The frozen policy is consumed, not re-derived here: the
// device scale (0.5), the output strategy (<2 reject, 2-3 WET, >=4 WET+DRY), and the
// input-route rules (<2 route = explicit, never an implicit copy) live in device_adapter.h.
// This owner only CHOOSES the default plan the host needs given the REAL connected
// channel counts (the mandate's "default-plan helper"), and feeds it to the adapter.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#endif

#include <lunar24/core/live_command_queue.h>
#include <lunar24/core/midi_map.h>
#include <lunar24/core/state_edit.h>
#include <lunar24/core/device_adapter.h>
#include <lunar24/core/keyboard_presets.h>
#include <lunar24/core/knob_taper.h>       // knob position <-> value (time / rate tapers)    // load/save/initialise preset transfer helpers
#include <lunar24/core/machine_candidate.h>   // buildMachineRuntimeCandidate (state-aware builder)
#include <lunar24/core/machine_definition.h>
#include <lunar24/core/state_default.h>       // make_default_device_state (safe-boot default)
#include <lunar24/core/state_validation.h>    // StateValidationResult (inspectable reject family/field)

namespace lunar24::host {

// Where the REC recorder taps the outputs (host/wav_recorder.h implements it). Called on the
// audio thread: armed is checked once per block, push gets the block's frames as 4
// floats each (WET L, WET R, DRY A, DRY B), after MUTE.
class AudioTap {
 public:
  virtual ~AudioTap() = default;
  virtual bool armed() const = 0;
  virtual void push(const float* frames, int count) = 0;
};

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
// uses to boot the machine BEFORE the identity / state layer can apply a saved
// program. It is deliberately NOT a third truth source — it is a named, stable,
// reproducible default, not an authoritative machine-program identity (that is 's
// job, out of scope for this slice). "LUNAR" as hex, so it is greppable and stable.
inline constexpr std::uint64_t kLunarStartupSeed = 0x4C554E4152ULL;

// Flush denormal floats to zero for the duration of a block (restores the caller's mode).
// Filter and reverb tails that decay toward zero otherwise enter the denormal range, where
// x86 CPUs slow down sharply. ARM64 sets FPCR.FZ; other targets do nothing.
class ScopedFlushDenormals {
 public:
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  ScopedFlushDenormals() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }  // FTZ | DAZ
  ~ScopedFlushDenormals() { _mm_setcsr(saved_); }
 private:
  unsigned int saved_;
#elif defined(__aarch64__)
  ScopedFlushDenormals() {
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_ | (1ull << 24)));
  }
  ~ScopedFlushDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_)); }
 private:
  unsigned long long saved_ = 0;
#else
  ScopedFlushDenormals() = default;
#endif
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
};

// The one repo-owned standalone host runtime owner. Held BY VALUE by LunarHostPlugin.
class StandaloneAudioEngine {
 public:
  // A fixed, inspectable outcome of a processBlock call. The host (and the oracle)
  // read this AFTER the call; it is never logged / allocated on the audio path.
  enum class Status : std::uint8_t {
    Idle = 0,               // no successful prepare yet.
    Rendered,               // the block went through the production delegate.
    DroppedNotReady,        // prepare not done or failed -> deterministic silence.
    DroppedFormatMismatch,  // the requested channel config differs from the prepared one.
    DroppedIllegal,         // frames <= 0 OR frames > prepared maxBlock (cannot render / block overrun).
    DroppedInvalidDefinition,  // the active definition's graph did not compile -> never render it.
  };

  // A fixed, inspectable outcome of applyDeviceState. This is deliberately the INVERSE failure
  // contract of prepare: prepare leaves the engine NOT-READY on any failure (the
  // host is re-configuring for a NEW stream), while an applyDeviceState rejection is ATOMIC — the
  // prior complete active definition/plan/format/canonical state is left UNCHANGED. Both share
  // the same state-aware candidate builder (buildMachineRuntimeCandidate); only the commitment
  // differs. This is the state-apply contract.
  enum class StateApplyStatus : std::uint8_t {
    NotAttempted = 0,     // no applyDeviceState / successful prepare-only yet.
    Accepted,             // the state validated, the machine compiled, identity configured, committed.
    RejectedFormat,       // illegal sample rate / block size / channel capability.
    RejectedInvalidState, // validate_device_state failed (family+field via lastStateValidation).
    RejectedGraph,        // state validated but the candidate graph did not compile.
    RejectedIdentity,     // state+graph ok but the identity/calibration did not configure.
    RejectedDspApply,     // state+graph+identity ok but the whole 169-parameter applied_to_DSP
                          // apply was not complete (first failure via the DspApply accessor).
    RejectedAdapter,      // state+graph+identity ok but the channel plan could not be built.
  };

  // engine-layer preset actions ------------------------------------------
  // LOAD / SAVE / INITIALISE of the four native keyboard presets, expressed at the ENGINE layer.
  // This is an INTERNAL engine enum: it never enters the wire format and is never a ControlEvent
  // (it is neither a ParameterId nor a ControlEventKind), so no new id/enum leaks into the device
  // protocol or the state schema.
  enum class PresetAction : std::uint8_t {
    Load = 0,        // recall the slot's payload into the live keyboard state
    Save = 1,        // snapshot the committed canonical live config into the slot
    Initialise = 2,  // reset ONLY the slot to its factory default (never implicitly loads it)
  };

  // The fixed, inspectable outcome of applyPresetAction. A not-ready engine / illegal slot /
  // unknown action is reported explicitly — never a false success. A candidate-level rejection is
  // reported as RejectedState with the exact StateApplyStatus in stateApplyStatus and the
  // family+field in lastStateValidation.
  enum class PresetActionStatus : std::uint8_t {
    Accepted = 0,           // the state-layer transfer applied and the candidate committed
    RejectedNotReady,       // no committed definition -> nothing to read or re-publish
    RejectedInvalidSlot,    // slot >= kDeviceKeyboardPresetCount (there is no 5th preset)
    RejectedInvalidAction,  // not one of Load / Save / Initialise
    RejectedState,          // the re-commit candidate was rejected; see stateApplyStatus
  };

  StandaloneAudioEngine() = default;
  ~StandaloneAudioEngine() { releaseGraphPlans_(); }
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
  //   seed — the deterministic startup seed (kLunarStartupSeed for a safe boot).
  //   sampleRate — the REAL device rate; must be finite and > 0.
  //   maxBlockSize — the device max block size (host GetBlockSize); must be > 0.
  //   inputCapability — physical input channels the device opened (>= 0).
  //   outputCapability— physical output channels the device opened (>= 2; <2 cannot satisfy
  //                     the frozen WET output strategy and is REJECTED).
  bool prepare(std::uint64_t seed, double sampleRate, int maxBlockSize, int inputCapability,
               int outputCapability);

  // publish a validated DeviceStateV1 candidate at the stopped-stream boundary. Builds
  // the definition + default plan as LOCAL candidates (same builder as prepare), then commits
  // definition + adapter + format + canonical state together ONCE. A REJECTION is atomic — the
  // prior complete active definition/plan/format/canonical state is unchanged and the engine stays
  // ready (it does NOT go NOT-READY). Returns the inspectable StateApplyStatus.
  StateApplyStatus applyDeviceState(const DeviceStateV1& state, double sampleRate, int maxBlockSize,
                                    int inputCapability, int outputCapability);

  // the engine-layer preset LOAD / SAVE / INITIALISE. Call ONLY at the existing
  // stopped-stream / non-audio-thread boundary (the same place prepare/applyDeviceState are
  // called) — it re-commits a complete candidate and is NOT a running-stream operation.
  //
  //   * Format: the engine's CURRENT committed format is reused; the caller does not restate it.
  //   * Path: copy the canonical DeviceStateV1, apply the ONE existing state-layer transfer helper
  //     (load_preset_to_live / save_live_to_preset / initialise_preset), then go through the ONE
  //     existing applyDeviceState candidate/commit path. No second owner, no parallel state bank,
  //     no rebuild from the audio callback.
  //   * LOAD writes the slot payload into the live keyboard state (the existing two-sided
  //     behaviour then consumes it). SAVE stores the LAST SUCCESSFULLY COMMITTED canonical live
  //     config into the target slot. INITIALISE resets ONLY the target slot and never implicitly
  //     loads it — a later LOAD is what makes it audible.
  //   * ALL THREE re-commit, so a success RESETS the performance/event timebase. This is the
  //     documented software behaviour: it is NOT a claim that a hardware SAVE re-triggers, and NOT
  //     a claim of seamless operation during a running stream.
  //   * SAVE / INITIALISE leave the live config CONTENT and the other three slots as they are.
  //   * Saving config is NOT capturing current DSP/keys transients: the caller must have committed
  //     its configuration through applyDeviceState first. A direct runtime setter or ControlEvent
  //     does not write back to the canonical state, so such a change is NOT saved.
  //   * A failure leaves the old owner, canonical state, format, plan and subsequent trace
  //     UNCHANGED (applyDeviceState's atomic rejection; reported as RejectedState).
  //   * There is NO APP/UI caller today (host/plugin.cpp builds MakeConfig(0,0); the menu is inert)
  //     — this is the engine seam, not a finished product entry.
  PresetActionStatus applyPresetAction(std::uint32_t slot, PresetAction action);

  // The production block delegate — the ONLY render entry the host's ProcessBlock calls. The
  // channel counts and block size are the ACTUAL device facts right now. `inputs` and `outputs`
  // are two planar arrays of channel pointers (inCh input channels / outCh output channels);
  // EACH channel buffer holds `frames` samples, and all channels share the same `frames` length
  // in a given call. Returns the Status; a dropped block has already had deterministic silence
  // written into the OUTPUT buffers, so the host's ProcessBlock is a pure delegate with no
  // further work. This path allocates nothing, locks nothing, and logs nothing.
  Status processBlock(const double* const* inputs, double* const* outputs, int inCh, int outCh,
                      int frames);

  // ---- live control (UI / MIDI) ------------------------------------------------------------
  // Non-audio thread (UI): change a knob, play a note, plug a cable, pick an effector program.
  // Each call updates the saved state immediately and hands the change to the audio thread
  // through a lock-free queue; it is applied at the start of the next audio block.
  bool postParameter(ParameterId id, double value);
  bool postEvent(const lunar24::core::ControlEvent& e);
  bool postConnect(lunar24::core::JackId source, lunar24::core::JackId sink);
  bool postDisconnect(lunar24::core::JackId sink);
  bool postEffectorProgram(int side, lunar24::core::ProgramId program);
  // DRONE VOICES key: open/close drone voice 0..5 (not saved; all open at power-on).
  bool postDroneKey(int voice, bool open);
  // The hand over a classic drone's photo sensor (group 0..3 = drone 1/2/4/5): 0 = away,
  // 1 = covering it. Live only, never saved.
  bool postPhotoShade(int group, double shade);
  // What that sensor sees, 0 = dark .. 1 = room light (written once per block, for the lamp).
  // OSC STATUS lamp of a classic drone's generator (group 0..3 = drone 1/2/4/5, gen 0..4),
  // 0..1: how much it sounds, pulsing with the beating. Written once per block.
  float oscLamp(int group, int gen) const {
    return group >= 0 && group < 4 && gen >= 0 && gen < 5
               ? oscLamps_[static_cast<std::size_t>(group * 5 + gen)].load(std::memory_order_relaxed)
               : 0.0f;
  }
  float photoLight(int group) const {
    return group >= 0 && group < 4 ? photoLight_[static_cast<std::size_t>(group)].load(std::memory_order_relaxed)
                                   : 1.0f;
  }
  // 16-step sequencer: set step `step` (0..15) of the left (side 0) or right bank.
  bool postSeqStep(int side, int step, int note, bool gate);
  // The RIGHT side's copy of a per-side keyboard menu setting (PLAY = SPLIT plays the right
  // half from it). False for a parameter that has no right-side copy.
  bool postKeyboardRightParameter(ParameterId id, double value);
  // Keyboard preset A-D (slot 0..3): load it, save the current keyboard settings into it, or
  // clear it (back to the factory keyboard settings). Live, no audio interruption.
  bool postKeyboardPreset(PresetAction action, std::uint32_t slot);
  // RHYTHM pattern of a side (0 left, 1 right): `seq` false = arpeggiator, true = sequencer.
  // `mutedMask` bit i mutes step i (0 = every step plays).
  bool postKeyboardRhythm(int side, bool seq, std::uint8_t mutedMask);
  std::uint8_t keyboardRhythm(int side, bool seq) const {
    const DeviceStateV1* st = canonicalState();
    if (st == nullptr) return 0;
    return (side == 0 ? st->keyboardClockSelectors : st->keyboardClockSelectorsR)[seq ? 3 : 1];
  }
  // Highest note of a sequencer step, in semitones above the held plate. // tuned by ear
  static constexpr int kSeqStepMaxNote = 24;
  bool droneKey(int voice) const { return voice >= 0 && voice < 6 && droneKeys_[voice]; }
  // Stopped-stream boundary (before prepare): close every DRONE VOICES key (RESET PANEL).
  void closeDroneKeys() {
    for (bool& k : droneKeys_) k = false;
  }
  // Panel indicator LEDs: brightness 0..1, written by the audio thread once per block and read
  // by the UI (relaxed atomics; a slightly stale value is fine for a light).
  enum PanelLed : int {
    kLedDrone1 = 0,           // drone 1..6 VCA envelope (amber, under HOLD): kLedDrone1 + voice
    kLedEnvA = 6, kLedEnvB,   // envelope A / B (amber)
    kLedLfoA, kLedLfoB,       // LFO A / B output (blue)
    kLedStep1,                // 5-step sequencer, the active step (red): kLedStep1 + step
    kLedPreampClip = kLedStep1 + 5,  // preamp near its rail (held ~0.15 s)
    kLedFollowerLevel,        // envelope follower level
    kLedFollowerGate,         // envelope follower gate detector
    kLedSh3, kLedSh6,         // drone 3 / 6 S&H: a short flash on each new sample
    kPanelLedCount
  };
  float panelLed(int led) const {
    return led >= 0 && led < kPanelLedCount ? leds_[static_cast<std::size_t>(led)].load(std::memory_order_relaxed)
                                            : 0.0f;
  }

  // MUTE button (UI thread): silence every output with a ~10 ms fade, without stopping the
  // machine, so unmuting returns to whatever is playing. Not saved: the app starts unmuted.
  void setMuted(bool on) { muted_.store(on, std::memory_order_relaxed); }
  bool muted() const { return muted_.load(std::memory_order_relaxed); }
  // UI and MIDI may toggle concurrently; neither toggle may overwrite the other.
  void toggleMuted() {
    bool current = muted_.load(std::memory_order_relaxed);
    while (!muted_.compare_exchange_weak(current, !current, std::memory_order_relaxed)) {}
  }
  // Bumped whenever a whole new machine state is committed (startup restore, preset load),
  // so the UI knows to redraw every control.
  std::uint64_t stateVersion() const { return stateVersion_; }
  // UI thread: bumped by every edit of the saved state (knob, cable, cartridge, MIDI CC), so
  // the host can autosave only when something changed.
  std::uint64_t editCount() const { return editCount_ + stateVersion_; }
  // Audio thread only (e.g. MIDI delivered inside the audio callback): schedule a note/clock
  // event `offset` samples into the coming block.
  bool enqueueEventFromAudioThread(const lunar24::core::ControlEvent& e, int offset = 0);
  // Diagnostics (any thread, one block old): the keyboard follows an external / MIDI clock,
  // and how many TEMPO edits the running engine has applied.
  bool keyboardFollowsExternalClock() const { return keyboardExternalClock_.load(std::memory_order_relaxed); }
  std::uint32_t keyboardTempoEdits() const { return keyboardTempoEdits_.load(std::memory_order_relaxed); }
  // Audio thread only: MIDI START, the arpeggiator / sequencer restart from their first step.
  void restartKeyboardPatternFromAudioThread() {
    if (definition_) definition_->runtime().restartKeyboardPattern();
  }
  // Audio thread only: MIDI pitch bend in semitones, applied to the keyboard V/OCT output.
  void pitchBendFromAudioThread(double semitones) {
    if (definition_) definition_->runtime().setKeyboardBendVolts(semitones / 12.0);
  }
  // Audio thread only: a knob changed by MIDI CC. The value is heard at once (as a live event)
  // and handed back to the UI thread, which records it in the saved state and redraws.
  bool parameterFromAudioThread(ParameterId id, double value);
  // UI thread: apply the MIDI-CC knob moves queued by the audio thread. Returns how many.
  int syncParametersFromAudioThread();
  // Event-scheduler pressure diagnostics, for tests and future UI/log
  // display. Safe from any thread: the underlying counters are relaxed atomics, so a
  // snapshot may be slightly stale under load but never races.
  struct EventDiagnostics {
    std::uint32_t parameterCoalesced = 0;
    std::uint32_t continuousOverflow = 0;
    std::uint32_t criticalOverflow = 0;
    std::uint32_t criticalFlushed = 0;
    std::uint32_t dispatchCapacity = 0;
    std::uint32_t lateCount = 0;
    std::uint32_t reconcileCount = 0;
    std::uint32_t batchAdmitted = 0;
    std::uint32_t batchRejected = 0;
    bool reconcilePending = false;
  };
  EventDiagnostics eventDiagnostics() const {
    EventDiagnostics d;
    if (!definition_) return d;
    const lunar24::core::EventTimebase& tb = definition_->runtime().eventTimebase();
    d.parameterCoalesced = tb.parameterCoalesced();
    d.continuousOverflow = tb.continuousOverflow();
    d.criticalOverflow = tb.criticalOverflow();
    d.criticalFlushed = tb.criticalFlushed();
    d.dispatchCapacity = tb.dispatchCapacity();
    d.lateCount = tb.lateCount();
    d.reconcileCount = tb.reconcileCount();
    d.batchAdmitted = tb.batchAdmitted();
    d.batchRejected = tb.batchRejected();
    d.reconcilePending = tb.reconcilePending();
    return d;
  }
  // UI note/gate events dropped by a full live queue (each one also requests the
  // all-gates-off reconcile). Cumulative across reopens.
  std::uint32_t liveEventDropCount() const { return liveEventDrops_.load(std::memory_order_relaxed); }

  // ---- MIDI bindings (the user's controller map) ---------------------------------
  // UI thread: publish a new binding snapshot — the map, the active input device's
  // name. The audio thread seeds bindings from its own current parameter values.
  void publishMidiMap(const lunar24::core::MidiMap& map, const char* inputDevice);
  // Audio thread: the published map's row for this message, or -1. Device-specific
  // bindings match only when their name equals the published input device. Apply
  // the returned row immediately, before another lookup or engine call.
  int midiBindingRow(std::uint8_t channel, lunar24::core::MidiBindingKind kind,
                     std::uint8_t number);
  // Audio thread: apply a matched row's raw data byte (0..127). Parameters honor the
  // binding's input mode (pickup for absolute, deltas for relative); drone-key and
  // mute actions take effect immediately and are recorded for the UI; cartridge /
  // preset actions hand over to the UI thread via fromAudioQueue_. A photo-sensor
  // binding moves the hand at once; for a pad, `channel` (1..16) is the pad's channel.
  void applyMidiBindingFromAudioThread(std::uint32_t row, int rawValue, std::uint8_t channel = 0);
  // Audio thread: a pad bound to a photo sensor. Release lifts the hand; pressure (poly
  // aftertouch on that note, or channel aftertouch with note -1) sets how close it is.
  // Both return true when a held photo pad took the message.
  bool photoPadRelease(std::uint8_t channel, std::uint8_t note);
  bool photoPadPressure(std::uint8_t channel, int note, double pressure01);
  // The published snapshot's binding at `row` (for the UI to label it). UI thread.
  const lunar24::core::MidiBinding* midiBindingAt(std::uint32_t row) const {
    const auto& s = midiUiMap_;
    return row < s.map.count() ? &s.map.at(row) : nullptr;
  }
  // Current value of a parameter as the user last set it (UI readback).
  double parameterValue(ParameterId id) const {
    const DeviceStateV1* st = canonicalState();
    return st ? st->parameters[static_cast<std::uint32_t>(id)] : 0.0;
  }

  // ---- inspectable (never mutated on the audio path beyond the monotonic counters) ----
  bool isReady() const { return ready_; }
  double sampleRate() const { return sampleRate_; }
  // UI thread: where REC taps the outputs (null = none). The tap must outlive the engine's use.
  void setAudioTap(AudioTap* tap) { audioTap_.store(tap, std::memory_order_release); }
  int blockSize() const { return blockSize_; }
  int inputCapability() const { return inputCapability_; }
  int outputCapability() const { return outputCapability_; }
  const DevicePlan& plan() const { return adapter_.plan(); }
  std::uint64_t nonFiniteSamples() const { return adapter_.nonFiniteSamples(); }
  std::uint64_t renderedBlocks() const { return renderedBlocks_; }
  std::uint64_t droppedBlocks() const { return droppedBlocks_; }

  // The canonical DeviceStateV1 of the active definition, read straight from the owned definition
  // (never a second snapshot living elsewhere). nullptr iff not ready.
  const DeviceStateV1* canonicalState() const {
    return definition_ ? &definition_->deviceState() : nullptr;
  }
  // Whether the identity/calibration profile was applied to the active definition. Named
  // precisely: this is the identity/calibration apply, NOT a whole-DeviceState "applied" claim.
  bool identityApplied() const { return definition_ && definition_->identityApplied(); }
  // The last applyDeviceState outcome (or NotAttempted). A RejectedInvalidState's family+field
  // detail is exposed by lastStateValidation.
  StateApplyStatus stateApplyStatus() const { return stateApplyStatus_; }
  // The active machine's SynthRuntime, read straight from the owned definition (non-shadow truth,
  // self-consistent with canonicalState). nullptr iff not ready. For the wired-path check:
  // the caller reads the actual derived profile consumers via the SynthRuntime getters.
  //
  // ⚠️ LIFETIME: the returned pointer is valid only until the NEXT commit_ — i.e. until
  // the next successful prepare/applyDeviceState — because each apply swaps `definition_` for a
  // freshly-built candidate and RELEASES the prior definition. A pointer held across a churn apply
  // would then dangle. For a test that must read getters across multiple applies, use the by-value
  // observeRuntime snapshot instead (copying scalars out at the moment of the call); this accessor
  // is for read-at-this-instant use with NO intervening apply.
  const SynthRuntime* runtime() const { return definition_ ? &definition_->runtime() : nullptr; }

  // A by-value snapshot of the LIVE identity/calibration consumers plus the seeded-voice
  // outputs of the active definition, read via the SynthRuntime getters ONCE at call time. This is
  // the safe way for a caller (the oracle) to inspect runtime getters across multiple applies: it
  // never holds a pointer across an apply, so a long-lived `runtime` pointer that dangles after
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
  // the first applied_to_DSP parameter whose write was rejected, and the reason, on
  // a RejectedDspApply outcome (sentinel kParameterCount / applied otherwise). Lets a caller
  // surface the exact failing id/status instead of only a coarse "rejected" bit.
  ParameterId dspApplyFirstFailParamId() const { return dspApplyFirstFailParamId_; }
  ParameterApplyStatus dspApplyFirstFailStatus() const { return dspApplyFirstFailStatus_; }

 private:
  // Write deterministic silence into the caller's ACTUAL output channels (bounded to what the
  // caller owns). Used on every dropped path so a dropped block is never a stale / partial /
  // repeated buffer. The audio path may touch no heap — this is a plain memset-like loop.
  void writeSilence_(double* const* outputs, int outCh, int frames) const;
  void applyMute_(double* const* outputs, int outCh, int frames, float* tap);
  // Cable edits: the UI thread compiles the new patch plan (it allocates); the audio thread
  // only swaps it in and hands the used plan back to be freed here.
  using GraphPlan = lunar24::core::SynthRuntime::GraphPlan;
  GraphPlan* planCableEdit_();                       // UI thread, after editing uiPatch_
  bool installGraphPlan_(SynthRuntime& rt, void* plan);  // audio thread
  void returnGraphPlan_(GraphPlan* plan);            // audio thread
  void releaseGraphPlans_();                         // UI thread, audio stopped: free every plan
  void updateLeds_(int frames);

  // Clear the ENTIRE committed state back to the "no prepare done" sentinel. Called on every
  // prepare failure so no half-written plan / stale format / old definition is observable:
  // the engine returns to NOT-READY with every inspectable field at its empty value, and the
  // old definition (if any) is released here — the ONLY place its destructor runs (the
  // stopped-stream boundary).
  void clearState_();

  // Complete the frozen default plan (the owner's one honest default-plan helper) on a LOCAL
  // adapter. Shared by prepare and applyDeviceState; never mutates committed state itself.
  // Returns false if the channel configuration cannot be planned (never invents a route).
  static bool completeDefaultPlan_(DeviceAdapter& adapter, int inputCapability, int outputCapability);

  // Commit a built candidate + adapter + format in ONE shot (releases the old definition at this
  // stopped-stream boundary). The caller has already decided success; this only installs.
  void commit_(std::unique_ptr<MachineRuntimeDefinition> cand, const DeviceAdapter& candAdapter,
               double sampleRate, int maxBlockSize, int inputCapability, int outputCapability);

  // The address-stable runtime owner. Lives on the heap (never the callback stack), and is
  // released ONLY at the prepare/OnReset stopped-stream boundary, never inside processBlock.
  std::unique_ptr<MachineRuntimeDefinition> definition_;
  // The single production DeviceAdapter. The preferred default plan is installed by
  // prepare; processBlock only ever forwards to it.
  DeviceAdapter adapter_;

  // The committed format (set only by a successful prepare; the processBlock format gate reads
  // these). Zero/empty values mean "no committed format" -> not-ready.
  double sampleRate_ = 0.0;
  int blockSize_ = 0;
  int inputCapability_ = 0;
  int outputCapability_ = 0;
  bool ready_ = false;

  // UI -> audio live command queue (single producer: the UI thread).
  lunar24::core::SpscQueue<1024> liveQueue_;
  lunar24::core::SpscQueue<256> fromAudioQueue_;  // audio -> UI (MIDI CC knob moves)
  // DRONE VOICES keys start closed: opening the app (or RESET PANEL) is silent until the player
  // opens a voice. The keys are not part of the saved state.
  bool droneKeys_[6] = {false, false, false, false, false, false};
  std::atomic<bool> muted_{false};
  std::atomic<AudioTap*> audioTap_{nullptr};  // REC; set by the UI thread
  std::optional<lunar24::core::PatchGraph> uiPatch_;  // UI thread: copy of the live patch, to plan edits on
  GraphPlan* planReturns_[16] = {};          // audio thread: used plans waiting for queue room
  std::uint32_t planReturnCount_ = 0;
  std::vector<float> tap_;                    // one block of tapped frames (sized by prepare)
  // A UI note/gate event dropped by a full liveQueue_ makes the queued note stream
  // untrustworthy (some note-off is gone for good, and the UI keeps no held-note
  // ledger to resend from): the audio thread drops the backlog's note events and
  // reconciles to all-gates-off at the next drain — the EventTimebase
  // critical-overflow failsafe, one level up.
  std::atomic<bool> eventReconcileRequested_{false};
  std::atomic<std::uint32_t> liveEventDrops_{0};

  // SPSC triple buffer: UI and audio each own a slot; exchange transfers the
  // third slot. The dirty bit means the middle slot contains a newer publication.
  struct MidiMapSlot {
    lunar24::core::MidiMap map;
    char device[lunar24::core::kMidiBindingDeviceCapacity] = {};
  };
  MidiMapSlot midiMapSlots_[3];
  MidiMapSlot midiUiMap_;  // UI-only readback
  std::atomic<std::uint32_t> midiMapMiddle_{1};
  std::uint32_t midiMapWriteIdx_ = 2;  // UI only
  std::uint32_t midiMapReadIdx_ = 0;   // audio only
  bool midiMatchPending_ = false;
  // Audio-owned current control values, initialized at the stopped-stream boundary.
  double midiParameterValue_[lunar24::core::kParameterIdSpace] = {};
  void refreshMidiMap_();
  double midiValue_[lunar24::core::kMidiMapCapacity] = {};
  int midiLastRaw_[lunar24::core::kMidiMapCapacity] = {};
  // When each binding's relative encoder last ticked (runtime sample), for its speed.
  static constexpr std::uint64_t kNoTick = ~std::uint64_t{0};
  std::uint64_t midiLastTickAt_[lunar24::core::kMidiMapCapacity] = {};  // kNoTick from prepare()
  // Photo-sensor hands driven by MIDI (audio thread): where each hand is, and the pad
  // holding it down (channel 1..16, note; channel 0 = no pad held).
  double photoShade_[4] = {0.0, 0.0, 0.0, 0.0};
  std::uint8_t photoPadChannel_[4] = {0, 0, 0, 0};
  std::uint8_t photoPadNote_[4] = {0, 0, 0, 0};
  void setPhotoShade_(int group, double shade);
  bool midiPickedUp_[lunar24::core::kMidiMapCapacity] = {};
  std::array<std::atomic<float>, kPanelLedCount> leds_{};
  std::array<std::atomic<float>, 4> photoLight_{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<std::atomic<float>, 20> oscLamps_{};
  double ledShLast_[2] = {0.0, 0.0};   // audio thread: last S&H value seen, per drone 3 / 6
  double ledShHold_[2] = {0.0, 0.0};   // audio thread: seconds left on each S&H flash
  std::atomic<bool> keyboardExternalClock_{false};      // diagnostics snapshot, per block
  std::atomic<std::uint32_t> keyboardTempoEdits_{0};
  double ledClipHold_ = 0.0;           // audio thread: seconds left on the clip LED
  double muteGain_ = 1.0;  // audio thread: the faded output gain the MUTE button drives
  std::uint64_t stateVersion_ = 0;
  std::uint64_t editCount_ = 0;
  void drainLive_(SynthRuntime& rt);
  // Audio thread: a parameter moved (any source) — rebase the MIDI pickup/relative state.
  void noteParameterSeen_(ParameterId id, double value);
  // UI thread: step the effector cartridge on both sides (a MIDI action handler).
  void stepCartridge(int delta);
  bool sendParameterFromAudioThread_(ParameterId id, double value, bool rebaseBindings, int originRow = -1);
  // Audio thread: rebase keyboard parameters from the runtime's own preset state.
  void reseedMidiBindingsFromState_();

  // Monotonic RT counters (plain, no lock).
  std::uint64_t renderedBlocks_ = 0;
  std::uint64_t droppedBlocks_ = 0;

  // The inspectable state-apply outcome and the last validation detail. Only set by
  // applyDeviceState (and Accepted by a successful prepare, which publishes the default state).
  StateApplyStatus stateApplyStatus_ = StateApplyStatus::NotAttempted;
  StateValidationResult lastStateValidation_;
  //  first applied_to_DSP failure detail (sentinel kParameterCount / applied iff ok).
  ParameterId dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  ParameterApplyStatus dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;
};

// ---- prepare --------------------------------------------------------------
inline bool StandaloneAudioEngine::prepare(std::uint64_t seed, double sampleRate,
                                           int maxBlockSize, int inputCapability,
                                           int outputCapability) {
  // Reset the host DSP first-fail diagnostic at the START of every prepare-only, exactly as
  // applyDeviceState does below (337-338), so a prior RejectedDspApply never leaks a stale
  // failure into a later successful/other prepare on the same owner.
  dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;
  // A new runtime starts with every photo-sensor hand away (the stream is stopped here).
  for (int g = 0; g < 4; ++g) {
    photoShade_[g] = 0.0;
    photoPadChannel_[g] = 0;
  }
  for (std::uint64_t& t : midiLastTickAt_) t = kNoTick;  // the new runtime's clock starts at 0
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
  // builder that applyDeviceState uses ("successful safe boot must delegate
  // to the same builder"). This is what makes a seed denote the exact make_default_device_state(seed)
  // canonical state, and what applies the identity/calibration profile to the default too.
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
  // INVERSE failure contract of prepare: a rejection here is ATOMIC — the
  // prior complete active definition / plan / format / canonical state is left unchanged and the
  // engine STAYS ready. It never goes NOT-READY on a bad state: this is a "state" problem, not a
  // stream re-configuration problem, so the current stream keeps running with the prior canonical
  // state. A rejected candidate is never observable as an intermediate or partial install.

  //  BLOCK #5: reset the DSP first-fail diagnostics to the "no failure" sentinel at the entry
  // of every apply. Only a RejectedDspApply outcome overwrites them below; every other terminal
  // outcome (Accepted or any other Rejected*) leaves them at the sentinel, so a prior DSP failure
  // never leaks into a later apply's diagnostics. Without this, the stale failure survived into a
  // subsequent Accepted apply (the "otherwise sentinel" contract was never honoured).
  dspApplyFirstFailParamId_ = static_cast<ParameterId>(kParameterCount);
  dspApplyFirstFailStatus_ = ParameterApplyStatus::applied;

  // Strict-format gate mirrors prepare's first two checks — an illegal rate / block / channel set
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

// ---- applyPresetAction ----------------------------------------------------
inline StandaloneAudioEngine::PresetActionStatus StandaloneAudioEngine::applyPresetAction(
    std::uint32_t slot, PresetAction action) {
  // Gate 1: an action reads the committed canonical state and re-publishes at the committed
  // format, so a not-ready engine can NEVER report success (there is nothing to read or apply).
  if (!ready_ || definition_ == nullptr) return PresetActionStatus::RejectedNotReady;
  // Gate 2: the slot bank is fixed at four native presets; a 5th slot is structurally impossible
  // and is rejected explicitly rather than silently indexing out of range.
  if (!lunar24::core::preset_slot_is_valid(slot)) return PresetActionStatus::RejectedInvalidSlot;
  // Gate 3: the action must be one of the three defined operations.
  switch (action) {
    case PresetAction::Load:
    case PresetAction::Save:
    case PresetAction::Initialise:
      break;
    default:
      return PresetActionStatus::RejectedInvalidAction;
  }

  // Copy the CANONICAL state and apply the ONE state-layer helper. The copy is deliberate: the
  // helper mutates in place, and a rejection below must leave the committed state untouched.
  DeviceStateV1 candidate = definition_->deviceState();
  bool ok = false;
  switch (action) {
    case PresetAction::Load:
      ok = lunar24::core::load_preset_to_live(candidate, slot);
      break;
    case PresetAction::Save:
      ok = lunar24::core::save_live_to_preset(candidate, slot);
      break;
    case PresetAction::Initialise:
      ok = lunar24::core::initialise_preset(candidate, slot);
      break;
    default:
      break;  // unreachable: gated above
  }
  // The helpers carry the same slot guard; a false here would mean the two guards disagree.
  if (!ok) return PresetActionStatus::RejectedInvalidSlot;

  // The ONE existing candidate/commit path, at the CURRENT committed format. A rejection is atomic
  // (old owner / canonical state / format / plan kept) and is reported, never swallowed.
  const StateApplyStatus st =
      applyDeviceState(candidate, sampleRate_, blockSize_, inputCapability_, outputCapability_);
  return (st == StateApplyStatus::Accepted) ? PresetActionStatus::Accepted
                                            : PresetActionStatus::RejectedState;
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
  // OnReset configuration (GetBlockSize), so frames beyond blockSize_ is a kernel/wiring defect
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

  // Defense-in-depth: a definition whose graph did NOT compile must never be
  // rendered. buildMachineRuntimeCandidate already rejects such a candidate before commit, so in
  // practice definition_ is always valid here; this guard forbids an invalid definition reaching
  // processBlock through any other construction, closing the "mint a valid-looking machine from an
  // invalid state" counter-example. Drop to deterministic silence + counter; never a partial render.
  if (!definition_->valid()) {
    ++droppedBlocks_;
    writeSilence_(outputs, outCh, frames);
    return Status::DroppedInvalidDefinition;
  }

  // THE single production delegate. The owner forwards the whole block to the
  // frozen adapter and does nothing else — no frame loop, no scaling, no mapping, no
  // pass-through. A second output bank / a host-side scale would be a wiring defect.
  const ScopedFlushDenormals noDenormals;  // decaying tails never hit slow denormal math
  drainLive_(definition_->runtime());
  AudioTap* const audioTap = audioTap_.load(std::memory_order_acquire);
  float* const tap = audioTap != nullptr && audioTap->armed() &&
                             tap_.size() >= static_cast<std::size_t>(frames) * 4
                         ? tap_.data()
                         : nullptr;
  adapter_.renderBlock(definition_->runtime(), inputs, outputs, frames, tap);
  applyMute_(outputs, outCh, frames, tap);
  if (tap != nullptr) audioTap->push(tap, frames);
  updateLeds_(frames);
  ++renderedBlocks_;
  return Status::Rendered;
}

// ---- applyMute_ ------------------------------------------------------------
// The MUTE button: a linear ~10 ms fade of every output toward 0 (muted) or 1. No work at all
// while unmuted and settled, so the normal render path is untouched.
inline void StandaloneAudioEngine::applyMute_(double* const* outputs, int outCh, int frames, float* tap) {
  const double target = muted() ? 0.0 : 1.0;
  if (muteGain_ == 1.0 && target == 1.0) return;
  const double step = 1.0 / (0.010 * (sampleRate_ > 0.0 ? sampleRate_ : 48000.0));
  double g = muteGain_;
  for (int f = 0; f < frames; ++f) {
    g = target > g ? std::min(target, g + step) : std::max(target, g - step);
    for (int c = 0; c < outCh; ++c)
      if (outputs != nullptr && outputs[c] != nullptr) outputs[c][f] *= g;
    if (tap != nullptr)
      for (int c = 0; c < 4; ++c) tap[4 * f + c] *= static_cast<float>(g);
  }
  muteGain_ = g;
}

// ---- updateLeds_ ----------------------------------------------------------
// One snapshot per block of what the panel LEDs show. Flash/hold times are tuned by eye.
inline void StandaloneAudioEngine::updateLeds_(int frames) {
  core::SynthRuntime& rt = definition_->runtime();
  const double blockSec = frames / (sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
  auto put = [this](int led, double v) {
    leds_[static_cast<std::size_t>(led)].store(static_cast<float>(std::clamp(v, 0.0, 1.0)),
                                               std::memory_order_relaxed);
  };
  for (int v = 0; v < 6; ++v) put(kLedDrone1 + v, rt.droneVoiceEnvLevel(v));
  for (int g = 0; g < 4; ++g) {
    photoLight_[static_cast<std::size_t>(g)].store(static_cast<float>(rt.dronePhotoLight01(g)),
                                                   std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i)
      oscLamps_[static_cast<std::size_t>(g * 5 + i)].store(static_cast<float>(rt.droneOscLamp(g, i)),
                                                           std::memory_order_relaxed);
  }
  put(kLedEnvA, rt.envelopeA().level01());
  put(kLedEnvB, rt.envelopeB().level01());
  put(kLedLfoA, 0.5 * (rt.lfoA().fundamental() + 1.0));  // the LFOs swing 0..+10 V
  put(kLedLfoB, 0.5 * (rt.lfoB().fundamental() + 1.0));
  const auto& seq = rt.sequencer();
  for (int i = 0; i < 5; ++i) put(kLedStep1 + i, seq.started() && seq.currentStep() == i ? 1.0 : 0.0);
  if (rt.takePreampPeak() > 0.9 * core::Preamp::kSaturationVoltage) ledClipHold_ = 0.15;
  put(kLedPreampClip, ledClipHold_ > 0.0 ? 1.0 : 0.0);
  ledClipHold_ = std::max(0.0, ledClipHold_ - blockSec);
  put(kLedFollowerLevel, rt.envFollowerLevel01());
  put(kLedFollowerGate, rt.envFollowerGateOn() ? 1.0 : 0.0);
  keyboardExternalClock_.store(rt.keyboardFollowsExternalClock(), std::memory_order_relaxed);
  keyboardTempoEdits_.store(rt.keyboardTempoEdits(), std::memory_order_relaxed);
  const int shVoice[2] = {2, 5};
  for (int k = 0; k < 2; ++k) {
    const double sh = rt.droneShOutVolts(shVoice[k]);
    if (sh != ledShLast_[k]) ledShHold_[k] = 0.06;
    ledShLast_[k] = sh;
    put(kLedSh3 + k, ledShHold_[k] > 0.0 ? 1.0 : 0.0);
    ledShHold_[k] = std::max(0.0, ledShHold_[k] - blockSec);
  }
}

// ---- clearState_ ----------------------------------------------------------
inline void StandaloneAudioEngine::clearState_() {
  // Release the old definition (its destructor runs ONCE here — the stopped-stream boundary)
  // and reset every committed-format field to the empty "no prepare done" sentinel. After
  // this the engine is fully NOT-READY with nothing inspectable left half-written.
  releaseGraphPlans_();
  liveQueue_.clear();
  fromAudioQueue_.clear();
  eventReconcileRequested_.store(false, std::memory_order_relaxed);
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
  // The owner's ONE honest default-plan helper (the frozen policy, consumed not re-derived).
  // Maps the REAL channel counts to a frozen route (never a silent copy):
  //
  //   inputCapability 0 -> Zero (both terminals read 0; no channel consumed)
  //   inputCapability 1 -> DuplicateOne (EXT = PREAMP = ch0: a mono mic, e.g. a laptop's
  //                                        built-in one, must reach the PREAMP like the
  //                                        hardware's contact mic, and EXT.AUDIO too)
  //   inputCapability >=2-> Distinct (EXT = ch0, PREAMP = ch1)
  //
  //   outputCapability 2-3 -> outputCount 2 (WET L/R only)
  //   outputCapability >=4-> outputCount 4 (WET L/R + DRY A/B; extra physical untouched)
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
    route = InputRoute::DuplicateOne;
    extCh = 0;
    preampCh = 0;
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
  // Both queue endpoints are idle at this stopped-stream/UI-thread boundary.
  // The candidate already contains the saved edits; old commands and note-ons
  // must not be replayed against it. Rejected candidates never reach this point.
  releaseGraphPlans_();
  liveQueue_.clear();
  fromAudioQueue_.clear();
  eventReconcileRequested_.store(false, std::memory_order_relaxed);
  definition_ = std::move(cand);
  uiPatch_.emplace(definition_->runtime().patchGraph());  // the audio thread is stopped here
  adapter_ = candAdapter;
  sampleRate_ = sampleRate;
  blockSize_ = maxBlockSize;
  tap_.assign(static_cast<std::size_t>(maxBlockSize) * 4, 0.0f);  // the REC tap's block buffer
  inputCapability_ = inputCapability;
  outputCapability_ = outputCapability;
  ready_ = true;
  for (int v = 0; v < 6; ++v) definition_->runtime().setDroneVoiceKey(v, droneKeys_[v]);
  ++stateVersion_;
  const auto& parameters = definition_->deviceState().parameters;
  for (std::size_t i = 0; i < lunar24::core::kParameterIdSpace; ++i)
    midiParameterValue_[i] = parameters[i];
  refreshMidiMap_();
  for (const auto& d : lunar24::registry::kParameters)
    noteParameterSeen_(d.id, midiParameterValue_[static_cast<std::uint32_t>(d.id)]);
}


// ---- live control ---------------------------------------------------------
// UI edits check queue capacity BEFORE mutating saved state. The UI is the sole
// producer, so the consumer cannot invalidate a successful capacity check.
inline bool StandaloneAudioEngine::postParameter(ParameterId id, double value) {
  if (!definition_ || !liveQueue_.canPush()) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  if (!lunar24::core::state_set_param(st, id, value)) return false;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::Parameter;
  c.parameter = id;
  c.value = st.parameters[static_cast<std::uint32_t>(id)];
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postEvent(const lunar24::core::ControlEvent& e) {
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::Event;
  c.event = e;
  if (liveQueue_.push(c)) return true;
  // The queue is full: if this was a note-off whose note-on is queued or already
  // playing, that gate would hang open forever. Count the drop and reconcile to
  // all-gates-off at the next drain rather than risking a stuck note.
  liveEventDrops_.fetch_add(1, std::memory_order_relaxed);
  eventReconcileRequested_.store(true, std::memory_order_release);
  return false;
}

inline bool StandaloneAudioEngine::postConnect(lunar24::core::JackId source,
                                               lunar24::core::JackId sink) {
  if (!definition_ || !liveQueue_.canPush()) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  const auto i = static_cast<std::uint32_t>(sink);
  const bool hadOld = i < lunar24::core::kDevicePatchCapacity && st.inputCable[i] != 0u;
  const lunar24::core::JackId oldSource = hadOld ? st.cableSource[i] : lunar24::core::JackId{0};
  if (!lunar24::core::state_connect(st, source, sink)) return false;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::Connect;
  c.source = source;
  c.sink = sink;
  c.oldSource = oldSource;
  c.hadOld = hadOld;
  if (hadOld) (void)uiPatch_->disconnect(oldSource, sink);  // the same edit the audio thread makes
  (void)uiPatch_->connect(source, sink);
  c.graphPlan = planCableEdit_();
  if (liveQueue_.push(c)) return true;
  delete static_cast<GraphPlan*>(c.graphPlan);
  return false;
}

inline bool StandaloneAudioEngine::postDisconnect(lunar24::core::JackId sink) {
  if (!definition_ || !liveQueue_.canPush()) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  const auto i = static_cast<std::uint32_t>(sink);
  if (i >= lunar24::core::kDevicePatchCapacity || st.inputCable[i] == 0u) return false;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::Disconnect;
  c.source = st.cableSource[i];
  c.sink = sink;
  lunar24::core::state_disconnect(st, sink);
  ++editCount_;
  (void)uiPatch_->disconnect(c.source, sink);
  c.graphPlan = planCableEdit_();
  if (liveQueue_.push(c)) return true;
  delete static_cast<GraphPlan*>(c.graphPlan);
  return false;
}

inline bool StandaloneAudioEngine::postEffectorProgram(int side, lunar24::core::ProgramId program) {
  if (!definition_ || !liveQueue_.canPush() || lunar24::core::find_program(program) == nullptr) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  (side == 0 ? st.leftEffector : st.rightEffector).program = program;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::EffectorProgram;
  c.side = side == 0 ? 0u : 1u;
  c.program = program;
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postDroneKey(int voice, bool open) {
  if (voice < 0 || voice > 5 || !liveQueue_.canPush()) return false;
  droneKeys_[voice] = open;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::DroneKey;
  c.side = static_cast<std::uint32_t>(voice);
  c.value = open ? 1.0 : 0.0;
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postPhotoShade(int group, double shade) {
  if (group < 0 || group > 3) return false;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::PhotoShade;
  c.side = static_cast<std::uint32_t>(group);
  c.value = std::clamp(shade, 0.0, 1.0);
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postSeqStep(int side, int step, int note, bool gate) {
  if (!definition_ || !liveQueue_.canPush() || step < 0 || step >= static_cast<int>(lunar24::core::kKeyboardSeqStepCount)) return false;
  const std::uint8_t n = static_cast<std::uint8_t>(std::clamp(note, 0, kSeqStepMaxNote));
  DeviceStateV1& st = definition_->mutableDeviceState();
  auto& q = side == 0 ? st.keyboardSeqCurrent : st.keyboardSeqCurrentR;
  q.steps[static_cast<std::size_t>(step)].note = n;
  q.steps[static_cast<std::size_t>(step)].gate = gate ? 1 : 0;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::SeqStep;
  c.side = side == 0 ? 0u : 1u;
  c.index = static_cast<std::uint32_t>(step);
  c.value = n;
  c.hadOld = gate;
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postKeyboardRightParameter(ParameterId id, double value) {
  if (!definition_ || !liveQueue_.canPush()) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  if (!lunar24::core::state_set_keyboard_right(st, id, value)) return false;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::KeyboardRight;
  c.parameter = id;
  c.value = st.keyboardScalarRight[static_cast<std::size_t>(lunar24::core::keyboard_scalar_index(id))];
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postKeyboardRhythm(int side, bool seq, std::uint8_t mutedMask) {
  if (!definition_ || !liveQueue_.canPush()) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  const std::uint32_t index = seq ? 3u : 1u;
  (side == 0 ? st.keyboardClockSelectors : st.keyboardClockSelectorsR)[index] = mutedMask;
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::KeyboardSelector;
  c.side = side == 0 ? 0u : 1u;
  c.index = index;
  c.value = mutedMask;
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::postKeyboardPreset(PresetAction action, std::uint32_t slot) {
  if (!definition_ || !liveQueue_.canPush() || !lunar24::core::preset_slot_is_valid(slot)) return false;
  DeviceStateV1& st = definition_->mutableDeviceState();
  int code = 0;
  switch (action) {
    case PresetAction::Load: code = 0; (void)lunar24::core::load_preset_to_live(st, slot); break;
    case PresetAction::Save: code = 1; (void)lunar24::core::save_live_to_preset(st, slot); break;
    case PresetAction::Initialise: code = 2; (void)lunar24::core::initialise_preset(st, slot); break;
    default: return false;
  }
  ++editCount_;
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::KeyboardPreset;
  c.side = static_cast<std::uint32_t>(code);
  c.index = slot;
  return liveQueue_.push(c);
}

inline bool StandaloneAudioEngine::parameterFromAudioThread(ParameterId id, double value) {
  return sendParameterFromAudioThread_(id, value, true);
}

// The shared tail of the MIDI-CC entry. `rebaseBindings` is false when the value came
// FROM a binding itself (its own output must not re-arm its own pickup).
inline bool StandaloneAudioEngine::sendParameterFromAudioThread_(ParameterId id, double value,
                                                                 bool rebaseBindings, int originRow) {
  if (!definition_) return false;
  const auto* descriptor = lunar24::core::find_parameter(id);
  if (descriptor == nullptr || !std::isfinite(value)) return false;
  value = std::clamp(value, descriptor->min, descriptor->max);
  if (descriptor->step > 0.0)
    value = descriptor->min + std::round((value - descriptor->min) / descriptor->step) * descriptor->step;
  midiParameterValue_[static_cast<std::uint32_t>(id)] = value;
  SynthRuntime& rt = definition_->runtime();
  if (rebaseBindings) noteParameterSeen_(id, value);
  else {
    const auto& map = midiMapSlots_[midiMapReadIdx_].map;
    for (std::uint32_t i = 0; i < map.count(); ++i) {
      const auto& binding = map.at(i);
      if (binding.targetKind != lunar24::core::MidiTargetKind::parameter || binding.parameter != id) continue;
      midiValue_[i] = value;
      if (static_cast<int>(i) != originRow) {
        midiPickedUp_[i] = false;
        midiLastRaw_[i] = -1;
      }
    }
  }
  // Keyboard menu params apply in place (the drainLive_ rule), never as queued events.
  if (!rt.setKeyboardParameter(id, value)) {
    lunar24::core::ControlEvent e{};
    e.kind = lunar24::core::ControlEventKind::parameter;
    e.parameter = id;
    e.value = static_cast<lunar24::core::SignalSample>(value);
    e.source = 101;  // MIDI CC producer
    (void)rt.enqueueControlEvent(lunar24::core::TimedControlEvent{e, rt.currentSample()});
  }
  lunar24::core::LiveCommand c;
  c.kind = lunar24::core::LiveCommand::Kind::Parameter;
  c.parameter = id;
  c.value = value;
  return fromAudioQueue_.push(c);
}

// UI-only producer; audio owns its reader slot until it explicitly exchanges it.
inline void StandaloneAudioEngine::publishMidiMap(const lunar24::core::MidiMap& map,
                                                  const char* inputDevice) {
  MidiMapSlot& s = midiMapSlots_[midiMapWriteIdx_];
  s.map = map;
  std::memset(s.device, 0, sizeof(s.device));
  if (inputDevice != nullptr) std::snprintf(s.device, sizeof(s.device), "%s", inputDevice);
  midiUiMap_ = s;
  midiMapWriteIdx_ = midiMapMiddle_.exchange(midiMapWriteIdx_ | 4u, std::memory_order_acq_rel) & 3u;
}

inline void StandaloneAudioEngine::refreshMidiMap_() {
  if ((midiMapMiddle_.load(std::memory_order_acquire) & 4u) == 0) return;
  midiMapReadIdx_ = midiMapMiddle_.exchange(midiMapReadIdx_, std::memory_order_acq_rel) & 3u;
  const auto& s = midiMapSlots_[midiMapReadIdx_];
  for (std::uint32_t i = 0; i < s.map.count(); ++i) {
    const auto& b = s.map.at(i);
    midiValue_[i] = b.targetKind == lunar24::core::MidiTargetKind::parameter
        ? midiParameterValue_[static_cast<std::uint32_t>(b.parameter)] : 0.0;
    midiLastRaw_[i] = -1;
    midiPickedUp_[i] = false;
    midiLastTickAt_[i] = kNoTick;
  }
}

inline int StandaloneAudioEngine::midiBindingRow(std::uint8_t channel,
                                                 lunar24::core::MidiBindingKind kind,
                                                 std::uint8_t number) {
  refreshMidiMap_();
  midiMatchPending_ = true;
  const auto& s = midiMapSlots_[midiMapReadIdx_];
  const auto* b = lunar24::core::midi_map_find(s.map, s.device, channel, kind, number);
  return b == nullptr ? -1 : static_cast<int>(b - &s.map.at(0));
}

inline void StandaloneAudioEngine::setPhotoShade_(int group, double shade) {
  photoShade_[group] = std::clamp(shade, 0.0, 1.0);
  definition_->runtime().setDronePhotoShade(group, photoShade_[group]);
}

// A pad's hit puts the hand on the eye, its pressure presses closer. Tuned by ear.
inline constexpr double kPhotoPadHitBase = 0.5, kPhotoPadPressureBase = 0.4;

inline bool StandaloneAudioEngine::photoPadRelease(std::uint8_t channel, std::uint8_t note) {
  if (!definition_) return false;
  bool took = false;
  for (int g = 0; g < 4; ++g)
    if (photoPadChannel_[g] == channel && photoPadNote_[g] == note) {
      photoPadChannel_[g] = 0;
      setPhotoShade_(g, 0.0);
      took = true;
    }
  return took;
}

inline bool StandaloneAudioEngine::photoPadPressure(std::uint8_t channel, int note, double pressure01) {
  if (!definition_ || channel == 0) return false;
  bool took = false;
  for (int g = 0; g < 4; ++g)
    if (photoPadChannel_[g] == channel && (note < 0 || photoPadNote_[g] == note)) {
      setPhotoShade_(g, kPhotoPadPressureBase + (1.0 - kPhotoPadPressureBase) * std::clamp(pressure01, 0.0, 1.0));
      took = true;
    }
  return took;
}

inline void StandaloneAudioEngine::applyMidiBindingFromAudioThread(std::uint32_t row,
                                                                   int rawValue, std::uint8_t channel) {
  if (!definition_) return;
  // A lookup and its application always use the same audio-owned snapshot.
  if (!midiMatchPending_) refreshMidiMap_();
  midiMatchPending_ = false;
  const auto& s = midiMapSlots_[midiMapReadIdx_];
  if (row >= s.map.count() || rawValue < 0 || rawValue > 127) return;
  const lunar24::core::MidiBinding& b = s.map.at(row);
  SynthRuntime& rt = definition_->runtime();

  if (b.targetKind == lunar24::core::MidiTargetKind::action &&
      lunar24::core::midi_action_is_continuous(b.action)) {
    const int g = static_cast<int>(b.action) - static_cast<int>(lunar24::core::MidiAction::photo_drone_1);
    if (b.key.kind == lunar24::core::MidiBindingKind::note) {  // a pad hit: the hand comes down
      photoPadChannel_[g] = channel;
      photoPadNote_[g] = b.key.number;
      setPhotoShade_(g, kPhotoPadHitBase + (1.0 - kPhotoPadHitBase) * rawValue / 127.0);
    } else if (b.mode == lunar24::core::MidiInputMode::absolute) {
      setPhotoShade_(g, rawValue / 127.0);
    } else {  // a relative knob: about two turns of an MPK encoder from away to covered
      setPhotoShade_(g, photoShade_[g] + lunar24::core::midi_relative_delta(b.mode, rawValue) / 64.0);
    }
    return;
  }

  if (b.targetKind == lunar24::core::MidiTargetKind::action) {
    if (b.key.kind == lunar24::core::MidiBindingKind::cc) {
      const bool wasOn = midiLastRaw_[row] >= 64;
      midiLastRaw_[row] = rawValue;
      if (rawValue < 64 || wasOn) return;
    } else if (rawValue == 0) return;
    using lunar24::core::MidiAction;
    using lunar24::core::LiveCommand;
    if (b.action <= MidiAction::drone_key_6) {
      // Heard at once (the runtime owns the live key state); recorded for the UI.
      const int v = static_cast<int>(b.action) - static_cast<int>(MidiAction::drone_key_1);
      const bool open = !rt.droneVoiceKey(v);
      rt.setDroneVoiceKey(v, open);
      LiveCommand c;
      c.kind = LiveCommand::Kind::DroneKey;
      c.side = static_cast<std::uint32_t>(v);
      c.value = open ? 1.0 : 0.0;
      (void)fromAudioQueue_.push(c);  // best effort: a lost record costs one UI refresh
      return;
    }
    if (b.action == MidiAction::master_mute) {
      toggleMuted();
      return;
    }
    // Cartridge / preset actions: the UI thread executes them (it owns the state).
    LiveCommand c;
    c.kind = LiveCommand::Kind::Action;
    c.index = static_cast<std::uint32_t>(b.action);
    (void)fromAudioQueue_.push(c);
    return;
  }

  const lunar24::core::ParameterDescriptor* d = lunar24::core::find_parameter(b.parameter);
  if (d == nullptr) return;
  const auto drive = lunar24::core::midi_parameter_drive(b);
  if (drive != lunar24::core::MidiParameterDrive::follow) {
    // A switch stepped by a press: a pad hit, or a CC button rising past 64.
    if (b.key.kind == lunar24::core::MidiBindingKind::cc) {
      const bool wasOn = midiLastRaw_[row] >= 64;
      midiLastRaw_[row] = rawValue;
      if (rawValue < 64 || wasOn) return;
    } else if (rawValue == 0) {
      return;
    }
    const int positions = lunar24::core::midi_parameter_positions(b.parameter);
    const double cur = midiParameterValue_[static_cast<std::uint32_t>(b.parameter)];
    const int at = static_cast<int>(std::lround((cur - d->min) / d->step));
    const int next = (std::clamp(at, 0, positions - 1) + 1) % positions;
    (void)sendParameterFromAudioThread_(b.parameter, d->min + next * d->step, false, static_cast<int>(row));
    return;
  }
  if (b.mode == lunar24::core::MidiInputMode::absolute) {
    // Pickup: the knob engages once the controller sweeps across the current value,
    // so a page/preset change never jumps the parameter.
    const double target01 = static_cast<double>(rawValue) / 127.0;
    double& cur = midiValue_[row];
    if (b.key.kind == lunar24::core::MidiBindingKind::cc && !midiPickedUp_[row]) {
      const double cur01 = lunar24::core::value_to_knob(*d, cur);
      const int curRaw = static_cast<int>(cur01 * 127.0 + 0.5);
      const int prevRaw = midiLastRaw_[row];
      midiLastRaw_[row] = rawValue;
      // Engage when the controller is at the current value (within one step, also on
      // the very first message) or has just moved across it; otherwise wait.
      const int now = rawValue - curRaw;
      const bool atValue = now >= -1 && now <= 1;
      const bool crossed = prevRaw >= 0 && ((prevRaw - curRaw) > 0) != (now > 0);
      if (!atValue && !crossed) return;
      midiPickedUp_[row] = true;
    }
    midiLastRaw_[row] = rawValue;
    cur = lunar24::core::knob_to_value(*d, target01);  // the controller turns the knob
    if (d->step > 0.0) cur = d->min + std::round((cur - d->min) / d->step) * d->step;
    (void)sendParameterFromAudioThread_(b.parameter, cur, false, static_cast<int>(row));
    return;
  }
  // Relative modes: apply the decoded delta to the cached value. A continuous knob steps
  // finely when the encoder turns slowly (1/512 of the range: about 5 cents of drone TUNE)
  // and up to 4x faster when it spins, so it can both fine-tune and sweep. // tuned by ear
  const int delta = lunar24::core::midi_relative_delta(b.mode, rawValue);
  if (delta == 0) return;
  double& cur = midiValue_[row];
  const std::uint64_t now = rt.currentSample();
  const std::uint64_t last = midiLastTickAt_[row];
  const double gapSeconds = last == kNoTick || now < last ? 1.0 : static_cast<double>(now - last) / sampleRate_;
  midiLastTickAt_[row] = now;
  const double speed = gapSeconds < 0.04 ? 4.0 : gapSeconds < 0.12 ? 2.0 : 1.0;
  if (d->step > 0.0) {
    cur = std::clamp(cur + static_cast<double>(delta) * d->step, d->min, d->max);
  } else {  // continuous: step the knob's travel, so tapered knobs stay fine at the slow end
    const double pos = lunar24::core::value_to_knob(*d, cur) + static_cast<double>(delta) / 512.0 * speed;
    cur = lunar24::core::knob_to_value(*d, pos);
  }
  (void)sendParameterFromAudioThread_(b.parameter, cur, false, static_cast<int>(row));
}

// Keep the pickup/relative base in step with parameter changes from every other
// source (panel drags arrive through drainLive_, MIDI CCs through the caller above).
inline void StandaloneAudioEngine::noteParameterSeen_(ParameterId id, double value) {
  midiParameterValue_[static_cast<std::uint32_t>(id)] = value;
  refreshMidiMap_();
  const MidiMapSlot& s = midiMapSlots_[midiMapReadIdx_];
  for (std::uint32_t i = 0; i < s.map.count(); ++i) {
    const lunar24::core::MidiBinding& b = s.map.at(i);
    if (b.targetKind == lunar24::core::MidiTargetKind::parameter && b.parameter == id) {
      midiValue_[i] = value;
      midiPickedUp_[i] = false;  // the value jumped: re-arm pickup
      midiLastRaw_[i] = -1;
    }
  }
}

inline void StandaloneAudioEngine::reseedMidiBindingsFromState_() {
  if (!definition_) return;
  for (const auto& d : lunar24::registry::kParameters) {
    if (d.stable_id.substr(0, 9) == "keyboard.")
      noteParameterSeen_(d.id, definition_->runtime().keyboardParameterValue(d.id));
  }
}

// UI thread: step the effector cartridge on BOTH sides (the CartridgeControl rule).
inline void StandaloneAudioEngine::stepCartridge(int delta) {
  const DeviceStateV1* st = canonicalState();
  if (st == nullptr) return;
  const int cur = static_cast<int>(static_cast<std::uint32_t>(st->leftEffector.program) / 3u);
  const int next = (cur + 13 + (delta < 0 ? -1 : 1)) % 13;
  (void)postEffectorProgram(0, static_cast<lunar24::core::ProgramId>(next * 3));
  (void)postEffectorProgram(1, static_cast<lunar24::core::ProgramId>(next * 3));
}

inline int StandaloneAudioEngine::syncParametersFromAudioThread() {
  if (!definition_) return 0;
  int n = 0;
  lunar24::core::LiveCommand c;
  while (fromAudioQueue_.pop(c)) {
    if (c.kind == lunar24::core::LiveCommand::Kind::GraphPlanDone) {
      delete static_cast<GraphPlan*>(c.graphPlan);  // the audio thread is done with it
      continue;
    }
    switch (c.kind) {
      case lunar24::core::LiveCommand::Kind::Parameter:
        lunar24::core::state_set_param(definition_->mutableDeviceState(), c.parameter, c.value);
        ++editCount_;
        break;
      case lunar24::core::LiveCommand::Kind::DroneKey:
        // Record-only: the audio thread already applied it to the runtime.
        if (c.side < 6) droneKeys_[c.side] = c.value > 0.5;
        ++editCount_;
        break;
      case lunar24::core::LiveCommand::Kind::Action: {
        const auto action = static_cast<lunar24::core::MidiAction>(c.index);
        if (action == lunar24::core::MidiAction::cartridge_next) {
          stepCartridge(1);
        } else if (action == lunar24::core::MidiAction::cartridge_prev) {
          stepCartridge(-1);
        } else if (action >= lunar24::core::MidiAction::preset_load_a &&
                   action <= lunar24::core::MidiAction::preset_load_d) {
          (void)postKeyboardPreset(PresetAction::Load,
                                   static_cast<std::uint32_t>(action) -
                                       static_cast<std::uint32_t>(lunar24::core::MidiAction::preset_load_a));
        }
        break;
      }
      default:
        break;
    }
    ++n;
  }
  return n;
}

inline bool StandaloneAudioEngine::enqueueEventFromAudioThread(const lunar24::core::ControlEvent& e,
                                                               int offset) {
  if (!definition_) return false;
  SynthRuntime& rt = definition_->runtime();
  const std::uint64_t at = rt.currentSample() + static_cast<std::uint64_t>(offset > 0 ? offset : 0);
  return rt.enqueueControlEvent(lunar24::core::TimedControlEvent{e, at});
}

inline void StandaloneAudioEngine::drainLive_(SynthRuntime& rt) {
  using lunar24::core::LiveCommand;
  LiveCommand c;
  bool graphChanged = false;
  // If a postEvent found the queue full, the backlog's note stream is untrustworthy
  // (a note-off is missing): skip every queued note event and reconcile to
  // all-gates-off below. Parameter/cable/menu commands stay — they are consistent
  // with the saved state by the post* capacity checks.
  const bool reconcile = eventReconcileRequested_.exchange(false, std::memory_order_acq_rel);
  // Plans that found the return queue full last time go back first.
  std::uint32_t keep = 0;
  for (std::uint32_t i = 0; i < planReturnCount_; ++i) {
    LiveCommand r;
    r.kind = LiveCommand::Kind::GraphPlanDone;
    r.graphPlan = planReturns_[i];
    if (!fromAudioQueue_.push(r)) planReturns_[keep++] = planReturns_[i];
  }
  planReturnCount_ = keep;
  while (liveQueue_.pop(c)) {
    if (reconcile && c.kind == LiveCommand::Kind::Event) continue;  // untrusted note stream
    switch (c.kind) {
      case LiveCommand::Kind::Parameter: {
        noteParameterSeen_(c.parameter, c.value);
        if (rt.setKeyboardParameter(c.parameter, c.value)) break;  // keyboard menu: now, in order
        lunar24::core::ControlEvent e{};
        e.kind = lunar24::core::ControlEventKind::parameter;
        e.parameter = c.parameter;
        e.value = static_cast<lunar24::core::SignalSample>(c.value);
        e.source = 100;  // the UI producer
        (void)rt.enqueueControlEvent(lunar24::core::TimedControlEvent{e, rt.currentSample()});
        break;
      }
      case LiveCommand::Kind::Event:
        (void)rt.enqueueControlEvent(lunar24::core::TimedControlEvent{c.event, rt.currentSample()});
        break;
      case LiveCommand::Kind::Connect:
        if (c.hadOld) (void)rt.disconnect(c.oldSource, c.sink);
        (void)rt.connect(c.source, c.sink);
        if (!installGraphPlan_(rt, c.graphPlan)) graphChanged = true;
        break;
      case LiveCommand::Kind::Disconnect:
        (void)rt.disconnect(c.source, c.sink);
        if (!installGraphPlan_(rt, c.graphPlan)) graphChanged = true;
        break;
      case LiveCommand::Kind::EffectorProgram:
        rt.setEffectorProgram(static_cast<int>(c.side), c.program);
        break;
      case LiveCommand::Kind::DroneKey:
        rt.setDroneVoiceKey(static_cast<int>(c.side), c.value > 0.5);
        break;
      case LiveCommand::Kind::PhotoShade:  // the mouse's hand; a relative knob continues from it
        if (c.side < 4) photoShade_[c.side] = c.value;
        rt.setDronePhotoShade(static_cast<int>(c.side), c.value);
        break;
      case LiveCommand::Kind::SeqStep:
        rt.setKeyboardSeqStep(static_cast<int>(c.side), static_cast<int>(c.index),
                              static_cast<std::uint8_t>(c.value), c.hadOld);
        break;
      case LiveCommand::Kind::KeyboardRight:
        rt.setKeyboardRightScalar(c.parameter, c.value);
        break;
      case LiveCommand::Kind::KeyboardPreset:
        rt.keyboardPresetAction(static_cast<int>(c.side), c.index);
        // Read the audio-owned preset result, never the UI's mutable saved state.
        reseedMidiBindingsFromState_();
        break;
      case LiveCommand::Kind::KeyboardSelector:
        rt.setKeyboardClockSelector(static_cast<int>(c.side), c.index, static_cast<std::uint8_t>(c.value));
        break;
      case LiveCommand::Kind::GraphPlanDone:
      case LiveCommand::Kind::Action:
        break;  // audio -> UI only (MIDI-triggered cartridge/preset actions); never drained here
    }
  }
  // Fallback only: every cable edit carries a plan compiled on the UI thread, so this
  // allocating rebuild runs only if a plan was missing (out of memory) or built for a
  // different patch, which the UI-side patch copy should never allow.
  if (graphChanged) (void)rt.rebuild();
  if (reconcile) {
    // All-gates-off: the failsafe sorts to phase 0 within this block, and no gate_on
    // survived the backlog drop above, so nothing can reopen a gate behind it.
    lunar24::core::ControlEvent e{};
    e.kind = lunar24::core::ControlEventKind::reset;
    e.value = 1;
    e.source = 100;  // the UI producer
    (void)rt.enqueueControlEvent(lunar24::core::TimedControlEvent{e, rt.currentSample()});
  }
}

// ---- cable edits: plan on the UI thread, install on the audio thread ----------
inline StandaloneAudioEngine::GraphPlan* StandaloneAudioEngine::planCableEdit_() {
  if (!uiPatch_) return nullptr;
  auto* plan = new (std::nothrow) GraphPlan;
  if (plan != nullptr) definition_->runtime().planGraph(*uiPatch_, *plan);
  return plan;
}

inline bool StandaloneAudioEngine::installGraphPlan_(SynthRuntime& rt, void* p) {
  auto* plan = static_cast<GraphPlan*>(p);
  if (plan == nullptr) return false;
  const bool matched = rt.installGraphPlan(*plan);
  returnGraphPlan_(plan);
  return matched;
}

inline void StandaloneAudioEngine::returnGraphPlan_(GraphPlan* plan) {
  lunar24::core::LiveCommand r;
  r.kind = lunar24::core::LiveCommand::Kind::GraphPlanDone;
  r.graphPlan = plan;
  if (fromAudioQueue_.push(r)) return;
  if (planReturnCount_ < sizeof(planReturns_) / sizeof(planReturns_[0])) planReturns_[planReturnCount_++] = plan;
  // else: 16 plans waiting and the queue full; leak this one rather than free it here.
}

inline void StandaloneAudioEngine::releaseGraphPlans_() {
  lunar24::core::LiveCommand c;
  while (liveQueue_.pop(c))
    if (c.kind == lunar24::core::LiveCommand::Kind::Connect || c.kind == lunar24::core::LiveCommand::Kind::Disconnect)
      delete static_cast<GraphPlan*>(c.graphPlan);
  while (fromAudioQueue_.pop(c))
    if (c.kind == lunar24::core::LiveCommand::Kind::GraphPlanDone) delete static_cast<GraphPlan*>(c.graphPlan);
  for (std::uint32_t i = 0; i < planReturnCount_; ++i) delete planReturns_[i];
  planReturnCount_ = 0;
}

}  // namespace lunar24::host
