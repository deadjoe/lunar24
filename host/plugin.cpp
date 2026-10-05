// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/plugin.cpp — the Lunar 24 host standalone plugin implementation.
//
//  mandate (the geometry choke point): the host MUST size its window by
// consuming lunar24::host::compute_window_layout, NOT by hardcoding a size or
// reverting to design scale. A host that bypasses this is exactly the clamp
// defect under test (it lets the window-manager crop the panel bottom). So:
//
//   * zoomScale come from the LUNAR_HOST_FORCE_CLAMP env var: set to "1" -> the
//     negative (clamp, design scale) path; otherwise -> the fit path.
//   * drawScale + logicalW/H are the geometry module's answer, never computed here.
//   * the open window = SetEditorSize(logicalW, logicalH), which drives the
//     ClientResize in the stock IPlugAPP_dialog MainDlgProc (WM_INITDIALOG).
//
// iPlug2's MakeGraphics(*this, designW, designH, fps, drawScale) is called with
// the DESIGN dims + drawScale so that the IGraphics internal view (
// WindowWidth = mWidth * mDrawScale) equals the logical window the dialog
// opens — the design space is drawn at drawScale into the logical window, and
// retina is a separate SetScreenScale multiplier (slice), never folded in.

#include "plugin.h"
#include "about.h"
#include <host/midi_timing.h>
#include "IPlug_include_in_plug_src.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <type_traits>
#include <lunar24/core/host_window_fit.h>
#include <host/window_layout.h>
#include <lunar24/core/state_default.h>
#include "panel_editor.h"

using namespace iplug::igraphics;

// Platform geometry probes implemented in host/main.mm (the ObjC++ bootstrap).
// The plugin (framework-free-ish C++) asks the host for the screen's LOGICAL
// area + retina scale; the layout math is all done here via the core module.
extern "C" double lunar_host_avail_logical_w();
extern "C" double lunar_host_avail_logical_h();
extern "C" double lunar_host_screen_scale();
extern "C" bool lunar_host_force_clamp();
extern "C" void lunar_host_place_view(void* view, double x, double y);
extern "C" void lunar_host_case_margins(void* view, double* side, double* top, double* bottom);
extern "C" void lunar_host_audio_watchdog();
extern "C" void lunar_host_request_audio_reopen();
extern "C" void lunar_host_log(const char* line);
extern "C" bool lunar_host_recordings_dir(char* out, std::size_t capacity);
extern "C" void lunar_host_reveal_dir(const char* utf8Path);

LunarHostPlugin::LunarHostPlugin(const InstanceInfo& info)
    : Plugin(info, MakeConfig(0, 0))
{
  engine_.setAudioTap(&recorder_);
#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    // zoomScale <= 0 -> fit (delegate to core); zoomScale > 0 -> explicit zoom.
    // The env var is the ONLY place the fit-vs-clamp decision is injected, so the
    // negative path (clamp) is reproducible on demand.
    const double zoomScale = lunar_host_force_clamp() ? 1.0 : 0.0;

    const double designW = lunar24::core::kDesignWidth;
    const double designH = lunar24::core::kDesignHeight;
    const double availW = lunar_host_avail_logical_w();
    const double availH = lunar_host_avail_logical_h();
    const double screenScale = lunar_host_screen_scale();

    // The design-space bottom row is the reachability predicate input; does
    // not make a control-reachability claim, but the choke point still computes it
    // from the window it actually opens, which is exactly the bug-detector shape.
    const lunar24::core::DesignRect fullDesign{0.0, 0.0, designW, designH};

    const auto layout = lunar24::host::compute_window_layout(
        designW, designH, availW, availH, zoomScale, screenScale, fullDesign);

    // THIS is the size the host opens: the geometry module's logical W/H. It runs
    // inside OpenWindow (before ClientResize), so GetEditorWidth/Height return it.
    SetEditorSize(static_cast<int>(layout.logicalW), static_cast<int>(layout.logicalH));

    // Design-space IGraphics. WindowWidth = designW * drawScale == logicalW,
    // so the internal view fills the dialog the host just sized.
    return MakeGraphics(*this, static_cast<int>(designW), static_cast<int>(designH),
                        PLUG_FPS, static_cast<float>(layout.drawScale));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    if (pGraphics->NControls() > 0) {
      return;  // already laid out; don't re-attach on a resize relayout
    }
    // The whole panel (host/panel_editor.h). The shared editor state lives as long as the plugin.
    auto shared = std::make_shared<lunar24::host::ui::EditorShared>(engine_);
    shared->factoryReset = [this]() { requestFactoryReset(); };
    // The MIDI settings overlay reads/edits the plugin's binding store through these bridges.
    shared->midiStore = &midiMapStore_;
    shared->midi.inputDeviceName = [this]() { return midiInputDeviceName_; };
    shared->midi.messageSeq = [this]() { return midiMessageSeq(); };
    shared->midi.lastMessage = [this]() { return midiLastMessage(); };
    shared->midi.litPlates = [this]() { return midiLitPlates(); };
    shared->midi.channelFilter = [this]() { return midiChannelFilter(); };
    shared->midi.octaveShift = [this]() { return midiOctaveShift(); };
    shared->midi.velocityCurve = [this]() { return midiVelocityCurve(); };
    shared->midi.splitNote = [this]() { return midiSplitNote(); };
    shared->midi.setRigSettings = [this](int c, int o, int v, int sp) { setMidiRigSettings(c, o, v, sp); };
    shared->rec.recording = [this]() { return recording(); };
    shared->rec.seconds = [this]() { return recordingSeconds(); };
    shared->rec.toggle = [this](int source) { toggleRecording(source); };
    shared->midi.bindingsChanged = [this]() {
      (void)midiMapStore_.save();
      republishMidiMap();
    };
    uiState_ = shared;
    lunar24::host::ui::BuildPanel(pGraphics, *shared);
  };
