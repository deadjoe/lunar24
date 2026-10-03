// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/plugin.h — the Lunar 24 host standalone plugin. This is the
// IPlugAPP plugin class the host opens. Its editor is intentionally empty for
// P5-①: the mandate is to prove the self-authored host bootstrap opens a real
// macOS window sized by the geometry choke point, not to render controls yet.
// The window-sizing logic lives in the mMakeGraphicsFunc lambda (below) — that is
// the one place the host consumes lunar24::host::compute_window_layout.

#pragma once
#include <host/midi_input_queue.h>

#include "IPlug_include_in_plug_hdr.h"

#include <atomic>
#include <chrono>
#include <memory>

#include <host/app_state_store.h>
#include <host/midi_map_store.h>
#include <host/midi_sustain.h>
#include <host/midi_timing.h>
#include <host/standalone_audio_engine.h>
#include <lunar24/core/input_state_machine.h>

using namespace iplug;

class LunarHostPlugin final : public Plugin
{
public:
  LunarHostPlugin(const InstanceInfo& info);

#if IPLUG_EDITOR
  // The window was resized (also once when it opens). iPlug2's default turns the window
  // size into the drawing size at scale 1, which crops the fixed 2400 x 1552 panel; keep the
  // panel's size and scale it to fit the window instead.
  void OnParentWindowResize(int width, int height) override;
#endif

#if IPLUG_DSP
  // GH#4 8B2 lifecycle gate: OnReset() runs at the stopped-stream boundary (CloseAudio
  // callbacks done -> SetBlockSize/SetSampleRate -> OnReset -> openStream/startStream). It is
  // the ONE place the host (re)prepares the runtime owner for the REAL device format.
  void OnReset() override;
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  // MIDI arrives on the audio thread (before ProcessBlock). Notes / velocity / aftertouch
  // go through the keyboard's input state machine; a few CCs drive existing panel knobs.
  void ProcessMidiMsg(const IMidiMsg& msg) override;

  // GH#4 8B3 (task#73): install the ACTUAL connected channel plan with fail-closed ADMISSION.
  // A derived class is the ONLY place the real host can drive the protected
  // IPlugProcessor::SetChannelConnections (the iPlug2 host is not a friend of IPlugProcessor), so
  // the app host calls this via a static_cast<LunarHostPlugin*> before OnReset(). It disconnects
  // ALL declared max channels (config.h APP branch "0-2 1-2 2-2 0-4 1-4 2-4" -> MaxNChannels 2/4)
  // then re-connects only [0,inCh)/[0,outCh), so OnReset()/AppProcess read the REAL count
  // (NInChansConnected/NOutChansConnected) and a 2-out device never re-asserts the 4-channel max.
  // An ILLEGAL plan (inCh not in {0,1,2}, outCh not in {0,2,4}, or exceeding the declared max) is
  // rejected: the host installs a 0-in/0-out sentinel and returns false so the owner is NOT-READY
  // (never "a device channel plan silently clamped/truncated"). The host MUST check the return.
  // 0/0 is itself legal (the fail-closed NOT-READY sentinel the invalidation helper installs).
  bool setActualChannelPlan(int inCh, int outCh);
#endif

  // GH#12 task#105: the APP host hands in the ALREADY-RESOLVED per-user settings directory (the
  // directory that holds settings.ini). The plugin never re-derives it — one resolution, one truth
  // (W17). Passing nullptr/"" means "no path": the store reports NoPath and performs no IO.
  void setStateDirectory(const char* dir);

  // Save the machine state. Called by IPlugAPPHost's destructor AFTER CloseAudio() has returned
  // (the exit save) and from OnIdle while running (the autosave). Both run on the UI thread,
  // the only thread that edits the saved state, so the save never races an edit.
  lunar24::host::StateSaveOutcome saveDeviceState();

  // UI thread, every ~20 ms: autosave at most every 30 s, and only after an edit, so a crash
  // or power cut loses at most the last half minute.
  void OnIdle() override;

  // UI thread: put the whole machine back to its power-on default (every knob, switch, cable,
  // keyboard setting and sequence). Done at the next stopped-stream boundary: the audio stream
  // is briefly reopened and OnReset publishes the default instead of the current state.
  void requestFactoryReset();

  // UI thread (the app's MIDI input was closed or switched in Preferences): notes
  // held from that input can never send their note-offs now. Invalidates the old
  // input queue; audio releases notes and clears sustain before admitting a new epoch.
  void midiInputClosed();
  // Driver callback pushes bytes; AppProcess drains before rendering each block.
  void queueMidiInput(lunar24::host::MidiInputQueue::Message message) { (void)midiQueue_.push(message); }
  void drainMidiInput(int frames);

