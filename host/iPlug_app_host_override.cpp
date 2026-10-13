// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0 AND Zlib
//
// Composite license: the Lunar 24 modifications are Apache-2.0; the retained upstream iPlug 2 body
// (see its zlib LICENSE.txt banner below) is Zlib. tools/check_core_headers.py requires this composite
// identifier — never the Apache-only form, which would misstate the upstream code's zlib terms.
//
// host/iPlug_app_host_override.cpp — a Lunar 24 FORK of the pinned third_party/iPlug2/IPlug/APP/
// IPlugAPP_host.cpp at submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4.
// The body is the upstream iPlug 2 library (its banner below is retained unchanged); the Lunar
// modifications are Apache-2.0 and live in InitAudio/AudioCallback/TryToChangeAudio, named below.
/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

// ---------------------------------------------------------------------------
// Lunar 24 modification.
// This is a repo-owned FORK of the pinned third_party/iPlug2/IPlug/APP/IPlugAPP_host.cpp at the
// submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4. The ONLY changes from upstream are:
//   * InitAudio now negotiates the ACTUAL stream plan from the real device channel capability
//     (lunar24::host::negotiate_stream_plan) and installs that actual connected count via
//     LunarHostPlugin::setActualChannelPlan BEFORE OnReset, opens only that many channels, and
//     fails-closed (installs 0-in/0-out + OnReset -> owner NOT-READY) on any negotiation/open/
//     start failure. The input/output pointer lists are cleared+rebuilt to the actual count each
//     open so a 2<->4 hot-swap can never accumulate stale pointers.
//   * AudioCallback reads nins/nouts from the pointer-list sizes (the ACTUAL count), not from
//     MaxNChannels.
//   * TryToChangeAudio allows a TRUE output-only open (input off -> inert input id, never a
//     forced input, no hard `if (inputID && outputID)` gate) and on a device lookup/disappear
//     failure quiesces (CloseAudio) then invalidates to 0-in/0-out + OnReset (owner NOT-READY)
//     instead of only returning false.
// Everything else is byte-identical to the upstream pin. tools/check_host_override_drift.py
// asserts override == upstream + these allowlisted hunks, so the two files are never two
// divergent host truths.
// ---------------------------------------------------------------------------

#include "IPlugAPP_host.h"

#ifdef OS_WIN
#include <sys/stat.h>
#include "win32_utf8.h"
#endif

#include "IPlugLogger.h"

// Lunar 24: plugin.h declares LunarHostPlugin::setActualChannelPlan (the plugin is the
// one place the real host may drive the protected IPlugProcessor::SetChannelConnections);
// stream_plan.h is the shared pure negotiation the host + oracle both call. The standard headers
// below serve those and the diagnostics log.
#include "plugin.h"
#include "about.h"
#include <host/diag_log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <host/midi_timing.h>
#include <host/stream_plan.h>

using namespace iplug;
using namespace lunar24::host;

namespace {
// Lunar 24: failure-invalidation. On ANY negotiation / device-disappear / open / start
// failure the host must make the runtime owner NOT-READY (never "no stream but engine ready").
// We install a 0-in/0-out actual plan and call the existing OnReset; the engine's <2 output
// fail-path then clears to not-ready. Kept in its own anonymous-namespace helper so InitAudio's
// success path still has the mandate's exact single OnReset ordering (CloseAudio -> ... ->
// setActualChannelPlan -> set rate/block -> OnReset -> open -> rebuild ptrs -> start -> publish).
void LunarInvalidateAudio(IPlugAPP* plug) {
  static_cast<LunarHostPlugin*>(plug)->setActualChannelPlan(0, 0);
  plug->OnReset();
}

// Lunar 24: which devices to open at startup (settings.ini keys "followdefault", "inputon").
// Output follows the system's current output device (plug in headphones, connect AirPods: the
// sound goes there) unless the user picked another one in Preferences. The microphone stays
// closed unless the user switched input on: few patches use it, macOS asks for permission, and
// opening a Bluetooth headset's microphone drops its sound to call quality.
std::atomic<bool> sFollowDefaultOutput{true};
bool sInputOn = false;
// Lunar 24: the MIDI inputs (the MIDI panel's DEVICES tab; which ports are on is decided by the
// plugin's host::MidiDeviceSetup). Each input slot has its own RtMidiIn and callback context, so
// its messages land in its own queue (one producer each). mMidiIn only lists the ports; MIDI out
// is not used. The slots are opened and closed on the UI thread.
struct MidiSlot {
  std::unique_ptr<RtMidiIn> in;
  IPlugAPPHost* host = nullptr;
  unsigned index = 0;
};
MidiSlot sMidiSlots[lunar24::host::kMidiInputDevices];
// The app's own virtual input other apps can send to (macOS only).
#ifdef OS_MAC
const char* const kVirtualMidiInput = "To " BUNDLE_NAME;
#else
const char* const kVirtualMidiInput = "";
#endif
// The next ProbeMidiIO only applies the DEVICES choice (a port turned on or off).
bool sMidiApplyOnly = false;
// RtMidi on Windows numbers every port name ("MPK mini IV 1"); we keep the name without it.
#ifdef OS_WIN
constexpr bool kIndexedMidiPortNames = true;
#else
constexpr bool kIndexedMidiPortNames = false;
#endif
// The input channels the user chose. An open that fails with the input (e.g. the Mac microphone
// together with Bluetooth headphones) falls back to output only for that open; the choice is kept
// here and in the settings file, and the next automatic reopen (a device change, RESET PANEL,
// the next launch) tries the input again.
uint32_t sWantInL = 0, sWantInR = 0;
// Set when an open dropped the input to get any sound at all. The next Preferences Apply / OK
// then still sees the chosen channels, not the fallback's zeros (OK right after a failed Apply
// used to save "input off" as the user's choice).
bool sInputDroppedByFallback = false;
bool sStartupOpen = true;  // the first TryToChangeAudio is the app starting up
// Set when the stream died under us (a device went away or reconfigured itself, e.g. Bluetooth
// headphones) or the system output device changed while we follow it. The UI thread then
// reopens on the current device (lunar_host_audio_watchdog).
std::atomic<bool> sReopen{false};
// MIDI hot-plug (lunar_host_audio_watchdog asks ProbeMidiIO for a check once a second): the
// port names seen last time, and whether the next ProbeMidiIO is such a check. UI thread only.
bool sMidiHotplugCheck = false;
std::vector<std::string> sMidiInsSeen;
// When the last stream was opened (steady_clock ticks). Device-list notifications within 2 s of an
// open are ignored: opening a headset can itself reconfigure devices, and reacting would loop.
std::atomic<long long> sLastOpenTicks{0};

// The callback hands the engine fixed APP_SIGNAL_VECTOR_SIZE blocks through these staging
// buffers, so a device may use any buffer size (at the cost of one block of latency). Sized for
// the largest plan (2 in, 4 out); one host per process.
double sStageIn[2][APP_SIGNAL_VECTOR_SIZE];
double sStageOut[4][APP_SIGNAL_VECTOR_SIZE];
}  // namespace

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 2048
#endif

#define STRBUFSZ 100

std::unique_ptr<IPlugAPPHost> IPlugAPPHost::sInstance;