#endif
}

bool LunarHostPlugin::OnHostRequestingAboutBox() { return lunar24::host::showAboutBox(); }

#if IPLUG_EDITOR
void LunarHostPlugin::OnParentWindowResize(int width, int height)
{
  // Zoom the panel to the window (iPlug2's default would reset the zoom to 1 and crop it).
  // On macOS the window also draws a metal case round the panel (host/main.mm).
  IGraphics* g = GetUI();
  if (g == nullptr || width <= 0 || height <= 0) return;
  const double windowScale = g->GetPlatformWindowScale();
  lunar24::host::CaseMargins margins;
  lunar_host_case_margins(g->GetWindow(), &margins.side, &margins.top, &margins.bottom);
  const auto p = lunar24::host::place_panel(width / windowScale, height / windowScale, lunar24::core::kDesignWidth,
                                            lunar24::core::kDesignHeight, margins);
  g->Resize(static_cast<int>(lunar24::core::kDesignWidth), static_cast<int>(lunar24::core::kDesignHeight),
            static_cast<float>(p.scale), false);
  lunar_host_place_view(g->GetWindow(), p.x * windowScale, p.y * windowScale);
}
#endif

#if IPLUG_DSP
// the ProcessBlock bridge below casts sample** <-> double**. That relabeling is only
// valid under iPlug2's DEFAULT `sample = double`. A SAMPLE_TYPE_FLOAT build would reinterpret the
// buffers with the wrong element type -> UB. This is a compile-time hard gate, not a runtime or
// generated-check: the host must never silently compile such a bridge. (The wiring gate greps for
// this guard so removing it fails loudly too.)
static_assert(std::is_same_v<sample, double>,
              "Lunar24 host ProcessBlock bridge requires iPlug2 sample == double");

