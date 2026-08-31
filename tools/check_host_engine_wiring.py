#!/usr/bin/env python3
# Copyright (c) 2026 Lunar 24 contributors
# SPDX-License-Identifier: Apache-2.0
"""Structural wiring gate for the GH#4 host audio-engine owner (task#72).

The iPlug2 host is compiled only on the hosted OS runners (mac / Windows), not on every CI
tester, and ProcessBlock/OnReset are not reachable from a plain unit test (they are iPlug
virtuals). So CMake and the CTest cannot be the gate for "is the host actually wired". This
oracle statically verifies the SHAPE of the host->engine wiring that the CTest
(test_host_engine_oracle) and the hosted link otherwise prove:

  W1  LunarHostPlugin owns a `StandaloneAudioEngine engine_` BY VALUE (the owner member is
      present, in the private section).
  W2  plugin.h overrides BOTH OnReset() and ProcessBlock() under IPLUG_DSP.
  W3  ProcessBlock is a PURE delegate to engine_.processBlock(...) — it contains no host frame
      loop and no direct `outputs[]`/`inputs[]` array write (the "old passthrough/silence stub"
      and any host-side scale/map/reorder would reintroduce `for` / array subscripts here).
  W4  OnReset actually prepares the owner with the REAL device getters (GetSampleRate,
      GetBlockSize, the two connected-channel getters) and does NOT hardcode a sample rate
      (a literal `48000` in OnReset is exactly the "OnReset not prepare / fixed 48k" defect).
  W5  The owner delegates the block to the task#71 DeviceAdapter::renderBlock — the ONE
      production render entry. (Sound correctness of that delegate is the behaviour oracle's
      job: grep alone cannot prove a frame loop, which is why this gate is a guardrail, not
      the primary detector.)
  W6  The fixed lifecycle ORDER is pinned — the repo-owned host must not itself open/start the
      audio stream (the app bootstrap does), so the OnReset prepare/candidate-swap is never
      pushed behind an already-running stream. (This alone does NOT prove the real APP host
      ordering — that is W8, which reads the pinned IPlug2 submodule.)
  W7  The ProcessBlock bridge casts sample** <-> double**; that relabeling is only valid under
      iPlug2's default `sample = double`. The bridge must carry a compile-time
      static_assert(std::is_same_v<sample,double>) so a SAMPLE_TYPE_FLOAT build cannot silently
      compile with UB. (Removing the guard / toggling float must fail loudly.)
  W8  The REAL lifecycle order comes from the repo IPlugAPP_host override (host/
      iPlug_app_host_override.cpp — a fork of the pinned submodule IPlugAPP_host.cpp @ d54f6905
      plus the allowlisted hunks), NOT the repo-owned plugin and NOT the untouched submodule. Within
      InitAudio the statements must run in the mandate's fixed dependency order:
      CloseAudio() done -> SetBlockSize/SetSampleRate -> OnReset() -> openStream -> startStream;
      and CloseAudio() itself must arm the ending trigger (mAudioEnding = true) and spin on
      while(!mAudioDone) Sleep(...) BEFORE abortStream/closeStream. This is a permanent pin —
      reordering OnReset after open/start must fail loudly, as must a missing trigger (the
      callback would then never set mAudioDone and CloseAudio could spin forever). The OTHER half
      of that quiescence is the callback itself: CloseAudio only spins on the flag; the callback
      must actually WRITE mAudioDone=true inside `if (_this->mAudioEnding)` (in the running-audio
      startWait &&!mAudioDone branch) or CloseAudio waits forever even though its own trigger/wait
      order is correct. So W8 also pins the completion write inside AudioCallback — deleting it,
      or moving it out of the ending-branch guard, must fail loudly. If the override is absent the
      order is UNPROVABLE, so this invariant fails rather than silently passing.

  W9  GH#4 8B3 (task#73): the host opens the ACTUAL negotiated plan, not the declared "2-4" cap.
      InitAudio must negotiate_stream_plan() and install it via setActualChannelPlan() BEFORE
      OnReset (engine prepares the real count; AppProcess attaches the same count), open only
      plan.openIn/plan.openOut (never MaxNChannels — the OOB-on-2-out-device defect), clear the
      per-open pointer lists (a 2<->4 hot-swap must never accumulate stale pointers), and fail-closed
      to NOT-READY via the LunarInvalidateAudio helper (0-in/0-out + OnReset) on any negotiation /
      open / start failure — never "no stream but engine ready". AudioCallback drives AppProcess by
      the pointer-list sizes (GetSize(), the actual count), never MaxNChannels.
  W10 GH#4 8B3 (task#73) — the other half of the one-truth mandate. The repo APP-side override
      (iPlug_app_override.cpp) AppProcess() must NOT re-connect all declared MaxNChannels() every
      block; it attaches/processes by NChannelsConnected (the actual count the host installed before
      OnReset). W9 proves the host drives AppProcess by the actual count; W10 proves the app callback
      doesn't re-assert the declared max on that same path.

  W11 GH#4 8B3 G1 — PLUG_CHANNEL_IO is an EXACT set of legal I/O configs, not a max string. The
      APP_API branch must declare the six legal combos "0-2 1-2 2-2 0-4 1-4 2-4" (from which iPlug2
      takes max -> MaxNChannels 2-in/4-out), and the harness must expose is_legal_io() so the
      parsed-config product criterion is a real invariant. A regression back to the old exact-only
      "2-4" (or to a max-string) trips RED.

  W12 GH#4 8B3 G2/G8 — InitAudio must feed BOTH selected output channels into negotiate_stream_plan
      (the output selection consumes R, so L=1,R=3 is rejected, not silently opened) and must
      fail-closed on a post-open buffer size that is not a multiple of APP_SIGNAL_VECTOR_SIZE (the
      callback chunks nFrames into 64-blocks; a non-multiple tail would read/write OOB). It must
      still open the plan count (not MaxNChannels) and CHECK the setActualChannelPlan admission bool.

  W13 GH#4 8B3 G3 — TryToChangeAudio must allow a TRUE output-only open when the input is disabled
      (never resolve / fall back to an input device, never touch its DeviceInfo/name; the inert 0
      input ID is passed and InitAudio opens no input stream), and any device-resolve / disappear
      failure must QUISCE (CloseAudio) + INVALIDATE (LunarInvalidateAudio -> 0/0 + OnReset) so the
      owner is NOT-READY. The old hard `if (inputID && outputID)` gate must be gone.

  W14 GH#4 8B3 G4 — setActualChannelPlan must be a FAIL-CLOSED admission returning bool: only
      in{0,1,2} x out{0,2,4} and <= the declared max; an ill-formed plan installs 0/0 and returns
      false (never a silent clamp/truncate), and the host MUST check the return.

Each invariant is named and reported; a violation exits nonzero. The 8B2 mandate §4/b says a
behaviour detector comes FIRST (the CTest) and this structural gate is the permanent second
line. It is deliberately narrow: it flags the high-level wiring shape, not DSP semantics.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN_H = (ROOT / "host" / "plugin.h").read_text()
PLUGIN_CPP = (ROOT / "host" / "plugin.cpp").read_text()
ENGINE_H = (ROOT / "host" / "include" / "host" / "standalone_audio_engine.h").read_text()
# GH#4 8B3 (task#73): the REAL lifecycle order now lives in the REPO ALSO OVERRIDE
# host/iPlug_app_host_override.cpp — a fork of the pinned submodule IPlugAPP_host.cpp @ d54f6905
# plus the allowlisted hunks (InitAudio negotiation + failure-invalidation, AudioCallback actual
# count). That override is the TU the host actually compiles (host/CMakeLists.txt swaps the two
# upstream APP TUs for it), so the wiring gate MUST read the override, not the untouched submodule
# (which would verify a file that is no longer compiled). The pin relationship (override == upstream
# + hunks) is a separate gate: tools/check_host_override_drift.py.
APP_HOST = ROOT / "host" / "iPlug_app_host_override.cpp"
APP_OVR = ROOT / "host" / "iPlug_app_override.cpp"
CONFIG_H = ROOT / "host" / "config.h"
STREAM_PLAN_H = ROOT / "host" / "include" / "host" / "stream_plan.h"

failures = []


def check(name: str, ok: bool, detail: str = ""):
    status = "PASS" if ok else "FAIL"
    print(f"  {status}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        failures.append(name)


def body_of(text: str, sig_pattern: str) -> str:
    """Return the first `{...}` body following a function signature, best-effort.

    Works for the plugin's one-line-render / one-line-OnReset (no nested braces). Returns
    "" if the signature is not found or the body cannot be bounded cleanly.
    """
    m = re.search(sig_pattern, text)
    if not m:
        return ""
    start = text.find("{", m.start())
    if start < 0:
        return ""
    end = text.find("}", start)
    if end < 0:
        return ""
    return text[start:end]


def body_balanced(text: str, sig_pattern: str) -> str:
    """Return the brace-matched `{...}` body following a function signature.

    Handles nested braces (a real host function has `if`/`for` blocks), unlike body_of.
    Returns "" if the signature or an unbalanced body is not found.
    """
    m = re.search(sig_pattern, text)
    if not m:
        return ""
    start = text.find("{", m.start())
    if start < 0:
        return ""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    return ""


def has(text: str, needle: str) -> bool:
    return needle in text


def strip_comments(text: str) -> str:
    """Remove C/C++ line and block comments.

    Used only for the "never references MaxNChannels" negative checks, where a comment that merely
    MENTIONS MaxNChannels() would otherwise trip the guard. Naive, but the two host bodies inspected
    have no `//` inside a string literal (their DBGMSG format strings are `%s`-only), so comment
    removal here is exact.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