namespace {
// Lunar 24: device, stream and MIDI-port lines for the diagnostics log (host/diag_log.h). The
// background writer does the disk work; the plugin adds notes, levels, panel changes and
// snapshots to the same session file.
using diag::Cat;  // lunar24::host::diag, through the using-directive above
using diag::Level;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void HostLog(Cat cat, Level level, const char* fmt, ...)
{
  if (!diag::log().enabled(cat, level))
    return;
  char line[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  diag::log().write(cat, level, line);
}
// Buffer over- / underruns the device reported to the audio callback (it counts, the UI logs).
std::atomic<std::uint32_t> sInputOverflows{0}, sOutputUnderflows{0};
}  // namespace
UINT gSCROLLMSG;

IPlugAPPHost::IPlugAPPHost()
: mIPlug(MakePlug(InstanceInfo{this}))
{
  diag::log().attach();  // the plugin attached too; the last of the two to detach ends the session
}

IPlugAPPHost::~IPlugAPPHost()
{
  mExiting = true;
  
  CloseAudio();
  
  // the lifecycle exit save. CloseAudio has returned, so the audio callback is
  // quiesced; the plugin (mIPlug, declared first and therefore destroyed last) is still alive. The
  // call is a pure delegate to the narrow state store -- no file logic in this destructor body and
  // nothing in the audio callback. It writes only when a successful legal config exists, so a file
  // that was present but unusable is never overwritten.
  static_cast<LunarHostPlugin*>(GetPlug())->saveDeviceState();

  for (auto& slot : sMidiSlots)
    if (slot.in)
    {
      slot.in->cancelCallback();
      slot.in->closePort();
    }
  HostLog(Cat::app, Level::info, "app quitting");
  diag::log().detach();
}

//static
IPlugAPPHost* IPlugAPPHost::Create()
{
  sInstance = std::make_unique<IPlugAPPHost>();
  return sInstance.get();
}

bool IPlugAPPHost::Init()
{
  mIPlug->SetHost("standalone", mIPlug->GetPluginVersion(false));
    
  if (!InitState())
    return false;
  
  TryToChangeAudioDriverType(); // will init RTAudio with an API type based on gState->mAudioDriverType
  ProbeAudioIO(); // find out what audio IO devs are available and put their IDs in the global variables gAudioInputDevs / gAudioOutputDevs
  InitMidi(); // Lunar 24: the RtMidiIn that lists the ports
  // Lunar 24: the MIDI panel's DEVICES tab turns ports on and off: apply it and save the choice.
  static_cast<LunarHostPlugin*>(GetPlug())->applyMidiDevices = [this]() {
    sMidiApplyOnly = true;
    ProbeMidiIO();
    UpdateINI();
  };
  ProbeMidiIO(); // Lunar 24: lists the ports and opens the ones that are on (first use: the first one)
  
  mIPlug->OnParamReset(kReset);
  mIPlug->OnActivate(true);
  
  return true;
}

bool IPlugAPPHost::OpenWindow(HWND pParent)
{
  return mIPlug->OpenWindow(pParent) != nullptr;
}

void IPlugAPPHost::CloseWindow()
{
  mIPlug->CloseWindow();
}

bool IPlugAPPHost::InitState()
{
#if defined OS_WIN
  char strPath[MAX_PATH_LEN];
  SHGetSpecialFolderPathUTF8(NULL, strPath, MAX_PATH_LEN, CSIDL_LOCAL_APPDATA, FALSE);
  mINIPath.SetFormatted(MAX_PATH_LEN, "%s\\%s\\", strPath, BUNDLE_NAME);
#elif defined OS_MAC
  mINIPath.SetFormatted(MAX_PATH_LEN, "%s/Library/Application Support/%s/", getenv("HOME"), BUNDLE_NAME);
#else
  #error NOT IMPLEMENTED
#endif
  // The app used to be called Lunar24Host and kept its settings (panel state, presets, audio
  // device) in a folder of that name. On the first start under the new name, move that folder
  // over so nothing is lost.
  {
    std::string fresh(mINIPath.Get());
    fresh.pop_back();  // drop the trailing separator
    std::string old = fresh.substr(0, fresh.size() - std::strlen(BUNDLE_NAME)) + LUNAR_OLD_BUNDLE_NAME;
    struct stat stFresh, stOld;
    if (stat(fresh.c_str(), &stFresh) != 0 && stat(old.c_str(), &stOld) == 0)
    {
#if defined OS_WIN
      MoveFileUTF8(old.c_str(), fresh.c_str());  // the user name in the path may be non-ASCII
#else
      rename(old.c_str(), fresh.c_str());
#endif
    }
  }
  {
#if defined(OS_MAC)
    const char* os = "macOS";
#else
    const char* os = "Windows";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#else
    const char* arch = "x64";
#endif
    char header[256];
    std::snprintf(header, sizeof header, "Lunar 24 app %s, %s, %s %s", aboutVersion(), aboutBuild(), os, arch);
    diag::log().open(mINIPath.Get(), "app", header);
    // The single audio.log of earlier builds is replaced by the logs folder.
    for (const char* old : {"audio.log", "audio.log.1", "audio.log.2", "audio.log.3"})
    {
      std::error_code ec;
      if (std::filesystem::remove(std::filesystem::u8path(std::string(mINIPath.Get()) + old), ec))
        HostLog(Cat::log, Level::info, "removed the old %s (logs now live in the logs folder)", old);
    }
  }

  // hand the ALREADY-RESOLVED per-user settings directory to the plugin. This must
  // happen BEFORE any Append("settings.ini") below mutates mINIPath, and the plugin must never
  // re-derive it (this is the one resolution point the APP host owns). The store only records the
  // directory: a not-yet-created directory is fine (the read reports NoFile, never a silent
  // fallback to cwd or another directory).
  static_cast<LunarHostPlugin*>(GetPlug())->setStateDirectory(mINIPath.Get());

  struct stat st;

  if (stat(mINIPath.Get(), &st) == 0) // if directory exists
  {
    mINIPath.Append("settings.ini"); // add file name to path

    char buf[STRBUFSZ];
    
    if (stat(mINIPath.Get(), &st) == 0) // if settings file exists read values into state
    {
      DBGMSG("Reading ini file from %s\n", mINIPath.Get());
      
      mState.mAudioDriverType = GetPrivateProfileInt("audio", "driver", 0, mINIPath.Get());

      GetPrivateProfileString("audio", "indev", "Built-in Input", buf, STRBUFSZ, mINIPath.Get()); mState.mAudioInDev.Set(buf);
      GetPrivateProfileString("audio", "outdev", "Built-in Output", buf, STRBUFSZ, mINIPath.Get()); mState.mAudioOutDev.Set(buf);

      //audio
      mState.mAudioInChanL = GetPrivateProfileInt("audio", "in1", 1, mINIPath.Get()); // 1 is first audio input
      mState.mAudioInChanR = GetPrivateProfileInt("audio", "in2", 2, mINIPath.Get());
      sWantInL = mState.mAudioInChanL;
      sWantInR = mState.mAudioInChanR;
      mState.mAudioOutChanL = GetPrivateProfileInt("audio", "out1", 1, mINIPath.Get()); // 1 is first audio output
      mState.mAudioOutChanR = GetPrivateProfileInt("audio", "out2", 2, mINIPath.Get());
      // mState.mAudioInIsMono = GetPrivateProfileInt("audio", "monoinput", 0, mINIPath.Get);

      mState.mBufferSize = GetPrivateProfileInt("audio", "buffer", 512, mINIPath.Get());
      mState.mAudioSR = GetPrivateProfileInt("audio", "sr", 44100, mINIPath.Get());

      //midi
      GetPrivateProfileString("midi", "indev", "no input", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiInDev.Set(buf);
      // Lunar 24: the MIDI inputs that are on (DEVICES). Before them the app had one input,
      // "indev" (chosen in Preferences): a real port there is on now; "off" / none / never set
      // is first use (the old app also picked the first port then).
      std::vector<std::string> midiOn;
      bool midiChosen = GetPrivateProfileInt("midi", "devicesset", 0, mINIPath.Get()) != 0;
      if (midiChosen)
      {
        for (unsigned i = 0; i < lunar24::host::kMidiInputDevices; ++i)
        {
          const std::string key = "dev" + std::to_string(i + 1);
          GetPrivateProfileString("midi", key.c_str(), "", buf, STRBUFSZ, mINIPath.Get());
          if (buf[0]) midiOn.push_back(buf);
        }
      }
      else if (std::strcmp(buf, "no input") != 0 && std::strcmp(buf, OFF_TEXT) != 0)
      {
        midiOn.push_back(std::strcmp(buf, "virtual input") == 0
                             ? std::string(kVirtualMidiInput)
                             : lunar24::host::midi_legacy_port_identity(buf, kIndexedMidiPortNames));
        midiChosen = !midiOn.back().empty();
      }
      static_cast<LunarHostPlugin*>(GetPlug())->midiDevices().restore(midiChosen, midiOn);
      GetPrivateProfileString("midi", "outdev", "no output", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiOutDev.Set(buf);

      mState.mMidiInChan = GetPrivateProfileInt("midi", "inchan", 0, mINIPath.Get()); // 0 is any
      mState.mMidiOutChan = GetPrivateProfileInt("midi", "outchan", 0, mINIPath.Get()); // 1 is first chan

      sFollowDefaultOutput = GetPrivateProfileInt("audio", "followdefault", 1, mINIPath.Get()) != 0;
      sInputOn = GetPrivateProfileInt("audio", "inputon", 0, mINIPath.Get()) != 0;
    }

    // if settings file doesn't exist, populate with default values, otherwise overwrite
    UpdateINI();
  }
  else // folder doesn't exist - make folder and make file
  {
#if defined OS_WIN
    // folder doesn't exist - make folder and make file
    CreateDirectory(mINIPath.Get(), NULL);
    mINIPath.Append("settings.ini");
    UpdateINI(); // will write file if doesn't exist
#elif defined OS_MAC
    mode_t process_mask = umask(0);
    int result_code = mkdir(mINIPath.Get(), S_IRWXU | S_IRWXG | S_IRWXO);
    umask(process_mask);

    if (!result_code)
    {
      mINIPath.Append("settings.ini");
      UpdateINI(); // will write file if doesn't exist
    }
    else
    {
      return false;
    }
#else
  #error NOT IMPLEMENTED
#endif
  }

  return true;
}

void IPlugAPPHost::UpdateINI()
{
  char buf[STRBUFSZ]; // temp buffer for writing integers to profile strings
  const char* ini = mINIPath.Get();

  sprintf(buf, "%u", mState.mAudioDriverType);
  WritePrivateProfileString("audio", "driver", buf, ini);

  WritePrivateProfileString("audio", "indev", mState.mAudioInDev.Get(), ini);
  WritePrivateProfileString("audio", "outdev", mState.mAudioOutDev.Get(), ini);

  sprintf(buf, "%u", sWantInL);  // the user's choice, not a fallback's
  WritePrivateProfileString("audio", "in1", buf, ini);
  sprintf(buf, "%u", sWantInR);
  WritePrivateProfileString("audio", "in2", buf, ini);
  sprintf(buf, "%u", mState.mAudioOutChanL);
  WritePrivateProfileString("audio", "out1", buf, ini);
  sprintf(buf, "%u", mState.mAudioOutChanR);
  WritePrivateProfileString("audio", "out2", buf, ini);
  //sprintf(buf, "%u", mState.mAudioInIsMono);
  //WritePrivateProfileString("audio", "monoinput", buf, ini);

  WDL_String str;
  str.SetFormatted(32, "%i", mState.mBufferSize);
  WritePrivateProfileString("audio", "buffer", str.Get(), ini);

  str.SetFormatted(32, "%i", mState.mAudioSR);
  WritePrivateProfileString("audio", "sr", str.Get(), ini);
  WritePrivateProfileString("audio", "followdefault", sFollowDefaultOutput ? "1" : "0", ini);
  WritePrivateProfileString("audio", "inputon", sInputOn ? "1" : "0", ini);

  {  // Lunar 24: the MIDI inputs that are on (DEVICES). "indev" keeps the first of them for an
     // older version of the app (which reads only that one).
    const auto& midiDevices = static_cast<LunarHostPlugin*>(GetPlug())->midiDevices();
    if (midiDevices.on().empty())
      mState.mMidiInDev.Set(midiDevices.configured() ? OFF_TEXT : "no input");
    else
      mState.mMidiInDev.Set(midiDevices.isVirtual(midiDevices.on().front()) ? "virtual input"
                                                                            : midiDevices.on().front().c_str());
    WritePrivateProfileString("midi", "indev", mState.mMidiInDev.Get(), ini);
    WritePrivateProfileString("midi", "devicesset", midiDevices.configured() ? "1" : "0", ini);
    for (unsigned i = 0; i < lunar24::host::kMidiInputDevices; ++i)
    {
      const std::string key = "dev" + std::to_string(i + 1);
      WritePrivateProfileString("midi", key.c_str(), i < midiDevices.on().size() ? midiDevices.on()[i].c_str() : "", ini);
    }
  }
  WritePrivateProfileString("midi", "outdev", mState.mMidiOutDev.Get(), ini);

  sprintf(buf, "%u", mState.mMidiInChan);
  WritePrivateProfileString("midi", "inchan", buf, ini);
  sprintf(buf, "%u", mState.mMidiOutChan);
  WritePrivateProfileString("midi", "outchan", buf, ini);
}

// Lunar 24: trimmed (lunar24::host::audio_device_display_name), so a name saved in settings.ini
// still matches its device after the file trims it on read.
std::string IPlugAPPHost::GetAudioDeviceName(uint32_t deviceID) const
{
  return std::string(lunar24::host::audio_device_display_name(mDAC->getDeviceInfo(deviceID).name));
}

std::optional<uint32_t> IPlugAPPHost::GetAudioDeviceID(const char* deviceNameToTest) const
{
  auto deviceIDs = mDAC->getDeviceIds();

  for (auto deviceID : deviceIDs)
  {
    auto name = GetAudioDeviceName(deviceID);

    if (std::string_view(deviceNameToTest) == name)
    {
      return deviceID;
    }
  }
  
  return std::nullopt;
}

int IPlugAPPHost::GetMIDIPortNumber(ERoute direction, const char* nameToTest) const
{
  if (!mMidiIn || (direction != ERoute::kInput && !mMidiOut))
    return -1;
  int start = 1;
  
  auto nameStrView = std::string_view(nameToTest);
  
  if (direction == ERoute::kInput)
  {
    if (nameStrView == OFF_TEXT) return 0;
    
  #ifdef OS_MAC
    start = 2;
    if (nameStrView == "virtual input") return 1;
  #endif
    
    for (int i = 0; i < mMidiIn->getPortCount(); i++)
    {
      if (nameStrView == mMidiIn->getPortName(i).c_str())
        return (i + start);
    }
  }
  else
  {
    if (nameStrView == OFF_TEXT) return 0;
  
  #ifdef OS_MAC
    start = 2;
    if (nameStrView == "virtual output") return 1;
  #endif
  
    for (int i = 0; i < mMidiOut->getPortCount(); i++)
    {
      if (nameStrView == mMidiOut->getPortName(i).c_str())
        return (i + start);
    }
  }
  
  return -1;
}

void IPlugAPPHost::ProbeAudioIO()
{
  mAudioInputDevIDs.clear();
  mAudioOutputDevIDs.clear();

  if (!mDAC)
    return;

  DBGMSG("\nRtAudio Version %s", RtAudio::getVersion().c_str());

  RtAudio::DeviceInfo info;

  auto deviceIDs = mDAC->getDeviceIds();

  for (auto deviceID : deviceIDs)
  {
    info = mDAC->getDeviceInfo(deviceID);
    HostLog(Cat::audio, Level::info, "  device %u '%s': in %u, out %u%s%s", deviceID, info.name.c_str(), info.inputChannels,
             info.outputChannels, info.isDefaultInput ? ", default input" : "",
             info.isDefaultOutput ? ", default output" : "");

    if (info.inputChannels > 0)
    {
      mAudioInputDevIDs.push_back(deviceID);
    }
    
    if (info.outputChannels > 0)
    {
      mAudioOutputDevIDs.push_back(deviceID);
    }
    
    if (info.isDefaultInput)
    {
      mDefaultInputDev = deviceID;
    }
    
    if (info.isDefaultOutput)
    {
      mDefaultOutputDev = deviceID;
    }
  }
}

// Lunar 24: the MIDI inputs. Lists the ports (on a hot-plug check, sMidiHotplugCheck, nothing
// happens unless ports came or went), tells the plugin's MidiDeviceSetup, then applies its plan:
// slots whose port was turned off or unplugged close (their notes are released), ports that are
// on and plugged in open. With sMidiApplyOnly only the plan is applied (DEVICES changed).
void IPlugAPPHost::ProbeMidiIO()
{
  const bool hotplug = sMidiHotplugCheck;
  const bool applyOnly = sMidiApplyOnly;
  sMidiHotplugCheck = sMidiApplyOnly = false;
  if (!mMidiIn)
    return;
  auto* plug = static_cast<LunarHostPlugin*>(GetPlug());
  auto& devices = plug->midiDevices();
  if (!applyOnly)
  {
    std::vector<std::string> ins;
    for (unsigned int i = 0; i < mMidiIn->getPortCount(); i++)
      ins.push_back(lunar24::host::midi_port_identity(mMidiIn->getPortName(i), i, kIndexedMidiPortNames));
    if (hotplug && ins == sMidiInsSeen)
      return;
    auto has = [](const std::vector<std::string>& v, const std::string& name) {
      return std::find(v.begin(), v.end(), name) != v.end();
    };
    if (hotplug)
    {
      HostLog(Cat::midi, Level::info, "devices changed: %zu inputs", ins.size());
      for (const auto& name : ins)
        if (!has(sMidiInsSeen, name)) HostLog(Cat::midi, Level::info, "port connected: %s", name.c_str());
      for (const auto& name : sMidiInsSeen)
        if (!has(ins, name)) HostLog(Cat::midi, Level::info, "port disconnected: %s", name.c_str());
    }
    sMidiInsSeen = ins;
    mMidiInputDevNames = ins;
    if (devices.setPorts(ins, kVirtualMidiInput))
    {
      HostLog(Cat::midi, Level::info, "first use: listening to %s", devices.on().front().c_str());
      UpdateINI();
    }
  }
  for (const auto& change : devices.plan())
  {
    MidiSlot& slot = sMidiSlots[change.slot];
    if (!change.open)
    {
      // Stop the producer before invalidating its queued messages; audio releases that
      // input's notes on its next block.
      if (slot.in)
        slot.in->closePort();
      plug->midiInputClosed(change.slot);
      plug->setMidiInputDevice(change.slot, "");
      HostLog(Cat::midi, Level::info, "input %u closed: %s", change.slot + 1, change.name.c_str());
      continue;
    }
    const bool isVirtual = devices.isVirtual(change.name);
    // The name bindings match ("" for the virtual input: only bindings for any device).
    plug->setMidiInputDevice(change.slot, isVirtual ? "" : change.name.c_str());
    try
    {
      if (!slot.in)
      {
        slot.in = std::make_unique<RtMidiIn>();
        slot.host = this;
        slot.index = change.slot;
        slot.in->setCallback(&MIDICallback, &slot);
        // Let MIDI clock (0xF8) through (the arpeggiator / sequencer follows it,
        // midi_timing.h); SysEx is not used.
        slot.in->ignoreTypes(true, false, false);
      }
      bool opened = false;
      if (isVirtual)
      {
        slot.in->openVirtualPort(change.name);
        opened = true;
      }
      else
      {
        for (unsigned int i = 0; i < slot.in->getPortCount() && !opened; i++)
          if (lunar24::host::midi_port_identity(slot.in->getPortName(i), i, kIndexedMidiPortNames) == change.name)
          {
            slot.in->openPort(i, "Lunar 24");
            opened = slot.in->isPortOpen();
          }
      }
      if (!opened)
        throw RtMidiError("the port did not open", RtMidiError::DRIVER_ERROR);
      HostLog(Cat::midi, Level::info, "input %u open: %s", change.slot + 1, change.name.c_str());
    }
    catch (RtMidiError& error)
    {
      devices.openFailed(change.slot);
      plug->setMidiInputDevice(change.slot, "");
      HostLog(Cat::midi, Level::warn, "input %s did not open: %s", change.name.c_str(), error.getMessage().c_str());
    }
  }
}

bool IPlugAPPHost::AudioSettingsInStateAreEqual(AppState& os, AppState& ns)
{
  if (os.mAudioDriverType != ns.mAudioDriverType) return false;
  if (std::string_view(os.mAudioInDev.Get()) != ns.mAudioInDev.Get()) return false;
  if (std::string_view(os.mAudioOutDev.Get()) != ns.mAudioOutDev.Get()) return false;
  if (os.mAudioSR != ns.mAudioSR) return false;
  if (os.mBufferSize != ns.mBufferSize) return false;
  if (os.mAudioInChanL != ns.mAudioInChanL) return false;
  if (os.mAudioInChanR != ns.mAudioInChanR) return false;
  if (os.mAudioOutChanL != ns.mAudioOutChanL) return false;
  if (os.mAudioOutChanR != ns.mAudioOutChanR) return false;
//  if (os.mAudioInIsMono != ns.mAudioInIsMono) return false;

  return true;
}

bool IPlugAPPHost::MIDISettingsInStateAreEqual(AppState& os, AppState& ns)
{
  if (std::string_view(os.mMidiInDev.Get()) != ns.mMidiInDev.Get()) return false;
  if (std::string_view(os.mMidiOutDev.Get()) != ns.mMidiOutDev.Get()) return false;
  if (os.mMidiInChan != ns.mMidiInChan) return false;
  if (os.mMidiOutChan != ns.mMidiOutChan) return false;

  return true;
}

bool IPlugAPPHost::TryToChangeAudioDriverType()
{
  // Lunar 24: Preferences' Cancel calls this (then TryToChangeAudio) whenever its saved and
  // current states differ, MIDI settings included, and restores only the saved state. When the
  // driver and every audio setting equal the running stream's, tearing the stream down only
  // rebuilds the engine (dropping held notes and arpeggios) with an audible thump: keep it.
  if (mDAC && mDAC->isStreamRunning() && mState.mAudioDriverType == mActiveState.mAudioDriverType &&
      AudioSettingsInStateAreEqual(mActiveState, mState))
  {
    HostLog(Cat::audio, Level::info, "preferences: audio driver and settings unchanged, stream kept");
    return true;
  }
  CloseAudio();

  if (mDAC)
  {
    mDAC = nullptr;
  }

  // Skip RtAudio initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

#if defined OS_WIN
  if (mState.mAudioDriverType == kDeviceASIO)
    mDAC = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
  else if (mState.mAudioDriverType == kDeviceDS)
    mDAC = std::make_unique<RtAudio>(RtAudio::WINDOWS_DS);
#elif defined OS_MAC
  if (mState.mAudioDriverType == kDeviceCoreAudio)
    mDAC = std::make_unique<RtAudio>(RtAudio::MACOSX_CORE);
  //else
  //mDAC = std::make_unique<RtAudio>(RtAudio::UNIX_JACK);
#else
  #error NOT IMPLEMENTED
#endif

  if (mDAC)
  {
    mDAC->setErrorCallback(ErrorCallback);
    return true;
  }

  return false;
}

bool IPlugAPPHost::TryToChangeAudio()
{
  // Skip audio initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

  // Lunar 24: startup and automatic reopens pick the devices (see sFollowDefaultOutput /
  // sInputOn); any other call comes from the Preferences dialog and records the user's choice.
  const bool reopen = sReopen.exchange(false);
  if (reopen)
  {
    if (mExiting)
      return false;
    CloseAudio();
    ProbeAudioIO();  // devices may have come or gone
  }
  const bool automatic = sStartupOpen || reopen;
  const char* why = sStartupOpen ? "startup" : reopen ? "reopen" : "preferences";
  // Lunar 24: OK / Apply in Preferences call this whenever ANY setting changed. When the
  // audio settings equal the running stream's, keep that stream: reopening rebuilds the
  // engine (dropping held notes and arpeggios) and the restart is audible.
  if (!automatic && mDAC && mDAC->isStreamRunning() && AudioSettingsInStateAreEqual(mActiveState, mState))
  {
    mActiveState = mState;
    UpdateINI();
    HostLog(Cat::audio, Level::info, "preferences: audio unchanged, stream kept");
    return true;
  }
  if (automatic)
  {
    if (sFollowDefaultOutput && mDefaultOutputDev)
      mState.mAudioOutDev.Set(GetAudioDeviceName(mDefaultOutputDev.value()).c_str());
    if (sStartupOpen && !sInputOn)
    {
      mState.mAudioInChanL = 0;
      mState.mAudioInChanR = 0;
    }
    else if (sInputOn && mState.mAudioInChanL == 0 && mState.mAudioInChanR == 0)
    {
      mState.mAudioInChanL = sWantInL;  // an earlier open fell back to output only: retry the input
      mState.mAudioInChanR = sWantInR;
    }
    sStartupOpen = false;
  }
  else
  {
    if (sInputDroppedByFallback && mState.mAudioInChanL == 0 && mState.mAudioInChanR == 0)
    {
      mState.mAudioInChanL = sWantInL;
      mState.mAudioInChanR = sWantInR;
    }
    sFollowDefaultOutput = mDefaultOutputDev && GetAudioDeviceName(mDefaultOutputDev.value()) == mState.mAudioOutDev.Get();
    sInputOn = mState.mAudioInChanL > 0 || mState.mAudioInChanR > 0;
    sWantInL = mState.mAudioInChanL;
    sWantInR = mState.mAudioInChanR;
  }
  sInputDroppedByFallback = false;
  UpdateINI();
  HostLog(Cat::audio, Level::info, "open (%s): in '%s' ch %u/%u, out '%s', %u Hz, inputOn %d, followDefault %d", why,
           mState.mAudioInDev.Get(), mState.mAudioInChanL, mState.mAudioInChanR, mState.mAudioOutDev.Get(),
           mState.mAudioSR, sInputOn ? 1 : 0, sFollowDefaultOutput.load() ? 1 : 0);

  // Lunar 24: the owner must allow a TRUE output-only open when the input is disabled.
  // inputSelected decides whether we resolve / fall back to an input device AT ALL; when it is
  // false the input ID stays the inert 0 and we never touch the input DeviceInfo/name. InitAudio
  // opens no input stream when openIn == 0 (it passes &iParams as null and reads deviceInputChans
  // as 0 because inputSelected is also false there), so the inert 0 is never dereferenced.
  const bool inputSelected = (mState.mAudioInChanL > 0 || mState.mAudioInChanR > 0);

  // Lunar 24: look a device up by name AND direction. A Bluetooth headset can appear as two
  // devices with the same name, one with only inputs and one with only outputs (AirPods Pro do);
  // a name-only lookup returned the input half for the output and the open failed.
  auto findDevice = [this](const char* name, bool output) -> std::optional<uint32_t> {
    for (auto id : mDAC->getDeviceIds())
    {
      const RtAudio::DeviceInfo info = mDAC->getDeviceInfo(id);
      if ((output ? info.outputChannels : info.inputChannels) == 0)
        continue;
      if (std::string_view(name) == audio_device_display_name(info.name))
        return id;
    }
    return std::nullopt;
  };

  std::optional<uint32_t> inputID;
  if (inputSelected)
  {
#if defined OS_WIN
    // ASIO has one device, use the output for the input ID
    inputID = findDevice(mState.mAudioDriverType == kDeviceASIO ? mState.mAudioOutDev.Get() : mState.mAudioInDev.Get(), false);
#elif defined OS_MAC
    inputID = findDevice(mState.mAudioInDev.Get(), false);
#else
  #error NOT IMPLEMENTED
#endif
  }
  auto outputID = findDevice(mState.mAudioOutDev.Get(), true);

  bool failedToFindDevice = false;
  bool resetToDefault = false;

  // Lunar 24: fall back to a default ONLY for a SELECTED direction. When the input is off
  // we never fall back to a default input (that would force an input onto an output-only open).
  if (inputSelected && !inputID)
  {
    if (mDefaultInputDev)
    {
      resetToDefault = true;
      inputID = mDefaultInputDev;

      if (mAudioInputDevIDs.size())
        mState.mAudioInDev.Set(GetAudioDeviceName(inputID.value()).c_str());
    }
    else
      failedToFindDevice = true;
  }

  if (!outputID)
  {
    if (mDefaultOutputDev)
    {
      resetToDefault = true;
      outputID = mDefaultOutputDev;

      if (mAudioOutputDevIDs.size())
        mState.mAudioOutDev.Set(GetAudioDeviceName(outputID.value()).c_str());
    }
    else
      failedToFindDevice = true;
  }

  if (resetToDefault)
  {
    DBGMSG("Couldn't find previous audio device, reseting to default\n");
    UpdateINI();
  }

  if (failedToFindDevice)
  {
    // Lunar 24: a device this configuration NEEDS (the input when input is selected, or
    // ANY output) cannot be resolved / has disappeared. No stream can service the engine, so first
    // QUISCE (CloseAudio spins for the callback) then invalidate (install the 0-in/0-out plan +
    // OnReset) so the owner is NOT-READY — never a silently-running "no stream but engine ready".
    MessageBox(gHWND, "Please check the audio settings", "Error", MB_OK);
    CloseAudio();
    LunarInvalidateAudio(GetPlug());
    return false;
  }

  // Lunar 24: output-only open keeps the inert inputID (0). InitAudio uses the channel
  // plan to decide the input stream: a 0-in VALID plan means the input is disabled, never a failure.
  // Open a device at the rate it already runs at (forcing another rate makes some devices
  // reconfigure: Bluetooth headphones drop and reconnect, which kills the stream), but never
  // below music quality (choose_sample_rate). A rate chosen in Preferences holds for the session.
  if (automatic)
  {
    const RtAudio::DeviceInfo info = mDAC->getDeviceInfo(outputID.value());
    mState.mAudioSR = choose_sample_rate(info.currentSampleRate, info.preferredSampleRate, info.sampleRates.data(),
                                         info.sampleRates.size());
    UpdateINI();
  }

  if (InitAudio(inputSelected ? inputID.value() : 0, outputID.value(), mState.mAudioSR, mState.mBufferSize))
    return true;

  // Lunar 24: the chosen setup did not open (e.g. a Bluetooth headset refuses the saved sample
  // rate, or its microphone is busy). Rather than leave the instrument silent, try in turn:
  // without input, at the output device's own sample rate, then on the system output device.
  // What opens is recorded, so the Preferences dialog shows it.
  //
  // When that means running without the chosen input, say so once per device and session: a
  // silently closed microphone looked like a broken instrument (a Bluetooth headset's microphone,
  // e.g. AirPods, only runs at call quality and refuses a music sample rate).
  const std::string failedInput = mState.mAudioInDev.Get();
  const uint32_t failedRate = mState.mAudioSR;
  auto reportInputOff = [&]() {
    if (!inputSelected)
      return;
    static std::string sReported;
    if (sReported == failedInput)
      return;
    sReported = failedInput;
    HostLog(Cat::audio, Level::warn, "  input '%s' did not open: running without input", failedInput.c_str());
    WDL_String msg;
    msg.SetFormatted(1024,
                     "The input device \"%s\" could not be opened at %u Hz, so Lunar 24 is running "
                     "without an input (PREAMP and EXT.AUDIO hear nothing).\n\n"
                     "A Bluetooth headset's microphone often only works at call quality. "
                     "Choose another input in Preferences, such as the computer's built-in microphone.",
                     failedInput.c_str(), failedRate);
    MessageBox(gHWND, msg.Get(), "Audio input is off", MB_OK);
  };
  if (inputSelected)
  {
    mState.mAudioInChanL = 0;
    mState.mAudioInChanR = 0;
    sInputDroppedByFallback = true;
    if (InitAudio(0, outputID.value(), mState.mAudioSR, mState.mBufferSize))
    {
      UpdateINI();
      reportInputOff();
      return true;
    }
  }
  std::vector<uint32_t> outputs{outputID.value()};
  if (mDefaultOutputDev && mDefaultOutputDev.value() != outputID.value())
    outputs.push_back(mDefaultOutputDev.value());
  for (uint32_t out : outputs)
  {
    const RtAudio::DeviceInfo info = mDAC->getDeviceInfo(out);
    const uint32_t preferred = choose_sample_rate(info.currentSampleRate, info.preferredSampleRate,
                                                  info.sampleRates.data(), info.sampleRates.size());
    for (uint32_t sr : {mState.mAudioSR, preferred})
    {
      if (sr < kMinMusicSampleRate || (out == outputID.value() && sr == mState.mAudioSR))
        continue;  // the first attempt above already tried this one
      if (InitAudio(0, out, sr, mState.mBufferSize))
      {
        mState.mAudioOutDev.Set(GetAudioDeviceName(out).c_str());
        mState.mAudioSR = sr;
        UpdateINI();
        reportInputOff();
        return true;
      }
    }
  }
  // Last resort: the device's own rate even if low (a headset held in call mode by another app).
  // Rough sound beats none.
  const uint32_t lowRate = mDAC->getDeviceInfo(outputID.value()).currentSampleRate;
  if (lowRate > 0 && lowRate < kMinMusicSampleRate && InitAudio(0, outputID.value(), lowRate, mState.mBufferSize))
  {
    mState.mAudioSR = lowRate;
    UpdateINI();
    reportInputOff();
    return true;
  }
  return false;
}

// Lunar 24: MIDI ports are chosen in the MIDI panel's DEVICES tab (ProbeMidiIO applies them);
// Preferences has no MIDI section any more. Kept for the upstream dialog code, which still names it.
bool IPlugAPPHost::SelectMIDIDevice(ERoute, const char*)
{
  return true;
}

void IPlugAPPHost::CloseAudio()
{
  if (mDAC && mDAC->isStreamOpen())
  {
    if (mDAC->isStreamRunning())
    {
      mAudioEnding = true;
    
      while (!mAudioDone)
        Sleep(10);
      
      mDAC->abortStream();
    }
    
    mDAC->closeStream();
  }
}

bool IPlugAPPHost::InitAudio(uint32_t inID, uint32_t outID, uint32_t sr, uint32_t iovs)
{
  CloseAudio();

  // Lunar 24: clear the per-open pointer lists so a 2<->4 hot-swap can NEVER accumulate
  // stale pointers. They are rebuilt (right after openStream succeeds) to the ACTUAL count for THIS
  // open, never to MaxNChannels. No callback runs between here and startStream, so the empty
  // lists are only observable after the successful rebuild -- a 2->4 then 4->2 reopen would
  // otherwise grow the lists (stale-accumulation defect).
  mInputBufPtrs.Empty();
  mOutputBufPtrs.Empty();

  // Lunar 24: negotiate the ACTUAL stream plan from the device's REAL channel capability
  // and the user's SELECTED channels (config.h now declares "2-4", the policy cap; a 2-out device
  // must open 2, never a forced 4). A non-Valid plan (non-contiguous / duplicate / out-of-range
  // input, or <2 openable outputs) fails-closed: no stream, engine NOT-READY. An output-only
  // device (input disabled) is a VALID 0-in plan, not a failure.
  const bool inputSelected = (mState.mAudioInChanL > 0 || mState.mAudioInChanR > 0);
  const int deviceInputChans = inputSelected ? mDAC->getDeviceInfo(inID).inputChannels : 0;
  const int deviceOutputChans = mDAC->getDeviceInfo(outID).outputChannels;
  // A selection this device cannot serve (e.g. the default input pair on a mono MacBook
  // microphone) falls back to one it can, rather than leaving the instrument silent and dead.
  const ResolvedStreamPlan resolved = resolve_stream_plan(deviceInputChans, deviceOutputChans,
                                                          static_cast<int>(mState.mAudioInChanL),
                                                          static_cast<int>(mState.mAudioInChanR),
                                                          static_cast<int>(mState.mAudioOutChanL),
                                                          static_cast<int>(mState.mAudioOutChanR));
  const StreamPlan plan = resolved.plan;
  if (resolved.fellBack && plan.status == StreamPlanStatus::Valid) {
    mState.mAudioInChanL = static_cast<uint32_t>(resolved.inL);
    mState.mAudioInChanR = static_cast<uint32_t>(resolved.inR);
    mState.mAudioOutChanL = static_cast<uint32_t>(resolved.outL);
    mState.mAudioOutChanR = static_cast<uint32_t>(resolved.outR);
    UpdateINI();
  }
  if (plan.status != StreamPlanStatus::Valid) {
    HostLog(Cat::audio, Level::error, "  no valid channel plan (device in %d / out %d channels)", deviceInputChans, deviceOutputChans);
    LunarInvalidateAudio(GetPlug());
    return false;
  }

  RtAudio::StreamParameters iParams, oParams;
  iParams.deviceId = inID;
  iParams.nChannels = plan.openIn;
  iParams.firstChannel = plan.firstIn;

  oParams.deviceId = outID;
  oParams.nChannels = plan.openOut;
  oParams.firstChannel = plan.firstOut;

  mBufferSize = iovs; // mBufferSize may get changed by stream

  // Lunar 24: only resolve the input device name when input is actually selected. On an
  // output-only open (input off) the inert inID (0) is NOT a real device, so dereferencing it via
  // GetAudioDeviceName(inID) would be wrong; the input name stays a placeholder that is never used
  // by the RtAudio open (openIn==0 -> &iParams is passed as null).
  std::string inDevName = inputSelected ? GetAudioDeviceName(inID) : std::string("(input off)");
  DBGMSG("trying to start audio stream @ %i sr, buffer size %i\nindev = %s\noutdev = %s\ninputs = %i\noutputs = %i\n",
    sr, mBufferSize, inDevName.c_str(), GetAudioDeviceName(outID).c_str(), iParams.nChannels, oParams.nChannels);

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_NONINTERLEAVED;
  // options.streamName = BUNDLE_NAME; // JACK stream name, not used on other streams

  mBufIndex = 0;
  mSamplesElapsed = 0;
  mSampleRate = static_cast<double>(sr);
  mVecWait = 0;
  mAudioEnding = false;
  mAudioDone = false;

  mIPlug->SetBlockSize(APP_SIGNAL_VECTOR_SIZE);
  mIPlug->SetSampleRate(mSampleRate);
  // Lunar 24: install the ACTUAL connected count BEFORE OnReset, so the engine (which
  // reads NInChansConnected/NOutChansConnected in OnReset) prepares for the REAL plan, and
  // AppProcess attaches/processes by that same count. Zero means "no channel connected" -> the
  // engine's <2 output fail-path makes it NOT-READY (used by the failure-invalidation helper).
  //
  // setActualChannelPlan is a FAIL-CLOSED admission (G4): only in{0,1,2} x out{0,2,4} and never
  // over the declared max. The negotiated plan always satisfies that, but the host must still CHECK
  // the bool and abort the open on false rather than proceed with a truncated/ill-formed channel set.
  if (!static_cast<LunarHostPlugin*>(GetPlug())->setActualChannelPlan(plan.openIn, plan.openOut)) {
    LunarInvalidateAudio(GetPlug());
    return false;
  }
  mIPlug->OnReset();

  auto status = mDAC->openStream(&oParams, iParams.nChannels > 0 ? &iParams : nullptr, RTAUDIO_FLOAT64, sr, &mBufferSize, &AudioCallback, this, &options);
  HostLog(Cat::audio, status == RtAudioErrorType::RTAUDIO_NO_ERROR ? Level::info : Level::error,
          "  try in '%s' %d ch (from %d), out '%s' %d ch, %u Hz, buffer %u: %s", inDevName.c_str(),
           static_cast<int>(iParams.nChannels), static_cast<int>(iParams.firstChannel),
           GetAudioDeviceName(outID).c_str(), static_cast<int>(oParams.nChannels), sr, mBufferSize,
           status == RtAudioErrorType::RTAUDIO_NO_ERROR ? "opened" : mDAC->getErrorText().c_str());

  if (status != RtAudioErrorType::RTAUDIO_NO_ERROR)
  {
    // Lunar 24: openStream failed -> no stream can service the engine; invalidate to
    // NOT-READY (never "no stream but engine ready").
    mDAC->closeStream();
    LunarInvalidateAudio(GetPlug());
    DBGMSG("%s", mDAC->getErrorText().c_str());
    return false;
  }

  // Lunar 24: the callback feeds the engine through fixed-size staging blocks (sStageIn /
  // sStageOut), so any buffer size the device settles on is safe. No callback runs yet: clear them.
  std::memset(sStageIn, 0, sizeof sStageIn);
  std::memset(sStageOut, 0, sizeof sStageOut);

  for (int i = 0; i < iParams.nChannels; i++)
  {
    mInputBufPtrs.Add(sStageIn[i]);
  }

  for (int i = 0; i < oParams.nChannels; i++)
  {
    mOutputBufPtrs.Add(sStageOut[i]);
  }

  if (mDAC->startStream() != RTAUDIO_NO_ERROR)
  {
    HostLog(Cat::audio, Level::error, "  start failed: %s", mDAC->getErrorText().c_str());
    DBGMSG("Error starting stream: %s\n", mDAC->getErrorText().c_str());
    // Lunar 24: startStream failed -> the just-opened stream is closed; invalidate to
    // NOT-READY.
    mDAC->closeStream();
    LunarInvalidateAudio(GetPlug());
    return false;
  }

  mActiveState = mState;
  sLastOpenTicks = std::chrono::steady_clock::now().time_since_epoch().count();

  return true;
}

bool IPlugAPPHost::InitMidi()
{
  // Skip MIDI initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

  try
  {
    mMidiIn = std::make_unique<RtMidiIn>();
  }
  catch (RtMidiError &error)
  {
    mMidiIn = nullptr;
    error.printMessage();
    return false;
  }

  // Lunar 24: mMidiIn only lists the ports (each input slot opens its own, ProbeMidiIO); no
  // MIDI out (mMidiOut stays empty, so Preferences shows no MIDI section).
  return true;
}

void ApplyFades(double *pBuffer, int nChans, int nFrames, bool down)
{
  for (int i = 0; i < nChans; i++)
  {
    double *pIO = pBuffer + (i * nFrames);
    
    if (down)
    {
      for (int j = 0; j < nFrames; j++)
        pIO[j] *= ((double) (nFrames - (j + 1)) / (double) nFrames);
    }
    else
    {
      for (int j = 0; j < nFrames; j++)
        pIO[j] *= ((double) j / (double) nFrames);
    }
  }
}

// static
int IPlugAPPHost::AudioCallback(void* pOutputBuffer, void* pInputBuffer, uint32_t nFrames, double streamTime, RtAudioStreamStatus status, void* pUserData)
{
  IPlugAPPHost* _this = (IPlugAPPHost*) pUserData;

  // Lunar 24: use the ACTUAL open counts (the pointer-list sizes rebuilt to the plan's
  // open count in InitAudio), NOT MaxNChannels (the declared "2-4" cap). On a 2-out device
  // MaxNChannels=4 but only 2 buffers exist; iterating 4 would read OOB. This is the same count
  // the plugin installed via setActualChannelPlan (NChannelsConnected), so AppProcess attaches the
  // same number the callback populated -- no separate driftable max/count.
  int nins = _this->mInputBufPtrs.GetSize();
  int nouts = _this->mOutputBufPtrs.GetSize();
  
  double* pInputBufferD = static_cast<double*>(pInputBuffer);
  // Lunar 24: count the device's xruns for the log (lunar_host_audio_watchdog writes them).
  if (status & RTAUDIO_INPUT_OVERFLOW)
    sInputOverflows.fetch_add(1, std::memory_order_relaxed);
  if (status & RTAUDIO_OUTPUT_UNDERFLOW)
    sOutputUnderflows.fetch_add(1, std::memory_order_relaxed);
  double* pOutputBufferD = static_cast<double*>(pOutputBuffer);

  bool startWait = _this->mVecWait >= APP_N_VECTOR_WAIT; // wait APP_N_VECTOR_WAIT * iovs before processing audio, to avoid clicks
  bool doFade = _this->mVecWait == APP_N_VECTOR_WAIT || _this->mAudioEnding;
  
  if (startWait && !_this->mAudioDone)
  {
    if (doFade)
      ApplyFades(pInputBufferD, nins, nFrames, _this->mAudioEnding);
    
    // Lunar 24: stage the device buffer through fixed APP_SIGNAL_VECTOR_SIZE blocks, whatever
    // nFrames is. Each output sample comes from the previous full block (one block of latency).
    for (uint32_t i = 0; i < nFrames; i++)
    {
      const uint32_t pos = _this->mBufIndex;
      for (int c = 0; c < nins; c++)
      {
        sStageIn[c][pos] = pInputBufferD[c * nFrames + i];
      }
      for (int c = 0; c < nouts; c++)
        pOutputBufferD[c * nFrames + i] = sStageOut[c][pos] * APP_MULT;

      if (++_this->mBufIndex == APP_SIGNAL_VECTOR_SIZE)
      {
        _this->mIPlug->AppProcess(_this->mInputBufPtrs.GetList(), _this->mOutputBufPtrs.GetList(), APP_SIGNAL_VECTOR_SIZE);
        _this->mSamplesElapsed += APP_SIGNAL_VECTOR_SIZE;
        _this->mBufIndex = 0;
      }
    }
    

    if (doFade)
      ApplyFades(pOutputBufferD, nouts, nFrames, _this->mAudioEnding);
    
    if (_this->mAudioEnding)
      _this->mAudioDone = true;
  }
  else
  {
    memset(pOutputBufferD, 0, nFrames * nouts * sizeof(double));
  }
  
  _this->mVecWait = std::min(_this->mVecWait + 1, uint32_t(APP_N_VECTOR_WAIT + 1));

  return 0;
}

// static
// Lunar 24: one input slot's callback (pUserData is its MidiSlot): its messages go to its own
// queue. SysEx is ignored (several slots must not share the SysEx queue's one producer).
void IPlugAPPHost::MIDICallback(double, std::vector<uint8_t>* pMsg, void* pUserData)
{
  const MidiSlot* slot = static_cast<const MidiSlot*>(pUserData);
  IPlugAPPHost* _this = slot->host;
  
  if (pMsg->size() == 0 || pMsg->size() > 3 || _this == nullptr || _this->mExiting)
    return;
  
  if (pMsg->size())
  {
    IMidiMsg msg;
    msg.mStatus = pMsg->at(0);
    pMsg->size() > 1 ? msg.mData1 = pMsg->at(1) : msg.mData1 = 0;
    pMsg->size() > 2 ? msg.mData2 = pMsg->at(2) : msg.mData2 = 0;
    // Lunar 24: stamp the arrival time; AppProcess turns it into the note's position inside
    // the next audio block, so MIDI timing does not jitter by up to a whole buffer.
    msg.mOffset = lunar24::host::midiArrivalStamp();

    static_cast<LunarHostPlugin*>(_this->GetPlug())->queueMidiInput(slot->index, {
        msg.mOffset, msg.mStatus, msg.mData1, msg.mData2});
  }
}

// static
void IPlugAPPHost::ErrorCallback(RtAudioErrorType type, const std::string &errorText)
{
  std::cerr << "\nerrorCallback: " << errorText << "\n\n";
  HostLog(Cat::audio, type == RTAUDIO_WARNING ? Level::warn : Level::error, "RtAudio error %d: %s",
          static_cast<int>(type), errorText.c_str());
  // Lunar 24: the device went away or reconfigured and the stream was closed: reopen (UI thread).
  if (type == RTAUDIO_DEVICE_DISCONNECT)
    sReopen = true;
}

// Lunar 24: called from the plugin's OnIdle (UI thread): reopen the audio stream when it died or
// the followed system output device changed, and once a second look for MIDI devices that were
// plugged in or pulled out.
extern "C" void lunar_host_audio_watchdog()
{
  if (sReopen.load() && IPlugAPPHost::sInstance)
    IPlugAPPHost::sInstance->TryToChangeAudio();
  static auto lastMidiCheck = std::chrono::steady_clock::now();
  if (IPlugAPPHost::sInstance && std::chrono::steady_clock::now() - lastMidiCheck >= std::chrono::seconds(1))
  {
    lastMidiCheck = std::chrono::steady_clock::now();
    sMidiHotplugCheck = true;
    IPlugAPPHost::sInstance->ProbeMidiIO();
  }

  // At most once a second: buffer under- / overruns the device reported (heard as clicks).
  static auto lastXrunCheck = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - lastXrunCheck >= std::chrono::seconds(1))
  {
    lastXrunCheck = now;
    const std::uint32_t over = sInputOverflows.exchange(0), under = sOutputUnderflows.exchange(0);
    if (over || under)
      HostLog(Cat::audio, Level::warn, "device xruns in the last second: %u output underflow, %u input overflow",
              under, over);
  }
}

// Lunar 24: the plugin needs a stopped-stream boundary (the panel's factory reset).
extern "C" void lunar_host_request_audio_reopen()
{
  sReopen = true;
}

// Lunar 24: an audio device was added or removed (macOS listener in main.mm). Reopen, so the device
// list (and the Preferences dialog) sees it and a device that just came back is used again.
extern "C" void lunar_host_devices_changed()
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto twoSeconds = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(2)).count();
  if (now - sLastOpenTicks.load() > twoSeconds)
    sReopen = true;
}

// Lunar 24: the system output device changed (macOS listener in main.mm).
extern "C" void lunar_host_default_output_changed()
{
  if (sFollowDefaultOutput.load())
    sReopen = true;
}

