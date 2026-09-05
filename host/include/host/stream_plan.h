// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// stream_plan.h — the one framework-free CHANNEL-SELECTION truth source (GH#4 8B3, task#73).
//
// The iPlug2 standalone host previously hard-wired the stream to MaxNChannels() (declared "1-2"),
// so the stream ALWAYS opened 1-in/2-out regardless of the device, and IPlugAPP::AppProcess()
// re-connected all max channels every block. To fix that without editing the pinned submodule we
// override TWO upstream TUs (IPlugAPP.cpp for AppProcess, IPlugAPP_host.cpp for InitAudio/
// AudioCallback) and give them ONE shared, deterministic answer for "how many input/output
// channels does THIS stream actually open, and starting where".
//
// This header is PURE (no heap, no std::string, no iPlug2 / RtAudio type, no allocation) so the
// exact same function runs inside the real host TUs AND in a framework-free oracle test
// (tests/host/test_host_stream_plan.cpp). That is the point: the host and the oracle cannot
// disagree because they call the same function.
//
// The plan encodes the GH#4 frozen policy (design/07 §5) at the *selection* level. The user selects
// the L and R of a MONO/STEREO INPUT anchor (inL/inR, 1-indexed, 0 = that side off) and the L and R
// of a STEREO OUTPUT anchor (outL/outR). The plan is VALID iff the selected channels are a real,
// contiguous, in-range set, and the openable run from the anchor satisfies the WET output strategy:
//
//   INPUT  — both off -> open 0 (output-only device / input disabled; VALID, never a failure).
//            one valid in-range L -> open 1 (mono; the owner routes ExtOnly).
//            two adjacent & in-range -> open 2 (owner routes Distinct; never an implicit copy).
//            non-contiguous / duplicate / out-of-range / R-only -> Invalid (no implicit copy).
//
//   OUTPUT — the WET strategy needs a stereo L/R pair that starts at outL and lies ON the device:
//            outR must be the adjacent channel (outR == outL+1) and both in range. A legal pair is
//            guaranteed at least 2 openable channels from outL (channels outL, outL+1), so:
//              openable run from outL >= 4  -> open 4  (WET L/R + DRY A/B)
//              openable run from outL 2..3  -> open 2  (honest WET-only; end-of-device "legal pair")
//              not a legal pair (off / duplicate / non-contiguous / out-of-range) -> Invalid.
//            So an 8-out device opened from an end legal pair yields 2, never a forced 4, and a
//            non-legal selection (L=1,R=3) is rejected rather than silently "succeeding" as 4.
//
// A non-Valid status is the signal that the host must NOT open a stream: it fails-closed and
// installs a 0-in/0-out plan so the owner becomes not-ready (see the host override).

#pragma once

#include <cstdint>

