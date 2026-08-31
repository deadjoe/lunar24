// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0 AND Zlib
//
// Composite license: the Lunar 24 modifications are Apache-2.0; the retained upstream iPlug 2 body
// (see its zlib LICENSE.txt banner below) is Zlib. tools/check_core_headers.py requires this composite
// identifier — never the Apache-only form, which would misstate the upstream code's zlib terms.
//
// host/iPlug_app_host_override.cpp — a Lunar 24 FORK of the pinned third_party/iPlug2/IPlug/APP/
// IPlugAPP_host.cpp at submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4 (GH#4 8B3, task#73).
// The body is the upstream iPlug 2 library (its banner below is retained unchanged); the Lunar
// modifications are Apache-2.0 and live in InitAudio/AudioCallback, named below the banner.
/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

// ---------------------------------------------------------------------------
// Lunar 24 modification (GH#4 8B3, task#73).
// This is a repo-owned FORK of the pinned third_party/iPlug2/IPlug/APP/IPlugAPP_host.cpp at the
// submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4. The ONLY changes from upstream are:
//   * InitAudio() now negotiates the ACTUAL stream plan from the real device channel capability
//     (lunar24::host::negotiate_stream_plan) and installs that actual connected count via
//     LunarHostPlugin::setActualChannelPlan() BEFORE OnReset, opens only that many channels, and
//     fails-closed (installs 0-in/0-out + OnReset -> owner NOT-READY) on any negotiation/open/
//     start failure. The input/output pointer lists are cleared+rebuilt to the actual count each
//     open so a 2<->4 hot-swap can never accumulate stale pointers.
//   * AudioCallback() reads nins/nouts from the pointer-list sizes (the ACTUAL count), not from
//     MaxNChannels().
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

// Lunar 24 (task#73): the two include additions are the ONLY include-level change from upstream.
// plugin.h declares LunarHostPlugin::setActualChannelPlan() (the plugin is the one place the real
// host may drive the protected IPlugProcessor::SetChannelConnections); stream_plan.h is the shared
// pure negotiation the host + oracle both call.
#include "plugin.h"
#include <host/stream_plan.h>

using namespace iplug;
using namespace lunar24::host;

namespace {
// Lunar 24 (task#73): failure-invalidation. On ANY negotiation / device-disappear / open / start
// failure the host must make the runtime owner NOT-READY (never "no stream but engine ready").
// We install a 0-in/0-out actual plan and call the existing OnReset(); the engine's <2 output
// fail-path then clears to not-ready. Kept in its own anonymous-namespace helper so InitAudio's
// success path still has the mandate's exact single OnReset() ordering (CloseAudio -> ... ->
// setActualChannelPlan -> set rate/block -> OnReset -> open -> rebuild ptrs -> start -> publish).
void LunarInvalidateAudio(IPlugAPP* plug) {
  static_cast<LunarHostPlugin*>(plug)->setActualChannelPlan(0, 0);
  plug->OnReset();
}
}  // namespace

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 2048
#endif

#define STRBUFSZ 100

std::unique_ptr<IPlugAPPHost> IPlugAPPHost::sInstance;
UINT gSCROLLMSG;

IPlugAPPHost::IPlugAPPHost()
: mIPlug(MakePlug(InstanceInfo{this}))
{
}