# W1 — the owner member is present, by value, in the plugin header.
check("W1 engine_ member present", has(PLUGIN_H, "StandaloneAudioEngine engine_"),
      "plugin.h must hold the owner by value")

# W2 — both iPlug overrides are declared.
check("W2 ProcessBlock override", has(PLUGIN_H, "ProcessBlock("),
      "plugin.h must declare ProcessBlock")
check("W2 OnReset override", has(PLUGIN_H, "OnReset()"), "plugin.h must declare OnReset()")

# W3 — ProcessBlock is a pure delegate: no host frame loop, no direct array write.
proc = body_of(PLUGIN_CPP, r"void LunarHostPlugin::ProcessBlock\s*\([^)]*\)")
check("W3 ProcessBlock delegates to engine_", has(proc, "engine_.processBlock("),
      "ProcessBlock must call engine_.processBlock(...)")
check("W3 no host frame loop", "for(" not in proc and "for (" not in proc,
      "a frame loop in ProcessBlock is the old passthrough/silence stub")
check("W3 no direct output write", "outputs[" not in proc and "sample(" not in proc,
      "no direct outputs[]/sample(...) write in ProcessBlock")

# W4 — OnReset prepares with the real device getters, never a hardcoded rate.
onreset = body_of(PLUGIN_CPP, r"void LunarHostPlugin::OnReset\s*\(\)")
check("W4 OnReset prepares engine_", has(onreset, "engine_.prepare("),
      "OnReset must call engine_.prepare(...)")