void LunarHostPlugin::OnReset()
{
  // the stopped-stream boundary. Rebuild the runtime owner for the REAL device
  // format the host is about to open. The physical connector counts are read from the host
  // (NOT hardcoded): with the plan is negotiated from the device capability and
  // installed via setActualChannelPlan BEFORE this runs, so NInChansConnected/
  // NOutChansConnected are the actual open counts. A failure (e.g. <2 outputs, or the 0/0
  // failure sentinel) leaves the engine not-ready and ProcessBlock fail-silent.
  //
  //  this SAME boundary carries the state policy, in this exact order:
  //   1. captureCanonical: keep the committed config BEFORE prepare releases the owner, so a
  //      device reopen can never fall back to the power-on default or re-read the disk;
  //   2. loadOnce: ONE startup read attempt per APP session (the store latches it explicitly);
  //   3. prepare: the unchanged owner (re)build for the real device format;
  //   4. publishPending: only when prepare produced a ready owner — publish the pending restore
  //      through the engine's ONE real candidate path. A rejection is atomic and the store records
  //      the reason; a failed prepare leaves the pending intact for the NEXT legal boundary.
  // Preserve the last MIDI knob edits before capturing the state and discarding
  // the old runtime queues. The audio callback has stopped at this boundary.
  engine_.syncParametersFromAudioThread();
  // REC keeps going across a device reopen at the same rate; at another rate the file would
  // play back at the wrong speed, so the recording stops there.
  if (recorder_.recording() && std::lround(GetSampleRate()) != recorder_.sampleRate()) {
    const auto r = recorder_.stop();
    char line[160];
    std::snprintf(line, sizeof line, "recording stopped (sample rate changed): %.1f s", r.seconds);
    lunar_host_log(line);
  }
  // The old runtime's notes die with it; the sustain pedal's held-note ledger
  // belongs to that stream (a stale pedal-down would defer the new stream's
  // note-offs forever).
  sustain_.reset();
  midiNotes_.reset();
  midiLights_.reset();
  midiQueue_.invalidate();
  stateStore_.captureCanonical(engine_);
  stateStore_.loadOnce();
  if (factoryResetRequested_) {  // the panel's RESET: the power-on default, keyboard presets kept
    factoryResetRequested_ = false;
    const lunar24::core::DeviceStateV1* current = stateStore_.pending();  // just captured above
    stateStore_.replacePending(
        current != nullptr
            ? lunar24::core::make_reset_device_state(*current, lunar24::host::kLunarStartupSeed)
            : lunar24::core::make_default_device_state(lunar24::host::kLunarStartupSeed));
    engine_.closeDroneKeys();  // like a fresh start: the drones wait for their keys
  }
  engine_.prepare(lunar24::host::kLunarStartupSeed, GetSampleRate(), GetBlockSize(),
                  NInChansConnected(), NOutChansConnected());
  if (engine_.isReady())
    stateStore_.publishPending(engine_, GetSampleRate(), GetBlockSize(), NInChansConnected(),
                               NOutChansConnected());
}

void LunarHostPlugin::setStateDirectory(const char* dir)
{
  // accept the host's ALREADY-RESOLVED per-user settings directory. This class
  // must never re-derive it (no environment lookup, no platform branch here) — the APP host owns
  // the one resolution, and this seam only carries it into the store.
  stateStore_.setDirectory(dir != nullptr ? std::string(dir) : std::string());
  // The MIDI map lives in the same directory; load it once here and publish the snapshot.
  midiMapStore_.setDirectory(dir != nullptr ? std::string(dir) : std::string());
  midiMapStore_.load();
  engine_.publishMidiMap(midiMapStore_.map(), midiInputDeviceName_.c_str());
  const lunar24::core::MidiRigSettings& s = midiMapStore_.settings();
  midiChannelFilter_.store(s.channelFilter, std::memory_order_relaxed);
  midiOctaveShift_.store(s.octaveShift, std::memory_order_relaxed);
  midiVelocityCurve_.store(static_cast<int>(s.velocityCurve), std::memory_order_relaxed);
  midiSplitNote_.store(s.splitNote, std::memory_order_relaxed);
}

void LunarHostPlugin::setMidiInputDeviceName(const char* name)
{
  midiInputDeviceName_ = name != nullptr ? name : "";
  engine_.publishMidiMap(midiMapStore_.map(), midiInputDeviceName_.c_str());
}

void LunarHostPlugin::setMidiRigSettings(int channelFilter, int octaveShift, int curve, int splitNote)
{
  using lunar24::core::kMidiSplitNoteLow;
  using lunar24::core::kMidiSplitNoteHigh;
  midiSplitNote_.store(std::clamp(splitNote, int(kMidiSplitNoteLow), int(kMidiSplitNoteHigh)),
                       std::memory_order_relaxed);
  midiChannelFilter_.store(std::clamp(channelFilter, 0, 16), std::memory_order_relaxed);
  midiOctaveShift_.store(std::clamp(octaveShift, -36, 36), std::memory_order_relaxed);
  midiVelocityCurve_.store(std::clamp(curve, 0, 2), std::memory_order_relaxed);
  lunar24::core::MidiRigSettings s;
  s.channelFilter = static_cast<std::uint8_t>(midiChannelFilter_.load(std::memory_order_relaxed));
  s.octaveShift = static_cast<std::int8_t>(midiOctaveShift_.load(std::memory_order_relaxed));
  s.velocityCurve = static_cast<lunar24::core::MidiVelocityCurve>(
      midiVelocityCurve_.load(std::memory_order_relaxed));
  s.splitNote = static_cast<std::uint8_t>(midiSplitNote_.load(std::memory_order_relaxed));
  if (midiMapStore_.setSettings(s)) (void)midiMapStore_.save();
}

