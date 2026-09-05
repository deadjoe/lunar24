// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0 AND Zlib
//
// Composite license: the Lunar 24 modifications are Apache-2.0; the retained upstream iPlug 2 body
// (see its zlib LICENSE.txt banner below) is Zlib. tools/check_core_headers.py requires this composite
// identifier — never the Apache-only form, which would misstate the upstream code's zlib terms.
//
// host/iPlug_app_override.cpp — a Lunar 24 FORK of the pinned third_party/iPlug2/IPlug/APP/
// IPlugAPP.cpp at submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4 (GH#4 8B3, task#73).
// The body is the upstream iPlug 2 library (its banner below is retained unchanged); the Lunar
// modifications are Apache-2.0. See the "Lunar 24 modification" comment below the banner for the
// exact diff. tools/check_host_override_drift.py asserts override == upstream + this allowlisted
// hunk, so this fork is never a second divergent host truth.
/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

// ---------------------------------------------------------------------------
// Lunar 24 modification (GH#4 8B3, task#73).
// This is a repo-owned FORK of the pinned third_party/iPlug2/IPlug/APP/IPlugAPP.cpp at the
// submodule pin d54f69050f517e43b941d88c2a170f0a840b9ee4. The ONLY change is inside
// IPlugAPP::AppProcess: it no longer re-connects all MaxNChannels() every block (a 2-out device
// would then re-assert the declared 4-channel max and the callback's smaller output buffers would
// be read out of bounds). It attaches/processes by the ACTUAL connected count the host installed
// via LunarHostPlugin::setActualChannelPlan() before OnReset. Everything else is byte-identical to
// the upstream pin. tools/check_host_override_drift.py asserts override == upstream + this
// allowlisted hunk, so these two files are never two divergent host truths.
// ---------------------------------------------------------------------------

#include "IPlugAPP.h"
#include "IPlugAPP_host.h"

#if defined OS_MAC || defined OS_LINUX
#include <IPlugSWELL.h>
#else
extern float GetScaleForHWND(HWND hWnd);
#endif

using namespace iplug;

extern HWND gHWND;

IPlugAPP::IPlugAPP(const InstanceInfo& info, const Config& config)
: IPlugAPIBase(config, kAPIAPP)
, IPlugProcessor(config, kAPIAPP)
{
  mAppHost = (IPlugAPPHost*) info.pAppHost;
  
  Trace(TRACELOC, "%s%s", config.pluginName, config.channelIOStr);

  SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), true);
  SetChannelConnections(ERoute::kOutput, 0, MaxNChannels(ERoute::kOutput), true);

  SetBlockSize(DEFAULT_BLOCK_SIZE);
  
  CreateTimer();
}

bool IPlugAPP::EditorResize(int viewWidth, int viewHeight)
{
  bool parentResized = false;
  
  if (viewWidth != GetEditorWidth() || viewHeight != GetEditorHeight())
  {
    #if defined OS_MAC || defined NO_IGRAPHICS 
    RECT rcClient, rcWindow;
    POINT ptDiff;
    
    GetClientRect(gHWND, &rcClient);
    GetWindowRect(gHWND, &rcWindow);
    
    ptDiff.x = (rcWindow.right - rcWindow.left) - rcClient.right;
    ptDiff.y = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;
    
    int flags = 0;
    
    #ifdef OS_WIN
    flags = SWP_NOMOVE;
    float ss = GetScaleForHWND(gHWND);
    #else
    float ss = 1.f;
    #endif
    
    SetWindowPos(gHWND, 0,
                 static_cast<LONG>(rcWindow.left * ss),
                 static_cast<LONG>((rcWindow.bottom - viewHeight - ptDiff.y) * ss),
                 static_cast<LONG>((viewWidth + ptDiff.x) * ss),
                 static_cast<LONG>((viewHeight + ptDiff.y) * ss), flags);
    parentResized = true;
    #endif
    
    SetEditorSize(viewWidth, viewHeight);
  }
  
  return parentResized;
}

bool IPlugAPP::SendMidiMsg(const IMidiMsg& msg)
{
  if (DoesMIDIOut() && mAppHost->mMidiOut)
  {
    //TODO: midi out channel
//    uint8_t status;
//
//    // if the midi channel out filter is set, reassign the status byte appropriately
//    if (mAppHost->mMidiOutChannel > -1)
//      status = mAppHost->mMidiOutChannel-1 | ((uint8_t) msg.StatusMsg() << 4) ;

    std::vector<uint8_t> message;
    message.push_back(msg.mStatus);
    message.push_back(msg.mData1);
    message.push_back(msg.mData2);

    mAppHost->mMidiOut->sendMessage(&message);
    
    return true;
  }

  return false;
}

bool IPlugAPP::SendSysEx(const ISysEx& msg)
{
  if (DoesMIDIOut() && mAppHost->mMidiOut)
  {
    //TODO: midi out channel
    std::vector<uint8_t> message;
    
    for (int i = 0; i < msg.mSize; i++)
    {
      message.push_back(msg.mData[i]);
    }
    
    mAppHost->mMidiOut->sendMessage(&message);
    return true;
  }
  
  return false;
}

void IPlugAPP::SendSysexMsgFromUI(const ISysEx& msg)
{
  SendSysEx(msg);
}

void IPlugAPP::AppProcess(double** inputs, double** outputs, int nFrames)
{
  // Lunar 24 (task#73): do NOT re-connect all MaxNChannels() every block. The host installs the
  // ACTUAL connected count (setActualChannelPlan) before OnReset, and AppProcess must attach/
  // process by that same count. Re-connecting the declared max here would re-assert 4 outputs on
  // a 2-out device and read the callback's (real-count-sized) output pointers out of bounds. This
  // is the ONLY change from the pinned upstream; the ctor still connects the declared max as the
  // pre-configuration default, which setActualChannelPlan replaces before any block runs.
  AttachBuffers(ERoute::kInput, 0, NChannelsConnected(ERoute::kInput), inputs, GetBlockSize());
  AttachBuffers(ERoute::kOutput, 0, NChannelsConnected(ERoute::kOutput), outputs, GetBlockSize());
  
  if (mMidiMsgsFromCallback.ElementsAvailable())
  {
    IMidiMsg msg;
    
    while (mMidiMsgsFromCallback.Pop(msg))
    {
      ProcessMidiMsg(msg);
      mMidiMsgsFromProcessor.Push(msg); // queue incoming MIDI for UI
    }
  }
  
  if (mSysExMsgsFromCallback.ElementsAvailable())
  {
    SysExData data;
    
    while (mSysExMsgsFromCallback.Pop(data))
    {
      ISysEx msg { data.mOffset, data.mData, data.mSize };
      ProcessSysEx(msg);
      mSysExDataFromProcessor.Push(data); // queue incoming Sysex for UI
    }
  }
  
  if (mMidiMsgsFromEditor.ElementsAvailable())
  {
    IMidiMsg msg;

    while (mMidiMsgsFromEditor.Pop(msg))
    {
      ProcessMidiMsg(msg);
    }
  }

  //Do not handle Sysex messages here - SendSysexMsgFromUI overridden

  ENTER_PARAMS_MUTEX
  ProcessBuffers(0.0, GetBlockSize());
  LEAVE_PARAMS_MUTEX
}