for getter in ("GetSampleRate()", "GetBlockSize()", "NInChansConnected()", "NOutChansConnected()"):
    check(f"W4 uses {getter}", has(onreset, getter),
          "OnReset must read the real device format, not a hardcoded value")
check("W4 no hardcoded sample rate", "48000" not in onreset,
      "OnReset must not fix the sample rate")

# W5 — the owner delegates the block to the production DeviceAdapter::renderBlock.
check("W5 owner delegates to DeviceAdapter::renderBlock", has(ENGINE_H, "adapter_.renderBlock("),
      "owner processBlock must call the task#71 DeviceAdapter::renderBlock")

# W6 — the repo-owned host must not itself open/start the stream (the app bootstrap does), so the
# OnReset prepare()/candidate-swap is never pushed behind an already-running stream.
check("W6 OnReset ordering not broken", "openStream(" not in PLUGIN_CPP and "startStream(" not in PLUGIN_CPP,
      "plugin.cpp must not open/start a stream (that is the app's job; OnReset runs first)")

# W7 — the ProcessBlock bridge relabels sample** <-> double**; that is only sound under the default
# `sample = double`. The bridge must carry a compile-time static_assert so a float build can never
# silently compile with UB. Removing the guard (or toggling SAMPLE_TYPE_FLOAT) fails loudly.
check("W7 sample==double static_assert present",
      has(PLUGIN_CPP, "static_assert(std::is_same_v<sample, double>"),
      "the ProcessBlock bridge must hard-gate sample==double at compile time")

