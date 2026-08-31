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
  W8  The REAL lifecycle order comes from the pinned IPlug2 APP host (third_party/iPlug2/
      IPlug/APP/IPlugAPP_host.cpp, submodule @ d54f6905), NOT the repo-owned plugin. Within
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
      or moving it out of the ending-branch guard, must fail loudly. If the submodule is not
      checked out the order is UNPROVABLE, so this invariant fails rather than silently passing.

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
# The pinned IPlug2 APP host submodule. The repo-owned host cannot prove the real ordering; the
# order the mandate pins lives here. If the submodule is absent the order is unprovable -> FAIL.
APP_HOST = ROOT / "third_party" / "iPlug2" / "IPlug" / "APP" / "IPlugAPP_host.cpp"

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

# W8 — the REAL lifecycle order is in the pinned IPlug2 APP host submodule. It is NOT proven by the
# repo-owned host (W6 only shows the plugin doesn't open a stream). Pin the exact fixed dependency
# order the mandate demands: CloseAudio() done -> SetBlockSize/SetSampleRate -> OnReset() ->
# openStream -> startStream, and CloseAudio must itself wait for the callback (while(!mAudioDone)).
if not APP_HOST.exists():
    check("W8 submodule APP host present", False,
          f"pinned IPlug2 APP host not found at {APP_HOST}; lifecycle order is UNPROVABLE here")
else:
    app_host = APP_HOST.read_text()
    check("W8 submodule APP host present", True, f"read {APP_HOST.relative_to(ROOT)}")

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


def main() -> int:
    print("check_host_engine_wiring")
    if failures:
        print(f"  {len(failures)} wiring invariant(s) FAILED")
        return 1
    print("  all host wiring invariants PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