  // UI thread (the app's MIDI input selection changed): the name bindings match
  // against. "" means no real device (off / virtual): only device-agnostic bindings
  // match then. Republishes the map so the audio thread's snapshot carries it.
  void setMidiInputDeviceName(const char* name);

  // ---- MIDI rig settings (channel filter / octave shift / velocity curve) -----------
  // The panel's MIDI settings overlay edits these; the store persists them. The audio
  // thread reads the atomics, the UI thread owns the store.
  int midiChannelFilter() const { return midiChannelFilter_.load(std::memory_order_relaxed); }
  int midiOctaveShift() const { return midiOctaveShift_.load(std::memory_order_relaxed); }
  int midiVelocityCurve() const { return midiVelocityCurve_.load(std::memory_order_relaxed); }
  void setMidiRigSettings(int channelFilter, int octaveShift, int curve);
  // The plugin's binding store (the MIDI settings overlay edits it through this).
  lunar24::host::MidiMapStore& midiStore() { return midiMapStore_; }
  // Republish the current map (after the overlay edits it).
  void republishMidiMap() {
    engine_.publishMidiMap(midiMapStore_.map(), midiInputDeviceName_.c_str());
  }
  // The last note/CC message seen, for the learn overlay: packed (kind << 20 |
  // channel << 8 | number) plus a sequence that bumps per message. Audio thread
  // writes, UI reads.
  std::uint32_t midiLastMessage() const { return midiLastMessage_.load(std::memory_order_relaxed); }
  std::uint64_t midiMessageSeq() const { return midiMessageSeq_.load(std::memory_order_relaxed); }
  // The keyboard plates MIDI notes are holding down (bit 0 = C .. bit 11 = B). UI thread.
  std::uint16_t midiLitPlates() const { return midiLights_.mask(); }

private:
  bool factoryResetRequested_ = false;  // UI thread only (OnReset runs on the UI thread in the app)
  lunar24::host::MidiInputQueue midiQueue_;
  lunar24::host::MidiNoteOwnership midiNotes_;
  lunar24::host::MidiPlateLights midiLights_;  // plates lit by MIDI notes (audio writes, UI reads)
  std::atomic<int> midiChannelFilter_{0};     // 0 = any, else 1..16
  std::atomic<int> midiOctaveShift_{0};       // semitones, -36..+36
  std::atomic<int> midiVelocityCurve_{0};     // core::MidiVelocityCurve
  std::atomic<std::uint32_t> midiLastMessage_{0};
  std::atomic<std::uint64_t> midiMessageSeq_{0};

  // The framework-free runtime owner, held BY VALUE. It owns the address-stable
  // MachineRuntimeDefinition (heap) + the single DeviceAdapter (task#71). ProcessBlock is a
  // PURE delegate to it.
  lunar24::host::StandaloneAudioEngine engine_;

  // The narrow APP-session persistence policy (one read attempt, the pending transfer payload and
  // the lifecycle-save gate) + the real FileOps. It holds NO second editable state bank and does
  // NO file IO on the audio path.
  lunar24::host::AppStateStore stateStore_;

  // The panel editor's shared state (host/panel_editor.h), type-erased so this header stays
  // free of IGraphics types.
  std::shared_ptr<void> uiState_;

  // The user's MIDI controller map (its own product file) and the active input's name
  // ("" = none/virtual). Both are UI-thread state; the audio thread sees only the
  // snapshot the engine publishes.
  lunar24::host::MidiMapStore midiMapStore_;
  std::string midiInputDeviceName_;

  // MIDI -> keyboard. Mod wheel / CC74 = filter cutoff, CC71 = resonance, CC91 = effector
  // blend, CC7 = master (CC learn only ever targets existing panel controls).
  static constexpr lunar24::core::CcBinding kMidiCc[5] = {
      {1, lunar24::core::ParameterId::vcf_l_freq},
      {74, lunar24::core::ParameterId::vcf_l_freq},
      {71, lunar24::core::ParameterId::vcf_l_res},
      {91, lunar24::core::ParameterId::effector_blend},
      {7, lunar24::core::ParameterId::effector_master}};
  lunar24::core::InputStateMachine midiInput_{kMidiCc, 5};
  // Pitch bend range (the common default), sustain pedal state and MIDI clock tick count.
  static constexpr double kPitchBendSemitones = 2.0;
  lunar24::host::MidiSustain sustain_;
  lunar24::host::MidiClockFollower midiClock_;  // audio thread: MIDI clock / START
  std::uint64_t savedEditCount_ = 0;
  std::chrono::steady_clock::time_point lastAutosave_ = std::chrono::steady_clock::now();
  std::uint64_t midiSeq_ = 0;
};