# W11 — GH#4 8B3 G1: PLUG_CHANNEL_IO is an EXACT set of legal I/O configs, not a max string. The APP
# branch must declare the six legal combos (from which the 2-in/4-out max is DERIVED), and the parser
# gate (is_legal_io) that ties a negotiated plan back to that declared set must exist in stream_plan.h.
config_h = CONFIG_H.read_text()
stream_plan_h = STREAM_PLAN_H.read_text()
check("W11 config.h declares the six legal APP configs",
      '#define PLUG_CHANNEL_IO "0-2 1-2 2-2 0-4 1-4 2-4"' in config_h,
      "the APP branch must list the exact 2/4 legal combos (not a single exact-only string)")
check("W11 config.h no longer uses the exact-only '2-4'",
      '#define PLUG_CHANNEL_IO "2-4"' not in config_h,
      "PLUG_CHANNEL_IO '2-4' was an EXACT config, not a max/policy cap; the six-combo set is the truth")
check("W11 stream_plan.h exposes is_legal_io", "is_legal_io" in stream_plan_h,
      "stream_plan.h must define is_legal_io(openIn, openOut) as the parsed-config admission test")
check("W11 stream_plan.h declares negotiate_stream_plan(6 args)",
      re.search(r"negotiate_stream_plan\s*\(\s*int [a-zA-Z]+,", stream_plan_h) is not None,
      "the plan function must take deviceIn, deviceOut, selInL, selInR, selOutL, selOutR (consumes R)")

# W14 — GH#4 8B3 G4: setActualChannelPlan is a FAIL-CLOSED ADMISSION returning bool. Only in{0,1,2} x
# out{0,2,4} and never over the parsed max; an ill-formed plan installs the 0/0 sentinel and returns
# false. plugin.cpp must also check against MaxNChannels (the derived cap), not a hardcoded literal.
plan_cpp = body_balanced(PLUGIN_CPP, r"bool LunarHostPlugin::setActualChannelPlan\s*\([^)]*\)")
check("W14 setActualChannelPlan returns bool", "bool LunarHostPlugin::setActualChannelPlan" in PLUGIN_CPP,
      "setActualChannelPlan must return bool (fail-closed admission, G4)")
check("W14 admission checks in-domain input", "inCh == 2" in plan_cpp and "inCh <= maxIn" in plan_cpp,
      "input must be 0/1/2 AND <= parsed max, else reject")
check("W14 admission checks in-domain output", "outCh == 4" in plan_cpp and "outCh <= maxOut" in plan_cpp,
      "output must be 0/2/4 AND <= parsed max, else reject")
check("W14 ill-formed plan installs 0/0 sentinel + returns false",
      re.search(r"inCh = 0;\s*outCh = 0;.*?return false", plan_cpp, re.DOTALL) is not None,
      "a rejected plan must clear to 0-in/0-out (NOT-READY) and return false")
check("W14 admission uses MaxNChannels cap", "MaxNChannels(ERoute::kInput)" in plan_cpp
      and "MaxNChannels(ERoute::kOutput)" in plan_cpp,
      "the cap must be read from the parsed config (MaxNChannels), never a hardcoded literal")

# W8 — the REAL lifecycle order is in the repo IPlugAPP_host override (the TU the host compiles).
# It is NOT proven by the repo-owned plugin (W6 only shows the plugin doesn't open a stream). Pin
# the exact fixed dependency order the mandate demands: CloseAudio() done -> SetBlockSize/
# SetSampleRate -> OnReset() -> openStream -> startStream, and CloseAudio must itself wait for the
# callback (while(!mAudioDone)).
if not APP_HOST.exists():
    check("W8 APP host override present", False,
          f"repo IPlugAPP_host override not found at {APP_HOST}; lifecycle order is UNPROVABLE here")