lunar24::host::StateSaveOutcome LunarHostPlugin::saveDeviceState()
{
  // the lifecycle (exit) save. Pure delegate to the narrow store: it picks the
  // source (committed canonical, else the retained last legal config), encodes to the exact wire
  // size and runs the atomic temp->flush->replace. Never a disk read, never an overwrite of a file
  // that was present but unusable.
  return stateStore_.save(engine_);
}

void LunarHostPlugin::requestFactoryReset()
{
  // Without a running machine there is nothing to swap: reset at the next stream open instead.
  if (!engine_.isReady()) {
    factoryResetRequested_ = true;
    lunar_host_request_audio_reopen();  // OnReset runs when the stream reopens
    return;
  }
  engine_.syncParametersFromAudioThread();  // the reset keeps presets edited by MIDI too
  const lunar24::core::DeviceStateV1 reset =
      lunar24::core::make_reset_device_state(*engine_.canonicalState(), lunar24::host::kLunarStartupSeed);
  engine_.closeDroneKeys();  // like a fresh start: the drones wait for their keys
  (void)engine_.swapDeviceState(reset);
}

void LunarHostPlugin::dropMidiLedgersAfterSwap_()
{
  const std::uint64_t swaps = engine_.swapCount();
  if (swaps == seenSwaps_) return;
  seenSwaps_ = swaps;
  sustain_.reset();
  midiNotes_.reset();
  midiLights_.reset();
}

void LunarHostPlugin::midiInputClosed()
{
  midiQueue_.invalidate();
}