IPlugAPPHost::~IPlugAPPHost()
{
  mExiting = true;
  
  CloseAudio();
  
  if (mMidiIn)
    mMidiIn->cancelCallback();

  if (mMidiOut)
    mMidiOut->closePort();
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
  InitMidi(); // creates RTMidiIn and RTMidiOut objects
  ProbeMidiIO(); // find out what midi IO devs are available and put their names in the global variables gMidiInputDevs / gMidiOutputDevs
  SelectMIDIDevice(ERoute::kInput, mState.mMidiInDev.Get());
  SelectMIDIDevice(ERoute::kOutput, mState.mMidiOutDev.Get());
  
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
      mState.mAudioOutChanL = GetPrivateProfileInt("audio", "out1", 1, mINIPath.Get()); // 1 is first audio output
      mState.mAudioOutChanR = GetPrivateProfileInt("audio", "out2", 2, mINIPath.Get());
      //mState.mAudioInIsMono = GetPrivateProfileInt("audio", "monoinput", 0, mINIPath.Get());

      mState.mBufferSize = GetPrivateProfileInt("audio", "buffer", 512, mINIPath.Get());
      mState.mAudioSR = GetPrivateProfileInt("audio", "sr", 44100, mINIPath.Get());

      //midi
      GetPrivateProfileString("midi", "indev", "no input", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiInDev.Set(buf);
      GetPrivateProfileString("midi", "outdev", "no output", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiOutDev.Set(buf);

      mState.mMidiInChan = GetPrivateProfileInt("midi", "inchan", 0, mINIPath.Get()); // 0 is any
      mState.mMidiOutChan = GetPrivateProfileInt("midi", "outchan", 0, mINIPath.Get()); // 1 is first chan
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

  sprintf(buf, "%u", mState.mAudioInChanL);
  WritePrivateProfileString("audio", "in1", buf, ini);
  sprintf(buf, "%u", mState.mAudioInChanR);
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

  WritePrivateProfileString("midi", "indev", mState.mMidiInDev.Get(), ini);
  WritePrivateProfileString("midi", "outdev", mState.mMidiOutDev.Get(), ini);

  sprintf(buf, "%u", mState.mMidiInChan);
  WritePrivateProfileString("midi", "inchan", buf, ini);
  sprintf(buf, "%u", mState.mMidiOutChan);
  WritePrivateProfileString("midi", "outchan", buf, ini);
}

std::string IPlugAPPHost::GetAudioDeviceName(uint32_t deviceID) const
{
  auto str = mDAC->getDeviceInfo(deviceID).name;
  std::size_t pos = str.find(':');

  if (pos != std::string::npos)
  {
    std::string subStr = str.substr(pos + 1);
    return subStr;
  }
  else
  {
    return str;
  }
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

void IPlugAPPHost::ProbeMidiIO()
{
  if (!mMidiIn || !mMidiOut)
    return;
  else
  {
    int nInputPorts = mMidiIn->getPortCount();

    mMidiInputDevNames.push_back(OFF_TEXT);

#ifdef OS_MAC
    mMidiInputDevNames.push_back("virtual input");
#endif

    for (int i=0; i<nInputPorts; i++)
    {
      mMidiInputDevNames.push_back(mMidiIn->getPortName(i));
    }

    int nOutputPorts = mMidiOut->getPortCount();

    mMidiOutputDevNames.push_back(OFF_TEXT);

#ifdef OS_MAC
    mMidiOutputDevNames.push_back("virtual output");
#endif

    for (int i=0; i<nOutputPorts; i++)
    {
      mMidiOutputDevNames.push_back(mMidiOut->getPortName(i));
      //This means the virtual output port wont be added as an input
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

  // Lunar 24 (task#73): the owner must allow a TRUE output-only open when the input is disabled.
  // inputSelected decides whether we resolve / fall back to an input device AT ALL; when it is
  // false the input ID stays the inert 0 and we never touch the input DeviceInfo/name. InitAudio
  // opens no input stream when openIn == 0 (it passes &iParams as null and reads deviceInputChans
  // as 0 because inputSelected is also false there), so the inert 0 is never dereferenced.
  const bool inputSelected = (mState.mAudioInChanL > 0 || mState.mAudioInChanR > 0);

  std::optional<uint32_t> inputID;
  if (inputSelected)
  {
#if defined OS_WIN
    // ASIO has one device, use the output for the input ID
    inputID = GetAudioDeviceID(mState.mAudioDriverType == kDeviceASIO ? mState.mAudioOutDev.Get() : mState.mAudioInDev.Get());
#elif defined OS_MAC
    inputID = GetAudioDeviceID(mState.mAudioInDev.Get());
#else
  #error NOT IMPLEMENTED
#endif
  }
  auto outputID = GetAudioDeviceID(mState.mAudioOutDev.Get());

  bool failedToFindDevice = false;
  bool resetToDefault = false;

  // Lunar 24 (task#73): fall back to a default ONLY for a SELECTED direction. When the input is off
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
    // Lunar 24 (task#73): a device this configuration NEEDS (the input when input is selected, or
    // ANY output) cannot be resolved / has disappeared. No stream can service the engine, so first
    // QUISCE (CloseAudio spins for the callback) then invalidate (install the 0-in/0-out plan +
    // OnReset) so the owner is NOT-READY — never a silently-running "no stream but engine ready".
    MessageBox(gHWND, "Please check the audio settings", "Error", MB_OK);
    CloseAudio();
    LunarInvalidateAudio(GetPlug());
    return false;
  }

  // Lunar 24 (task#73): output-only open keeps the inert inputID (0). InitAudio uses the channel
  // plan to decide the input stream: a 0-in VALID plan means the input is disabled, never a failure.
  if (!inputSelected)
  {
    return InitAudio(0, outputID.value(), mState.mAudioSR, mState.mBufferSize);
  }

  return InitAudio(inputID.value(), outputID.value(), mState.mAudioSR, mState.mBufferSize);
}

bool IPlugAPPHost::SelectMIDIDevice(ERoute direction, const char* pPortName)
{
  int port = GetMIDIPortNumber(direction, pPortName);

  if (direction == ERoute::kInput)
  {
    if (port == -1)
    {
      mState.mMidiInDev.Set(OFF_TEXT);
      UpdateINI();
      port = 0;
    }

    //TODO: send all notes off?
    if (mMidiIn)
    {
      mMidiIn->closePort();

      if (port == 0)
      {
        return true;
      }
  #if defined OS_WIN
      else
      {
        mMidiIn->openPort(port-1);
        return true;
      }
  #elif defined OS_MAC
      else if (port == 1)
      {
        std::string virtualMidiInputName = "To ";
        virtualMidiInputName += BUNDLE_NAME;
        mMidiIn->openVirtualPort(virtualMidiInputName);
        return true;
      }
      else
      {
        mMidiIn->openPort(port-2);
        return true;
      }
  #else
   #error NOT IMPLEMENTED
  #endif
    }
  }
  else
  {
    if (port == -1)
    {
      mState.mMidiOutDev.Set(OFF_TEXT);
      UpdateINI();
      port = 0;
    }
    
    if (mMidiOut)
    {
      //TODO: send all notes off?
      mMidiOut->closePort();
      
      if (port == 0)
        return true;
#if defined OS_WIN
      else
      {
        mMidiOut->openPort(port-1);
        return true;
      }
#elif defined OS_MAC
      else if (port == 1)
      {
        std::string virtualMidiOutputName = "From ";
        virtualMidiOutputName += BUNDLE_NAME;
        mMidiOut->openVirtualPort(virtualMidiOutputName);
        return true;
      }
      else
      {
        mMidiOut->openPort(port-2);
        return true;
      }
#else
  #error NOT IMPLEMENTED
#endif
    }
  }
  
  return false;
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

  // Lunar 24 (task#73): clear the per-open pointer lists so a 2<->4 hot-swap can NEVER accumulate
  // stale pointers. They are rebuilt (right after openStream succeeds) to the ACTUAL count for THIS
  // open, never to MaxNChannels(). No callback runs between here and startStream, so the empty
  // lists are only observable after the successful rebuild -- a 2->4 then 4->2 reopen would
  // otherwise grow the lists (stale-accumulation defect).
  mInputBufPtrs.Empty();
  mOutputBufPtrs.Empty();

  // Lunar 24 (task#73): negotiate the ACTUAL stream plan from the device's REAL channel capability
  // and the user's SELECTED channels (config.h now declares "2-4", the policy cap; a 2-out device
  // must open 2, never a forced 4). A non-Valid plan (non-contiguous / duplicate / out-of-range
  // input, or <2 openable outputs) fails-closed: no stream, engine NOT-READY. An output-only
  // device (input disabled) is a VALID 0-in plan, not a failure.
  const bool inputSelected = (mState.mAudioInChanL > 0 || mState.mAudioInChanR > 0);
  const int deviceInputChans = inputSelected ? mDAC->getDeviceInfo(inID).inputChannels : 0;
  const int deviceOutputChans = mDAC->getDeviceInfo(outID).outputChannels;
  const StreamPlan plan = negotiate_stream_plan(deviceInputChans, deviceOutputChans,
                                                static_cast<int>(mState.mAudioInChanL),
                                                static_cast<int>(mState.mAudioInChanR),
                                                static_cast<int>(mState.mAudioOutChanL),
                                                static_cast<int>(mState.mAudioOutChanR));
  if (plan.status != StreamPlanStatus::Valid) {
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

  // Lunar 24 (task#73): only resolve the input device name when input is actually selected. On an
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
  // Lunar 24 (task#73): install the ACTUAL connected count BEFORE OnReset, so the engine (which
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

  if (status != RtAudioErrorType::RTAUDIO_NO_ERROR)
  {
    // Lunar 24 (task#73): openStream failed -> no stream can service the engine; invalidate to
    // NOT-READY (never "no stream but engine ready").
    mDAC->closeStream();
    LunarInvalidateAudio(GetPlug());
    DBGMSG("%s", mDAC->getErrorText().c_str());
    return false;
  }

  // Lunar 24 (task#73, G8 callback-boundary): the callback chunks every nFrames into
  // APP_SIGNAL_VECTOR_SIZE (64) blocks and IPlugAPP::AppProcess always touches a full 64-vector.
  // If RtAudio rewrote mBufferSize to a value that is NOT a multiple of 64, the final partial block
  // would let AppProcess read/write 64 samples past a channel buffer's end (an OOB tail write). We
  // fail-closed on that: only a buffer size the callback can chunk EXACTLY is accepted, otherwise
  // quiesce + invalidate to NOT-READY (never a stream open with an unsafe tail block).
  if ((mBufferSize % APP_SIGNAL_VECTOR_SIZE) != 0) {
    mDAC->closeStream();
    LunarInvalidateAudio(GetPlug());
    DBGMSG("buffer size %u not a multiple of APP_SIGNAL_VECTOR_SIZE (%d); refusing unsafe open.\n",
           mBufferSize, static_cast<unsigned>(APP_SIGNAL_VECTOR_SIZE));
    return false;
  }

  for (int i = 0; i < iParams.nChannels; i++)
  {
    mInputBufPtrs.Add(nullptr); //will be set in callback
  }

  for (int i = 0; i < oParams.nChannels; i++)
  {
    mOutputBufPtrs.Add(nullptr); //will be set in callback
  }

  if (mDAC->startStream() != RTAUDIO_NO_ERROR)
  {
    DBGMSG("Error starting stream: %s\n", mDAC->getErrorText().c_str());
    // Lunar 24 (task#73): startStream failed -> the just-opened stream is closed; invalidate to
    // NOT-READY.
    mDAC->closeStream();
    LunarInvalidateAudio(GetPlug());
    return false;
  }

  mActiveState = mState;

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

  try
  {
    mMidiOut = std::make_unique<RtMidiOut>();
  }
  catch (RtMidiError &error)
  {
    mMidiOut = nullptr;
    error.printMessage();
    return false;
  }

  mMidiIn->setCallback(&MIDICallback, this);
  mMidiIn->ignoreTypes(false, true, false );

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

  // Lunar 24 (task#73): use the ACTUAL open counts (the pointer-list sizes rebuilt to the plan's
  // open count in InitAudio), NOT MaxNChannels() (the declared "2-4" cap). On a 2-out device
  // MaxNChannels=4 but only 2 buffers exist; iterating 4 would read OOB. This is the same count
  // the plugin installed via setActualChannelPlan (NChannelsConnected), so AppProcess attaches the
  // same number the callback populated -- no separate driftable max/count.
  int nins = _this->mInputBufPtrs.GetSize();
  int nouts = _this->mOutputBufPtrs.GetSize();
  
  double* pInputBufferD = static_cast<double*>(pInputBuffer);
  double* pOutputBufferD = static_cast<double*>(pOutputBuffer);

  bool startWait = _this->mVecWait >= APP_N_VECTOR_WAIT; // wait APP_N_VECTOR_WAIT * iovs before processing audio, to avoid clicks
  bool doFade = _this->mVecWait == APP_N_VECTOR_WAIT || _this->mAudioEnding;
  
  if (startWait && !_this->mAudioDone)
  {
    if (doFade)
      ApplyFades(pInputBufferD, nins, nFrames, _this->mAudioEnding);
    
    for (int i = 0; i < nFrames; i++)
    {
      _this->mBufIndex %= APP_SIGNAL_VECTOR_SIZE;

      if (_this->mBufIndex == 0)
      {
        for (int c = 0; c < nins; c++)
        {
          _this->mInputBufPtrs.Set(c, (pInputBufferD + (c * nFrames)) + i);
        }
        
        for (int c = 0; c < nouts; c++)
        {
          _this->mOutputBufPtrs.Set(c, (pOutputBufferD + (c * nFrames)) + i);
        }
        
        _this->mIPlug->AppProcess(_this->mInputBufPtrs.GetList(), _this->mOutputBufPtrs.GetList(), APP_SIGNAL_VECTOR_SIZE);

        _this->mSamplesElapsed += APP_SIGNAL_VECTOR_SIZE;
      }
      
      for (int c = 0; c < nouts; c++)
      {
        pOutputBufferD[c * nFrames + i] *= APP_MULT;
      }

      _this->mBufIndex++;
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
void IPlugAPPHost::MIDICallback(double deltatime, std::vector<uint8_t>* pMsg, void* pUserData)
{
  IPlugAPPHost* _this = (IPlugAPPHost*) pUserData;
  
  if (pMsg->size() == 0 || _this->mExiting)
    return;
  
  if (pMsg->size() > 3)
  {
    if (pMsg->size() > MAX_SYSEX_SIZE)
    {
      DBGMSG("SysEx message exceeds MAX_SYSEX_SIZE\n");
      return;
    }
    
    SysExData data { 0, static_cast<int>(pMsg->size()), pMsg->data() };
    
    _this->mIPlug->mSysExMsgsFromCallback.Push(data);
    return;
  }
  else if (pMsg->size())
  {
    IMidiMsg msg;
    msg.mStatus = pMsg->at(0);
    pMsg->size() > 1 ? msg.mData1 = pMsg->at(1) : msg.mData1 = 0;
    pMsg->size() > 2 ? msg.mData2 = pMsg->at(2) : msg.mData2 = 0;

    _this->mIPlug->mMidiMsgsFromCallback.Push(msg);
  }
}

// static
void IPlugAPPHost::ErrorCallback(RtAudioErrorType type, const std::string &errorText)
{
  std::cerr << "\nerrorCallback: " << errorText << "\n\n";
}