else:
    app_host = APP_HOST.read_text()
    check("W8 APP host override present", True, f"read {APP_HOST.relative_to(ROOT)}")

    # The mandate's fixed dependency order, inside InitAudio:
    #   CloseAudio() done -> SetBlockSize/SetSampleRate -> OnReset() -> openStream -> startStream.
    init = body_balanced(app_host, r"bool IPlugAPPHost::InitAudio\s*\(")
    check("W8 finds InitAudio body", init != "", "IPlugAPP_host.cpp must define InitAudio()")
    order = [("CloseAudio()", init.find("CloseAudio();")),
             ("SetBlockSize", init.find("SetBlockSize(")),
             ("SetSampleRate", init.find("SetSampleRate(")),
             ("OnReset()", init.find("OnReset(")),
             ("openStream", init.find("openStream(")),
             ("startStream", init.find("startStream("))]
    missing = [name for name, pos in order if pos < 0]
    check("W8 lifecycle statements all present", not missing,
          "missing in InitAudio: " + (", ".join(missing) if missing else "none"))
    # Monotonic increasing positions = the exact mandated order.
    ok_order = all(0 <= order[i][1] < order[i + 1][1] for i in range(len(order) - 1))
    check("W8 fixed lifecycle order", ok_order,
          "must be CloseAudio -> SetBlockSize -> SetSampleRate -> OnReset -> openStream -> startStream")

    # CloseAudio's FULL callback-quiescence causal chain — not just co-presence of while/Sleep/
    # mAudioDone (the @Codex false-green: deleting `mAudioEnding = true` still passes co-presence,
    # but then the callback never enters the ending branch to set mAudioDone, so CloseAudio can
    # spin forever). Pin the trigger AND the strict order:
    #   mAudioEnding = true -> while(!mAudioDone){ Sleep } -> abortStream -> closeStream.
    close_body = body_balanced(app_host, r"void IPlugAPPHost::CloseAudio\s*\(")
    check("W8 CloseAudio body present", close_body != "",
          "IPlugAPP_host.cpp must define CloseAudio()")
    check("W8 CloseAudio sets mAudioEnding trigger",
          "mAudioEnding = true" in close_body,
          "must arm the ending trigger so the callback sets mAudioDone")
    check("W8 CloseAudio waits for callback",
          "while (!mAudioDone)" in close_body and "Sleep(" in close_body,
          "must spin on while(!mAudioDone) Sleep(...) before abort/close")
    close_order = [("mAudioEnding = true", close_body.find("mAudioEnding = true")),
                   ("while(!mAudioDone)", close_body.find("while (!mAudioDone)")),
                   ("abortStream", close_body.find("abortStream(")),
                   ("closeStream", close_body.find("closeStream("))]
    close_missing = [nm for nm, pos in close_order if pos < 0]
    check("W8 CloseAudio quiescence statements present", not close_missing,
          "missing in CloseAudio: " + (", ".join(close_missing) if close_missing else "none"))
    ok_close = all(0 <= close_order[i][1] < close_order[i + 1][1] for i in range(len(close_order) - 1))
    check("W8 CloseAudio quiescence order",
          ok_close,
          "must be mAudioEnding=true -> while(!mAudioDone){Sleep} -> abortStream -> closeStream")

    # W8 completion — the OTHER half of the quiescence. CloseAudio only spins on while(!mAudioDone);
    # the callback must actually WRITE that flag, or CloseAudio can wait forever even though its own
    # trigger/wait/order are correct (the @Codex false-green: deleting `_this->mAudioDone = true` in
    # AudioCallback left the gate green). Pin the completion write inside AudioCallback: it must be
    # the body of `if (_this->mAudioEnding)` in the running-audio startWait &&!mAudioDone branch.
    cb = body_balanced(app_host, r"int IPlugAPPHost::AudioCallback\s*\(")
    check("W8 AudioCallback body present", cb != "",
          "IPlugAPP_host.cpp must define AudioCallback()")
    check("W8 callback reads mAudioEnding", "_this->mAudioEnding" in cb,
          "the callback must read the ending flag to decide whether to complete")
    check("W8 callback writes mAudioDone=true", "_this->mAudioDone = true" in cb,
          "the callback must complete mAudioDone=true, or CloseAudio spins forever")
    runbody = body_balanced(cb, r"if \(startWait && !_this->mAudioDone\)")
    check("W8 completion write in running-audio branch",
          "_this->mAudioDone = true" in runbody,
          "mAudioDone=true must be inside the startWait && !mAudioDone branch, not the silent else")
    check("W8 completion write gated by ending flag",
          re.search(r"if \(_this->mAudioEnding\)\s*_this->mAudioDone", runbody) is not None,
          "mAudioDone=true must be the body of `if (_this->mAudioEnding)` (deleting or unguarding it fails)")

    # ----- GH#4 8B3 (task#73) wiring: the ACTUAL plan opens the stream, not the declared cap. ----
    # W9  The host override negotiates the REAL stream plan and installs it BEFORE OnReset. A host
    #     that opens MaxNChannels (the declared "2-4" cap) on a smaller device reads OOB; a host
    #     that connects after OnReset prepares the engine for the wrong count. These invariants
    #     catch: (a) reconnecting all max / NChannelsConnected prefix, (b) calling OnReset before
    #     setActualChannelPlan, (c) opening the declared cap instead of the negotiated plan.
    check("W9 override includes stream_plan.h", "#include <host/stream_plan.h>" in app_host,
          "the host override must include the shared plan header")
    check("W9 override includes plugin.h", '#include "plugin.h"' in app_host,
          "the host override must include plugin.h (setActualChannelPlan lives on the plugin)")
    check("W9 InitAudio negotiates the plan", "negotiate_stream_plan(" in init,
          "InitAudio must negotiate the actual plan from the device capability")
    init_code = strip_comments(init)
    check("W9 InitAudio opens the plan count, not the cap",
          "MaxNChannels(" not in init_code and "plan.openIn" in init and "plan.openOut" in init,
          "InitAudio must use plan.openIn/plan.openOut, never MaxNChannels (the declared cap)")
    pos_plan = init.find("setActualChannelPlan(")
    pos_reset = init.find("OnReset(")
    check("W9 actual connections installed before OnReset",
          0 <= pos_plan < pos_reset,
          "setActualChannelPlan(plan) must run before OnReset() so the engine prepares the real count")
    check("W9 pointer lists cleared each open",
          "mInputBufPtrs.Empty()" in init and "mOutputBufPtrs.Empty()" in init,
          "the per-open pointer lists must be cleared (2<->4 hot-swap must not accumulate stale ptrs)")
    # The failure-invalidation helper turns the owner NOT-READY (install 0/0 + OnReset) so there is
    # never "no stream but engine ready". Pin its existence AND that every failure path uses it.
    helper = body_balanced(app_host, r"void LunarInvalidateAudio\s*\(")
    check("W9 failure-invalidation helper present", "setActualChannelPlan(0, 0)" in helper and "OnReset()" in helper,
          "LunarInvalidateAudio must install a 0-in/0-out plan and call OnReset (owner NOT-READY)")
    check("W9 InitAudio uses the helper on failure", "LunarInvalidateAudio(GetPlug())" in init,
          "a negotiation/open/start failure must invalidate to NOT-READY")
    check("W9 openStream-failure closes + invalidates",
          re.search(r"if \(status != RtAudioErrorType::RTAUDIO_NO_ERROR\)", init) is not None
          and "mDAC->closeStream()" in init and "LunarInvalidateAudio(GetPlug())" in init,
          "a failed openStream must close the stream and invalidate (never 'stream open but engine ready')")
    # The callback must drive AppProcess by the ACTUAL count (pointer-list sizes), not MaxNChannels.
    cb_code = strip_comments(cb)
    check("W9 callback uses pointer-list sizes, not MaxNChannels",
          "mInputBufPtrs.GetSize()" in cb and "mOutputBufPtrs.GetSize()" in cb and "MaxNChannels(" not in cb_code,
          "AudioCallback must read the actual open count (the pointer-list size), never MaxNChannels")
    check("W9 callback AppProcess gets the raw pointer lists",
          "_this->mIPlug->AppProcess(" in cb,
          "AppProcess must receive the built pointer lists (attach/process by installed actual count)")

    # ----- GH#4 8B3 (task#73) G2/G8: output selection consumes R + fail-closed callback block. ----
    # W12  InitAudio must feed BOTH selected output channels into negotiate_stream_plan. The output
    # selection CONSUMES R (a non-contiguous / duplicate / out-of-range R is rejected, not silently
    # dropped), so the negotiated call must pass mAudioOutChanR alongside mAudioOutChanL. It must
    # also fail-closed on a post-open buffer size not a multiple of APP_SIGNAL_VECTOR_SIZE (the
    # callback chunks every nFrames into 64-blocks; a non-multiple tail would let AppProcess
    # read/write 64 samples past a channel buffer's end). And it must CHECK the setActualChannelPlan
    # admission bool (a false = an ill-formed plan must abort the open, not proceed).
    check("W12 InitAudio passes BOTH selected output channels to negotiate",
          "mAudioOutChanR" in init and "mAudioOutChanL" in init
          and re.search(r"negotiate_stream_plan\s*\(\s*deviceInputChans,\s*deviceOutputChans,",
                        init) is not None,
          "InitAudio must feed selectedOutL AND selectedOutR into negotiate_stream_plan (output "
          "selection consumes R, so L=1,R=3 is rejected, not silently opened)")
    check("W12 InitAudio fail-closes a non-multiple-of-64 block",
          "mBufferSize % APP_SIGNAL_VECTOR_SIZE" in init and "LunarInvalidateAudio(GetPlug())" in init,
          "InitAudio must refuse (close + invalidate) a post-open buffer size not a multiple of "
          "APP_SIGNAL_VECTOR_SIZE, or the 64-chunking callback tail reads/writes OOB")
    check("W12 InitAudio checks the setActualChannelPlan admission bool",
          "if (!static_cast<LunarHostPlugin*>(GetPlug())->setActualChannelPlan" in init,
          "InitAudio must CHECK the setActualChannelPlan bool and abort the open on false (G4)")

    # ----- GH#4 8B3 (task#73) G3: output-only open + device-disappear invalidation. --------------
    # W13  TryToChangeAudio (not just InitAudio) must allow a TRUE output-only open when the input is
    # disabled, and any device-resolve / disappear failure must QUISCE (CloseAudio) + INVALIDATE
    # (0/0 + OnReset). The old hard `if (inputID && outputID)` gate is removed, and the input is
    # never resolved/fallen back when off.
    tca = body_balanced(app_host, r"bool IPlugAPPHost::TryToChangeAudio\s*\(")
    check("W13 TryToChangeAudio body present", tca != "", "IPlugAPP_host.cpp must define TryToChangeAudio()")
    tca_code = strip_comments(tca)
    check("W13 TryToChangeAudio computes inputSelected",
          "const bool inputSelected =" in tca_code
          and "mAudioInChanL > 0" in tca_code and "mAudioInChanR > 0" in tca_code,
          "TryToChangeAudio must branch on whether the input is selected")
    check("W13 TryToChangeAudio allows output-only open",
          re.search(r"InitAudio\(0,\s*outputID\.value\(\)", tca_code) is not None,
          "input-off must open output-only (inert 0 input ID), never a forced input")
    check("W13 TryToChangeAudio no hard inputANDoutput gate",
          "if (inputID && outputID)" not in tca_code,
          "the old `if (inputID && outputID)` gate is gone (it blocked a true output-only open)")
    check("W13 TryToChangeAudio invalidates on device failure",
          "failedToFindDevice" in tca_code and "CloseAudio();" in tca_code
          and "LunarInvalidateAudio(GetPlug())" in tca_code,
          "a device-resolve/disappear failure must quiesce (CloseAudio) + invalidate (0/0 + OnReset)")