void LunarHostPlugin::toggleRecording(int source)
{
  char line[1200];
  if (recorder_.recording()) {
    const auto r = recorder_.stop();
    std::snprintf(line, sizeof line, "recording stopped: %.1f s, %llu frames dropped", r.seconds,
                  static_cast<unsigned long long>(r.droppedFrames));
    lunar_host_log(line);
    if (!recordingDir_.empty()) lunar_host_reveal_dir(recordingDir_.c_str());
    return;
  }
  const int rate = static_cast<int>(std::lround(engine_.sampleRate()));
  char dir[1024];
  if (rate <= 0 || !lunar_host_recordings_dir(dir, sizeof dir)) {
    lunar_host_log("recording not started: no audio stream or no Music/Lunar 24 folder");
    return;
  }
  recordingDir_ = dir;
  // "Lunar24 2026-10-04 12-30-05.wav" (WET) and "... dry.wav" (DRY A left, DRY B right).
  char stamp[64];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  const bool haveTime = localtime_s(&local, &now) == 0;  // MSVC rejects std::localtime under /WX
#else
  const bool haveTime = localtime_r(&now, &local) != nullptr;
#endif
  // snprintf, not strftime: iPlug2's Windows UTF-8 layer redefines strftime as a macro.
  if (haveTime)
    std::snprintf(stamp, sizeof stamp, "Lunar24 %04d-%02d-%02d %02d-%02d-%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  else
    std::snprintf(stamp, sizeof stamp, "Lunar24 %lld", static_cast<long long>(now));
  const auto what = static_cast<lunar24::host::RecordSource>(std::clamp(source, 0, 2));
  const bool wet = what != lunar24::host::RecordSource::dry;
  const bool dry = what != lunar24::host::RecordSource::wet;
  const std::string base = lunar24::host::app_state_file_ops::joinUtf8(recordingDir_, stamp);
  std::FILE* wetFile = wet ? lunar24::host::app_state_file_ops::openNative(base + ".wav", "wb") : nullptr;
  std::FILE* dryFile = dry ? lunar24::host::app_state_file_ops::openNative(base + " dry.wav", "wb") : nullptr;
  if ((wet && wetFile == nullptr) || (dry && dryFile == nullptr)) {
    if (wetFile != nullptr) std::fclose(wetFile);
    if (dryFile != nullptr) std::fclose(dryFile);
    wetFile = dryFile = nullptr;
  }
  if ((wetFile == nullptr && dryFile == nullptr) || !recorder_.start(wetFile, dryFile, rate)) {  // start closes on failure
    std::snprintf(line, sizeof line, "recording not started: cannot write %s", base.c_str());
    lunar_host_log(line);
    return;
  }
  std::snprintf(line, sizeof line, "recording %s at %d Hz: %s", lunar24::host::record_source_name(what), rate,
                base.c_str());
  lunar_host_log(line);
}

void LunarHostPlugin::OnIdle()
{
  // Record what MIDI changed on the audio thread. Here, not in the panel's redraw, so it is
  // saved even while the panel is closed (a plugin window usually is).
  engine_.syncParametersFromAudioThread();
  lunar_host_audio_watchdog();  // reopen audio if the device went away or the system output changed
  logMidiClock_();

  constexpr auto kAutosaveInterval = std::chrono::seconds(30);
  const auto now = std::chrono::steady_clock::now();
  if (!engine_.isReady() || engine_.editCount() == savedEditCount_ || now - lastAutosave_ < kAutosaveInterval)
    return;
  lastAutosave_ = now;
  savedEditCount_ = engine_.editCount();
  (void)saveDeviceState();
}

// At most every 2 s, on a transport or clock source change: what MIDI transport arrived and which clock the keyboard
// follows, so a clock problem on the owner's machine can be read from audio.log.
void LunarHostPlugin::logMidiClock_()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - lastClockLog_ < std::chrono::seconds(2)) return;
  lastClockLog_ = now;
  const std::uint32_t counts[4] = {midiClockTicksIn_.load(std::memory_order_relaxed),
                                   midiStartsIn_.load(std::memory_order_relaxed),
                                   midiContinuesIn_.load(std::memory_order_relaxed),
                                   midiStopsIn_.load(std::memory_order_relaxed)};
  const bool ext = engine_.keyboardFollowsExternalClock();
  const std::uint32_t edits = engine_.keyboardTempoEdits();
  // Ticks alone do not log (they arrive all through playback); a start / continue / stop, a
  // clock source switch or a tempo edit does, with the ticks since the previous line.
  if (counts[1] == loggedClock_[1] && counts[2] == loggedClock_[2] && counts[3] == loggedClock_[3] &&
      ext == loggedExtClock_ && edits == loggedTempoEdits_)
    return;
  char line[200];
  std::snprintf(line, sizeof line,
                "midi clock: +%u ticks, +%u start, +%u continue, +%u stop; keyboard clock %s; tempo edits %u",
                counts[0] - loggedClock_[0], counts[1] - loggedClock_[1], counts[2] - loggedClock_[2],
                counts[3] - loggedClock_[3], ext ? "external" : "internal", edits);
  lunar_host_log(line);
  for (int i = 0; i < 4; ++i) loggedClock_[i] = counts[i];
  loggedExtClock_ = ext;
  loggedTempoEdits_ = edits;
}

bool LunarHostPlugin::setActualChannelPlan(int inCh, int outCh)
{
  //  disconnect ALL declared max channels first, then connect only
  // [0,inCh)/[0,outCh). Called at the stopped-stream boundary (InitAudio) BEFORE OnReset, so the
  // engine prepares for the REAL plan and AppProcess attaches by the same count.
  //
  // FAIL-CLOSED ADMISSION. (inCh == 0 && outCh == 0) is the NOT-READY sentinel (LunarInvalidateAudio
  // installs it), NOT an iPlug2 IOConfig, so it is allowed as-is. Otherwise the ONLY accepted pairs
  // are one of the six APP configs declared in PLUG_CHANNEL_IO ("0-2 1-2 2-2 0-4 1-4 2-4") — tested
  // by the AUTHORITATIVE parsed-config admission IPlugProcessor::LegalIO(in,out), not by a hand-
  // written mirror. LegalIO returns true iff (in,out) is exactly one of those configs, so it rejects
  // (1,0)/(2,0) (no config has 0 outputs), (3,2), (2,3), and any out-of-domain pair — a per-direction
  // in{0,1,2} x out{0,2,4} check would wrongly accept an input-only plan with no outputs. Also cap at
  // the declared max so setActualChannelPlan never silently clamps/truncates. An ill-formed plan
  // REJECTS: it installs the 0-in/0-out NOT-READY sentinel and returns false; the host (InitAudio)
  // must check the bool and abort the open rather than proceeding with a truncated channel set.
  const int maxIn = MaxNChannels(ERoute::kInput);
  const int maxOut = MaxNChannels(ERoute::kOutput);
  const bool sentinel = (inCh == 0 && outCh == 0);
  if (!sentinel && (!LegalIO(inCh, outCh) || inCh > maxIn || outCh > maxOut)) {
    inCh = 0;
    outCh = 0;  // failure sentinel: the owner's <2 output fail-path turns it NOT-READY.
    SetChannelConnections(ERoute::kInput, 0, maxIn, false);
    SetChannelConnections(ERoute::kOutput, 0, maxOut, false);
    return false;
  }
  SetChannelConnections(ERoute::kInput, 0, maxIn, false);
  SetChannelConnections(ERoute::kOutput, 0, maxOut, false);
  if (inCh > 0) SetChannelConnections(ERoute::kInput, 0, inCh, true);
  if (outCh > 0) SetChannelConnections(ERoute::kOutput, 0, outCh, true);
  return true;
}

void LunarHostPlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // a PURE delegate to the framework-free owner. The owner either renders through
  // the production DeviceAdapter::renderBlock or returns a dropped status after
  // writing deterministic silence into the outputs. There is NO frame loop / scale / mapping /
  // pass-through / second output bank in the host — that would be a wiring defect. The host's
  // only own output is silence while the UI thread swaps the machine.
  const lunar24::host::StandaloneAudioEngine::AudioScope scope(engine_);
  if (!scope) {  // the UI thread is swapping the machine
    for (int c = 0; c < NOutChansConnected(); ++c)
      if (outputs[c] != nullptr) std::fill_n(outputs[c], nFrames, 0.0);
    return;
  }
  dropMidiLedgersAfterSwap_();
  // sample==double, so the pointer casts into the owner's framework-free surface are a
  // same-representation reinterpret_to_const (double** -> const double* const*): it only
  // adds const / re-types at the compile-time layer, and the host touches no data itself.
  engine_.processBlock(reinterpret_cast<const double* const*>(inputs),
                       reinterpret_cast<double* const*>(outputs),
                       NInChansConnected(), NOutChansConnected(), nFrames);
}

void LunarHostPlugin::drainMidiInput(int frames)
{
  // During a machine swap the messages wait in the queue for the next block.
  const lunar24::host::StandaloneAudioEngine::AudioScope scope(engine_);
  if (!scope) return;
  dropMidiLedgersAfterSwap_();
  const int now = lunar24::host::midiArrivalStamp();
  midiQueue_.drain([&] {
    // The old input's stream ended: release only the notes it still holds, so mouse and
    // computer-keyboard playing (and an arpeggio they hold) keep going. Should the event
    // queue be full, fall back to the all-gates-off failsafe rather than leave a note stuck.
    bool released = true;
    lunar24::host::release_midi_notes(midiNotes_, sustain_, 3, [&](lunar24::core::PerformanceInput in) {
      in.seq = ++midiSeq_;
      in.side = midiSides_.of(in.channel, in.noteId - 1);
      lunar24::core::ControlEvent event[1];
      if (midiInput_.translate(in, event, 1) == 1 && !engine_.enqueueEventFromAudioThread(event[0], 0))
        released = false;
    });
    midiLights_.reset();
    engine_.pitchBendFromAudioThread(0.0);  // a bend held on the old input springs back
    if (!released) {
      lunar24::core::ControlEvent event{};
      event.kind = lunar24::core::ControlEventKind::reset;
      event.value = 1;
      event.source = 3;
      engine_.enqueueEventFromAudioThread(event, 0);
    }
  }, [&](lunar24::host::MidiInputQueue::Message input) {
    IMidiMsg message;
    message.mStatus = input.status;
    message.mData1 = input.data1;
    message.mData2 = input.data2;
    message.mOffset = lunar24::host::midiBlockOffset(input.stamp, now, GetSampleRate(), frames);
    ProcessMidiMsg(message);
  });
}