namespace lunar24::host {

enum class StreamPlanStatus : std::uint8_t {
  Valid = 0,
  InputInvalid,   // non-contiguous / duplicate / out-of-range / R-only input selection (no implicit copy).
  OutputInvalid,  // off / duplicate / non-contiguous / out-of-range output selection (no WET pair).
};

struct StreamPlan {
  int firstIn = 0;             // 0-indexed first input channel to open.
  int firstOut = 0;            // 0-indexed first output channel to open.
  int openIn = 0;              // 0 / 1 / 2.
  int openOut = 0;             // 0 / 2 / 4.
  StreamPlanStatus status = StreamPlanStatus::Valid;
};

// is_legal_io is a STREAM-POLICY invariant, NOT the parsed-config admission. The AUTHORITATIVE
// "is this (in,out) a plugin config" test is iPlug2's IPlugProcessor::LegalIO(in,out) (IPlugProcessor.h),
// which parses the APP's declared PLUG_CHANNEL_IO configs ("0-2 1-2 2-2 0-4 1-4 2-4" on the APP_API
// branch) and returns true iff (in,out) is exactly one of them. This header is FRAMEWORK-FREE (no
// iPlug2 include) by design, so it cannot call LegalIO; instead is_legal_io states the product policy
// every VALID negotiated plan must satisfy and that LegalIO must agree with:
//
//   * a plan with outputs always has 2 or 4 open outputs (the WET stereo pair + the optional DRY pair),
//   * a plan never opens more than the declared max (MaxNChannels(output)=4, MaxNChannels(input)=2),
//   * the input side is always 0/1/2 (off / mono / stereo).
//
// The host boundary (LunarHostPlugin::setActualChannelPlan, host/plugin.cpp) is where the two are
// joined: it does NOT trust this policy alone — it calls the real LegalIO(in,out) and requires it to
// ADMIT the plan, so a (openIn,openOut) that this policy would accept but LegalIO rejects (e.g. an
// input-only (1,0)/(2,0) with no outputs, which no declared config has) is correctly rejected.
inline bool is_legal_io(int openIn, int openOut) {
  return (openIn == 0 || openIn == 1 || openIn == 2) && (openOut == 2 || openOut == 4);
}

// Negotiate the actual stream plan from the DEVICE's physical channel capability and the user's
// SELECTED channels. L/R are 1-indexed user channels (0 = that side disabled). The output selection
// consumes BOTH L and R (a non-contiguous / out-of-range R is rejected, not silently dropped), so the
// plan can never "succeed" on an output selection that does not form a real in-range adjacent pair.
//
// A VALID plan is always an is_legal_io(openIn, openOut) config (<= the iPlug2 declared max), so it
// can never over-read a smaller device's buffers. No heap, no std::string: pure deterministic.
inline StreamPlan negotiate_stream_plan(int deviceInputChans, int deviceOutputChans,
                                        int selectedInL, int selectedInR,
                                        int selectedOutL, int selectedOutR) {
  StreamPlan plan;

  // ---- INPUT resolution ----------------------------------------------------------
  if (selectedInL <= 0 && selectedInR <= 0) {
    // Input disabled (or an output-only device). openIn = 0 is a VALID plan, never a failure.
    plan.firstIn = 0;
    plan.openIn = 0;
  } else if (selectedInR <= 0) {
    // One chosen input channel (mono). Valid iff it is a real channel on the device.
    if (selectedInL >= 1 && selectedInL <= deviceInputChans) {
      plan.firstIn = selectedInL - 1;
      plan.openIn = 1;
    } else {
      plan.status = StreamPlanStatus::InputInvalid;
      return plan;
    }
  } else {
    // A chosen input pair. Valid iff the two are DISTINCT, ADJACENT and in-range: the owner then
    // routes Distinct (EXT=ch0, PREAMP=ch1). A non-contiguous / duplicate / out-of-range pair is
    // rejected with no implicit copy (never silently copy one channel to the other).
    const bool distinct = (selectedInR != selectedInL);
    const bool adjacent = (selectedInR == selectedInL + 1);
    const bool inRange = (selectedInL >= 1 && selectedInR <= deviceInputChans);
    if (distinct && adjacent && inRange) {
      plan.firstIn = selectedInL - 1;
      plan.openIn = 2;
    } else {
      plan.status = StreamPlanStatus::InputInvalid;
      return plan;
    }
  }

  // ---- OUTPUT resolution ---------------------------------------------------------
  // The WET strategy needs a stereo L/R output pair that starts at outL and lies ON the device. The
  // selection consumes BOTH outL and outR: an off/incomplete (outR==0), duplicate (outR==outL),
  // non-contiguous (outR != outL+1) or out-of-range (outR > deviceOutputChans) pair is an
  // OUTPUT-INVALID selection with no implicit copy. Because a legal pair means outL+1 is a real
  // channel, there are always at least 2 openable channels from outL, so the open count is never 0
  // for a legal pair:
  const bool oDistinct = (selectedOutR != selectedOutL);
  const bool oAdjacent = (selectedOutR == selectedOutL + 1);
  const bool oInRange = (selectedOutL >= 1 && selectedOutR <= deviceOutputChans);
  if (!(selectedOutL >= 1 && selectedOutR >= 1 && oDistinct && oAdjacent && oInRange)) {
    plan.status = StreamPlanStatus::OutputInvalid;
    return plan;
  }
  plan.firstOut = selectedOutL - 1;
  const int contiguousFromFirst = deviceOutputChans - plan.firstOut;  // channels firstOut .. end.
  if (contiguousFromFirst >= 4) {
    plan.openOut = 4;
  } else {
    plan.openOut = 2;  // honest WET-only (a 2-3 out device, or an end-of-device "legal pair").
  }

  plan.status = StreamPlanStatus::Valid;
  return plan;
}

}  // namespace lunar24::host