# W10  The app override's AppProcess() must NOT re-connect all declared MaxNChannels() every block
# (the other half of the "one truth" mandate). W9 proves the HOST installs the actual count and
# drives AppProcess by it; W10 proves the APP-side callback DOESN'T re-assert the declared max each
# block (which would re-connect 4 output pointers on a 2-out device and over-read the smaller
# callback buffers). The app TU is a second file the host compiles, so this invariant is checked
# against iPlug_app_override.cpp itself. The fork's banner comment legitimately NAMES
# SetChannelConnections (in prose), so the negative check runs on the comment-stripped body.
if not APP_OVR.exists():
    check("W10 app override present", False,
          f"repo iPlug_app_override.cpp not found at {APP_OVR}; AppProcess is UNPROVABLE here")
else:
    app_ovr = APP_OVR.read_text()
    check("W10 app override present", True, f"read {APP_OVR.relative_to(ROOT)}")
    app_proc = body_balanced(app_ovr, r"void IPlugAPP::AppProcess\s*\(")
    check("W10 AppProcess body present", app_proc != "", "IPlugAPP.cpp must define AppProcess()")
    app_proc_code = strip_comments(app_proc)
    check("W10 AppProcess does NOT re-connect channel max",
          "SetChannelConnections" not in app_proc_code,
          "AppProcess must attach/process by the installed actual count, never re-assert MaxNChannels")
    check("W10 AppProcess attaches by actual connected count",
          "NChannelsConnected(" in app_proc_code and "AttachBuffers(" in app_proc_code,
          "AppProcess must AttachBuffers by NChannelsConnected (the count setActualChannelPlan installed)")


def main() -> int:
    print("check_host_engine_wiring")
    if failures:
        print(f"  {len(failures)} wiring invariant(s) FAILED")
        return 1
    print("  all host wiring invariants PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