void LunarHostPlugin::ProcessMidiMsg(const IMidiMsg& msg)
{
  using namespace lunar24::core;
  const lunar24::host::StandaloneAudioEngine::AudioScope scope(engine_);
  if (!scope) return;  // the machine is being swapped; its notes are gone anyway
  dropMidiLedgersAfterSwap_();
  const int offset = msg.mOffset > 0 ? msg.mOffset : 0;  // sample position inside the block
  auto sendEvent = [&](ControlEventKind kind) {
    ControlEvent e{};
    e.kind = kind;
    e.value = 1;
    e.source = 3;
    e.producerSequence = ++midiSeq_;
    engine_.enqueueEventFromAudioThread(e, offset);
  };

  // System real-time: MIDI clock steps the keyboard arpeggiator / sequencer; START restarts
  // the pattern from its first step. Transport never stops held notes (midi_timing.h).
  if (msg.mStatus >= 0xF8)
  {
    switch (msg.mStatus)  // counted for the audio.log diagnostics line
    {
      case 0xF8: midiClockTicksIn_.fetch_add(1, std::memory_order_relaxed); break;
      case 0xFA: midiStartsIn_.fetch_add(1, std::memory_order_relaxed); break;
      case 0xFB: midiContinuesIn_.fetch_add(1, std::memory_order_relaxed); break;
      case 0xFC: midiStopsIn_.fetch_add(1, std::memory_order_relaxed); break;
      default: break;
    }
    switch (midiClock_.onRealtime(msg.mStatus))
    {
      case lunar24::host::MidiClockFollower::Action::step: sendEvent(ControlEventKind::clock); break;
      case lunar24::host::MidiClockFollower::Action::restart: engine_.restartKeyboardPatternFromAudioThread(); break;
      default: break;
    }
    return;
  }
  if (msg.mStatus >= 0xF0) return;  // other system messages (MIDI time code, song position)

  PerformanceInput in{};
  in.source = 3;  // MIDI producer
  in.channel = static_cast<std::uint8_t>(msg.Channel());
  in.seq = ++midiSeq_;
  const int note = msg.NoteNumber();
  // Releases must reach notes/pedals admitted before the channel filter changed.
  const bool noteRelease = msg.StatusMsg() == IMidiMsg::kNoteOff ||
      (msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() == 0);
  const bool pedalRelease = msg.StatusMsg() == IMidiMsg::kControlChange &&
      msg.ControlChangeIdx() == IMidiMsg::kSustainOnOff && msg.mData2 < 64;
  const int channelFilter = midiChannelFilter_.load(std::memory_order_relaxed);
  if (channelFilter > 0 && msg.Channel() + 1 != channelFilter && !noteRelease && !pedalRelease) return;
  // Remember the last note/CC for the learn overlay (the overlay polls the seq).
  if ((msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0) ||
      (msg.StatusMsg() == IMidiMsg::kControlChange && msg.ControlChangeIdx() != IMidiMsg::kSustainOnOff)) {
    const bool isCc = msg.StatusMsg() == IMidiMsg::kControlChange;
    midiLastMessage_.store((static_cast<std::uint32_t>(msg.mData2 & 0x7F) << 21) |
                               (isCc ? 1u << 20 : 0u) |
                               ((static_cast<std::uint32_t>(in.channel + 1) & 0x1Fu) << 8) |
                               (static_cast<std::uint32_t>(isCc ? msg.ControlChangeIdx()
                                                                : (note & 127)) & 0xFFu),
                           std::memory_order_relaxed);
    midiMessageSeq_.fetch_add(1, std::memory_order_relaxed);
  }
  const int octaveShift = midiOctaveShift_.load(std::memory_order_relaxed);
  const auto velocityCurve =
      static_cast<lunar24::core::MidiVelocityCurve>(midiVelocityCurve_.load(std::memory_order_relaxed));
  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
      if (msg.Velocity() > 0)
      {
        // A bound note is a pad command, never a played note (bindings are 1-based channels).
        const int bound = engine_.midiBindingRow(static_cast<std::uint8_t>(in.channel + 1),
                                                 lunar24::core::MidiBindingKind::note,
                                                 static_cast<std::uint8_t>(note & 127));
        if (bound >= 0)
        {
          engine_.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(bound),
                                                  msg.Velocity(), static_cast<std::uint8_t>(in.channel + 1));
          return;
        }
        midiNotes_.played(in.channel, note & 127);
        midiLights_.on(in.channel, note & 127, note + octaveShift);  // light the plate by note name
        sustain_.noteOn(in.channel, note & 127);  // pressed again: no longer held only by the pedal
        in.kind = PerfInputKind::note_on;
        // TWIN / SPLIT: the played key (before TRANSPOSE) picks the side.
        in.side = midiSides_.noteOn(in.channel, note & 127, midiSplitNote_.load(std::memory_order_relaxed));
        // A3 (MIDI 57) = 0 V = 220 Hz; the rig's octave shift transposes the MIDI input.
        in.pitch = static_cast<SignalSample>((note - 57 + octaveShift) / 12.0);
        in.value = static_cast<SignalSample>(
            lunar24::core::midi_velocity_shape(velocityCurve, msg.Velocity() / 127.0));
        in.noteId = static_cast<NoteId>(note + 1);
        break;
      }
      [[fallthrough]];  // note-on with velocity 0 is a note-off
    case IMidiMsg::kNoteOff:
      // Learn may have bound this note since it started playing. Release follows
      // the note-on's ownership, never the current map (or current filter).
      if (!midiNotes_.release(in.channel, note & 127))
      {
        // Not a played note: maybe a pad holding a photo sensor's hand down.
        (void)engine_.photoPadRelease(static_cast<std::uint8_t>(in.channel + 1), static_cast<std::uint8_t>(note & 127));
        return;
      }
      midiLights_.off(in.channel, note & 127);
      if (sustain_.deferNoteOff(in.channel, note & 127))
      {
        return;
      }
      in.kind = PerfInputKind::note_off;
      in.noteId = static_cast<NoteId>(note + 1);
      in.side = midiSides_.of(in.channel, note & 127);
      break;
    case IMidiMsg::kPolyAftertouch:
      if (engine_.photoPadPressure(static_cast<std::uint8_t>(in.channel + 1), note & 127,
                                   msg.PolyAfterTouch() / 127.0))
        return;  // a photo pad's pressure moves its hand, not the keyboard
      in.kind = PerfInputKind::aftertouch;
      in.value = static_cast<SignalSample>(msg.PolyAfterTouch() / 127.0);
      in.noteId = static_cast<NoteId>(note + 1);
      in.side = midiSides_.of(in.channel, note & 127);
      break;
    case IMidiMsg::kChannelAftertouch:
      if (engine_.photoPadPressure(static_cast<std::uint8_t>(in.channel + 1), -1,
                                   msg.ChannelAfterTouch() / 127.0))
        return;
      in.kind = PerfInputKind::aftertouch;
      in.side = midiSides_.latest(in.channel);
      in.value = static_cast<SignalSample>(msg.ChannelAfterTouch() / 127.0);
      break;
    case IMidiMsg::kPitchWheel:
      engine_.pitchBendFromAudioThread(kPitchBendSemitones * msg.PitchWheel());
      return;
    case IMidiMsg::kControlChange:
      if (msg.ControlChangeIdx() == IMidiMsg::kSustainOnOff)
      {
        const bool on = msg.ControlChange(IMidiMsg::kSustainOnOff) >= 0.5;
        sustain_.pedal(in.channel, on, in.source, [&](PerformanceInput released) {
          released.seq = ++midiSeq_;
          released.side = midiSides_.of(released.channel, released.noteId - 1);
          ControlEvent event[1];
          if (midiInput_.translate(released, event, 1) == 1)
            engine_.enqueueEventFromAudioThread(event[0], offset);
        });
        return;
      }
      // A user binding wins over the factory CC table (the map is the user's rig).
      {
        const int bound = engine_.midiBindingRow(static_cast<std::uint8_t>(in.channel + 1),
                                                 lunar24::core::MidiBindingKind::cc,
                                                 static_cast<std::uint8_t>(msg.ControlChangeIdx()));
        if (bound >= 0)
        {
          engine_.applyMidiBindingFromAudioThread(static_cast<std::uint32_t>(bound), msg.mData2);
          return;
        }
      }
      in.kind = PerfInputKind::cc;
      in.controller = static_cast<std::uint16_t>(msg.ControlChangeIdx());
      in.value = static_cast<SignalSample>(msg.ControlChange(msg.ControlChangeIdx()));
      break;
    default:
      return;
  }
  ControlEvent ev[3];
  const std::uint32_t n = midiInput_.translate(in, ev, 3);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (ev[i].kind == ControlEventKind::parameter) {
      // CC -> an existing panel knob: the 0..1 controller turns the knob (its taper decides the
      // value), play it now and let the UI thread record it (so the knob moves and is saved).
      const ParameterDescriptor* d = find_parameter(ev[i].parameter);
      if (d != nullptr)
        engine_.parameterFromAudioThread(ev[i].parameter, knob_to_value(*d, double(ev[i].value)));
    } else {
      engine_.enqueueEventFromAudioThread(ev[i], offset);
    }
  }
}

#endif
